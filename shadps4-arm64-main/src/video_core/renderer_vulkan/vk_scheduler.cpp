// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_gpu_profile.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

constexpr u64 MAX_IN_FLIGHT_SUBMISSIONS = 8;

namespace {

/// A count kept by the thread that draws and read by the one that logs. Adding is a plain add:
/// an atomic one, a dozen times per draw, cost as much as the rest of a draw's bookkeeping.
/// The reader takes what was added since it last looked.
struct Tally {
    std::atomic<u64> value{};
    u64 taken{};

    void Add(u64 amount = 1) {
        value.store(value.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
    }

    u64 Take() {
        const u64 now = value.load(std::memory_order_relaxed);
        return now - std::exchange(taken, now);
    }
};

struct FrameCounters {
    Tally passes;
    Tally loading_passes;
    Tally pass_pixels;
    Tally draws;
    Tally computes;
    Tally submits;
    std::atomic<u64> frames{};
    std::chrono::steady_clock::time_point since{std::chrono::steady_clock::now()};
};

FrameCounters g_frame_counters;

struct StageTimes {
    std::array<std::atomic<u64>, static_cast<size_t>(FrameStats::Stage::Count)> total{};
    std::array<std::atomic<u64>, static_cast<size_t>(FrameStats::Stage::Count)> count{};
    std::atomic<s64> guest_submit{};
};

StageTimes g_stage_times;

std::array<Tally, static_cast<size_t>(FrameStats::Counter::Count)> g_counters{};

/// Draws after which what has been recorded goes to the GPU at the next end of a render pass;
/// 0 leaves everything to the end of the frame. SHADPS4_FLUSH_DRAWS sets it.
u32 FlushDraws() {
    static const u32 draws = [] {
        const char* value = std::getenv("SHADPS4_FLUSH_DRAWS");
#ifdef ENABLE_BACHATA_RUNTIME
        constexpr u32 Default = 64;
#else
        constexpr u32 Default = 0;
#endif
        return value != nullptr ? static_cast<u32>(std::max(std::atoi(value), 0)) : Default;
    }();
    return draws;
}

/// One render pass of the frame that is being listed.
struct PassRecord {
    u32 width{};
    u32 height{};
    u32 colors{};
    u32 loaded_colors{};
    bool depth{};
    bool depth_loaded{};
    u32 color_view{};
    u32 depth_view{};
    vk::ImageLayout depth_layout{};
    u32 draws{};
    u64 vertices{};
    const char* ended_in{};
    u32 ended_at{};

    bool SameAs(const PassRecord& other) const {
        return width == other.width && height == other.height && colors == other.colors &&
               loaded_colors == other.loaded_colors && depth == other.depth &&
               depth_loaded == other.depth_loaded && color_view == other.color_view &&
               depth_view == other.depth_view && depth_layout == other.depth_layout &&
               draws == other.draws && vertices == other.vertices &&
               ended_in == other.ended_in && ended_at == other.ended_at;
    }
};

enum class Listing { Idle, Armed, Recording };

struct FrameListing {
    std::mutex mutex;
    Listing state{Listing::Idle};
    std::vector<PassRecord> passes;
};

FrameListing g_frame_listing;

int FrameStatsLevel() {
    static const int level = [] {
        const char* value = std::getenv("SHADPS4_FRAME_STATS");
        return value != nullptr ? std::atoi(value) : 0;
    }();
    return level;
}

const char* FileName(const char* path) {
    const char* name = path;
    for (const char* at = path; *at != '\0'; ++at) {
        if (*at == '/' || *at == '\\') {
            name = at + 1;
        }
    }
    return name;
}

} // namespace

bool FrameStats::Enabled() {
    return FrameStatsLevel() > 0;
}

void FrameStats::RenderPass(const RenderState& state) {
    if (!Enabled()) {
        return;
    }
    PassRecord record;
    record.width = state.width;
    record.height = state.height;
    record.colors = state.num_color_attachments;
    for (u32 i = 0; i < state.num_color_attachments; ++i) {
        const auto& attachment = state.color_attachments[i];
        if (!attachment.image_view) {
            continue;
        }
        if (record.color_view == 0) {
            record.color_view = static_cast<u32>(
                reinterpret_cast<uintptr_t>(static_cast<VkImageView>(attachment.image_view)));
        }
        if (!attachment.is_clear) {
            record.loaded_colors |= 1u << i;
        }
    }
    const auto& db = state.depth_stencil_attachment;
    record.depth = db.has_depth || db.has_stencil;
    record.depth_loaded =
        (db.has_depth && !db.depth_clear) || (db.has_stencil && !db.stencil_clear);
    if (record.depth) {
        record.depth_view = static_cast<u32>(
            reinterpret_cast<uintptr_t>(static_cast<VkImageView>(db.image_view)));
        record.depth_layout = db.image_layout;
    }

    const bool has_targets = record.color_view != 0 || record.depth;
    g_frame_counters.passes.Add();
    g_frame_counters.loading_passes.Add(record.loaded_colors != 0 || record.depth_loaded ? 1 : 0);
    // A pass without targets is as large as a framebuffer may be, and costs nothing.
    g_frame_counters.pass_pixels.Add(has_targets ? u64{state.width} * state.height : 0);

    if (FrameStatsLevel() > 1) {
        std::scoped_lock lock{g_frame_listing.mutex};
        if (g_frame_listing.state == Listing::Recording) {
            g_frame_listing.passes.push_back(record);
        }
    }
}

void FrameStats::PassEnded(const std::source_location& where) {
    if (FrameStatsLevel() < 2) {
        return;
    }
    std::scoped_lock lock{g_frame_listing.mutex};
    if (g_frame_listing.state == Listing::Recording && !g_frame_listing.passes.empty() &&
        g_frame_listing.passes.back().ended_in == nullptr) {
        g_frame_listing.passes.back().ended_in = FileName(where.file_name());
        g_frame_listing.passes.back().ended_at = where.line();
    }
}

void FrameStats::Draw() {
    if (!Enabled()) {
        return;
    }
    g_frame_counters.draws.Add();
    if (FrameStatsLevel() > 1) {
        std::scoped_lock lock{g_frame_listing.mutex};
        if (g_frame_listing.state == Listing::Recording && !g_frame_listing.passes.empty()) {
            ++g_frame_listing.passes.back().draws;
        }
    }
}

void FrameStats::Compute() {
    if (Enabled()) {
        g_frame_counters.computes.Add();
    }
}

void FrameStats::Submit() {
    if (Enabled()) {
        g_frame_counters.submits.Add();
    }
}

void FrameStats::Time(Stage stage, std::chrono::nanoseconds time) {
    if (Enabled() && time.count() >= 0) {
        g_stage_times.total[static_cast<size_t>(stage)] += static_cast<u64>(time.count());
        ++g_stage_times.count[static_cast<size_t>(stage)];
    }
}

void FrameStats::Add(Counter counter, u64 amount) {
    if (!Enabled()) {
        return;
    }
    g_counters[static_cast<size_t>(counter)].Add(amount);
    if (counter == Counter::Vertices && FrameStatsLevel() > 1) {
        std::scoped_lock lock{g_frame_listing.mutex};
        if (g_frame_listing.state == Listing::Recording && !g_frame_listing.passes.empty()) {
            g_frame_listing.passes.back().vertices += amount;
        }
    }
}

void FrameStats::GuestSubmit() {
    if (!Enabled()) {
        return;
    }
    s64 expected = 0;
    g_stage_times.guest_submit.compare_exchange_strong(
        expected, std::chrono::steady_clock::now().time_since_epoch().count());
}

std::chrono::steady_clock::time_point FrameStats::TakeGuestSubmit() {
    using Clock = std::chrono::steady_clock;
    return Clock::time_point{Clock::duration{g_stage_times.guest_submit.exchange(0)}};
}

namespace {
std::atomic<u32> g_gpu_looks{};
std::atomic<u32> g_gpu_busy_looks{};
/// Looks at a known clock, the clocks (MHz) of those that found the GPU busy added up, and the
/// fastest clock seen.
std::atomic<u32> g_gpu_clocked_looks{};
std::atomic<u64> g_gpu_busy_clocks{};
std::atomic<u32> g_gpu_fastest_clock{};
std::atomic<u64> g_gpu_thread_ns{};
std::atomic<u32> g_gpu_thread_frames{};
/// The same looks for the frame statistics' lines: all of them, the busy ones, and the clocks
/// of each (MHz, where known) added up.
std::atomic<u64> g_log_looks{};
std::atomic<u64> g_log_busy_looks{};
std::atomic<u64> g_log_clocked_looks{};
std::atomic<u64> g_log_clocks{};

/// The GPU's clock now, in MHz, where the system tells it (a Qualcomm GPU on Linux); 0 if not.
u32 GpuClockMhz() {
#ifdef __linux__
    static const int file = open("/sys/class/kgsl/kgsl-3d0/gpuclk", O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return 0;
    }
    char text[24];
    const ssize_t length = pread(file, text, sizeof(text) - 1, 0);
    if (length <= 0) {
        return 0;
    }
    text[length] = '\0';
    return static_cast<u32>(std::strtoull(text, nullptr, 10) / 1000000);
#else
    return 0;
#endif
}
} // namespace

void FrameStats::GpuLook(bool busy) {
    g_gpu_looks.fetch_add(1, std::memory_order_relaxed);
    if (busy) {
        g_gpu_busy_looks.fetch_add(1, std::memory_order_relaxed);
    }
    // The clock is read every eighth look: it does not change faster than that. (One thread
    // looks.)
    static u32 looks = 0;
    static u32 clock = 0;
    if (looks++ % 8 == 0) {
        clock = GpuClockMhz();
        if (clock > g_gpu_fastest_clock.load(std::memory_order_relaxed)) {
            g_gpu_fastest_clock.store(clock, std::memory_order_relaxed);
        }
    }
    if (clock != 0) {
        g_gpu_clocked_looks.fetch_add(1, std::memory_order_relaxed);
        if (busy) {
            g_gpu_busy_clocks.fetch_add(clock, std::memory_order_relaxed);
        }
        g_log_clocked_looks.fetch_add(1, std::memory_order_relaxed);
        g_log_clocks.fetch_add(clock, std::memory_order_relaxed);
    }
    g_log_looks.fetch_add(1, std::memory_order_relaxed);
    if (busy) {
        g_log_busy_looks.fetch_add(1, std::memory_order_relaxed);
    }
}

namespace {
std::array<std::atomic<u64>, static_cast<size_t>(FrameStats::Call::Count)> g_call_ns{};
std::array<std::atomic<u64>, static_cast<size_t>(FrameStats::Call::Count)> g_call_count{};
constexpr const char* CallNames[] = {
    "draw", "push constants", "push descriptors", "vertex input", "vertex buffers",
    "index buffer", "dynamic state", "pipeline", "pass begin", "pass end", "barrier", "submit",
};
} // namespace

bool FrameStats::CallTimesEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DBG_CALL_TIMES");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

FrameStats::CallTimer::~CallTimer() {
    if (!timing) {
        return;
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    g_call_ns[static_cast<size_t>(call)].fetch_add(static_cast<u64>(ns), std::memory_order_relaxed);
    g_call_count[static_cast<size_t>(call)].fetch_add(1, std::memory_order_relaxed);
}

bool FrameStats::AbConcerns(const char* name) {
    static const std::string only = [] {
        const char* value = std::getenv("SHADPS4_DBG_AB_ONLY");
        return std::string{value != nullptr ? value : ""};
    }();
    if (only.empty()) {
        return true;
    }
    // (Several names may be given, with commas between them.)
    const std::string_view wanted{name};
    std::string_view rest{only};
    while (true) {
        const size_t comma = rest.find(',');
        if (rest.substr(0, comma) == wanted) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        rest.remove_prefix(comma + 1);
    }
}

u32 FrameStats::FastestGpuClock() {
    return g_gpu_fastest_clock.load(std::memory_order_relaxed);
}

float FrameStats::TakeGpuWork() {
    const u32 looks = g_gpu_clocked_looks.exchange(0, std::memory_order_relaxed);
    const u64 clocks = g_gpu_busy_clocks.exchange(0, std::memory_order_relaxed);
    const u32 fastest = g_gpu_fastest_clock.load(std::memory_order_relaxed);
    if (looks == 0 || fastest == 0) {
        return -1.0f;
    }
    return static_cast<float>(static_cast<double>(clocks) /
                              (static_cast<double>(looks) * static_cast<double>(fastest)));
}

void FrameStats::NoteGpuThreadFrame(std::chrono::nanoseconds time) {
    if (time.count() > 0) {
        g_gpu_thread_ns.fetch_add(static_cast<u64>(time.count()), std::memory_order_relaxed);
        g_gpu_thread_frames.fetch_add(1, std::memory_order_relaxed);
    }
}

double FrameStats::TakeGpuThreadTime() {
    const u64 ns = g_gpu_thread_ns.exchange(0, std::memory_order_relaxed);
    const u32 frames = g_gpu_thread_frames.exchange(0, std::memory_order_relaxed);
    return frames != 0 ? static_cast<double>(ns) / frames / 1e9 : 0.0;
}

float FrameStats::TakeGpuLoad() {
    const u32 looks = g_gpu_looks.exchange(0, std::memory_order_relaxed);
    const u32 busy = g_gpu_busy_looks.exchange(0, std::memory_order_relaxed);
    return looks != 0 ? static_cast<float>(busy) / static_cast<float>(looks) : -1.0f;
}

std::chrono::nanoseconds FrameStats::ThreadTime() {
#ifdef _WIN32
    // Counted by the system at its clock's ticks: right on average over a few seconds.
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        return std::chrono::nanoseconds{0};
    }
    const auto ticks = [](const FILETIME& time) {
        return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    };
    return std::chrono::nanoseconds{(ticks(kernel) + ticks(user)) * 100};
#else
    timespec time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return std::chrono::seconds{time.tv_sec} + std::chrono::nanoseconds{time.tv_nsec};
#endif
}

void FrameStats::EndFrame() {
    // The A/B measuring aid (AbOff): which way the frames from now on are drawn.
    static const double ab_period = [] {
        const char* value = std::getenv("SHADPS4_DBG_AB");
        return value != nullptr ? std::max(std::atof(value), 0.0) : 0.0;
    }();
    static u32 ab_frames[2]{};
    if (ab_period > 0.0) {
        static const auto ab_start = std::chrono::steady_clock::now();
        const double since =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - ab_start).count();
        const bool off = static_cast<s64>(since / ab_period) % 2 == 1;
        ++ab_frames[ab_off.load(std::memory_order_relaxed) ? 1 : 0];
        ab_off.store(off, std::memory_order_relaxed);
        // SHADPS4_DBG_AB_TU_DEBUG=<options>: as "tudebug", those of the Turnip driver's options
        // that it takes while it runs are on in the frames left out: they go into the file named
        // by TU_DEBUG_FILE, which the driver watches (TU_DEBUG may not name them as well).
        // (nolrz switched this way stopped the Quest's GPU for good within three minutes.)
        static const std::string tu_options = [] {
            const char* value = std::getenv("SHADPS4_DBG_AB_TU_DEBUG");
            return std::string{value != nullptr ? value : ""};
        }();
        static const char* const tu_file = std::getenv("TU_DEBUG_FILE");
        static int tu_written = -1;
        if (!tu_options.empty() && tu_file != nullptr && AbConcerns("tudebug") &&
            tu_written != (off ? 1 : 0)) {
            if (std::FILE* file = std::fopen(tu_file, "w"); file != nullptr) {
                std::fputs(off ? tu_options.c_str() : "", file);
                std::fclose(file);
            }
            tu_written = off ? 1 : 0;
        }
    }
    if (!Enabled()) {
        return;
    }
    auto& c = g_frame_counters;
    const u64 frames = ++c.frames;

    if (FrameStatsLevel() > 1) {
        std::vector<PassRecord> passes;
        {
            std::scoped_lock lock{g_frame_listing.mutex};
            if (g_frame_listing.state == Listing::Recording) {
                passes.swap(g_frame_listing.passes);
                g_frame_listing.state = Listing::Idle;
            } else if (g_frame_listing.state == Listing::Armed) {
                g_frame_listing.state = Listing::Recording;
            }
        }
        for (size_t i = 0; i < passes.size();) {
            const auto& pass = passes[i];
            size_t run = 1;
            while (i + run < passes.size() && passes[i + run].SameAs(pass)) {
                ++run;
            }
            LOG_INFO(Render_Vulkan,
                     "  pass {:3}{}: {}x{} colour {:04x} x{} (loaded {:#x}) depth {:04x}{}{} "
                     "{} draws, {} thousand vertices, ended by {}:{}",
                     i, run > 1 ? fmt::format(" (x{})", run) : std::string{}, pass.width,
                     pass.height, pass.color_view & 0xffff, pass.colors, pass.loaded_colors,
                     pass.depth_view & 0xffff,
                     !pass.depth ? " (none)" : pass.depth_loaded ? " (loaded)" : " (cleared)",
                     pass.depth ? " " + vk::to_string(pass.depth_layout) : std::string{},
                     pass.draws, (pass.vertices + 500) / 1000,
                     pass.ended_in != nullptr ? pass.ended_in : "the frame",
                     pass.ended_at);
            i += run;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - c.since).count();
    // Every five seconds, or as often as SHADPS4_FRAME_STATS_EVERY (seconds) says.
    static const double interval = [] {
        const char* value = std::getenv("SHADPS4_FRAME_STATS_EVERY");
        const double every = value != nullptr ? std::atof(value) : 0.0;
        return every > 0.0 ? every : 5.0;
    }();
    if (seconds < interval) {
        return;
    }
    if (FrameStatsLevel() > 1) {
        std::scoped_lock lock{g_frame_listing.mutex};
        if (g_frame_listing.state == Listing::Idle) {
            g_frame_listing.state = Listing::Armed;
        }
    }
    const double n = static_cast<double>(frames);
    static const auto start = now;
    std::string ab;
    if (ab_period > 0.0) {
        ab = ab_frames[0] != 0 && ab_frames[1] != 0 ? " [A+B]"
             : ab_frames[1] != 0                    ? " [B]"
                                                    : " [A]";
        ab_frames[0] = ab_frames[1] = 0;
    }
    // How busy the GPU was, and at what clock, since the last line.
    const u64 looks = g_log_looks.exchange(0, std::memory_order_relaxed);
    const u64 busy_looks = g_log_busy_looks.exchange(0, std::memory_order_relaxed);
    const u64 clocked_looks = g_log_clocked_looks.exchange(0, std::memory_order_relaxed);
    const u64 clocks = g_log_clocks.exchange(0, std::memory_order_relaxed);
    std::string gpu;
    if (looks != 0) {
        gpu = fmt::format(", GPU busy {:.0f}%", 100.0 * busy_looks / looks);
        if (clocked_looks != 0) {
            gpu += fmt::format(" at {:.0f} MHz", static_cast<double>(clocks) / clocked_looks);
        }
    }
    LOG_INFO(Render_Vulkan,
             "frame stats at {:.0f} s{}: {:.1f} fps, per frame {:.1f} passes ({:.1f} loading their "
             "targets, {:.1f} Mpx), {:.0f} draws, {:.1f} dispatches, {:.1f} submits{}",
             std::chrono::duration<double>(now - start).count(), ab, n / seconds,
             c.passes.Take() / n, c.loading_passes.Take() / n, c.pass_pixels.Take() / n / 1e6,
             c.draws.Take() / n, c.computes.Take() / n, c.submits.Take() / n, gpu);
    const auto stage_ms = [](Stage stage) {
        const u64 total = g_stage_times.total[static_cast<size_t>(stage)].exchange(0);
        const u64 count = g_stage_times.count[static_cast<size_t>(stage)].exchange(0);
        return count != 0 ? static_cast<double>(total) / static_cast<double>(count) / 1e6 : 0.0;
    };
    if (g_stage_times.count[static_cast<size_t>(Stage::Latency)] != 0) {
        LOG_INFO(Render_Vulkan,
                 "frame path: the title hands its lists over within {:.1f} ms, the GPU thread "
                 "is through them {:.1f} ms later ({:.1f} ms of its time per frame), the frame "
                 "is taken {:.1f} ms after that and flipped {:.1f} ms later; {:.1f} ms from the "
                 "first command list to the flip",
                 stage_ms(Stage::Submitting), stage_ms(Stage::CatchUp),
                 stage_ms(Stage::Translate), stage_ms(Stage::Queued), stage_ms(Stage::GpuWait),
                 stage_ms(Stage::Latency));
    }
    const auto per_frame = [n](Counter counter) {
        return static_cast<double>(g_counters[static_cast<size_t>(counter)].Take()) / n;
    };
    const double lookups = per_frame(Counter::ImageLookups);
    const double binds = per_frame(Counter::PipelineBinds);
    if (lookups > 0.0 || binds > 0.0) {
        const double stages = per_frame(Counter::StagePrograms);
        const double stages_short = per_frame(Counter::StageProgramsShort);
        const double pipelines = per_frame(Counter::PipelineLookups);
        const double pipelines_short = per_frame(Counter::PipelineLookupsShort);
        LOG_INFO(Render_Vulkan,
                 "frame shortcuts: {:.0f} of {:.0f} image lookups ({:.1f} images added or "
                 "removed), {:.0f} of {:.0f} pipeline binds, {:.0f} of {:.0f} shader "
                 "permutations, {:.0f} of {:.0f} pipeline lookups; {:.0f} thousand vertices "
                 "drawn; {:.0f} draws left out that write nothing",
                 per_frame(Counter::ImageLookupsShort), lookups,
                 per_frame(Counter::ImageRegistrations), per_frame(Counter::PipelineBindsShort),
                 binds, stages_short, stages, pipelines_short, pipelines,
                 per_frame(Counter::Vertices) / 1e3, per_frame(Counter::EmptyDraws));
    }
    if (CallTimesEnabled()) {
        std::string calls;
        double total_ms = 0.0;
        for (size_t i = 0; i < g_call_ns.size(); ++i) {
            const double ms = static_cast<double>(g_call_ns[i].exchange(0)) / n / 1e6;
            const double count = static_cast<double>(g_call_count[i].exchange(0)) / n;
            if (count == 0.0) {
                continue;
            }
            total_ms += ms;
            calls += fmt::format("{}{} {:.0f}x {:.2f} ms", calls.empty() ? "" : ", ", CallNames[i],
                                 count, ms);
        }
        LOG_INFO(Render_Vulkan, "driver calls per frame: {:.2f} ms in all: {}", total_ms, calls);
    }
    c.frames = 0;
    c.since = now;
}

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance)
    : instance{instance}, master_semaphore{instance}, command_pool{instance, &master_semaphore} {
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

/// Whether a pass begun with `current` can take a draw that asks for `next`: the same targets in
/// the same layouts, with nothing left to clear. Titles clear a target with the first draw to
/// it; ending the pass there, only to load everything again for the second draw, costs a GPU
/// that renders in tiles a round trip of the targets through memory.
static bool ContinuesPass(const RenderState& current, const RenderState& next) {
    if (current.width != next.width || current.height != next.height ||
        current.num_layers != next.num_layers ||
        current.num_color_attachments != next.num_color_attachments) {
        return false;
    }
    for (u32 i = 0; i < next.num_color_attachments; ++i) {
        const auto& ours = current.color_attachments[i];
        const auto& theirs = next.color_attachments[i];
        if (ours.image_view != theirs.image_view || ours.image_layout != theirs.image_layout ||
            theirs.is_clear != 0) {
            return false;
        }
    }
    if (current.shading_rate_view != next.shading_rate_view) {
        return false;
    }
    const auto& ours = current.depth_stencil_attachment;
    const auto& theirs = next.depth_stencil_attachment;
    return ours.image_view == theirs.image_view && ours.image_layout == theirs.image_layout &&
           ours.has_depth == theirs.has_depth && ours.has_stencil == theirs.has_stencil &&
           !theirs.depth_clear && !theirs.stencil_clear;
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    if (is_rendering && ContinuesPass(render_state, new_state)) {
        // From here on the pass is one without clears: a draw that asks for a clear again
        // means a new one.
        render_state = new_state;
        return;
    }
    EndRendering();
    // A pass that clears a target writes it again after the passes before, and those may have
    // written it too: both eyes, for one, are drawn into the same depth buffer, the second
    // cleared right after the first is done. Nothing else stands between such passes when the
    // target stays in its layout, and without a barrier the Quest's GPU driver, drawing straight
    // to memory, lets what the earlier pass left in its cache land on the cleared target later,
    // block by block: black squares in the right eye where its sky then failed the depth test
    // (issue #19). Under the A/B measuring aid as "clearbarrier".
    const auto& depth_target = new_state.depth_stencil_attachment;
    const bool clears_depth =
        depth_target.image_view && (depth_target.depth_clear || depth_target.stencil_clear);
    bool clears_color = false;
    for (u32 i = 0; i < new_state.num_color_attachments; ++i) {
        clears_color |= new_state.color_attachments[i].image_view &&
                        new_state.color_attachments[i].is_clear;
    }
    if ((clears_depth || clears_color) && !FrameStats::AbOff("clearbarrier")) {
        const vk::MemoryBarrier2 earlier_writes = {
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                            vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                            vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                             vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                             vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        };
        current_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &earlier_writes,
        });
    }
    is_rendering = true;
    render_state = new_state;
    bound_pipeline = nullptr;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const u32 texel = instance.FoveationTexel();
    const vk::RenderingFragmentShadingRateAttachmentInfoKHR shading_rate = {
        .imageView = render_state.shading_rate_view,
        .imageLayout = vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR,
        .shadingRateAttachmentTexelSize = {texel, texel},
    };
    const vk::RenderingInfo rendering_info = {
        .pNext = render_state.shading_rate_view ? &shading_rate : nullptr,
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    {
        const FrameStats::CallTimer timer{FrameStats::Call::BeginRendering};
        current_cmdbuf.beginRendering(rendering_info);
    }

    FrameStats::RenderPass(render_state);
}

void Scheduler::EndRendering(std::source_location where) {
    if (!is_rendering) {
        return;
    }
    FrameStats::PassEnded(where);
    GpuProfile::NotePassEnd(where);
    is_rendering = false;
    bound_pipeline = nullptr;
    // (SHADPS4_DBG_PUSH_ONCE: what is pushed between passes, by the blit helper among others,
    // is not known here.)
    last_push_layout = vk::PipelineLayout{};
    if (const u32 limit = FlushDraws(); limit != 0 && draws_since_flush >= limit) {
        flush_due = true;
    }
    const FrameStats::CallTimer timer{FrameStats::Call::EndRendering};
    current_cmdbuf.endRendering();
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
    PopPendingOperations();
}

void Scheduler::Wait(u64 tick) {
    if (tick >= master_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    // From here on the tick has been submitted and the wait is for the GPU: milliseconds. A
    // driver that has lost track of the tick only finds it again when something newer is
    // submitted, and the thread that would do that is this one.
    while (!master_semaphore.Wait(tick, std::chrono::seconds{1})) {
        LOG_WARNING(Render_Vulkan,
                    "Tick {} was not reached within a second (the GPU is at {}): submitting again "
                    "to move the driver along",
                    tick, master_semaphore.KnownGpuTick());
        SubmitInfo info{};
        Flush(info);
    }
}

void Scheduler::PopPendingOperations() {
    // SubmitExecution's in-flight limit periodically advances the cached GPU tick. Querying the
    // timeline semaphore here makes Turnip enter a blocking ioctl on every submission, defeating
    // that batching. Only reclaim objects already proven idle; Finish() drains after its wait.
    while (!pending_ops.empty() && master_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));
    last_push_layout = vk::PipelineLayout{};

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();
    bound_pipeline = nullptr;

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

bool IsSyncTraceEnabled() {
    static const bool enabled = std::getenv("SHADPS4_TRACE_SYNC") != nullptr;
    return enabled;
}

void TraceSync(const char* format, ...) {
    // The monotonic clock, as the driver shim's trace uses.
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    char line[320];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "vk   %ld.%03ld %s\n", static_cast<long>(now / 1000 % 1000),
                 static_cast<long>(now % 1000), line);
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = master_semaphore.NextTick();
    if (signal_value > MAX_IN_FLIGHT_SUBMISSIONS) {
        master_semaphore.Wait(signal_value - MAX_IN_FLIGHT_SUBMISSIONS);
    }

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    EndRendering();
    draws_since_flush = 0;
    flush_due = false;
    Check(current_cmdbuf.end());

    const vk::Semaphore timeline = master_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    if (IsSyncTraceEnabled()) {
        char waits[160] = "";
        int length = 0;
        // The last signal is this scheduler's own timeline.
        for (u32 i = 0; i < info.num_wait_semas && length < 120; ++i) {
            length += std::snprintf(waits + length, sizeof(waits) - length, " wait %llx@%llu",
                                    static_cast<unsigned long long>(
                                        reinterpret_cast<uintptr_t>(static_cast<VkSemaphore>(
                                            info.wait_semas[i]))),
                                    static_cast<unsigned long long>(info.wait_ticks[i]));
        }
        TraceSync("submit timeline %llx tick %llu%s fence %llx",
                  static_cast<unsigned long long>(
                      reinterpret_cast<uintptr_t>(static_cast<VkSemaphore>(timeline))),
                  static_cast<unsigned long long>(signal_value), waits,
                  static_cast<unsigned long long>(
                      reinterpret_cast<uintptr_t>(static_cast<VkFence>(info.fence))));
    }
    FrameStats::Submit();
    std::optional<FrameStats::CallTimer> submit_timer;
    submit_timer.emplace(FrameStats::Call::Submit);
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    submit_timer.reset();
    if (IsSyncTraceEnabled()) {
        TraceSync("submit tick %llu returned %d", static_cast<unsigned long long>(signal_value),
                  static_cast<int>(submit_result));
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    if (submit_result != vk::Result::eSuccess) {
        // Whatever was to be signalled by this submission never will be: say so, or the
        // resulting wait looks like a hang out of nowhere.
        LOG_CRITICAL(Render_Vulkan, "Queue submission {} failed: {}", signal_value,
                     vk::to_string(submit_result));
    }

    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        master_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan
