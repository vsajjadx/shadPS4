// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <numeric>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_occlusion_counter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

constexpr u32 NumQueries = 4096;
constexpr u64 CounterValidMask = 0x8000000000000000ULL;
constexpr u32 FramesBeforeIdle = 120;

OcclusionCounter::OcclusionCounter(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    // Queries are reset from the host as soon as their result is read, so a query never has to
    // be reset in a command buffer outside a rendering pass.
    if (!instance.IsHostQueryResetSupported()) {
        LOG_WARNING(Render_Vulkan, "Host query reset unsupported, occlusion counters are faked");
        return;
    }
    auto [result, pool] = instance.GetDevice().createQueryPoolUnique({
        .queryType = vk::QueryType::eOcclusion,
        .queryCount = NumQueries,
    });
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to create the occlusion query pool: {}",
                    vk::to_string(result));
        return;
    }
    query_pool = std::move(pool);
    instance.GetDevice().resetQueryPool(*query_pool, 0, NumQueries);
    if (instance.IsOcclusionQueryPreciseSupported()) {
        control_flags = vk::QueryControlFlagBits::ePrecise;
    }
    free_queries.resize(NumQueries);
    std::iota(free_queries.rbegin(), free_queries.rend(), 0U);
}

OcclusionCounter::~OcclusionCounter() = default;

void OcclusionCounter::Dump(u64* results, u32 num_pairs) {
    dumped_this_frame = true;
    counting = true;
    const bool rendering = scheduler.IsRendering();
    if (rendering) {
        EndQuery(scheduler.CommandBuffer());
    }
    dumps.push_back({
        .results = results,
        .num_pairs = num_pairs,
        .num_queries_before = num_ended_queries,
    });
    if (rendering) {
        BeginQuery(scheduler.CommandBuffer());
    }
    Resolve();
}

void OcclusionCounter::Resolve() {
    const auto device = instance.GetDevice();
    for (;;) {
        while (!dumps.empty() && dumps.front().num_queries_before <= num_resolved_queries) {
            WriteDump(dumps.front());
            dumps.pop_front();
        }
        if (ended_queries.empty() || !scheduler.IsFree(ended_queries.front().tick)) {
            break;
        }
        const auto query = ended_queries.front();
        ended_queries.pop_front();
        u64 query_samples{};
        const auto result = device.getQueryPoolResults(
            *query_pool, query.index, 1, sizeof(query_samples), &query_samples,
            sizeof(query_samples), vk::QueryResultFlagBits::e64);
        if (result == vk::Result::eSuccess) {
            samples += query_samples;
        }
        device.resetQueryPool(*query_pool, query.index, 1);
        free_queries.push_back(query.index);
        ++num_resolved_queries;
    }
}

void OcclusionCounter::TickFrame() {
    frames_without_dumps = dumped_this_frame ? 0 : frames_without_dumps + 1;
    dumped_this_frame = false;
    if (frames_without_dumps >= FramesBeforeIdle) {
        counting = false;
    }
    Resolve();
}

void OcclusionCounter::OnRenderingBegin(CommandRecorder cmdbuf) {
    BeginQuery(cmdbuf);
}

void OcclusionCounter::OnRenderingEnd(CommandRecorder cmdbuf) {
    EndQuery(cmdbuf);
}

void OcclusionCounter::BeginQuery(CommandRecorder cmdbuf) {
    if (!counting || active_query || !query_pool) {
        return;
    }
    if (free_queries.empty()) {
        Resolve();
        if (free_queries.empty()) {
            // Samples drawn until a query is free again are not counted.
            return;
        }
    }
    active_query = free_queries.back();
    free_queries.pop_back();
    // A query ends in the command buffer it began in. Its tick is read here because the end can
    // be recorded while that command buffer is being submitted, after the tick has advanced.
    active_query_tick = scheduler.CurrentTick();
    cmdbuf.beginQuery(*query_pool, *active_query, control_flags);
}

void OcclusionCounter::EndQuery(CommandRecorder cmdbuf) {
    if (!active_query) {
        return;
    }
    cmdbuf.endQuery(*query_pool, *active_query);
    ended_queries.push_back({
        .index = *active_query,
        .tick = active_query_tick,
    });
    active_query.reset();
    ++num_ended_queries;
}

void OcclusionCounter::WriteDump(const PendingDump& dump) const {
    // The whole count goes to the first block; a game sums the differences over all blocks.
    for (u32 i = 0; i < dump.num_pairs; ++i) {
        dump.results[i * 2] = (i == 0 ? samples : 0) | CounterValidMask;
    }
}

} // namespace Vulkan
