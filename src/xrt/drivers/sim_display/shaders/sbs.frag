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
	// Determine which tile column we're in based on screen X
	float col = floor(in_uv.x * tile.tile_columns);
	col = min(col, tile.tile_columns - 1.0);

	// Map screen UV to atlas UV
	float atlas_u = (col + fract(in_uv.x * tile.tile_columns)) * tile.inv_tile_columns;
	float atlas_v = in_uv.y * tile.inv_tile_rows;

	out_color = texture(atlas_tex, vec2(atlas_u, atlas_v));
}
