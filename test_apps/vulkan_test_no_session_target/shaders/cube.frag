// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
// Cube fragment shader - simple directional lighting

#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 worldViewProj;
    vec4 color;
} ubo;

layout(location = 0) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

void main() {
    // Simple directional lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float ndotl = max(dot(fragNormal, lightDir), 0.0);
    vec3 ambient = vec3(0.2, 0.2, 0.3);
    vec3 diffuse = ubo.color.rgb * ndotl;
    outColor = vec4(ambient + diffuse, 1.0);
}
