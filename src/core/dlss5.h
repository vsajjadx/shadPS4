//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

/// Settings of DLSS5 neural rendering, which the renderer runs on the game picture.
namespace Dlss5 {

struct Settings {
    bool enabled = false;
    /// NGX style: Standard, Natural or Cinematic.
    int style = 0;
    float intensity = 1.0f;
    float globalTone = 1.0f;
    float localTone = 1.5f;
    float localStructure = 1.5f;
    float skinStructure = 1.5f;
    int preset = 0;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

const std::vector<std::string>& StyleOptions();
const std::vector<std::string>& NetworkOptions();

/// Counts the settings changes, so the renderer rebuilds its pass after one.
u32 SettingsGeneration();
void ApplySettings();

std::optional<bool> ToggleDLSS5();

} // namespace Dlss5
