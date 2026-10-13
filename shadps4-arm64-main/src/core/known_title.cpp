// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/known_title.h"
#include "core/known_title_builds.h"
#include "core/known_title_scenes.h"
#include "core/memory.h"
#include "core/vr/vr_runtime.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Core::KnownTitle {

namespace {

using Clock = std::chrono::steady_clock;

// Astro Bot Rescue Mission, CUSA12392. Where things are in each build of it that is known from
// inside, and how a build is told from the others, is in known_title_builds.h; what follows is
// the same in all of them.
using Builds::Build;
using Builds::ConsoleSizes;
using Builds::FirstHeadsetLevel;
using Builds::LastHeadsetLevel;

// The tracking manager, a singleton: the point it counts positions from, and for the headset
// the position counted from it.
constexpr u64 ManagerOrigin = 0x6210;
constexpr u64 ManagerHeadState = 0x6848;
constexpr u64 StatePosition = 0x10;
constexpr u64 StatePositionValid = 0x54;

// What sets the size the scene is drawn at, a singleton made when first asked for: the size is
// one of a list (ConsoleSizes, the first three are for a television), a base (4 on the
// console, 6 on its Pro model) plus an offset that the game moves between a lowest and a
// highest one by how long it finds the GPU to take over its drawing. It reads that off
// timestamps which, emulated, tell how long the emulator took to pass the drawing on, not how
// long the GPU takes over it: left to itself, the game draws large where the GPU is busiest.
constexpr u64 ResolutionBase = 0x0;     // s32
constexpr u64 ResolutionLevel = 0x4;    // s32
constexpr u64 ResolutionOffset = 0x20;  // s32
constexpr u64 ResolutionHighest = 0x28; // s32, an offset
constexpr u64 ResolutionLowest = 0x2c;  // s32, an offset

/// The build of the title that runs, and where its image starts, from the moment its image is
/// loaded and found to be one of those known (OnGameLoaded); nullptr for every other title and
/// every other build.
std::atomic<const Build*> known_build{nullptr};
VAddr known_base{};

/// The size the title draws its scene at for an eye right now, width << 16 | height; 0 while
/// it is not known.
std::atomic<u32> scene_size{0};

/// The title drawing larger than it does on the console: every size of the headset's list (and
/// the pictures handed to the headset) grown by the same factor, and the memory that takes.
/// SHADPS4_TITLE_EYE_WIDTH=<pixels> is the width of the largest, 1440 on the console.
struct Larger : Builds::Sizes {
    double factor{1.0};
    /// What the console's memory has to grow by for it, in MB.
    s32 extra_memory_mb{};
};

const Larger& GetLarger() {
    static const Larger larger = [] {
        Larger result;
        const char* value = std::getenv("SHADPS4_TITLE_EYE_WIDTH");
        const s32 width = value != nullptr ? std::atoi(value) : 0;
        if (width <= static_cast<s32>(ConsoleSizes[LastHeadsetLevel][0])) {
            return result;
        }
        result.factor = std::min(static_cast<double>(width), 4320.0) / ConsoleSizes[6][0];
        const auto to_eighths = [&](u32 size) {
            return static_cast<u32>(std::lround(size * result.factor / 8.0)) * 8;
        };
        for (s32 level = FirstHeadsetLevel; level <= LastHeadsetLevel; ++level) {
            result.sizes[level] = {to_eighths(ConsoleSizes[level][0]),
                                   to_eighths(ConsoleSizes[level][1])};
        }
        // What they take grows with their pixels, and some room besides.
        static constexpr u64 MB = 1ull << 20;
        const double pixels = result.factor * result.factor;
        const auto megabytes = [&](u64 bytes) {
            return (static_cast<u64>(static_cast<double>(bytes) * pixels * 1.1) + MB - 1) / MB * MB;
        };
        result.target_pool = static_cast<u32>(megabytes(Builds::ConsoleTargetPool));
        result.small_pool = static_cast<u32>(megabytes(Builds::ConsoleSmallPool));
        const u64 growth = (result.target_pool - Builds::ConsoleTargetPool) +
                           (result.small_pool - Builds::ConsoleSmallPool) +
                           static_cast<u64>(200.0 * MB * (pixels - 1.0));
        result.graphics_heap = Builds::ConsoleGraphicsHeap + growth;
        result.extra_memory_mb = static_cast<s32>((growth + 256 * MB) / (256 * MB) * 256);
        return result;
    }();
    return larger;
}

/// "1440x1536" and the like, for a level of the list.
std::string SizeName(s32 level) {
    if (level < 0 || level > LastHeadsetLevel) {
        return "a size not known";
    }
    const auto& size = GetLarger().sizes[level];
    return fmt::format("{}x{}", size[0], size[1]);
}

template <typename T>
T Read(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

template <typename T>
void Write(VAddr address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

/// The time step the title was made for.
constexpr double Nominal = 1.0 / 60.0;
/// A frame that takes several times what frames take is a load or a hitch: nothing the game is
/// to catch up with by running fast afterwards, and nothing that says how fast it draws.
constexpr double Stall = 0.25;
/// The most refreshes of the headset a frame is ever given.
constexpr s32 SlowestPace = 6;
/// The refreshes of the display frames are given at the moment, for the thread that makes
/// the headset's.
std::atomic<u32> frame_pace{0};

struct Settings {
    bool time_step{true};
    /// Frames longer than this are not made up for: the game slows down instead of taking
    /// steps it was never tried with. Twice the step it was made for is as far as that goes:
    /// with steps of 37 ms and more (35 are still fine) something in the game comes out as no
    /// number at all. Seen in level 3-2 (INDY CAVE 1): the robot that hangs tied up there is
    /// then at no place from the moment the level loads, and its cry for help, which should
    /// fade with distance, is heard at full strength wherever the player is. At 30 frames a
    /// second every hitch got it there, because the frames after one are given longer steps to
    /// catch up: those steps end here too.
    double longest_step{1.0 / 30.0};

    enum class Resolution { Title, Pinned, Governed };
    Resolution resolution{Resolution::Governed};
    s32 pinned_level{FirstHeadsetLevel};

    /// Refreshes of the display a frame is given: 0 is chosen by what the title manages, 1
    /// and more is that many throughout.
    s32 pace{};
    /// The fewest the choice may come to where the display refreshes faster than the title
    /// was made to draw: 2 is the title's own way (a frame for every two refreshes), 1 has it
    /// draw a frame for every refresh, faster than it ever did on the console.
    s32 fastest_pace{2};
    /// The most frames a second wanted, 0 for no such limit: frames are given at least as many
    /// refreshes as keep them to that.
    double fps_cap{};
    /// Frames a second to hold the title to throughout, 0 to choose: frames are given the most
    /// refreshes that still make that many (3 at 90 Hz and 4 at 120 Hz for 30), and only the
    /// size of the scene follows what the GPU manages.
    double steady_fps{};
    /// The largest size the scene is drawn at where the size is chosen here.
    s32 largest_level{LastHeadsetLevel};

    /// The title's physics take each step with the time step its bodies were sent with
    /// (Builds::PhysicsStepChanges): what a time step that changes needs.
    bool physics_step{true};
    /// Where the bodies the title moves by hand end up goes to the log (PhysicsWatch).
    bool physics_watch{false};

    /// How much of what the title draws is drawn (SceneFile).
    Scenes::Detail scene_detail{};
};

const Settings& GetSettings() {
    static const Settings settings = [] {
        Settings parsed;
        if (const char* value = std::getenv("SHADPS4_TITLE_TIMESTEP"); value != nullptr) {
            const double rate = std::atof(value);
            if (rate <= 0.0) {
                parsed.time_step = false;
            } else {
                parsed.longest_step = 1.0 / std::clamp(rate, 10.0, 60.0);
            }
        }
        if (const char* value = std::getenv("SHADPS4_TITLE_RESOLUTION"); value != nullptr) {
            const std::string_view text{value};
            const s32 level = std::atoi(value);
            if (text == "title") {
                parsed.resolution = Settings::Resolution::Title;
            } else if (level >= FirstHeadsetLevel && level <= LastHeadsetLevel) {
                parsed.resolution = Settings::Resolution::Pinned;
                parsed.pinned_level = level;
            }
        }
        if (const char* value = std::getenv("SHADPS4_VR_PACE"); value != nullptr) {
            const s32 pace = std::atoi(value);
            parsed.pace = pace >= 1 ? std::min(pace, SlowestPace) : 0;
        }
        if (const char* value = std::getenv("SHADPS4_VR_FASTEST_PACE"); value != nullptr) {
            parsed.fastest_pace = std::atoi(value) == 1 ? 1 : 2;
        }
        // SHADPS4_VR_FPS_CAP=<frames a second>: as many as that at most, as many refreshes of
        // the display a frame as that takes (at 120 Hz: 120, 60, 40 or 30; at 90 Hz: 90, 45 or
        // 30). Frames come faster than the console's 60 only this way.
        if (const char* value = std::getenv("SHADPS4_VR_FPS_CAP"); value != nullptr) {
            const double cap = std::atof(value);
            if (cap >= 10.0) {
                parsed.fps_cap = std::min(cap, 240.0);
            }
        }
        // SHADPS4_VR_STEADY_FPS=<frames a second>: always that many (as many refreshes a frame
        // as still make them: 3 at 90 Hz, 4 at 120 Hz for 30; at 72 Hz 2, which is 36), the
        // scene's size alone following what the GPU manages.
        if (const char* value = std::getenv("SHADPS4_VR_STEADY_FPS"); value != nullptr) {
            const double fps = std::atof(value);
            if (fps >= 10.0) {
                parsed.steady_fps = std::min(fps, 120.0);
            }
        }
        // SHADPS4_VR_LARGEST_SIZE=<3..6>: where the size is chosen here, none larger than that
        // (3 = 816x870 ... 6 = 1440x1536).
        if (const char* value = std::getenv("SHADPS4_VR_LARGEST_SIZE"); value != nullptr) {
            const s32 level = std::atoi(value);
            if (level >= FirstHeadsetLevel && level <= LastHeadsetLevel) {
                parsed.largest_level = level;
            }
        }
        // SHADPS4_TITLE_PHYSICS_STEP=0 leaves the title's physics as the console has them;
        // SHADPS4_TITLE_PHYSICS_WATCH=1 tells in the log where its bodies end up.
        if (const char* value = std::getenv("SHADPS4_TITLE_PHYSICS_STEP"); value != nullptr) {
            parsed.physics_step = std::atoi(value) != 0;
        }
        if (const char* value = std::getenv("SHADPS4_TITLE_PHYSICS_WATCH"); value != nullptr) {
            parsed.physics_watch = std::atoi(value) != 0;
        }
        // SHADPS4_TITLE_LOD=<share>: the distances at which models change to their coarser
        // forms, as a share of the title's own (0.33 = a third); SHADPS4_TITLE_SHADOW_MAP=
        // <1024|1536>: the sun's shadow maps no larger than that; SHADPS4_TITLE_DEPTH_PREPASS=0:
        // the scene's depth not drawn on its own first.
        if (const char* value = std::getenv("SHADPS4_TITLE_LOD"); value != nullptr) {
            const double share = std::atof(value);
            if (share > 0.0) {
                parsed.scene_detail.lod_scale = std::clamp(share, 0.1, 1.0);
            }
        }
        if (const char* value = std::getenv("SHADPS4_TITLE_SHADOW_MAP"); value != nullptr) {
            const int size = std::atoi(value);
            parsed.scene_detail.shadow_map_size = size == 1024 || size == 1536 ? size : 0;
        }
        if (const char* value = std::getenv("SHADPS4_TITLE_DEPTH_PREPASS"); value != nullptr) {
            parsed.scene_detail.depth_prepass = std::atoi(value) != 0;
        }
        // SHADPS4_TITLE_PREPASS_DISTANCE=<m>, SHADPS4_TITLE_PREPASS_RADIUS=<m>: how far away
        // models still get the depth pre-pass, and how small they may be (measuring aids).
        if (const char* value = std::getenv("SHADPS4_TITLE_PREPASS_DISTANCE"); value != nullptr) {
            parsed.scene_detail.prepass_distance = std::atof(value);
        }
        if (const char* value = std::getenv("SHADPS4_TITLE_PREPASS_RADIUS"); value != nullptr) {
            parsed.scene_detail.prepass_radius = std::atof(value);
        }
        // SHADPS4_TITLE_DARK_SUN_SHADOWS=0 keeps the sun's shadow maps in the levels whose sun
        // is black (Scenes::DarkSunFiles), =2 also leaves them out in 3-2.
        if (const char* value = std::getenv("SHADPS4_TITLE_DARK_SUN_SHADOWS"); value != nullptr) {
            parsed.scene_detail.dark_sun = std::clamp(std::atoi(value), 0, 2);
        }
        return parsed;
    }();
    return settings;
}

/// The share of a frame's GPU work that does not grow with the scene's pixels: the shadow maps,
/// the game's eye pictures (1440x1536 whatever the scene's size), the copies for the headset and
/// what every draw costs whatever its size. Measured on the headset with the models lighter and
/// the edges shaded coarsely, the two sizes taking turns within one run at the same clock
/// (2026-10-09, level 3-2, 30 frames a second): 960x1080 cost 1.21 times what 816x870 did, and
/// 1200x1280 1.20 times what 960x1080 did (counted here as 1.26: the larger sizes a little
/// dearer than seen, where a step up that fails is what costs most).
constexpr double FixedShare = 0.55;
/// What a frame's GPU work at each size is counted as, in terms of the smallest.
constexpr std::array<double, 7> SizeCost = [] {
    std::array<double, 7> cost{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    const double smallest = static_cast<double>(ConsoleSizes[FirstHeadsetLevel][0]) *
                            static_cast<double>(ConsoleSizes[FirstHeadsetLevel][1]);
    for (s32 level = FirstHeadsetLevel; level <= LastHeadsetLevel; ++level) {
        const double pixels = static_cast<double>(ConsoleSizes[level][0]) *
                              static_cast<double>(ConsoleSizes[level][1]);
        cost[level] = FixedShare + (1.0 - FixedShare) * pixels / smallest;
    }
    return cost;
}();
/// The same as measured on the headset with the title's own detail (2026-10-02), where most of
/// a frame's work does not grow with its pixels: what a smaller size is counted to save where a
/// faster pace is weighed.
constexpr std::array<double, 7> CautiousSmallerCost{1.0, 1.0, 1.0, 1.0, 1.08, 1.27, 1.54};

/// Chooses how many refreshes of the headset a frame is given, and the size the scene is drawn
/// at, from what the title manages and how busy the GPU really is. Asked every frame with what
/// the frame took; answers with the size to hold the title to.
///
/// The title waits for two refreshes to pass before it goes on to the next frame, and goes
/// on at once when the frame took longer than that: it then draws as fast as it can, at a
/// rate that has nothing to do with the display's, and its frames are shown for two refreshes
/// or for three as they happen to fall, which is what jerky motion is. So the question is
/// never "how fast" alone but "how many refreshes, and what fits into them":
///  - the pace is the fastest at which the smallest size wanted fits with room to spare. That
///    size is the console's own smallest (960x1080) wherever frames are given less than 30 ms
///    (40 frames a second and more): a sharper picture is worth more than 40 frames a second
///    instead of 30. Only at the slowest pace frames are held to (30 frames a second; at 72 or
///    80 Hz the one pace there is, 36 or 40), or where the pace is not the emulator's to choose,
///    will the smallest size of all (816x870) do, since fewer frames than that are worse than a
///    softer picture.
///    The emulated headset refreshes at half that pace (FramePace), which is what the title
///    keeps time by, and every frame is shown for as long as the next;
///  - the size is the largest that fits that pace with room to spare;
///  - what a size costs is taken from what it was seen to cost, at the clock the GPU could
///    have run at, as if about half the work grew with the pixels (FixedShare); where frames
///    do not fit, by the time the GPU really took. (What a step between two sizes was seen to
///    cost goes to the log; while the player moves it says more about the view than about the
///    size, so it does not count.) The picture only grows where that has left a fifth of a
///    frame's time to spare for four seconds in a row (a frame that came late, or one that held
///    everything up, starts the count again), not within ten seconds of the last change, and
///    never beyond the largest size allowed (SHADPS4_VR_LARGEST_SIZE: the Quest app has
///    1200x1280; 1440x1536 fits only in light scenes, and the heavy scene that follows one is
///    drawn too large until the size comes down); frames that do not fit are given a smaller
///    picture before they are given more refreshes;
///  - every change costs frames that come unevenly while it settles, and a faster pace that
///    turns out not to fit costs seconds of them. So a faster pace is only tried after five
///    seconds in which all a frame takes fits it with a quarter to spare: the GPU's work, the
///    GPU thread's processor time, and the title's own work where a try at that pace showed it
///    to be too much, the GPU's work counted as if a smaller picture saved little. A failed try
///    makes the next one wait longer, a size that had to be taken back within half a minute is
///    gone back from (and counts as dearer, until the title loads another scene; the next step
///    up waits a quarter of a minute, then twice as long each time), the picture only grows
///    once a faster pace has held, and after two seconds of loading in which nothing was
///    drawn it starts again from the console's smallest size at most. (Astro Bot loads its
///    levels while it goes on drawing at full speed: that is not seen as loading.)
class Governor {
public:
    /// For `fixed`: the size is the title's own business.
    static constexpr s32 NotMine = -1;

    struct Summary {
        s32 pace;
        double slot;
        double load;
        /// What the next larger size is counted to cost, in terms of this one's work (0 at the
        /// largest).
        double next_cost;
    };

    /// `fixed_pace_`: refreshes to give every frame, 0 to choose; `fastest_pace_`: the fewest
    /// the choice may come to on a display that refreshes faster than the title draws.
    Governor(s32 fixed_pace_, s32 fastest_pace_, double fps_cap_, double steady_fps_,
             s32 largest_level_)
        : fixed_pace{fixed_pace_}, fastest_pace{fastest_pace_}, fps_cap{fps_cap_},
          steady_fps{fixed_pace_ >= 1 ? 0.0 : steady_fps_},
          largest{std::clamp(largest_level_, FirstHeadsetLevel, LastHeadsetLevel)},
          pace{fixed_pace_ >= 1 ? fixed_pace_ : 2} {
        level = std::min(level, largest);
        size_before_up = level;
    }

    /// `fixed` is a size to keep to, 0 where the size is to be chosen here, NotMine where the
    /// title chooses it. Answers with the size to hold the title to, 0 for none.
    s32 Level(double frame, Clock::time_point now, s32 fixed) {
        const float rate = Vr::Runtime::Instance().HeadsetRefreshRate();
        refresh = 1.0 / static_cast<double>(rate > 0.0f ? rate : 120.0f);
        if (fixed > 0) {
            level = fixed;
        }
        // A display that refreshes no faster than the title draws on the console (60 times a
        // second, or half as often as a faster one where the system makes up every other
        // picture itself): the title's two refreshes for a frame would halve its frame
        // rate, one refresh for a frame is its own.
        s32 fastest_now = refresh > SlowDisplay ? 1 : fastest_pace;
        if (fps_cap > 0.0) {
            // The fewest refreshes that keep frames to the cap.
            fastest_now = std::max<s32>(
                1, static_cast<s32>(std::ceil(1.0 / (refresh * fps_cap) - 0.01)));
        }
        if (steady_fps > 0.0) {
            // The most refreshes that still make the frames asked for, whatever the display's
            // rate is now.
            const s32 steady = std::clamp<s32>(
                static_cast<s32>(std::floor(1.0 / (refresh * steady_fps) + 0.01)), 1, SlowestPace);
            if (steady != pace && started) {
                LOG_INFO(Core,
                         "Frames are given {} refreshes from now on ({:.0f} a second): the "
                         "display refreshes {:.0f} times a second",
                         steady, 1.0 / (steady * refresh), 1.0 / refresh);
            }
            pace = steady;
            fastest_now = steady;
        }
        if (fixed_pace == 0 && steady_fps == 0.0 &&
            (fastest_now != fastest || pace < fastest_now)) {
            // The display turned out to be another kind than was thought (its rate is only
            // known once the headset is there): no waiting to find out what fits.
            if (fastest_now == 1 && refresh > SlowDisplay) {
                pace = 1;
            } else {
                pace = std::max(pace, fastest_now);
            }
        }
        fastest = fastest_now;
        if (!started) {
            started = true;
            window_start = now;
            changed = now;
            Vulkan::FrameStats::TakeGpuLoad();
            Vulkan::FrameStats::TakeGpuWork();
            Vulkan::FrameStats::TakeGpuThreadTime();
        } else {
            if (frame <= Stall) {
                window_time += frame;
                ++window_frames;
            } else {
                ++window_stalls;
            }
            if (now - window_start >= Window) {
                Decide(now, fixed);
            }
        }
        return fixed == NotMine ? 0 : level;
    }

    /// The refreshes of the display a frame is given.
    u32 Pace() const {
        return static_cast<u32>(pace);
    }

    /// What frames were given and how busy the GPU was since this was last asked.
    Summary TakeSummary() {
        const Summary summary{
            .pace = pace,
            .slot = pace * refresh,
            .load = summary_windows != 0 ? summary_load / summary_windows : 0.0,
            .next_cost = sizing && level >= FirstHeadsetLevel && level < largest
                             ? cost[level + 1] / cost[level] * markup[level + 1] / markup[level]
                             : 0.0,
        };
        summary_load = 0.0;
        summary_windows = 0;
        return summary;
    }

private:
    /// What frames took, and the GPU thread's processor time a frame, when a try at a pace
    /// failed with the GPU resting often enough: the title's own work did not fit it.
    struct OwnWork {
        double taken{};
        double thread{};
        Clock::time_point at{};
    };

    void Decide(Clock::time_point now, s32 fixed) {
        const float load = Vulkan::FrameStats::TakeGpuLoad();
        const float clocked = Vulkan::FrameStats::TakeGpuWork();
        const double thread = Vulkan::FrameStats::TakeGpuThreadTime();
        const u32 frames = window_frames;
        const u32 stalls = window_stalls;
        const double taken = frames != 0 ? window_time / frames : 0.0;
        window_start = now;
        window_time = 0.0;
        window_frames = 0;
        window_stalls = 0;
        sizing = fixed == 0;
        const bool idle = load < 0.0f || frames < FewestFrames;
        if (idle || stalls != 0) {
            // Loading, nothing drawn, or a frame that held everything up for long (the GPU
            // rested through it, and what it was busy with is not what frames take): nothing to
            // go by. Nor does what comes after it say what a step of size made before it cost.
            tight_windows = 0;
            faster_windows = 0;
            up_windows = 0;
            watched.reset();
            level_windows = 0;
            if (!idle) {
                return;
            }
            if (++idle_windows == NewScene) {
                // A scene loaded for long, with nothing drawn, is another one: what the last one
                // taught of the sizes is not this one's, nor is a step up that fitted it. And it
                // starts at the console's smallest size at most: one the last scene had room
                // for may be far too much for this one, and frames that come late at its start
                // are seen. (A title that loads while it goes on drawing, as Astro Bot does
                // between its levels, is not caught by this.)
                markup.fill(1.0);
                cost = SizeCost;
                patience = FirstPatience;
                went_up = {};
                up_allowed = now + SettleUp;
                up_windows = 0;
                if (sizing && level > UsualLevel) {
                    Change(UsualLevel, now, taken, load, false, false);
                }
            }
            return;
        }
        idle_windows = 0;
        summary_load += std::min(load, 1.0f);
        ++summary_windows;
        // For the log.
        window_work = clocked >= 0.0f ? std::min(clocked, load) : -1.0f;
        window_thread = thread;

        const bool sized = fixed == 0;
        // (Frames held to a steady rate: the pace is not this one's to choose.)
        const bool paced = fixed_pace == 0 && steady_fps == 0.0;
        const double slot = pace * refresh;
        // How long the GPU worked on a frame, at the fastest clock it was seen to run at where
        // its clock is known (at least: one that never rests may have more to do than it gets
        // done).
        const float work = clocked >= 0.0f ? std::min(clocked, load) : load;
        const double gpu = std::min(work, 1.0f) * taken;
        const auto since_change = now - changed;
        // The GPU's work a frame at this size and pace over the windows since it was taken, for
        // what a step from it teaches (the window that asks for a step is the least like the
        // rest: averaged in with a quarter's weight).
        if (level_windows == 0 || level != level_of_average || pace != pace_of_average) {
            level_average = gpu;
            level_of_average = level;
            pace_of_average = pace;
            level_windows = 1;
        } else {
            level_average += (gpu - level_average) * 0.25;
            ++level_windows;
        }
        if (watched && watched->to == level) {
            if (load < LearningLoad) {
                // The GPU rests too much for a step of size to show what it costs.
                watched.reset();
            } else if (!watched->settled) {
                // Frames drawn at the old size are still in the first window after the step.
                watched->settled = true;
            } else {
                watched->after += gpu;
                if (++watched->windows == StepWindows) {
                    Learn();
                }
            }
        }
        // What another size would cost the GPU, from what this one does.
        const auto cost_at = [&](s32 other) {
            return gpu * cost[other] / cost[level] * markup[other] / markup[level];
        };
        // The same where a faster pace is weighed, a smaller size counted as saving little: a
        // faster pace that does not hold costs more than one not tried.
        const auto cautious_cost_at = [&](s32 other) {
            if (other >= level) {
                return cost_at(other);
            }
            return std::max(cost_at(other), gpu * CautiousSmallerCost[other] /
                                                 CautiousSmallerCost[level] * markup[other] /
                                                 markup[level]);
        };
        // The slowest pace frames are ever held to, and the smallest size wanted at a pace.
        const s32 slowest = std::max(std::max(2, fastest), static_cast<s32>(SlowestWanted / refresh));
        const auto smallest_at = [&](s32 refreshes_given) {
            return std::min(!paced || refreshes_given >= slowest ||
                                    refreshes_given * refresh >= SmallestSizeSlot
                                ? FirstHeadsetLevel
                                : UsualLevel,
                            largest);
        };
        // The largest size that fits a pace with room to spare, if any does.
        const auto largest_fitting = [&](s32 refreshes_given, s32 from, s32 down_to) {
            for (s32 size = from; size > down_to; --size) {
                if (cost_at(size) * (1.0 + SizeMargin) <= refreshes_given * refresh) {
                    return size;
                }
            }
            return down_to;
        };
        if (paced && pace > slowest) {
            // The display refreshes more slowly than it did.
            SetPace(slowest, now, taken, load);
            tight_windows = 0;
            faster_windows = 0;
            return;
        }
        if (now - last_regret > std::max<Clock::duration>(4 * patience, Calm)) {
            patience = FirstPatience;
        }
        if (now - last_pace_regret > std::max<Clock::duration>(4 * pace_patience, Calm)) {
            pace_patience = FirstPacePatience;
        }

        if (taken > slot * (1.0 + Over)) {
            // Frames do not fit the refreshes they are given: the title is on its own pace.
            faster_windows = 0;
            up_windows = 0;
            ++tight_windows;
            if (tight_windows < 2 || since_change < SettleDown) {
                return;
            }
            // A GPU that hardly rests is what holds frames up, and a smaller picture helps;
            // one that rests for long is not, and a smaller picture would change nothing.
            const bool gpu_short = load > GpuShort;
            const auto slower = [&](s32 to) {
                if (now - went_faster < PaceRegret) {
                    // The faster pace was a mistake: the next try waits longer, and where the
                    // GPU was not what held frames up, what the title's own work took at that
                    // pace is kept in mind.
                    pace_patience = std::min(pace_patience * 2, LongestPatience);
                    last_pace_regret = now;
                    if (!gpu_short) {
                        own_work[pace] = {.taken = taken, .thread = thread, .at = now};
                    }
                }
                SetPace(to, now, taken, load);
                went_faster = {};
                faster_allowed = now + pace_patience;
            };
            if (sized && gpu_short && now - went_up < Regret && level > size_before_up) {
                // The step up was a mistake: back to the size that fitted, at this pace, and
                // that size counts as dearer from now on (and so do the larger ones).
                patience = std::min(patience * 2, LongestPatience);
                for (s32 size = level; size <= LastHeadsetLevel; ++size) {
                    markup[size] = std::min(markup[size] * Dearer, DearestMarkup);
                }
                last_regret = now;
                Change(size_before_up, now, taken, load, false, false);
                went_up = {};
                up_allowed = now + patience;
                tight_windows = 0;
                return;
            }
            if (sized && gpu_short && (level > smallest_at(pace) || (paced && pace < slowest))) {
                // Straight to what the GPU's time says will do: the largest size that fits
                // this pace, or the pace at which the smallest size wanted fits and the largest
                // that fits that. Judged by the time the GPU really took: one that runs slower
                // than it did (warm, or held back by the system) takes longer than its work at
                // the fastest clock, and the frames that do not fit are what counts. (A GPU
                // that never rests may need more than it is seen to take; then this comes
                // round again.)
                const double gpu_now = std::max(gpu, static_cast<double>(std::min(load, 1.0f)) *
                                                         taken);
                const auto cost_now_at = [&](s32 other) {
                    return gpu_now * cost[other] / cost[level] * markup[other] / markup[level];
                };
                const auto largest_fitting_now = [&](s32 refreshes_given, s32 from, s32 down_to) {
                    for (s32 size = from; size > down_to; --size) {
                        if (cost_now_at(size) * (1.0 + SizeMargin) <= refreshes_given * refresh) {
                            return size;
                        }
                    }
                    return down_to;
                };
                const s32 smallest = smallest_at(pace);
                s32 size = level > smallest ? largest_fitting_now(pace, level - 1, smallest) : level;
                // (The size frames are drawn at does not fit: that is what this is about.)
                const bool fits =
                    size != level && cost_now_at(size) * (1.0 + SizeMargin) <= slot;
                const bool other_pace = !fits && paced && pace < slowest;
                if (other_pace) {
                    s32 to = pace + 1;
                    while (to < slowest &&
                           cost_now_at(smallest_at(to)) * (1.0 + SizeMargin) > to * refresh) {
                        ++to;
                    }
                    slower(to);
                    size = largest_fitting_now(pace, level, smallest_at(pace));
                }
                if (size != level) {
                    Change(size, now, taken, load, other_pace);
                }
                went_up = {};
                up_allowed = now + patience;
                tight_windows = 0;
            } else if (paced && pace < slowest && tight_windows >= 3) {
                // The title's own work is what takes long, or the size is not this one's to
                // choose: the pace that what frames take now fits into.
                const s32 needed = static_cast<s32>(std::ceil(taken * (1.0 + Over) / refresh));
                slower(std::clamp(needed, pace + 1, slowest));
                went_up = {};
                tight_windows = 0;
            }
            return;
        }
        tight_windows = 0;

        // A faster pace, when all a frame takes would fit it with room to spare, and has for a
        // while: the GPU's work at the smallest size wanted for it, the GPU thread's processor
        // time, and the title's own work where a try at that pace found it too much.
        if (paced && pace > fastest && now > faster_allowed) {
            const s32 to = pace - 1;
            const double faster_slot = to * refresh;
            const s32 smallest = sized ? smallest_at(to) : level;
            if (cautious_cost_at(smallest) * (1.0 + PaceMargin) <= faster_slot &&
                thread * (1.0 + PaceMargin) <= faster_slot && OwnWorkFits(to, now, thread)) {
                if (++faster_windows >= FasterWindows && since_change > SettleUp) {
                    faster_windows = 0;
                    const s32 size = sized ? largest_fitting(to, level, smallest) : level;
                    SetPace(to, now, taken, load);
                    if (size != level) {
                        Change(size, now, taken, load, true);
                    }
                    went_up = {};
                    went_faster = now;
                    // The picture grows only once the faster pace has held.
                    up_allowed = std::max(up_allowed, now + PaceRegret);
                }
                return;
            }
        }
        faster_windows = 0;
        // A larger picture, when it would have fitted the pace with a fifth to spare for a few
        // seconds in a row (a view that is lighter for a moment is no reason), up to the
        // largest allowed.
        if (sized && level < largest && cost_at(level + 1) * (1.0 + SizeUpMargin) <= slot) {
            ++up_windows;
        } else {
            up_windows = 0;
        }
        if (up_windows >= UpWindows && since_change > UpSettle && now > up_allowed) {
            size_before_up = level;
            Change(level + 1, now, taken, load);
            went_up = now;
            up_windows = 0;
        }
    }

    /// Whether the title's own work may fit `to` refreshes: no try at that pace failed for it,
    /// or long ago, or the scene is clearly lighter for the GPU thread than it was then.
    bool OwnWorkFits(s32 to, Clock::time_point now, double thread) const {
        const OwnWork& seen = own_work[to];
        if (seen.taken == 0.0 || now - seen.at > Forget) {
            return true;
        }
        return seen.thread > 0.0 && thread > 0.0 && thread < seen.thread * Lighter;
    }

    /// What a step of size that is watched showed it to cost, against what it was counted as,
    /// for the log. (It does not change what sizes are counted as costing: while the player
    /// moves, a step shows as much of the view changing as of the size.)
    void Learn() {
        const Step step = *watched;
        watched.reset();
        const double after = step.windows != 0 ? step.after / step.windows : 0.0;
        // (The work is counted at the fastest clock seen: if that rose meanwhile, before and
        // after are counted at different ones.)
        if (step.before <= 0.0 || after <= 0.0 ||
            step.clock != Vulkan::FrameStats::FastestGpuClock()) {
            return;
        }
        const s32 upper = std::max(step.from, step.to);
        const double seen = upper == step.to ? after / step.before : step.before / after;
        const double counted = cost[upper] / cost[upper - 1];
        const double pixels = static_cast<double>(ConsoleSizes[upper][0]) * ConsoleSizes[upper][1] /
                              (static_cast<double>(ConsoleSizes[upper - 1][0]) *
                               ConsoleSizes[upper - 1][1]);
        // Never cheaper than the smaller size, never dearer than its pixels.
        (void)pixels;
        LOG_INFO(Core, "Going from {} to {} cost the GPU {:.2f} times the work of {} (counted as "
                       "{:.2f})",
                 SizeName(upper - 1), SizeName(upper), seen, SizeName(upper - 1), counted);
    }

    /// `paced`: the size goes with a pace that has just changed. `watch`: what the step costs
    /// is to be learned from it.
    void Change(s32 to, Clock::time_point now, double taken, float load, bool paced = false,
                bool watch = true) {
        if (watched) {
            // Taken back before it was seen for long: what was seen counts.
            Learn();
        }
        if (watch && !paced && std::abs(to - level) == 1 && load >= LearningLoad &&
            level_windows >= 3 && level_of_average == level && pace_of_average == pace) {
            watched = Step{.from = level,
                           .to = to,
                           .before = level_average,
                           .clock = Vulkan::FrameStats::FastestGpuClock()};
        }
        if (paced) {
            LOG_INFO(Core,
                     "The scene is drawn at {} an eye from now on instead of {}: the largest "
                     "that fits the {:.1f} ms frames are given now",
                     SizeName(to), SizeName(level), pace * refresh * 1e3);
        } else {
            LOG_INFO(Core,
                     "The scene is drawn at {} an eye from now on instead of {}, which is {} for "
                     "the {:.1f} ms frames are given: they took {:.1f} ms, with the GPU busy "
                     "{:.0f}% of the time{}",
                     SizeName(to), SizeName(level),
                     to < level ? "too much" : "less than there is room", pace * refresh * 1e3,
                     taken * 1e3, load * 100.0f, AtClock());
        }
        level = to;
        changed = now;
        up_windows = 0;
    }

    void SetPace(s32 to, Clock::time_point now, double taken, float load) {
        watched.reset();
        LOG_INFO(Core,
                 "Frames are given {} refreshes from now on ({:.0f} a second), {} was {}: frames "
                 "took {:.1f} ms at {}, the GPU was busy {:.0f}% of the time{}, the GPU thread "
                 "{:.1f} ms a frame",
                 to, 1.0 / (to * refresh), pace, to > pace ? "too few" : "more than needed",
                 taken * 1e3, SizeName(level), load * 100.0f, AtClock(), window_thread * 1e3);
        pace = to;
        changed = now;
        up_windows = 0;
    }

    /// " (N% at its fastest clock)" where the clock is known, for the log.
    std::string AtClock() const {
        return window_work >= 0.0f
                   ? fmt::format(" ({:.0f}% at its fastest clock)", window_work * 100.0f)
                   : std::string{};
    }

    /// The console's smallest size in most levels.
    static constexpr s32 UsualLevel = 4;
    /// Frames are never held to a pace slower than this (in seconds a frame): a title that
    /// cannot make it either is left to draw as fast as it can.
    static constexpr double SlowestWanted = 0.0345;
    /// Frames given at least this long (30 a second) may have the smallest size of all; frames
    /// given less have the console's smallest at least.
    static constexpr double SmallestSizeSlot = 0.030;
    /// A display whose refreshes are further apart than this refreshes no faster than the
    /// title draws.
    static constexpr double SlowDisplay = 1.0 / 65.0;
    static constexpr auto Window = std::chrono::milliseconds{500};
    static constexpr u32 FewestFrames = 5;
    static constexpr auto SettleDown = std::chrono::milliseconds{900};
    static constexpr auto SettleUp = std::chrono::seconds{6};
    /// No larger picture this soon after any change of size or pace.
    static constexpr auto UpSettle = std::chrono::seconds{10};
    /// A step down this soon after a step up takes the step up back: it was a mistake.
    static constexpr auto Regret = std::chrono::seconds{30};
    static constexpr auto PaceRegret = std::chrono::seconds{15};
    static constexpr auto Calm = std::chrono::seconds{90};
    static constexpr auto FirstPatience = std::chrono::seconds{15};
    /// What the first failed try at a faster pace makes the next one wait (each one after
    /// that doubles it).
    static constexpr auto FirstPacePatience = std::chrono::seconds{20};
    static constexpr auto LongestPatience = std::chrono::seconds{120};
    static constexpr auto Forget = std::chrono::minutes{10};
    /// Windows in a row in which a faster pace would fit before it is tried: five seconds.
    static constexpr u32 FasterWindows = 10;
    /// By how much of what they are given frames may take longer, on average, and still count
    /// as fitting: frames that fit take exactly what they are given.
    static constexpr double Over = 0.03;
    /// The share of the time the GPU must be busy to be what holds frames up.
    static constexpr float GpuShort = 0.85f;
    /// Room the size chosen for a pace must leave, a step up to a larger size, and a faster
    /// pace. (A step up that does not fit is taken back within seconds, a faster pace that does
    /// not costs more: see Regret and PaceRegret.)
    static constexpr double SizeMargin = 0.10;
    static constexpr double SizeUpMargin = 0.20;
    /// Windows in a row in which a larger picture would have fitted before it is taken: four
    /// seconds.
    static constexpr u32 UpWindows = 8;
    static constexpr double PaceMargin = 0.25;
    /// What a size that had to be taken back is counted as costing more, and the most that
    /// adds up to.
    static constexpr double Dearer = 1.06;
    static constexpr double DearestMarkup = 1.2;
    /// Windows in a row with nothing to go by after which the title is taken to be in another
    /// scene: two seconds of loading.
    static constexpr u32 NewScene = 4;
    /// A step of size is seen (for the log) in two windows after the first one after it
    /// (frames drawn at the old size are through the GPU by then), where the GPU is busy at
    /// least this much of the time (resting more, it shows nothing of what a size costs).
    static constexpr u32 StepWindows = 2;
    static constexpr float LearningLoad = 0.5f;
    /// A scene this much lighter for the GPU thread than one in which the title's own work did
    /// not fit a pace is worth another try at it.
    static constexpr double Lighter = 0.8;

    const s32 fixed_pace;
    const s32 fastest_pace;
    const double fps_cap;
    const double steady_fps;
    /// The largest size chosen here.
    const s32 largest;
    s32 fastest{2};
    bool started{};
    s32 level{UsualLevel};
    s32 pace;
    double refresh{1.0 / 120.0};
    // What each size has been found to cost more than counted (a size that had to be taken
    // back, and the ones larger than it).
    std::array<double, 7> markup{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    // What each size's GPU work is counted as, in terms of the smallest (SizeCost).
    std::array<double, 7> cost{SizeCost};
    // A step of size whose cost is being watched: the GPU's work a frame before it, and the
    // sum of it over the windows seen after it.
    struct Step {
        s32 from{};
        s32 to{};
        double before{};
        double after{};
        u32 windows{};
        bool settled{};
        u32 clock{};
    };
    std::optional<Step> watched;
    // The GPU's work a frame at the present size and pace, averaged over the windows since it
    // was taken (level_windows of them), for what a step from it teaches.
    double level_average{};
    s32 level_of_average{};
    s32 pace_of_average{};
    u32 level_windows{};
    // Frames that held everything up in the present window; whether the size is chosen here.
    u32 window_stalls{};
    bool sizing{};
    Clock::time_point window_start;
    double window_time{};
    u32 window_frames{};
    u32 tight_windows{};
    u32 faster_windows{};
    u32 up_windows{};
    u32 idle_windows{};
    Clock::time_point changed;
    Clock::time_point up_allowed;
    Clock::time_point faster_allowed;
    Clock::time_point last_regret;
    Clock::time_point last_pace_regret;
    std::chrono::seconds patience{FirstPatience};
    std::chrono::seconds pace_patience{FirstPacePatience};
    std::array<OwnWork, SlowestPace + 1> own_work{};
    // When the size last went up (and from what) and the pace last got faster, if that still
    // stands.
    Clock::time_point went_up;
    s32 size_before_up{UsualLevel};
    Clock::time_point went_faster;
    // The last window's GPU work at the fastest clock (below zero where not known) and GPU
    // thread's time a frame, for the log.
    float window_work{-1.0f};
    double window_thread{};
    double summary_load{};
    u32 summary_windows{};
};

// The size the title draws its scene at right now, as an index into ConsoleSizes; -1 when
/// it has not got that far. Holds it to `wanted` on the way unless that is 0.
s32 TendResolution(VAddr base, const Build& build, s32 wanted) {
    const u64 control = Read<u64>(base + build.resolution_pointer);
    if (control == 0) {
        return -1;
    }
    const s32 level = Read<s32>(control + ResolutionLevel);
    const s32 base_level = Read<s32>(control + ResolutionBase);
    if (level < 0 || level > LastHeadsetLevel || base_level < 0 || base_level > LastHeadsetLevel) {
        // Not what is known of it: looked at, never written to.
        return -1;
    }
    if (wanted != 0 && level >= FirstHeadsetLevel) {
        const s32 offset = wanted - base_level;
        Write<s32>(control + ResolutionOffset, offset);
        Write<s32>(control + ResolutionHighest, offset);
        Write<s32>(control + ResolutionLowest, offset);
    }
    return level;
}

/// Keeps the title's time step at what its frames take. Asked every frame with what the frame
/// took; answers with the step in effect from now on.
class TimeStep {
public:
    /// `shortest`: the least a step may be. The title's own (a sixtieth of a second) unless
    /// it is given a refresh for every frame on a display that is faster than that.
    double Next(double frame, double longest, double shortest) {
        if (frame > Stall) {
            return step;
        }
        stepped += step;
        average += (frame - average) * Follow;
        owed = std::clamp(owed + frame - step, -MostOwed, MostOwed);
        step = std::clamp(average + owed * Repay, std::min(shortest, longest), longest);
        // At the rate the title was made for, exactly what it would use itself.
        if (std::abs(average - Nominal) < 0.0004 && std::abs(owed) < 0.004) {
            step = Nominal;
            owed = 0.0;
        }
        return step;
    }

    /// Game time gone by since this was last asked.
    double TakeStepped() {
        return std::exchange(stepped, 0.0);
    }

private:
    // How much of the difference to the last frame goes into what frames are taken to take.
    // A step that follows every frame would be wrong twice over where long and short frames
    // alternate, which they do when the display's refreshes do not divide by the frame rate.
    static constexpr double Follow = 0.15;
    // Real time the game's clock has fallen behind (or run ahead) by, at most, and how much
    // of it a frame makes up for. This is what keeps what is seen in step with what is heard.
    static constexpr double MostOwed = 0.1;
    static constexpr double Repay = 0.1;

    double average{Nominal};
    double step{Nominal};
    double owed{};
    double stepped{};
};

/// Whether the bodies the title moves by hand arrive where it sends them, for the log
/// (SHADPS4_TITLE_PHYSICS_WATCH=1). A body that is sent somewhere is given the speed that
/// takes it there in one step, worked out with the time step the library has written down
/// (Builds::PhysicsStepChanges), and keeps that speed: how far a step moved it, against its
/// speed times the time step that was written down when it was sent, tells whether it got
/// there. Looked at when the title reads its controller, once a frame on the thread it runs
/// on and before anything of the frame has moved: the title sends its bodies and then takes
/// the step, both after that.
class PhysicsWatch {
public:
    void Look(VAddr base, const Build& build) {
        namespace Physics = Builds::Physics;
        std::scoped_lock lock{mutex};
        auto* const memory = Core::Memory::Instance();
        const auto there = [&](u64 address, u64 size) {
            return address != 0 && (address & 3) == 0 && memory->IsValidMapping(address, size);
        };
        const u64 game = Read<u64>(base + build.game_pointer);
        if (!there(game, Physics::GameWorld + 8)) {
            return;
        }
        const u64 world = Read<u64>(game + Physics::GameWorld);
        if (!there(world, Physics::WorldInner + 8)) {
            return;
        }
        const u64 inner = Read<u64>(world + Physics::WorldInner);
        if (!there(inner, Physics::InnerScale + 4)) {
            return;
        }
        const u64 library = Read<u64>(inner + Physics::InnerLibrary);
        if (!there(library, Physics::LibraryTimeStep + 4)) {
            return;
        }
        const u32 count = Read<u32>(library + Physics::LibraryBodyCount);
        const u64 bodies = Read<u64>(library + Physics::LibraryBodies);
        const double scale = Read<float>(inner + Physics::InnerScale);
        const double time_step = Read<float>(library + Physics::LibraryTimeStep);
        if (count == 0 || count > MostBodies || !there(bodies, count * Physics::BodySize) ||
            !(scale > 0.0) || !(time_step > 0.0)) {
            return;
        }
        if (bodies != seen_bodies) {
            // Another world, or another level's.
            seen_bodies = bodies;
            seen.clear();
        }
        seen.resize(count);
        bool stepped = false;
        for (u32 i = 0; i < count; ++i) {
            const VAddr body = bodies + u64{i} * Physics::BodySize;
            Seen& was = seen[i];
            const bool by_hand = Read<u8>(body + Physics::BodyMotion) == Physics::MotionKeyframe;
            const auto segment = Read<std::array<s32, 3>>(body + Physics::BodySegment);
            const auto position = Read<std::array<float, 3>>(body + Physics::BodyPosition);
            if (was.by_hand && by_hand && segment == was.segment && position != was.position) {
                // It moved: a step was taken since it was last looked at, with the speed it
                // still has.
                const auto velocity = Read<std::array<float, 3>>(body + Physics::BodyVelocity);
                double way = 0.0;
                double off = 0.0;
                for (u32 axis = 0; axis < 3; ++axis) {
                    const double moved = position[axis] - was.position[axis];
                    const double sent = velocity[axis] * written_down;
                    way += sent * sent;
                    off += (moved - sent) * (moved - sent);
                }
                way = std::sqrt(way) / scale;
                off = std::sqrt(off) / scale;
                // (A body further from where it was sent than it had to go twice over is
                // another body in the place of the one that was looked at.)
                if (way > 0.0 && off <= 2.0 * way + 0.01) {
                    stepped = true;
                    ++moves;
                    missed += off > Off ? 1 : 0;
                    farthest = std::max(farthest, way);
                    if (off > worst_off) {
                        worst_off = off;
                        worst_way = way;
                    }
                }
            }
            was.by_hand = by_hand;
            was.segment = segment;
            was.position = position;
        }
        if (stepped && written_down > 0.0) {
            // How unlike two time steps in a row are, which is what the bodies are off by.
            most_unlike = std::max(most_unlike, std::abs(time_step / written_down - 1.0));
        }
        written_down = time_step;
    }

    /// What was seen since this was last asked, "" for nothing.
    std::string TakeSummary() {
        std::scoped_lock lock{mutex};
        if (moves == 0) {
            return {};
        }
        std::string summary = fmt::format(
            "The title's bodies: {} times one was sent somewhere and moved, {:.2f} at the "
            "farthest; where they ended was {:.4f} from where they were sent at worst (by one "
            "sent {:.2f}), {} times more than {} away; two time steps in a row were {:.1f}% "
            "apart at most",
            moves, farthest, worst_off, worst_way, missed, Off, most_unlike * 100.0);
        moves = 0;
        missed = 0;
        farthest = 0.0;
        worst_off = 0.0;
        worst_way = 0.0;
        most_unlike = 0.0;
        return summary;
    }

private:
    struct Seen {
        bool by_hand{};
        std::array<s32, 3> segment{};
        std::array<float, 3> position{};
    };

    // More than any level has: a count that is not one.
    static constexpr u32 MostBodies = 1u << 17;
    // What counts as not having arrived, in the game's units (a block of a level is about
    // half of one).
    static constexpr double Off = 0.01;

    std::mutex mutex;
    u64 seen_bodies{};
    std::vector<Seen> seen;
    /// The library's time step when the bodies were last looked at: what they are sent with
    /// until the step that follows.
    double written_down{};
    u64 moves{};
    u64 missed{};
    double farthest{};
    double worst_off{};
    double worst_way{};
    double most_unlike{};
};

PhysicsWatch physics_watch;

} // namespace

void OnControllerRead() {
    const Build* const build = known_build.load(std::memory_order_acquire);
    if (build == nullptr || !GetSettings().physics_watch) {
        return;
    }
    physics_watch.Look(known_base, *build);
}

void OnFrameSubmitted() {
    const Build* const build = known_build.load(std::memory_order_acquire);
    if (build == nullptr) {
        return;
    }
    const VAddr base = known_base;
    const Settings& settings = GetSettings();

    static bool running = false;
    static Clock::time_point last;
    static TimeStep time_step;
    static Governor governor{settings.pace, settings.fastest_pace, settings.fps_cap,
                             settings.steady_fps, settings.largest_level};
    // For the log.
    static Clock::time_point report_time;
    static double report_real = 0.0;
    static u32 report_frames = 0;
    static double step = Nominal;

    const auto now = Clock::now();
    if (!running) {
        running = true;
        last = now;
        report_time = now;
        LOG_INFO(Core,
                 "The title's time step {}; the size of its scene is {}; the refreshes a frame "
                 "is given are {}",
                 settings.time_step ? "follows what its frames take" : "is left alone",
                 settings.resolution == Settings::Resolution::Title
                     ? std::string{"left to the title"}
                 : settings.resolution == Settings::Resolution::Pinned ? std::string{"held to one"}
                 : settings.largest_level < LastHeadsetLevel
                     ? fmt::format("chosen by how busy the GPU is, {} at most",
                                   SizeName(settings.largest_level))
                     : std::string{"chosen by how busy the GPU is"},
                 settings.pace != 0 ? std::string{"one number"}
                 : settings.steady_fps > 0.0
                     ? fmt::format("as many as make {:.0f} frames a second", settings.steady_fps)
                     : std::string{"chosen by what the title manages"});
        return;
    }
    const double frame = std::chrono::duration<double>(now - last).count();
    last = now;

    s32 wanted = 0;
    switch (settings.resolution) {
    case Settings::Resolution::Title:
        governor.Level(frame, now, Governor::NotMine);
        break;
    case Settings::Resolution::Pinned: {
        // A measuring aid: SHADPS4_DBG_RESOLUTION_CYCLE=<seconds> has the pinned size and the
        // next larger take turns, so that what a change of size does to a frame can be seen.
        static const double cycle = [] {
            const char* value = std::getenv("SHADPS4_DBG_RESOLUTION_CYCLE");
            return value != nullptr ? std::atof(value) : 0.0;
        }();
        static const auto cycle_start = now;
        s32 pinned = settings.pinned_level;
        if (cycle > 0.0 &&
            static_cast<s64>(std::chrono::duration<double>(now - cycle_start).count() / cycle) %
                    2 ==
                1) {
            pinned = std::min(pinned + 1, LastHeadsetLevel);
        }
        // The same by the A/B measuring aid, as "size": the "B" frames are drawn a size larger.
        if (Vulkan::FrameStats::AbOff("size")) {
            pinned = std::min(settings.pinned_level + 1, LastHeadsetLevel);
        }
        // SHADPS4_DBG_TRACE_SIZE_CHANGE=<s>[,<frames>]: the draws from so many frames after the
        // first change of size that many seconds in are traced (issue #19).
        {
            static s32 trace_delay = 0;
            static const double trace_after = [] {
                const char* value = std::getenv("SHADPS4_DBG_TRACE_SIZE_CHANGE");
                if (value == nullptr) {
                    return -1.0;
                }
                if (const char* comma = std::strchr(value, ','); comma != nullptr) {
                    trace_delay = std::atoi(comma + 1);
                }
                return std::atof(value);
            }();
            static s32 last_pinned = pinned;
            static bool traced = false;
            static s32 countdown = -1;
            if (pinned != last_pinned && !traced && trace_after >= 0.0 &&
                std::chrono::duration<double>(now - cycle_start).count() >= trace_after) {
                traced = true;
                countdown = trace_delay;
                LOG_INFO(Core, "TRACE size change to level {}", pinned);
            }
            if (countdown >= 0 && countdown-- == 0) {
                LOG_INFO(Core, "TRACE from {} frames after the size change", trace_delay);
                Vulkan::Rasterizer::StartDrawTrace(7000);
            }
            last_pinned = pinned;
        }
        wanted = governor.Level(frame, now, pinned);
        break;
    }
    case Settings::Resolution::Governed:
        wanted = governor.Level(frame, now, 0);
        break;
    }
    const s32 resolution = TendResolution(base, *build, wanted);
    frame_pace.store(governor.Pace(), std::memory_order_relaxed);
    if (resolution >= FirstHeadsetLevel) {
        const auto& size = GetLarger().sizes[resolution];
        scene_size.store(size[0] << 16 | size[1], std::memory_order_relaxed);
    }

    if (settings.time_step) {
        // Shorter steps than the title's own only where it is made to draw faster than it
        // was made for: a frame for every refresh of a display faster than 60 Hz.
        const double shortest = governor.Pace() == 1 ? 1.0 / 250.0 : Nominal;
        step = time_step.Next(frame, settings.longest_step, shortest);
        Write<double>(base + build->frame_rate, 1.0 / step);
        Write<float>(base + build->frame_seconds, static_cast<float>(step));
        Write<u64>(base + build->frame_microseconds, static_cast<u64>(step * 1e6));
    }

    if (frame <= Stall) {
        report_real += frame;
        ++report_frames;
    }
    if (now - report_time >= std::chrono::seconds{10} && report_frames != 0) {
        const double stepped =
            settings.time_step ? time_step.TakeStepped() : Nominal * report_frames;
        const Governor::Summary summary = governor.TakeSummary();
        LOG_INFO(Core,
                 "The title's clock: frames take {:.1f} ms, its time step is {:.1f} ms, the game "
                 "ran at {:.0f}% of its speed over the last {:.0f} s (it would have at {:.0f}% "
                 "left to itself); it draws the scene at {} an eye and is given {} refreshes a "
                 "frame ({:.1f} ms), the GPU busy {:.0f}% of the time{}",
                 report_real / report_frames * 1e3, step * 1e3, 100.0 * stepped / report_real,
                 report_real, 100.0 * Nominal * report_frames / report_real,
                 SizeName(resolution),
                 summary.pace, summary.slot * 1e3, summary.load * 100.0,
                 summary.next_cost > 0.0
                     ? fmt::format(" (the next size up counted as {:.2f} times the work)",
                                   summary.next_cost)
                     : std::string{});
        if (const std::string seen = physics_watch.TakeSummary(); !seen.empty()) {
            LOG_INFO(Core, "{}", seen);
        }
        report_time = now;
        report_real = 0.0;
        report_frames = 0;
    }
}

bool IsEyeSize(u32 width, u32 height) {
    if (known_build.load(std::memory_order_acquire) == nullptr) {
        return false;
    }
    const auto& size = GetLarger().sizes[LastHeadsetLevel];
    return width == size[0] && height == size[1];
}

u32 FramePace() {
    return frame_pace.load(std::memory_order_relaxed);
}

void Prepare() {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA12392") {
        return;
    }
    const Larger& larger = GetLarger();
    if (larger.extra_memory_mb == 0) {
        return;
    }
    if (EmulatorSettings.GetExtraDmemInMBytes() < larger.extra_memory_mb) {
        EmulatorSettings.SetExtraDmemInMBytes(larger.extra_memory_mb);
    }
    LOG_INFO(Core, "The title is to draw at up to {} an eye: its memory grows by {} MB",
             SizeName(LastHeadsetLevel), EmulatorSettings.GetExtraDmemInMBytes());
}

void OnGameLoaded(VAddr base, u64 size) {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA12392") {
        return;
    }
    // Asked here and nowhere else: this is the one moment at which the image is as its build
    // has it. Its sizes may be other ones from here on, and its clock is once it runs.
    const std::span<u8> image{reinterpret_cast<u8*>(base), static_cast<size_t>(size)};
    const Build* const build = Builds::Recognise(image);
    if (build == nullptr) {
        LOG_WARNING(Core,
                    "This build of CUSA12392 is none of those known from inside (its versions "
                    "1.00 and 1.04 are): it is left to itself. It then counts time in frames, "
                    "which is slow motion wherever a frame takes longer than a sixtieth of a "
                    "second, and draws at the console's sizes");
        return;
    }
    LOG_INFO(Core, "CUSA12392 in a build known from inside: {}", build->name);

    const Larger& larger = GetLarger();
    if (larger.factor != 1.0) {
        // Every place is checked for what the console's build has there before anything is
        // written: a title that turns out to be other than thought is left as it is.
        const auto changes = Builds::SizeChanges(*build, larger);
        if (const Builds::Change* unexpected = Builds::Apply(image, changes);
            unexpected != nullptr) {
            LOG_WARNING(Core,
                        "The title does not have {:#x} at {:#x} as expected: it draws at the "
                        "console's sizes",
                        unexpected->was, unexpected->at);
        } else {
            LOG_INFO(Core,
                     "The title draws at up to {} an eye instead of 1440x1536 ({:.2f} times as "
                     "wide), the smallest {}; its render targets have {} MB, its graphics memory "
                     "{} MB",
                     SizeName(LastHeadsetLevel), larger.factor, SizeName(FirstHeadsetLevel),
                     larger.target_pool >> 20, larger.graphics_heap >> 20);
        }
    }
    if (const Settings& settings = GetSettings(); settings.time_step && settings.physics_step) {
        const auto changes = Builds::PhysicsStepChanges(*build);
        if (const Builds::Change* unexpected = Builds::Apply(image, changes);
            unexpected != nullptr) {
            LOG_WARNING(Core,
                        "The title does not have {:#x} at {:#x} as expected: its physics are "
                        "left as they are, and collisions that it moves may end up beside what "
                        "is drawn",
                        unexpected->was, unexpected->at);
        } else {
            LOG_INFO(Core, "The title's physics take every step with the time step its bodies "
                           "were sent with");
        }
    }
    known_base = base;
    known_build.store(build, std::memory_order_release);
}

bool IsSceneSize(u32 width, u32 height) {
    const u32 size = scene_size.load(std::memory_order_relaxed);
    return size != 0 && width == size >> 16 && height == (size & 0xffff);
}

bool IsHeadsetSceneSize(u32 width, u32 height) {
    if (known_build.load(std::memory_order_acquire) == nullptr) {
        return false;
    }
    const auto& sizes = GetLarger().sizes;
    for (s32 level = FirstHeadsetLevel; level <= LastHeadsetLevel; ++level) {
        if (width == sizes[level][0] && height == sizes[level][1]) {
            return true;
        }
    }
    return false;
}

bool IsSceneEnlargement(u64 pgm_hash) {
    // The same program in versions 1.00 and 1.04 (traced on the headset and on the PC).
    static constexpr u64 FinalPass = 0x2ed5e2b0;
    return pgm_hash == FinalPass && known_build.load(std::memory_order_acquire) != nullptr;
}

std::filesystem::path SceneFile(std::string_view guest_path, std::filesystem::path host_path) {
    if (!guest_path.ends_with(".spx") || !guest_path.starts_with("/app0/")) {
        return host_path;
    }
    const Scenes::Detail& detail = GetSettings().scene_detail;
    if (detail.IsTitles() || known_build.load(std::memory_order_acquire) == nullptr) {
        return host_path;
    }
    static std::mutex mutex;
    static std::unordered_map<std::string, std::filesystem::path> made;
    std::scoped_lock lock{mutex};
    const std::string key = host_path.string();
    if (const auto found = made.find(key); found != made.end()) {
        return found->second;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(host_path, error)) {
        return host_path;
    }
    std::ifstream in{host_path, std::ios::binary};
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (!in.good() && !in.eof()) {
        return host_path;
    }
    // A copy for every file the title has, by where it was found: an update's file of the
    // same name is another one.
    const std::filesystem::path folder =
        Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "CUSA12392-scenes";
    std::filesystem::create_directories(folder, error);
    const std::filesystem::path copy =
        folder / fmt::format("{:016x}-{}", std::hash<std::string>{}(key),
                             host_path.filename().string());
    const std::string name = host_path.filename().string();
    const bool base = name == "default_hmd.spx";
    const std::string adjusted = Scenes::Adjust(text, detail, base, name);
    if (adjusted == text) {
        // Nothing to change: the title's own file.
        made.emplace(key, host_path);
        return host_path;
    }
    {
        // Written beside it first and put in its place once all of it is out: a copy that could
        // not be written whole (a full disk) is never what the title reads.
        std::filesystem::path partial = copy;
        partial += ".part";
        std::ofstream out{partial, std::ios::binary | std::ios::trunc};
        out.write(adjusted.data(), static_cast<std::streamsize>(adjusted.size()));
        out.close();
        if (out.fail()) {
            std::filesystem::remove(partial, error);
        } else {
            std::filesystem::rename(partial, copy, error);
        }
        if (out.fail() || error) {
            LOG_WARNING(Core, "The scene parameters {} could not be written changed to {}: the "
                              "title's are read",
                        host_path.filename().string(), copy.string());
            made.emplace(key, host_path);
            return host_path;
        }
    }
    // What was asked of this file (the reach of the depth pre-pass only goes into the base).
    std::string changes;
    const auto add = [&](std::string what) {
        changes += (changes.empty() ? "" : ", ") + std::move(what);
    };
    if (detail.lod_scale != 1.0) {
        add(fmt::format("models changing to coarser forms at {:.0f}% of the title's distances",
                        detail.lod_scale * 100.0));
    }
    if (detail.shadow_map_size != 0) {
        add(fmt::format("shadow maps of {} at most", detail.shadow_map_size));
    }
    if (!detail.depth_prepass) {
        add("no depth pre-pass");
    } else if (base && detail.prepass_distance >= 0.0) {
        add(fmt::format("the depth pre-pass to {} m", detail.prepass_distance));
    }
    if (detail.depth_prepass && base && detail.prepass_radius >= 0.0) {
        add(fmt::format("the depth pre-pass for models from {} m across", detail.prepass_radius));
    }
    if (Scenes::LeavesSunShadowsOut(detail, name)) {
        add(std::ranges::find(Scenes::MixedSunFiles, name) != std::end(Scenes::MixedSunFiles)
                ? "no sun shadow maps (a measuring aid: part of the level has a sun)"
                : "no sun shadow maps (the level's sun is black)");
    }
    LOG_INFO(Core, "The scene parameters {} are read with {}", name, changes);
    made.emplace(key, copy);
    return copy;
}

void NoteView(const Vr::Vec3& tracker_head) {
    static constexpr auto Interval = std::chrono::seconds{10};

    const Build* const build = known_build.load(std::memory_order_acquire);
    if (build == nullptr) {
        return;
    }
    const VAddr base = known_base;
    static std::mutex mutex;
    static Vr::Vec3 last_origin;
    static Clock::time_point last_report;
    static bool reported = false;

    const std::unique_lock lock{mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        return;
    }
    const u64 manager = Read<u64>(base + build->manager_pointer);
    if (manager == 0) {
        return;
    }
    const auto origin = Read<Vr::Vec3>(manager + ManagerOrigin);
    const u64 state = Read<u64>(manager + ManagerHeadState);
    if (state == 0 || Read<u8>(state + StatePositionValid) == 0) {
        return;
    }
    const auto head = Read<Vr::Vec3>(state + StatePosition);

    const bool moved = !reported || std::abs(origin.x - last_origin.x) > 0.01f ||
                       std::abs(origin.y - last_origin.y) > 0.01f ||
                       std::abs(origin.z - last_origin.z) > 0.01f;
    const auto now = Clock::now();
    if (!moved && now - last_report < Interval) {
        return;
    }
    // Towards the camera of a PlayStation VR is where the player faces: ahead.
    LOG_INFO(Core_Vr,
             "{} {:.2f} {:.2f} {:.2f} of the tracker's space; the head, at {:.2f} {:.2f} {:.2f}, "
             "is {:.2f} m to the right of that, {:.2f} above and {:.2f} ahead",
             moved ? "The title now takes the player to sit at"
                   : "The title has the player's seat at",
             origin.x, origin.y, origin.z, tracker_head.x, tracker_head.y, tracker_head.z, head.x,
             head.y, -head.z);
    last_origin = origin;
    last_report = now;
    reported = true;
}

} // namespace Core::KnownTitle
