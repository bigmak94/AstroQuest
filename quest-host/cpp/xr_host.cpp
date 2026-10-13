// SPDX-License-Identifier: GPL-2.0-or-later

#include "xr_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "gl_frames.h"

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_TIMESPEC
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "core_process.h"
#include "held_pad.h"
#include "log.h"
#include "pad_router.h"
#include "panel_place.h"

namespace Protocol = Core::Vr::Protocol;

static_assert(PadRouter::RecenterPad == XrRecenter::Pad &&
              PadRouter::RecenterSeat == XrRecenter::Seat);
static_assert(static_cast<int>(PadRouter::Source::Gamepad) == 1 &&
              static_cast<int>(PadRouter::Source::Touch) == 2);

namespace {

constexpr int64_t FormatSrgb8Alpha8 = 0x8C43; // GL_SRGB8_ALPHA8
constexpr int64_t FormatRgba8 = 0x8058;       // GL_RGBA8

bool Check(XrResult result, const char* what) {
    if (XR_FAILED(result)) {
        LOGE("%s failed: %d", what, static_cast<int>(result));
        return false;
    }
    return true;
}

template <typename Function>
Function GetFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    xrGetInstanceProcAddr(instance, name, &function);
    return reinterpret_cast<Function>(function);
}

const char* SessionStateName(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_IDLE:
        return "idle";
    case XR_SESSION_STATE_READY:
        return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED:
        return "synchronized";
    case XR_SESSION_STATE_VISIBLE:
        return "visible";
    case XR_SESSION_STATE_FOCUSED:
        return "focused";
    case XR_SESSION_STATE_STOPPING:
        return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING:
        return "loss pending";
    case XR_SESSION_STATE_EXITING:
        return "exiting";
    default:
        return "unknown";
    }
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f t{
        u.y * v.z - u.z * v.y + q.w * v.x,
        u.z * v.x - u.x * v.z + q.w * v.y,
        u.x * v.y - u.y * v.x + q.w * v.z,
    };
    return {
        v.x + 2.0f * (u.y * t.z - u.z * t.y),
        v.y + 2.0f * (u.z * t.x - u.x * t.z),
        v.z + 2.0f * (u.x * t.y - u.y * t.x),
    };
}

struct Swapchain {
    XrSwapchain handle{XR_NULL_HANDLE};
    uint32_t width{};
    uint32_t height{};
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> framebuffers;
};

class Host {
public:
    Host(JavaVM* vm_, jobject activity_, CoreProcess& core_, PadRouter& pads_,
         StatusImage& status_, const XrHostOptions& options_, XrHostStatus& host_status_,
         std::atomic<bool>& quit_, std::atomic<uint32_t>& recenter_requests_)
        : vm{vm_}, activity{activity_}, core{core_}, pads{pads_}, status{status_},
          options{options_}, host_status{host_status_}, quit{quit_},
          recenter_requests{recenter_requests_} {}

    void Run() {
        if (gl.Create() && InitOpenXr() && InitRendering()) {
            Loop();
        } else {
            LOGE("the headset session could not be set up");
        }
        Shutdown();
    }

private:
    bool InitOpenXr() {
        const auto initialize_loader =
            GetFunction<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
        if (initialize_loader == nullptr) {
            LOGE("the OpenXR loader has no xrInitializeLoaderKHR");
            return false;
        }
        XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loader_info.applicationVM = vm;
        loader_info.applicationContext = activity;
        if (!Check(initialize_loader(
                       reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info)),
                   "xrInitializeLoaderKHR")) {
            return false;
        }

        uint32_t count = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
        std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data());
        const auto has = [&](const char* name) {
            return std::any_of(available.begin(), available.end(), [&](const auto& property) {
                return std::strcmp(property.extensionName, name) == 0;
            });
        };

        std::vector<const char*> extensions = {
            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        };
        const auto enable_if_present = [&](const char* name) {
            const bool present = has(name);
            if (present) {
                extensions.push_back(name);
            }
            return present;
        };
        has_refresh_rate = enable_if_present(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        has_performance = enable_if_present(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
        has_thread_settings = enable_if_present(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
        has_layer_settings = options.sharpen >= 2 &&
                             enable_if_present(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
        // Lets the system drop the sharpening while the GPU has nothing to spare for it. With
        // an emulator on the same GPU that is nearly always: in a level of the game the filter
        // was off two thirds of the time, which is why it is only left to the system on request.
        has_auto_filter = has_layer_settings && options.sharpen == 3 &&
                          enable_if_present(XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME);
        has_swapchain_state =
            options.cubic && enable_if_present(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME) &&
            enable_if_present(XR_FB_SWAPCHAIN_UPDATE_STATE_OPENGL_ES_EXTENSION_NAME);
        has_hand_tracking =
            options.track_hands && enable_if_present(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        has_layer_resolution =
            options.dynamic_resolution &&
            enable_if_present(XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME);
        has_metrics = enable_if_present(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
        // Only the probe needs a time without a frame to take it from.
        has_time_conversion =
            options.probe && enable_if_present(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);

        XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android_info.applicationVM = vm;
        android_info.applicationActivity = activity;
        XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_info.next = &android_info;
        std::strcpy(instance_info.applicationInfo.applicationName, "Astro VR Host");
        instance_info.applicationInfo.applicationVersion = 1;
        instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        instance_info.enabledExtensionNames = extensions.data();
        if (!Check(xrCreateInstance(&instance_info, &instance), "xrCreateInstance")) {
            return false;
        }

        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (!Check(xrGetSystem(instance, &system_info, &system), "xrGetSystem")) {
            return false;
        }
        XrSystemHandTrackingPropertiesEXT hand_properties{
            XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
        XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
        if (has_hand_tracking) {
            system_properties.next = &hand_properties;
        }
        xrGetSystemProperties(instance, system, &system_properties);
        max_swapchain_width = system_properties.graphicsProperties.maxSwapchainImageWidth;
        has_hand_tracking = has_hand_tracking && hand_properties.supportsHandTracking == XR_TRUE;
        LOGI("OpenXR system: %s, max swapchain %ux%u, hand tracking %d, layer filters %d (left "
             "to the system %d), cubic sampling %d, recommended layer size %d",
             system_properties.systemName, max_swapchain_width,
             system_properties.graphicsProperties.maxSwapchainImageHeight, has_hand_tracking,
             has_layer_settings, has_auto_filter, has_swapchain_state, has_layer_resolution);
        if (has_layer_resolution) {
            get_layer_resolution = GetFunction<PFN_xrGetRecommendedLayerResolutionMETA>(
                instance, "xrGetRecommendedLayerResolutionMETA");
        }

        // The runtime insists on being asked this before a session is created.
        const auto get_requirements = GetFunction<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(
            instance, "xrGetOpenGLESGraphicsRequirementsKHR");
        XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        if (get_requirements == nullptr ||
            !Check(get_requirements(instance, system, &requirements),
                   "xrGetOpenGLESGraphicsRequirementsKHR")) {
            return false;
        }

        XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        binding.display = gl.display;
        binding.config = gl.config;
        binding.context = gl.context;
        XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
        session_info.next = &binding;
        session_info.systemId = system;
        if (!Check(xrCreateSession(instance, &session_info, &session), "xrCreateSession")) {
            return false;
        }

        XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_info.poseInReferenceSpace.orientation.w = 1.0f;
        // LOCAL is the seated space: its origin is where the head was when the user last
        // recentred, which is exactly the emulator's "host space".
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!Check(xrCreateReferenceSpace(session, &space_info, &local_space), "create LOCAL")) {
            return false;
        }
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        if (!Check(xrCreateReferenceSpace(session, &space_info, &view_space), "create VIEW")) {
            return false;
        }

        if (has_hand_tracking) {
            CreateHandTrackers();
        }
        if (options.own_controllers) {
            CreateActions();
        }
        if (has_metrics) {
            FindMetrics();
        }
        return true;
    }

    /// The runtime's own counters (GPU load, the compositor's time, frames it had to repeat,
    /// load per processor core...): whatever it offers goes into the log, since the app cannot
    /// see any of that itself.
    void FindMetrics() {
        const auto enumerate = GetFunction<PFN_xrEnumeratePerformanceMetricsCounterPathsMETA>(
            instance, "xrEnumeratePerformanceMetricsCounterPathsMETA");
        const auto set_state = GetFunction<PFN_xrSetPerformanceMetricsStateMETA>(
            instance, "xrSetPerformanceMetricsStateMETA");
        query_metric = GetFunction<PFN_xrQueryPerformanceMetricsCounterMETA>(
            instance, "xrQueryPerformanceMetricsCounterMETA");
        uint32_t count = 0;
        if (enumerate == nullptr || set_state == nullptr || query_metric == nullptr ||
            XR_FAILED(enumerate(instance, 0, &count, nullptr)) || count == 0) {
            return;
        }
        std::vector<XrPath> paths(count);
        if (XR_FAILED(enumerate(instance, count, &count, paths.data()))) {
            return;
        }
        XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
        state.enabled = XR_TRUE;
        if (XR_FAILED(set_state(session, &state))) {
            return;
        }
        for (uint32_t i = 0; i < count && i < paths.size(); ++i) {
            char name[XR_MAX_PATH_LENGTH]{};
            uint32_t length = 0;
            if (XR_FAILED(xrPathToString(instance, paths[i], sizeof(name), &length, name))) {
                continue;
            }
            // "/perfmetrics_meta/device/gpu_utilization" -> "device/gpu_utilization"
            std::string text{name};
            if (const size_t slash = text.find('/', 1); slash != std::string::npos) {
                text.erase(0, slash + 1);
            }
            metrics.push_back({paths[i], text});
        }
        LOGI("the runtime offers %zu performance counters", metrics.size());
    }

    /// One line with the present value of every counter the runtime has one for.
    void ReportMetrics() {
        if (metrics.empty()) {
            return;
        }
        std::string line;
        for (const Metric& metric : metrics) {
            XrPerformanceMetricsCounterMETA counter{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
            if (XR_FAILED(query_metric(session, metric.path, &counter))) {
                continue;
            }
            char value[48];
            if ((counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) !=
                0) {
                std::snprintf(value, sizeof(value), "%.2f", counter.floatValue);
            } else if ((counter.counterFlags &
                        XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META) != 0) {
                std::snprintf(value, sizeof(value), "%u", counter.uintValue);
            } else {
                continue;
            }
            static constexpr const char* Units[] = {"", "%", " ms", " B", " Hz"};
            const auto unit = static_cast<size_t>(counter.counterUnit);
            line += (line.empty() ? "" : ", ") + metric.name + " " + value +
                    (unit < std::size(Units) ? Units[unit] : "");
        }
        if (!line.empty()) {
            LOGI("runtime: %s", line.c_str());
        }
    }

    void CreateHandTrackers() {
        const auto create =
            GetFunction<PFN_xrCreateHandTrackerEXT>(instance, "xrCreateHandTrackerEXT");
        locate_hand_joints =
            GetFunction<PFN_xrLocateHandJointsEXT>(instance, "xrLocateHandJointsEXT");
        if (create == nullptr || locate_hand_joints == nullptr) {
            has_hand_tracking = false;
            return;
        }
        for (int hand = 0; hand < 2; ++hand) {
            XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            info.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (!Check(create(session, &info, &hand_trackers[hand]), "xrCreateHandTrackerEXT")) {
                has_hand_tracking = false;
                return;
            }
        }
        LOGI("hand trackers created");
    }

    /// The headset's own controllers as the game's gamepad, for a player who holds them
    /// instead of one (PadRouter says what does what). Where one of them is and how it points
    /// (the right one, unless the settings say the left) is where the controller in the game
    /// is.
    void CreateActions() {
        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(set_info.actionSetName, "gamepad");
        std::strcpy(set_info.localizedActionSetName, "Gamepad");
        if (!Check(xrCreateActionSet(instance, &set_info, &action_set), "xrCreateActionSet")) {
            action_set = XR_NULL_HANDLE;
            return;
        }
        xrStringToPath(instance, "/user/hand/left", &hand_paths[0]);
        xrStringToPath(instance, "/user/hand/right", &hand_paths[1]);
        const auto make = [&](XrAction& action, const char* name, const char* shown,
                              XrActionType type, bool per_hand = false) {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            std::strcpy(info.actionName, name);
            std::strcpy(info.localizedActionName, shown);
            info.actionType = type;
            if (per_hand) {
                info.countSubactionPaths = 2;
                info.subactionPaths = hand_paths;
            }
            return XR_SUCCEEDED(xrCreateAction(action_set, &info, &action));
        };
        bool made = make(act_move, "left_stick", "Left stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
        made = made && make(act_finger, "touchpad_finger", "Finger on the touchpad",
                            XR_ACTION_TYPE_VECTOR2F_INPUT);
        made = made && make(act_left_in, "l3", "L3", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_right_in, "touchpad_press", "Press the touchpad",
                            XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_a, "cross", "Cross", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_b, "square", "Square", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_x, "circle", "Circle", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_y, "triangle", "Triangle", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_menu, "options", "Options", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_left_grip, "l1", "L1", XR_ACTION_TYPE_FLOAT_INPUT);
        made = made && make(act_right_grip, "r1", "R1", XR_ACTION_TYPE_FLOAT_INPUT);
        made = made && make(act_left_trigger, "l2", "L2", XR_ACTION_TYPE_FLOAT_INPUT);
        made = made && make(act_right_trigger, "r2", "R2", XR_ACTION_TYPE_FLOAT_INPUT);
        made = made && make(act_pose, "controller", "Controller", XR_ACTION_TYPE_POSE_INPUT);
        made = made &&
               make(act_rumble, "rumble", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
        if (!made) {
            LOGW("the headset's own controllers could not be set up as a gamepad");
            return;
        }

        std::vector<XrActionSuggestedBinding> bindings;
        const auto bind = [&](XrAction action, const char* path) {
            XrPath binding = XR_NULL_PATH;
            if (XR_SUCCEEDED(xrStringToPath(instance, path, &binding))) {
                bindings.push_back({action, binding});
            }
        };
        bind(act_move, "/user/hand/left/input/thumbstick");
        bind(act_finger, "/user/hand/right/input/thumbstick");
        bind(act_left_in, "/user/hand/left/input/thumbstick/click");
        bind(act_right_in, "/user/hand/right/input/thumbstick/click");
        bind(act_a, "/user/hand/right/input/a/click");
        bind(act_b, "/user/hand/right/input/b/click");
        bind(act_x, "/user/hand/left/input/x/click");
        bind(act_y, "/user/hand/left/input/y/click");
        bind(act_menu, "/user/hand/left/input/menu/click");
        bind(act_left_grip, "/user/hand/left/input/squeeze/value");
        bind(act_right_grip, "/user/hand/right/input/squeeze/value");
        bind(act_left_trigger, "/user/hand/left/input/trigger/value");
        bind(act_right_trigger, "/user/hand/right/input/trigger/value");
        bind(act_pose, options.pad_hand == 0 ? "/user/hand/left/input/aim/pose"
                                             : "/user/hand/right/input/aim/pose");
        bind(act_rumble, "/user/hand/left/output/haptic");
        bind(act_rumble, "/user/hand/right/output/haptic");
        // (The profile every controller of this make answers to, whatever its generation.)
        XrInteractionProfileSuggestedBinding suggested{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller",
                       &suggested.interactionProfile);
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        if (!Check(xrSuggestInteractionProfileBindings(instance, &suggested),
                   "xrSuggestInteractionProfileBindings")) {
            return;
        }
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &action_set;
        if (!Check(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets")) {
            return;
        }
        XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space_info.action = act_pose;
        space_info.poseInActionSpace.orientation.w = 1.0f;
        if (!Check(xrCreateActionSpace(session, &space_info, &controller_space),
                   "xrCreateActionSpace")) {
            controller_space = XR_NULL_HANDLE;
            return;
        }
        actions_ready = true;
        LOGI("the headset's own controllers can play the game: %zu controls bound, the %s one "
             "is the controller in the game",
             bindings.size(), options.pad_hand == 0 ? "left" : "right");
    }

    /// What the headset's own controllers say right now. Nothing of it counts unless at
    /// least one control is live: lying somewhere asleep, they say nothing at all.
    void ReadControllers(TouchState& touch) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        bool any_active = false;
        const auto pressed = [&](XrAction action) {
            get.action = action;
            XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_FAILED(xrGetActionStateBoolean(session, &get, &state)) ||
                state.isActive != XR_TRUE) {
                return false;
            }
            any_active = true;
            return state.currentState == XR_TRUE;
        };
        const auto pulled = [&](XrAction action) {
            get.action = action;
            XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_FAILED(xrGetActionStateFloat(session, &get, &state)) ||
                state.isActive != XR_TRUE) {
                return 0.0f;
            }
            any_active = true;
            return std::clamp(state.currentState, 0.0f, 1.0f);
        };
        const auto stick = [&](XrAction action) {
            get.action = action;
            XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_FAILED(xrGetActionStateVector2f(session, &get, &state)) ||
                state.isActive != XR_TRUE) {
                return XrVector2f{};
            }
            any_active = true;
            return state.currentState;
        };
        touch.a = pressed(act_a);
        touch.b = pressed(act_b);
        touch.x = pressed(act_x);
        touch.y = pressed(act_y);
        touch.menu = pressed(act_menu);
        touch.left_stick_in = pressed(act_left_in);
        touch.right_stick_in = pressed(act_right_in);
        touch.left_grip = pulled(act_left_grip);
        touch.right_grip = pulled(act_right_grip);
        touch.left_trigger = pulled(act_left_trigger);
        touch.right_trigger = pulled(act_right_trigger);
        const XrVector2f move = stick(act_move);
        const XrVector2f finger = stick(act_finger);
        touch.left_x = move.x;
        touch.left_y = move.y;
        touch.right_x = finger.x;
        touch.right_y = finger.y;
        touch.present = any_active;
    }

    /// Reads the headset's own controllers for the router, which decides whether the game is
    /// played with them, and finds the one that is the controller in the game if it is.
    void UpdateControllers(XrTime time) {
        TouchState touch;
        if (actions_ready) {
            const XrActiveActionSet active{action_set, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            // (Anything but plain success: the game is not what the player has in front of
            // them, and what they press is not meant for it.)
            if (xrSyncActions(session, &sync) == XR_SUCCESS) {
                ReadControllers(touch);
            }
        }
        // (With or without controllers: this is also what keeps the router's time, for
        // buttons that are held and for a stick that moves a finger over the touchpad.)
        pads.SetTouch(touch, PadRouter::Now());
        pad_view = pads.Look();

        controllers_used = pad_view.source == PadRouter::Source::Touch;
        if (controllers_used && !layout_logged) {
            layout_logged = true;
            LOGI("the headset's own controllers play the game: left stick to move, A = cross, "
                 "B = square, X = circle, Y = triangle (X and Y together blow), right stick = "
                 "finger on the touchpad, pressed in = touchpad pressed, grips = L1 and R1, "
                 "triggers = L2 and R2, left menu button = OPTIONS (held for a second: view "
                 "reset), both sticks pressed in = view reset; on the touchpad, the %s trigger "
                 "presses, the %s grip swipes forward, the %s trigger pulls back and lets go",
                 options.pad_hand == 0 ? "left" : "right", options.pad_hand == 0 ? "left" : "right",
                 options.pad_hand == 0 ? "right" : "left");
        }
        controller_located = false;
        if (controllers_used && touch.present) {
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            location.next = &velocity;
            if (XR_SUCCEEDED(xrLocateSpace(controller_space, local_space, time, &location)) &&
                (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0 &&
                (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
                controller_located = true;
                controller_pose = location.pose;
                controller_linear =
                    (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0
                        ? velocity.linearVelocity
                        : XrVector3f{};
                controller_angular =
                    (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0
                        ? velocity.angularVelocity
                        : XrVector3f{};
            }
        }
        ApplyRumble(controllers_used && touch.present);
    }

    /// What the game asks of the gamepad's two motors, on the two controllers: the heavy
    /// motor is in the gamepad's left grip, the light one in its right, and each is felt a
    /// little in the other hand too.
    void ApplyRumble(bool on) {
        if (!actions_ready) {
            return;
        }
        // (The small motor in the lowest byte, the large one above it.)
        const uint32_t wanted =
            on && options.rumble ? static_cast<uint32_t>(core.GetPadFeedback() & 0xffff) : 0;
        const auto now = std::chrono::steady_clock::now();
        // A vibration is asked for by its length: one that goes on is asked for again.
        if (wanted == rumble_applied &&
            (wanted == 0 || now - rumble_time < std::chrono::milliseconds{250})) {
            return;
        }
        rumble_applied = wanted;
        rumble_time = now;
        const float light = static_cast<float>(wanted & 0xff) / 255.0f;
        const float heavy = static_cast<float>((wanted >> 8) & 0xff) / 255.0f;
        for (int hand = 0; hand < 2; ++hand) {
            const float amplitude =
                hand == 0 ? std::max(heavy, light * 0.4f) : std::max(light, heavy * 0.4f);
            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = act_rumble;
            info.subactionPath = hand_paths[hand];
            if (amplitude > 0.0f) {
                XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
                vibration.duration = 500'000'000;
                vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
                vibration.amplitude = std::min(amplitude, 1.0f);
                xrApplyHapticFeedback(session, &info,
                                      reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
            } else {
                xrStopHapticFeedback(session, &info);
            }
        }
    }

    bool CreateSwapchain(Swapchain& swapchain, uint32_t width, uint32_t height) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = swapchain_format;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!Check(xrCreateSwapchain(session, &info, &swapchain.handle), "xrCreateSwapchain")) {
            return false;
        }
        swapchain.width = width;
        swapchain.height = height;

        uint32_t count = 0;
        xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
        swapchain.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (!Check(xrEnumerateSwapchainImages(
                       swapchain.handle, count, &count,
                       reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())),
                   "xrEnumerateSwapchainImages")) {
            return false;
        }
        swapchain.framebuffers.assign(count, 0);
        glGenFramebuffers(static_cast<GLsizei>(count), swapchain.framebuffers.data());
        for (uint32_t i = 0; i < count; ++i) {
            glBindFramebuffer(GL_FRAMEBUFFER, swapchain.framebuffers[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   swapchain.images[i].image, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    bool InitRendering() {
        uint32_t count = 0;
        xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(session, count, &count, formats.data());
        const bool srgb = std::find(formats.begin(), formats.end(), FormatSrgb8Alpha8) !=
                          formats.end();
        swapchain_format = srgb ? FormatSrgb8Alpha8 : FormatRgba8;
        decode_srgb = srgb;

        // Both eyes side by side in one image, like the frames arrive.
        const uint32_t frame_width = options.eye_width * 2;
        const uint32_t frame_height = options.eye_height;
        const uint32_t swapchain_width =
            max_swapchain_width != 0 ? std::min(frame_width, max_swapchain_width) : frame_width;
        if (!CreateSwapchain(game_swapchain, swapchain_width, frame_height) ||
            !CreateSwapchain(status_swapchain, StatusImage::Width, StatusImage::Height)) {
            return false;
        }
        LOGI("swapchains: %s, the game's %ux%u in %zu images, the panel's %ux%u in %zu; %u "
             "formats on offer",
             srgb ? "sRGB" : "plain RGBA", game_swapchain.width, game_swapchain.height,
             game_swapchain.images.size(), status_swapchain.width, status_swapchain.height,
             status_swapchain.images.size(), count);
        for (const Swapchain* swapchain : {&game_swapchain, &status_swapchain}) {
            for (const GLuint framebuffer : swapchain->framebuffers) {
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                const GLenum state = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (state != GL_FRAMEBUFFER_COMPLETE) {
                    LOGE("a swapchain image cannot be drawn to (0x%x)", state);
                }
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        layer_width = shown_width = game_swapchain.width;
        layer_height = shown_height = game_swapchain.height;
        if (has_swapchain_state) {
            UseCubicSampling(game_swapchain);
        }

        if (!blitter.Create()) {
            return false;
        }
        LOGI("frames are copied to the compositor's image %s%s",
             !srgb || blitter.WritesUnencoded() ? "as they are" : "through a decoder",
             copy_timer.Create() ? "; the GPU's time for that is timed" : "");
        return frames.Create(gl.display, frame_width, frame_height, core);
    }

    void PollEvents() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& changed =
                    *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                LOGI("session state: %s", SessionStateName(changed.state));
                switch (changed.state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (Check(xrBeginSession(session, &begin), "xrBeginSession")) {
                        session_running = true;
                        host_status.session_running = true;
                        ApplySessionSettings();
                        // The game waited while nobody wore the headset.
                        core.SetPaused(false);
                        if (panel_pose) {
                            // Whoever puts the headset back on finds the panel in front of
                            // them, wherever they turned meanwhile.
                            panel_pose.reset();
                            panel_reason = "the headset put on again";
                        }
                    }
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    xrEndSession(session);
                    session_running = false;
                    host_status.session_running = false;
                    // The headset came off: a game nobody sees should not play on.
                    core.SetPaused(true);
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    exit_requested = true;
                    break;
                default:
                    break;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                exit_requested = true;
            } else if (event.type == XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT) {
                // The system saying that the headset runs hot or cannot keep up: levels are
                // 0 normal, 25 warning, 75 impaired.
                const auto& changed = *reinterpret_cast<const XrEventDataPerfSettingsEXT*>(&event);
                LOGW("performance notice: %s %s went from level %d to %d",
                     changed.domain == XR_PERF_SETTINGS_DOMAIN_CPU_EXT ? "processor" : "GPU",
                     changed.subDomain == XR_PERF_SETTINGS_SUB_DOMAIN_THERMAL_EXT
                         ? "temperature"
                         : changed.subDomain == XR_PERF_SETTINGS_SUB_DOMAIN_COMPOSITING_EXT
                               ? "compositing"
                               : "rendering",
                     static_cast<int>(changed.fromLevel), static_cast<int>(changed.toLevel));
            } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                const auto& changed =
                    *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
                if (changed.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    // (0 would be "none".)
                    space_change_time = std::max<XrTime>(changed.changeTime, 1);
                    // The panel that stands in the room comes along: from that moment it is
                    // placed afresh, in front of the player, where the reset puts ahead.
                    // (Its own time: SendPadPose takes space_change_time against a pose time
                    // beyond the frame's.)
                    panel_change_time = std::max<XrTime>(changed.changeTime, 1);
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
                const auto& changed =
                    *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
                LOGI("display refresh rate %.0f -> %.0f Hz", changed.fromDisplayRefreshRate,
                     changed.toDisplayRefreshRate);
                host_status.refresh_rate = changed.toDisplayRefreshRate;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void ApplySessionSettings() {
        if (has_refresh_rate) {
            const auto enumerate = GetFunction<PFN_xrEnumerateDisplayRefreshRatesFB>(
                instance, "xrEnumerateDisplayRefreshRatesFB");
            uint32_t count = 0;
            if (enumerate != nullptr && XR_SUCCEEDED(enumerate(session, 0, &count, nullptr))) {
                std::vector<float> rates(count);
                enumerate(session, count, &count, rates.data());
                std::string list;
                uint32_t offered = 0;
                for (uint32_t i = 0; i < count && i < rates.size(); ++i) {
                    list += (list.empty() ? "" : ", ") + std::to_string(static_cast<int>(rates[i]));
                    const int hz = static_cast<int>(rates[i] + 0.5f);
                    offered |= hz == 72 ? 1u : hz == 80 ? 2u : hz == 90 ? 4u : hz == 120 ? 8u : 0u;
                }
                host_status.offered_rates = offered;
                LOGI("display refresh rates on offer: %s Hz", list.c_str());
            }
            request_refresh_rate = GetFunction<PFN_xrRequestDisplayRefreshRateFB>(
                instance, "xrRequestDisplayRefreshRateFB");
            // What the startup menu chose last, if it changed it while this session ran.
            const float asked = asked_refresh_rate > 0.0f ? asked_refresh_rate
                                                          : options.refresh_rate;
            const bool granted = request_refresh_rate != nullptr && asked > 0.0f &&
                                 Check(request_refresh_rate(session, asked),
                                       "xrRequestDisplayRefreshRateFB");
            if (request_refresh_rate != nullptr && !granted) {
                // Nothing asked for, or a rate this headset does not have. The emulated headset
                // refreshes when this display does, and the game draws one frame for a whole
                // number of refreshes (the emulator chooses how many): at 90 Hz 45 or 30 frames
                // a second, and the levels get the 30 with three pictures of the compositor's
                // each instead of 120 Hz's four (each of them GPU time the game does not have).
                // At 120 Hz it is 60, 40 or 30, at 80 Hz 40 and at 72 Hz 36.
                for (const float rate : {90.0f, 120.0f, 80.0f, 72.0f}) {
                    if (XR_SUCCEEDED(request_refresh_rate(session, rate))) {
                        break;
                    }
                }
            }
            const auto get = GetFunction<PFN_xrGetDisplayRefreshRateFB>(
                instance, "xrGetDisplayRefreshRateFB");
            float rate = 0.0f;
            if (get != nullptr && XR_SUCCEEDED(get(session, &rate))) {
                LOGI("display refresh rate %.0f Hz", rate);
                host_status.refresh_rate = rate;
            }
        }
        if (has_performance) {
            // An emulator needs every cycle it can get, on both processors.
            const auto set_level = GetFunction<PFN_xrPerfSettingsSetPerformanceLevelEXT>(
                instance, "xrPerfSettingsSetPerformanceLevelEXT");
            if (set_level != nullptr) {
                const XrResult cpu = set_level(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                                               options.cpu_boost
                                                   ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT
                                                   : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                const XrResult gpu = set_level(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
                                               XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                LOGI("performance levels asked for: processor %s -> %d, GPU sustained high -> %d "
                     "(0 is accepted)",
                     options.cpu_boost ? "boost" : "sustained high", static_cast<int>(cpu),
                     static_cast<int>(gpu));
            }
        }
        if (has_thread_settings) {
            // This thread has to get its turn at every refresh. The emulator's threads cannot
            // be named to the runtime: it only takes threads of the process that asks (those of
            // a child are refused, see ProbeIdleSession), so their priorities are the
            // emulator's own doing (SHADPS4_THREAD_NICE).
            const auto set_thread = GetFunction<PFN_xrSetAndroidApplicationThreadKHR>(
                instance, "xrSetAndroidApplicationThreadKHR");
            if (set_thread != nullptr) {
                const XrResult result =
                    set_thread(session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR,
                               static_cast<uint32_t>(::gettid()));
                LOGI("thread hint for the frame loop: %d", static_cast<int>(result));
            }
        }
    }

    /// What a session that has not begun answers to the settings the host makes when one does.
    void ProbeIdleSession() {
        ApplySessionSettings();
        const auto set_thread = GetFunction<PFN_xrSetAndroidApplicationThreadKHR>(
            instance, "xrSetAndroidApplicationThreadKHR");
        if (has_thread_settings && set_thread != nullptr) {
            // A thread of another process of the app, which is what the emulator's threads are.
            // (On Horizon OS 207: XR_ERROR_ANDROID_THREAD_SETTINGS_ID_INVALID_KHR.)
            const pid_t child = ::fork();
            if (child == 0) {
                ::sleep(2);
                ::_exit(0);
            }
            if (child > 0) {
                const XrResult result =
                    set_thread(session, XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR,
                               static_cast<uint32_t>(child));
                ::waitpid(child, nullptr, 0);
                LOGI("thread hint for a thread of a child process: %d", static_cast<int>(result));
            }
        }
        ProbeBlit();
        ProbePanel();
        ProbeLayerSize();
    }

    /// Puts a picture on the status panel the way the app's text gets there, and looks at
    /// what the panel's image holds afterwards: the upper half red, the lower half blue.
    void ProbePanel() {
        {
            std::scoped_lock lock{status.mutex};
            status.pixels.resize(size_t{StatusImage::Width} * StatusImage::Height * 4);
            for (int y = 0; y < StatusImage::Height; ++y) {
                for (int x = 0; x < StatusImage::Width; ++x) {
                    uint8_t* pixel = status.pixels.data() + (size_t{StatusImage::Width} * y + x) * 4;
                    const bool upper = y < StatusImage::Height / 2;
                    pixel[0] = upper ? 200 : 30;
                    pixel[1] = 30;
                    pixel[2] = upper ? 30 : 200;
                    pixel[3] = 255;
                }
            }
            ++status.version;
        }
        status_ready = false;
        UpdateStatusPanel();
        if (!status_ready) {
            LOGW("probe: the status panel's image could not be written");
            return;
        }
        // Every image of the swapchain that has the picture now has it the right way up?
        int matching = 0;
        for (const GLuint framebuffer : status_swapchain.framebuffers) {
            uint8_t top[4]{};
            uint8_t bottom[4]{};
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glReadPixels(StatusImage::Width / 2, StatusImage::Height * 3 / 4, 1, 1, GL_RGBA,
                         GL_UNSIGNED_BYTE, top);
            glReadPixels(StatusImage::Width / 2, StatusImage::Height / 4, 1, 1, GL_RGBA,
                         GL_UNSIGNED_BYTE, bottom);
            if (top[0] > 190 && top[2] < 40 && bottom[2] > 190 && bottom[0] < 40) {
                ++matching;
            }
        }
        const GLenum error = glGetError();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        LOGI("probe: status panel written; %d of %zu images hold it the right way up (GL error "
             "0x%x)",
             matching, status_swapchain.framebuffers.size(), error);
    }

    /// What the system recommends for the game's layer while nothing is being shown.
    void ProbeLayerSize() {
        const auto convert = GetFunction<PFN_xrConvertTimespecTimeToTimeKHR>(
            instance, "xrConvertTimespecTimeToTimeKHR");
        if (get_layer_resolution == nullptr || !has_time_conversion || convert == nullptr) {
            LOGI("probe: no recommended layer size to ask for");
            return;
        }
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        XrTime time = 0;
        if (XR_FAILED(convert(instance, &now, &time))) {
            return;
        }
        // A frame as the emulator would deliver it: looking straight ahead, the game's view.
        Protocol::Frame frame{};
        frame.orientation[3] = 1.0f;
        frame.fov[0] = 1.207f;
        frame.fov[1] = 1.181f;
        frame.fov[2] = 1.263f;
        frame.fov[3] = 1.263f;
        XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        FillProjection(frame, game_swapchain.width, game_swapchain.height, views, projection);
        XrRecommendedLayerResolutionGetInfoMETA info{
            XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META};
        info.layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        info.predictedDisplayTime = time + 20'000'000;
        XrRecommendedLayerResolutionMETA recommended{XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META};
        const XrResult result = get_layer_resolution(session, &info, &recommended);
        LOGI("probe: recommended layer size for %ux%u an eye: %dx%d, valid %d (result %d)",
             game_swapchain.width / 2, game_swapchain.height,
             recommended.recommendedImageDimensions.width,
             recommended.recommendedImageDimensions.height,
             static_cast<int>(recommended.isValid), static_cast<int>(result));
    }

    /// Copies a test picture into the image the compositor would show, the way frames are, and
    /// reads it back: the top left of a frame has to end up top left and in its own colours,
    /// at full size and at the smaller size the picture is shown at while the GPU is short.
    void ProbeBlit() {
        // Four fields, first row on top as in the emulator's frames: red, green / blue, yellow.
        constexpr int Width = 64;
        constexpr int Height = 32;
        static constexpr uint8_t Fields[4][3] = {
            {200, 30, 30}, {30, 200, 30}, {30, 30, 200}, {200, 200, 30}};
        std::vector<uint8_t> picture(Width * Height * 4);
        for (int y = 0; y < Height; ++y) {
            for (int x = 0; x < Width; ++x) {
                const uint8_t* field = Fields[(y >= Height / 2 ? 2 : 0) + (x >= Width / 2 ? 1 : 0)];
                uint8_t* pixel = picture.data() + (y * Width + x) * 4;
                pixel[0] = field[0];
                pixel[1] = field[1];
                pixel[2] = field[2];
                pixel[3] = 255;
            }
        }
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Width, Height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     picture.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        for (const float scale : {1.0f, MinLayerScale}) {
            const auto width = static_cast<uint32_t>(static_cast<float>(game_swapchain.width) * scale);
            const auto height =
                static_cast<uint32_t>(static_cast<float>(game_swapchain.height) * scale);
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            const XrResult acquired =
                xrAcquireSwapchainImage(game_swapchain.handle, &acquire, &index);
            if (XR_FAILED(acquired)) {
                LOGW("probe: no swapchain image to be had outside a session (%d)",
                     static_cast<int>(acquired));
                break;
            }
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = 1'000'000'000;
            const XrResult waited = xrWaitSwapchainImage(game_swapchain.handle, &wait);

            glBindFramebuffer(GL_FRAMEBUFFER, game_swapchain.framebuffers[index]);
            blitter.Draw(texture, width, height, false, decode_srgb);
            // Top left and bottom right field, as GL counts rows: from the bottom.
            uint8_t top_left[4]{};
            uint8_t bottom_right[4]{};
            glReadPixels(static_cast<GLint>(width / 4), static_cast<GLint>(height * 3 / 4), 1, 1,
                         GL_RGBA, GL_UNSIGNED_BYTE, top_left);
            glReadPixels(static_cast<GLint>(width * 3 / 4), static_cast<GLint>(height / 4), 1, 1,
                         GL_RGBA, GL_UNSIGNED_BYTE, bottom_right);
            const GLenum error = glGetError();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const XrResult released = xrReleaseSwapchainImage(game_swapchain.handle, &release);

            const auto close = [](const uint8_t* seen, const uint8_t* expected) {
                return std::abs(seen[0] - expected[0]) <= 3 && std::abs(seen[1] - expected[1]) <= 3 &&
                       std::abs(seen[2] - expected[2]) <= 3;
            };
            const bool right = close(top_left, Fields[0]) && close(bottom_right, Fields[3]);
            LOGI("probe: picture copied to swapchain image %u at %ux%u: top left %u,%u,%u and "
                 "bottom right %u,%u,%u, %s (wait %d, release %d, GL error 0x%x)",
                 index, width, height, top_left[0], top_left[1], top_left[2], bottom_right[0],
                 bottom_right[1], bottom_right[2], right ? "as it should be" : "WRONG",
                 static_cast<int>(waited), static_cast<int>(released), error);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &texture);
    }

    /// Has the compositor read a swapchain's images with the GPU's cubic filter where it
    /// enlarges them. The game's picture has less than half as many pixels across as the
    /// display has for it; a linear filter turns that into a blur.
    void UseCubicSampling(const Swapchain& swapchain) {
        // From IMG_texture_filter_cubic.
        static constexpr EGLenum CubicImg = 0x9139;
        const auto get_state =
            GetFunction<PFN_xrGetSwapchainStateFB>(instance, "xrGetSwapchainStateFB");
        const auto update =
            GetFunction<PFN_xrUpdateSwapchainFB>(instance, "xrUpdateSwapchainFB");
        if (get_state == nullptr || update == nullptr) {
            return;
        }
        XrSwapchainStateSamplerOpenGLESFB sampler{XR_TYPE_SWAPCHAIN_STATE_SAMPLER_OPENGL_ES_FB};
        const XrResult got = get_state(
            swapchain.handle, reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&sampler));
        if (XR_FAILED(got)) {
            LOGW("the compositor does not tell how it reads the game's picture (%d)",
                 static_cast<int>(got));
            return;
        }
        const EGLenum before = sampler.magFilter;
        sampler.magFilter = CubicImg;
        const XrResult updated = update(
            swapchain.handle, reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&sampler));
        LOGI("the compositor enlarges the game's picture with a cubic filter (was 0x%x): %s (%d)",
             before, XR_SUCCEEDED(updated) ? "accepted" : "refused", static_cast<int>(updated));
    }

    /// Asks the system at what size it wants the game's picture: all of the image the frames
    /// are copied to, or less of it while the GPU is short. That it is asked is what counts:
    /// the system lets the GPU run faster for an app that gives way. The game gives way in the
    /// size it draws its scene at, which is where the GPU's time goes; showing the finished
    /// picture smaller saves nothing and blurs it, and is only done on request.
    void UpdateLayerSize(XrTime display_time) {
        if (get_layer_resolution == nullptr || !shown_frame) {
            return;
        }
        // The question is about a layer: the one that would be shown at full size.
        XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        FillProjection(*shown_frame, game_swapchain.width, game_swapchain.height, views,
                       projection);
        XrRecommendedLayerResolutionGetInfoMETA info{
            XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META};
        info.layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        info.predictedDisplayTime = display_time;
        XrRecommendedLayerResolutionMETA recommended{XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META};
        if (XR_FAILED(get_layer_resolution(session, &info, &recommended)) ||
            recommended.isValid != XR_TRUE) {
            return;
        }
        recommended_width = recommended.recommendedImageDimensions.width;
        recommended_height = recommended.recommendedImageDimensions.height;
        // One eye's size. Never more than there is, and not so little that it shows.
        const float full_width = static_cast<float>(game_swapchain.width / 2);
        const float full_height = static_cast<float>(game_swapchain.height);
        const float scale = std::clamp(
            std::min(static_cast<float>(recommended.recommendedImageDimensions.width) / full_width,
                     static_cast<float>(recommended.recommendedImageDimensions.height) /
                         full_height),
            options.follow_layer_size ? MinLayerScale : 1.0f, 1.0f);
        // In steps, so that it does not change with every frame.
        const uint32_t eye_width =
            std::min(game_swapchain.width / 2,
                     (static_cast<uint32_t>(full_width * scale) + 15) / 16 * 16);
        const uint32_t height =
            std::min(game_swapchain.height,
                     (static_cast<uint32_t>(full_height * scale) + 15) / 16 * 16);
        if (!layer_size_logged) {
            layer_size_logged = true;
            LOGI("the system recommends %dx%d for an eye; the game's picture is shown at %ux%u",
                 recommended.recommendedImageDimensions.width,
                 recommended.recommendedImageDimensions.height, eye_width, height);
        }
        layer_width = eye_width * 2;
        layer_height = height;
    }

    /// Copies the emulator's frame into the image the compositor shows.
    bool BlitFrame(const Protocol::Frame& frame) {
        if (!frames.IsValid(frame.buffer)) {
            return false;
        }
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(game_swapchain.handle, &acquire, &index))) {
            return false;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(game_swapchain.handle, &wait);

        // Into the lower left corner when the picture is to be smaller than the image.
        glBindFramebuffer(GL_FRAMEBUFFER, game_swapchain.framebuffers[index]);
        // What the image held is drawn over, or lies outside what the compositor shows: the
        // GPU need not keep it (a driver that draws in tiles would read it in first).
        const GLenum color = GL_COLOR_ATTACHMENT0;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &color);
        copy_timer.Begin();
        blitter.Draw(frames.Texture(frame.buffer), layer_width, layer_height,
                     frame.swap_red_blue != 0, decode_srgb, host_status.reduced_fov, frame.fov);
        copy_timer.End();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        shown_width = layer_width;
        shown_height = layer_height;

        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(game_swapchain.handle, &release);
        return true;
    }

    void UpdateStatusPanel() {
        std::vector<uint8_t> pixels;
        {
            std::scoped_lock lock{status.mutex};
            if (status.version == status_version || status.pixels.empty()) {
                return;
            }
            pixels = status.pixels;
            status_version = status.version;
        }
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(status_swapchain.handle, &acquire, &index))) {
            return;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(status_swapchain.handle, &wait);

        // Bitmaps start at the top, GL textures at the bottom.
        const size_t row_bytes = size_t{StatusImage::Width} * 4;
        std::vector<uint8_t> flipped(pixels.size());
        for (int y = 0; y < StatusImage::Height; ++y) {
            std::memcpy(flipped.data() + y * row_bytes,
                        pixels.data() + (StatusImage::Height - 1 - y) * row_bytes, row_bytes);
        }
        glBindTexture(GL_TEXTURE_2D, status_swapchain.images[index].image);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, StatusImage::Width, StatusImage::Height, GL_RGBA,
                        GL_UNSIGNED_BYTE, flipped.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(status_swapchain.handle, &release);
        status_ready = true;
    }

    /// Places the panel that stands still in the room (panel_pose) where it is not placed yet,
    /// and anew once the headset's own reset of the view has moved the seated space: ahead of
    /// where the head faces at `time`. False while the headset cannot say where the head is.
    bool PlacePanel(XrTime time) {
        if (panel_change_time != 0 && time >= panel_change_time) {
            panel_change_time = 0;
            panel_pose.reset();
            panel_reason = "the headset's own view reset";
        }
        if (panel_pose) {
            return true;
        }
        const XrSpaceLocationFlags needed =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const bool located =
            XR_SUCCEEDED(xrLocateSpace(view_space, local_space, time, &location)) &&
            (location.locationFlags & needed) == needed;
        const XrPosef& head = location.pose;
        if (!located || !std::isfinite(head.position.x) || !std::isfinite(head.position.y) ||
            !std::isfinite(head.position.z)) {
            if (!panel_wait_logged) {
                panel_wait_logged = true;
                LOGI("the headset cannot say where the head is yet: the panel follows the head "
                     "until it can");
            }
            return false;
        }
        const PanelPlace::Pose placed = PanelPlace::InFront(
            {head.position.x, head.position.y, head.position.z},
            {head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w},
            PanelDistance, PanelDrop);
        panel_pose = XrPosef{{placed.orientation.x, placed.orientation.y, placed.orientation.z,
                              placed.orientation.w},
                             {placed.position.x, placed.position.y, placed.position.z}};
        LOGI("the panel stands still in the room: %.1f m ahead of the head, %.0f degrees to the "
             "left of the headset's straight ahead (%s)",
             PanelDistance, placed.heading * Degrees, panel_reason);
        return true;
    }

    void Loop() {
        while (!quit && !exit_requested) {
            PollEvents();
            if (session_running && host_status.wanted_refresh_rate.load() > 0.0f) {
                // Chosen in the startup menu: the display switches now.
                const float wanted = host_status.wanted_refresh_rate.exchange(0.0f);
                asked_refresh_rate = wanted;
                if (request_refresh_rate != nullptr) {
                    const XrResult result = request_refresh_rate(session, wanted);
                    LOGI("display refresh rate %.0f Hz asked for in the startup menu (%d)", wanted,
                         static_cast<int>(result));
                    if (XR_FAILED(result)) {
                        // The display keeps the rate it has; a later start asks for that.
                        asked_refresh_rate = host_status.refresh_rate.load();
                    }
                }
            }
            if (!session_running) {
                if (options.probe && !probed) {
                    probed = true;
                    ProbeIdleSession();
                }
                // (Nobody wears the headset, and nobody plays: but a button held on a
                // gamepad still takes its time.)
                pads.Tick(PadRouter::Now());
                usleep(50'000);
                continue;
            }

            XrFrameState frame_state{XR_TYPE_FRAME_STATE};
            if (const XrResult waited = xrWaitFrame(session, nullptr, &frame_state);
                XR_FAILED(waited)) {
                NoteFrameFailure("xrWaitFrame", waited);
                usleep(5'000);
                continue;
            }
            // The runtime lets go of xrWaitFrame once for every refresh of the display, at
            // the same point of each: the emulated headset takes its own refreshes from that.
            if (host_status.refresh_rate > 0.0f) {
                core.SendRefresh(host_status.refresh_rate);
            }
            if (const XrResult begun = xrBeginFrame(session, nullptr); XR_FAILED(begun)) {
                NoteFrameFailure("xrBeginFrame", begun);
                continue;
            }

            const XrTime pose_time =
                frame_state.predictedDisplayTime +
                static_cast<XrTime>(std::clamp(options.predict_ms, 0.0f, 80.0f) * 1e6f);
            SendHeadPose(pose_time);
            UpdateControllers(pose_time);
            SendPadPose(pose_time);
            UpdateLayerSize(frame_state.predictedDisplayTime);

            const auto now = std::chrono::steady_clock::now();
            if (const auto frame = core.TakeFrame()) {
                if (BlitFrame(*frame)) {
                    shown_frame = *frame;
                    last_frame_time = now;
                    ++frame_count;
                }
            }
            ReportPacing(now);
            UpdateStatusPanel();

            // The panel covers boot, loading screens that last, and a core that died.
            const bool stale = !shown_frame ||
                               now - last_frame_time > std::chrono::milliseconds{2500};

            XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            XrCompositionLayerSettingsFB layer_settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            std::vector<const XrCompositionLayerBaseHeader*> layers;

            if (frame_state.shouldRender == XR_TRUE) {
                if (shown_frame) {
                    FillProjection(*shown_frame, shown_width, shown_height, views, projection);
                    if (has_layer_settings) {
                        // The game draws fewer pixels per degree than the display has.
                        layer_settings.layerFlags =
                            options.sharpen == 4
                                ? XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB
                                : XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
                        if (has_auto_filter) {
                            layer_settings.layerFlags |=
                                XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META;
                        }
                        projection.next = &layer_settings;
                    }
                    layers.push_back(
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
                }
                const bool card = host_status.show_panel;
                if ((stale || options.show_stats || card) && status_ready) {
                    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    quad.space = view_space;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain = status_swapchain.handle;
                    quad.subImage.imageRect.extent = {StatusImage::Width, StatusImage::Height};
                    quad.pose.orientation.w = 1.0f;
                    if (!shown_frame) {
                        // The start-up menu, and what the panel says after it until the game
                        // has a picture: it stands still in the room where the player faced
                        // when it appeared, and the head looks at it and away from it. Until
                        // the headset can say where the head is, it hangs where it will stand
                        // and follows the head.
                        if (PlacePanel(frame_state.predictedDisplayTime)) {
                            quad.space = local_space;
                            quad.pose = *panel_pose;
                        } else {
                            quad.pose.position = {0.0f, -PanelDrop, -PanelDistance};
                        }
                        quad.size = PanelSize;
                    } else if (stale) {
                        // The game stopped drawing (a long load, or a core that died): said
                        // wherever the player looks.
                        quad.pose.position = {0.0f, 0.0f, -1.6f};
                        quad.size = {1.4f, 0.7f};
                    } else if (card) {
                        // To be read while playing: large enough for that, below the
                        // middle of the view.
                        quad.pose.position = {0.0f, -0.40f, -1.3f};
                        quad.size = {0.9f, 0.45f};
                    } else {
                        // Over the game: out of the way of what is being played.
                        quad.pose.position = {0.0f, -0.55f, -1.6f};
                        quad.size = {0.56f, 0.28f};
                    }
                    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
                }
            }

            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = frame_state.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount = static_cast<uint32_t>(layers.size());
            end.layers = layers.data();
            if (const XrResult ended = xrEndFrame(session, &end); XR_FAILED(ended)) {
                // A layer the runtime does not accept is a frame nobody sees.
                NoteFrameFailure("xrEndFrame", ended);
            } else if (!layers.empty() && !shown_logged) {
                shown_logged = true;
                LOGI("first frame handed to the compositor: %zu layer(s), %s", layers.size(),
                     shown_frame ? "the game's picture" : "the status panel");
            } else if (shown_frame && !game_shown_logged && !layers.empty()) {
                game_shown_logged = true;
                LOGI("the game's first picture handed to the compositor: %ux%u an eye, field of "
                     "view %.1f/%.1f/%.1f/%.1f degrees (out, in, up, down)",
                     shown_width / 2, shown_height,
                     std::atan(shown_frame->fov[0]) * 57.29578f,
                     std::atan(shown_frame->fov[1]) * 57.29578f,
                     std::atan(shown_frame->fov[2]) * 57.29578f,
                     std::atan(shown_frame->fov[3]) * 57.29578f);
            }
        }
    }

    /// Frame calls that fail are frames nobody sees, and nothing else would tell: the first
    /// few go to the log, then every thousandth.
    void NoteFrameFailure(const char* what, XrResult result) {
        ++frame_failures;
        if (frame_failures <= 5 || frame_failures % 1000 == 0) {
            LOGE("%s failed: %d (%u frame calls failed so far)", what, static_cast<int>(result),
                 frame_failures);
        }
    }

    /// Every ten seconds: how many frames the game delivered against how many the headset
    /// showed (the first number over the second is how much of the motion is the game's own),
    /// what the emulator used to get there, and what was seen of the hands and the controller
    /// they hold.
    void ReportPacing(std::chrono::steady_clock::time_point now) {
        ++display_frames;
        if (now - last_report < std::chrono::seconds{10}) {
            return;
        }
        const float seconds = std::chrono::duration<float>(now - last_report).count();
        const CoreProcess::Usage usage = core.GetUsage();
        const GpuTimer::Summary copy = copy_timer.Take();
        char copy_text[128] = "";
        if (copy.count != 0) {
            // From its start on the GPU to its end, so with whatever the compositor did in
            // between (it takes the GPU at every refresh): the worst is mostly that.
            std::snprintf(copy_text, sizeof(copy_text),
                          "; a frame's copy took %.2f ms of GPU time from start to end, %.2f at "
                          "worst",
                          copy.mean_ms, copy.worst_ms);
        }
        LOGI("pacing: game %.1f fps, display %.1f Hz; emulator %.0f%% of a processor on %s, "
             "%d MB; head tracked %.0f%%; picture shown at %ux%u an eye (the system recommends "
             "%dx%d)%s",
             static_cast<float>(frame_count - reported_frames) / seconds,
             static_cast<float>(display_frames) / seconds,
             (usage.cpu_seconds - reported_cpu_seconds) * 100.0 / seconds,
             usage.cpus.empty() ? "?" : usage.cpus.c_str(), usage.memory_mb,
             100.0f * static_cast<float>(head_tracked_frames) / static_cast<float>(display_frames),
             shown_width / 2, shown_height, recommended_width, recommended_height, copy_text);
        if (head_tracked_frames != 0) {
            // Where the head is in the headset's own space, which the emulator counts from the
            // player's seat: a view that is off shows here as a head that has moved away from
            // where it was when the view was last reset.
            const XrVector3f forward = Rotate(head_pose.orientation, {0.0f, 0.0f, -1.0f});
            LOGI("head: at %.2f %.2f %.2f of the headset's space (right, up, back), within "
                 "%.2f %.2f %.2f of it these ten seconds; facing %.0f degrees to the left of its "
                 "straight ahead, %.0f up",
                 head_pose.position.x, head_pose.position.y, head_pose.position.z,
                 head_high.x - head_low.x, head_high.y - head_low.y, head_high.z - head_low.z,
                 std::atan2(-forward.x, -forward.z) * 57.29578f,
                 std::asin(std::clamp(forward.y, -1.0f, 1.0f)) * 57.29578f);
        }
        if (has_hand_tracking) {
            // Where the controller was taken to be, seen from the head: to the right, up and
            // forward of it. The game's first screen wants it around 0, -0.17, 0.50.
            const float palms = palm_samples != 0 ? palm_distance / palm_samples : 0.0f;
            const float held = pad_samples != 0 ? 1.0f / static_cast<float>(pad_samples) : 0.0f;
            LOGI("hands: both seen %.0f%% of the time, %.2f m apart; holding the controller "
                 "%.0f%%, on average %.2f right, %.2f up, %.2f ahead of the head",
                 100.0f * static_cast<float>(palm_samples) / static_cast<float>(display_frames),
                 palms,
                 100.0f * static_cast<float>(pad_samples) / static_cast<float>(display_frames),
                 pad_from_head.x * held, pad_from_head.y * held, -pad_from_head.z * held);
        }
        if (held_samples != 0) {
            const HeldPad::Vec front = HeldPad::Rotate(held_orientation, {0.0f, 0.0f, -1.0f});
            LOGI("gamepad: no motion sensors say how it is held, so the hands holding it do: "
                 "%.0f%% of the time, now pointing %.0f degrees left and %.0f up",
                 100.0f * static_cast<float>(held_samples) / static_cast<float>(display_frames),
                 std::atan2(-front.x, -front.z) * Degrees,
                 std::asin(std::clamp(front.y, -1.0f, 1.0f)) * Degrees);
        }
        if (compare_samples >= 50) {
            // What tells how well the hands do as motion sensors: for a gamepad that has
            // both, how far the two are apart and how much that wanders.
            const double count = compare_samples;
            const auto spread = [&](double sum, double squares) {
                return std::sqrt(std::max(0.0, squares / count - (sum / count) * (sum / count)));
            };
            LOGI("hands against the gamepad's motion sensors: by the hands its front points "
                 "%.0f degrees higher (give or take %.0f) and its right side is %.0f degrees "
                 "higher (give or take %.0f) than by the sensors; %u samples",
                 compare_pitch / count, spread(compare_pitch, compare_pitch_squares),
                 compare_roll / count, spread(compare_roll, compare_roll_squares),
                 compare_samples);
        }
        if (controllers_used) {
            const float seen =
                controller_samples != 0 ? 1.0f / static_cast<float>(controller_samples) : 0.0f;
            LOGI("controllers: the headset's own play the game; the %s one, which is the "
                 "controller in the game, was seen %.0f%% of the time, on average %.2f right, "
                 "%.2f up, %.2f ahead of the head",
                 options.pad_hand == 0 ? "left" : "right",
                 100.0f * static_cast<float>(controller_samples) /
                     static_cast<float>(display_frames),
                 controller_from_head.x * seen, controller_from_head.y * seen,
                 -controller_from_head.z * seen);
        }
        ReportMetrics();
        reported_frames = frame_count;
        reported_cpu_seconds = usage.cpu_seconds;
        display_frames = 0;
        head_tracked_frames = 0;
        palm_samples = 0;
        palm_distance = 0.0f;
        pad_samples = 0;
        pad_from_head = {};
        held_samples = 0;
        compare_samples = 0;
        compare_pitch = compare_pitch_squares = compare_roll = compare_roll_squares = 0.0;
        controller_samples = 0;
        controller_from_head = {};
        last_report = now;
    }

    void SendHeadPose(XrTime display_time) {
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.next = &velocity;
        if (XR_SUCCEEDED(xrLocateSpace(view_space, local_space, display_time, &location)) &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0 &&
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
            Protocol::Pose pose;
            pose.position[0] = location.pose.position.x;
            pose.position[1] = location.pose.position.y;
            pose.position[2] = location.pose.position.z;
            pose.orientation[0] = location.pose.orientation.x;
            pose.orientation[1] = location.pose.orientation.y;
            pose.orientation[2] = location.pose.orientation.z;
            pose.orientation[3] = location.pose.orientation.w;
            if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) {
                pose.linear_velocity[0] = velocity.linearVelocity.x;
                pose.linear_velocity[1] = velocity.linearVelocity.y;
                pose.linear_velocity[2] = velocity.linearVelocity.z;
            }
            if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0) {
                pose.angular_velocity[0] = velocity.angularVelocity.x;
                pose.angular_velocity[1] = velocity.angularVelocity.y;
                pose.angular_velocity[2] = velocity.angularVelocity.z;
            }
            core.SendPose(pose);
            head_pose = location.pose;
            if (head_tracked_frames == 0) {
                head_low = head_high = location.pose.position;
            }
            head_low = {std::min(head_low.x, location.pose.position.x),
                        std::min(head_low.y, location.pose.position.y),
                        std::min(head_low.z, location.pose.position.z)};
            head_high = {std::max(head_high.x, location.pose.position.x),
                         std::max(head_high.y, location.pose.position.y),
                         std::max(head_high.z, location.pose.position.z)};
            ++head_tracked_frames;
        }

        // The real headset supplies both the projection and the lens adjustment. Learn its
        // full FOV while the startup menu is shown, before the title asks for its optics.
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = display_time;
        locate.space = view_space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        XrView eye_views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t view_count = 0;
        if (XR_SUCCEEDED(xrLocateViews(session, &locate, &view_state, 2, &view_count, eye_views)) &&
            view_count == 2) {
            // The emulated headset has mirrored optics. Enclose both eyes' runtime frusta
            // so 100% never clips either eye if the reported projections differ slightly.
            const std::array<float, 4> fov{
                std::max(-std::tan(eye_views[0].fov.angleLeft),
                         std::tan(eye_views[1].fov.angleRight)),
                std::max(std::tan(eye_views[0].fov.angleRight),
                         -std::tan(eye_views[1].fov.angleLeft)),
                std::max(std::tan(eye_views[0].fov.angleUp),
                         std::tan(eye_views[1].fov.angleUp)),
                std::max(-std::tan(eye_views[0].fov.angleDown),
                         -std::tan(eye_views[1].fov.angleDown)),
            };
            if (std::all_of(fov.begin(), fov.end(), [](float value) {
                    return std::isfinite(value) && value > 0.1f && value < 10.0f;
                })) {
                std::scoped_lock lock{host_status.optics_mutex};
                host_status.headset_fov = fov;
            }
            if ((view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
                return;
            }
            const float dx = eye_views[1].pose.position.x - eye_views[0].pose.position.x;
            const float dy = eye_views[1].pose.position.y - eye_views[0].pose.position.y;
            const float dz = eye_views[1].pose.position.z - eye_views[0].pose.position.z;
            const float measured = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(measured) || measured < 0.04f || measured > 0.09f) {
                return;
            }
            if (std::abs(measured - ipd) > 0.0002f || ++frames_since_optics > 300) {
                ipd = measured;
                frames_since_optics = 0;
                core.SendOptics(ipd);
            }
        }
    }

    struct Palm {
        XrVector3f position;
        /// The way the hand points, from the wrist to the knuckles: if the headset says.
        bool pointing_known;
        XrVector3f pointing;
    };

    /// Where the palm of a hand is, if the headset sees it.
    std::optional<Palm> LocatePalm(int hand, XrTime time) {
        XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locations.jointLocations = joints;
        XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        info.baseSpace = local_space;
        info.time = time;
        if (XR_FAILED(locate_hand_joints(hand_trackers[hand], &info, &locations)) ||
            locations.isActive != XR_TRUE) {
            return std::nullopt;
        }
        const auto& palm = joints[XR_HAND_JOINT_PALM_EXT];
        if ((palm.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
            return std::nullopt;
        }
        Palm result{palm.pose.position, false, {}};
        if ((palm.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
            // (A palm's -Z runs along the bone of its middle finger, towards the fingers.)
            result.pointing_known = true;
            result.pointing = Rotate(palm.pose.orientation, {0.0f, 0.0f, -1.0f});
        }
        return result;
    }

    /// For a gamepad that has motion sensors and is held in hands the headset sees: how far
    /// what the hands say of its attitude is from what its own sensors say, while it is held
    /// still enough for those to know which way is up. Only counted, for the log.
    void CompareWithSensors(const HeldPad::Pose& held) {
        const float* accel = pad_view.gamepad_accel;
        const float gravity = std::hypot(accel[0], accel[1], accel[2]);
        if (!held.pitch_measured || !(gravity > 0.8f * 9.81f && gravity < 1.2f * 9.81f)) {
            return;
        }
        // What is up, seen from the gamepad (+X its right, +Y out of its face, +Z towards
        // the player), tells how far its front and its right side are above level.
        const HeldPad::Vec front = HeldPad::Rotate(held.orientation, {0.0f, 0.0f, -1.0f});
        const HeldPad::Vec right = HeldPad::Rotate(held.orientation, {1.0f, 0.0f, 0.0f});
        const auto angle = [](float sine) {
            return std::asin(std::clamp(sine, -1.0f, 1.0f)) * Degrees;
        };
        const double pitch = angle(front.y) - angle(-accel[2] / gravity);
        const double roll = angle(right.y) - angle(accel[0] / gravity);
        ++compare_samples;
        compare_pitch += pitch;
        compare_pitch_squares += pitch * pitch;
        compare_roll += roll;
        compare_roll_squares += roll * roll;
    }

    /// Says where the controller the game is played with is. One of the headset's own says
    /// that of itself. A gamepad cannot be tracked, but the hands that hold it can: both
    /// palms a controller's width apart give away where it is and which way it points, and
    /// for a gamepad without motion sensors also how it is tilted (see HeldPad).
    void SendPadPose(XrTime display_time) {
        Protocol::PadPose message;
        const uint32_t requests = recenter_requests.exchange(0) | pads.TakeRecenter();
        if ((requests & XrRecenter::Pad) != 0) {
            message.flags |= Protocol::PadPose::RecenterYaw;
        }
        // The headset's own "reset view" moves the space the poses are given in: from the
        // moment it says, the head is where it rests, and the emulator has to hear of it.
        const bool system_reset = space_change_time != 0 && display_time >= space_change_time;
        if (system_reset) {
            space_change_time = 0;
        }
        if ((requests & XrRecenter::Seat) != 0 || system_reset) {
            message.flags |= Protocol::PadPose::RecenterSeat;
            LOGI("view reset: %s", system_reset ? "the headset's own" : "asked for by the player");
        }
        if ((requests & XrRecenter::Pad) != 0 && panel_pose && !shown_frame) {
            // A reset the player asks for (OPTIONS held, both sticks in, the PS button; not
            // the first press of X, which only takes the seat) brings the start-up panel in
            // front of them as well.
            panel_pose.reset();
            panel_reason = "the player's view reset";
        }
        // The steps the player wants to be turned round by where they sit (a button held
        // and the right stick flicked, see PadRouter): one a message.
        turns_asked += pads.TakeTurn();
        if (turns_asked > 0) {
            --turns_asked;
            message.flags |= Protocol::PadPose::TurnRight;
        } else if (turns_asked < 0) {
            ++turns_asked;
            message.flags |= Protocol::PadPose::TurnLeft;
        }

        // For the log: a place as seen from the head.
        const auto from_head = [&](const XrVector3f& place) {
            const XrQuaternionf to_head{-head_pose.orientation.x, -head_pose.orientation.y,
                                        -head_pose.orientation.z, head_pose.orientation.w};
            return Rotate(to_head, {place.x - head_pose.position.x, place.y - head_pose.position.y,
                                    place.z - head_pose.position.z});
        };

        bool seen = false;
        bool hands_seen = false;
        if (controllers_used) {
            // (Hands that hold the headset's own controllers hold no gamepad.)
            if (controller_located) {
                message.flags |=
                    Protocol::PadPose::PositionValid | Protocol::PadPose::OrientationValid;
                message.position[0] = controller_pose.position.x;
                message.position[1] = controller_pose.position.y;
                message.position[2] = controller_pose.position.z;
                message.orientation[0] = controller_pose.orientation.x;
                message.orientation[1] = controller_pose.orientation.y;
                message.orientation[2] = controller_pose.orientation.z;
                message.orientation[3] = controller_pose.orientation.w;
                message.linear_velocity[0] = controller_linear.x;
                message.linear_velocity[1] = controller_linear.y;
                message.linear_velocity[2] = controller_linear.z;
                message.angular_velocity[0] = controller_angular.x;
                message.angular_velocity[1] = controller_angular.y;
                message.angular_velocity[2] = controller_angular.z;
                const XrVector3f offset = from_head(controller_pose.position);
                controller_from_head.x += offset.x;
                controller_from_head.y += offset.y;
                controller_from_head.z += offset.z;
                ++controller_samples;
                seen = true;
            }
        } else if (has_hand_tracking) {
            const auto left = LocatePalm(0, display_time);
            const auto right = LocatePalm(1, display_time);
            if (left && right) {
                HeldPad::Hands hands;
                hands.left_palm = {left->position.x, left->position.y, left->position.z};
                hands.right_palm = {right->position.x, right->position.y, right->position.z};
                hands.pointing_known = left->pointing_known && right->pointing_known;
                hands.left_pointing = {left->pointing.x, left->pointing.y, left->pointing.z};
                hands.right_pointing = {right->pointing.x, right->pointing.y, right->pointing.z};
                ++palm_samples;
                palm_distance += HeldPad::Length(HeldPad::Sub(hands.right_palm, hands.left_palm));
                // Hands further apart than a controller is wide, or closer together, are not
                // holding one.
                if (const auto held =
                        HeldPad::Locate(hands, options.pad_tilt / Degrees, held_pitch)) {
                    const XrVector3f centre{held->position.x, held->position.y,
                                            held->position.z};
                    if (held->turned_known) {
                        message.yaw = held->yaw;
                        message.flags |= Protocol::PadPose::YawValid;
                    }

                    if (hands_held && display_time > pad_time) {
                        const float elapsed =
                            static_cast<float>(display_time - pad_time) * 1e-9f;
                        if (elapsed < 0.1f) {
                            const float blend = 0.4f;
                            pad_velocity.x +=
                                ((centre.x - pad_position.x) / elapsed - pad_velocity.x) * blend;
                            pad_velocity.y +=
                                ((centre.y - pad_position.y) / elapsed - pad_velocity.y) * blend;
                            pad_velocity.z +=
                                ((centre.z - pad_position.z) / elapsed - pad_velocity.z) * blend;
                        }
                    } else {
                        pad_velocity = {};
                    }
                    pad_position = centre;
                    pad_time = display_time;

                    const XrVector3f offset = from_head(centre);
                    pad_from_head.x += offset.x;
                    pad_from_head.y += offset.y;
                    pad_from_head.z += offset.z;
                    ++pad_samples;

                    message.flags |= Protocol::PadPose::PositionValid;
                    message.position[0] = centre.x;
                    message.position[1] = centre.y;
                    message.position[2] = centre.z;
                    message.linear_velocity[0] = pad_velocity.x;
                    message.linear_velocity[1] = pad_velocity.y;
                    message.linear_velocity[2] = pad_velocity.z;

                    if (held->turned_known) {
                        if (held->pitch_measured) {
                            held_pitch = held->pitch;
                        }
                        // Hands tremble, and what is seen of them more so: what is passed
                        // on follows them a little behind. After a gap it starts afresh.
                        const float elapsed =
                            held_shown && display_time > held_time
                                ? static_cast<float>(display_time - held_time) * 1e-9f
                                : 1.0f;
                        held_orientation =
                            elapsed < 0.25f
                                ? HeldPad::Towards(held_orientation, held->orientation,
                                                   1.0f - std::exp(-elapsed / HeldFollowTime))
                                : held->orientation;
                        held_shown = true;
                        held_time = display_time;
                        if (!pad_view.gamepad_motion) {
                            // No motion sensors say how the gamepad is held (it has none,
                            // or there is no gamepad at all yet): the hands do.
                            message.flags |= Protocol::PadPose::OrientationValid |
                                             Protocol::PadPose::HeldInHands;
                            message.orientation[0] = held_orientation.x;
                            message.orientation[1] = held_orientation.y;
                            message.orientation[2] = held_orientation.z;
                            message.orientation[3] = held_orientation.w;
                            ++held_samples;
                        } else {
                            CompareWithSensors(*held);
                        }
                    }
                    seen = true;
                    hands_seen = true;
                }
            }
        }

        // While the controller is seen, every frame; once more when it is lost, so that the
        // emulator stops trusting the last position.
        if (seen || pad_seen || message.flags != 0) {
            core.SendPadPose(message);
        }
        pad_seen = seen;
        hands_held = hands_seen;
        host_status.hands_tracked = hands_seen;
        host_status.controller_tracked = controllers_used && seen;
    }

    /// Describes the shown frame to the compositor: where the head was when the emulator drew
    /// it and with what field of view, so that it can be corrected for how the head has moved.
    /// `width` by `height` is the part of the image the frame (both eyes) was copied to.
    void FillProjection(const Protocol::Frame& frame, uint32_t width, uint32_t height,
                        XrCompositionLayerProjectionView* views,
                        XrCompositionLayerProjection& projection) const {
        // The runtime refuses a layer whose orientation is not a rotation, and with it the
        // whole frame: whatever arrives is brought to length one, and nothing usable becomes
        // "straight ahead".
        XrQuaternionf orientation{frame.orientation[0], frame.orientation[1],
                                  frame.orientation[2], frame.orientation[3]};
        const float length =
            std::sqrt(orientation.x * orientation.x + orientation.y * orientation.y +
                      orientation.z * orientation.z + orientation.w * orientation.w);
        if (std::isfinite(length) && length > 0.5f) {
            orientation = {orientation.x / length, orientation.y / length, orientation.z / length,
                           orientation.w / length};
        } else {
            orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        }
        const float angle_out = std::atan(frame.fov[0]);
        const float angle_in = std::atan(frame.fov[1]);
        const float angle_up = std::atan(frame.fov[2]);
        const float angle_down = std::atan(frame.fov[3]);
        const int32_t eye_width = static_cast<int32_t>(width / 2);

        for (int eye = 0; eye < 2; ++eye) {
            const float side = eye == 0 ? -0.5f : 0.5f;
            const XrVector3f offset = Rotate(orientation, {side * ipd, 0.0f, 0.0f});
            views[eye].pose.orientation = orientation;
            views[eye].pose.position = {frame.position[0] + offset.x,
                                        frame.position[1] + offset.y,
                                        frame.position[2] + offset.z};
            // "Out" is towards the temple: left for the left eye, right for the right one.
            views[eye].fov.angleLeft = eye == 0 ? -angle_out : -angle_in;
            views[eye].fov.angleRight = eye == 0 ? angle_in : angle_out;
            views[eye].fov.angleUp = angle_up;
            views[eye].fov.angleDown = -angle_down;
            views[eye].subImage.swapchain = game_swapchain.handle;
            views[eye].subImage.imageRect.offset = {eye * eye_width, 0};
            views[eye].subImage.imageRect.extent = {eye_width, static_cast<int32_t>(height)};
        }
        projection.space = local_space;
        projection.viewCount = 2;
        projection.views = views;
    }

    void Shutdown() {
        frames.Destroy(gl.display);
        blitter.Destroy();
        copy_timer.Destroy();
        if (instance != XR_NULL_HANDLE) {
            const auto destroy_hand_tracker =
                GetFunction<PFN_xrDestroyHandTrackerEXT>(instance, "xrDestroyHandTrackerEXT");
            for (XrHandTrackerEXT tracker : hand_trackers) {
                if (tracker != XR_NULL_HANDLE && destroy_hand_tracker != nullptr) {
                    destroy_hand_tracker(tracker);
                }
            }
        }
        for (Swapchain* swapchain : {&game_swapchain, &status_swapchain}) {
            if (swapchain->handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain->handle);
            }
        }
        if (controller_space != XR_NULL_HANDLE) {
            xrDestroySpace(controller_space);
        }
        if (action_set != XR_NULL_HANDLE) {
            // (Its actions go with it.)
            xrDestroyActionSet(action_set);
        }
        if (local_space != XR_NULL_HANDLE) {
            xrDestroySpace(local_space);
        }
        if (view_space != XR_NULL_HANDLE) {
            xrDestroySpace(view_space);
        }
        if (session != XR_NULL_HANDLE) {
            xrDestroySession(session);
        }
        if (instance != XR_NULL_HANDLE) {
            xrDestroyInstance(instance);
        }
        gl.Destroy();
    }

    JavaVM* vm;
    jobject activity;
    CoreProcess& core;
    PadRouter& pads;
    StatusImage& status;
    XrHostOptions options;
    XrHostStatus& host_status;
    std::atomic<bool>& quit;
    std::atomic<uint32_t>& recenter_requests;

    GlContext gl;

    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSpace local_space{XR_NULL_HANDLE};
    XrSpace view_space{XR_NULL_HANDLE};
    bool has_refresh_rate{};
    bool has_swapchain_state{};
    int32_t recommended_width{};
    int32_t recommended_height{};
    bool has_performance{};
    bool has_thread_settings{};
    bool has_layer_settings{};
    bool has_auto_filter{};
    bool has_hand_tracking{};
    bool has_layer_resolution{};
    bool has_metrics{};
    bool has_time_conversion{};
    bool session_running{};
    bool exit_requested{};
    bool probed{};
    uint32_t frame_failures{};
    bool shown_logged{};
    bool game_shown_logged{};
    uint32_t max_swapchain_width{};

    XrHandTrackerEXT hand_trackers[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    PFN_xrLocateHandJointsEXT locate_hand_joints{};
    /// When the headset's system redefines the space poses are given in (its "reset view"),
    /// or 0.
    XrTime space_change_time{};
    int turns_asked{};
    bool pad_seen{};
    /// The hands were seen holding a gamepad in the frame before.
    bool hands_held{};
    XrVector3f pad_position{};
    XrVector3f pad_velocity{};
    XrTime pad_time{};

    static constexpr float Degrees = 57.29578f;

    // The headset's own controllers, which play the game where the player holds them
    // instead of a gamepad.
    XrActionSet action_set{XR_NULL_HANDLE};
    XrAction act_move{XR_NULL_HANDLE};
    XrAction act_finger{XR_NULL_HANDLE};
    XrAction act_left_in{XR_NULL_HANDLE};
    XrAction act_right_in{XR_NULL_HANDLE};
    XrAction act_a{XR_NULL_HANDLE};
    XrAction act_b{XR_NULL_HANDLE};
    XrAction act_x{XR_NULL_HANDLE};
    XrAction act_y{XR_NULL_HANDLE};
    XrAction act_menu{XR_NULL_HANDLE};
    XrAction act_left_grip{XR_NULL_HANDLE};
    XrAction act_right_grip{XR_NULL_HANDLE};
    XrAction act_left_trigger{XR_NULL_HANDLE};
    XrAction act_right_trigger{XR_NULL_HANDLE};
    XrAction act_pose{XR_NULL_HANDLE};
    XrAction act_rumble{XR_NULL_HANDLE};
    XrPath hand_paths[2]{XR_NULL_PATH, XR_NULL_PATH};
    XrSpace controller_space{XR_NULL_HANDLE};
    bool actions_ready{};
    /// What the game is played with, as of this frame.
    PadRouter::View pad_view;
    bool controllers_used{};
    bool layout_logged{};
    bool controller_located{};
    XrPosef controller_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {}};
    XrVector3f controller_linear{};
    XrVector3f controller_angular{};
    uint32_t rumble_applied{};
    std::chrono::steady_clock::time_point rumble_time{};

    // A gamepad as the hands holding it turn and tilt it.
    /// How quickly what is passed on follows what the hands say, in seconds.
    static constexpr float HeldFollowTime = 0.06f;
    HeldPad::Quat held_orientation;
    bool held_shown{};
    XrTime held_time{};
    /// How far up the hands pointed when the headset could last tell: about how a gamepad
    /// is held, before it ever could.
    float held_pitch{0.35f};

    struct Metric {
        XrPath path;
        std::string name;
    };
    std::vector<Metric> metrics;
    PFN_xrQueryPerformanceMetricsCounterMETA query_metric{};
    PFN_xrRequestDisplayRefreshRateFB request_refresh_rate{};
    // The refresh rate the startup menu last asked for while the session ran (0: none).
    float asked_refresh_rate{};

    /// The least of the image the game's picture is shown at, whatever the system recommends.
    static constexpr float MinLayerScale = 0.85f;
    PFN_xrGetRecommendedLayerResolutionMETA get_layer_resolution{};
    bool layer_size_logged{};
    /// The part of the game's image the next frame is copied to, and the part the frame that
    /// is being shown was copied to: both eyes, side by side from the lower left corner.
    uint32_t layer_width{};
    uint32_t layer_height{};
    uint32_t shown_width{};
    uint32_t shown_height{};

    int64_t swapchain_format{FormatRgba8};
    bool decode_srgb{};
    Swapchain game_swapchain;
    Swapchain status_swapchain;
    FrameBlitter blitter;
    GpuTimer copy_timer;
    FrameBuffers frames;

    std::optional<Protocol::Frame> shown_frame;
    std::chrono::steady_clock::time_point last_frame_time{};
    std::chrono::steady_clock::time_point last_report{std::chrono::steady_clock::now()};
    uint32_t frame_count{};
    uint32_t reported_frames{};
    uint32_t display_frames{};
    double reported_cpu_seconds{};
    // What the log says of the last ten seconds.
    XrPosef head_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {}};
    // The corners of the box the head stayed in since the last report.
    XrVector3f head_low{};
    XrVector3f head_high{};
    uint32_t head_tracked_frames{};
    uint32_t palm_samples{};
    float palm_distance{};
    uint32_t pad_samples{};
    XrVector3f pad_from_head{};
    uint32_t held_samples{};
    uint32_t compare_samples{};
    double compare_pitch{};
    double compare_pitch_squares{};
    double compare_roll{};
    double compare_roll_squares{};
    uint32_t controller_samples{};
    XrVector3f controller_from_head{};
    uint64_t status_version{};
    bool status_ready{};
    /// Where the panel stands in the seated space while the game has no picture yet (the
    /// start-up menu and what follows it): still in the room, not following the head. Placed
    /// the first time it is shown, and again after a reset of the view or when the headset
    /// is put back on (see PlacePanel).
    std::optional<XrPosef> panel_pose;
    /// When the headset's own reset of the view moves the seated space (0: it does not): the
    /// panel is placed afresh from then on.
    XrTime panel_change_time{};
    /// What the next placing of the panel is for, for the log.
    const char* panel_reason{"first shown"};
    bool panel_wait_logged{};
    /// The panel standing in the room: as large to the eye as the one that followed the head
    /// (47 by 25 degrees; its 1024 pixels at 22 a degree, against the display's 25 or so), 1.5 m
    /// away and a hand's breadth below the eyes (its middle 4 degrees down).
    static constexpr float PanelDistance = 1.5f;
    static constexpr float PanelDrop = 0.1f;
    static constexpr XrExtent2Df PanelSize{1.3f, 0.65f};
    float ipd{0.063f};
    uint32_t frames_since_optics{};
};

} // namespace

void RunXrHost(JavaVM* vm, jobject activity, CoreProcess& core, PadRouter& pads,
               StatusImage& status, const XrHostOptions& options, XrHostStatus& host_status,
               std::atomic<bool>& quit, std::atomic<uint32_t>& recenter_requests) {
    Host host{vm, activity, core, pads, status, options, host_status, quit, recenter_requests};
    host.Run();
}
