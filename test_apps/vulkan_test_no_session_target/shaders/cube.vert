// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
// Cube vertex shader - transforms vertices and passes normals to fragment shader

#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 worldViewProj;
    vec4 color;
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragNormal;

void main() {
    gl_Position = ubo.worldViewProj * vec4(inPosition, 1.0);
    fragNormal = inNormal;
}
