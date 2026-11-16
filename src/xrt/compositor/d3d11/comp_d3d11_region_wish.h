// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-062 D5 (tier 4): the published zone wish limited to what can be
 *         seen, on a D3D11 device.
 *
 * On a display processor whose zone grid is larger than 1x1 the runtime
 * publishes the visible part of every 3D region: a wish mask (zones, sticky)
 * with its covered pixels cleared, or for an app with no wish of its own an
 * implicit one (its visible client region minus its Local2D rects), or the
 * empty wish while the window is hidden or the session is in 2D mode. The
 * geometry is @ref u_wv_wish_plan_build (aux_util, unit-tested); this unit only
 * executes a plan into an R8 mask: a copy of the source (or a clear to 0), then
 * ClearView 1 over the ON rects and 0 over the OFF rects. It re-rasters only
 * when the plan's content signature changes, and the publish generation it
 * hands back bumps only then (the #2677 rule).
 *
 * The runtime decides WHAT is 3D; how the panel's cells follow is the display
 * processor's business (ADR-007, ADR-027 Decision 6). Nothing here runs on a
 * 1x1 grid: callers gate on @ref u_wv_region_mask_applies.
 *
 * @ingroup comp_d3d11
 */

#pragma once

#include "util/u_window_visibility.h"

#include <d3d11_1.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One publisher's tier-4 raster. Zero-init; owned by the compositor, used under its frame lock.
struct comp_d3d11_region_wish
{
	ID3D11Device *device; //!< the device the raster lives on (not referenced)
	ID3D11Texture2D *tex;
	ID3D11RenderTargetView *rtv;
	ID3D11ShaderResourceView *srv;
	uint32_t w;
	uint32_t h;
	//! The publish generation (bumps on a content change only).
	struct u_wv_wish_seq seq;
	//! A raster matching seq's signature exists.
	bool rastered;
	//! The last zone verdict string logged (change-gated WARN), e.g. "cut 3 off".
	char last_log[64];
};

/*!
 * Raster @p in on @p device / @p context and return the mask to publish, with
 * its generation in @p out_seq. @p source is the CUT source (an R8_UNORM
 * texture view of @p in->mask_w x @p in->mask_h on the same device), ignored
 * otherwise. A CUT whose source cannot be copied (another format or size)
 * publishes the source uncut: the 3D side.
 *
 * @p who labels the one WARN per content change (a publish generation), never
 * per frame. Returns NULL on a resource failure (the caller keeps its
 * previous publish).
 */
ID3D11ShaderResourceView *
comp_d3d11_region_wish_resolve(struct comp_d3d11_region_wish *rw,
                               ID3D11Device *device,
                               ID3D11DeviceContext *context,
                               const struct u_wv_wish_input *in,
                               ID3D11ShaderResourceView *source,
                               const char *who,
                               uint64_t *out_seq);

//! Release the raster's resources (the generation is kept: it must stay monotonic).
void
comp_d3d11_region_wish_release(struct comp_d3d11_region_wish *rw);

#ifdef __cplusplus
}
#endif
