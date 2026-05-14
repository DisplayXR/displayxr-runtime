// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

// Post-weave alpha-gate: compose-mode replacement for the chroma-key strip.
// Samples the woven back-buffer and the original atlas. For each screen pixel
// at tile-local UV, tests whether EVERY view's atlas has α==0 at the matching
// tile-local position. Pixels passing the test get (0,0,0,0) so DWM blends the
// live desktop. Others keep the woven RGB at α=1.
//
// No chroma keying — captured-bg lag is bypassed in flat transparent regions
// and silhouettes show the smooth lenticular blend the weaver produced from
// captured-bg ↔ atlas-content inputs.
//
// Screen UV equals tile-local UV when target = (tile_columns × view_w,
// tile_rows × view_h), the canvas-fills-target case.

#version 450

layout(binding = 0) uniform sampler2D backbuffer;
layout(binding = 1) uniform sampler2D atlas;

layout(push_constant) uniform PC {
	uvec2 tile_count;
	uvec2 pad;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	bool all_transparent = true;
	for (uint ty = 0u; ty < pc.tile_count.y; ty++) {
		for (uint tx = 0u; tx < pc.tile_count.x; tx++) {
			vec2 uv_at_tile = (vec2(tx, ty) + in_uv) / vec2(pc.tile_count);
			if (textureLod(atlas, uv_at_tile, 0.0).a > 0.0) {
				all_transparent = false;
			}
		}
	}
	vec3 rgb = texture(backbuffer, in_uv).rgb;
	float m = all_transparent ? 0.0 : 1.0;
	out_color = vec4(rgb * m, m);   // premultiplied for DWM
}
