// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <span>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>
#include "ffx_vk_fsr3_3_1_5_bridge.h"

namespace Vulkan {

namespace FrameGeneration {

static std::atomic_bool enabled{false};
static std::atomic_bool available{true};

void SetEnabled(bool enable) {
    enabled = enable;
}

bool IsEnabled() {
    return enabled && available;
}

bool IsAvailable() {
    return available;
}

} // namespace FrameGeneration

namespace {

constexpr vk::Format ColorFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::Format DepthFormat = vk::Format::eR32Sfloat;
constexpr vk::Format MotionFormat = vk::Format::eR16G16Sfloat;

constexpr vk::ImageUsageFlags ColorUsage = vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eTransferDst;
constexpr vk::ImageUsageFlags InterpolatedUsage = vk::ImageUsageFlagBits::eStorage |
                                                  vk::ImageUsageFlagBits::eSampled |
                                                  vk::ImageUsageFlagBits::eTransferSrc;
constexpr vk::ImageUsageFlags InputUsage = vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eTransferDst;

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

constexpr vk::ImageSubresourceLayers ColorLayers{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .mipLevel = 0,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

FfxVkFsr3_3_1_6FrameGenerationImage MakeImage(vk::Image image, vk::Format format, u32 width,
                                              u32 height, vk::ImageUsageFlags usage) {
    return FfxVkFsr3_3_1_6FrameGenerationImage{
        .image = image,
        .format = static_cast<VkFormat>(format),
        .width = width,
        .height = height,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
        .usage = static_cast<VkImageUsageFlags>(usage),
    };
}

vk::ImageMemoryBarrier2 Barrier(vk::Image image, vk::PipelineStageFlags2 src_stage,
                                vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                                vk::AccessFlags2 dst_access, vk::ImageLayout old_layout,
                                vk::ImageLayout new_layout) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

void PipelineBarriers(vk::CommandBuffer cmdbuf, std::span<const vk::ImageMemoryBarrier2> barriers) {
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
}

void CopyImage(vk::CommandBuffer cmdbuf, vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
               vk::ImageLayout dst_layout, u32 width, u32 height) {
    const vk::ImageCopy region{
        .srcSubresource = ColorLayers,
        .srcOffset = {0, 0, 0},
        .dstSubresource = ColorLayers,
        .dstOffset = {0, 0, 0},
        .extent = {width, height, 1},
    };
    cmdbuf.copyImage(src, src_layout, dst, dst_layout, region);
}

} // Anonymous namespace

FrameGenerator::FrameGenerator(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

FrameGenerator::~FrameGenerator() {
    Destroy();
}

void FrameGenerator::Fail(const char* what) {
    LOG_ERROR(Render_Vulkan, "FSR frame generation disabled: {}", what);
    failed = true;
    FrameGeneration::available = false;
}

bool FrameGenerator::CreateTarget(Target& target, vk::Format format, vk::ImageUsageFlags usage,
                                  u32 target_width, u32 target_height, const char* name) {
    const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {target_width, target_height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const VmaAllocationCreateInfo alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage image{};
    if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                       &target.allocation, nullptr) != VK_SUCCESS) {
        target = {};
        return false;
    }
    target.image = vk::Image{image};
    SetObjectName(instance.GetDevice(), target.image, "Frame generation {}", name);
    return true;
}

void FrameGenerator::DestroyTarget(Target& target) {
    if (target.image) {
        vmaDestroyImage(instance.GetAllocator(), static_cast<VkImage>(target.image),
                        target.allocation);
    }
    target = {};
}

void FrameGenerator::Destroy() {
    if (context || color.image) {
        const u64 tick = scheduler.CurrentTick();
        if (tick > 1) {
            scheduler.Wait(tick - 1);
        }
    }
    if (context) {
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context, frame_id);
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
        context = nullptr;
    }
    pending.clear();
    DestroyTarget(color);
    DestroyTarget(interpolated);
    DestroyTarget(depth);
    DestroyTarget(motion);
    width = 0;
    height = 0;
}

bool FrameGenerator::Recreate(vk::CommandBuffer cmdbuf, u32 new_width, u32 new_height) {
    Destroy();
    if (!CreateTarget(color, ColorFormat, ColorUsage, new_width, new_height, "color") ||
        !CreateTarget(interpolated, ColorFormat, InterpolatedUsage, new_width, new_height,
                      "output") ||
        !CreateTarget(depth, DepthFormat, InputUsage, new_width, new_height, "depth") ||
        !CreateTarget(motion, MotionFormat, InputUsage, new_width, new_height, "motion")) {
        Destroy();
        Fail("out of video memory for its images");
        return false;
    }

    const FfxVkFsr3_3_1_6FrameGenerationCreateInfo create_info{
        .physicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice()),
        .device = static_cast<VkDevice>(instance.GetDevice()),
        .maxRenderWidth = new_width,
        .maxRenderHeight = new_height,
        .displayWidth = new_width,
        .displayHeight = new_height,
        .colorFormat = static_cast<VkFormat>(ColorFormat),
    };
    const auto result = ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create_info, &context);
    if (result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        context = nullptr;
        Destroy();
        LOG_ERROR(Render_Vulkan, "FSR frame generation context creation failed: {}",
                  static_cast<int>(result));
        Fail("its context could not be created on this GPU");
        return false;
    }
    width = new_width;
    height = new_height;
    reset = true;

    // The game provides no depth or motion, so the interpolation follows the optical flow of
    // the presented frames alone.
    const std::array to_clear{
        Barrier(depth.image, vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral),
        Barrier(motion.image, vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral),
    };
    PipelineBarriers(cmdbuf, to_clear);
    const vk::ClearColorValue zero{};
    cmdbuf.clearColorImage(depth.image, vk::ImageLayout::eGeneral, zero, ColorRange);
    cmdbuf.clearColorImage(motion.image, vk::ImageLayout::eGeneral, zero, ColorRange);
    const std::array cleared{
        Barrier(depth.image, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eAllCommands,
                vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral,
                vk::ImageLayout::eGeneral),
        Barrier(motion.image, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eAllCommands,
                vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral,
                vk::ImageLayout::eGeneral),
    };
    PipelineBarriers(cmdbuf, cleared);

    FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
    ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(context, &usage);
    LOG_INFO(Render_Vulkan, "FSR 3.1 frame generation ready at {}x{} ({:.1f} MB)", width, height,
             static_cast<double>(usage.totalUsageInBytes) / 1e6);
    return true;
}

bool FrameGenerator::Generate(vk::CommandBuffer cmdbuf, vk::Image frame, vk::Image output,
                              u32 frame_width, u32 frame_height) {
    if (failed || frame_width == 0 || frame_height == 0) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_generate).count();
    last_generate = now;
    if (!(frame_ms >= 1.0f && frame_ms <= 200.0f)) {
        frame_ms = 33.3f;
        reset = true;
    }

    while (context && !pending.empty() && scheduler.IsFree(pending.front().tick)) {
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context, pending.front().frame_id);
        pending.pop_front();
    }

    if (!context || width != frame_width || height != frame_height) {
        if (!Recreate(cmdbuf, frame_width, frame_height)) {
            return false;
        }
    }

    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    using Layout = vk::ImageLayout;

    const std::array to_copy{
        Barrier(frame, Stage::eAllCommands,
                Access::eColorAttachmentWrite | Access::eShaderWrite | Access::eTransferWrite,
                Stage::eTransfer, Access::eTransferRead, Layout::eGeneral, Layout::eGeneral),
        Barrier(color.image, Stage::eAllCommands, Access::eNone, Stage::eTransfer,
                Access::eTransferWrite, Layout::eUndefined, Layout::eGeneral),
    };
    PipelineBarriers(cmdbuf, to_copy);
    CopyImage(cmdbuf, frame, Layout::eGeneral, color.image, Layout::eGeneral, width, height);

    const std::array to_interpolate{
        Barrier(color.image, Stage::eTransfer, Access::eTransferWrite, Stage::eAllCommands,
                Access::eShaderRead, Layout::eGeneral, Layout::eGeneral),
        Barrier(interpolated.image, Stage::eAllCommands, Access::eNone, Stage::eAllCommands,
                Access::eShaderWrite | Access::eShaderRead, Layout::eUndefined, Layout::eGeneral),
    };
    PipelineBarriers(cmdbuf, to_interpolate);

    const bool was_reset = reset;
    const u64 id = ++frame_id;
    const FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{
        .commandBuffer = static_cast<VkCommandBuffer>(cmdbuf),
        .color = MakeImage(color.image, ColorFormat, width, height, ColorUsage),
        .depth = MakeImage(depth.image, DepthFormat, width, height, InputUsage),
        .motionVectors = MakeImage(motion.image, MotionFormat, width, height, InputUsage),
        .renderWidth = width,
        .renderHeight = height,
        .jitterOffsetX = 0.0f,
        .jitterOffsetY = 0.0f,
        .motionVectorScaleX = 1.0f,
        .motionVectorScaleY = 1.0f,
        .frameTimeMilliseconds = frame_ms,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB,
        .cameraNear = 0.1f,
        .cameraFar = 1000.0f,
        .viewSpaceToMeters = 1.0f,
        .cameraVerticalFovRadians = 1.0f,
        .cameraPosition = {0.0f, 0.0f, 0.0f},
        .cameraUp = {0.0f, 1.0f, 0.0f},
        .cameraRight = {1.0f, 0.0f, 0.0f},
        .cameraForward = {0.0f, 0.0f, 1.0f},
        .frameId = id,
        .reset = was_reset ? VK_TRUE : VK_FALSE,
    };
    if (ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context, &prepare) !=
        FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        pending.push_back({id, scheduler.CurrentTick()});
        Fail("its prepare pass failed");
        return false;
    }

    const FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{
        .commandBuffer = static_cast<VkCommandBuffer>(cmdbuf),
        .color = prepare.color,
        .output = MakeImage(interpolated.image, ColorFormat, width, height, InterpolatedUsage),
        .distortionField = {},
        .displayWidth = width,
        .displayHeight = height,
        .frameTimeMilliseconds = frame_ms,
        .cameraNear = prepare.cameraNear,
        .cameraFar = prepare.cameraFar,
        .viewSpaceToMeters = prepare.viewSpaceToMeters,
        .cameraVerticalFovRadians = prepare.cameraVerticalFovRadians,
        .minLuminance = prepare.minLuminance,
        .maxLuminance = prepare.maxLuminance,
        .transferFunction = prepare.transferFunction,
        .frameId = id,
        .reset = prepare.reset,
    };
    const auto result = ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &dispatch);
    pending.push_back({id, scheduler.CurrentTick()});
    if (result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        Fail("its interpolation pass failed");
        return false;
    }
    reset = false;

    const std::array to_output{
        Barrier(interpolated.image, Stage::eAllCommands, Access::eShaderWrite, Stage::eTransfer,
                Access::eTransferRead, Layout::eGeneral, Layout::eGeneral),
        Barrier(output, Stage::eAllCommands, Access::eNone, Stage::eTransfer,
                Access::eTransferWrite, Layout::eUndefined, Layout::eTransferDstOptimal),
    };
    PipelineBarriers(cmdbuf, to_output);
    CopyImage(cmdbuf, interpolated.image, Layout::eGeneral, output, Layout::eTransferDstOptimal,
              width, height);

    const std::array to_present{
        Barrier(output, Stage::eTransfer, Access::eTransferWrite, Stage::eFragmentShader,
                Access::eShaderRead, Layout::eTransferDstOptimal, Layout::eShaderReadOnlyOptimal),
        Barrier(frame, Stage::eTransfer, Access::eTransferRead, Stage::eAllCommands,
                Access::eShaderRead | Access::eColorAttachmentWrite, Layout::eGeneral,
                Layout::eGeneral),
    };
    PipelineBarriers(cmdbuf, to_present);
    return !was_reset;
}

} // namespace Vulkan
