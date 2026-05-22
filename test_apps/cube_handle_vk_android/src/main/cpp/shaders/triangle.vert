// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// cube_handle_vk_android cube renderer — vertex shader.
//
// Generates a 36-vertex cube (12 triangles, 2 per face) from
// gl_VertexIndex. 8 unique corners, indexed in-shader. Per-face color
// (6 distinct values) makes back-face occlusion + rotation visually
// obvious. model_view_proj = proj_from_fov * view_from_pose * model,
// where model is a time-rotated translation set up on the C++ side.

#version 450

layout(push_constant) uniform PC {
    mat4 model_view_proj;
} pc;

layout(location = 0) out vec3 v_color;

void main() {
    // 8 unique cube corners at ±0.5. Model matrix on the C++ side
    // scales to ~0.2 m and translates 1 m in front of viewer. Cube
    // convention: +X right, +Y up, +Z toward viewer; viewer looks
    // along -Z so the "front" face is the +Z side.
    const vec3 corners[8] = vec3[8](
        vec3(-0.5, -0.5, -0.5),  // 0: left-bottom-back
        vec3( 0.5, -0.5, -0.5),  // 1: right-bottom-back
        vec3( 0.5,  0.5, -0.5),  // 2: right-top-back
        vec3(-0.5,  0.5, -0.5),  // 3: left-top-back
        vec3(-0.5, -0.5,  0.5),  // 4: left-bottom-front
        vec3( 0.5, -0.5,  0.5),  // 5: right-bottom-front
        vec3( 0.5,  0.5,  0.5),  // 6: right-top-front
        vec3(-0.5,  0.5,  0.5)   // 7: left-top-front
    );

    // 36 indices = 12 triangles, 2 per face, CCW winding when viewed
    // from outside (matches VK_FRONT_FACE_COUNTER_CLOCKWISE).
    const int indices[36] = int[36](
        // Front (+Z): 4,5,6  4,6,7
        4, 5, 6,  4, 6, 7,
        // Back (-Z): 1,0,3  1,3,2
        1, 0, 3,  1, 3, 2,
        // Left (-X): 0,4,7  0,7,3
        0, 4, 7,  0, 7, 3,
        // Right (+X): 5,1,2  5,2,6
        5, 1, 2,  5, 2, 6,
        // Top (+Y): 7,6,2  7,2,3
        7, 6, 2,  7, 2, 3,
        // Bottom (-Y): 0,1,5  0,5,4
        0, 1, 5,  0, 5, 4
    );

    // One color per face — 6 vertices per face share a color.
    const vec3 face_colors[6] = vec3[6](
        vec3(1.0, 0.2, 0.2),  // front:  red
        vec3(0.2, 1.0, 0.2),  // back:   green
        vec3(0.2, 0.2, 1.0),  // left:   blue
        vec3(1.0, 1.0, 0.2),  // right:  yellow
        vec3(1.0, 0.2, 1.0),  // top:    magenta
        vec3(0.2, 1.0, 1.0)   // bottom: cyan
    );

    int face = gl_VertexIndex / 6;
    int corner = indices[gl_VertexIndex];

    gl_Position = pc.model_view_proj * vec4(corners[corner], 1.0);
    v_color = face_colors[face];
}
