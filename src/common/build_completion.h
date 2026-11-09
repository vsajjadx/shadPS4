// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

namespace Common {

class BuildCompletion {
public:
    [[nodiscard]] bool IsReady() const noexcept {
        return ready.load(std::memory_order_acquire);
    }

    void Complete() noexcept {
        ready.store(true, std::memory_order_release);
        ready.notify_all();
    }

    void Wait() const noexcept {
        ready.wait(false, std::memory_order_acquire);
    }

private:
    std::atomic<bool> ready{false};
};

} // namespace Common
