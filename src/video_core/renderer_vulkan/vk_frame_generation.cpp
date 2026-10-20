// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
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

constexpr u32 SignatureMaxWidth = 48;
constexpr float SceneCutThreshold = 0.08f;

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
                                  u32 target_width, u32 target_height, const char* name,
                                  u32 mip_levels) {
    const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {target_width, target_height, 1},
        .mipLevels = mip_levels,
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
    DestroySignature();
    DestroyTarget(color);
    DestroyTarget(interpolated);
    DestroyTarget(depth);
    DestroyTarget(motion);
    width = 0;
    height = 0;
}

bool FrameGenerator::CreateSignature(u32 frame_width, u32 frame_height) {
    u32 level_width = std::max(frame_width / 2, 1u);
    u32 level_height = std::max(frame_height / 2, 1u);
    const u32 base_width = level_width;
    const u32 base_height = level_height;
    u32 levels = 1;
    while (level_width > SignatureMaxWidth) {
        level_width = std::max(level_width / 2, 1u);
        level_height = std::max(level_height / 2, 1u);
        ++levels;
    }
    if (!CreateTarget(signature, ColorFormat,
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
                      base_width, base_height, "signature", levels)) {
        return false;
    }
    signature_levels = levels;
    signature_width = level_width;
    signature_height = level_height;

    const VkBufferCreateInfo buffer_info = static_cast<VkBufferCreateInfo>(vk::BufferCreateInfo{
        .size = static_cast<vk::DeviceSize>(level_width) * level_height * 4,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    std::scoped_lock lock{signature_mutex};
    for (auto& slot : signature_slots) {
        VkBuffer buffer{};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_info, &alloc_info, &buffer,
                            &slot.allocation, &info) != VK_SUCCESS) {
            slot = {};
            return false;
        }
        slot.buffer = vk::Buffer{buffer};
        slot.mapped = info.pMappedData;
        slot.frame_id = 0;
    }
    previous_signature.clear();
    previous_signature_id = 0;
    return true;
}

void FrameGenerator::DestroySignature() {
    std::scoped_lock lock{signature_mutex};
    for (auto& slot : signature_slots) {
        if (slot.buffer) {
            vmaDestroyBuffer(instance.GetAllocator(), static_cast<VkBuffer>(slot.buffer),
                             slot.allocation);
        }
        slot = {};
    }
    previous_signature.clear();
    previous_signature_id = 0;
    DestroyTarget(signature);
    signature_levels = 0;
}

void FrameGenerator::RecordSignature(vk::CommandBuffer cmdbuf, vk::Image frame, u32 frame_width,
                                     u32 frame_height, u64 id) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    using Layout = vk::ImageLayout;

    const auto mip_barrier = [&](u32 mip, Access src_access, Access dst_access, Layout old_layout,
                                 Layout new_layout, u32 level_count) {
        auto barrier = Barrier(signature.image, Stage::eAllCommands, src_access, Stage::eTransfer,
                               dst_access, old_layout, new_layout);
        barrier.subresourceRange.baseMipLevel = mip;
        barrier.subresourceRange.levelCount = level_count;
        return barrier;
    };

    const std::array to_blit{
        mip_barrier(0, Access::eTransferRead | Access::eTransferWrite, Access::eTransferWrite,
                    Layout::eUndefined, Layout::eTransferDstOptimal, signature_levels),
    };
    PipelineBarriers(cmdbuf, to_blit);

    const u32 base_width = std::max(frame_width / 2, 1u);
    const u32 base_height = std::max(frame_height / 2, 1u);
    u32 source_width = frame_width;
    u32 source_height = frame_height;
    for (u32 level = 0; level < signature_levels; ++level) {
        const u32 level_width = std::max(base_width >> level, 1u);
        const u32 level_height = std::max(base_height >> level, 1u);
        if (level > 0) {
            const std::array to_source{
                mip_barrier(level - 1, Access::eTransferWrite, Access::eTransferRead,
                            Layout::eTransferDstOptimal, Layout::eTransferSrcOptimal, 1),
            };
            PipelineBarriers(cmdbuf, to_source);
        }
        const vk::ImageBlit blit{
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = level > 0 ? level - 1 : 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .srcOffsets = {{vk::Offset3D{0, 0, 0},
                            vk::Offset3D{static_cast<s32>(source_width),
                                         static_cast<s32>(source_height), 1}}},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = level,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .dstOffsets = {{vk::Offset3D{0, 0, 0},
                            vk::Offset3D{static_cast<s32>(level_width),
                                         static_cast<s32>(level_height), 1}}},
        };
        cmdbuf.blitImage(level > 0 ? signature.image : frame,
                         level > 0 ? Layout::eTransferSrcOptimal : Layout::eGeneral,
                         signature.image, Layout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        source_width = level_width;
        source_height = level_height;
    }

    const u32 last = signature_levels - 1;
    const std::array to_readback{
        mip_barrier(last, Access::eTransferWrite, Access::eTransferRead,
                    Layout::eTransferDstOptimal, Layout::eTransferSrcOptimal, 1),
    };
    PipelineBarriers(cmdbuf, to_readback);

    SignatureSlot& slot = signature_slots[id % SignatureSlots];
    const vk::BufferImageCopy copy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .mipLevel = last,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {signature_width, signature_height, 1},
    };
    cmdbuf.copyImageToBuffer(signature.image, Layout::eTransferSrcOptimal, slot.buffer, copy);

    const vk::MemoryBarrier2 to_host{
        .srcStageMask = Stage::eTransfer,
        .srcAccessMask = Access::eTransferWrite,
        .dstStageMask = Stage::eHost,
        .dstAccessMask = Access::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &to_host,
    });

    std::scoped_lock lock{signature_mutex};
    slot.frame_id = id;
}

bool FrameGenerator::IsSceneCut(u64 id) {
    std::scoped_lock lock{signature_mutex};
    if (id == 0) {
        return false;
    }
    const SignatureSlot& slot = signature_slots[id % SignatureSlots];
    if (!slot.mapped || slot.frame_id != id) {
        return false;
    }
    const size_t size = static_cast<size_t>(signature_width) * signature_height * 4;
    vmaInvalidateAllocation(instance.GetAllocator(), slot.allocation, 0, size);
    const auto* data = static_cast<const u8*>(slot.mapped);

    bool cut = false;
    if (previous_signature_id + 1 == id && previous_signature.size() == size) {
        u64 difference = 0;
        for (size_t i = 0; i < size; i += 4) {
            for (size_t channel = 0; channel < 3; ++channel) {
                const int delta = static_cast<int>(data[i + channel]) -
                                  static_cast<int>(previous_signature[i + channel]);
                difference += static_cast<u64>(delta < 0 ? -delta : delta);
            }
        }
        const double mean = static_cast<double>(difference) / (size / 4 * 3 * 255.0);
        cut = mean > SceneCutThreshold;
    }
    previous_signature.assign(data, data + size);
    previous_signature_id = id;
    if (cut) {
        reset = true;
    }
    return cut;
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
    if (!CreateSignature(new_width, new_height)) {
        Destroy();
        Fail("out of video memory for its scene change signature");
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
    signature_id = 0;
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
    RecordSignature(cmdbuf, frame, width, height, frame_id + 1);
    signature_id = frame_id + 1;

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
