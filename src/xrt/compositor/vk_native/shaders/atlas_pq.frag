// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// #2340 HDR10 - re-encode the composed (sRGB-encoded) atlas as SMPTE ST 2084
// PQ, 1.0 linear = 203 cd/m^2 (BT.2408 reference white), primaries passed
// through. The GLSL twin of the D3D11 compositor's atlas_pq_encode_ps_source
// (#2294): it reads a copy of the atlas texel for texel (texelFetch, no
// filtering), so the pass is exact. Alpha is untouched.

#version 450

layout(binding = 0) uniform sampler2D src_tex;

layout(location = 0) out vec4 out_color;

vec3 srgb_to_linear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

vec3 linear_to_pq(vec3 y)
{
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	vec3 ym = pow(clamp(y, 0.0, 1.0), vec3(m1));
	return pow((c1 + c2 * ym) / (1.0 + c3 * ym), vec3(m2));
}

void main()
{
	vec4 c = texelFetch(src_tex, ivec2(gl_FragCoord.xy), 0);
	out_color = vec4(linear_to_pq(srgb_to_linear(clamp(c.rgb, 0.0, 1.0)) * (203.0 / 10000.0)), c.a);
}
