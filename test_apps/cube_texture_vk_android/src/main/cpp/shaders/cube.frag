// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// cube_handle_vk_android cube renderer — fragment shader.
// Pass through the per-face color produced by cube.vert.

#version 450

layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(v_color, 1.0);
}
