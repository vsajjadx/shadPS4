// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::DlssG {

/// Loads the Streamline interposer when DLSS-G is selected and returns its
/// vkGetInstanceProcAddr, or nullptr when the regular Vulkan loader must be used.
PFN_vkGetInstanceProcAddr Initialize();

void OnDeviceCreated(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device,
                     u32 queue_family_index);

void Shutdown();

bool IsAvailable();
void SetEnabled(bool enabled);
bool IsEnabled();

} // namespace Vulkan::DlssG
