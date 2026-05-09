// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

// Post-weave chroma-key strip: woven RGB -> alpha=0 where RGB exact-matches
// the chroma key, alpha=1 elsewhere with RGB premultiplied so DWM's
//     src.rgb + (1-alpha)*dst.rgb
// blend doesn't add the matched chroma color to the desktop and saturate.

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
	vec3 c = texture(src, in_uv).rgb;
	vec3 d = abs(c - pc.chroma_rgb);
	bool match = max(max(d.r, d.g), d.b) < (1.0 / 512.0);
	float a = match ? 0.0 : 1.0;
	out_color = vec4(c * a, a);
}
