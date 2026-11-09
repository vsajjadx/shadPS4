// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "common/recoverable.h"
#include "core/signals.h"
#include "emulator.h"

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(ARCH_X86_64)
#define Crash() __asm__ __volatile__("int $3")
#elif defined(ARCH_ARM64)
#define Crash() __asm__ __volatile__("brk 0")
#else
#error "Missing Crash() implementation for target CPU architecture."
#endif

namespace Common {
int& RecoverableDepth() {
    static thread_local int depth = 0;
    return depth;
}
} // namespace Common

void assert_fail_impl() {
    if (Common::RecoverableDepth() > 0) {
        // The failed check is logged; the owner of the RecoverableScope contains the failure.
        throw Common::RecoverableFailure("assertion failed (see the log line above)");
    }
    Core::WriteCrashDump(nullptr);
    Core::Signals::Instance()->RemoveHandlers();
#ifdef _WIN32
    if (!IsDebuggerPresent()) {
        // Shutdown() joins threads and closes the database. After a failed assert one of them can
        // block forever, which left a windowless emulator process running and the launcher stuck
        // behind it. Kill the process if the orderly shutdown does not finish in time.
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            TerminateProcess(GetCurrentProcess(), 3);
        }).detach();
    }
#endif
    Common::Singleton<Core::Emulator>::Instance()->Shutdown();
#ifdef _WIN32
    if (!IsDebuggerPresent()) {
        // Exit directly instead of raising a breakpoint exception: that goes through Windows Error
        // Reporting, which keeps the dying process alive for a long time.
        TerminateProcess(GetCurrentProcess(), 3);
    }
#endif
    Crash();
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
