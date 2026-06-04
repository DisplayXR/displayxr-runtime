// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GL->[0,1] clip-depth conversion for display3d projections.
 *
 * display3d_compute_projection() emits an OpenGL-convention projection matrix
 * (clip-space z in [-1,1]). D3D11/D3D12/Vulkan/Metal rasterizers clip at [0,1],
 * so the depth row must be remapped (z' = 0.5*z + 0.5*w) before the matrix is
 * handed to those APIs — otherwise everything nearer than the mid-range
 * crossover is wrongly near-clipped. This is harmless with a tiny near plane
 * (old test apps passed 0.01) but is exposed once the ZDP-anchored near
 * (near = ez - vH) sits close to the content (#396 W1).
 *
 * Apply ONLY to the 3D display3d output (per-view projection_matrix), never to
 * runtime/mono projections (xr.projMatrices), which already carry the correct
 * convention. The Metal test apps have a local equivalent
 * (convert_projection_gl_to_metal) for the same reason.
 *
 * In-place on a column-major float[16] (the layout display3d emits).
 */

#pragma once

static inline void
convert_projection_gl_to_zero_to_one(float m[16])
{
	m[2]  = 0.5f * m[2]  + 0.5f * m[3];
	m[6]  = 0.5f * m[6]  + 0.5f * m[7];
	m[10] = 0.5f * m[10] + 0.5f * m[11];
	m[14] = 0.5f * m[14] + 0.5f * m[15];
}
