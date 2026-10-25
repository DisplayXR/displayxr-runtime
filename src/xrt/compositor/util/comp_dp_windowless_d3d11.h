// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Make a WINDOWLESS D3D11 display processor for one screen, kept only
 *         when it phases from `set_present_origin` (ADR-060 R2's contract).
 *
 * One implementation of the rule, shared by the two in-process callers that
 * weave a window the runtime does not hold:
 *
 *  - the native D3D11 tier's bound texture session (ADR-060 R1, §6);
 *  - the D3D11 hub's client-presents presenter strategy (#2370), which weaves
 *    a texture-class session into the CALLER's shared texture.
 *
 * The rule (ADR-060 decision 4): ask only when the plug-in HINTS windowless
 * phase for the screen (@ref XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE — so no
 * vendor weaver is brought up just to be destroyed), create through the
 * screen's `create_dp_d3d11_for_screen` with window NULL, and keep the
 * instance only when IT declares @ref XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN
 * (the authority; a vendor SDK without external routing yields a weaver that
 * ignores the origin).
 *
 * Header-only and C-compatible. Logs nothing: the reason is returned as a
 * closed token so each caller words its own lifecycle WARN.
 *
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_plugin.h"

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @name Why no windowless DP was made — closed tokens
 * @{
 */
//! No registry entry, no per-screen D3D11 factory, or no screen rect to phase against.
#define COMP_DP_WINDOWLESS_NO_FACTORY "no_per_screen_factory"
//! The plug-in did not hint windowless phase for this screen.
#define COMP_DP_WINDOWLESS_NO_HINT "no_windowless_hint"
//! The per-screen factory refused (or returned no DP).
#define COMP_DP_WINDOWLESS_REFUSED "factory_refused"
//! The DP was made but does not declare PHASE_FROM_PRESENT_ORIGIN; it was destroyed.
#define COMP_DP_WINDOWLESS_NO_FLAG "no_phase_from_present_origin"
/*! @} */

/*!
 * Make the windowless DP for registry entry @p e on @p device / @p context.
 *
 * @param e            The screen's registry entry (`xrt_dp_registry_entry`).
 * @param device       `ID3D11Device *` the DP weaves on.
 * @param context      `ID3D11DeviceContext *` the DP weaves on.
 * @param out_dp       Receives the DP; NULL on every failure.
 * @param out_reason   Receives a `COMP_DP_WINDOWLESS_*` token on failure, NULL on success.
 * @param out_ret      Optional: the factory's return value (meaningful for REFUSED).
 *
 * @return true when @p out_dp is a windowless DP that phases from the present origin.
 */
static inline bool
comp_dp_make_windowless_d3d11(const struct xrt_dp_registry_entry *e,
                              void *device,
                              void *context,
                              struct xrt_display_processor_d3d11 **out_dp,
                              const char **out_reason,
                              xrt_result_t *out_ret)
{
	*out_dp = NULL;
	*out_reason = NULL;
	if (out_ret != NULL) {
		*out_ret = XRT_SUCCESS;
	}
	const struct xrt_plugin_iface *iface = e != NULL ? (const struct xrt_plugin_iface *)e->owning_iface : NULL;
	if (e == NULL || !xrt_plugin_iface_has_create_dp_d3d11_for_screen(iface) || e->pixel_width == 0 ||
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

	struct xrt_display_processor_d3d11 *dp = NULL;
	xrt_result_t ret = iface->create_dp_d3d11_for_screen((struct xrt_plugin_instance *)e->owning_instance, device,
	                                                     context, NULL, &b, &dp);
	if (out_ret != NULL) {
		*out_ret = ret;
	}
	if (ret != XRT_SUCCESS || dp == NULL) {
		if (dp != NULL) {
			xrt_display_processor_d3d11_destroy(&dp);
		}
		*out_reason = COMP_DP_WINDOWLESS_REFUSED;
		return false;
	}
	if (!xrt_display_processor_d3d11_phases_from_present_origin(dp)) {
		xrt_display_processor_d3d11_destroy(&dp);
		*out_reason = COMP_DP_WINDOWLESS_NO_FLAG;
		return false;
	}
	*out_dp = dp;
	return true;
}

#ifdef __cplusplus
}
#endif
