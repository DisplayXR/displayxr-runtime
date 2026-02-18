// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#version 450

layout(binding = 0) uniform sampler2D left_tex;
layout(binding = 1) uniform sampler2D right_tex;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	if (in_uv.x < 0.5) {
		// Left half: sample left eye
		vec2 uv = vec2(in_uv.x * 2.0, in_uv.y);
		out_color = texture(left_tex, uv);
	} else {
		// Right half: sample right eye
		vec2 uv = vec2((in_uv.x - 0.5) * 2.0, in_uv.y);
		out_color = texture(right_tex, uv);
	}
}
