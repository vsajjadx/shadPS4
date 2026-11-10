// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/passes/workgroup_uniformity.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block, bool subgroup = false) {
    const auto emit_barrier = [subgroup](IR::IREmitter& ir) {
        if (subgroup) {
            ir.SubgroupBarrier();
        } else {
            ir.Barrier();
        }
    };
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                emit_barrier(ir);
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                emit_barrier(ir);
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        // IR control flow lives in the ASL, not in a terminator instruction.
        // Inserting before back() can leave a final LDS store unpublished.
        IR::IREmitter ir{*block, block->end()};
        emit_barrier(ir);
    }
}

static void EmitBarrierAtBlockStart(IR::Block* block) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    IR::IREmitter ir{*block, insert_point};
    ir.Barrier();
}

// GCN waves run in lockstep, so games load from a buffer what another invocation of the wave
// stored without a barrier. Inserts one after such stores.
static void EmitBufferBarriers(IR::Program& program,
                               const std::vector<IR::Block*>& uniform_blocks) {
    using Type = IR::AbstractSyntaxNode::Type;
    // Index of the last load from each buffer, counting instructions in program order.
    std::array<u32, NUM_BUFFERS> last_load{};
    u32 index{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::Block) {
            for (const IR::Inst& inst : node.data.block->Instructions()) {
                ++index;
                if (IsBufferInstruction(inst) && !IsBufferStore(inst)) {
                    last_load[inst.Arg(0).U32()] = index;
                }
            }
        }
    }
    std::vector<u32> loops;
    bool pending_divergent_store{};
    index = 0;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::Loop) {
            loops.push_back(index);
        } else if (node.type == Type::Repeat) {
            loops.pop_back();
        } else if (node.type == Type::Block) {
            const bool is_uniform = std::ranges::contains(uniform_blocks, node.data.block);
            const bool publish_pending = is_uniform && pending_divergent_store;
            if (publish_pending) {
                pending_divergent_store = false;
            }
            const auto emit_barrier = [&](IR::Block::iterator insert_point) {
                IR::IREmitter{*node.data.block, insert_point}.Barrier(true);
            };
            // Last store to a buffer that is loaded later, in a loop also on the next iteration.
            IR::Inst* store{};
            for (IR::Inst& inst : node.data.block->Instructions()) {
                ++index;
                if (store && IsBufferInstruction(inst) && !IsBufferStore(inst)) {
                    emit_barrier(IR::Block::InstructionList::s_iterator_to(inst));
                    store = nullptr;
                } else if (IsBufferStore(inst) && !IsBufferAtomic(inst) &&
                           last_load[inst.Arg(0).U32()] > (loops.empty() ? index : loops.front())) {
                    if (is_uniform) {
                        store = &inst;
                    } else {
                        pending_divergent_store = true;
                    }
                }
            }
            if (store) {
                emit_barrier(std::next(IR::Block::InstructionList::s_iterator_to(*store)));
            }
            if (publish_pending) {
                // Stores in a partial wave become visible at its uniform merge,
                // never at a rendezvous inside the divergent producer block.
                emit_barrier(std::ranges::find_if_not(node.data.block->Instructions(), IR::IsPhi));
            }
        }
    }
}

static constexpr u32 GcnSubgroupSize = 64;

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto uniform_blocks = FindWorkgroupUniformBlocks(program);
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    if (threadgroup_size == GcnSubgroupSize && profile.subgroup_size != GcnSubgroupSize) {
        EmitBufferBarriers(program, uniform_blocks);
    }
    if (shared_memory_size == 0) {
        return;
    }
    // A 32-lane host subgroup cannot order accesses across a 64-lane GCN wave. In particular,
    // NHL 24's 64-thread lighting, exposure and HiZ passes rely on the two halves of a wave
    // observing each other's LDS writes. A subgroup barrier only orders each half independently,
    // leaving stale exposure values and incomplete depth/light data on NVIDIA GPUs.
    const bool splits_wave =
        threadgroup_size == GcnSubgroupSize && profile.subgroup_size < GcnSubgroupSize;
    const bool large_group = threadgroup_size > GcnSubgroupSize && profile.needs_lds_barriers;
    if ((!splits_wave && !large_group) || !profile.needs_lds_barriers) {
        // A host wave at least as wide as the guest wave preserves its implicit lockstep. The
        // compiler may still reorder the accesses, so retain subgroup-scoped memory ordering.
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            if (node.type == IR::AbstractSyntaxNode::Type::Block) {
                EmitBarrierInBlock(node.data.block, true);
            }
        }
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type != Type::Block) {
            continue;
        }
        const bool is_uniform = std::ranges::contains(uniform_blocks, node.data.block);
        if (large_group || !is_uniform) {
            // Never rendezvous independent guest waves or partially active invocations
            // at a workgroup execution barrier. Retain local memory ordering instead.
            EmitBarrierInBlock(node.data.block, true);
            continue;
        }
        if (node.data.block != program.blocks.front()) {
            // Publish stores from preceding divergent blocks only after reconvergence.
            // Uniformity also excludes merges reached after a divergent early return.
            EmitBarrierAtBlockStart(node.data.block);
        }
        EmitBarrierInBlock(node.data.block);
    }
}

} // namespace Shader::Optimization
