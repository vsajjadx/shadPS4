// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

// A loop's barriers need every thread of the workgroup to run it the same number of times, so its
// exits must not depend on anything that can differ between threads or waves: lane and invocation
// ids, exec and vcc masks, subgroup operations, or memory the threads load or write.
static bool IsVaryingCondition(const IR::U1& condition) {
    if (condition.IsImmediate()) {
        return false;
    }
    return IR::BreadthFirstSearch(
               condition.Inst(),
               [](const IR::Inst* inst) -> std::optional<bool> {
                   const IR::Opcode opcode = inst->GetOpcode();
                   if (opcode == IR::Opcode::GetAttributeU32) {
                       if (inst->Arg(0).Attribute() != IR::Attribute::WorkgroupId) {
                           return true;
                       }
                       return std::nullopt;
                   }
                   if (opcode == IR::Opcode::ReadConstBuffer) {
                       return std::nullopt;
                   }
                   const std::string_view name = IR::NameOf(opcode);
                   for (const std::string_view varying :
                        {"Lane", "Ballot", "Shuffle", "Quad", "GroupAny", "Shared", "Buffer",
                         "Image", "Attribute", "Exec", "Vcc", "ThreadBit"}) {
                       if (name.find(varying) != std::string_view::npos) {
                           return true;
                       }
                   }
                   return std::nullopt;
               })
        .value_or(false);
}

static std::vector<IR::Block*> FindUniformBlocks(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;

    struct ConditionalScope {
        const IR::Block* merge;
        bool divergent;
    };

    // A loop whose every exit condition is uniform runs the same number of iterations on all
    // threads of the workgroup, so its body may still host the barriers.
    std::vector<const IR::Block*> divergent_loops;
    {
        std::vector<const IR::Block*> loop_stack;
        std::vector<bool> if_stack;
        u32 divergent_ifs{};
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            switch (node.type) {
            case Type::If:
                if_stack.push_back(IsVaryingCondition(node.data.if_node.cond));
                divergent_ifs += static_cast<u32>(if_stack.back());
                break;
            case Type::EndIf:
                if (if_stack.empty()) {
                    return {};
                }
                divergent_ifs -= static_cast<u32>(if_stack.back());
                if_stack.pop_back();
                break;
            case Type::Loop:
                loop_stack.push_back(node.data.loop.merge);
                break;
            case Type::Break:
                if (!loop_stack.empty() &&
                    (divergent_ifs > 0 || IsVaryingCondition(node.data.break_node.cond))) {
                    divergent_loops.push_back(loop_stack.back());
                }
                break;
            case Type::Return:
                // A divergent early exit can strand the other half-wave at any later
                // workgroup barrier, including a barrier in a previous loop iteration.
                if (divergent_ifs > 0 || !loop_stack.empty()) {
                    return {};
                }
                break;
            case Type::Repeat:
                if (loop_stack.empty() || loop_stack.back() != node.data.repeat.merge) {
                    return {};
                }
                if (IsVaryingCondition(node.data.repeat.cond)) {
                    divergent_loops.push_back(loop_stack.back());
                }
                loop_stack.pop_back();
                break;
            default:
                break;
            }
        }
        if (!loop_stack.empty()) {
            return {};
        }
    }

    std::vector<IR::Block*> blocks;
    std::vector<ConditionalScope> conditionals;
    std::vector<ConditionalScope> loops;
    u32 divergence_depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsVaryingCondition(node.data.if_node.cond);
            conditionals.push_back({node.data.if_node.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::EndIf:
            ASSERT(!conditionals.empty() && conditionals.back().merge == node.data.end_if.merge);
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Loop: {
            const bool divergent = std::ranges::contains(divergent_loops, node.data.loop.merge);
            loops.push_back({node.data.loop.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::Repeat:
            if (loops.empty() || loops.back().merge != node.data.repeat.merge) {
                return {};
            }
            divergence_depth -= static_cast<u32>(loops.back().divergent);
            loops.pop_back();
            break;
        case Type::Block:
            if (divergence_depth == 0) {
                blocks.push_back(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty() || !loops.empty()) {
        return {};
    }
    return blocks;
}

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || profile.subgroup_size == 64) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    if (num_threads <= 32) {
        return;
    }

    std::vector<IR::Inst*> worklist;
    const auto uniform_blocks = FindUniformBlocks(program);
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
