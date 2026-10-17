// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
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

void SetEnabled(bool enabled);
bool IsEnabled();

bool TryRecover(RegisterFile& regs, u64 fault_address, bool is_execute, const MemoryProbe& probe,
                const char* thread_name);

} // namespace Core::GuestFaultRecovery
