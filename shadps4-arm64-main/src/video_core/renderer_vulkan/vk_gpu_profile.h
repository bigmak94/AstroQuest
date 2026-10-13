// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <source_location>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// SHADPS4_GPU_PROFILE=<seconds>: once in that many seconds the GPU times every draw and
/// dispatch of one frame on its own, and the log gets what the frame's GPU time went on: pass by
/// pass, by the targets drawn to and by shader. Turnip lets the GPU run dry before it writes a
/// timestamp, so each draw is timed without the ones around it; that frame takes longer than
/// the others. A measuring aid for finding out what a title's frame costs, not a setting.
class GpuProfile {
public:
    /// What a draw or dispatch is, as far as the tables go.
    struct Work {
        u64 vs{};     ///< The guest's vertex shader (the compute shader of a dispatch), by hash.
        u64 ps{};     ///< The guest's pixel shader, 0 when the draw has none.
        u16 width{};  ///< Size of what is drawn to; 0 for a dispatch.
        u16 height{};
        u8 colors{};  ///< Colour targets written.
        bool depth{}; ///< A depth (or stencil) target is bound.
        bool compute{};
        u32 format{}; ///< The first colour target's guest format (data << 8 | number), or 0.
        u64 target{}; ///< Guest address of the first colour target, else of the depth target.

        bool SameTargets(const Work& other) const {
            return width == other.width && height == other.height && colors == other.colors &&
                   depth == other.depth && compute == other.compute && format == other.format &&
                   target == other.target;
        }
    };

    GpuProfile(const Instance& instance, Scheduler& scheduler);
    ~GpuProfile();

    GpuProfile(const GpuProfile&) = delete;
    GpuProfile& operator=(const GpuProfile&) = delete;

    /// Whether SHADPS4_GPU_PROFILE asks for it.
    static bool Enabled();

    /// The GPU thread is through a frame. Called where it hands a frame on; the profile itself
    /// looks at the count with its next draw.
    static void NoteFrameDone();

    /// What the emulator itself has the GPU do between the title's draws. While a frame is
    /// timed, it is counted for the draw that comes next and listed with its run.
    enum class HostWork : u8 {
        BufferUpload, ///< Guest memory copied into a buffer of the GPU's.
        BufferCopy,   ///< A buffer copied into another (or written by the command buffer).
        BufferFill,   ///< A buffer filled with a value.
        ImageUpload,  ///< Guest memory turned into an image (copied, detiled where it must be).
        ImageCopy,    ///< An image copied or resolved into another.
        Clear,        ///< An image cleared outside a render pass.
        Barrier,      ///< A pipeline barrier for an image's layout or access.
        PassEnd,      ///< A render pass ended.
        Count,
    };
    static void Note(HostWork kind, u64 bytes = 0);
    /// A render pass ended, and where in the emulator.
    static void NotePassEnd(const std::source_location& where);

    /// A draw or dispatch is about to be recorded into the scheduler's command buffer. True
    /// when it is timed: After must then follow it.
    bool Before(const Work& work, u64 vertices);
    void After();

private:
    enum class State { Waiting, Recording, Reading };

    struct Record {
        Work work;
        u64 vertices{};
        /// What the emulator had the GPU do since the draw before.
        std::array<u32, static_cast<size_t>(HostWork::Count)> host_count{};
        std::array<u64, static_cast<size_t>(HostWork::Count)> host_bytes{};
        /// Where the last render pass before it was ended, if one was.
        const char* ended_in{};
        u32 ended_at{};
    };

    void TakeFrame();
    void Read(std::vector<Record> records);

    const Instance& instance;
    Scheduler& scheduler;
    vk::QueryPool pool{};
    double tick_ns{};
    u64 valid_mask{};
    State state{State::Waiting};
    u32 frames_seen{};
    u32 frames_recorded{};
    /// Whether the frame being timed was drawn with the A/B measuring aid's "B" (FrameStats).
    bool recorded_b{};
    std::chrono::steady_clock::time_point last_profile{};
    std::vector<Record> records;
};

} // namespace Vulkan
