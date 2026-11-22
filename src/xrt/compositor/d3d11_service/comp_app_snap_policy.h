// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window-drag phase snap for an IPC handle client — may the D3D11
 *         service snap this client's window right now? A pure function, so
 *         ctest can pin the rule without a device, a DP or a service.
 *
 * An ordinary handle app forced onto the IPC path is woven in the service and,
 * on the direct single-client pipeline, presented into the app's OWN window
 * (PRESENTER_APP_HWND). In process, the vendor weaver bound to that window
 * phase-snaps its drags itself; the service cannot subclass a window owned by
 * another process, so the app's runtime does it (the drag-snap subclass in the
 * app) and asks the service for the snap over `compositor_snap_window_rect`.
 *
 * The service answers only when the display processor it asks is the one that
 * weaves THIS client's window into THIS client's window. Every other case says
 * "not snapped" and the window moves freely, which is the behaviour before the
 * call existed. comp_d3d11_service.cpp fills the state under render_mutex and
 * acts on the verdict.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum comp_app_snap_verdict
{
	//! Ask the display processor that weaves the client's window.
	COMP_APP_SNAP_ASK_DP = 0,
	//! Legacy standalone path (DXR_LEGACY_STANDALONE=1): not handled.
	COMP_APP_SNAP_NO_PIPELINE,
	//! Not a per-client swap chain on the app's own window (service window,
	//! client texture, present-owner, none).
	COMP_APP_SNAP_NOT_APP_HWND,
	//! The client does not hold the panel: another client is presented, or a
	//! workspace controller composes the panel (and is not handing this
	//! client the direct path through a foreground override).
	COMP_APP_SNAP_NOT_PRESENTED,
	//! The client's window is closed or minimized.
	COMP_APP_SNAP_WINDOW_GONE,
	//! No panel display processor (device removed, no vendor plug-in, or a
	//! rebind in progress).
	COMP_APP_SNAP_NO_DP,
	//! The panel display processor is bound to some other window (a rebind
	//! is pending, or it was left on the service window).
	COMP_APP_SNAP_DP_ELSEWHERE,
};

struct comp_app_snap_state
{
	//! pipeline_always_on(sys).
	bool pipeline_always_on;
	//! The client's presenter kind is PRESENTER_APP_HWND with a live HWND.
	bool presenter_is_app_hwnd;
	//! sys->workspace_mode (a controller is attached).
	bool workspace_mode;
	//! The client's slot in the multi-compositor (-1: none).
	int32_t client_slot;
	//! mc->focused_slot (the default policy's presenter).
	int32_t focused_slot;
	//! mc->foreground_override_slot (-1: no override).
	int32_t foreground_override_slot;
	//! The client's window was closed or minimized.
	bool window_gone;
	//! panel_dp() resolves (the device is live and a DP is bound).
	bool have_panel_dp;
	//! The panel DP is bound to the client's own HWND.
	bool panel_dp_on_client_window;
};

static inline const char *
comp_app_snap_verdict_str(enum comp_app_snap_verdict v)
{
	switch (v) {
	case COMP_APP_SNAP_ASK_DP: return "ask_dp";
	case COMP_APP_SNAP_NO_PIPELINE: return "no_pipeline";
	case COMP_APP_SNAP_NOT_APP_HWND: return "not_app_hwnd";
	case COMP_APP_SNAP_NOT_PRESENTED: return "not_presented";
	case COMP_APP_SNAP_WINDOW_GONE: return "window_gone";
	case COMP_APP_SNAP_NO_DP: return "no_dp";
	case COMP_APP_SNAP_DP_ELSEWHERE: return "dp_elsewhere";
	default: return "?";
	}
}

/*!
 * The verdict, in the order the service evaluates the inputs.
 */
static inline enum comp_app_snap_verdict
comp_app_snap_decide(const struct comp_app_snap_state *s)
{
	if (s == 0 || !s->pipeline_always_on) {
		return COMP_APP_SNAP_NO_PIPELINE;
	}
	if (!s->presenter_is_app_hwnd) {
		return COMP_APP_SNAP_NOT_APP_HWND;
	}
	// Who the direct path presents: under a controller only a foreground
	// override hands one client the direct path; otherwise the panel is
	// composed and no client window is woven into. Without a controller the
	// default policy presents the focused slot.
	const int32_t presented = s->workspace_mode ? s->foreground_override_slot : s->focused_slot;
	if (s->client_slot < 0 || presented != s->client_slot) {
		return COMP_APP_SNAP_NOT_PRESENTED;
	}
	if (s->window_gone) {
		return COMP_APP_SNAP_WINDOW_GONE;
	}
	if (!s->have_panel_dp) {
		return COMP_APP_SNAP_NO_DP;
	}
	if (!s->panel_dp_on_client_window) {
		return COMP_APP_SNAP_DP_ELSEWHERE;
	}
	return COMP_APP_SNAP_ASK_DP;
}

#ifdef __cplusplus
}
#endif
