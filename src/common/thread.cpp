// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>

#include "core/libraries/fiber/fiber.h"
#include "core/libraries/kernel/threads/pthread.h"

#include "common/error.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "ntapi.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#elif defined(_WIN32)
#include <tlhelp32.h>
#include <windows.h>
#include "common/string_util.h"
#else
#if defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#else
#include <pthread.h>
#endif
#include <sched.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __FreeBSD__
#define cpu_set_t cpuset_t
#endif

namespace Common {

#ifdef __APPLE__

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // CPU time to grant.
    const std::chrono::nanoseconds computation_ns = period_ns / 2;

    // Determine the timebase for converting time to ticks.
    struct mach_timebase_info timebase {};
    mach_timebase_info(&timebase);
    const auto ticks_per_ns =
        static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer);

    const auto period_ticks =
        static_cast<u32>(static_cast<double>(period_ns.count()) * ticks_per_ns);
    const auto computation_ticks =
        static_cast<u32>(static_cast<double>(computation_ns.count()) * ticks_per_ns);

    thread_time_constraint_policy policy = {
        .period = period_ticks,
        .computation = computation_ticks,
        // Should not matter since preemptible is false, but needs to be >= computation regardless.
        .constraint = computation_ticks,
        .preemptible = false,
    };

    int ret = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (ret != KERN_SUCCESS) {
        LOG_ERROR(Common, "Could not set thread to real-time with period {} ns: {}",
                  period_ns.count(), ret);
    }
}

#else

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // Not implemented
}

#endif

#ifdef _WIN32

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    auto handle = GetCurrentThread();
    int windows_priority = 0;
    switch (new_priority) {
    case ThreadPriority::Low:
        windows_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case ThreadPriority::Normal:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    case ThreadPriority::High:
        windows_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case ThreadPriority::VeryHigh:
        windows_priority = THREAD_PRIORITY_HIGHEST;
        break;
    case ThreadPriority::Critical:
        windows_priority = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    }
    SetThreadPriority(handle, windows_priority);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();

    LARGE_INTEGER interval{
        .QuadPart = -1 * (duration.count() / 100u),
    };
    // A plain waitable timer only fires on the ~15.6 ms system tick, so short guest sleeps
    // overshoot badly. Use a high-resolution timer where available, created once per thread.
    thread_local const HANDLE timer = [] {
        constexpr DWORD HighResolution = 0x00000002; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        HANDLE handle =
            ::CreateWaitableTimerExW(nullptr, nullptr, HighResolution, TIMER_ALL_ACCESS);
        if (handle == nullptr) {
            handle = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        return handle;
    }();
    SetWaitableTimer(timer, &interval, 0, NULL, NULL, 0);
    const auto ret = WaitForSingleObjectEx(timer, INFINITE, interruptible);

    if (remaining) {
        const auto end_sleep = std::chrono::high_resolution_clock::now();
        const auto sleep_time = end_sleep - begin_sleep;
        *remaining = duration > sleep_time ? duration - sleep_time : std::chrono::nanoseconds(0);
    }
    return ret == WAIT_OBJECT_0;
}

#else

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    pthread_t this_thread = pthread_self();

    const auto scheduling_type = SCHED_OTHER;
    s32 max_prio = sched_get_priority_max(scheduling_type);
    s32 min_prio = sched_get_priority_min(scheduling_type);
    u32 level = std::max(static_cast<u32>(new_priority) + 1, 4U);

    struct sched_param params;
    if (max_prio > min_prio) {
        params.sched_priority = min_prio + ((max_prio - min_prio) * level) / 4;
    } else {
        params.sched_priority = min_prio - ((min_prio - max_prio) * level) / 4;
    }

    pthread_setschedparam(this_thread, scheduling_type, &params);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    timespec request = {
        .tv_sec = duration.count() / 1'000'000'000,
        .tv_nsec = duration.count() % 1'000'000'000,
    };
    timespec remain;
    int ret;
    while ((ret = nanosleep(&request, &remain)) < 0 && errno == EINTR) {
        if (interruptible) {
            break;
        }
        request = remain;
    }
    if (remaining) {
        *remaining = std::chrono::nanoseconds(remain.tv_sec * 1'000'000'000 + remain.tv_nsec);
    }
    return ret == 0 || errno != EINTR;
}

#endif

#ifdef _WIN32

// Sets the debugger-visible name of the current thread.
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());
}

void SetThreadName(void* thread, const char* name) {
    SetThreadDescription(thread, UTF8ToUTF16W(name).data());
}

#else // !_WIN32, so must be POSIX threads

// MinGW with the POSIX threading model does not support pthread_setname_np
#if !defined(_WIN32) || defined(_MSC_VER)
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
#ifdef __APPLE__
    pthread_setname_np(name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__)
    // Linux limits thread names to 15 characters and will outright reject any
    // attempt to set a longer name with ERANGE.
    std::string truncated(name, std::min(strlen(name), static_cast<std::size_t>(15)));
    if (int e = pthread_setname_np(pthread_self(), truncated.c_str())) {
        errno = e;
        LOG_ERROR(Common, "Failed to set thread name to '{}': {}", truncated, GetLastErrorMsg());
    }
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void SetThreadName(void* thread, const char* name) {
    // TODO
}
#endif

#if defined(_WIN32)
void SetCurrentThreadName(const char* name) {
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    // Do Nothing on MinGW
}

void SetThreadName(void* thread, const char* name) {
    // Do Nothing on MinGW
}
#endif

#endif

AccurateTimer::AccurateTimer(std::chrono::nanoseconds target_interval)
    : target_interval(target_interval) {}

void AccurateTimer::Start() {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();
    if (total_wait.count() > 0) {
        // OS timers wake up to a millisecond late. This timer paces vblank and therefore the
        // frames a variable refresh rate display shows, so sleep most of the wait and spin the
        // rest to keep every interval even. (From the Bloodborne DLSS/FSR build by IFreemz.)
        constexpr std::chrono::nanoseconds spin = std::chrono::microseconds(1500);
        if (total_wait > spin) {
            AccurateSleep(total_wait - spin, nullptr, false);
        }
        const auto deadline = begin_sleep + total_wait;
        while (std::chrono::high_resolution_clock::now() < deadline) {
            std::this_thread::yield();
        }
    }
    start_time = std::chrono::high_resolution_clock::now();
    total_wait -= std::chrono::duration_cast<std::chrono::nanoseconds>(start_time - begin_sleep);
}

void AccurateTimer::End() {
    auto now = std::chrono::high_resolution_clock::now();
    total_wait +=
        target_interval - std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time);
}

std::string GetCurrentThreadName() {
    using namespace Libraries::Kernel;
    if (g_curthread && !g_curthread->name.empty()) {
        if (g_curthread->tcb->tcb_fiber) {
            return fmt::format("{}@@{}", g_curthread->name,
                               g_curthread->tcb->tcb_fiber->current_fiber->name);
        }
        return g_curthread->name;
    }
#ifdef _WIN32
    PWSTR name{};
    if (FAILED(GetThreadDescription(GetCurrentThread(), &name)) || name == nullptr) {
        return "<unknown name>";
    }
    const auto result = Common::UTF16ToUTF8(name);
    LocalFree(name);
    return result;
#else
    char name[256];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return std::string{name};
#endif
}

#ifdef _WIN32

namespace {

struct PhysicalCore {
    u64 mask;
    u8 efficiency_class;
};

std::vector<PhysicalCore> EnumeratePhysicalCores() {
    std::vector<PhysicalCore> cores;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) {
        return cores;
    }
    std::vector<std::byte> buffer(length);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        return cores;
    }
    for (std::byte* it = buffer.data(); it < buffer.data() + length;) {
        const auto* entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(it);
        if (entry->Relationship == RelationProcessorCore && entry->Processor.GroupCount > 0 &&
            entry->Processor.GroupMask[0].Group == 0 && entry->Processor.GroupMask[0].Mask != 0) {
            cores.push_back({static_cast<u64>(entry->Processor.GroupMask[0].Mask),
                             entry->Processor.EfficiencyClass});
        }
        it += entry->Size;
    }
    // The last core is the highest numbered one of the most performant class.
    std::ranges::sort(cores, [](const PhysicalCore& a, const PhysicalCore& b) {
        if (a.efficiency_class != b.efficiency_class) {
            return a.efficiency_class < b.efficiency_class;
        }
        return std::countr_zero(a.mask) < std::countr_zero(b.mask);
    });
    return cores;
}

std::atomic<u64> reserved_core_mask{0};
std::atomic<DWORD> reserved_core_owner{0};

void ExcludeReservedCoreFromOtherThreads() {
    const u64 reserved = reserved_core_mask.load(std::memory_order_acquire);
    if (reserved == 0) {
        return;
    }
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        return;
    }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    const DWORD pid = GetCurrentProcessId();
    const DWORD owner = reserved_core_owner.load(std::memory_order_acquire);
    THREADENTRY32 entry{.dwSize = sizeof(THREADENTRY32)};
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == owner) {
            continue;
        }
        const HANDLE thread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE,
                                         entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        GROUP_AFFINITY affinity{};
        u64 current = static_cast<u64>(process_mask);
        if (GetThreadGroupAffinity(thread, &affinity) && affinity.Group == 0) {
            current = static_cast<u64>(affinity.Mask);
        }
        const u64 next = current & ~reserved;
        if ((current & reserved) != 0 && next != 0) {
            affinity = GROUP_AFFINITY{.Mask = static_cast<KAFFINITY>(next), .Group = 0};
            SetThreadGroupAffinity(thread, &affinity, nullptr);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

} // Anonymous namespace

void ReserveCoreForCurrentThread() {
    if (std::thread::hardware_concurrency() < 6 || reserved_core_mask.load() != 0) {
        return;
    }
    const auto cores = EnumeratePhysicalCores();
    if (cores.size() < 4) {
        return;
    }
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        return;
    }
    const u64 core = cores.back().mask & static_cast<u64>(process_mask);
    if (core == 0 || (static_cast<u64>(process_mask) & ~core) == 0) {
        return;
    }
    if (SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(core)) == 0) {
        LOG_WARNING(Common, "Could not reserve CPU core {:#x}: {}", core, GetLastErrorMsg());
        return;
    }
    reserved_core_owner = GetCurrentThreadId();
    reserved_core_mask = core;
    LOG_INFO(Common, "Reserved CPU core {:#x} of {} physical cores for {}", core, cores.size(),
             GetCurrentThreadName());
    ExcludeReservedCoreFromOtherThreads();
    // Windows threads do not inherit their creator's affinity, so threads started later are
    // moved off the core periodically.
    std::thread([] {
        SetCurrentThreadName("shadPS4:CoreReserve");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            ExcludeReservedCoreFromOtherThreads();
        }
    }).detach();
}

#else

void ReserveCoreForCurrentThread() {}

#endif

} // namespace Common
