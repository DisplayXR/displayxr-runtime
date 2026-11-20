// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// The weave-rect cursor of the Vulkan XR_DXR_weave engines (ADR-046 Amendment
// 6), softened against crosstalk (Amendment 2 B.7). The kernel is the one of
// vk_native's zone_blit_soft.frag, the Metal `cursor_fragment_soft` and the
// D3D11 `DXR_SOFTEN` pixel shader: 13 taps (centre, 4 at r/2, 8 at r; sigma
// 0.6 r), a tap outside sprite uv 0..1 transparent (the box is padded by the
// radius). The sprite is PREMULTIPLIED (every carrier is), so the opacity
// scales all four channels and the pipeline blends One / OneMinusSrcAlpha.

#version 450

layout(binding = 0) uniform sampler2D src_tex;

layout(push_constant) uniform Push {
	vec4 uv_rect; // xy = sprite uv of the box's top-left, zw = the uv it spans
	vec2 radius;  // blur radius in sprite uv (0 = crisp)
	float alpha;  // opacity
	float pad;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec4 tap(vec2 uv)
{
	vec4 s = texture(src_tex, uv);
	bool inside = uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
	return inside ? s : vec4(0.0);
}

void main()
{
	const vec2 uv = pc.uv_rect.xy + in_uv * pc.uv_rect.zw;
	const vec2 r = pc.radius;
	vec4 acc = tap(uv);
	float wsum = 1.0;
	if (r.x > 0.0 || r.y > 0.0) {
		const float wi = 0.7066;
		const float wo = 0.2494;
		acc += wi * tap(uv + vec2(0.5, 0.0) * r);
		acc += wi * tap(uv + vec2(-0.5, 0.0) * r);
		acc += wi * tap(uv + vec2(0.0, 0.5) * r);
		acc += wi * tap(uv + vec2(0.0, -0.5) * r);
		for (int k = 0; k < 8; k++) {
			float a = 0.3927 + 0.7854 * float(k);
			acc += wo * tap(uv + vec2(cos(a), sin(a)) * r);
		}
		wsum += 4.0 * wi + 8.0 * wo;
	}
	out_color = acc / wsum * pc.alpha;
}
