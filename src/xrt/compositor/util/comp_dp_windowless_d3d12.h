// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Make a WINDOWLESS D3D12 display processor for one screen, kept only
 *         when it phases from `set_present_origin` (ADR-060's contract).
 *
 * The D3D12 twin of comp_dp_windowless_d3d11.h, for the native D3D12 tier's
 * bound texture session (ADR-060 R1 on D3D12, #2382). Same rule, same closed
 * reason tokens (comp_dp_windowless_reason.h):
 *
 * ask only when the plug-in HINTS windowless phase for the screen
 * (@ref XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE), create through the screen's
 * `create_dp_d3d12_for_screen` with window NULL — the factory the D3D12
 * segment manager already makes its windowless per-screen DPs with
 * (comp_d3d12_segments.cpp) — and keep the instance only when IT declares
 * @ref XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN (the authority). The hint
 * is per plug-in, not per API: a plug-in whose D3D11 DP phases from the origin
 * but whose D3D12 DP does not is asked, answers without the flag, and its DP
 * is destroyed (the caller falls back to the window anchor).
 *
 * Header-only and C-compatible. Logs nothing: the reason is returned as a
 * closed token so the caller words its own lifecycle WARN.
 *
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_processor_d3d12.h"
#include "xrt/xrt_plugin.h"

#include "util/comp_dp_windowless_reason.h"

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Make the windowless DP for registry entry @p e on @p device / @p queue.
 *
 * @param e            The screen's registry entry (`xrt_dp_registry_entry`).
 * @param device       `ID3D12Device *` the DP weaves on.
 * @param queue        `ID3D12CommandQueue *` the DP's weave is submitted on.
 * @param out_dp       Receives the DP; NULL on every failure.
 * @param out_reason   Receives a `COMP_DP_WINDOWLESS_*` token on failure, NULL on success.
 * @param out_ret      Optional: the factory's return value (meaningful for REFUSED).
 *
 * @return true when @p out_dp is a windowless DP that phases from the present origin.
 */
static inline bool
comp_dp_make_windowless_d3d12(const struct xrt_dp_registry_entry *e,
                              void *device,
                              void *queue,
                              struct xrt_display_processor_d3d12 **out_dp,
                              const char **out_reason,
                              xrt_result_t *out_ret)
{
	*out_dp = NULL;
	*out_reason = NULL;
	if (out_ret != NULL) {
		*out_ret = XRT_SUCCESS;
	}
	const struct xrt_plugin_iface *iface = e != NULL ? (const struct xrt_plugin_iface *)e->owning_iface : NULL;
	if (e == NULL || !xrt_plugin_iface_has_create_dp_d3d12_for_screen(iface) || e->pixel_width == 0 ||
	    e->pixel_height == 0) {
		*out_reason = COMP_DP_WINDOWLESS_NO_FACTORY;
		return false;
	}
	if ((e->claim_apis & XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE) == 0) {
		*out_reason = COMP_DP_WINDOWLESS_NO_HINT;
		return false;
	}

	struct xrt_screen_binding b;
	memset(&b, 0, sizeof(b));
	b.struct_size = (uint32_t)sizeof(b);
	b.monitor_id = e->monitor_id;
	b.desktop_left = e->screen_left;
	b.desktop_top = e->screen_top;
	b.desktop_width = e->pixel_width;
	b.desktop_height = e->pixel_height;
	snprintf(b.serial, sizeof(b.serial), "%s", e->serial);

	struct xrt_display_processor_d3d12 *dp = NULL;
	xrt_result_t ret = iface->create_dp_d3d12_for_screen((struct xrt_plugin_instance *)e->owning_instance, device,
	                                                     queue, NULL, &b, &dp);
	if (out_ret != NULL) {
		*out_ret = ret;
	}
	if (ret != XRT_SUCCESS || dp == NULL) {
		if (dp != NULL) {
			xrt_display_processor_d3d12_destroy(&dp);
		}
		*out_reason = COMP_DP_WINDOWLESS_REFUSED;
		return false;
	}
	if (!xrt_display_processor_d3d12_phases_from_present_origin(dp)) {
		xrt_display_processor_d3d12_destroy(&dp);
		*out_reason = COMP_DP_WINDOWLESS_NO_FLAG;
		return false;
	}
	*out_dp = dp;
	return true;
}

#ifdef __cplusplus
}
#endif
