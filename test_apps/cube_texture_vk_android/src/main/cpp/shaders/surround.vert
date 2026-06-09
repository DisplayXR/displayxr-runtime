// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
// Fullscreen triangle (no vertex buffer) — mirrors the Windows blit VS.
#version 450

layout(location = 0) out vec2 vUV;

void main() {
    vUV = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(vUV * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
