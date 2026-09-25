// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// cube_handle_vk_android cube renderer — fragment shader.
// Pass through the per-face color produced by cube.vert.

#version 450

// ADR-021 / INV-4.6: v_color is authored DISPLAY-REFERRED. When the colour
// swapchain is `_SRGB` its attachment encodes on write, so the host
// specializes uLinearize = true (g_scene_linear) and the standard sRGB EOTF
// decodes the colour first — otherwise it would be encoded twice (washed out).
layout(constant_id = 0) const bool uLinearize = false;

layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 col = v_color;
    if (uLinearize) {
        col = mix(pow((col + 0.055) / 1.055, vec3(2.4)),
                  col / 12.92,
                  lessThanEqual(col, vec3(0.04045)));
    }
    out_color = vec4(col, 1.0);
}
