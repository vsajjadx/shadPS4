// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);

/// General purpose register by its x86 encoding: rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15.
u64* GetGprPointer(void* ctx, u8 index);

void IncrementRip(void* ctx, u64 length);

bool IsWriteError(void* ctx);
bool IsExecuteError(void* ctx);

} // namespace Common