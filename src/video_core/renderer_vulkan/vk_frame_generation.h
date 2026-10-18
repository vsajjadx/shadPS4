// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <deque>
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

private:
    struct Target {
        VmaAllocation allocation{};
        vk::Image image{};
    };
    struct PendingFrame {
        u64 frame_id;
        u64 tick;
    };

    bool Recreate(vk::CommandBuffer cmdbuf, u32 width, u32 height);
    void Destroy();
    void DestroyTarget(Target& target);
    bool CreateTarget(Target& target, vk::Format format, vk::ImageUsageFlags usage, u32 width,
                      u32 height, const char* name);
    void Fail(const char* what);

    const Instance& instance;
    Scheduler& scheduler;
    FfxVkFsr3_3_1_6FrameGenerationContext* context{};
    Target color;
    Target interpolated;
    Target depth;
    Target motion;
    u32 width{};
    u32 height{};
    u64 frame_id{};
    bool reset{true};
    bool failed{false};
    std::deque<PendingFrame> pending;
    std::chrono::steady_clock::time_point last_generate{};
};

} // namespace Vulkan
