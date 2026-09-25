// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's FAKE photo → Gaussian-splat output (SIM_DISPLAY_FAKE_LIFT,
 *         XR_DXR_lift GAUSSIANS mode, ADR-042).
 *
 * Not a model: a tiny, VALID reference-3DGS binary PLY — two layers of splats
 * (a front layer coloured from the photo on a grid, a darker back layer behind
 * it) — so the blob path (lift thread → IPC varlen → xrAcquireLiftBlobDXR →
 * a splat viewer) can be exercised end to end without vendor hardware.
 * Platform-neutral so its format is pinned host-side (tests_lift_mailbox.cpp).
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Splats per layer along x / y.
#define SIM_FAKE_PLY_GRID_X 16
#define SIM_FAKE_PLY_GRID_Y 12
//! Two layers.
#define SIM_FAKE_PLY_SPLATS (2 * SIM_FAKE_PLY_GRID_X * SIM_FAKE_PLY_GRID_Y)
//! Floats per splat: x y z nx ny nz f_dc_0..2 opacity scale_0..2 rot_0..3.
#define SIM_FAKE_PLY_FLOATS_PER_SPLAT 17

/*!
 * Write the fake PLY for an RGBA8 image into @p dst (capacity @p cap).
 * Returns the number of bytes the PLY needs; writes nothing when @p dst is
 * NULL or @p cap is smaller than that. @p rgba may be NULL (flat grey).
 */
size_t
sim_fake_ply_write(uint8_t *dst, size_t cap, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t row_pitch);

#ifdef __cplusplus
}
#endif
