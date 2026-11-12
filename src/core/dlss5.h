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
    int style = 1;
    float intensity = 1.0f;
    float globalTone = 1.0f;
    float localTone = 0.0f;
    float localStructure = 1.0f;
    float skinStructure = 1.0f;
    /// Network: 0 Default, 1 Full Quality, 2 Fast. Full Quality blends earlier frames itself
    /// (moving players ghost) and ran at a third of the frame rate (NHL 24: 149 fps off, 50 Full
    /// Quality, 71 Default/Fast), so Default is the default.
    int preset = 0;
    bool autoMask = true;
    /// Blends earlier frames in; without motion vectors moving objects leave a trail.
    bool temporal = false;
    bool useDepth = true;
    bool useMotion = true;
    bool beforeHud = true;
    int uiCorrection = 0;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

const std::vector<std::string>& StyleOptions();
const std::vector<std::string>& NetworkOptions();

/// Picks up changes another program (the launcher) made to the settings file.
void ReloadIfChanged();

/// Counts the settings changes, so the renderer rebuilds its pass after one.
u32 SettingsGeneration();
void ApplySettings();

std::optional<bool> ToggleDLSS5();

} // namespace Dlss5
