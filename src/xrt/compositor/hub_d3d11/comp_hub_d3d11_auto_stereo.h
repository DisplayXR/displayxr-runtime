// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Legacy automatic 2D + automatic re-convergence on the D3D11 hub:
 *         the GPU probe, the worker and the pre-DP shift / one-view crop.
 * @ingroup comp_hub_d3d11
 *
 * The policy is util/u_stereo_auto.h (pure, host-tested). This unit is the
 * D3D11 half, written against nothing but a device + immediate context, so any
 * tier that weaves through a D3D11 context can adopt it:
 *
 *  - **The probe** (≤ 10 Hz, app frames only): the stereo atlas's content box
 *    is copied into a mip-chained scratch, `GenerateMips`, and ONE small mip
 *    (each eye ≥ 256 and < 512 px wide) is copied into a staging texture. The
 *    staging texture is read on a LATER weave with `D3D11_MAP_FLAG_DO_NOT_WAIT`:
 *    still drawing = try again next weave. Nothing on the frame path ever
 *    waits for the GPU. A two-deep staging ring keeps one probe in flight
 *    while the previous one is read.
 *  - **The worker**: one thread turns the read-back bytes into the two eye
 *    grids and runs @ref u_stereo_auto_probe (a few ms of CPU at 10 Hz, never
 *    on the app thread). Results are published under a mutex.
 *  - **The output**, before the display processor (crop-before-DP; the
 *    compositor never weaves): while automatic 2D holds, tile 0 alone (one
 *    view) — the hub asks for hardware 2D itself, see comp_hub_d3d11.cpp;
 *    otherwise, with a non-zero re-convergence shift, both tiles copied into
 *    a scratch with the left eye moved by −Δ/2 and the right by +Δ/2, black
 *    where nothing was. No shift and not 2D = the atlas as it came, no copy.
 *
 * Only a 2×1 stereo atlas is touched. Everything else idles.
 */

#pragma once

#include "util/u_camera_profile.h"

#include <stdbool.h>
#include <stdint.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

struct comp_hub_auto_stereo;

//! One weave's DP input: replaced by @ref comp_hub_auto_stereo_frame when it acts.
struct comp_hub_auto_stereo_view
{
	ID3D11ShaderResourceView *srv;
	uint32_t cols, rows;
	uint32_t view_w, view_h;
};

//! The last probe, for the edge WARN.
struct comp_hub_auto_stereo_stats
{
	double unshifted;       //!< eye difference at shift 0 (luma 0..255)
	double best;            //!< after the best shift
	double best_shift_frac; //!< that shift, fraction of the eye width (the right eye sampled at x - shift)
	float shift;            //!< smoothed re-convergence shift, fraction of the eye width
	uint32_t blocks;        //!< confident disparity blocks
	uint64_t probes;
	uint32_t cuts;
};

//! Starts the worker. NULL when @p cfg enables nothing.
struct comp_hub_auto_stereo *
comp_hub_auto_stereo_create(const struct u_legacy_stereo_auto *cfg);

//! Stops the worker and frees the GPU objects. Call before the device goes.
void
comp_hub_auto_stereo_destroy(struct comp_hub_auto_stereo **as_ptr);

/*!
 * Is a 2D/3D edge owed? @p session_3d = the session's own wish, @p cols /
 * @p rows = this weave's atlas grid. Ineligible (session 2D, not 2×1) always
 * wants 3D, so a held 2D is released at once. On true the caller enacts it
 * and calls @ref comp_hub_auto_stereo_lens_commit.
 */
bool
comp_hub_auto_stereo_lens_edge(
    struct comp_hub_auto_stereo *as, bool session_3d, uint32_t cols, uint32_t rows, bool *out_flat);

void
comp_hub_auto_stereo_lens_commit(struct comp_hub_auto_stereo *as, bool flat);

/*!
 * Per weave, on the context that then calls process_atlas: poll the readback,
 * issue a probe when due (not on a repaint), and replace @p io with the
 * one-view crop or the shifted copy when either applies. @p seq names the
 * frame the atlas holds (0 = unknown): a repaint of the same frame and shift
 * reuses the output without copying again. Returns true when @p io changed.
 */
bool
comp_hub_auto_stereo_frame(struct comp_hub_auto_stereo *as,
                           struct ID3D11Device *dev,
                           struct ID3D11DeviceContext *ctx,
                           bool is_repaint,
                           uint64_t seq,
                           bool session_3d,
                           struct comp_hub_auto_stereo_view *io);

void
comp_hub_auto_stereo_get_stats(struct comp_hub_auto_stereo *as, struct comp_hub_auto_stereo_stats *out);
