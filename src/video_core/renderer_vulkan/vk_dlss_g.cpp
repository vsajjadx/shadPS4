// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <string>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_dlss_g.h"

#if defined(ENABLE_DLSS_G) && defined(_WIN32)
#include <windows.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_helpers_vk.h>
#endif

namespace Vulkan::DlssG {

namespace {
std::atomic_bool enabled{false};
std::atomic_bool available{false};

bool Selected() {
    return EmulatorSettings.IsFrameGenerationEnabled() &&
           EmulatorSettings.GetFrameGenerationBackend() == "DLSS-G";
}
} // namespace

#if defined(ENABLE_DLSS_G) && defined(_WIN32)

namespace {
using InitFn = sl::Result (*)(const sl::Preferences&, uint64_t);
using ShutdownFn = sl::Result (*)();
using IsSupportedFn = sl::Result (*)(sl::Feature, const sl::AdapterInfo&);
using SetVulkanInfoFn = sl::Result (*)(const sl::VulkanInfo&);

HMODULE interposer{};
ShutdownFn sl_shutdown{};
IsSupportedFn sl_is_supported{};
SetVulkanInfoFn sl_set_vulkan_info{};
bool sl_initialized{};

void LogCallback(sl::LogType type, const char* message) {
    LOG_INFO(Render_Vulkan, "Streamline: {}", message);
}
} // namespace

PFN_vkGetInstanceProcAddr Initialize() {
    if (!Selected()) {
        return nullptr;
    }
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring dir{exe_path};
    dir.resize(dir.find_last_of(L'\\'));
    const std::wstring dll = dir + L"\\sl.interposer.dll";

    interposer = LoadLibraryW(dll.c_str());
    if (!interposer) {
        LOG_ERROR(Render_Vulkan, "DLSS-G: sl.interposer.dll was not found next to the emulator");
        return nullptr;
    }
    const auto init = reinterpret_cast<InitFn>(GetProcAddress(interposer, "slInit"));
    sl_shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(interposer, "slShutdown"));
    sl_is_supported =
        reinterpret_cast<IsSupportedFn>(GetProcAddress(interposer, "slIsFeatureSupported"));
    sl_set_vulkan_info =
        reinterpret_cast<SetVulkanInfoFn>(GetProcAddress(interposer, "slSetVulkanInfo"));
    const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(interposer, "vkGetInstanceProcAddr"));
    if (!init || !sl_shutdown || !sl_is_supported || !sl_set_vulkan_info || !get_proc) {
        LOG_ERROR(Render_Vulkan, "DLSS-G: sl.interposer.dll is missing required exports");
        FreeLibrary(interposer);
        interposer = nullptr;
        return nullptr;
    }

    const wchar_t* plugin_paths[] = {dir.c_str()};
    sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeaturePCL};
    sl::Preferences prefs{};
    prefs.showConsole = false;
    prefs.logLevel = sl::LogLevel::eDefault;
    prefs.pathsToPlugins = plugin_paths;
    prefs.numPathsToPlugins = 1;
    prefs.logMessageCallback = LogCallback;
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = 2;
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = "shadPS4";
    prefs.projectId = "6d6f6eb8-3b0a-4f6f-8f35-6a7c1f2f4d11";
    prefs.renderAPI = sl::RenderAPI::eVulkan;
    const auto result = init(prefs, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "DLSS-G: slInit failed ({})", static_cast<int>(result));
        FreeLibrary(interposer);
        interposer = nullptr;
        return nullptr;
    }
    sl_initialized = true;
    return get_proc;
}

void OnDeviceCreated(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device,
                     u32 queue_family_index) {
    if (!sl_initialized) {
        return;
    }
    sl::VulkanInfo info{};
    info.instance = instance;
    info.physicalDevice = physical_device;
    info.device = device;
    info.graphicsQueueFamily = queue_family_index;
    info.graphicsQueueIndex = 0;
    const auto info_result = sl_set_vulkan_info(info);
    if (info_result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "DLSS-G: slSetVulkanInfo failed ({})",
                  static_cast<int>(info_result));
        return;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical_device;
    const auto support = sl_is_supported(sl::kFeatureDLSS_G, adapter);
    LOG_INFO(Render_Vulkan, "DLSS-G: slIsFeatureSupported returned {}", static_cast<int>(support));
    available = support == sl::Result::eOk;
    enabled = available && Selected();
}

void Shutdown() {
    if (sl_initialized && sl_shutdown) {
        sl_shutdown();
        sl_initialized = false;
    }
    available = false;
}

#else

PFN_vkGetInstanceProcAddr Initialize() {
    if (Selected()) {
        LOG_ERROR(Render_Vulkan, "DLSS-G is not part of this build");
    }
    return nullptr;
}

void OnDeviceCreated(vk::Instance, vk::PhysicalDevice, vk::Device, u32) {}

void Shutdown() {}

#endif

bool IsAvailable() {
    return available;
}

void SetEnabled(bool value) {
    enabled = value && available;
}

bool IsEnabled() {
    return enabled;
}

} // namespace Vulkan::DlssG
