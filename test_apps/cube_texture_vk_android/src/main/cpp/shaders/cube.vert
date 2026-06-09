// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
// Textured cube vertex shader (ports cube_handle_vk_win g_cubeTexturedVertSpv):
// position + uv + normal + tangent; MVP transform; world-space normal/tangent.
#version 450

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;   // unused (kept for vertex-format parity)
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aNormal;
layout(location = 4) in vec3 aTangent;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) out vec3 fragWorldTangent;

void main() {
    gl_Position = pc.mvp * vec4(aPos, 1.0);
    mat3 m = mat3(pc.model);
    fragUV = aUV;
    fragWorldNormal = m * aNormal;
    fragWorldTangent = m * aTangent;
}
