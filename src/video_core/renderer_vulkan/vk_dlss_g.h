// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

namespace Vulkan::DlssG {

inline std::atomic_bool enabled{false};

inline bool IsAvailable() {
    return false;
}

inline void SetEnabled(bool value) {
    enabled = value;
}

inline bool IsEnabled() {
    return enabled;
}

} // namespace Vulkan::DlssG
