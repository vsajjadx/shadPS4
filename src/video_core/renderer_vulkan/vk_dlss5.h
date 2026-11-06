// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// The game's scene depth, sampled in the general layout.
struct Dlss5Depth {
    vk::ImageView view;
    u32 width;
    u32 height;
    bool reversed;
};

/// NVIDIA's DLSS 5 neural rendering, run on the final game picture inside the emulator.
class Dlss5Pass {
public:
    Dlss5Pass(const Instance& instance, Scheduler& scheduler);
    ~Dlss5Pass();

    /// True when NVIDIA's runtime and the neural rendering library came up on this device.
    bool IsAvailable() const;

    /// Replaces the contents of an image in the general layout with the neural rendering result.
    void Render(vk::CommandBuffer cmdbuf, vk::Image image, vk::ImageView view, vk::Format format,
                u32 width, u32 height, const Dlss5Depth* depth);

    /// Starts again without the history of the previous frames, such as after a scene cut.
    void Reset();

    /// Releases the feature on the next frame, once its settings changed.
    void Invalidate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

namespace Dlss5 {

/// Creates a throwaway device with VK_NVX_binary_import before the emulator reserves its guest
/// address space; afterwards the NVIDIA driver cannot bring up the interop the runtime needs.
void PrepareDriver();

/// The pass of the running presenter, null when DLSS 5 is unavailable.
Dlss5Pass* Get();

} // namespace Dlss5

} // namespace Vulkan
