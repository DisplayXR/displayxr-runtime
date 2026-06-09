// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
// Textured cube fragment shader (ports cube_handle_vk_win g_cubeTexturedFragSpv):
// Wood_Crate basecolor + normal-map + ambient-occlusion, single directional light.
#version 450

layout(set = 0, binding = 0) uniform sampler2D uBasecolorTex;
layout(set = 0, binding = 1) uniform sampler2D uNormalTex;
layout(set = 0, binding = 2) uniform sampler2D uAOTex;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec3 fragWorldTangent;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 baseColor = texture(uBasecolorTex, fragUV).xyz;
    vec3 normalMap = texture(uNormalTex, fragUV).xyz * 2.0 - 1.0;
    float ao = texture(uAOTex, fragUV).x;
    vec3 N = normalize(fragWorldNormal);
    vec3 T = normalize(fragWorldTangent);
    mat3 TBN = mat3(T, cross(N, T), N);
    vec3 normal = normalize(TBN * normalMap);
    vec3 lightDir = normalize(vec3(0.3, 0.8, 0.5));
    float diffuse = max(dot(normal, lightDir), 0.0) * 0.7 + 0.3;
    outColor = vec4(baseColor * ao * diffuse, 1.0);
}
