// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// nvngx.dll_vkprobe.exe - answers one question on the user's own GPU: does the
// NeuralScreen NR runtime (nvngx_dlssnr.dll, feature 18) create its feature
// through its Vulkan entry points? It mirrors what the D3D12 worker does
// (driver NGX core init + parameter block, then the runtime's own Init_Ext /
// CreateFeature) with the NVSDK_NGX_VULKAN_* variants, and writes every step
// to nr_vk_probe.log next to the exe. The file name carries "nvngx.dll" on
// purpose: the runtime serves only callers whose module path contains it.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>

#include <vulkan/vulkan.h>

#include "nvsdk_ngx_vk.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static FILE* g_log = nullptr;

static void Log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    puts(line);
    if (g_log) {
        fputs(line, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

static const char* Name(NVSDK_NGX_Result r) {
    static char buf[160];
    const wchar_t* w = GetNGXResultAsString(r);
    snprintf(buf, sizeof(buf), "0x%08X %ls", static_cast<unsigned>(r), w ? w : L"?");
    return buf;
}

static int Finish(int code) {
    Log("RESULT: %s", code == 0 ? "NR feature was created through Vulkan" : "NR through Vulkan did not work (see above)");
    if (g_log) fclose(g_log);
    if (!GetEnvironmentVariableA("NR_PROBE_NOPAUSE", nullptr, 0)) {
        puts("Press Enter to close...");
        getchar();
    }
    return code;
}

#define VKFN(name) static PFN_##name name##_
static PFN_vkGetInstanceProcAddr gipa;
VKFN(vkCreateInstance);
VKFN(vkEnumerateInstanceVersion);

using PFN_RtInitExt2 = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, VkInstance,
                                                    VkPhysicalDevice, VkDevice,
                                                    PFN_vkGetInstanceProcAddr,
                                                    PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version,
                                                    const NVSDK_NGX_Parameter*);
using PFN_RtCreate = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, NVSDK_NGX_Feature,
                                                  const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_RtRelease = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

// __try cannot live in a function with C++ objects to unwind, so the create call
// sits in a function of its own.
static NVSDK_NGX_Result SafeCreate(PFN_RtCreate create, VkCommandBuffer cmd,
                                   NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out,
                                   unsigned long* exception) {
    *exception = 0;
    __try {
        return create(cmd, NVSDK_NGX_Feature_Reserved18, params, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exception = GetExceptionCode();
        return NVSDK_NGX_Result_Fail;
    }
}

int main() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    g_log = _wfopen((dir + L"nr_vk_probe.log").c_str(), L"w");
    Log("nr_vk_probe: module path %ls", exe);

    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) {
        Log("vulkan-1.dll did not load (error %lu)", GetLastError());
        return Finish(1);
    }
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr"));
    vkCreateInstance_ = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    vkEnumerateInstanceVersion_ = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        gipa(nullptr, "vkEnumerateInstanceVersion"));

    // What the runtime is asked about: feature 18, the same project id the
    // D3D12 worker uses.
    static NVSDK_NGX_FeatureCommonInfo common = {};
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_FeatureDiscoveryInfo di = {};
    di.SDKVersion = NVSDK_NGX_Version_API;
    di.FeatureID = NVSDK_NGX_Feature_Reserved18;
    di.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    di.Identifier.v.ProjectDesc.ProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
    di.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    di.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    di.ApplicationDataPath = dir.c_str();
    di.FeatureInfo = &common;

    // 1. instance extensions the feature wants
    uint32_t icount = 0;
    VkExtensionProperties* iprops = nullptr;
    auto r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&di, &icount, &iprops);
    Log("[1] instance extension requirements -> %s (%u)", Name(r), icount);
    std::vector<const char*> iext;
    if (!NVSDK_NGX_FAILED(r))
        for (uint32_t i = 0; i < icount; ++i) {
            Log("      %s", iprops[i].extensionName);
            iext.push_back(iprops[i].extensionName);
        }

    uint32_t apiVersion = VK_API_VERSION_1_2;
    if (vkEnumerateInstanceVersion_) {
        uint32_t v = 0;
        vkEnumerateInstanceVersion_(&v);
        if (v >= VK_API_VERSION_1_3) apiVersion = VK_API_VERSION_1_3;
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nr_vk_probe";
    app.apiVersion = apiVersion;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(iext.size());
    ici.ppEnabledExtensionNames = iext.data();
    VkInstance inst = nullptr;
    VkResult vr = vkCreateInstance_(&ici, nullptr, &inst);
    Log("[2] vkCreateInstance (api 1.%u) -> %d", VK_VERSION_MINOR(apiVersion), vr);
    if (vr != VK_SUCCESS) return Finish(1);

#define IFN(n) auto n = reinterpret_cast<PFN_##n>(gipa(inst, #n))
    IFN(vkEnumeratePhysicalDevices);
    IFN(vkGetPhysicalDeviceProperties);
    IFN(vkGetPhysicalDeviceFeatures2);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkEnumerateDeviceExtensionProperties);
    IFN(vkCreateDevice);
    IFN(vkGetDeviceProcAddr);

    uint32_t pdc = 0;
    vkEnumeratePhysicalDevices(inst, &pdc, nullptr);
    std::vector<VkPhysicalDevice> pds(pdc);
    vkEnumeratePhysicalDevices(inst, &pdc, pds.data());
    VkPhysicalDevice pd = nullptr;
    for (auto d : pds) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        Log("      GPU: %s (vendor 0x%04X, api %u.%u)", p.deviceName, p.vendorID,
            VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion));
        if (p.vendorID == 0x10DE && !pd) pd = d;
    }
    if (!pd) {
        Log("[3] no NVIDIA GPU found - the NR runtime needs one");
        return Finish(1);
    }

    // 3. does the runtime accept this adapter
    NVSDK_NGX_FeatureRequirement req = {};
    r = NVSDK_NGX_VULKAN_GetFeatureRequirements(inst, pd, &di, &req);
    Log("[3] feature requirements -> %s, supported mask %u, min arch 0x%X", Name(r),
        static_cast<unsigned>(req.FeatureSupported), req.MinHWArchitecture);

    // 4. device extensions, then a device with every supported feature on
    uint32_t dcount = 0;
    VkExtensionProperties* dprops = nullptr;
    r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(inst, pd, &di, &dcount, &dprops);
    Log("[4] device extension requirements -> %s (%u)", Name(r), dcount);
    std::vector<const char*> dext;
    if (!NVSDK_NGX_FAILED(r))
        for (uint32_t i = 0; i < dcount; ++i) {
            Log("      %s", dprops[i].extensionName);
            dext.push_back(dprops[i].extensionName);
        }

    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(pd, &pp);
    f11.pNext = &f12;
    if (pp.apiVersion >= VK_API_VERSION_1_3) f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 feats = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feats.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(pd, &feats);

    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qc);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qf.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < qc; ++i)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            family = i;
            break;
        }
    if (family == UINT32_MAX) return Finish(1);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &feats;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dext.size());
    dci.ppEnabledExtensionNames = dext.data();
    VkDevice dev = nullptr;
    vr = vkCreateDevice(pd, &dci, nullptr, &dev);
    Log("[5] vkCreateDevice -> %d", vr);
    if (vr != VK_SUCCESS) return Finish(1);
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkGetDeviceProcAddr);
#define DFN(n) auto n = reinterpret_cast<PFN_##n>(gdpa(dev, #n))
    DFN(vkGetDeviceQueue);
    DFN(vkCreateCommandPool);
    DFN(vkAllocateCommandBuffers);
    DFN(vkBeginCommandBuffer);
    DFN(vkEndCommandBuffer);
    DFN(vkQueueSubmit);
    DFN(vkQueueWaitIdle);
    DFN(vkDeviceWaitIdle);

    // 6. driver NGX core, then the parameter block (what the D3D12 worker does)
    r = NVSDK_NGX_VULKAN_Init(0x1000000ULL, dir.c_str(), inst, pd, dev, gipa, gdpa, &common,
                              NVSDK_NGX_Version_API);
    Log("[6] NVSDK_NGX_VULKAN_Init (driver core) -> %s", Name(r));
    NVSDK_NGX_Parameter* params = nullptr;
    if (!NVSDK_NGX_FAILED(r)) {
        r = NVSDK_NGX_VULKAN_AllocateParameters(&params);
        Log("[7] AllocateParameters -> %s", Name(r));
    }
    if (!params) return Finish(1);

    // 7. the runtime itself, called from this module (its path carries nvngx.dll)
    HMODULE rt = LoadLibraryW((dir + L"nvngx_dlssnr.dll").c_str());
    if (!rt) {
        Log("[8] nvngx_dlssnr.dll did not load from %ls (error %lu) - put the probe in NeuralScreen\\native", dir.c_str(), GetLastError());
        return Finish(1);
    }
    auto initExt2 = reinterpret_cast<PFN_RtInitExt2>(GetProcAddress(rt, "NVSDK_NGX_VULKAN_Init_Ext2"));
    auto create = reinterpret_cast<PFN_RtCreate>(GetProcAddress(rt, "NVSDK_NGX_VULKAN_CreateFeature"));
    auto release = reinterpret_cast<PFN_RtRelease>(GetProcAddress(rt, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    Log("[8] runtime exports: Init_Ext2 %s, CreateFeature %s, ReleaseFeature %s",
        initExt2 ? "ok" : "MISSING", create ? "ok" : "MISSING", release ? "ok" : "MISSING");
    if (!initExt2 || !create || !release) return Finish(1);

    r = initExt2(0x1000000ULL, dir.c_str(), inst, pd, dev, gipa, gdpa, NVSDK_NGX_Version_API, params);
    Log("[9] runtime Init_Ext2 -> %s", Name(r));
    if (NVSDK_NGX_FAILED(r)) return Finish(1);

    params->Set("CreationNodeMask", 1u);
    params->Set("VisibilityNodeMask", 1u);
    params->Set("DLSSNR.Width", 1280u);
    params->Set("DLSSNR.Height", 720u);
    params->Set("DLSSNR.ScalingRatio", 1.0f);
    params->Set("DLSSNR.Hint.Render.Preset", 0u);
    params->Set("DLSS.Feature.Create.Flags", 0u);

    VkQueue queue = nullptr;
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    VkCommandPool pool = nullptr;
    vkCreateCommandPool(dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = nullptr;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    NVSDK_NGX_Handle* feature = nullptr;
    unsigned long exc = 0;
    NVSDK_NGX_Result cr = SafeCreate(create, cmd, params, &feature, &exc);
    if (exc != 0) {
        Log("[10] CreateFeature raised exception 0x%08lX", exc);
        return Finish(1);
    }
    Log("[10] CreateFeature(18) -> %s", Name(cr));
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vr = vkQueueSubmit(queue, 1, &si, nullptr);
    vkQueueWaitIdle(queue);
    Log("[11] submit + wait -> %d", vr);
    if (feature) release(feature);
    vkDeviceWaitIdle(dev);
    return Finish(NVSDK_NGX_FAILED(cr) || vr != VK_SUCCESS ? 1 : 0);
}
