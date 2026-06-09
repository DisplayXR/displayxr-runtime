// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// cube_handle_vk_android triangle renderer — vertex shader.
//
// Generates 3 triangle vertices from gl_VertexIndex (no vertex buffer).
// The triangle sits 1 m in front of the user, ~30 cm tall, so xrLocateViews-
// derived projection + view matrices land it visibly with parallax.

#version 450

layout(push_constant) uniform PC {
    mat4 view_proj;  // proj_from_fov * view_from_pose
} pc;

layout(location = 0) out vec3 v_color;

void main() {
    // World-space triangle: 1 m in front of the user, centered on Y axis.
    // Wide enough to be visible at typical Lume Pad viewing distance.
    const vec3 positions[3] = vec3[3](
        vec3(-0.15, -0.15, -1.0),  // bottom-left, red
        vec3( 0.15, -0.15, -1.0),  // bottom-right, green
        vec3( 0.0,   0.15, -1.0)   // top-center, blue
    );
    const vec3 colors[3] = vec3[3](
        vec3(1.0, 0.0, 0.0),
        vec3(0.0, 1.0, 0.0),
        vec3(0.0, 0.0, 1.0)
    );

    gl_Position = pc.view_proj * vec4(positions[gl_VertexIndex], 1.0);
    v_color = colors[gl_VertexIndex];
}
