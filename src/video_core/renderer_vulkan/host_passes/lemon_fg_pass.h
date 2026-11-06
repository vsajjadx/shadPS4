// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Lemon Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace Vulkan::HostPasses {

/// Lemon frame generation: interpolates a frame between the two last presented frames from
/// the pictures alone. Block motion is searched on a 3-level luma pyramid at a quarter of the
/// output resolution (3DRS-style), screen-still content (HUD) is masked, and the generated frame
/// samples both frames along the motion projected to its time.
class LemonFgPass {
public:
    static constexpr u32 Levels = 3;

    struct Settings {
        /// Searches motion at half the output resolution instead of a quarter.
        bool high_quality{false};
        /// Keeps screen-still content (HUD) in place instead of moving it with the scene.
        bool hud_protection{true};
    };

    /// Whether the GPU can run the pass.
    static bool IsSupported(const Instance& instance);

    void Create(const Instance& instance, Scheduler& scheduler, Settings settings);

    /// Records the frames between the previous `frame` and this one into `outputs`, evenly
    /// spaced in time, and returns how many were written. `frame` has the given 32-bit RGBA or
    /// BGRA format, is in the general layout and stays there; the outputs have the same format
    /// and end in the shader read only layout when they were written.
    u32 Generate(vk::CommandBuffer cmdbuf, vk::Image frame, vk::Format frame_format,
                 std::span<const vk::Image> outputs, u32 width, u32 height);

    /// Searches the motion from the previous `frame` to this one and writes per-pixel motion
    /// vectors (offsets in pixels to where each pixel was in the previous frame) into an R16G16
    /// float image in the general layout. Returns false while there is no previous frame.
    bool EstimateMotion(vk::CommandBuffer cmdbuf, vk::Image frame, vk::Format frame_format,
                        u32 width, u32 height);

    vk::Image MotionImage() const {
        return motion.image;
    }

    vk::ImageView MotionView() const {
        return motion.view.get();
    }

    /// The next frame starts a new sequence (scene cut, resize, frame generation toggled).
    void Reset() {
        reset = true;
    }

private:
    struct Pass {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout pipeline_layout;
        vk::UniquePipeline pipeline;
        u32 push_size{};
    };

    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::UniqueImageView luma_view;
        vk::Extent2D size{};
    };

    Pass CreatePass(std::span<const u32> code, u32 sampled_count, u32 push_size,
                    const char* name) const;
    void CreateTarget(Target& target, vk::Format format, vk::ImageUsageFlags usage,
                      vk::Extent2D size, const char* name, bool luma_swizzle = false) const;
    void Recreate(vk::CommandBuffer cmdbuf, u32 width, u32 height, vk::Format frame_format);
    bool Track(vk::CommandBuffer cmdbuf, vk::Image frame, vk::Format frame_format, u32 width,
               u32 height, u32& slot);
    void Run(vk::CommandBuffer cmdbuf, const Pass& pass, std::span<const vk::ImageView> sampled,
             const Target& destination, const void* push, u32 groups_x, u32 groups_y) const;
    void BuildPyramid(vk::CommandBuffer cmdbuf, u32 slot);
    void SearchMotion(vk::CommandBuffer cmdbuf, u32 slot, bool use_temporal);
    void MaskStillContent(vk::CommandBuffer cmdbuf, u32 slot);
    void ClearStillMask(vk::CommandBuffer cmdbuf);
    void Interpolate(vk::CommandBuffer cmdbuf, u32 slot, float t);

    vk::Device device{};
    VmaAllocator allocator{};
    Scheduler* scheduler{};
    Settings settings{};

    vk::UniqueSampler sampler;
    Pass luma_pass;
    Pass search_pass;
    Pass median_pass;
    Pass project_pass;
    Pass still_pass;
    Pass generate_pass;
    Pass mvec_pass;

    vk::Extent2D extent{};
    vk::Format format{};
    vk::Extent2D still_size{};
    std::array<vk::Extent2D, Levels> level_size{};
    std::array<vk::Extent2D, Levels> grid{};

    std::array<Target, 2> frames;
    std::array<std::array<Target, Levels>, 2> pyramid;
    std::array<std::array<Target, 2>, Levels> fields;
    std::array<Target, 2> flows;
    Target offsets;
    Target offsets_filtered;
    std::array<Target, 2> still_masks;
    Target interpolated;
    Target motion;

    u64 frame_count{};
    u64 last_flow{~0ULL};
    bool reset{true};
};

} // namespace Vulkan::HostPasses
