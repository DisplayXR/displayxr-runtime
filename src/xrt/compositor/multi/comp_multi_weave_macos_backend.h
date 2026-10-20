// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Private backend seam of the macOS XR_DXR_weave engine (#759).
 * @author David Fattal
 * @ingroup comp_multi
 *
 * The platform front end (comp_multi_weave_macos.c) owns everything that is
 * graphics-API neutral: the IPC entry points, the per-client lock, the
 * retained caller IOSurfaceRefs (identity cache keyed by IOSurfaceID), layout
 * validation, output sizing, the window geometry and its panel-relative
 * present origin, and the hardware 2D/3D bookkeeping. A backend owns the GPU
 * objects and the display processor:
 *
 *  - `vk`    (comp_multi_weave_macos_vk.c)   — MoltenVK + a dp_factory_vk DP;
 *            the original engine, unchanged (sim_display's shipping path).
 *  - `metal` (comp_multi_weave_macos_metal.m) — native Metal + a
 *            dp_factory_metal DP (the only DP family the Leia macOS plug-in
 *            exports).
 *
 * Selection: DXR_WEAVE_MAC_BACKEND=auto|metal|vk (default auto). auto picks
 * metal iff the plug-in has no Vulkan factory but has a Metal one, else vk.
 *
 * Every hook is called with mc->weave.mutex held, on the client's IPC thread.
 */

#pragma once

#include "comp_multi_private.h"

#ifdef XRT_OS_MACOS

#include <IOSurface/IOSurface.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_eye_positions;

/*!
 * One submit's recording parameters, resolved by the front end.
 */
struct comp_multi_weave_macos_params
{
	//! Batch (v3) rects; rect_count == 0 is the legacy single-rect submit whose
	//! rect is (0, 0, want_w, want_h).
	uint32_t rect_count;
	const struct xrt_rect *rects;
	//! The output size the front end asked ensure_output for.
	uint32_t want_w, want_h;
	//! v5 firstChunk: clear the SBS scratch to (0,0,0,0) before the rects.
	bool first_chunk;

	//! @name Spec-v6 N-view atlas (#774)
	//! @{
	bool nview;
	bool v6_zero_copy; //!< packed region == the whole input
	uint32_t cvw, cvh; //!< content view size
	uint32_t packed_w, packed_h;
	uint32_t tile_columns, tile_rows;
	uint32_t view_count;
	//! @}

	//! Panel-relative present origin (backing px, y down) for the DP phase;
	//! valid only when have_present_origin.
	bool have_present_origin;
	int32_t present_origin_x, present_origin_y;

	//! @name XR_DXR_lift §6: this submit's lifted rects (0 unless lifts_rects)
	//! @{
	uint32_t lift_count;
	uint64_t lift_owner;
	const struct xrt_lift_weave_rect *lift_rects;
	//! @}
};

struct comp_multi_weave_macos_backend
{
	const char *name;

	//! Feeds the panel-relative present origin to its DP before every weave
	//! (the Metal backend; the vk backend is unchanged and has no phase feed).
	bool feeds_present_origin;

	//! Lifts weave rects (XR_DXR_lift §6, comp_multi_weave_lift.h): both
	//! backends -- vk records through vk_conv_weave, Metal through
	//! comp_multi_weave_metal_lift as a vk_conv_weave host client (the results'
	//! IOSurface export sampled in Metal).
	bool lifts_rects;

	//! Bring up the device objects + DP once; true when ready.
	bool (*ensure_engine)(struct multi_compositor *mc);

	//! Wrap @p surface as the input; sets mc->weave.in_w / in_h. The front end
	//! keeps the IOSurfaceRef itself.
	bool (*import_input)(struct multi_compositor *mc, IOSurfaceRef surface);
	void (*release_input)(struct multi_compositor *mc);

	//! v4 overlay atlas; sets mc->weave.overlay_w / overlay_h.
	bool (*import_overlay)(struct multi_compositor *mc, IOSurfaceRef surface);
	void (*release_overlay)(struct multi_compositor *mc);

	//! May the v6 output stay one content view and be resampled to the window
	//! by the present-owner? false = size the v6 output to the window.
	bool (*output_tolerates_resample)(struct multi_compositor *mc);

	//! (Re)allocate the exported output (+ the SBS scratch when !nview) at
	//! want_w x want_h; sets mc->weave.out_iosurface / out_w / out_h.
	bool (*ensure_output)(struct multi_compositor *mc, uint32_t want_w, uint32_t want_h, bool nview);

	//! Record, submit and WAIT for one weave (the synchronous macOS contract).
	bool (*record_and_wait)(struct multi_compositor *mc, const struct comp_multi_weave_macos_params *p);

	bool (*get_eyes)(struct multi_compositor *mc, struct xrt_eye_positions *out_eyes);

	//! Forward a hardware 2D/3D request to the DP. @p out_has_slot reports
	//! whether the DP has a request_display_mode slot (no slot = mode-neutral,
	//! returns true).
	bool (*request_display_mode)(struct multi_compositor *mc, bool enable_3d, bool *out_has_slot);
	bool (*get_hardware_3d_state)(struct multi_compositor *mc, bool *out_is_3d);

	//! Phase-snap a drag step (xrWeaveSnapWindowRectDXR): the DP's snap slot with
	//! the front end's points already translated to PANEL-RELATIVE backing px
	//! (y down). true only when the DP produced a snap (outputs valid, same
	//! frame); false = identity. NULL = no snap on this backend.
	bool (*snap_window_rect)(struct multi_compositor *mc,
	                         int32_t origin_x,
	                         int32_t origin_y,
	                         int32_t target_x,
	                         int32_t target_y,
	                         int32_t *out_x,
	                         int32_t *out_y);

	//! Release every backend object (the DP first-class: it is destroyed before
	//! the device objects it was created on).
	void (*fini)(struct multi_compositor *mc);
};

/*
 *
 * Per-screen display processors (#2218 item 6) — the front-end half both
 * backends share (comp_multi_weave_macos.c). The backend owns the manager
 * (mc->weave.segs: comp_vk_native_segments or comp_metal_segments); these
 * keep the screen-table snapshot, the eligibility rule, the window in CG
 * terms and the change-only log in one place.
 *
 */

struct comp_segment_table;
struct comp_weave_window_cg;

/*!
 * Snapshot the system's screen table (comp_multi_system_set_screens) into
 * mc->weave.segs_list / segs_info when its generation moved past
 * mc->weave.segs_generation. Does NOT update segs_generation: the backend
 * compares and rebuilds (or re-tables) its manager, then stores it.
 *
 * @return the current generation; 0 = never handed a table (single-DP weave).
 */
uint64_t
comp_multi_weave_macos_segs_snapshot(struct multi_compositor *mc);

//! Free the snapshot lists (the backend destroys its manager first).
void
comp_multi_weave_macos_segs_free(struct multi_compositor *mc);

/*!
 * May this submit be woven per screen at all? Batch (v3) or v6 only — both
 * weave the WHOLE window as one canvas, which is what the segment table cuts —
 * with a reported geometry and a resolved panel (its backing scale is the
 * window's), and only while the output IS the window (v6 at viewScale 1, or a
 * non-resample-tolerant DP's window-sized v6 output). A legacy single-rect
 * submit, or anything else, keeps the single-DP weave.
 */
bool
comp_multi_weave_macos_segs_eligible(const struct multi_compositor *mc, const struct comp_multi_weave_macos_params *p);

//! The window as the CG projection reads it (backing px + the resolved panel's scale).
void
comp_multi_weave_macos_segs_window(const struct multi_compositor *mc, struct comp_weave_window_cg *out);

/*!
 * One WARN when what the weave does per screen changes — split or not, and
 * which screens — never per drag step. @p t is the manager's last table in
 * window px.
 */
void
comp_multi_weave_macos_segs_log(struct multi_compositor *mc, const struct comp_segment_table *t, bool split);

extern const struct comp_multi_weave_macos_backend comp_multi_weave_macos_backend_vk;

#ifdef COMP_MULTI_WEAVE_HAVE_METAL
extern const struct comp_multi_weave_macos_backend comp_multi_weave_macos_backend_metal;
#endif

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_MACOS
