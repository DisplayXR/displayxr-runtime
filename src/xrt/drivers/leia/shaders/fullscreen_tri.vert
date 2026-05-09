// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#version 450

layout(location = 0) out vec2 out_uv;

void main()
{
	// Fullscreen triangle: 3 vertices, no VBO.
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
	out_uv = uv;
}
