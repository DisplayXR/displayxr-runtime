// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

// #817 — 1-pixel-period column interlace: a phase-sensitive proxy for a
// lenticular weave.
//
// Every other sim_display mode is a fullscreen triangle whose result is
// "correct" at any target size, at any position on the panel, and after any
// resample — which is exactly why a green sim run says nothing about the two
// properties a real weaver depends on. This one picks the view per PANEL
// column, so both failures become visible: a resample smears the alternating
// columns into grey/moire, and a wrong origin flips which eye lands on the
// even columns.
//
// The panel column is `gl_FragCoord.x + phase_px`, where `phase_px` is the
// weave target's panel-relative X origin (`canvas_offset_x`). Both views are
// sampled at the SAME normalized UV, so the output stays position-preserving
// (like anaglyph, unlike SBS) and is safe under a zone mask.

#version 450

layout(binding = 0) uniform sampler2D atlas_tex;

layout(push_constant) uniform TileParams {
	float inv_tile_columns;  // 1.0 / tile_columns
	float inv_tile_rows;     // 1.0 / tile_rows
	float tile_columns;
	float tile_rows;
	float phase_px;          // #817: canvas_offset_x — the interlace phase
	float period_px;         // #817: SIM_DISPLAY_INTERLACE_PERIOD (default 1)
	float pad0;
	float pad1;
} tile;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	// Which stripe of the panel this fragment falls in. gl_FragCoord.x is
	// target-relative; adding the target's panel origin makes the pattern
	// absolute, so moving the window shifts the phase exactly as a real
	// lenticular would see it.
	float period = max(tile.period_px, 1.0);
	float panel_col = floor(gl_FragCoord.x) + tile.phase_px;
	float eye_index = mod(floor(panel_col / period), 2.0);

	// Same tile addressing as anaglyph.frag: view N at (col,row) in the atlas,
	// sampled at this fragment's own UV.
	float col = mod(eye_index, tile.tile_columns);
	float row = floor(eye_index / tile.tile_columns);
	vec2 uv = vec2((in_uv.x + col) * tile.inv_tile_columns,
	               (in_uv.y + row) * tile.inv_tile_rows);

	out_color = texture(atlas_tex, uv);
}
