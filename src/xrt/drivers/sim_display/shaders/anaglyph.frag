// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#version 450

layout(binding = 0) uniform sampler2D left_tex;
layout(binding = 1) uniform sampler2D right_tex;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	// Red-cyan anaglyph: red channel from left eye,
	// green+blue channels from right eye.
	vec4 left = texture(left_tex, in_uv);
	vec4 right = texture(right_tex, in_uv);
	out_color = vec4(left.r, right.g, right.b, 1.0);
}
