// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include "common/types.h"

namespace Vulkan {

struct ComputeSubgroupLimits {
    bool size_control_enabled{};
    bool compute_required_size{};
    u32 default_size{};
    u32 min_size{};
    u32 max_size{};
    u32 max_workgroup_subgroups{};
    u32 max_workgroup_invocations{};
    std::array<u32, 3> max_workgroup_size{};
};

struct ComputeSubgroupPolicy {
    u32 effective_size{};
    u32 required_size{};

    bool operator==(const ComputeSubgroupPolicy&) const = default;
};

constexpr std::optional<ComputeSubgroupPolicy> SelectComputeSubgroupPolicy(
    const ComputeSubgroupLimits& limits, const std::array<u32, 3>& workgroup_size) {
    u64 invocations = 1;
    for (size_t axis = 0; axis < workgroup_size.size(); ++axis) {
        const u32 size = workgroup_size[axis];
        if (size == 0 || size > limits.max_workgroup_size[axis] ||
            invocations > limits.max_workgroup_invocations / size) {
            return std::nullopt;
        }
        invocations *= size;
    }

    const auto fits_subgroups = [&](u32 size) {
        return invocations <= u64{size} * limits.max_workgroup_subgroups;
    };
    if (limits.size_control_enabled && limits.compute_required_size) {
        for (const u32 size : {64u, 32u}) {
            if (limits.min_size <= size && size <= limits.max_size && fits_subgroups(size)) {
                return ComputeSubgroupPolicy{size, size};
            }
        }
    }

    // SPIR-V 1.6 permits varying subgroup sizes without an explicit size request.
    if ((limits.default_size == 32 || limits.default_size == 64) &&
        limits.default_size == limits.min_size && limits.min_size == limits.max_size &&
        fits_subgroups(limits.default_size)) {
        return ComputeSubgroupPolicy{limits.default_size, 0};
    }
    return std::nullopt;
}

} // namespace Vulkan
