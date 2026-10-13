// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <queue>
#include <source_location>

#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;
    /// How coarsely to shade each part of the pass (Foveation), null to shade every pixel.
    vk::ImageView shading_rate_view;

    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, sizeof(RenderState)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

/// What the guest asked of the GPU, counted between two presented frames. SHADPS4_FRAME_STATS=1
/// logs the averages every few seconds: they tell a frame made of a handful of passes from one
/// that restarts rendering to the same targets hundreds of times, which is what costs on a GPU
/// that renders in tiles. SHADPS4_FRAME_STATS=2 also lists the passes of one frame each time,
/// with what they were drawn to and which code ended them.
struct FrameStats {
    /// SHADPS4_DBG_CALL_TIMES=1: a measuring aid. How long the calls into the GPU driver that
    /// every draw makes take, kind by kind, in the frame statistics.
    enum class Call : u32 {
        Draw,
        PushConstants,
        PushDescriptors,
        VertexInput,
        VertexBuffers,
        IndexBuffer,
        DynamicState,
        BindPipeline,
        BeginRendering,
        EndRendering,
        Barrier,
        Submit,
        Count,
    };
    static bool CallTimesEnabled();

    /// Frames for the headset handed on so far (counted on the GPU thread).
    static void NoteHmdFrame() {
        hmd_frames.fetch_add(1, std::memory_order_relaxed);
    }
    static u32 HmdFrames() {
        return hmd_frames.load(std::memory_order_relaxed);
    }
    /// Times a call from its making to the end of the scope.
    class CallTimer {
    public:
        explicit CallTimer(Call call_) : call{call_}, timing{CallTimesEnabled()} {
            if (timing) {
                start = std::chrono::steady_clock::now();
            }
        }
        ~CallTimer();

    private:
        Call call;
        bool timing;
        std::chrono::steady_clock::time_point start{};
    };

    static bool Enabled();
    static void RenderPass(const RenderState& state);
    static void PassEnded(const std::source_location& where);
    static void Draw();
    static void Compute();
    static void Submit();
    /// Called once per presented frame.
    static void EndFrame();

    /// SHADPS4_DBG_AB=<seconds>: a measuring aid. Whatever asks AbOff is left out for that many
    /// seconds, then made again for as many, and so on, so that what it does shows in the frame
    /// statistics of one run, under the same heat and in the same scene. Every "frame stats"
    /// line says which way its frames were drawn ("A": made, "B": left out, "A+B": both).
    /// SHADPS4_DBG_AB_ONLY=<name>[,<name>...] limits that to what asks under those names.
    static bool AbOff(const char* name = "") {
        return ab_off.load(std::memory_order_relaxed) && AbConcerns(name);
    }
    static bool AbConcerns(const char* name);

    /// The way of a frame from the title to the display, for the averages in the log.
    enum class Stage : u32 {
        Submitting, ///< From the title's first command list until it handed the frame over.
        CatchUp,    ///< From there until the GPU thread had translated all of it.
        Translate, ///< Processor time the GPU thread spent turning the frame into host commands.
        Queued,    ///< From the frame being handed on until a display refresh took it up.
        GpuWait,   ///< From there until the GPU had finished drawing it.
        Latency,   ///< From the title's first command list of the frame until its flip.
        Count,
    };
    static void Time(Stage stage, std::chrono::nanoseconds time);

    /// How often the shortcuts of the draw path are taken, per frame.
    enum class Counter : u32 {
        ImageLookups,       ///< Texture cache lookups by address.
        ImageLookupsShort,  ///< ... answered from what was found before.
        ImageRegistrations, ///< Images added to or removed from the cache.
        PipelineBinds,      ///< Graphics pipeline binds asked for.
        PipelineBindsShort, ///< ... that found the pipeline bound already.
        StagePrograms,      ///< Shader stages whose permutation was looked for.
        StageProgramsShort, ///< ... that a draw before had come with the same for.
        PipelineLookups,    ///< Graphics pipelines looked for.
        PipelineLookupsShort, ///< ... that were the one the draw before got.
        EmptyDraws,         ///< Draws left out because they write nothing.
        Vertices,           ///< Vertices (or indices) drawn, instances counted.
        Count,
    };
    static void Add(Counter counter, u64 amount = 1);
    /// The title handed a command list over. The first one after a frame was taken marks the
    /// start of the next.
    static void GuestSubmit();
    /// When the frame now being flipped started, or the epoch if nothing was noted.
    static std::chrono::steady_clock::time_point TakeGuestSubmit();
    /// Processor time the calling thread has used so far.
    static std::chrono::nanoseconds ThreadTime();

    /// How busy the GPU is with what the title draws, whether statistics are logged or not.
    /// Somebody looks many times a second whether the GPU still has work of the title's
    /// before it (GpuLook); the share of looks that found it busy since the last asking is the
    /// answer (TakeGpuLoad, below zero when nobody looked).
    static void GpuLook(bool busy);
    static float TakeGpuLoad();

private:
    static inline std::atomic<bool> ab_off{false};
    static inline std::atomic<u32> hmd_frames{0};

public:
    /// The same looks, each counted by how fast the GPU's clock ran at the time, as a share of
    /// what they would have been at the fastest clock seen so far: how much of the GPU there
    /// would have been room for. A GPU with little to do slows its clock down and is busy for
    /// longer with the same work. Below zero where the clock cannot be read (or nobody looked).
    static float TakeGpuWork();
    /// The fastest the GPU's clock was seen to run so far (what TakeGpuWork counts by), in its
    /// own units; 0 where it cannot be read.
    static u32 FastestGpuClock();

    /// Processor time the GPU thread spent on a frame for the headset, noted once a frame
    /// whether statistics are logged or not; TakeGpuThreadTime is the average of those since
    /// the last asking, in seconds, 0 where none was noted or the system does not tell.
    static void NoteGpuThreadFrame(std::chrono::nanoseconds time);
    static double TakeGpuThreadTime();
};

/// True when SHADPS4_TRACE_SYNC is set: submissions and fence operations are then written to
/// stderr with a millisecond clock, to be read next to the GPU driver shim's own trace.
bool IsSyncTraceEnabled();
void TraceSync(const char* format, ...) __attribute__((format(printf, 1, 2)));

struct SubmitInfo {
    std::array<vk::Semaphore, 3> wait_semas;
    std::array<u64, 3> wait_ticks;
    std::array<vk::Semaphore, 3> signal_semas;
    std::array<u64, 3> signal_ticks;
    vk::Fence fence;
    u32 num_wait_semas;
    u32 num_signal_semas;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1) {
        wait_semas[num_wait_semas] = semaphore;
        wait_ticks[num_wait_semas++] = tick;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// Commits the dynamic state to the provided command buffer.
    void Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf);

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        std::memset(&dirty_state, 0xFF, sizeof(dirty_state));
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }
};

class Scheduler {
public:
    explicit Scheduler(const Instance& instance);
    ~Scheduler();

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush(SubmitInfo& info);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish();

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick);

    /// Attempts to execute operations whose tick the GPU has caught up with.
    void PopPendingOperations();

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state);

    /// Ends current rendering scope.
    void EndRendering(std::source_location where = std::source_location::current());

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Binds a graphics pipeline for the draws of the title, unless it is the one the last draw
    /// of this render pass was made with: a run of objects drawn with the same pipeline is
    /// common, and binding it again makes the driver send the GPU its whole state again.
    void BindGraphicsPipeline(vk::Pipeline pipeline) {
        FrameStats::Add(FrameStats::Counter::PipelineBinds);
        if (bound_pipeline != pipeline) {
            const FrameStats::CallTimer timer{FrameStats::Call::BindPipeline};
            current_cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
            bound_pipeline = pipeline;
        } else {
            FrameStats::Add(FrameStats::Counter::PipelineBindsShort);
        }
    }

    /// Counts a draw towards the next flush (see IsFlushDue).
    void NoteDraw() {
        ++draws_since_flush;
    }

    /// True when enough has been recorded, and a render pass has ended since, for the GPU to be
    /// given it now instead of at the end of the frame. A frame takes the GPU thread about as
    /// long to translate as it takes the GPU to draw: handing everything over at the end makes
    /// the two take turns, handing it over in parts lets them work at the same time.
    bool IsFlushDue() const {
        return flush_due;
    }

    /// The pipeline layout descriptors were last pushed for in the command buffer (a measuring
    /// aid's: SHADPS4_DBG_PUSH_ONCE).
    vk::PipelineLayout LastPushLayout() const {
        return last_push_layout;
    }
    void NotePushLayout(vk::PipelineLayout layout) {
        last_push_layout = layout;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the current command buffer.
    vk::CommandBuffer CommandBuffer() const {
        return current_cmdbuf;
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (master_semaphore.IsFree(tick)) {
            return true;
        }
        master_semaphore.Refresh();
        return master_semaphore.IsFree(tick);
    }

    /// Returns the master timeline semaphore.
    [[nodiscard]] MasterSemaphore* GetMasterSemaphore() noexcept {
        return &master_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        pending_ops.emplace(std::move(func), CurrentTick());
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

    static std::mutex submit_mutex;

private:
    void AllocateWorkerCommandBuffers();

    void SubmitExecution(SubmitInfo& info);

    void PriorityPendingOpsThread(std::stop_token stoken);

private:
    const Instance& instance;
    MasterSemaphore master_semaphore;
    CommandPool command_pool;
    DynamicState dynamic_state;
    vk::CommandBuffer current_cmdbuf;
    vk::PipelineLayout last_push_layout{};
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
    u32 draws_since_flush{};
    bool flush_due{};
    /// What BindGraphicsPipeline bound last in the current render pass. Other code binds
    /// pipelines of its own, always in a pass of its own: nothing is assumed across passes.
    vk::Pipeline bound_pipeline{};
    tracy::VkCtxScope* profiler_scope{};
};

} // namespace Vulkan
