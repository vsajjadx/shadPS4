// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

// Resolves a multi-sample depth image into a single-sample color image: sample 0 of the depth
// goes to R, so a shader sampling it as .r or .rrrr sees the depth. Averaging samples would be
// wrong for depth-based effects that need one visibility decision per pixel.

layout (binding = 0, set = 0) uniform texture2DMS depth_ms;

layout (location = 0) out vec4 out_color;

void main()
{
    const float d = texelFetch(depth_ms, ivec2(gl_FragCoord.xy), 0).r;
    out_color = vec4(d, 0.0, 0.0, 1.0);
}
