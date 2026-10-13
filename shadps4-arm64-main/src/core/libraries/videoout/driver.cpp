// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <fmt/ranges.h>

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "platform/bachata/runtime_client.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/known_title.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_gpu_profile.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {

static std::atomic_uint32_t submit_traces{};
static std::atomic_uint32_t dispatch_traces{};
static std::atomic_uint32_t internal_traces{};
static std::atomic_uint32_t queue_traces{};
static std::atomic_uint32_t dequeue_traces{};
static std::atomic_uint32_t present_traces{};

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr s32 MainPortHandle = 1;
constexpr s32 SocialPortHandle = 2;
constexpr u32 SocialScreenRefreshRate = 60;

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    social_port.resolution = main_port.resolution;
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params, s32 bus_type) {
    if (bus_type == SCE_VIDEO_OUT_BUS_TYPE_AUX_SOCIAL_SCREEN) {
        if (social_port.is_open) {
            return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
        }
        social_port.is_open = true;
        return SocialPortHandle;
    }
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    liverpool->SetVoPort(&main_port);
    return MainPortHandle;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};
    auto* port = GetPort(handle);
    if (port == nullptr) {
        return;
    }

    // Mark as closed
    port->is_open = false;
    port->flip_rate = 0;
    port->prev_index = -1;
    port->flip_labels.ResetAll();

    // Clear port information
    std::memset(port->buffer_labels.data(), 0, sizeof(port->buffer_labels));
    std::memset(port->groups.data(), 0, sizeof(port->groups));
    std::memset(&port->vblank_status, 0, sizeof(port->vblank_status));
    port->flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(port->buffer_slots.data(), 0, sizeof(port->buffer_slots));
    for (auto& buffer : port->buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    port->flip_events.clear();
    for (auto event : port->vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    port->vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    switch (handle) {
    case MainPortHandle:
        return &main_port;
    case SocialPortHandle:
        return &social_port;
    default:
        return nullptr;
    }
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->flip_labels.ResetBuffer(static_cast<s32>(startIndex + i));
        port->SignalVoLabel();

        // Social screen buffers are never presented, the renderer has no business with them.
        if (port != &social_port) {
            presenter->RegisterVideoOutSurface(group, address);
        }
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (s32 i = 0; i < static_cast<s32>(port->buffer_slots.size()); ++i) {
        auto& buffer = port->buffer_slots[i];
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
        port->flip_labels.ResetBuffer(i);
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

void VideoOutDriver::Flip(const Request& req) {
    if (present_traces.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOG_INFO(Lib_VideoOut,
                 "BACHATA_FLIP_TRACE stage=present index={} arg={} eop={} frame={}", req.index,
                 req.flip_arg, req.eop, static_cast<const void*>(req.frame));
    }
    // Update HDR status before presenting.
    presenter->SetHDR(req.port->is_hdr);

    // Tell the host which pose this frame belongs to before it can see the image.
    using Clock = std::chrono::steady_clock;
    using Vulkan::FrameStats;
    const auto taken = Clock::now();
    const auto note_flip = [&] {
        if (req.is_hmd && req.started != Clock::time_point{}) {
            const auto now = Clock::now();
            FrameStats::Time(FrameStats::Stage::GpuWait, now - taken);
            FrameStats::Time(FrameStats::Stage::Latency, now - req.started);
        }
    };
    if (req.is_hmd) {
        Core::Vr::Runtime::Instance().NotifyFramePresented(req.hmd_frame);
        // The machine's own headset gets its picture first; the window's is a look at it.
        if (req.export_frame != nullptr) {
            presenter->DeliverHmdFrame(req.export_frame, req.hmd_frame);
        }
        // A VR host shows the frame itself, there is nothing to put on the window.
        if (req.frame == nullptr || presenter->DeliverHmdFrame(req.frame, req.hmd_frame)) {
            Platform::Bachata::ReportPresentedFrame();
            note_flip();
            // (Frames that are put on a window are counted where that is done.)
            FrameStats::EndFrame();
            FinishFlip(req.port, req.index, req.flip_arg, req.eop, req.lock_generation);
            return;
        }
    }

    // Present the frame.
    presenter->Present(req.frame);
    note_flip();
    Platform::Bachata::ReportPresentedFrame();
    if (present_traces.load(std::memory_order_relaxed) <= 32) {
        LOG_INFO(Lib_VideoOut, "BACHATA_FLIP_TRACE stage=present_returned index={} arg={}",
                 req.index, req.flip_arg);
    }

    FinishFlip(req.port, req.index, req.flip_arg, req.eop, req.lock_generation);
}

void VideoOutDriver::FinishFlip(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop,
                                u64 lock_generation) {
    // Update flip status.
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = flip_arg;
        flip_status.current_buffer = index;
        if (is_eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        {
            std::scoped_lock lock{port->port_mutex};
            port->flip_labels.CancelRetirementForIndex(port->prev_index);
        }
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = index;
    if (is_eop && lock_generation != FlipLabelTracker::kInvalidGeneration && index >= 0) {
        std::scoped_lock lock{port->port_mutex};
        port->flip_labels.ScheduleRetirement(index, lock_generation,
                                             port->vblank_status.count + 1);
    }
}

void VideoOutDriver::DrawBlankFrame() {
    const auto empty_frame = presenter->PrepareBlankFrame(false);
    if (empty_frame) {
        presenter->Present(empty_frame);
    }
}

void VideoOutDriver::DrawLastFrame() {
    const auto frame = presenter->PrepareLastFrame();
    if (frame != nullptr) {
        presenter->Present(frame, true);
    }
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/, u64 lock_generation) {
    if (submit_traces.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOG_INFO(Lib_VideoOut,
                 "BACHATA_FLIP_TRACE stage=driver_submit index={} arg={} eop={} pending={}", index,
                 flip_arg, is_eop, port->flip_status.flip_pending_num);
    }
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (port == &social_port) {
        // Nothing displays the TV image while the headset is in use, so the flip only has to
        // complete from the guest's point of view.
        FinishFlip(port, index, flip_arg, is_eop, lock_generation);
        return true;
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() {
            if (dispatch_traces.fetch_add(1, std::memory_order_relaxed) < 32) {
                LOG_INFO(Lib_VideoOut,
                         "BACHATA_FLIP_TRACE stage=gpu_dispatch index={} arg={} eop={}", index,
                         flip_arg, is_eop);
            }
            SubmitFlipInternal(port, index, flip_arg, is_eop, lock_generation);
        });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop, lock_generation);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop,
                                        u64 lock_generation) {
    if (internal_traces.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOG_INFO(Lib_VideoOut,
                 "BACHATA_FLIP_TRACE stage=prepare_enter index={} arg={} eop={}", index, flip_arg,
                 is_eop);
    }
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }
    if (!frame) {
        // GpuComm was inside a graphics task; waiting on the present fence
        // would stall PM4. Retry after the current task yields.
        liverpool->EnqueueCommand([=, this] {
            SubmitFlipInternal(port, index, flip_arg, is_eop, lock_generation);
        });
        return;
    }

    if (queue_traces.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOG_INFO(Lib_VideoOut,
                 "BACHATA_FLIP_TRACE stage=prepare_done index={} arg={} frame={}", index, flip_arg,
                 static_cast<const void*>(frame));
    }
    Vulkan::GpuProfile::NoteFrameDone();

    std::scoped_lock lock{mutex};
    requests.push({
        .frame = frame,
        .port = port,
        .flip_arg = flip_arg,
        .index = index,
        .eop = is_eop,
        .lock_generation = lock_generation,
    });
    if (queue_traces.load(std::memory_order_relaxed) <= 32) {
        LOG_INFO(Lib_VideoOut,
                 "BACHATA_FLIP_TRACE stage=queue_insert index={} arg={} depth={}", index, flip_arg,
                 requests.size());
    }
}

bool VideoOutDriver::SubmitHmdFrame(VideoOutPort* port, const HmdFrame& hmd_frame) {
    {
        std::unique_lock lock{port->port_mutex};
        if (port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }
        ++port->flip_status.flip_pending_num;
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    // The eye textures live in the texture cache, so the frame is composed on the GPU thread.
    // Where the head was for it is worked out here and now: the seat the title's pose was
    // counted from may be another by the time the GPU thread gets to the frame.
    HmdFrame frame = hmd_frame;
    frame.render_pose = Core::Vr::Runtime::Instance().ToHostSpace(hmd_frame.render_pose);
    const u32 frame_id = Core::Vr::Runtime::Instance().NextFrameId();
    const auto started = Vulkan::FrameStats::TakeGuestSubmit();
    const auto handed_over = std::chrono::steady_clock::now();
    if (started != std::chrono::steady_clock::time_point{}) {
        Vulkan::FrameStats::Time(Vulkan::FrameStats::Stage::Submitting, handed_over - started);
    }
    liverpool->SendCommand([=, this]() {
        SubmitHmdFrameInternal(port, frame, frame_id, started, handed_over);
    });
    return true;
}

void VideoOutDriver::SubmitHmdFrameInternal(VideoOutPort* port, const HmdFrame& hmd_frame,
                                            u32 frame_id,
                                            std::chrono::steady_clock::time_point started,
                                            std::chrono::steady_clock::time_point handed_over) {
    u32 eye_width = 0;
    u32 eye_height = 0;
    const Vulkan::HmdFrames frames =
        presenter->PrepareHmdFrame(hmd_frame.eye_textures, hmd_frame.fov, frame_id, eye_width,
                                   eye_height);
    if (!frames) {
        // Same as regular flips: retry once the current graphics task yields.
        liverpool->EnqueueCommand([=, this] {
            SubmitHmdFrameInternal(port, hmd_frame, frame_id, started, handed_over);
        });
        return;
    }
    Vulkan::FrameStats::Time(Vulkan::FrameStats::Stage::CatchUp,
                             std::chrono::steady_clock::now() - handed_over);
    Vulkan::GpuProfile::NoteFrameDone();
    Vulkan::FrameStats::NoteHmdFrame();

    {
        // This runs on the GPU thread, once per frame: what it has used of the processor
        // since the last time is what the frame cost it. (The choice of how many refreshes a
        // frame is given goes by it too.)
        static std::chrono::nanoseconds last_thread_time{};
        const auto thread_time = Vulkan::FrameStats::ThreadTime();
        if (last_thread_time.count() != 0) {
            Vulkan::FrameStats::Time(Vulkan::FrameStats::Stage::Translate,
                                     thread_time - last_thread_time);
            Vulkan::FrameStats::NoteGpuThreadFrame(thread_time - last_thread_time);
        }
        last_thread_time = thread_time;
    }

    std::scoped_lock lock{mutex};
    requests.push({
        .frame = frames.shown,
        .port = port,
        .flip_arg = hmd_frame.flip_arg,
        .index = hmd_frame.display_index,
        .eop = false,
        .lock_generation = FlipLabelTracker::kInvalidGeneration,
        .is_hmd = true,
        .hmd_frame{
            .id = frame_id,
            .render_pose = hmd_frame.render_pose,
            .fov = hmd_frame.fov,
            .eye_width = eye_width,
            .eye_height = eye_height,
        },
        .started = started,
        .prepared = std::chrono::steady_clock::now(),
        .export_frame = frames.exported,
    });
}

namespace {

/// Decides which of the present thread's looks is a refresh of the emulated headset: every so
/// many looks, by the clock; or, with a host that says when its own display refreshes
/// (Core::Vr::Protocol::Refresh), in step with that display. A clock of its own drifts
/// against the display, and a title that draws one frame for every two refreshes then has
/// its frames arrive around the moment the host looks for them: shown for one refresh, then
/// three, instead of two each. In step, the refreshes are also put at the moment after the
/// display's that has the frames arrive well before the host looks.
class HeadsetRefreshClock {
public:
    using Clock = std::chrono::steady_clock;

    HeadsetRefreshClock(std::chrono::nanoseconds period_, u32 looks_per_refresh_)
        : period{period_}, looks_per_refresh{looks_per_refresh_} {}

    /// `halves` is how many half refreshes of the display (or of the rate the headset was
    /// made with, where there is no display to follow) one refresh of the headset lasts: two
    /// for a headset that refreshes when the display does, three for one that takes one and
    /// a half times as long, so that a title drawing a frame for every two of the headset's
    /// refreshes draws one for every three of the display's.
    bool IsRefresh(Clock::time_point now, u32 halves) {
        halves = std::max(halves, 1u);
        const auto display = Core::Vr::Runtime::Instance().GetDisplayRefresh();
        // A host that misses a few refreshes of its display is still followed: the display
        // went on refreshing. One that says nothing for longer has stopped showing pictures.
        if (display.sequence == 0 || now - display.time > Silence * period) {
            if (following) {
                following = false;
                look = 0;
                LOG_INFO(Lib_VideoOut, "The host's display is silent: the headset refreshes by "
                                       "the clock again");
            }
            if (++look < looks_per_refresh * halves / 2) {
                return false;
            }
            look = 0;
            return true;
        }
        const auto display_period =
            display.rate > 30.0f
                ? std::chrono::nanoseconds{static_cast<s64>(1e9 / display.rate)}
                : period;
        if (!following) {
            following = true;
            last_refresh = now;
            LOG_INFO(Lib_VideoOut, "The headset refreshes in step with the host's display, "
                                   "{:.1f} times a second", display.rate);
            return true;
        }
        host_period = display_period;
        delay = std::clamp(delay, std::chrono::nanoseconds{0}, host_period);
        // The refreshes of the display and the moments half-way between them, those the host
        // did not tell of included, shifted by the delay that has come round: the headset
        // refreshes at the latest of them once its refresh has lasted its time.
        const auto anchor = display.time + delay;
        const s64 behind = (now - anchor).count();
        const s64 length = host_period.count() / 2;
        const s64 steps = behind >= 0 ? behind / length : -((-behind + length - 1) / length);
        const auto instant = anchor + std::chrono::nanoseconds{steps * length};
        if ((instant - last_refresh).count() < length * halves - length / 2) {
            return false;
        }
        last_refresh = instant;
        Report(now, display.rate);
        return true;
    }

    /// A frame was handed to the host just now; `refreshes` is how many of the display's the
    /// title's frames take when they come at its pace.
    void NoteDelivery(Clock::time_point now, u32 refreshes) {
        if (!following) {
            return;
        }
        const auto interval = now - last_delivery;
        last_delivery = now;
        const auto display = Core::Vr::Runtime::Instance().GetDisplayRefresh();
        const double length = std::chrono::duration<double>(host_period).count();
        if (length <= 0.0 || now < display.time) {
            return;
        }
        const double phase =
            std::fmod(std::chrono::duration<double>(now - display.time).count(), length);
        // Only frames that come at the title's own pace say where they fall: one for two
        // refreshes of the headset. A title that cannot keep up delivers when it can.
        const double pace =
            std::chrono::duration<double>(interval).count() / (std::max(refreshes, 1u) * length);
        const bool paced = pace > 0.92 && pace < 1.08;
        ++deliveries;
        ++phases[std::min<size_t>(static_cast<size_t>(phase / length * phases.size()),
                                  phases.size() - 1)];
        if (!paced) {
            return;
        }
        ++paced_deliveries;
        phase_sum += phase;
        // Where a frame should arrive in the display's refresh: late enough to be fresh, with
        // room for one that takes a little longer than the last.
        static constexpr double Wanted = 0.55;
        static constexpr double Gain = 0.06;
        double error = Wanted * length - phase;
        error -= length * std::round(error / length);
        const auto moved =
            delay + std::chrono::nanoseconds{static_cast<s64>(error * Gain * 1e9)};
        delay = std::chrono::nanoseconds{((moved.count() % host_period.count()) +
                                           host_period.count()) %
                                          host_period.count()};
    }

private:
    void Report(Clock::time_point now, float rate) {
        if (report_time == Clock::time_point{}) {
            report_time = now;
        }
        if (now - report_time < std::chrono::seconds{10}) {
            return;
        }
        LOG_INFO(Lib_VideoOut,
                 "The headset refreshes {:.1f} ms after the host's display ({:.1f} Hz); {} of {} "
                 "frames came at the title's own pace, on average {:.1f} ms into a refresh (by "
                 "eighths of one: {})",
                 std::chrono::duration<double, std::milli>(delay).count(), rate, paced_deliveries,
                 deliveries, paced_deliveries != 0 ? phase_sum / paced_deliveries * 1e3 : 0.0,
                 fmt::join(phases, " "));
        report_time = now;
        deliveries = 0;
        paced_deliveries = 0;
        phase_sum = 0.0;
        phases.fill(0);
    }

    static constexpr s64 Silence = 10;

    const std::chrono::nanoseconds period;
    const u32 looks_per_refresh;
    u32 look{};
    std::array<u32, 8> phases{};
    bool following{};
    std::chrono::nanoseconds host_period{};
    std::chrono::nanoseconds delay{};
    Clock::time_point last_refresh;
    Clock::time_point last_delivery;
    Clock::time_point report_time;
    u32 deliveries{};
    u32 paced_deliveries{};
    double phase_sum{};
};

} // namespace

void VideoOutDriver::PresentThread(std::stop_token token) {
    // A connected headset drives the output at its panel's refresh rate instead of the TV's.
    const auto& vr = Core::Vr::Runtime::Instance();
    const u32 vblank_frequency = vr.IsHeadsetConnected() ? vr.GetConfig().refresh_rate
                                                         : EmulatorSettings.GetVblankFrequency();
    const std::chrono::nanoseconds vblank_period(1000000000 / vblank_frequency);
    u32 social_vblank_accumulator = 0;

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    // A frame for the headset is flipped as soon as the GPU has finished drawing it instead of
    // at the next refresh. The host shows it when its own display is due anyway, and the title
    // learns half a refresh earlier (on average) that it may go on: with frames that take about
    // as long as two refreshes, that decides whether the next one is ready after two or only
    // after three. For that the thread looks at the queue several times per refresh, without
    // ever waiting for the GPU: the refresh signals themselves stay on time.
    // SHADPS4_EARLY_FLIP=0 goes back to flipping at refreshes only.
    static constexpr u32 LooksPerRefresh = 8;
    const char* early_setting = std::getenv("SHADPS4_EARLY_FLIP");
    const bool early_flips =
        vr.IsHeadsetConnected() && !(early_setting != nullptr && early_setting[0] == '0');
    HeadsetRefreshClock refresh_clock{vblank_period, LooksPerRefresh};

    Common::AccurateTimer timer{early_flips ? vblank_period / LooksPerRefresh : vblank_period};

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };
    // The frame at the head of the queue, if it is one for the headset that the GPU is done
    // with.
    const auto receive_finished_hmd_frame = [this] -> Request {
        std::scoped_lock lk{mutex};
        // (The picture for the window and the one for a headset of the machine's own are
        // drawn together: one is finished when the other is.)
        if (!requests.empty() && requests.front().is_hmd &&
            presenter->IsFrameFinished(requests.front().frame != nullptr
                                           ? requests.front().frame
                                           : requests.front().export_frame)) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };
    const auto flip_hmd_frame = [this, &refresh_clock](const Request& request) {
        const auto now = std::chrono::steady_clock::now();
        Vulkan::FrameStats::Time(Vulkan::FrameStats::Stage::Queued, now - request.prepared);
        Flip(request);
        refresh_clock.NoteDelivery(now, Core::KnownTitle::FramePace());
        FRAME_END;
    };

    // How many refreshes of the display two of the headset's last.
    u32 pace = 2;

    while (!token.stop_requested()) {
        timer.Start();

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        if (early_flips) {
            if (const auto request = receive_finished_hmd_frame()) {
                flip_hmd_frame(request);
            }
            // Eight looks a refresh is also often enough to tell how busy the GPU is.
            Vulkan::FrameStats::GpuLook(presenter->IsGpuBusy());
            // A title that takes more than two refreshes of the display for a frame is on a
            // pace of its own: its frames are shown for two refreshes or for three as they
            // happen to fall, and what moves in the picture moves in jerks. Where it is known
            // how many refreshes a frame should be given (Core::KnownTitle::FramePace), the
            // headset refreshes that much more slowly instead: the title, which takes two
            // refreshes of the headset for a frame, then takes that many of the display.
            // (A title nothing is known about takes its two refreshes for a frame.)
            if (const u32 wanted = Core::KnownTitle::FramePace(); wanted != 0) {
                pace = wanted;
            }
            if (!refresh_clock.IsRefresh(std::chrono::steady_clock::now(), pace)) {
                timer.End();
                continue;
            }
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        ApplyDueLabelRetirement();
        const bool hmd_frame_waits = [this, early_flips] {
            std::scoped_lock lk{mutex};
            return early_flips && !requests.empty() && requests.front().is_hmd;
        }();
        if (hmd_frame_waits) {
            // Not finished yet: it is looked at again in a moment.
        } else if (vblank_status.count % (main_port.flip_rate + 1) == 0) {
            const auto request = receive_request();
            if (!request) {
                if (timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                if (dequeue_traces.fetch_add(1, std::memory_order_relaxed) < 32) {
                    LOG_INFO(Lib_VideoOut,
                             "BACHATA_FLIP_TRACE stage=queue_dequeue index={} arg={}", request.index,
                             request.flip_arg);
                }
                if (request.is_hmd) {
                    const auto now = std::chrono::steady_clock::now();
                    Vulkan::FrameStats::Time(Vulkan::FrameStats::Stage::Queued,
                                             now - request.prepared);
                    refresh_clock.NoteDelivery(now, pace);
                }
                Flip(request);
                FRAME_END;
            }
        }

        SignalVblank(main_port);

        // The TV keeps refreshing at its own rate next to the headset, however slowly that
        // refreshes at the moment.
        social_vblank_accumulator += SocialScreenRefreshRate * pace / 2;
        while (social_vblank_accumulator >= vblank_frequency) {
            social_vblank_accumulator -= vblank_frequency;
            if (social_port.is_open) {
                SignalVblank(social_port);
            }
        }

        // The headset reprojects once per refresh.
        Libraries::Hmd::OnVblank();

        timer.End();
    }
}

void VideoOutDriver::SignalVblank(VideoOutPort& port) {
    // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
    std::scoped_lock lock{port.vo_mutex};
    auto& vblank_status = port.vblank_status;

    // Trigger flip events for the port
    for (auto event : port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                                        (vblank_status.count << 16)));
        }
    }

    // Update vblank status
    vblank_status.count++;
    vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
    vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
    port.vblank_cv.notify_all();
}

void VideoOutDriver::ApplyDueLabelRetirement() {
    std::optional<s32> index;
    {
        std::scoped_lock lock{main_port.port_mutex};
        index = main_port.flip_labels.ConsumeDueRetirement(main_port.vblank_status.count);
    }
    if (!index.has_value()) {
        return;
    }
    main_port.buffer_labels[*index] = 0;
    main_port.SignalVoLabel();
    LOG_INFO(Lib_VideoOut, "retired scanout label index={}", *index);
}

} // namespace Libraries::VideoOut
