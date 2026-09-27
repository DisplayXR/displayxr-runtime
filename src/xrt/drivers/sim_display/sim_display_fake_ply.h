// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's FAKE photo → Gaussian-splat output (SIM_DISPLAY_FAKE_LIFT,
 *         XR_DXR_lift GAUSSIANS mode, ADR-042).
 *
 * Not a model, but a VISUALLY MEANINGFUL, valid reference-3DGS binary PLY, so the
 * blob path (lift thread → IPC varlen → xrAcquireLiftBlobDXR → a splat viewer)
 * can be exercised end to end without vendor hardware and a black view is
 * unambiguous. The scene is the INPUT FRAME, camera-consistent:
 *
 *  - Frame: OpenCV camera axes — camera at the origin looking down +z, +x
 *    right, +y DOWN — the convention of photo → splat lifters (SHARP) and of
 *    the web SDK's splat rig for a PLY whose meta names no `axes`.
 *  - Front layer: SIM_FAKE_PLY_GRID_X x SIM_FAKE_PLY_GRID_Y splats, one per grid
 *    cell, coloured with the sampled input pixel, back-projected through the
 *    pinhole (fx = fy = @p focal_px, principal point = the frame centre) at
 *    Z = SIM_FAKE_PLY_Z_FRONT: X = (u·w − w/2)·Z/f, Y = (v·h − h/2)·Z/f. Every
 *    splat projects back onto its own pixel, so the capture view is the photo.
 *  - Back ("hidden") layer: the same rays at Z = SIM_FAKE_PLY_Z_BACK, darker —
 *    occluded from the capture viewpoint, revealed as the view moves.
 *  - Opacity logit 2.0 (the PLY stores the pre-sigmoid value, ~0.88);
 *    scale_0..2 = ln(0.6 · (w/N) · Z/f), N = splats across, so neighbours
 *    overlap; rotation identity (w x y z = 1 0 0 0); f_dc = (c − 0.5) / SH_C0.
 *  - A PLY header comment carries the capture metadata a consumer needs to
 *    frame it (`comment dxr-lift-meta {"focalPx":…,"w":…,"h":…,"pivotZ":1.8,
 *    "axes":"opencv"}`); comments are ignored by PLY readers that do not look.
 *
 * Platform-neutral and shared by the D3D11 and the Vulkan / Android fakes, so
 * its format is pinned host-side (tests_lift_mailbox.cpp).
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Splats per layer along x / y (16:9 grid; a non-16:9 frame gets anisotropic cells).
#define SIM_FAKE_PLY_GRID_X 96
#define SIM_FAKE_PLY_GRID_Y 54
//! Layer depths, metres (OpenCV +z forward). The front one is the pivot.
#define SIM_FAKE_PLY_Z_FRONT 1.8f
#define SIM_FAKE_PLY_Z_BACK 2.6f
//! Horizontal field of view assumed when no focal length is given, degrees.
#define SIM_FAKE_PLY_DEFAULT_HFOV_DEG 60.0f
//! Two layers.
#define SIM_FAKE_PLY_SPLATS (2 * SIM_FAKE_PLY_GRID_X * SIM_FAKE_PLY_GRID_Y)
//! Floats per splat: x y z nx ny nz f_dc_0..2 opacity scale_0..2 rot_0..3.
#define SIM_FAKE_PLY_FLOATS_PER_SPLAT 17

/*!
 * Write the fake PLY for an RGBA8 image into @p dst (capacity @p cap).
 * Returns the number of bytes the PLY needs; writes nothing when @p dst is
 * NULL or @p cap is smaller than that. @p rgba may be NULL (flat grey).
 * @p focal_px is the input's focal length in pixels (xrt_dp_lift_params::focal_px,
 * the app's focalPx); <= 0 = SIM_FAKE_PLY_DEFAULT_HFOV_DEG across @p w.
 */
size_t
sim_fake_ply_write(
    uint8_t *dst, size_t cap, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t row_pitch, float focal_px);

//! The focal length sim_fake_ply_write uses for @p focal_px / @p w.
float
sim_fake_ply_focal(float focal_px, uint32_t w);

#ifdef __cplusplus
}
#endif
