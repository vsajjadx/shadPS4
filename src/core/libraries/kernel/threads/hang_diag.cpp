// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "common/decoder.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/threads/hang_diag.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/thread_state.h"
#include "core/linker.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Libraries::Kernel {

namespace {

u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

const char* WaitKindName(WaitKind kind) {
    switch (kind) {
    case WaitKind::None:
        return "running";
    case WaitKind::Mutex:
        return "pthread_mutex_lock";
    case WaitKind::Cond:
        return "pthread_cond_wait";
    case WaitKind::Sema:
        return "sceKernelWaitSema";
    case WaitKind::PosixSem:
        return "sem_wait";
    case WaitKind::EventFlag:
        return "sceKernelWaitEventFlag";
    case WaitKind::Equeue:
        return "sceKernelWaitEqueue";
    case WaitKind::Sleep:
        return "sleep";
    }
    return "?";
}

constexpr size_t MaxStackCopy = 0x8000;
constexpr size_t MaxStackFrames = 32;

struct ThreadSnapshot {
    std::string name;
    bool guest{};
    const Pthread* thread{};
    WaitKind wait_kind{};
    const void* wait_obj{};
    const void* wait_caller{};
    u64 wait_since_ms{};
    bool have_context{};
    u64 rip{};
    u64 rsp{};
    u64 rbp{};
    std::array<u64, 16> gprs{};
    std::vector<u64> stack;
};

#ifdef _WIN32
struct HostThread {
    std::string name;
    HANDLE handle;
    DWORD id;
};
std::mutex host_threads_mutex;
std::vector<HostThread> host_threads;

bool IsExecutable(u64 addr) {
    if (addr < 0x10000 || addr >= 0x800000000000ULL) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    constexpr DWORD exec =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return mbi.State == MEM_COMMIT && (mbi.Protect & exec) != 0;
}

// Must only be called while the thread is suspended. Does not allocate: snapshot.stack has its
// capacity reserved beforehand, because the suspended thread may hold the heap lock.
void CaptureSuspended(HANDLE handle, u64 stack_lo, u64 stack_hi, ThreadSnapshot& snap) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(handle, &ctx)) {
        return;
    }
    snap.have_context = true;
    snap.rip = ctx.Rip;
    snap.rsp = ctx.Rsp;
    snap.rbp = ctx.Rbp;
    snap.gprs = {ctx.Rax, ctx.Rcx, ctx.Rdx, ctx.Rbx, ctx.Rsp, ctx.Rbp, ctx.Rsi, ctx.Rdi,
                 ctx.R8,  ctx.R9,  ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15};

    u64 lo = ctx.Rsp;
    u64 hi = 0;
    if (stack_lo != 0 && lo >= stack_lo && lo < stack_hi) {
        hi = stack_hi;
    } else {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(lo), &mbi, sizeof(mbi)) != 0 &&
            mbi.State == MEM_COMMIT) {
            hi = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
        }
    }
    if (hi <= lo) {
        return;
    }
    hi = std::min<u64>(hi, lo + MaxStackCopy);
    const auto* words = reinterpret_cast<const u64*>(lo & ~u64{7});
    const size_t count = static_cast<size_t>((hi - (lo & ~u64{7})) / sizeof(u64));
    for (size_t i = 0; i < count && snap.stack.size() < snap.stack.capacity(); ++i) {
        snap.stack.push_back(words[i]);
    }
}
#endif

std::string DescribeAddress(u64 addr);

/// For a thread found running guest code: its registers and the instructions at its rip, which
/// show what a spinning loop waits for.
void LogRunningCode(const ThreadSnapshot& snap) {
    const auto& r = snap.gprs;
    LOG_CRITICAL(Kernel_Pthread,
                 "    rax={:#x} rcx={:#x} rdx={:#x} rbx={:#x} rsi={:#x} rdi={:#x} r8={:#x} "
                 "r9={:#x}",
                 r[0], r[1], r[2], r[3], r[6], r[7], r[8], r[9]);
    LOG_CRITICAL(Kernel_Pthread, "    r10={:#x} r11={:#x} r12={:#x} r13={:#x} r14={:#x} r15={:#x}",
                 r[10], r[11], r[12], r[13], r[14], r[15]);
#ifdef _WIN32
    std::array<u8, 96> code{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(snap.rip),
                           code.data(), code.size(), &read) ||
        read == 0) {
        return;
    }
    size_t offset = 0;
    for (u32 i = 0; i < 12 && offset < read; ++i) {
        ZydisDecodedInstruction inst;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                inst, operands, code.data() + offset, read - offset))) {
            break;
        }
        LOG_CRITICAL(
            Kernel_Pthread, "    {}: {}", DescribeAddress(snap.rip + offset),
            Common::Decoder::Instance()->disassembleInst(inst, operands, snap.rip + offset));
        offset += inst.length;
    }
#endif
}

std::string DescribeAddress(u64 addr) {
    if (addr == 0) {
        return "null";
    }
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    if (auto* module = linker->FindByAddress(addr); module != nullptr) {
        return fmt::format("{}+{:#x}", module->file.filename().string(),
                           addr - module->GetBaseAddress());
    }
#ifdef _WIN32
    HMODULE hmod{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &hmod)) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(hmod, path, MAX_PATH);
        std::string name = path;
        if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
        return fmt::format("{}+{:#x}", name, addr - reinterpret_cast<u64>(hmod));
    }
#endif
    return fmt::format("{:#x}", addr);
}

std::string DescribeThread(const Pthread* thread, const std::vector<ThreadSnapshot>& all) {
    if (thread == nullptr) {
        return "none";
    }
    for (const auto& other : all) {
        if (other.guest && other.thread == thread) {
            return fmt::format("{} ({})", other.name, fmt::ptr(thread));
        }
    }
    return fmt::format("{} (exited)", fmt::ptr(thread));
}

std::string DescribeAge(u64 since_ms, u64 now) {
    return since_ms == 0 ? std::string{"never"}
                         : fmt::format("{:.1f}s ago", (now - since_ms) / 1000.0);
}

std::string DescribeWait(const ThreadSnapshot& snap, const std::vector<ThreadSnapshot>& all,
                         u64 now);

std::string DescribeOwnerWait(const Pthread* owner, const std::vector<ThreadSnapshot>& all,
                              u64 now) {
    for (const auto& other : all) {
        if (other.guest && other.thread == owner && other.wait_kind != WaitKind::Mutex) {
            return DescribeWait(other, all, now);
        }
    }
    return "?";
}

std::string DescribeWait(const ThreadSnapshot& snap, const std::vector<ThreadSnapshot>& all,
                         u64 now) {
    if (snap.wait_kind == WaitKind::None) {
        return "not in a tracked wait";
    }
    std::string out =
        fmt::format("in {} obj={} for {:.1f}s, called from {}", WaitKindName(snap.wait_kind),
                    fmt::ptr(snap.wait_obj), (now - snap.wait_since_ms) / 1000.0,
                    DescribeAddress(reinterpret_cast<u64>(snap.wait_caller)));
    if (snap.wait_kind == WaitKind::Mutex && snap.wait_obj != nullptr) {
        const auto* mutex = static_cast<const PthreadMutex*>(snap.wait_obj);
        const Pthread* owner = mutex->m_owner.load(std::memory_order_relaxed);
        out += fmt::format(", owner {}", DescribeThread(owner, all));
        if (owner != nullptr) {
            out += fmt::format(" which is {}", DescribeOwnerWait(owner, all, now));
        }
    } else if (snap.wait_kind == WaitKind::Sema && snap.wait_obj != nullptr) {
        SemaDiag diag{};
        if (GetSemaDiag(snap.wait_obj, diag)) {
            out += fmt::format(
                ", tokens {}/{} waiters {} signals {}, last signal by {} from {} {}, last taken "
                "by {} from {} {}",
                diag.tokens, diag.max_tokens, diag.waiters, diag.signals,
                DescribeThread(diag.last_signaler, all), DescribeAddress(diag.last_signal_caller),
                DescribeAge(diag.last_signal_ms, now), DescribeThread(diag.last_taker, all),
                DescribeAddress(diag.last_take_caller), DescribeAge(diag.last_take_ms, now));
        }
    } else if (snap.wait_kind == WaitKind::Equeue && snap.wait_obj != nullptr) {
        out += fmt::format(", equeue '{}'",
                           static_cast<const EqueueInternal*>(snap.wait_obj)->GetName());
    }
    return out;
}

} // namespace

WaitScope::WaitScope(WaitKind kind, const void* obj, const void* caller) : thread{g_curthread} {
    if (thread == nullptr) {
        return;
    }
    prev_kind = thread->hang_wait_kind.load(std::memory_order_relaxed);
    prev_obj = thread->hang_wait_obj;
    prev_caller = thread->hang_wait_caller;
    prev_since_ms = thread->hang_wait_since_ms;
    thread->hang_wait_obj = obj;
    thread->hang_wait_caller = caller;
    thread->hang_wait_since_ms = NowMs();
    thread->hang_wait_kind.store(kind, std::memory_order_release);
}

WaitScope::~WaitScope() {
    if (thread == nullptr) {
        return;
    }
    thread->hang_wait_obj = prev_obj;
    thread->hang_wait_caller = prev_caller;
    thread->hang_wait_since_ms = prev_since_ms;
    thread->hang_wait_kind.store(prev_kind, std::memory_order_release);
}

void RegisterHangDiagHostThread(std::string_view name) {
    // The GPU command processor is the emulator's busiest thread.
    if (name == "shadPS4:GpuCommandProcessor" && EmulatorSettings.IsGpuThreadCoreReserve()) {
        Common::ReserveCoreForCurrentThread();
    }
#ifdef _WIN32
    HANDLE handle{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        return;
    }
    std::scoped_lock lk{host_threads_mutex};
    host_threads.push_back({std::string{name}, handle, GetCurrentThreadId()});
#endif
}

void DumpThreadsForHang(std::string_view reason) {
    static std::mutex dump_mutex;
    std::scoped_lock dump_lock{dump_mutex};

    std::vector<ThreadSnapshot> snaps;
    auto* state = ThrState::Instance();
    {
        std::scoped_lock lk{state->thread_list_lock};
        snaps.reserve(state->threads.size() + 8);
        for (Pthread* thread : state->threads) {
            if (thread == g_curthread) {
                continue;
            }
            auto& snap = snaps.emplace_back();
            snap.name = thread->name;
            snap.guest = true;
            snap.thread = thread;
            snap.stack.reserve(MaxStackCopy / sizeof(u64));
#ifdef _WIN32
            HANDLE handle = thread->native_thr
                                ? reinterpret_cast<HANDLE>(thread->native_thr->GetHandle())
                                : nullptr;
            const u64 stack_lo = reinterpret_cast<u64>(thread->attr.stackaddr_attr);
            const u64 stack_hi = stack_lo + thread->attr.stacksize_attr;
            if (handle != nullptr && SuspendThread(handle) != static_cast<DWORD>(-1)) {
                snap.wait_kind = thread->hang_wait_kind.load(std::memory_order_acquire);
                snap.wait_obj = thread->hang_wait_obj;
                snap.wait_caller = thread->hang_wait_caller;
                snap.wait_since_ms = thread->hang_wait_since_ms;
                CaptureSuspended(handle, stack_lo, stack_hi, snap);
                ResumeThread(handle);
                continue;
            }
#endif
            snap.wait_kind = thread->hang_wait_kind.load(std::memory_order_acquire);
            snap.wait_obj = thread->hang_wait_obj;
            snap.wait_caller = thread->hang_wait_caller;
            snap.wait_since_ms = thread->hang_wait_since_ms;
        }
    }
#ifdef _WIN32
    {
        std::scoped_lock lk{host_threads_mutex};
        const DWORD self = GetCurrentThreadId();
        for (const auto& host : host_threads) {
            if (host.id == self) {
                continue;
            }
            auto& snap = snaps.emplace_back();
            snap.name = host.name;
            snap.stack.reserve(MaxStackCopy / sizeof(u64));
            if (SuspendThread(host.handle) != static_cast<DWORD>(-1)) {
                CaptureSuspended(host.handle, 0, 0, snap);
                ResumeThread(host.handle);
            }
        }
    }
#endif

    const u64 now = NowMs();
    LOG_CRITICAL(Kernel_Pthread, "==== Hang dump: {} ({} threads) ====", reason, snaps.size());
    for (const auto& snap : snaps) {
        std::string frames;
        size_t num_frames = 0;
#ifdef _WIN32
        for (const u64 value : snap.stack) {
            if (num_frames >= MaxStackFrames) {
                break;
            }
            if (IsExecutable(value)) {
                frames += fmt::format(" {}", DescribeAddress(value));
                ++num_frames;
            }
        }
#endif
        LOG_CRITICAL(Kernel_Pthread, "[{}] {} ({}): {}", snap.guest ? "guest" : "host", snap.name,
                     fmt::ptr(snap.thread), DescribeWait(snap, snaps, now));
        if (snap.have_context) {
            LOG_CRITICAL(Kernel_Pthread, "    rip={} rsp={:#x} rbp={:#x}",
                         DescribeAddress(snap.rip), snap.rsp, snap.rbp);
            if (snap.guest && snap.wait_kind == WaitKind::None) {
                LogRunningCode(snap);
            }
        }
        if (!frames.empty()) {
            LOG_CRITICAL(Kernel_Pthread, "    stack:{}", frames);
        }
    }
    LOG_CRITICAL(Kernel_Pthread, "==== End of hang dump ====");
}

} // namespace Libraries::Kernel
