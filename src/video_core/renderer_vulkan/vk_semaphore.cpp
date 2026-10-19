// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <limits>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = 1'000'000'000ULL;
constexpr auto SLOW_WAIT_REPORT = std::chrono::seconds(5);

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
        if (counter_result == vk::Result::eErrorDeviceLost) {
            DumpWorkOnDeviceLost();
        }
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick, std::source_location caller) {
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

    const auto start = std::chrono::steady_clock::now();
    auto next_report = start + SLOW_WAIT_REPORT;
    while (true) {
        const auto result = instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT);
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            DumpWorkOnDeviceLost();
        }
        ASSERT_MSG(result == vk::Result::eTimeout, "Timeline semaphore wait failed: {}",
                   vk::to_string(result));
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_report) {
            Refresh();
            LOG_CRITICAL(Render_Vulkan,
                         "GPU wait for tick {} blocked for {}s (gpu tick {}, current tick {}) at "
                         "{}:{}",
                         tick, std::chrono::duration_cast<std::chrono::seconds>(now - start).count(),
                         KnownGpuTick(), CurrentTick(), caller.file_name(), caller.line());
            next_report = now + SLOW_WAIT_REPORT;
        }
    }
    Refresh();
}

} // namespace Vulkan
