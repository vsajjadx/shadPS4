// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <unordered_map>
#include <gtest/gtest.h>

#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

namespace {
using namespace Shader;

class Wave64LoweringTest : public ::testing::Test {
protected:
    Info info{};
    Pools pools;
    IR::Program program{info};
    RuntimeInfo runtime{};
    Profile profile{};
    IR::Block* block{};

    void SetUp() override {
        info.hw_stage = HwStage::Compute;
        info.sw_stage = SwStage::Compute;
        runtime.Initialize(HwStage::Compute, SwStage::Compute);
        runtime.hw.cs.workgroup_size = {64, 1, 1};
        profile.subgroup_size = 32;
        profile.max_shared_memory_size = 65536;
        block = NewBlock();
        AddBlock(block);
    }

    IR::Block* NewBlock() {
        auto* result = pools.block_pool.Create(pools.inst_pool);
        program.blocks.push_back(result);
        return result;
    }

    void AddBlock(IR::Block* target) {
        IR::AbstractSyntaxNode node{};
        node.type = IR::AbstractSyntaxNode::Type::Block;
        node.data.block = target;
        program.syntax_list.push_back(node);
    }

    IR::Block* AddIf(const IR::U1& condition) {
        auto* body = NewBlock();
        auto* merge = NewBlock();
        IR::AbstractSyntaxNode node{};
        node.type = IR::AbstractSyntaxNode::Type::If;
        node.data.if_node = {condition, body, merge};
        program.syntax_list.push_back(node);
        AddBlock(body);
        node.type = IR::AbstractSyntaxNode::Type::EndIf;
        node.data.end_if = {merge};
        program.syntax_list.push_back(node);
        AddBlock(merge);
        return body;
    }

    void Lower() {
        Optimization::LowerWave64BallotPass(program, runtime, profile);
    }

    void InsertSharedMemoryBarriers() {
        Optimization::SharedMemoryBarrierPass(program, runtime, profile);
    }

    size_t Count(IR::Opcode opcode) const {
        size_t count{};
        for (const auto* b : program.blocks) {
            for (const auto& inst : b->Instructions()) {
                count += inst.GetOpcode() == opcode;
            }
        }
        return count;
    }

    // Execute the lowered straight-line IR in lockstep, including native wave32
    // ballots and LDS exchanges. No Vulkan device or game data is required.
    std::vector<u64> Execute(const IR::Inst* result, u32 threads) {
        std::unordered_map<const IR::Inst*, std::vector<u64>> values;
        std::unordered_map<u32, u64> scratch;
        const auto arg = [&](const IR::Value& value, u32 lane) -> u64 {
            if (!value.IsImmediate()) {
                return values.at(value.Inst()).at(lane);
            }
            if (value.Type() == IR::Type::U1) {
                return value.U1();
            }
            return value.Type() == IR::Type::U64 ? value.U64() : value.U32();
        };
        for (const auto& inst : block->Instructions()) {
            auto& out = values[&inst];
            out.resize(threads);
            for (u32 lane = 0; lane < threads; ++lane) {
                const auto a = [&](size_t i) { return arg(inst.Arg(i), lane); };
                switch (inst.GetOpcode()) {
                case IR::Opcode::GetAttributeU32:
                    EXPECT_EQ(inst.Arg(0).Attribute(), IR::Attribute::LocalInvocationIndex);
                    out[lane] = lane;
                    break;
                case IR::Opcode::IAdd32:
                    out[lane] = u32(a(0) + a(1));
                    break;
                case IR::Opcode::ISub64:
                    out[lane] = a(0) - a(1);
                    break;
                case IR::Opcode::BitwiseAnd64:
                case IR::Opcode::BitwiseAnd32:
                    out[lane] = a(0) & a(1);
                    break;
                case IR::Opcode::BitwiseXor32:
                    out[lane] = a(0) ^ a(1);
                    break;
                case IR::Opcode::ShiftLeftLogical32:
                    out[lane] = u32(a(0) << a(1));
                    break;
                case IR::Opcode::ShiftLeftLogical64:
                    out[lane] = a(0) << a(1);
                    break;
                case IR::Opcode::UMin32:
                    out[lane] = std::min(a(0), a(1));
                    break;
                case IR::Opcode::ULessThan32:
                    out[lane] = a(0) < a(1);
                    break;
                case IR::Opcode::SelectU32:
                    out[lane] = a(0) ? a(1) : a(2);
                    break;
                case IR::Opcode::Ballot:
                    for (u32 source = lane & ~31u; source < std::min((lane & ~31u) + 32, threads);
                         ++source) {
                        out[lane] |= arg(inst.Arg(0), source) << (source & 31);
                    }
                    break;
                case IR::Opcode::UnpackUint2x32:
                case IR::Opcode::PackUint2x32:
                    out[lane] = a(0);
                    break;
                case IR::Opcode::CompositeExtractU32x2:
                    out[lane] = u32(a(0) >> (32 * a(1)));
                    break;
                case IR::Opcode::CompositeConstructU32x2:
                    out[lane] = a(0) | (a(1) << 32);
                    break;
                case IR::Opcode::WriteSharedU32:
                    scratch[u32(a(0))] = a(1);
                    break;
                case IR::Opcode::LoadSharedU32:
                    out[lane] = scratch.at(u32(a(0)));
                    break;
                case IR::Opcode::BitCount32:
                    out[lane] = std::popcount(u32(a(0)));
                    break;
                case IR::Opcode::Void:
                case IR::Opcode::Barrier:
                    break;
                default:
                    ADD_FAILURE() << "Unexpected opcode " << IR::NameOf(inst.GetOpcode());
                    return {};
                }
            }
        }
        return values.at(result);
    }
};

TEST_F(Wave64LoweringTest, DynamicReadLaneCrossesHostSubgroupsAndKeepsGuestWavesSeparate) {
    runtime.hw.cs.workgroup_size = {128, 1, 1};
    IR::IREmitter ir{*block};
    const auto id = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
    const auto source = ir.BitwiseXor(id, ir.Imm32(32));
    const auto result = ir.IAdd(ir.ReadLane(ir.IAdd(id, ir.Imm32(1000)), source), ir.Imm32(0));
    Lower();
    EXPECT_EQ(Count(IR::Opcode::ReadLane), 0);
    EXPECT_EQ(Count(IR::Opcode::Barrier), 2);
    const auto values = Execute(result.Inst(), 128);
    ASSERT_EQ(values.size(), 128);
    for (u32 lane = 0; lane < 128; ++lane) {
        EXPECT_EQ(values[lane], (lane ^ 32u) + 1000) << lane;
    }
}

TEST_F(Wave64LoweringTest, ConstantReadLaneUsesHighHalfAndMasksSourceToSixBits) {
    IR::IREmitter ir{*block};
    const auto id = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
    const auto result = ir.IAdd(ir.ReadLane(id, ir.Imm32(127)), ir.Imm32(0));
    Lower();
    for (const auto value : Execute(result.Inst(), 64)) {
        EXPECT_EQ(value, 63);
    }
}

TEST_F(Wave64LoweringTest, BallotLowersEvenWithoutAnUnpackConsumer) {
    IR::IREmitter ir{*block};
    const auto mask = ir.Ballot(ir.Imm1(true));
    // A raw U64 consumer used to be omitted from the worklist.
    const IR::U64 result{ir.BitwiseAnd(mask, ir.Imm64(~u64{0}))};
    Lower();
    EXPECT_EQ(Count(IR::Opcode::Barrier), 2);
    EXPECT_EQ(Count(IR::Opcode::WriteSharedU32), 1);
    for (const auto value : Execute(result.Inst(), 64)) {
        EXPECT_EQ(value, ~u64{0});
    }
}

TEST_F(Wave64LoweringTest, PartialLastWaveDoesNotReadUninitializedHighBallot) {
    runtime.hw.cs.workgroup_size = {96, 1, 1};
    IR::IREmitter ir{*block};
    const auto result = ir.UnpackUint2x32(ir.Ballot(ir.Imm1(true)));
    Lower();
    const auto values = Execute(result.Inst(), 96);
    ASSERT_EQ(values.size(), 96);
    for (u32 lane = 0; lane < 96; ++lane) {
        EXPECT_EQ(values[lane], lane < 64 ? ~u64{0} : u64{0xffffffff});
    }
}

TEST_F(Wave64LoweringTest, MaskedBitCountsUseGuestLaneAndNeedNoScratch) {
    IR::IREmitter ir{*block};
    const auto low = ir.MaskedBitCount(ir.Imm32(~0u), ir.Imm32(0), false);
    const auto result = ir.MaskedBitCount(ir.Imm32(~0u), low, true);
    const auto sink = ir.IAdd(result, ir.Imm32(0));
    Lower();
    EXPECT_EQ(Count(IR::Opcode::MaskedBitCount32), 0);
    EXPECT_EQ(Count(IR::Opcode::Barrier), 0);
    EXPECT_EQ(info.shared_memory_scratch_size, 0);
    const auto values = Execute(sink.Inst(), 64);
    ASSERT_EQ(values.size(), 64);
    for (u32 lane = 0; lane < 64; ++lane) {
        EXPECT_EQ(values[lane], lane);
    }
}

TEST_F(Wave64LoweringTest, DivergentReadLaneNeverInjectsWorkgroupBarriers) {
    IR::IREmitter entry{*block};
    const auto id = entry.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
    auto* body = AddIf(entry.ILessThan(id, entry.Imm32(32), false));
    IR::IREmitter ir{*body};
    const auto result = ir.ReadLane(ir.Imm32(7), ir.Imm32(63));
    const auto sink = ir.IAdd(result, ir.Imm32(0));
    (void)sink;
    Lower();
    EXPECT_EQ(Count(IR::Opcode::ReadLane), 1);
    EXPECT_EQ(Count(IR::Opcode::Barrier), 0);
    EXPECT_EQ(info.shared_memory_scratch_size, 0);
}

TEST_F(Wave64LoweringTest, LoadedConditionIsNotAssumedWorkgroupUniform) {
    IR::IREmitter entry{*block};
    const IR::U32 loaded{entry.LoadShared(32, false, entry.Imm32(0))};
    auto* body = AddIf(entry.INotEqual(loaded, entry.Imm32(0)));
    IR::IREmitter ir{*body};
    const auto sink = ir.IAdd(ir.ReadLane(ir.Imm32(7), ir.Imm32(63)), ir.Imm32(0));
    (void)sink;
    Lower();
    EXPECT_EQ(Count(IR::Opcode::Barrier), 0);
}

TEST_F(Wave64LoweringTest, DivergentEarlyReturnPreventsLaterBarriers) {
    IR::IREmitter entry{*block};
    const auto id = entry.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
    AddIf(entry.ILessThan(id, entry.Imm32(32), false));
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list.insert(program.syntax_list.begin() + 3, node);
    IR::IREmitter ir{*program.blocks.back()};
    const auto sink = ir.IAdd(ir.ReadLane(ir.Imm32(7), ir.Imm32(63)), ir.Imm32(0));
    (void)sink;
    Lower();
    EXPECT_EQ(Count(IR::Opcode::Barrier), 0);
}

TEST_F(Wave64LoweringTest, NativeWave64NeedsNoEmulation) {
    profile.subgroup_size = 64;
    IR::IREmitter ir{*block};
    const auto sink = ir.IAdd(ir.ReadLane(ir.Imm32(7), ir.Imm32(63)), ir.Imm32(0));
    (void)sink;
    Lower();
    EXPECT_EQ(Count(IR::Opcode::ReadLane), 1);
    EXPECT_EQ(Count(IR::Opcode::Barrier), 0);
}

TEST_F(Wave64LoweringTest, ScratchIsAlignedAfterGuestLdsAndNeverExceedsDeviceLimit) {
    runtime.hw.cs.shared_memory_size = 17;
    IR::IREmitter ir{*block};
    const auto sink = ir.IAdd(ir.ReadLane(ir.Imm32(7), ir.Imm32(63)), ir.Imm32(0));
    (void)sink;
    Lower();
    EXPECT_EQ(17 + info.shared_memory_scratch_size, 24 + 256);
}

TEST_F(Wave64LoweringTest, FullGuestLdsDoesNotGenerateOutOfRangeScratch) {
    runtime.hw.cs.shared_memory_size = profile.max_shared_memory_size;
    IR::IREmitter ir{*block};
    const auto sink = ir.IAdd(ir.ReadLane(ir.Imm32(7), ir.Imm32(63)), ir.Imm32(0));
    (void)sink;
    Lower();
    EXPECT_EQ(Count(IR::Opcode::ReadLane), 1);
    EXPECT_EQ(Count(IR::Opcode::Barrier), 0);
    EXPECT_EQ(info.shared_memory_scratch_size, 0);
}

TEST_F(Wave64LoweringTest, SplitGuestWaveUsesWorkgroupLdsBarriersOnWave32Hosts) {
    runtime.hw.cs.shared_memory_size = 256;
    profile.needs_lds_barriers = true;
    IR::IREmitter ir{*block};
    const auto id = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
    const auto offset = ir.ShiftLeftLogical(id, ir.Imm32(2));
    ir.WriteShared(32, id, offset);
    const auto sink = ir.IAdd(IR::U32{ir.LoadShared(32, false, offset)}, ir.Imm32(0));
    (void)sink;
    InsertSharedMemoryBarriers();
    // A subgroup barrier does not make lane 0..31's stores visible to lane 32..63.
    // One barrier publishes the stores before the loads; the second protects the
    // next wave iteration from overwriting the values being consumed.
    EXPECT_EQ(Count(IR::Opcode::Barrier), 2);
    EXPECT_EQ(Count(IR::Opcode::SubgroupBarrier), 0);
}

TEST_F(Wave64LoweringTest, MultiWaveGroupDoesNotRendezvousIndependentWaves) {
    runtime.hw.cs.workgroup_size = {128, 1, 1};
    runtime.hw.cs.shared_memory_size = 512;
    profile.needs_lds_barriers = true;
    IR::IREmitter ir{*block};
    ir.WriteShared(32, ir.Imm32(7), ir.Imm32(0));
    const auto sink = ir.IAdd(IR::U32{ir.LoadShared(32, false, ir.Imm32(0))}, ir.Imm32(0));
    (void)sink;
    InsertSharedMemoryBarriers();
    EXPECT_EQ(Count(IR::Opcode::Barrier), 0);
    EXPECT_EQ(Count(IR::Opcode::SubgroupBarrier), 2);
}
} // namespace
