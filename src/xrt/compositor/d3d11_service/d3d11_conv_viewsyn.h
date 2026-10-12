// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reference view synthesis (ADR-053 S5) on D3D11: image + depth ->
 *         N views at explicit viewpoints. Port of the browser's live-dibr.js.
 *
 * The contract and the math live in util/u_conv_viewsyn.h; this is the GPU
 * half. Passes, in order, on the CALLER's context (the stage owns no thread,
 * takes no lock — never the service's render mutex — and logs nothing per
 * frame):
 *
 *  1. prep (depth res): raw sample -> decoded -> nearness -> normalised u;
 *  2. optional foreground dilation (separable max, `dilate`);
 *  3. snap cores (separable min / max, radius from `snap_core`);
 *  4. guide (colour box-filtered to depth res);
 *  5. joint-bilateral upsample + edge snap to colour res (long side <= 1280);
 *  6. optional max filter after the upsample (`dilate_hi`);
 *  7. one draw per view: ray march + disocclusion fill into the target.
 *
 * Passes 1-6 re-run only when an input or a parameter they read changed
 * (live-dibr.js `upDirty`); pass 7 runs every call.
 *
 * Inputs: a colour SRV (UNORM, NOT an sRGB view, for parity with the
 * browser, whose filters see the encoded values) and a single-channel depth
 * SRV whose `.r` is the raw sample the depth descriptor decodes.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include "util/u_conv_viewsyn.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <stdint.h>

struct d3d11_conv_viewsyn;

//! Where the N views go.
struct d3d11_conv_viewsyn_target
{
	ID3D11Texture2D *texture; //!< BIND_RENDER_TARGET
	DXGI_FORMAT rtv_format;   //!< DXGI_FORMAT_UNKNOWN = the texture's own (must be typed then)
	uint32_t layout;          //!< U_CONV_VIEWSYN_LAYOUT_ROW (width / N per view) or _ARRAY (slice i)
};

//! One synthesis' inputs.
struct d3d11_conv_viewsyn_inputs
{
	ID3D11ShaderResourceView *color; //!< row 0 = image top
	uint32_t color_width, color_height;
	ID3D11ShaderResourceView *depth; //!< `.r` = raw sample, row 0 = image top
	uint32_t depth_width, depth_height;
	//! True when the colour or depth CONTENT changed since the last call (a
	//! new frame / new depth). Resource, size and parameter changes are
	//! detected internally.
	bool new_content;
};

/*!
 * Compile the shaders and create the fixed state on @p device.
 * @return nullptr on failure (logged once).
 */
struct d3d11_conv_viewsyn *
d3d11_conv_viewsyn_create(ID3D11Device *device);

void
d3d11_conv_viewsyn_destroy(struct d3d11_conv_viewsyn **vs_ptr);

/*!
 * Render @p params->view_count views into @p target on @p ctx (a context of
 * the device the stage was created on). Leaves no SRV / RTV of its own bound.
 * @return false on invalid geometry / inputs / target, or a resource failure.
 */
bool
d3d11_conv_viewsyn_render(struct d3d11_conv_viewsyn *vs,
                          ID3D11DeviceContext *ctx,
                          const struct d3d11_conv_viewsyn_inputs *in,
                          const struct u_conv_viewsyn_params *params,
                          const struct d3d11_conv_viewsyn_target *target);

/*! Diagnostics: how many times passes 1-6 ran (live-dibr.js `stats.upsamples`). */
uint64_t
d3d11_conv_viewsyn_prepass_count(const struct d3d11_conv_viewsyn *vs);
