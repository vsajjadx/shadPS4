//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#endif

#include "neural_screen.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"

namespace NeuralScreen {

using json = nlohmann::json;

constexpr int ControlPort = 53194;
constexpr const char* SettingsFileName = "neural_screen.json";
constexpr const char* AppConfigFileName = "neural_screen_app.json";

const std::vector<std::string>& ProfileOptions() {
    static const std::vector<std::string> options = {"Faithful", "Natural", "Strong / Cinematic",
                                                     "Extreme / Overdrive"};
    return options;
}

const std::vector<std::string>& ModelOptions() {
    static const std::vector<std::string> options = {"Default", "Natural", "Cinematic"};
    return options;
}

static Settings g_settings;
static bool g_loaded = false;
#ifdef _WIN32
static PROCESS_INFORMATION g_process = {};
static bool g_running = false;
#endif

static std::filesystem::path SettingsPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / SettingsFileName;
}

static std::filesystem::path AppConfigPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / AppConfigFileName;
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
        json j = json::parse(file);
        auto& s = g_settings;
        s.enabled = j.value("Enabled", s.enabled);
        s.boost = j.value("Boost", s.boost);
        s.networkResolutionPercent =
            std::clamp(j.value("NetworkResolutionPercent", s.networkResolutionPercent), 30, 100);
        s.passes = std::clamp(j.value("Passes", s.passes), 1, 4);
        s.profile = std::clamp(j.value("Profile", s.profile), 0,
                               static_cast<int>(ProfileOptions().size()) - 1);
        s.model = std::clamp(j.value("Model", s.model), 0,
                             static_cast<int>(ModelOptions().size()) - 1);
        s.intensity = std::clamp(j.value("Intensity", s.intensity), 0.0f, 1.0f);
        s.localTone = std::clamp(j.value("LocalTone", s.localTone), 0.0f, 2.0f);
        s.localStructure = std::clamp(j.value("LocalStructure", s.localStructure), 0.0f, 2.0f);
        s.skinStructure = std::clamp(j.value("SkinStructure", s.skinStructure), -1.0f, 2.5f);
    } catch (const json::exception& e) {
        LOG_ERROR(Loader, "NeuralScreen settings could not be read: {}", e.what());
    }
}

void SaveSettings() {
    const auto& s = g_settings;
    json j;
    j["Enabled"] = s.enabled;
    j["Boost"] = s.boost;
    j["NetworkResolutionPercent"] = s.networkResolutionPercent;
    j["Passes"] = s.passes;
    j["Profile"] = s.profile;
    j["Model"] = s.model;
    j["Intensity"] = s.intensity;
    j["LocalTone"] = s.localTone;
    j["LocalStructure"] = s.localStructure;
    j["SkinStructure"] = s.skinStructure;

    std::error_code ec;
    auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    std::filesystem::create_directories(dir, ec);
    std::ofstream file(SettingsPath());
    file << j.dump(2) << std::endl;
}

// The NeuralScreen config carries only the keys this integration manages;
// NeuralScreen fills the rest from its own defaults and rewrites the file.
static void WriteAppConfig() {
    const auto& s = g_settings;
    json j;
    j["fullscreen"] = false;
    j["open_menu_on_start"] = false;
    j["nr_on"] = s.enabled;
    j["frame_generation"] = EmulatorSettings.IsFrameGenerationEnabled();
    j["nr_small"] = s.boost;
    j["work_scale"] = std::clamp(s.networkResolutionPercent, 30, 100) / 100.0;
    j["nr_passes"] = s.passes;
    j["profile"] = ProfileOptions()[s.profile];
    // NeuralScreen pins the fine tuning to its own fixed value unless told
    // that the emulator owns it; "style" is its name for the model.
    j["external_tuning"] = true;
    j["style"] = std::clamp(s.model, 0, static_cast<int>(ModelOptions().size()) - 1);
    j["intensity"] = s.intensity;
    j["local_tone"] = s.localTone;
    j["local_structure"] = s.localStructure;
    j["skin_structure"] = s.skinStructure;

    std::error_code ec;
    auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    std::filesystem::create_directories(dir, ec);
    std::ofstream file(AppConfigPath());
    file << j.dump(2) << std::endl;
}

bool IsRunning() {
#ifdef _WIN32
    return g_running;
#else
    return false;
#endif
}

#ifdef _WIN32

static bool SendCommand(const std::string& word) {
    static bool winsockReady = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsockReady) {
        return false;
    }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ControlPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    timeval timeout{0, 300000};
    if (select(static_cast<int>(sock) + 1, nullptr, &set, nullptr, &timeout) <= 0) {
        closesocket(sock);
        return false;
    }
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
    std::string line = word + "\n";
    if (send(sock, line.c_str(), static_cast<int>(line.size()), 0) <= 0) {
        closesocket(sock);
        return false;
    }
    char reply[64]{};
    recv(sock, reply, sizeof(reply) - 1, 0);
    closesocket(sock);
    return std::string(reply).find("ok") != std::string::npos;
}

static std::filesystem::path FindExecutable() {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return {};
    }
    std::filesystem::path baseDir = std::filesystem::u8path(base);
    SDL_free(const_cast<char*>(base));
    const std::filesystem::path candidates[] = {
        baseDir / "NeuralScreen" / "NeuralScreen.exe",
        baseDir / "NeuralScreen.exe",
        baseDir / "neuralscreen" / "NeuralScreen.exe",
    };
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

bool StartForGameWindow(SDL_Window* window) {
    if (!GetSettings().enabled || g_running) {
        return false;
    }
    const auto exe = FindExecutable();
    if (exe.empty()) {
        LOG_WARNING(Loader, "NeuralScreen was not found next to the emulator - "
                            "DLSS5 is configured but will not run");
        return false;
    }
    WriteAppConfig();

    HWND hwnd = reinterpret_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) {
        LOG_WARNING(Loader, "NeuralScreen: the game window handle was not found");
        return false;
    }

    std::wostringstream cmd;
    cmd << L'"' << exe.wstring() << L'"' << L" --config \"" << AppConfigPath().wstring() << L'"'
        << L" --window " << reinterpret_cast<ptrdiff_t>(hwnd) << L" --ctl " << ControlPort
        << L" --instance shadPS4";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring command = cmd.str();
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        exe.parent_path().wstring().c_str(), &startup, &g_process)) {
        LOG_WARNING(Loader, "NeuralScreen could not be started ({}), DLSS5 is off", GetLastError());
        g_running = false;
        return false;
    }
    g_running = true;
    LOG_INFO(Loader, "NeuralScreen started on the game window (DLSS5 on | FG {} | Boost {})",
             EmulatorSettings.IsFrameGenerationEnabled() ? "on" : "off",
             GetSettings().boost ? "on" : "off");
    return true;
}

void Stop() {
    if (!g_running) {
        return;
    }
    SendCommand("quit");
    if (WaitForSingleObject(g_process.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(g_process.hProcess, 0);
    }
    CloseHandle(g_process.hProcess);
    CloseHandle(g_process.hThread);
    g_process = {};
    g_running = false;
    LOG_INFO(Loader, "NeuralScreen stopped");
}

void ApplySettings() {
    WriteAppConfig();
    if (g_running) {
        std::thread([] { SendCommand("reload"); }).detach();
    }
}

static bool SendHotkey(const char* word) {
    if (!g_running) {
        return false;
    }
    // Detached: a dead endpoint must not stall the frame that pressed the key.
    std::thread([word] { SendCommand(word); }).detach();
    return true;
}

bool ToggleFrameGeneration() {
    return SendHotkey("fg");
}

bool ToggleDLSS5() {
    return SendHotkey("nr");
}

bool ToggleBoost() {
    return SendHotkey("boost");
}

#else

bool StartForGameWindow(SDL_Window*) {
    return false;
}

void Stop() {}

void ApplySettings() {
    WriteAppConfig();
}

bool ToggleFrameGeneration() {
    return false;
}

bool ToggleDLSS5() {
    return false;
}

bool ToggleBoost() {
    return false;
}

#endif

} // namespace NeuralScreen
