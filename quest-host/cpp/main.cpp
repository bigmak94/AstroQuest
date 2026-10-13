// SPDX-License-Identifier: GPL-2.0-or-later

// JNI surface of com.astrobotquest.vrhost.MainActivity. The activity owns the lifecycle and
// everything that is easier in Java (controller events, drawing text, unpacking the runtime);
// this side owns the OpenXR session and the emulator process.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <jni.h>
#include <sys/resource.h>

#include "core_process.h"
#include "log.h"
#include "pad_router.h"
#include "xr_host.h"

namespace {

struct App {
    JavaVM* vm{};
    jobject activity{};
    CoreProcess core;
    StatusImage status;
    XrHostStatus xr_status;
    std::thread xr_thread;
    std::atomic<bool> quit{};
    std::atomic<uint32_t> recenter_requests{};
    /// What the game is played with: a gamepad the activity hears of, the headset's own
    /// controllers the session reads, or both.
    PadRouter pads;
};

std::unique_ptr<App> g_app;

std::string ToString(JNIEnv* env, jstring text) {
    if (text == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string result{chars != nullptr ? chars : ""};
    env->ReleaseStringUTFChars(text, chars);
    return result;
}

std::vector<std::string> ToStrings(JNIEnv* env, jobjectArray array) {
    std::vector<std::string> result;
    const jsize count = array != nullptr ? env->GetArrayLength(array) : 0;
    for (jsize i = 0; i < count; ++i) {
        const auto element = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        result.push_back(ToString(env, element));
        env->DeleteLocalRef(element);
    }
    return result;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeStartXr(
    JNIEnv* env, jobject activity, jfloat refresh_rate, jint eye_width, jint eye_height,
    jboolean track_hands, jint sharpen, jboolean cubic, jboolean show_stats, jfloat predict_ms,
    jint dynamic_resolution, jboolean cpu_boost, jboolean own_controllers, jint pad_hand,
    jfloat pad_tilt, jboolean rumble, jboolean stick_touchpad) {
    if (g_app) {
        return;
    }
    g_app = std::make_unique<App>();
    env->GetJavaVM(&g_app->vm);
    g_app->activity = env->NewGlobalRef(activity);
    g_app->pads.SetStickTouchpad(stick_touchpad == JNI_TRUE);
    g_app->pads.SetPadHand(pad_hand);
    g_app->pads.SetNotice([](const char* text) { LOGI("%s", text); });
    g_app->pads.SetSink([app = g_app.get()](const PadRouter::Output& output) {
        app->core.SetPad(output.pad);
        app->core.SetBlowing(output.blowing);
    });

    XrHostOptions options;
    options.refresh_rate = refresh_rate;
    options.eye_width = static_cast<uint32_t>(eye_width);
    options.eye_height = static_cast<uint32_t>(eye_height);
    options.track_hands = track_hands == JNI_TRUE;
    options.sharpen = sharpen;
    options.cubic = cubic == JNI_TRUE;
    options.show_stats = show_stats == JNI_TRUE;
    options.predict_ms = predict_ms;
    options.dynamic_resolution = dynamic_resolution != 0;
    options.follow_layer_size = dynamic_resolution == 2;
    options.cpu_boost = cpu_boost == JNI_TRUE;
    options.own_controllers = own_controllers == JNI_TRUE;
    options.pad_hand = pad_hand == 0 ? 0 : 1;
    options.pad_tilt = pad_tilt;
    options.rumble = rumble == JNI_TRUE;
    g_app->xr_thread = std::thread{[options] {
        App& app = *g_app;
        JNIEnv* thread_env = nullptr;
        app.vm->AttachCurrentThread(&thread_env, nullptr);
        // This thread has little to do, but it has to do it every refresh: it must not wait
        // for the emulator's threads, which outnumber the processor cores.
        ::setpriority(PRIO_PROCESS, 0, -16);
        RunXrHost(app.vm, app.activity, app.core, app.pads, app.status, options, app.xr_status,
                  app.quit, app.recenter_requests);
        LOGI("XR thread finished");
        if (!app.quit) {
            // The system ended the session (the user quit from its menu), or there never was
            // one: without a headset to show it in there is no point in the game running on.
            const jclass activity_class = thread_env->GetObjectClass(app.activity);
            const jmethodID ended =
                thread_env->GetMethodID(activity_class, "onSessionEnded", "()V");
            if (ended != nullptr) {
                thread_env->CallVoidMethod(app.activity, ended);
            }
            if (thread_env->ExceptionCheck()) {
                thread_env->ExceptionClear();
            }
        }
        app.vm->DetachCurrentThread();
    }};
}

JNIEXPORT jboolean JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeStartCore(
    JNIEnv* env, jobject, jstring loader, jstring runtime_root, jstring storage_root, jstring game,
    jstring log_file, jobjectArray extra_args, jobjectArray extra_env) {
    if (!g_app) {
        return JNI_FALSE;
    }
    CoreLaunch launch;
    launch.loader = ToString(env, loader);
    launch.runtime_root = ToString(env, runtime_root);
    launch.storage_root = ToString(env, storage_root);
    launch.game = ToString(env, game);
    launch.log_file = ToString(env, log_file);
    launch.extra_args = ToStrings(env, extra_args);
    // NAME=value pairs.
    for (const std::string& entry : ToStrings(env, extra_env)) {
        const size_t equals = entry.find('=');
        if (equals != std::string::npos && equals > 0) {
            launch.extra_env.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
        }
    }
    return g_app->core.Start(launch) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeStop(JNIEnv* env, jobject) {
    if (!g_app) {
        return;
    }
    g_app->quit = true;
    g_app->core.Stop();
    if (g_app->xr_thread.joinable()) {
        g_app->xr_thread.join();
    }
    env->DeleteGlobalRef(g_app->activity);
    g_app.reset();
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetFieldOfView(
    JNIEnv*, jobject, jint percent) {
    if (g_app) {
        g_app->xr_status.reduced_fov = percent < 100;
    }
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetRefreshRate(
    JNIEnv*, jobject, jfloat rate) {
    if (g_app && rate > 0.0f) {
        g_app->xr_status.wanted_refresh_rate = rate;
    }
}

JNIEXPORT jint JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeRefreshRates(JNIEnv*,
                                                                                     jobject) {
    return g_app ? static_cast<jint>(g_app->xr_status.offered_rates.load()) : 0;
}

JNIEXPORT jfloatArray JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeHeadsetFov(
    JNIEnv* env, jobject) {
    if (!g_app) {
        return nullptr;
    }
    std::scoped_lock lock{g_app->xr_status.optics_mutex};
    const auto& fov = g_app->xr_status.headset_fov;
    if (fov[0] <= 0.0f) {
        return nullptr;
    }
    const jfloatArray result = env->NewFloatArray(4);
    if (result != nullptr) {
        env->SetFloatArrayRegion(result, 0, 4, fov.data());
    }
    return result;
}

JNIEXPORT jint JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeCoreState(JNIEnv*,
                                                                                  jobject) {
    return g_app ? static_cast<jint>(g_app->core.GetState()) : 0;
}

JNIEXPORT jstring JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeCoreMessage(JNIEnv* env,
                                                                                       jobject) {
    return env->NewStringUTF(g_app ? g_app->core.GetMessage().c_str() : "");
}

JNIEXPORT jlong JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeFrameCount(JNIEnv*,
                                                                                    jobject) {
    return g_app ? static_cast<jlong>(g_app->core.GetPresentedFrames()) : 0;
}

/// `pixels` is a direct buffer of StatusImage::Width x Height RGBA pixels, top row first.
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetStatusImage(
    JNIEnv* env, jobject, jobject pixels) {
    if (!g_app) {
        return;
    }
    const auto* data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(pixels));
    const jlong size = env->GetDirectBufferCapacity(pixels);
    const jlong expected = jlong{StatusImage::Width} * StatusImage::Height * 4;
    if (data == nullptr || size < expected) {
        return;
    }
    std::scoped_lock lock{g_app->status.mutex};
    g_app->status.pixels.assign(data, data + expected);
    ++g_app->status.version;
}

/// What the gamepad says: buttons in the PS4 pad's bit layout, sticks from -1 to 1 (to the
/// right and towards the player), triggers from 0 to 1, and the finger on its own touchpad
/// if it has one (0 to 1 from the pad's left and from its far edge).
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeInput(
    JNIEnv*, jobject, jint buttons, jfloat left_x, jfloat left_y, jfloat right_x, jfloat right_y,
    jfloat left_trigger, jfloat right_trigger, jboolean touch_down, jfloat touch_x,
    jfloat touch_y) {
    if (!g_app) {
        return;
    }
    GamepadState state;
    state.buttons = static_cast<uint32_t>(buttons);
    state.left_x = left_x;
    state.left_y = left_y;
    state.right_x = right_x;
    state.right_y = right_y;
    state.left_trigger = left_trigger;
    state.right_trigger = right_trigger;
    state.touch_down = touch_down == JNI_TRUE;
    state.touch_x = touch_x;
    state.touch_y = touch_y;
    g_app->pads.SetGamepad(state, PadRouter::Now());
}

/// Gyroscope in rad/s and accelerometer in m/s², in the controller's frame.
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeMotion(
    JNIEnv*, jobject, jfloat gx, jfloat gy, jfloat gz, jfloat ax, jfloat ay, jfloat az) {
    if (!g_app) {
        return;
    }
    const float gyro[3]{gx, gy, gz};
    const float accel[3]{ax, ay, az};
    g_app->pads.SetGamepadMotion(gyro, accel, PadRouter::Now());
}

/// A gamepad is there to play with (again, or another one than before), or none is any more.
/// `touchpad`: it has a touchpad of its own.
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetGamepad(
    JNIEnv*, jobject, jboolean connected, jboolean touchpad) {
    if (g_app) {
        g_app->pads.SetGamepadConnected(connected == JNI_TRUE, touchpad == JNI_TRUE,
                                        PadRouter::Now());
    }
}

/// How often OPTIONS was pressed in the game so far.
JNIEXPORT jint JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeOptionsPresses(JNIEnv*,
                                                                                       jobject) {
    return g_app ? static_cast<jint>(g_app->pads.Look().options_presses) : 0;
}

/// Shows the panel over the game although the game has a picture (for what the player
/// should read while they play), or no longer.
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeShowPanel(JNIEnv*, jobject,
                                                                                  jboolean show) {
    if (g_app) {
        g_app->xr_status.show_panel = show == JNI_TRUE;
    }
}

/// The host's own menu is over (or not yet): what the player presses is the game's.
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetPlaying(
    JNIEnv*, jobject, jboolean playing) {
    if (g_app) {
        g_app->pads.SetPlaying(playing == JNI_TRUE, PadRouter::Now());
    }
}

/// What the game is played with: 0 nothing, 1 the gamepad, 2 the headset's own controllers.
/// 4 is added while those are in the player's hands, 8 where what the game is played with
/// has no touchpad and buttons do its gestures.
JNIEXPORT jint JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeController(JNIEnv*,
                                                                                   jobject) {
    if (!g_app) {
        return 0;
    }
    const PadRouter::View view = g_app->pads.Look();
    return static_cast<jint>(view.source) | (view.touch_present ? 4 : 0) |
           (view.gesture_buttons ? 8 : 0);
}

/// The player chose with it in the host's menu: 1 the gamepad, 2 the headset's controllers.
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeUseController(
    JNIEnv*, jobject, jint which) {
    if (g_app) {
        g_app->pads.Use(which == 2 ? PadRouter::Source::Touch : PadRouter::Source::Gamepad,
                        PadRouter::Now());
    }
}

/// What the headset's own controllers say to the host's menu: 1 a stick held to the left, 2
/// to the right, 4 what confirms (A or a trigger), 8 a stick held up, 16 down.
JNIEXPORT jint JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeTouchMenu(JNIEnv*,
                                                                                  jobject) {
    return g_app ? static_cast<jint>(g_app->pads.TouchMenu()) : 0;
}

/// What the game asks of the controller: see CoreProcess::GetPadFeedback for the layout.
JNIEXPORT jlong JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativePadFeedback(JNIEnv*,
                                                                                     jobject) {
    return g_app ? static_cast<jlong>(g_app->core.GetPadFeedback()) : 0;
}

/// The player asks for the view to be reset (see XrRecenter in xr_host.h for what of it).
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeRecenter(JNIEnv*, jobject,
                                                                                 jint what) {
    if (g_app) {
        g_app->recenter_requests.fetch_or(static_cast<uint32_t>(what));
    }
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetMicrophone(
    JNIEnv*, jobject, jboolean enabled, jfloat gain) {
    if (g_app) {
        g_app->core.SetMicrophone(enabled == JNI_TRUE, gain);
    }
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetLogFile(JNIEnv* env,
                                                                                   jclass,
                                                                                   jstring path) {
    SetLogFile(ToString(env, path).c_str());
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeLog(JNIEnv* env, jclass,
                                                                            jint priority,
                                                                            jstring message) {
    HostLog(priority, "%s", ToString(env, message).c_str());
}

/// Bit 0: the headset session is running. Bit 1: the hands holding the controller are seen.
/// Bit 2: the one of the headset's own controllers that is the controller in the game is.
/// Bits 8 to 15: the display's refresh rate in Hz.
JNIEXPORT jint JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeXrStatus(JNIEnv*,
                                                                                 jobject) {
    if (!g_app) {
        return 0;
    }
    const int refresh_rate =
        std::clamp(static_cast<int>(g_app->xr_status.refresh_rate.load() + 0.5f), 0, 255);
    return (g_app->xr_status.session_running ? 1 : 0) | (g_app->xr_status.hands_tracked ? 2 : 0) |
           (g_app->xr_status.controller_tracked ? 4 : 0) | (refresh_rate << 8);
}

} // extern "C"
