// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Lemon Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "video_core/host_shaders/lemon_fg_generate_comp.h"
#include "video_core/host_shaders/lemon_fg_luma_comp.h"
#include "video_core/host_shaders/lemon_fg_median_comp.h"
#include "video_core/host_shaders/lemon_fg_project_comp.h"
#include "video_core/host_shaders/lemon_fg_search_comp.h"
#include "video_core/host_shaders/lemon_fg_still_comp.h"
#include "video_core/renderer_vulkan/host_passes/lemon_fg_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

namespace {

using Common::DivCeil;
using Stage = vk::PipelineStageFlagBits2;
using Access = vk::AccessFlagBits2;
using Layout = vk::ImageLayout;

constexpr vk::Format FrameFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::Format WorkFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr u32 Block = 8;
constexpr u32 SearchReduction = 4;
constexpr u32 HighQualitySearchReduction = 2;
constexpr u32 StillPasses = 3;
constexpr u32 MaxSampled = 4;

constexpr float ZeroBias = 0.2f;
constexpr float SmoothBias = 1.0f;

enum SearchMode : u32 {
    Exhaustive = 0,
    FromCoarser = 1,
    Refine = 2,
};

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

u32 Bits(float value) {
    return std::bit_cast<u32>(value);
}

vk::Extent2D Reduce(vk::Extent2D size, u32 divisor) {
    return {std::max(1u, DivCeil(size.width, divisor)),
            std::max(1u, DivCeil(size.height, divisor))};
}

vk::ImageMemoryBarrier2 ImageBarrier(vk::Image image, vk::PipelineStageFlags2 src_stage,
                                     vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                                     vk::AccessFlags2 dst_access, Layout old_layout,
                                     Layout new_layout) {
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

void ImageBarriers(vk::CommandBuffer cmdbuf, std::span<const vk::ImageMemoryBarrier2> barriers) {
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
}

void CopyImage(vk::CommandBuffer cmdbuf, vk::Image src, Layout src_layout, vk::Image dst,
               Layout dst_layout, vk::Extent2D size) {
    const vk::ImageCopy region{
        .srcSubresource = ColorLayers,
        .srcOffset = {0, 0, 0},
        .dstSubresource = ColorLayers,
        .dstOffset = {0, 0, 0},
        .extent = {size.width, size.height, 1},
    };
    cmdbuf.copyImage(src, src_layout, dst, dst_layout, region);
}

} // Anonymous namespace

bool LemonFgPass::IsSupported(const Instance& instance) {
    return instance.IsComputeSubgroupArithmeticSupported();
}

void LemonFgPass::Create(const Instance& instance, Scheduler& scheduler_, Settings settings_) {
    device = instance.GetDevice();
    allocator = instance.GetAllocator();
    scheduler = &scheduler_;
    settings = settings_;

    sampler = Check<"create lemon fg sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    }));

    luma_pass = CreatePass(LEMON_FG_LUMA_COMP, 1, sizeof(u32) * 5, "lemon_fg_luma.comp");
    search_pass = CreatePass(LEMON_FG_SEARCH_COMP, 4, sizeof(u32) * 11, "lemon_fg_search.comp");
    median_pass = CreatePass(LEMON_FG_MEDIAN_COMP, 1, sizeof(u32) * 2, "lemon_fg_median.comp");
    project_pass = CreatePass(LEMON_FG_PROJECT_COMP, 1, sizeof(u32) * 3, "lemon_fg_project.comp");
    still_pass = CreatePass(LEMON_FG_STILL_COMP, 2, sizeof(u32) * 5, "lemon_fg_still.comp");
    generate_pass =
        CreatePass(LEMON_FG_GENERATE_COMP, 4, sizeof(u32) * 9, "lemon_fg_generate.comp");
}

LemonFgPass::Pass LemonFgPass::CreatePass(std::span<const u32> code, u32 sampled_count,
                                          u32 push_size, const char* name) const {
    ASSERT(sampled_count <= MaxSampled);
    std::array<vk::DescriptorSetLayoutBinding, MaxSampled + 2> bindings{};
    bindings[0] = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
    };
    for (u32 binding = 1; binding <= sampled_count; ++binding) {
        bindings[binding] = {
            .binding = binding,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        };
    }
    bindings[sampled_count + 1] = {
        .binding = sampled_count + 1,
        .descriptorType = vk::DescriptorType::eStorageImage,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
    };

    Pass pass;
    pass.push_size = push_size;
    pass.set_layout =
        Check<"create lemon fg descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = sampled_count + 2,
            .pBindings = bindings.data(),
        }));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = push_size,
    };
    pass.pipeline_layout =
        Check<"create lemon fg pipeline layout">(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &pass.set_layout.get(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        }));
    SetObjectName(device, pass.pipeline_layout.get(), "{} pipeline layout", name);

    const auto module = CompileSPV(code, device);
    ASSERT(module);
    SetObjectName(device, module, name);
    const vk::ComputePipelineCreateInfo pipeline_info{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
        },
        .layout = pass.pipeline_layout.get(),
    };
    pass.pipeline = Check<"create lemon fg compute pipeline">(
        device.createComputePipelineUnique({}, pipeline_info));
    SetObjectName(device, pass.pipeline.get(), "{} pipeline", name);
    device.destroyShaderModule(module);
    return pass;
}

void LemonFgPass::CreateTarget(Target& target, vk::Format target_format, vk::ImageUsageFlags usage,
                               vk::Extent2D size, const char* name, bool luma_swizzle) const {
    target.luma_view.reset();
    target.view.reset();
    target.image.Destroy();
    target.image = VideoCore::UniqueImage(device, allocator);
    target.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = target_format,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, static_cast<vk::Image>(target.image), "Lemon FG {}", name);

    vk::ImageViewCreateInfo view_info{
        .image = target.image,
        .viewType = vk::ImageViewType::e2D,
        .format = target_format,
        .subresourceRange = ColorRange,
    };
    target.view = Check<"create lemon fg image view">(device.createImageViewUnique(view_info));
    if (luma_swizzle) {
        view_info.components = {
            .r = vk::ComponentSwizzle::eB,
            .b = vk::ComponentSwizzle::eR,
        };
    }
    target.luma_view = Check<"create lemon fg image view">(device.createImageViewUnique(view_info));
    target.size = size;
}

void LemonFgPass::Recreate(vk::CommandBuffer cmdbuf, u32 width, u32 height,
                           vk::Format frame_format) {
    if (frames[0].image) {
        const u64 tick = scheduler->CurrentTick();
        if (tick > 1) {
            scheduler->Wait(tick - 1);
        }
    }

    extent = {width, height};
    format = frame_format;
    level_size[0] =
        Reduce(extent, settings.high_quality ? HighQualitySearchReduction : SearchReduction);
    for (u32 level = 1; level < Levels; ++level) {
        level_size[level] = Reduce(level_size[level - 1], 2);
    }
    for (u32 level = 0; level < Levels; ++level) {
        grid[level] = Reduce(level_size[level], Block);
    }
    still_size = Reduce(extent, 2);

    // The frames are copied in as RGBA whatever the presented order is; only the luma needs the
    // real channels back.
    const bool bgra =
        frame_format == vk::Format::eB8G8R8A8Unorm || frame_format == vk::Format::eB8G8R8A8Srgb;
    constexpr auto frame_usage =
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    constexpr auto work_usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
    for (auto& frame : frames) {
        CreateTarget(frame, FrameFormat, frame_usage, extent, "frame", bgra);
    }
    for (auto& slot : pyramid) {
        for (u32 level = 0; level < Levels; ++level) {
            CreateTarget(slot[level], WorkFormat, work_usage, level_size[level], "luma");
        }
    }
    for (u32 level = 0; level < Levels; ++level) {
        for (auto& field : fields[level]) {
            CreateTarget(field, WorkFormat, work_usage, grid[level], "search field");
        }
    }
    for (auto& flow : flows) {
        CreateTarget(flow, WorkFormat, work_usage, grid[0], "flow");
    }
    CreateTarget(offsets, WorkFormat, work_usage, grid[0], "offsets");
    CreateTarget(offsets_filtered, WorkFormat, work_usage, grid[0], "filtered offsets");
    for (auto& mask : still_masks) {
        CreateTarget(mask, WorkFormat, work_usage | vk::ImageUsageFlagBits::eTransferDst,
                     still_size, "still mask");
    }
    CreateTarget(interpolated, FrameFormat, work_usage | vk::ImageUsageFlagBits::eTransferSrc,
                 extent, "output");

    std::vector<vk::ImageMemoryBarrier2> to_general;
    const auto add = [&](const Target& target) {
        to_general.push_back(ImageBarrier(target.image, Stage::eNone, Access::eNone,
                                          Stage::eComputeShader,
                                          Access::eShaderSampledRead | Access::eShaderStorageWrite,
                                          Layout::eUndefined, Layout::eGeneral));
    };
    for (const auto& slot : pyramid) {
        std::ranges::for_each(slot, add);
    }
    for (const auto& level : fields) {
        std::ranges::for_each(level, add);
    }
    std::ranges::for_each(flows, add);
    std::ranges::for_each(still_masks, add);
    add(offsets);
    add(offsets_filtered);
    ImageBarriers(cmdbuf, to_general);

    frame_count = 0;
    last_flow = ~0ULL;
    reset = true;
    LOG_INFO(Render_Vulkan,
             "Lemon frame generation ready at {}x{} (motion search {}x{}, HUD protection {})",
             extent.width, extent.height, level_size[0].width, level_size[0].height,
             settings.hud_protection ? "on" : "off");
}

void LemonFgPass::Run(vk::CommandBuffer cmdbuf, const Pass& pass,
                      std::span<const vk::ImageView> sampled, const Target& destination,
                      const void* push, u32 groups_x, u32 groups_y) const {
    std::array<vk::DescriptorImageInfo, MaxSampled + 2> infos{};
    std::array<vk::WriteDescriptorSet, MaxSampled + 2> writes{};
    u32 count = 0;
    const auto add = [&](vk::ImageView view, vk::DescriptorType type) {
        infos[count] = {
            .sampler = type == vk::DescriptorType::eSampler ? sampler.get() : vk::Sampler{},
            .imageView = view,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
        writes[count] = {
            .dstBinding = count,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = &infos[count],
        };
        ++count;
    };
    add({}, vk::DescriptorType::eSampler);
    for (const vk::ImageView view : sampled) {
        add(view, vk::DescriptorType::eSampledImage);
    }
    add(destination.view.get(), vk::DescriptorType::eStorageImage);

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pass.pipeline.get());
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pass.pipeline_layout.get(), 0,
                                count, writes.data());
    cmdbuf.pushConstants(pass.pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                         pass.push_size, push);
    cmdbuf.dispatch(groups_x, groups_y, 1);

    const vk::MemoryBarrier2 write_to_read{
        .srcStageMask = Stage::eComputeShader,
        .srcAccessMask = Access::eShaderStorageWrite,
        .dstStageMask = Stage::eComputeShader,
        .dstAccessMask = Access::eShaderSampledRead | Access::eShaderStorageWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &write_to_read,
    });
}

void LemonFgPass::BuildPyramid(vk::CommandBuffer cmdbuf, u32 slot) {
    const std::array<u32, 5> luma_push{extent.width, extent.height, level_size[0].width,
                                       level_size[0].height, 0};
    Run(cmdbuf, luma_pass, std::array{frames[slot].luma_view.get()}, pyramid[slot][0],
        luma_push.data(), DivCeil(level_size[0].width, 8u), DivCeil(level_size[0].height, 8u));
    for (u32 level = 1; level < Levels; ++level) {
        const std::array<u32, 5> down_push{level_size[level - 1].width,
                                           level_size[level - 1].height, level_size[level].width,
                                           level_size[level].height, 1};
        Run(cmdbuf, luma_pass, std::array{pyramid[slot][level - 1].view.get()},
            pyramid[slot][level], down_push.data(), DivCeil(level_size[level].width, 8u),
            DivCeil(level_size[level].height, 8u));
    }
}

void LemonFgPass::SearchMotion(vk::CommandBuffer cmdbuf, u32 slot, bool use_temporal) {
    const auto& luma_a = pyramid[1 - slot];
    const auto& luma_c = pyramid[slot];
    const Target& temporal = flows[1 - slot];
    const auto step = [&](u32 level, SearchMode mode, const Target& in, vk::Extent2D in_grid,
                          const Target& out) {
        const std::array<u32, 11> push{
            level_size[level].width,
            level_size[level].height,
            grid[level].width,
            grid[level].height,
            in_grid.width,
            in_grid.height,
            mode,
            use_temporal ? 1u : 0u,
            Bits(1.0f / static_cast<float>(1u << level)),
            Bits(ZeroBias),
            Bits(SmoothBias),
        };
        const std::array sampled{luma_a[level].view.get(), luma_c[level].view.get(), in.view.get(),
                                 temporal.view.get()};
        Run(cmdbuf, search_pass, sampled, out, push.data(), grid[level].width, grid[level].height);
    };

    // Coarsest level: exhaustive then two passes; middle levels: two passes; finest: one.
    constexpr u32 Coarsest = Levels - 1;
    auto& coarsest = fields[Coarsest];
    step(Coarsest, Exhaustive, coarsest[1], grid[Coarsest], coarsest[0]);
    step(Coarsest, Refine, coarsest[0], grid[Coarsest], coarsest[1]);
    step(Coarsest, Refine, coarsest[1], grid[Coarsest], coarsest[0]);
    const Target* result = &coarsest[0];
    for (u32 level = Coarsest; level-- > 0;) {
        auto& current = fields[level];
        step(level, FromCoarser, *result, grid[level + 1], current[0]);
        result = &current[0];
        if (level > 0) {
            step(level, Refine, current[0], grid[level], current[1]);
            result = &current[1];
        }
    }

    const std::array<u32, 2> median_push{grid[0].width, grid[0].height};
    Run(cmdbuf, median_pass, std::array{result->view.get()}, flows[slot], median_push.data(),
        DivCeil(grid[0].width, 8u), DivCeil(grid[0].height, 8u));
}

void LemonFgPass::MaskStillContent(vk::CommandBuffer cmdbuf, u32 slot) {
    // Raw mask from the frames, then majority and dilation, ping-ponging so the last pass
    // leaves the result in still_masks[0].
    for (u32 pass = 0; pass < StillPasses; ++pass) {
        const Target& in_a = pass == 0 ? frames[1 - slot] : still_masks[pass % 2 == 1 ? 0 : 1];
        const Target& in_c = pass == 0 ? frames[slot] : in_a;
        const Target& out = still_masks[pass % 2 == 0 ? 0 : 1];
        const std::array<u32, 5> push{still_size.width, still_size.height, extent.width,
                                      extent.height, pass};
        Run(cmdbuf, still_pass, std::array{in_a.view.get(), in_c.view.get()}, out, push.data(),
            DivCeil(still_size.width, 8u), DivCeil(still_size.height, 8u));
    }
}

void LemonFgPass::ClearStillMask(vk::CommandBuffer cmdbuf) {
    const std::array to_clear{
        ImageBarrier(still_masks[0].image, Stage::eComputeShader, Access::eNone, Stage::eClear,
                     Access::eTransferWrite, Layout::eGeneral, Layout::eGeneral),
    };
    ImageBarriers(cmdbuf, to_clear);
    cmdbuf.clearColorImage(still_masks[0].image, Layout::eGeneral, vk::ClearColorValue{},
                           ColorRange);
    const std::array cleared{
        ImageBarrier(still_masks[0].image, Stage::eClear, Access::eTransferWrite,
                     Stage::eComputeShader, Access::eShaderSampledRead, Layout::eGeneral,
                     Layout::eGeneral),
    };
    ImageBarriers(cmdbuf, cleared);
}

void LemonFgPass::Interpolate(vk::CommandBuffer cmdbuf, u32 slot, float t) {
    const std::array<u32, 3> project_push{grid[0].width, grid[0].height, Bits(t)};
    Run(cmdbuf, project_pass, std::array{flows[slot].view.get()}, offsets, project_push.data(),
        DivCeil(grid[0].width, 8u), DivCeil(grid[0].height, 8u));
    const std::array<u32, 2> median_push{grid[0].width, grid[0].height};
    Run(cmdbuf, median_pass, std::array{offsets.view.get()}, offsets_filtered, median_push.data(),
        DivCeil(grid[0].width, 8u), DivCeil(grid[0].height, 8u));

    const std::array to_write{
        ImageBarrier(interpolated.image, Stage::eTransfer, Access::eNone, Stage::eComputeShader,
                     Access::eShaderStorageWrite, Layout::eUndefined, Layout::eGeneral),
    };
    ImageBarriers(cmdbuf, to_write);
    const std::array<u32, 9> generate_push{
        extent.width,         extent.height,     level_size[0].width,
        level_size[0].height, grid[0].width,     grid[0].height,
        still_size.width,     still_size.height, Bits(t),
    };
    const std::array sampled{frames[1 - slot].view.get(), frames[slot].view.get(),
                             offsets_filtered.view.get(), still_masks[0].view.get()};
    Run(cmdbuf, generate_pass, sampled, interpolated, generate_push.data(),
        DivCeil(extent.width, 16u), DivCeil(extent.height, 8u));
}

u32 LemonFgPass::Generate(vk::CommandBuffer cmdbuf, vk::Image frame, vk::Format frame_format,
                          std::span<const vk::Image> outputs, u32 width, u32 height) {
    if (width == 0 || height == 0 || outputs.empty()) {
        return 0;
    }
    if (extent != vk::Extent2D{width, height} || format != frame_format) {
        Recreate(cmdbuf, width, height, frame_format);
    }

    const u32 slot = static_cast<u32>(frame_count % 2);
    const std::array to_copy{
        ImageBarrier(frame, Stage::eAllCommands,
                     Access::eColorAttachmentWrite | Access::eShaderWrite | Access::eTransferWrite,
                     Stage::eTransfer, Access::eTransferRead, Layout::eGeneral, Layout::eGeneral),
        ImageBarrier(frames[slot].image, Stage::eComputeShader, Access::eNone, Stage::eTransfer,
                     Access::eTransferWrite, Layout::eUndefined, Layout::eGeneral),
    };
    ImageBarriers(cmdbuf, to_copy);
    CopyImage(cmdbuf, frame, Layout::eGeneral, frames[slot].image, Layout::eGeneral, extent);
    const std::array copied{
        ImageBarrier(frames[slot].image, Stage::eTransfer, Access::eTransferWrite,
                     Stage::eComputeShader, Access::eShaderSampledRead, Layout::eGeneral,
                     Layout::eGeneral),
        ImageBarrier(frame, Stage::eTransfer, Access::eTransferRead, Stage::eAllCommands,
                     Access::eShaderRead | Access::eColorAttachmentWrite, Layout::eGeneral,
                     Layout::eGeneral),
    };
    ImageBarriers(cmdbuf, copied);

    BuildPyramid(cmdbuf, slot);

    const bool was_reset = std::exchange(reset, false);
    const u64 pair = frame_count++;
    if (was_reset) {
        return 0;
    }
    SearchMotion(cmdbuf, slot, last_flow + 1 == pair);
    last_flow = pair;
    if (settings.hud_protection) {
        MaskStillContent(cmdbuf, slot);
    } else {
        ClearStillMask(cmdbuf);
    }

    const u32 count = static_cast<u32>(outputs.size());
    for (u32 i = 0; i < count; ++i) {
        const vk::Image output = outputs[i];
        Interpolate(cmdbuf, slot, static_cast<float>(i + 1) / static_cast<float>(count + 1));
        const std::array to_output{
            ImageBarrier(interpolated.image, Stage::eComputeShader, Access::eShaderStorageWrite,
                         Stage::eTransfer, Access::eTransferRead, Layout::eGeneral,
                         Layout::eGeneral),
            ImageBarrier(output, Stage::eAllCommands, Access::eNone, Stage::eTransfer,
                         Access::eTransferWrite, Layout::eUndefined, Layout::eTransferDstOptimal),
        };
        ImageBarriers(cmdbuf, to_output);
        CopyImage(cmdbuf, interpolated.image, Layout::eGeneral, output, Layout::eTransferDstOptimal,
                  extent);
        const std::array to_present{
            ImageBarrier(output, Stage::eTransfer, Access::eTransferWrite, Stage::eFragmentShader,
                         Access::eShaderRead, Layout::eTransferDstOptimal,
                         Layout::eShaderReadOnlyOptimal),
        };
        ImageBarriers(cmdbuf, to_present);
    }
    return count;
}

} // namespace Vulkan::HostPasses
