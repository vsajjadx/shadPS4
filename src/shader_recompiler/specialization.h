// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bitset>
#include <string>
#include <type_traits>

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "common/hack_features.h"
#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace Shader {

struct VsAttribSpecialization {
    u32 divisor{};
    AmdGpu::NumberClass num_class{};
    AmdGpu::CompMapping dst_select{};

    bool operator==(const VsAttribSpecialization&) const = default;
};

struct BufferSpecialization {
    u32 stride : 14;
    u32 is_formatted : 1;
    u32 swizzle_enable : 1;
    u32 data_format : 6;
    u32 num_format : 4;
    u32 index_stride : 2;
    u32 element_size : 2;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};

    bool operator==(const BufferSpecialization& other) const {
        return stride == other.stride && is_formatted == other.is_formatted &&
               swizzle_enable == other.swizzle_enable &&
               (!is_formatted ||
                (data_format == other.data_format && num_format == other.num_format &&
                 dst_select == other.dst_select && num_conversion == other.num_conversion)) &&
               (!swizzle_enable ||
                (index_stride == other.index_stride && element_size == other.element_size));
    }
};

struct ImageSpecialization {
    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    bool is_integer = false;
    bool is_storage = false;
    bool is_srgb = false;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};
    // FIXME any pipeline cache changes needed?
    u32 num_bindings = 0;

    bool operator==(const ImageSpecialization&) const = default;
};

struct FMaskSpecialization {
    u32 width;
    u32 height;

    bool operator==(const FMaskSpecialization&) const = default;
};

struct SamplerSpecialization {
    u8 force_unnormalized : 1;
    u8 force_degamma : 1;

    bool operator==(const SamplerSpecialization&) const = default;
};

/**
 * Alongside runtime information, this structure also checks bound resources
 * for compatibility. Can be used as a key for storing shader permutations.
 * Is separate from runtime information, because resource layout can only be deduced
 * after the first compilation of a module.
 */
struct StageSpecialization {
    static constexpr size_t MaxStageResources = 128;

    const Info* info{};
    RuntimeInfo runtime_info{};
    std::bitset<MaxStageResources> bitset{};
    Gcn::FetchShaderData fetch_shader_data{};
    SmallVector<VsAttribSpecialization, 32> vs_attribs;
    SmallVector<BufferSpecialization, 16> buffers;
    SmallVector<ImageSpecialization, 16> images;
    SmallVector<FMaskSpecialization, 8> fmasks;
    SmallVector<SamplerSpecialization, 16> samplers;
    Backend::Bindings start{};

    StageSpecialization() = default;
    StageSpecialization(const Info& info_, RuntimeInfo runtime_info_, const Profile& profile_,
                        Backend::Bindings start_)
        : info{&info_}, runtime_info{runtime_info_}, start{start_} {
        if (info_.sw_stage == SwStage::Vertex && Gcn::ParseFetchShader(info_, fetch_shader_data)) {
            // Specialize shader on VS input number types to follow spec.
            ForEachSharp(vs_attribs, fetch_shader_data.attributes,
                         [this](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                             using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
                             if (const auto step_rate = desc.GetStepRate();
                                 step_rate != InstanceIdType::None) {
                                 spec.divisor = step_rate == InstanceIdType::OverStepRate0
                                                    ? runtime_info.sw.vs.step_rate_0
                                                    : (step_rate == InstanceIdType::OverStepRate1
                                                           ? runtime_info.sw.vs.step_rate_1
                                                           : 1);
                             }
                             spec.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                         });
        }
        u32 binding{};
        ForEachSharp(binding, buffers, info->buffers,
                     [](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                         spec.stride = sharp.GetStride();
                         spec.is_formatted = desc.is_formatted;
                         spec.swizzle_enable = sharp.swizzle_enable;
                         if (spec.is_formatted) {
                             spec.data_format = static_cast<u32>(sharp.GetDataFmt());
                             spec.num_format = static_cast<u32>(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                             spec.num_conversion = sharp.GetNumberConversion();
                         }
                         if (spec.swizzle_enable) {
                             spec.index_stride = sharp.index_stride;
                             spec.element_size = sharp.element_size;
                         }
                     });
        const bool ignore_images = HasGarbageImageSharps(*info);
        for (const auto& desc : info->images) {
            auto& spec = images.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            // Garbage T#s (e.g. from an SRT walker failure) change on every dispatch;
            // specializing on them compiles a new permutation each time. They are bound as
            // null images anyway.
            const bool is_bound = !ignore_images && sharp && IsPlausibleImageSharp(sharp);
            if (is_bound || (Common::HackFeatures::isTheOrder1886 && !ignore_images)) {
                // For The Order: 1886 GetSharp already replaced unusable T#s with a null one,
                // which is also what the module was compiled with. Record it so a later draw
                // binding a real texture of the same type can reuse this module.
                FillImageSpec(spec, desc, sharp);
            }
            bitset[binding++] = is_bound;
        }
        ForEachSharp(binding, fmasks, info->fmasks,
                     [](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec.width = sharp.width;
                         spec.height = sharp.height;
                     });
        ForEachSharp(samplers, info->samplers,
                     [](auto& spec, const auto& desc, AmdGpu::Sampler sharp) {
                         spec.force_unnormalized = sharp.force_unnormalized;
                         spec.force_degamma = sharp.force_degamma;
                     });

        // Initialize runtime_info fields that rely on analysis in tessellation passes
        if (info->sw_stage == SwStage::TessellationControl ||
            info->sw_stage == SwStage::TessellationEval) {
            TessellationDataConstantBuffer tess_constants{};
            info->ReadTessConstantBuffer(tess_constants);
            runtime_info.InitFromTessConstants(tess_constants);
        }
    }

    void ForEachSharp(auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                continue;
            }
            func(spec, desc, sharp);
        }
    }

    void ForEachSharp(u32& binding, auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                binding++;
                continue;
            }
            if constexpr (std::is_same_v<decltype(func(spec, desc, sharp)), bool>) {
                if (!func(spec, desc, sharp)) {
                    spec = {};
                    binding++;
                    continue;
                }
            } else {
                func(spec, desc, sharp);
            }
            bitset[binding++] = true;
        }
    }

    void FillImageSpec(ImageSpecialization& spec, const ImageResource& desc,
                       const AmdGpu::Image& sharp) const {
        spec.type = sharp.GetViewType(desc.is_array);
        spec.is_integer = AmdGpu::IsInteger(sharp.GetNumberFmt());
        spec.is_storage = desc.is_written;
        if (spec.is_storage) {
            spec.dst_select = sharp.DstSelect();
        } else {
            spec.is_srgb = sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
        }
        spec.num_conversion = sharp.GetNumberConversion();
        spec.num_bindings = desc.NumBindings(*info);
    }

    static bool HasGarbageImageSharps(const Info& info) {
        // The Order: 1886 compute shaders whose T#s are bound as null images in
        // Rasterizer::BindTextures (keep these lists in sync).
        return Common::HackFeatures::isTheOrder1886 &&
               (info.pgm_hash == 0xa7b66f58 || info.pgm_hash == 0xefaaab2b ||
                info.pgm_hash == 0x804aeead);
    }

    /// Describes which part of the specialization differs from other, for diagnostics.
    std::string DescribeDifference(const StageSpecialization& other) const {
        if (vs_attribs != other.vs_attribs) {
            return "vs_attribs";
        }
        if (runtime_info != other.runtime_info) {
            return "runtime_info";
        }
        if (fetch_shader_data != other.fetch_shader_data) {
            return "fetch_shader_data";
        }
        if (fmasks != other.fmasks) {
            return "fmasks";
        }
        if (start != other.start) {
            return "start_bindings";
        }
        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++) {
            if (other.bitset[binding++] && buffers[i] != other.buffers[i]) {
                return fmt::format("buffer[{}] stride={}->{} fmt={}->{}", i, buffers[i].stride,
                                   other.buffers[i].stride, buffers[i].data_format,
                                   other.buffers[i].data_format);
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (other.bitset[binding++] && images[i] != other.images[i]) {
                return fmt::format("image[{}] type={}->{} int={}->{} srgb={}->{}", i,
                                   static_cast<u32>(images[i].type),
                                   static_cast<u32>(other.images[i].type), images[i].is_integer,
                                   other.images[i].is_integer, images[i].is_srgb,
                                   other.images[i].is_srgb);
            }
        }
        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                return fmt::format("sampler[{}]", i);
            }
        }
        return "none";
    }

    [[nodiscard]] bool Valid() const {
        return info != nullptr;
    }

    /// Returns true if other only differs from this in sampled images that this module could
    /// still be used for by binding them as null. Their indices are set in null_mask.
    bool DiffersOnlyInSampledImages(const StageSpecialization& other, u64& null_mask,
                                    u32& num_differences) const {
        null_mask = 0;
        num_differences = 0;
        if (!Valid() || vs_attribs != other.vs_attribs || runtime_info != other.runtime_info ||
            fetch_shader_data != other.fetch_shader_data || fmasks != other.fmasks ||
            start != other.start || images.size() != other.images.size()) {
            return false;
        }
        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++) {
            if (other.bitset[binding++] && buffers[i] != other.buffers[i]) {
                return false;
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (!other.bitset[binding++] || images[i] == other.images[i]) {
                continue;
            }
            if (images[i].is_storage || other.images[i].is_storage || images[i].num_bindings != 1 ||
                other.images[i].num_bindings != 1) {
                return false;
            }
            null_mask |= 1ULL << i;
            ++num_differences;
        }
        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                return false;
            }
        }
        return true;
    }

    bool operator==(const StageSpecialization& other) const {
        if (!Valid()) {
            return false;
        }

        if (vs_attribs != other.vs_attribs) {
            return false;
        }

        if (runtime_info != other.runtime_info) {
            return false;
        }

        if (fetch_shader_data != other.fetch_shader_data) {
            return false;
        }

        if (fmasks != other.fmasks) {
            return false;
        }

        // For VS which only generates geometry and doesn't have any inputs, its start
        // bindings still may change as they depend on previously processed FS. The check below
        // handles this case and prevents generation of redundant permutations. This is also safe
        // for other types of shaders with no bindings.
        if (buffers.empty() && images.empty() && samplers.empty()) {
            return true;
        }

        if (start != other.start) {
            return false;
        }

        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++) {
            if (other.bitset[binding++] && buffers[i] != other.buffers[i]) {
                return false;
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (other.bitset[binding++] && images[i] != other.images[i]) {
                return false;
            }
        }

        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                return false;
            }
        }
        return true;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
