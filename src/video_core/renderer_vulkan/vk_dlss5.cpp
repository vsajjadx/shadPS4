// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_dlss5.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <span>
#include <string>
#include <vector>
#include <windows.h>

#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/dlss5.h"
#include "video_core/host_shaders/dlss5_clamp_comp.h"
#include "video_core/host_shaders/dlss5_depth_comp.h"
#include "video_core/renderer_vulkan/host_passes/lemon_fg_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <boost/container/static_vector.hpp>
#include <vk_mem_alloc.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_vk.h>

namespace Vulkan {

namespace {

constexpr NVSDK_NGX_Feature NeuralRendering = NVSDK_NGX_Feature_Reserved18;
constexpr const char* ProjectId = "5ad95500-5f9b-4b6e-9c11-2b6c4a8d7e31";

using PFN_Init = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, VkInstance,
                                               VkPhysicalDevice, VkDevice,
                                               PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                               NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using PFN_CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice, VkCommandBuffer,
                                                        NVSDK_NGX_Feature,
                                                        const NVSDK_NGX_Parameter*,
                                                        NVSDK_NGX_Handle**);
using PFN_EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, const NVSDK_NGX_Handle*,
                                                          const NVSDK_NGX_Parameter*,
                                                          PFN_NVSDK_NGX_ProgressCallback);
using PFN_ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using PFN_Shutdown = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice);
using PFN_GetModuleFileNameW = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

PFN_GetModuleFileNameW g_system_get_module_file_name{};
HMODULE g_emulator_module{};
Dlss5Pass* g_pass{};

void NVSDK_CONV LogNgx(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    LOG_DEBUG(Render_Vulkan, "NGX: {}", message);
}

/// The library only runs when the module calling it is NVIDIA's core, nvngx.dll.
DWORD WINAPI LibraryGetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size) {
    constexpr wchar_t Name[] = L"nvngx.dll";
    constexpr DWORD Length = static_cast<DWORD>(std::size(Name) - 1);
    if (!module || module != g_emulator_module || !buffer || !size) {
        return g_system_get_module_file_name(module, buffer, size);
    }
    if (size <= Length) {
        buffer[0] = 0;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    std::memcpy(buffer, Name, sizeof(Name));
    return Length;
}

/// Points the library's GetModuleFileNameW import at LibraryGetModuleFileNameW; the file on disk
/// is not changed.
bool RedirectModuleName(HMODULE library) {
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LibraryGetModuleFileNameW), &g_emulator_module)) {
        return false;
    }
    auto* base = reinterpret_cast<u8*>(library);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return false;
    }
    bool patched = false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         desc->Name; ++desc) {
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            const auto* import =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), "GetModuleFileNameW")) {
                continue;
            }
            DWORD protect{};
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE,
                           &protect);
            if (!g_system_get_module_file_name) {
                g_system_get_module_file_name =
                    reinterpret_cast<PFN_GetModuleFileNameW>(slots->u1.Function);
            }
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(&LibraryGetModuleFileNameW);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), protect, &protect);
            patched = true;
        }
    }
    return patched;
}

std::vector<std::filesystem::path> LibraryCandidates() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto plugins = std::filesystem::path{exe}.parent_path() / "qtplugins";
    return {
        plugins / "DLSS5" / "nvngx_dlssnr.dll",
    };
}

struct WorkImage {
    vk::Image image{};
    vk::ImageView view{};
    VmaAllocation allocation{};
    vk::Format format{};
};

} // namespace

struct Dlss5Pass::Impl {
    const Instance& instance;
    vk::Device device;
    bool core_ready{};
    bool available{};
    HMODULE library{};
    PFN_CreateFeature create{};
    PFN_EvaluateFeature evaluate{};
    PFN_ReleaseFeature release{};
    PFN_Shutdown shutdown{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    u32 width{};
    u32 height{};
    bool reset{true};
    bool invalidated{};
    u32 generation{};
    std::array<WorkImage, 2> color{};
    WorkImage depth{};
    WorkImage motion{};
    WorkImage srgb{};
    vk::Format frame_format{};
    HostPasses::LemonFgPass motion_search;
    bool motion_supported{};
    bool depth_reversed{};
    struct ComputePass {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    ComputePass depth_pass;
    ComputePass clamp_pass;

    /// A compute pass reading one sampled image and writing one storage image.
    ComputePass CreatePass(std::span<const u32> code) const {
        const std::array bindings{
            vk::DescriptorSetLayoutBinding{
                .binding = 0,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute,
            },
            vk::DescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute,
            },
        };
        ComputePass pass;
        pass.set_layout =
            Check<"create DLSS5 pass set layout">(device.createDescriptorSetLayoutUnique({
                .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                .bindingCount = static_cast<u32>(bindings.size()),
                .pBindings = bindings.data(),
            }));
        const vk::PushConstantRange push{
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .offset = 0,
            .size = sizeof(u32) * 4,
        };
        pass.layout = Check<"create DLSS5 pass layout">(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &pass.set_layout.get(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
        const auto module = CompileSPV(code, device);
        if (!module) {
            return {};
        }
        pass.pipeline = Check<"create DLSS5 pass pipeline">(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage{
                        .stage = vk::ShaderStageFlagBits::eCompute,
                        .module = module,
                        .pName = "main",
                    },
                    .layout = pass.layout.get(),
                }));
        device.destroyShaderModule(module);
        return pass;
    }

    void Dispatch(vk::CommandBuffer cmdbuf, const ComputePass& pass, vk::ImageView source,
                  vk::ImageView destination, const std::array<u32, 4>& push) const {
        const vk::DescriptorImageInfo source_info{
            .imageView = source,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
        const vk::DescriptorImageInfo destination_info{
            .imageView = destination,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
        const std::array writes{
            vk::WriteDescriptorSet{
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &source_info,
            },
            vk::WriteDescriptorSet{
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .pImageInfo = &destination_info,
            },
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pass.pipeline.get());
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pass.layout.get(), 0, writes);
        cmdbuf.pushConstants(pass.layout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             push.data());
        cmdbuf.dispatch(Common::DivCeil(width, 8u), Common::DivCeil(height, 8u), 1);
    }

    Impl(const Instance& instance_, Scheduler& scheduler)
        : instance{instance_}, device{instance_.GetDevice()} {
        if (HostPasses::LemonFgPass::IsSupported(instance)) {
            motion_search.Create(instance, scheduler, {});
            motion_supported = true;
        }
        if (!instance.IsNeuralRenderingSupported()) {
            LOG_INFO(Render_Vulkan, "DLSS5: the device lacks the NVIDIA interop extensions");
            return;
        }
        const auto data_path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "dlss5";
        std::error_code ec;
        std::filesystem::create_directories(data_path, ec);
        NVSDK_NGX_FeatureCommonInfo common{};
        common.LoggingInfo.LoggingCallback = LogNgx;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        const auto core_result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data_path.wstring().c_str(),
            instance.GetInstance(), instance.GetPhysicalDevice(), device,
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr, &common, NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(core_result)) {
            LOG_WARNING(Render_Vulkan, "DLSS5: NVIDIA's NGX runtime did not start ({:#x})",
                        static_cast<u32>(core_result));
            return;
        }
        core_ready = true;
        for (const auto& path : LibraryCandidates()) {
            if (std::filesystem::exists(path, ec) && LoadNeuralLibrary(path, data_path)) {
                available = true;
                return;
            }
        }
        LOG_WARNING(Render_Vulkan, "DLSS5: no usable nvngx_dlssnr.dll was found in qtplugins");
    }

    bool LoadNeuralLibrary(const std::filesystem::path& path,
                           const std::filesystem::path& data_path) {
        const UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
        const HMODULE module =
            LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        SetErrorMode(old_mode);
        if (!module) {
            LOG_WARNING(Render_Vulkan, "DLSS5: {} did not load ({})", path.string(),
                        GetLastError());
            return false;
        }
        const auto init =
            reinterpret_cast<PFN_Init>(GetProcAddress(module, "NVSDK_NGX_VULKAN_Init_Ext2"));
        create = reinterpret_cast<PFN_CreateFeature>(
            GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature1"));
        evaluate = reinterpret_cast<PFN_EvaluateFeature>(
            GetProcAddress(module, "NVSDK_NGX_VULKAN_EvaluateFeature"));
        release = reinterpret_cast<PFN_ReleaseFeature>(
            GetProcAddress(module, "NVSDK_NGX_VULKAN_ReleaseFeature"));
        shutdown =
            reinterpret_cast<PFN_Shutdown>(GetProcAddress(module, "NVSDK_NGX_VULKAN_Shutdown1"));
        if (!init || !create || !evaluate || !release || !shutdown || !RedirectModuleName(module)) {
            LOG_WARNING(Render_Vulkan, "DLSS5: {} is not a neural rendering library",
                        path.string());
            return false;
        }
        const auto result = init(
            0, data_path.wstring().c_str(), instance.GetInstance(), instance.GetPhysicalDevice(),
            device, VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr, NVSDK_NGX_Version_API, nullptr);
        if (NVSDK_NGX_FAILED(result)) {
            LOG_WARNING(Render_Vulkan, "DLSS5: {} did not start ({:#x})", path.string(),
                        static_cast<u32>(result));
            return false;
        }
        if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters))) {
            shutdown(device);
            return false;
        }
        library = module;
        depth_pass = CreatePass(DLSS5_DEPTH_COMP);
        clamp_pass = CreatePass(DLSS5_CLAMP_COMP);
        LOG_INFO(Render_Vulkan, "DLSS5: neural rendering is available ({})", path.string());
        return true;
    }

    ~Impl() {
        (void)device.waitIdle();
        if (feature) {
            release(feature);
        }
        DestroyImages();
        if (parameters) {
            NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        }
        if (available) {
            shutdown(device);
        }
        if (core_ready) {
            NVSDK_NGX_VULKAN_Shutdown1(device);
        }
    }

    WorkImage CreateImage(vk::Format format, vk::ImageUsageFlags usage) {
        WorkImage work{.format = format};
        const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = usage,
        });
        const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage image{};
        if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                           &work.allocation, nullptr) != VK_SUCCESS) {
            return {};
        }
        work.image = image;
        work.view = Check<"create DLSS5 image view">(device.createImageView({
            .image = work.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = Range(),
        }));
        return work;
    }

    void DestroyImages() {
        for (auto* work : {&color[0], &color[1], &depth, &motion, &srgb}) {
            if (work->view) {
                device.destroyImageView(work->view);
            }
            if (work->image) {
                vmaDestroyImage(instance.GetAllocator(), work->image, work->allocation);
            }
            *work = {};
        }
    }

    /// The frame holds gamma-encoded colors in a UNORM format. Copying them into an image of
    /// the matching sRGB format lets the blits to and from the float work images decode and
    /// encode them, so DLSS5 sees linear light.
    static vk::Format SrgbFormat(vk::Format format) {
        switch (format) {
        case vk::Format::eB8G8R8A8Unorm:
            return vk::Format::eB8G8R8A8Srgb;
        case vk::Format::eR8G8B8A8Unorm:
            return vk::Format::eR8G8B8A8Srgb;
        case vk::Format::eA8B8G8R8UnormPack32:
            return vk::Format::eA8B8G8R8SrgbPack32;
        default:
            return vk::Format::eUndefined;
        }
    }

    void Copy(vk::CommandBuffer cmdbuf, vk::Image src, vk::Image dst) const {
        const vk::ImageSubresourceLayers layers = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const vk::ImageCopy region = {
            .srcSubresource = layers,
            .dstSubresource = layers,
            .extent = {width, height, 1},
        };
        cmdbuf.copyImage(src, vk::ImageLayout::eGeneral, dst, vk::ImageLayout::eGeneral, region);
    }

    static vk::ImageSubresourceRange Range() {
        return {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
    }

    static NVSDK_NGX_Resource_VK Resource(const WorkImage& work, u32 w, u32 h, bool written) {
        const VkImageSubresourceRange range = static_cast<VkImageSubresourceRange>(Range());
        return NVSDK_NGX_Create_ImageView_Resource_VK(
            work.view, work.image, range, static_cast<VkFormat>(work.format), w, h, written);
    }

    static void FullBarrier(vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    }

    static void Blit(vk::CommandBuffer cmdbuf, vk::Image src, u32 src_width, u32 src_height,
                     vk::Image dst, u32 dst_width, u32 dst_height) {
        const vk::ImageSubresourceLayers layers = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const vk::ImageBlit region = {
            .srcSubresource = layers,
            .srcOffsets =
                std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(src_width),
                                                               static_cast<s32>(src_height), 1}},
            .dstSubresource = layers,
            .dstOffsets =
                std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(dst_width),
                                                               static_cast<s32>(dst_height), 1}},
        };
        const bool scaled = src_width != dst_width || src_height != dst_height;
        cmdbuf.blitImage(src, vk::ImageLayout::eGeneral, dst, vk::ImageLayout::eGeneral, region,
                         scaled ? vk::Filter::eLinear : vk::Filter::eNearest);
    }

    /// Makes the work images and the feature for this size; the feature evaluates from the next
    /// frame on.
    void Recreate(vk::CommandBuffer cmdbuf, u32 new_width, u32 new_height) {
        (void)device.waitIdle();
        if (feature) {
            release(feature);
            feature = nullptr;
        }
        DestroyImages();
        width = new_width;
        height = new_height;
        constexpr auto usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eTransferSrc |
                               vk::ImageUsageFlagBits::eTransferDst;
        color[0] = CreateImage(vk::Format::eR16G16B16A16Sfloat, usage);
        color[1] = CreateImage(vk::Format::eR16G16B16A16Sfloat, usage);
        depth = CreateImage(vk::Format::eR32Sfloat, usage);
        motion = CreateImage(vk::Format::eR16G16Sfloat, usage);
        if (const auto srgb_format = SrgbFormat(frame_format);
            srgb_format != vk::Format::eUndefined) {
            srgb = CreateImage(srgb_format, vk::ImageUsageFlagBits::eTransferSrc |
                                                vk::ImageUsageFlagBits::eTransferDst);
        }
        if (!color[0].image || !color[1].image || !depth.image || !motion.image) {
            LOG_WARNING(Render_Vulkan, "DLSS5: could not allocate {}x{} work images", width,
                        height);
            DestroyImages();
            width = height = 0;
            return;
        }

        boost::container::static_vector<vk::ImageMemoryBarrier2, 5> barriers;
        for (const vk::Image image :
             {color[0].image, color[1].image, depth.image, motion.image, srgb.image}) {
            if (!image) {
                continue;
            }
            barriers.push_back({
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask =
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = image,
                .subresourceRange = Range(),
            });
        }
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                                 .pImageMemoryBarriers = barriers.data()});
        // The picture has no depth or motion of its own: a flat depth and no motion.
        const vk::ClearColorValue zero{std::array{0.0f, 0.0f, 0.0f, 0.0f}};
        cmdbuf.clearColorImage(depth.image, vk::ImageLayout::eGeneral, zero, Range());
        cmdbuf.clearColorImage(motion.image, vk::ImageLayout::eGeneral, zero, Range());
        FullBarrier(cmdbuf);

        const auto& settings = ::Dlss5::GetSettings();
        parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        parameters->Set("DLSSNR.Enabled", 1);
        parameters->Set("DLSSNR.Width", width);
        parameters->Set("DLSSNR.Height", height);
        parameters->Set("DLSSNR.DepthInverted", depth_reversed ? 1 : 0);
        parameters->Set("DLSSNR.Style", static_cast<u32>(settings.style));
        parameters->Set("DLSSNR.Intensity", settings.intensity);
        parameters->Set("DLSSNR.LocalToneStrength", settings.localTone);
        parameters->Set("DLSSNR.LocalStructureStrength", settings.localStructure);
        parameters->Set("DLSSNR.GlobalToneStrength", settings.globalTone);
        parameters->Set("DLSSNR.SkinStructureStrength", settings.skinStructure);
        parameters->Set("DLSSNR.UseAutoMask", settings.autoMask ? 1 : 0);
        parameters->Set("DLSSNR.UICorrection", settings.uiCorrection);
        parameters->Set("DLSSNR.Hint.Render.Preset", settings.preset);
        parameters->Set("DLSSNR.ScalingRatio", 1.0f);
        const auto result = create(device, cmdbuf, NeuralRendering, parameters, &feature);
        if (NVSDK_NGX_FAILED(result)) {
            LOG_WARNING(Render_Vulkan, "DLSS5: the {}x{} feature was not created ({:#x})", width,
                        height, static_cast<u32>(result));
            feature = nullptr;
            return;
        }
        reset = true;
        LOG_INFO(Render_Vulkan, "DLSS5: neural rendering at {}x{}", width, height);
    }

    void SetSubrect(const char* name, u32 w, u32 h) {
        const std::string prefix = std::string{"DLSSNR."} + name + "Subrect";
        parameters->Set((prefix + "BaseX").c_str(), 0);
        parameters->Set((prefix + "BaseY").c_str(), 0);
        parameters->Set((prefix + "Width").c_str(), static_cast<int>(w));
        parameters->Set((prefix + "Height").c_str(), static_cast<int>(h));
    }

    void Render(vk::CommandBuffer cmdbuf, vk::Image image, vk::Format format, u32 w, u32 h,
                const Dlss5Depth* scene_depth) {
        if (!available || w == 0 || h == 0) {
            return;
        }
        if (format != frame_format) {
            frame_format = format;
            invalidated = true;
        }
        const bool reversed = scene_depth && scene_depth->reversed;
        if (reversed != depth_reversed) {
            depth_reversed = reversed;
            invalidated = true;
        }
        if (const u32 current = ::Dlss5::SettingsGeneration(); current != generation) {
            generation = current;
            invalidated = true;
        }
        const u32 work_width = w;
        const u32 work_height = h;
        if (invalidated || work_width != width || work_height != height || !feature) {
            invalidated = false;
            Recreate(cmdbuf, work_width, work_height);
            return;
        }
        const bool has_motion =
            motion_supported && ::Dlss5::GetSettings().useMotion &&
            motion_search.EstimateMotion(cmdbuf, image, format, w, h);
        FullBarrier(cmdbuf);
        if (scene_depth && depth_pass.pipeline) {
            Dispatch(cmdbuf, depth_pass, scene_depth->view, depth.view,
                     {width, height, scene_depth->width, scene_depth->height});
        } else {
            const vk::ClearColorValue far_depth{
                std::array{reversed ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f}};
            cmdbuf.clearColorImage(depth.image, vk::ImageLayout::eGeneral, far_depth, Range());
        }
        FullBarrier(cmdbuf);
        if (srgb.image) {
            Copy(cmdbuf, image, srgb.image);
            FullBarrier(cmdbuf);
            Blit(cmdbuf, srgb.image, w, h, color[0].image, width, height);
        } else {
            Blit(cmdbuf, image, w, h, color[0].image, width, height);
        }
        FullBarrier(cmdbuf);

        auto color_resource = Resource(color[0], width, height, false);
        auto output_resource = Resource(color[1], width, height, true);
        auto depth_resource = Resource(depth, width, height, false);
        const WorkImage estimated_motion{.image = motion_search.MotionImage(),
                                         .view = motion_search.MotionView(),
                                         .format = vk::Format::eR16G16Sfloat};
        auto motion_resource =
            Resource(has_motion ? estimated_motion : motion, width, height, false);
        parameters->Set("DLSSNR.Color", static_cast<void*>(&color_resource));
        parameters->Set("DLSSNR.Output", static_cast<void*>(&output_resource));
        parameters->Set("DLSSNR.Depth", static_cast<void*>(&depth_resource));
        parameters->Set("DLSSNR.MVec", static_cast<void*>(&motion_resource));
        parameters->Set("DLSSNR.Reset", reset || !::Dlss5::GetSettings().temporal ? 1 : 0);
        SetSubrect("Color", width, height);
        SetSubrect("Output", width, height);
        SetSubrect("Depth", width, height);
        SetSubrect("MVec", width, height);
        parameters->Set("DLSSNR.MVecScaleX", 1.0f);
        parameters->Set("DLSSNR.MVecScaleY", 1.0f);
        const auto result = evaluate(cmdbuf, feature, parameters, nullptr);
        reset = false;
        FullBarrier(cmdbuf);
        if (NVSDK_NGX_FAILED(result)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOG_WARNING(Render_Vulkan, "DLSS5: evaluate failed ({:#x})",
                            static_cast<u32>(result));
            }
            return;
        }
        if (clamp_pass.pipeline) {
            Dispatch(cmdbuf, clamp_pass, color[0].view, color[1].view, {width, height, 0, 0});
            FullBarrier(cmdbuf);
        }
        if (srgb.image) {
            Blit(cmdbuf, color[1].image, width, height, srgb.image, w, h);
            FullBarrier(cmdbuf);
            Copy(cmdbuf, srgb.image, image);
        } else {
            Blit(cmdbuf, color[1].image, width, height, image, w, h);
        }
        FullBarrier(cmdbuf);
    }
};

Dlss5Pass::Dlss5Pass(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {
    if (impl->available) {
        g_pass = this;
    }
}

Dlss5Pass::~Dlss5Pass() {
    if (g_pass == this) {
        g_pass = nullptr;
    }
}

bool Dlss5Pass::IsAvailable() const {
    return impl->available;
}

void Dlss5Pass::Render(vk::CommandBuffer cmdbuf, vk::Image image, vk::ImageView, vk::Format format,
                       u32 width, u32 height, const Dlss5Depth* depth) {
    impl->Render(cmdbuf, image, format, width, height, depth);
}

void Dlss5Pass::Reset() {
    impl->reset = true;
}

void Dlss5Pass::Invalidate() {
    impl->invalidated = true;
}

namespace Dlss5 {

void PrepareDriver() {
    const HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!vulkan) {
        return;
    }
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    const auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VkInstance vk_instance{};
    if (!create_instance || create_instance(&instance_info, nullptr, &vk_instance) != VK_SUCCESS) {
        return;
    }
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        gipa(vk_instance, "vkEnumeratePhysicalDevices"));
    const auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        gipa(vk_instance, "vkEnumerateDeviceExtensionProperties"));
    const auto create_device =
        reinterpret_cast<PFN_vkCreateDevice>(gipa(vk_instance, "vkCreateDevice"));
    u32 count = 0;
    enumerate(vk_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(vk_instance, &count, devices.data());
    for (const VkPhysicalDevice physical : devices) {
        u32 extension_count = 0;
        enumerate_extensions(physical, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        enumerate_extensions(physical, nullptr, &extension_count, extensions.data());
        const bool has_import = std::ranges::any_of(extensions, [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_NVX_BINARY_IMPORT_EXTENSION_NAME) == 0;
        });
        if (!has_import) {
            continue;
        }
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        const char* names[] = {VK_NVX_BINARY_IMPORT_EXTENSION_NAME};
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = names;
        // The device is kept: the interop stays usable for the emulator's own device.
        VkDevice device{};
        create_device(physical, &device_info, nullptr, &device);
    }
}

Dlss5Pass* Get() {
    return g_pass;
}

} // namespace Dlss5

} // namespace Vulkan

#else

namespace Vulkan {

struct Dlss5Pass::Impl {};

Dlss5Pass::Dlss5Pass(const Instance&, Scheduler&) : impl{std::make_unique<Impl>()} {}
Dlss5Pass::~Dlss5Pass() = default;
bool Dlss5Pass::IsAvailable() const {
    return false;
}
void Dlss5Pass::Render(vk::CommandBuffer, vk::Image, vk::ImageView, vk::Format, u32, u32,
                       const Dlss5Depth*) {}
void Dlss5Pass::Reset() {}
void Dlss5Pass::Invalidate() {}

namespace Dlss5 {
void PrepareDriver() {}
Dlss5Pass* Get() {
    return nullptr;
}
} // namespace Dlss5

} // namespace Vulkan

#endif
