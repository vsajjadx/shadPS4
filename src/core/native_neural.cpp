// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/native_neural.h"

#if defined(_WIN32) && defined(ENABLE_NATIVE_NEURAL)

#include <cstring>
#include <filesystem>

#include <SDL3/SDL.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "common/logging/log.h"
#include "nvsdk_ngx.h"

using Microsoft::WRL::ComPtr;

namespace NativeNeural {

namespace {

// The neural runtime serves a call only when the calling module's path contains "nvngx.dll", so
// the calls go through NeuralScreen's forwarder module (see NeuralScreen native/ns_forwarder.cpp).
using PFN_FwdLoad = int (*)(const wchar_t*);
using PFN_FwdInitExt = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*,
                                                      ID3D12Device*, NVSDK_NGX_Version,
                                                      const NVSDK_NGX_Parameter*);

constexpr unsigned long long kAppId = 0x1000000ULL;

ComPtr<ID3D12Device> g_device;
NVSDK_NGX_Parameter* g_params = nullptr;
bool g_ngx_inited = false;

std::filesystem::path FindRuntimeDir() {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return {};
    }
    const std::filesystem::path baseDir = std::filesystem::u8path(base);
    const auto root = baseDir / "qtplugins" / "NeuralScreen";
    std::error_code ec;
    for (const auto& dir : {root / "native", root}) {
        if (std::filesystem::is_regular_file(dir / "nvngx.dll_ns-forwarder.dll", ec) &&
            std::filesystem::is_regular_file(dir / "nvngx_dlssnr.dll", ec)) {
            return dir;
        }
    }
    return {};
}

} // namespace

bool Probe(const std::array<uint8_t, 8>& adapter_luid) {
    if (g_device) {
        return g_ngx_inited;
    }
    const auto dir = FindRuntimeDir();
    if (dir.empty()) {
        LOG_WARNING(Loader, "NativeNeural: forwarder / nvngx_dlssnr.dll not found next to NeuralScreen");
        return false;
    }

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        LOG_WARNING(Loader, "NativeNeural: CreateDXGIFactory1 failed");
        return false;
    }
    LUID luid{};
    std::memcpy(&luid, adapter_luid.data(), sizeof(luid));
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
        LOG_WARNING(Loader, "NativeNeural: no DXGI adapter matches the Vulkan device");
        return false;
    }
    if (const HRESULT hr =
            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device));
        FAILED(hr)) {
        LOG_WARNING(Loader, "NativeNeural: D3D12CreateDevice failed 0x{:08X}",
                    static_cast<unsigned>(hr));
        return false;
    }

    const std::wstring data_path = dir.wstring();
    auto r = NVSDK_NGX_D3D12_Init(kAppId, data_path.c_str(), g_device.Get(), nullptr,
                                  NVSDK_NGX_Version_API);
    LOG_INFO(Loader, "NativeNeural: NVSDK_NGX_D3D12_Init -> 0x{:08X}", static_cast<unsigned>(r));
    if (NVSDK_NGX_FAILED(r)) {
        return false;
    }
    g_ngx_inited = true;
    r = NVSDK_NGX_D3D12_AllocateParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || !g_params) {
        LOG_WARNING(Loader, "NativeNeural: AllocateParameters failed 0x{:08X}",
                    static_cast<unsigned>(r));
        return false;
    }

    const HMODULE fwd = LoadLibraryW((dir / "nvngx.dll_ns-forwarder.dll").c_str());
    if (!fwd) {
        LOG_WARNING(Loader, "NativeNeural: forwarder did not load, error {}", GetLastError());
        return false;
    }
    const auto load = reinterpret_cast<PFN_FwdLoad>(GetProcAddress(fwd, "NsFwdLoad"));
    const auto init_ext = reinterpret_cast<PFN_FwdInitExt>(GetProcAddress(fwd, "NsFwdInitExt"));
    if (!load || !init_ext) {
        LOG_WARNING(Loader, "NativeNeural: forwarder is missing exports");
        return false;
    }
    if (const int lr = load((dir / "nvngx_dlssnr.dll").c_str()); lr != 0) {
        LOG_WARNING(Loader, "NativeNeural: forwarder could not load nvngx_dlssnr.dll ({})", lr);
        return false;
    }
    r = init_ext(kAppId, data_path.c_str(), g_device.Get(), NVSDK_NGX_Version_API, g_params);
    LOG_INFO(Loader, "NativeNeural: DLSSNR Init_Ext through the forwarder -> 0x{:08X}",
             static_cast<unsigned>(r));
    return NVSDK_NGX_SUCCEED(r);
}

void Shutdown() {
    if (g_params) {
        NVSDK_NGX_D3D12_DestroyParameters(g_params);
        g_params = nullptr;
    }
    if (g_ngx_inited && g_device) {
        NVSDK_NGX_D3D12_Shutdown1(g_device.Get());
    }
    g_ngx_inited = false;
    g_device.Reset();
}

} // namespace NativeNeural

#else

namespace NativeNeural {
bool Probe(const std::array<uint8_t, 8>&) {
    return false;
}
void Shutdown() {}
} // namespace NativeNeural

#endif
