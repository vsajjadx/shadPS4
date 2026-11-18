// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <fstream>
#include <boost/container/small_vector.hpp>

#include "common/path_util.h"
#include "common/scm_rev.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 const Shader::HwComputeRuntimeInfo& cs_info_,
                                 std::vector<u32> spirv, SerializationSupport& sdata,
                                 bool preloading /*=false*/, bool defer_build /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_}, cs_info{cs_info_}, shader_hash{info_.pgm_hash},
      stage_spirv{std::move(spirv)}, stage_module{module}, pipeline_cache_handle{pipeline_cache} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        if (num_bindings == 0) {
            continue;
        }
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        if (sampler.is_heap) {
            continue;
        }
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    BuildHeapSetLayouts();
    pipeline_layout = CreatePipelineLayout(push_constants);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    if (!defer_build) {
        TryBuild();
    }
}

void ComputePipeline::TryBuild() {
    if (!build_started.exchange(true, std::memory_order_acq_rel)) {
        Build();
    }
}

void ComputePipeline::Build() {
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = cs_info.required_subgroup_size,
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage =
            {
                .pNext = cs_info.required_subgroup_size ? &subgroup_size_ci : nullptr,
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = stage_module,
                .pName = "main",
            },
        .layout = *pipeline_layout,
    };
    auto [pipeline_result, pipe] = instance.GetDevice().createComputePipelineUnique(
        pipeline_cache_handle, compute_pipeline_ci);
    if (pipeline_result != vk::Result::eSuccess) {
        LogBuildFailure(pipeline_result);
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(instance.GetDevice(), *pipeline, "Compute Pipeline {}", GetDebugString());
    std::vector<u32>{}.swap(stage_spirv);
    ready.Complete();
}

void ComputePipeline::LogBuildFailure(vk::Result result) const {
    const auto limits = instance.GetComputeSubgroupLimits();
    const auto details = fmt::format(
        "Compute pipeline failure: result={} revision={} shader_cache={} GPU={} driver_id={} "
        "driver_version={} shader={:#x} pipeline={:#x} SPIR-V={:#x} words={} local={}x{}x{} "
        "profile={} effective={} requested={} enabled={} compute_stage={} range={}..{} "
        "max_subgroups={} max_invocations={} max_local={}x{}x{} require_full_subgroups=false",
        vk::to_string(result), Common::g_scm_rev, Common::g_shader_cache_hash,
        instance.GetModelName(), vk::to_string(instance.GetDriverID()), instance.GetDriverVersion(),
        shader_hash, compute_key.value, stage_spirv.size() > 1 ? stage_spirv[1] : 0,
        stage_spirv.size(), cs_info.workgroup_size[0], cs_info.workgroup_size[1],
        cs_info.workgroup_size[2], profile.subgroup_size, cs_info.subgroup_size,
        cs_info.required_subgroup_size, limits.size_control_enabled, limits.compute_required_size,
        limits.min_size, limits.max_size, limits.max_workgroup_subgroups,
        limits.max_workgroup_invocations, limits.max_workgroup_size[0],
        limits.max_workgroup_size[1], limits.max_workgroup_size[2]);
    LOG_CRITICAL(Render_Vulkan, "{}", details);

    const auto dump_dir = Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "failures";
    std::error_code error;
    std::filesystem::create_directories(dump_dir, error);
    if (error || stage_spirv.empty()) {
        LOG_ERROR(Render_Vulkan, "Could not save failed compute SPIR-V: {}",
                  error ? error.message() : "shader binary unavailable");
        return;
    }
    const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    const auto name =
        fmt::format("cs_{:#018x}_{:#018x}_{}", shader_hash, compute_key.value, timestamp);
    const auto binary_path = dump_dir / (name + ".spv");
    std::ofstream binary{binary_path, std::ios::binary};
    binary.write(reinterpret_cast<const char*>(stage_spirv.data()),
                 static_cast<std::streamsize>(stage_spirv.size() * sizeof(u32)));
    binary.close();
    std::ofstream metadata{dump_dir / (name + ".txt")};
    metadata << details << '\n';
    metadata.close();
    if (binary && metadata) {
        LOG_CRITICAL(Render_Vulkan, "Saved failed compute SPIR-V and metadata to {}",
                     Common::FS::PathToUTF8String(binary_path));
    } else {
        LOG_ERROR(Render_Vulkan, "Could not save complete compute failure evidence to {}",
                  Common::FS::PathToUTF8String(binary_path));
    }
}

ComputePipeline::~ComputePipeline() = default;

} // namespace Vulkan
