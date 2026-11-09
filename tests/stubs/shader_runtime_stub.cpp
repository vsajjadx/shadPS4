// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/hack_features.h"
#include "shader_recompiler/resource.h"

namespace Common {
// No game is loaded by the instruction/compiler unit tests.
bool HackFeatures::isTheOrder1886 = false;
} // namespace Common

namespace Shader {
bool IsPlausibleImageSharp(const AmdGpu::Image& /*sharp*/) {
    // Instruction tests do not bind guest image descriptors. The actual validator
    // lives in the Vulkan renderer, which is deliberately not linked into tests.
    return false;
}
} // namespace Shader
