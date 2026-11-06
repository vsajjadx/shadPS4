//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

/// DLSS5 neural rendering through the bundled Magpie build (DLSS5-RTX30, RTX 30 series): the
/// emulator writes a Magpie profile that scales its own window with the DLSSNR -> DLSS_SR chain
/// and starts or stops Magpie for the DLSS5 toggle.
namespace Dlss5 {

struct Settings {
    bool enabled = false;
    /// Runs the network below the window resolution.
    bool boost = false;
    int networkResolutionPercent = 50;
    /// NGX style: Standard, Natural or Cinematic.
    int style = 1;
    float intensity = 1.0f;
    float localTone = 1.0f;
    float localStructure = 1.0f;
    float skinStructure = -1.0f;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

const std::vector<std::string>& StyleOptions();

bool IsRunning();
bool StartForGameWindow(SDL_Window* window);
void Stop();
/// Writes the settings to Magpie and restarts it when it runs.
void ApplySettings();

std::optional<bool> ToggleDLSS5();
std::optional<bool> ToggleBoost();

} // namespace Dlss5
