// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

Id SubgroupScope(EmitContext& ctx) {
    return ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup));
}

// Which vertices share a wave is up to the hardware, so a vertex shader cannot depend on its
// neighbours and running each invocation as a wave of its own is a valid GCN execution. Host
// subgroups do not keep the lanes of a readlane loop over the exec mask together, which made
// some iterations read another vertex's value.
static bool IsSingleLane(const EmitContext& ctx) {
    return ctx.info.sw_stage == SwStage::Vertex;
}

Id EmitWarpId(EmitContext& ctx) {
    UNREACHABLE();
}

Id EmitLaneId(EmitContext& ctx) {
    if (IsSingleLane(ctx)) {
        return ctx.u32_zero_value;
    }
    return ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
}

Id EmitQuadBroadcast(EmitContext& ctx, Id value, Id index) {
    if (IsSingleLane(ctx)) {
        return value;
    }
    return ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], SubgroupScope(ctx), value, index);
}

Id EmitReadFirstLane(EmitContext& ctx, Id value) {
    if (IsSingleLane(ctx)) {
        return value;
    }
    return ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], SubgroupScope(ctx), value);
}

Id EmitShuffle(EmitContext& ctx, Id value, Id index) {
    if (IsSingleLane(ctx)) {
        return value;
    }
    return ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, index);
}

Id EmitShuffleXor(EmitContext& ctx, Id value, Id mask) {
    if (IsSingleLane(ctx)) {
        return value;
    }
    return ctx.OpGroupNonUniformShuffleXor(ctx.U32[1], SubgroupScope(ctx), value, mask);
}

Id EmitReadLane(EmitContext& ctx, Id value, Id lane) {
    if (IsSingleLane(ctx)) {
        return value;
    }
    return ctx.OpGroupNonUniformBroadcast(ctx.U32[1], SubgroupScope(ctx), value, lane);
}

Id EmitWriteLane(EmitContext& ctx, Id value, Id write_value, u32 lane) {
    return ctx.u32_zero_value;
}

Id EmitBallot(EmitContext& ctx, Id bit) {
    if (IsSingleLane(ctx)) {
        return ctx.OpSelect(ctx.U64, bit, ctx.u64_one_value, ctx.u64_zero_value);
    }
    const Id ballot{ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), bit)};
    return ctx.OpBitcast(ctx.U64, ctx.OpVectorShuffle(ctx.U32[2], ballot, ballot, 0, 1));
}

Id EmitBallotFindLsb(EmitContext& ctx, Id mask) {
    const Id value{ctx.OpCompositeConstruct(ctx.U32[4], ctx.OpBitcast(ctx.U32[2], mask),
                                            ctx.u32_zero_value, ctx.u32_zero_value)};
    return ctx.OpGroupNonUniformBallotFindLSB(ctx.U32[1], SubgroupScope(ctx), value);
}

Id EmitInverseBallot(EmitContext& ctx, Id mask) {
    if (IsSingleLane(ctx)) {
        return ctx.OpINotEqual(ctx.U1[1], ctx.OpBitwiseAnd(ctx.U64, mask, ctx.u64_one_value),
                               ctx.u64_zero_value);
    }
    const Id value{ctx.OpCompositeConstruct(ctx.U32[4], ctx.OpBitcast(ctx.U32[2], mask),
                                            ctx.u32_zero_value, ctx.u32_zero_value)};
    return ctx.OpGroupNonUniformInverseBallot(ctx.U1[1], SubgroupScope(ctx), value);
}

Id EmitGroupAny(EmitContext& ctx, Id bit) {
    if (IsSingleLane(ctx)) {
        return bit;
    }
    return ctx.OpGroupNonUniformAny(ctx.U1[1], SubgroupScope(ctx), bit);
}

} // namespace Shader::Backend::SPIRV
