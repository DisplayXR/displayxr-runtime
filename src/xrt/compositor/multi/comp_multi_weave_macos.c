// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave on the macOS service path (#759) — the platform front
 *         end over the vk / metal backends.
 * @author David Fattal
 * @ingroup comp_multi
 *
 * A window-bound synchronous weave service for present-owners (a browser GPU
 * process, a CEF host): the caller owns its NSWindow and presents itself, but
 * hands the runtime pre-weave side-by-side stereo pixels + window-relative
 * rect(s) and composites back a weaved shared texture. The caller NEVER weaves
 * (ADR-007 / ADR-019).
 *
 * macOS platform mapping (vs the Windows/D3D11 original in
 * comp_d3d11_service.cpp):
 *
 *  - Input texture   = a caller-allocated IOSurface. It crosses the IPC as a
 *    global IOSurfaceID (ipc_message_channel_unix.c) and arrives here as a
 *    retained IOSurfaceRef. This file owns that ref (an identity cache keyed by
 *    IOSurfaceID); the backend wraps it as its own texture.
 *  - Output texture  = a service-allocated IOSurface, exported back to the
 *    caller as an IOSurfaceRef.
 *  - Input-ready sync: there is no keyed mutex on macOS. The contract is that
 *    the caller completes its GPU writes into the input IOSurface before
 *    calling xrWeaveSubmitDXR.
 *  - Completion sync: SYNCHRONOUS — the backend waits for the GPU before the
 *    IPC reply returns, so xrWeaveSubmitDXR returning IS the completion signal.
 *  - Output sizing: batch (v3) = the INPUT IOSurface dims (the v3 contract
 *    makes the input window-client-sized); legacy single rect = rect
 *    offset+extent; v6 = one content view, or the reported window when the
 *    DP does not tolerate a resample of its output (a lenticular lattice must
 *    reach the panel 1:1).
 *  - Window geometry (spec v7): `windowOriginOnScreen` is global CoreGraphics
 *    space in BACKING pixels (y down). The engine resolves the panel
 *    (displayId = CGDirectDisplayID, else the display containing the origin —
 *    ambiguous with mixed backing scales, so such setups must send displayId —
 *    else the main display) and feeds the DP `origin - panel origin` as its
 *    present origin before every weave (Metal backend).
 *
 * The GPU work lives behind comp_multi_weave_macos_backend.h:
 *  - vk    — MoltenVK + the plug-in's Vulkan DP (sim_display's shipping path,
 *            unchanged from the single-file engine);
 *  - metal — native Metal + the plug-in's Metal DP (the only family the Leia
 *            macOS plug-in exports).
 * DXR_WEAVE_MAC_BACKEND=auto|metal|vk picks it once per client (auto: metal
 * iff the plug-in has no Vulkan DP factory but has a Metal one).
 */

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_session.h"

#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_logging.h"

#include "comp_multi_private.h"
#include "comp_multi_weave_macos_backend.h"

#include "os/os_display_macos.h"

#ifdef XRT_OS_MACOS

#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <math.h>
#include <string.h>

DEBUG_GET_ONCE_OPTION(weave_mac_backend, "DXR_WEAVE_MAC_BACKEND", "auto")


/*
 *
 * Helpers.
 *
 */

//! Lazily create the per-client engine lock (multi_compositor is zero-alloced).
static void
weave_ensure_mutex(struct multi_compositor *mc)
{
	os_mutex_lock(&mc->msc->list_and_timing_lock);
	if (!mc->weave.mutex_initialized) {
		os_mutex_init(&mc->weave.mutex);
		mc->weave.mutex_initialized = true;
	}
	os_mutex_unlock(&mc->msc->list_and_timing_lock);
}

/*!
 * Pick the backend once per client. auto: metal iff the plug-in exports no
 * Vulkan DP factory but a Metal one (Leia), else vk — so sim_display (which
 * exports both) keeps the shipping Vulkan path byte for byte.
 */
static const struct comp_multi_weave_macos_backend *
weave_pick_backend(struct multi_compositor *mc)
{
	const struct xrt_system_compositor_info *info = &mc->msc->base.info;
	const bool have_vk = info->dp_factory_vk != NULL;
	const bool have_metal = info->dp_factory_metal != NULL;
	const char *opt = debug_get_option_weave_mac_backend();
	const struct comp_multi_weave_macos_backend *b = &comp_multi_weave_macos_backend_vk;
	const char *why = "auto";

	if (opt != NULL && strcmp(opt, "vk") == 0) {
		why = "DXR_WEAVE_MAC_BACKEND=vk";
	} else if (opt != NULL && strcmp(opt, "metal") == 0) {
#ifdef COMP_MULTI_WEAVE_HAVE_METAL
		b = &comp_multi_weave_macos_backend_metal;
		why = "DXR_WEAVE_MAC_BACKEND=metal";
#else
		why = "DXR_WEAVE_MAC_BACKEND=metal, but this build has no Metal backend";
#endif
	} else {
		if (opt != NULL && strcmp(opt, "auto") != 0) {
			U_LOG_W("weave(#759): DXR_WEAVE_MAC_BACKEND='%s' is not auto|metal|vk — using auto", opt);
		}
#ifdef COMP_MULTI_WEAVE_HAVE_METAL
		if (!have_vk && have_metal) {
			b = &comp_multi_weave_macos_backend_metal;
		}
#endif
	}

	U_LOG_W("weave(#759): macOS weave backend=%s (%s; plug-in DP factories: vk=%s metal=%s)", b->name, why,
	        have_vk ? "yes" : "no", have_metal ? "yes" : "no");
	return b;
}

/*!
 * One display's rect in global CoreGraphics BACKING pixels, from the shared
 * macOS monitor record (#1872, os_display_macos_fill_desktop_info): its rect
 * is top-down POINTS and its scale the current mode's backing pixels over
 * points, so backing = points x scale. One source for the weave's phase origin
 * and its per-window Kooima (comp_multi_weave_macos_window_on_panel).
 */
static bool
weave_display_backing_rect(
    CGDirectDisplayID d, int32_t *out_x, int32_t *out_y, uint32_t *out_w, uint32_t *out_h, double *out_scale)
{
	struct os_display_desktop_info info;
	if (!os_display_macos_fill_desktop_info((uint32_t)d, &info) || info.width == 0 || info.height == 0) {
		return false;
	}
	const double scale = info.scale > 0.0 ? info.scale : 1.0;
	*out_x = (int32_t)lround((double)info.left * scale);
	*out_y = (int32_t)lround((double)info.top * scale);
	*out_w = info.native_width > 0 ? info.native_width : (uint32_t)lround((double)info.width * scale);
	*out_h = info.native_height > 0 ? info.native_height : (uint32_t)lround((double)info.height * scale);
	*out_scale = scale;
	return true;
}

/*!
 * The panel a point in the weave geometry convention (global CoreGraphics
 * space in BACKING px, y down) lies on, and that panel's backing rect + scale.
 * Panel: @p display_id (a CGDirectDisplayID; -1 and 0 mean not reported), else
 * the active display containing the point, else the main display. Pure: the
 * caller logs. @p out_matches counts the containing displays (>1 = ambiguous,
 * mixed backing scales — the non-main one is picked).
 */
static CGDirectDisplayID
weave_resolve_panel(int32_t wx,
                    int32_t wy,
                    int32_t display_id,
                    int32_t *out_px,
                    int32_t *out_py,
                    uint32_t *out_pw,
                    uint32_t *out_ph,
                    double *out_scale,
                    const char **out_how,
                    bool *out_fallback,
                    uint32_t *out_matches)
{
	int32_t px = 0, py = 0;
	uint32_t pw = 0, ph = 0;
	double scale = 1.0;
	CGDirectDisplayID panel = 0;
	const char *how = NULL;
	bool fallback = false;
	uint32_t matches = 0;

	// displayId -1 (spec: unknown / single display) and 0 are "not reported".
	if (display_id != 0 && display_id != -1 &&
	    weave_display_backing_rect((CGDirectDisplayID)display_id, &px, &py, &pw, &ph, &scale)) {
		panel = (CGDirectDisplayID)display_id;
		how = "reported displayId";
	}
	if (panel == 0) {
		// Containment, per display in that display's OWN units: the origin is
		// backing px, so test origin / scale_d against the display's point
		// bounds. With mixed scales several displays can match (the backing-px
		// spaces of different-scale displays overlap) — prefer a non-main one
		// and ask the caller for displayId.
		uint32_t ids[OS_DISPLAY_DESKTOP_MAX_MONITORS];
		const uint32_t n = os_display_macos_list_displays(ids, OS_DISPLAY_DESKTOP_MAX_MONITORS);
		const CGDirectDisplayID main_id = CGMainDisplayID();
		{
			for (uint32_t i = 0; i < n; i++) {
				int32_t x = 0, y = 0;
				uint32_t w = 0, h = 0;
				double s = 1.0;
				if (!weave_display_backing_rect(ids[i], &x, &y, &w, &h, &s)) {
					continue;
				}
				// The display's point bounds, back from its backing rect.
				const CGRect b = CGRectMake((double)x / s, (double)y / s, (double)w / s, (double)h / s);
				const CGPoint pt = CGPointMake((double)wx / s, (double)wy / s);
				if (!CGRectContainsPoint(b, pt)) {
					continue;
				}
				matches++;
				if (panel == 0 || (panel == main_id && ids[i] != main_id)) {
					panel = ids[i];
					px = x, py = y, pw = w, ph = h, scale = s;
				}
			}
		}
		if (matches > 1) {
			how = "display containing the origin (AMBIGUOUS: several match, picked the non-main one)";
		} else if (matches == 1) {
			how = "display containing the origin";
		}
	}
	if (panel == 0) {
		panel = CGMainDisplayID();
		if (!weave_display_backing_rect(panel, &px, &py, &pw, &ph, &scale)) {
			px = py = 0;
			pw = ph = 0;
			scale = 1.0;
		}
		how = "main display (fallback)";
		fallback = true;
	}
	*out_px = px;
	*out_py = py;
	*out_pw = pw;
	*out_ph = ph;
	*out_scale = scale;
	*out_how = how;
	*out_fallback = fallback;
	*out_matches = matches;
	return panel;
}

/*!
 * Resolve the panel-relative present origin from the stored geometry — only
 * when it changed (geometry_dirty), the result is cached and re-sent to the DP
 * every submit. Panel: see weave_resolve_panel(). WARNs on the first resolve
 * and whenever the resolved panel or its scale changes; an origin-only change
 * (a drag) is INFO.
 */
static void
weave_resolve_present_origin_locked(struct multi_compositor *mc)
{
	if (!mc->weave.have_geometry || !mc->weave.geometry_dirty) {
		return;
	}
	mc->weave.geometry_dirty = false;

	const int32_t wx = mc->weave.win_x;
	const int32_t wy = mc->weave.win_y;
	int32_t px = 0, py = 0;
	uint32_t pw = 0, ph = 0;
	double scale = 1.0;
	const char *how = NULL;
	bool fallback = false;
	uint32_t matches = 0;
	const CGDirectDisplayID panel = weave_resolve_panel(wx, wy, mc->weave.win_display_id, &px, &py, &pw, &ph,
	                                                    &scale, &how, &fallback, &matches);
	if (matches > 1 && !mc->weave.present_ambiguity_logged) {
		mc->weave.present_ambiguity_logged = true;
		U_LOG_W(
		    "weave(#759): window origin (%d,%d) backing px lies in %u displays (mixed backing "
		    "scales) — picked 0x%x; the caller should chain XrWeaveWindowGeometryDXR.displayId",
		    wx, wy, matches, (unsigned)panel);
	}

	const bool first = !mc->weave.have_present_origin;
	const bool panel_changed =
	    first || mc->weave.present_panel != (uint32_t)panel || mc->weave.present_panel_scale != scale;

	mc->weave.have_present_origin = true;
	mc->weave.present_origin_x = wx - px;
	mc->weave.present_origin_y = wy - py;
	mc->weave.present_panel = (uint32_t)panel;
	mc->weave.present_panel_x = px;
	mc->weave.present_panel_y = py;
	mc->weave.present_panel_w = pw;
	mc->weave.present_panel_h = ph;
	mc->weave.present_panel_scale = scale;
	mc->weave.present_panel_fallback = fallback;
	mc->weave.present_check_counter = 0;

	const bool fed = mc->weave.backend != NULL && mc->weave.backend->feeds_present_origin;
	if (panel_changed) {
		U_LOG_W(
		    "weave(#759): present origin (%d,%d) panel-relative backing px on display 0x%x (%s; panel %d,%d "
		    "%ux%u backing px, scale %.2f) — %s every submit",
		    mc->weave.present_origin_x, mc->weave.present_origin_y, (unsigned)panel, how, px, py, pw, ph, scale,
		    fed ? "fed to the DP phase slot (set_present_origin)" : "NOT fed (this backend has no phase feed)");
	} else {
		U_LOG_I("weave(#759): present origin (%d,%d) on display 0x%x", mc->weave.present_origin_x,
		        mc->weave.present_origin_y, (unsigned)panel);
	}
}

//! Submits between display-reconfiguration checks of the cached panel.
#define WEAVE_PANEL_RECHECK_SUBMITS 60

/*!
 * Display reconfiguration (arrangement / scale change) without a window move:
 * every WEAVE_PANEL_RECHECK_SUBMITS submits, re-read the cached panel's
 * backing rect and re-resolve if it moved, rescaled or vanished (a fallback
 * resolve is always retried — a display may now contain the origin).
 */
static void
weave_recheck_panel_locked(struct multi_compositor *mc)
{
	if (!mc->weave.have_present_origin || mc->weave.geometry_dirty) {
		return;
	}
	if (++mc->weave.present_check_counter < WEAVE_PANEL_RECHECK_SUBMITS) {
		return;
	}
	mc->weave.present_check_counter = 0;
	int32_t x = 0, y = 0;
	uint32_t w = 0, h = 0;
	double s = 1.0;
	const bool ok = weave_display_backing_rect((CGDirectDisplayID)mc->weave.present_panel, &x, &y, &w, &h, &s);
	if (!ok || mc->weave.present_panel_fallback || x != mc->weave.present_panel_x ||
	    y != mc->weave.present_panel_y || w != mc->weave.present_panel_w || h != mc->weave.present_panel_h ||
	    s != mc->weave.present_panel_scale) {
		mc->weave.geometry_dirty = true;
	}
}

/*!
 * Forward a hardware 2D/3D wish to the backend's DP and, once it accepted a
 * change, tell this client's session (#961 semantics; the Linux twin is
 * comp_multi_weave_linux.c weave_apply_display_mode_locked).
 */
static bool
weave_apply_display_mode_locked(struct multi_compositor *mc, bool want_3d, const char *why)
{
	bool has_slot = false;
	const int64_t t0_ns = os_monotonic_get_ns();
	const bool accepted = mc->weave.backend->request_display_mode(mc, want_3d, &has_slot);
	const double dp_ms = (double)(os_monotonic_get_ns() - t0_ns) / 1e6;
	const char *slow = dp_ms > 50.0 ? " (slow)" : "";
	if (!accepted) {
		U_LOG_W(
		    "weave(#759): the display processor REJECTED hardware %s (%s) in %.1f ms%s — panel state "
		    "unchanged, no event sent",
		    want_3d ? "3D" : "2D", why, dp_ms, slow);
		return false;
	}

	const bool prev_3d = !mc->weave.hw_2d_confirmed;
	mc->weave.hw_2d_confirmed = !want_3d;
	if (prev_3d == want_3d) {
		return true; // already there (a re-assert keeps a drifted vendor honest)
	}

	bool panel_3d = false;
	const bool have_readback = mc->weave.backend->get_hardware_3d_state(mc, &panel_3d);
	U_LOG_W(
	    "weave(#759): hardware %s -> %s (%s) on the %s weave backend's display processor (%s) in %.1f ms%s; "
	    "DP readback: %s",
	    prev_3d ? "3D" : "2D", want_3d ? "3D" : "2D", why, mc->weave.backend->name,
	    has_slot ? "request_display_mode accepted" : "mode-neutral DP, nothing to switch", dp_ms, slow,
	    have_readback ? (panel_3d ? "3D (may lag — the lens switches asynchronously)"
	                              : "2D (may lag — the lens switches asynchronously)")
	                  : "none (the DP has no get_hardware_3d_state)");

	union xrt_session_event xse = {0};
	xse.hardware_display_state_change.type = XRT_SESSION_EVENT_HARDWARE_DISPLAY_STATE_CHANGE;
	xse.hardware_display_state_change.hardware_display_3d = want_3d;
	const xrt_result_t xret = multi_compositor_push_event(mc, &xse);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("weave(#759): could not push the hardware-state event to the session: %d", xret);
	}
	return true;
}

//! Pick + bring up the backend; applies a 2D wish recorded before bring-up.
static bool
weave_ensure_backend_locked(struct multi_compositor *mc)
{
	if (mc->weave.backend == NULL) {
		mc->weave.backend = weave_pick_backend(mc);
	}
	if (!mc->weave.backend->ensure_engine(mc)) {
		return false;
	}
	if (!mc->weave.backend_ready) {
		mc->weave.backend_ready = true;
		// 3D is the default, so only a 2D wish that arrived before this DP
		// existed needs acting on (the Linux engine's bring-up rule).
		if (!mc->hardware_display_3d) {
			(void)weave_apply_display_mode_locked(mc, false,
			                                      "deferred request, applied at engine bring-up");
		}
	}
	return true;
}


/*
 *
 * Public entry points (called from ipc_server_handler.c).
 *
 */

bool
comp_multi_weave_bind_window(struct xrt_compositor *xc, uint64_t window_id)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL) {
		return false;
	}
	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);
	// Stored only: the present origin comes from the explicit geometry (spec v7),
	// never from the id (it is an opaque handle on macOS).
	mc->weave.window_id = window_id;
	os_mutex_unlock(&mc->weave.mutex);
	U_LOG_W("weave(#759): bound present-owner window id 0x%" PRIx64, window_id);
	return true;
}

bool
comp_multi_weave_set_window_geometry(struct xrt_compositor *xc,
                                     int32_t origin_x,
                                     int32_t origin_y,
                                     uint32_t client_w,
                                     uint32_t client_h,
                                     int32_t display_id)
{
	// Spec v7 (#1036). Two consumers:
	//  - the DP phase (Metal backend): the panel-relative present origin is
	//    resolved from this at the next submit and fed every submit;
	//  - window metrics (#1116): a weave-only present-owner has no
	//    session_render, so this report is the only window rect
	//    multi_compositor_get_window_metrics() can serve.
	// ADR-033: the placement authority reports geometry; the weaver owns phase.
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL || client_w == 0 || client_h == 0) {
		return false;
	}
	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);
	const bool changed = !mc->weave.have_geometry || mc->weave.win_x != origin_x || mc->weave.win_y != origin_y ||
	                     mc->weave.win_display_id != display_id;
	mc->weave.have_geometry = true;
	mc->weave.win_x = origin_x;
	mc->weave.win_y = origin_y;
	mc->weave.win_w = client_w;
	mc->weave.win_h = client_h;
	mc->weave.win_display_id = display_id;
	if (changed) {
		mc->weave.geometry_dirty = true;
	}
	os_mutex_unlock(&mc->weave.mutex);
	return true;
}

bool
comp_multi_weave_macos_window_on_panel(struct multi_compositor *mc,
                                       int32_t *out_x,
                                       int32_t *out_y,
                                       uint32_t *out_w,
                                       uint32_t *out_h,
                                       uint32_t *out_panel_w,
                                       uint32_t *out_panel_h)
{
	// Lock-free read of the geometry, the same trade
	// multi_compositor_get_window_metrics() makes for this very rect: the
	// client's IPC thread is the only writer (bind/geometry/submit are its own
	// RPCs, serialized with its locates), so a locate never races a write here.
	if (mc == NULL || !mc->weave.have_geometry || mc->weave.win_w == 0 || mc->weave.win_h == 0) {
		return false;
	}
	const int32_t wx = mc->weave.win_x;
	const int32_t wy = mc->weave.win_y;

	int32_t px = 0, py = 0;
	uint32_t pw = 0, ph = 0;
	if (mc->weave.have_present_origin && !mc->weave.geometry_dirty) {
		// The origin the DP phase is being fed right now — one answer for the
		// weave and the Kooima, and no display query on the per-frame path
		// (weave_recheck_panel_locked keeps it fresh across reconfigurations).
		px = mc->weave.present_panel_x;
		py = mc->weave.present_panel_y;
		pw = mc->weave.present_panel_w;
		ph = mc->weave.present_panel_h;
	} else {
		// Before the first submit, or moved since the last one: resolve the
		// panel exactly as the next submit will (pure, no state written).
		double s = 1.0;
		const char *how = NULL;
		bool fallback = false;
		uint32_t matches = 0;
		(void)weave_resolve_panel(wx, wy, mc->weave.win_display_id, &px, &py, &pw, &ph, &s, &how, &fallback,
		                          &matches);
	}
	if (pw == 0 || ph == 0) {
		return false;
	}
	*out_x = wx - px;
	*out_y = wy - py;
	*out_w = mc->weave.win_w;
	*out_h = mc->weave.win_h;
	*out_panel_w = pw;
	*out_panel_h = ph;
	return true;
}

bool
comp_multi_weave_submit(struct xrt_compositor *xc,
                        xrt_graphics_buffer_handle_t in_handle,
                        int32_t rect_x,
                        int32_t rect_y,
                        uint32_t rect_w,
                        uint32_t rect_h,
                        uint32_t rect_count,
                        const struct xrt_rect *rects,
                        xrt_graphics_buffer_handle_t overlay_handle,
                        bool weave_frame_first,
                        const struct xrt_weave_atlas_layout *layout,
                        uint32_t flat_rect_count,
                        const struct xrt_rect *flat_rects,
                        uint32_t *out_width,
                        uint32_t *out_height,
                        uint64_t *out_fence_value,
                        struct xrt_eye_positions *out_eyes)
{
	// v8 (browser#88): accepted and ignored — the per-region hardware wish is
	// published through the D3D11 service's zone-wish channel and has no macOS
	// counterpart yet. Ignoring it is CONFORMANT, not a stub: the wish is advisory
	// and hardware-only (ADR-027 D6), so the woven pixels are unaffected and the
	// panel simply stays as 3D as it was pre-v8.
	(void)flat_rect_count;
	(void)flat_rects;

	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL || in_handle == NULL) {
		return false;
	}

	// The handler hands us ownership of the retained IOSurfaceRef the IPC
	// receive looked up; we either adopt it into the cache or release it.
	IOSurfaceRef surface = (IOSurfaceRef)in_handle;
	uint32_t surface_id = (uint32_t)IOSurfaceGetID(surface);

	// v4 overlay atlas (browser#18): the handler passes a second retained
	// IOSurfaceRef when the caller chained XrWeaveSubmitOverlaysDXR. We own it —
	// adopt into the overlay cache (keyed by IOSurfaceID) or release it below.
	IOSurfaceRef overlay = (IOSurfaceRef)overlay_handle; // may be NULL
	uint32_t overlay_id = overlay != NULL ? (uint32_t)IOSurfaceGetID(overlay) : 0;

	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);

	bool ok = false;
	do {
		if (!weave_ensure_backend_locked(mc)) {
			break;
		}
		const struct comp_multi_weave_macos_backend *be = mc->weave.backend;

		// (Re)import the input on identity change (new IOSurface = new id).
		if (mc->weave.in_iosurface == NULL || mc->weave.in_iosurface_id != surface_id) {
			be->release_input(mc);
			if (mc->weave.in_iosurface != NULL) {
				CFRelease((IOSurfaceRef)mc->weave.in_iosurface);
				mc->weave.in_iosurface = NULL;
			}
			mc->weave.in_iosurface_id = 0;
			if (!be->import_input(mc, surface)) {
				break;
			}
			mc->weave.in_iosurface = (void *)surface; // adopt the retained ref
			mc->weave.in_iosurface_id = surface_id;
			surface = NULL; // ownership transferred
		}

		// v4 overlay: (re)import on identity change. On a matching id we keep the
		// cached import and release the (redundant) per-call ref at the epilogue.
		if (overlay != NULL &&
		    (mc->weave.overlay_iosurface == NULL || mc->weave.overlay_iosurface_id != overlay_id)) {
			be->release_overlay(mc);
			if (mc->weave.overlay_iosurface != NULL) {
				CFRelease((IOSurfaceRef)mc->weave.overlay_iosurface);
				mc->weave.overlay_iosurface = NULL;
			}
			mc->weave.overlay_iosurface_id = 0;
			if (be->import_overlay(mc, overlay)) {
				mc->weave.overlay_iosurface = (void *)overlay; // adopt the retained ref
				mc->weave.overlay_iosurface_id = overlay_id;
				overlay = NULL; // ownership transferred
				U_LOG_W("weave(#759) v4: overlay import cached (%ux%u)", mc->weave.overlay_w,
				        mc->weave.overlay_h);
			}
		}

		// Spec-v6 N-view atlas (#774): a non-NULL layout with view_count > 0
		// means the caller already packed the atlas the way every DisplayXR app
		// does — tiles contiguous from the top-left at (content_view_w,
		// content_view_h) in a worst-case-sized input (ADR-010). No SBS scratch,
		// no per-rect unpack, no firstChunk clear: crop the top-left packed
		// region if the worst case is bigger (ADR-030 crop-before-DP) and weave
		// once. The woven output IS one content view (= the whole window); the
		// present-owner reads back per-element window-regions from it.
		const bool nview = (layout != NULL && layout->view_count > 0);
		uint32_t cvw = 0, cvh = 0, packed_w = 0, packed_h = 0;
		if (nview) {
			cvw = layout->content_view_w;
			cvh = layout->content_view_h;
			packed_w = layout->tile_columns * cvw;
			packed_h = layout->tile_rows * cvh;
			if (packed_w > mc->weave.in_w || packed_h > mc->weave.in_h) {
				U_LOG_E("weave(#759) v6: packed region %ux%u exceeds input atlas %ux%u "
				        "(views=%u grid=%ux%u content=%ux%u)",
				        packed_w, packed_h, mc->weave.in_w, mc->weave.in_h, layout->view_count,
				        layout->tile_columns, layout->tile_rows, cvw, cvh);
				break;
			}
		}

		// Output dims: v6 = one content view (cvw x cvh), or the window (if
		// reported) when the DP's lattice must not be resampled; batch = the (window-client-sized)
		// input; legacy = rect offset+extent (the Windows GetClientRect-less
		// fallback).
		uint32_t want_w = 0, want_h = 0;
		if (nview) {
			want_w = cvw;
			want_h = cvh;
			// A non-resample-tolerant DP's lattice must reach the panel 1:1:
			// size the output to the window when the caller reported one.
			// Without geometry stay at one content view — the worst-case input
			// (N views wide) is never the window.
			if (!be->output_tolerates_resample(mc) && mc->weave.have_geometry && mc->weave.win_w > 0 &&
			    mc->weave.win_h > 0) {
				want_w = mc->weave.win_w;
				want_h = mc->weave.win_h;
			}
		} else if (rect_count > 0) {
			want_w = mc->weave.in_w;
			want_h = mc->weave.in_h;
		} else {
			want_w = (uint32_t)rect_x + rect_w;
			want_h = (uint32_t)rect_y + rect_h;
		}
		if (want_w == 0 || want_h == 0) {
			break;
		}

		if (!be->ensure_output(mc, want_w, want_h, nview)) {
			break;
		}

		weave_recheck_panel_locked(mc);
		weave_resolve_present_origin_locked(mc);

		const struct comp_multi_weave_macos_params params = {
		    .rect_count = rect_count,
		    .rects = rects,
		    .want_w = want_w,
		    .want_h = want_h,
		    .first_chunk = weave_frame_first,
		    .nview = nview,
		    .v6_zero_copy = nview && (packed_w == mc->weave.in_w && packed_h == mc->weave.in_h),
		    .cvw = cvw,
		    .cvh = cvh,
		    .packed_w = packed_w,
		    .packed_h = packed_h,
		    .tile_columns = nview ? layout->tile_columns : 2,
		    .tile_rows = nview ? layout->tile_rows : 1,
		    .view_count = nview ? layout->view_count : 2,
		    .have_present_origin = mc->weave.have_present_origin,
		    .present_origin_x = mc->weave.present_origin_x,
		    .present_origin_y = mc->weave.present_origin_y,
		};
		if (!be->record_and_wait(mc, &params)) {
			break;
		}

		mc->weave.fence_value++;

		*out_width = mc->weave.out_w;
		*out_height = mc->weave.out_h;
		*out_fence_value = mc->weave.fence_value;

		// Eyes flow OUT (runtime -> caller) for the caller's next off-axis
		// frame; the weave itself reads the tracker DP-internally.
		U_ZERO(out_eyes);
		if (!be->get_eyes(mc, out_eyes)) {
			U_ZERO(out_eyes);
		}

		ok = true;
	} while (false);

	os_mutex_unlock(&mc->weave.mutex);

	// Release the per-call refs that weren't adopted into a cache.
	if (surface != NULL) {
		CFRelease(surface);
	}
	if (overlay != NULL) {
		CFRelease(overlay);
	}
	return ok;
}

bool
comp_multi_weave_export_output(struct xrt_compositor *xc,
                               xrt_graphics_buffer_handle_t *out_handle,
                               uint32_t *out_width,
                               uint32_t *out_height)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || !mc->weave.mutex_initialized) {
		return false;
	}
	os_mutex_lock(&mc->weave.mutex);
	bool ok = false;
	if (mc->weave.out_iosurface != NULL && mc->weave.out_w != 0) {
		// The IPC send path only reads the IOSurfaceID out of the ref; the
		// cache keeps ownership (released on resize/teardown).
		CFRetain((IOSurfaceRef)mc->weave.out_iosurface);
		*out_handle = (xrt_graphics_buffer_handle_t)mc->weave.out_iosurface;
		*out_width = mc->weave.out_w;
		*out_height = mc->weave.out_h;
		ok = true;
	}
	os_mutex_unlock(&mc->weave.mutex);
	return ok;
}

bool
comp_multi_weave_export_fence(struct xrt_compositor *xc, xrt_graphics_sync_handle_t *out_handle)
{
	// No cross-process GPU fence on macOS — completion is synchronous
	// (xrWeaveSubmitDXR returns after the weave finished on the GPU).
	(void)xc;
	(void)out_handle;
	return false;
}

bool
comp_multi_weave_snap_window_rect(struct xrt_compositor *xc,
                                  int32_t origin_x,
                                  int32_t origin_y,
                                  int32_t target_x,
                                  int32_t target_y,
                                  int32_t *out_snapped_x,
                                  int32_t *out_snapped_y)
{
	// The caller's points are in the weave geometry convention (spec v7 §5:
	// global CoreGraphics space in BACKING px, y down — the same space as
	// XrWeaveWindowGeometryDXR.windowOriginOnScreen). A macOS DP's snap slot
	// takes panel-relative backing px (the set_present_origin lattice), so
	// translate by the panel the gesture-start origin lies on, snap, and
	// translate back. Resolved per call from the origin (the stored displayId
	// disambiguates mixed scales when geometry has been sent; nothing about the
	// cached present origin is touched), so a snap before any geometry or any
	// submit still finds its panel. false (identity) when the engine is not up
	// yet or its backend/DP has no snap — the vk backend never snaps.
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL || out_snapped_x == NULL || out_snapped_y == NULL) {
		return false;
	}
	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);
	bool snapped = false;
	const struct comp_multi_weave_macos_backend *b = mc->weave.backend;
	if (mc->weave.backend_ready && b != NULL && b->snap_window_rect != NULL) {
		int32_t px = 0, py = 0;
		uint32_t pw = 0, ph = 0;
		double scale = 1.0;
		const char *how = NULL;
		bool fallback = false;
		uint32_t matches = 0;
		const int32_t display_id = mc->weave.have_geometry ? mc->weave.win_display_id : -1;
		(void)weave_resolve_panel(origin_x, origin_y, display_id, &px, &py, &pw, &ph, &scale, &how, &fallback,
		                          &matches);
		int32_t sx = target_x - px, sy = target_y - py;
		if (b->snap_window_rect(mc, origin_x - px, origin_y - py, target_x - px, target_y - py, &sx, &sy)) {
			*out_snapped_x = sx + px;
			*out_snapped_y = sy + py;
			snapped = true;
		}
	}
	os_mutex_unlock(&mc->weave.mutex);
	return snapped;
}

bool
comp_multi_weave_macos_request_display_mode(struct multi_compositor *mc, bool enable_3d)
{
	if (mc == NULL || mc->msc == NULL) {
		return false;
	}
	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);
	const bool had_pending_2d = !mc->weave.backend_ready && !mc->hardware_display_3d;
	mc->hardware_display_3d = enable_3d;
	bool ok = true;
	if (mc->weave.backend_ready) {
		ok = weave_apply_display_mode_locked(mc, enable_3d, "xrRequestDisplayModeDXR");
	} else if (enable_3d && had_pending_2d) {
		U_LOG_W(
		    "weave(#759): hardware 3D requested before the weave engine exists — the recorded 2D request is "
		    "withdrawn, nothing to apply at bring-up");
	} else if (!enable_3d) {
		U_LOG_W(
		    "weave(#759): hardware 2D requested before the weave engine exists — recorded, applied when the "
		    "engine comes up");
	}
	os_mutex_unlock(&mc->weave.mutex);
	return ok;
}

void
comp_multi_weave_fini(struct multi_compositor *mc)
{
	if (mc == NULL || !mc->weave.mutex_initialized) {
		return;
	}
	os_mutex_lock(&mc->weave.mutex);
	if (mc->weave.backend != NULL) {
		mc->weave.backend->fini(mc);
	}
	if (mc->weave.in_iosurface != NULL) {
		CFRelease((IOSurfaceRef)mc->weave.in_iosurface);
		mc->weave.in_iosurface = NULL;
	}
	mc->weave.in_iosurface_id = 0;
	if (mc->weave.overlay_iosurface != NULL) {
		CFRelease((IOSurfaceRef)mc->weave.overlay_iosurface);
		mc->weave.overlay_iosurface = NULL;
	}
	mc->weave.overlay_iosurface_id = 0;
	mc->weave.backend = NULL;
	mc->weave.backend_ready = false;
	os_mutex_unlock(&mc->weave.mutex);
	os_mutex_destroy(&mc->weave.mutex);
	mc->weave.mutex_initialized = false;
}

#endif // XRT_OS_MACOS
