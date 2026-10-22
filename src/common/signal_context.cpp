// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#else
#error "Unsupported architecture"
#endif
}

u64* GetGprPointer(void* ctx, u8 index) {
    ASSERT_MSG(index < 16, "Invalid general purpose register index: {}", index);
#if defined(_WIN32)
    return reinterpret_cast<u64*>(&((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rax) + index;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    auto& ss = ((ucontext_t*)ctx)->uc_mcontext->__ss;
    u64* const regs[] = {&ss.__rax, &ss.__rcx, &ss.__rdx, &ss.__rbx, &ss.__rsp, &ss.__rbp,
                         &ss.__rsi, &ss.__rdi, &ss.__r8,  &ss.__r9,  &ss.__r10, &ss.__r11,
                         &ss.__r12, &ss.__r13, &ss.__r14, &ss.__r15};
    return regs[index];
#elif defined(__FreeBSD__)
    auto& mc = ((ucontext_t*)ctx)->uc_mcontext;
    u64* const regs[] = {reinterpret_cast<u64*>(&mc.mc_rax), reinterpret_cast<u64*>(&mc.mc_rcx),
                         reinterpret_cast<u64*>(&mc.mc_rdx), reinterpret_cast<u64*>(&mc.mc_rbx),
                         reinterpret_cast<u64*>(&mc.mc_rsp), reinterpret_cast<u64*>(&mc.mc_rbp),
                         reinterpret_cast<u64*>(&mc.mc_rsi), reinterpret_cast<u64*>(&mc.mc_rdi),
                         reinterpret_cast<u64*>(&mc.mc_r8),  reinterpret_cast<u64*>(&mc.mc_r9),
                         reinterpret_cast<u64*>(&mc.mc_r10), reinterpret_cast<u64*>(&mc.mc_r11),
                         reinterpret_cast<u64*>(&mc.mc_r12), reinterpret_cast<u64*>(&mc.mc_r13),
                         reinterpret_cast<u64*>(&mc.mc_r14), reinterpret_cast<u64*>(&mc.mc_r15)};
    return regs[index];
#elif defined(ARCH_X86_64)
    static constexpr int regs[] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                                   REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
                                   REG_R12, REG_R13, REG_R14, REG_R15};
    return reinterpret_cast<u64*>(&((ucontext_t*)ctx)->uc_mcontext.gregs[regs[index]]);
#else
    UNREACHABLE_MSG("General purpose registers are only available on x86-64");
    return nullptr;
#endif
}

void IncrementRip(void* ctx, u64 length) {
#if defined(_WIN32)
    ((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip += length;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__rip += length;
#elif defined(__FreeBSD__)
    ((ucontext_t*)ctx)->uc_mcontext.mc_rip += length;
#elif defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP] += length;
#else
    UNREACHABLE_MSG("Instruction pointer is only adjustable on x86-64");
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#else
#error "Unsupported architecture"
#endif
}

bool IsExecuteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 8;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x10;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x10;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x10;
#else
#error "Unsupported architecture"
#endif
}

} // namespace Common
