// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include "common/types.h"

namespace Libraries::Kernel {

struct Pthread;

/// What a guest thread is blocked on, recorded for the hang dump.
enum class WaitKind : u8 {
    None,
    Mutex,
    Cond,
    Sema,
    PosixSem,
    EventFlag,
    Equeue,
    Sleep,
};

/// Marks the current guest thread as blocked in an HLE wait until the scope ends.
class WaitScope {
public:
    WaitScope(WaitKind kind, const void* obj, const void* caller);
    ~WaitScope();

    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    Pthread* thread;
    WaitKind prev_kind{};
    const void* prev_obj{};
    const void* prev_caller{};
    u64 prev_since_ms{};
};

#define HANG_DIAG_WAIT(kind, obj)                                                                  \
    ::Libraries::Kernel::WaitScope hang_diag_wait_scope {                                          \
        ::Libraries::Kernel::WaitKind::kind, obj, __builtin_return_address(0)                      \
    }

/// State of an sceKernel semaphore for the hang dump.
struct SemaDiag {
    s32 tokens{};
    s32 max_tokens{};
    s32 waiters{-1}; ///< -1 when the semaphore was busy.
    u32 signals{};
    const Pthread* last_signaler{};
    u64 last_signal_caller{};
    u64 last_signal_ms{};
    const Pthread* last_taker{};
    u64 last_take_caller{};
    u64 last_take_ms{};
};
bool GetSemaDiag(const void* sema, SemaDiag& out);

/// Registers the calling host thread (GPU, presenter, ...) so it is included in the hang dump.
void RegisterHangDiagHostThread(std::string_view name);

/// Logs the state of every guest and registered host thread: what each one waits on, its
/// instruction pointer and the code addresses found on its stack.
void DumpThreadsForHang(std::string_view reason);

} // namespace Libraries::Kernel
