//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "dlss5.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"

namespace Dlss5 {

using json = nlohmann::json;

constexpr const char* SettingsFileName = "dlss5.json";
constexpr const char* ScalingModeName = "DLSS5-RTX30-PTX";
constexpr const char* ProfileName = "shadPS4";

const std::vector<std::string>& StyleOptions() {
    static const std::vector<std::string> options = {"Standard", "Natural", "Cinematic"};
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
        s.boost = j.value("Boost", s.boost);
        s.networkResolutionPercent =
            std::clamp(j.value("NetworkResolutionPercent", s.networkResolutionPercent), 30, 100);
        s.style =
            std::clamp(j.value("Style", s.style), 0, static_cast<int>(StyleOptions().size()) - 1);
        s.intensity = std::clamp(j.value("Intensity", s.intensity), 0.0f, 1.0f);
        s.localTone = std::clamp(j.value("LocalTone", s.localTone), 0.0f, 2.0f);
        s.localStructure = std::clamp(j.value("LocalStructure", s.localStructure), 0.0f, 2.0f);
        s.skinStructure = std::clamp(j.value("SkinStructure", s.skinStructure), -1.0f, 2.5f);
    } catch (const json::exception& e) {
        LOG_ERROR(Loader, "DLSS5 settings could not be read: {}", e.what());
    }
}

void SaveSettings() {
    const auto& s = g_settings;
    json j;
    j["Enabled"] = s.enabled;
    j["Boost"] = s.boost;
    j["NetworkResolutionPercent"] = s.networkResolutionPercent;
    j["Style"] = s.style;
    j["Intensity"] = s.intensity;
    j["LocalTone"] = s.localTone;
    j["LocalStructure"] = s.localStructure;
    j["SkinStructure"] = s.skinStructure;

    std::error_code ec;
    std::filesystem::create_directories(SettingsPath().parent_path(), ec);
    std::ofstream file(SettingsPath());
    file << j.dump(2) << std::endl;
}

#ifdef _WIN32

static PROCESS_INFORMATION g_process = {};
static bool g_running = false;
static std::wstring g_window_class;

/// A job that ends Magpie when the emulator exits in any way, even when it crashes or is killed.
static HANDLE LifetimeJob() {
    static const HANDLE job = [] {
        const HANDLE created = CreateJobObjectW(nullptr, nullptr);
        if (!created) {
            return created;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits));
        return created;
    }();
    return job;
}

static std::filesystem::path FindDirectory() {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return {};
    }
    const auto dir = std::filesystem::u8path(base) / "qtplugins" / "DLSS5";
    std::error_code ec;
    if (std::filesystem::is_regular_file(dir / "Magpie.exe", ec) &&
        std::filesystem::is_regular_file(dir / "nvngx_dlssnr.dll", ec)) {
        return dir;
    }
    return {};
}

static std::filesystem::path ExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

/// Points Magpie's DLSS5 scaling mode at the current settings and adds a profile that scales the
/// emulator window with it as soon as the window is in front.
static bool WriteMagpieConfig(const std::filesystem::path& dir) {
    const auto path = dir / "config" / "config.json";
    json config;
    try {
        std::ifstream file(path);
        config = json::parse(file);
    } catch (const json::exception& e) {
        LOG_ERROR(Loader, "DLSS5: Magpie's config.json could not be read: {}", e.what());
        return false;
    }

    auto& modes = config["scalingModes"];
    const auto mode = std::ranges::find_if(
        modes, [](const json& m) { return m.value("name", std::string{}) == ScalingModeName; });
    if (mode == modes.end() || !(*mode)["effects"].is_array() || (*mode)["effects"].empty()) {
        LOG_ERROR(Loader, "DLSS5: Magpie has no {} scaling mode", ScalingModeName);
        return false;
    }
    const auto& s = g_settings;
    auto& params = (*mode)["effects"][0]["parameters"];
    params["style"] = static_cast<double>(s.style);
    params["intensity"] = s.intensity;
    params["localToneStrength"] = s.localTone;
    params["localStructureStrength"] = s.localStructure;
    params["skinStructureStrength"] = s.skinStructure;
    params["enableInputResolutionScaling"] = s.boost ? 1.0 : 0.0;
    params["inputResolutionPercent"] = static_cast<double>(s.networkResolutionPercent);

    auto& profiles = config["profiles"];
    if (!profiles.is_array() || profiles.empty()) {
        LOG_ERROR(Loader, "DLSS5: Magpie's config.json has no default profile");
        return false;
    }
    std::erase_if(profiles.get_ref<json::array_t&>(),
                  [](const json& p) { return p.value("name", std::string{}) == ProfileName; });
    const std::string executable = Common::FS::PathToUTF8String(ExecutablePath());
    json profile = profiles[0];
    profile["name"] = ProfileName;
    profile["packaged"] = false;
    profile["pathRule"] = executable;
    profile["classNameRule"] = Common::FS::PathToUTF8String(g_window_class);
    profile["launcherPath"] = executable;
    profile["launchParameters"] = "";
    profile["autoScale"] = true;
    profile["scalingMode"] = std::distance(modes.begin(), mode);
    profiles.push_back(std::move(profile));

    std::ofstream file(path);
    file << config.dump(4) << std::endl;
    return static_cast<bool>(file);
}

static bool StartMagpie() {
    const auto dir = FindDirectory();
    if (dir.empty()) {
        LOG_WARNING(Loader, "DLSS5: Magpie was not found in qtplugins\\DLSS5 next to the "
                            "emulator, neural rendering is off");
        return false;
    }
    if (!WriteMagpieConfig(dir)) {
        return false;
    }
    std::wstring command = L"\"" + (dir / "Magpie.exe").wstring() + L"\" -t";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        dir.wstring().c_str(), &startup, &g_process)) {
        LOG_WARNING(Loader, "DLSS5: Magpie could not be started ({}), neural rendering is off",
                    GetLastError());
        return false;
    }
    if (const HANDLE job = LifetimeJob();
        !job || !AssignProcessToJobObject(job, g_process.hProcess)) {
        LOG_WARNING(Loader, "DLSS5: could not tie Magpie to the emulator's lifetime ({})",
                    GetLastError());
    }
    ResumeThread(g_process.hThread);
    g_running = true;
    LOG_INFO(Loader, "DLSS5 started (Magpie {}, style {}, input {})", ScalingModeName,
             StyleOptions()[g_settings.style],
             g_settings.boost ? std::to_string(g_settings.networkResolutionPercent) + "%"
                              : std::string{"100%"});
    return true;
}

bool IsRunning() {
    return g_running;
}

bool StartForGameWindow(SDL_Window* window) {
    const HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    std::wstring window_class(256, L'\0');
    if (hwnd) {
        window_class.resize(
            GetClassNameW(hwnd, window_class.data(), static_cast<int>(window_class.size())));
    }
    g_window_class = window_class.empty() ? L"*" : window_class;
    if (g_running || !GetSettings().enabled) {
        return false;
    }
    return StartMagpie();
}

void Stop() {
    if (!g_running) {
        return;
    }
    // Killed rather than closed, so Magpie does not write its own state over the config.
    TerminateProcess(g_process.hProcess, 0);
    WaitForSingleObject(g_process.hProcess, 5000);
    CloseHandle(g_process.hProcess);
    CloseHandle(g_process.hThread);
    g_process = {};
    g_running = false;
    LOG_INFO(Loader, "DLSS5 stopped");
}

void ApplySettings() {
    if (g_running) {
        Stop();
        StartMagpie();
    }
}

std::optional<bool> ToggleDLSS5() {
    if (g_running) {
        Stop();
        return false;
    }
    if (!StartMagpie()) {
        return std::nullopt;
    }
    return true;
}

std::optional<bool> ToggleBoost() {
    GetSettings().boost = !GetSettings().boost;
    SaveSettings();
    ApplySettings();
    return GetSettings().boost;
}

#else

bool IsRunning() {
    return false;
}

bool StartForGameWindow(SDL_Window*) {
    return false;
}

void Stop() {}

void ApplySettings() {}

std::optional<bool> ToggleDLSS5() {
    return std::nullopt;
}

std::optional<bool> ToggleBoost() {
    return std::nullopt;
}

#endif

} // namespace Dlss5
