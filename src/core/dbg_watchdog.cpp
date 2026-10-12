// Diagnostic watchdog: periodically reports what every guest thread is doing.
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/dbg_wait.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/dbg_watchdog.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/thread_state.h"
#include "core/linker.h"
#include "core/module.h"
#include "core/thread.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Core {

namespace {

std::string Describe(u64 addr) {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    if (auto* m = linker->FindByAddress(addr)) {
        return fmt::format("{}+{:#x}", m->name, addr - m->GetBaseAddress());
    }
#ifdef _WIN32
    const auto base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
    if (addr >= base && addr < base + 0x10000000) {
        return fmt::format("host+{:#x}", addr - base);
    }
#endif
    return fmt::format("{:#x}", addr);
}

bool LooksLikeReturnAddress(u64 addr) {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* m = linker->FindByAddress(addr);
    if (!m || addr - m->GetBaseAddress() < 8) {
        return false;
    }
    const auto* p = reinterpret_cast<const u8*>(addr);
    return p[-5] == 0xE8 || p[-2] == 0xFF || p[-3] == 0xFF || p[-6] == 0xFF || p[-7] == 0xFF;
}

struct Sample {
    std::string name;
    s32 tid{};
    bool waiting{};
    std::string what;
    u64 waited_ms{};
    u64 rip{};
    std::vector<u64> frames;
};

void Report(int n) {
    using namespace Libraries::Kernel;
    const u64 now = Dbg::NowMs();
    std::vector<Sample> samples;
    std::vector<std::pair<Pthread*, void*>> targets;

    auto* ts = ThrState::Instance();
    {
        std::unique_lock lk{ts->thread_list_lock, std::try_to_lock};
        if (!lk.owns_lock()) {
            LOG_ERROR(Loader, "WATCHDOG #{}: thread list busy", n);
            return;
        }
        for (auto* t : ts->threads) {
            Sample s;
            s.name = t->name;
            s.tid = t->tid.load();
            for (auto& slot : Dbg::g_slots) {
                const char* what = slot.what.load();
                if (what && slot.owner.load() == static_cast<void*>(t)) {
                    s.waiting = true;
                    s.what = std::string(what) + " " + slot.detail;
                    s.waited_ms = now - slot.since_ms.load();
                    break;
                }
            }
            void* handle = nullptr;
            if (!s.waiting && t->native_thr) {
                handle = reinterpret_cast<void*>(t->native_thr->GetHandle());
            }
            samples.push_back(std::move(s));
            targets.emplace_back(t, handle);
        }
    }

#ifdef _WIN32
    for (size_t i = 0; i < samples.size(); ++i) {
        auto& s = samples[i];
        HANDLE h = static_cast<HANDLE>(targets[i].second);
        if (s.waiting || !h) {
            continue;
        }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(h) == static_cast<DWORD>(-1)) {
            continue;
        }
        const bool ok = GetThreadContext(h, &ctx) != 0;
        ResumeThread(h);
        if (!ok) {
            continue;
        }
        s.rip = ctx.Rip;
        std::vector<u64> stack(1024);
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ctx.Rsp), stack.data(),
                              stack.size() * 8, &got) ||
            got > 0) {
            for (size_t k = 0; k < got / 8 && s.frames.size() < 8; ++k) {
                if (LooksLikeReturnAddress(stack[k])) {
                    s.frames.push_back(stack[k]);
                }
            }
        }
    }
#endif

    LOG_ERROR(Loader, "WATCHDOG #{}: flips={} gpu_submits={} threads={}", n,
              Dbg::g_flips.load(), Dbg::g_gpu_submits.load(), samples.size());
    for (const auto& s : samples) {
        if (s.waiting) {
            LOG_ERROR(Loader, "WATCHDOG #{}:  [{}] tid={} WAIT {} for {} ms", n, s.name, s.tid,
                      s.what, s.waited_ms);
        } else {
            std::string fr;
            for (u64 f : s.frames) {
                fr += Describe(f) + " ";
            }
            LOG_ERROR(Loader, "WATCHDOG #{}:  [{}] tid={} RUN rip={} guest_frames: {}", n, s.name,
                      s.tid, s.rip ? Describe(s.rip) : "?", fr);
        }
    }
}

} // namespace

std::string DescribeGuestCrash(u64 rip, u64 rsp) {
    std::string out = fmt::format("rip={}", Describe(rip));
#ifdef _WIN32
    if (rsp != 0) {
        std::vector<u64> stack(1024);
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(rsp), stack.data(),
                              stack.size() * 8, &got) ||
            got > 0) {
            out += " frames:";
            size_t count = 0;
            for (size_t k = 0; k < got / 8 && count < 12; ++k) {
                if (LooksLikeReturnAddress(stack[k])) {
                    out += " " + Describe(stack[k]);
                    ++count;
                }
            }
        }
    }
#endif
    return out;
}

void StartDebugWatchdog() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(40));
        for (int n = 1; n <= 80; ++n) {
            Report(n);
            std::this_thread::sleep_for(std::chrono::seconds(15));
        }
    }).detach();
}

} // namespace Core
