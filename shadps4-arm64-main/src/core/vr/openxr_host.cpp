// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>

#include <mmdeviceapi.h>
#include <objbase.h>
#include <propidl.h>
#include <unknwn.h>
#endif

#include <SDL3/SDL_events.h>

#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/vr/openxr_host.h"
#include "input/controller.h"
#include "input/pad_source.h"
#include "input/scripted_input.h"
#include "input/pad_gestures.h"
#include "input/stick_finger.h"
#include "input/view_turn.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef _WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "core/vr/openxr_view.h"

namespace Core::Vr {

namespace {

using Clock = std::chrono::steady_clock;

/// Images the title's frames are drawn into on their way to the headset: one being drawn, one
/// waiting to be shown, one being copied, and one to spare.
constexpr u32 NumSlots = 4;
constexpr u32 NumCommandBuffers = 4;
/// Pictures smaller than this, either way, are a title's way of showing nothing.
constexpr u32 MinFrameSize = 64;

/// What runtimes are known to want of a Vulkan device they share images with. It is
/// enabled whenever a runtime is there, so that a headset that turns up after the renderer has
/// made its device (the player starts the game first and puts the headset on then) can still be
/// drawn to; what the runtime itself asks for comes on top once it can be asked.
constexpr const char* UsualInstanceExtensions[] = {
    "VK_KHR_get_physical_device_properties2",
    "VK_KHR_external_memory_capabilities",
    "VK_KHR_external_semaphore_capabilities",
    "VK_KHR_external_fence_capabilities",
};
constexpr const char* UsualDeviceExtensions[] = {
    "VK_KHR_dedicated_allocation",    "VK_KHR_get_memory_requirements2",
    "VK_KHR_bind_memory2",            "VK_KHR_external_memory",
#ifdef _WIN32
    "VK_KHR_external_memory_win32",   "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_win32", "VK_KHR_external_fence",
    "VK_KHR_external_fence_win32",    "VK_KHR_timeline_semaphore",
    "VK_KHR_win32_keyed_mutex",       "VK_KHR_image_format_list",
#else
    "VK_KHR_external_memory_fd",      "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_fd",   "VK_KHR_external_fence",
    "VK_KHR_external_fence_fd",       "VK_KHR_timeline_semaphore",
    "VK_KHR_image_format_list",
#endif
    "VK_KHR_maintenance1",            "VK_KHR_maintenance2",
    "VK_KHR_multiview",               "VK_KHR_create_renderpass2",
};

const char* StateName(XrSessionState state) {
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

std::string ResultText(XrInstance instance, XrResult result) {
    char buffer[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, buffer))) {
        return buffer;
    }
    return fmt::format("{}", static_cast<int>(result));
}

template <typename Function>
Function GetFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    xrGetInstanceProcAddr(instance, name, &function);
    return reinterpret_cast<Function>(function);
}

std::vector<std::string> SplitNames(const std::string& names) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < names.size()) {
        const size_t end = std::min(names.find(' ', start), names.size());
        // The lists come with their terminating zero counted.
        std::string name = names.substr(start, end - start);
        while (!name.empty() && name.back() == '\0') {
            name.pop_back();
        }
        if (!name.empty()) {
            result.push_back(std::move(name));
        }
        start = end + 1;
    }
    return result;
}

void AddName(std::vector<std::string>& names, const std::string& name) {
    if (std::ranges::find(names, name) == names.end()) {
        names.push_back(name);
    }
}

float EnvFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? static_cast<float>(std::atof(value)) : fallback;
}

bool EnvFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value[0] != '0' : fallback;
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const Vec3 rotated = Core::Vr::Rotate(Quat{q.x, q.y, q.z, q.w}, Vec3{v.x, v.y, v.z});
    return {rotated.x, rotated.y, rotated.z};
}

#ifdef _WIN32
std::string Narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

/// The name Windows lists a sound device under, for the identifier a VR runtime gives for the
/// headset's: either the device's own identifier or the GUID inside it.
std::string AudioDeviceName(const wchar_t* identifier, EDataFlow flow) {
    static constexpr PROPERTYKEY FriendlyName{
        {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

    if (identifier == nullptr || identifier[0] == L'\0') {
        return {};
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string name;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator)))) {
        const auto name_of = [&](IMMDevice* device) {
            IPropertyStore* properties = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
                    name = Narrow(value.pwszVal);
                }
                PropVariantClear(&value);
                properties->Release();
            }
        };
        IMMDevice* device = nullptr;
        if (SUCCEEDED(enumerator->GetDevice(identifier, &device)) && device != nullptr) {
            name_of(device);
            device->Release();
        } else {
            // Only the GUID: the device whose identifier ends with it.
            std::wstring wanted{identifier};
            std::ranges::transform(wanted, wanted.begin(), ::towlower);
            IMMDeviceCollection* devices = nullptr;
            if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices))) {
                UINT count = 0;
                devices->GetCount(&count);
                for (UINT i = 0; i < count && name.empty(); ++i) {
                    IMMDevice* candidate = nullptr;
                    if (FAILED(devices->Item(i, &candidate)) || candidate == nullptr) {
                        continue;
                    }
                    LPWSTR id = nullptr;
                    if (SUCCEEDED(candidate->GetId(&id)) && id != nullptr) {
                        std::wstring text{id};
                        std::ranges::transform(text, text.begin(), ::towlower);
                        if (text.find(wanted) != std::wstring::npos) {
                            name_of(candidate);
                        }
                        CoTaskMemFree(id);
                    }
                    candidate->Release();
                }
                devices->Release();
            }
        }
        enumerator->Release();
    }
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    return name;
}

/// The name of the sound device in use whose name ends that way, empty if there is none.
std::string ActiveAudioDeviceEnding(std::string_view ending, EDataFlow flow) {
    static constexpr PROPERTYKEY FriendlyName{
        {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

    if (ending.empty()) {
        return {};
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string found;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator)))) {
        IMMDeviceCollection* devices = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices))) {
            UINT count = 0;
            devices->GetCount(&count);
            for (UINT i = 0; i < count && found.empty(); ++i) {
                IMMDevice* device = nullptr;
                if (FAILED(devices->Item(i, &device)) || device == nullptr) {
                    continue;
                }
                IPropertyStore* properties = nullptr;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                    PROPVARIANT value;
                    PropVariantInit(&value);
                    if (SUCCEEDED(properties->GetValue(FriendlyName, &value)) &&
                        value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
                        if (std::string name = Narrow(value.pwszVal); name.ends_with(ending)) {
                            found = std::move(name);
                        }
                    }
                    PropVariantClear(&value);
                    properties->Release();
                }
                device->Release();
            }
            devices->Release();
        }
        enumerator->Release();
    }
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    return found;
}
#endif

} // namespace

struct OpenXrHost::Impl {
    struct Slot {
        enum class State { Free, Drawing, Ready, Reading };

        vk::Image image;
        vk::DeviceMemory memory;
        vk::ImageView view;
        u32 width{};
        u32 height{};
        State state{State::Free};
        PresentedFrame info;
    };

    struct Retired {
        vk::Image image;
        vk::DeviceMemory memory;
        vk::ImageView view;
        Clock::time_point since;
    };

    // Settings.
    bool enabled{true};
    bool track_hands{true};
    bool feed_head{true};
    float predict_ms{20.0f};

    // What Connect found.
    bool available{};
    bool found_at_connect{};
    bool hand_extension{};
    bool has_hand_tracking{};
    bool has_refresh_rate{};
    bool has_audio_guid{};
    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system{XR_NULL_SYSTEM_ID};
    std::string runtime_name;
    mutable std::mutex names_mutex;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    std::string audio_output;
    std::string audio_input;
    bool audio_names_known{};
    // When the runtime is to be asked for the headset's sound devices again (the frame
    // thread's clock, 0: not at all).
    std::atomic<s64> audio_ask_at{0};

    // The session, all of it the frame thread's.
    Graphics graphics{};
    std::jthread thread;
    bool device_mismatch{};
    XrSession session{XR_NULL_HANDLE};
    XrSpace local_space{XR_NULL_HANDLE};
    XrSpace view_space{XR_NULL_HANDLE};
    XrHandTrackerEXT hand_trackers[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    PFN_xrLocateHandJointsEXT locate_hand_joints{};
    PFN_xrGetDisplayRefreshRateFB get_refresh_rate{};
    XrSessionState state{XR_SESSION_STATE_UNKNOWN};
    Clock::time_point state_since;
    Clock::time_point session_started;
    bool was_focused{};
    bool waiting_for_focus{};
    u32 session_failures{};
    bool reported_worn{true};
    bool session_running{};
    bool session_lost{};
    bool instance_lost{};
    u32 system_failures{};
    Clock::time_point last_wanted;
    bool ever_wanted{};
    /// Whether a session that is asked for no pictures is made anew after a while (see
    /// CheckWanted): for the runtime that says nothing when its headset goes away.
    bool renew_unwanted{};
    Clock::time_point last_frame_ended;
    float lose_after{};
    bool pause_when_away{true};
    static constexpr std::chrono::seconds FirstIdlePatience{30};
    static constexpr std::chrono::seconds LongestIdlePatience{300};
    std::chrono::seconds idle_patience{FirstIdlePatience};
    static constexpr std::chrono::seconds FirstFocusPatience{3};
    std::chrono::seconds focus_patience{FirstFocusPatience};
    float unwanted_from{};
    float unwanted_to{};
    float hide_for{};
    Clock::time_point connect_time;
    bool exit_requested{};
    XrTime space_change_time{};
    u32 max_swapchain_width{};
    u32 max_swapchain_height{};
    Clock::time_point last_search;
    bool search_reported{};

    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkanKHR> swapchain_images;
    u32 swapchain_width{};
    u32 swapchain_height{};
    vk::Format swapchain_format{vk::Format::eUndefined};
    bool swapchain_failed{};

    vk::CommandPool command_pool;
    std::array<vk::CommandBuffer, NumCommandBuffers> command_buffers;
    std::array<vk::Fence, NumCommandBuffers> fences;
    u32 next_command_buffer{};

    // The title's frames on their way here.
    std::mutex slot_mutex;
    std::array<Slot, NumSlots> slots;
    s32 latest{-1};
    u32 next_slot{};
    std::vector<Retired> retired;
    std::atomic<bool> accepting{};
    std::atomic<bool> showing{};
    std::atomic<u32> delivered_frames{};
    // (Under slot_mutex.) How long the title's frames were apart, for the log.
    Clock::time_point last_delivery;
    double longest_gap{};
    u32 long_gaps{};

    // What is being shown.
    bool have_frame{};
    PresentedFrame shown{};
    u32 shown_width{};
    u32 shown_height{};
    float ipd{0.063f};
    u32 frames_since_optics{};
    u32 fov_samples{};

    // The headset's own controllers, which stand in for a gamepad where the PC has none, or
    // has one that is not the one in use (see Input::PadSource).
    bool use_controllers{true};
    // What of them was in use at the last look (the buttons that play, the sticks).
    u32 controllers_in_use{};
    int pad_hand{1};
    XrVector3f pad_offset{};
    XrActionSet action_set{XR_NULL_HANDLE};
    XrAction act_move{XR_NULL_HANDLE};
    XrAction act_finger{XR_NULL_HANDLE};
    XrAction act_cross{XR_NULL_HANDLE};
    XrAction act_square{XR_NULL_HANDLE};
    XrAction act_circle{XR_NULL_HANDLE};
    XrAction act_triangle{XR_NULL_HANDLE};
    XrAction act_l1{XR_NULL_HANDLE};
    XrAction act_r1{XR_NULL_HANDLE};
    XrAction act_l2{XR_NULL_HANDLE};
    XrAction act_r2{XR_NULL_HANDLE};
    XrAction act_options{XR_NULL_HANDLE};
    XrAction act_l3{XR_NULL_HANDLE};
    XrAction act_finger_press{XR_NULL_HANDLE};
    XrAction act_pose{XR_NULL_HANDLE};
    XrAction act_grip{XR_NULL_HANDLE};
    XrAction act_rumble{XR_NULL_HANDLE};
    XrPath hand_paths[2]{XR_NULL_PATH, XR_NULL_PATH};
    XrSpace controller_space{XR_NULL_HANDLE};
    // Where each hand holds something, as far as the headset's runtime says where its own
    // controllers are: with a gamepad in both hands and those controllers put away, a runtime
    // that makes controllers up from the hands it sees (Virtual Desktop does) says where the
    // hands are, which is where the gamepad is.
    bool use_grips{true};
    XrSpace grip_spaces[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool actions_ready{};
    bool actions_synced{};
    XrVector3f grip_rest{};
    Clock::time_point grip_moved{};
    // What the gamepad is placed by: 0 nothing, 1 the joints of the hands, 2 the above.
    int pad_source{};
    bool controllers_used{};
    bool controller_tracked{};
    bool view_reset_held{};
    Libraries::Pad::OrbisPadButtonDataOffset sent_buttons{};
    std::array<int, 6> sent_axes{128, 128, 128, 128, 0, 0};
    bool sent_touch{};
    float sent_touch_x{0.5f};
    float sent_touch_y{0.5f};
    Input::StickFinger stick_finger;
    Input::PadGestures gestures;
    Input::ViewTurn view_turn;
    bool blowing{};
    std::atomic<u32> rumble_wanted{};
    u32 rumble_applied{};
    Clock::time_point rumble_time;

    // The controller, as far as hands around it give it away.
    bool pad_seen{};
    XrVector3f pad_position{};
    XrVector3f pad_velocity{};
    XrTime pad_time{};

    // For the log.
    bool motion_warned{};
    Clock::time_point last_report{};
    u32 display_frames{};
    u32 reported_delivered{};
    u32 copied_frames{};
    u32 head_tracked_frames{};
    u32 palm_samples{};
    float palm_distance{};
    u32 pad_samples{};
    XrVector3f pad_from_head{};
    u32 controller_samples{};
    XrPosef controller_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {}};
    XrPosef head_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {}};
    double end_frame_time{};
    double end_frame_worst{};
    double copy_time{};
    float correction{};
    float correction_worst{};
    u32 correction_samples{};
    u32 frame_failures{};
    bool first_shown_logged{};
    float refresh_rate{};

    ~Impl() {
        if (thread.joinable()) {
            thread.request_stop();
            thread.join();
        }
    }

    bool CreateInstance() {
        uint32_t count = 0;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) {
            return false;
        }
        std::vector<XrExtensionProperties> offered(count, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, count, &count, offered.data());
        const auto has = [&](const char* name) {
            return std::ranges::any_of(offered, [&](const auto& property) {
                return std::strcmp(property.extensionName, name) == 0;
            });
        };
        if (!has(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
            LOG_WARNING(Core_Vr, "The OpenXR runtime does not take pictures drawn with Vulkan");
            return false;
        }
        std::vector<const char*> extensions{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
        const auto enable_if_offered = [&](const char* name) {
            const bool offered_one = has(name);
            if (offered_one) {
                extensions.push_back(name);
            }
            return offered_one;
        };
        hand_extension = track_hands && enable_if_offered(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        has_refresh_rate = enable_if_offered(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
#ifdef _WIN32
        has_audio_guid = enable_if_offered(XR_OCULUS_AUDIO_DEVICE_GUID_EXTENSION_NAME);
#endif

        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(info.applicationInfo.applicationName, "shadPS4");
        info.applicationInfo.applicationVersion = 1;
        std::strcpy(info.applicationInfo.engineName, "shadPS4");
        info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.enabledExtensionNames = extensions.data();
        const XrResult result = xrCreateInstance(&info, &instance);
        if (XR_FAILED(result)) {
            LOG_WARNING(Core_Vr, "The OpenXR runtime refused an instance: {}",
                        ResultText(XR_NULL_HANDLE, result));
            instance = XR_NULL_HANDLE;
            return false;
        }
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_FAILED(xrGetInstanceProperties(instance, &properties))) {
            LOG_WARNING(Core_Vr, "The OpenXR runtime did not report its identity");
            DestroyInstance();
            return false;
        }
        runtime_name = fmt::format("{} {}.{}.{}", properties.runtimeName,
                                   XR_VERSION_MAJOR(properties.runtimeVersion),
                                   XR_VERSION_MINOR(properties.runtimeVersion),
                                   XR_VERSION_PATCH(properties.runtimeVersion));
        // (SHADPS4_XR_RENEW_UNWANTED=1 or 0 says so for any runtime, for tests.)
        const char* renew = std::getenv("SHADPS4_XR_RENEW_UNWANTED");
        renew_unwanted = renew != nullptr && renew[0] != '\0'
                             ? renew[0] != '0'
                             : runtime_name.starts_with("VirtualDesktopXR");
        return true;
    }

    /// Asks the runtime for its headset. False while there is none.
    bool FindSystem() {
        Runtime::Instance().SetHeadsetIdentity({});
        // SHADPS4_XR_HIDE_FOR=<seconds>, for tests: the runtime's headset is not there for
        // that long after the start, as Virtual Desktop's is not until the headset connects.
        if (hide_for > 0.0f &&
            Clock::now() - connect_time < std::chrono::duration<float>(hide_for)) {
            system = XR_NULL_SYSTEM_ID;
            return false;
        }
        XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
        info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        const XrResult result = xrGetSystem(instance, &info, &system);
        if (XR_FAILED(result)) {
            system = XR_NULL_SYSTEM_ID;
            if (result != XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
                // (No headset is what a runtime says while none is connected. Anything else,
                // again and again: the runtime itself is started over with.)
                if (++system_failures <= 3 || system_failures % 40 == 0) {
                    LOG_WARNING(Core_Vr, "Asking {} for its headset failed: {} ({} times)",
                                runtime_name, ResultText(instance, result), system_failures);
                }
                if (result == XR_ERROR_INSTANCE_LOST || system_failures % 5 == 0) {
                    instance_lost = true;
                }
            }
            return false;
        }
        system_failures = 0;

        XrSystemHandTrackingPropertiesEXT hand_properties{
            XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        if (hand_extension) {
            properties.next = &hand_properties;
        }
        if (XR_FAILED(xrGetSystemProperties(instance, system, &properties))) {
            LOG_WARNING(Core_Vr, "The OpenXR runtime did not report its headset's identity");
            system = XR_NULL_SYSTEM_ID;
            return false;
        }
        Runtime::Instance().SetHeadsetIdentity(
            {runtime_name, properties.systemName, properties.vendorId});
        fov_samples = 0;
        max_swapchain_width = properties.graphicsProperties.maxSwapchainImageWidth;
        max_swapchain_height = properties.graphicsProperties.maxSwapchainImageHeight;
        // (Asked every time: a headset that comes back may track hands where it did not.)
        const bool hands = hand_extension && hand_properties.supportsHandTracking == XR_TRUE;

        // What the runtime wants of the Vulkan side. It insists on being asked for its
        // requirements before a session is made.
        const auto get_requirements = GetFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>(
            instance, "xrGetVulkanGraphicsRequirementsKHR");
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        if (get_requirements != nullptr) {
            get_requirements(instance, system, &requirements);
        }
        const auto names_from = [&](const char* function_name) {
            using Function = XrResult(XRAPI_PTR*)(XrInstance, XrSystemId, uint32_t, uint32_t*,
                                                   char*);
            const auto function = GetFunction<Function>(instance, function_name);
            uint32_t size = 0;
            if (function == nullptr || XR_FAILED(function(instance, system, 0, &size, nullptr)) ||
                size == 0) {
                return std::vector<std::string>{};
            }
            std::string names(size, '\0');
            if (XR_FAILED(function(instance, system, size, &size, names.data()))) {
                return std::vector<std::string>{};
            }
            return SplitNames(names);
        };
        const auto wanted_instance = names_from("xrGetVulkanInstanceExtensionsKHR");
        const auto wanted_device = names_from("xrGetVulkanDeviceExtensionsKHR");
        {
            std::scoped_lock lock{names_mutex};
            for (const auto& name : wanted_instance) {
                AddName(instance_extensions, name);
            }
            for (const auto& name : wanted_device) {
                AddName(device_extensions, name);
            }
        }
        LOG_INFO(Core_Vr,
                 "Headset found through {}: {}, pictures of up to {}x{}, hands {}, Vulkan {}.{} "
                 "or later",
                 runtime_name, properties.systemName, max_swapchain_width, max_swapchain_height,
                 hands ? "tracked" : "not tracked",
                 XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                 XR_VERSION_MINOR(requirements.minApiVersionSupported));
        has_hand_tracking = hands;
        FindAudioDevices();
        return true;
    }

    /// Where the headset's sound goes and comes from, by the names Windows lists the devices
    /// under: a runtime that streams to a headset has devices of its own for that. Elsewhere
    /// the system's default devices.
    void FindAudioDevices() {
        std::string output_name;
        std::string input_name;
#ifdef _WIN32
        if (has_audio_guid) {
            using Function = XrResult(XRAPI_PTR*)(XrInstance, wchar_t*);
            const auto output =
                GetFunction<Function>(instance, "xrGetAudioOutputDeviceGuidOculus");
            const auto input =
                GetFunction<Function>(instance, "xrGetAudioInputDeviceGuidOculus");
            wchar_t buffer[XR_MAX_AUDIO_DEVICE_STR_SIZE_OCULUS]{};
            if (output != nullptr && XR_SUCCEEDED(output(instance, buffer))) {
                output_name = AudioDeviceName(buffer, eRender);
            }
            std::memset(buffer, 0, sizeof(buffer));
            if (input != nullptr && XR_SUCCEEDED(input(instance, buffer))) {
                input_name = AudioDeviceName(buffer, eCapture);
            }
        }
        // Virtual Desktop plays in the headset what is played on a sound device of its own,
        // which is in the system only while it streams to a headset. While it is there, it is
        // the headset's, whatever the runtime in use says: Virtual Desktop's own names the
        // device the system prefers, which need not be that one (a player whose system
        // preferred the PC's speakers heard the game from those), and SteamVR names none.
        // Its microphone is always in the system, and only the headset's while it streams.
        // (SHADPS4_XR_STREAM_AUDIO=<how the names end>, for tests and for other such programs.)
        static const std::string streamed = [] {
            const char* ending = std::getenv("SHADPS4_XR_STREAM_AUDIO");
            return std::string{ending != nullptr ? ending : "(Virtual Desktop Audio)"};
        }();
        if (std::string own = ActiveAudioDeviceEnding(streamed, eRender); !own.empty()) {
            output_name = std::move(own);
            if (std::string microphone = ActiveAudioDeviceEnding(streamed, eCapture);
                !microphone.empty()) {
                input_name = std::move(microphone);
            }
        }
#endif
        std::scoped_lock lock{names_mutex};
        if (audio_names_known && output_name == audio_output && input_name == audio_input) {
            return;
        }
        LOG_INFO(Core_Vr, "The headset's sound: output \"{}\", microphone \"{}\"",
                 output_name.empty() ? "the system's default" : output_name,
                 input_name.empty() ? "the system's default" : input_name);
        audio_names_known = true;
        audio_output = std::move(output_name);
        audio_input = std::move(input_name);
    }

    /// FindAudioDevices again, a moment after sound devices came or went (on the frame
    /// thread: the instance is its to use).
    void AskAudioDevicesIfDue() {
        const s64 due = audio_ask_at.load(std::memory_order_relaxed);
        if (due == 0 || Clock::now().time_since_epoch().count() < due) {
            return;
        }
        audio_ask_at.store(0, std::memory_order_relaxed);
        if (instance != XR_NULL_HANDLE) {
            FindAudioDevices();
        }
    }

    // --- the frame thread ---------------------------------------------------------------------

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("shadPS4:XrHost");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
        while (!stop.stop_requested() && !device_mismatch && !exit_requested) {
            AskAudioDevicesIfDue();
            if (!EnsureSession()) {
                Common::StoppableTimedWait(stop, std::chrono::milliseconds{1500});
                continue;
            }
            RunSession(stop);
            DestroySession();
            if (session_lost) {
                // A headset that went away comes back as a new one: it is asked for again,
                // and a session made with what answers.
                session_lost = false;
                system = XR_NULL_SYSTEM_ID;
                Runtime::Instance().SetHeadsetIdentity({});
                if (instance_lost) {
                    DestroyInstance();
                }
                Common::StoppableTimedWait(stop, std::chrono::milliseconds{2000});
            }
        }
        if (exit_requested) {
            LOG_INFO(Core_Vr, "The headset's system asks for the game to be closed");
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
    }

    void DestroyInstance() {
        Runtime::Instance().SetHeadsetIdentity({});
        instance_lost = false;
        system_failures = 0;
        if (instance != XR_NULL_HANDLE) {
            xrDestroyInstance(instance);
        }
        instance = XR_NULL_HANDLE;
        system = XR_NULL_SYSTEM_ID;
    }

    bool EnsureSession() {
        if (instance_lost) {
            DestroyInstance();
        }
        if (instance == XR_NULL_HANDLE && !CreateInstance()) {
            return false;
        }
        if (system == XR_NULL_SYSTEM_ID && !FindSystem()) {
            const auto now = Clock::now();
            if (!search_reported || now - last_search > std::chrono::seconds{60}) {
                search_reported = true;
                last_search = now;
                LOG_INFO(Core_Vr,
                         "No headset yet: the game is on the window until one is connected "
                         "through {}",
                         runtime_name);
            }
            return false;
        }
        search_reported = false;

        // The runtime's compositor works on one graphics card: frames can only be handed over
        // on that one.
        const auto get_device = GetFunction<PFN_xrGetVulkanGraphicsDeviceKHR>(
            instance, "xrGetVulkanGraphicsDeviceKHR");
        VkPhysicalDevice wanted = VK_NULL_HANDLE;
        if (get_device != nullptr &&
            XR_SUCCEEDED(get_device(instance, system, graphics.instance, &wanted)) &&
            wanted != static_cast<VkPhysicalDevice>(graphics.physical_device)) {
            LOG_ERROR(Core_Vr,
                      "The headset is driven by another graphics card than the one the emulator "
                      "draws with: choose that one (Vulkan/gpu_id in config.json) or start the "
                      "game with the headset connected. The game stays on the window");
            device_mismatch = true;
            return false;
        }

        XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        binding.instance = graphics.instance;
        binding.physicalDevice = graphics.physical_device;
        binding.device = graphics.device;
        binding.queueFamilyIndex = graphics.queue_family;
        binding.queueIndex = graphics.queue_index;
        XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
        session_info.next = &binding;
        session_info.systemId = system;
        XrResult result;
        {
            // Runtimes that draw with something else underneath set up what they share with
            // this device now, on its queue.
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            result = xrCreateSession(instance, &session_info, &session);
        }
        if (XR_FAILED(result)) {
            if (++session_failures <= 3 || session_failures % 40 == 0) {
                LOG_ERROR(Core_Vr, "The headset's session could not be made: {} ({} attempts)",
                          ResultText(instance, result), session_failures);
            }
            session = XR_NULL_HANDLE;
            // Whatever was wrong, the next attempt starts from the beginning.
            system = XR_NULL_SYSTEM_ID;
            if (result == XR_ERROR_INSTANCE_LOST) {
                instance_lost = true;
            }
            return false;
        }
        session_failures = 0;

        XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_info.poseInReferenceSpace.orientation.w = 1.0f;
        // LOCAL is the seated space, with its origin where the head was when the view was last
        // reset in the headset's own system: the emulator's "host space".
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        xrCreateReferenceSpace(session, &space_info, &local_space);
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        xrCreateReferenceSpace(session, &space_info, &view_space);
        if (local_space == XR_NULL_HANDLE || view_space == XR_NULL_HANDLE) {
            LOG_ERROR(Core_Vr, "The headset's spaces could not be made");
            DestroySession();
            return false;
        }
        if (has_hand_tracking) {
            CreateHandTrackers();
        }
        if (use_controllers || use_grips) {
            CreateActions();
        }
        if (has_refresh_rate) {
            get_refresh_rate = GetFunction<PFN_xrGetDisplayRefreshRateFB>(
                instance, "xrGetDisplayRefreshRateFB");
        }
        if (!command_pool && !CreateCopyResources()) {
            DestroySession();
            device_mismatch = true;
            return false;
        }
        state = XR_SESSION_STATE_UNKNOWN;
        state_since = Clock::now();
        session_started = state_since;
        last_wanted = state_since;
        ever_wanted = false;
        last_frame_ended = state_since;
        session_running = false;
        have_frame = false;
        first_shown_logged = false;
        StartReport(state_since);
        LOG_INFO(Core_Vr, "Session with the headset made");
        return true;
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
            if (XR_FAILED(create(session, &info, &hand_trackers[hand]))) {
                hand_trackers[hand] = XR_NULL_HANDLE;
                has_hand_tracking = false;
                return;
            }
        }
    }

    /// The headset's own controllers as the title's gamepad, for a player who has none on the
    /// PC. The left stick is the left stick; the right one is the finger on the touchpad (as
    /// it is for gamepads without one) and pressing it in presses the touchpad; A and B, under
    /// the right thumb, are ✕ and □, which is what a hand is on all the time, X and Y are ○
    /// and △; the triggers are L2 and R2, the grips L1 and R1, the left controller's menu
    /// button is OPTIONS. Where one of them is and how it points (the right one, unless
    /// SHADPS4_XR_PAD_HAND=left) is where the controller in the game is.
    void CreateActions() {
        actions_ready = false;
        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(set_info.actionSetName, "gamepad");
        std::strcpy(set_info.localizedActionSetName, "Gamepad");
        if (XR_FAILED(xrCreateActionSet(instance, &set_info, &action_set))) {
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
        made = made && make(act_finger_press, "touchpad_press", "Press the touchpad",
                            XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_cross, "cross", "Cross", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_square, "square", "Square", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_circle, "circle", "Circle", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_triangle, "triangle", "Triangle", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_l1, "l1", "L1", XR_ACTION_TYPE_FLOAT_INPUT);
        made = made && make(act_r1, "r1", "R1", XR_ACTION_TYPE_FLOAT_INPUT);
        made = made && make(act_l2, "l2", "L2", XR_ACTION_TYPE_FLOAT_INPUT);
        made = made && make(act_r2, "r2", "R2", XR_ACTION_TYPE_FLOAT_INPUT);
        made = made && make(act_options, "options", "Options", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_l3, "l3", "L3", XR_ACTION_TYPE_BOOLEAN_INPUT);
        made = made && make(act_pose, "controller", "Controller", XR_ACTION_TYPE_POSE_INPUT);
        made = made && make(act_grip, "hands", "Hands", XR_ACTION_TYPE_POSE_INPUT, true);
        made = made &&
               make(act_rumble, "rumble", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
        if (!made) {
            LOG_WARNING(Core_Vr, "The headset's controllers could not be set up as a gamepad");
            return;
        }

        for (const bool index : {false, true}) {
            std::vector<XrActionSuggestedBinding> bindings;
            const auto bind = [&](XrAction action, const char* path) {
                XrPath binding = XR_NULL_PATH;
                if (XR_SUCCEEDED(xrStringToPath(instance, path, &binding))) {
                    bindings.push_back({action, binding});
                }
            };
            bind(act_move, "/user/hand/left/input/thumbstick");
            bind(act_finger, "/user/hand/right/input/thumbstick");
            bind(act_finger_press, "/user/hand/right/input/thumbstick/click");
            bind(act_cross, "/user/hand/right/input/a/click");
            bind(act_square, "/user/hand/right/input/b/click");
            bind(act_circle,
                 index ? "/user/hand/left/input/a/click" : "/user/hand/left/input/x/click");
            bind(act_triangle,
                 index ? "/user/hand/left/input/b/click" : "/user/hand/left/input/y/click");
            bind(act_l1, "/user/hand/left/input/squeeze/value");
            bind(act_r1, "/user/hand/right/input/squeeze/value");
            bind(act_l2, "/user/hand/left/input/trigger/value");
            bind(act_r2, "/user/hand/right/input/trigger/value");
            bind(act_options, index ? "/user/hand/left/input/trackpad/force"
                                    : "/user/hand/left/input/menu/click");
            bind(act_l3, "/user/hand/left/input/thumbstick/click");
            bind(act_pose, pad_hand == 0 ? "/user/hand/left/input/aim/pose"
                                         : "/user/hand/right/input/aim/pose");
            bind(act_grip, "/user/hand/left/input/grip/pose");
            bind(act_grip, "/user/hand/right/input/grip/pose");
            bind(act_rumble, "/user/hand/left/output/haptic");
            bind(act_rumble, "/user/hand/right/output/haptic");
            XrInteractionProfileSuggestedBinding suggested{
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            xrStringToPath(instance,
                           index ? "/interaction_profiles/valve/index_controller"
                                 : "/interaction_profiles/oculus/touch_controller",
                           &suggested.interactionProfile);
            suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
            suggested.suggestedBindings = bindings.data();
            if (const XrResult result = xrSuggestInteractionProfileBindings(instance, &suggested);
                XR_FAILED(result)) {
                LOG_WARNING(Core_Vr, "The headset's runtime takes no {} controller bindings: {}",
                            index ? "Index" : "Touch", ResultText(instance, result));
            }
        }

        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &action_set;
        if (const XrResult result = xrAttachSessionActionSets(session, &attach);
            XR_FAILED(result)) {
            LOG_WARNING(Core_Vr, "The headset's controllers could not be attached: {}",
                        ResultText(instance, result));
            return;
        }
        XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space_info.action = act_pose;
        space_info.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateActionSpace(session, &space_info, &controller_space))) {
            controller_space = XR_NULL_HANDLE;
            LOG_WARNING(Core_Vr, "The headset's controllers cannot be located");
            return;
        }
        for (int hand = 0; hand < 2; ++hand) {
            space_info.action = act_grip;
            space_info.subactionPath = hand_paths[hand];
            if (XR_FAILED(xrCreateActionSpace(session, &space_info, &grip_spaces[hand]))) {
                grip_spaces[hand] = XR_NULL_HANDLE;
            }
        }
        actions_ready = true;
    }

    void ReleaseControllers(const char* why) {
        if (!controllers_used) {
            return;
        }
        controllers_used = false;
        controller_tracked = false;
        view_reset_held = false;
        sent_buttons = {};
        sent_axes = {128, 128, 128, 128, 0, 0};
        sent_touch = false;
        stick_finger.Reset();
        gestures.Reset();
        view_turn.Reset();
        if (blowing) {
            blowing = false;
            Input::SetBlowing(false);
        }
        (*Common::Singleton<Input::GameControllers>::Instance())[0]->SetHeadsetPlays(false);
        Runtime::Instance().ReleasePad();
        LOG_INFO(Core_Vr, "The headset's controllers no longer stand in for the gamepad: {}",
                 why);
    }

    /// Reads the headset's controllers and hands what they say on as the first player's
    /// gamepad, while the PC has no gamepad of its own or they were used after it.
    void UpdateControllers(XrTime time) {
        actions_synced = false;
        if (!actions_ready) {
            return;
        }
        const XrActiveActionSet active{action_set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        // (Anything but plain success: the game does not have the player's attention.)
        if (xrSyncActions(session, &sync) != XR_SUCCESS) {
            ReleaseControllers("the game is not what the player has in front of them");
            ApplyRumble(false);
            return;
        }
        actions_synced = true;
        if (!use_controllers) {
            // Only where they are is of interest (see UpdatePad).
            return;
        }
        auto* const controller = (*Common::Singleton<Input::GameControllers>::Instance())[0];
        const bool gamepad = controller->m_sdl_gamepad != nullptr;

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

        // SHADPS4_XR_TEST_PRESS=<seconds>[,...] and SHADPS4_XR_TEST_TURN=<seconds>[,...], for
        // tests: so long after the controllers were first read, the right one's A is held for
        // half a second, or the other hand's grip is held and the right stick flicked to the
        // right (to the left for a negative time), as if somebody did.
        bool test_press = false;
        float test_grip = 0.0f;
        float test_flick = 0.0f;
        {
            static const auto times = [](const char* name) {
                std::vector<double> list;
                const char* at = std::getenv(name);
                while (at != nullptr && *at != '\0') {
                    list.push_back(std::atof(at));
                    at = std::strchr(at, ',');
                    at = at != nullptr ? at + 1 : nullptr;
                }
                return list;
            };
            static const std::vector<double> presses = times("SHADPS4_XR_TEST_PRESS");
            static const std::vector<double> turns = times("SHADPS4_XR_TEST_TURN");
            static const auto start = Clock::now();
            const double now = std::chrono::duration<double>(Clock::now() - start).count();
            for (const double at : presses) {
                test_press = test_press || (now >= at && now < at + 0.5);
            }
            for (const double at : turns) {
                const double from = std::abs(at);
                if (now >= from && now < from + 0.6) {
                    test_grip = 1.0f;
                    if (now >= from + 0.2 && now < from + 0.4) {
                        test_flick = at < 0.0 ? -1.0f : 1.0f;
                    }
                }
            }
        }

        using Buttons = Libraries::Pad::OrbisPadButtonDataOffset;
        Buttons buttons{};
        const auto add = [&](bool down, Buttons button) {
            if (down) {
                buttons |= button;
            }
        };
        add(pressed(act_cross) || test_press, Buttons::Cross);
        add(pressed(act_square), Buttons::Square);
        // The left controller's two buttons together are not circle and triangle: they blow
        // into the microphone, for as long as they are held (Input::SetBlowing).
        const bool circle = pressed(act_circle);
        const bool triangle = pressed(act_triangle);
        const bool blow = circle && triangle;
        add(circle && !blow, Buttons::Circle);
        add(triangle && !blow, Buttons::Triangle);
        add(pressed(act_options), Buttons::Options);
        const bool left_stick_in = pressed(act_l3);
        const bool right_stick_in = pressed(act_finger_press);
        const float left_grip = std::max(pulled(act_l1), pad_hand == 0 ? 0.0f : test_grip);
        const float right_grip = std::max(pulled(act_r1), pad_hand == 0 ? test_grip : 0.0f);
        add(left_grip > 0.5f, Buttons::L1);
        add(right_grip > 0.5f, Buttons::R1);
        const float left_trigger = pulled(act_l2);
        const float right_trigger = pulled(act_r2);
        const XrVector2f move = stick(act_move);
        XrVector2f finger = stick(act_finger);
        if (test_flick != 0.0f) {
            finger.x = test_flick;
        }
        if (!any_active) {
            // Both lie somewhere, asleep.
            controllers_in_use = 0;
            ReleaseControllers("they are not in the player's hands");
            ApplyRumble(false);
            return;
        }
        // With a gamepad on the PC as well, whichever was used last plays. Used: a button
        // that plays pressed, a stick pushed or pressed in. (Not a trigger, a grip or the
        // menu button: a hand does that to a controller it only rests on or puts away.)
        const double seconds =
            std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        const auto pushed = [](const XrVector2f& stick) {
            return std::max(std::abs(stick.x), std::abs(stick.y)) > 0.5f;
        };
        const u32 in_use = (True(buttons & Buttons::Cross) ? 1u : 0u) |
                           (True(buttons & Buttons::Square) ? 2u : 0u) | (circle ? 4u : 0u) |
                           (triangle ? 8u : 0u) | (left_stick_in ? 16u : 0u) |
                           (right_stick_in ? 32u : 0u) | (pushed(move) ? 64u : 0u) |
                           (pushed(finger) ? 128u : 0u);
        const bool used = (in_use & ~controllers_in_use) != 0;
        controllers_in_use = in_use;
        if (!Input::FirstPadSource().Update(seconds, gamepad, used)) {
            ReleaseControllers("the gamepad on the PC was used");
            ApplyRumble(false);
            return;
        }
        if (blow != blowing) {
            blowing = blow;
            Input::SetBlowing(blow);
        }
        if (!controllers_used) {
            controllers_used = true;
            controller->SetHeadsetPlays(true);
            LOG_INFO(Core_Vr,
                     "{}: the headset's controllers stand in for "
                     "it (left stick to move, right A = cross, right B = square, left X/A = "
                     "circle, left Y/B = triangle, right stick = finger on the touchpad, "
                     "pressed in = touchpad pressed, left menu/trackpad = OPTIONS, "
                     "both sticks pressed in = reset the view; the {} "
                     "one is the controller in the game: its trigger presses the touchpad, its "
                     "grip swipes forward, the other trigger pulls back and lets go; the other "
                     "grip held, the right stick flicked to a side turns the view a step)",
                     gamepad ? "The headset's controllers were used, the gamepad on the PC was "
                               "not (it plays again when it is used)"
                             : "No gamepad is connected to the PC",
                     pad_hand == 0 ? "left" : "right");
        }

        // Both sticks pressed in is not something a game is played with: it resets the view,
        // which a gamepad's OPTIONS button does when held and which the headset's own menu
        // button may not be free for.
        const bool view_reset = left_stick_in && right_stick_in;
        if (view_reset && !view_reset_held) {
            LOG_INFO(Core_Vr, "Both sticks pressed in: view reset");
            Runtime::Instance().RequestRecenter();
        }
        view_reset_held = view_reset;
        add(left_stick_in && !view_reset, Buttons::L3);
        add(right_stick_in && !view_reset, Buttons::TouchPad);

        const auto axis = [](float value) {
            return std::clamp(static_cast<int>(std::lround(128.0f + value * 127.0f)), 0, 255);
        };
        // (A trigger that is not quite at rest is not pulled: any pull counts as the button.)
        const auto trigger = [](float value) {
            return value < 0.12f ? 0 : static_cast<int>(std::lround(value * 255.0f));
        };
        // A stick pushed away from the player says +1 here, and 0 on a gamepad.
        const std::array<int, 6> axes{
            axis(move.x), axis(-move.y), 128, 128, trigger(left_trigger), trigger(right_trigger),
        };
        // (The trigger buttons follow from the axes where they are applied.)
        if (axes[4] > 0) {
            buttons |= Buttons::L2;
        }
        if (axes[5] > 0) {
            buttons |= Buttons::R2;
        }
        // What the title wants done on the touchpad is on buttons (see Input::PadGestures):
        // the trigger of the hand that holds the controller in the game presses the pad for
        // as long as it is pulled, the grip of that hand swipes forward once, the other
        // hand's trigger pulls back and lets go when it is let go. (They stay L1, R1, L2 and
        // R2 besides: the title has no use for those in play, but its menus have.)
        Input::PadGestures::Controls controls;
        controls.press = (pad_hand == 0 ? left_trigger : right_trigger) > 0.5f;
        controls.swipe = (pad_hand == 0 ? left_grip : right_grip) > 0.5f;
        controls.pull = (pad_hand == 0 ? right_trigger : left_trigger) > 0.5f;
        const Input::PadGestures::Touch made = gestures.Update(seconds, controls);
        // The grip of the other hand, which is left without a meaning, held: each flick of
        // the right stick to a side turns the view a step that way (see Input::ViewTurn), for
        // players who cannot turn round where they sit. The stick moves no finger meanwhile.
        const bool turning = (pad_hand == 0 ? right_grip : left_grip) > 0.5f;
        if (const int steps = view_turn.Update(turning, finger.x); steps != 0) {
            Runtime::Instance().TurnView(steps);
        }
        // Besides that, the right stick moves a finger over the touchpad (see
        // Input::StickFinger: a stick let go lifts the finger where it was, which is what
        // lets a pull on the stick shoot the game's catapults). A stick pushed away from the
        // player is the pad's far edge. The finger also touches, in the middle of the pad,
        // when the stick is only pressed in: a touchpad cannot be pressed without touching it.
        Input::StickFinger::Touch dragged{false, 0.5f, 0.5f};
        if (made.down) {
            stick_finger.Reset();
            if (made.pressed) {
                buttons |= Buttons::TouchPad;
            }
        } else if (turning) {
            stick_finger.Reset();
        } else {
            dragged = stick_finger.Update(seconds, finger.x, -finger.y);
        }
        const bool swiping = made.down || dragged.down;
        const bool touch = swiping || (right_stick_in && !view_reset);
        const float touch_x = made.down ? made.x : dragged.down ? dragged.x : 0.5f;
        const float touch_y = made.down ? made.y : dragged.down ? dragged.y : 0.5f;
        if (buttons != sent_buttons || axes != sent_axes || touch != sent_touch ||
            (touch && (std::abs(touch_x - sent_touch_x) > 0.002f ||
                       std::abs(touch_y - sent_touch_y) > 0.002f))) {
            sent_buttons = buttons;
            sent_axes = axes;
            sent_touch = touch;
            if (touch) {
                sent_touch_x = touch_x;
                sent_touch_y = touch_y;
            }
            // (A finger that lifts does so from where it was.)
            controller->ApplyHeadsetState(buttons, axes, touch, sent_touch_x, sent_touch_y);
        }

        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.next = &velocity;
        const bool located =
            XR_SUCCEEDED(xrLocateSpace(controller_space, local_space, time, &location)) &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0 &&
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        if (located) {
            // (SHADPS4_XR_PAD_OFFSET: where the gamepad is taken to be, from the point the
            // controller is located at and in the controller's own directions.)
            const XrVector3f shift = Rotate(location.pose.orientation, pad_offset);
            location.pose.position.x += shift.x;
            location.pose.position.y += shift.y;
            location.pose.position.z += shift.z;
            DeviceState state;
            state.pose.position = {location.pose.position.x, location.pose.position.y,
                                   location.pose.position.z};
            state.pose.orientation =
                Normalize({location.pose.orientation.x, location.pose.orientation.y,
                           location.pose.orientation.z, location.pose.orientation.w});
            if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) {
                state.linear_velocity = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                         velocity.linearVelocity.z};
            }
            if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0) {
                state.angular_velocity = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                          velocity.angularVelocity.z};
            }
            state.tracked = true;
            Runtime::Instance().UpdatePad(state);
            controller_pose = location.pose;
            ++controller_samples;
        } else if (controller_tracked) {
            // Out of the headset's sight: it is where a gamepad would be assumed to be.
            Runtime::Instance().ReleasePad();
        }
        controller_tracked = located;
        ApplyRumble(true);
    }

    /// What the title asks of the gamepad's two motors, on the two controllers: the heavy
    /// motor is in the gamepad's left grip, the light one in its right, and each is felt a
    /// little in the other hand too.
    void ApplyRumble(bool on) {
        if (!actions_ready) {
            return;
        }
        const u32 wanted = on ? rumble_wanted.load(std::memory_order_relaxed) : 0;
        const auto now = Clock::now();
        // A vibration is asked for by its length: one that goes on is asked for again.
        if (wanted == rumble_applied &&
            (wanted == 0 || now - rumble_time < std::chrono::milliseconds{250})) {
            return;
        }
        rumble_applied = wanted;
        rumble_time = now;
        const float heavy = static_cast<float>(wanted & 0xff) / 255.0f;
        const float light = static_cast<float>((wanted >> 8) & 0xff) / 255.0f;
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

    bool CreateCopyResources() {
        const vk::Device device = graphics.device;
        const auto [pool_result, pool] = device.createCommandPool(vk::CommandPoolCreateInfo{
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = graphics.queue_family,
        });
        if (pool_result != vk::Result::eSuccess) {
            return false;
        }
        command_pool = pool;
        const vk::CommandBufferAllocateInfo allocate_info = {
            .commandPool = command_pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = NumCommandBuffers,
        };
        if (device.allocateCommandBuffers(&allocate_info, command_buffers.data()) !=
            vk::Result::eSuccess) {
            return false;
        }
        for (vk::Fence& fence : fences) {
            const auto [fence_result, created] = device.createFence(
                vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
            if (fence_result != vk::Result::eSuccess) {
                return false;
            }
            fence = created;
        }
        return true;
    }

    void DestroySwapchain() {
        if (swapchain != XR_NULL_HANDLE) {
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            xrDestroySwapchain(swapchain);
            swapchain = XR_NULL_HANDLE;
        }
        swapchain_images.clear();
        swapchain_width = 0;
        swapchain_height = 0;
    }

    void DestroySession() {
        accepting = false;
        showing = false;
        if (session_lost && was_focused) {
            // The headset went away in the middle of the game (its connection broke, it was
            // switched off): the game waits for it, as it does on the console.
            waiting_for_focus = true;
            if (reported_worn && pause_when_away) {
                reported_worn = false;
                LOG_INFO(Core_Vr, "The headset is gone: the game waits until it is back");
                Runtime::Instance().SetHeadsetWorn(false);
            }
        }
        was_focused = false;
        // What is being copied has to have been copied before its target goes away.
        if (command_pool) {
            (void)graphics.device.waitForFences(fences, true, 1'000'000'000);
        }
        DestroySwapchain();
        if (instance != XR_NULL_HANDLE) {
            const auto destroy_hand_tracker =
                GetFunction<PFN_xrDestroyHandTrackerEXT>(instance, "xrDestroyHandTrackerEXT");
            for (XrHandTrackerEXT& tracker : hand_trackers) {
                if (tracker != XR_NULL_HANDLE && destroy_hand_tracker != nullptr) {
                    destroy_hand_tracker(tracker);
                }
                tracker = XR_NULL_HANDLE;
            }
        }
        ReleaseControllers("the headset's session ended");
        actions_ready = false;
        actions_synced = false;
        pad_source = 0;
        if (controller_space != XR_NULL_HANDLE) {
            xrDestroySpace(controller_space);
            controller_space = XR_NULL_HANDLE;
        }
        for (XrSpace& space : grip_spaces) {
            if (space != XR_NULL_HANDLE) {
                xrDestroySpace(space);
                space = XR_NULL_HANDLE;
            }
        }
        if (action_set != XR_NULL_HANDLE) {
            // (Its actions go with it.)
            xrDestroyActionSet(action_set);
            action_set = XR_NULL_HANDLE;
        }
        if (local_space != XR_NULL_HANDLE) {
            xrDestroySpace(local_space);
            local_space = XR_NULL_HANDLE;
        }
        if (view_space != XR_NULL_HANDLE) {
            xrDestroySpace(view_space);
            view_space = XR_NULL_HANDLE;
        }
        if (session != XR_NULL_HANDLE) {
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        session_running = false;
        have_frame = false;
        pad_seen = false;
        Runtime::Instance().ClearPadPosition();
        Runtime::Instance().EnableViewGestures(false);
        {
            // Frames that were on their way are nobody's any more.
            std::scoped_lock lock{slot_mutex};
            for (Slot& slot : slots) {
                if (slot.state != Slot::State::Drawing) {
                    slot.state = Slot::State::Free;
                }
            }
            latest = -1;
        }
    }

    void PollEvents() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& changed =
                    *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                state = changed.state;
                LOG_INFO(Core_Vr, "Headset session: {}", StateName(state));
                switch (state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    const XrResult result = xrBeginSession(session, &begin);
                    if (XR_SUCCEEDED(result)) {
                        session_running = true;
                        accepting = true;
                        // Nothing but the controller is at hand for resetting the view.
                        Runtime::Instance().EnableViewGestures(true);
                    } else {
                        LOG_ERROR(Core_Vr, "The headset's session could not be begun: {}",
                                  ResultText(instance, result));
                    }
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    accepting = false;
                    xrEndSession(session);
                    session_running = false;
                    break;
                case XR_SESSION_STATE_EXITING:
                    exit_requested = true;
                    break;
                case XR_SESSION_STATE_LOSS_PENDING:
                    session_lost = true;
                    break;
                default:
                    break;
                }
                state_since = Clock::now();
                showing = session_running && (state == XR_SESSION_STATE_VISIBLE ||
                                              state == XR_SESSION_STATE_FOCUSED);
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOG_INFO(Core_Vr, "The headset's runtime is going away");
                session_lost = true;
                instance_lost = true;
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                const auto& changed =
                    *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
                if (changed.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    space_change_time = changed.changeTime;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                const auto& changed =
                    *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
                LOG_INFO(Core_Vr, "The headset's display now refreshes {:.0f} times a second",
                         changed.toDisplayRefreshRate);
                break;
            }
            default:
                break;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    /// A picture nobody looks at: the title is told the headset came off, which is what makes
    /// it wait for the player. A session is "focused" while the headset is on the head and
    /// nothing of the headset's own system is in front of the game; states it only passes
    /// through on its way there do not count. And a runtime that asks for no pictures shows
    /// something else in the headset (Virtual Desktop the PC's desktop, say): nobody looks at
    /// the game then either.
    void UpdateWorn() {
        was_focused = was_focused || state == XR_SESSION_STATE_FOCUSED;
        const auto now = Clock::now();
        // (A headset that went away altogether in the middle of the game is waited for until
        // it shows the game again; one that has not been there yet is not waited for.)
        const bool engaged = waiting_for_focus
                                 ? state == XR_SESSION_STATE_FOCUSED
                                 : state != XR_SESSION_STATE_VISIBLE &&
                                       state != XR_SESSION_STATE_STOPPING &&
                                       (state != XR_SESSION_STATE_IDLE || !was_focused);
        const bool shown = now - last_wanted < std::chrono::milliseconds{1000};
        // (A headset that is waited for is only back once its session is asked for pictures:
        // one made anew for a headset that lies on the table is neither.)
        if (state == XR_SESSION_STATE_FOCUSED && shown && ever_wanted) {
            waiting_for_focus = false;
        }
        // (SHADPS4_XR_PAUSE=0: the game is never made to wait, whatever the headset says.)
        const bool worn =
            !pause_when_away || (engaged && shown && (ever_wanted || !waiting_for_focus)) ||
            (!waiting_for_focus && now - state_since < std::chrono::milliseconds{700});
        if (worn != reported_worn) {
            reported_worn = worn;
            LOG_INFO(Core_Vr, "The headset is {}",
                     worn ? "on the head and shows the game"
                          : "off the head, or shows something else: the game waits");
            Runtime::Instance().SetHeadsetWorn(worn);
        }
    }

    void NoteFrameFailure(const char* what, XrResult result) {
        ++frame_failures;
        if (frame_failures <= 5 || frame_failures % 1000 == 0) {
            LOG_ERROR(Core_Vr, "{} failed: {} ({} frame calls failed so far)", what,
                      ResultText(instance, result), frame_failures);
        }
        // A session whose calls only fail any more is of no use: its headset is gone, or the
        // runtime behind it is (Virtual Desktop's streamer was closed, say).
        if (result == XR_ERROR_INSTANCE_LOST) {
            instance_lost = true;
            session_lost = true;
        } else if (result == XR_ERROR_SESSION_LOST) {
            session_lost = true;
        } else if (!session_lost && Clock::now() - last_frame_ended > std::chrono::seconds{2}) {
            LOG_INFO(Core_Vr, "The headset's session has only failed for two seconds: starting "
                              "over with it");
            session_lost = true;
        }
    }

    /// A runtime does not always say that its headset went away. Virtual Desktop's stops asking
    /// for pictures instead, for good: the session it had is dead, and a headset that comes
    /// back is only shown to by a new one. But it also asks for none while it shows the PC's
    /// desktop in the headset, and that passes by itself; a new session would pull the player
    /// back into the game. So a session is made anew
    ///  - when the headset was put on (again) and no picture has been asked for since, nor
    ///    is for three seconds: whatever happened while it was off, the session did not
    ///    survive it;
    ///  - when the headset is off and nothing has been asked for for half a minute: nobody
    ///    is looking, and a headset whose connection broke is found again this way.
    /// Either is done less and less often while it changes nothing: a headset that lies on
    /// the table is not a reason to make sessions all day, and a new session takes the
    /// headset away from whatever else it shows.
    /// A session that is on the head and was asked for pictures since is left alone.
    /// All of this for Virtual Desktop's runtime only. Others say what becomes of their
    /// sessions through the states and events OpenXR has for it, and ask for no pictures
    /// whenever nobody looks (SteamVR while its headset rests, or while its dashboard is in
    /// front): a session ended under them for that is a session they were still using.
    void CheckWanted(bool wanted, Clock::time_point now) {
        if (wanted) {
            last_wanted = now;
            ever_wanted = true;
            idle_patience = FirstIdlePatience;
            focus_patience = FirstFocusPatience;
            return;
        }
        if (session_lost || !renew_unwanted) {
            return;
        }
        if (state == XR_SESSION_STATE_FOCUSED) {
            if (last_wanted < state_since && now - state_since > focus_patience) {
                LOG_INFO(Core_Vr, "The headset is on the head but its runtime asks for no "
                                  "pictures: starting over with it");
                session_lost = true;
                focus_patience = std::clamp(focus_patience * 2, FirstIdlePatience,
                                            LongestIdlePatience);
            }
        } else if (now - last_wanted > idle_patience && now - state_since > idle_patience) {
            LOG_INFO(Core_Vr,
                     "The headset's runtime has asked for no picture for {} seconds: starting "
                     "over with it",
                     idle_patience.count());
            session_lost = true;
            idle_patience = std::min(idle_patience * 2, LongestIdlePatience);
        }
    }

    void RunSession(std::stop_token stop) {
        auto& runtime = Runtime::Instance();
        while (!stop.stop_requested() && !session_lost && !exit_requested) {
            PollEvents();
            if (session_lost || exit_requested) {
                break;
            }
            UpdateWorn();
            // SHADPS4_XR_LOSE_AFTER=<seconds>, for tests: the session is taken for lost once,
            // that long after it was made, as it is when a headset's connection breaks.
            if (lose_after > 0.0f &&
                Clock::now() - session_started > std::chrono::duration<float>(lose_after)) {
                LOG_INFO(Core_Vr, "Taking the headset's session for lost (SHADPS4_XR_LOSE_AFTER)");
                lose_after = 0.0f;
                session_lost = true;
                break;
            }
            if (!session_running) {
                // (Nothing is asked for and nothing can fail while the session rests.)
                last_wanted = Clock::now();
                last_frame_ended = last_wanted;
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                continue;
            }

            XrFrameState frame_state{XR_TYPE_FRAME_STATE};
            if (const XrResult waited = xrWaitFrame(session, nullptr, &frame_state);
                XR_FAILED(waited)) {
                NoteFrameFailure("xrWaitFrame", waited);
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
                continue;
            }
            // SHADPS4_XR_UNWANTED=<from>,<to>, for tests: between those seconds after the
            // start the runtime is taken to ask for no pictures, as Virtual Desktop's does
            // while it shows the desktop.
            if (unwanted_to > 0.0f) {
                const float since =
                    std::chrono::duration<float>(Clock::now() - connect_time).count();
                if (since >= unwanted_from && since < unwanted_to) {
                    frame_state.shouldRender = XR_FALSE;
                }
            }
            // The runtime lets go of xrWaitFrame once for every picture it wants, at the same
            // point of each: the emulated headset takes its own refreshes from that. Under a
            // runtime that makes up every other picture itself that is half as often as the
            // display refreshes, which is the rate that counts here.
            const auto woke = Clock::now();
            float rate = frame_state.predictedDisplayPeriod > 0
                             ? 1e9f / static_cast<float>(frame_state.predictedDisplayPeriod)
                             : 0.0f;
            if (rate < 30.0f || rate > 400.0f) {
                rate = 0.0f;
                if (get_refresh_rate != nullptr) {
                    get_refresh_rate(session, &rate);
                }
            }
            if (rate > 30.0f) {
                refresh_rate = rate;
                runtime.NoteDisplayRefresh(
                    rate, static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               woke.time_since_epoch())
                                               .count()));
            }

            XrResult begun;
            {
                std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
                begun = xrBeginFrame(session, nullptr);
            }
            if (XR_FAILED(begun)) {
                NoteFrameFailure("xrBeginFrame", begun);
                continue;
            }

            const XrTime pose_time =
                frame_state.predictedDisplayTime +
                static_cast<XrTime>(std::clamp(predict_ms, 0.0f, 80.0f) * 1e6f);
            UpdateHead(pose_time);
            UpdateControllers(pose_time);
            AskAudioDevicesIfDue();
            UpdatePad(pose_time);

            if (const s32 index = TakeFrame(); index >= 0) {
                const auto started = Clock::now();
                bool blank = false;
                const bool copied = CopyFrame(static_cast<u32>(index), blank);
                copy_time += std::chrono::duration<double>(Clock::now() - started).count();
                std::scoped_lock lock{slot_mutex};
                Slot& slot = slots[index];
                if (copied) {
                    shown = slot.info;
                    shown_width = slot.width;
                    shown_height = slot.height;
                    have_frame = true;
                    ++copied_frames;
                } else if (blank) {
                    if (have_frame) {
                        LOG_INFO(Core_Vr, "The title shows nothing: neither does the headset");
                    }
                    have_frame = false;
                    first_shown_logged = false;
                }
                if (slot.state == Slot::State::Reading) {
                    slot.state = Slot::State::Free;
                }
            }

            XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            const XrCompositionLayerBaseHeader* layers[1]{};
            uint32_t layer_count = 0;
            if (frame_state.shouldRender == XR_TRUE && have_frame &&
                swapchain != XR_NULL_HANDLE) {
                FillProjection(views, projection);
                layers[layer_count++] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            }

            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = frame_state.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount = layer_count;
            end.layers = layers;
            const auto ending = Clock::now();
            XrResult ended;
            {
                std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
                ended = xrEndFrame(session, &end);
            }
            const double took = std::chrono::duration<double>(Clock::now() - ending).count();
            end_frame_time += took;
            end_frame_worst = std::max(end_frame_worst, took);
            if (layer_count != 0) {
                // How far the head has turned from where the picture being shown was drawn
                // for: what the compositor makes up for. Next to nothing while the head rests.
                const Quat& drawn = shown.render_pose.orientation;
                const XrQuaternionf& now_at = head_pose.orientation;
                const float dot = std::abs(drawn.x * now_at.x + drawn.y * now_at.y +
                                           drawn.z * now_at.z + drawn.w * now_at.w);
                const float angle = 2.0f * std::acos(std::min(dot, 1.0f)) * 57.29578f;
                correction += angle;
                correction_worst = std::max(correction_worst, angle);
                ++correction_samples;
            }
            if (XR_SUCCEEDED(ended)) {
                last_frame_ended = Clock::now();
                CheckWanted(frame_state.shouldRender == XR_TRUE, last_frame_ended);
            }
            if (XR_FAILED(ended)) {
                // A layer the runtime does not accept is a frame nobody sees.
                NoteFrameFailure("xrEndFrame", ended);
            } else if (layer_count != 0 && !first_shown_logged) {
                first_shown_logged = true;
                LOG_INFO(Core_Vr,
                         "The title's first picture handed to the headset: {}x{} an eye, field "
                         "of view {:.1f}/{:.1f}/{:.1f}/{:.1f} degrees (out, in, up, down), the "
                         "display at {:.0f} Hz",
                         shown_width / 2, shown_height, std::atan(shown.fov.tan_out) * 57.29578f,
                         std::atan(shown.fov.tan_in) * 57.29578f,
                         std::atan(shown.fov.tan_top) * 57.29578f,
                         std::atan(shown.fov.tan_bottom) * 57.29578f, refresh_rate);
            }
            Report(woke);
        }
    }

    // --- tracking -----------------------------------------------------------------------------

    void UpdateHead(XrTime time) {
        auto& runtime = Runtime::Instance();
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.next = &velocity;
        if (XR_SUCCEEDED(xrLocateSpace(view_space, local_space, time, &location)) &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0 &&
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
            DeviceState state;
            state.pose.position = {location.pose.position.x, location.pose.position.y,
                                   location.pose.position.z};
            state.pose.orientation =
                Normalize({location.pose.orientation.x, location.pose.orientation.y,
                           location.pose.orientation.z, location.pose.orientation.w});
            if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) {
                state.linear_velocity = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                         velocity.linearVelocity.z};
            }
            if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0) {
                state.angular_velocity = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                          velocity.angularVelocity.z};
            }
            state.tracked = true;
            // (SHADPS4_XR_HEAD=0, for tests: the head is somebody else's to move, a script's.)
            if (feed_head) {
                runtime.UpdateHead(state);
            }
            head_pose = location.pose;
            ++head_tracked_frames;
        }

        // The headset's own "reset view" moves the space poses are given in: from the moment
        // it says, where the head is then is where the player sits.
        if (space_change_time != 0 && time >= space_change_time) {
            space_change_time = 0;
            LOG_INFO(Core_Vr, "View reset in the headset's own system");
            runtime.RequestRecenter();
            runtime.ResetPadYaw();
        }

        // What the headset shows, and the distance between the eyes, which follows the headset's
        // lens adjustment.
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = time;
        locate.space = view_space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        XrView eye_views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t view_count = 0;
        if (!XR_SUCCEEDED(
                xrLocateViews(session, &locate, &view_state, 2, &view_count, eye_views)) ||
            view_count != 2) {
            return;
        }
        if ((view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
            return;
        }
        const auto seen = ParallelStereoFov(eye_views[0], eye_views[1]);
        if (seen && ++fov_samples % 600 == 1) {
            runtime.NoteHeadsetFov(*seen);
        }
        if ((view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
            const float dx = eye_views[1].pose.position.x - eye_views[0].pose.position.x;
            const float dy = eye_views[1].pose.position.y - eye_views[0].pose.position.y;
            const float dz = eye_views[1].pose.position.z - eye_views[0].pose.position.z;
            const float measured = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (measured > 0.04f && measured < 0.09f &&
                (std::abs(measured - ipd) > 0.0002f || ++frames_since_optics > 600)) {
                if (std::abs(measured - ipd) > 0.0002f) {
                    LOG_INFO(Core_Vr, "The eyes are {:.1f} mm apart", measured * 1000.0f);
                }
                ipd = measured;
                frames_since_optics = 0;
                // The field of view stays what the title was told (it asks once).
                runtime.UpdateOptics(runtime.GetConfig().fov, ipd);
            }
        }
    }

    /// Where the palm of a hand is, if the headset sees it.
    std::optional<XrVector3f> LocatePalm(int hand, XrTime time) {
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
        return palm.pose.position;
    }

    /// Where a hand holds something, by where the runtime has the controller of that hand.
    std::optional<XrVector3f> LocateGrip(int hand, XrTime time) {
        if (grip_spaces[hand] == XR_NULL_HANDLE) {
            return std::nullopt;
        }
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = act_grip;
        get.subactionPath = hand_paths[hand];
        XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(xrGetActionStatePose(session, &get, &pose)) || pose.isActive != XR_TRUE) {
            return std::nullopt;
        }
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        static constexpr XrSpaceLocationFlags Needed =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if (XR_FAILED(xrLocateSpace(grip_spaces[hand], local_space, time, &location)) ||
            (location.locationFlags & Needed) != Needed) {
            return std::nullopt;
        }
        return location.pose.position;
    }

    /// A gamepad cannot be tracked, the hands that hold it can: both palms a controller's width
    /// apart give away where it is and which way it points.
    void UpdatePad(XrTime time) {
        auto& runtime = Runtime::Instance();
        bool seen = false;
        std::optional<XrVector3f> left;
        std::optional<XrVector3f> right;
        int source = 0;
        // (Hands that hold the headset's own controllers hold no gamepad.)
        if (has_hand_tracking && !controllers_used && hand_trackers[0] != XR_NULL_HANDLE &&
            hand_trackers[1] != XR_NULL_HANDLE) {
            left = LocatePalm(0, time);
            right = LocatePalm(1, time);
            source = 1;
        }
        if (!(left && right) && use_grips && actions_synced && !controllers_used) {
            left = LocateGrip(0, time);
            right = LocateGrip(1, time);
            source = 2;
            if (left && right) {
                // The headset's own controllers, lying side by side on a table, are not a
                // gamepad in two hands: hands are never quite still, and never far from the
                // head.
                const XrVector3f middle{(left->x + right->x) * 0.5f, (left->y + right->y) * 0.5f,
                                        (left->z + right->z) * 0.5f};
                const auto now = Clock::now();
                const float moved = std::hypot(middle.x - grip_rest.x, middle.y - grip_rest.y,
                                               middle.z - grip_rest.z);
                if (moved > 0.0015f) {
                    grip_rest = middle;
                    grip_moved = now;
                }
                const float reach = std::hypot(middle.x - head_pose.position.x,
                                               middle.y - head_pose.position.y,
                                               middle.z - head_pose.position.z);
                if (now - grip_moved > std::chrono::seconds{4} || reach > 0.9f) {
                    left.reset();
                    right.reset();
                }
            }
        }
        if (left && right) {
            {
                const XrVector3f across{right->x - left->x, right->y - left->y,
                                        right->z - left->z};
                const float distance =
                    std::sqrt(across.x * across.x + across.y * across.y + across.z * across.z);
                ++palm_samples;
                palm_distance += distance;
                // Hands further apart, or closer together, are not holding a controller.
                if (distance > 0.05f && distance < 0.32f) {
                    XrVector3f centre{(left->x + right->x) * 0.5f, (left->y + right->y) * 0.5f,
                                      (left->z + right->z) * 0.5f};
                    const float level = std::sqrt(across.x * across.x + across.z * across.z);
                    if (level > 0.6f * distance) {
                        // The line from the left to the right palm is the controller's
                        // sideways axis; its heading follows from that.
                        const float yaw = std::atan2(-across.z, across.x);
                        runtime.UpdatePadYawReference(yaw);
                        // The palms close around the grips, which sit behind and below the
                        // middle of the controller.
                        centre.x += -std::sin(yaw) * 0.035f;
                        centre.z += -std::cos(yaw) * 0.035f;
                    }
                    centre.y += 0.015f;

                    if (pad_seen && time > pad_time) {
                        const float elapsed = static_cast<float>(time - pad_time) * 1e-9f;
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
                    pad_time = time;

                    // For the log: the same, as seen from the head.
                    const XrQuaternionf to_head{-head_pose.orientation.x, -head_pose.orientation.y,
                                                -head_pose.orientation.z, head_pose.orientation.w};
                    const XrVector3f offset =
                        Rotate(to_head, {centre.x - head_pose.position.x,
                                         centre.y - head_pose.position.y,
                                         centre.z - head_pose.position.z});
                    pad_from_head.x += offset.x;
                    pad_from_head.y += offset.y;
                    pad_from_head.z += offset.z;
                    ++pad_samples;

                    runtime.UpdatePadPosition({centre.x, centre.y, centre.z},
                                              {pad_velocity.x, pad_velocity.y, pad_velocity.z});
                    seen = true;
                }
            }
        }
        // Once more when the hands are lost, so that the last position is no longer trusted.
        if (!seen && pad_seen) {
            runtime.ClearPadPosition();
        }
        pad_seen = seen;
        if (seen && source != pad_source) {
            pad_source = source;
            LOG_INFO(Core_Vr, "The gamepad is placed in the game by where the hands are, which {}",
                     source == 1 ? "the headset tracks"
                                 : "its runtime tells by the places of its own controllers");
        }
    }

    // --- pictures -----------------------------------------------------------------------------

    /// The newest finished frame that has not been shown yet, marked as being read; -1 for none.
    s32 TakeFrame() {
        // SHADPS4_XR_FREEZE_AFTER=<seconds>, for tests: from then on the picture that is being
        // shown stays, as a title's does while it is busy. The compositor has to keep it where
        // it was drawn for while the head moves on; a picture that follows the head, or
        // swims, was described wrongly to it.
        static const float freeze_after = EnvFloat("SHADPS4_XR_FREEZE_AFTER", 0.0f);
        if (freeze_after > 0.0f && have_frame &&
            Clock::now() - session_started > std::chrono::duration<float>(freeze_after)) {
            return -1;
        }
        std::scoped_lock lock{slot_mutex};
        if (latest < 0 || slots[latest].state != Slot::State::Ready) {
            return -1;
        }
        slots[latest].state = Slot::State::Reading;
        return latest;
    }

    bool EnsureSwapchain(u32 width, u32 height) {
        if (max_swapchain_width != 0) {
            width = std::min(width, max_swapchain_width);
        }
        if (max_swapchain_height != 0) {
            height = std::min(height, max_swapchain_height);
        }
        if (swapchain != XR_NULL_HANDLE && swapchain_width == width && swapchain_height == height) {
            return true;
        }
        if (swapchain_failed) {
            return false;
        }
        if (swapchain != XR_NULL_HANDLE) {
            (void)graphics.device.waitForFences(fences, true, 1'000'000'000);
            DestroySwapchain();
        }

        uint32_t count = 0;
        xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(session, count, &count, formats.data());
        // What is handed over is encoded the way displays want it, and the image has to say
        // so: a runtime takes anything else for linear light and shows it too bright.
        static constexpr vk::Format Wanted[] = {vk::Format::eR8G8B8A8Srgb,
                                                vk::Format::eB8G8R8A8Srgb};
        swapchain_format = vk::Format::eUndefined;
        for (const vk::Format wanted : Wanted) {
            if (std::ranges::find(formats, static_cast<int64_t>(wanted)) != formats.end()) {
                swapchain_format = wanted;
                break;
            }
        }
        if (swapchain_format == vk::Format::eUndefined) {
            LOG_ERROR(Core_Vr, "The headset's runtime offers no picture format that can be used");
            swapchain_failed = true;
            return false;
        }

        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                          XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = static_cast<int64_t>(swapchain_format);
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        XrResult result;
        {
            // Creating the images, and listing them, has some runtimes prepare them on the
            // queue.
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            result = xrCreateSwapchain(session, &info, &swapchain);
            if (XR_SUCCEEDED(result)) {
                uint32_t image_count = 0;
                xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr);
                swapchain_images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
                result = xrEnumerateSwapchainImages(
                    swapchain, image_count, &image_count,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain_images.data()));
            }
        }
        if (XR_FAILED(result)) {
            LOG_ERROR(Core_Vr, "The headset's runtime gave no images of {}x{} to show frames in: {}",
                      width, height, ResultText(instance, result));
            if (swapchain != XR_NULL_HANDLE) {
                DestroySwapchain();
            }
            swapchain_failed = true;
            return false;
        }
        swapchain_width = width;
        swapchain_height = height;
        LOG_INFO(Core_Vr, "Frames are shown through {} images of {}x{}, {}",
                 swapchain_images.size(), width, height, vk::to_string(swapchain_format));
        return true;
    }

    /// Copies the frame in slot `index` into the image the compositor shows next. `blank` says
    /// that there was nothing to copy.
    bool CopyFrame(u32 index, bool& blank) {
        vk::Image source;
        u32 width;
        u32 height;
        {
            std::scoped_lock lock{slot_mutex};
            source = slots[index].image;
            width = slots[index].width;
            height = slots[index].height;
        }
        // A title with nothing to show hands over a black picture of a pixel an eye (this one
        // does while it waits for the headset to be put back on). That is not worth images of
        // its size from the runtime, which would have to be made again for the next real one:
        // the headset is shown nothing instead.
        if (width < MinFrameSize || height < MinFrameSize) {
            blank = true;
            return false;
        }
        if (!EnsureSwapchain(width, height)) {
            return false;
        }

        uint32_t image_index = 0;
        XrResult result;
        {
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            result = xrAcquireSwapchainImage(swapchain, &acquire, &image_index);
        }
        if (XR_FAILED(result)) {
            NoteFrameFailure("xrAcquireSwapchainImage", result);
            return false;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain, &wait);
        if (XR_FAILED(result)) {
            NoteFrameFailure("xrWaitSwapchainImage", result);
        }
        const vk::Image target{swapchain_images[image_index].image};

        const vk::Device device = graphics.device;
        const u32 buffer = next_command_buffer;
        next_command_buffer = (next_command_buffer + 1) % NumCommandBuffers;
        (void)device.waitForFences(fences[buffer], true, 1'000'000'000);
        (void)device.resetFences(fences[buffer]);
        const vk::CommandBuffer cmdbuf = command_buffers[buffer];
        (void)cmdbuf.reset();
        (void)cmdbuf.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        });

        constexpr vk::ImageSubresourceRange Color = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        };
        constexpr vk::ImageSubresourceLayers ColorLayer = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .layerCount = 1,
        };
        const std::array before{
            // The frame was left in the General layout by whoever drew it.
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                                 vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source,
                .subresourceRange = Color,
            },
            // The runtime hands its images over, and wants them back, as colour attachments.
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = target,
                .subresourceRange = Color,
            },
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(before.size()),
            .pImageMemoryBarriers = before.data(),
        });
        if (swapchain_format == FrameFormat && swapchain_width == width &&
            swapchain_height == height) {
            cmdbuf.copyImage(source, vk::ImageLayout::eTransferSrcOptimal, target,
                             vk::ImageLayout::eTransferDstOptimal,
                             vk::ImageCopy{
                                 .srcSubresource = ColorLayer,
                                 .dstSubresource = ColorLayer,
                                 .extent = {width, height, 1},
                             });
        } else {
            cmdbuf.blitImage(
                source, vk::ImageLayout::eTransferSrcOptimal, target,
                vk::ImageLayout::eTransferDstOptimal,
                vk::ImageBlit{
                    .srcSubresource = ColorLayer,
                    .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                             vk::Offset3D{static_cast<s32>(width),
                                                          static_cast<s32>(height), 1}},
                    .dstSubresource = ColorLayer,
                    .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                             vk::Offset3D{static_cast<s32>(swapchain_width),
                                                          static_cast<s32>(swapchain_height), 1}},
                },
                vk::Filter::eLinear);
        }
        const std::array after{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eNone,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source,
                .subresourceRange = Color,
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                                 vk::AccessFlagBits2::eColorAttachmentWrite |
                                 vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = target,
                .subresourceRange = Color,
            },
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(after.size()),
            .pImageMemoryBarriers = after.data(),
        });
        (void)cmdbuf.end();

        bool submitted;
        {
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            const vk::SubmitInfo submit_info = {
                .commandBufferCount = 1,
                .pCommandBuffers = &cmdbuf,
            };
            submitted =
                graphics.queue.submit(submit_info, fences[buffer]) == vk::Result::eSuccess;
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            result = xrReleaseSwapchainImage(swapchain, &release);
        }
        if (XR_FAILED(result)) {
            NoteFrameFailure("xrReleaseSwapchainImage", result);
            return false;
        }
        return submitted;
    }

    /// Describes the shown frame to the compositor: where the head was when the title drew it
    /// and with what field of view, so that it can be corrected for how the head has moved.
    void FillProjection(XrCompositionLayerProjectionView* views,
                        XrCompositionLayerProjection& projection) const {
        // A runtime refuses a layer whose orientation is not a rotation, and with it the whole
        // frame: whatever arrives is brought to length one, and nothing usable becomes
        // "straight ahead".
        const Quat& q = shown.render_pose.orientation;
        XrQuaternionf orientation{q.x, q.y, q.z, q.w};
        const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (std::isfinite(length) && length > 0.5f) {
            orientation = {q.x / length, q.y / length, q.z / length, q.w / length};
        } else {
            orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        }
        const float angle_out = std::atan(shown.fov.tan_out);
        const float angle_in = std::atan(shown.fov.tan_in);
        const float angle_up = std::atan(shown.fov.tan_top);
        const float angle_down = std::atan(shown.fov.tan_bottom);
        const auto eye_width = static_cast<int32_t>(swapchain_width / 2);

        for (int eye = 0; eye < 2; ++eye) {
            const float side = eye == 0 ? -0.5f : 0.5f;
            const XrVector3f offset = Rotate(orientation, {side * ipd, 0.0f, 0.0f});
            views[eye].pose.orientation = orientation;
            views[eye].pose.position = {shown.render_pose.position.x + offset.x,
                                        shown.render_pose.position.y + offset.y,
                                        shown.render_pose.position.z + offset.z};
            // "Out" is towards the temple: left for the left eye, right for the right one.
            views[eye].fov.angleLeft = eye == 0 ? -angle_out : -angle_in;
            views[eye].fov.angleRight = eye == 0 ? angle_in : angle_out;
            views[eye].fov.angleUp = angle_up;
            views[eye].fov.angleDown = -angle_down;
            views[eye].subImage.swapchain = swapchain;
            views[eye].subImage.imageRect.offset = {eye * eye_width, 0};
            views[eye].subImage.imageRect.extent = {eye_width,
                                                    static_cast<int32_t>(swapchain_height)};
        }
        projection.space = local_space;
        projection.viewCount = 2;
        projection.views = views;
    }

    /// Every ten seconds: how many frames the title delivered against how many the headset
    /// asked for, what handing them over cost, and what was seen of the hands.
    void Report(Clock::time_point now) {
        ++display_frames;
        if (now - last_report < std::chrono::seconds{10}) {
            return;
        }
        const float seconds = std::chrono::duration<float>(now - last_report).count();
        const u32 delivered = delivered_frames.load(std::memory_order_relaxed);
        const float frames = static_cast<float>(display_frames);
        const XrVector3f forward = Rotate(head_pose.orientation, {0.0f, 0.0f, -1.0f});
        double gap;
        u32 gaps;
        {
            std::scoped_lock lock{slot_mutex};
            gap = longest_gap;
            gaps = long_gaps;
            longest_gap = 0.0;
            long_gaps = 0;
        }
        LOG_INFO(Core_Vr,
                 "Headset: the title delivered {:.1f} frames a second ({:.0f} ms the longest "
                 "wait for one, {} waits of more than 50 ms), {:.1f} were shown, the "
                 "headset took {:.1f} pictures a second ({}); handing one over took {:.2f} ms, "
                 "ending a picture {:.2f} ms ({:.1f} at worst); head tracked {:.0f}% of the "
                 "time, at {:.2f} {:.2f} {:.2f} facing {:.0f} degrees left; pictures were shown "
                 "{:.1f} degrees from where they were drawn for ({:.1f} at most)",
                 static_cast<float>(delivered - reported_delivered) / seconds, gap * 1e3, gaps,
                 static_cast<float>(copied_frames) / seconds, frames / seconds, StateName(state),
                 copied_frames != 0 ? copy_time / copied_frames * 1e3 : 0.0,
                 end_frame_time / frames * 1e3, end_frame_worst * 1e3,
                 100.0f * static_cast<float>(head_tracked_frames) / frames,
                 head_pose.position.x, head_pose.position.y, head_pose.position.z,
                 std::atan2(-forward.x, -forward.z) * 57.29578f,
                 correction_samples != 0 ? correction / static_cast<float>(correction_samples)
                                         : 0.0f,
                 correction_worst);
        if ((has_hand_tracking || (use_grips && actions_ready)) && !controllers_used) {
            const float held = pad_samples != 0 ? 1.0f / static_cast<float>(pad_samples) : 0.0f;
            LOG_INFO(Core_Vr,
                     "Hands: both seen {:.0f}% of the time, {:.2f} m apart; holding the "
                     "controller {:.0f}%, on average {:.2f} right, {:.2f} up, {:.2f} ahead of "
                     "the head",
                     100.0f * static_cast<float>(palm_samples) / frames,
                     palm_samples != 0 ? palm_distance / static_cast<float>(palm_samples) : 0.0f,
                     100.0f * static_cast<float>(pad_samples) / frames, pad_from_head.x * held,
                     pad_from_head.y * held, -pad_from_head.z * held);
        }
        if (controllers_used) {
            // Where the one that is the controller in the game is, as seen from the head, and
            // where it points.
            const XrQuaternionf to_head{-head_pose.orientation.x, -head_pose.orientation.y,
                                        -head_pose.orientation.z, head_pose.orientation.w};
            const XrVector3f offset =
                Rotate(to_head, {controller_pose.position.x - head_pose.position.x,
                                 controller_pose.position.y - head_pose.position.y,
                                 controller_pose.position.z - head_pose.position.z});
            const XrVector3f pointing = Rotate(controller_pose.orientation, {0.0f, 0.0f, -1.0f});
            LOG_INFO(Core_Vr,
                     "Controllers: standing in for the gamepad; the {} one tracked {:.0f}% of "
                     "the time, {:.2f} right, {:.2f} up, {:.2f} ahead of the head, pointing "
                     "{:.0f} degrees left and {:.0f} up; buttons {:#x}, stick {} {}, finger {}",
                     pad_hand == 0 ? "left" : "right",
                     100.0f * static_cast<float>(controller_samples) / frames, offset.x, offset.y,
                     -offset.z, std::atan2(-pointing.x, -pointing.z) * 57.29578f,
                     std::asin(std::clamp(pointing.y, -1.0f, 1.0f)) * 57.29578f,
                     static_cast<u32>(sent_buttons), sent_axes[0], sent_axes[1],
                     sent_touch ? fmt::format("at {:.2f} {:.2f}", sent_touch_x, sent_touch_y)
                                : std::string{"up"});
        }
        // A gamepad whose motion sensors never said anything cannot be pointed with in the
        // game; the usual reason is that it is not connected to the PC but to the headset.
        if (!motion_warned && was_focused && now - session_started > std::chrono::seconds{20}) {
            const auto* const gamepad =
                (*Common::Singleton<Input::GameControllers>::Instance())[0];
            if (gamepad->m_sdl_gamepad != nullptr && !Runtime::Instance().PadMotionKnown()) {
                motion_warned = true;
                LOG_WARNING(Core_Vr,
                            "The gamepad has said nothing of how it is held (no motion sensor "
                            "readings): in the game it neither turns nor tilts. A gamepad paired "
                            "with the headset reaches the PC through Virtual Desktop without its "
                            "motion sensors and touchpad: connect it to the PC itself (USB cable "
                            "or Bluetooth)");
            }
        }
        StartReport(now);
    }

    void StartReport(Clock::time_point now) {
        {
            std::scoped_lock lock{slot_mutex};
            longest_gap = 0.0;
            long_gaps = 0;
        }
        reported_delivered = delivered_frames.load(std::memory_order_relaxed);
        display_frames = 0;
        copied_frames = 0;
        head_tracked_frames = 0;
        palm_samples = 0;
        palm_distance = 0.0f;
        pad_samples = 0;
        pad_from_head = {};
        controller_samples = 0;
        end_frame_time = 0.0;
        end_frame_worst = 0.0;
        copy_time = 0.0;
        correction = 0.0f;
        correction_worst = 0.0f;
        correction_samples = 0;
        last_report = now;
    }

    // --- the images frames are drawn into, for the threads that draw and present --------------

    void Retire(Slot& slot) {
        if (slot.image) {
            retired.push_back({slot.image, slot.memory, slot.view, Clock::now()});
        }
        slot.image = nullptr;
        slot.memory = nullptr;
        slot.view = nullptr;
        slot.width = 0;
        slot.height = 0;
    }

    void DestroyRetired() {
        // Whatever still used an image that was replaced has long finished by then.
        const auto now = Clock::now();
        std::erase_if(retired, [&](const Retired& old) {
            if (now - old.since < std::chrono::seconds{3}) {
                return false;
            }
            graphics.device.destroyImageView(old.view);
            graphics.device.destroyImage(old.image);
            graphics.device.freeMemory(old.memory);
            return true;
        });
    }

    bool Create(Slot& slot, u32 width, u32 height) {
        const vk::Device device = graphics.device;
        Retire(slot);
        const auto [image_result, image] = device.createImage(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = FrameFormat,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
        });
        if (image_result != vk::Result::eSuccess) {
            return false;
        }
        const auto requirements = device.getImageMemoryRequirements(image);
        const auto memory_properties = graphics.physical_device.getMemoryProperties();
        u32 memory_type = memory_properties.memoryTypeCount;
        for (u32 type = 0; type < memory_properties.memoryTypeCount; ++type) {
            if ((requirements.memoryTypeBits & (1u << type)) != 0 &&
                (memory_properties.memoryTypes[type].propertyFlags &
                 vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                memory_type = type;
                break;
            }
        }
        if (memory_type == memory_properties.memoryTypeCount) {
            device.destroyImage(image);
            return false;
        }
        const auto [memory_result, memory] = device.allocateMemory(vk::MemoryAllocateInfo{
            .allocationSize = requirements.size,
            .memoryTypeIndex = memory_type,
        });
        if (memory_result != vk::Result::eSuccess) {
            device.destroyImage(image);
            return false;
        }
        if (device.bindImageMemory(image, memory, 0) != vk::Result::eSuccess) {
            device.destroyImage(image);
            device.freeMemory(memory);
            return false;
        }
        const auto [view_result, view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = FrameFormat,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            },
        });
        if (view_result != vk::Result::eSuccess) {
            device.destroyImage(image);
            device.freeMemory(memory);
            return false;
        }
        slot.image = image;
        slot.memory = memory;
        slot.view = view;
        slot.width = width;
        slot.height = height;
        return true;
    }
};

OpenXrHost::OpenXrHost() : impl{std::make_unique<Impl>()} {}

OpenXrHost::~OpenXrHost() = default;

OpenXrHost& OpenXrHost::Instance() {
    // Never taken down by itself: when the process ends, its threads are gone before the
    // objects are, and there is nothing left to wind the session up with (see Shutdown).
    static OpenXrHost* const instance = new OpenXrHost;
    return *instance;
}

void OpenXrHost::Shutdown() {
    if (impl->thread.joinable()) {
        impl->thread.request_stop();
        impl->thread.join();
    }
    if (impl->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(impl->instance);
        impl->instance = XR_NULL_HANDLE;
        impl->system = XR_NULL_SYSTEM_ID;
    }
    impl->available = false;
}

bool OpenXrHost::Connect() {
    impl->enabled = EnvFlag("SHADPS4_OPENXR", true);
    impl->track_hands = EnvFlag("SHADPS4_XR_HANDS", true);
    impl->feed_head = EnvFlag("SHADPS4_XR_HEAD", true);
    // (A script that plays the game is the gamepad: nothing else has a say then.)
    const char* script = std::getenv("SHADPS4_INPUT_SCRIPT");
    impl->use_controllers =
        EnvFlag("SHADPS4_XR_CONTROLLERS", script == nullptr || script[0] == '\0');
    impl->use_grips = impl->track_hands && EnvFlag("SHADPS4_XR_GRIPS", true);
    if (const char* hand = std::getenv("SHADPS4_XR_PAD_HAND"); hand != nullptr) {
        impl->pad_hand = hand[0] == 'l' || hand[0] == 'L' ? 0 : 1;
    }
    if (const char* offset = std::getenv("SHADPS4_XR_PAD_OFFSET"); offset != nullptr) {
        // Metres to the right, up and towards the player.
        XrVector3f value{};
        if (std::sscanf(offset, "%f,%f,%f", &value.x, &value.y, &value.z) == 3) {
            impl->pad_offset = value;
        }
    }
    impl->predict_ms = EnvFloat("SHADPS4_XR_PREDICT_MS", 20.0f);
    impl->lose_after = EnvFloat("SHADPS4_XR_LOSE_AFTER", 0.0f);
    impl->pause_when_away = EnvFlag("SHADPS4_XR_PAUSE", true);
    if (const char* unwanted = std::getenv("SHADPS4_XR_UNWANTED"); unwanted != nullptr) {
        std::sscanf(unwanted, "%f,%f", &impl->unwanted_from, &impl->unwanted_to);
    }
    impl->hide_for = EnvFloat("SHADPS4_XR_HIDE_FOR", 0.0f);
    impl->connect_time = Clock::now();
    if (!impl->enabled) {
        LOG_INFO(Core_Vr, "The headset of this machine is not looked for (SHADPS4_OPENXR=0)");
        return false;
    }
    if (!impl->CreateInstance()) {
        LOG_INFO(Core_Vr, "No OpenXR runtime that could show frames in a headset: the game is "
                          "shown on the window");
        return false;
    }
    impl->available = true;
    {
        std::scoped_lock lock{impl->names_mutex};
        for (const char* name : UsualInstanceExtensions) {
            AddName(impl->instance_extensions, name);
        }
        for (const char* name : UsualDeviceExtensions) {
            AddName(impl->device_extensions, name);
        }
    }
    // A headset that is there by now says exactly what it needs; one that comes later has to
    // do with the usual.
    const float wait = std::clamp(EnvFloat("SHADPS4_XR_WAIT", 0.0f), 0.0f, 600.0f);
    const auto deadline =
        Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(wait));
    bool told = false;
    while (!(impl->found_at_connect = impl->FindSystem())) {
        if (Clock::now() >= deadline) {
            LOG_INFO(Core_Vr,
                     "{} has no headset connected yet. The game starts on the window and moves "
                     "to the headset when it is connected",
                     impl->runtime_name);
            break;
        }
        if (!told) {
            told = true;
            LOG_INFO(Core_Vr, "Waiting up to {:.0f} s for a headset to be connected through {}",
                     wait, impl->runtime_name);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
    }
    return true;
}

bool OpenXrHost::IsAvailable() const {
    return impl->available;
}

bool OpenXrHost::HasHeadset() const {
    return impl->found_at_connect;
}

std::vector<std::string> OpenXrHost::VulkanInstanceExtensions() const {
    std::scoped_lock lock{impl->names_mutex};
    return impl->instance_extensions;
}

std::vector<std::string> OpenXrHost::VulkanDeviceExtensions() const {
    std::scoped_lock lock{impl->names_mutex};
    return impl->device_extensions;
}

vk::PhysicalDevice OpenXrHost::PreferredPhysicalDevice(vk::Instance instance) const {
    if (!impl->available || impl->system == XR_NULL_SYSTEM_ID) {
        return nullptr;
    }
    const auto get_device = GetFunction<PFN_xrGetVulkanGraphicsDeviceKHR>(
        impl->instance, "xrGetVulkanGraphicsDeviceKHR");
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (get_device == nullptr ||
        XR_FAILED(get_device(impl->instance, impl->system, instance, &device))) {
        return nullptr;
    }
    return vk::PhysicalDevice{device};
}

void OpenXrHost::Start(const Graphics& graphics) {
    if (!impl->available || impl->thread.joinable()) {
        return;
    }
    impl->graphics = graphics;
    if (impl->use_controllers) {
        // What the title asks of the gamepad's motors, for the headset's controllers while
        // they stand in for one. (A gamepad on the PC is driven where its buttons are read.)
        Runtime::Instance().SetPadFeedbackListener([this](const PadFeedback& feedback) {
            impl->rumble_wanted.store(u32{feedback.large_motor} | (u32{feedback.small_motor} << 8),
                                      std::memory_order_relaxed);
        });
    }
    impl->thread = std::jthread{[this](std::stop_token stop) { impl->Run(stop); }};
}

std::optional<OpenXrHost::Target> OpenXrHost::BeginFrame(u32 width, u32 height) {
    if (!impl->accepting.load(std::memory_order_relaxed) || width == 0 || height == 0) {
        return std::nullopt;
    }
    std::scoped_lock lock{impl->slot_mutex};
    impl->DestroyRetired();
    for (u32 attempt = 0; attempt < NumSlots; ++attempt) {
        const u32 index = (impl->next_slot + attempt) % NumSlots;
        Impl::Slot& slot = impl->slots[index];
        // Not what is still being drawn or copied, and not the frame that waits to be shown.
        const bool reusable = slot.state == Impl::Slot::State::Free ||
                              (slot.state == Impl::Slot::State::Ready &&
                               static_cast<s32>(index) != impl->latest);
        if (!reusable) {
            continue;
        }
        if ((!slot.image || slot.width != width || slot.height != height) &&
            !impl->Create(slot, width, height)) {
            LOG_ERROR(Core_Vr, "No image of {}x{} to hand frames to the headset in", width,
                      height);
            return std::nullopt;
        }
        impl->next_slot = (index + 1) % NumSlots;
        slot.state = Impl::Slot::State::Drawing;
        return Target{
            .index = index,
            .image = slot.image,
            .view = slot.view,
            .width = slot.width,
            .height = slot.height,
        };
    }
    return std::nullopt;
}

void OpenXrHost::EndFrame(u32 index, const PresentedFrame& info) {
    std::scoped_lock lock{impl->slot_mutex};
    Impl::Slot& slot = impl->slots[index % NumSlots];
    if (slot.state != Impl::Slot::State::Drawing) {
        return;
    }
    slot.state = Impl::Slot::State::Ready;
    slot.info = info;
    impl->latest = static_cast<s32>(index % NumSlots);
    impl->delivered_frames.fetch_add(1, std::memory_order_relaxed);
    const auto now = Clock::now();
    if (impl->last_delivery != Clock::time_point{}) {
        const double gap = std::chrono::duration<double>(now - impl->last_delivery).count();
        impl->longest_gap = std::max(impl->longest_gap, gap);
        // (Three of a console's frames: a hitch anybody sees.)
        if (gap > 0.050) {
            ++impl->long_gaps;
        }
    }
    impl->last_delivery = now;
}

void OpenXrHost::DropFrame(u32 index) {
    std::scoped_lock lock{impl->slot_mutex};
    Impl::Slot& slot = impl->slots[index % NumSlots];
    if (slot.state == Impl::Slot::State::Drawing) {
        slot.state = Impl::Slot::State::Free;
    }
}

bool OpenXrHost::IsShowing() const {
    return impl->showing.load(std::memory_order_relaxed);
}

std::string OpenXrHost::AudioOutputName() const {
    std::scoped_lock lock{impl->names_mutex};
    return impl->audio_output;
}

std::string OpenXrHost::AudioInputName() const {
    std::scoped_lock lock{impl->names_mutex};
    return impl->audio_input;
}

void OpenXrHost::AudioDevicesChanged() {
    // (A moment later: the system is still settling when it says so.)
    impl->audio_ask_at.store(
        (Clock::now() + std::chrono::milliseconds{700}).time_since_epoch().count(),
        std::memory_order_relaxed);
}

} // namespace Core::Vr
