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
#include <atomic>
#include <chrono>
#include <cstdlib>
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

/// A job that ends NeuralScreen when the emulator exits in any way, even when it crashes or is
/// killed; a NeuralScreen left behind falls back to capturing the whole screen.
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
#endif
static bool g_frameGenerationOn = false;
static bool g_dlss5On = false;
static bool g_boostOn = false;

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
        s.autoQuality = j.value("AutoQuality", s.autoQuality);
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
    j["AutoQuality"] = s.autoQuality;
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
    j["auto_quality"] = s.autoQuality;
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
    g_frameGenerationOn = EmulatorSettings.IsFrameGenerationEnabled();
    g_dlss5On = s.enabled;
    g_boostOn = s.boost;

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

static std::string SendForReply(const std::string& word) {
    static bool winsockReady = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsockReady) {
        return {};
    }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        return {};
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
        return {};
    }
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
    std::string line = word + "\n";
    if (send(sock, line.c_str(), static_cast<int>(line.size()), 0) <= 0) {
        closesocket(sock);
        return {};
    }
    char reply[64]{};
    recv(sock, reply, sizeof(reply) - 1, 0);
    closesocket(sock);
    return reply;
}

static bool SendCommand(const std::string& word) {
    return SendForReply(word).find("ok") != std::string::npos;
}

// The frame rate NeuralScreen really puts on the screen. The emulator's own
// counter counts the frames the game presents and cannot see the work done in
// the NeuralScreen process afterwards, so while NeuralScreen runs it is asked.
// Negative: nothing to report (NR off with no Frame Generation, or no answer).
static std::atomic<float> g_outputFps{-1.0f};
static std::atomic<bool> g_pollStop{false};
static std::thread g_poller;

static void PollOutputFps() {
    while (!g_pollStop) {
        float value = -1.0f;
        const std::string reply = SendForReply("fps");
        if (reply.rfind("ok ", 0) == 0 && reply.compare(3, 4, "none") != 0) {
            value = static_cast<float>(std::atof(reply.c_str() + 3));
        }
        g_outputFps = value;
        for (int i = 0; i < 5 && !g_pollStop; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    g_outputFps = -1.0f;
}

static std::filesystem::path FindDirectory() {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return {};
    }
    std::filesystem::path baseDir = std::filesystem::u8path(base);
    SDL_free(const_cast<char*>(base));
    const auto dir = baseDir / "qtplugins" / "NeuralScreen";
    std::error_code ec;
    if (std::filesystem::is_regular_file(dir / "main.py", ec)) {
        return dir;
    }
    return {};
}

static std::filesystem::path FindPython(const std::filesystem::path& dir) {
    wchar_t overridePath[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"NEURALSCREEN_PYTHON", overridePath, MAX_PATH);
    std::error_code ec;
    if (length > 0 && length < MAX_PATH && std::filesystem::is_regular_file(overridePath, ec)) {
        return overridePath;
    }
    const auto bundled = dir / "runtime" / "pythonw.exe";
    if (std::filesystem::is_regular_file(bundled, ec)) {
        return bundled;
    }
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"pythonw.exe", nullptr, MAX_PATH, found, nullptr)) {
        return found;
    }
    return {};
}

bool StartForGameWindow(SDL_Window* window) {
    if (g_running) {
        return false;
    }
    // NeuralScreen also carries Frame Generation, so it starts with DLSS5 off
    // too: NR then comes up off (nr_on = false in the generated config) and the
    // frame generation setting and hotkey still work. A missing install is only
    // reported when something that needs it is switched on.
    const bool wanted = GetSettings().enabled || EmulatorSettings.IsFrameGenerationEnabled();
    const auto dir = FindDirectory();
    if (dir.empty()) {
        if (wanted) {
            LOG_WARNING(Loader, "NeuralScreen was not found next to the emulator - "
                                "DLSS5 / frame generation is configured but will not run");
        }
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dir / "native" / "nvngx.dll", ec) ||
        !std::filesystem::is_regular_file(dir / "native" / "nvngx_dlssnr.dll", ec)) {
        if (wanted) {
            LOG_WARNING(Loader, "NeuralScreen: native\\nvngx.dll or native\\nvngx_dlssnr.dll is "
                                "missing, DLSS5 / frame generation is off");
        }
        return false;
    }
    const auto python = FindPython(dir);
    if (python.empty()) {
        if (wanted) {
            LOG_WARNING(Loader, "NeuralScreen: pythonw.exe was not found, DLSS5 / frame "
                                "generation is off");
        }
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
    cmd << L'"' << python.wstring() << L'"' << L" -u \"" << (dir / "main.py").wstring() << L'"'
        << L" --config \"" << AppConfigPath().wstring() << L'"'
        << L" --window " << reinterpret_cast<ptrdiff_t>(hwnd) << L" --ctl " << ControlPort
        << L" --instance shadPS4";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    std::wstring command = cmd.str();
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir.wstring().c_str(),
                        &startup, &g_process)) {
        LOG_WARNING(Loader, "NeuralScreen could not be started ({}), DLSS5 is off", GetLastError());
        g_running = false;
        return false;
    }
    if (const HANDLE job = LifetimeJob();
        !job || !AssignProcessToJobObject(job, g_process.hProcess)) {
        LOG_WARNING(Loader, "NeuralScreen: could not tie it to the emulator's lifetime ({})",
                    GetLastError());
    }
    ResumeThread(g_process.hThread);
    g_running = true;
    g_pollStop = false;
    g_poller = std::thread(PollOutputFps);
    LOG_INFO(Loader, "NeuralScreen started on the game window (DLSS5 {} | FG {} | Boost {})",
             GetSettings().enabled ? "on" : "off",
             EmulatorSettings.IsFrameGenerationEnabled() ? "on" : "off",
             GetSettings().boost ? "on" : "off");
    return true;
}

void Stop() {
    if (!g_running) {
        return;
    }
    g_pollStop = true;
    if (g_poller.joinable()) {
        g_poller.join();
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

// The emulator owns the target state and sends it as an absolute word
// (nr_on / nr_off ...), so what the notification shows is what NeuralScreen
// was told to be. The state only changes once NeuralScreen has answered ok.
static std::optional<bool> SendHotkey(const char* word, bool& state) {
    if (!g_running) {
        return std::nullopt;
    }
    const bool target = !state;
    if (!SendCommand(std::string(word) + (target ? "_on" : "_off"))) {
        return std::nullopt;
    }
    state = target;
    return state;
}

std::optional<float> GetOutputFps() {
    const float value = g_outputFps;
    if (!g_running || value < 0.0f) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> ToggleFrameGeneration() {
    return SendHotkey("fg", g_frameGenerationOn);
}

std::optional<bool> ToggleDLSS5() {
    return SendHotkey("nr", g_dlss5On);
}

std::optional<bool> ToggleBoost() {
    return SendHotkey("boost", g_boostOn);
}

#else

bool StartForGameWindow(SDL_Window*) {
    return false;
}

void Stop() {}

void ApplySettings() {
    WriteAppConfig();
}

std::optional<float> GetOutputFps() {
    return std::nullopt;
}

std::optional<bool> ToggleFrameGeneration() {
    return std::nullopt;
}

std::optional<bool> ToggleDLSS5() {
    return std::nullopt;
}

std::optional<bool> ToggleBoost() {
    return std::nullopt;
}

#endif

} // namespace NeuralScreen
