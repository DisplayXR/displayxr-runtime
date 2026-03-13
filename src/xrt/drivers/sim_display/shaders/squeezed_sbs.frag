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
	// Squeezed SBS: left half of screen = left eye, right half = right eye.
	// No cropping — each tile is displayed as-is in its half of the screen.
	float eye_index;
	float eye_u;
	if (in_uv.x < 0.5) {
		eye_index = 0.0;
		eye_u = in_uv.x / 0.5;
	} else {
		eye_index = 1.0;
		eye_u = (in_uv.x - 0.5) / 0.5;
	}

	// Compute tile position for this eye
	float col = mod(eye_index, tile.tile_columns);
	float row = floor(eye_index / tile.tile_columns);

	// No crop — sample full tile
	float atlas_u = (eye_u + col) * tile.inv_tile_columns;
	float atlas_v = (in_uv.y + row) * tile.inv_tile_rows;

	out_color = texture(atlas_tex, vec2(atlas_u, atlas_v));
}
