// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include "common/logging/log.h"

namespace Common {

/// Logs when a driver call takes longer than the threshold. Used to find which call a GPU/worker
/// thread is stuck in when the driver stalls for a long time.
class SlowCallScope {
public:
    explicit SlowCallScope(const char* name_, std::chrono::milliseconds threshold_ =
                                                  std::chrono::milliseconds{1000})
        : name{name_}, threshold{threshold_}, start{std::chrono::steady_clock::now()} {}

    ~SlowCallScope() {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= threshold) {
            LOG_CRITICAL(Render_Vulkan, "Slow driver call: {} took {} ms", name,
                         std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        }
    }

private:
    const char* name;
    std::chrono::milliseconds threshold;
    std::chrono::steady_clock::time_point start;
};

} // namespace Common
