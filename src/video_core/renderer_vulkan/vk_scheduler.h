// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <queue>

#include "common/assert.h"
#include "common/interval_set.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "vulkan/vulkan.hpp"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;
class CommandRecorder;

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

    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, sizeof(RenderState)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    std::array<vk::Semaphore, 4> wait_semas;
    std::array<u64, 4> wait_ticks;
    std::array<vk::Semaphore, 4> signal_semas;
    std::array<u64, 4> signal_ticks;
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
    void Commit(const Instance& instance, CommandRecorder cmdbuf);

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

using SessionFunc = Common::UniqueFunction<void>;
using SubmitFunc = Common::UniqueFunction<void, SubmitInfo&>;
using RenderingFunc = Common::UniqueFunction<void, CommandRecorder>;

/// Commands recorded for a recording thread, stored back to back with the arrays they point to.
class CommandChunk {
public:
    static constexpr std::size_t Size = 64_KB;

    /// A chunk larger than Size holds a command whose arrays need more room.
    explicit CommandChunk(std::size_t capacity_ = Size)
        : capacity{capacity_},
          data{std::make_unique<std::max_align_t[]>((capacity_ + sizeof(std::max_align_t) - 1) /
                                                    sizeof(std::max_align_t))} {}

    [[nodiscard]] std::size_t Capacity() const noexcept {
        return capacity;
    }

    /// Recording hands a chunk to its thread once this much of it is used.
    static constexpr std::size_t DispatchSize = 8_KB;
    /// Room kept free for the command itself after its arrays.
    static constexpr std::size_t CommandRoom = 512;

    [[nodiscard]] bool Empty() const noexcept {
        return first == nullptr;
    }

    [[nodiscard]] std::size_t Used() const noexcept {
        return offset;
    }

    [[nodiscard]] bool HasRoom(std::size_t bytes) const noexcept {
        return offset + bytes + CommandRoom <= capacity;
    }

    template <typename T>
    T* Store(const T* src, std::size_t count) {
        static_assert(std::is_trivially_copyable_v<T>);
        auto* dst = reinterpret_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
        std::memcpy(dst, src, sizeof(T) * count);
        return dst;
    }

    template <typename F>
    void Record(F&& func) {
        using Func = std::decay_t<F>;
        static_assert(std::is_trivially_destructible_v<Func>,
                      "Recorded commands are never destroyed");
        auto* command = new (Allocate(sizeof(TypedCommand<Func>), alignof(TypedCommand<Func>)))
            TypedCommand<Func>{std::forward<F>(func)};
        if (last) {
            last->next = command;
        } else {
            first = command;
        }
        last = command;
    }

    /// Runs the commands, each on the command buffer current when it runs.
    void Execute(const vk::CommandBuffer& cmdbuf) const {
        for (const Command* command = first; command; command = command->next) {
            command->Execute(cmdbuf);
        }
    }

    void Reset() noexcept {
        first = nullptr;
        last = nullptr;
        offset = 0;
    }

private:
    struct Command {
        virtual void Execute(const vk::CommandBuffer& cmdbuf) const = 0;
        Command* next{};
    };

    template <typename F>
    struct TypedCommand final : Command {
        explicit TypedCommand(F&& func_) : func{std::move(func_)} {}
        explicit TypedCommand(const F& func_) : func{func_} {}
        void Execute(const vk::CommandBuffer& cmdbuf) const override {
            func(cmdbuf);
        }
        F func;
    };

    void* Allocate(std::size_t bytes, std::size_t align) {
        offset = (offset + align - 1) & ~(align - 1);
        ASSERT_MSG(offset + bytes <= capacity, "Command chunk overflow");
        void* ptr = reinterpret_cast<u8*>(data.get()) + offset;
        offset += bytes;
        return ptr;
    }

    std::size_t capacity;
    std::unique_ptr<std::max_align_t[]> data;
    std::size_t offset{};
    Command* first{};
    Command* last{};
};

class Scheduler {
public:
    /// A deferred scheduler records its commands on its own thread, away from the caller.
    explicit Scheduler(const Instance& instance, bool deferred = false);
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
    void EndRendering();

    /// Starts a new session.
    void BeginSession();

    /// Sets a function to be called on every session finalization.
    void SetSessionCallback(SessionFunc&& on_session) {
        this->on_session = std::move(on_session);
    }

    /// Sets a function to be called on every scheduler submission.
    void SetSubmitCallback(SubmitFunc&& on_submit) {
        this->on_submit = std::move(on_submit);
    }

    /// Sets functions called with the command buffer after rendering begins and before it ends.
    void SetRenderingCallbacks(RenderingFunc&& on_begin, RenderingFunc&& on_end) {
        on_rendering_begin = std::move(on_begin);
        on_rendering_end = std::move(on_end);
    }

    /// Returns true while a rendering pass is open in the current command buffer.
    [[nodiscard]] bool IsRendering() const noexcept {
        return is_rendering;
    }

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the recorder of the current command buffer.
    CommandRecorder CommandBuffer();

    /// Returns the current command buffer itself, after every recorded command is in it. Commands
    /// recorded through CommandBuffer() afterwards go after anything written to it directly.
    vk::CommandBuffer DirectCommandBuffer();

    [[nodiscard]] bool IsDeferred() const noexcept {
        return deferred;
    }

    /// Waits until everything flushed so far is submitted. Another thread waiting for a tick of
    /// this scheduler while it holds the queue, like presentation, must not run before that.
    void WaitSubmitted() {
        if (deferred) {
            Drain();
        }
    }

    /// Makes room for a command and arrays of the given total size.
    void Reserve(std::size_t bytes) {
        if (deferred && !chunk->HasRoom(bytes)) {
            DispatchChunk();
            if (!chunk->HasRoom(bytes)) {
                chunk = std::make_unique<CommandChunk>(bytes + CommandChunk::Size);
            }
        }
    }

    /// Returns an array that stays valid until the command recorded next has executed.
    template <typename T>
    const T* Store(const T* data, std::size_t count) {
        if (!deferred || count == 0 || data == nullptr) {
            return data;
        }
        arrays_stored = true;
        return chunk->Store(data, count);
    }

    template <typename F>
    void Record(F&& func) {
        if (!deferred) {
            func(exec_cmdbuf);
            return;
        }
        // A command whose arrays are stored already has its room reserved with them. Making room
        // again could hand the chunk holding the arrays to the recording thread, which recycles it
        // while the command, recorded into the next chunk, still points into it.
        if (!std::exchange(arrays_stored, false)) {
            Reserve(0);
        }
        chunk->Record(std::forward<F>(func));
        if (chunk->Used() >= CommandChunk::DispatchSize) {
            DispatchChunk();
        }
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return work_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (work_semaphore.IsFree(tick)) {
            return true;
        }
        work_semaphore.Refresh();
        return work_semaphore.IsFree(tick);
    }

    /// Returns the scheduler timeline semaphore.
    [[nodiscard]] Semaphore* GetWorkSemaphore() noexcept {
        return &work_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
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
    void EndSession();

    void SubmitExecution(SubmitInfo& info);

    /// Submits the command buffers recorded since the last submission.
    void SubmitRecorded(SubmitInfo info);

    void PriorityPendingOpsThread(std::stop_token stoken);

    void DispatchChunk();

    /// Waits until the recording thread has executed every dispatched command.
    void Drain();

    void RecordThread(std::stop_token stoken);

private:
    const Instance& instance;
    Semaphore work_semaphore;
    CommandPool command_pool;
    DynamicState dynamic_state;
    SessionFunc on_session{};
    SubmitFunc on_submit{};
    RenderingFunc on_rendering_begin{};
    RenderingFunc on_rendering_end{};
    bool deferred{};
    bool has_session{};
    // Owned by whatever executes the commands: the recording thread, or the caller when the
    // scheduler is not deferred or has been drained.
    vk::CommandBuffer exec_cmdbuf{};
    std::vector<vk::CommandBuffer> exec_cmdbufs;
    std::unique_ptr<CommandChunk> chunk;
    bool arrays_stored{};
    std::deque<std::unique_ptr<CommandChunk>> chunk_queue;
    std::vector<std::unique_ptr<CommandChunk>> free_chunks;
    std::mutex chunk_mutex;
    std::condition_variable_any chunk_cv;
    std::condition_variable_any idle_cv;
    bool recording{};
    std::jthread record_thread;
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
    tracy::VkCtxScope* profiler_scope{};
};

// windows.h defines MemoryBarrier as a macro, which would rename vk::MemoryBarrier.
#pragma push_macro("MemoryBarrier")
#undef MemoryBarrier

/// Records into a scheduler's command buffer, with the methods of vk::CommandBuffer that the
/// renderer uses. Arrays are copied, so they only need to live for the call.
class CommandRecorder {
public:
    explicit CommandRecorder(Scheduler& scheduler_) : scheduler{&scheduler_} {}

    /// The command buffer itself, once every command recorded before is in it.
    operator vk::CommandBuffer() const {
        return scheduler->DirectCommandBuffer();
    }

    void bindPipeline(vk::PipelineBindPoint bind_point, vk::Pipeline pipeline) const {
        Record([=](vk::CommandBuffer cmd) { cmd.bindPipeline(bind_point, pipeline); });
    }

    void pipelineBarrier2(const vk::DependencyInfo& info) const {
        Reserve(info.memoryBarrierCount * sizeof(vk::MemoryBarrier2) +
                info.bufferMemoryBarrierCount * sizeof(vk::BufferMemoryBarrier2) +
                info.imageMemoryBarrierCount * sizeof(vk::ImageMemoryBarrier2));
        vk::DependencyInfo copy = info;
        copy.pMemoryBarriers = Store(info.pMemoryBarriers, info.memoryBarrierCount);
        copy.pBufferMemoryBarriers =
            Store(info.pBufferMemoryBarriers, info.bufferMemoryBarrierCount);
        copy.pImageMemoryBarriers = Store(info.pImageMemoryBarriers, info.imageMemoryBarrierCount);
        Record([=](vk::CommandBuffer cmd) { cmd.pipelineBarrier2(copy); });
    }

    void pipelineBarrier(vk::PipelineStageFlags src, vk::PipelineStageFlags dst,
                         vk::DependencyFlags flags,
                         vk::ArrayProxy<const vk::MemoryBarrier> const& memory,
                         vk::ArrayProxy<const vk::BufferMemoryBarrier> const& buffers,
                         vk::ArrayProxy<const vk::ImageMemoryBarrier> const& images) const {
        Reserve(memory.size() * sizeof(vk::MemoryBarrier) +
                buffers.size() * sizeof(vk::BufferMemoryBarrier) +
                images.size() * sizeof(vk::ImageMemoryBarrier));
        const auto* m = Store(memory.data(), memory.size());
        const auto* b = Store(buffers.data(), buffers.size());
        const auto* i = Store(images.data(), images.size());
        const u32 nm = memory.size();
        const u32 nb = buffers.size();
        const u32 ni = images.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier(src, dst, flags, nm, m, nb, b, ni, i);
        });
    }

    void pushDescriptorSetKHR(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
                              vk::ArrayProxy<const vk::WriteDescriptorSet> const& writes) const {
        std::size_t bytes = writes.size() * sizeof(vk::WriteDescriptorSet);
        for (const auto& write : writes) {
            bytes += write.descriptorCount * sizeof(vk::DescriptorImageInfo) + 16;
        }
        Reserve(bytes);
        const u32 count = writes.size();
        const vk::WriteDescriptorSet* stored = Store(writes.data(), count);
        if (scheduler->IsDeferred()) {
            auto* patched = const_cast<vk::WriteDescriptorSet*>(stored);
            for (u32 i = 0; i < count; ++i) {
                const u32 n = patched[i].descriptorCount;
                patched[i].pImageInfo = Store(patched[i].pImageInfo, n);
                patched[i].pBufferInfo = Store(patched[i].pBufferInfo, n);
                patched[i].pTexelBufferView = Store(patched[i].pTexelBufferView, n);
            }
        }
        Record([=](vk::CommandBuffer cmd) {
            cmd.pushDescriptorSetKHR(bind_point, layout, set, count, stored);
        });
    }

    void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 offset, u32 size,
                       const void* values) const {
        Reserve(size);
        const auto* data = Store(static_cast<const u8*>(values), size);
        Record(
            [=](vk::CommandBuffer cmd) { cmd.pushConstants(layout, stages, offset, size, data); });
    }

    void bindDescriptorSets(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 first,
                            vk::ArrayProxy<const vk::DescriptorSet> const& sets,
                            vk::ArrayProxy<const u32> const& offsets) const {
        Reserve(sets.size() * sizeof(vk::DescriptorSet) + offsets.size() * sizeof(u32));
        const auto* s = Store(sets.data(), sets.size());
        const auto* o = Store(offsets.data(), offsets.size());
        const u32 ns = sets.size();
        const u32 no = offsets.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.bindDescriptorSets(bind_point, layout, first, ns, s, no, o);
        });
    }

    void dispatch(u32 x, u32 y, u32 z) const {
        Record([=](vk::CommandBuffer cmd) { cmd.dispatch(x, y, z); });
    }

    void dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) const {
        Record([=](vk::CommandBuffer cmd) { cmd.dispatchIndirect(buffer, offset); });
    }

    void draw(u32 vertices, u32 instances, u32 first_vertex, u32 first_instance) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.draw(vertices, instances, first_vertex, first_instance);
        });
    }

    void drawIndexed(u32 indices, u32 instances, u32 first_index, s32 vertex_offset,
                     u32 first_instance) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.drawIndexed(indices, instances, first_index, vertex_offset, first_instance);
        });
    }

    void drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 count, u32 stride) const {
        Record([=](vk::CommandBuffer cmd) { cmd.drawIndirect(buffer, offset, count, stride); });
    }

    void drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 count,
                             u32 stride) const {
        Record(
            [=](vk::CommandBuffer cmd) { cmd.drawIndexedIndirect(buffer, offset, count, stride); });
    }

    void drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                           vk::DeviceSize count_offset, u32 max_count, u32 stride) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.drawIndirectCount(buffer, offset, count_buffer, count_offset, max_count, stride);
        });
    }

    void drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                                  vk::DeviceSize count_offset, u32 max_count, u32 stride) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.drawIndexedIndirectCount(buffer, offset, count_buffer, count_offset, max_count,
                                         stride);
        });
    }

    void bindVertexBuffers(u32 first, u32 count, const vk::Buffer* buffers,
                           const vk::DeviceSize* offsets) const {
        Reserve(count * (sizeof(vk::Buffer) + sizeof(vk::DeviceSize)));
        const auto* b = Store(buffers, count);
        const auto* o = Store(offsets, count);
        Record([=](vk::CommandBuffer cmd) { cmd.bindVertexBuffers(first, count, b, o); });
    }

    void bindVertexBuffers2(u32 first, u32 count, const vk::Buffer* buffers,
                            const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                            const vk::DeviceSize* strides) const {
        Reserve(count * (sizeof(vk::Buffer) + 3 * sizeof(vk::DeviceSize)));
        const auto* b = Store(buffers, count);
        const auto* o = Store(offsets, count);
        const auto* s = Store(sizes, count);
        const auto* t = Store(strides, count);
        Record([=](vk::CommandBuffer cmd) { cmd.bindVertexBuffers2(first, count, b, o, s, t); });
    }

    void bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType type) const {
        Record([=](vk::CommandBuffer cmd) { cmd.bindIndexBuffer(buffer, offset, type); });
    }

    void bindIndexBuffer2KHR(vk::Buffer buffer, vk::DeviceSize offset, vk::DeviceSize size,
                             vk::IndexType type) const {
        Record([=](vk::CommandBuffer cmd) { cmd.bindIndexBuffer2KHR(buffer, offset, size, type); });
    }

    void setVertexInputEXT(
        vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> const& bindings,
        vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> const& attributes) const {
        Reserve(bindings.size() * sizeof(vk::VertexInputBindingDescription2EXT) +
                attributes.size() * sizeof(vk::VertexInputAttributeDescription2EXT));
        const auto* b = Store(bindings.data(), bindings.size());
        const auto* a = Store(attributes.data(), attributes.size());
        const u32 nb = bindings.size();
        const u32 na = attributes.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setVertexInputEXT(nb, b, na, a); });
    }

    void setViewport(u32 first, vk::ArrayProxy<const vk::Viewport> const& viewports) const {
        Reserve(viewports.size() * sizeof(vk::Viewport));
        const auto* v = Store(viewports.data(), viewports.size());
        const u32 n = viewports.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setViewport(first, n, v); });
    }

    void setScissor(u32 first, vk::ArrayProxy<const vk::Rect2D> const& scissors) const {
        Reserve(scissors.size() * sizeof(vk::Rect2D));
        const auto* s = Store(scissors.data(), scissors.size());
        const u32 n = scissors.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setScissor(first, n, s); });
    }

    void setViewportWithCount(vk::ArrayProxy<const vk::Viewport> const& viewports) const {
        Reserve(viewports.size() * sizeof(vk::Viewport));
        const auto* v = Store(viewports.data(), viewports.size());
        const u32 n = viewports.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setViewportWithCount(n, v); });
    }

    void setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> const& scissors) const {
        Reserve(scissors.size() * sizeof(vk::Rect2D));
        const auto* s = Store(scissors.data(), scissors.size());
        const u32 n = scissors.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setScissorWithCount(n, s); });
    }

    void setColorWriteMaskEXT(u32 first,
                              vk::ArrayProxy<const vk::ColorComponentFlags> const& masks) const {
        Reserve(masks.size() * sizeof(vk::ColorComponentFlags));
        const auto* m = Store(masks.data(), masks.size());
        const u32 n = masks.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setColorWriteMaskEXT(first, n, m); });
    }

    void setBlendConstants(const float constants[4]) const {
        const std::array<float, 4> c{constants[0], constants[1], constants[2], constants[3]};
        Record([=](vk::CommandBuffer cmd) { cmd.setBlendConstants(c.data()); });
    }

    void setDepthTestEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthTestEnable(enable); });
    }

    void setDepthWriteEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthWriteEnable(enable); });
    }

    void setDepthCompareOp(vk::CompareOp op) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthCompareOp(op); });
    }

    void setDepthBoundsTestEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthBoundsTestEnable(enable); });
    }

    void setDepthBounds(float min, float max) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthBounds(min, max); });
    }

    void setDepthBiasEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthBiasEnable(enable); });
    }

    void setDepthBias(float constant, float clamp, float slope) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthBias(constant, clamp, slope); });
    }

    void setStencilTestEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setStencilTestEnable(enable); });
    }

    void setStencilOp(vk::StencilFaceFlags faces, vk::StencilOp fail, vk::StencilOp pass,
                      vk::StencilOp depth_fail, vk::CompareOp compare) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.setStencilOp(faces, fail, pass, depth_fail, compare);
        });
    }

    void setStencilReference(vk::StencilFaceFlags faces, u32 value) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setStencilReference(faces, value); });
    }

    void setStencilWriteMask(vk::StencilFaceFlags faces, u32 value) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setStencilWriteMask(faces, value); });
    }

    void setStencilCompareMask(vk::StencilFaceFlags faces, u32 value) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setStencilCompareMask(faces, value); });
    }

    void setPrimitiveRestartEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setPrimitiveRestartEnable(enable); });
    }

    void setRasterizerDiscardEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setRasterizerDiscardEnable(enable); });
    }

    void setLineWidth(float width) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setLineWidth(width); });
    }

    void setFrontFace(vk::FrontFace face) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setFrontFace(face); });
    }

    void setCullMode(vk::CullModeFlags mode) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setCullMode(mode); });
    }

    void setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlags aspects) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setAttachmentFeedbackLoopEnableEXT(aspects); });
    }

    void beginRendering(const vk::RenderingInfo& info) const {
        Reserve((info.colorAttachmentCount + 2) * sizeof(vk::RenderingAttachmentInfo));
        vk::RenderingInfo copy = info;
        copy.pColorAttachments = Store(info.pColorAttachments, info.colorAttachmentCount);
        copy.pDepthAttachment = Store(info.pDepthAttachment, info.pDepthAttachment ? 1 : 0);
        copy.pStencilAttachment = Store(info.pStencilAttachment, info.pStencilAttachment ? 1 : 0);
        Record([=](vk::CommandBuffer cmd) { cmd.beginRendering(copy); });
    }

    void endRendering() const {
        Record([](vk::CommandBuffer cmd) { cmd.endRendering(); });
    }

    void beginQuery(vk::QueryPool pool, u32 query, vk::QueryControlFlags flags) const {
        Record([=](vk::CommandBuffer cmd) { cmd.beginQuery(pool, query, flags); });
    }

    void endQuery(vk::QueryPool pool, u32 query) const {
        Record([=](vk::CommandBuffer cmd) { cmd.endQuery(pool, query); });
    }

    void copyBuffer(vk::Buffer src, vk::Buffer dst,
                    vk::ArrayProxy<const vk::BufferCopy> const& regions) const {
        Reserve(regions.size() * sizeof(vk::BufferCopy));
        const auto* r = Store(regions.data(), regions.size());
        const u32 n = regions.size();
        Record([=](vk::CommandBuffer cmd) { cmd.copyBuffer(src, dst, n, r); });
    }

    void fillBuffer(vk::Buffer dst, vk::DeviceSize offset, vk::DeviceSize size, u32 value) const {
        Record([=](vk::CommandBuffer cmd) { cmd.fillBuffer(dst, offset, size, value); });
    }

    void updateBuffer(vk::Buffer dst, vk::DeviceSize offset, vk::DeviceSize size,
                      const void* values) const {
        Reserve(size);
        const auto* data = Store(static_cast<const u8*>(values), size);
        Record([=](vk::CommandBuffer cmd) { cmd.updateBuffer(dst, offset, size, data); });
    }

    void copyImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                   vk::ImageLayout dst_layout,
                   vk::ArrayProxy<const vk::ImageCopy> const& regions) const {
        Reserve(regions.size() * sizeof(vk::ImageCopy));
        const auto* r = Store(regions.data(), regions.size());
        const u32 n = regions.size();
        Record(
            [=](vk::CommandBuffer cmd) { cmd.copyImage(src, src_layout, dst, dst_layout, n, r); });
    }

    void copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout layout,
                           vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
        Reserve(regions.size() * sizeof(vk::BufferImageCopy));
        const auto* r = Store(regions.data(), regions.size());
        const u32 n = regions.size();
        Record([=](vk::CommandBuffer cmd) { cmd.copyBufferToImage(src, dst, layout, n, r); });
    }

    void copyImageToBuffer(vk::Image src, vk::ImageLayout layout, vk::Buffer dst,
                           vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
        Reserve(regions.size() * sizeof(vk::BufferImageCopy));
        const auto* r = Store(regions.data(), regions.size());
        const u32 n = regions.size();
        Record([=](vk::CommandBuffer cmd) { cmd.copyImageToBuffer(src, layout, dst, n, r); });
    }

    void resolveImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                      vk::ImageLayout dst_layout,
                      vk::ArrayProxy<const vk::ImageResolve> const& regions) const {
        Reserve(regions.size() * sizeof(vk::ImageResolve));
        const auto* r = Store(regions.data(), regions.size());
        const u32 n = regions.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.resolveImage(src, src_layout, dst, dst_layout, n, r);
        });
    }

    void blitImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                   vk::ImageLayout dst_layout, vk::ArrayProxy<const vk::ImageBlit> const& regions,
                   vk::Filter filter) const {
        Reserve(regions.size() * sizeof(vk::ImageBlit));
        const auto* r = Store(regions.data(), regions.size());
        const u32 n = regions.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.blitImage(src, src_layout, dst, dst_layout, n, r, filter);
        });
    }

    void clearColorImage(vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue& color,
                         vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const {
        Reserve(ranges.size() * sizeof(vk::ImageSubresourceRange));
        const auto* r = Store(ranges.data(), ranges.size());
        const u32 n = ranges.size();
        Record([=](vk::CommandBuffer cmd) { cmd.clearColorImage(image, layout, &color, n, r); });
    }

    void beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
        const auto copy = StoreLabel(label);
        Record([=](vk::CommandBuffer cmd) { cmd.beginDebugUtilsLabelEXT(copy); });
    }

    void insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
        const auto copy = StoreLabel(label);
        Record([=](vk::CommandBuffer cmd) { cmd.insertDebugUtilsLabelEXT(copy); });
    }

    void endDebugUtilsLabelEXT() const {
        Record([](vk::CommandBuffer cmd) { cmd.endDebugUtilsLabelEXT(); });
    }

private:
    void Reserve(std::size_t bytes) const {
        scheduler->Reserve(bytes);
    }

    template <typename T>
    const T* Store(const T* data, std::size_t count) const {
        return scheduler->Store(data, count);
    }

    template <typename F>
    void Record(F&& func) const {
        scheduler->Record(std::forward<F>(func));
    }

    vk::DebugUtilsLabelEXT StoreLabel(const vk::DebugUtilsLabelEXT& label) const {
        const std::size_t length = label.pLabelName ? std::strlen(label.pLabelName) + 1 : 0;
        Reserve(length);
        vk::DebugUtilsLabelEXT copy = label;
        copy.pNext = nullptr;
        copy.pLabelName = Store(label.pLabelName, length);
        return copy;
    }

    Scheduler* scheduler;
};

#pragma pop_macro("MemoryBarrier")

inline CommandRecorder Scheduler::CommandBuffer() {
    return CommandRecorder{*this};
}

} // namespace Vulkan
