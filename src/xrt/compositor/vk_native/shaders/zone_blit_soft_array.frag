// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// The runtime-drawn cursor, softened against crosstalk (ADR-046 Amendment 2
// B.7), sampler2DArray source (layer params.x). Identical to
// zone_blit_soft.frag but for the sampler - see that file for the kernel and
// the push-constant packing.

#version 450

layout(constant_id = 0) const bool straight = false;

layout(binding = 0) uniform sampler2DArray src_tex;

layout(push_constant) uniform ComposeParams {
	mat4 mvp;
	vec4 src_rect;
	vec4 params;      // x = array slice; y = quad flag; zw = blur radius (uv)
	vec4 color_scale;
	vec4 color_bias;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec4 tap(vec2 uv)
{
	vec4 s = texture(src_tex, vec3(uv, pc.params.x));
	if (straight) {
		s.rgb *= s.a;
	}
	bool inside = uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
	return inside ? s : vec4(0.0);
}

void main()
{
	const vec2 r = pc.params.zw;
	vec4 acc = tap(in_uv);
	float wsum = 1.0;
	if (r.x > 0.0 || r.y > 0.0) {
		const float wi = 0.7066;
		const float wo = 0.2494;
		acc += wi * tap(in_uv + vec2(0.5, 0.0) * r);
		acc += wi * tap(in_uv + vec2(-0.5, 0.0) * r);
		acc += wi * tap(in_uv + vec2(0.0, 0.5) * r);
		acc += wi * tap(in_uv + vec2(0.0, -0.5) * r);
		for (int k = 0; k < 8; k++) {
			float a = 0.3927 + 0.7854 * float(k);
			acc += wo * tap(in_uv + vec2(cos(a), sin(a)) * r);
		}
		wsum += 4.0 * wi + 8.0 * wo;
	}
	acc /= wsum;
	if (straight) {
		vec3 rgb = acc.a > 1e-5 ? acc.rgb / acc.a : vec3(0.0);
		acc = vec4(rgb, acc.a);
	}
	out_color = acc * pc.color_scale + pc.color_bias;
}
