// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/hash.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/vk_compute_subgroup.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

inline bool IsShaderCacheEntryCompatible(const Shader::Info& info,
                                         const Shader::RuntimeInfo& runtime, size_t perm_idx,
                                         std::optional<u64> expected_compute_key,
                                         const ComputeSubgroupLimits& limits) {
    const bool is_compute = expected_compute_key.has_value();
    if (info.hw_stage != runtime.hw_stage || info.sw_stage != runtime.sw_stage ||
        (info.hw_stage == Shader::HwStage::Compute) != is_compute ||
        (info.sw_stage == Shader::SwStage::Compute) != is_compute ||
        (is_compute && HashCombine(info.pgm_hash, perm_idx) != *expected_compute_key)) {
        return false;
    }
    if (!is_compute) {
        return true;
    }
    const auto subgroup = SelectComputeSubgroupPolicy(limits, runtime.hw.cs.workgroup_size);
    return subgroup && subgroup->effective_size == runtime.hw.cs.subgroup_size &&
           subgroup->required_size == runtime.hw.cs.required_subgroup_size;
}

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata);
void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata);
void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash, size_t perm_idx);
void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx);

} // namespace Vulkan
