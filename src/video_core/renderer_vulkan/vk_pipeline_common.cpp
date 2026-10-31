// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <mutex>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    cmdbuf.pushConstants(*pipeline_layout, stage_flags, 0u, sizeof(push_data), &push_data);

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(bind_point, *pipeline_layout, 0, set_writes);
        return;
    }

    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(bind_point, *pipeline_layout, 0, desc_set, {});
}

void Pipeline::BindHeapSets(std::span<const vk::DescriptorSet> heap_sets) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    scheduler.CommandBuffer().bindDescriptorSets(bind_point, *pipeline_layout, 1, heap_sets, {});
}

void Pipeline::BuildHeapSetLayouts() {
    for (const auto* stage : GetStages()) {
        if (!stage) {
            continue;
        }
        for (const auto& image : stage->images) {
            if (image.is_heap) {
                heap_is_sampler.push_back(false);
            }
        }
        for (const auto& sampler : stage->samplers) {
            if (sampler.is_heap) {
                heap_is_sampler.push_back(true);
            }
        }
    }
}

vk::DescriptorSetLayout Pipeline::GetHeapSetLayout(const Instance& instance, bool is_sampler) {
    static std::mutex layouts_mutex;
    static std::array<vk::UniqueDescriptorSetLayout, 2> layouts;
    std::scoped_lock lk{layouts_mutex};
    auto& layout = layouts[is_sampler];
    if (!layout) {
        // Slots change while the GPU may still run earlier work that reads other slots.
        const vk::DescriptorBindingFlags binding_flags =
            vk::DescriptorBindingFlagBits::ePartiallyBound |
            vk::DescriptorBindingFlagBits::eUpdateAfterBind |
            vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
        const vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_ci = {
            .bindingCount = 1,
            .pBindingFlags = &binding_flags,
        };
        const vk::DescriptorSetLayoutBinding binding = {
            .binding = 0,
            .descriptorType =
                is_sampler ? vk::DescriptorType::eSampler : vk::DescriptorType::eSampledImage,
            .descriptorCount = is_sampler ? Shader::MaxHeapSamplers : Shader::MaxHeapImages,
            .stageFlags = vk::ShaderStageFlagBits::eAll,
        };
        const vk::DescriptorSetLayoutCreateInfo layout_ci = {
            .pNext = &flags_ci,
            .flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
            .bindingCount = 1,
            .pBindings = &binding,
        };
        auto [result, new_layout] = instance.GetDevice().createDescriptorSetLayoutUnique(layout_ci);
        ASSERT_MSG(result == vk::Result::eSuccess,
                   "Failed to create heap descriptor set layout: {}", vk::to_string(result));
        layout = std::move(new_layout);
    }
    return *layout;
}

vk::UniquePipelineLayout Pipeline::CreatePipelineLayout(
    const vk::PushConstantRange& push_constants) {
    boost::container::small_vector<vk::DescriptorSetLayout, 8> set_layouts{*desc_layout};
    for (const bool is_sampler : heap_is_sampler) {
        set_layouts.push_back(GetHeapSetLayout(instance, is_sampler));
    }
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = static_cast<u32>(set_layouts.size()),
        .pSetLayouts = set_layouts.data(),
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(result));
    return std::move(layout);
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
