// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <limits>
#include <vector>
#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = 2'000'000'000ULL;
// A GPU submission that is still not done after this long will not finish: it is a hang the
// driver did not reset (no device loss is ever reported), and waiting on only freezes the game.
constexpr u32 GPU_HANG_ABORT_S = 30;

namespace {
constexpr size_t TRACE_ENTRIES = 2048;
struct TraceEntry {
    std::atomic<u64> seq{0}; // 0 = empty, written last
    std::atomic<u64> tick{0};
    std::atomic<u64> primary{0};
    std::atomic<u64> secondary{0};
    std::atomic<bool> compute{false};
};
std::array<TraceEntry, TRACE_ENTRIES> g_trace;
std::atomic<u64> g_trace_seq{0};
} // namespace

void TraceGpuWork(u64 tick, bool compute, u64 primary_hash, u64 secondary_hash) {
    const u64 seq = g_trace_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& entry = g_trace[seq % TRACE_ENTRIES];
    entry.tick.store(tick, std::memory_order_relaxed);
    entry.primary.store(primary_hash, std::memory_order_relaxed);
    entry.secondary.store(secondary_hash, std::memory_order_relaxed);
    entry.compute.store(compute, std::memory_order_relaxed);
    entry.seq.store(seq, std::memory_order_release);
}

void LogGpuWorkSince(u64 first_tick) {
    struct Item {
        u64 seq, tick, primary, secondary;
        bool compute;
    };
    std::vector<Item> items;
    for (auto& entry : g_trace) {
        const u64 seq = entry.seq.load(std::memory_order_acquire);
        if (seq == 0) {
            continue;
        }
        const u64 tick = entry.tick.load(std::memory_order_relaxed);
        if (tick >= first_tick) {
            items.push_back({seq, tick, entry.primary.load(std::memory_order_relaxed),
                             entry.secondary.load(std::memory_order_relaxed),
                             entry.compute.load(std::memory_order_relaxed)});
        }
    }
    std::ranges::sort(items, {}, &Item::seq);
    LOG_CRITICAL(Render_Vulkan, "GPU work not finished (tick >= {}): {} draws/dispatches",
                 first_tick, items.size());
    constexpr size_t MaxLogged = 150;
    const size_t start = items.size() > MaxLogged ? items.size() - MaxLogged : 0;
    for (size_t i = start; i < items.size(); ++i) {
        const auto& item = items[i];
        if (item.compute) {
            LOG_CRITICAL(Render_Vulkan, "  tick {} dispatch cs {:#x}", item.tick, item.primary);
        } else {
            LOG_CRITICAL(Render_Vulkan, "  tick {} draw vs {:#x} fs {:#x}", item.tick,
                         item.primary, item.secondary);
        }
    }
}

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    // Only a timeout is worth retrying. Any other result (most importantly a lost device after a
    // GPU reset) will never turn into success, so retrying it spins the calling thread forever
    // and freezes the game instead of failing.
    u32 waited_s = 0;
    for (;;) {
        const vk::Result result = instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT);
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            instance.LogDeviceFault();
        }
        ASSERT_MSG(result == vk::Result::eTimeout, "Failed to wait for the GPU: vk::Result={}",
                   vk::to_string(result));
        waited_s += 2;
        Refresh();
        LOG_WARNING(Render_Vulkan,
                    "Waited {}s for GPU tick {} (gpu tick {}, next submit tick {}, thread {}){}",
                    waited_s, tick, KnownGpuTick(), CurrentTick(), Common::GetCurrentThreadName(),
                    tick >= CurrentTick() ? ": tick was never submitted" : "");
        if (waited_s >= GPU_HANG_ABORT_S) {
            // Fail like a lost device would, so the fault is logged and the crash handler writes
            // its dump.
            instance.LogDeviceFault();
            LogGpuWorkSince(tick);
            UNREACHABLE_MSG("GPU hang: waited {}s for GPU tick {} (gpu tick {}, next submit tick {})",
                            waited_s, tick, KnownGpuTick(), CurrentTick());
        }
    }
    Refresh();
}

} // namespace Vulkan
