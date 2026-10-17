// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include "common/types.h"

namespace Core::GuestFaultRecovery {

struct RegisterFile {
    // Zydis GPR64 order: rax rcx rdx rbx rsp rbp rsi rdi r8 ... r15
    std::array<u64*, 16> gpr{};
    u64* rip{};
    u64* rflags{};
    std::array<u8*, 16> xmm{};
};

struct MemoryProbe {
    bool (*is_accessible)(u64 address, u64 size, bool write, void* user);
    bool (*is_executable)(u64 address, void* user);
    void* user;
};

// Formats a code address for recovery logs, e.g. as module+offset.
using AddressDescriber = std::string (*)(u64 address);
void SetAddressDescriber(AddressDescriber describer);

void SetEnabled(bool enabled);
bool IsEnabled();

// For a failed stack-protector check that called __stack_chk_fail and returned to
// return_address, finds where the function continues when the check passes. Code is not read
// below code_start.
std::optional<u64> FindStackCheckPassPath(u64 return_address, u64 code_start);

bool TryRecover(RegisterFile& regs, u64 fault_address, bool is_execute, const MemoryProbe& probe,
                const char* thread_name);

} // namespace Core::GuestFaultRecovery
