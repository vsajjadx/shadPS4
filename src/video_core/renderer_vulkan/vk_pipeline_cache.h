// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <unordered_set>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>
#include <queue>
#include <tsl/robin_map.h>
#include "common/polyfill_thread.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "vulkan/vulkan.hpp"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

/// Runs Vulkan pipeline creation off the GPU command processor thread.
class PipelineCompileWorkers {
public:
    explicit PipelineCompileWorkers(u32 num_threads);
    ~PipelineCompileWorkers();

    void Submit(std::function<void()>&& task);
    void WaitIdle();

    [[nodiscard]] u32 NumThreads() const noexcept {
        return static_cast<u32>(threads.size());
    }

private:
    void Run(std::stop_token stoken, u32 index);

    std::mutex mutex;
    std::condition_variable_any work_cv;
    std::condition_variable_any idle_cv;
    std::queue<std::function<void()>> tasks;
    u32 in_flight{};
    std::vector<std::jthread> threads;
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
    bool is_indirect;
};

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {});

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule, u64>;
    /// With async set, a shader that isn't translated yet is translated on a worker and an empty
    /// result is returned until it is ready.
    Result GetProgram(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding,
                      bool async = false);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();
    bool IsDrawSkippable();
    bool IsDispatchSkippable();
    template <typename PipelineType>
    bool AwaitPipeline(PipelineType* pipeline, bool skippable);

    struct ModuleBuild {
        vk::ShaderModule module{};
        std::vector<u32> spv;
        std::optional<std::vector<u32>> patch;
        bool failed{};
        std::string error;
    };
    struct PendingTranslation;

    std::unordered_set<u64> failed_shaders;
    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    ModuleBuild BuildModule(Shader::Pools& build_pools, Shader::Info& info,
                            Shader::RuntimeInfo& runtime_info, std::span<const u32> code,
                            size_t perm_idx, Shader::Backend::Bindings& binding);
    vk::ShaderModule FinishModule(const Shader::Info& info, std::span<const u32> code,
                                  size_t perm_idx, ModuleBuild&& build);
    void SubmitTranslation(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                           const Shader::ShaderParams& params,
                           const Shader::RuntimeInfo& runtime_info,
                           const Shader::Backend::Bindings& start_binding, size_t perm_idx,
                           std::optional<Shader::StageSpecialization> spec);
    void CollectTranslations();
    void RunTranslation(PendingTranslation& translation, Shader::Pools& build_pools);
    void WaitTranslations(u64 hash);
    size_t NewPermutationIndex(u64 hash, size_t num_modules);
    std::filesystem::path DriverCachePath() const;
    void SaveDriverCache();
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::HwStage stage, Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    DrawIndirectParams draw_indirect_params{};
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<u64, size_t> next_permutation;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    GraphicsPipelineKey last_graphics_key{};
    const GraphicsPipeline* last_graphics_pipeline = nullptr;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    Shader::Gcn::FetchShaderData* fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    u32 num_pending_draws_skipped{};
    u32 num_pending_translations_skipped{};
    bool draw_skippable{};
    struct TargetStreak {
        u32 last_frame{~0u};
        u32 frames{};
    };
    tsl::robin_map<VAddr, TargetStreak> target_streaks;
    u32 driver_cache_saved_pipelines{};
    std::chrono::steady_clock::time_point driver_cache_saved_at{};
    // Shared with the queued worker task, which may still run after the GPU thread did the
    // translation itself and collected it.
    tsl::robin_map<u64, std::deque<std::shared_ptr<PendingTranslation>>> pending_translations;
    std::chrono::steady_clock::time_point pending_wait_window_start{};
    std::chrono::microseconds pending_wait_spent{};
    // Declared last so the workers finish before the pipelines they build are destroyed.
    PipelineCompileWorkers compile_workers;
};

} // namespace Vulkan
