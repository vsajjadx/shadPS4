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
    bool fxaa;
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

// What a sample looks like on screen, which is the space FXAA has to judge edges in.
vec3 display(vec3 rgb) {
    if (pp.hdr) {
        return rgb;
    }
    return gamma(pp.srgb_input ? degamma(rgb) : rgb);
}

float luma_at(vec2 position) {
    const vec3 weights = vec3(0.299, 0.587, 0.114);
    return dot(display(textureLod(texSampler, position, 0.0).rgb), weights);
}

// FXAA 3.11 (Timothy Lottes): finds the edge a pixel lies on and shifts the sample across it.
// Returns the position to sample the frame at.
vec2 fxaa_position() {
    const float subpix_quality = 0.75;
    const float edge_threshold = 0.166;
    const float edge_threshold_min = 0.0833;
    // Distance of the successive samples along the edge, in pixels.
    const float steps[8] = float[8](1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0, 12.0);

    const vec2 rcp_frame = 1.0 / vec2(textureSize(texSampler, 0));
    vec2 pos_m = uv;

    const float luma_m = luma_at(pos_m);
    float luma_n = luma_at(pos_m + vec2(0.0, -1.0) * rcp_frame);
    float luma_s = luma_at(pos_m + vec2(0.0, 1.0) * rcp_frame);
    float luma_w = luma_at(pos_m + vec2(-1.0, 0.0) * rcp_frame);
    float luma_e = luma_at(pos_m + vec2(1.0, 0.0) * rcp_frame);

    const float range_max = max(max(luma_n, luma_s), max(max(luma_w, luma_e), luma_m));
    const float range_min = min(min(luma_n, luma_s), min(min(luma_w, luma_e), luma_m));
    const float range = range_max - range_min;
    if (range < max(edge_threshold_min, range_max * edge_threshold)) {
        return pos_m;
    }

    const float luma_nw = luma_at(pos_m + vec2(-1.0, -1.0) * rcp_frame);
    const float luma_ne = luma_at(pos_m + vec2(1.0, -1.0) * rcp_frame);
    const float luma_sw = luma_at(pos_m + vec2(-1.0, 1.0) * rcp_frame);
    const float luma_se = luma_at(pos_m + vec2(1.0, 1.0) * rcp_frame);

    const float luma_ns = luma_n + luma_s;
    const float luma_we = luma_w + luma_e;
    const float luma_nwsw = luma_nw + luma_sw;
    const float luma_nese = luma_ne + luma_se;
    const float luma_nwne = luma_nw + luma_ne;
    const float luma_swse = luma_sw + luma_se;

    const float edge_horz = abs(-2.0 * luma_w + luma_nwsw) +
                            abs(-2.0 * luma_m + luma_ns) * 2.0 +
                            abs(-2.0 * luma_e + luma_nese);
    const float edge_vert = abs(-2.0 * luma_n + luma_nwne) +
                            abs(-2.0 * luma_m + luma_we) * 2.0 +
                            abs(-2.0 * luma_s + luma_swse);
    const bool horz_span = edge_horz >= edge_vert;
    const float subpix_a = (luma_ns + luma_we) * 2.0 + luma_nwsw + luma_nese;

    float length_sign = rcp_frame.x;
    if (!horz_span) {
        luma_n = luma_w;
        luma_s = luma_e;
    } else {
        length_sign = rcp_frame.y;
    }

    const float subpix_b = subpix_a * (1.0 / 12.0) - luma_m;
    const float gradient_n = luma_n - luma_m;
    const float gradient_s = luma_s - luma_m;
    float luma_nn = luma_n + luma_m;
    const float luma_ss = luma_s + luma_m;
    const bool pair_n = abs(gradient_n) >= abs(gradient_s);
    const float gradient = max(abs(gradient_n), abs(gradient_s));
    if (pair_n) {
        length_sign = -length_sign;
    }
    const float subpix_c = clamp(abs(subpix_b) / range, 0.0, 1.0);

    vec2 pos_b = pos_m;
    const vec2 off_np = horz_span ? vec2(rcp_frame.x, 0.0) : vec2(0.0, rcp_frame.y);
    if (!horz_span) {
        pos_b.x += length_sign * 0.5;
    } else {
        pos_b.y += length_sign * 0.5;
    }

    vec2 pos_n = pos_b - off_np * steps[0];
    vec2 pos_p = pos_b + off_np * steps[0];
    const float subpix_d = -2.0 * subpix_c + 3.0;
    float luma_end_n = luma_at(pos_n);
    const float subpix_e = subpix_c * subpix_c;
    float luma_end_p = luma_at(pos_p);
    if (!pair_n) {
        luma_nn = luma_ss;
    }
    const float gradient_scaled = gradient * 0.25;
    const float luma_mm = luma_m - luma_nn * 0.5;
    const float subpix_f = subpix_d * subpix_e;
    const bool luma_mm_negative = luma_mm < 0.0;

    luma_end_n -= luma_nn * 0.5;
    luma_end_p -= luma_nn * 0.5;
    bool done_n = abs(luma_end_n) >= gradient_scaled;
    bool done_p = abs(luma_end_p) >= gradient_scaled;
    if (!done_n) {
        pos_n -= off_np * steps[1];
    }
    if (!done_p) {
        pos_p += off_np * steps[1];
    }

    for (int i = 2; i < 8 && (!done_n || !done_p); ++i) {
        if (!done_n) {
            luma_end_n = luma_at(pos_n) - luma_nn * 0.5;
        }
        if (!done_p) {
            luma_end_p = luma_at(pos_p) - luma_nn * 0.5;
        }
        done_n = abs(luma_end_n) >= gradient_scaled;
        done_p = abs(luma_end_p) >= gradient_scaled;
        if (!done_n) {
            pos_n -= off_np * steps[i];
        }
        if (!done_p) {
            pos_p += off_np * steps[i];
        }
    }

    float dst_n = pos_m.x - pos_n.x;
    float dst_p = pos_p.x - pos_m.x;
    if (!horz_span) {
        dst_n = pos_m.y - pos_n.y;
        dst_p = pos_p.y - pos_m.y;
    }

    const bool good_span_n = (luma_end_n < 0.0) != luma_mm_negative;
    const bool good_span_p = (luma_end_p < 0.0) != luma_mm_negative;
    const float span_length_rcp = 1.0 / (dst_p + dst_n);
    const bool direction_n = dst_n < dst_p;
    const float dst = min(dst_n, dst_p);
    const bool good_span = direction_n ? good_span_n : good_span_p;
    const float subpix_g = subpix_f * subpix_f;
    const float pixel_offset = dst * -span_length_rcp + 0.5;
    const float subpix_h = subpix_g * subpix_quality;
    const float pixel_offset_good = good_span ? pixel_offset : 0.0;
    const float pixel_offset_subpix = max(pixel_offset_good, subpix_h);
    if (!horz_span) {
        pos_m.x += pixel_offset_subpix * length_sign;
    } else {
        pos_m.y += pixel_offset_subpix * length_sign;
    }
    return pos_m;
}

void main() {
    vec4 color_linear = texture(texSampler, pp.fxaa ? fxaa_position() : uv);
    if (pp.hdr) {
        color = color_linear;
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
