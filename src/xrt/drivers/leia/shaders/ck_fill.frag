// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

// Pre-weave chroma-key fill: atlas RGBA -> opaque RGB. Pixels with alpha=0
// become the chroma key; alpha=1 pixels pass through unchanged. The output
// alpha is forced to 1.0 because the SR weaver requires opaque RGB.

#version 450

layout(binding = 0) uniform sampler2D src;

layout(push_constant) uniform PC {
	vec3 chroma_rgb;
	float pad;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	vec4 c = texture(src, in_uv);
	out_color = vec4(mix(pc.chroma_rgb, c.rgb, c.a), 1.0);
}
