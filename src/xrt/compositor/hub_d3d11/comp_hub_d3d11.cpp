// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The D3D11 hub (fill arm): weave + present on a D3D11 device for any producer.
 * @ingroup comp_hub_d3d11
 *
 * Born as VK-1, the in-process Vulkan output-device split (#918 / #1178), in
 * comp_vk_native (as comp_vk_native_split.cpp); extracted into its own library
 * by #2317 P1a because it was never Vulkan code. Symbols renamed
 * comp_vk_split_* -> comp_hub_d3d11_*; behaviour unchanged.
 *
 * Topology, the ordering contract and what this rung deliberately leaves to VK-2
 * are all stated in hub_d3d11/comp_hub_d3d11.h — read that first.
 */

#include "comp_hub_d3d11.h"
#include "comp_hub_d3d11_handoff.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_display_processor_d3d11.h"
// XR_DXR_depth_budget: struct xrt_dp_background_preview (the per-API slot
// headers only forward-declare it - the vtable takes it by pointer).
#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_screen.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_tiling.h"
#include "util/u_image_capture.h"

#include "os/os_time.h"
#include "util/u_time.h"

#include <string.h>

/*
 * The split is a Windows mechanism end to end (a D3D11 device on the scanout
 * adapter, a DXGI swapchain, the D3D12 cross-adapter transport behind
 * comp_xbridge). Everything below compiles to nothing elsewhere; the header keeps
 * its signatures so the one caller needs no per-platform spelling beyond the
 * guard it already has.
 *
 * XRT_HAVE_D3D11 as well as the OS: comp_xbridge and comp_d3d11 are themselves
 * built only when it is on, so without it there is nothing to link against and
 * the honest answer is the same one every non-Windows build gives.
 */
#include "xrt/xrt_config_have.h"

#if defined(XRT_OS_WINDOWS) && defined(XRT_HAVE_D3D11)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>

#include "comp_split_gate.h"
#include "comp_xbridge.h"
#include "d3d11/comp_d3d11_outcomp.h"
#include "d3d11/comp_d3d11_segments.h"
#include "d3d11/comp_d3d11_target.h"
#include "d3d/d3d_scanout_helpers.hpp"

#include "os/os_threading.h"
#include "util/comp_segments.h"
#include "util/comp_dp_windowless_d3d11.h" // #2370 client-presents: the windowless DP rule
#include "util/comp_frame_witness.h"       // #2370 client-presents: no target, so its own witness

// #2342 P2c: the hub's capture site encodes its own PNGs. STB_IMAGE_WRITE_STATIC
// scopes the symbols to this TU (the tiers that link the hub carry their own).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


/*
 *
 * Struct.
 *
 */

struct comp_hub_d3d11
{
	struct xrt_device *xdev;
	HWND hwnd;
	void *dp_factory_d3d11;
	bool transparent_background;

	/*!
	 * #2370: the CLIENT-PRESENTS presenter strategy (comp_hub_d3d11_info::
	 * client_presents). @ref target is NULL and stays NULL: the weave writes
	 * @ref client's texture and the caller presents it.
	 */
	bool client_presents;
	struct
	{
		ID3D11Texture2D *tex;        //!< the caller's shared texture, opened on @ref out_dev
		ID3D11RenderTargetView *rtv; //!< the weave's target
		uint32_t w, h;               //!< its extent
		DXGI_FORMAT fmt;
		//! This frame's target on the desktop (comp_hub_d3d11_set_client_target).
		bool target_valid;
		struct comp_seg_rect target;
		//! The windowless primary DP's screen origin, desktop device px.
		int32_t panel_left, panel_top;
		bool origin_logged;
		int32_t origin_x, origin_y;
		//! Display period for the DP's frame timing (no target to ask).
		uint64_t period_ns;
		//! What the last weave did (comp_hub_d3d11_get_client_weave_state).
		bool segmented;
		bool any_flat;
		uint64_t handoffs;
		uint64_t repaints_refused;
		bool repaint_logged;
		bool clamp_logged;
	} client;

	/*!
	 * The panel's requested 2D/3D state, latched from every successful
	 * request_display_mode forward. Sessions begin 3D (begin_session
	 * requests it), so it initializes true at stage A. Feeds the weave
	 * marks' mode_3d — which were HARDCODED true here, so under the split
	 * the frame witness could never report mode=2d and the perf ladder's
	 * MODE-2D gate flagged MODE_UNSETTLED on every rep once the
	 * same-adapter split became the default path (latent since #918
	 * hybrid; the panel itself switched fine — the flag lied).
	 */
	bool mode_3d;
	/*!
	 * #2247: the last weave's table does not cover the primary screen (the
	 * window has wholly left it): the split's DP, which weaves the primary,
	 * is asked for 2D whatever @ref mode_3d says, and gets @ref mode_3d back
	 * when the window returns. Under dp_mutex.
	 */
	bool seg_primary_uncovered;

	//! @name The SCANOUT adapter — everything the weave and the present touch.
	//! @{
	ID3D11Device *out_dev;
	ID3D11DeviceContext *out_ctx;
	IDXGIFactory4 *out_factory;
	struct comp_d3d11_target *target;
	struct xrt_display_processor_d3d11 *dp;
	LUID out_luid;
	WCHAR out_desc[128];
	//! @}

	//! @name The RENDER adapter — the deposit's own D3D11 device, borrowed.
	//! @{
	ID3D11Device *app_dev;
	ID3D11DeviceContext *app_ctx;
	//! `Wait` / `Signal` live here; NULL on a stack without the interface.
	ID3D11DeviceContext4 *app_ctx4;
	IDXGIAdapter *app_adapter;
	LUID render_luid;
	//! @}

	struct comp_xbridge *xbridge;

	//! The Stage-A gate's verdict: render and scanout are one adapter.
	bool same_adapter;
	//! #2317 P1b: @ref comp_hub_d3d11_set_post_weave_hook.
	comp_hub_d3d11_post_weave_fn post_weave_fn;
	void *post_weave_userdata;
	//! #2317 P1b: @ref comp_hub_d3d11_info::stable_atlas.
	bool stable_atlas;
	//! #2317 P2a: @ref comp_hub_d3d11_info::ring_direct.
	bool ring_direct;
	//! #2317 P2c: @ref comp_hub_d3d11_info::ring_direct_planes.
	bool ring_direct_planes;
	//! #2317 P1c: @ref comp_hub_d3d11_set_predicted_display_time.
	uint64_t predicted_display_time_ns;

	/*!
	 * #2317 P1b: DIRECT ingress. The producer's stable atlas opened on THIS
	 * device (same adapter), ordered by two shared fences instead of a transport:
	 * the producer's (signalled after its passes, waited by every weave) and the
	 * hub's (signalled after every weave's reads, waited by the producer's next
	 * passes via @ref comp_hub_d3d11_pre_render). One texture, no ring: the
	 * producer and the hub are serialised on the GPU exactly as the tier's
	 * single-device path serialises render and weave on one context.
	 */
	struct
	{
		bool active;
		ID3D11DeviceContext4 *out_ctx4;
		ID3D11Fence *prod_fence;     //!< the producer's fence, opened on the out device
		uint64_t prod_value;         //!< what the next weave waits for
		ID3D11Fence *cons_fence;     //!< the hub's fence, on the out device
		HANDLE cons_handle;          //!< NT share of @ref cons_fence
		ID3D11Fence *cons_fence_app; //!< the same fence, opened on the producer's device
		uint64_t cons_value;         //!< last value a weave signalled
		ID3D11Texture2D *tex;        //!< the atlas, opened on the out device
		ID3D11ShaderResourceView *srv;
		uint64_t tex_gen;
		uint32_t tex_w, tex_h;
		uint32_t content_w, content_h;
		uint64_t gen; //!< layout generation of the frame @ref tex holds
		bool have_frame;
		//! The producer frame (its hub `seq`) the crop scratch holds, 0 = none:
		//! a repaint re-weaves the same frame, so it reuses the crop.
		uint64_t cropped_seq;
		//! The hub `seq` of the frame @ref have_frame nominates.
		uint64_t frame_seq;
		uint64_t frames;
		uint64_t reopens;

		/*!
		 * #2317 P2b: the three PLANES on this route — the producer's own
		 * NT-shared plane surfaces (Local2D over-flatten, 2D-under backdrop,
		 * authored mask) opened on THIS device, read in place. They are
		 * written on the producer's immediate context before it signals
		 * @ref prod_value and read before this hub signals @ref cons_value,
		 * so the atlas's two fences order them too: no transport, no copy,
		 * no extra fence.
		 */
		struct split_direct_plane
		{
			HANDLE handle; //!< the source handle the open was made from
			uint64_t gen;  //!< its allocation generation; a change re-opens
			ID3D11Texture2D *tex;
			ID3D11ShaderResourceView *srv;
			uint32_t w, h;
			//! The content seq staged for the next submit; 0 = unused this frame.
			uint64_t seq;
			//! The handle + generation whose open FAILED: never retried (or
			//! logged) again until the producer hands a different one.
			HANDLE failed_handle;
			uint64_t failed_gen;
		} planes[COMP_XBRIDGE_PLANE_COUNT];
		/*!
		 * The composite recipe of the frame @ref tex holds, snapshotted at
		 * submit (#1140's rule, one level down: a repaint between two app
		 * frames composites the recipe of the frame it re-weaves, never the
		 * live CPU state). Same shape as an xbridge slot's stamp, so the one
		 * composite reads either.
		 */
		struct comp_xbridge_recipe rec;
		/*!
		 * The zone-wish publish owed by the app frame being submitted. On this
		 * route the authored mask plane IS the producer's surface, rewritten on
		 * the producer's context before its signal, so the publish (which
		 * samples it on THIS context) runs inside that frame's weave, after the
		 * producer wait — not from the deposit half, where nothing orders it.
		 */
		bool publish_pending;
		uint64_t publish_seq;
		/*
		 * #2317 P2a: the RING producers (VK deposit, D3D12 reroute). Each
		 * deposit slot is opened on this device once per ring generation;
		 * @ref cur names the slot the hub reads. Unused (ring=false) for the
		 * stable atlas, which keeps @ref tex / @ref srv.
		 */
		bool ring;
		//! The deposit is KEYED-MUTEX mode: no fences; each weave brackets
		//! its reads with the slot's keyed mutex.
		bool km;
		struct
		{
			ID3D11Texture2D *tex;
			ID3D11ShaderResourceView *srv;
			IDXGIKeyedMutex *km; //!< KEYED-MUTEX mode only
			uint64_t gen;        //!< the deposit ring generation it was opened at
			uint32_t w, h;
		} slots[COMP_HUB_D3D11_DIRECT_RING];
		int32_t cur; //!< nominated slot, -1 = none
		//! Per slot: the consumer-fence value the producer must wait for before
		//! rewriting it, owed once the hub has moved off it (0 = nothing owed).
		uint64_t release_owed[COMP_HUB_D3D11_DIRECT_RING];
		uint64_t km_skips;
	} direct;

	/*!
	 * #2317 P2a: why the ingress is xbridge (a `COMP_HUB_INGRESS_REASON_*`
	 * token), NULL while DIRECT. And what a mid-session move to xbridge needs
	 * to wire the bridge: the deposit's format and extent at A2.
	 */
	const char *ingress_reason;
	bool direct_to_xbridge_failed;
	//! #2317 P2a review: the session's placement line is out, so a later move
	//! to xbridge prints a CHANGED line (before it, the move is reported by
	//! comp_hub_d3d11_log_placement itself).
	bool placement_logged;
	//! #2317: the producer's route token for the placement line (NULL = none).
	const char *route_reason;
	//! #2317 P2a review: once-per-SESSION warnings (a function static would
	//! silence every later session in the process).
	bool warned_slot_open;
	bool warned_atlas_open;
	bool warned_ring_contract;
	bool warned_typed_srgb; //!< #2345 review: split_refuse_typed_srgb
	uint32_t wire_fmt, wire_w, wire_h;

	/*!
	 * #2327: the predicted display time promised for each recent frame, keyed
	 * by its hub `seq`, so a weave marks the latency CSV with the promise of
	 * the frame it actually weaves — on the xbridge ring routes that can be an
	 * earlier frame than the one most recently submitted.
	 */
	struct
	{
		uint64_t seq;
		uint64_t pdt_ns;
	} pdt_ring[8];
	//! The `seq` whose promise @ref pdt_ring last recorded (first set per frame wins).
	uint64_t pdt_seq;

	//! Panel extent, for the placement line and the bridge's plane sizing.
	uint32_t panel_w, panel_h;

	//! Monotonic app-frame counter — the one sequence every bridge fence uses.
	uint64_t seq;

	/*!
	 * #918 R1 — the layout generation stamped on every slot, and the geometry
	 * snapshot minted WITH it (#1140). The weave reads its tile grid from the
	 * snapshot, never from live CPU state, because the slot it is weaving was
	 * filled by an earlier frame.
	 */
	uint64_t layout_gen;
	uint64_t layout_sig;
	uint32_t gen_cols, gen_rows, gen_view_w, gen_view_h;

	//! The crop the OUTPUT device owes when R2 hysteresis holds a worst-case ring.
	ID3D11Texture2D *dp_input_tex;
	ID3D11ShaderResourceView *dp_input_srv;
	uint32_t dp_input_w, dp_input_h;
	//! #1178 — the format the crop scratch was created in, which must track the
	//! EGRESS's rather than be assumed: the copy that fills it is dropped in
	//! silence if the two are different DXGI typeless families.
	DXGI_FORMAT dp_input_fmt;

	/*!
	 * #1264 — the format of the CALLER's 2D plane surfaces (Local2D +
	 * backdrop), which the bridge chains must match for the same
	 * typeless-family reason as dp_input_fmt. The VK deposit's planes are
	 * BGRA (the default); the d3d12 reroute's are RGBA (its atlas/flatten
	 * family) and it says so via comp_hub_d3d11_set_plane_format. The mask
	 * plane stays R8 regardless.
	 */
	DXGI_FORMAT plane_fmt;

	/*!
	 * VK-1b — a REGION-SIZED output-device view of a panel-sized bridge plane,
	 * for the display-processor sideband entry points that take dims but no uv
	 * scale. See @ref split_plane_dp_view.
	 */
	struct split_plane_view
	{
		ID3D11Texture2D *tex;
		ID3D11ShaderResourceView *srv;
		uint32_t w, h;
		uint64_t seq;
		bool warned;
		//! #1178 — one WARN, ever, for a caller whose requested format is not the
		//! plane's own. This runs per frame; the repo forbids a per-frame WARN.
		bool fmt_warned;
	};
	struct split_plane_view dp_bd_view;

	//! @name VK-1b — this frame's BACKDROP plane, staged before the submit.
	//! @{
	bool bd_plane_live;
	uint32_t bd_w, bd_h;
	//! #2317 P2b: the backdrop extent last handed to the DP, for the
	//! change-only lifecycle WARN in @ref split_set_backdrop.
	uint32_t bd_handed_w, bd_handed_h;
	bool bd_handed_logged;
	uint64_t sideband_copies, sideband_skips, sideband_bytes;
	//! @}

	/*!
	 * VK-1b-2 — one out-device R8 mask raster. RTV texture plus a staged SRV
	 * sibling, because an RT is not an SRV; the same decouple the D3D11 leg's
	 * four rasterisers use.
	 */
	struct split_mask_raster
	{
		ID3D11Texture2D *tex;
		ID3D11Texture2D *staged;
		ID3D11RenderTargetView *rtv;
		ID3D11ShaderResourceView *srv;
		uint32_t w, h;
		//! Dirty-check cache — re-rasters only on a real change.
		uint32_t kind;
		uint32_t rect_count;
		struct xrt_rect rects[XRT_MAX_LAYERS];
		float feather[XRT_MAX_LAYERS];
		bool warned;
	};
	struct split_mask_raster mask;
	/*!
	 * #2317 P2b review: the BINARY zone wish of a FEATHERED zones frame. The
	 * composite samples the feather ramp (@ref mask); the published wish must
	 * stay binary (#800/#803), so a feathered frame rasters both, exactly as the
	 * D3D11 tier natively rasters its wish and feather masks separately. Only
	 * ever ZONE_BINARY; only rastered when @ref mask is ZONE_FEATHER.
	 */
	struct split_mask_raster wish;

	/*!
	 * The raster the LAST APP FRAME produced. #868: a repaint replays
	 * RENDERING, never state transitions, so it composites from this instead of
	 * re-rastering — re-rastering drives a once-per-app-frame machine (including
	 * the wish publish) at panel rate.
	 */
	ID3D11ShaderResourceView *repaint_mask_srv;

	//! The masked composite, on the OUT device. Created lazily on the first
	//! frame that needs it — see @ref split_ensure_outcomp.
	struct comp_d3d11_outcomp *outcomp;
	bool outcomp_failed;

	//! @name VK-1b-2 — the composite recipe this frame stages.
	//! @{
	bool l2d_plane_live;
	uint32_t comp_region_w, comp_region_h;
	int32_t comp_cx, comp_cy;
	uint32_t comp_cw, comp_ch;
	uint32_t comp_mode;
	uint32_t comp_mask_kind;
	bool comp_opaque;
	//! @}

	//! @name VK-1b-2 — the CPU-rastered diagnostic HUD, on the OUT device.
	//! @{
	ID3D11Texture2D *hud_tex;
	uint32_t hud_w, hud_h;
	bool hud_live;
	//! @}

	//! @name VK-1b-3 — the app-authored (Tier-3) mask plane.
	//! @{
	bool mask_plane_live;
	uint64_t mask_plane_gen;
	//! The region-sized out-device view of the mask plane, for the wish publish.
	struct split_plane_view dp_mask_view;
	//! @}

	//! ADR-027 P4 — the scanout weaver's zone capability, probed once.
	int zone_dp_state;
	//! #2317 P2b: what that probe returned, for @ref comp_hub_d3d11_get_zone_grid.
	struct xrt_dp_local_zone_caps zone_caps;
	bool zone_published;

	/*!
	 * @name Multi-screen M6 under the split (ADR-047 Amendments 1 + 2)
	 *
	 * The D3D11 segment manager, on the OUTPUT device. @ref segments is made
	 * lazily on the first weave after @ref comp_hub_d3d11_set_screens.
	 * @{
	 */
	struct comp_d3d11_segments *segments;
	struct xrt_screen_list *seg_screens;             //!< heap copy, NULL = never handed over
	struct xrt_system_compositor_info *seg_sys_info; //!< heap copy (the DP registry)
	uint64_t seg_pinned_display_id;
	bool seg_rebuild; //!< a new list arrived: rebuild on the next weave
	//! ADR-047 Amendment 8 (#2305): the screen whose DP @ref dp is while the
	//! home screen is gone (0 = the home's); kept across rebuilds.
	uint64_t seg_away_screen_id;
	//! Guards @ref segments (create / destroy) and @ref seg_pub against the
	//! app thread's xrLocateViews. Taken before @ref dp_mutex, never after.
	struct os_mutex seg_mutex;
	struct xrt_segment_metrics seg_pub; //!< the table the last weave took (count 0 = one view set)
	//! #1883: the partition staged for the next submit, and each submitted
	//! frame's partition keyed by its bridge sequence (the egress slot lags).
	struct comp_segments_content seg_content_staged;
	struct comp_segments_content_ring seg_content_ring;
	/*!
	 * Guards @ref dp against the app-thread forwarders below: the segment
	 * manager swaps it for a windowless per-screen DP (and back) when the
	 * window handle moves between screens (Amendment 2). Leaf lock.
	 */
	struct os_mutex dp_mutex;
	bool mutexes_init;
	//! The session's eye-tracking mode, re-sent to a swapped-in DP.
	uint32_t eye_tracking_mode;
	bool eye_tracking_mode_set;
	//! #2340: the declared atlas encoding, re-sent to a swapped-in DP.
	enum xrt_atlas_encoding atlas_encoding;
	bool atlas_encoding_set;
	/*! @} */

	uint64_t composites, composite_bails;

	/*!
	 * #2342 P2c: the one capture site (@ref comp_hub_d3d11_capture_arm). Set
	 * by the tier on its app thread and served by the next app-frame weave on
	 * the same thread, both under the compositor mutex.
	 */
	struct
	{
		bool armed;
		bool projection_only;
		bool served;
		bool ok;
		char path[512];
		char woven_path[512];
		uint32_t in_w, in_h, out_w, out_h;
		bool woven_ok;
	} cap;

	//! @name Counters for the `[RENDER]` line and the F4/R1 tripwires.
	//! @{
	uint64_t no_slot;
	uint64_t out_crop;
	uint64_t stale_refusals;
	uint64_t inflight_weaves;
	uint64_t diag_window_ns;
	//! @}

	//! @name #1178 — the swapchain-follows-the-window tripwire.
	//! @{
	//! When the swapchain and the window client rect FIRST disagreed, or 0
	//! while they agree. A live drag legitimately disagrees for a frame or two,
	//! so the complaint waits out @ref SPLIT_DRIFT_GRACE_NS of continuous drift.
	uint64_t tgt_drift_since_ns;
	uint64_t tgt_drift_log_ns;
	uint64_t tgt_drift_frames;
	//! @}
};


/*
 *
 * Helpers.
 *
 */

//! #1178 — how long the swapchain may lag the window before the tripwire says so.
#define SPLIT_DRIFT_GRACE_NS (500 * 1000 * 1000ULL)
//! ...and how often it may repeat itself. Never per frame; the repo forbids that.
#define SPLIT_DRIFT_LOG_NS (5 * 1000 * 1000 * 1000ULL)

//! windows.h-free mirror, for @ref comp_split_gate_inputs.
static struct comp_split_luid
split_luid(LUID l)
{
	struct comp_split_luid out = {};
	out.low = (uint32_t)l.LowPart;
	out.high = (int32_t)l.HighPart;
	return out;
}

static LUID
split_luid_from_packed(uint64_t packed)
{
	LUID l = {};
	memcpy(&l, &packed, sizeof(l));
	return l;
}

/*!
 * `DXR_TEST_FAKE_DP_REFUSE=1` — the VENDOR-REFUSAL arm of the fallback matrix.
 *
 * Same shape and same name as the two D3D in-process legs deliberately: it fires
 * ONLY on the scanout-adapter create, so the app-device fallback is allowed to
 * succeed and what gets walked is the RECOVERY. Latched on first call.
 */
static bool
split_test_fake_dp_refuse(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("DXR_TEST_FAKE_DP_REFUSE");
		cached = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return cached == 1;
}

/*!
 * ADR-037 §3a — ask the plug-in for a weaver on the scanout adapter.
 *
 * The single creation point, so no weaver can ever be stranded on the HWND: it
 * either succeeds and becomes the session's weaver, or nothing was created at all
 * and the caller falls through to the Vulkan factory on the app device.
 */
static bool
split_make_dp(struct comp_hub_d3d11 *s)
{
	if (s->dp_factory_d3d11 == NULL) {
		U_LOG_W("VK output-device split: no D3D11 display-processor factory — the scanout adapter "
		        "has nothing to weave with (#1178)");
		return false;
	}

	if (split_test_fake_dp_refuse()) {
		U_LOG_W("DXR_TEST_FAKE_DP_REFUSE=1: refusing the display processor on the SCANOUT device (test)");
		s->dp = NULL;
		return false;
	}

	auto factory = (xrt_dp_factory_d3d11_fn_t)s->dp_factory_d3d11;
	xrt_result_t dp_ret = factory(s->out_dev, s->out_ctx, s->hwnd, &s->dp);
	if (dp_ret != XRT_SUCCESS || s->dp == NULL) {
		U_LOG_W("VK output-device split: D3D11 display processor factory failed (error %d) on the "
		        "SCANOUT device",
		        (int)dp_ret);
		s->dp = NULL;
		return false;
	}

	U_LOG_W("VK output-device split: D3D11 display processor created on the SCANOUT device (hwnd %p)",
	        (void *)s->hwnd);

	// Session settings that must follow the weaver to whichever device it
	// landed on. `client_presents` is false: under the split the RUNTIME
	// presents, on the scanout adapter.
	xrt_display_processor_d3d11_set_transparent_background(s->dp, s->transparent_background,
	                                                       /*client_presents=*/false);
	// A shared-texture session is refused by the gate before we get here, so
	// this is always false — stated rather than assumed.
	xrt_display_processor_d3d11_set_shared_texture_present(s->dp, false);
	return true;
}

/*!
 * #2370: the client-presents strategy has no swapchain, so the target's frame
 * witness never runs; this one counts its weaves and its handoffs (the
 * caller's present is out of sight, so "present" here = the texture handed
 * back). Same DXR_FRAME_WITNESS switch and line shape as every other site.
 */
static comp_frame_witness g_frame_witness_hub_client{"d3d11-hub-client"};

/*!
 * #2370: the client-presents weaver — a WINDOWLESS primary DP for the session
 * screen, phased from the caller's target (ADR-060 R2's contract, one rule
 * shared with the native tier: comp_dp_make_windowless_d3d11). Same single
 * creation point as @ref split_make_dp: it succeeds and becomes the session's
 * weaver, or nothing was created at all.
 */
static bool
split_make_client_dp(struct comp_hub_d3d11 *s, const struct xrt_dp_registry_entry *e)
{
	if (split_test_fake_dp_refuse()) {
		U_LOG_W("DXR_TEST_FAKE_DP_REFUSE=1: refusing the client-presents display processor (test)");
		return false;
	}
	const char *why = nullptr;
	xrt_result_t ret = XRT_SUCCESS;
	if (!comp_dp_make_windowless_d3d11(e, s->out_dev, s->out_ctx, &s->dp, &why, &ret)) {
		U_LOG_W(
		    "D3D11 hub client-presents: no windowless display processor (%s%s%s, error %d) — the session "
		    "needs XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN (ADR-060 R2) to weave a texture it does not "
		    "present (#2370)",
		    why, e != nullptr ? ", plug-in " : "", e != nullptr ? e->plugin_id : "", (int)ret);
		s->dp = nullptr;
		return false;
	}
	s->client.panel_left = e->screen_left;
	s->client.panel_top = e->screen_top;
	// The CALLER presents (client_presents=true), and the target is its shared
	// texture — the native texture tier's two session settings, verbatim.
	xrt_display_processor_d3d11_set_transparent_background(s->dp, s->transparent_background,
	                                                       /*client_presents=*/true);
	xrt_display_processor_d3d11_set_shared_texture_present(s->dp, true);
	U_LOG_W(
	    "D3D11 hub client-presents: WINDOWLESS primary DP from '%s' (screen at %d,%d %ux%u) on the hub device — "
	    "phased from the caller's target via set_present_origin; no window is ever handed to the vendor "
	    "(#2370, ADR-060 R2)",
	    e->plugin_id, e->screen_left, e->screen_top, e->pixel_width, e->pixel_height);
	return true;
}

/*!
 * #2370: open the caller's shared texture on the hub device — an NT handle
 * (`OpenSharedResource1`, what a D3D12 caller shares) or a legacy share handle
 * (`OpenSharedResource`, what `D3D11_RESOURCE_MISC_SHARED` gives) — and make
 * the weave's render-target view. Same adapter by construction (the hub device
 * is on the caller's adapter).
 */
static bool
split_open_client_texture(struct comp_hub_d3d11 *s, void *handle)
{
	auto &cl = s->client;
	if (handle == nullptr) {
		return false;
	}
	HRESULT hr = E_FAIL;
	ID3D11Device1 *d1 = nullptr;
	if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1)) && d1 != nullptr) {
		hr = d1->OpenSharedResource1((HANDLE)handle, __uuidof(ID3D11Texture2D), (void **)&cl.tex);
		d1->Release();
	}
	if (FAILED(hr) || cl.tex == nullptr) {
		cl.tex = nullptr;
		hr = s->out_dev->OpenSharedResource((HANDLE)handle, __uuidof(ID3D11Texture2D), (void **)&cl.tex);
	}
	if (FAILED(hr) || cl.tex == nullptr) {
		U_LOG_W("D3D11 hub client-presents: could not open the caller's shared texture %p on the hub device "
		        "(hr=0x%08lx)",
		        handle, (unsigned long)hr);
		cl.tex = nullptr;
		return false;
	}
	D3D11_TEXTURE2D_DESC td = {};
	cl.tex->GetDesc(&td);
	cl.w = td.Width;
	cl.h = td.Height;
	cl.fmt = td.Format;
	// A NULL desc, exactly as the native tier makes its RTV on the same texture.
	hr = s->out_dev->CreateRenderTargetView(cl.tex, nullptr, &cl.rtv);
	if (FAILED(hr) || cl.rtv == nullptr) {
		U_LOG_W("D3D11 hub client-presents: no render-target view on the caller's texture (format %u, "
		        "hr=0x%08lx)",
		        (unsigned)td.Format, (unsigned long)hr);
		cl.rtv = nullptr;
		cl.tex->Release();
		cl.tex = nullptr;
		return false;
	}
	U_LOG_W("D3D11 hub client-presents: the caller's shared texture %ux%u format=%u is the weave target",
	        td.Width, td.Height, (unsigned)td.Format);
	return true;
}

static void
split_release_client_texture(struct comp_hub_d3d11 *s)
{
	if (s->client.rtv != nullptr) {
		s->client.rtv->Release();
		s->client.rtv = nullptr;
	}
	if (s->client.tex != nullptr) {
		s->client.tex->Release();
		s->client.tex = nullptr;
	}
}

/*!
 * VK-1b — a region-sized OUTPUT-DEVICE view of a panel-sized bridge plane.
 *
 * The 2D planes are allocated at the panel and the composite writes only the
 * region's top-left sub-rect (#464), but the display processor's
 * `set_background_2d` takes width/height and no uv scale — hand it the panel-sized
 * egress and it stretches a mostly-empty texture over the region. So the sub-rect
 * is copied into a right-sized texture on the device that samples it.
 *
 * Three properties, each of which the D3D11 leg learned in review:
 *   - **Passthrough** when the plane already IS the requested extent (no texture,
 *     no copy, no bytes).
 *   - **Seq-gated** (#918 review F8): the copy re-runs only when the slot's plane
 *     generation differs from the one this view already holds. Without it a
 *     forced repaint re-copies a full region on every panel-rate tick.
 *   - **Clamped and metered** (#918 review F6): the box is min(src, dst), because
 *     D3D11 DROPS an oversized `CopySubresourceRegion` silently and would leave
 *     the vendor sampling an uninitialised texture.
 *
 * TODO(#1178 VK-1b): a near-duplicate of `d3d11_plane_dp_view`
 * (src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp:4975), which is `static`
 * and wired to `struct comp_d3d11_compositor`. Extracting the two into one unit
 * is tracked separately — it touches the shipped D3D11 leg, and this epic's rule
 * is land-before-restructure.
 *
 * Returns NULL on any failure, which the caller treats as "no backdrop" — a
 * degraded feature, never a broken frame.
 */
/*!
 * #2342 P2c, ADR-044 at the import boundary: the NON-DECODING view format of a
 * producer surface the hub samples (the atlas, a ring slot, the crop scratch, a
 * plane). Every producer atlas holds ENCODED bytes (ADR-044 §1, "Atlas the DP
 * receives: encoded"), and every 2D plane holds the encoded flatten, so the
 * hub must read them through the UNORM member of their family: an `_SRGB` view
 * would decode them (the DP would weave linear values — too dark), and a
 * TYPELESS format cannot be a view format at all.
 *
 * A UNORM view can only be made of a typed UNORM texture or a TYPELESS one: a
 * texture created TYPED `_SRGB` takes only `_SRGB` views. So the producer
 * contract is "typed UNORM or TYPELESS", and a typed `_SRGB` producer surface is
 * REFUSED by name (@ref split_refuse_typed_srgb) rather than viewed decoding.
 * Today every producer hands a typed UNORM texture (the D3D11 renderer atlas
 * R8G8B8A8_UNORM, the D3D12 deposit R8G8B8A8_UNORM, the VK deposit
 * B8G8R8A8_UNORM / R8G8B8A8_UNORM / the #2273 deep formats) and maps to itself;
 * the D3D11 shim's planes are the #1795 TYPELESS scratch with an `_SRGB` RTV
 * sibling, viewed UNORM here. The hub's OWN crop scratch is created TYPELESS
 * (@ref split_typeless_format) so its UNORM view always exists.
 *
 * Mirrors d3d_dxgi_format_to_unorm_sample (aux_d3d), which this Vulkan-free
 * unit cannot include (that header pulls in the Vulkan headers).
 */
static DXGI_FORMAT
split_nondecoding_format(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	// The #2273 deep atlas is RGBA16F; a typeless 16-bit surface is viewed so.
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
	default: return f;
	}
}

//! #2345 review: the TYPELESS member of @p f's family (the hub's crop scratch),
//! or @p f itself when it has none listed here.
static DXGI_FORMAT
split_typeless_format(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_TYPELESS;
	default: return f;
	}
}

/*!
 * #2345 review: a producer surface created TYPED `_SRGB` cannot take the
 * non-decoding view (a typed texture's views must match its format), so it is
 * refused by name — once per session — and the caller treats it as an open
 * failure (DIRECT falls to xbridge; a plane degrades alone). Returns true when
 * @p tex must be refused.
 */
static bool
split_refuse_typed_srgb(struct comp_hub_d3d11 *s, ID3D11Texture2D *tex, const char *what)
{
	D3D11_TEXTURE2D_DESC d = {};
	tex->GetDesc(&d);
	if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
		return false;
	}
	if (!s->warned_typed_srgb) {
		s->warned_typed_srgb = true;
		U_LOG_W(
		    "#2342 hub: the producer's %s is a TYPED _SRGB texture (format %d) — it cannot be viewed "
		    "without decoding its encoded bytes (ADR-044), so the hub refuses it; producers hand typed "
		    "UNORM or TYPELESS surfaces",
		    what, (int)d.Format);
	}
	return true;
}

/*!
 * The format the DP is told for the atlas it weaves: the SRV's own, when the
 * split carries a deep atlas (#2273: R10G10B10A2 / RGBA16F), else the 8-bit
 * value it always declared. Twin of the in-process d3d11_dp_view_format.
 */
static DXGI_FORMAT
split_dp_view_format(ID3D11ShaderResourceView *srv)
{
	if (srv == nullptr) {
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
	srv->GetDesc(&d);
	if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
		return d.Format;
	}
	return DXGI_FORMAT_R8G8B8A8_UNORM;
}

static ID3D11ShaderResourceView *
split_plane_dp_view(struct comp_hub_d3d11 *s,
                    struct comp_hub_d3d11::split_plane_view *v,
                    void *plane_srv,
                    uint64_t content_seq,
                    uint32_t w,
                    uint32_t h,
                    DXGI_FORMAT fmt,
                    const char *what)
{
	auto *psrv = static_cast<ID3D11ShaderResourceView *>(plane_srv);
	if (psrv == nullptr || w == 0 || h == 0) {
		return nullptr;
	}
	ID3D11Resource *pres = nullptr;
	psrv->GetResource(&pres);
	if (pres == nullptr) {
		return nullptr;
	}
	ID3D11Texture2D *ptex = nullptr;
	D3D11_TEXTURE2D_DESC pd = {};
	if (FAILED(pres->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&ptex)) || ptex == nullptr) {
		pres->Release();
		return nullptr;
	}
	ptex->GetDesc(&pd);
	ptex->Release();
	/*
	 * #2317 P2b: the format the copy below must land in is the SOURCE VIEW's.
	 * A bridge egress plane is typed, so its view and texture agree; a DIRECT
	 * plane is the producer's own scratch, which is TYPELESS when it carries an
	 * `_SRGB` RTV sibling (#1795) — and a typeless format cannot back the SRV
	 * created below. Same family either way, so the copy is valid.
	 */
	D3D11_SHADER_RESOURCE_VIEW_DESC psd = {};
	psrv->GetDesc(&psd);
	// #2342 P2c (ADR-044): never a decoding view of an encoded plane.
	const DXGI_FORMAT src_fmt = split_nondecoding_format(psd.Format);

	// Passthrough: the plane is exactly what the display processor asked for.
	if (pd.Width == w && pd.Height == h) {
		pres->Release();
		return psrv;
	}

	/*
	 * #1178 — the view is filled by a CopySubresourceRegion out of `ptex`, so its
	 * format is the SOURCE's and not the caller's wish. The two agree today (the
	 * planes are bound BGRA and the mask R8, and the bridge now refuses a bind
	 * whose source disagrees), and taking it from the source anyway is what stops
	 * them silently disagreeing tomorrow: a cross-family copy here would be
	 * dropped, not failed, and the display processor would sample a never-written
	 * texture with every counter healthy.
	 */
	if (fmt != src_fmt && !v->fmt_warned) {
		v->fmt_warned = true;
		U_LOG_W(
		    "%s: DP-view format 0x%x requested but the plane is 0x%x — using the plane's, because a "
		    "cross-family CopySubresourceRegion is silently dropped (#1178)",
		    what, (unsigned)fmt, (unsigned)src_fmt);
	}
	fmt = src_fmt;

	const bool need_alloc = (v->tex == nullptr) || (v->srv == nullptr) || v->w != w || v->h != h;
	if (need_alloc) {
		if (v->srv != nullptr) {
			v->srv->Release();
			v->srv = nullptr;
		}
		if (v->tex != nullptr) {
			v->tex->Release();
			v->tex = nullptr;
		}
		v->w = 0;
		v->h = 0;
		v->seq = 0;

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = s->out_dev->CreateTexture2D(&td, nullptr, &v->tex);
		if (SUCCEEDED(hr) && v->tex != nullptr) {
			hr = s->out_dev->CreateShaderResourceView(v->tex, nullptr, &v->srv);
		}
		if (FAILED(hr) || v->srv == nullptr) {
			// Latched: this runs on the frame path and on every repaint tick,
			// and the repo forbids a per-frame WARN (#918 review D7).
			if (!v->warned) {
				v->warned = true;
				U_LOG_W(
				    "%s: DP-view alloc (%ux%u) failed 0x%08lx — that sideband degrades for "
				    "this session (#918)",
				    what, w, h, (unsigned long)hr);
			}
			if (v->tex != nullptr) {
				v->tex->Release();
				v->tex = nullptr;
			}
			pres->Release();
			return nullptr;
		}
		v->w = w;
		v->h = h;
	} else if (content_seq != 0 && v->seq == content_seq) {
		// Same pixels, already copied — the steady state under a repaint.
		s->sideband_skips++;
		pres->Release();
		return v->srv;
	}

	const uint32_t cw = (w < pd.Width) ? w : pd.Width;
	const uint32_t ch = (h < pd.Height) ? h : pd.Height;
	if (cw == 0 || ch == 0) {
		pres->Release();
		return nullptr;
	}
	D3D11_BOX box = {0, 0, 0, cw, ch, 1};
	s->out_ctx->CopySubresourceRegion(v->tex, 0, 0, 0, 0, pres, 0, &box);
	pres->Release();
	v->seq = content_seq;
	s->sideband_copies++;
	s->sideband_bytes += (uint64_t)cw * ch * 4u;
	return v->srv;
}

/*
 *
 * VK-1b-2 — the OUT-DEVICE mask raster.
 *
 */

//! Free the raster's four D3D objects. Idempotent.
static void
split_release_mask(struct comp_hub_d3d11::split_mask_raster *m)
{
	if (m->srv != nullptr) {
		m->srv->Release();
		m->srv = nullptr;
	}
	if (m->staged != nullptr) {
		m->staged->Release();
		m->staged = nullptr;
	}
	if (m->rtv != nullptr) {
		m->rtv->Release();
		m->rtv = nullptr;
	}
	if (m->tex != nullptr) {
		m->tex->Release();
		m->tex = nullptr;
	}
	m->w = 0;
	m->h = 0;
	m->kind = COMP_HUB_D3D11_MASK_NONE;
	m->rect_count = 0;
}

//! (Re)allocate the R8 RTV + staged SRV pair at @p w x @p h on the OUT device.
static bool
split_mask_ensure(struct comp_hub_d3d11 *s, struct comp_hub_d3d11::split_mask_raster *m, uint32_t w, uint32_t h)
{
	if (m->tex != nullptr && m->w == w && m->h == h) {
		return true;
	}
	split_release_mask(m);

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	HRESULT hr = s->out_dev->CreateTexture2D(&td, nullptr, &m->tex);
	if (SUCCEEDED(hr) && m->tex != nullptr) {
		hr = s->out_dev->CreateRenderTargetView(m->tex, nullptr, &m->rtv);
	}
	if (SUCCEEDED(hr) && m->rtv != nullptr) {
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		hr = s->out_dev->CreateTexture2D(&td, nullptr, &m->staged);
	}
	if (SUCCEEDED(hr) && m->staged != nullptr) {
		hr = s->out_dev->CreateShaderResourceView(m->staged, nullptr, &m->srv);
	}
	if (FAILED(hr) || m->srv == nullptr) {
		// Latched: this is on the frame path and the repo forbids a per-frame WARN.
		if (!m->warned) {
			m->warned = true;
			U_LOG_W(
			    "#918 VK-1b: the out-device mask raster (%ux%u) could not be allocated 0x%08lx — 2D "
			    "content does not composite under the split for this session; the 3D weave is "
			    "unaffected",
			    w, h, (unsigned long)hr);
		}
		split_release_mask(m);
		return false;
	}
	m->w = w;
	m->h = h;
	return true;
}

//! One `ClearView` of @p value over @p r, clamped into the raster.
static void
split_mask_fill(struct comp_hub_d3d11 *s,
                struct comp_hub_d3d11::split_mask_raster *m,
                ID3D11DeviceContext1 *ctx1,
                const struct xrt_rect *r,
                int32_t inset,
                float value)
{
	(void)s;
	int32_t left = r->offset.w + inset;
	int32_t top = r->offset.h + inset;
	int32_t right = r->offset.w + r->extent.w - inset;
	int32_t bottom = r->offset.h + r->extent.h - inset;
	if (left < 0) {
		left = 0;
	}
	if (top < 0) {
		top = 0;
	}
	if (right > (int32_t)m->w) {
		right = (int32_t)m->w;
	}
	if (bottom > (int32_t)m->h) {
		bottom = (int32_t)m->h;
	}
	if (right <= left || bottom <= top) {
		return;
	}
	const float val[4] = {value, 0.0f, 0.0f, 0.0f};
	D3D11_RECT dr = {left, top, right, bottom};
	ctx1->ClearView(m->rtv, val, &dr, 1);
}

static void
split_release_plane_view(struct comp_hub_d3d11::split_plane_view *v)
{
	if (v->srv != nullptr) {
		v->srv->Release();
		v->srv = nullptr;
	}
	if (v->tex != nullptr) {
		v->tex->Release();
		v->tex = nullptr;
	}
	v->w = 0;
	v->h = 0;
	v->seq = 0;
}

static void
split_segments_release(struct comp_hub_d3d11 *s);

//! #2317 P2a: drop the ring slots opened on this device (not the fences).
static void
split_release_direct_slots(struct comp_hub_d3d11 *s)
{
	auto &d = s->direct;
	for (uint32_t i = 0; i < COMP_HUB_D3D11_DIRECT_RING; i++) {
		auto &e = d.slots[i];
		if (e.km != NULL) {
			e.km->Release();
		}
		if (e.srv != NULL) {
			e.srv->Release();
		}
		if (e.tex != NULL) {
			e.tex->Release();
		}
		memset(&e, 0, sizeof(e));
	}
	d.cur = -1;
	d.have_frame = false;
}

//! Release the output half. Idempotent; leaves the borrowed app end alone.
static void
split_release_direct(struct comp_hub_d3d11 *s)
{
	auto &d = s->direct;
	for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
		if (d.planes[p].srv != NULL) {
			d.planes[p].srv->Release();
		}
		if (d.planes[p].tex != NULL) {
			d.planes[p].tex->Release();
		}
	}
	if (d.srv != NULL) {
		d.srv->Release();
	}
	if (d.tex != NULL) {
		d.tex->Release();
	}
	if (d.cons_fence_app != NULL) {
		d.cons_fence_app->Release();
	}
	if (d.cons_handle != NULL) {
		CloseHandle(d.cons_handle);
	}
	if (d.cons_fence != NULL) {
		d.cons_fence->Release();
	}
	if (d.prod_fence != NULL) {
		d.prod_fence->Release();
	}
	if (d.out_ctx4 != NULL) {
		d.out_ctx4->Release();
	}
	split_release_direct_slots(s);
	memset(&d, 0, sizeof(d));
	d.cur = -1;
}

static void
split_release_out(struct comp_hub_d3d11 *s)
{
	split_release_direct(s);
	if (s->xbridge != NULL) {
		comp_xbridge_quiesce(s->xbridge);
		comp_xbridge_destroy(&s->xbridge);
	}
	/*
	 * VK-1b-2 — the zone contribution is withdrawn BEFORE the weaver goes away
	 * (#224 P4's clear edge). A session that just disappears leaves the vendor
	 * unioning a mask nobody owns.
	 */
	comp_hub_d3d11_clear_zone_wish(s);
	comp_d3d11_outcomp_destroy(&s->outcomp);
	split_release_mask(&s->mask);
	split_release_mask(&s->wish);
	s->repaint_mask_srv = NULL;
	if (s->hud_tex != NULL) {
		s->hud_tex->Release();
		s->hud_tex = NULL;
	}
	s->hud_live = false;
	split_release_plane_view(&s->dp_bd_view);
	split_release_plane_view(&s->dp_mask_view);
	if (s->dp_input_srv != NULL) {
		s->dp_input_srv->Release();
		s->dp_input_srv = NULL;
	}
	if (s->dp_input_tex != NULL) {
		s->dp_input_tex->Release();
		s->dp_input_tex = NULL;
	}
	s->dp_input_w = 0;
	s->dp_input_h = 0;
	/*
	 * Multi-screen M6: the segment DPs go first, with no window hand-back (the
	 * split's own DP goes right after) — so whichever DP holds the window
	 * (a segment DP or the split's) is gone before the app-device create
	 * that follows a retire binds the window again.
	 */
	split_segments_release(s);
	// #2370: the caller's texture view goes with the device it was opened on.
	split_release_client_texture(s);
	/*
	 * The weaver dies BEFORE its device, and before the target it was writing
	 * into is gone — the (hwnd, device) bind key means a weaver left alive here
	 * would silently refuse the app-device create that follows a retire.
	 */
	xrt_display_processor_d3d11_destroy(&s->dp);
	/*
	 * #2317 review: unbind the back buffer and flush BEFORE the swapchain goes.
	 * D3D11 defers a swapchain's teardown while its buffers are still bound to
	 * a context, and the HWND stays claimed until then — so the target the
	 * caller creates next on the same window (a retire, or a create-time
	 * refusal after Stage A1) could be refused with E_ACCESSDENIED.
	 */
	if (s->out_ctx != NULL) {
		s->out_ctx->ClearState();
		s->out_ctx->Flush();
	}
	comp_d3d11_target_destroy(&s->target);
	if (s->out_factory != NULL) {
		s->out_factory->Release();
		s->out_factory = NULL;
	}
	if (s->out_ctx != NULL) {
		s->out_ctx->Release();
		s->out_ctx = NULL;
	}
	if (s->out_dev != NULL) {
		s->out_dev->Release();
		s->out_dev = NULL;
	}
	if (s->app_ctx4 != NULL) {
		s->app_ctx4->Release();
		s->app_ctx4 = NULL;
	}
	// app_dev / app_ctx / app_adapter are the DEPOSIT's and are borrowed.
	s->app_dev = NULL;
	s->app_ctx = NULL;
	s->app_adapter = NULL;
}


static void
split_destroy_mutexes(struct comp_hub_d3d11 *s)
{
	if (s->mutexes_init) {
		os_mutex_destroy(&s->seg_mutex);
		os_mutex_destroy(&s->dp_mutex);
		s->mutexes_init = false;
	}
}


/*
 *
 * Multi-screen M6 under the split (ADR-047 Amendments 1 + 2).
 *
 */

/*!
 * #2247: the split's DP weaves the primary screen, so its lens wish is the
 * session's mode while the window covers the primary and 2D once the window
 * has been off it for the hand-off's 0.5 s dwell
 * (comp_d3d11_segments_primary_covered); covered again, the session's mode is
 * re-sent at once. Sends it on a coverage edge, or always with @p force (a
 * freshly swapped-in DP). One WARN per edge. Weave thread.
 */
static void
split_seg_primary_lens(struct comp_hub_d3d11 *s, bool force)
{
	const bool uncovered = !comp_d3d11_segments_primary_covered(s->segments);
	os_mutex_lock(&s->dp_mutex);
	const bool edge = uncovered != s->seg_primary_uncovered;
	const bool want = s->mode_3d && !uncovered;
	if (edge || force) {
		s->seg_primary_uncovered = uncovered;
		if (s->dp != nullptr) {
			xrt_display_processor_d3d11_request_display_mode(s->dp, want);
		}
	}
	os_mutex_unlock(&s->dp_mutex);
	if (edge) {
		U_LOG_W("segments: the window %s the primary screen%s — its lens wish %s (#2247, VK split)",
		        uncovered ? "has been off" : "is back on", uncovered ? " for 0.5 s" : "", want ? "3D" : "2D");
	}
}

/*!
 * Amendment 2 hook: install @p dp as the split's weaver (weave thread, the
 * compositor lock held) and return the previous one for the segment manager
 * to destroy. Re-sends the session-level state a new DP has not seen; the
 * per-frame inputs follow, because the hand-off runs before them in
 * @ref comp_hub_d3d11_weave_and_present.
 */
static struct xrt_display_processor_d3d11 *
split_seg_swap_primary_cb(void *userdata, struct xrt_display_processor_d3d11 *dp)
{
	struct comp_hub_d3d11 *s = static_cast<struct comp_hub_d3d11 *>(userdata);
	os_mutex_lock(&s->dp_mutex);
	struct xrt_display_processor_d3d11 *old = s->dp;
	s->dp = dp;
	os_mutex_unlock(&s->dp_mutex);
	if (dp != nullptr) {
		xrt_display_processor_d3d11_set_transparent_background(dp, s->transparent_background,
		                                                       /*client_presents=*/false);
		xrt_display_processor_d3d11_set_shared_texture_present(dp, false);
		if (s->eye_tracking_mode_set) {
			xrt_display_processor_d3d11_set_eye_tracking_mode(dp, s->eye_tracking_mode);
		}
		if (s->atlas_encoding_set) {
			xrt_display_processor_d3d11_set_atlas_encoding(dp, s->atlas_encoding);
		}
		// #2247: the session's mode, but not on a primary screen the window left.
		split_seg_primary_lens(s, true);
	}
	// A zone wish published to the old DP goes with it; the next publish
	// starts fresh instead of clearing a mask the new DP never saw.
	s->zone_published = false;
	return old;
}

//! Tear the segment manager down (no window hand-back: the split's DP goes next).
static void
split_segments_release(struct comp_hub_d3d11 *s)
{
	if (!s->mutexes_init) {
		return;
	}
	os_mutex_lock(&s->seg_mutex);
	comp_d3d11_segments_set_hwnd_hooks(s->segments, nullptr);
	comp_d3d11_segments_destroy(&s->segments);
	memset(&s->seg_pub, 0, sizeof(s->seg_pub));
	os_mutex_unlock(&s->seg_mutex);
	free(s->seg_screens);
	s->seg_screens = nullptr;
	free(s->seg_sys_info);
	s->seg_sys_info = nullptr;
}

/*!
 * One metric update for the segment manager, run on the weave thread before
 * any per-frame DP input is sent (a window-handle hand-off may swap @ref dp).
 * Builds the manager on the OUTPUT device on first use or after
 * @ref comp_hub_d3d11_set_screens, reads the window's client rect (device px:
 * every DisplayXR executable is per-monitor DPI aware, #1201) and publishes
 * the table this weave took for the next xrLocateViews.
 *
 * @param out_window The window rect this weave segmented (optional, NULL = not wanted).
 * @return true when this weave takes the per-segment path.
 */
static bool
split_segments_update(struct comp_hub_d3d11 *s, uint32_t tgt_w, uint32_t tgt_h, struct comp_seg_rect *out_window)
{
	bool split = false;
	struct comp_seg_rect win = {0, 0, 0, 0};
	if (s->client_presents) {
		// #2370: the segment window IS the caller's target on the desktop
		// (ADR-060 §2: one resolve feeds phase, canvas and segments).
		if (s->seg_screens != nullptr && s->seg_sys_info != nullptr && s->dp != nullptr &&
		    s->client.target_valid) {
			win = s->client.target;
		}
	} else if (s->seg_screens != nullptr && s->seg_sys_info != nullptr && s->hwnd != nullptr && s->dp != nullptr) {
		RECT rc = {};
		POINT org = {0, 0};
		if (GetClientRect(s->hwnd, &rc) && ClientToScreen(s->hwnd, &org)) {
			win.x = org.x;
			win.y = org.y;
			win.w = (uint32_t)(rc.right > rc.left ? rc.right - rc.left : 0);
			win.h = (uint32_t)(rc.bottom > rc.top ? rc.bottom - rc.top : 0);
		}
	}
	if (win.w > 0 && win.h > 0) {
		if (s->seg_rebuild || s->segments == nullptr) {
			os_mutex_lock(&s->seg_mutex); // the app thread reads s->segments for eyes
			if (s->seg_rebuild) {
				// A new screen list (a hotplug re-push, #2305) while another
				// screen's segment DP holds the window: the hooks stay
				// installed so the destroy hands the window back to the
				// split's own DP first (ADR-047 Amendment 2), as the D3D11,
				// Vulkan and GL rebuilds do. Clearing them first (the
				// teardown shape, split_segments_release, where the split's
				// DP goes next) destroyed the DP holding the window and left
				// the split's DP windowless, while the new manager starts
				// out believing the primary holds it — so a window that came
				// back to the panel within the hand-off dwell was never
				// handed back and the panel stayed flat.
				// The home gone from the new list, the window stays
				// with a ready screen instead (ADR-047 A8, #2305).
				comp_d3d11_segments_destroy_for_rebuild(&s->segments, s->seg_screens);
				s->seg_rebuild = false;
			}
			if (s->segments == nullptr) {
				s->segments = comp_d3d11_segments_create(s->out_dev);
				if (s->segments != nullptr && s->client_presents) {
					// #2370: no window to hand off — the primary DP is
					// windowless by construction and phases from its origin
					// on the split path too (ADR-060, R1's segment rule).
					comp_d3d11_segments_set_screens(s->segments, s->seg_screens, s->seg_sys_info,
					                                s->seg_pinned_display_id);
					comp_d3d11_segments_set_primary_phases_from_origin(s->segments, true);
				} else if (s->segments != nullptr) {
					// ADR-047 Amendment 2: the window follows the majority
					// screen, by swapping this split's own DP.
					struct comp_d3d11_segments_hwnd_hooks hooks = {};
					hooks.hwnd = s->hwnd;
					hooks.userdata = s;
					hooks.swap_primary = split_seg_swap_primary_cb;
					// The SDK's WndProc drag snap needs no runtime provider.
					hooks.bracket = nullptr;
					hooks.away_screen_id = &s->seg_away_screen_id;
					comp_d3d11_segments_set_hwnd_hooks(s->segments, &hooks);
					comp_d3d11_segments_set_screens(s->segments, s->seg_screens, s->seg_sys_info,
					                                s->seg_pinned_display_id);
					if (comp_d3d11_segments_enabled(s->segments)) {
						// Lifecycle (once per manager): names the device the
						// segment DPs are made on, the rig's proof they present.
						U_LOG_W(
						    "segments: under the VK #918 output-device split — segment DPs are "
						    "created on the OUTPUT device (LUID=%08lx:%08lx, device %p) and "
						    "weave "
						    "the egress slot (ADR-047 Amendment 1, Vulkan)",
						    (unsigned long)s->out_luid.HighPart,
						    (unsigned long)s->out_luid.LowPart, (void *)s->out_dev);
					}
				}
			}
			os_mutex_unlock(&s->seg_mutex);
		}
		if (comp_d3d11_segments_enabled(s->segments)) {
			// The canvas is the window, clamped to the chain: a window that has
			// just grown is ahead of the swapchain for a frame (#1178).
			const struct comp_seg_rect canvas = {0, 0, win.w < tgt_w ? win.w : tgt_w,
			                                     win.h < tgt_h ? win.h : tgt_h};
			const uint32_t mode_index = (s->xdev != nullptr && s->xdev->hmd != nullptr)
			                                ? s->xdev->hmd->active_rendering_mode_index
			                                : 0;
			split = comp_d3d11_segments_update(s->segments, &win, &canvas, s->out_ctx, mode_index);
			// Every segment DP follows the session's 2D/3D mode on the
			// screens the window covers, 2D on one it left (#2247).
			comp_d3d11_segments_set_display_mode(s->segments, s->mode_3d);
		}
	}
	// #2247: the primary screen's lens follows coverage too (edges only).
	split_seg_primary_lens(s, false);

	// Multi-screen M3: publish the table this weave took (count 0 = one view set).
	struct xrt_segment_metrics m;
	memset(&m, 0, sizeof(m));
	if (split) {
		const struct comp_seg_rect canvas = {0, 0, win.w < tgt_w ? win.w : tgt_w,
		                                     win.h < tgt_h ? win.h : tgt_h};
		(void)comp_d3d11_segments_get_metrics(s->segments, &win, &canvas, s->dp != nullptr, &m);
	}
	os_mutex_lock(&s->seg_mutex);
	m.generation = s->seg_pub.generation;
	struct xrt_segment_metrics prev = s->seg_pub;
	prev.generation = m.generation;
	if (memcmp(&prev, &m, sizeof(m)) != 0) {
		m.generation++;
	}
	s->seg_pub = m;
	os_mutex_unlock(&s->seg_mutex);

	if (out_window != nullptr) {
		*out_window = win;
	}
	return split;
}

extern "C" void
comp_hub_d3d11_set_screens(struct comp_hub_d3d11 *s,
                           const struct xrt_screen_list *list,
                           const struct xrt_system_compositor_info *info,
                           uint64_t pinned_display_id)
{
	if (s == nullptr || list == nullptr || info == nullptr) {
		return;
	}
	struct xrt_screen_list *lc = U_TYPED_CALLOC(struct xrt_screen_list);
	struct xrt_system_compositor_info *ic = U_TYPED_CALLOC(struct xrt_system_compositor_info);
	if (lc == nullptr || ic == nullptr) {
		free(lc);
		free(ic);
		return;
	}
	*lc = *list;
	*ic = *info;
	free(s->seg_screens);
	free(s->seg_sys_info);
	s->seg_screens = lc;
	s->seg_sys_info = ic;
	s->seg_pinned_display_id = pinned_display_id;
	s->seg_rebuild = true;
	// Nothing may frame views from the old table meanwhile.
	os_mutex_lock(&s->seg_mutex);
	memset(&s->seg_pub, 0, sizeof(s->seg_pub));
	os_mutex_unlock(&s->seg_mutex);
}

extern "C" void
comp_hub_d3d11_stage_segment_content(struct comp_hub_d3d11 *s, const struct comp_segments_content *content)
{
	if (s == nullptr) {
		return;
	}
	if (content != nullptr) {
		s->seg_content_staged = *content;
	} else {
		memset(&s->seg_content_staged, 0, sizeof(s->seg_content_staged));
	}
}

extern "C" bool
comp_hub_d3d11_get_segment_metrics(struct comp_hub_d3d11 *s, struct xrt_segment_metrics *out)
{
	if (out == nullptr) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (s == nullptr || !s->mutexes_init) {
		return false;
	}
	os_mutex_lock(&s->seg_mutex);
	*out = s->seg_pub;
	// The eyes are predicted NOW, per segment: the primary from the split's
	// own DP, every other screen from its segment DP (guarded against the
	// weave destroying it).
	for (uint32_t k = 0; k < out->count && k < XRT_MAX_SEGMENTS; k++) {
		struct xrt_segment_metric *g = &out->seg[k];
		if (g->is_primary) {
			os_mutex_lock(&s->dp_mutex);
			g->have_eyes = s->dp != nullptr &&
			               xrt_display_processor_d3d11_get_predicted_eye_positions(s->dp, &g->eyes) &&
			               g->eyes.valid;
			os_mutex_unlock(&s->dp_mutex);
		} else {
			g->have_eyes = comp_d3d11_segments_get_eyes(s->segments, g->screen_id, &g->eyes);
		}
	}
	os_mutex_unlock(&s->seg_mutex);
	return out->count > 0;
}


/*
 *
 * Stage A.
 *
 */

// #2317 P1b: DIRECT ingress, defined with the weave below.
static bool
split_direct_forced_off(void);
static bool
split_direct_forced_on(void);
static bool
split_wire_direct(struct comp_hub_d3d11 *s, const struct comp_hub_d3d11_handoff *handoff);
static void
split_submit_direct(struct comp_hub_d3d11 *s,
                    const struct comp_hub_d3d11_handoff *handoff,
                    uint32_t content_w,
                    uint32_t content_h,
                    const struct comp_xbridge_recipe *rec);
static bool
split_weave_direct(struct comp_hub_d3d11 *s, bool is_repaint, const struct xrt_rect *canvas);
static void
split_direct_to_xbridge(struct comp_hub_d3d11 *s, const char *reason);
static void
split_direct_demote_for_planes(struct comp_hub_d3d11 *s);
static void
split_direct_ring_vet(struct comp_hub_d3d11 *s,
                      const struct comp_hub_d3d11_handoff *handoff,
                      uint32_t content_w,
                      uint32_t content_h);
static void
split_pdt_put(struct comp_hub_d3d11 *s, uint64_t seq, uint64_t pdt_ns);
static uint64_t
split_pdt_get(struct comp_hub_d3d11 *s, uint64_t seq);

/*!
 * The hub's runtime-owned D3D11 device on @p adapter, for BOTH presenter
 * strategies (#2370 review: one bring-up, so the two cannot drift): device +
 * immediate context, multithread protection (the vendor DP may touch the
 * context from its own threads), the one-deep DXGI queue, the adapter's DXGI
 * factory and its description. The swapchain strategy presents on this
 * device; client-presents never does, for which the frame-latency setting is
 * inert.
 *
 * @return NULL on success, else a short phrase for the DISABLED WARN; the
 *         caller's split_release_out undoes whatever was made.
 */
static const char *
split_create_out_device(struct comp_hub_d3d11 *s, IDXGIAdapter *adapter)
{
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL got = {};
	static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels),
	                               D3D11_SDK_VERSION, &s->out_dev, &got, &s->out_ctx);
	if (FAILED(hr) || s->out_dev == nullptr || s->out_ctx == nullptr) {
		U_LOG_W("D3D11 hub: D3D11CreateDevice failed 0x%08lx", (unsigned long)hr);
		return "D3D11CreateDevice failed";
	}
	// The output context has one caller at a time (layer_commit and the repaint
	// loop, both under the compositor mutex), but the vendor DP may touch it
	// from its own threads.
	ID3D10Multithread *omt = nullptr;
	if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)&omt)) && omt != nullptr) {
		omt->SetMultithreadProtected(TRUE);
		omt->Release();
	}
	// Same one-deep DXGI queue the app device gets: on the swapchain strategy
	// the present happens HERE, so this is where the pacing lever lives.
	IDXGIDevice1 *od1 = nullptr;
	if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(IDXGIDevice1), (void **)&od1)) && od1 != nullptr) {
		od1->SetMaximumFrameLatency(1);
		od1->Release();
	}
	hr = adapter->GetParent(__uuidof(IDXGIFactory4), (void **)&s->out_factory);
	if (FAILED(hr) || s->out_factory == nullptr) {
		U_LOG_W("D3D11 hub: the adapter's DXGI factory failed 0x%08lx", (unsigned long)hr);
		return "DXGI factory unavailable";
	}
	DXGI_ADAPTER_DESC ad{};
	if (SUCCEEDED(adapter->GetDesc(&ad))) {
		wcsncpy_s(s->out_desc, ad.Description, _TRUNCATE);
	}
	return nullptr;
}

/*!
 * #2370: Stage A1 of the CLIENT-PRESENTS presenter strategy. Nothing here is
 * the scanout adapter's: the caller presents, so the hub's device is made on
 * the caller's (render) adapter, its weaver is a windowless DP, and its target
 * is the caller's shared texture. Every refusal is the closed token
 * `shared_texture_session` (the tier's own legs compose, exactly as before
 * #2370) behind one WARN naming the rung.
 */
static xrt_result_t
split_stage_a_client(const struct comp_hub_d3d11_info *info,
                     struct comp_hub_d3d11 **out_split,
                     const char **out_short_reason)
{
	*out_split = NULL;
	*out_short_reason = COMP_SPLIT_REASON_SHARED_TEXTURE;
	const uint64_t t0 = os_monotonic_get_ns();

	const uint32_t panel_w =
	    (info->xdev != NULL && info->xdev->hmd != NULL) ? info->xdev->hmd->screens[0].w_pixels : 0;
	const uint32_t panel_h =
	    (info->xdev != NULL && info->xdev->hmd != NULL) ? info->xdev->hmd->screens[0].h_pixels : 0;
	const char *why = nullptr;
	if (info->client_texture_handle == NULL) {
		why = "no shared texture handle";
	} else if (info->windowless_dp_entry == NULL) {
		why = "no DP registry entry for the session screen";
	} else if (info->render_packed_luid == 0) {
		why = "the caller's adapter LUID is unknown";
	} else if (panel_w == 0 || panel_h == 0) {
		why = "no panel dimensions";
	}
	if (why != nullptr) {
		U_LOG_W("D3D11 hub client-presents DISABLED (%s) — the tier's own legs compose (#2370)", why);
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	struct comp_hub_d3d11 *s = U_TYPED_CALLOC(struct comp_hub_d3d11);
	if (s == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	os_mutex_init(&s->seg_mutex);
	os_mutex_init(&s->dp_mutex);
	s->mutexes_init = true;
	s->xdev = info->xdev;
	s->hwnd = NULL; // nothing is ever bound to a window
	s->dp_factory_d3d11 = info->dp_factory_d3d11;
	s->transparent_background = info->transparent_background;
	s->client_presents = true;
	s->mode_3d = true;
	s->render_luid = split_luid_from_packed(info->render_packed_luid);
	s->out_luid = s->render_luid;
	s->panel_w = panel_w;
	s->panel_h = panel_h;
	// The hub device IS on the caller's adapter: DIRECT ingress, no transport.
	s->same_adapter = true;
	s->stable_atlas = info->stable_atlas;
	s->ring_direct = info->ring_direct;
	s->ring_direct_planes = info->ring_direct_planes;
	s->direct.cur = -1;

	// The caller's adapter, by LUID: the device the weave and the caller's
	// present share.
	IDXGIAdapter *adapter = nullptr;
	{
		IDXGIFactory4 *f4 = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&f4)) && f4 != nullptr) {
			(void)f4->EnumAdapterByLuid(s->render_luid, __uuidof(IDXGIAdapter), (void **)&adapter);
			f4->Release();
		}
		if (adapter == nullptr) {
			why = "the caller's adapter could not be resolved by LUID";
		}
	}
	if (why == nullptr) {
		why = split_create_out_device(s, adapter);
	}
	if (adapter != nullptr) {
		adapter->Release();
	}
	if (why == nullptr) {
		if (!split_make_client_dp(s, (const struct xrt_dp_registry_entry *)info->windowless_dp_entry)) {
			why = "the plug-in cannot weave this screen windowless";
		}
	}
	if (why == nullptr && !split_open_client_texture(s, info->client_texture_handle)) {
		why = "the caller's shared texture could not be opened on the hub device";
	}
	if (why != nullptr) {
		split_release_out(s);
		split_destroy_mutexes(s);
		free(s);
		U_LOG_W("D3D11 hub client-presents DISABLED (%s) — the tier's own legs compose (#2370)", why);
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	s->diag_window_ns = os_monotonic_get_ns();
	*out_split = s;
	*out_short_reason = nullptr;
	U_LOG_W(
	    "D3D11 hub client-presents: stage A1 took %.1f ms (device on the CALLER's adapter '%ls' "
	    "LUID=%08lx:%08lx + windowless DP + the caller's texture; no swapchain — the caller presents)",
	    (double)(os_monotonic_get_ns() - t0) / 1.0e6, s->out_desc, (unsigned long)s->out_luid.HighPart,
	    (unsigned long)s->out_luid.LowPart);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_hub_d3d11_stage_a(const struct comp_hub_d3d11_info *info,
                       struct comp_hub_d3d11 **out_split,
                       const char **out_short_reason)
{
	*out_split = NULL;
	*out_short_reason = COMP_SPLIT_REASON_KILLED_BY_ENV;

	// #2370: the client-presents strategy has its own Stage A1 (no scanout
	// adapter, no swapchain); the split gate below is the swapchain strategy's.
	if (info->client_presents && info->has_shared_texture) {
		return split_stage_a_client(info, out_split, out_short_reason);
	}

	if (!comp_split_gate_env_requested()) {
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	const uint64_t stage_a_start_ns = os_monotonic_get_ns();

	/*
	 * The gate's inputs are gathered in the ORIGINAL order and nothing is
	 * resolved past the first refusal — an ineligible session must not go near
	 * getScanoutAdapter, and a session whose own adapter would not resolve has
	 * nothing to compare a scanout adapter against.
	 */
	struct comp_split_gate_inputs gin = {};
	gin.requested = true;

	const uint32_t panel_w = (info->xdev != NULL && info->xdev->hmd != NULL)
	                             ? info->xdev->hmd->screens[0].w_pixels
	                             : 0;
	const uint32_t panel_h = (info->xdev != NULL && info->xdev->hmd != NULL)
	                             ? info->xdev->hmd->screens[0].h_pixels
	                             : 0;

	if (!info->app_timeline_semaphores) {
		/*
		 * Checked FIRST and before any adapter work: without
		 * VK_KHR_timeline_semaphore the deposit cannot exist, and the runtime
		 * cannot turn the feature on after the app created its device. See the
		 * token's own doc comment for why this is not `api_unsupported`.
		 */
		gin.ineligible_reason = COMP_SPLIT_REASON_NO_TIMELINE_SEMAPHORE;
	} else if (info->hwnd == NULL) {
		gin.ineligible_reason = COMP_SPLIT_REASON_NO_HWND;
	} else if (info->has_shared_texture) {
		gin.ineligible_reason = COMP_SPLIT_REASON_SHARED_TEXTURE;
	} else if (panel_w == 0 || panel_h == 0) {
		gin.ineligible_reason = COMP_SPLIT_REASON_NO_PANEL_DIMS;
	} else if (info->render_packed_luid == 0) {
		// The driver would not report a device LUID, so there is nothing to
		// compare the scanout adapter against — and the deposit could not
		// LUID-match a D3D11 device either.
		gin.ineligible_reason = COMP_SPLIT_REASON_RENDER_UNRESOLVABLE;
	}

	const LUID render_luid = split_luid_from_packed(info->render_packed_luid);

	wil::com_ptr<IDXGIAdapter> scanout;
	DXGI_ADAPTER_DESC sdesc{};
	if (gin.ineligible_reason == nullptr) {
		scanout = xrt::auxiliary::d3d::getScanoutAdapter(info->display_screen_left, info->display_screen_top,
		                                                 panel_w, panel_h, U_LOGGING_INFO);
		gin.scanout_resolved = scanout && SUCCEEDED(scanout->GetDesc(&sdesc));
		gin.render_luid = split_luid(render_luid);
		gin.scanout_luid = split_luid(sdesc.AdapterLuid);
	}

	// ADR-039 Phase A: the VK tier consults the same-adapter bring-up switch.
	gin.allow_same_adapter = comp_split_gate_env_same_adapter();

	struct comp_split_gate_result gate = {};
	comp_split_gate_evaluate(&gin, &gate);
	*out_short_reason = gate.short_reason;

	// #2317 step 2: a caller that engages by DEFAULT stays off a single adapter
	// until the desktop run (decided before the ENGAGE line, so nothing is said
	// about an engage that does not happen).
	if (gate.same_adapter && gate.split_active && info->same_adapter_needs_opt_in) {
		*out_short_reason = COMP_HUB_REASON_SAME_ADAPTER_OPT_IN;
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}
	if (gate.same_adapter && gate.split_active) {
		// ADR-039: same adapter, split ENGAGED anyway — the fill engine is
		// the point, not the copy. The ingress below is a same-adapter
		// shared-texture open (no PCIe hop).
		U_LOG_W(
		    "D3D11 hub: ADR-039 same-adapter ENGAGE on '%ls' "
		    "LUID=%08lx:%08lx — one fill engine for every tier "
		    "(DXR_SPLIT_SAME_ADAPTER)",
		    sdesc.Description, (unsigned long)sdesc.AdapterLuid.HighPart,
		    (unsigned long)sdesc.AdapterLuid.LowPart);
	} else if (gate.same_adapter) {
		// Not a failure: on a MUX'd / single-GPU box the weave is already
		// local, so the split has nothing to do. One line, no WARN storm.
		U_LOG_W(
		    "D3D11 hub: scanout adapter '%ls' LUID=%08lx:%08lx IS the app's adapter — "
		    "the output-device split is a no-op (#918)",
		    sdesc.Description, (unsigned long)sdesc.AdapterLuid.HighPart,
		    (unsigned long)sdesc.AdapterLuid.LowPart);
	}
	if (!gate.split_active) {
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	struct comp_hub_d3d11 *s = U_TYPED_CALLOC(struct comp_hub_d3d11);
	if (s == NULL) {
		*out_short_reason = COMP_SPLIT_REASON_STAGE_A_FAILED;
		return XRT_ERROR_ALLOCATION;
	}
	// Multi-screen M6 (segments + the window-handle hand-off).
	os_mutex_init(&s->seg_mutex);
	os_mutex_init(&s->dp_mutex);
	s->mutexes_init = true;
	s->xdev = info->xdev;
	s->hwnd = (HWND)info->hwnd;
	s->dp_factory_d3d11 = info->dp_factory_d3d11;
	s->transparent_background = info->transparent_background;
	// Sessions begin 3D (begin_session requests it right after create);
	// zero-init would make every weave mark read 2D until the first request.
	s->mode_3d = true;
	s->render_luid = render_luid;
	s->out_luid = sdesc.AdapterLuid;
	s->panel_w = panel_w;
	s->panel_h = panel_h;
	s->same_adapter = gate.same_adapter;
	s->stable_atlas = info->stable_atlas;
	s->ring_direct = info->ring_direct;
	s->ring_direct_planes = info->ring_direct_planes;
	s->direct.cur = -1;
	wcsncpy_s(s->out_desc, sdesc.Description, _TRUNCATE);

	const char *reason = nullptr;
	const char *stage_a_token = nullptr;

	// Runtime-owned D3D11 device on the scanout adapter (#2370 review: the one
	// bring-up both presenter strategies share).
	reason = split_create_out_device(s, scanout.get());

	/*
	 * ADR-037 §3a — ASK THE PLUG-IN BEFORE THE SPLIT COMMITS (the D3D11 leg's
	 * #1169 design, taken over D3D12's after-the-fact retire).
	 *
	 * The runtime picks the adapters; it does not get to assume the vendor
	 * plug-in can weave on the one it picked. Asking here — before the target
	 * exists and before the caller has skipped its Vulkan weaver — means a
	 * refusal is just another Stage-A failure, and the teardown below is the
	 * whole recovery. No retire path, no half-built state to unwind.
	 *
	 * Cost is zero: this is the display-processor create the session was going
	 * to pay for regardless. It is MOVED, not added.
	 */
	if (reason == nullptr && !split_make_dp(s)) {
		reason = "the display processor declined a weaver on the scanout adapter";
		stage_a_token = COMP_SPLIT_REASON_DP_REFUSED_SCANOUT;
	}

	// The DXGI swapchain, on the scanout adapter. This is what removes the
	// cross-adapter present the whole epic is about.
	if (reason == nullptr) {
		xrt_result_t xret = comp_d3d11_target_create(/*c=*/nullptr, s->hwnd, s->out_dev, s->out_ctx,
		                                             s->out_factory, info->preferred_width,
		                                             info->preferred_height, info->transparent_background,
		                                             &s->target);
		if (xret != XRT_SUCCESS || s->target == nullptr) {
			U_LOG_W("VK output-device split: could not create the scanout-adapter swapchain (%d)",
			        (int)xret);
			reason = "scanout swapchain creation failed";
		}
	}

	if (reason != nullptr) {
		split_release_out(s);
		split_destroy_mutexes(s);
		free(s);
		*out_short_reason = (stage_a_token != nullptr) ? stage_a_token : COMP_SPLIT_REASON_STAGE_A_FAILED;
		U_LOG_W(
		    "D3D11 hub DISABLED (%s) — the calling compositor's own legs take the session "
		    "(#918)",
		    reason);
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	s->diag_window_ns = os_monotonic_get_ns();

	*out_split = s;
	*out_short_reason = nullptr;
	U_LOG_W("VK output-device split: stage A1 took %.1f ms (device+DP+swapchain on the scanout adapter)",
	        (double)(os_monotonic_get_ns() - stage_a_start_ns) / 1.0e6);
	return XRT_SUCCESS;
}

/*!
 * Wire comp_xbridge between the deposit's device (already recorded in @p s by
 * @ref comp_hub_d3d11_wire_bridge) and the output device, and allocate the
 * egress ring. Shared by Stage A2 and #2317 P2a's mid-session move off DIRECT.
 */
static bool
split_wire_xbridge(struct comp_hub_d3d11 *s, uint32_t dxgi_format, uint32_t width, uint32_t height)
{
	// Worst-case system atlas — the cross-adapter ring is sized for this once,
	// so a mode switch never reallocates the heap mid-flight.
	uint32_t sys_w = 0, sys_h = 0;
	if (s->xdev != nullptr && s->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(s->xdev->rendering_modes, s->xdev->rendering_mode_count, &sys_w,
		                              &sys_h);
	}
	if (sys_w == 0 || sys_h == 0) {
		sys_w = width;
		sys_h = height;
	}

	struct comp_xbridge_info xbi = {};
	/*
	 * D3D11 ENDS — and this is the whole reason VK-0 exists. The deposit made
	 * the Vulkan atlas an NT-shared ID3D11Texture2D with its own device,
	 * context and adapter, which is exactly what this flavour consumes. No
	 * third ends flavour, no VK<->D3D12 external memory.
	 */
	xbi.d3d12_ends = false;
	xbi.app_device = s->app_dev;
	xbi.app_context = s->app_ctx;
	xbi.app_adapter = s->app_adapter;
	xbi.out_device = s->out_dev;
	xbi.out_context = s->out_ctx;
	xbi.out_adapter = nullptr;
	xbi.max_width = sys_w;
	xbi.max_height = sys_h;
	xbi.panel_width = s->panel_w;
	xbi.panel_height = s->panel_h;
	/*
	 * #1178 — THE ATLAS FORMAT, and the whole of this bug.
	 *
	 * The deposit's D3D11 texture takes its format from the VULKAN atlas, which on
	 * this stack is `VK_FORMAT_B8G8R8A8_UNORM` -> `DXGI_FORMAT_B8G8R8A8_UNORM`.
	 * The bridge used to build its rings in a hardcoded `R8G8B8A8`, and
	 * `CopySubresourceRegion` between two different DXGI typeless families is not
	 * an error Windows reports — it is DROPPED. Every counter stayed green, the
	 * egress kept its zero-fill, and the panel was black.
	 *
	 * Declaring the format here (rather than coercing the Vulkan atlas to RGBA) is
	 * the right direction: the atlas format is chosen upstream by the swapchain,
	 * and forcing it would either swizzle the channels or break the VK render
	 * path. Everything downstream of this — the cross-adapter heap, the egress
	 * ring, the display processor's SRV — is already format-agnostic.
	 */
	xbi.atlas_format = dxgi_format;

	// The bridge's out_adapter is used for its own consumer D3D12 device, so it
	// must be the scanout adapter the out device lives on.
	IDXGIDevice *od = nullptr;
	IDXGIAdapter *out_adapter = nullptr;
	if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&od)) && od != nullptr) {
		od->GetAdapter(&out_adapter);
		od->Release();
	}
	xbi.out_adapter = out_adapter;

	const char *xb_reason = nullptr;
	bool ok = (comp_xbridge_create(&xbi, &s->xbridge, &xb_reason) == XRT_SUCCESS);
	if (!ok) {
		s->xbridge = nullptr;
		U_LOG_W("VK output-device split: bridge creation failed (%s)",
		        xb_reason != nullptr ? xb_reason : "cross-adapter heap unsupported");
	}

	if (ok) {
		/*
		 * INGRESS OPTION II, chosen up front and not as a fallback.
		 *
		 * Two independent reasons, either sufficient. First, the deposit is a
		 * RING: the source texture's identity alternates every app frame, which
		 * is precisely the caller shape `force_staged_ingress` documents (an
		 * Option-I caller would re-open a shared handle per frame and drain the
		 * producer each time).
		 *
		 * Second, and the one that is not negotiable: Option II's staging copy
		 * runs on the APP IMMEDIATE CONTEXT inside comp_xbridge_submit. That is
		 * the only context this leg can order against Vulkan's timeline signal,
		 * and it is what lets the deposit slot be RELEASED back to Vulkan
		 * immediately after the submit returns. Under Option I the producer's
		 * own copy queue reads the slot at a time no app-context signal can
		 * bound, so the ring's back-pressure would have nothing to hang off.
		 */
		/*
		 * #2317 P1b: neither reason holds for a STABLE atlas (one texture,
		 * re-created only on a generation bump, and the producer takes
		 * comp_hub_d3d11_pre_render — the Option-I back-fence — before it
		 * rewrites it). That is the D3D11 tier's own split shape, so it gets
		 * the D3D11 tier's ingress: bound once per generation, read in place.
		 */
		if (!s->stable_atlas && !comp_xbridge_force_staged_ingress(s->xbridge)) {
			U_LOG_W("VK output-device split: the ingress staging ring could not be allocated");
			ok = false;
		}
	}

	if (ok) {
		/*
		 * The egress ring is allocated HERE, not on the frame path: the split
		 * must not be able to reach its first weave and only then discover it
		 * has nothing to weave into. Sized at the ACTIVE mode's nominal content
		 * box rather than the worst-case atlas, so the session warmup does not
		 * commit (and immediately free) a large scanout-adapter allocation.
		 */
		uint32_t eg_w = 0, eg_h = 0;
		if (s->xdev != nullptr && s->xdev->hmd != nullptr) {
			uint32_t mi = s->xdev->hmd->active_rendering_mode_index;
			if (mi < s->xdev->rendering_mode_count) {
				const struct xrt_rendering_mode *m = &s->xdev->rendering_modes[mi];
				eg_w = m->tile_columns * m->view_width_pixels;
				eg_h = m->tile_rows * m->view_height_pixels;
			}
		}
		if (eg_w > 0 && eg_h > 0) {
			// Falls back to a worst-case ring internally if this fails.
			comp_xbridge_set_content_size(s->xbridge, eg_w, eg_h, 0);
			uint32_t gw = 0, gh = 0;
			comp_xbridge_get_egress_dims(s->xbridge, &gw, &gh);
			ok = (gw > 0 && gh > 0);
		} else {
			ok = comp_xbridge_alloc_worstcase_egress(s->xbridge);
		}
		if (!ok) {
			U_LOG_W("VK output-device split: egress share failed");
		}
	}

	if (out_adapter != nullptr) {
		out_adapter->Release();
	}

	if (!ok) {
		return false;
	}

	U_LOG_W("VK output-device split ACTIVE: weave/repaint/present move to '%ls' LUID=%08lx:%08lx (app "
	        "device on LUID=%08lx:%08lx); the VK-0 D3D11 deposit crosses via a D3D12 cross-adapter heap "
	        "once per frame (#918 VK-1)",
	        s->out_desc, (unsigned long)s->out_luid.HighPart, (unsigned long)s->out_luid.LowPart,
	        (unsigned long)s->render_luid.HighPart, (unsigned long)s->render_luid.LowPart);
	return true;
}

extern "C" bool
comp_hub_d3d11_wire_bridge(struct comp_hub_d3d11 *s, const struct comp_hub_d3d11_handoff *handoff)
{
	if (s == nullptr || handoff == nullptr || handoff->d3d11_device == nullptr) {
		U_LOG_W("VK output-device split: no deposit to bridge from (#1178)");
		return false;
	}

	const uint64_t t0 = os_monotonic_get_ns();

	s->app_dev = (ID3D11Device *)handoff->d3d11_device;
	s->app_ctx = (ID3D11DeviceContext *)handoff->d3d11_context;
	s->app_adapter = (IDXGIAdapter *)handoff->dxgi_adapter;

	/*
	 * The GPU-side ordering handle. Without it there is no way to make the
	 * bridge's read wait for Vulkan's write except a CPU wait, which this rung
	 * may not take — so its absence fails the wiring rather than degrading it.
	 */
	if (FAILED(s->app_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&s->app_ctx4)) ||
	    s->app_ctx4 == nullptr) {
		U_LOG_W(
		    "VK output-device split: the deposit's device has no ID3D11DeviceContext4, so the "
		    "bridge cannot GPU-wait on the atlas fence (#1178)");
		s->app_ctx4 = nullptr;
		return false;
	}

	// What a mid-session move to xbridge (#2317 P2a) needs to wire the bridge.
	s->wire_fmt = handoff->dxgi_format;
	s->wire_w = handoff->width;
	s->wire_h = handoff->height;

	/*
	 * #2317 P1b/P2a: on ONE adapter neither producer shape needs a transport —
	 * open the atlas (the D3D11 tier's stable texture, or each ring slot of the
	 * VK / D3D12 deposit) on this device and weave it in place. Falls through
	 * to the bridge if any piece of the direct wiring is unavailable;
	 * DXR_HUB_INGRESS=xbridge forces the bridge, as the A/B control.
	 */
	if (s->client_presents) {
		/*
		 * #2370: the client-presents hub is on the caller's adapter by
		 * construction, and DIRECT is its only ingress: an xbridge ring would
		 * be a transport copy with nowhere to go (there is no scanout device),
		 * and its keyed-mutex ring shape has no consumer fence to hand the
		 * texture back with. No DIRECT, no hub — the caller refuses by name.
		 */
		if ((s->stable_atlas || s->ring_direct) && handoff->keyed_mutex == nullptr &&
		    split_wire_direct(s, handoff)) {
			s->ingress_reason = nullptr;
			U_LOG_W("D3D11 hub client-presents: stage A2 (direct) took %.1f ms",
			        (double)(os_monotonic_get_ns() - t0) / 1.0e6);
			return true;
		}
		U_LOG_W("D3D11 hub client-presents: DIRECT ingress unavailable (%s) — not wired (#2370)",
		        (s->stable_atlas || s->ring_direct) ? "fence wiring failed (or a keyed-mutex deposit)"
		                                            : "the producer does not honour the DIRECT contract");
		return false;
	}
	if (!s->same_adapter) {
		s->ingress_reason = COMP_HUB_INGRESS_REASON_CROSS_ADAPTER;
	} else if (split_direct_forced_off()) {
		s->ingress_reason = COMP_HUB_INGRESS_REASON_FORCED;
	} else if (!s->stable_atlas && !s->ring_direct) {
		s->ingress_reason = COMP_HUB_INGRESS_REASON_PRODUCER;
	} else if (!s->stable_atlas && !split_direct_forced_on()) {
		/*
		 * #2317: DIRECT is OPT-IN for the ring producers (the D3D12 reroute,
		 * the VK split) until a single-dGPU desktop has run VK fence-mode
		 * DIRECT and Unreal; the default flip is a one-line follow-up here.
		 * The D3D11 shim's stable atlas keeps DIRECT: DXR_HUB=1 is itself the
		 * opt-in, and that path is measured.
		 */
		s->ingress_reason = COMP_HUB_INGRESS_REASON_DEFAULT_XBRIDGE;
	} else if (split_wire_direct(s, handoff)) {
		s->ingress_reason = nullptr;
		U_LOG_W("D3D11 hub: stage A2 (direct) took %.1f ms", (double)(os_monotonic_get_ns() - t0) / 1.0e6);
		return true;
	} else {
		s->ingress_reason = COMP_HUB_INGRESS_REASON_DIRECT_FAILED;
		U_LOG_W("#2317 hub: DIRECT ingress unavailable on this device — taking the comp_xbridge route");
	}

	const bool ok = split_wire_xbridge(s, handoff->dxgi_format, handoff->width, handoff->height);
	if (ok) {
		U_LOG_W("VK output-device split: stage A2 (bridge) took %.1f ms",
		        (double)(os_monotonic_get_ns() - t0) / 1.0e6);
	}
	return ok;
}

extern "C" void
comp_hub_d3d11_destroy(struct comp_hub_d3d11 **split_ptr)
{
	struct comp_hub_d3d11 *s = *split_ptr;
	if (s == nullptr) {
		return;
	}
	*split_ptr = nullptr;

	U_LOG_W(
	    "D3D11 hub: %llu frames bridged, %llu weaves with no usable slot, %llu stale-slot "
	    "refusals, %llu in-flight (transition) weaves, %llu output-device crops, %llu composites, %llu "
	    "composite bails (#918)",
	    (unsigned long long)s->seq, (unsigned long long)s->no_slot, (unsigned long long)s->stale_refusals,
	    (unsigned long long)s->inflight_weaves, (unsigned long long)s->out_crop, (unsigned long long)s->composites,
	    (unsigned long long)s->composite_bails);

	split_release_out(s);
	split_destroy_mutexes(s);
	free(s);
}

extern "C" void
comp_hub_d3d11_retire(struct comp_hub_d3d11 **split_ptr, const char *why, const char *short_reason)
{
	struct comp_hub_d3d11 *s = *split_ptr;
	if (s == nullptr) {
		return;
	}

	U_LOG_W("#918 output-device split RETIRED (%s) — moving the weave back to the app adapter for the rest "
	        "of the session (#918 VK-1)",
	        why);

	/*
	 * The placement CORRECTION, emitted before the teardown so it cannot be
	 * lost to a failure below. The contract the D3D12 leg set and this keeps:
	 * the LAST `weave placement:` line in a log is always the truth.
	 *
	 * Deliberately does NOT re-resolve the adapters — that would be a
	 * QueryDisplayConfig on the frame path with no new information to gain.
	 */
	U_LOG_W("weave placement: CHANGED — weave/present move back to the RENDER adapter for the rest of this "
	        "session (split=0 reason=%s) (#918)",
	        short_reason != nullptr ? short_reason : COMP_SPLIT_REASON_STAGE_A_FAILED);

	comp_hub_d3d11_destroy(split_ptr);
}


/*
 *
 * Frame path.
 *
 */

//! #1264 — the caller's 2D-plane format, defaulting to the VK deposit's BGRA
//! for every session that never says otherwise (zero-init reads as unset).
static inline DXGI_FORMAT
split_plane_fmt(struct comp_hub_d3d11 *s)
{
	return (s->plane_fmt != DXGI_FORMAT_UNKNOWN) ? s->plane_fmt : DXGI_FORMAT_B8G8R8A8_UNORM;
}

extern "C" void
comp_hub_d3d11_set_plane_format(struct comp_hub_d3d11 *s, uint32_t dxgi_format)
{
	if (s == nullptr) {
		return;
	}
	// Before the first bind, by contract (see the header): a chain already
	// created in another family would need a drain+re-open this setter
	// deliberately does not do.
	s->plane_fmt = (DXGI_FORMAT)dxgi_format;
}

/*
 * #2317 P2b — the plane TRANSPORT, picked by the ingress. Everything above the
 * transport (the recipe, the raster, the composite, the wish publish) is one
 * code path; only "how does a producer plane surface become an SRV on this
 * device" differs:
 *
 *  - comp_xbridge ingress: the bridge's plane chains (Option I producer read,
 *    copy into the slot's egress plane), exactly as the VK and D3D12 legs ride.
 *  - DIRECT ingress (same adapter, the D3D11 tier): the producer's NT-shared
 *    surface opened on THIS device and read in place. Ordered by the atlas's
 *    own two fences, because the producer writes its planes on the same
 *    immediate context, before the producer signal, and every read here is
 *    recorded before the consumer signal.
 */

//! True when some plane transport exists on this route.
static inline bool
split_has_plane_transport(struct comp_hub_d3d11 *s)
{
	return s->xbridge != nullptr || s->direct.active;
}

/*!
 * DIRECT: open @p handle on the hub device (re-opened only on a new handle or
 * generation, so the steady state is a compare). The SRV is created in @p fmt —
 * the producer's scratch is TYPELESS when it carries an `_SRGB` RTV sibling
 * (#1795), so the texture's own format cannot be used to view it.
 */
static bool
split_direct_bind_plane(struct comp_hub_d3d11 *s, uint32_t plane, void *handle, uint64_t generation, DXGI_FORMAT fmt)
{
	if (plane >= COMP_XBRIDGE_PLANE_COUNT || handle == nullptr) {
		return false;
	}
	auto &p = s->direct.planes[plane];
	if (p.srv != nullptr && p.handle == (HANDLE)handle && p.gen == generation) {
		return true;
	}
	if (p.failed_handle == (HANDLE)handle && p.failed_gen == generation) {
		return false; // this exact surface already refused; the caller names the rung
	}
	if (p.srv != nullptr) {
		p.srv->Release();
	}
	if (p.tex != nullptr) {
		p.tex->Release();
	}
	p = {};

	ID3D11Device1 *od1 = nullptr;
	HRESULT hr = E_NOINTERFACE;
	if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&od1)) && od1 != nullptr) {
		hr = od1->OpenSharedResource1((HANDLE)handle, __uuidof(ID3D11Texture2D), (void **)&p.tex);
		od1->Release();
	}
	if (SUCCEEDED(hr) && p.tex != nullptr && split_refuse_typed_srgb(s, p.tex, "2D plane")) {
		hr = E_INVALIDARG;
	}
	if (SUCCEEDED(hr) && p.tex != nullptr) {
		D3D11_TEXTURE2D_DESC td = {};
		p.tex->GetDesc(&td);
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		// #2342 P2c (ADR-044): the non-decoding member, whatever was asked.
		sd.Format = split_nondecoding_format(fmt);
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		hr = s->out_dev->CreateShaderResourceView(p.tex, &sd, &p.srv);
		p.w = td.Width;
		p.h = td.Height;
	}
	if (FAILED(hr) || p.srv == nullptr) {
		if (p.tex != nullptr) {
			p.tex->Release();
		}
		p = {};
		p.failed_handle = (HANDLE)handle;
		p.failed_gen = generation;
		// On a (re)bind only, never per frame: a failed open is retried only
		// when the producer hands a new handle or generation.
		U_LOG_W(
		    "#2317 P2b hub: plane %u would not open on the hub device (hr=0x%08lx) — that plane "
		    "does not composite on this route",
		    plane, (unsigned long)hr);
		return false;
	}
	p.handle = (HANDLE)handle;
	p.gen = generation;
	return true;
}

//! Bind a producer plane on whichever transport this route has.
static bool
split_plane_bind(struct comp_hub_d3d11 *s,
                 uint32_t plane,
                 void *handle,
                 uint64_t generation,
                 DXGI_FORMAT fmt,
                 uint32_t alloc_w,
                 uint32_t alloc_h)
{
	if (s->direct.active) {
		return split_direct_bind_plane(s, plane, handle, generation, fmt);
	}
	return comp_xbridge_bind_plane(s->xbridge, plane, handle, generation, (uint32_t)fmt, alloc_w, alloc_h);
}

//! Stage (or, with @p seq 0, un-stage) a plane for the next submit.
static void
split_plane_stage(
    struct comp_hub_d3d11 *s, uint32_t plane, uint64_t seq, int32_t dx, int32_t dy, uint32_t dw, uint32_t dh)
{
	if (s->direct.active) {
		if (plane < COMP_XBRIDGE_PLANE_COUNT) {
			s->direct.planes[plane].seq = seq;
		}
		return;
	}
	comp_xbridge_stage_plane(s->xbridge, plane, seq, dx, dy, dw, dh);
}

/*
 * #2317 P2b — the consume-half RESOLVERS: "this frame's recipe" and "this
 * frame's plane", from an xbridge egress slot or from the one DIRECT atlas. The
 * composite, the backdrop hand-off and the wish publish read only these, so both
 * routes run the same code (@p slot is ignored on DIRECT).
 */

//! The recipe of the frame @p slot holds (xbridge), or of the DIRECT atlas.
static bool
split_frame_recipe(struct comp_hub_d3d11 *s, int32_t slot, struct comp_xbridge_recipe *out)
{
	if (s->direct.active) {
		if (!s->direct.have_frame) {
			return false;
		}
		*out = s->direct.rec;
		return true;
	}
	if (slot < 0) {
		return false;
	}
	return comp_xbridge_slot_recipe(s->xbridge, slot, out);
}

/*!
 * The SRV of @p plane for the frame whose recipe carried @p seq, or NULL. On
 * DIRECT the recipe's valid bit is the proof (the frame staged that plane and
 * the surface is the producer's own, never rewritten under a weave); on xbridge
 * the bridge proves @p seq against the slot (#918 review F3).
 */
static ID3D11ShaderResourceView *
split_frame_plane_srv(struct comp_hub_d3d11 *s, int32_t slot, uint32_t plane, uint64_t seq)
{
	if (s->direct.active) {
		return plane < COMP_XBRIDGE_PLANE_COUNT ? s->direct.planes[plane].srv : nullptr;
	}
	return static_cast<ID3D11ShaderResourceView *>(comp_xbridge_get_plane_srv(s->xbridge, slot, plane, seq));
}

//! The allocated extent of @p plane's surface on this device.
static bool
split_frame_plane_extent(struct comp_hub_d3d11 *s, uint32_t plane, uint32_t *out_w, uint32_t *out_h)
{
	if (s->direct.active) {
		if (plane >= COMP_XBRIDGE_PLANE_COUNT || s->direct.planes[plane].srv == nullptr) {
			return false;
		}
		*out_w = s->direct.planes[plane].w;
		*out_h = s->direct.planes[plane].h;
		return true;
	}
	return comp_xbridge_plane_extent(s->xbridge, plane, out_w, out_h);
}

/*!
 * This frame's composite recipe from the staged CPU state — the one stamp the
 * xbridge route hands to @ref comp_xbridge_stage_recipe and the DIRECT route
 * snapshots beside its atlas.
 */
static void
split_build_recipe(struct comp_hub_d3d11 *s, uint32_t content_w, uint32_t content_h, struct comp_xbridge_recipe *r)
{
	*r = {};
	r->composite = s->l2d_plane_live;
	r->region_w = s->l2d_plane_live ? s->comp_region_w : content_w;
	r->region_h = s->l2d_plane_live ? s->comp_region_h : content_h;
	r->bd_w = s->bd_w;
	r->bd_h = s->bd_h;
	if (s->l2d_plane_live) {
		r->composite_mode = s->comp_mode;
		r->mask_kind = s->comp_mask_kind;
		r->opaque_present = s->comp_opaque;
		r->cx = s->comp_cx;
		r->cy = s->comp_cy;
		r->cw = s->comp_cw;
		r->ch = s->comp_ch;
	}
}

/*!
 * VK-1b — hand the display processor the weaving frame's 2D-under backdrop, from
 * that frame's own recipe rather than live CPU state (#1140). A NULL SRV is the
 * correct clear: the frame produced no 2D-under layers, and the display
 * processor falls back to the captured desktop alone. Must precede process_atlas.
 */
static void
split_set_backdrop(struct comp_hub_d3d11 *s, int32_t slot)
{
	struct comp_xbridge_recipe rec = {};
	ID3D11ShaderResourceView *bd_srv = nullptr;
	uint32_t bd_w = 0, bd_h = 0;
	if (split_frame_recipe(s, slot, &rec) && rec.bd_w > 0 && rec.bd_h > 0 &&
	    (rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_BACKDROP)) != 0) {
		const uint64_t bd_seq = rec.plane_seq[COMP_XBRIDGE_PLANE_BACKDROP];
		bd_srv = split_plane_dp_view(s, &s->dp_bd_view,
		                             split_frame_plane_srv(s, slot, COMP_XBRIDGE_PLANE_BACKDROP, bd_seq),
		                             bd_seq, rec.bd_w, rec.bd_h, split_plane_fmt(s), "backdrop publish");
		if (bd_srv != nullptr) {
			bd_w = rec.bd_w;
			bd_h = rec.bd_h;
		}
	}
	/*
	 * On a change of what the DP is handed (appear / resize / withdraw) only —
	 * and only the session's FIRST appearance is a WARN: an app that toggles an
	 * under-layer every other frame would otherwise log at frame rate.
	 */
	if (bd_w != s->bd_handed_w || bd_h != s->bd_handed_h) {
		s->bd_handed_w = bd_w;
		s->bd_handed_h = bd_h;
		if (bd_w != 0 && !s->bd_handed_logged) {
			s->bd_handed_logged = true;
			U_LOG_W("#2317 P2b hub: 2D-under backdrop handed to the DP: %ux%u (%s)", bd_w, bd_h,
			        s->direct.active ? "direct" : "xbridge");
		} else {
			U_LOG_I("#2317 P2b hub: 2D-under backdrop handed to the DP: %ux%u", bd_w, bd_h);
		}
	}
	xrt_display_processor_d3d11_set_background_2d(s->dp, bd_srv, bd_w, bd_h);
}

extern "C" bool
comp_hub_d3d11_stage_backdrop(struct comp_hub_d3d11 *s,
                              void *nt_handle,
                              uint64_t generation,
                              uint32_t alloc_w,
                              uint32_t alloc_h,
                              uint64_t content_seq,
                              int32_t dirty_x,
                              int32_t dirty_y,
                              uint32_t dirty_w,
                              uint32_t dirty_h,
                              uint32_t bd_w,
                              uint32_t bd_h)
{
	// #2317 P2a: real backdrop content on a RING producer that did not declare
	// the plane contract needs the xbridge ingress (P2c: one that did keeps it).
	if (s != nullptr && nt_handle != nullptr && content_seq != 0 && bd_w != 0 && bd_h != 0) {
		split_direct_demote_for_planes(s);
	}
	if (s == nullptr || !split_has_plane_transport(s)) {
		return false;
	}

	/*
	 * 0 is the bridge's "this frame does not use the plane", and it is what the
	 * recipe then stamps invalid — which is the honest answer rather than
	 * lending the weave an older frame's backdrop.
	 */
	if (nt_handle == nullptr || content_seq == 0 || bd_w == 0 || bd_h == 0) {
		split_plane_stage(s, COMP_XBRIDGE_PLANE_BACKDROP, 0, 0, 0, 0, 0);
		s->bd_plane_live = false;
		s->bd_w = 0;
		s->bd_h = 0;
		return true; // nothing asked for, nothing failed
	}

	/*
	 * Panel-sized, always. The steady-state call is the handle+generation
	 * early-out inside the bridge; a generation change re-opens the handle and
	 * drains the producer first, which is why the surface is allocated once at
	 * the panel and never at the region.
	 */
	if (!split_plane_bind(s, COMP_XBRIDGE_PLANE_BACKDROP, nt_handle, generation, split_plane_fmt(s), alloc_w,
	                      alloc_h)) {
		/*
		 * The backdrop degrades on its OWN — a session without one simply has
		 * no 2D-under band, and the 3D weave is untouched. The VK and D3D12
		 * legs keep the scanout adapter; the D3D11 tier reads the false and
		 * names it (#2317 P2b `plane_unavailable`).
		 */
		split_plane_stage(s, COMP_XBRIDGE_PLANE_BACKDROP, 0, 0, 0, 0, 0);
		s->bd_plane_live = false;
		s->bd_w = 0;
		s->bd_h = 0;
		return false;
	}

	split_plane_stage(s, COMP_XBRIDGE_PLANE_BACKDROP, content_seq, dirty_x, dirty_y, dirty_w, dirty_h);
	s->bd_plane_live = true;
	s->bd_w = bd_w;
	s->bd_h = bd_h;
	return true;
}

extern "C" void
comp_hub_d3d11_invalidate_plane(struct comp_hub_d3d11 *s, uint32_t plane)
{
	// DIRECT has no slots holding older copies of a plane: the one surface IS
	// the producer's, so there is nothing to invalidate.
	if (s == nullptr || s->xbridge == nullptr) {
		return;
	}
	comp_xbridge_invalidate_plane(s->xbridge, plane);
}

static bool
split_raster_into(struct comp_hub_d3d11 *s,
                  struct comp_hub_d3d11::split_mask_raster *m,
                  uint32_t kind,
                  const struct xrt_rect *rects,
                  const float *feather_px,
                  uint32_t rect_count,
                  uint32_t region_w,
                  uint32_t region_h)
{
	// Dirty-check on everything the raster is a function of. The steady-state
	// frame reuses the staged SRV and touches the GPU not at all.
	bool dirty =
	    m->srv == nullptr || m->w != region_w || m->h != region_h || m->kind != kind || m->rect_count != rect_count;
	for (uint32_t i = 0; !dirty && i < rect_count; i++) {
		if (memcmp(&m->rects[i], &rects[i], sizeof(rects[i])) != 0) {
			dirty = true;
		}
		if (kind == COMP_HUB_D3D11_MASK_ZONE_FEATHER && feather_px != nullptr &&
		    m->feather[i] != feather_px[i]) {
			dirty = true;
		}
	}
	if (!dirty) {
		return true;
	}

	if (!split_mask_ensure(s, m, region_w, region_h)) {
		return false;
	}

	ID3D11DeviceContext1 *ctx1 = nullptr;
	if (FAILED(s->out_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1)) || ctx1 == nullptr) {
		if (!m->warned) {
			m->warned = true;
			U_LOG_W(
			    "#918 VK-1b: the out device has no ID3D11DeviceContext1, so the mask raster has no "
			    "ClearView — 2D content does not composite under the split for this session");
		}
		return false;
	}

	/*
	 * The three rasters, and all three are pure CLEARS — no shader, no draw.
	 * That is what makes "rebuild the mask where it is consumed" cheap enough to
	 * be the rule rather than a transport.
	 *
	 * TODO(#1178 VK-1b): duplicates of `d3d11_update_implicit_mask`
	 * (src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp:5502),
	 * `d3d11_update_zone_wish_mask` (:5652) and `d3d11_update_zone_feather_mask`
	 * (:5820). Those are `static` and wired to `struct comp_d3d11_compositor`;
	 * extracting all four into one unit touches the shipped D3D11 leg, so it is
	 * tracked as its own change rather than folded into new Vulkan transport.
	 */
	const float base = (kind == COMP_HUB_D3D11_MASK_IMPLICIT) ? 1.0f : 0.0f;
	const float all[4] = {base, 0.0f, 0.0f, 0.0f};
	s->out_ctx->ClearRenderTargetView(m->rtv, all);

	for (uint32_t i = 0; i < rect_count; i++) {
		if (kind == COMP_HUB_D3D11_MASK_IMPLICIT) {
			// Inverse of the zone raster: M=0 (show the flattened 2D) inside
			// each Local2D rect, M=1 (keep the weave) everywhere else.
			split_mask_fill(s, m, ctx1, &rects[i], 0, 0.0f);
			continue;
		}
		if (kind == COMP_HUB_D3D11_MASK_ZONE_BINARY) {
			split_mask_fill(s, m, ctx1, &rects[i], 0, 1.0f);
			continue;
		}

		/*
		 * #803 — the per-zone inward ramp, by the rings idiom: ascending value
		 * WITH ascending inset, so later, deeper, higher-value clears overwrite
		 * the inner part of earlier ones. The edge keeps the low values and the
		 * core reaches 1. 2px steps, capped at a 64px ramp (beyond which the
		 * step widens instead), and the inset is clamped so a small zone's
		 * centre still reaches 1.
		 */
		const float radius = (feather_px != nullptr) ? feather_px[i] : 0.0f;
		const bool feathered = radius > 0.0f;
		int32_t steps = 1;
		int32_t step_px = 0;
		if (feathered) {
			step_px = 2;
			steps = (int32_t)(radius / (float)step_px + 0.5f);
			if (steps < 1) {
				steps = 1;
			}
			if (steps > 32) {
				step_px = (int32_t)(radius / 32.0f + 0.5f);
				steps = 32;
			}
		}
		const int32_t min_ext = rects[i].extent.w < rects[i].extent.h ? rects[i].extent.w : rects[i].extent.h;
		int32_t max_inset = (min_ext - 1) / 2;
		if (max_inset < 0) {
			max_inset = 0;
		}
		for (int32_t st = 1; st <= steps; st++) {
			const float v = (float)st / (float)steps; // 1.0 for the hard single step
			int32_t inset = feathered ? st * step_px : 0;
			if (inset > max_inset) {
				inset = max_inset;
			}
			split_mask_fill(s, m, ctx1, &rects[i], inset, v);
		}
	}
	ctx1->Release();

	// Stage the snapshot the composite samples (RT is not an SRV).
	s->out_ctx->CopyResource(m->staged, m->tex);

	m->kind = kind;
	m->rect_count = rect_count;
	memcpy(m->rects, rects, sizeof(rects[0]) * rect_count);
	if (feather_px != nullptr) {
		memcpy(m->feather, feather_px, sizeof(feather_px[0]) * rect_count);
	} else {
		memset(m->feather, 0, sizeof(m->feather));
	}

	// On a rect/dims change only, never per frame.
	U_LOG_W("#918 VK-1b: out-device mask raster kind=%u %ux%u, %u rect(s)", kind, region_w, region_h, rect_count);
	return true;
}

extern "C" bool
comp_hub_d3d11_raster_mask(struct comp_hub_d3d11 *s,
                           uint32_t kind,
                           const struct xrt_rect *rects,
                           const float *feather_px,
                           uint32_t rect_count,
                           uint32_t region_w,
                           uint32_t region_h)
{
	if (s == nullptr || s->out_dev == nullptr || region_w == 0 || region_h == 0) {
		return false;
	}
	if (kind == COMP_HUB_D3D11_MASK_NONE || rect_count == 0 || rects == nullptr) {
		s->repaint_mask_srv = nullptr;
		return false;
	}
	if (rect_count > XRT_MAX_LAYERS) {
		rect_count = XRT_MAX_LAYERS;
	}
	const bool ok = split_raster_into(s, &s->mask, kind, rects, feather_px, rect_count, region_w, region_h);
	s->repaint_mask_srv = ok ? s->mask.srv : nullptr;
	/*
	 * #2317 P2b review: a FEATHERED zones frame also rasters its BINARY wish,
	 * for the publish — the composite's ramp never reaches the vendor
	 * (#800/#803), and publishing nothing would leave the vendor holding a stale
	 * wish while the zone moves. A failure here costs the publish only.
	 */
	if (ok && kind == COMP_HUB_D3D11_MASK_ZONE_FEATHER &&
	    !split_raster_into(s, &s->wish, COMP_HUB_D3D11_MASK_ZONE_BINARY, rects, nullptr, rect_count, region_w,
	                       region_h)) {
		s->wish.kind = COMP_HUB_D3D11_MASK_NONE;
	}
	return ok;
}

/*!
 * The masked composite unit, on the OUT device.
 *
 * LAZY, following the D3D12 leg rather than the D3D11 one: the create compiles
 * shaders, and paying that inside Stage A would add it to the session-warmup
 * critical path for every split session, including the projection-only ones that
 * never composite anything. A failure here is feature-local — one latched WARN,
 * `composite=false` on the recipe, projection-only frames — and never a retire.
 */
static bool
split_ensure_outcomp(struct comp_hub_d3d11 *s)
{
	if (s->outcomp != nullptr) {
		return true;
	}
	if (s->outcomp_failed) {
		return false;
	}
	if (comp_d3d11_outcomp_create(s->out_dev, s->out_ctx, &s->outcomp) != XRT_SUCCESS || s->outcomp == nullptr) {
		s->outcomp_failed = true;
		s->outcomp = nullptr;
		U_LOG_W(
		    "#918 VK-1b: the output composite unit could not be created on the scanout device — 2D "
		    "content does not composite under the split for this session; the 3D weave is unaffected");
		return false;
	}
	U_LOG_W("#918 VK-1b: output composite unit up on the scanout device");
	return true;
}

extern "C" void
comp_hub_d3d11_stage_no_composite(struct comp_hub_d3d11 *s)
{
	if (s == nullptr || !split_has_plane_transport(s)) {
		return;
	}
	split_plane_stage(s, COMP_XBRIDGE_PLANE_LOCAL2D, 0, 0, 0, 0, 0);
	s->l2d_plane_live = false;
}

extern "C" bool
comp_hub_d3d11_stage_local2d(struct comp_hub_d3d11 *s,
                             void *nt_handle,
                             uint64_t generation,
                             uint32_t alloc_w,
                             uint32_t alloc_h,
                             uint64_t content_seq,
                             int32_t dirty_x,
                             int32_t dirty_y,
                             uint32_t dirty_w,
                             uint32_t dirty_h,
                             uint32_t region_w,
                             uint32_t region_h,
                             int32_t cx,
                             int32_t cy,
                             uint32_t cw,
                             uint32_t ch,
                             uint32_t composite_mode,
                             bool opaque_present,
                             bool mask_is_plane)
{
	// #2317 P2a: a real Local2D composite on a RING producer that did not
	// declare the plane contract needs the xbridge ingress (P2c).
	if (s != nullptr && nt_handle != nullptr && content_seq != 0 && region_w != 0 && region_h != 0) {
		split_direct_demote_for_planes(s);
	}
	if (s == nullptr || !split_has_plane_transport(s)) {
		return false;
	}

	/*
	 * The unit the CONSUME half will need is created HERE rather than there,
	 * because this half stamps the recipe: a create failure stamps
	 * `composite = false` and the frame ships projection-only, where discovering
	 * it in the consume half would leave a slot marked compositable that nothing
	 * can composite.
	 */
	if (nt_handle == nullptr || content_seq == 0 || region_w == 0 || region_h == 0 || !split_ensure_outcomp(s)) {
		comp_hub_d3d11_stage_no_composite(s);
		return false;
	}

	if (!split_plane_bind(s, COMP_XBRIDGE_PLANE_LOCAL2D, nt_handle, generation, split_plane_fmt(s), alloc_w,
	                      alloc_h)) {
		/*
		 * #918 review D4 — the Local2D plane IS the composite's `twod` under the
		 * split, so a frame that could not bind it has no composite to stamp.
		 * Claiming otherwise stamps `composite=true` on a slot whose plane the
		 * submit then marks invalid, and the consume half bails on every frame
		 * afterwards with no log.
		 */
		static bool warned = false;
		if (!warned) {
			warned = true;
			U_LOG_W(
			    "#918 VK-1b: the Local2D plane could not be bound — 2D content does not composite "
			    "under the split for this session; the 3D weave is unaffected");
		}
		comp_hub_d3d11_stage_no_composite(s);
		return false;
	}

	split_plane_stage(s, COMP_XBRIDGE_PLANE_LOCAL2D, content_seq, dirty_x, dirty_y, dirty_w, dirty_h);
	s->l2d_plane_live = true;
	s->comp_region_w = region_w;
	s->comp_region_h = region_h;
	s->comp_cx = cx;
	s->comp_cy = cy;
	s->comp_cw = cw;
	s->comp_ch = ch;
	s->comp_mode = composite_mode;
	s->comp_opaque = opaque_present;
	/*
	 * VK-1b-3 — which mask the consume half will actually SAMPLE, which is the
	 * only thing this stamp may describe. A bridged Tier-3 mask reads the plane
	 * that landed with this slot; every other kind is the out-device raster.
	 */
	s->comp_mask_kind = mask_is_plane ? COMP_XBRIDGE_MASK_PLANE : COMP_XBRIDGE_MASK_OUT_RASTER;
	return true;
}

extern "C" bool
comp_hub_d3d11_bind_mask_plane(struct comp_hub_d3d11 *s, void *nt_handle, uint64_t generation, uint32_t w, uint32_t h)
{
	// #2317 P2a: an authored mask plane on a RING producer that did not
	// declare the plane contract needs the xbridge ingress (P2c).
	if (s != nullptr && nt_handle != nullptr && w != 0 && h != 0) {
		split_direct_demote_for_planes(s);
	}
	if (s == nullptr || !split_has_plane_transport(s)) {
		return true; // nothing to bind against, and nothing broken
	}
	if (nt_handle == nullptr || w == 0 || h == 0) {
		s->mask_plane_live = false;
		s->mask_plane_gen = 0;
		return true; // no authored mask this frame — not a failure
	}

	// Sized at the MASK (#918 review F5), so a dims change rebuilds the chain.
	// On-change only: the steady-state call is the bridge's own early-out.
	if (!split_plane_bind(s, COMP_XBRIDGE_PLANE_MASK, nt_handle, generation, DXGI_FORMAT_R8_UNORM, w, h)) {
		s->mask_plane_live = false;
		s->mask_plane_gen = 0;
		return false;
	}
	s->mask_plane_live = true;
	s->mask_plane_gen = generation;
	return true;
}

extern "C" void
comp_hub_d3d11_stage_mask_plane(struct comp_hub_d3d11 *s, uint64_t content_seq, uint32_t w, uint32_t h)
{
	if (s == nullptr || !split_has_plane_transport(s)) {
		return;
	}
	if (!s->mask_plane_live || content_seq == 0) {
		// 0 is the bridge's "this frame does not use the plane".
		split_plane_stage(s, COMP_XBRIDGE_PLANE_MASK, 0, 0, 0, 0, 0);
		return;
	}
	split_plane_stage(s, COMP_XBRIDGE_PLANE_MASK, content_seq, 0, 0, w, h);
}

extern "C" bool
comp_hub_d3d11_zone_dp_supported(struct comp_hub_d3d11 *s)
{
	if (s == nullptr || s->dp == nullptr) {
		return false;
	}
	if (s->zone_dp_state == 0) {
		struct xrt_dp_local_zone_caps caps = {};
		caps.struct_size = sizeof(caps);
		const bool ok = xrt_display_processor_d3d11_get_local_zone_caps(s->dp, &caps) && caps.supported != 0;
		s->zone_dp_state = ok ? 1 : 2;
		s->zone_caps = caps;
		if (ok) {
			U_LOG_W("#918 VK-1b: scanout weaver supports local zones — grid %ux%u max_mask %ux%u",
			        caps.zone_grid_width, caps.zone_grid_height, caps.max_mask_width, caps.max_mask_height);
		}
	}
	return s->zone_dp_state == 1;
}

static void
split_publish_zone_wish_now(struct comp_hub_d3d11 *s, uint64_t seq)
{
	if (s == nullptr || s->dp == nullptr || s->hwnd == nullptr || !comp_hub_d3d11_zone_dp_supported(s)) {
		return;
	}
	/*
	 * The BINARY raster and nothing else. A feather ramp is a cosmetic composite
	 * opt-in (#803) and must never reach the vendor as geometry, so a frame whose
	 * composite sampled the feather mask still publishes the binary one — which
	 * is why the publish reads the raster's KIND rather than just its SRV.
	 */
	ID3D11ShaderResourceView *srv = nullptr;
	uint32_t mw = 0, mh = 0;
	if (s->mask_plane_live) {
		/*
		 * VK-1b-3 - an APP-AUTHORED wish publishes VERBATIM, off the plane it
		 * crossed on. That is the whole point of a Tier-3 wish: the app chose the
		 * geometry and the runtime must not substitute its own. Falling back to
		 * the auto raster when the plane has not landed would publish DIFFERENT
		 * geometry for one frame, which is a flicker rather than a degradation -
		 * so a not-yet-landed authored wish publishes nothing and the vendor keeps
		 * the one it already has.
		 */
		// #2317 P2b: the weave slot on xbridge; the frame being woven on DIRECT.
		const int32_t wslot = s->direct.active ? -1 : comp_xbridge_get_weave_slot(s->xbridge);
		struct comp_xbridge_recipe rec = {};
		if (!split_frame_recipe(s, wslot, &rec) || (rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_MASK)) == 0) {
			return;
		}
		const uint64_t want = rec.plane_seq[COMP_XBRIDGE_PLANE_MASK];
		void *plane_srv = split_frame_plane_srv(s, wslot, COMP_XBRIDGE_PLANE_MASK, want);
		uint32_t pw = 0, ph = 0;
		if (plane_srv == nullptr || !split_frame_plane_extent(s, COMP_XBRIDGE_PLANE_MASK, &pw, &ph) ||
		    pw == 0 || ph == 0) {
			return;
		}
		// Normally a passthrough - the mask plane is sized AT the mask (#918
		// review F5) - but the helper still handles a publish that asks for dims
		// the plane does not have, seq-gated and clamped.
		srv = split_plane_dp_view(s, &s->dp_mask_view, plane_srv, want, pw, ph, DXGI_FORMAT_R8_UNORM,
		                          "zone-mask publish");
		mw = pw;
		mh = ph;
	} else {
		/*
		 * The BINARY raster and nothing else. A feather ramp is a cosmetic
		 * composite opt-in (#803) and must never reach the vendor as geometry, so
		 * a frame whose composite sampled the feather mask still publishes the
		 * binary one - which is why this reads the raster's KIND, not just its SRV.
		 */
		// #2317 P2b review: a feathered frame publishes its separate binary wish.
		const struct comp_hub_d3d11::split_mask_raster *wm =
		    (s->mask.kind == COMP_HUB_D3D11_MASK_ZONE_BINARY) ? &s->mask
		    : (s->mask.kind == COMP_HUB_D3D11_MASK_ZONE_FEATHER &&
		       s->wish.kind == COMP_HUB_D3D11_MASK_ZONE_BINARY)
		        ? &s->wish
		        : nullptr;
		if (wm == nullptr || wm->srv == nullptr) {
			return;
		}
		srv = wm->srv;
		mw = wm->w;
		mh = wm->h;
	}
	if (srv == nullptr || mw == 0 || mh == 0) {
		return;
	}

	RECT r;
	POINT origin = {0, 0};
	if (!GetClientRect(s->hwnd, &r) || r.right <= 0 || r.bottom <= 0 || !ClientToScreen(s->hwnd, &origin)) {
		return;
	}
	if (xrt_display_processor_d3d11_publish_local_zone_mask(s->dp, s->out_ctx, srv, mw, mh, (int32_t)origin.x,
	                                                        (int32_t)origin.y, (uint32_t)r.right,
	                                                        (uint32_t)r.bottom, seq)) {
		s->zone_published = true;
	}
	/*
	 * #2342 P2c (#2218 #4, the native D3D11 rule): a window spanning screens —
	 * every segment DP gets the mask cropped to its screen, so a 2D zone on
	 * screen B stays 2D there even when screen A has a 3D zone (the vendor's
	 * verdict is per panel). The manager lives on THIS device, and @p srv is
	 * this device's (an out-device raster or plane view), so the crop is one
	 * CopySubresourceRegion per segment. A screen the window has left is
	 * cleared inside. seg_mutex: the app thread reads the manager for eyes;
	 * every caller of this is serialised with the weave by the compositor
	 * mutex, which is what serialises the manager's rebuild.
	 */
	if (s->mutexes_init) {
		os_mutex_lock(&s->seg_mutex);
		comp_d3d11_segments_publish_zone_mask(s->segments, s->out_ctx, srv, mw, mh, (uint32_t)r.right,
		                                      (uint32_t)r.bottom, seq);
		os_mutex_unlock(&s->seg_mutex);
	}
}

extern "C" bool
comp_hub_d3d11_get_zone_grid(struct comp_hub_d3d11 *s, uint32_t *out_grid_w, uint32_t *out_grid_h)
{
	if (!comp_hub_d3d11_zone_dp_supported(s)) {
		return false;
	}
	if (out_grid_w != nullptr) {
		*out_grid_w = s->zone_caps.zone_grid_width;
	}
	if (out_grid_h != nullptr) {
		*out_grid_h = s->zone_caps.zone_grid_height;
	}
	return true;
}

extern "C" void
comp_hub_d3d11_publish_zone_wish(struct comp_hub_d3d11 *s, uint64_t seq)
{
	if (s == nullptr) {
		return;
	}
	/*
	 * #2317 P2b — DIRECT defers to the frame's own weave (see
	 * comp_hub_d3d11::direct.publish_pending): the plane this samples is the
	 * producer's surface, written before the producer's signal and ordered for
	 * THIS context only by the weave's wait on it. The publish is a per-frame
	 * sideband, so running it a few hundred microseconds later in the same app
	 * frame changes nothing the vendor can see.
	 */
	if (s->direct.active) {
		s->direct.publish_pending = true;
		s->direct.publish_seq = seq;
		return;
	}
	split_publish_zone_wish_now(s, seq);
}

extern "C" void
comp_hub_d3d11_clear_zone_wish(struct comp_hub_d3d11 *s)
{
	if (s == nullptr) {
		return;
	}
	s->direct.publish_pending = false; // a withdrawn wish owes no publish
	// #2342 P2c (#2218 #4): every segment DP that holds a published crop too.
	// Idempotent; a manager with nothing published is a no-op.
	if (s->mutexes_init) {
		os_mutex_lock(&s->seg_mutex);
		comp_d3d11_segments_clear_zone_mask(s->segments);
		os_mutex_unlock(&s->seg_mutex);
	}
	if (s->dp == nullptr || !s->zone_published) {
		return;
	}
	xrt_display_processor_d3d11_clear_local_zone_mask(s->dp);
	s->zone_published = false;
}

extern "C" void
comp_hub_d3d11_set_hud(struct comp_hub_d3d11 *s, const void *pixels, uint32_t w, uint32_t h, bool dirty)
{
	if (s == nullptr || s->out_dev == nullptr) {
		return;
	}
	if (pixels == nullptr || w == 0 || h == 0) {
		s->hud_live = false;
		return;
	}

	if (s->hud_tex == nullptr || s->hud_w != w || s->hud_h != h) {
		if (s->hud_tex != nullptr) {
			s->hud_tex->Release();
			s->hud_tex = nullptr;
		}
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		if (FAILED(s->out_dev->CreateTexture2D(&td, nullptr, &s->hud_tex)) || s->hud_tex == nullptr) {
			s->hud_tex = nullptr;
			s->hud_live = false;
			return;
		}
		s->hud_w = w;
		s->hud_h = h;
		dirty = true; // a fresh texture holds nothing
	}
	if (dirty) {
		s->out_ctx->UpdateSubresource(s->hud_tex, 0, nullptr, pixels, w * 4u, 0);
	}
	s->hud_live = true;
}

extern "C" void
comp_hub_d3d11_submit_atlas(struct comp_hub_d3d11 *s,
                            const struct comp_hub_d3d11_handoff *handoff,
                            uint32_t cols,
                            uint32_t rows,
                            uint32_t view_w,
                            uint32_t view_h)
{
	if (s == nullptr || (s->xbridge == nullptr && !s->direct.active) || handoff == nullptr ||
	    handoff->texture == nullptr) {
		return;
	}

	/*
	 * THE ordering edge of this rung. Vulkan's atlas submit signalled the
	 * shared fence at `fence_value`; the bridge's staging copy is about to read
	 * that texture on this very context. A GPU-side wait — this thread does not
	 * block, and neither does the app's render thread.
	 */
	// #2317 P1b: a stable atlas is written on this very context (or, DIRECT, the
	// fence is the hub's to wait on from its own context), so there is nothing to
	// order here.
	/*
	 * #2317 P2a review: a ring producer's frame is vetted for DIRECT before
	 * anything is recorded — a slot that will not open (or does not hold the
	 * content box), or a producer that broke the ring contract, moves the
	 * session to xbridge here, and this very frame is then bridged whole.
	 */
	if (s->direct.active && s->direct.ring) {
		split_direct_ring_vet(s, handoff, cols * view_w, rows * view_h);
	}

	// #2317 P2a: nor for a DIRECT ring producer — the hub waits on the
	// producer's fence from its OWN context, at the weave.
	if (!s->stable_atlas && !s->direct.active && s->app_ctx4 != nullptr && handoff->fence != nullptr &&
	    handoff->fence_value != 0) {
		s->app_ctx4->Wait((ID3D11Fence *)handoff->fence, handoff->fence_value);
	}

	const uint32_t content_w = cols * view_w;
	const uint32_t content_h = rows * view_h;
	if (content_w == 0 || content_h == 0) {
		return;
	}

	/*
	 * #918 R1 — the layout GENERATION.
	 *
	 * Bumped only when the tile GRID changes, deliberately not when the tile
	 * dimensions do: those move continuously through an interactive resize, and
	 * treating every step as a mode switch would refuse every slot. A grid
	 * change is a real recipe change and a slot composited under the old one
	 * must never be woven. The geometry snapshot is minted WITH the generation
	 * (#1140) so the weave reads its grid from the slot's own era.
	 */
	const uint64_t sig = ((uint64_t)cols << 20) | (uint64_t)rows;
	if (sig != s->layout_sig) {
		s->layout_sig = sig;
		s->layout_gen++;
		s->gen_cols = cols;
		s->gen_rows = rows;
		s->gen_view_w = view_w;
		s->gen_view_h = view_h;
		U_LOG_W("#918: layout generation %llu — %ux%u tiles of %ux%u (#918 R1)",
		        (unsigned long long)s->layout_gen, cols, rows, view_w, view_h);
	} else {
		// Same recipe, possibly a new scale. The snapshot tracks the tile size
		// so a resize weaves at the size the slot was actually painted at.
		s->gen_view_w = view_w;
		s->gen_view_h = view_h;
	}

	struct comp_xbridge_recipe r = {};
	split_build_recipe(s, content_w, content_h, &r);

	// #2317 P1b: DIRECT ingress has no transport to size, nominate or submit
	// (P2b: the recipe rides beside the one atlas instead of in a slot).
	if (s->direct.active) {
		split_submit_direct(s, handoff, content_w, content_h, &r);
		return;
	}

	/*
	 * CROP-BEFORE-DP (ADR-030), done by the transport itself: right-sizing the
	 * egress ring to the content box makes the consumer's copy land
	 * content-sized, so the bridge delivers CROPPED pixels and no scratch copy
	 * happens anywhere. Only the R2 hysteresis (an interactive resize whose size
	 * will not hold still) leaves a worst-case ring, and that residual crop runs
	 * on the OUTPUT device in the weave below.
	 */
	comp_xbridge_set_content_size(s->xbridge, content_w, content_h, s->layout_gen);

	/*
	 * #1140 — the recipe travels with the pixels. `composite` stays false until
	 * VK-1b-2 puts the masked composite on the output device; what this rung
	 * adds is the BACKDROP's own extent, which the recipe carries separately
	 * from the composite for exactly this case: a frame can flatten a backdrop
	 * and run no composite at all, and `set_background_2d` still needs its real
	 * width and height. A consume half can never read a slot claiming pixels it
	 * does not have — every `plane_valid` test fails closed.
	 */
	comp_xbridge_stage_recipe(s->xbridge, &r);

	/*
	 * KEYED-MUTEX mode (ADR-039 Phase A): no importable fence on this driver,
	 * so the ingress copy is bracketed by the slot's mutex instead — key 0 on
	 * both sides, plain mutual exclusion against Vulkan's next rewrite of the
	 * slot. The acquire is BOUNDED and a timeout SKIPS the frame (the fill arm
	 * keeps re-weaving the previous egress slot): never an unbounded wait on
	 * this thread (#925). Ordering of write-before-read is carried by the
	 * frame path's per-frame CPU wait after the atlas submit (#837), so a
	 * successful acquire here always sees a complete atlas.
	 */
	IDXGIKeyedMutex *km = (IDXGIKeyedMutex *)handoff->keyed_mutex;
	if (km != nullptr) {
		const HRESULT km_hr = km->AcquireSync(0, 100);
		if (km_hr != S_OK) {
			// WAIT_ABANDONED: acquired after all (the last owner died holding
			// it) — give it back, or the producer's next acquire times out.
			if (km_hr == (HRESULT)WAIT_ABANDONED) {
				km->ReleaseSync(0);
			}
			static uint32_t km_skips = 0;
			if ((km_skips++ % 300u) == 0u) {
				U_LOG_W(
				    "#1264: deposit keyed-mutex AcquireSync did not resolve (hr=0x%08lx) — "
				    "atlas ingress skipped this frame (%u skips so far)",
				    (unsigned long)km_hr, km_skips);
			}
			return;
		}
	}
	s->seq++;
	// #1883: the slot this fills carries the frame's segment partition.
	comp_segments_content_ring_put(&s->seg_content_ring, s->seq, &s->seg_content_staged);
	// #2317 P1b: a stable atlas is bound Option I (re-opened only on a new
	// generation; a failed open latches the staged ring by itself).
	if (s->stable_atlas && handoff->shared_handle != nullptr) {
		comp_xbridge_bind_atlas(s->xbridge, handoff->shared_handle, handoff->generation);
	}
	comp_xbridge_submit(s->xbridge, s->seq, s->layout_gen, handoff->texture, content_w, content_h);
	if (km != nullptr) {
		km->ReleaseSync(0);
	}

	/*
	 * VK-1b — THE PLANE BACK-FENCE, and the one place this leg diverges from
	 * the two D3D ones.
	 *
	 * A plane's ingress is Option I: the producer's copy queue opened the
	 * app-device texture above and reads it in place. `pre_plane_write` makes
	 * the app IMMEDIATE CONTEXT wait for that read — which is all the D3D legs
	 * need, because the immediate context is also what WRITES their scratch.
	 * Here the plane is written by the Vulkan queue, so this wait on its own
	 * orders nothing; it is the first half of a pair. The second half is the
	 * caller's `comp_vk_deposit_note_planes_consumed`, which signals the shared
	 * fence on this same context — behind this wait, one ordered stream — and
	 * which Vulkan's next flatten waits for on the timeline.
	 *
	 * Without the pair, Vulkan laps the bridge and tears the 2D band: the same
	 * defect VK-1a's atlas release edge closed, one level down. It costs one
	 * queued wait and one queued signal — no deeper ring, and no change to the
	 * staged ingress the atlas depends on.
	 */
	if (s->bd_plane_live) {
		comp_xbridge_pre_plane_write(s->xbridge, COMP_XBRIDGE_PLANE_BACKDROP);
	}
	if (s->l2d_plane_live) {
		comp_xbridge_pre_plane_write(s->xbridge, COMP_XBRIDGE_PLANE_LOCAL2D);
	}
	if (s->mask_plane_live) {
		comp_xbridge_pre_plane_write(s->xbridge, COMP_XBRIDGE_PLANE_MASK);
	}
}

/*!
 * VK-1b-2 — the masked 2D-over-3D composite, on the OUT device, over the frame
 * the display processor has just woven into @p dst.
 *
 * Every parameter comes FROM THE SLOT (#1140). The slot was filled by an earlier
 * frame, so reading the composite's recipe from live CPU state would pair one
 * frame's pixels with another frame's blend — exactly what `eg_gen` already
 * forbids for the atlas, one level down.
 *
 * Returns false on every "no composite this frame" path, which is a normal
 * outcome and never a broken frame: the weave has already gone into @p dst and
 * is presented regardless.
 */
static bool
split_composite(struct comp_hub_d3d11 *s, int32_t slot, bool is_repaint, ID3D11Texture2D *dst)
{
	if (s->outcomp == nullptr || dst == nullptr) {
		return false;
	}

	struct comp_xbridge_recipe rec = {};
	if (!split_frame_recipe(s, slot, &rec) || !rec.composite) {
		// A projection-only frame filled this slot. The correct answer, not a bail.
		return false;
	}

	D3D11_TEXTURE2D_DESC dd = {};
	dst->GetDesc(&dd);
	uint32_t region_w = rec.region_w;
	uint32_t region_h = rec.region_h;
	if (region_w > dd.Width) {
		region_w = dd.Width;
	}
	if (region_h > dd.Height) {
		region_h = dd.Height;
	}
	if (region_w == 0 || region_h == 0) {
		s->composite_bails++;
		return false;
	}

	/*
	 * The `twod` source is the LOCAL2D PLANE that landed with THIS slot, proved
	 * against the recipe's own generation (#918 review F3) — a later submit that
	 * rewrote the plane under this weave comes back NULL rather than as
	 * mismatched pixels.
	 */
	if ((rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_LOCAL2D)) == 0) {
		s->composite_bails++;
		return false;
	}
	ID3D11ShaderResourceView *twod_srv =
	    split_frame_plane_srv(s, slot, COMP_XBRIDGE_PLANE_LOCAL2D, rec.plane_seq[COMP_XBRIDGE_PLANE_LOCAL2D]);
	if (twod_srv == nullptr) {
		s->composite_bails++;
		return false;
	}

	/*
	 * The mask is the OUT-DEVICE raster the last app frame produced. A repaint
	 * reads the same pointer without re-rastering (#868), which is the whole
	 * reason `is_repaint` is an explicit parameter of the weave rather than
	 * inferred: the raster is not just a draw, it feeds the once-per-app-frame
	 * wish publish, and driving that at panel rate desynchronises the sideband.
	 */
	(void)is_repaint;
	ID3D11ShaderResourceView *mask_srv = nullptr;
	if (rec.mask_kind == COMP_XBRIDGE_MASK_PLANE) {
		/*
		 * VK-1b-3 — the app drew these pixels, so they crossed as a plane and
		 * are proved against the slot's own generation like every other plane.
		 * Resolved FROM THE SLOT on a repaint too, rather than cached: a repaint
		 * re-weaves the slot the last app frame wove, so the same call returns
		 * the same resource under the same seq test — and a cached pointer would
		 * be the one thing that could outlive its seq.
		 */
		if ((rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_MASK)) == 0) {
			s->composite_bails++;
			return false;
		}
		mask_srv =
		    split_frame_plane_srv(s, slot, COMP_XBRIDGE_PLANE_MASK, rec.plane_seq[COMP_XBRIDGE_PLANE_MASK]);
	} else {
		mask_srv = s->repaint_mask_srv;
	}
	if (mask_srv == nullptr) {
		s->composite_bails++;
		return false;
	}

	const DXGI_FORMAT unorm_fmt = (dd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)   ? DXGI_FORMAT_B8G8R8A8_UNORM
	                              : (dd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM
	                                                                               : dd.Format;
	if (!comp_d3d11_outcomp_ensure_weave_scratch(s->outcomp, region_w, region_h, (uint32_t)unorm_fmt)) {
		s->composite_bails++;
		return false;
	}
	// The display processor wrote dst and an RT is not an SRV, so the lerp reads
	// a snapshot. Unit-owned: source and destination are both its device's.
	void *weave_srv = comp_d3d11_outcomp_snapshot_weave(s->outcomp, dst, region_w, region_h);
	if (weave_srv == nullptr) {
		s->composite_bails++;
		return false;
	}

	uint32_t cx_u = (rec.cx < 0) ? 0u : (uint32_t)rec.cx;
	uint32_t cy_u = (rec.cy < 0) ? 0u : (uint32_t)rec.cy;
	if (cx_u > region_w) {
		cx_u = region_w;
	}
	if (cy_u > region_h) {
		cy_u = region_h;
	}
	const uint32_t cright = (cx_u + rec.cw > region_w) ? region_w : cx_u + rec.cw;
	const uint32_t cbottom = (cy_u + rec.ch > region_h) ? region_h : cy_u + rec.ch;

	const xrt_result_t xret = comp_d3d11_outcomp_composite_2d_masked(
	    s->outcomp, dst, twod_srv, mask_srv, weave_srv, region_w, region_h, (int32_t)cx_u, (int32_t)cy_u,
	    cright - cx_u, cbottom - cy_u, rec.composite_mode, rec.opaque_present);
	if (xret != XRT_SUCCESS) {
		s->composite_bails++;
		return false;
	}

	s->composites++;
	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("#918 VK-1b: Local2D composite on the SCANOUT device — %ux%u region, mode=%u mask_kind=%u",
		        region_w, region_h, rec.composite_mode, rec.mask_kind);
	}
	return true;
}

/*!
 * VK-1b-2 — the diagnostic HUD, copied onto the back buffer after the composite.
 *
 * Not a bridge plane and not transported: `u_hud` rasterises to a CPU buffer, so
 * the bitmap was uploaded straight to an out-device texture by
 * @ref comp_hub_d3d11_set_hud. Same shape as `d3d11_render_hud_overlay`'s.
 */
static void
split_draw_hud(struct comp_hub_d3d11 *s, ID3D11Texture2D *dst, uint32_t tgt_w, uint32_t tgt_h)
{
	if (!s->hud_live || s->hud_tex == nullptr || dst == nullptr) {
		return;
	}
	if (tgt_w < s->hud_w + 10u || tgt_h < s->hud_h + 10u) {
		return;
	}
	const uint32_t dst_x = 10u;
	const uint32_t dst_y = tgt_h - s->hud_h - 10u;
	D3D11_BOX src = {0, 0, 0, s->hud_w, s->hud_h, 1};
	s->out_ctx->CopySubresourceRegion(dst, 0, dst_x, dst_y, 0, s->hud_tex, 0, &src);
}

/*!
 * #2342 P2c: read back the top-left @p w x @p h of @p tex on THIS device and
 * encode it as an RGBA8 PNG at @p path. 8-bit RGBA / BGRA families only (a deep
 * atlas is refused by name, as the tiers' captures do); BGRA is swizzled; alpha
 * forced opaque unless DXR_ATLAS_CAPTURE_RAW_ALPHA=1. Capture frames only: one
 * staging copy and a Map, which waits for this context.
 */
static bool
split_capture_tex_png(struct comp_hub_d3d11 *s, ID3D11Texture2D *tex, uint32_t w, uint32_t h, const char *path)
{
	if (tex == nullptr || w == 0 || h == 0 || path == nullptr || path[0] == '\0') {
		return false;
	}
	D3D11_TEXTURE2D_DESC d = {};
	tex->GetDesc(&d);
	bool swap_rb = false;
	switch (d.Format) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: break;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: swap_rb = true; break;
	default:
		U_LOG_W("#2342 hub capture: texture format %d is not 8-bit RGBA/BGRA — %s skipped", (int)d.Format,
		        path);
		return false;
	}
	if (w > d.Width) {
		w = d.Width;
	}
	if (h > d.Height) {
		h = d.Height;
	}
	D3D11_TEXTURE2D_DESC sd = d;
	sd.Width = w;
	sd.Height = h;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	ID3D11Texture2D *staging = nullptr;
	if (FAILED(s->out_dev->CreateTexture2D(&sd, nullptr, &staging)) || staging == nullptr) {
		return false;
	}
	D3D11_BOX box = {0, 0, 0, w, h, 1};
	s->out_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);
	D3D11_MAPPED_SUBRESOURCE m = {};
	bool ok = false;
	if (SUCCEEDED(s->out_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
		const size_t pitch = (size_t)w * 4u;
		uint8_t *tight = (uint8_t *)malloc(pitch * h);
		if (tight != nullptr) {
			u_image_capture_to_rgba8(tight, (const uint8_t *)m.pData, w, h, m.RowPitch, swap_rb,
			                         u_image_capture_raw_alpha());
			ok = stbi_write_png(path, (int)w, (int)h, 4, tight, (int)pitch) != 0;
			free(tight);
		}
		s->out_ctx->Unmap(staging, 0);
	}
	staging->Release();
	return ok;
}

//! The texture behind @p srv (AddRef'd), or NULL.
static ID3D11Texture2D *
split_srv_texture(ID3D11ShaderResourceView *srv)
{
	if (srv == nullptr) {
		return nullptr;
	}
	ID3D11Resource *res = nullptr;
	srv->GetResource(&res);
	if (res == nullptr) {
		return nullptr;
	}
	ID3D11Texture2D *tex = nullptr;
	if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex))) {
		tex = nullptr;
	}
	res->Release();
	return tex;
}

/*!
 * #2342 P2c: the DP-input half of an armed capture, right after the weave
 * consumed @p atlas_srv: its content box (@p w x @p h at the origin — the crop
 * when one ran, else the content box of the slot/atlas). App frames only.
 */
static void
split_capture_dp_input(
    struct comp_hub_d3d11 *s, bool is_repaint, ID3D11ShaderResourceView *atlas_srv, uint32_t w, uint32_t h)
{
	if (is_repaint || !s->cap.armed || s->cap.served) {
		return;
	}
	ID3D11Texture2D *tex = split_srv_texture(atlas_srv);
	s->cap.ok = split_capture_tex_png(s, tex, w, h, s->cap.path);
	if (tex != nullptr) {
		tex->Release();
	}
	s->cap.in_w = w;
	s->cap.in_h = h;
	s->cap.served = true;
}

/*!
 * #2342 P2c: the post-weave half (full triggers only) — the back buffer after
 * the weave, the masked composite and the HUD, i.e. what is about to be
 * presented — and the one WARN naming the files.
 */
static void
split_capture_woven(struct comp_hub_d3d11 *s, bool is_repaint, ID3D11Texture2D *dst, uint32_t tgt_w, uint32_t tgt_h)
{
	if (is_repaint || !s->cap.armed || !s->cap.served) {
		return;
	}
	s->cap.armed = false;
	if (!s->cap.projection_only) {
		s->cap.woven_ok = split_capture_tex_png(s, dst, tgt_w, tgt_h, s->cap.woven_path);
		s->cap.out_w = tgt_w;
		s->cap.out_h = tgt_h;
		U_LOG_W("#2342 hub capture (%s): DP input %ux%u -> %s%s; post-weave %ux%u -> %s%s",
		        s->direct.active ? "direct" : "xbridge", s->cap.in_w, s->cap.in_h, s->cap.path,
		        s->cap.ok ? "" : " FAILED", tgt_w, tgt_h, s->cap.woven_path, s->cap.woven_ok ? "" : " FAILED");
	} else {
		U_LOG_W("#2342 hub capture (%s, projection-only): DP input %ux%u -> %s%s",
		        s->direct.active ? "direct" : "xbridge", s->cap.in_w, s->cap.in_h, s->cap.path,
		        s->cap.ok ? "" : " FAILED");
	}
}

/*!
 * The residual crop, on the OUTPUT device — only ever reached when the R2 resize
 * hysteresis is holding a worst-case egress ring.
 */
static ID3D11ShaderResourceView *
split_crop_for_dp(struct comp_hub_d3d11 *s, ID3D11ShaderResourceView *src_srv, uint32_t content_w, uint32_t content_h)
{
	if (src_srv == nullptr) {
		return nullptr;
	}

	/*
	 * #1178 — the crop texture's format is the EGRESS's, read off the source,
	 * never a constant. This was the second live instance of the defect: the
	 * egress ring is BGRA on the Vulkan leg, this scratch was hardcoded
	 * `R8G8B8A8_UNORM`, and the CopySubresourceRegion below across two DXGI
	 * typeless families is silently dropped rather than failed. It only fires when
	 * the R2 resize hysteresis is holding a worst-case ring, which is exactly the
	 * kind of conditional path a byte counter can never speak for.
	 */
	DXGI_FORMAT src_fmt = DXGI_FORMAT_UNKNOWN;
	{
		ID3D11Resource *probe_res = nullptr;
		src_srv->GetResource(&probe_res);
		if (probe_res == nullptr) {
			return src_srv;
		}
		ID3D11Texture2D *probe_tex = nullptr;
		if (SUCCEEDED(probe_res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&probe_tex)) &&
		    probe_tex != nullptr) {
			D3D11_TEXTURE2D_DESC pd = {};
			probe_tex->GetDesc(&pd);
			src_fmt = pd.Format;
			probe_tex->Release();
		}
		probe_res->Release();
	}
	if (src_fmt == DXGI_FORMAT_UNKNOWN) {
		return src_srv;
	}

	if (s->dp_input_tex == nullptr || s->dp_input_w != content_w || s->dp_input_h != content_h ||
	    s->dp_input_fmt != src_fmt) {
		if (s->dp_input_srv != nullptr) {
			s->dp_input_srv->Release();
			s->dp_input_srv = nullptr;
		}
		if (s->dp_input_tex != nullptr) {
			s->dp_input_tex->Release();
			s->dp_input_tex = nullptr;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = content_w;
		desc.Height = content_h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		// #2345 review: TYPELESS, so its non-decoding view exists whatever the
		// source's typed member is (the copy below stays in the family).
		desc.Format = split_typeless_format(src_fmt);
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(s->out_dev->CreateTexture2D(&desc, nullptr, &s->dp_input_tex))) {
			return src_srv;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		// #2342 P2c (ADR-044): the scratch is in the source's family (the copy
		// below is a raw reinterpretation); its view never decodes.
		srv_desc.Format = split_nondecoding_format(desc.Format);
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;
		if (FAILED(s->out_dev->CreateShaderResourceView(s->dp_input_tex, &srv_desc, &s->dp_input_srv))) {
			s->dp_input_tex->Release();
			s->dp_input_tex = nullptr;
			return src_srv;
		}
		s->dp_input_w = content_w;
		s->dp_input_h = content_h;
		s->dp_input_fmt = src_fmt;
	}

	ID3D11Resource *src_res = nullptr;
	src_srv->GetResource(&src_res);
	if (src_res == nullptr) {
		return src_srv;
	}
	D3D11_BOX box = {0, 0, 0, content_w, content_h, 1};
	s->out_ctx->CopySubresourceRegion(s->dp_input_tex, 0, 0, 0, 0, src_res, 0, &box);
	src_res->Release();
	s->out_crop++;
	return s->dp_input_srv;
}

extern "C" bool
comp_hub_d3d11_resize_target(struct comp_hub_d3d11 *s, uint32_t width, uint32_t height)
{
	if (s == nullptr || s->target == nullptr || width == 0 || height == 0) {
		return false;
	}

	uint32_t cur_w = 0, cur_h = 0;
	comp_d3d11_target_get_dimensions(s->target, &cur_w, &cur_h);
	if (cur_w == width && cur_h == height) {
		return true;
	}

	/*
	 * No ID3D10Multithread bracket here, and that is the split's whole point:
	 * this swapchain lives on the runtime's OWN output device, which the app's
	 * render thread never touches. The non-split D3D11 leg takes one at its
	 * equivalent site precisely because there the chain shares the app's
	 * immediate context (`resize_needs_lock` in d3d11_compositor_begin_frame),
	 * and it too drops the bracket once the split is active.
	 *
	 * The caller holds the compositor mutex, which is what keeps the #868
	 * repaint thread — the other user of this target — out for the duration.
	 */
	const xrt_result_t xret = comp_d3d11_target_resize(s->target, width, height);
	if (xret != XRT_SUCCESS) {
		U_LOG_E(
		    "VK output-device split: the scanout swapchain REFUSED %ux%u (%d) and stays at %ux%u — "
		    "the present will keep the OLD geometry (#1178)",
		    width, height, (int)xret, cur_w, cur_h);
		return false;
	}

	// A window resize, not a frame event.
	U_LOG_W("VK output-device split: scanout swapchain follows the window, %ux%u -> %ux%u (#1178)", cur_w, cur_h,
	        width, height);
	s->tgt_drift_since_ns = 0;
	return true;
}

/*!
 * #1178 — THE TRIPWIRE, at the point of use.
 *
 * The fix one level up makes the swapchain follow the window; this makes a
 * failure to do so DIAGNOSABLE, which is the part that outlives the instance.
 * Nothing in the pipeline can see this defect: the atlas, the content box, the
 * egress ring and every `[RENDER]` counter follow the window correctly, so the
 * only witness is the geometry of the surface being presented against the
 * geometry of the window presenting it. #1178 shipped with `split=1 no_slot=0
 * ing_leak=0 out_crop=0` and a visibly wrong picture.
 *
 * Grace before complaint, because a real drag DOES disagree briefly: a repaint
 * tick can weave between the window changing and the next `begin_frame`
 * observing it. Only drift that persists past @ref SPLIT_DRIFT_GRACE_NS is a
 * defect rather than a frame of latency.
 */
static void
split_check_target_follows_window(struct comp_hub_d3d11 *s, uint32_t tgt_w, uint32_t tgt_h)
{
	if (s->hwnd == nullptr) {
		return;
	}
	RECT rc = {};
	if (!GetClientRect(s->hwnd, &rc)) {
		return;
	}
	const uint32_t win_w = (uint32_t)(rc.right - rc.left);
	const uint32_t win_h = (uint32_t)(rc.bottom - rc.top);
	if (win_w == 0 || win_h == 0 || (win_w == tgt_w && win_h == tgt_h)) {
		s->tgt_drift_since_ns = 0;
		return;
	}

	const uint64_t now = os_monotonic_get_ns();
	if (s->tgt_drift_since_ns == 0) {
		s->tgt_drift_since_ns = now;
		return;
	}
	s->tgt_drift_frames++;
	if (now - s->tgt_drift_since_ns < SPLIT_DRIFT_GRACE_NS) {
		return;
	}
	if (s->tgt_drift_log_ns != 0 && now - s->tgt_drift_log_ns < SPLIT_DRIFT_LOG_NS) {
		return;
	}
	s->tgt_drift_log_ns = now;
	U_LOG_E(
	    "VK output-device split: THE SCANOUT SWAPCHAIN IS NOT FOLLOWING THE WINDOW — the window's client "
	    "rect is %ux%u but the chain is %ux%u, and has been for %llu ms / %llu frames. Every weave is "
	    "composed for the CHAIN's geometry and DXGI then scales or clips it into the window, so the picture "
	    "keeps the old size while the atlas, the content box, the egress ring and every [RENDER] counter "
	    "track the window correctly and read healthy. Whatever moved the window did not reach "
	    "comp_hub_d3d11_resize_target (#1178).",
	    win_w, win_h, tgt_w, tgt_h, (unsigned long long)((now - s->tgt_drift_since_ns) / (1000 * 1000)),
	    (unsigned long long)s->tgt_drift_frames);
}

/*
 *
 * #2317 P1b: DIRECT ingress (same adapter, stable atlas).
 *
 */

//! `DXR_HUB_INGRESS`: 0 unset (the per-producer default), 1 `xbridge`, 2 `direct`.
static int
split_ingress_env(void)
{
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("DXR_HUB_INGRESS");
		v = (e != nullptr && strcmp(e, "xbridge") == 0)  ? 1
		    : (e != nullptr && strcmp(e, "direct") == 0) ? 2
		                                                 : 0;
	}
	return v;
}

//! DXR_HUB_INGRESS=xbridge: keep the comp_xbridge route even when DIRECT is
//! possible — the A/B control for the transport's cost.
static bool
split_direct_forced_off(void)
{
	return split_ingress_env() == 1;
}

//! DXR_HUB_INGRESS=direct: the opt-in a RING producer needs for DIRECT ingress
//! until the single-GPU desktop gate passes (#2317; see the call site).
static bool
split_direct_forced_on(void)
{
	return split_ingress_env() == 2;
}

/*!
 * Open the producer's fence here and share a consumer fence back. Every piece
 * is required: without the producer fence the weave could read a half-written
 * atlas, and without the consumer fence the producer could overwrite one a
 * repaint is still reading — and neither may become a CPU wait (ADR-055).
 *
 * #2317 P2a: a RING producer (the VK deposit, the D3D12 reroute's deposit)
 * takes the same two fences, except that the consumer fence is opened by the
 * producer's OWN API (an `ID3D12Fence` / an imported Vulkan timeline) rather
 * than on the deposit's D3D11 device — the producer's queue is what rewrites
 * the slot. A KEYED-MUTEX deposit takes neither: its slot mutexes carry both
 * edges.
 */
static bool
split_wire_direct(struct comp_hub_d3d11 *s, const struct comp_hub_d3d11_handoff *handoff)
{
	auto &d = s->direct;
	const bool ring = !s->stable_atlas;
	const bool km = ring && handoff->keyed_mutex != nullptr;
	if ((!km && handoff->fence_shared_handle == nullptr) || s->app_ctx4 == nullptr) {
		return false;
	}

	ID3D11Device5 *out5 = nullptr;
	ID3D11Device5 *app5 = nullptr;
	bool ok = SUCCEEDED(s->out_dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&out5)) && out5 != nullptr &&
	          SUCCEEDED(s->out_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&d.out_ctx4)) &&
	          d.out_ctx4 != nullptr;
	HRESULT hr = S_OK;
	if (ok && !km) {
		hr = out5->OpenSharedFence((HANDLE)handoff->fence_shared_handle, __uuidof(ID3D11Fence),
		                           (void **)&d.prod_fence);
		ok = SUCCEEDED(hr) && d.prod_fence != nullptr;
	}
	if (ok && !km) {
		hr = out5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&d.cons_fence);
		ok = SUCCEEDED(hr) && d.cons_fence != nullptr;
	}
	if (ok && !km) {
		hr = d.cons_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &d.cons_handle);
		ok = SUCCEEDED(hr) && d.cons_handle != nullptr;
	}
	// The stable atlas is rewritten on the deposit's own D3D11 context, so
	// that is where its back-fence wait lives (comp_hub_d3d11_pre_render).
	if (ok && !ring) {
		ok = SUCCEEDED(s->app_dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&app5)) && app5 != nullptr;
		if (ok) {
			hr = app5->OpenSharedFence(d.cons_handle, __uuidof(ID3D11Fence), (void **)&d.cons_fence_app);
			ok = SUCCEEDED(hr) && d.cons_fence_app != nullptr;
		}
	}
	if (out5 != nullptr) {
		out5->Release();
	}
	if (app5 != nullptr) {
		app5->Release();
	}
	if (!ok) {
		U_LOG_W("#2317 hub: DIRECT wiring failed (hr=0x%08lx)", (unsigned long)hr);
		split_release_direct(s);
		return false;
	}

	d.ring = ring;
	d.km = km;
	d.cur = -1;
	d.active = true;
	if (s->client_presents) {
		U_LOG_W(
		    "D3D11 hub ACTIVE (client-presents): weave moves to the hub device on '%ls' LUID=%08lx:%08lx, the "
		    "caller's own adapter; the producer's atlas is read IN PLACE (DIRECT) and the weave is written "
		    "into the caller's texture, handed back on the consumer fence (#2370)",
		    s->out_desc, (unsigned long)s->out_luid.HighPart, (unsigned long)s->out_luid.LowPart);
		return true;
	}
	U_LOG_W(
	    "D3D11 hub ACTIVE: weave/repaint/present move to '%ls' LUID=%08lx:%08lx (app "
	    "device on LUID=%08lx:%08lx); the producer's atlas is read IN PLACE on this device — DIRECT "
	    "ingress, no transport copy (#2317 %s)",
	    s->out_desc, (unsigned long)s->out_luid.HighPart, (unsigned long)s->out_luid.LowPart,
	    (unsigned long)s->render_luid.HighPart, (unsigned long)s->render_luid.LowPart,
	    !ring ? "P1b" : (km ? "P2a, ring deposit, keyed-mutex mode" : "P2a, ring deposit, fence mode"));
	return true;
}

/*!
 * #2317 P2a: open ring slot @p idx on this device (once per ring generation).
 * False leaves the slot unusable for this frame; the weave then presents
 * nothing and the panel holds the last woven frame (#918 F4).
 */
static bool
split_direct_open_slot(struct comp_hub_d3d11 *s, uint32_t idx, const struct comp_hub_d3d11_handoff *handoff)
{
	auto &e = s->direct.slots[idx];
	if (e.tex != nullptr && e.gen == handoff->generation) {
		return true;
	}
	if (e.km != nullptr) {
		e.km->Release();
	}
	if (e.srv != nullptr) {
		e.srv->Release();
	}
	if (e.tex != nullptr) {
		e.tex->Release();
	}
	memset(&e, 0, sizeof(e));

	ID3D11Device1 *od1 = nullptr;
	HRESULT hr = E_NOINTERFACE;
	if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&od1)) && od1 != nullptr) {
		hr = od1->OpenSharedResource1((HANDLE)handoff->shared_handle, __uuidof(ID3D11Texture2D),
		                              (void **)&e.tex);
		od1->Release();
	}
	D3D11_TEXTURE2D_DESC td = {};
	if (SUCCEEDED(hr) && e.tex != nullptr && split_refuse_typed_srgb(s, e.tex, "ring slot")) {
		hr = E_INVALIDARG;
	}
	if (SUCCEEDED(hr) && e.tex != nullptr) {
		e.tex->GetDesc(&td);
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		// #2342 P2c (ADR-044): the slot holds encoded bytes; view, don't decode.
		sd.Format = split_nondecoding_format(td.Format);
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		hr = s->out_dev->CreateShaderResourceView(e.tex, &sd, &e.srv);
	}
	if (SUCCEEDED(hr) && s->direct.km) {
		hr = e.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&e.km);
	}
	if (FAILED(hr) || e.tex == nullptr || e.srv == nullptr || (s->direct.km && e.km == nullptr)) {
		if (e.km != nullptr) {
			e.km->Release();
		}
		if (e.srv != nullptr) {
			e.srv->Release();
		}
		if (e.tex != nullptr) {
			e.tex->Release();
		}
		memset(&e, 0, sizeof(e));
		if (!s->warned_slot_open) {
			s->warned_slot_open = true;
			U_LOG_W("#2317 hub: a deposit slot would not open on the hub device (hr=0x%08lx)",
			        (unsigned long)hr);
		}
		return false;
	}
	e.gen = handoff->generation;
	e.w = td.Width;
	e.h = td.Height;
	s->direct.reopens++;
	return true;
}

/*!
 * #2317 P2b/P2c: snapshot @p rec beside the frame being nominated, with the
 * planes this frame staged marked valid — the DIRECT twin of the bridge's
 * per-slot plane stamp. A plane is valid when the frame staged content for it
 * (seq != 0) AND its surface is open here; otherwise every consumer fails
 * closed, as on a slot. One snapshot serves a ring too: a DIRECT weave (and its
 * repaints) only ever reads the NEWEST nominated frame.
 */
static void
split_direct_stamp_recipe(struct comp_hub_d3d11 *s, const struct comp_xbridge_recipe *rec)
{
	auto &d = s->direct;
	d.rec = *rec;
	d.rec.plane_valid = 0;
	for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
		if (d.planes[p].seq != 0 && d.planes[p].srv != nullptr) {
			d.rec.plane_valid |= 1u << p;
			d.rec.plane_seq[p] = d.planes[p].seq;
		}
	}
}

/*!
 * Nominate this frame's atlas: re-open it when its generation moved, and record
 * the fence value the weave waits for. Nothing here waits or copies.
 */
static void
split_submit_direct(struct comp_hub_d3d11 *s,
                    const struct comp_hub_d3d11_handoff *handoff,
                    uint32_t content_w,
                    uint32_t content_h,
                    const struct comp_xbridge_recipe *rec)
{
	auto &d = s->direct;
	// Every return that does not nominate this frame drops its recipe with it.
	d.rec = {};

	if (d.ring) {
		/*
		 * #2317 P2a: the hub moves OFF the slot it was reading. Every read of
		 * it is already recorded (weaves and repaints are serialised with this
		 * call), so the consumer fence's current value is that slot's release:
		 * owe it to the producer, whose next write into the slot waits for it.
		 * Keyed-mutex mode owes nothing — the slot mutex is the release.
		 */
		const uint32_t idx = handoff->slot % COMP_HUB_D3D11_DIRECT_RING;
		if (d.cur >= 0 && (uint32_t)d.cur != idx && !d.km && d.cons_value != 0) {
			d.release_owed[d.cur] = d.cons_value;
		}
		d.cur = -1;
		d.have_frame = false;
		if (handoff->shared_handle == nullptr || !split_direct_open_slot(s, idx, handoff)) {
			return;
		}
		const auto &e = d.slots[idx];
		if (content_w > e.w || content_h > e.h) {
			return; // never weave a box the texture does not hold
		}
		d.cur = (int32_t)idx;
		d.tex_w = e.w;
		d.tex_h = e.h;
		d.prod_value = d.km ? 0 : handoff->fence_value;
		d.content_w = content_w;
		d.content_h = content_h;
		d.gen = s->layout_gen;
		// #2317 P2c: the ring frame's planes ride beside it (the producer
		// wrote them before the signal prod_value names).
		split_direct_stamp_recipe(s, rec);
		d.have_frame = true;
		d.frames++;
		d.frame_seq = ++s->seq;
		return;
	}

	/*
	 * #2317 review: the producer has ALREADY signalled this frame's value. Any
	 * return below that does not nominate it must also drop the previous one —
	 * a weave that kept it would wait only on the OLD value and read an atlas
	 * the producer is rewriting.
	 */
	if (handoff->shared_handle == nullptr) {
		d.have_frame = false;
		return;
	}
	if (d.tex == nullptr || d.tex_gen != handoff->generation) {
		if (d.srv != nullptr) {
			d.srv->Release();
			d.srv = nullptr;
		}
		if (d.tex != nullptr) {
			d.tex->Release();
			d.tex = nullptr;
		}
		d.have_frame = false;
		ID3D11Device1 *od1 = nullptr;
		HRESULT hr = E_NOINTERFACE;
		if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&od1)) && od1 != nullptr) {
			hr = od1->OpenSharedResource1((HANDLE)handoff->shared_handle, __uuidof(ID3D11Texture2D),
			                              (void **)&d.tex);
			od1->Release();
		}
		if (FAILED(hr) || d.tex == nullptr) {
			d.tex = nullptr;
			if (!s->warned_atlas_open) {
				s->warned_atlas_open = true;
				U_LOG_W("#2317 hub: the producer atlas would not open on the hub device (hr=0x%08lx)",
				        (unsigned long)hr);
			}
			return;
		}
		if (split_refuse_typed_srgb(s, d.tex, "atlas")) {
			d.tex->Release();
			d.tex = nullptr;
			d.have_frame = false;
			return;
		}
		D3D11_TEXTURE2D_DESC td = {};
		d.tex->GetDesc(&td);
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		// #2342 P2c (ADR-044): the atlas holds encoded bytes; view, don't decode.
		sd.Format = split_nondecoding_format(td.Format);
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		if (FAILED(s->out_dev->CreateShaderResourceView(d.tex, &sd, &d.srv)) || d.srv == nullptr) {
			d.srv = nullptr;
			d.tex->Release();
			d.tex = nullptr;
			d.have_frame = false;
			return;
		}
		d.tex_gen = handoff->generation;
		d.tex_w = td.Width;
		d.tex_h = td.Height;
		d.reopens++;
	}
	if (content_w > d.tex_w || content_h > d.tex_h) {
		d.have_frame = false; // never weave a box the texture does not hold
		return;
	}
	d.prod_value = handoff->fence_value;
	d.content_w = content_w;
	d.content_h = content_h;
	d.gen = s->layout_gen;
	/*
	 * #2317 P2b: the recipe, with the planes this frame staged marked valid —
	 * the DIRECT twin of the bridge's per-slot plane stamp. A plane is valid
	 * when the frame staged content for it (seq != 0) AND its surface is open
	 * here; otherwise every consumer fails closed, as on a slot.
	 */
	split_direct_stamp_recipe(s, rec);
	d.have_frame = true;
	d.frames++;
	d.frame_seq = ++s->seq;
}

/*!
 * #2327: record @p pdt_ns as the promise for frame @p seq, and look one up.
 * 0 = unknown (the weave then writes no D row rather than a mispaired one).
 */
static void
split_pdt_put(struct comp_hub_d3d11 *s, uint64_t seq, uint64_t pdt_ns)
{
	auto &e = s->pdt_ring[seq % (sizeof(s->pdt_ring) / sizeof(s->pdt_ring[0]))];
	e.seq = seq;
	e.pdt_ns = pdt_ns;
}

static uint64_t
split_pdt_get(struct comp_hub_d3d11 *s, uint64_t seq)
{
	if (seq == 0) {
		return 0;
	}
	const auto &e = s->pdt_ring[seq % (sizeof(s->pdt_ring) / sizeof(s->pdt_ring[0]))];
	return e.seq == seq ? e.pdt_ns : 0;
}

/*!
 * The DIRECT twin of the bridge weave below: one atlas, no slot to pick. The
 * crop-before-DP rule (ADR-030) still holds — the atlas is worst-case sized —
 * so a content box smaller than the texture is cropped on THIS device, the same
 * single copy the tier's own single-device path pays.
 */
static bool
split_weave_direct(struct comp_hub_d3d11 *s, bool is_repaint, const struct xrt_rect *canvas)
{
	auto &d = s->direct;
	ID3D11ShaderResourceView *src_srv = d.srv;
	IDXGIKeyedMutex *km = nullptr;
	if (d.ring) {
		src_srv = nullptr;
		if (d.cur >= 0) {
			src_srv = d.slots[d.cur].srv;
			km = d.slots[d.cur].km;
		}
	}
	if (s->target == nullptr || s->dp == nullptr || !d.have_frame || src_srv == nullptr) {
		s->no_slot++;
		return false;
	}
	if (d.gen != s->layout_gen) {
		s->stale_refusals++;
		s->no_slot++;
		return false;
	}
	const uint32_t weave_cols = s->gen_cols != 0 ? s->gen_cols : 1;
	const uint32_t weave_rows = s->gen_rows != 0 ? s->gen_rows : 1;
	const uint32_t weave_view_w = d.content_w / weave_cols;
	const uint32_t weave_view_h = d.content_h / weave_rows;
	if (weave_view_w == 0 || weave_view_h == 0) {
		s->no_slot++;
		return false;
	}

	// A repaint of the frame the crop scratch already holds re-reads the crop,
	// not the atlas: one crop per PRODUCER frame, never one per weave.
	const bool needs_crop = d.content_w != d.tex_w || d.content_h != d.tex_h;
	const bool crop_cached = needs_crop && d.cropped_seq != 0 && d.cropped_seq == d.frame_seq &&
	                         s->dp_input_srv != nullptr && s->dp_input_w == d.content_w &&
	                         s->dp_input_h == d.content_h;
	const bool reads_atlas = !crop_cached;

	/*
	 * #2317 P2a, KEYED-MUTEX deposits: the slot's mutex is the ordering, both
	 * ways. Bounded and uncontended by construction (the producer re-enters a
	 * slot only after the hub has moved off it, and its own acquire/release
	 * ride its submit); a timeout SKIPS the weave — presents nothing, the panel
	 * holds the last frame — and is counted, never waited out.
	 */
	if (km != nullptr && reads_atlas) {
		const HRESULT km_hr = km->AcquireSync(0, 2);
		if (km_hr != S_OK) {
			// #2317 P2a review: WAIT_ABANDONED means the mutex WAS acquired;
			// release it before skipping, or Vulkan's next acquire on this
			// slot times out every frame.
			if (km_hr == (HRESULT)WAIT_ABANDONED) {
				km->ReleaseSync(0);
			}
			d.km_skips++;
			s->no_slot++;
			return false;
		}
	}

	/*
	 * #2317 review: acquire FIRST. Everything after the first read of the
	 * producer's atlas (the crop below, or the weave) must reach the consumer
	 * fence signal, and the acquire is the one step after it that can fail.
	 */
	uint32_t target_index = 0;
	if (comp_d3d11_target_acquire(s->target, &target_index) != XRT_SUCCESS) {
		if (km != nullptr && reads_atlas) {
			km->ReleaseSync(0);
		}
		s->no_slot++;
		return false;
	}

	// THE ordering edge: GPU-side, this thread does not block.
	if (reads_atlas && d.prod_fence != nullptr && d.prod_value != 0) {
		d.out_ctx4->Wait(d.prod_fence, d.prod_value);
	}

	ID3D11ShaderResourceView *atlas_srv = src_srv;
	if (needs_crop) {
		if (crop_cached) {
			atlas_srv = s->dp_input_srv;
		} else {
			atlas_srv = split_crop_for_dp(s, atlas_srv, d.content_w, d.content_h);
			d.cropped_seq = (atlas_srv == s->dp_input_srv) ? d.frame_seq : 0;
		}
	}

	uint32_t tgt_w = 0, tgt_h = 0;
	comp_d3d11_target_get_dimensions(s->target, &tgt_w, &tgt_h);
	split_check_target_follows_window(s, tgt_w, tgt_h);

	struct comp_seg_rect seg_window = {0, 0, 0, 0};
	const bool seg_split = split_segments_update(s, tgt_w, tgt_h, &seg_window);

	if (is_repaint) {
		comp_d3d11_target_weave_mark_repaint(s->target, s->mode_3d);
	} else {
		// #2317 P1c: the caller's xrWaitFrame promise for THIS frame (#2327:
		// looked up by the woven frame's seq), so the DXR_WEAVE_LATENCY_CSV
		// site emits the D (prediction error) rows the tiers' own sites do.
		// 0 (never set) keeps the old no-D-row behaviour.
		comp_d3d11_target_weave_mark(s->target, split_pdt_get(s, d.frame_seq), s->mode_3d);
	}

	xrt_display_processor_d3d11_set_frame_timing(s->dp, comp_d3d11_target_get_measured_weave_ns(s->target),
	                                             comp_d3d11_target_get_display_period_ns(s->target));
	xrt_display_processor_d3d11_set_predicted_scanout(s->dp,
	                                                  comp_d3d11_target_predict_weave_to_scanout_ns(s->target));

	// #2317 P2b: the frame's zone-wish publish, now that the producer wait above
	// orders this context after the mask plane's write (app frames only: a
	// repaint replays rendering, never the once-per-app-frame sideband, #868).
	if (!is_repaint && d.publish_pending) {
		d.publish_pending = false;
		split_publish_zone_wish_now(s, d.publish_seq);
	}

	comp_d3d11_target_bind(s->target);

	// #2317 P2b: the 2D-under backdrop, from the producer's plane surface.
	split_set_backdrop(s, -1);

	struct xrt_rect cv = {};
	if (canvas != nullptr) {
		cv = *canvas;
	}
	if (seg_split) {
		struct comp_d3d11_segments_frame sf = {};
		sf.content = &s->seg_content_staged; // the one atlas IS the latest frame
		sf.context = s->out_ctx;
		sf.src_srv = atlas_srv;
		sf.view_width = weave_view_w;
		sf.view_height = weave_view_h;
		sf.tile_columns = weave_cols;
		sf.tile_rows = weave_rows;
		sf.src_format = (uint32_t)split_dp_view_format(atlas_srv);
		sf.target_rtv = comp_d3d11_target_get_rtv(s->target);
		sf.target_width = tgt_w;
		sf.target_height = tgt_h;
		sf.canvas.x = 0;
		sf.canvas.y = 0;
		sf.canvas.w = seg_window.w < tgt_w ? seg_window.w : tgt_w;
		sf.canvas.h = seg_window.h < tgt_h ? seg_window.h : tgt_h;
		sf.transparent_background = s->transparent_background;
		sf.atlas_encoding = -1;
		sf.primary_dp = s->dp;
		sf.outcomp = split_ensure_outcomp(s) ? s->outcomp : nullptr;
		(void)comp_d3d11_segments_record(s->segments, &sf);
		comp_d3d11_target_bind(s->target);
	} else {
		xrt_display_processor_d3d11_process_atlas(
		    s->dp, s->out_ctx, atlas_srv, weave_view_w, weave_view_h, weave_cols, weave_rows,
		    (uint32_t)split_dp_view_format(atlas_srv), tgt_w, tgt_h, cv.offset.w, cv.offset.h,
		    (uint32_t)cv.extent.w, (uint32_t)cv.extent.h);
	}
	// #2342 P2c: an armed capture's DP input — read before the consumer signal
	// below releases the atlas back to the producer.
	split_capture_dp_input(s, is_repaint, atlas_srv, d.content_w, d.content_h);

	{
		auto *dst = static_cast<ID3D11Texture2D *>(comp_d3d11_target_get_back_buffer(s->target));
		// #2317 P2b: the masked 2D-over-3D composite — the same unit, recipe
		// and mask the xbridge route runs, reading the producer's planes.
		(void)split_composite(s, -1, is_repaint, dst);

		// Every read of the producer's atlas AND its planes is now recorded
		// (backdrop view, weave, composite): release them back. Signalled
		// after the composite, not after the weave, because the composite
		// samples the Local2D and mask planes the producer rewrites next.
		// #2317 P2a: a keyed-mutex ring deposit releases its slot mutex
		// instead (no fences on that driver).
		if (km != nullptr) {
			if (reads_atlas) {
				km->ReleaseSync(0);
			}
		} else if (d.cons_fence != nullptr) {
			d.cons_value++;
			d.out_ctx4->Signal(d.cons_fence, d.cons_value);
		}

		split_draw_hud(s, dst, tgt_w, tgt_h);
		split_capture_woven(s, is_repaint, dst, tgt_w, tgt_h);
		if (s->post_weave_fn != nullptr) {
			s->post_weave_fn(s->post_weave_userdata, s->out_dev, s->out_ctx, dst, tgt_w, tgt_h, is_repaint);
		}
	}

	if (comp_d3d11_target_present(s->target, 1) != XRT_SUCCESS) {
		// The consumer signal above is owed to a producer GPU wait: make sure
		// it reaches the GPU even when the present (which flushes) failed.
		s->out_ctx->Flush();
		return false;
	}
	return true;
}

/*!
 * #2370: the CLIENT-PRESENTS weave — split_weave_direct's ordering, written
 * into the caller's shared texture instead of a back buffer, with no Present.
 *
 * Ordering, both ways on the GPU, no CPU wait (ADR-055):
 *  - IN: the producer fence. The caller's frame-N passes signal it on its
 *    immediate context, and everything the caller did with the texture after
 *    the previous handoff (its blit / present of frame N-1) precedes those
 *    passes there — so waiting it orders this write after the caller's last
 *    read as well as after the atlas.
 *  - OUT: the consumer fence, signalled once the weave, the composite and an
 *    armed capture have recorded; the caller's context waits it in
 *    comp_hub_d3d11_pre_render at the end of its frame submit. The flush below
 *    is what lets the caller's queue ever see the signal (there is no Present
 *    to flush it).
 *
 * App thread, compositor mutex held. Exactly one process_atlas per handoff
 * (per segment on the split path, as natively).
 */
static bool
split_weave_client(struct comp_hub_d3d11 *s, bool is_repaint)
{
	auto &d = s->direct;
	auto &cl = s->client;
	if (is_repaint) {
		// Suppressed by design: see comp_hub_d3d11_repaint.
		return false;
	}
	ID3D11ShaderResourceView *src_srv = d.srv;
	if (d.ring) {
		src_srv = d.cur >= 0 ? d.slots[d.cur].srv : nullptr;
	}
	if (!d.active || d.km || s->dp == nullptr || cl.rtv == nullptr || !d.have_frame || src_srv == nullptr ||
	    !cl.target_valid) {
		s->no_slot++;
		return false;
	}
	if (d.gen != s->layout_gen) {
		s->stale_refusals++;
		s->no_slot++;
		return false;
	}
	const uint32_t weave_cols = s->gen_cols != 0 ? s->gen_cols : 1;
	const uint32_t weave_rows = s->gen_rows != 0 ? s->gen_rows : 1;
	const uint32_t weave_view_w = d.content_w / weave_cols;
	const uint32_t weave_view_h = d.content_h / weave_rows;
	if (weave_view_w == 0 || weave_view_h == 0) {
		s->no_slot++;
		return false;
	}

	// THE ordering edge (IN).
	if (d.prod_fence != nullptr && d.prod_value != 0) {
		d.out_ctx4->Wait(d.prod_fence, d.prod_value);
	}

	// Crop-before-DP: one crop per producer frame (there is no repaint to reuse it).
	ID3D11ShaderResourceView *atlas_srv = src_srv;
	if (d.content_w != d.tex_w || d.content_h != d.tex_h) {
		atlas_srv = split_crop_for_dp(s, atlas_srv, d.content_w, d.content_h);
		d.cropped_seq = (atlas_srv == s->dp_input_srv) ? d.frame_seq : 0;
	}

	// ADR-060 section 3: the canvas is the target's size at the texture's
	// origin, clamped to the texture.
	const uint32_t tgt_w = cl.w;
	const uint32_t tgt_h = cl.h;
	const uint32_t cv_w = cl.target.w < tgt_w ? cl.target.w : tgt_w;
	const uint32_t cv_h = cl.target.h < tgt_h ? cl.target.h : tgt_h;
	if ((cv_w != cl.target.w || cv_h != cl.target.h) && !cl.clamp_logged) {
		cl.clamp_logged = true;
		U_LOG_W("D3D11 hub client-presents: target %ux%u larger than the shared texture %ux%u — woven clamped",
		        cl.target.w, cl.target.h, tgt_w, tgt_h);
	}

	// The segment window IS cl.target (split_segments_update reads it), so
	// both the single-DP and the segmented canvas come from cv_w x cv_h above.
	const bool seg_split = split_segments_update(s, tgt_w, tgt_h, nullptr);

	// No swapchain, so no measured weave->scanout and no vblank grid: the
	// period only (0 measured = the DP's own heuristic, as on a cold target).
	xrt_display_processor_d3d11_set_frame_timing(s->dp, 0, cl.period_ns);
	xrt_display_processor_d3d11_set_predicted_scanout(s->dp, 0);

	// #2317 P2b: the frame's zone-wish publish, ordered after the producer wait.
	if (d.publish_pending) {
		d.publish_pending = false;
		split_publish_zone_wish_now(s, d.publish_seq);
	}

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)tgt_w;
	vp.Height = (float)tgt_h;
	vp.MaxDepth = 1.0f;
	s->out_ctx->OMSetRenderTargets(1, &cl.rtv, nullptr);
	s->out_ctx->RSSetViewports(1, &vp);

	// #2317 P2b: the 2D-under backdrop, from the producer's plane surface.
	split_set_backdrop(s, -1);

	bool any_flat = false;
	if (seg_split) {
		struct comp_d3d11_segments_frame sf = {};
		sf.content = &s->seg_content_staged;
		sf.context = s->out_ctx;
		sf.src_srv = atlas_srv;
		sf.view_width = weave_view_w;
		sf.view_height = weave_view_h;
		sf.tile_columns = weave_cols;
		sf.tile_rows = weave_rows;
		sf.src_format = (uint32_t)split_dp_view_format(atlas_srv);
		sf.target_rtv = cl.rtv;
		sf.target_width = tgt_w;
		sf.target_height = tgt_h;
		sf.canvas.x = 0;
		sf.canvas.y = 0;
		sf.canvas.w = cv_w;
		sf.canvas.h = cv_h;
		sf.transparent_background = s->transparent_background;
		sf.atlas_encoding = -1;
		sf.primary_dp = s->dp;
		sf.outcomp = split_ensure_outcomp(s) ? s->outcomp : nullptr;
		sf.client_presents = true; // the caller presents the texture
		sf.out_any_flat = &any_flat;
		(void)comp_d3d11_segments_record(s->segments, &sf);
		s->out_ctx->OMSetRenderTargets(1, &cl.rtv, nullptr);
		s->out_ctx->RSSetViewports(1, &vp);
	} else {
		// ADR-060 R2: the windowless DP's phase = the target's origin on its screen.
		const int32_t ox = cl.target.x - cl.panel_left;
		const int32_t oy = cl.target.y - cl.panel_top;
		xrt_display_processor_d3d11_set_present_origin(s->dp, ox, oy);
		if (!cl.origin_logged) {
			cl.origin_logged = true;
			U_LOG_W(
			    "D3D11 hub client-presents: windowless DP phased from present origin (%d,%d) — target at "
			    "(%d,%d) %ux%u, screen at (%d,%d) (#2370)",
			    ox, oy, cl.target.x, cl.target.y, cl.target.w, cl.target.h, cl.panel_left, cl.panel_top);
		} else if (ox != cl.origin_x || oy != cl.origin_y) {
			// A drag: INFO, only on change — never a per-frame WARN.
			U_LOG_I("D3D11 hub client-presents: set_present_origin -> (%d,%d)", ox, oy);
		}
		cl.origin_x = ox;
		cl.origin_y = oy;
		xrt_display_processor_d3d11_process_atlas(s->dp, s->out_ctx, atlas_srv, weave_view_w, weave_view_h,
		                                          weave_cols, weave_rows, (uint32_t)split_dp_view_format(atlas_srv),
		                                          tgt_w, tgt_h, 0, 0, cv_w, cv_h);
	}
	cl.segmented = seg_split;
	cl.any_flat = any_flat;
	g_frame_witness_hub_client.count_weave(false, s->mode_3d);

	split_capture_dp_input(s, false, atlas_srv, d.content_w, d.content_h);

	// The masked 2D-over-3D composite (zones / Local2D / authored mask) onto the
	// caller's texture — the unit, recipe and mask the swapchain strategy runs.
	(void)split_composite(s, -1, false, cl.tex);
	// `.woven.png` = the caller's texture, as the caller is about to present it.
	split_capture_woven(s, false, cl.tex, tgt_w, tgt_h);
	if (s->post_weave_fn != nullptr) {
		s->post_weave_fn(s->post_weave_userdata, s->out_dev, s->out_ctx, cl.tex, tgt_w, tgt_h, false);
	}

	// The handoff (OUT): every read of the atlas and planes and every write of
	// the texture is recorded. Unbind the texture so no hub state keeps it
	// bound while the caller uses it.
	ID3D11RenderTargetView *null_rtv = nullptr;
	s->out_ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
	if (d.cons_fence != nullptr) {
		d.cons_value++;
		d.out_ctx4->Signal(d.cons_fence, d.cons_value);
	}
	s->out_ctx->Flush();
	cl.handoffs++;
	g_frame_witness_hub_client.count_present();
	return true;
}

/*!
 * #2317 P2a review: vet a ring producer's frame for DIRECT before it is
 * recorded. Every failure here would otherwise leave the hub with no frame for
 * the rest of the session (have_frame stays false, every weave is `no_slot`),
 * so each one moves the session to xbridge, which bridges this frame whole.
 */
static void
split_direct_ring_vet(struct comp_hub_d3d11 *s,
                      const struct comp_hub_d3d11_handoff *handoff,
                      uint32_t content_w,
                      uint32_t content_h)
{
	auto &d = s->direct;
	const uint32_t idx = handoff->slot % COMP_HUB_D3D11_DIRECT_RING;

	/*
	 * Backstop for the ring contract: the producer may only rewrite a slot
	 * after the hub moved OFF it (it then owes the release). The same slot,
	 * same ring generation, at a NEWER fence value means it rewrote the slot
	 * the hub and its repaints are still reading. Fence mode only: a
	 * keyed-mutex deposit carries no value to compare.
	 */
	if (!d.km && d.cur >= 0 && (uint32_t)d.cur == idx && d.slots[idx].tex != nullptr &&
	    d.slots[idx].gen == handoff->generation && handoff->fence_value > d.prod_value) {
		if (!s->warned_ring_contract) {
			s->warned_ring_contract = true;
			U_LOG_W(
			    "#2317 hub: the producer re-nominated the slot the hub is reading (slot %u, fence %llu "
			    "-> %llu) — a ring-contract violation; leaving DIRECT ingress",
			    idx, (unsigned long long)d.prod_value, (unsigned long long)handoff->fence_value);
		}
		split_direct_to_xbridge(s, COMP_HUB_INGRESS_REASON_RING_CONTRACT);
		return;
	}
	if (handoff->shared_handle == nullptr || content_w == 0 || content_h == 0) {
		return; // nothing to nominate; split_submit_direct drops the frame
	}
	if (!split_direct_open_slot(s, idx, handoff)) {
		split_direct_to_xbridge(s, COMP_HUB_INGRESS_REASON_DIRECT_FAILED);
		return;
	}
	if (content_w > d.slots[idx].w || content_h > d.slots[idx].h) {
		split_direct_to_xbridge(s, COMP_HUB_INGRESS_REASON_DIRECT_FAILED);
	}
}

/*!
 * #2317 P2a: a RING producer's frame staged real 2D-plane content, which only
 * the xbridge ingress carries. Wire the bridge FIRST (so a failure leaves the
 * session exactly as it was: DIRECT, planes degraded alone), then stop reading
 * the deposit directly. Called on the producer's thread before this frame's
 * submit, so the frame that brought the planes is the first one bridged.
 *
 * The consumer fence and every release already owed stay alive: the producer
 * still waits for the DIRECT era's last reads, and the slot the hub was on is
 * owed now.
 */
static void
split_direct_to_xbridge(struct comp_hub_d3d11 *s, const char *reason)
{
	auto &d = s->direct;
	// Only a RING session ever moves (the stable atlas reads its planes in
	// place on DIRECT): this guard first, so a stable-atlas client-presents
	// session's plane frames never reach the branch below.
	if (!d.active || !d.ring || s->xbridge != nullptr || s->direct_to_xbridge_failed) {
		return;
	}
	if (s->client_presents) {
		// #2370: client-presents has no xbridge leg (no scanout device to
		// bridge to): the ring session stays DIRECT and the frame that asked
		// is woven from its slot without the move. Said once.
		s->direct_to_xbridge_failed = true;
		U_LOG_W("D3D11 hub client-presents: a ring frame asked for the xbridge ingress (%s), which this strategy "
		        "does not carry — the session stays DIRECT (#2370)",
		        reason != nullptr ? reason : "?");
		return;
	}
	const uint64_t t0 = os_monotonic_get_ns();
	if (!split_wire_xbridge(s, s->wire_fmt, s->wire_w, s->wire_h)) {
		s->direct_to_xbridge_failed = true;
		U_LOG_W(
		    "#2317 hub: 2D-plane content arrived but the comp_xbridge ingress could not be wired — the "
		    "session stays on DIRECT ingress and its 2D planes do not composite; the 3D weave is "
		    "unaffected");
		return;
	}
	if (d.cur >= 0 && !d.km && d.cons_value != 0) {
		d.release_owed[d.cur] = d.cons_value;
	}
	split_release_direct_slots(s);
	d.active = false;
	s->ingress_reason = reason;
	if (!s->placement_logged) {
		// #2317 P2a review: before the session's placement line, the move is
		// reported BY that line (ingress=xbridge ingress_reason=<reason>), so
		// the log never shows a CHANGED ahead of the line it corrects.
		return;
	}
	U_LOG_W(
	    "hub placement: CHANGED hub=1 ingress=xbridge reason=%s — this session's frames carry 2D-plane "
	    "content, which the DIRECT ingress does not transport; the comp_xbridge ingress takes over for the "
	    "rest of the session (wired in %.1f ms) (#2317 P2a)",
	    reason, (double)(os_monotonic_get_ns() - t0) / 1.0e6);
}

/*!
 * #2317 P2c: a ring frame staged real 2D-plane content. A FENCE-mode producer
 * that declared @ref comp_hub_d3d11_info::ring_direct_planes keeps DIRECT (its
 * planes are read in place, their rewrite ordered on the consumer fence); any
 * other moves to xbridge (P2a's `plane_content`). A KEYED-MUTEX deposit has no
 * consumer fence to order the rewrite on, and DIRECT samples the plane in place
 * on every repaint, which widens the #1274 timing-only race beyond xbridge's
 * (whose copy lands in a hub-owned slot) — so it keeps the P2a move (#2345
 * review). The stable atlas never moves (split_direct_to_xbridge is ring-only).
 */
static void
split_direct_demote_for_planes(struct comp_hub_d3d11 *s)
{
	if (s->ring_direct_planes && !s->direct.km) {
		return;
	}
	split_direct_to_xbridge(s, COMP_HUB_INGRESS_REASON_PLANES);
}

extern "C" bool
comp_hub_d3d11_weave_and_present(struct comp_hub_d3d11 *s, bool is_repaint, const struct xrt_rect *canvas)
{
	if (s != nullptr && s->client_presents) {
		// #2370: the canvas is the caller's target (comp_hub_d3d11_set_client_target).
		(void)canvas;
		return split_weave_client(s, is_repaint);
	}
	if (s != nullptr && s->direct.active) {
		return split_weave_direct(s, is_repaint, canvas);
	}
	if (s == nullptr || s->xbridge == nullptr || s->target == nullptr || s->dp == nullptr) {
		return false;
	}

	/*
	 * THE SLOT IS PICKED BEFORE A SINGLE COMMAND IS RECORDED. Both #918 F4 and
	 * R1 depend on that ordering: deciding after the back buffer had been
	 * acquired and cleared would force a choice between presenting black and
	 * abandoning a half-recorded frame.
	 */
	const uint64_t want_gen = s->layout_gen;
	int32_t slot = -1;
	if (is_repaint) {
		slot = comp_xbridge_get_weave_slot(s->xbridge);
	} else {
		slot = comp_xbridge_pick_slot(s->xbridge, want_gen);
		if (slot < 0) {
			/*
			 * #918 R1, the TRANSITION pick. Presenting nothing here is not
			 * neutral: with FLIP_DISCARD the panel keeps showing the frame it
			 * was last given, which was woven for the mode the display has
			 * just left. Weaving the in-flight slot costs a GPU-side wait and
			 * the frame goes out correct.
			 */
			slot = comp_xbridge_pick_inflight_slot(s->xbridge, want_gen);
			if (slot >= 0) {
				s->inflight_weaves++;
			}
		}
	}

	uint64_t slot_gen = 0;
	uint32_t slot_cw = 0, slot_ch = 0;
	if (slot >= 0) {
		if (!comp_xbridge_slot_layout(s->xbridge, slot, &slot_gen, &slot_cw, &slot_ch) || slot_cw == 0 ||
		    slot_ch == 0) {
			slot = -1;
		} else if (slot_gen != want_gen) {
			// A slot composited under a layout the display processor has
			// since switched away from must never be woven.
			s->stale_refusals++;
			slot = -1;
		}
	}

	// #918 F7 — a repaint re-weaves a slot it did not pick itself, so nothing
	// else orders it against a consumer copy rewriting that same slot.
	if (slot >= 0 && is_repaint && !comp_xbridge_slot_ready(s->xbridge, slot)) {
		slot = -1;
	}

	if (slot < 0) {
		// #918 F4 — present NOTHING. The panel holds the last woven frame.
		s->no_slot++;
		return false;
	}

	comp_xbridge_gpu_wait_slot(s->xbridge, slot);
	ID3D11ShaderResourceView *atlas_srv = (ID3D11ShaderResourceView *)comp_xbridge_get_srv(s->xbridge, slot);
	if (atlas_srv == nullptr) {
		s->no_slot++;
		return false;
	}

	/*
	 * #1178 — the CONTENT probe, off unless DXR_SPLIT_CONTENT_PROBE=1. This is the
	 * weave thread, which is where it has to run: it drives the output immediate
	 * context. It reads the EGRESS slot rather than the (possibly cropped) SRV
	 * above, because the destination of the bridge's own copy is the thing whose
	 * emptiness no other diagnostic can see.
	 */
	comp_xbridge_content_probe(s->xbridge, slot);

	/*
	 * #1140 — the weave's geometry comes from the snapshot minted with the
	 * generation just proved, and its content box from the SLOT's own stamp,
	 * never from live CPU state. During a resize the slot is a frame behind the
	 * window, and cropping it to the current frame's box would slice the tiles
	 * at the wrong stride.
	 */
	const uint32_t weave_cols = s->gen_cols != 0 ? s->gen_cols : 1;
	const uint32_t weave_rows = s->gen_rows != 0 ? s->gen_rows : 1;
	const uint32_t weave_view_w = slot_cw / weave_cols;
	const uint32_t weave_view_h = slot_ch / weave_rows;
	if (weave_view_w == 0 || weave_view_h == 0) {
		s->no_slot++;
		return false;
	}

	// The residual crop, only when the ring is oversized (R2 hysteresis).
	uint32_t eg_w = 0, eg_h = 0;
	comp_xbridge_get_egress_dims(s->xbridge, &eg_w, &eg_h);
	if (slot_cw != eg_w || slot_ch != eg_h) {
		atlas_srv = split_crop_for_dp(s, atlas_srv, slot_cw, slot_ch);
	}

	uint32_t target_index = 0;
	if (comp_d3d11_target_acquire(s->target, &target_index) != XRT_SUCCESS) {
		s->no_slot++;
		return false;
	}

	uint32_t tgt_w = 0, tgt_h = 0;
	comp_d3d11_target_get_dimensions(s->target, &tgt_w, &tgt_h);
	split_check_target_follows_window(s, tgt_w, tgt_h);

	/*
	 * Multi-screen M6 (ADR-047 Amendment 1): a window spanning screens is
	 * woven per segment on THIS device. Run first: the window-handle hand-off
	 * (Amendment 2) may swap s->dp, and everything below feeds s->dp.
	 */
	struct comp_seg_rect seg_window = {0, 0, 0, 0};
	const bool seg_split = split_segments_update(s, tgt_w, tgt_h, &seg_window);

	// Late-weave pacing + the weave-latency harness mark, on the scanout
	// adapter where the present now happens.
	if (is_repaint) {
		comp_d3d11_target_weave_mark_repaint(s->target, s->mode_3d);
	} else {
		// #2317 P1c: the caller's xrWaitFrame promise for this frame, so the
		// DXR_WEAVE_LATENCY_CSV site emits the D (prediction error) rows the
		// tiers' own sites do. 0 (never set) keeps the old no-D-row behaviour.
		// #2327: the promise of the frame that FILLED this slot (by its seq) —
		// the newest weavable slot can be an earlier frame than the one most
		// recently submitted, and pairing it with that frame's promise reads a
		// period early. Unknown seq = no D row, never a mispaired one.
		uint64_t slot_seq = 0;
		const uint64_t pdt =
		    comp_xbridge_slot_seq(s->xbridge, slot, &slot_seq) ? split_pdt_get(s, slot_seq) : 0;
		comp_d3d11_target_weave_mark(s->target, pdt, s->mode_3d);
	}

	/*
	 * Hand the vendor eye predictor last frame's MEASURED weave->scanout
	 * residual, so it runs with an exact horizon (0 = unknown, DP heuristic)
	 * -- AND the display period, which this arm passed as a literal 0.
	 *
	 * `frame_period_ns` is documented "Display refresh period; 0 = unknown",
	 * and the app's own d3d12 arm has always passed the real value
	 * (U_TIME_1S_IN_NS / display_refresh_rate). Only the reroute's fill arm
	 * said "unknown", leaving the DP to infer the period itself. The governor
	 * on this target already knows it, so there is no reason to withhold it.
	 *
	 * NOT a fix for the #1339 lateral shiver -- that was the hypothesis this
	 * change was written to test, and it was WRONG: with the real period
	 * passed (verified non-zero, log shows "display period 16.7 ms") the
	 * shiver is unchanged. Kept because the inconsistency is real on its own
	 * terms: two arms told the same DP different things about the same panel.
	 */
	xrt_display_processor_d3d11_set_frame_timing(s->dp, comp_d3d11_target_get_measured_weave_ns(s->target),
	                                             comp_d3d11_target_get_display_period_ns(s->target));

	// #206: and the FORWARD-computed horizon for THIS weave, from the
	// vsync-locked vblank grid — exact per weave, no estimator lag under
	// variable cadence. 0 = no trusted grid; the DP then keeps the
	// retrospective value above.
	xrt_display_processor_d3d11_set_predicted_scanout(
	    s->dp, comp_d3d11_target_predict_weave_to_scanout_ns(s->target));

	comp_d3d11_target_bind(s->target);

	/*
	 * VK-1b — hand the display processor THIS SLOT's backdrop, read from the
	 * slot's own recipe rather than from live CPU state (#1140). The slot the
	 * weave is consuming was filled by an earlier frame, so a backdrop resolved
	 * from the current frame would pair one frame's pixels with another's
	 * extent. Must precede process_atlas.
	 *
	 * A NULL SRV here is the correct clear, not a failure: it means the frame
	 * that filled this slot produced no 2D-under layers, and the display
	 * processor falls back to the captured desktop alone.
	 */
	split_set_backdrop(s, slot);

	struct xrt_rect cv = {};
	if (canvas != nullptr) {
		cv = *canvas;
	}
	if (seg_split) {
		/*
		 * The D3D11 segment manager, exactly as the in-process D3D11
		 * compositor drives it under its own split: crop per segment out of
		 * the egress slot, each screen's DP over its canvas, flat 2D through
		 * the output composite unit where a segment cannot weave.
		 *
		 * #1883: the slot was painted a frame or two before the frame being
		 * committed, so its per-segment mosaic sits where THAT frame's routing
		 * put it; the crop reads the slot's own partition (by its sequence).
		 * A slot no longer in the ring is treated as unrouted (live crop).
		 */
		struct comp_segments_content slot_content = {};
		{
			uint64_t slot_seq = 0;
			if (!comp_xbridge_slot_seq(s->xbridge, slot, &slot_seq) ||
			    !comp_segments_content_ring_get(&s->seg_content_ring, slot_seq, &slot_content)) {
				memset(&slot_content, 0, sizeof(slot_content));
			}
		}
		struct comp_d3d11_segments_frame sf = {};
		sf.content = &slot_content;
		sf.context = s->out_ctx;
		sf.src_srv = atlas_srv;
		sf.view_width = weave_view_w;
		sf.view_height = weave_view_h;
		sf.tile_columns = weave_cols;
		sf.tile_rows = weave_rows;
		sf.src_format = (uint32_t)split_dp_view_format(atlas_srv);
		sf.target_rtv = comp_d3d11_target_get_rtv(s->target);
		sf.target_width = tgt_w;
		sf.target_height = tgt_h;
		sf.canvas.x = 0;
		sf.canvas.y = 0;
		sf.canvas.w = seg_window.w < tgt_w ? seg_window.w : tgt_w;
		sf.canvas.h = seg_window.h < tgt_h ? seg_window.h : tgt_h;
		sf.transparent_background = s->transparent_background;
		sf.atlas_encoding = -1; // the split declares none to its own DP either
		sf.primary_dp = s->dp;
		sf.outcomp = split_ensure_outcomp(s) ? s->outcomp : nullptr;
		(void)comp_d3d11_segments_record(s->segments, &sf);
		// Everything after sees the state the single-DP path leaves.
		comp_d3d11_target_bind(s->target);
	} else {
		xrt_display_processor_d3d11_process_atlas(s->dp, s->out_ctx, atlas_srv, weave_view_w, weave_view_h,
		                                          weave_cols, weave_rows, (uint32_t)split_dp_view_format(atlas_srv),
		                                          tgt_w, tgt_h, cv.offset.w, cv.offset.h, (uint32_t)cv.extent.w,
		                                          (uint32_t)cv.extent.h);
	}
	// #2342 P2c: an armed capture's DP input (the slot's content box).
	split_capture_dp_input(s, is_repaint, atlas_srv, slot_cw, slot_ch);

	/*
	 * VK-1b-2 — the masked 2D-over-3D composite, then the HUD, both on the OUT
	 * device and both over the frame the weave has just written. This is the tail
	 * the split moved to the scanout adapter along with the weave itself; every
	 * input belongs to that device, which is the single rule
	 * comp_d3d11_outcomp states and the plane transports exist to satisfy.
	 */
	{
		auto *dst = static_cast<ID3D11Texture2D *>(comp_d3d11_target_get_back_buffer(s->target));
		(void)split_composite(s, slot, is_repaint, dst);
		split_draw_hud(s, dst, tgt_w, tgt_h);
		split_capture_woven(s, is_repaint, dst, tgt_w, tgt_h);
		if (s->post_weave_fn != nullptr) {
			s->post_weave_fn(s->post_weave_userdata, s->out_dev, s->out_ctx, dst, tgt_w, tgt_h, is_repaint);
		}
	}

	if (!is_repaint) {
		// Publish the slot the app frame wove, so a repaint replays exactly
		// that one with zero bridge traffic.
		comp_xbridge_set_weave_slot(s->xbridge, slot);
	}

	return comp_d3d11_target_present(s->target, 1) == XRT_SUCCESS;
}

extern "C" bool
comp_hub_d3d11_is_client_presents(struct comp_hub_d3d11 *s)
{
	return s != nullptr && s->client_presents;
}

extern "C" void
comp_hub_d3d11_set_client_target(struct comp_hub_d3d11 *s, bool valid, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	if (s == nullptr || !s->client_presents) {
		return;
	}
	s->client.target_valid = valid && w > 0 && h > 0;
	s->client.target.x = x;
	s->client.target.y = y;
	s->client.target.w = w;
	s->client.target.h = h;
}

extern "C" void
comp_hub_d3d11_get_client_weave_state(struct comp_hub_d3d11 *s, bool *out_segmented, bool *out_any_flat)
{
	*out_segmented = s != nullptr && s->client_presents && s->client.segmented;
	*out_any_flat = s != nullptr && s->client_presents && s->client.any_flat;
}

extern "C" bool
comp_hub_d3d11_has_weave_slot(struct comp_hub_d3d11 *s)
{
	if (s != nullptr && s->direct.active) {
		return s->direct.have_frame;
	}
	return s != nullptr && s->xbridge != nullptr && comp_xbridge_get_weave_slot(s->xbridge) >= 0;
}

extern "C" bool
comp_hub_d3d11_is_direct(struct comp_hub_d3d11 *s)
{
	return s != nullptr && s->direct.active;
}

extern "C" void
comp_hub_d3d11_pre_render(struct comp_hub_d3d11 *s)
{
	if (s == nullptr || !s->stable_atlas) {
		return;
	}
	if (s->direct.active) {
		// GPU-side: the producer's passes start only after the hub's last read.
		if (s->app_ctx4 != nullptr && s->direct.cons_fence_app != nullptr && s->direct.cons_value != 0) {
			s->app_ctx4->Wait(s->direct.cons_fence_app, s->direct.cons_value);
		}
		return;
	}
	if (s->xbridge != nullptr) {
		comp_xbridge_pre_render(s->xbridge);
	}
}

extern "C" void
comp_hub_d3d11_set_predicted_display_time(struct comp_hub_d3d11 *s, uint64_t predicted_display_time_ns)
{
	if (s != nullptr) {
		s->predicted_display_time_ns = predicted_display_time_ns;
		// #2327: keyed by the frame just submitted (callers set this after
		// comp_hub_d3d11_submit_atlas); the first set per frame wins, so a
		// submit that nominated nothing never relabels the previous frame.
		if (s->seq != 0 && s->seq != s->pdt_seq) {
			split_pdt_put(s, s->seq, predicted_display_time_ns);
			s->pdt_seq = s->seq;
		}
	}
}

extern "C" void
comp_hub_d3d11_set_display_period(struct comp_hub_d3d11 *s, uint64_t period_ns)
{
	if (s != nullptr && s->client_presents && period_ns != 0) {
		s->client.period_ns = period_ns; // #2370: no target to hold it
		return;
	}
	if (s == nullptr || s->target == nullptr || period_ns == 0) {
		return;
	}
	comp_d3d11_target_set_display_period(s->target, period_ns);
}

extern "C" uint64_t
comp_hub_d3d11_get_predicted_lookahead_ns(struct comp_hub_d3d11 *s)
{
	return (s != nullptr && s->target != nullptr) ? comp_d3d11_target_get_predicted_lookahead_ns(s->target) : 0;
}

extern "C" void
comp_hub_d3d11_mark_wait_frame(struct comp_hub_d3d11 *s)
{
	if (s != nullptr && s->target != nullptr) {
		comp_d3d11_target_mark_wait_frame(s->target);
	}
}

extern "C" void
comp_hub_d3d11_set_post_weave_hook(struct comp_hub_d3d11 *s, comp_hub_d3d11_post_weave_fn fn, void *userdata)
{
	if (s != nullptr) {
		s->post_weave_fn = fn;
		s->post_weave_userdata = userdata;
	}
}

extern "C" void
comp_hub_d3d11_capture_arm(struct comp_hub_d3d11 *s, bool projection_only, const char *path, const char *woven_path)
{
	if (s == nullptr) {
		return;
	}
	// Arm even with an empty path: the write fails and the intent completes as
	// failed (an early return here would leave _take false and the tier's
	// intent re-arming every frame). The buffers hold MCP_CAPTURE_PATH_MAX (256)
	// plus the ".woven" infix with room to spare.
	auto &c = s->cap;
	c = {};
	c.armed = true;
	c.projection_only = projection_only;
	snprintf(c.path, sizeof(c.path), "%s", path != nullptr ? path : "");
	snprintf(c.woven_path, sizeof(c.woven_path), "%s", woven_path != nullptr ? woven_path : "");
}

extern "C" bool
comp_hub_d3d11_capture_take(struct comp_hub_d3d11 *s, bool *out_ok)
{
	if (out_ok != nullptr) {
		*out_ok = false;
	}
	if (s == nullptr || (!s->cap.armed && !s->cap.served)) {
		return false;
	}
	const bool ok = s->cap.served && s->cap.ok;
	if (!s->cap.served) {
		// Armed, but the app frame wove nothing (#918 F4): consumed unserved.
		U_LOG_W("#2342 hub capture: the app frame wove nothing — %s not written", s->cap.path);
	}
	s->cap = {};
	if (out_ok != nullptr) {
		*out_ok = ok;
	}
	return true;
}

extern "C" uint64_t
comp_hub_d3d11_get_hub_luid(struct comp_hub_d3d11 *s)
{
	if (s == nullptr) {
		return 0;
	}
	return ((uint64_t)(uint32_t)s->out_luid.HighPart << 32) | (uint64_t)(uint32_t)s->out_luid.LowPart;
}

extern "C" void *
comp_hub_d3d11_get_consumer_fence_handle(struct comp_hub_d3d11 *s)
{
	if (s == nullptr || !s->direct.ring) {
		return nullptr;
	}
	return (void *)s->direct.cons_handle;
}

extern "C" uint64_t
comp_hub_d3d11_get_plane_release_value(struct comp_hub_d3d11 *s)
{
	// Fence-mode ring sessions only; survives a move to xbridge (the fence and
	// its last value outlive the DIRECT era, see split_direct_to_xbridge).
	if (s == nullptr || !s->direct.ring || s->direct.km || s->direct.cons_fence == nullptr) {
		return 0;
	}
	return s->direct.cons_value;
}

extern "C" bool
comp_hub_d3d11_take_slot_release(struct comp_hub_d3d11 *s, uint32_t *out_slot, uint64_t *out_value)
{
	if (s == nullptr) {
		return false;
	}
	for (uint32_t i = 0; i < COMP_HUB_D3D11_DIRECT_RING; i++) {
		if (s->direct.release_owed[i] != 0) {
			*out_slot = i;
			*out_value = s->direct.release_owed[i];
			s->direct.release_owed[i] = 0;
			return true;
		}
	}
	return false;
}

extern "C" void
comp_hub_d3d11_leave_direct(struct comp_hub_d3d11 *s, const char *reason)
{
	if (s != nullptr) {
		split_direct_to_xbridge(s, reason != nullptr ? reason : COMP_HUB_INGRESS_REASON_DIRECT_FAILED);
	}
}

extern "C" const char *
comp_hub_d3d11_ingress_reason(struct comp_hub_d3d11 *s)
{
	if (s == nullptr) {
		return nullptr;
	}
	return s->direct.active ? nullptr : s->ingress_reason;
}

extern "C" void
comp_hub_d3d11_log_placement(struct comp_hub_d3d11 *s, const char *producer)
{
	if (s == nullptr) {
		return;
	}
	const char *why = comp_hub_d3d11_ingress_reason(s);
	s->placement_logged = true;
	if (s->client_presents) {
		// #2370: its own line shape; `presenter=client` is the parse target.
		U_LOG_W(
		    "hub placement: hub=1 render=%08lx:%08lx hub_dev=%08lx:%08lx ingress=%s presenter=client — weave on "
		    "the D3D11 hub's own device into the app's shared texture; the app presents (#2370; %s ring "
		    "deposit; forced by the DXR_HUB env var)",
		    (unsigned long)s->render_luid.HighPart, (unsigned long)s->render_luid.LowPart,
		    (unsigned long)s->out_luid.HighPart, (unsigned long)s->out_luid.LowPart,
		    s->direct.active ? "direct" : "xbridge", producer != nullptr ? producer : "?");
		return;
	}
	U_LOG_W(
	    "hub placement: hub=1 render=%08lx:%08lx hub_dev=%08lx:%08lx ingress=%s%s%s%s%s — weave/present on "
	    "the D3D11 hub's own device (#2317 P2a; %s ring deposit%s)",
	    (unsigned long)s->render_luid.HighPart, (unsigned long)s->render_luid.LowPart,
	    (unsigned long)s->out_luid.HighPart, (unsigned long)s->out_luid.LowPart,
	    s->direct.active ? "direct" : "xbridge", why != nullptr ? " ingress_reason=" : "",
	    why != nullptr ? why : "", s->route_reason != nullptr ? " reason=" : "",
	    s->route_reason != nullptr ? s->route_reason : "", producer != nullptr ? producer : "?",
	    s->direct.active ? (s->direct.km ? ", keyed-mutex mode" : ", fence mode")
	                     : "; DXR_HUB_INGRESS=direct opts in to DIRECT on one adapter");
}

extern "C" void
comp_hub_d3d11_set_route_reason(struct comp_hub_d3d11 *s, const char *reason)
{
	if (s != nullptr) {
		s->route_reason = reason;
	}
}

extern "C" bool
comp_hub_d3d11_repaint_admit(struct comp_hub_d3d11 *s)
{
	return s == nullptr || s->target == nullptr || comp_d3d11_target_repaint_admit(s->target);
}

extern "C" void *
comp_hub_d3d11_repaint_pace_hold(struct comp_hub_d3d11 *s)
{
	return (s != nullptr && s->target != nullptr) ? comp_d3d11_target_repaint_pace_hold(s->target) : nullptr;
}

extern "C" void
comp_hub_d3d11_repaint_pace_wait(void *hold)
{
	comp_d3d11_target_repaint_pace_held(hold);
}

extern "C" enum comp_hub_d3d11_repaint_result
comp_hub_d3d11_repaint(struct comp_hub_d3d11 *s, bool partition_engaged, const struct xrt_rect *canvas)
{
	if (s != nullptr && s->client_presents) {
		// #2370: suppressed — see the header. Counted, one lifecycle WARN.
		s->client.repaints_refused++;
		if (!s->client.repaint_logged) {
			s->client.repaint_logged = true;
			U_LOG_W(
			    "D3D11 hub client-presents: repaint suppressed — the caller presents its texture on its own "
			    "cadence; a re-weave between its frames has no fence it could wait on (#2370)");
		}
		return COMP_HUB_D3D11_REPAINT_NOT_ADMITTED;
	}
	if (!comp_hub_d3d11_has_weave_slot(s)) {
		return COMP_HUB_D3D11_REPAINT_NO_FRAME;
	}
	// #1339 under the lock, as natively: the app frame waits on the same
	// waitable while holding it, so a token taken outside could be the app's.
	if (!partition_engaged && !comp_hub_d3d11_repaint_admit(s)) {
		return COMP_HUB_D3D11_REPAINT_NOT_ADMITTED;
	}
	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("#2342 hub: repaints paced and admitted by the hub (the native D3D11 leg's shape)");
	}
	return comp_hub_d3d11_weave_and_present(s, /*is_repaint=*/true, canvas) ? COMP_HUB_D3D11_REPAINT_FIRED
	                                                                        : COMP_HUB_D3D11_REPAINT_NOT_WOVEN;
}

extern "C" void
comp_hub_d3d11_set_app_paced(struct comp_hub_d3d11 *s, bool paced)
{
	if (s == nullptr || s->target == nullptr) {
		return;
	}
	comp_d3d11_target_set_app_paced(s->target, paced);
}

extern "C" void
comp_hub_d3d11_render_diag(struct comp_hub_d3d11 *s)
{
	if (s != nullptr && s->direct.active) {
		const uint64_t dnow = os_monotonic_get_ns();
		if (dnow - s->diag_window_ns < 10ULL * U_TIME_1S_IN_NS) {
			return;
		}
		s->diag_window_ns = dnow;
		if (s->client_presents) {
			// #2370: the client-presents strategy: no transport, no present.
			U_LOG_W(
			    "[RENDER] split=1 hub_ingress=direct presenter=client handoffs=%llu repaints_refused=%llu "
			    "frames=%llu reopens=%llu no_slot=%llu out_crop=%llu (session totals)",
			    (unsigned long long)s->client.handoffs, (unsigned long long)s->client.repaints_refused,
			    (unsigned long long)s->direct.frames, (unsigned long long)s->direct.reopens,
			    (unsigned long long)s->no_slot, (unsigned long long)s->out_crop);
			return;
		}
		// #2317 P1b: no transport, so no xb_kb / ingress counters to report.
		U_LOG_W(
		    "[RENDER] split=1 hub_ingress=direct ring=%d km=%d km_skips=%llu frames=%llu reopens=%llu "
		    "no_slot=%llu out_crop=%llu (session totals)",
		    s->direct.ring ? 1 : 0, s->direct.km ? 1 : 0, (unsigned long long)s->direct.km_skips,
		    (unsigned long long)s->direct.frames, (unsigned long long)s->direct.reopens,
		    (unsigned long long)s->no_slot, (unsigned long long)s->out_crop);
		return;
	}
	if (s == nullptr || s->xbridge == nullptr) {
		return;
	}

	const uint64_t now = os_monotonic_get_ns();
	if (now - s->diag_window_ns < 10ULL * U_TIME_1S_IN_NS) {
		return;
	}
	s->diag_window_ns = now;

	const uint64_t xb_bytes = comp_xbridge_take_atlas_bytes(s->xbridge);
	struct comp_xbridge_ingress_stats ing = {};
	comp_xbridge_take_ingress_stats(s->xbridge, &ing);
	const char *ing_name = ing.mode == COMP_XBRIDGE_INGRESS_ADAPTIVE ? "adaptive"
	                       : ing.mode == COMP_XBRIDGE_INGRESS_DIRECT ? "direct"
	                       : ing.mode == COMP_XBRIDGE_INGRESS_STAGED ? "staged"
	                                                                 : "none";

	U_LOG_W(
	    "[RENDER] split=1 xb_kb=%llu xb_degraded=%d no_slot=%llu out_crop=%llu ingress=%s "
	    "ing_direct=%llu ing_staged=%llu ing_rebind=%llu ing_churn=%llu ing_leak=%llu window_s=10",
	    (unsigned long long)(xb_bytes / 1024u), (int)comp_xbridge_is_degraded(s->xbridge),
	    (unsigned long long)s->no_slot, (unsigned long long)s->out_crop, ing_name, (unsigned long long)ing.direct,
	    (unsigned long long)ing.staged, (unsigned long long)ing.rebind, (unsigned long long)ing.churn,
	    (unsigned long long)ing.leak);

	/*
	 * VK-1b — one line per live PLANE, in the same shape and with the same field
	 * names as the two D3D legs', so one parser reads all three. Emitted only for
	 * a plane that has actually transported something, so a projection-only
	 * session's log is byte-identical to what it was before the planes existed.
	 */
	for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
		uint64_t pb = 0, pc = 0, ps = 0;
		bool half = false;
		comp_xbridge_take_plane_stats(s->xbridge, p, &pb, &pc, &ps, &half);
		if (pc == 0 && ps == 0) {
			continue;
		}
		U_LOG_W("[RENDER] plane=%s kb=%llu copies=%llu skips=%llu half_rate=%d window_s=10",
		        comp_xbridge_plane_label(p), (unsigned long long)(pb / 1024u), (unsigned long long)pc,
		        (unsigned long long)ps, (int)half);
	}
	if (s->sideband_copies != 0 || s->sideband_skips != 0) {
		U_LOG_W("[RENDER] sideband copies=%llu skips=%llu kb=%llu window_s=10",
		        (unsigned long long)s->sideband_copies, (unsigned long long)s->sideband_skips,
		        (unsigned long long)(s->sideband_bytes / 1024u));
		s->sideband_copies = 0;
		s->sideband_skips = 0;
		s->sideband_bytes = 0;
	}
}


/*
 *
 * Display-processor forwarding.
 *
 */

/*
 * Every forwarder below runs on the APP thread and holds dp_mutex across the
 * call: the weave may swap s->dp for another screen's DP (ADR-047 Amendment 2,
 * a window-handle hand-off), and the old one is destroyed right after.
 */

extern "C" bool
comp_hub_d3d11_get_predicted_eye_positions(struct comp_hub_d3d11 *s, struct xrt_eye_positions *out_eye_pos)
{
	if (s == nullptr) {
		return false;
	}
	os_mutex_lock(&s->dp_mutex);
	const bool ok = s->dp != nullptr && xrt_display_processor_d3d11_get_predicted_eye_positions(s->dp, out_eye_pos);
	os_mutex_unlock(&s->dp_mutex);
	return ok;
}

extern "C" bool
comp_hub_d3d11_has_eye_positions_at(struct comp_hub_d3d11 *s)
{
	if (s == nullptr) {
		return false;
	}
	os_mutex_lock(&s->dp_mutex);
	const bool has = s->dp != nullptr && xrt_display_processor_d3d11_has_eye_positions_at(s->dp);
	os_mutex_unlock(&s->dp_mutex);
	return has;
}

extern "C" bool
comp_hub_d3d11_get_eye_positions_at(struct comp_hub_d3d11 *s, int64_t target_ns, struct xrt_eye_positions *out_eye_pos)
{
	if (s == nullptr) {
		return false;
	}
	os_mutex_lock(&s->dp_mutex);
	const bool ok =
	    s->dp != nullptr && xrt_display_processor_d3d11_get_eye_positions_at(s->dp, target_ns, out_eye_pos);
	os_mutex_unlock(&s->dp_mutex);
	return ok;
}

extern "C" bool
comp_hub_d3d11_get_display_dimensions(struct comp_hub_d3d11 *s, float *out_width_m, float *out_height_m)
{
	if (s == nullptr) {
		return false;
	}
	os_mutex_lock(&s->dp_mutex);
	const bool ok =
	    s->dp != nullptr && xrt_display_processor_d3d11_get_display_dimensions(s->dp, out_width_m, out_height_m);
	os_mutex_unlock(&s->dp_mutex);
	return ok;
}

extern "C" bool
comp_hub_d3d11_get_display_pixel_info(
    struct comp_hub_d3d11 *s, uint32_t *out_width_px, uint32_t *out_height_px, int32_t *out_left, int32_t *out_top)
{
	if (s == nullptr) {
		return false;
	}
	os_mutex_lock(&s->dp_mutex);
	const bool ok = s->dp != nullptr && xrt_display_processor_d3d11_get_display_pixel_info(
	                                        s->dp, out_width_px, out_height_px, out_left, out_top);
	os_mutex_unlock(&s->dp_mutex);
	return ok;
}

extern "C" bool
comp_hub_d3d11_request_display_mode(struct comp_hub_d3d11 *s, bool enable_3d)
{
	if (s == nullptr) {
		return false;
	}
	os_mutex_lock(&s->dp_mutex);
	// #2247: the primary screen stays 2D while the window has wholly left it;
	// the weave restores mode_3d when it returns.
	const bool ok = s->dp != nullptr &&
	                xrt_display_processor_d3d11_request_display_mode(s->dp, enable_3d && !s->seg_primary_uncovered);
	if (ok) {
		// The weave marks' mode_3d source — see the field doc.
		s->mode_3d = enable_3d;
	}
	os_mutex_unlock(&s->dp_mutex);
	return ok;
}

extern "C" void
comp_hub_d3d11_set_eye_tracking_mode(struct comp_hub_d3d11 *s, uint32_t mode)
{
	if (s == nullptr) {
		return;
	}
	os_mutex_lock(&s->dp_mutex);
	// Remembered for a swapped-in DP (Amendment 2).
	s->eye_tracking_mode = mode;
	s->eye_tracking_mode_set = true;
	if (s->dp != nullptr) {
		xrt_display_processor_d3d11_set_eye_tracking_mode(s->dp, mode);
	}
	os_mutex_unlock(&s->dp_mutex);
}

extern "C" bool
comp_hub_d3d11_accepts_atlas_encoding(struct comp_hub_d3d11 *s, enum xrt_atlas_encoding encoding)
{
	if (s == nullptr) {
		return false;
	}
	os_mutex_lock(&s->dp_mutex);
	const bool ok = s->dp != nullptr && xrt_display_processor_d3d11_accepts_atlas_encoding(s->dp, encoding);
	os_mutex_unlock(&s->dp_mutex);
	return ok;
}

extern "C" void
comp_hub_d3d11_set_atlas_encoding(struct comp_hub_d3d11 *s, enum xrt_atlas_encoding encoding)
{
	if (s == nullptr) {
		return;
	}
	os_mutex_lock(&s->dp_mutex);
	// Remembered for a swapped-in DP (Amendment 2).
	s->atlas_encoding = encoding;
	s->atlas_encoding_set = true;
	if (s->dp != nullptr) {
		xrt_display_processor_d3d11_set_atlas_encoding(s->dp, encoding);
	}
	os_mutex_unlock(&s->dp_mutex);
}

extern "C" bool
comp_hub_d3d11_get_background_preview(struct comp_hub_d3d11 *s, struct xrt_dp_background_preview *out_preview)
{
	if (s == nullptr || out_preview == nullptr) {
		return false;
	}
	// Pixels only. The inline wrapper already answers false for a plug-in whose
	// vtable predates the slot, so an older weaver is "no source", not a crash.
	os_mutex_lock(&s->dp_mutex);
	const bool ok = s->dp != nullptr && xrt_display_processor_d3d11_get_background_preview(s->dp, out_preview);
	os_mutex_unlock(&s->dp_mutex);
	return ok;
}

#else /* !(XRT_OS_WINDOWS && XRT_HAVE_D3D11) */

extern "C" xrt_result_t
comp_hub_d3d11_stage_a(const struct comp_hub_d3d11_info *info,
                       struct comp_hub_d3d11 **out_split,
                       const char **out_short_reason)
{
	(void)info;
	*out_split = NULL;
	/*
	 * COMP_SPLIT_REASON_API_UNSUPPORTED, spelled out: comp_split_gate.h and the
	 * transport behind it are linked on Windows only, so the token cannot be
	 * referenced by name here. It is a fixed parse target and is never
	 * reformatted — see comp_split_gate.h for the catalogue.
	 */
	*out_short_reason = "api_unsupported";
	return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
}

extern "C" bool
comp_hub_d3d11_wire_bridge(struct comp_hub_d3d11 *split, const struct comp_hub_d3d11_handoff *handoff)
{
	(void)split;
	(void)handoff;
	return false;
}

extern "C" void
comp_hub_d3d11_destroy(struct comp_hub_d3d11 **split_ptr)
{
	(void)split_ptr;
}

extern "C" void
comp_hub_d3d11_retire(struct comp_hub_d3d11 **split_ptr, const char *why, const char *short_reason)
{
	(void)split_ptr;
	(void)why;
	(void)short_reason;
}

extern "C" void
comp_hub_d3d11_submit_atlas(struct comp_hub_d3d11 *split,
                            const struct comp_hub_d3d11_handoff *handoff,
                            uint32_t cols,
                            uint32_t rows,
                            uint32_t view_w,
                            uint32_t view_h)
{
	(void)split;
	(void)handoff;
	(void)cols;
	(void)rows;
	(void)view_w;
	(void)view_h;
}

extern "C" bool
comp_hub_d3d11_stage_backdrop(struct comp_hub_d3d11 *split,
                              void *nt_handle,
                              uint64_t generation,
                              uint32_t alloc_w,
                              uint32_t alloc_h,
                              uint64_t content_seq,
                              int32_t dirty_x,
                              int32_t dirty_y,
                              uint32_t dirty_w,
                              uint32_t dirty_h,
                              uint32_t bd_w,
                              uint32_t bd_h)
{
	(void)split;
	(void)nt_handle;
	(void)generation;
	(void)alloc_w;
	(void)alloc_h;
	(void)content_seq;
	(void)dirty_x;
	(void)dirty_y;
	(void)dirty_w;
	(void)dirty_h;
	(void)bd_w;
	(void)bd_h;
	return false;
}

extern "C" void
comp_hub_d3d11_invalidate_plane(struct comp_hub_d3d11 *split, uint32_t plane)
{
	(void)split;
	(void)plane;
}

extern "C" bool
comp_hub_d3d11_raster_mask(struct comp_hub_d3d11 *split,
                           uint32_t kind,
                           const struct xrt_rect *rects,
                           const float *feather_px,
                           uint32_t rect_count,
                           uint32_t region_w,
                           uint32_t region_h)
{
	(void)split;
	(void)kind;
	(void)rects;
	(void)feather_px;
	(void)rect_count;
	(void)region_w;
	(void)region_h;
	return false;
}

extern "C" bool
comp_hub_d3d11_stage_local2d(struct comp_hub_d3d11 *split,
                             void *nt_handle,
                             uint64_t generation,
                             uint32_t alloc_w,
                             uint32_t alloc_h,
                             uint64_t content_seq,
                             int32_t dirty_x,
                             int32_t dirty_y,
                             uint32_t dirty_w,
                             uint32_t dirty_h,
                             uint32_t region_w,
                             uint32_t region_h,
                             int32_t cx,
                             int32_t cy,
                             uint32_t cw,
                             uint32_t ch,
                             uint32_t composite_mode,
                             bool opaque_present,
                             bool mask_is_plane)
{
	(void)split;
	(void)nt_handle;
	(void)generation;
	(void)alloc_w;
	(void)alloc_h;
	(void)content_seq;
	(void)dirty_x;
	(void)dirty_y;
	(void)dirty_w;
	(void)dirty_h;
	(void)region_w;
	(void)region_h;
	(void)cx;
	(void)cy;
	(void)cw;
	(void)ch;
	(void)composite_mode;
	(void)opaque_present;
	(void)mask_is_plane;
	return false;
}

extern "C" void
comp_hub_d3d11_stage_no_composite(struct comp_hub_d3d11 *split)
{
	(void)split;
}

extern "C" bool
comp_hub_d3d11_bind_mask_plane(
    struct comp_hub_d3d11 *split, void *nt_handle, uint64_t generation, uint32_t w, uint32_t h)
{
	(void)split;
	(void)nt_handle;
	(void)generation;
	(void)w;
	(void)h;
	return true;
}

extern "C" void
comp_hub_d3d11_stage_mask_plane(struct comp_hub_d3d11 *split, uint64_t content_seq, uint32_t w, uint32_t h)
{
	(void)split;
	(void)content_seq;
	(void)w;
	(void)h;
}

extern "C" bool
comp_hub_d3d11_zone_dp_supported(struct comp_hub_d3d11 *split)
{
	(void)split;
	return false;
}

extern "C" bool
comp_hub_d3d11_get_zone_grid(struct comp_hub_d3d11 *split, uint32_t *out_grid_w, uint32_t *out_grid_h)
{
	(void)split;
	(void)out_grid_w;
	(void)out_grid_h;
	return false;
}

extern "C" void
comp_hub_d3d11_publish_zone_wish(struct comp_hub_d3d11 *split, uint64_t seq)
{
	(void)split;
	(void)seq;
}

extern "C" void
comp_hub_d3d11_clear_zone_wish(struct comp_hub_d3d11 *split)
{
	(void)split;
}

extern "C" void
comp_hub_d3d11_set_hud(struct comp_hub_d3d11 *split, const void *pixels, uint32_t w, uint32_t h, bool dirty)
{
	(void)split;
	(void)pixels;
	(void)w;
	(void)h;
	(void)dirty;
}

extern "C" bool
comp_hub_d3d11_resize_target(struct comp_hub_d3d11 *split, uint32_t width, uint32_t height)
{
	(void)split;
	(void)width;
	(void)height;
	return false;
}

extern "C" bool
comp_hub_d3d11_weave_and_present(struct comp_hub_d3d11 *split, bool is_repaint, const struct xrt_rect *canvas)
{
	(void)split;
	(void)is_repaint;
	(void)canvas;
	return false;
}

extern "C" bool
comp_hub_d3d11_has_weave_slot(struct comp_hub_d3d11 *split)
{
	(void)split;
	return false;
}

extern "C" bool
comp_hub_d3d11_is_client_presents(struct comp_hub_d3d11 *split)
{
	(void)split;
	return false;
}

extern "C" void
comp_hub_d3d11_set_client_target(
    struct comp_hub_d3d11 *split, bool valid, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	(void)split;
	(void)valid;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

extern "C" void
comp_hub_d3d11_get_client_weave_state(struct comp_hub_d3d11 *split, bool *out_segmented, bool *out_any_flat)
{
	(void)split;
	*out_segmented = false;
	*out_any_flat = false;
}

extern "C" void
comp_hub_d3d11_render_diag(struct comp_hub_d3d11 *split)
{
	(void)split;
}

extern "C" bool
comp_hub_d3d11_get_predicted_eye_positions(struct comp_hub_d3d11 *split, struct xrt_eye_positions *out_eye_pos)
{
	(void)split;
	(void)out_eye_pos;
	return false;
}

extern "C" bool
comp_hub_d3d11_get_display_dimensions(struct comp_hub_d3d11 *split, float *out_width_m, float *out_height_m)
{
	(void)split;
	(void)out_width_m;
	(void)out_height_m;
	return false;
}

extern "C" bool
comp_hub_d3d11_get_display_pixel_info(
    struct comp_hub_d3d11 *split, uint32_t *out_width_px, uint32_t *out_height_px, int32_t *out_left, int32_t *out_top)
{
	(void)split;
	(void)out_width_px;
	(void)out_height_px;
	(void)out_left;
	(void)out_top;
	return false;
}

extern "C" bool
comp_hub_d3d11_request_display_mode(struct comp_hub_d3d11 *split, bool enable_3d)
{
	(void)split;
	(void)enable_3d;
	return false;
}

extern "C" void
comp_hub_d3d11_set_eye_tracking_mode(struct comp_hub_d3d11 *split, uint32_t mode)
{
	(void)split;
	(void)mode;
}

extern "C" bool
comp_hub_d3d11_accepts_atlas_encoding(struct comp_hub_d3d11 *split, enum xrt_atlas_encoding encoding)
{
	(void)split;
	(void)encoding;
	return false;
}

extern "C" void
comp_hub_d3d11_set_atlas_encoding(struct comp_hub_d3d11 *split, enum xrt_atlas_encoding encoding)
{
	(void)split;
	(void)encoding;
}

extern "C" bool
comp_hub_d3d11_get_background_preview(struct comp_hub_d3d11 *split, struct xrt_dp_background_preview *out_preview)
{
	(void)split;
	(void)out_preview;
	return false;
}

extern "C" void
comp_hub_d3d11_set_screens(struct comp_hub_d3d11 *split,
                           const struct xrt_screen_list *list,
                           const struct xrt_system_compositor_info *info,
                           uint64_t pinned_display_id)
{
	(void)split;
	(void)list;
	(void)info;
	(void)pinned_display_id;
}

extern "C" void
comp_hub_d3d11_stage_segment_content(struct comp_hub_d3d11 *split, const struct comp_segments_content *content)
{
	(void)split;
	(void)content;
}

extern "C" bool
comp_hub_d3d11_get_segment_metrics(struct comp_hub_d3d11 *split, struct xrt_segment_metrics *out)
{
	(void)split;
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	return false;
}

extern "C" bool
comp_hub_d3d11_is_direct(struct comp_hub_d3d11 *split)
{
	(void)split;
	return false;
}

extern "C" bool
comp_hub_d3d11_has_eye_positions_at(struct comp_hub_d3d11 *split)
{
	(void)split;
	return false;
}

extern "C" bool
comp_hub_d3d11_get_eye_positions_at(struct comp_hub_d3d11 *split,
                                    int64_t target_ns,
                                    struct xrt_eye_positions *out_eye_pos)
{
	(void)split;
	(void)target_ns;
	(void)out_eye_pos;
	return false;
}

extern "C" void
comp_hub_d3d11_pre_render(struct comp_hub_d3d11 *split)
{
	(void)split;
}

extern "C" void
comp_hub_d3d11_set_display_period(struct comp_hub_d3d11 *split, uint64_t period_ns)
{
	(void)split;
	(void)period_ns;
}

extern "C" void
comp_hub_d3d11_set_predicted_display_time(struct comp_hub_d3d11 *split, uint64_t predicted_display_time_ns)
{
	(void)split;
	(void)predicted_display_time_ns;
}

extern "C" uint64_t
comp_hub_d3d11_get_predicted_lookahead_ns(struct comp_hub_d3d11 *split)
{
	(void)split;
	return 0;
}

extern "C" void
comp_hub_d3d11_mark_wait_frame(struct comp_hub_d3d11 *split)
{
	(void)split;
}

extern "C" uint64_t
comp_hub_d3d11_get_hub_luid(struct comp_hub_d3d11 *split)
{
	(void)split;
	return 0;
}

extern "C" void *
comp_hub_d3d11_get_consumer_fence_handle(struct comp_hub_d3d11 *split)
{
	(void)split;
	return NULL;
}

extern "C" uint64_t
comp_hub_d3d11_get_plane_release_value(struct comp_hub_d3d11 *split)
{
	(void)split;
	return 0;
}

extern "C" bool
comp_hub_d3d11_take_slot_release(struct comp_hub_d3d11 *split, uint32_t *out_slot, uint64_t *out_value)
{
	(void)split;
	(void)out_slot;
	(void)out_value;
	return false;
}

extern "C" const char *
comp_hub_d3d11_ingress_reason(struct comp_hub_d3d11 *split)
{
	(void)split;
	return NULL;
}

extern "C" void
comp_hub_d3d11_leave_direct(struct comp_hub_d3d11 *split, const char *reason)
{
	(void)split;
	(void)reason;
}

extern "C" void
comp_hub_d3d11_log_placement(struct comp_hub_d3d11 *split, const char *producer)
{
	(void)split;
	(void)producer;
}

extern "C" void
comp_hub_d3d11_set_route_reason(struct comp_hub_d3d11 *split, const char *reason)
{
	(void)split;
	(void)reason;
}

extern "C" void
comp_hub_d3d11_set_post_weave_hook(struct comp_hub_d3d11 *split, comp_hub_d3d11_post_weave_fn fn, void *userdata)
{
	(void)split;
	(void)fn;
	(void)userdata;
}

extern "C" void
comp_hub_d3d11_capture_arm(struct comp_hub_d3d11 *split, bool projection_only, const char *path, const char *woven_path)
{
	(void)split;
	(void)projection_only;
	(void)path;
	(void)woven_path;
}

extern "C" bool
comp_hub_d3d11_capture_take(struct comp_hub_d3d11 *split, bool *out_ok)
{
	(void)split;
	if (out_ok != NULL) {
		*out_ok = false;
	}
	return false;
}

extern "C" void *
comp_hub_d3d11_repaint_pace_hold(struct comp_hub_d3d11 *split)
{
	(void)split;
	return NULL;
}

extern "C" void
comp_hub_d3d11_repaint_pace_wait(void *hold)
{
	(void)hold;
}

extern "C" enum comp_hub_d3d11_repaint_result
comp_hub_d3d11_repaint(struct comp_hub_d3d11 *split, bool partition_engaged, const struct xrt_rect *canvas)
{
	(void)split;
	(void)partition_engaged;
	(void)canvas;
	return COMP_HUB_D3D11_REPAINT_NO_FRAME;
}

#endif /* XRT_OS_WINDOWS && XRT_HAVE_D3D11 */
