// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>

#include "video_core/renderer_vulkan/vk_frame_generation.h"

namespace Vulkan::FrameGeneration {

namespace {
std::atomic<Backend> backend{Backend::Lemon};
std::atomic_bool enabled{false};
std::array<std::atomic_bool, 2> available{true, true};
std::atomic_bool retry_requested{false};
} // Anonymous namespace

Backend BackendFromSetting([[maybe_unused]] std::string_view name) {
#ifdef ENABLE_FSR_FRAMEGEN
    if (name != "Lemon") {
        return Backend::Fsr;
    }
#endif
    return Backend::Lemon;
}

std::string_view BackendName(Backend selected) {
    return selected == Backend::Fsr ? "FSR 3.1" : "Multi FG";
}

void SetBackend(Backend selected) {
    backend = selected;
}

Backend GetBackend() {
    return backend;
}

void SetEnabled(bool enable) {
    enabled = enable;
    if (enable) {
        // Turning it on again also resumes a generator that paused after a failed frame.
        retry_requested = true;
    }
}

bool IsEnabled() {
    return enabled && IsAvailable();
}

bool IsAvailable() {
    return available[static_cast<size_t>(GetBackend())];
}

void SetAvailable(Backend selected, bool is_available) {
    available[static_cast<size_t>(selected)] = is_available;
}

bool RetryRequested() {
    return retry_requested;
}

void ClearRetryRequest() {
    retry_requested = false;
}

} // namespace Vulkan::FrameGeneration
