// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <bit>
#include <cstring>
#include <optional>
#include <string>
#include "common/decoder.h"
#include "common/logging/log.h"
#include "core/guest_fault_recovery.h"

namespace Core::GuestFaultRecovery {

namespace {

constexpr u64 FLAG_CF = 1ull << 0;
constexpr u64 FLAG_PF = 1ull << 2;
constexpr u64 FLAG_AF = 1ull << 4;
constexpr u64 FLAG_ZF = 1ull << 6;
constexpr u64 FLAG_SF = 1ull << 7;
constexpr u64 FLAG_OF = 1ull << 11;
constexpr u64 ARITH_FLAGS = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;

std::atomic_bool g_enabled{true};
std::atomic<u64> g_recoveries{0};

struct Decoded {
    ZydisDecodedInstruction inst;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
};

u64 WidthMask(u32 bits) {
    return bits >= 64 ? ~0ull : ((1ull << bits) - 1);
}

std::optional<u32> GprIndex(ZydisRegister reg) {
    const auto full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (full < ZYDIS_REGISTER_RAX || full > ZYDIS_REGISTER_R15) {
        return std::nullopt;
    }
    return static_cast<u32>(full - ZYDIS_REGISTER_RAX);
}

bool IsHighByte(ZydisRegister reg) {
    return reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH ||
           reg == ZYDIS_REGISTER_BH;
}

std::optional<u64> ReadGpr(const RegisterFile& regs, ZydisRegister reg) {
    if (reg == ZYDIS_REGISTER_RIP) {
        return *regs.rip;
    }
    const auto index = GprIndex(reg);
    if (!index || !regs.gpr[*index]) {
        return std::nullopt;
    }
    const u64 value = *regs.gpr[*index];
    if (IsHighByte(reg)) {
        return (value >> 8) & 0xFF;
    }
    switch (ZydisRegisterGetClass(reg)) {
    case ZYDIS_REGCLASS_GPR8:
        return value & 0xFF;
    case ZYDIS_REGCLASS_GPR16:
        return value & 0xFFFF;
    case ZYDIS_REGCLASS_GPR32:
        return value & 0xFFFFFFFF;
    default:
        return value;
    }
}

bool WriteGpr(RegisterFile& regs, ZydisRegister reg, u64 value) {
    const auto index = GprIndex(reg);
    if (!index || !regs.gpr[*index]) {
        return false;
    }
    u64& dst = *regs.gpr[*index];
    if (IsHighByte(reg)) {
        dst = (dst & ~0xFF00ull) | ((value & 0xFF) << 8);
        return true;
    }
    switch (ZydisRegisterGetClass(reg)) {
    case ZYDIS_REGCLASS_GPR8:
        dst = (dst & ~0xFFull) | (value & 0xFF);
        break;
    case ZYDIS_REGCLASS_GPR16:
        dst = (dst & ~0xFFFFull) | (value & 0xFFFF);
        break;
    case ZYDIS_REGCLASS_GPR32:
        dst = value & 0xFFFFFFFF;
        break;
    default:
        dst = value;
        break;
    }
    return true;
}

bool ZeroVector(RegisterFile& regs, ZydisRegister reg) {
    s32 index = -1;
    switch (ZydisRegisterGetClass(reg)) {
    case ZYDIS_REGCLASS_XMM:
        index = reg - ZYDIS_REGISTER_XMM0;
        break;
    case ZYDIS_REGCLASS_YMM:
        index = reg - ZYDIS_REGISTER_YMM0;
        break;
    default:
        return false;
    }
    if (index < 0 || index >= 16 || !regs.xmm[index]) {
        return false;
    }
    std::memset(regs.xmm[index], 0, 16);
    return true;
}

bool ZeroRegister(RegisterFile& regs, ZydisRegister reg) {
    if (GprIndex(reg)) {
        return WriteGpr(regs, reg, 0);
    }
    return ZeroVector(regs, reg);
}

std::optional<u64> EffectiveAddress(const RegisterFile& regs, const Decoded& d,
                                    const ZydisDecodedOperand& op) {
    if (op.type != ZYDIS_OPERAND_TYPE_MEMORY || op.mem.type != ZYDIS_MEMOP_TYPE_MEM) {
        return std::nullopt;
    }
    if (op.mem.segment == ZYDIS_REGISTER_FS || op.mem.segment == ZYDIS_REGISTER_GS) {
        return std::nullopt;
    }
    u64 address = 0;
    if (op.mem.base == ZYDIS_REGISTER_RIP || op.mem.base == ZYDIS_REGISTER_EIP) {
        address = *regs.rip + d.inst.length;
    } else if (op.mem.base != ZYDIS_REGISTER_NONE) {
        const auto base = ReadGpr(regs, op.mem.base);
        if (!base) {
            return std::nullopt;
        }
        address = *base;
    }
    if (op.mem.index != ZYDIS_REGISTER_NONE) {
        const auto index = ReadGpr(regs, op.mem.index);
        if (!index) {
            return std::nullopt;
        }
        address += *index * op.mem.scale;
    }
    address += static_cast<u64>(op.mem.disp.value);
    if (d.inst.address_width == 32) {
        address &= 0xFFFFFFFF;
    }
    return address;
}

std::optional<u64> OperandValue(const RegisterFile& regs, const ZydisDecodedOperand& op) {
    switch (op.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        return ReadGpr(regs, op.reg.value);
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        return op.imm.is_signed ? static_cast<u64>(op.imm.value.s) : op.imm.value.u;
    default:
        return std::nullopt;
    }
}

void SetResultFlags(RegisterFile& regs, u64 result, u32 width, bool carry, bool overflow) {
    if (!regs.rflags) {
        return;
    }
    const u64 masked = result & WidthMask(width);
    u64 flags = *regs.rflags & ~ARITH_FLAGS;
    if (carry) {
        flags |= FLAG_CF;
    }
    if (overflow) {
        flags |= FLAG_OF;
    }
    if (masked == 0) {
        flags |= FLAG_ZF;
    }
    if ((masked >> (width - 1)) & 1) {
        flags |= FLAG_SF;
    }
    if ((std::popcount(masked & 0xFF) & 1) == 0) {
        flags |= FLAG_PF;
    }
    *regs.rflags = flags;
}

bool EmulateReturn(RegisterFile& regs, const MemoryProbe& probe) {
    u64& rsp = *regs.gpr[ZYDIS_REGISTER_RSP - ZYDIS_REGISTER_RAX];
    if (!probe.is_accessible(rsp, 8, false, probe.user)) {
        return false;
    }
    const u64 return_address = *reinterpret_cast<const u64*>(rsp);
    if (!probe.is_executable(return_address, probe.user)) {
        return false;
    }
    rsp += 8;
    *regs.gpr[0] = 0;
    if (regs.xmm[0]) {
        std::memset(regs.xmm[0], 0, 16);
    }
    *regs.rip = return_address;
    return true;
}

bool IsPureLoad(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_MOV:
    case ZYDIS_MNEMONIC_MOVZX:
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD:
    case ZYDIS_MNEMONIC_MOVBE:
    case ZYDIS_MNEMONIC_MOVD:
    case ZYDIS_MNEMONIC_MOVQ:
    case ZYDIS_MNEMONIC_MOVSS:
    case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVDQA:
    case ZYDIS_MNEMONIC_MOVDQU:
    case ZYDIS_MNEMONIC_MOVNTDQA:
    case ZYDIS_MNEMONIC_LDDQU:
    case ZYDIS_MNEMONIC_MOVDDUP:
    case ZYDIS_MNEMONIC_MOVSHDUP:
    case ZYDIS_MNEMONIC_MOVSLDUP:
    case ZYDIS_MNEMONIC_VMOVD:
    case ZYDIS_MNEMONIC_VMOVQ:
    case ZYDIS_MNEMONIC_VMOVSS:
    case ZYDIS_MNEMONIC_VMOVSD:
    case ZYDIS_MNEMONIC_VMOVAPS:
    case ZYDIS_MNEMONIC_VMOVUPS:
    case ZYDIS_MNEMONIC_VMOVAPD:
    case ZYDIS_MNEMONIC_VMOVUPD:
    case ZYDIS_MNEMONIC_VMOVDQA:
    case ZYDIS_MNEMONIC_VMOVDQU:
    case ZYDIS_MNEMONIC_VMOVNTDQA:
    case ZYDIS_MNEMONIC_VLDDQU:
    case ZYDIS_MNEMONIC_VMOVDDUP:
    case ZYDIS_MNEMONIC_VMOVSHDUP:
    case ZYDIS_MNEMONIC_VMOVSLDUP:
    case ZYDIS_MNEMONIC_VBROADCASTSS:
    case ZYDIS_MNEMONIC_VBROADCASTSD:
    case ZYDIS_MNEMONIC_VBROADCASTF128:
    case ZYDIS_MNEMONIC_VBROADCASTI128:
    case ZYDIS_MNEMONIC_VPBROADCASTB:
    case ZYDIS_MNEMONIC_VPBROADCASTW:
    case ZYDIS_MNEMONIC_VPBROADCASTD:
    case ZYDIS_MNEMONIC_VPBROADCASTQ:
    case ZYDIS_MNEMONIC_IMUL:
    case ZYDIS_MNEMONIC_PMOVZXBW:
    case ZYDIS_MNEMONIC_PMOVZXBD:
    case ZYDIS_MNEMONIC_PMOVZXBQ:
    case ZYDIS_MNEMONIC_PMOVZXWD:
    case ZYDIS_MNEMONIC_PMOVZXWQ:
    case ZYDIS_MNEMONIC_PMOVZXDQ:
    case ZYDIS_MNEMONIC_PMOVSXBW:
    case ZYDIS_MNEMONIC_PMOVSXBD:
    case ZYDIS_MNEMONIC_PMOVSXBQ:
    case ZYDIS_MNEMONIC_PMOVSXWD:
    case ZYDIS_MNEMONIC_PMOVSXWQ:
    case ZYDIS_MNEMONIC_PMOVSXDQ:
        return true;
    default:
        return false;
    }
}

bool IsStackOp(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_PUSH:
    case ZYDIS_MNEMONIC_POP:
    case ZYDIS_MNEMONIC_PUSHFQ:
    case ZYDIS_MNEMONIC_POPFQ:
    case ZYDIS_MNEMONIC_CALL:
    case ZYDIS_MNEMONIC_RET:
    case ZYDIS_MNEMONIC_ENTER:
    case ZYDIS_MNEMONIC_LEAVE:
        return true;
    default:
        return false;
    }
}

bool IsStringOp(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_MOVSB:
    case ZYDIS_MNEMONIC_MOVSW:
    case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_MOVSQ:
    case ZYDIS_MNEMONIC_STOSB:
    case ZYDIS_MNEMONIC_STOSW:
    case ZYDIS_MNEMONIC_STOSD:
    case ZYDIS_MNEMONIC_STOSQ:
    case ZYDIS_MNEMONIC_LODSB:
    case ZYDIS_MNEMONIC_LODSW:
    case ZYDIS_MNEMONIC_LODSD:
    case ZYDIS_MNEMONIC_LODSQ:
    case ZYDIS_MNEMONIC_CMPSB:
    case ZYDIS_MNEMONIC_CMPSW:
    case ZYDIS_MNEMONIC_CMPSD:
    case ZYDIS_MNEMONIC_CMPSQ:
    case ZYDIS_MNEMONIC_SCASB:
    case ZYDIS_MNEMONIC_SCASW:
    case ZYDIS_MNEMONIC_SCASD:
    case ZYDIS_MNEMONIC_SCASQ:
        return true;
    default:
        return false;
    }
}

bool HasStringOperands(const Decoded& d) {
    for (u32 i = 0; i < d.inst.operand_count; i++) {
        const auto& op = d.ops[i];
        if (op.type == ZYDIS_OPERAND_TYPE_MEMORY &&
            (op.mem.base == ZYDIS_REGISTER_RSI || op.mem.base == ZYDIS_REGISTER_RDI) &&
            op.visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN) {
            return true;
        }
    }
    return false;
}

enum class Outcome { NotHandled, Advance, Redirected };

Outcome EmulateInstruction(RegisterFile& regs, const Decoded& d, const MemoryProbe& probe,
                           s32 bad_operand) {
    const auto& inst = d.inst;
    const auto& ops = d.ops;
    const auto mnemonic = inst.mnemonic;
    const bool mem_first = bad_operand == 0;
    const u32 visible = inst.operand_count_visible;

    if (IsStackOp(mnemonic) && ops[bad_operand].visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
        return Outcome::NotHandled;
    }

    if (IsStringOp(mnemonic) && HasStringOperands(d)) {
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_LODSB:
        case ZYDIS_MNEMONIC_LODSW:
        case ZYDIS_MNEMONIC_LODSD:
        case ZYDIS_MNEMONIC_LODSQ:
            *regs.gpr[0] &= ~WidthMask(ops[0].size);
            break;
        default:
            break;
        }
        if (inst.attributes &
            (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE)) {
            *regs.gpr[ZYDIS_REGISTER_RCX - ZYDIS_REGISTER_RAX] = 0;
        }
        return Outcome::Advance;
    }

    switch (mnemonic) {
    case ZYDIS_MNEMONIC_CALL:
        *regs.gpr[0] = 0;
        if (regs.xmm[0]) {
            std::memset(regs.xmm[0], 0, 16);
        }
        return Outcome::Advance;
    case ZYDIS_MNEMONIC_JMP:
        return EmulateReturn(regs, probe) ? Outcome::Redirected : Outcome::NotHandled;
    case ZYDIS_MNEMONIC_PUSH: {
        u64& rsp = *regs.gpr[ZYDIS_REGISTER_RSP - ZYDIS_REGISTER_RAX];
        if (!probe.is_accessible(rsp - 8, 8, true, probe.user)) {
            return Outcome::NotHandled;
        }
        rsp -= 8;
        *reinterpret_cast<u64*>(rsp) = 0;
        return Outcome::Advance;
    }
    case ZYDIS_MNEMONIC_POP:
        *regs.gpr[ZYDIS_REGISTER_RSP - ZYDIS_REGISTER_RAX] += 8;
        return Outcome::Advance;
    case ZYDIS_MNEMONIC_CMP: {
        if (visible < 2) {
            return Outcome::NotHandled;
        }
        const u32 width = ops[0].size;
        if (mem_first) {
            const auto other = OperandValue(regs, ops[1]);
            if (!other) {
                return Outcome::NotHandled;
            }
            const u64 b = *other & WidthMask(width);
            SetResultFlags(regs, 0 - b, width, b != 0, b == (1ull << (width - 1)));
        } else {
            const auto other = OperandValue(regs, ops[0]);
            if (!other) {
                return Outcome::NotHandled;
            }
            SetResultFlags(regs, *other, width, false, false);
        }
        return Outcome::Advance;
    }
    case ZYDIS_MNEMONIC_TEST:
    case ZYDIS_MNEMONIC_BT:
        SetResultFlags(regs, 0, ops[0].size ? ops[0].size : 64, false, false);
        return Outcome::Advance;
    case ZYDIS_MNEMONIC_AND:
        if (!mem_first && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            WriteGpr(regs, ops[0].reg.value, 0);
        }
        SetResultFlags(regs, 0, ops[0].size, false, false);
        return Outcome::Advance;
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_SUB: {
        u64 result = 0;
        if (!mem_first && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            result = ReadGpr(regs, ops[0].reg.value).value_or(0);
            WriteGpr(regs, ops[0].reg.value, result);
        }
        SetResultFlags(regs, result, ops[0].size, false, false);
        return Outcome::Advance;
    }
    case ZYDIS_MNEMONIC_XCHG:
    case ZYDIS_MNEMONIC_XADD:
        for (u32 i = 0; i < visible; i++) {
            if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                WriteGpr(regs, ops[i].reg.value, 0);
            }
        }
        return Outcome::Advance;
    case ZYDIS_MNEMONIC_CMPXCHG: {
        const u32 width = ops[0].size;
        const u64 accumulator = *regs.gpr[0] & WidthMask(width);
        if (accumulator != 0) {
            WriteGpr(regs,
                     width == 64   ? ZYDIS_REGISTER_RAX
                     : width == 32 ? ZYDIS_REGISTER_EAX
                     : width == 16 ? ZYDIS_REGISTER_AX
                                   : ZYDIS_REGISTER_AL,
                     0);
        }
        SetResultFlags(regs, accumulator, width, false, false);
        return Outcome::Advance;
    }
    default:
        break;
    }

    if (!mem_first && visible >= 2 && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        (ops[0].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) && IsPureLoad(mnemonic)) {
        ZeroRegister(regs, ops[0].reg.value);
        return Outcome::Advance;
    }

    return Outcome::Advance;
}

} // Anonymous namespace

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsEnabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

bool TryRecover(RegisterFile& regs, u64 fault_address, bool is_execute, const MemoryProbe& probe,
                const char* thread_name) {
    if (!IsEnabled() || !regs.rip || !regs.gpr[ZYDIS_REGISTER_RSP - ZYDIS_REGISTER_RAX]) {
        return false;
    }
    for (const auto* reg : regs.gpr) {
        if (!reg) {
            return false;
        }
    }

    const u64 rip = *regs.rip;
    const char* name = thread_name ? thread_name : "<none>";

    if (is_execute || rip == fault_address || rip < 0x10000) {
        const u64 rsp = *regs.gpr[ZYDIS_REGISTER_RSP - ZYDIS_REGISTER_RAX];
        if (!EmulateReturn(regs, probe)) {
            return false;
        }
        const u64 n = g_recoveries.fetch_add(1, std::memory_order_relaxed);
        if (n < 64 || (n & (n - 1)) == 0) {
            LOG_WARNING(Core,
                        "Guest fault recovery #{} [{}]: call into invalid code {:#x}, returning 0 "
                        "to {:#x} (rsp={:#x})",
                        n + 1, name, rip, *regs.rip, rsp);
        }
        return true;
    }

    Decoded d{};
    if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
            d.inst, d.ops, reinterpret_cast<void*>(rip)))) {
        return false;
    }

    s32 bad_operand = -1;
    u64 bad_address = 0;
    for (u32 i = 0; i < d.inst.operand_count; i++) {
        const auto& op = d.ops[i];
        const auto address = EffectiveAddress(regs, d, op);
        if (!address) {
            continue;
        }
        const bool write = (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
        const u64 size = op.size >= 8 ? op.size / 8 : 1;
        if (!probe.is_accessible(*address, size, write, probe.user)) {
            bad_operand = static_cast<s32>(i);
            bad_address = *address;
            break;
        }
    }
    if (bad_operand < 0) {
        return false;
    }

    const auto outcome = EmulateInstruction(regs, d, probe, bad_operand);
    if (outcome == Outcome::NotHandled) {
        return false;
    }
    if (outcome == Outcome::Advance) {
        *regs.rip = rip + d.inst.length;
    }

    const u64 n = g_recoveries.fetch_add(1, std::memory_order_relaxed);
    if (n < 64 || (n & (n - 1)) == 0) {
        LOG_WARNING(Core,
                    "Guest fault recovery #{} [{}]: '{}' at {:#x} touched invalid address {:#x} "
                    "(fault {:#x}), continuing at {:#x}",
                    n + 1, name, Common::Decoder::Instance()->disassembleInst(d.inst, d.ops, rip),
                    rip, bad_address, fault_address, *regs.rip);
    }
    return true;
}

} // namespace Core::GuestFaultRecovery
