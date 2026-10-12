// Diagnostic helpers: lightweight registry of blocking waits + GPU progress counters.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace Dbg {

struct WaitSlot {
    std::atomic<const char*> what{nullptr};
    char detail[48]{};
    std::atomic<std::uint64_t> since_ms{0};
    std::atomic<void*> owner{nullptr};
};

inline std::array<WaitSlot, 1024> g_slots;
inline std::atomic<int> g_next_slot{0};
inline thread_local int t_slot = -1;
inline std::atomic<std::uint64_t> g_flips{0};
inline std::atomic<std::uint64_t> g_gpu_submits{0};

inline std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

class WaitGuard {
public:
    explicit WaitGuard(const char* what, const char* detail = nullptr, void* owner = nullptr) {
        if (t_slot < 0) {
            const int idx = g_next_slot.fetch_add(1);
            if (idx >= static_cast<int>(g_slots.size())) {
                return;
            }
            t_slot = idx;
        }
        auto& s = g_slots[t_slot];
        if (detail) {
            std::strncpy(s.detail, detail, sizeof(s.detail) - 1);
            s.detail[sizeof(s.detail) - 1] = 0;
        } else {
            s.detail[0] = 0;
        }
        s.owner.store(owner);
        s.since_ms.store(NowMs());
        s.what.store(what);
        active = true;
    }
    ~WaitGuard() {
        if (active && t_slot >= 0) {
            g_slots[t_slot].what.store(nullptr);
        }
    }
    WaitGuard(const WaitGuard&) = delete;
    WaitGuard& operator=(const WaitGuard&) = delete;

private:
    bool active{};
};

} // namespace Dbg
