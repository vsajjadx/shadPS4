// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

struct FfxVkFsr3_3_1_6FrameGenerationContext;
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

class Instance;
class Scheduler;

namespace FrameGeneration {
void SetEnabled(bool enabled);
bool IsEnabled();
bool IsAvailable();
} // namespace FrameGeneration

/// Interpolates a frame between the two last presented frames with FSR 3.1 frame generation.
class FrameGenerator {
public:
    explicit FrameGenerator(const Instance& instance, Scheduler& scheduler);
    ~FrameGenerator();

    /// Records the frame between the previous `frame` and this one into `output`. Both images
    /// have the presenter's 32-bit frame format and the given size; `frame` is in the general
    /// layout and stays there, `output` ends in the shader read only layout when it was written.
    bool Generate(vk::CommandBuffer cmdbuf, vk::Image frame, vk::Image output, u32 width,
                  u32 height);

    /// The next frame starts a new sequence (scene cut, resize, frame generation toggled).
    void Reset() {
        reset = true;
    }

    /// Identifies the signature recorded by the last `Generate` call, or zero when none was.
    u64 SignatureId() const {
        return signature_id;
    }

    /// Whether the frame with the given signature id differs so much from the previous one that
    /// interpolating between them would blend two unrelated shots. Needs the frame's GPU work to
    /// be complete.
    bool IsSceneCut(u64 id);

private:
    struct Target {
        VmaAllocation allocation{};
        vk::Image image{};
    };
    struct PendingFrame {
        u64 frame_id;
        u64 tick;
    };
    static constexpr u32 SignatureSlots = 8;
    struct SignatureSlot {
        VmaAllocation allocation{};
        vk::Buffer buffer{};
        void* mapped{};
        u64 frame_id{};
    };

    bool Recreate(vk::CommandBuffer cmdbuf, u32 width, u32 height);
    void Destroy();
    void DestroyTarget(Target& target);
    bool CreateTarget(Target& target, vk::Format format, vk::ImageUsageFlags usage, u32 width,
                      u32 height, const char* name, u32 mip_levels = 1);
    bool CreateSignature(u32 frame_width, u32 frame_height);
    void DestroySignature();
    void RecordSignature(vk::CommandBuffer cmdbuf, vk::Image frame, u32 frame_width,
                         u32 frame_height, u64 id);
    void Fail(const char* what);

    const Instance& instance;
    Scheduler& scheduler;
    FfxVkFsr3_3_1_6FrameGenerationContext* context{};
    Target color;
    Target interpolated;
    Target depth;
    Target motion;
    Target signature;
    u32 signature_levels{};
    u32 signature_width{};
    u32 signature_height{};
    std::array<SignatureSlot, SignatureSlots> signature_slots;
    std::vector<u8> previous_signature;
    u64 previous_signature_id{};
    u64 signature_id{};
    std::mutex signature_mutex;
    u32 width{};
    u32 height{};
    u64 frame_id{};
    std::atomic_bool reset{true};
    bool failed{false};
    std::deque<PendingFrame> pending;
    std::chrono::steady_clock::time_point last_generate{};
};

} // namespace Vulkan
