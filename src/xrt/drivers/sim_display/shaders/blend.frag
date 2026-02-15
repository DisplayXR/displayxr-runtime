// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#version 450

layout(binding = 0) uniform sampler2D left_tex;
layout(binding = 1) uniform sampler2D right_tex;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	// 50/50 alpha blend of both views for stereo alignment checking.
	out_color = mix(texture(left_tex, in_uv), texture(right_tex, in_uv), 0.5);
}
