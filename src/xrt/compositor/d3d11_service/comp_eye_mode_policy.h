// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Which eye-tracking mode (MANAGED / MANUAL) the D3D11 service's
 *         shared panel display processor should carry, and whether the vendor
 *         has to be told. A pure function, so ctest can pin the rule without a
 *         device, a DP or a service.
 *
 * In process every session owns its DP. In the service one panel DP is handed
 * from client to client, so xrRequestEyeTrackingModeDXR is a per-client WISH
 * and the panel carries the wish of the client holding it on the direct path;
 * everyone else (nobody presented, a presenter the panel DP does not weave, a
 * workspace controller's compose) gets the system default. A client that asked
 * for MANUAL therefore never leaves the DP in MANUAL for whoever comes next.
 *
 * comp_d3d11_service.cpp (pipeline_eye_mode_sync) fills the state under
 * render_mutex and acts on the result.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_eye_mode_state
{
	//! The panel holder on the direct path exists and the panel DP weaves it
	//! (APP_HWND / SERVICE_WINDOW presenter). False under a workspace
	//! controller's compose, with nobody presented, or for a present-owner /
	//! client-texture presenter.
	bool holder_woven_by_panel_dp;
	//! That holder's wish (0 MANAGED, 1 MANUAL; -1 never asked).
	int32_t holder_wish;
	//! The system's default_eye_tracking_mode.
	uint32_t default_mode;
	//! What the live panel DP was last told, plus one (0 = unknown: a fresh DP
	//! on the plug-in's own default).
	uint32_t told_plus1;
	//! Some DP was told a non-default mode and none has been told the default
	//! since (the vendor's policy can outlive one DP instance).
	bool nondefault_told;
};

struct comp_eye_mode_result
{
	//! The mode the panel DP should carry.
	uint32_t mode;
	//! The mode comes from the holder's request (false: the system default).
	bool from_holder;
	//! Call the DP's set_eye_tracking_mode with @ref mode.
	bool tell_dp;
};

static inline struct comp_eye_mode_result
comp_eye_mode_decide(const struct comp_eye_mode_state *s)
{
	struct comp_eye_mode_result r;
	r.mode = s->default_mode;
	r.from_holder = false;
	r.tell_dp = false;
	if (s->holder_woven_by_panel_dp && s->holder_wish >= 0) {
		r.mode = (uint32_t)s->holder_wish;
		r.from_holder = true;
	}
	if (s->told_plus1 == r.mode + 1u) {
		return r; // already carried
	}
	// A fresh DP on its own default, and nothing else was ever asked for:
	// leave the vendor alone, exactly as before the service applied modes.
	if (s->told_plus1 == 0 && r.mode == s->default_mode && !s->nondefault_told) {
		return r;
	}
	r.tell_dp = true;
	return r;
}

#ifdef __cplusplus
}
#endif
