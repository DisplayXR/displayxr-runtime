// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

// Pre-weave compose-under-bg: composite the captured desktop region UNDER
// each per-view atlas tile, outputting opaque RGB the SR weaver can consume.
// Replaces the chroma-key trick on the Vulkan DP — preserves AA edges and
// genuinely semi-transparent (0<a<1) pixels.
//
//   out = mix(bg, atlas.rgb, atlas.a),  out.a = 1
//
// The desktop is at z=0 (display plane), so the same captured region is
// sampled into every tile; per-eye parallax comes from the atlas content,
// not the background.

#version 450

layout(binding = 0) uniform sampler2D atlas;
layout(binding = 1) uniform sampler2D bg;

layout(push_constant) uniform PC {
	vec2  bg_uv_origin;   // window TL on monitor, normalized
	vec2  bg_uv_extent;   // window size on monitor, normalized
	uvec2 tile_count;     // (tile_columns, tile_rows) — unused here but kept for layout symmetry
	uvec2 pad;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	// Plain compose-with-bg. Transparency holes are produced by the
	// post-weave alpha-gate pass — this shader never emits a chroma sentinel.
	vec4 a = texture(atlas, in_uv);
	vec2 tile_local = fract(in_uv * vec2(pc.tile_count));
	vec2 bg_uv = pc.bg_uv_origin + tile_local * pc.bg_uv_extent;
	vec3 b = textureLod(bg, bg_uv, 0.0).rgb;
	out_color = vec4(mix(b, a.rgb, a.a), 1.0);
}
