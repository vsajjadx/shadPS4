// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    bool anti_aliasing;
    float brightness;
    float contrast;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

// A power curve on the encoded signal keeps black and white in place and moves the tones in
// between, so raising brightness shows dark scenes without washing out highlights.
vec3 adjust_brightness(vec3 rgb) {
    return pow(max(rgb, vec3(0.0)), vec3(1.0 / pp.brightness));
}

// Contrast stretches the encoded signal around mid-grey.
vec3 adjust_contrast(vec3 rgb) {
    return clamp((rgb - 0.5) * pp.contrast + 0.5, 0.0, 1.0);
}

vec3 display_color(vec2 coord) {
    vec3 rgb = texture(texSampler, coord).rgb;
    if (pp.hdr) {
        // Linear HDR values are stretched around a mid-grey of 0.18 instead.
        return 0.18 * pow(max(rgb * pp.brightness, vec3(0.0)) / 0.18, vec3(pp.contrast));
    }
    if (pp.srgb_input) rgb = degamma(rgb);
    return adjust_contrast(adjust_brightness(gamma(rgb)));
}

float luma(vec3 rgb) {
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

const float fxaa_reduce_min = 1.0 / 128.0;
const float fxaa_reduce_mul = 1.0 / 8.0;
const float fxaa_span_max = 8.0;

vec3 fxaa(vec2 coord) {
    const vec2 texel = 1.0 / vec2(textureSize(texSampler, 0));
    const vec3 rgb_nw = display_color(coord + vec2(-1.0, -1.0) * texel);
    const vec3 rgb_ne = display_color(coord + vec2(1.0, -1.0) * texel);
    const vec3 rgb_sw = display_color(coord + vec2(-1.0, 1.0) * texel);
    const vec3 rgb_se = display_color(coord + vec2(1.0, 1.0) * texel);
    const vec3 rgb_m = display_color(coord);

    const float luma_nw = luma(rgb_nw);
    const float luma_ne = luma(rgb_ne);
    const float luma_sw = luma(rgb_sw);
    const float luma_se = luma(rgb_se);
    const float luma_m = luma(rgb_m);
    const float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    const float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));

    vec2 dir = vec2(-((luma_nw + luma_ne) - (luma_sw + luma_se)),
                    (luma_nw + luma_sw) - (luma_ne + luma_se));
    const float dir_reduce =
        max((luma_nw + luma_ne + luma_sw + luma_se) * 0.25 * fxaa_reduce_mul, fxaa_reduce_min);
    const float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, vec2(-fxaa_span_max), vec2(fxaa_span_max)) * texel;

    const vec3 rgb_a = 0.5 * (display_color(coord + dir * (1.0 / 3.0 - 0.5)) +
                              display_color(coord + dir * (2.0 / 3.0 - 0.5)));
    const vec3 rgb_b = rgb_a * 0.5 + 0.25 * (display_color(coord + dir * -0.5) +
                                             display_color(coord + dir * 0.5));
    const float luma_b = luma(rgb_b);
    return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}

void main() {
    const float alpha = texture(texSampler, uv).a;
    if (pp.anti_aliasing) {
        color = vec4(fxaa(uv), alpha);
    } else {
        color = vec4(display_color(uv), alpha);
    }
}
