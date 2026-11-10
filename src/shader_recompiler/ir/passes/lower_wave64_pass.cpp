// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/workgroup_uniformity.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (true || program.info.hw_stage != HwStage::Compute || profile.subgroup_size == 64) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    if (num_threads <= 32) {
        return;
    }

    std::vector<IR::Inst*> worklist;
    const auto uniform_blocks = FindWorkgroupUniformBlocks(program);
    bool needs_scratch{};
    for (IR::Block* block : program.blocks) {
        const bool is_uniform = std::ranges::contains(uniform_blocks, block);
        for (IR::Inst& inst : block->Instructions()) {
            const auto opcode = inst.GetOpcode();
            if (opcode == IR::Opcode::MaskedBitCount32) {
                // This is a per-invocation arithmetic operation, not a collective.
                // Its lane number must not reset at the host's 32-lane boundary.
                worklist.push_back(&inst);
            } else if (opcode == IR::Opcode::ReadLane || opcode == IR::Opcode::Ballot) {
                if (is_uniform) {
                    worklist.push_back(&inst);
                    needs_scratch = true;
                } else {
                    LOG_WARNING(Render_Recompiler,
                                "Cannot safely lower {} in non uniform control flow "
                                "(cs shader {:#x}, workgroup {}x{}x{})",
                                opcode, program.info.pgm_hash, size_x, size_y, size_z);
                }
            }
        }
    }
    if (worklist.empty()) {
        return;
    }

    const u32 scratch_base = Common::AlignUp(runtime_info.hw.cs.shared_memory_size, sizeof(u64));
    // One slot per invocation supports runtime-selected ReadLane sources. Separate
    // slots also avoid concurrent non-atomic stores to the same ballot word.
    const u32 scratch_size = needs_scratch ? Common::AlignUp(num_threads, 64) * sizeof(u32) : 0;
    const bool scratch_fits =
        !needs_scratch || scratch_base + scratch_size <= profile.max_shared_memory_size;
    if (needs_scratch && scratch_fits) {
        program.info.shared_memory_scratch_size =
            scratch_base + scratch_size - runtime_info.hw.cs.shared_memory_size;
    } else if (!scratch_fits) {
        LOG_WARNING(Render_Recompiler,
                    "Insufficient shared memory for wave64 lowering (cs shader {:#x})",
                    program.info.pgm_hash);
    }

    for (IR::Inst* inst : worklist) {
        if (inst->GetOpcode() != IR::Opcode::MaskedBitCount32 && !scratch_fits) {
            continue;
        }
        LOG_INFO(Render_Recompiler, "Lowering {} instruction for wave64 (cs shader {:#x})",
                 inst->GetOpcode(), program.info.pgm_hash);
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 wave_base = ir.BitwiseAnd(invocation_index, ir.Imm32(~63u));
        const auto scratch_offset = [&](const IR::U32& index) {
            return ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(index, ir.Imm32(2u)));
        };
        if (inst->GetOpcode() == IR::Opcode::Ballot) {
            const IR::U32 mask_low =
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
            ir.WriteShared(32, mask_low, scratch_offset(invocation_index));
            ir.Barrier();
            const IR::U32 low{ir.LoadShared(32, false, scratch_offset(wave_base))};
            const IR::U32 high_index = ir.IAdd(wave_base, ir.Imm32(32u));
            // The last guest wave may have no upper host subgroup. Do not read
            // uninitialized scratch in that case; inactive lanes have a zero ballot.
            const IR::U32 valid_high_index = ir.UMin(high_index, ir.Imm32(num_threads - 1));
            const IR::U32 high{ir.LoadShared(32, false, scratch_offset(valid_high_index))};
            const IR::U32 high_or_zero{ir.Select(
                ir.ILessThan(high_index, ir.Imm32(num_threads), false), high, ir.Imm32(0u))};
            const IR::U64 mask = ir.PackUint2x32(ir.CompositeConstruct(low, high_or_zero));
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(mask);
        } else if (inst->GetOpcode() == IR::Opcode::ReadLane) {
            // Constant-only lowering left dynamic source lanes 32..63 as invalid
            // native subgroup broadcasts. Publish every invocation's value, then
            // address the selected lane within its guest wave, not its host subgroup.
            ir.WriteShared(32, inst->Arg(0), scratch_offset(invocation_index));
            ir.Barrier();
            const IR::U32 lane = ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(63u));
            const IR::U32 source = ir.IAdd(wave_base, lane);
            const IR::U32 valid_source = ir.UMin(source, ir.Imm32(num_threads - 1));
            const IR::U32 value{ir.LoadShared(32, false, scratch_offset(valid_source))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::MaskedBitCount32) {
            const IR::U32 subgroup_invocation_id = ir.BitwiseAnd(invocation_index, ir.Imm32(63));
            const IR::U64 mask = ir.ISub(
                ir.ShiftLeftLogical(ir.Imm64(u64{1}), subgroup_invocation_id), ir.Imm64(u64{1}));
            const IR::U32 thread_mask{
                ir.CompositeExtract(ir.UnpackUint2x32(mask), inst->Arg(2).U1() ? 1u : 0u)};
            const IR::U32 masked_value{
                ir.BitCount(ir.BitwiseAnd(IR::U32{inst->Arg(0)}, thread_mask))};
            inst->ReplaceUsesWithAndRemove(ir.IAdd(masked_value, IR::U32{inst->Arg(1)}));
        }
    }
}

} // namespace Shader::Optimization
