// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// cube_handle_vk_android cube renderer — vertex shader.
//
// Reads position + color from a vertex buffer (36 verts, 12 triangles).
// Kept deliberately trivial — Adreno's pipeline linker rejected the earlier
// "bake the cube in a big const array indexed by gl_VertexIndex" form
// ("Failed to link shaders"), so geometry lives in a real vertex buffer.
// The push-constant MVP (proj * view * model_rotation) places + spins it.

#version 450

layout(push_constant) uniform PC {
    mat4 mvp;  // proj * view * model
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 v_color;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    v_color = in_color;
}
