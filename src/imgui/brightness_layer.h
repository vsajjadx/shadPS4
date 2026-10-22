// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include "imgui/imgui_layer.h"

namespace ImGui {

/// Brightness slider opened from the window's title bar menu.
class BrightnessLayer final : public Layer {
public:
    static void Toggle();

    void Draw() override;

private:
    std::atomic<bool> visible{false};
    bool added{false};
};

} // namespace ImGui
