// Copyright 2026, Leia Inc.
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
	// Quad display: 2x2 grid on screen.
	// TL=view0, TR=view1, BL=view2, BR=view3.
	float col_idx = (in_uv.x < 0.5) ? 0.0 : 1.0;
	float row_idx = (in_uv.y < 0.5) ? 0.0 : 1.0;
	float view_index = row_idx * 2.0 + col_idx;

	// Local UV within the quadrant [0,1]
	float local_u = fract(in_uv.x * 2.0);
	float local_v = fract(in_uv.y * 2.0);

	// Map to the correct tile in the atlas
	float col = mod(view_index, tile.tile_columns);
	float row = floor(view_index / tile.tile_columns);
	float atlas_u = (local_u + col) * tile.inv_tile_columns;
	float atlas_v = (local_v + row) * tile.inv_tile_rows;

	out_color = texture(atlas_tex, vec2(atlas_u, atlas_v));
}
