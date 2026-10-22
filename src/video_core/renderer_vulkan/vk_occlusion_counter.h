// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <optional>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class CommandRecorder;
class Instance;
class Scheduler;

/// Emulates the sample counters of the depth blocks. A ZPASS_DONE dump stores, for each block,
/// the number of samples that have passed the depth test so far; games subtract the dump before
/// a draw from the dump after it to know how much of it is visible (lens flares, light glows,
/// visibility culling). The samples are counted with host occlusion queries, one for each
/// rendering pass between two dumps, and a dump is written once every query before it is done.
class OcclusionCounter {
public:
    explicit OcclusionCounter(const Instance& instance, Scheduler& scheduler);
    ~OcclusionCounter();

    [[nodiscard]] bool IsSupported() const noexcept {
        return static_cast<bool>(query_pool);
    }

    /// Writes the counters to results, a begin/end pair of 64-bit words per block, once the
    /// GPU has counted the samples drawn before this point.
    void Dump(u64* results, u32 num_pairs);

    /// Writes the dumps whose samples the GPU has counted.
    void Resolve();

    /// Stops counting when the game no longer dumps the counters.
    void TickFrame();

    [[nodiscard]] bool HasPendingDumps() const noexcept {
        return !dumps.empty();
    }

    void OnRenderingBegin(CommandRecorder cmdbuf);
    void OnRenderingEnd(CommandRecorder cmdbuf);

private:
    struct EndedQuery {
        u32 index;
        u64 tick;
    };
    struct PendingDump {
        u64* results;
        u32 num_pairs;
        u64 num_queries_before;
    };

    void BeginQuery(CommandRecorder cmdbuf);
    void EndQuery(CommandRecorder cmdbuf);
    void WriteDump(const PendingDump& dump) const;

    const Instance& instance;
    Scheduler& scheduler;
    vk::UniqueQueryPool query_pool;
    vk::QueryControlFlags control_flags{};
    std::vector<u32> free_queries;
    std::optional<u32> active_query;
    u64 active_query_tick{};
    std::deque<EndedQuery> ended_queries;
    std::deque<PendingDump> dumps;
    u64 num_ended_queries{};
    u64 num_resolved_queries{};
    u64 samples{};
    bool counting{};
    u32 frames_without_dumps{};
    bool dumped_this_frame{};
};

} // namespace Vulkan
