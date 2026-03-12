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
	// Sample left eye (tile 0,0)
	vec2 left_uv = in_uv * vec2(tile.inv_tile_columns, tile.inv_tile_rows);
	vec4 left = texture(atlas_tex, left_uv);

	// Sample right eye (tile 1,0)
	vec2 right_uv = left_uv + vec2(tile.inv_tile_columns, 0.0);
	vec4 right = texture(atlas_tex, right_uv);

	// 50/50 alpha blend of both views for stereo alignment checking.
	out_color = mix(left, right, 0.5);
}
