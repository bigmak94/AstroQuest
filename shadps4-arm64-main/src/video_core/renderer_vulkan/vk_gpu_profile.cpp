// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <tuple>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_gpu_profile.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

/// Two timestamps a draw: room for a frame of 8192 draws and dispatches.
constexpr u32 MaxQueries = 16384;

std::atomic<u32> g_frames_done{0};

/// Host work noted since the last timed draw, while a frame is timed.
constexpr size_t HostWorkKinds = static_cast<size_t>(GpuProfile::HostWork::Count);
std::atomic<bool> g_noting{false};
std::array<std::atomic<u32>, HostWorkKinds> g_host_count{};
std::array<std::atomic<u64>, HostWorkKinds> g_host_bytes{};
std::atomic<const char*> g_ended_in{nullptr};
std::atomic<u32> g_ended_at{0};

constexpr const char* HostWorkNames[HostWorkKinds] = {
    "buffer uploads", "buffer copies", "buffer fills", "image uploads",
    "image copies",   "clears",        "barriers",     "pass ends",
};

const char* FileName(const char* path) {
    const char* name = path;
    for (const char* c = path; *c != '\0'; ++c) {
        if (*c == '/' || *c == '\\') {
            name = c + 1;
        }
    }
    return name;
}

/// When the emulator started, for the times in the log.
const auto g_started = std::chrono::steady_clock::now();

double ProfileInterval() {
    static const double seconds = [] {
        const char* value = std::getenv("SHADPS4_GPU_PROFILE");
        return value != nullptr ? std::max(std::atof(value), 0.0) : 0.0;
    }();
    return seconds;
}

/// The GPU's clock now, in MHz, where the system tells it (a Qualcomm GPU on Linux); 0 if not.
double GpuClockMhz() {
#ifdef __linux__
    if (std::FILE* file = std::fopen("/sys/class/kgsl/kgsl-3d0/gpuclk", "r"); file != nullptr) {
        unsigned long long hz = 0;
        const int read = std::fscanf(file, "%llu", &hz);
        std::fclose(file);
        return read == 1 ? static_cast<double>(hz) / 1e6 : 0.0;
    }
#endif
    return 0.0;
}

std::string Describe(const GpuProfile::Work& work) {
    if (work.compute) {
        return "dispatch";
    }
    std::string text = fmt::format("{}x{}", work.width, work.height);
    if (work.colors != 0) {
        text += fmt::format(" colour x{} fmt {:04x}", work.colors, work.format);
    }
    if (work.depth) {
        text += work.colors != 0 ? " + depth" : " depth only";
    }
    return text + fmt::format(" @{:x}", work.target);
}

struct Sum {
    u32 count{};
    u64 vertices{};
    double ns{};
    double gap_ns{};

    void Add(u64 vertices_, double ns_, double gap_ns_) {
        ++count;
        vertices += vertices_;
        ns += ns_;
        gap_ns += gap_ns_;
    }
};

} // namespace

GpuProfile::GpuProfile(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const auto physical = instance.GetPhysicalDevice();
    const auto properties = physical.getProperties();
    tick_ns = properties.limits.timestampPeriod;
    const auto families = physical.getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    const u32 bits = family < families.size() ? families[family].timestampValidBits : 0;
    valid_mask = bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const auto features =
        physical.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
    const bool host_reset = features.get<vk::PhysicalDeviceVulkan12Features>().hostQueryReset;
    if (bits == 0 || !host_reset || tick_ns <= 0.0) {
        LOG_WARNING(Render_Vulkan,
                    "GPU profile: the GPU cannot time draws here (timestamp bits {}, host query "
                    "reset {}, {} ns a tick)",
                    bits, host_reset, tick_ns);
        return;
    }
    const auto device = instance.GetDevice();
    auto [result, created] = device.createQueryPool({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = MaxQueries,
    });
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "GPU profile: no query pool: {}", vk::to_string(result));
        return;
    }
    pool = created;
    device.resetQueryPool(pool, 0, MaxQueries);
    last_profile = std::chrono::steady_clock::now();
    LOG_INFO(Render_Vulkan,
             "GPU profile: one frame every {:.0f} s is timed draw by draw ({:.2f} ns a tick, {} "
             "bits)",
             ProfileInterval(), tick_ns, bits);
}

GpuProfile::~GpuProfile() {
    if (pool) {
        instance.GetDevice().destroyQueryPool(pool);
    }
}

bool GpuProfile::Enabled() {
    return ProfileInterval() > 0.0;
}

void GpuProfile::NoteFrameDone() {
    if (Enabled()) {
        g_frames_done.fetch_add(1, std::memory_order_relaxed);
    }
}

void GpuProfile::Note(HostWork kind, u64 bytes) {
    if (!g_noting.load(std::memory_order_relaxed)) {
        return;
    }
    const size_t index = static_cast<size_t>(kind);
    g_host_count[index].fetch_add(1, std::memory_order_relaxed);
    g_host_bytes[index].fetch_add(bytes, std::memory_order_relaxed);
}

void GpuProfile::NotePassEnd(const std::source_location& where) {
    if (!g_noting.load(std::memory_order_relaxed)) {
        return;
    }
    Note(HostWork::PassEnd);
    g_ended_in.store(where.file_name(), std::memory_order_relaxed);
    g_ended_at.store(where.line(), std::memory_order_relaxed);
}

void GpuProfile::TakeFrame() {
    const u32 frames = g_frames_done.load(std::memory_order_relaxed);
    if (frames == frames_seen) {
        return;
    }
    frames_seen = frames;
    if (state == State::Recording) {
        if (records.empty()) {
            return;
        }
        // The frame's timestamps are read once the GPU is through the command buffer that has
        // the last of them, which is the one being recorded now.
        state = State::Reading;
        g_noting.store(false, std::memory_order_relaxed);
        scheduler.DeferOperation(
            [this, taken = std::move(records)]() mutable { Read(std::move(taken)); });
        records = {};
        return;
    }
    if (state == State::Waiting && pool &&
        std::chrono::steady_clock::now() - last_profile >=
            std::chrono::duration<double>(ProfileInterval())) {
        state = State::Recording;
        recorded_b = FrameStats::AbOff("eye2") || FrameStats::AbOff();
        records.reserve(MaxQueries / 2);
        for (size_t i = 0; i < HostWorkKinds; ++i) {
            g_host_count[i].store(0, std::memory_order_relaxed);
            g_host_bytes[i].store(0, std::memory_order_relaxed);
        }
        g_ended_in.store(nullptr, std::memory_order_relaxed);
        g_noting.store(true, std::memory_order_relaxed);
    }
}

bool GpuProfile::Before(const Work& work, u64 vertices) {
    TakeFrame();
    if (state != State::Recording || (records.size() + 1) * 2 > MaxQueries) {
        return false;
    }
    const u32 query = static_cast<u32>(records.size()) * 2;
    scheduler.CommandBuffer().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool,
                                             query);
    Record& record = records.emplace_back(Record{.work = work, .vertices = vertices});
    for (size_t i = 0; i < HostWorkKinds; ++i) {
        record.host_count[i] = g_host_count[i].exchange(0, std::memory_order_relaxed);
        record.host_bytes[i] = g_host_bytes[i].exchange(0, std::memory_order_relaxed);
    }
    if (const char* file = g_ended_in.exchange(nullptr, std::memory_order_relaxed)) {
        record.ended_in = FileName(file);
        record.ended_at = g_ended_at.load(std::memory_order_relaxed);
    }
    return true;
}

void GpuProfile::After() {
    const u32 query = static_cast<u32>(records.size()) * 2 - 1;
    scheduler.CommandBuffer().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool,
                                             query);
}

void GpuProfile::Read(std::vector<Record> taken) {
    const auto device = instance.GetDevice();
    const u32 count = static_cast<u32>(taken.size()) * 2;
    std::vector<u64> stamps(count);
    const vk::Result result =
        device.getQueryPoolResults(pool, 0, count, stamps.size() * sizeof(u64), stamps.data(),
                                   sizeof(u64), vk::QueryResultFlagBits::e64);
    device.resetQueryPool(pool, 0, MaxQueries);
    state = State::Waiting;
    last_profile = std::chrono::steady_clock::now();
    ++frames_recorded;
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "GPU profile: the frame's timestamps are not there: {}",
                    vk::to_string(result));
        return;
    }
    const auto span_ns = [&](u32 from, u32 to) {
        const u64 ticks = (stamps[to] - stamps[from]) & valid_mask;
        // A timestamp written before an earlier one (it should not be) counts as nothing.
        return ticks > valid_mask / 2 ? 0.0 : static_cast<double>(ticks) * tick_ns;
    };

    using ShaderKey = std::tuple<u64, u64, u16, u16, u8, bool, bool, u32, u64>;
    const auto shader_key = [](const Work& w) {
        return ShaderKey{w.vs,     w.ps,      w.width,  w.height, w.colors,
                         w.depth,  w.compute, w.format, w.target};
    };
    using TargetKey = std::tuple<u16, u16, u8, bool, bool, u32, u64>;
    const auto target_key = [](const Work& w) {
        return TargetKey{w.width, w.height, w.colors, w.depth, w.compute, w.format, w.target};
    };
    std::map<ShaderKey, std::pair<Work, Sum>> by_shader;
    std::map<TargetKey, std::pair<Work, Sum>> by_target;
    std::map<u64, Sum> by_ps;

    struct Run {
        Work work;
        Sum sum;
        std::array<u32, HostWorkKinds> host_count{};
        std::array<u64, HostWorkKinds> host_bytes{};
        /// Where the passes ended that ended before its draws, and how often.
        std::map<std::pair<std::string_view, u32>, u32> ends;
    };
    std::array<u32, HostWorkKinds> frame_count{};
    std::array<u64, HostWorkKinds> frame_bytes{};
    const auto host_text = [](const auto& counts, const auto& bytes) {
        std::string text;
        for (size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] == 0) {
                continue;
            }
            text += fmt::format("{}{} {}", text.empty() ? "" : ", ", counts[i], HostWorkNames[i]);
            if (bytes[i] != 0) {
                text += fmt::format(" ({} KB)", (bytes[i] + 1023) / 1024);
            }
        }
        return text;
    };
    std::vector<Run> runs;
    /// Each draw's wait before it: how long, which draw, in which run (counted before it is
    /// added: a draw that starts a run has the index of the run it starts).
    struct Gap {
        double ns;
        u32 record;
        u32 run;
    };
    std::vector<Gap> gaps;

    double total_ns = 0.0;
    double total_gap_ns = 0.0;
    u32 draws = 0;
    u32 dispatches = 0;
    u64 vertices = 0;
    for (u32 i = 0; i < taken.size(); ++i) {
        const auto& record = taken[i];
        const double ns = span_ns(i * 2, i * 2 + 1);
        const double gap_ns = i == 0 ? 0.0 : span_ns(i * 2 - 1, i * 2);
        total_ns += ns;
        total_gap_ns += gap_ns;
        vertices += record.vertices;
        gaps.push_back({gap_ns, i, static_cast<u32>(runs.size())});
        (record.work.compute ? dispatches : draws) += 1;

        auto& shader = by_shader[shader_key(record.work)];
        shader.first = record.work;
        shader.second.Add(record.vertices, ns, gap_ns);
        auto& target = by_target[target_key(record.work)];
        target.first = record.work;
        target.second.Add(record.vertices, ns, gap_ns);
        if (!record.work.compute) {
            by_ps[record.work.ps].Add(record.vertices, ns, gap_ns);
        }
        if (runs.empty() || !runs.back().work.SameTargets(record.work)) {
            runs.push_back({record.work, {}});
        }
        auto& run = runs.back();
        run.sum.Add(record.vertices, ns, gap_ns);
        for (size_t k = 0; k < HostWorkKinds; ++k) {
            run.host_count[k] += record.host_count[k];
            run.host_bytes[k] += record.host_bytes[k];
            frame_count[k] += record.host_count[k];
            frame_bytes[k] += record.host_bytes[k];
        }
        if (record.ended_in != nullptr) {
            ++run.ends[{record.ended_in, record.ended_at}];
        }
    }
    const double all_ns = total_ns + total_gap_ns;
    const auto share = [all_ns](double ns) { return all_ns > 0.0 ? 100.0 * ns / all_ns : 0.0; };

    LOG_INFO(Render_Vulkan,
             "GPU profile {}{} at {:.0f} s: {} draws ({} thousand vertices) and {} dispatches took "
             "{:.2f} ms timed one by one: {:.2f} in themselves, {:.2f} in what came between them "
             "(clears, copies, barriers, passes starting and ending); GPU clock now {:.0f} MHz",
             frames_recorded, recorded_b ? " [B]" : "",
             std::chrono::duration<double>(std::chrono::steady_clock::now() - g_started).count(),
             draws, (vertices + 500) / 1000, dispatches, all_ns / 1e6, total_ns / 1e6,
             total_gap_ns / 1e6, GpuClockMhz());
    LOG_INFO(Render_Vulkan, "GPU profile {}: the emulator's own work in the frame: {}",
             frames_recorded, host_text(frame_count, frame_bytes));

    LOG_INFO(Render_Vulkan, "GPU profile {}: the frame in order, a line for each run of work "
                            "on the same targets:",
             frames_recorded);
    for (u32 i = 0; i < runs.size(); ++i) {
        const auto& run = runs[i];
        LOG_INFO(Render_Vulkan, "  run {:3}: {:6.3f} ms (+{:.3f} before) {:4} {} {:6}k vertices "
                                "on {}",
                 i, run.sum.ns / 1e6, run.sum.gap_ns / 1e6, run.sum.count,
                 run.work.compute ? "dispatches" : "draws", (run.sum.vertices + 500) / 1000,
                 Describe(run.work));
        const std::string host = host_text(run.host_count, run.host_bytes);
        if (!host.empty()) {
            std::string ends;
            for (const auto& [place, times] : run.ends) {
                ends += fmt::format("{}{}:{} x{}", ends.empty() ? "; passes ended in " : ", ",
                                    place.first, place.second, times);
            }
            LOG_INFO(Render_Vulkan, "           before and among them: {}{}", host, ends);
        }
    }

    std::ranges::sort(gaps, [](const Gap& a, const Gap& b) { return a.ns > b.ns; });
    LOG_INFO(Render_Vulkan, "GPU profile {}: the longest waits before a draw", frames_recorded);
    for (u32 i = 0; i < std::min<size_t>(gaps.size(), 20); ++i) {
        const Gap& gap = gaps[i];
        const Record& record = taken[gap.record];
        // A draw that started a new run has the run's index; one inside a run, the run before.
        const bool starts_run =
            gap.record == 0 || !taken[gap.record - 1].work.SameTargets(record.work);
        std::string ended;
        if (record.ended_in != nullptr) {
            ended = fmt::format("; pass ended in {}:{}", record.ended_in, record.ended_at);
        }
        LOG_INFO(Render_Vulkan, "  {:6.3f} ms before draw {:4} ({} run {}) on {}: {}{}",
                 gap.ns / 1e6, gap.record, starts_run ? "starting" : "in",
                 starts_run ? gap.run : gap.run - 1, Describe(record.work),
                 host_text(record.host_count, record.host_bytes), ended);
    }

    std::vector<std::pair<Work, Sum>> targets;
    for (const auto& [key, value] : by_target) {
        targets.push_back(value);
    }
    std::ranges::sort(targets, [](const auto& a, const auto& b) {
        return a.second.ns + a.second.gap_ns > b.second.ns + b.second.gap_ns;
    });
    LOG_INFO(Render_Vulkan, "GPU profile {}: by target", frames_recorded);
    for (u32 i = 0; i < std::min<size_t>(targets.size(), 24); ++i) {
        const auto& [work, sum] = targets[i];
        LOG_INFO(Render_Vulkan, "  {:6.3f} ms {:4.1f}% (+{:.3f} between) {:4} {} {:6}k vertices "
                                "on {}",
                 sum.ns / 1e6, share(sum.ns + sum.gap_ns), sum.gap_ns / 1e6, sum.count,
                 work.compute ? "dispatches" : "draws", (sum.vertices + 500) / 1000,
                 Describe(work));
    }

    std::vector<std::pair<Work, Sum>> shaders;
    for (const auto& [key, value] : by_shader) {
        shaders.push_back(value);
    }
    std::ranges::sort(shaders, [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    LOG_INFO(Render_Vulkan, "GPU profile {}: by shader and target", frames_recorded);
    for (u32 i = 0; i < std::min<size_t>(shaders.size(), 48); ++i) {
        const auto& [work, sum] = shaders[i];
        LOG_INFO(Render_Vulkan, "  {:6.3f} ms {:4.1f}% {:4} {} {:6}k vertices {} {:#x} ps {:#x} "
                                "on {}",
                 sum.ns / 1e6, share(sum.ns), sum.count, work.compute ? "dispatches" : "draws",
                 (sum.vertices + 500) / 1000, work.compute ? "cs" : "vs", work.vs, work.ps,
                 Describe(work));
    }

    std::vector<std::pair<u64, Sum>> pixel_shaders(by_ps.begin(), by_ps.end());
    std::ranges::sort(pixel_shaders,
                      [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    LOG_INFO(Render_Vulkan, "GPU profile {}: by pixel shader", frames_recorded);
    for (u32 i = 0; i < std::min<size_t>(pixel_shaders.size(), 32); ++i) {
        const auto& [ps, sum] = pixel_shaders[i];
        LOG_INFO(Render_Vulkan, "  {:6.3f} ms {:4.1f}% {:4} draws {:6}k vertices ps {:#x}",
                 sum.ns / 1e6, share(sum.ns), sum.count, (sum.vertices + 500) / 1000, ps);
    }
}

} // namespace Vulkan
