// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <string_view>
#include <vector>
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

// All passes introducing workgroup execution barriers must use the same proof.
// Lane-only checks miss memory-dependent branches and divergent early returns.
// A loop's barriers need every thread of the workgroup to run it the same number of times, so its
// exits must not depend on anything that can differ between threads or waves: lane and invocation
// ids, exec and vcc masks, subgroup operations, or memory the threads load or write.
inline bool IsWorkgroupVaryingCondition(const IR::U1& condition) {
    if (condition.IsImmediate()) {
        return false;
    }
    return IR::BreadthFirstSearch(
               condition.Inst(),
               [](const IR::Inst* inst) -> std::optional<bool> {
                   const IR::Opcode opcode = inst->GetOpcode();
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
                if_stack.push_back(IsWorkgroupVaryingCondition(node.data.if_node.cond));
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
                    (divergent_ifs > 0 || IsWorkgroupVaryingCondition(node.data.break_node.cond))) {
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
                if (IsWorkgroupVaryingCondition(node.data.repeat.cond)) {
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
            const bool divergent = IsWorkgroupVaryingCondition(node.data.if_node.cond);
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

} // namespace Shader::Optimization
