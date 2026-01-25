// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
// Grid vertex shader - simple transform for line rendering

#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 worldViewProj;
    vec4 color;
} ubo;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = ubo.worldViewProj * vec4(inPosition, 1.0);
}
