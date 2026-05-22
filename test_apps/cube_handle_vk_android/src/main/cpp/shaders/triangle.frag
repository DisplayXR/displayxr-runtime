// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// cube_handle_vk_android triangle renderer — fragment shader.
// Pass through the per-vertex color produced by triangle.vert.

#version 450

layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(v_color, 1.0);
}
