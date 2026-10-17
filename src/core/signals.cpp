// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <string>
#include <fmt/format.h>
#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "core/cpu_patches.h" // Windows static guest red-zone protection
#include "core/guest_fault_recovery.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/signals.h"
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <dlfcn.h>
#include <pthread.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

namespace Core {

#if defined(_WIN32)

static constexpr u64 GuestUserAddressLimit = 0x800000000000ull;

static bool WindowsIsAccessible(u64 address, u64 size, bool write, void*) {
    const u64 last = address + (size ? size : 1) - 1;
    if (address < 0x10000 || last < address || last >= GuestUserAddressLimit) {
        return false;
    }
    u64 page = address & ~0xFFFull;
    while (page <= last) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof(mbi)) == 0 ||
            mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) {
            return false;
        }
        const DWORD prot = mbi.Protect & 0xFF;
        if (prot == PAGE_NOACCESS) {
            return false;
        }
        if (write && !(prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY))) {
            return false;
        }
        page = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

static bool WindowsIsExecutable(u64 address, void*) {
    if (address < 0x10000 || address >= GuestUserAddressLimit) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT) {
        return false;
    }
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)) != 0;
}

static bool IsHostModuleCode(u64 address) {
    HMODULE module{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) != 0;
}

static bool TryRecoverGuestFault(EXCEPTION_POINTERS* pExp) {
    using namespace Libraries::Kernel;
    if (!GuestFaultRecovery::IsEnabled() || pExp == nullptr || pExp->ExceptionRecord == nullptr ||
        pExp->ContextRecord == nullptr || g_curthread == nullptr) {
        return false;
    }
    auto* cr = pExp->ContextRecord;
    const auto* er = pExp->ExceptionRecord;
    if (IsHostModuleCode(cr->Rip)) {
        return false;
    }
    const u64 access_kind = er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0;
    const u64 fault_address = er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0;

    u64 rflags = cr->EFlags;
    GuestFaultRecovery::RegisterFile regs{};
    regs.gpr = {&cr->Rax, &cr->Rcx, &cr->Rdx, &cr->Rbx, &cr->Rsp, &cr->Rbp, &cr->Rsi, &cr->Rdi,
                &cr->R8,  &cr->R9,  &cr->R10, &cr->R11, &cr->R12, &cr->R13, &cr->R14, &cr->R15};
    regs.rip = &cr->Rip;
    regs.rflags = &rflags;
    M128A* xmm = &cr->Xmm0;
    for (size_t i = 0; i < regs.xmm.size(); i++) {
        regs.xmm[i] = reinterpret_cast<u8*>(&xmm[i]);
    }
    const GuestFaultRecovery::MemoryProbe probe{WindowsIsAccessible, WindowsIsExecutable, nullptr};
    if (!GuestFaultRecovery::TryRecover(regs, fault_address, access_kind == 8, probe,
                                        g_curthread->name.c_str())) {
        return false;
    }
    cr->EFlags = static_cast<DWORD>(rflags);
    return true;
}

// A handler that claims a fault without removing its cause makes the thread re-run the faulting
// instruction forever. Hand such a fault on once it has repeated far more often than any real
// page-tracking fault does.
static bool IsRepeatingFault(EXCEPTION_POINTERS* pExp) {
    static constexpr u32 RepeatLimit = 100000;
    thread_local u64 last_rip = 0;
    thread_local u64 last_address = 0;
    thread_local u32 repeats = 0;
    thread_local std::chrono::steady_clock::time_point first_seen{};
    const u64 rip = pExp->ContextRecord->Rip;
    const u64 address = pExp->ExceptionRecord->NumberParameters > 1
                            ? pExp->ExceptionRecord->ExceptionInformation[1]
                            : 0;
    const auto now = std::chrono::steady_clock::now();
    if (rip != last_rip || address != last_address || now - first_seen > std::chrono::seconds(2)) {
        last_rip = rip;
        last_address = address;
        first_seen = now;
        repeats = 0;
        return false;
    }
    if (++repeats < RepeatLimit) {
        return false;
    }
    repeats = 0;
    LOG_ERROR(Core, "Access violation at {:#x} (address {:#x}) keeps repeating after being handled",
              rip, address);
    return true;
}

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    using namespace Libraries::Kernel;
    const auto* signals = Signals::Instance();

    const bool use_static_windows_guest_red_zone_protection =
        WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    Ucontext guest_context{pExp->ContextRecord};
    Siginfo guest_info{
        ._si_signo = 0,
        ._si_errno = 0,
        ._si_code = POSIX_SI_NOINFO,
        ._si_addr = (void*)guest_context.uc_mcontext.mc_rip,
    };

    bool handled = false;
    bool static_protection_exception = false; // Windows static guest red-zone protection
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        guest_info._si_signo = POSIX_SIGSEGV;
        guest_info._si_code = POSIX_SEGV_MAPERR;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_ILLOPC;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case EXCEPTION_PRIV_INSTRUCTION: // Windows static guest red-zone protection
        if (use_static_windows_guest_red_zone_protection) {
            static_protection_exception = true;
            handled = signals->DispatchIllegalInstruction(pExp);
        }
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        guest_info._si_signo = POSIX_SIGBUS;
        guest_info._si_code = POSIX_BUS_ADRALN;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTDIV;
        break;
    case EXCEPTION_INT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTOVF;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTDIV;
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTINV;
        break;
    case EXCEPTION_FLT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTOVF;
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTUND;
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTSUB; // i am not sure about this one
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTRES;
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_BADSTK; // i am not sure about this one either
        break;
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        guest_info._si_signo = POSIX_SIGTRAP;
        guest_info._si_code = POSIX_TRAP_BRKPT;
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        break;
    }

    if (handled && code == EXCEPTION_ACCESS_VIOLATION && IsRepeatingFault(pExp)) {
        handled = false;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION && TryRecoverGuestFault(pExp)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (guest_info._si_signo != 0) {
        if (g_curthread &&
            g_curthread->DispatchSignal(guest_info._si_signo, &guest_info, &guest_context)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    const bool report_unhandled =
        use_static_windows_guest_red_zone_protection ? static_protection_exception : true;
    if (report_unhandled) {
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
        if (pExp != nullptr && pExp->ExceptionRecord != nullptr && pExp->ContextRecord != nullptr) {
            // Extended crash diagnostics: access type, fault address, registers, code bytes and
            // the protection of the faulting page.
            const auto* er = pExp->ExceptionRecord;
            const auto* cr = pExp->ContextRecord;
            const u64 access_kind = er->NumberParameters > 0 ? er->ExceptionInformation[0] : ~0ull;
            const u64 fault_addr = er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0;
            LOG_CRITICAL(Debug, "Fault: access={} ({}) fault_address={:#x} rip={:#x} thread={}",
                         access_kind,
                         access_kind == 0   ? "read"
                         : access_kind == 1 ? "write"
                         : access_kind == 8 ? "execute"
                                            : "other",
                         fault_addr, cr->Rip, g_curthread ? g_curthread->name : "<none>");
            LOG_CRITICAL(Debug,
                         "Regs: rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x} rsi={:#x} rdi={:#x} "
                         "rbp={:#x} rsp={:#x}",
                         cr->Rax, cr->Rbx, cr->Rcx, cr->Rdx, cr->Rsi, cr->Rdi, cr->Rbp, cr->Rsp);
            LOG_CRITICAL(Debug,
                         "Regs: r8={:#x} r9={:#x} r10={:#x} r11={:#x} r12={:#x} r13={:#x} "
                         "r14={:#x} r15={:#x} eflags={:#x}",
                         cr->R8, cr->R9, cr->R10, cr->R11, cr->R12, cr->R13, cr->R14, cr->R15,
                         cr->EFlags);
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(fault_addr), &mbi, sizeof(mbi)) != 0) {
                LOG_CRITICAL(Debug,
                             "FaultPage: base={:#x} alloc_base={:#x} size={:#x} state={:#x} "
                             "protect={:#x} alloc_protect={:#x} type={:#x}",
                             reinterpret_cast<u64>(mbi.BaseAddress),
                             reinterpret_cast<u64>(mbi.AllocationBase), mbi.RegionSize, mbi.State,
                             mbi.Protect, mbi.AllocationProtect, mbi.Type);
            }
            MEMORY_BASIC_INFORMATION cbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cr->Rip), &cbi, sizeof(cbi)) != 0 &&
                cbi.State == MEM_COMMIT) {
                std::string bytes;
                const u8* code_ptr = reinterpret_cast<const u8*>(cr->Rip);
                const u64 avail = reinterpret_cast<u64>(cbi.BaseAddress) + cbi.RegionSize - cr->Rip;
                for (u64 i = 0; i < std::min<u64>(32, avail); i++) {
                    bytes += fmt::format("{:02x} ", code_ptr[i]);
                }
                LOG_CRITICAL(Debug, "CodeBytes@rip: {}", bytes);
            }
            // Poor man's backtrace: every stack slot that points into guest code (eboot text is
            // mapped at 0x5790000..0xa3e8000 in the logs seen so far; log raw values in range).
            {
                std::string frames;
                int found = 0;
                const u64 rsp = cr->Rsp;
                for (u64 i = 0; i < 2048 && found < 40; i++) {
                    MEMORY_BASIC_INFORMATION sbi{};
                    const u64 slot_addr = rsp + i * 8;
                    if (VirtualQuery(reinterpret_cast<void*>(slot_addr), &sbi, sizeof(sbi)) == 0 ||
                        sbi.State != MEM_COMMIT || (sbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                        break;
                    }
                    const u64 v = *reinterpret_cast<const u64*>(slot_addr);
                    if (v >= 0x5790000ull && v < 0xa3e8000ull) {
                        frames += fmt::format("{:#x}(+{:#x}) ", v, v - 0x5790000ull);
                        found++;
                    }
                }
                LOG_CRITICAL(Debug, "StackCodeRefs: {}", frames);
            }
        }
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

static s32 NativeSiCodeToGuest(s32 sig, s32 code) {
    using namespace Libraries::Kernel;
    switch (sig) {
    case SIGUSR1:
        return POSIX_SI_LWP;
    case SIGSEGV:
        switch (code) {
        case SEGV_MAPERR:
            return POSIX_SEGV_MAPERR;
        case SEGV_ACCERR:
            return POSIX_SEGV_ACCERR;
        }
    case SIGBUS:
        switch (code) {
        case BUS_ADRALN:
            return POSIX_BUS_ADRALN;
        case BUS_ADRERR:
            return POSIX_BUS_ADRERR;
        case BUS_OBJERR:
            return POSIX_BUS_OBJERR;
        }
    case SIGILL:
        switch (code) {
        case ILL_ILLOPC:
            return POSIX_ILL_ILLOPC;
        case ILL_ILLOPN:
            return POSIX_ILL_ILLOPN;
        case ILL_ILLADR:
            return POSIX_ILL_ILLADR;
        case ILL_ILLTRP:
            return POSIX_ILL_ILLTRP;
        case ILL_PRVOPC:
            return POSIX_ILL_PRVOPC;
        case ILL_PRVREG:
            return POSIX_ILL_PRVREG;
        case ILL_COPROC:
            return POSIX_ILL_COPROC;
        case ILL_BADSTK:
            return POSIX_ILL_BADSTK;
        }
    case SIGFPE:
        switch (code) {
        case FPE_INTOVF:
            return POSIX_FPE_INTOVF;
        case FPE_INTDIV:
            return POSIX_FPE_INTDIV;
        case FPE_FLTDIV:
            return POSIX_FPE_FLTDIV;
        case FPE_FLTOVF:
            return POSIX_FPE_FLTOVF;
        case FPE_FLTUND:
            return POSIX_FPE_FLTUND;
        case FPE_FLTRES:
            return POSIX_FPE_FLTRES;
        case FPE_FLTINV:
            return POSIX_FPE_FLTINV;
        case FPE_FLTSUB:
            return POSIX_FPE_FLTSUB;
        }
    case SIGTRAP:
        switch (code) {
        case TRAP_BRKPT:
            return POSIX_TRAP_BRKPT;
        case TRAP_TRACE:
            return POSIX_TRAP_TRACE;
#ifdef __FreeBSD__
        case TRAP_DTRACE:
            return POSIX_TRAP_DTRACE;
#endif
        }

    default:
        return POSIX_SI_NOINFO;
    }
}

#if defined(__linux__) && defined(ARCH_X86_64)
struct LinuxFaultInfo {
    u64 fault_page;
};

static bool LinuxIsAccessible(u64 address, u64 size, bool, void* user) {
    const auto* info = static_cast<const LinuxFaultInfo*>(user);
    const u64 last = address + (size ? size : 1) - 1;
    if (address < 0x10000 || last < address || last >= 0x800000000000ull) {
        return false;
    }
    return (address & ~0xFFFull) != info->fault_page && (last & ~0xFFFull) != info->fault_page;
}

static bool LinuxIsExecutable(u64 address, void* user) {
    const auto* info = static_cast<const LinuxFaultInfo*>(user);
    return address >= 0x10000 && address < 0x800000000000ull &&
           (address & ~0xFFFull) != info->fault_page;
}

static bool TryRecoverGuestFault(siginfo_t* info, void* raw_context) {
    using namespace Libraries::Kernel;
    if (!GuestFaultRecovery::IsEnabled() || info == nullptr || raw_context == nullptr ||
        g_curthread == nullptr) {
        return false;
    }
    auto* uc = reinterpret_cast<ucontext_t*>(raw_context);
    auto& gregs = uc->uc_mcontext.gregs;
    const u64 rip = static_cast<u64>(gregs[REG_RIP]);
    Dl_info dl{};
    if (dladdr(reinterpret_cast<void*>(rip), &dl) != 0 && dl.dli_fname != nullptr) {
        return false;
    }
    const u64 fault_address = reinterpret_cast<u64>(info->si_addr);
    LinuxFaultInfo fault{fault_address & ~0xFFFull};

    GuestFaultRecovery::RegisterFile regs{};
    const int order[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                           REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    for (size_t i = 0; i < regs.gpr.size(); i++) {
        regs.gpr[i] = reinterpret_cast<u64*>(&gregs[order[i]]);
    }
    regs.rip = reinterpret_cast<u64*>(&gregs[REG_RIP]);
    regs.rflags = reinterpret_cast<u64*>(&gregs[REG_EFL]);
    if (uc->uc_mcontext.fpregs != nullptr) {
        for (size_t i = 0; i < regs.xmm.size(); i++) {
            regs.xmm[i] = reinterpret_cast<u8*>(&uc->uc_mcontext.fpregs->_xmm[i]);
        }
    }
    const GuestFaultRecovery::MemoryProbe probe{LinuxIsAccessible, LinuxIsExecutable, &fault};
    return GuestFaultRecovery::TryRecover(regs, fault_address, Common::IsExecuteError(raw_context),
                                          probe, g_curthread->name.c_str());
}
#endif

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    using namespace Libraries::Kernel;
    auto* thread = g_curthread;
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    Ucontext context{info, reinterpret_cast<ucontext_t*>(raw_context)};
    Siginfo guest_info{};
    if (info) {
        guest_info = *reinterpret_cast<Siginfo*>(info);
        guest_info._si_signo = sig == SIGUSR1 ? 0 : NativeToOrbisSignal(info->si_signo);
        guest_info._si_errno = NativeToPosixErrno(info->si_errno);
        guest_info._si_code = NativeSiCodeToGuest(sig, info->si_code);
        guest_info._si_addr = (void*)context.uc_mcontext.mc_rip;
    }
    Siginfo* info_p = info ? &guest_info : nullptr;
    Ucontext* context_p = raw_context ? &context : nullptr;

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        const bool is_write = Common::IsWriteError(raw_context);
        const bool is_exec = Common::IsExecuteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
#if defined(__linux__) && defined(ARCH_X86_64)
            if (TryRecoverGuestFault(info, raw_context)) {
                return;
            }
#endif
            if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
                return;
            }
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address),
                            is_write  ? "Write to"
                            : is_exec ? "Executed from"
                                      : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (signals->DispatchIllegalInstruction(raw_context)) {
            return;
        }
    case SIGFPE:
    case SIGTRAP:
    case SIGABRT:
    case SIGSYS: {
        if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
            return;
        }

        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
    case SIGSLEEP: {
        if (thread) {
            thread->suspended_context = context.uc_mcontext;
            thread->is_suspended_in_signal = true;
        }
        // Sleep thread until signal is received again
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGSLEEP);
        sigwait(&sigset, &sig);
        if (thread) {
            thread->is_suspended_in_signal = false;
        }
        break;
    }
    case SIGUSR1:
        if (thread) {
            thread->DispatchPendingSignals(info_p, context_p);
        }
        break;
    default:
        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
}

#endif

SignalDispatch::SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(
        sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
            sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGABRT, &action, nullptr) == 0 &&
            sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
            sigaction(SIGUSR1, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
            sigaction(SIGSLEEP, &action, nullptr) == 0,
        "Failed to register signal handlers.");
#endif
}

void SignalDispatch::RemoveHandlers() {
    // asserting here would get into an infinite loop until too
    // many nested exceptions makes the OS kill the process
#if defined(_WIN32)
    if (!(RemoveVectoredExceptionHandler(handle))) {
        LOG_CRITICAL(Core, "Failed to remove exception handler.");
        std::quick_exit(1);
    }
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    if (!(sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
          sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
          sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
          sigaction(SIGUSR1, &action, nullptr) == 0 && sigaction(SIGABRT, &action, nullptr) == 0 &&
          sigaction(SIGSLEEP, &action, nullptr) == 0)) {
        LOG_CRITICAL(Core, "Failed to remove signal handlers.");
        std::quick_exit(1);
    }
#endif
}

SignalDispatch::~SignalDispatch() {
    RemoveHandlers();
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
