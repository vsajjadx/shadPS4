// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace Common::Log {

// A per-class fixed-window budget. The epoch and count are one atomic state so
// two producers cannot both reset a window and admit an unbounded trace storm.
// Callers supply steady-clock seconds; delayed callers cannot roll time backward.
class alignas(64) VerboseLogBudget {
public:
    static constexpr std::uint32_t Limit = 512;

    [[nodiscard]] bool Allow(std::uint64_t second) {
        auto state = window.load(std::memory_order_relaxed);
        for (;;) {
            const auto epoch = state >> 32;
            const auto count = static_cast<std::uint32_t>(state);
            std::uint64_t next{};
            if (second > epoch) {
                next = (second << 32) | 1;
            } else if (count < Limit) {
                next = state + 1;
            } else {
                suppressed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (window.compare_exchange_weak(state, next, std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    [[nodiscard]] std::uint64_t Suppressed() const {
        return suppressed.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> window{};
    std::atomic<std::uint64_t> suppressed{};
};

} // namespace Common::Log
