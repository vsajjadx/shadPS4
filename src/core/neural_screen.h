//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

struct SDL_Window;

namespace NeuralScreen {

struct Settings {
    bool enabled = true;
    bool boost = true;
    // NeuralScreen's automatic resolution (auto_quality); acts with Boost on.
    bool autoQuality = false;
    int networkResolutionPercent = 65;
    int passes = 1;
    int profile = 1;
    // Which network produces the picture: Default, Natural or Cinematic.
    int model = 1;
    float intensity = 1.0f;
    // Fine tuning. The defaults are what NeuralScreen used before these
    // options were exposed (its fixed 1.5), so an untouched install looks
    // the same.
    float localTone = 1.5f;
    float localStructure = 1.5f;
    float skinStructure = 1.5f;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

const std::vector<std::string>& ProfileOptions();
const std::vector<std::string>& ModelOptions();

bool IsRunning();
bool StartForGameWindow(SDL_Window* window);
void Stop();
void ApplySettings();

// The rate NeuralScreen really shows (real + generated frames), or nothing when
// it is not running or has nothing to report - then the emulator's own counter
// is the right one.
std::optional<float> GetOutputFps();

std::optional<bool> ToggleFrameGeneration();
std::optional<bool> ToggleDLSS5();
std::optional<bool> ToggleBoost();

} // namespace NeuralScreen
