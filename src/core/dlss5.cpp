//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "dlss5.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"

namespace Dlss5 {

using json = nlohmann::json;

constexpr const char* SettingsFileName = "dlss5.json";

const std::vector<std::string>& StyleOptions() {
    static const std::vector<std::string> options = {"Standard", "Natural", "Cinematic"};
    return options;
}

const std::vector<std::string>& NetworkOptions() {
    static const std::vector<std::string> options = {"Default", "Full Quality", "Fast"};
    return options;
}

static Settings g_settings;
static bool g_loaded = false;

static std::filesystem::path SettingsPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / SettingsFileName;
}

Settings& GetSettings() {
    if (!g_loaded) {
        LoadSettings();
    }
    return g_settings;
}

void LoadSettings() {
    g_loaded = true;
    g_settings = Settings{};
    std::ifstream file(SettingsPath());
    if (!file) {
        return;
    }
    try {
        const json j = json::parse(file);
        auto& s = g_settings;
        s.enabled = j.value("Enabled", s.enabled);
        s.style =
            std::clamp(j.value("Style", s.style), 0, static_cast<int>(StyleOptions().size()) - 1);
        s.intensity = std::clamp(j.value("Intensity", s.intensity), 0.0f, 1.0f);
        s.globalTone = std::clamp(j.value("GlobalTone", s.globalTone), 0.0f, 2.0f);
        s.localTone = std::clamp(j.value("LocalTone", s.localTone), 0.0f, 2.0f);
        s.localStructure = std::clamp(j.value("LocalStructure", s.localStructure), 0.0f, 2.0f);
        s.skinStructure = std::clamp(j.value("SkinStructure", s.skinStructure), -1.0f, 2.5f);
        s.autoMask = j.value("AutoMask", s.autoMask);
        s.uiCorrection = std::clamp(j.value("UICorrection", s.uiCorrection), 0, 1);
        s.preset = std::clamp(j.value("Preset", s.preset), 0,
                              static_cast<int>(NetworkOptions().size()) - 1);
    } catch (const json::exception& e) {
        LOG_ERROR(Loader, "DLSS5 settings could not be read: {}", e.what());
    }
}

void SaveSettings() {
    const auto& s = g_settings;
    json j;
    j["Enabled"] = s.enabled;
    j["Style"] = s.style;
    j["Intensity"] = s.intensity;
    j["GlobalTone"] = s.globalTone;
    j["LocalTone"] = s.localTone;
    j["LocalStructure"] = s.localStructure;
    j["SkinStructure"] = s.skinStructure;
    j["Preset"] = s.preset;
    j["AutoMask"] = s.autoMask;
    j["UICorrection"] = s.uiCorrection;

    std::error_code ec;
    std::filesystem::create_directories(SettingsPath().parent_path(), ec);
    std::ofstream file(SettingsPath());
    file << j.dump(2) << std::endl;
}

static std::atomic<u32> g_generation{0};

u32 SettingsGeneration() {
    return g_generation.load(std::memory_order_relaxed);
}

void ApplySettings() {
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

std::optional<bool> ToggleDLSS5() {
    auto& settings = GetSettings();
    settings.enabled = !settings.enabled;
    SaveSettings();
    ApplySettings();
    return settings.enabled;
}

} // namespace Dlss5
