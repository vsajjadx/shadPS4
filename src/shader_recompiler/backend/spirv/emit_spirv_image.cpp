// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/static_vector.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Backend::SPIRV {

template <bool is_float>
static Id FixImageCoords(EmitContext& ctx, Id coords, AmdGpu::ImageType image_type) {
    const auto coord_type = is_float ? ctx.F32 : ctx.U32;
    const auto zero = is_float ? ctx.f32_zero_value : ctx.u32_zero_value;

    switch (image_type) {
    case AmdGpu::ImageType::Color1D: {
        // Lowered to 2D with height 1
        const auto x = coords;
        return ctx.OpCompositeConstruct(coord_type[2], x, zero);
    }
    case AmdGpu::ImageType::Color1DArray: {
        // Lowered to 2D array with height 1
        const auto x = ctx.OpCompositeExtract(coord_type[1], coords, 0U);
        const auto slice = ctx.OpCompositeExtract(coord_type[1], coords, 1U);
        return ctx.OpCompositeConstruct(coord_type[3], x, zero, slice);
    }
    case AmdGpu::ImageType::Cube: {
        // Lowered to 2D array
        if (is_float) {
            const auto x = ctx.OpCompositeExtract(coord_type[1], coords, 0U);
            const auto y = ctx.OpCompositeExtract(coord_type[1], coords, 1U);
            const auto face = ctx.OpCompositeExtract(coord_type[1], coords, 2U);

            // AMD cube math results in coordinates in the range [1.0, 2.0]. We need
            // to convert this to the range [0.0, 1.0] to get correct results.
            const auto one = ctx.ConstF32(1.f);
            const auto fixed_x = ctx.OpFSub(coord_type[1], x, one);
            const auto fixed_y = ctx.OpFSub(coord_type[1], y, one);
            const auto fixed_face = ctx.OpFma(
                coord_type[1],
                ctx.OpFloor(coord_type[1], ctx.OpFDiv(coord_type[1], face, ctx.ConstF32(8.f))),
                ctx.ConstF32(-2.f), face);
            return ctx.OpCompositeConstruct(coord_type[3], fixed_x, fixed_y, fixed_face);
        }
        return coords;
    }
    default:
        return coords;
    }
}

template <bool is_float>
static Id PadImageOperand(EmitContext& ctx, Id operand, AmdGpu::ImageType image_type) {
    if (image_type == AmdGpu::ImageType::Color1D || image_type == AmdGpu::ImageType::Color1DArray) {
        const auto coord_type = is_float ? ctx.F32 : ctx.U32;
        const auto zero = is_float ? ctx.f32_zero_value : ctx.u32_zero_value;
        return ctx.OpCompositeConstruct(coord_type[2], operand, zero);
    }
    return operand;
}

struct ImageOperands {
    void Add(spv::ImageOperandsMask new_mask, Id value) {
        if (!Sirit::ValidId(value)) {
            return;
        }
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value);
    }
    void Add(spv::ImageOperandsMask new_mask, Id value1, Id value2) {
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value1);
        operands.push_back(value2);
    }

    void AddOffset(EmitContext& ctx, AmdGpu::ImageType image_type, const IR::Value& offset,
                   bool can_use_runtime_offsets = false) {
        if (offset.IsEmpty()) {
            return;
        }
        spv::ImageOperandsMask op_mask{};
        Id value{};
        if (offset.IsImmediate()) {
            op_mask = spv::ImageOperandsMask::ConstOffset;
            value = ctx.ConstS32(static_cast<s32>(offset.U32()));
        } else {
            IR::Inst* const inst{offset.Inst()};
            if (inst->AreAllArgsImmediates()) {
                switch (inst->GetOpcode()) {
                case IR::Opcode::CompositeConstructU32x2:
                    op_mask = spv::ImageOperandsMask::ConstOffset;
                    value = ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                         static_cast<s32>(inst->Arg(1).U32()));
                    break;
                case IR::Opcode::CompositeConstructU32x3:
                    op_mask = spv::ImageOperandsMask::ConstOffset;
                    value = ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                         static_cast<s32>(inst->Arg(1).U32()),
                                         static_cast<s32>(inst->Arg(2).U32()));
                    break;
                default:
                    break;
                }
            }
        }

        if (op_mask != spv::ImageOperandsMask::ConstOffset) {
            if (!can_use_runtime_offsets) {
                LOG_WARNING(Render_Vulkan,
                            "Runtime offset provided to unsupported image sample instruction");
                return;
            }
            op_mask = spv::ImageOperandsMask::Offset;
            value = ctx.Def(offset);
        }

        value = PadImageOperand<false>(ctx, value, image_type);
        Add(op_mask, value);
    }

    void AddDerivatives(EmitContext& ctx, AmdGpu::ImageType image_type, Id derivatives_dx,
                        Id derivatives_dy) {
        if (!Sirit::ValidId(derivatives_dx) || !Sirit::ValidId(derivatives_dy)) {
            return;
        }
        derivatives_dx = PadImageOperand<true>(ctx, derivatives_dx, image_type);
        derivatives_dy = PadImageOperand<true>(ctx, derivatives_dy, image_type);
        Add(spv::ImageOperandsMask::Grad, derivatives_dx, derivatives_dy);
    }

    spv::ImageOperandsMask mask{};
    boost::container::static_vector<Id, 4> operands;
};

Id EmitImageHandle(EmitContext& ctx, Id, Id) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitImageHeapIndex(EmitContext& ctx, u32 handle, Id index) {
    ctx.images[handle & 0xFFFF].heap_index = index;
}

void EmitSamplerHeapIndex(EmitContext& ctx, u32 handle, Id index) {
    ctx.samplers[handle].heap_index = index;
}

static Id LoadHeapDescriptor(EmitContext& ctx, Id type, Id heap, Id index) {
    const Id pointer_type = ctx.TypePointer(spv::StorageClass::UniformConstant, type);
    const Id pointer = ctx.OpAccessChain(pointer_type, heap, index);
    ctx.Decorate(pointer, spv::Decoration::NonUniform);
    const Id descriptor = ctx.OpLoad(type, pointer);
    ctx.Decorate(descriptor, spv::Decoration::NonUniform);
    return descriptor;
}

static Id LoadImage(EmitContext& ctx, const EmitContext::TextureDefinition& texture) {
    if (!texture.is_heap) {
        return ctx.OpLoad(texture.image_type, texture.id);
    }
    return LoadHeapDescriptor(ctx, texture.image_type, texture.id, texture.heap_index);
}

static Id LoadSampler(EmitContext& ctx, const EmitContext::SamplerDefinition& sampler) {
    if (!sampler.is_heap) {
        return ctx.OpLoad(ctx.sampler_type, sampler.id);
    }
    return LoadHeapDescriptor(ctx, ctx.sampler_type, sampler.id, sampler.heap_index);
}

static Id SampledImage(EmitContext& ctx, u32 handle, const EmitContext::TextureDefinition& texture,
                       Id image) {
    const auto& sampler = ctx.samplers[handle >> 16];
    const Id sampled_image =
        ctx.OpSampledImage(texture.sampled_type, image, LoadSampler(ctx, sampler));
    if (texture.is_heap || sampler.is_heap) {
        ctx.Decorate(sampled_image, spv::Decoration::NonUniform);
    }
    return sampled_image;
}

/// A sampler whose force_unnormalized bit the shader sets takes its coordinates in texels.
/// Vulkan's unnormalized samplers are too restricted to map that onto, so the coordinates
/// are divided by the size of the base level here instead.
static Id NormalizeCoords(EmitContext& ctx, u32 handle, const auto& texture, Id image,
                          Id coords) {
    const u32 sampler_index = handle >> 16;
    if (sampler_index >= ctx.info.samplers.size() ||
        ctx.info.samplers[sampler_index].post_op != SharpFetchPostOp::ForceUnnormalizedCoords ||
        texture.view_type != AmdGpu::ImageType::Color2D) {
        return coords;
    }
    const Id size = ctx.OpImageQuerySizeLod(ctx.S32[2], image, ctx.ConstU32(0u));
    return ctx.OpFDiv(ctx.F32[2], coords, ctx.OpConvertSToF(ctx.F32[2], size));
}

static bool IsConstantOffset(const IR::Value& offset) {
    if (offset.IsEmpty() || offset.IsImmediate()) {
        return true;
    }
    const IR::Inst* const inst{offset.Inst()};
    return inst->AreAllArgsImmediates() &&
           (inst->GetOpcode() == IR::Opcode::CompositeConstructU32x2 ||
            inst->GetOpcode() == IR::Opcode::CompositeConstructU32x3);
}

/// Sample instructions only take constant offsets, so an offset known at run time moves the
/// coordinates by that many texels of the sampled level instead.
static Id OffsetCoords(EmitContext& ctx, const EmitContext::TextureDefinition& texture, Id image,
                       Id sampled_image, Id coords, const IR::Value& offset, Id lod) {
    const auto view_type = texture.view_type;
    if (IsConstantOffset(offset) || view_type == AmdGpu::ImageType::Cube ||
        view_type == AmdGpu::ImageType::Color2DMsaa) {
        return coords;
    }
    const bool is_3d = view_type == AmdGpu::ImageType::Color3D;
    const bool is_array = view_type == AmdGpu::ImageType::Color1DArray ||
                          view_type == AmdGpu::ImageType::Color2DArray;
    const u32 num_dims = is_3d ? 3 : 2;
    Id level = ctx.ConstS32(0);
    if (Sirit::ValidId(lod)) {
        level = ctx.OpConvertFToS(ctx.S32[1], lod);
    } else if (ctx.info.hw_stage == HwStage::Fragment) {
        const Id lod_coords =
            is_array ? ctx.OpVectorShuffle(ctx.F32[2], coords, coords, 0, 1) : coords;
        const Id query = ctx.OpImageQueryLod(ctx.F32[2], sampled_image, lod_coords);
        level = ctx.OpConvertFToS(ctx.S32[1], ctx.OpCompositeExtract(ctx.F32[1], query, 1U));
    }
    Id size = ctx.OpImageQuerySizeLod(ctx.S32[num_dims + (is_array ? 1 : 0)], image, level);
    if (is_array) {
        size = ctx.OpVectorShuffle(ctx.S32[2], size, size, 0, 1);
    }
    const Id texels = PadImageOperand<false>(ctx, ctx.Def(offset), view_type);
    const Id delta = ctx.OpFDiv(ctx.F32[num_dims], ctx.OpConvertSToF(ctx.F32[num_dims], texels),
                                ctx.OpConvertSToF(ctx.F32[num_dims], size));
    if (!is_array) {
        return ctx.OpFAdd(ctx.F32[num_dims], coords, delta);
    }
    const Id moved =
        ctx.OpFAdd(ctx.F32[2], ctx.OpVectorShuffle(ctx.F32[2], coords, coords, 0, 1), delta);
    return ctx.OpVectorShuffle(ctx.F32[3], moved, coords, 0, 1, 4);
}

Id EmitImageSampleRaw(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address1, Id address2,
                      Id address3, Id address4) {
    UNREACHABLE_MSG("Unreachable instruction");
}

Id EmitImageSampleImplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id bias,
                              const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = LoadImage(ctx, texture);
    const Id result_type = texture.data_types->Get(4);
    const Id sampled_image = SampledImage(ctx, handle, texture, image);
    const Id fixed_coords =
        OffsetCoords(ctx, texture, image, sampled_image,
                     NormalizeCoords(ctx, handle, texture, image,
                                     FixImageCoords<true>(ctx, coords, texture.view_type)),
                     offset, {});
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    if (IsConstantOffset(offset)) {
        operands.AddOffset(ctx, texture.view_type, offset);
    }
    const Id sample = ctx.OpImageSampleImplicitLod(result_type, sampled_image, fixed_coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageSampleExplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod,
                              const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = LoadImage(ctx, texture);
    const Id result_type = texture.data_types->Get(4);
    const Id sampled_image = SampledImage(ctx, handle, texture, image);
    const Id fixed_coords =
        OffsetCoords(ctx, texture, image, sampled_image,
                     NormalizeCoords(ctx, handle, texture, image,
                                     FixImageCoords<true>(ctx, coords, texture.view_type)),
                     offset, lod);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, lod);
    if (IsConstantOffset(offset)) {
        operands.AddOffset(ctx, texture.view_type, offset);
    }
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, fixed_coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageSampleDrefImplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id bias, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = LoadImage(ctx, texture);
    const Id result_type = texture.data_types->Get(1);
    const Id sampled_image = SampledImage(ctx, handle, texture, image);
    const Id fixed_coords =
        OffsetCoords(ctx, texture, image, sampled_image,
                     FixImageCoords<true>(ctx, coords, texture.view_type), offset, {});
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    if (IsConstantOffset(offset)) {
        operands.AddOffset(ctx, texture.view_type, offset);
    }
    const Id sample = ctx.OpImageSampleDrefImplicitLod(result_type, sampled_image, fixed_coords,
                                                       dref, operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

Id EmitImageSampleDrefExplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id lod, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = LoadImage(ctx, texture);
    const Id result_type = texture.data_types->Get(1);
    const Id sampled_image = SampledImage(ctx, handle, texture, image);
    const Id fixed_coords =
        OffsetCoords(ctx, texture, image, sampled_image,
                     FixImageCoords<true>(ctx, coords, texture.view_type), offset, lod);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, lod);
    if (IsConstantOffset(offset)) {
        operands.AddOffset(ctx, texture.view_type, offset);
    }
    const Id sample = ctx.OpImageSampleDrefExplicitLod(result_type, sampled_image, fixed_coords,
                                                       dref, operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

Id EmitImageGather(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                   const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = LoadImage(ctx, texture);
    const Id result_type = texture.data_types->Get(4);
    const Id sampled_image = SampledImage(ctx, handle, texture, image);
    const Id fixed_coords = NormalizeCoords(
        ctx, handle, texture, image, FixImageCoords<true>(ctx, coords, texture.view_type));
    const u32 comp = inst->Flags<IR::TextureInstInfo>().gather_comp.Value();
    ImageOperands operands;
    operands.AddOffset(ctx, texture.view_type, offset, true);
    const Id texels = ctx.OpImageGather(result_type, sampled_image, fixed_coords,
                                        ctx.ConstU32(comp), operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageGatherDref(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                       const IR::Value& offset, Id dref) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = LoadImage(ctx, texture);
    const Id result_type = texture.data_types->Get(4);
    const Id sampled_image = SampledImage(ctx, handle, texture, image);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.AddOffset(ctx, texture.view_type, offset, true);
    const Id texels = ctx.OpImageDrefGather(result_type, sampled_image, fixed_coords, dref,
                                            operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageQueryDimensions(EmitContext& ctx, IR::Inst* inst, u32 handle, Id lod, bool has_mips) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = LoadImage(ctx, texture);
    const Id zero = ctx.u32_zero_value;
    const auto mips{[&] { return has_mips ? ctx.OpImageQueryLevels(ctx.U32[1], image) : zero; }};
    const bool uses_lod{texture.view_type != AmdGpu::ImageType::Color2DMsaa && !texture.is_storage};
    const auto query{[&](Id type) {
        return uses_lod ? ctx.OpImageQuerySizeLod(type, image, lod)
                        : ctx.OpImageQuerySize(type, image);
    }};
    switch (texture.view_type) {
    case AmdGpu::ImageType::Color1D: {
        const auto width = ctx.OpCompositeExtract(ctx.U32[1], query(ctx.U32[2]), 0U);
        return ctx.OpCompositeConstruct(ctx.U32[4], width, zero, zero, mips());
    }
    case AmdGpu::ImageType::Color1DArray: {
        const auto base = query(ctx.U32[3]);
        const auto width = ctx.OpCompositeExtract(ctx.U32[1], base, 0U);
        const auto slices = ctx.OpCompositeExtract(ctx.U32[1], base, 2U);
        return ctx.OpCompositeConstruct(ctx.U32[4], width, slices, zero, mips());
    }
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[2]), zero, mips());
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube:
    case AmdGpu::ImageType::Color3D:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[3]), mips());
    default:
        UNREACHABLE_MSG("SPIR-V Instruction");
    }
}

Id EmitImageQueryLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = LoadImage(ctx, texture);
    const Id sampled_image = SampledImage(ctx, handle, texture, image);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    return ctx.OpImageQueryLod(ctx.F32[2], sampled_image, fixed_coords);
}

Id EmitImageGradient(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id derivatives_dx,
                     Id derivatives_dy, const IR::Value& offset, const IR::Value& lod_clamp) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = LoadImage(ctx, texture);
    const Id result_type = texture.data_types->Get(4);
    const Id sampled_image = SampledImage(ctx, handle, texture, image);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.AddDerivatives(ctx, texture.view_type, derivatives_dx, derivatives_dy);
    operands.AddOffset(ctx, texture.view_type, offset);
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, fixed_coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageRead(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id color_type = texture.data_types->Get(4);
    const Id fixed_coords = FixImageCoords<false>(ctx, coords, texture.view_type);
    ImageOperands operands;
    Id texel;
    if (!texture.is_storage) {
        const Id image = LoadImage(ctx, texture);
        if (texture.view_type == AmdGpu::ImageType::Color2DMsaa) {
            // GCN hardware wraps out-of-range MSAA sample indices
            if (Sirit::ValidId(ms)) {
                const Id sample_count = ctx.OpImageQuerySamples(ctx.U32[1], image);
                const Id wrapped_ms = ctx.OpUMod(ctx.U32[1], ms, sample_count);
                operands.Add(spv::ImageOperandsMask::Sample, wrapped_ms);
            }
        } else {
            if (Sirit::ValidId(ms)) {
                LOG_ERROR(Render_Recompiler, "image is not MS but ms operand is provided");
            }
            operands.Add(spv::ImageOperandsMask::Lod, lod);
            operands.Add(spv::ImageOperandsMask::Sample, ms);
        }
        texel = ctx.OpImageFetch(color_type, image, fixed_coords, operands.mask, operands.operands);
    } else {
        Id image_ptr = texture.id;
        if (ctx.profile.supports_image_load_store_lod) {
            operands.Add(spv::ImageOperandsMask::Lod, lod);
        } else if (Sirit::ValidId(lod)) {
#if 1
            // It's confusing what interactions will cause this code path so leave it as
            // unreachable until a case is found.
            // Normally IMAGE_LOAD_MIP should translate -> OpImageFetch
            UNREACHABLE_MSG("Unsupported ImageRead with Lod");
#else
            LOG_WARNING(Render, "Fallback for ImageRead with LOD");
            ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
            const Id single_image_ptr_type =
                ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
            image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
#endif
        }
        const Id image = ctx.OpLoad(texture.image_type, image_ptr);
        texel = ctx.OpImageRead(color_type, image, fixed_coords, operands.mask, operands.operands);
    }
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texel) : texel;
}

void EmitImageWrite(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms,
                    Id color) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    Id image_ptr = texture.id;
    const Id color_type = texture.data_types->Get(4);
    const Id fixed_coords = FixImageCoords<false>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Sample, ms);
    if (ctx.profile.supports_image_load_store_lod) {
        operands.Add(spv::ImageOperandsMask::Lod, lod);
    } else if (Sirit::ValidId(lod)) {
        LOG_WARNING(Render, "Fallback for ImageWrite with LOD");
        ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
        const Id single_image_ptr_type =
            ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
        image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
    }
    const Id image = ctx.OpLoad(texture.image_type, image_ptr);
    Id texel = texture.is_integer ? ctx.OpBitcast(color_type, color) : color;
    // 11/11/10 unsigned floats store negatives and NaN as 0 and overflow as 65024 on the PS4;
    // Vulkan leaves those undefined (Driveclub's compute post-processing writes them). The PS4
    // also rounds toward zero, which the host does once the dropped mantissa bits are cleared;
    // rounding to nearest keeps bright texels of feedback passes alive forever.
    const auto sharp = ctx.info.images[handle & 0xFFFF].GetSharp(ctx.info);
    const auto dfmt = sharp.GetDataFmt();
    if (!texture.is_integer && sharp.GetNumberFmt() == AmdGpu::NumberFormat::Float &&
        (dfmt == AmdGpu::DataFormat::Format10_11_11 ||
         dfmt == AmdGpu::DataFormat::Format11_11_10)) {
        std::array<Id, 4> parts;
        for (u32 i = 0; i < 4; ++i) {
            const Id part = ctx.OpCompositeExtract(ctx.F32[1], texel, i);
            if (i == 3) {
                parts[i] = part;
                continue;
            }
            const Id clamped = ctx.OpSelect(
                ctx.F32[1], ctx.OpFOrdGreaterThan(ctx.U1[1], part, ctx.ConstF32(0.f)),
                ctx.OpFMin(ctx.F32[1], part, ctx.ConstF32(65024.f)), ctx.ConstF32(0.f));
            const u32 kept_bits_mask = i == 2 ? 0xFFFC0000u : 0xFFFE0000u;
            parts[i] = ctx.OpBitcast(
                ctx.F32[1], ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpBitcast(ctx.U32[1], clamped),
                                             ctx.ConstU32(kept_bits_mask)));
        }
        texel = ctx.OpCompositeConstruct(ctx.F32[4], parts);
    }
    ctx.OpImageWrite(image, fixed_coords, texel, operands.mask, operands.operands);
}

static Id SelectCubeResult(EmitContext& ctx, Id x, Id y, Id z, Id x_res, Id y_res, Id z_res) {
    const auto abs_x = ctx.OpFAbs(ctx.F32[1], x);
    const auto abs_y = ctx.OpFAbs(ctx.F32[1], y);
    const auto abs_z = ctx.OpFAbs(ctx.F32[1], z);

    const auto z_face_cond{ctx.OpLogicalAnd(ctx.U1[1],
                                            ctx.OpFOrdGreaterThanEqual(ctx.U1[1], abs_z, abs_x),
                                            ctx.OpFOrdGreaterThanEqual(ctx.U1[1], abs_z, abs_y))};
    const auto y_face_cond{ctx.OpFOrdGreaterThanEqual(ctx.U1[1], abs_y, abs_x)};

    return ctx.OpSelect(ctx.F32[1], z_face_cond, z_res,
                        ctx.OpSelect(ctx.F32[1], y_face_cond, y_res, x_res));
}

Id EmitCubeFaceIndex(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    if (ctx.profile.supports_native_cube_calc) {
        return ctx.OpCubeFaceIndexAMD(ctx.F32[1], ctx.OpCompositeConstruct(ctx.F32[3], x, y, z));
    }

    const auto x_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], x, ctx.f32_zero_value)};
    const auto y_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], y, ctx.f32_zero_value)};
    const auto z_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], z, ctx.f32_zero_value)};
    const auto x_face{ctx.OpSelect(ctx.F32[1], x_neg_cond, ctx.ConstF32(1.f), ctx.ConstF32(0.f))};
    const auto y_face{ctx.OpSelect(ctx.F32[1], y_neg_cond, ctx.ConstF32(3.f), ctx.ConstF32(2.f))};
    const auto z_face{ctx.OpSelect(ctx.F32[1], z_neg_cond, ctx.ConstF32(5.f), ctx.ConstF32(4.f))};

    return SelectCubeResult(ctx, x, y, z, x_face, y_face, z_face);
}

Id EmitCubeFaceCoordS(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    const auto x_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], x, ctx.f32_zero_value)};
    const auto z_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], z, ctx.f32_zero_value)};
    const auto x_sc{ctx.OpSelect(ctx.F32[1], x_neg_cond, z, ctx.OpFNegate(ctx.F32[1], z))};
    const auto y_sc{x};
    const auto z_sc{ctx.OpSelect(ctx.F32[1], z_neg_cond, ctx.OpFNegate(ctx.F32[1], x), x)};

    return SelectCubeResult(ctx, x, y, z, x_sc, y_sc, z_sc);
}

Id EmitCubeFaceCoordT(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    const auto y_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], y, ctx.f32_zero_value)};
    const auto x_z_tc{ctx.OpFNegate(ctx.F32[1], y)};
    const auto y_tc{ctx.OpSelect(ctx.F32[1], y_neg_cond, ctx.OpFNegate(ctx.F32[1], z), z)};

    return SelectCubeResult(ctx, x, y, z, x_z_tc, y_tc, x_z_tc);
}

Id EmitCubeFaceMajorAxis(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    const auto two{ctx.ConstF32(2.f)};
    const auto x_major_axis{ctx.OpFMul(ctx.F32[1], x, two)};
    const auto y_major_axis{ctx.OpFMul(ctx.F32[1], y, two)};
    const auto z_major_axis{ctx.OpFMul(ctx.F32[1], z, two)};

    return SelectCubeResult(ctx, x, y, z, x_major_axis, y_major_axis, z_major_axis);
}

} // namespace Shader::Backend::SPIRV
