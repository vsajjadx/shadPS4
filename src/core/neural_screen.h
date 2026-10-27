//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include <SDL3/SDL.h>

struct SDL_Window;

namespace NeuralScreen {

struct Settings {
    bool enabled = true;
    bool boost = true;
    int networkResolutionPercent = 65;
    int passes = 1;
    int profile = 1;
    float intensity = 1.0f;
    float localTone = 0.5f;
    float localStructure = 1.0f;
    float skinStructure = -1.0f;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

const std::vector<std::string>& ProfileOptions();

bool IsRunning();
bool StartForGameWindow(SDL_Window* window);
void Stop();
void ApplySettings();

bool ToggleFrameGeneration();
bool ToggleDLSS5();
bool ToggleBoost();

} // namespace NeuralScreen
