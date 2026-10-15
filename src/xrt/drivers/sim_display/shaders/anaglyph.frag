// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#version 450

layout(binding = 0) uniform sampler2D atlas_tex;

layout(push_constant) uniform TileParams {
	float inv_tile_columns;  // 1.0 / tile_columns
	float inv_tile_rows;     // 1.0 / tile_rows
	float tile_columns;
	float tile_rows;
} tile;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	// Sample left eye (tile index 0: col=0, row=0)
	vec2 left_uv = in_uv * vec2(tile.inv_tile_columns, tile.inv_tile_rows);
	vec4 left = texture(atlas_tex, left_uv);

	// Sample right eye (tile index 1: general position)
	float col = mod(1.0, tile.tile_columns);
	float row = floor(1.0 / tile.tile_columns);
	vec2 right_uv = vec2((in_uv.x + col) * tile.inv_tile_columns,
	                     (in_uv.y + row) * tile.inv_tile_rows);
	vec4 right = texture(atlas_tex, right_uv);

	// Red-cyan anaglyph: red channel from left eye,
	// green+blue channels from right eye. Alpha passes through so
	// transparent-background regions survive the anaglyph mix (issue #392).
	//
	// It is the alpha both views cover (#2121). A pixel has one alpha while its
	// red and its green/blue come from different views, so where only one view
	// covers (a silhouette's edge, shifted by the disparity) the max of the two
	// showed that view's channels alone, opaque: a red and a cyan copy of a
	// transparent app's silhouette side by side, read as ghosting over the
	// desktop. Where the views agree (opaque content, a 2D zone) min == max and
	// the colour is unchanged. The atlas is premultiplied: each view's colour is
	// taken out of its own alpha and premultiplied by the shared one.
	float a = min(left.a, right.a);
	vec3 lc = left.a > 0.0 ? left.rgb / left.a : vec3(0.0);
	vec3 rc = right.a > 0.0 ? right.rgb / right.a : vec3(0.0);
	out_color = vec4(vec3(lc.r, rc.g, rc.b) * a, a);
}
