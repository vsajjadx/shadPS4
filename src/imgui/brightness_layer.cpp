// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "imgui/brightness_layer.h"

namespace ImGui {

namespace {

constexpr int MinBrightness = 25;
constexpr int MaxBrightness = 300;

BrightnessLayer& Instance() {
    static BrightnessLayer layer;
    return layer;
}

// Only the brightness entry is written, so values the emulator forces for a game at boot don't
// end up saved in its config.
void SaveBrightness(const std::filesystem::path& path, int brightness) {
    nlohmann::json config = nlohmann::json::object();
    if (std::ifstream in{path}; in) {
        config = nlohmann::json::parse(in, nullptr, false);
        if (!config.is_object()) {
            LOG_ERROR(Config, "Not saving brightness: {} is not valid JSON", path.string());
            return;
        }
    }
    config["GPU"]["brightness"] = brightness;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path};
    out << config.dump(2);
}

} // namespace

void BrightnessLayer::Toggle() {
    auto& layer = Instance();
    if (!layer.added) {
        layer.added = true;
        AddLayer(&layer);
    }
    layer.visible = !layer.visible;
}

void BrightnessLayer::Draw() {
    if (!visible) {
        return;
    }
    const auto& io = GetIO();
    SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.15f}, ImGuiCond_Appearing,
                     {0.5f, 0.0f});
    SetNextWindowSize({420.0f, 0.0f}, ImGuiCond_Appearing);
    bool open = true;
    if (Begin("Brightness", &open, ImGuiWindowFlags_NoCollapse)) {
        int brightness = EmulatorSettings.GetBrightness();
        const auto game_specific = std::filesystem::exists(
            Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
            (std::string{Common::ElfInfo::Instance().GameSerial()} + ".json"));
        SetNextItemWidth(-1.0f);
        if (SliderInt("##brightness", &brightness, MinBrightness, MaxBrightness, "%d%%")) {
            EmulatorSettings.SetBrightness(brightness, game_specific);
        }
        if (Button("Reset")) {
            brightness = 100;
            EmulatorSettings.SetBrightness(brightness, game_specific);
        }
        SameLine();
        if (Button("Save for this game")) {
            const auto serial = std::string{Common::ElfInfo::Instance().GameSerial()};
            EmulatorSettings.SetBrightness(brightness, true);
            SaveBrightness(Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
                               (serial + ".json"),
                           brightness);
        }
        SameLine();
        if (Button("Save for all games")) {
            EmulatorSettings.SetBrightness(brightness, false);
            SaveBrightness(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.json",
                           brightness);
        }
    }
    End();
    if (!open) {
        visible = false;
    }
}

} // namespace ImGui
