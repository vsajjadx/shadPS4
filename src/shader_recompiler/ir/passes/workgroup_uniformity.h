// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <string_view>
#include <vector>
#include "common/hack_features.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

// All passes introducing workgroup execution barriers must use the same proof.
// Lane-only checks miss memory-dependent branches and divergent early returns.
// A loop's barriers need every thread of the workgroup to run it the same number of times, so its
// exits must not depend on anything that can differ between threads or waves: lane and invocation
// ids, exec and vcc masks, subgroup operations, or memory the threads load or write.
//
// A load from a buffer the program never writes returns the same value to every thread that
// reads the same address, so it is uniform when its address is: Call of Duty: Black Ops Cold
// War's video decoder wraps its whole body in a branch on a per-workgroup table entry, and
// treating that load as varying left the two 32-lane halves of its 64-lane wave without the
// barriers their shared-memory exchange needs (scrambled 8x8 blocks on NVIDIA).
inline bool IsWorkgroupVaryingCondition(const IR::U1& condition,
                                        const std::vector<u32>* written_buffers = nullptr) {
    if (condition.IsImmediate()) {
        return false;
    }
    return IR::BreadthFirstSearch(
               condition.Inst(),
               [written_buffers](const IR::Inst* inst) -> std::optional<bool> {
                   const IR::Opcode opcode = inst->GetOpcode();
                   if (written_buffers && IsBufferInstruction(*inst) && !IsBufferStore(*inst) &&
                       !IsBufferAtomic(*inst) && inst->Arg(0).IsImmediate() &&
                       !std::ranges::contains(*written_buffers, inst->Arg(0).U32())) {
                       // Uniform if its address is: keep searching its arguments.
                       return std::nullopt;
                   }
                   if (opcode == IR::Opcode::Phi) {
                       // Different incoming constants can still select different values
                       // per invocation. A data-only walk cannot prove a phi uniform.
                       return true;
                   }
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
                   if (written_buffers && name.starts_with("Composite")) {
                       // CompositeShuffle* reorders vector components of one invocation; it
                       // is not a subgroup shuffle.
                       return std::nullopt;
                   }
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

inline std::vector<IR::Block*> FindWorkgroupUniformBlocks(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;

    // Buffers the program stores to (or writes atomically); a buffer index that is not an
    // immediate makes every buffer count as written.
    std::vector<u32> written_buffers;
    bool any_buffer_written_dynamically{};
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (IsBufferStore(inst) || IsBufferAtomic(inst)) {
                if (inst.Arg(0).IsImmediate()) {
                    written_buffers.push_back(inst.Arg(0).U32());
                } else {
                    any_buffer_written_dynamically = true;
                }
            }
        }
    }
    // Only Call of Duty: Black Ops Cold War uses these refinements. NHL 24 hung on its lighting
    // shader 0xa9657ebd with them (more of its blocks got workgroup barriers), so every other
    // game keeps the stricter proof.
    const std::vector<u32>* written =
        any_buffer_written_dynamically || !Common::HackFeatures::isCodColdWar ? nullptr
                                                                               : &written_buffers;
    const auto is_varying = [written](const IR::U1& cond) {
        return IsWorkgroupVaryingCondition(cond, written);
    };

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
                if_stack.push_back(is_varying(node.data.if_node.cond));
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
                    (divergent_ifs > 0 || is_varying(node.data.break_node.cond))) {
                    divergent_loops.push_back(loop_stack.back());
                }
                break;
            case Type::Unreachable:
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
                if (is_varying(node.data.repeat.cond)) {
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
            const bool divergent = is_varying(node.data.if_node.cond);
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
            if (loops.back().divergent && Common::HackFeatures::isFc27) {
                // The loop header precedes the Loop node in the syntax list but runs on every
                // iteration, after threads that took a divergent exit have left. A workgroup
                // barrier there deadlocks the remaining half-wave (FC 27 cs 0x5e4f7d97). Only
                // for FC 27: NHL 24 has such barriers in 9 shaders, never hangs on them (its
                // threads run equal iteration counts) and may need them for its LDS exchange.
                std::erase(blocks, node.data.repeat.loop_header);
            }
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

} // namespace Shader::Optimization
