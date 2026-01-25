// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
// Grid fragment shader - outputs solid color from uniform

#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 worldViewProj;
    vec4 color;
} ubo;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = ubo.color;
}
