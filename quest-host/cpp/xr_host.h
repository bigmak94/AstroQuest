// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#include <jni.h>

class CoreProcess;
class PadRouter;

/// A picture the Java side draws (text is far easier there) and the headset shows on a panel
/// whenever the game itself has nothing on screen: one that stands still in the room before the
/// game's first picture (the start-up menu and what follows it), one that follows the head over
/// the game (the game stopped drawing, the controls card, stats=1).
struct StatusImage {
    static constexpr int Width = 1024;
    static constexpr int Height = 512;

    std::mutex mutex;
    std::vector<uint8_t> pixels; ///< RGBA, top row first
    uint64_t version{};
};

struct XrHostOptions {
    /// Display refresh rate to ask for; 0, or one the display refuses: the host's own order (90,
    /// 120, 80, 72 Hz).
    float refresh_rate{0.0f};
    /// Size of one eye in the buffers frames are delivered in.
    uint32_t eye_width{1440};
    uint32_t eye_height{1536};
    /// Use the wearer's hands, as the headset sees them around the gamepad, to place the
    /// controller in the game.
    bool track_hands{true};
    /// What the compositor does to the game's picture, which is coarser than the display, on
    /// top of showing it: 0 and 1 nothing (with 1 the emulator sharpens the picture itself,
    /// which is not this struct's business), 2 its Super Resolution filter on every refresh,
    /// 3 the same but only while the system finds the GPU has the time for it, 4 its plainer
    /// and cheaper sharpening filter.
    int sharpen{1};
    /// Have the compositor read the game's picture with a cubic filter instead of a linear
    /// one where it enlarges it: sharper, for a little of the GPU's time on every refresh.
    bool cubic{};
    /// Keep the status panel in view (small, below the middle) while the game runs, for the
    /// figures the app writes on it.
    bool show_stats{};
    /// Ask the system from moment to moment for the size it recommends for the picture. It
    /// counts an app that asks among those that give way when the GPU is short, which is its
    /// condition for the higher GPU clock levels. This app gives way where the GPU's time
    /// goes: in the size the game draws its scene at, which the emulator sees to.
    bool dynamic_resolution{true};
    /// Also show the picture at the recommended size instead of at full size. That saves next
    /// to nothing (the picture is copied once per frame of the game) and blurs what the game
    /// drew; it is what the app did up to version 0.6.
    bool follow_layer_size{};
    /// Ask for the processor's "boost" level: its fastest clock for the first 45 seconds and
    /// for up to a fifth of the time after that, as the system sees fit. That is when the
    /// emulator translates most of the game's code.
    bool cpu_boost{true};
    /// For the probe that runs the host without anybody wearing the headset: ask the session
    /// that never begins for everything that is otherwise asked when it does (refresh rate,
    /// performance levels, thread hints), to have the answers in the log.
    bool probe{};
    /// How far beyond the next display refresh the head pose given to the game is predicted,
    /// in milliseconds. A frame is on the display some tens of milliseconds after the game
    /// read the pose it drew it for; aiming at that moment leaves the compositor less to
    /// correct, which is what shows as black edges when the head turns.
    float predict_ms{25.0f};
    /// Let the headset's own two controllers play the game where the player holds them
    /// instead of a gamepad (see PadRouter for what does what).
    bool own_controllers{true};
    /// Which of the two is the controller in the game then: 1 the right one, 0 the left.
    int pad_hand{1};
    /// Let them shake when the game shakes the gamepad.
    bool rumble{true};
    /// For a gamepad without motion sensors, which the hands holding it turn and tilt in the
    /// game: how far its front is taken to point above (or, negative, below) the way the
    /// hands point, in degrees.
    float pad_tilt{0.0f};
};

/// What a request to reset the view asks for (bits of the counter RunXrHost is given).
namespace XrRecenter {
/// The way the controller points right now is straight ahead.
inline constexpr uint32_t Pad = 1;
/// Where the head is now is where the player sits, and the way it faces is straight ahead.
inline constexpr uint32_t Seat = 2;
} // namespace XrRecenter

/// What the host worked out about the session, for the status panel and for logging.
struct XrHostStatus {
    std::atomic<bool> session_running{};
    std::atomic<bool> hands_tracked{};
    /// The one of the headset's own controllers that is the controller in the game is seen.
    std::atomic<bool> controller_tracked{};
    /// The panel is to be shown over the game: it has something to say to who is playing.
    std::atomic<bool> show_panel{};
    std::atomic<float> refresh_rate{};
    /// A display refresh rate the startup menu asks to switch to now, 0 when nothing is asked.
    std::atomic<float> wanted_refresh_rate{};
    /// The refresh rates the display offers: bit 0 72 Hz, bit 1 80, bit 2 90, bit 3 120 (0 until
    /// the session has said).
    std::atomic<uint32_t> offered_rates{};
    /// Set when the startup choice is confirmed, before the game can deliver any frames.
    std::atomic<bool> reduced_fov{};
    /// Full projection tangents reported by this headset: out, in, up, down.
    /// The startup menu snapshots them before the emulator is launched.
    std::mutex optics_mutex;
    std::array<float, 4> headset_fov{};
};

/// Owns the OpenXR session: feeds the head pose to the core and shows the stereo frames it
/// delivers, reads the headset's own controllers for `pads` and places the controller the
/// game is played with. Blocks until `quit` is set or the system ends the session.
void RunXrHost(JavaVM* vm, jobject activity, CoreProcess& core, PadRouter& pads,
               StatusImage& status, const XrHostOptions& options, XrHostStatus& host_status,
               std::atomic<bool>& quit, std::atomic<uint32_t>& recenter_requests);
