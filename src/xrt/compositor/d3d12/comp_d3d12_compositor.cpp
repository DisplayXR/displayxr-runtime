// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native D3D12 compositor implementation.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#include "comp_d3d12_compositor.h"
#include "comp_d3d12_swapchain.h"
#include "comp_d3d12_target.h"
#include "util/comp_display_refresh_win.h"
#include "comp_d3d12_renderer.h"

#include "d3d11/comp_d3d11_window.h"

#include "util/comp_layer_accum.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_time.h"

#include "xrt/xrt_system.h"
#include "xrt/xrt_display_processor_d3d12.h"

#include "math/m_api.h"
#include "util/u_tiling.h"
#include "util/u_canvas.h"
#include "util/u_capture_intent.h"
#include "util/u_capture_dims.h"
#include "util/u_image_capture.h"
#include "util/u_hud.h"
#include <displayxr_mcp/mcp_capture.h>

// STB_IMAGE_WRITE_STATIC scopes all stbi_write_* to this TU.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <cmath>

/*!
 * Minimal settings struct for D3D12 compositor.
 */
struct comp_settings
{
	struct
	{
		uint32_t width;
		uint32_t height;
	} preferred;

	int64_t nominal_frame_interval_ns;
};

/*!
 * The D3D12 native compositor structure.
 */
// Decoupled presentation (#833): same env the target reads. On a transparent
// session the DP's alpha-gate flattens against the captured desktop (plugin
// #116), so the post-weave Local2D composite must flatten too instead of
// emitting DWM-dependent alpha.
DEBUG_GET_ONCE_BOOL_OPTION(present_opaque_comp, "DXR_PRESENT_OPAQUE", false)

struct comp_d3d12_compositor
{
	//! Base type - must be first!
	struct xrt_compositor_native base;

	//! The device we are rendering for.
	struct xrt_device *xdev;

	//! D3D12 device (from app's graphics binding, we add a reference).
	ID3D12Device *device;

	//! D3D12 command queue (from app's graphics binding, we add a reference).
	ID3D12CommandQueue *command_queue;

	//! Compositor's own command allocator.
	ID3D12CommandAllocator *cmd_allocator;

	//! Compositor's command list.
	ID3D12GraphicsCommandList *cmd_list;

	//! Fence for GPU synchronization.
	ID3D12Fence *fence;

	//! Current fence value.
	UINT64 fence_value;

	//! Fence event handle.
	HANDLE fence_event;

	//! Output target (DXGI swapchain).
	struct comp_d3d12_target *target;

	//! Renderer for layer compositing.
	struct comp_d3d12_renderer *renderer;

	//! Accumulated layers for the current frame.
	struct comp_layer_accum layer_accum;

	//! Compositor settings.
	struct comp_settings settings;

	//! Window handle (either from app or self-created).
	//! NULL in shared texture mode — compositor doesn't own a swapchain.
	HWND hwnd;

	//! App HWND for position tracking in shared texture mode.
	//! The display processor uses this for weaver alignment.
	HWND app_hwnd;

	//! Self-created window (NULL if app provided window).
	struct comp_d3d11_window *own_window;

	//! True if we created the window ourselves.
	bool owns_window;

	//! Shared texture resource (opened from app-provided handle).
	ID3D12Resource *shared_texture;

	//! RTV descriptor heap for shared texture (1 descriptor).
	ID3D12DescriptorHeap *shared_texture_rtv_heap;

	//! True if shared texture mode is active (offscreen rendering).
	bool has_shared_texture;

	//! Transparent session (#573): the DP composes-under-bg. With
	//! DXR_PRESENT_OPAQUE (#833) this also flips the Local2D composite into
	//! its flatten mode (plugin #116 flattens the gate).
	bool transparent_background;

	//! #727 dual-tap diagnostics (DXR_WEAVE_TAP): a post-composite dump is
	//! pending for this frame; index names the output file.
	bool tap_postcomposite_pending;
	long tap_postcomposite_idx;

	//! D3D12 display processor.
	struct xrt_display_processor_d3d12 *display_processor;

	//! SRV descriptor heap for display processor.
	ID3D12DescriptorHeap *dp_srv_heap;

	//! System devices (for qwerty driver keyboard input).
	struct xrt_system_devices *xsysd;

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

	//! Per-frame effective CONTENT layout (#542): the atlas grid actually
	//! painted and handed to the DP this frame — submission-derived,
	//! decoupled from hardware_display_3d. views == 0 until the first
	//! layer commit computes it.
	struct comp_d3d12_eff_layout eff_layout;

	//! Last known 3D rendering mode index (for V-key toggle restore).
	uint32_t last_3d_mode_index;

	//! True when a legacy app is using a compromise view scale.
	bool legacy_app_tile_scaling;

	//! Compromise view scale for legacy apps. Only valid when legacy_app_tile_scaling is true.
	float legacy_view_scale_x;
	float legacy_view_scale_y;

	//! Lazily allocated intermediate resource for cropping atlas to content dims.
	ID3D12Resource *dp_input_resource;

	//! Cached dimensions for lazy reallocation.
	uint32_t dp_input_width, dp_input_height;

	//! Active authored zone mask (#439, XR_DXR_local_3d_zone). Set by
	//! comp_d3d12_compositor_zone_mask_submit (sticky, last-submit-wins),
	//! cleared when that mask is destroyed. NOT owned — the oxr handle owns
	//! the mask; lifetime is guaranteed by the destroy hook clearing this.
	struct comp_d3d12_zone_mask *active_zone_mask;

	//! Scratch copy of the weave target for the masked composite (#439): the
	//! window region of the weave target (RTV-only → the lerp samples this
	//! snapshot). Lazily (re)allocated window-sized (#464); steady state
	//! COMMON. Removed in Phase 3 when the weave lands in an SRV-capable RT
	//! directly.
	ID3D12Resource *weave_scratch;

	//! #439 Phase 3 — Local2D consumer state (mirrors the D3D11 leg). True if
	//! this frame's accumulator carried any XRT_LAYER_LOCAL_2D layer; set once
	//! under c->mutex at the top of layer_commit. Drives the effective-canvas
	//! supersede + the composite's have_local_2d branch.
	bool local_2d_last_frame;

	//! Implicit zone mask rasterized from the frame's Local2D layer rects
	//! (R8_UNORM RT + staged SRV copy), reused across frames via dirty-check.
	//! INVERSE of an authored set_rects mask: M=1 (keep weave) everywhere, M=0
	//! (show the flattened 2D) inside the layer rects.
	//! XR_DXR_display_zones (ADR-027): in zones frames the SAME resources hold
	//! the AUTO wish (BINARY union of the zone rects — #800/#801: the wish is
	//! hardware-only, hard-edged by default — re-rastered every zones frame;
	//! the implicit rule is inert there); the raster invalidates
	//! implicit_rect_count so a later legacy frame re-rasters.
	ID3D12Resource *implicit_mask_tex;
	ID3D12DescriptorHeap *implicit_mask_rtv_heap;
	ID3D12Resource *implicit_mask_staged;
	uint32_t implicit_mask_w, implicit_mask_h;
	uint32_t implicit_rect_count;
	struct xrt_rect implicit_rects[XRT_MAX_LAYERS];

	//! Zones COMPOSITE mask with per-zone opt-in feather
	//! (XrDisplayZoneFeatherDXR, #800/#803). Allocated only when a frame's
	//! zones request feather — the published wish must stay binary (the
	//! implicit_mask raster above), so a feathered composite needs its own
	//! resources. All-hard frames sample the binary raster for the
	//! composite. Re-rastered every feathered zones frame (VK-style).
	ID3D12Resource *feather_mask_tex;
	ID3D12DescriptorHeap *feather_mask_rtv_heap;
	ID3D12Resource *feather_mask_staged;
	uint32_t feather_mask_w, feather_mask_h;

	//! XR_DXR_display_zones (ADR-027): true when the current frame's
	//! accumulator carries XRT_LAYER_ZONE_3D layers (a "zones frame"). In a
	//! zones frame the canvas output rect, the sticky submitted mask, and
	//! the implicit-mask-from-Local2D rule are all inert; the effective
	//! canvas is the full client window; the wish (explicit frame wish or
	//! the auto union-of-zone-rects raster) drives the DP publish ONLY —
	//! the post-weave composite gates the weave by the BINARY zone raster
	//! (or the #803 opt-in feather raster), never by an explicit wish
	//! (#801: the wish is hardware-only). Set once under c->mutex at the
	//! top of layer_commit, beside local_2d_last_frame.
	bool zones_frame;

	//! Explicit per-frame wish (XrDisplayZonesFrameEndInfoDXR.wishMask),
	//! handed in via comp_d3d12_compositor_zones_set_frame_wish before
	//! layer_commit and consumed by that commit. NULL = auto-derive. Not
	//! owned — the mask handle owns the resources; handle destroy clears
	//! any dangling reference via zone_mask_destroy.
	struct comp_d3d12_zone_mask *frame_wish;

	//! Tier-1 fallback edge state: request_display_mode(true) fired once
	//! on the zones rising edge ("any zone active => request 3D"); never
	//! forces 2D on the falling edge (mode restore stays with the V-toggle
	//! logic). P4: only taken for legacy DPs (caps.supported == 0) — a
	//! zone-capable DP gets the per-frame wish publish instead.
	bool zones_mode_requested;

	//! #224 / ADR-027 hardware-DP zone leg (P4): cached get_local_zone_caps
	//! result. 0 = not queried yet, 1 = supported, 2 = legacy DP.
	int zone_dp_state;
	//! DP zone caps when zone_dp_state == 1.
	struct xrt_dp_local_zone_caps zone_dp_caps;
	//! Published-content generation: bumped on zone_mask_submit, on an
	//! auto-wish re-raster whose rect set / dims actually changed, and on
	//! an explicit-frame-wish source change — NOT per frame.
	uint64_t zone_publish_seq;
	//! True while this client's mask is published to the DP — drives the
	//! clear-on-deactivate edge.
	bool zone_published;
	//! This frame's resolved wish resource + dims (steady
	//! PIXEL_SHADER_RESOURCE), set by d3d12_update_zone_wish_state and
	//! reset at the top of layer_commit. The publish runs AFTER the frame's
	//! ExecuteCommandLists + fence wait, so the content is GPU-complete —
	//! exactly the publish contract.
	ID3D12Resource *zone_publish_res; //!< Borrowed (frame-wish staged / implicit_mask_staged) — not owned.
	uint32_t zone_publish_w, zone_publish_h;
	//! Seq-bump caches: last explicit wish pointer actually published, and
	//! the auto raster's rect set (mirrors d3d11's wish_rects; dims tracked
	//! via zone_publish_w/h persisting across frames).
	struct comp_d3d12_zone_mask *zone_frame_wish_last;
	struct xrt_rect zone_wish_rects[XRT_MAX_LAYERS];
	uint32_t zone_wish_rect_count;

	//! Flattened Local2D layers (the `twod` source). R8G8B8A8_UNORM render
	//! target — dedicated. Lazily (re)allocated window-sized.
	ID3D12Resource *local2d_scratch;
	ID3D12DescriptorHeap *local2d_scratch_rtv_heap;
	uint32_t local2d_scratch_w, local2d_scratch_h;

	//! #491 part 3 — 2D-under backdrop flatten target (same trio as
	//! local2d_scratch). UNDER Local2D layers (before the projection in list
	//! order) flatten here PRE-weave, left in PIXEL_SHADER_RESOURCE; the
	//! ID3D12Resource* is handed to the DP via set_background_2d (the DP creates
	//! its own shader-visible SRV). Compositor-owned so it outlives process_atlas.
	ID3D12Resource *backdrop_scratch;
	ID3D12DescriptorHeap *backdrop_scratch_rtv_heap;
	uint32_t backdrop_scratch_w, backdrop_scratch_h;

	//! HUD overlay.
	struct u_hud *hud;

	//! HUD texture (DEFAULT heap, for GPU copy source).
	ID3D12Resource *hud_texture;

	//! HUD upload buffer (UPLOAD heap, for CPU staging).
	ID3D12Resource *hud_upload_buffer;

	//! HUD upload buffer row pitch (aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
	uint32_t hud_upload_pitch;

	//! Whether HUD texture has been created.
	bool hud_initialized;

	//! Frame timing for HUD FPS display.
	uint64_t last_frame_time_ns;
	float smoothed_frame_time_ms;

	//! Thread safety.
	std::mutex mutex;

	/*!
	 * #868 — weave-rate decoupling.
	 *
	 * Weave is f(atlas, eye_position) and the display processor re-pulls the
	 * viewer's eyes at weave time, so re-weaving an UNCHANGED atlas against
	 * fresh eyes is not a no-op: it re-derives the parallax for where the
	 * viewer is now. An app rendering below the panel rate would otherwise
	 * hold one interlace pattern across several refreshes, and head motion in
	 * that window shows up as a stale view.
	 *
	 * The repaint thread replays the weave+present of the last frame at panel
	 * rate whenever the app has gone quiet. Two invariants make it safe:
	 *
	 *  - It runs ONLY when the last frame was not zero-copy. On the zero-copy
	 *    path the "atlas" is the app's own swapchain image, which the app
	 *    reacquires and overwrites — re-weaving it would race the app and
	 *    sample a half-drawn frame. Off zero-copy the DP reads
	 *    @ref dp_input_resource, a compositor-owned crop that stays valid until
	 *    the next layer_commit. This is a per-frame branch on the existing
	 *    u_tiling_can_zero_copy() gate, not a policy flag.
	 *  - It holds @ref mutex across the whole replay, so the display processor
	 *    still only ever sees one caller. process_atlas has only ever been
	 *    called from the app's thread; the plug-in contract does not promise
	 *    thread-safety, and adding a loop on our side must not silently make
	 *    thread-safety a vendor requirement.
	 */
	struct
	{
		//! Kill switch: DXR_WEAVE_REPAINT=0. -1 = unprobed.
		int enabled;

		//! DXR_WEAVE_REPAINT_FORCE=1 — bypass the quiet-gate so the repaint
		//! path runs on hardware where no app is slow enough to trip it.
		//! A correctness probe; it costs frame rate by design.
		int force;

		//! Last frame was DP-woven and not zero-copy ⟹ safe to replay.
		bool armed;

		/*!
		 * True from layer_begin until the following layer_commit — i.e. while
		 * the app is part-way through submitting a frame.
		 *
		 * Holding @ref mutex across the replay is NOT sufficient on its own.
		 * layer_begin resets layer_accum and releases the lock; each
		 * layer_projection / layer_local_2d call then takes and releases it
		 * again before layer_commit. So a repaint can win the lock mid-
		 * submission and see layer_accum empty or half-filled — and the replay
		 * reads it live (d3d12_composite_zone_mask derives the zone/Local2D
		 * mask from it). The observed failure was the 2D bubble dropping out
		 * and the woven desktop compose-under showing through in its place,
		 * flickering at the repaint rate.
		 *
		 * Outside this window layer_accum holds exactly the last COMPLETED
		 * frame's layers, which is precisely what a repaint wants to replay.
		 * The window is short in practice — an app renders between
		 * xrWaitFrame and xrEndFrame, not between layer_begin and
		 * layer_commit — so gating on it costs the repaint almost nothing.
		 */
		bool app_frame_in_progress;

		//! Everything process_atlas needs, captured at the last real weave.
		//! Stable by construction: the resources are compositor-owned and the
		//! geometry only changes when a new frame arrives (which re-arms).
		uint32_t tgt_w, tgt_h;
		uint32_t view_w, view_h;
		uint32_t cols, rows;
		struct u_canvas_rect canvas;
		struct xrt_eye_positions eye_pos;

		//! The renderer atlas the last real frame wove from. The repaint
		//! re-crops and re-flattens from this rather than caching the
		//! downstream resources: the 2D-under backdrop is rebuilt from
		//! layer_accum every weave, so caching it hands the display processor
		//! a backdrop from an older frame and the desktop compose-under shows
		//! where the 2D should be. Replaying is cheap next to the weave.
		ID3D12Resource *atlas;
		uint32_t content_w, content_h;

		//! The 2D-under backdrop the last app frame flattened. Reused for the
		//! same reason as mask_res — the flatten reads app-owned textures.
		ID3D12Resource *backdrop;
		uint32_t backdrop_w, backdrop_h;

		//! The zone / Local2D mask the last app frame RESOLVED. A repaint
		//! composites from this instead of re-deriving it, because deriving it
		//! ticks per-frame state machines (wish publish, implicit raster) that
		//! must run exactly once per app frame. See d3d12_composite_zone_mask.
		ID3D12Resource *mask_res;

		//! os_monotonic_get_ns() of the last weave driven by a REAL app frame.
		//! Gates the repaint so it only fires once the app has actually gone
		//! quiet. Deliberately not touched by repaints — see the thread.
		uint64_t last_app_frame_ns;

		//! Diagnostics: where the loop goes, counted so a repaint that never
		//! fires can be told apart from one that fires and does nothing.
		uint64_t count;      // repaints actually issued
		uint64_t ticks;      // loop wakeups
		uint64_t bail_armed; // not armed (no non-zero-copy DP frame yet)
		uint64_t bail_gate;  // app still making rate
		uint64_t bail_race;  // app frame landed while we paced / took the lock
	} repaint;

	std::thread repaint_thread;
	std::atomic<bool> repaint_quit;

	//! MCP capture_frame request box (serviced at end of layer_commit).
	//! Mirrors the pattern in comp_metal/gl/d3d11_compositor. See issue #210.
	struct mcp_capture_request mcp_capture;

	//! Per-frame capture intent. See u_capture_intent.h.
	struct u_capture_intent capture_intent;
};

/*
 *
 * Helper functions
 *
 */

static inline struct comp_d3d12_compositor *
d3d12_comp(struct xrt_compositor *xc)
{
	return reinterpret_cast<struct comp_d3d12_compositor *>(xc);
}

// #439 authored zone-mask helpers (XR_DXR_local_3d_zone). Defined near the
// bottom of the file alongside the comp_d3d12_compositor_zone_mask_* entry
// points, called from the layer-commit paths + destroy above them.
static bool d3d12_composite_zone_mask(struct comp_d3d12_compositor *c,
                                      bool reuse_mask,
                                      bool prepare_only,
                                       ID3D12Resource *dst,
                                       uint64_t dst_rtv,
                                       D3D12_RESOURCE_STATES dst_pre_state,
                                       D3D12_RESOURCE_STATES dst_post_state,
                                       uint32_t dst_w, uint32_t dst_h,
                                       const struct u_canvas_rect *eff_canvas);
// #491 part 3 — pre-weave 2D-under backdrop flatten (defined with the Local2D
// helpers near the bottom; called from the process_atlas sites above).
static ID3D12Resource *d3d12_flatten_backdrop_2d(struct comp_d3d12_compositor *c, uint32_t dst_w, uint32_t dst_h,
                                                 uint32_t *out_w, uint32_t *out_h);
static void d3d12_release_zone_state(struct comp_d3d12_compositor *c);
// #224 / ADR-027 hardware-DP zone leg (P4): one-time caps probe + per-frame
// sideband publish of the wish / sticky mask. Defined with the zone helpers
// near the bottom; called after each path's fence wait in layer_commit.
static bool d3d12_zone_dp_supported(struct comp_d3d12_compositor *c);
static void d3d12_sync_zone_mask_to_dp(struct comp_d3d12_compositor *c);
// #740 diagnostic (DXR_PHASE_DEBUG=1): dump the geometry that seeds the weave
// interlace phase, to localize the position/size-dependent phase offset.
static void d3d12_phase_debug_dump(struct comp_d3d12_compositor *c, const char *where);

// #439 Phase 2: an active zone mask supersedes the canvas output rect —
// the weave region, view dims, Kooima metrics, and composite region all
// become the client-window rect (top-left anchored per #464). With no mask
// this returns an invalid rect, so readers fall back to full-window/target
// dims, leaving the no-mask path unchanged.
// Returning a *valid* window rect (not just "invalid") matters on the
// shared-texture path: the texture is display-sized worst-case, so an
// invalid canvas there would fall back to display dims — the window rect
// keeps the #464 clamp. Callers in the frame path hold c->mutex, which
// zone_mask_submit/destroy also take, so the mask cannot flip mid-frame.
static struct u_canvas_rect
d3d12_effective_canvas(struct comp_d3d12_compositor *c)
{
	// #439 Phase 3: Local2D layers supersede the canvas the same way an
	// authored mask does — the composite writes the whole window region.
	// XR_DXR_display_zones: a zones frame spans the full client window by
	// definition (each zone rect is its own canvas; the output rect is
	// inert) — same supersede geometry as the mask/Local2D rules.
	if (!c->zones_frame && c->active_zone_mask == nullptr && !c->local_2d_last_frame) {
		return {};
	}
	struct u_canvas_rect win = {};
	HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	RECT r;
	if (wnd != nullptr && GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
		win.valid = true;
		win.x = 0;
		win.y = 0;
		win.w = (uint32_t)r.right;
		win.h = (uint32_t)r.bottom;
		return win;
	}
	return win; // invalid → existing full-target fallbacks
}

// #740 diagnostic. For each active zone rect, log the panel-pixel CORNER seed
// (window client origin + zone offset — size-INVARIANT at a fixed placement)
// vs the CENTER seed (corner + zone extent/2 — drifts with zone SIZE). If the
// vendor weaver phases the interlace from the CENTER, the center-seed's
// (mod lens-pitch) residual is the observed global phase error; the corner
// seed is what a size-independent phase would use. One-shot per geometry
// change so there is no per-frame spam. Gated by DXR_PHASE_DEBUG=1. Remove
// once #740 is resolved.
static void
d3d12_phase_debug_dump(struct comp_d3d12_compositor *c, const char *where)
{
	static bool phase_dbg = getenv("DXR_PHASE_DEBUG") != nullptr;
	if (!phase_dbg || c->display_processor == nullptr) {
		return;
	}
	HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	POINT origin = {0, 0};
	if (wnd == nullptr || !ClientToScreen(wnd, &origin)) {
		return;
	}
	uint32_t dpx_w = 0, dpx_h = 0;
	int32_t disp_left = 0, disp_top = 0;
	xrt_display_processor_d3d12_get_display_pixel_info(c->display_processor, &dpx_w, &dpx_h,
	                                                   &disp_left, &disp_top);
	// Window client-area corner in panel pixels (absolute, size-invariant).
	const int32_t win_cx = (int32_t)origin.x - disp_left;
	const int32_t win_cy = (int32_t)origin.y - disp_top;
	if (c->zone_wish_rect_count == 0) {
		// No zone rects staged this frame (e.g. glued-window case where the
		// whole client rect is the canvas): log the window rect itself.
		RECT r;
		if (GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
			const int32_t zcx = win_cx + (int32_t)r.right / 2;
			const int32_t zcy = win_cy + (int32_t)r.bottom / 2;
			static int64_t s_last_win = -1;
			const int64_t sig = ((int64_t)win_cx << 40) ^ ((int64_t)win_cy << 20) ^
			                    ((int64_t)r.right << 10) ^ (int64_t)r.bottom;
			if (s_last_win != sig) {
				s_last_win = sig;
				U_LOG_W("#740 PHASE(%s) window: corner_panelpx=(%d,%d) client=%ldx%ld => "
				        "CENTER_seed_panelpx=(%d,%d) [disp %ux%u @ (%d,%d)]",
				        where, win_cx, win_cy, r.right, r.bottom, zcx, zcy,
				        dpx_w, dpx_h, disp_left, disp_top);
			}
		}
		return;
	}
	for (uint32_t i = 0; i < c->zone_wish_rect_count && i < XRT_MAX_LAYERS; i++) {
		const struct xrt_rect zr = c->zone_wish_rects[i];
		const int32_t zcorner_x = win_cx + zr.offset.w;
		const int32_t zcorner_y = win_cy + zr.offset.h;
		const int32_t zcenter_x = zcorner_x + zr.extent.w / 2;
		const int32_t zcenter_y = zcorner_y + zr.extent.h / 2;
		// Signature: log only on a genuine geometry change (no per-frame spam).
		const int64_t sig = ((int64_t)zcorner_x << 40) ^ ((int64_t)zcorner_y << 20) ^
		                    ((int64_t)zr.extent.w << 10) ^ (int64_t)zr.extent.h;
		static int64_t s_last_sig[XRT_MAX_LAYERS] = {};
		if (s_last_sig[i] == sig) {
			continue;
		}
		s_last_sig[i] = sig;
		U_LOG_W("#740 PHASE(%s) zone[%u]: win_corner_panelpx=(%d,%d) "
		        "zone_rect_clientpx=(off %d,%d ext %dx%d) => "
		        "CORNER_seed_panelpx=(%d,%d) CENTER_seed_panelpx=(%d,%d) "
		        "[disp %ux%u @ (%d,%d)]",
		        where, i, win_cx, win_cy,
		        zr.offset.w, zr.offset.h, zr.extent.w, zr.extent.h,
		        zcorner_x, zcorner_y, zcenter_x, zcenter_y,
		        dpx_w, dpx_h, disp_left, disp_top);
	}
}

/*!
 * Wait for GPU to finish all submitted work.
 */
static void
gpu_wait_idle(struct comp_d3d12_compositor *c)
{
	c->fence_value++;
	c->command_queue->Signal(c->fence, c->fence_value);

	if (c->fence->GetCompletedValue() < c->fence_value) {
		c->fence->SetEventOnCompletion(c->fence_value, c->fence_event);
		WaitForSingleObject(c->fence_event, INFINITE);
	}

	// #747: report a device reset the moment WE can see it, and dump DRED.
	//
	// This is the earliest point the runtime observes an adapter reset: on
	// removal the fence jumps to UINT64_MAX, so the compare above passes, the
	// wait is skipped, and we return as if nothing happened — silently. The
	// host then hits DXGI_ERROR_DEVICE_REMOVED in ITS present and aborts.
	//
	// That ordering is why DRED has produced nothing in the field: the
	// breadcrumbs live in the faulted process and die with it, and an
	// aborting host (Unity aborts inside its own Present) never reads them.
	// If the runtime does not read them here, nobody does.
	//
	// Once-only: after a reset every subsequent call fails, and a per-frame
	// dump would bury the one interesting readout.
	{
		static bool s_reported = false;
		if (!s_reported && c->device != nullptr) {
			HRESULT rr = c->device->GetDeviceRemovedReason();
			if (rr != S_OK) {
				s_reported = true;
				U_LOG_E("#747 DEVICE REMOVED observed by the compositor at gpu_wait_idle: "
				        "GetDeviceRemovedReason=0x%08x",
				        (unsigned)rr);
				comp_d3d12_log_dred_state(c->device, "gpu_wait_idle/device-removed");
			}
		}
	}
}

/*
 *
 * xrt_compositor member functions
 *
 */

static xrt_result_t
d3d12_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                                  const struct xrt_swapchain_create_info *info,
                                                  struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 3;
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_create_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_swapchain **out_xsc)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	return comp_d3d12_swapchain_create(c, info, out_xsc);
}

static xrt_result_t
d3d12_compositor_import_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_image_native *native_images,
                                   uint32_t image_count,
                                   struct xrt_swapchain **out_xsc)
{
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
d3d12_compositor_import_fence(struct xrt_compositor *xc,
                               xrt_graphics_sync_handle_t handle,
                               struct xrt_compositor_fence **out_xcf)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
d3d12_compositor_create_semaphore(struct xrt_compositor *xc,
                                   xrt_graphics_sync_handle_t *out_handle,
                                   struct xrt_compositor_semaphore **out_xcsem)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
d3d12_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	U_LOG_I("D3D12 compositor session begin");

	// Switch display to 3D mode
	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d12_request_display_mode(c->display_processor, true);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_end_session(struct xrt_compositor *xc)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	U_LOG_I("D3D12 compositor session end");

	// Switch display back to 2D mode
	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d12_request_display_mode(c->display_processor, false);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_predict_frame(struct xrt_compositor *xc,
                                int64_t *out_frame_id,
                                int64_t *out_wake_time_ns,
                                int64_t *out_predicted_gpu_time_ns,
                                int64_t *out_predicted_display_time_ns,
                                int64_t *out_predicted_display_period_ns)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->display_refresh_rate);

	// #867: measured wait_frame->scanout lookahead when available; the
	// period*2 constant only holds for an app keeping up at queue depth 1.
	int64_t lookahead_ns = period_ns * 2;
	if (c->target != nullptr) {
		const uint64_t measured = comp_d3d12_target_get_predicted_lookahead_ns(c->target);
		if (measured != 0) {
			lookahead_ns = (int64_t)measured;
		}
		comp_d3d12_target_mark_wait_frame(c->target);
	}
	*out_predicted_display_time_ns = now_ns + lookahead_ns;
	*out_predicted_display_period_ns = period_ns;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = period_ns;

	// The spec requires predictedDisplayTime to strictly increase across
	// xrWaitFrame calls, and CTS enforces it. The old period*2 constant
	// satisfied that for free — `now` only advances — but a MEASURED
	// lookahead can shrink between calls (the EMA moves), and if it shrinks
	// by more than `now` advanced the prediction would step backwards.
	// Clamp forward; `now` overtakes the floor again within a frame or two.
	if (*out_predicted_display_time_ns <= (int64_t)c->last_display_time_ns) {
		*out_predicted_display_time_ns = (int64_t)c->last_display_time_ns + 1;
	}
	c->last_display_time_ns = static_cast<uint64_t>(*out_predicted_display_time_ns);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_wait_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	// Check if window was closed
	if (c->owns_window && c->own_window != nullptr &&
	    !comp_d3d11_window_is_valid(c->own_window)) {
		U_LOG_I("Window closed - signaling session exit");
		return XRT_ERROR_IPC_FAILURE;
	}

	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->display_refresh_rate);

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
	// #867: measured wait_frame->scanout lookahead when available; the
	// period*2 constant only holds for an app keeping up at queue depth 1.
	int64_t lookahead_ns = period_ns * 2;
	if (c->target != nullptr) {
		const uint64_t measured = comp_d3d12_target_get_predicted_lookahead_ns(c->target);
		if (measured != 0) {
			lookahead_ns = (int64_t)measured;
		}
		comp_d3d12_target_mark_wait_frame(c->target);
	}
	*out_predicted_display_time_ns = now_ns + lookahead_ns;
	*out_predicted_display_period_ns = period_ns;

	// The spec requires predictedDisplayTime to strictly increase across
	// xrWaitFrame calls, and CTS enforces it. The old period*2 constant
	// satisfied that for free — `now` only advances — but a MEASURED
	// lookahead can shrink between calls (the EMA moves), and if it shrinks
	// by more than `now` advanced the prediction would step backwards.
	// Clamp forward; `now` overtakes the floor again within a frame or two.
	if (*out_predicted_display_time_ns <= (int64_t)c->last_display_time_ns) {
		*out_predicted_display_time_ns = (int64_t)c->last_display_time_ns + 1;
	}
	c->last_display_time_ns = static_cast<uint64_t>(*out_predicted_display_time_ns);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_mark_frame(struct xrt_compositor *xc,
                             int64_t frame_id,
                             enum xrt_compositor_frame_point point,
                             int64_t when_ns)
{
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Check for window resize — resize immediately to keep backbuffer in sync.
	// The GPU is already idle here: layer_commit() calls gpu_wait_idle() at
	// the end of every frame, so no additional GPU drain is needed.
	// Immediate resize is critical for 3D displays: the weaver outputs
	// pixel-precise interlacing patterns, and any DXGI stretching (from a
	// backbuffer/window size mismatch) destroys the interlacing.
	if (c->hwnd != nullptr && c->target != nullptr) {
		RECT rect;
		if (GetClientRect(c->hwnd, &rect)) {
			uint32_t new_width = static_cast<uint32_t>(rect.right - rect.left);
			uint32_t new_height = static_cast<uint32_t>(rect.bottom - rect.top);

			if (new_width > 0 && new_height > 0) {
				uint32_t current_width, current_height;
				comp_d3d12_target_get_dimensions(c->target, &current_width, &current_height);

				if (new_width != current_width || new_height != current_height) {
					U_LOG_I("Window resized: %ux%u -> %ux%u",
					        current_width, current_height, new_width, new_height);

					// Resize child window first if fallback is active (no-op otherwise)
					comp_d3d12_target_resize_child_window(c->target, new_width, new_height);

					xrt_result_t xret =
					    comp_d3d12_target_resize(c->target, new_width, new_height);
					if (xret == XRT_SUCCESS) {
						c->settings.preferred.width = new_width;
						c->settings.preferred.height = new_height;
					}
				}
			}
		}
	}

	// Reset layer accumulator
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	// #868: layer_accum is now mid-rewrite and the lock is about to be
	// released — keep the repaint thread out until layer_commit.
	c->repaint.app_frame_in_progress = true;
	comp_layer_accum_begin(&c->layer_accum, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_projection(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                   const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                         struct xrt_device *xdev,
                                         struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                         struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                         const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_quad(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_cube(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_cube(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_cylinder(struct xrt_compositor *xc,
                                 struct xrt_device *xdev,
                                 struct xrt_swapchain *xsc,
                                 const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_cylinder(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_equirect1(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_equirect1(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_equirect2(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_equirect2(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_passthrough(struct xrt_compositor *xc,
                                    struct xrt_device *xdev,
                                    const struct xrt_layer_data *data)
{
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_window_space(struct xrt_compositor *xc,
                                     struct xrt_device *xdev,
                                     struct xrt_swapchain *xsc,
                                     const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_window_space(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * Local-2D layer (XR_DXR_local_3d_zone v3, #439 Phase 3) — accumulate only;
 * the D3D12 consumer is a Windows follow-up leg
 * (docs/roadmap/unified-2d-3d-phase3-impl.md §7).
 */
static xrt_result_t
d3d12_compositor_layer_local_2d(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc,
                                const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_local_2d(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * 3D display zone layer (XR_DXR_display_zones, ADR-027) — multi-swapchain
 * accumulate like projection; consumed by the zones-frame branch of
 * layer_commit (zone rect scaled into the window-spanning atlas tile).
 */
static xrt_result_t
d3d12_compositor_layer_zone_3d(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                               const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_zone_3d(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * Render the HUD overlay onto the back buffer (D3D12 version).
 *
 * The back buffer must be in D3D12_RESOURCE_STATE_COPY_DEST when this is called.
 */
static void
d3d12_render_hud_overlay(struct comp_d3d12_compositor *c,
                         ID3D12GraphicsCommandList *cmd_list,
                         ID3D12Resource *back_buffer,
                         uint32_t win_w, uint32_t win_h,
                         const struct xrt_eye_positions *eye_pos)
{
	if (!c->owns_window || c->hud == NULL || !u_hud_is_visible()) {
		return;
	}

	// Compute FPS from frame timestamps
	uint64_t now_ns = os_monotonic_get_ns();
	if (c->last_frame_time_ns != 0) {
		float dt_ms = (float)(now_ns - c->last_frame_time_ns) / 1e6f;
		// Exponential moving average (alpha=0.1 for smooth display)
		c->smoothed_frame_time_ms = c->smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	c->last_frame_time_ns = now_ns;

	float fps = (c->smoothed_frame_time_ms > 0.0f) ? (1000.0f / c->smoothed_frame_time_ms) : 0.0f;

	// Get render and window dimensions
	uint32_t render_w = 0, render_h = 0;
	if (c->renderer != nullptr) {
		comp_d3d12_renderer_get_view_dimensions(c->renderer, &render_w, &render_h);
	}

	// Get display physical dimensions from display processor
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;
	comp_d3d12_compositor_get_display_dimensions(&c->base.base, &disp_w_m, &disp_h_m);
	float disp_w_mm = disp_w_m * 1000.0f;
	float disp_h_mm = disp_h_m * 1000.0f;

	// Fill HUD data
	struct u_hud_data data = {};
	data.device_name = c->xdev->str;
	data.fps = fps;
	data.frame_time_ms = c->smoothed_frame_time_ms;
	data.mode_3d = c->hardware_display_3d;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			data.rendering_mode_name = c->xdev->rendering_modes[idx].mode_name;
		}
	}
	data.render_width = render_w;
	data.render_height = render_h;
	if (c->xdev != NULL && c->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(c->xdev->rendering_modes, c->xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = win_w;
	data.window_height = win_h;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	// Use the active rendering mode's view_count for eye display (not eye_pos->count,
	// which may report more eyes than the mode uses — e.g. tracker returns L/R in 2D mode).
	uint32_t mode_eye_count = eye_pos->count;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t midx = c->xdev->hmd->active_rendering_mode_index;
		if (midx < c->xdev->rendering_mode_count) {
			mode_eye_count = c->xdev->rendering_modes[midx].view_count;
		}
	}
	if (mode_eye_count > eye_pos->count) {
		mode_eye_count = eye_pos->count;
	}
	data.eye_count = mode_eye_count;
	for (uint32_t e = 0; e < mode_eye_count && e < 8; e++) {
		data.eyes[e].x = eye_pos->eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos->eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos->eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos->is_tracking;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != nullptr) {
		// Virtual display position + forward vector from qwerty device pose.
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(c->xsysd->xdevs, c->xsysd->xdev_count, &qwerty_pose)) {
			data.vdisp_x = qwerty_pose.position.x;
			data.vdisp_y = qwerty_pose.position.y;
			data.vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			data.forward_x = fwd_out.x;
			data.forward_y = fwd_out.y;
			data.forward_z = fwd_out.z;
		}

		struct qwerty_view_state ss;
		if (qwerty_get_view_state(c->xsysd->xdevs, c->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.ipd_factor = ss.ipd_factor;
			data.parallax_factor = ss.parallax_factor;
			data.inv_convergence_distance = ss.inv_convergence_distance;
			data.half_tan_vfov = ss.half_tan_vfov;
			data.m2v = ss.m2v;
			data.virtual_display_height = ss.virtual_display_height;
			data.perspective_factor = ss.perspective_factor;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(c->hud, &data);

	// Lazy-create HUD texture and upload buffer
	if (!c->hud_initialized) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);

		// Aligned row pitch for D3D12 upload buffer
		uint32_t aligned_pitch = (hud_w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
		                         ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		c->hud_upload_pitch = aligned_pitch;

		// Create DEFAULT heap texture (GPU copy source)
		D3D12_RESOURCE_DESC tex_desc = {};
		tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		tex_desc.Width = hud_w;
		tex_desc.Height = hud_h;
		tex_desc.DepthOrArraySize = 1;
		tex_desc.MipLevels = 1;
		tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		tex_desc.SampleDesc.Count = 1;
		tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		D3D12_HEAP_PROPERTIES default_heap = {};
		default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		HRESULT hr = c->device->CreateCommittedResource(
		    &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
		    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		    __uuidof(ID3D12Resource),
		    reinterpret_cast<void **>(&c->hud_texture));
		if (FAILED(hr)) {
			U_LOG_E("Failed to create HUD texture: 0x%08x", hr);
			return;
		}

		// Create UPLOAD heap buffer for CPU staging
		D3D12_RESOURCE_DESC buf_desc = {};
		buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buf_desc.Width = (uint64_t)aligned_pitch * hud_h;
		buf_desc.Height = 1;
		buf_desc.DepthOrArraySize = 1;
		buf_desc.MipLevels = 1;
		buf_desc.Format = DXGI_FORMAT_UNKNOWN;
		buf_desc.SampleDesc.Count = 1;
		buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		D3D12_HEAP_PROPERTIES upload_heap = {};
		upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

		hr = c->device->CreateCommittedResource(
		    &upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
		    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		    __uuidof(ID3D12Resource),
		    reinterpret_cast<void **>(&c->hud_upload_buffer));
		if (FAILED(hr)) {
			U_LOG_E("Failed to create HUD upload buffer: 0x%08x", hr);
			c->hud_texture->Release();
			c->hud_texture = nullptr;
			return;
		}

		c->hud_initialized = true;
		dirty = true; // Force initial upload
	}

	// Upload pixels to upload buffer if changed
	if (dirty && c->hud_texture != nullptr && c->hud_upload_buffer != nullptr) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);
		const uint8_t *pixels = u_hud_get_pixels(c->hud);

		// Map upload buffer and copy row by row with aligned pitch
		void *mapped = nullptr;
		D3D12_RANGE read_range = {0, 0}; // We won't read from this buffer
		HRESULT hr = c->hud_upload_buffer->Map(0, &read_range, &mapped);
		if (SUCCEEDED(hr)) {
			uint8_t *dst = static_cast<uint8_t *>(mapped);
			for (uint32_t row = 0; row < hud_h; row++) {
				memcpy(dst + row * c->hud_upload_pitch,
				       pixels + row * hud_w * 4,
				       hud_w * 4);
			}
			c->hud_upload_buffer->Unmap(0, nullptr);

			// Copy from upload buffer to hud_texture
			D3D12_TEXTURE_COPY_LOCATION src_loc = {};
			src_loc.pResource = c->hud_upload_buffer;
			src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src_loc.PlacedFootprint.Offset = 0;
			src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			src_loc.PlacedFootprint.Footprint.Width = hud_w;
			src_loc.PlacedFootprint.Footprint.Height = hud_h;
			src_loc.PlacedFootprint.Footprint.Depth = 1;
			src_loc.PlacedFootprint.Footprint.RowPitch = c->hud_upload_pitch;

			D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
			dst_loc.pResource = c->hud_texture;
			dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst_loc.SubresourceIndex = 0;

			cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
		}
	}

	// Copy hud_texture to back buffer at bottom-left
	if (c->hud_texture != nullptr && back_buffer != nullptr) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);

		// Transition hud_texture: COPY_DEST → COPY_SOURCE
		D3D12_RESOURCE_BARRIER hud_barrier = {};
		hud_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		hud_barrier.Transition.pResource = c->hud_texture;
		hud_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		hud_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		hud_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cmd_list->ResourceBarrier(1, &hud_barrier);

		// Position at bottom-left with 10px margin
		uint32_t dst_x = 10;
		uint32_t dst_y = (win_h > hud_h + 10) ? (win_h - hud_h - 10) : 0;

		D3D12_TEXTURE_COPY_LOCATION src_loc = {};
		src_loc.pResource = c->hud_texture;
		src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src_loc.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
		dst_loc.pResource = back_buffer;
		dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst_loc.SubresourceIndex = 0;

		D3D12_BOX src_box = {0, 0, 0, hud_w, hud_h, 1};
		cmd_list->CopyTextureRegion(&dst_loc, dst_x, dst_y, 0, &src_loc, &src_box);

		// Transition hud_texture back: COPY_SOURCE → COPY_DEST
		hud_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		hud_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		cmd_list->ResourceBarrier(1, &hud_barrier);
	}
}

/*!
 * Crop atlas to content dimensions before passing to display processor.
 * Called within an already-recording command list. The atlas is assumed to be
 * in COMMON state (already transitioned by the caller).
 *
 * Returns the resource to pass to process_atlas().
 */
static ID3D12Resource *
d3d12_crop_atlas_for_dp(struct comp_d3d12_compositor *c,
                        ID3D12Resource *atlas_resource,
                        uint32_t content_w,
                        uint32_t content_h)
{
	D3D12_RESOURCE_DESC atlas_desc = atlas_resource->GetDesc();

	if (content_w == (uint32_t)atlas_desc.Width && content_h == atlas_desc.Height) {
		return atlas_resource;
	}

	// Lazily (re)create intermediate resource at content dimensions
	if (c->dp_input_width != content_w || c->dp_input_height != content_h) {
		if (c->dp_input_resource != nullptr) {
			c->dp_input_resource->Release();
			c->dp_input_resource = nullptr;
		}

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = content_w;
		desc.Height = content_h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = atlas_desc.Format;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		HRESULT hr = c->device->CreateCommittedResource(
		    &heap, D3D12_HEAP_FLAG_NONE, &desc,
		    D3D12_RESOURCE_STATE_COMMON, nullptr,
		    IID_PPV_ARGS(&c->dp_input_resource));
		if (FAILED(hr)) {
			U_LOG_E("Failed to create D3D12 DP input resource %ux%u: 0x%lx",
			        content_w, content_h, hr);
			return atlas_resource;
		}
		c->dp_input_resource->SetName(L"DXR.dp_input_crop"); // #747: debug-layer attribution

		c->dp_input_width = content_w;
		c->dp_input_height = content_h;
		U_LOG_I("D3D12 crop: created DP input resource %ux%u (atlas %llux%u)",
		        content_w, content_h,
		        (unsigned long long)atlas_desc.Width, (unsigned)atlas_desc.Height);
	}

	// Transition intermediate: COMMON → COPY_DEST
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = c->dp_input_resource;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(1, &barrier);

	// #747: transition the ATLAS explicitly for the copy read, and put it back.
	//
	// The copy below READS the atlas. Without this the atlas (COMMON on entry)
	// is IMPLICITLY PROMOTED to COPY_SOURCE, and it does not decay back until
	// ExecuteCommandLists — so the caller's closing barrier, which declares
	// `StateBefore = COMMON` on the way to PIXEL_SHADER_RESOURCE, lies:
	//
	//   D3D12 ERROR id=527: Before state (COMMON|PRESENT) ... does not match
	//   with the current resource state (COPY_SOURCE) (promoted from COMMON)
	//
	// Wrong before-states are undefined behaviour. Making the transition
	// explicit here (rather than fixing up the caller's StateBefore) keeps the
	// invariant the callers already assume: **this function returns with the
	// atlas in COMMON on BOTH paths** — the early-out above never touches it,
	// and the copy path restores it. That symmetry is why the bug hid: the
	// early-out fires whenever content == atlas dims, so it only bites once the
	// atlas outgrows the content, which the high-water allocation in
	// create_atlas_texture() makes permanent after any shrink.
	D3D12_RESOURCE_BARRIER atlas_rb = {};
	atlas_rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	atlas_rb.Transition.pResource = atlas_resource;
	atlas_rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	atlas_rb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	atlas_rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(1, &atlas_rb);

	// Copy content region from atlas to intermediate
	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = c->dp_input_resource;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_loc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = atlas_resource;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;

	D3D12_BOX src_box = {0, 0, 0, content_w, content_h, 1};
	c->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

	// Atlas COPY_SOURCE → COMMON: restore the entry state the callers assume.
	atlas_rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	atlas_rb.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	c->cmd_list->ResourceBarrier(1, &atlas_rb);

	// Transition intermediate: COPY_DEST → COMMON (for DP)
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	c->cmd_list->ResourceBarrier(1, &barrier);

	return c->dp_input_resource;
}

/*
 *
 * MCP capture helpers
 *
 */

// u_capture_dims provider: report the renderer's CURRENT window-scaled per-view
// dims + tile layout so xrCaptureAtlasDXR can fill XrAtlasCaptureResultDXR with
// what the capture actually writes, not the nominal system info (#431).
static bool
d3d12_compositor_capture_dims_provider(void *userdata,
                                       uint32_t *out_view_w,
                                       uint32_t *out_view_h,
                                       uint32_t *out_tile_cols,
                                       uint32_t *out_tile_rows)
{
	struct comp_d3d12_compositor *c = static_cast<struct comp_d3d12_compositor *>(userdata);
	if (c == nullptr || c->renderer == nullptr) {
		return false;
	}
	// #542: report the frame's effective layout (what the capture actually
	// holds), falling back to the renderer's mode layout pre-first-commit.
	if (c->eff_layout.views > 0 && c->eff_layout.tile_w > 0 && c->eff_layout.tile_h > 0) {
		*out_view_w = c->eff_layout.tile_w;
		*out_view_h = c->eff_layout.tile_h;
		*out_tile_cols = c->eff_layout.cols;
		*out_tile_rows = c->eff_layout.rows;
		return true;
	}
	uint32_t vw = 0, vh = 0, cols = 1, rows = 1;
	comp_d3d12_renderer_get_view_dimensions(c->renderer, &vw, &vh);
	comp_d3d12_renderer_get_tile_layout(c->renderer, &cols, &rows);
	if (vw == 0 || vh == 0) {
		return false;
	}
	*out_view_w = vw;
	*out_view_h = vh;
	*out_tile_cols = cols;
	*out_tile_rows = rows;
	return true;
}

// Copy the content region of the renderer's atlas (tile_columns × view_width
// by tile_rows × view_height — what the app actually wrote, same region the
// compositor crops and sends to the DP) into a READBACK heap buffer, then
// write @p path as PNG. D3D12 renderer uses DXGI_FORMAT_R8G8B8A8_UNORM so no
// channel swap is needed.
//
// Caller must ensure the GPU is idle on entry (gpu_wait_idle has been called
// or the existing layer_commit fence-waits before returning). On exit the
// atlas is left in PIXEL_SHADER_RESOURCE state (matching the renderer's
// expected steady state between frames).
static bool
d3d12_compositor_capture_atlas_to_png(struct comp_d3d12_compositor *c, const char *path)
{
	ID3D12Resource *atlas = static_cast<ID3D12Resource *>(
	    comp_d3d12_renderer_get_atlas_resource(c->renderer));
	if (atlas == nullptr || c->renderer == nullptr) {
		return false;
	}

	// #542: capture the frame's effective content region (what the passes
	// painted), falling back to the renderer's mode layout pre-first-commit.
	uint32_t tile_columns = 1, tile_rows = 1;
	uint32_t view_w = 0, view_h = 0;
	if (c->eff_layout.views > 0 && c->eff_layout.tile_w > 0 && c->eff_layout.tile_h > 0) {
		tile_columns = c->eff_layout.cols;
		tile_rows = c->eff_layout.rows;
		view_w = c->eff_layout.tile_w;
		view_h = c->eff_layout.tile_h;
	} else {
		comp_d3d12_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);
		comp_d3d12_renderer_get_view_dimensions(c->renderer, &view_w, &view_h);
	}
	if (tile_columns == 0 || tile_rows == 0 || view_w == 0 || view_h == 0) {
		return false;
	}

	D3D12_RESOURCE_DESC adesc = atlas->GetDesc();
	uint32_t content_w = tile_columns * view_w;
	uint32_t content_h = tile_rows * view_h;
	if (content_w > adesc.Width)  content_w = (uint32_t)adesc.Width;
	if (content_h > adesc.Height) content_h = adesc.Height;

	// D3D12 readback row pitch must be aligned to 256.
	const UINT64 align = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	UINT64 row_pitch = ((UINT64)content_w * 4 + align - 1) & ~(align - 1);
	UINT64 rb_bytes = row_pitch * content_h;

	// Allocate a transient READBACK buffer. Lifetime = single capture.
	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC rb_desc = {};
	rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rb_desc.Width = rb_bytes;
	rb_desc.Height = 1;
	rb_desc.DepthOrArraySize = 1;
	rb_desc.MipLevels = 1;
	rb_desc.Format = DXGI_FORMAT_UNKNOWN;
	rb_desc.SampleDesc.Count = 1;
	rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	ID3D12Resource *readback = nullptr;
	if (FAILED(c->device->CreateCommittedResource(
	        &heap_props, D3D12_HEAP_FLAG_NONE, &rb_desc,
	        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	        IID_PPV_ARGS(&readback))) || readback == nullptr) {
		return false;
	}

	// Re-arm the cmd_allocator + cmd_list for our private use. GPU is
	// guaranteed idle at this point because layer_commit's existing
	// fence wait runs before we get here.
	c->cmd_allocator->Reset();
	c->cmd_list->Reset(c->cmd_allocator, nullptr);

	// Atlas: PIXEL_SHADER_RESOURCE → COPY_SOURCE.
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = atlas;
	b.Transition.Subresource = 0;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	c->cmd_list->ResourceBarrier(1, &b);

	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = atlas;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = readback;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst_loc.PlacedFootprint.Offset = 0;
	dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	dst_loc.PlacedFootprint.Footprint.Width = content_w;
	dst_loc.PlacedFootprint.Footprint.Height = content_h;
	dst_loc.PlacedFootprint.Footprint.Depth = 1;
	dst_loc.PlacedFootprint.Footprint.RowPitch = (UINT)row_pitch;

	D3D12_BOX src_box = {0, 0, 0, content_w, content_h, 1};
	c->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

	// Atlas: COPY_SOURCE → PIXEL_SHADER_RESOURCE (steady state).
	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	c->cmd_list->ResourceBarrier(1, &b);

	c->cmd_list->Close();
	ID3D12CommandList *lists[] = {c->cmd_list};
	c->command_queue->ExecuteCommandLists(1, lists);
	gpu_wait_idle(c);

	// Map readback, repack to tightly-packed rows, encode PNG.
	bool ok = false;
	void *mapped = nullptr;
	D3D12_RANGE read_range = {0, (SIZE_T)rb_bytes};
	if (SUCCEEDED(readback->Map(0, &read_range, &mapped)) && mapped != nullptr) {
		size_t tight_pitch = (size_t)content_w * 4;
		uint8_t *tight = (uint8_t *)malloc(tight_pitch * content_h);
		if (tight != nullptr) {
			const uint8_t *rb_pixels = (const uint8_t *)mapped;
			for (uint32_t y = 0; y < content_h; y++) {
				memcpy(tight + (size_t)y * tight_pitch,
				       rb_pixels + (size_t)y * row_pitch,
				       tight_pitch);
			}
			// Swapchain alpha is undefined for display output — force opaque
			// so the PNG doesn't render fully transparent/black (issue #425).
			// #672 diag: DXR_CAPTURE_KEEP_ALPHA=1 preserves the real atlas
			// alpha so transparency (zone bg / margins alpha=0) can be verified.
			if (getenv("DXR_CAPTURE_KEEP_ALPHA") == nullptr) {
				u_image_force_opaque_rgba8(tight, content_w, content_h, tight_pitch);
			}
			ok = stbi_write_png(path, (int)content_w, (int)content_h, 4,
			                    tight, (int)tight_pitch) != 0;
			free(tight);
		}
		D3D12_RANGE empty_range = {0, 0};
		readback->Unmap(0, &empty_range);
	}

	readback->Release();
	return ok;
}

// #672 diag: capture the WOVEN back buffer (post-DP output) to PNG. Unlike the
// atlas capture (pre-weave), this shows what the display processor actually
// produced — the interlaced panel image — so a zone dropped by the WEAVE (not
// by compositing) is visible. File-triggered: touch %TEMP%\dxr_woven_trigger,
// output %TEMP%\dxr_woven.png. back_buffer must be in `entry_state` on entry
// (PRESENT for the post-Present capture; COMMON for the #727 weave taps);
// left in `entry_state` on exit. Resets + reuses c->cmd_list, so the caller
// must have executed any recorded-but-unsubmitted work first.
static bool
d3d12_capture_backbuffer_to_png(struct comp_d3d12_compositor *c,
                                ID3D12Resource *back_buffer,
                                D3D12_RESOURCE_STATES entry_state,
                                const char *path)
{
	if (back_buffer == nullptr || c->device == nullptr) {
		return false;
	}
	D3D12_RESOURCE_DESC bd = back_buffer->GetDesc();
	uint32_t w = (uint32_t)bd.Width, h = (uint32_t)bd.Height;
	bool bgra = (bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
	             bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

	const UINT64 align = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	UINT64 row_pitch = ((UINT64)w * 4 + align - 1) & ~(align - 1);
	UINT64 rb_bytes = row_pitch * h;

	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC rb_desc = {};
	rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rb_desc.Width = rb_bytes;
	rb_desc.Height = 1;
	rb_desc.DepthOrArraySize = 1;
	rb_desc.MipLevels = 1;
	rb_desc.Format = DXGI_FORMAT_UNKNOWN;
	rb_desc.SampleDesc.Count = 1;
	rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ID3D12Resource *readback = nullptr;
	if (FAILED(c->device->CreateCommittedResource(
	        &heap_props, D3D12_HEAP_FLAG_NONE, &rb_desc,
	        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	        IID_PPV_ARGS(&readback))) || readback == nullptr) {
		return false;
	}

	c->cmd_allocator->Reset();
	c->cmd_list->Reset(c->cmd_allocator, nullptr);

	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = back_buffer;
	b.Transition.Subresource = 0;
	b.Transition.StateBefore = entry_state;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	c->cmd_list->ResourceBarrier(1, &b);

	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = back_buffer;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = readback;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst_loc.PlacedFootprint.Footprint.Format = bd.Format;
	dst_loc.PlacedFootprint.Footprint.Width = w;
	dst_loc.PlacedFootprint.Footprint.Height = h;
	dst_loc.PlacedFootprint.Footprint.Depth = 1;
	dst_loc.PlacedFootprint.Footprint.RowPitch = (UINT)row_pitch;
	D3D12_BOX src_box = {0, 0, 0, w, h, 1};
	c->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	c->cmd_list->ResourceBarrier(1, &b);
	c->cmd_list->Close();
	ID3D12CommandList *lists[] = {c->cmd_list};
	c->command_queue->ExecuteCommandLists(1, lists);
	gpu_wait_idle(c);

	bool ok = false;
	void *mapped = nullptr;
	D3D12_RANGE read_range = {0, (SIZE_T)rb_bytes};
	if (SUCCEEDED(readback->Map(0, &read_range, &mapped)) && mapped != nullptr) {
		size_t tight_pitch = (size_t)w * 4;
		uint8_t *tight = (uint8_t *)malloc(tight_pitch * h);
		if (tight != nullptr) {
			const uint8_t *rb = (const uint8_t *)mapped;
			for (uint32_t y = 0; y < h; y++) {
				memcpy(tight + (size_t)y * tight_pitch,
				       rb + (size_t)y * row_pitch, tight_pitch);
			}
			if (bgra) {
				for (size_t i = 0; i < (size_t)w * h; i++) {
					uint8_t t = tight[i * 4 + 0];
					tight[i * 4 + 0] = tight[i * 4 + 2];
					tight[i * 4 + 2] = t;
				}
			}
			// #672: keep real alpha when DXR_CAPTURE_KEEP_ALPHA is set, so a
			// zone the post-weave alpha-gate wrongly zeroed (→ transparent →
			// invisible on panel) is distinguishable from opaque woven content.
			if (getenv("DXR_CAPTURE_KEEP_ALPHA") == nullptr) {
				u_image_force_opaque_rgba8(tight, w, h, tight_pitch);
			}
			ok = stbi_write_png(path, (int)w, (int)h, 4, tight, (int)tight_pitch) != 0;
			free(tight);
		}
		D3D12_RANGE empty = {0, 0};
		readback->Unmap(0, &empty);
	}
	readback->Release();
	return ok;
}

// Run the capture readback if the per-frame intent matches @p mode_filter.
static void
d3d12_compositor_dispatch_capture(struct comp_d3d12_compositor *c, uint32_t mode_filter)
{
	if (!u_capture_intent_should_capture(&c->capture_intent, mode_filter)) {
		return;
	}
	bool ok = d3d12_compositor_capture_atlas_to_png(c, c->capture_intent.path);
	if (ok) {
		U_LOG_I("Atlas captured (mode=%u) to %s",
		        c->capture_intent.mode, c->capture_intent.path);
	} else {
		U_LOG_W("Atlas capture failed (mode=%u path=%s)",
		        c->capture_intent.mode, c->capture_intent.path);
	}
	u_capture_intent_complete(&c->capture_intent, &c->mcp_capture, ok);
}


// #854: write @p dp_resource's SRV into dp_srv_heap, bind the heap on the open
// cmd_list, and return the GPU handle for process_atlas. The sim DP samples the
// atlas through root descriptor table 0, and until now both call sites passed
// literal 0 (the SR weaver takes its input via setInputViewTexture and ignores
// the handle) — binding GPU VA 0 is what the debug layer reports as descriptor
// corruption and what retail UMDs intermittently AV on (nvwgf2umx, #854).
// Rewriting the single slot every frame is safe: layer_commit ends in an
// unconditional Signal+Wait, so the GPU never reads last frame's descriptor
// while this one is written. Returns 0 (DPs must then skip sampling) only when
// the heap is missing.
static uint64_t
d3d12_bind_dp_atlas_srv(struct comp_d3d12_compositor *c, ID3D12Resource *dp_resource)
{
	if (c->dp_srv_heap == nullptr || dp_resource == nullptr) {
		return 0;
	}

	D3D12_RESOURCE_DESC dp_desc = dp_resource->GetDesc();
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	// Typeless atlases (and anything unviewable) fall back to the path's
	// R8G8B8A8_UNORM contract — the same format process_atlas advertises.
	srv_desc.Format = (dp_desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) ? DXGI_FORMAT_R8G8B8A8_UNORM
	                                                                    : dp_desc.Format;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MipLevels = 1;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	c->device->CreateShaderResourceView(dp_resource, &srv_desc,
	                                    c->dp_srv_heap->GetCPUDescriptorHandleForHeapStart());

	// Every later pass in this cmd_list (renderer draw, zone composite,
	// Local2D flatten) binds its own heap before use, so this bind is scoped
	// to the DP call that follows.
	ID3D12DescriptorHeap *heaps[] = {c->dp_srv_heap};
	c->cmd_list->SetDescriptorHeaps(1, heaps);
	return c->dp_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr;
}

/*!
 * #868: record the display-processor weave into the open command list, then
 * close, execute and present it.
 *
 * Everything this reads lives in `c->repaint`, which layer_commit fills in
 * immediately before the call. That is the whole point: the weave is a pure
 * function of (atlas, geometry, canvas, backdrop) plus a freshly acquired back
 * buffer and whatever eye positions the display processor pulls for itself at
 * weave time. Hold those fixed, re-run this, and you get the same content
 * re-derived for where the viewer is NOW — which is exactly what the repaint
 * thread wants.
 *
 * Entry contract: `c->cmd_list` is open and freshly reset, `c->repaint.atlas` is
 * in PIXEL_SHADER_RESOURCE, and layer_accum holds a COMPLETE frame's layers (the
 * crop and the 2D-under flatten below are rebuilt from it every weave).
 *
 * @param is_repaint Selects the pacing mark that keeps repaints out of the
 *        saturation governor and the #867 prediction ledger.
 * @param[out] out_back_buffer The image this weave went into. Captured before
 *        the present, which advances the swapchain's current index.
 */
static xrt_result_t
d3d12_dp_weave_and_present(struct comp_d3d12_compositor *c, bool is_repaint, ID3D12Resource **out_back_buffer)
{
	const uint32_t tgt_width = c->repaint.tgt_w;
	const uint32_t tgt_height = c->repaint.tgt_h;
	const struct u_canvas_rect eff_canvas = c->repaint.canvas;

	uint32_t bb_index = comp_d3d12_target_get_current_index(c->target);
	ID3D12Resource *back_buffer =
	    static_cast<ID3D12Resource *>(comp_d3d12_target_get_back_buffer(c->target, bb_index));
	uint64_t rtv_handle_raw = comp_d3d12_target_get_rtv_handle(c->target, bb_index);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
	rtv_handle.ptr = static_cast<SIZE_T>(rtv_handle_raw);
	if (out_back_buffer != nullptr) {
		*out_back_buffer = back_buffer;
	}

	// One-time diagnostic: log back buffer vs viewport dimensions
	static bool dp_dims_logged = false;
	if (!dp_dims_logged && back_buffer != nullptr) {
		dp_dims_logged = true;
		D3D12_RESOURCE_DESC bb_desc = back_buffer->GetDesc();
		U_LOG_W("D3D12 DP dims: back_buffer=%llux%u, viewport=%ux%u, "
		        "view=%ux%u, atlas=%ux%u (tile %ux%u)",
		        (unsigned long long)bb_desc.Width, (unsigned)bb_desc.Height, tgt_width, tgt_height,
		        c->repaint.view_w, c->repaint.view_h, c->repaint.cols * c->repaint.view_w,
		        c->repaint.rows * c->repaint.view_h, c->repaint.cols, c->repaint.rows);
	}

	// Transition back buffer: PRESENT → RENDER_TARGET
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = back_buffer;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(1, &barrier);

	// Bind back buffer as render target
	c->cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

	// Atlas → COMMON for the DP, and rebuild BOTH downstream inputs from
	// scratch on every weave, repaint included.
	//
	// Caching the cropped atlas and the flattened backdrop across a repaint was
	// wrong: the 2D-under backdrop is rebuilt from layer_accum each weave, so a
	// repaint that reuses the previous one hands the display processor a
	// backdrop belonging to an older frame — the 2D region then disagrees
	// between app frames and repaints, which reads as the desktop
	// compose-under flickering on top of the 2D. Both are cheap next to the
	// weave itself, and replaying them makes a repaint bit-identical in
	// construction to the app frame it stands in for.
	D3D12_RESOURCE_BARRIER atlas_barrier = {};
	atlas_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	atlas_barrier.Transition.pResource = c->repaint.atlas;
	atlas_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	atlas_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	atlas_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	c->cmd_list->ResourceBarrier(1, &atlas_barrier);

	ID3D12Resource *dp_resource =
	    d3d12_crop_atlas_for_dp(c, c->repaint.atlas, c->repaint.content_w, c->repaint.content_h);

	/*
	 * The 2D-under backdrop flatten samples the APP'S OWN Local2D swapchain
	 * images, exactly like the over-layer flatten in the composite — so a
	 * repaint must not re-run it either, and reuses what the last app frame
	 * produced. backdrop_scratch is compositor-owned and stable until the next
	 * layer_commit. (Invisible in the avatar scene, where the backdrop is NULL
	 * on every weave, but it is the same class of bug as the one that produced
	 * the 2D flicker — see the composite.)
	 */
	if (!is_repaint) {
		uint32_t bd_w = 0, bd_h = 0;
		c->repaint.backdrop = d3d12_flatten_backdrop_2d(c, tgt_width, tgt_height, &bd_w, &bd_h);
		c->repaint.backdrop_w = bd_w;
		c->repaint.backdrop_h = bd_h;
	}
	xrt_display_processor_d3d12_set_background_2d(c->display_processor, c->repaint.backdrop,
	                                              c->repaint.backdrop_w, c->repaint.backdrop_h);


	D3D12_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(tgt_width);
	viewport.Height = static_cast<float>(tgt_height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	c->cmd_list->RSSetViewports(1, &viewport);

	D3D12_RECT scissor = {};
	scissor.left = 0;
	scissor.top = 0;
	scissor.right = static_cast<LONG>(tgt_width);
	scissor.bottom = static_cast<LONG>(tgt_height);
	c->cmd_list->RSSetScissorRects(1, &scissor);

	// Late-weave pacing + weave-latency harness mark (env-gated no-ops
	// otherwise). A repaint already paced itself unlocked, and is deliberately
	// kept out of the governor's frame-interval EMA and out of the #867
	// prediction ledger — it has no app frame and no promised photon time
	// behind it.
	if (is_repaint) {
		comp_d3d12_target_weave_mark_repaint(c->target);
	} else {
		comp_d3d12_target_weave_mark(c->target, c->last_display_time_ns);
	}

	// Timing feedback: hand the DP last frame's MEASURED weave→scanout residual
	// so the vendor eye predictor runs with an exact horizon (0 = unknown ⟹ DP
	// heuristic).
	xrt_display_processor_d3d12_set_frame_timing(c->display_processor,
	                                             comp_d3d12_target_get_measured_weave_ns(c->target),
	                                             (uint64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate));

	// Pass actual backbuffer dimensions to the DP. Canvas offset and size are
	// passed separately — the DP uses them to set a viewport sub-rect for
	// correct interlacing phase.
	xrt_display_processor_d3d12_process_atlas(
	    c->display_processor, c->cmd_list, dp_resource,
	    d3d12_bind_dp_atlas_srv(c, dp_resource), // #854: real SRV — sim DP binds it; SR weaver ignores it
	    rtv_handle.ptr, back_buffer, c->repaint.view_w, c->repaint.view_h, c->repaint.cols, c->repaint.rows,
	    static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM), tgt_width, tgt_height,
	    eff_canvas.valid ? eff_canvas.x : 0, eff_canvas.valid ? eff_canvas.y : 0,
	    eff_canvas.valid ? eff_canvas.w : 0, eff_canvas.valid ? eff_canvas.h : 0);

	// #439 / ADR-027: an authored zone mask or Local2D layers composite the
	// 2D/3D regions of the back buffer. Back buffer is still in RENDER_TARGET
	// from the DP; leave it in RENDER_TARGET so HUD's existing RT→COPY_DEST
	// transition (below) proceeds unchanged. No-op when this frame carries no
	// zones / Local2D / explicit mask.
	d3d12_composite_zone_mask(c, /*reuse_mask=*/true, /*prepare_only=*/false, back_buffer, rtv_handle.ptr,
	                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
	                          tgt_width, tgt_height, &eff_canvas);


	// Transition atlas back: COMMON → PIXEL_SHADER_RESOURCE
	atlas_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	atlas_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	c->cmd_list->ResourceBarrier(1, &atlas_barrier);

	// Transition back buffer for HUD overlay
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	c->cmd_list->ResourceBarrier(1, &barrier);

	// HUD overlay
	d3d12_render_hud_overlay(c, c->cmd_list, back_buffer, tgt_width, tgt_height, &c->repaint.eye_pos);

	// Transition back buffer -> PRESENT, assuming the HUD overlay above
	// left it in COPY_DEST.
	//
	// UNSOUND — tracked as #747. This StateBefore is an ASSUMPTION, not
	// a tracked state: process_atlas hands the back buffer to the vendor
	// display processor, which is a plug-in DLL (ADR-019) free to leave
	// it in any state, and D3D12 offers no way to query it back. The
	// assumption is unverifiable here and wrong states are undefined
	// behaviour — a credible mechanism for the DEVICE_HUNG seen during
	// interactive resize churn.
	//
	// The fix belongs in the plug-in contract (xrt_plugin_iface should
	// SPECIFY the required entry state and the guaranteed exit state for
	// the atlas and the target), not in more guessing here. Do not
	// "improve" this by inferring what a particular vendor's DP does
	// internally — that was the previous comment's mistake, and it
	// justified this state by describing a chroma-key alpha pass that
	// #573 deleted (set_chroma_key is gone from all five DP vtables;
	// see xrt_plugin.h). The reasoning outlived the mechanism.
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	c->cmd_list->ResourceBarrier(1, &barrier);

	// Close and execute
	c->cmd_list->Close();
	ID3D12CommandList *weave_lists[] = {c->cmd_list};
	c->command_queue->ExecuteCommandLists(1, weave_lists);

	// Present with VSync
	xrt_result_t xret = comp_d3d12_target_present(c->target, 1);

	// Wait for frame completion (frame pacing)
	gpu_wait_idle(c);

	// Only a real frame resets the quiet-gate; see d3d12_repaint_thread.
	if (!is_repaint) {
		c->repaint.last_app_frame_ns = os_monotonic_get_ns();
	}

	// #672 diag: file-triggered WOVEN back-buffer capture (post-DP). Shows the
	// actual panel image, so a zone dropped by the weave is visible. Lives here
	// rather than in layer_commit so it can capture a REPAINT too — the output
	// name says which kind of weave produced it (#868).
	if (back_buffer != nullptr) {
		const char *tmp = getenv("TEMP");
		if (tmp != nullptr) {
			// One trigger arms BOTH kinds, so the pair captured is adjacent in
			// time — that is what makes a pixel diff meaningful. Comparing an
			// app weave against a repaint captured seconds apart would drown
			// the signal in scene animation.
			static bool want_app = false, want_repaint = false;
			char trig[512];
			snprintf(trig, sizeof(trig), "%s\\dxr_woven_trigger", tmp);
			FILE *tf = fopen(trig, "rb");
			if (tf != nullptr) {
				fclose(tf);
				remove(trig);
				want_app = true;
				want_repaint = true;
			}
			bool *want = is_repaint ? &want_repaint : &want_app;
			if (*want) {
				*want = false;
				char out[512];
				snprintf(out, sizeof(out), "%s\\dxr_woven_%s.png", tmp, is_repaint ? "repaint" : "app");
				bool wok =
				    d3d12_capture_backbuffer_to_png(c, back_buffer, D3D12_RESOURCE_STATE_PRESENT, out);
				U_LOG_W("#672 woven back-buffer capture %s -> %s", wok ? "OK" : "FAILED", out);
			}
		}
	}

	return xret;
}

/*!
 * #868 repaint loop: re-weave the last atlas at panel rate while the app is
 * between frames.
 *
 * Ticks once per display period and replays the last weave only when the app
 * has actually gone quiet (no real frame for ~1.5 periods). That gate is what
 * keeps this from stealing time from an app that is already making rate: an app
 * at or above the panel rate never trips it, and the thread costs one wakeup per
 * refresh doing nothing.
 *
 * The lock is held across the whole replay. That serialises the repaint against
 * layer_commit, so the display processor still only ever sees one caller — a
 * loop we added must not become a thread-safety requirement on vendor plug-ins.
 * The cost is that a real frame arriving mid-repaint waits for it; bounded by
 * one weave+present, and only reachable when the app was idle enough to trip the
 * gate in the first place.
 */
static void
d3d12_repaint_thread(struct comp_d3d12_compositor *c)
{
	while (!c->repaint_quit.load(std::memory_order_relaxed)) {
		const double hz = (c->display_refresh_rate > 1.0f) ? (double)c->display_refresh_rate : 60.0;
		const uint64_t period_ns = (uint64_t)(U_TIME_1S_IN_NS / hz);

		// Tick well inside a period so "the app went quiet" is noticed near the
		// refresh it matters for, rather than up to a full period late.
		os_nanosleep((int64_t)(period_ns / 4));

		if (c->repaint_quit.load(std::memory_order_relaxed)) {
			break;
		}

		c->repaint.ticks++;

		// Cheap unlocked pre-check, re-tested under the lock below. Avoids
		// paying the pacing wait on every tick of an app that is making rate.
		if (!c->repaint.armed || c->repaint.app_frame_in_progress) {
			c->repaint.bail_armed++;
			continue;
		}
		// Fire only once the app has ALREADY missed a full refresh — i.e. the
		// panel has shown this atlas for a whole period and is about to show it
		// again. Keyed on the last APP frame, never on the last repaint —
		// otherwise repaints would pace off their own timestamps and drift below
		// panel rate. Their cadence comes from the scanout wait instead.
		//
		// Two periods, not one-and-a-bit, and that margin is the whole design.
		// An app whose interval merely straddles a period (measured: the Unity
		// avatar at 46.7 fps on this 60 Hz panel, a 1.28-period interval) is
		// about to submit anyway: by the time a repaint paces and takes the
		// lock, the real frame has landed, so it bails having bought nothing —
		// and on the ticks where it does not bail it is competing with the app
		// for the same GPU to fill a gap that was closing on its own. The win
		// only exists when a refresh is genuinely unclaimed, which is what
		// >= 2 periods means. That is also exactly the case #868 is FOR: a
		// 60 fps app on a 240 Hz panel sits at 4 periods.
		//
		// DXR_WEAVE_REPAINT_FORCE=1 bypasses the gate so the repaint path can be
		// exercised on hardware where no app is slow enough to trip it. It makes
		// the app SLOWER (it is meant to) — it is a correctness probe, never a
		// perf setting.
		if (c->repaint.force != 1 && os_monotonic_get_ns() - c->repaint.last_app_frame_ns < period_ns * 2) {
			c->repaint.bail_gate++;
			continue;
		}

		// Pace to the panel BEFORE taking the lock — this blocks for up to a
		// few periods, and holding the lock across it would stall an arriving
		// app frame for exactly that long.
		comp_d3d12_target_repaint_pace(c->target);

		std::lock_guard<std::mutex> lock(c->mutex);

		// Re-test everything: a real frame may have landed while we paced, in
		// which case it just did this work and there is nothing stale to fix.
		// Re-test under the lock. app_frame_in_progress is the load-bearing one:
		// the app can have opened a submission while we paced. It is NOT
		// bypassed by the force probe — forcing a repaint into a half-written
		// layer_accum does not exercise the feature, it just corrupts the frame.
		if (c->repaint_quit.load(std::memory_order_relaxed) || !c->repaint.armed ||
		    c->repaint.app_frame_in_progress || c->display_processor == NULL || c->target == nullptr ||
		    c->repaint.atlas == nullptr) {
			c->repaint.bail_armed++;
			continue;
		}
		if (c->repaint.force != 1 && os_monotonic_get_ns() - c->repaint.last_app_frame_ns < period_ns) {
			c->repaint.bail_race++;
			continue;
		}

		c->cmd_allocator->Reset();
		c->cmd_list->Reset(c->cmd_allocator, nullptr);

		d3d12_dp_weave_and_present(c, true, nullptr);

		c->repaint.count++;
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("#868: repainting last atlas at %.1f Hz while the app is between frames "
			        "(set DXR_WEAVE_REPAINT=0 to disable)",
			        hz);
		}
		// Periodic counters: apps are usually killed rather than shut down
		// cleanly, so the destroy-time dump alone is rarely seen.
		if ((c->repaint.count % 240) == 0) {
			U_LOG_W("#868: repaints=%llu ticks=%llu bail{armed=%llu gate=%llu race=%llu}",
			        (unsigned long long)c->repaint.count, (unsigned long long)c->repaint.ticks,
			        (unsigned long long)c->repaint.bail_armed,
			        (unsigned long long)c->repaint.bail_gate,
			        (unsigned long long)c->repaint.bail_race);
		}
	}
}

static xrt_result_t
d3d12_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// #868: the submission window closes here. Cleared at the TOP rather than
	// on the way out so it cannot be leaked by one of this function's several
	// early returns — the lock is held throughout, so no repaint can interleave
	// with layer_commit itself regardless.
	c->repaint.app_frame_in_progress = false;

	// Capture-intent poll — see u_capture_intent.h. Consumed at the
	// projection-done boundary (PROJECTION_ONLY, once renderer split
	// lands) or end of frame (POST_COMPOSE).
	u_capture_intent_poll(&c->capture_intent, &c->mcp_capture);

	// Get predicted eye positions
	struct xrt_eye_positions eye_pos = {};
	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d12_get_predicted_eye_positions(c->display_processor, &eye_pos);
	}
	if (!eye_pos.valid) {
		// Use view_count from the active rendering mode for the fallback
		uint32_t fallback_count = 2;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count) {
				fallback_count = c->xdev->rendering_modes[idx].view_count;
			}
		}
		if (fallback_count == 1) {
			eye_pos.count = 1;
			eye_pos.eyes[0] = {0.0f, 0.0f, 0.6f};
		} else {
			eye_pos.count = 2;
			eye_pos.eyes[0] = {-0.032f, 0.0f, 0.6f};
			eye_pos.eyes[1] = { 0.032f, 0.0f, 0.6f};
		}
	}

	// Extract eye positions for renderer (display processor still needs L/R)
	struct xrt_vec3 left_eye = {eye_pos.eyes[0].x, eye_pos.eyes[0].y, eye_pos.eyes[0].z};
	struct xrt_vec3 right_eye = {eye_pos.eyes[1].x, eye_pos.eyes[1].y, eye_pos.eyes[1].z};

	// Sync hardware_display_3d and tile layout from device's active rendering mode
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			c->hardware_display_3d = mode->hardware_display_3d;
			// Clamp eye count to the active mode's view_count
			if (eye_pos.count > mode->view_count) {
				eye_pos.count = mode->view_count;
			}
			if (mode->tile_columns > 0 && c->renderer != NULL) {
				comp_d3d12_renderer_set_tile_layout(
				    c->renderer, mode->tile_columns, mode->tile_rows);
			}
		}
	}

	// Diagnostic: log layer info for first 5 frames then every ~300 frames
	static uint32_t diag_counter = 0;
	bool diag_log = (diag_counter < 5 || diag_counter % 300 == 0);
	diag_counter++;
	if (diag_log) {
		U_LOG_I("D3D12 layer_commit: layers=%u, 3d=%d, dp=%p, target=%p",
		        c->layer_accum.layer_count, c->hardware_display_3d,
		        (void *)c->display_processor, (void *)c->target);
	}

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != nullptr) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = c->xsysd->static_roles.head;
			if (head != nullptr && head->hmd != NULL) {
				if (force_2d) {
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						c->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					head->hmd->active_rendering_mode_index = c->last_3d_mode_index;
				}
			}
			comp_d3d12_compositor_request_display_mode(&c->base.base, !force_2d);
		}

		// Rendering mode change from qwerty 1/2/3 keys (disabled for legacy apps).
		if (!c->legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(c->xsysd->xdevs, c->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = c->xsysd->static_roles.head;
				if (head != nullptr) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// #439 Phase 3: detect Local2D layers once per frame (under c->mutex).
	// Drives the effective-canvas supersede + the composite's have_local_2d
	// branch — mirrors the D3D11 leg (set before eff_canvas is computed).
	// XR_DXR_display_zones: the zones-frame flag is resolved in the same
	// scan (one coherent per-frame decision).
	c->local_2d_last_frame = false;
	c->zones_frame = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			c->local_2d_last_frame = true;
		} else if (c->layer_accum.layers[i].data.type == XRT_LAYER_ZONE_3D) {
			c->zones_frame = true;
		}
	}

	// XR_DXR_display_zones hardware leg (P4). Zone-capable DP: the per-frame
	// wish publish after each path's fence wait drives the per-region switch
	// — skip the global fallback. Legacy DP (no zone slots): tier-1 fallback
	// — "any zone active => request 3D" once on the rising edge, no forced
	// 2D on the falling edge.
	if (c->zones_frame && !c->zones_mode_requested && !d3d12_zone_dp_supported(c)) {
		c->zones_mode_requested = true;
		comp_d3d12_compositor_request_display_mode(&c->base.base, true);
	} else if (!c->zones_frame) {
		c->zones_mode_requested = false;
	}

	// Reset this frame's resolved wish source — d3d12_update_zone_wish_state
	// sets it in zones frames; a stale pointer from an earlier frame must
	// never publish. (zone_publish_w/h persist as the previous raster's dims
	// for the auto-wish seq dirty-check.)
	c->zone_publish_res = nullptr;

	// #439 Phase 2: the one canvas authority for this frame. While a zone
	// mask is active (or Local2D layers are present) this is the client-window
	// rect (it supersedes the output rect); otherwise it is an invalid rect
	// (readers fall back to full-window/target dims). Computed once under c->mutex (held for this whole function)
	// so the weave region, view dims, and composite all see the same rect even
	// if submit/destroy race the frame.
	const struct u_canvas_rect eff_canvas = d3d12_effective_canvas(c);

	// Get target (window) dimensions for mono viewport sizing + zone-rect
	// placement scale. In shared-texture mode (no target) use the canvas dims
	// when available — zone rects are window-pixel, so the zones placement
	// scale (tile/target) must divide by the WINDOW, not the display-sized
	// shared texture (mirrors the D3D11 leg; without this `else if` branch
	// zones scale ~display/window too small and overlap at the origin — #613).
	// The DP weave target stays the shared-texture dims (computed separately
	// from c->shared_texture below).
	uint32_t tgt_width = c->settings.preferred.width;
	uint32_t tgt_height = c->settings.preferred.height;
	if (c->target != nullptr) {
		comp_d3d12_target_get_dimensions(c->target, &tgt_width, &tgt_height);
	} else if (eff_canvas.valid && eff_canvas.w > 0 && eff_canvas.h > 0) {
		tgt_width = eff_canvas.w;
		tgt_height = eff_canvas.h;
	}

	// Sync renderer view dims from active mode — set_tile_layout derives
	// view dims from atlas invariance, but actual mode dims may differ
	// (e.g. 2D mode needs full display height). Resize if needed.
	// Legacy apps: view dims are fixed at compromise scale, skip mode sync.
	if (!c->legacy_app_tile_scaling &&
	    c->xdev != NULL && c->xdev->hmd != NULL && c->renderer != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			if (mode->view_width_pixels > 0) {
				uint32_t new_vw = mode->view_width_pixels;
				uint32_t new_vh = mode->view_height_pixels;
				if (eff_canvas.valid) {
					u_tiling_compute_canvas_view(mode, eff_canvas.w, eff_canvas.h,
					                             &new_vw, &new_vh);
				} else if (!c->owns_window && tgt_width > 0 && tgt_height > 0) {
					// Handle app: window may differ from display size,
					// derive view dims from actual window client area.
					u_tiling_compute_canvas_view(mode, tgt_width, tgt_height,
					                             &new_vw, &new_vh);
				}
				uint32_t cur_vw, cur_vh;
				comp_d3d12_renderer_get_view_dimensions(c->renderer, &cur_vw, &cur_vh);
				if (cur_vw != new_vw || cur_vh != new_vh) {
					uint32_t resize_target_h = (c->display_processor != NULL)
					    ? new_vh : tgt_height;
					comp_d3d12_renderer_resize(
					    c->renderer,
					    new_vw,
					    new_vh,
					    resize_target_h);
				}
			}
		}
	}

	// Per-frame effective CONTENT layout (#542): the mode's recipe, with the
	// submission clamped to it. Feeds both renderer passes, the DP handoffs,
	// and the capture providers — they must all agree on the frame's
	// geometry.
	comp_d3d12_renderer_compute_effective_layout(c->renderer, &c->layer_accum, &c->eff_layout);

	// Zero-copy check: can we pass the app's swapchain directly to the DP?
	bool zero_copy = false;
	void *zc_resource = nullptr;
	{
		const struct xrt_rendering_mode *mode = NULL;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count)
				mode = &c->xdev->rendering_modes[idx];
		}
		if (mode != NULL && c->layer_accum.layer_count == 1) {
			struct comp_layer *layer = &c->layer_accum.layers[0];
			if (layer->data.type == XRT_LAYER_PROJECTION ||
			    layer->data.type == XRT_LAYER_PROJECTION_DEPTH) {
				uint32_t vc = mode->view_count;
				// #542: a hardware/content divergence frame (submitted
				// views != mode views) must take the atlas path — the
				// per-view loops below would read stale proj.v[] slots,
				// and zero-copy can't re-tile a mismatched submission.
				bool same_sc = (vc > 0 && vc <= XRT_MAX_VIEWS && layer->data.view_count == vc &&
				                layer->sc_array[0] != NULL);
				for (uint32_t v = 1; v < vc && same_sc; v++) {
					if (layer->sc_array[v] != layer->sc_array[0])
						same_sc = false;
				}
				if (same_sc) {
					uint32_t img_idx = layer->data.proj.v[0].sub.image_index;
					bool same_idx = true;
					for (uint32_t v = 1; v < vc; v++) {
						if (layer->data.proj.v[v].sub.image_index != img_idx) {
							same_idx = false;
							break;
						}
					}
					bool all_array_zero = same_idx;
					for (uint32_t v = 0; v < vc && all_array_zero; v++) {
						if (layer->data.proj.v[v].sub.array_index != 0)
							all_array_zero = false;
					}
					if (all_array_zero) {
						uint32_t sw, sh;
						comp_d3d12_swapchain_get_dimensions(layer->sc_array[0], &sw, &sh);
						int32_t rxs[XRT_MAX_VIEWS], rys[XRT_MAX_VIEWS];
						uint32_t rws[XRT_MAX_VIEWS], rhs_arr[XRT_MAX_VIEWS];
						for (uint32_t v = 0; v < vc; v++) {
							rxs[v] = layer->data.proj.v[v].sub.rect.offset.w;
							rys[v] = layer->data.proj.v[v].sub.rect.offset.h;
							rws[v] = layer->data.proj.v[v].sub.rect.extent.w;
							rhs_arr[v] = layer->data.proj.v[v].sub.rect.extent.h;
						}
						if (u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs_arr, sw, sh, mode)) {
							zc_resource = comp_d3d12_swapchain_get_resource(layer->sc_array[0], img_idx);
							if (zc_resource != nullptr)
								zero_copy = true;
						}
					}
				}
			}
		}
	}

	// Reset command allocator and command list
	c->cmd_allocator->Reset();
	c->cmd_list->Reset(c->cmd_allocator, nullptr);

	// Verify app renders at the expected resolution (not stretched)
	{
		static int rect_check_log = 0;
		uint32_t expected_vw, expected_vh;
		comp_d3d12_renderer_get_view_dimensions(c->renderer, &expected_vw, &expected_vh);
		for (uint32_t li = 0; li < c->layer_accum.layer_count && rect_check_log < 8; li++) {
			struct comp_layer *layer = &c->layer_accum.layers[li];
			if (layer->data.type != XRT_LAYER_PROJECTION &&
			    layer->data.type != XRT_LAYER_PROJECTION_DEPTH)
				continue;
			for (uint32_t v = 0; v < layer->data.view_count && v < XRT_MAX_VIEWS; v++) {
				const struct xrt_rect *r = &layer->data.proj.v[v].sub.rect;
				if ((uint32_t)r->extent.w != expected_vw || (uint32_t)r->extent.h != expected_vh) {
					if (rect_check_log < 5) {
						U_LOG_W("VIEW SIZE MISMATCH: view[%u] app_rect=%dx%d "
						        "expected=%ux%u (legacy=%d)",
						        v, r->extent.w, r->extent.h,
						        expected_vw, expected_vh,
						        c->legacy_app_tile_scaling);
					}
					rect_check_log++;
				} else if (rect_check_log < 3) {
					U_LOG_I("VIEW SIZE OK: view[%u] app_rect=%dx%d matches expected=%ux%u",
					        v, r->extent.w, r->extent.h, expected_vw, expected_vh);
					rect_check_log++;
				}
			}
		}
	}

	// Render layers to atlas texture (skip if zero-copy). Split into a
	// projection pass + window-space pass so a projection-only capture
	// can read the atlas in between.
	xrt_result_t xret = XRT_SUCCESS;
	if (!zero_copy) {
		xret = comp_d3d12_renderer_draw_projection_pass(
		    c->renderer, c->cmd_list, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, &c->eff_layout);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render projection pass");
			return xret;
		}

		// Projection-only capture point. Atlas is in RENDER_TARGET with
		// uncommitted projection commands in the cmd_list. To read it back
		// we need to commit those commands, transition the atlas to
		// PIXEL_SHADER_RESOURCE, run the capture (which uses the cmd_list
		// for its own copy + barriers), then transition back to
		// RENDER_TARGET so the window-space pass can append draws.
		if (c->capture_intent.pending && c->capture_intent.mode == MCP_CAPTURE_MODE_PROJECTION_ONLY) {
			ID3D12Resource *atlas_res = static_cast<ID3D12Resource *>(
			    comp_d3d12_renderer_get_atlas_resource(c->renderer));

			D3D12_RESOURCE_BARRIER ws_barrier = {};
			ws_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			ws_barrier.Transition.pResource = atlas_res;
			ws_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			ws_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			ws_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			c->cmd_list->ResourceBarrier(1, &ws_barrier);

			c->cmd_list->Close();
			ID3D12CommandList *flush_lists[] = {c->cmd_list};
			c->command_queue->ExecuteCommandLists(1, flush_lists);
			gpu_wait_idle(c);

			// Capture handles its own cmd_list reset + barriers (PSR↔COPY_SOURCE).
			d3d12_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_PROJECTION_ONLY);

			// Re-arm cmd_list and put atlas back in RENDER_TARGET.
			c->cmd_allocator->Reset();
			c->cmd_list->Reset(c->cmd_allocator, nullptr);
			ws_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			ws_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			c->cmd_list->ResourceBarrier(1, &ws_barrier);
		}

		xret = comp_d3d12_renderer_draw_window_space_pass(
		    c->renderer, c->cmd_list, &c->layer_accum, tgt_width, tgt_height, &c->eff_layout);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render window-space pass");
			return xret;
		}
	}

	// Shared texture mode: weave (or copy) atlas into shared texture, skip window present
	if (c->has_shared_texture && c->shared_texture != nullptr) {
		ID3D12Resource *atlas_resource = zero_copy
		    ? static_cast<ID3D12Resource *>(zc_resource)
		    : static_cast<ID3D12Resource *>(comp_d3d12_renderer_get_atlas_resource(c->renderer));

		if (atlas_resource != nullptr && c->display_processor != NULL && c->shared_texture_rtv_heap != nullptr) {
			// DP path: weave atlas directly into shared texture
			static bool st_dp_logged = false;
			if (!st_dp_logged) {
				U_LOG_W("D3D12 shared texture: weaving via display processor");
				st_dp_logged = true;
			}

			// Execute atlas rendering commands first
			c->cmd_list->Close();
			ID3D12CommandList *copy_lists[] = {c->cmd_list};
			c->command_queue->ExecuteCommandLists(1, copy_lists);
			gpu_wait_idle(c);

			// Fresh command list for weaver
			c->cmd_allocator->Reset();
			c->cmd_list->Reset(c->cmd_allocator, nullptr);

			// Transition: shared texture COMMON→RENDER_TARGET, atlas PSR→COMMON
			D3D12_RESOURCE_BARRIER barriers[2] = {};
			barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[0].Transition.pResource = c->shared_texture;
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

			barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[1].Transition.pResource = atlas_resource;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

			c->cmd_list->ResourceBarrier(2, barriers);

			// Bind shared texture as render target
			D3D12_CPU_DESCRIPTOR_HANDLE st_rtv = c->shared_texture_rtv_heap->GetCPUDescriptorHandleForHeapStart();
			c->cmd_list->OMSetRenderTargets(1, &st_rtv, FALSE, nullptr);

			// #542: the DP gets the frame's EFFECTIVE content layout —
			// the grid the passes above actually painted (== the mode
			// layout for matched submissions) — not the mode layout.
			uint32_t view_width = c->eff_layout.tile_w;
			uint32_t view_height = c->eff_layout.tile_h;
			uint32_t tile_columns = c->eff_layout.cols;
			uint32_t tile_rows = c->eff_layout.rows;

			// Crop atlas to content dimensions
			uint32_t content_w = tile_columns * view_width;
			uint32_t content_h = tile_rows * view_height;
			ID3D12Resource *dp_resource = d3d12_crop_atlas_for_dp(c, atlas_resource, content_w, content_h);

			// Pass actual shared texture dimensions to the DP. The DP uses
			// canvas offset/size to set a viewport sub-rect within the shared
			// texture for correct interlacing phase alignment.
			D3D12_RESOURCE_DESC st_desc = c->shared_texture->GetDesc();
			uint32_t dp_target_w = static_cast<uint32_t>(st_desc.Width);
			uint32_t dp_target_h = static_cast<uint32_t>(st_desc.Height);

			// Log the first 5 frames AND every canvas-validity flip. The flip
			// log matters: apps that activate zones lazily (e.g. the zones
			// test apps, frame ~10) flip invalid→valid AFTER the first-5
			// window, so a first-5-only log makes the steady-state weave look
			// like the full-frame fallback when it is actually zone-confined
			// (#727 was mis-triaged off exactly that artifact).
			static uint32_t pa_log = 0;
			static int pa_prev_valid = -1;
			const int pa_valid = eff_canvas.valid ? 1 : 0;
			if (pa_log < 5 || pa_valid != pa_prev_valid) {
				U_LOG_W("process_atlas: view=%ux%u tiles=%ux%u dp_target=%ux%u "
				        "canvas=(%d,%d %ux%u)%s",
				        view_width, view_height, tile_columns, tile_rows,
				        dp_target_w, dp_target_h,
				        eff_canvas.valid ? eff_canvas.x : -1,
				        eff_canvas.valid ? eff_canvas.y : -1,
				        eff_canvas.valid ? eff_canvas.w : 0,
				        eff_canvas.valid ? eff_canvas.h : 0,
				        (pa_prev_valid != -1 && pa_valid != pa_prev_valid)
				            ? " [canvas validity CHANGED]" : "");
				pa_log++;
			}
			pa_prev_valid = pa_valid;

			// #491 part 3 — flatten the 2D-under layers PRE-weave (records into
			// the open cmd_list, leaves backdrop_scratch in PSR) and hand the
			// resource to the DP. NULL ⟹ no under-layers (DP clears its backdrop).
			uint32_t bd_w = 0, bd_h = 0;
			ID3D12Resource *bd_res = d3d12_flatten_backdrop_2d(c, dp_target_w, dp_target_h, &bd_w, &bd_h);
			xrt_display_processor_d3d12_set_background_2d(c->display_processor, bd_res, bd_w, bd_h);

			d3d12_phase_debug_dump(c, "process_atlas_shared_tex");

			xrt_display_processor_d3d12_process_atlas(
			    c->display_processor,
			    c->cmd_list,
			    dp_resource,
			    d3d12_bind_dp_atlas_srv(c, dp_resource), // #854: real SRV — sim DP binds it; SR weaver ignores it
			    st_rtv.ptr,
			    c->shared_texture,
			    view_width, view_height,
			    tile_columns, tile_rows,
			    static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
			    dp_target_w, dp_target_h,
			    eff_canvas.valid ? eff_canvas.x : 0,
			    eff_canvas.valid ? eff_canvas.y : 0,
			    eff_canvas.valid ? eff_canvas.w : 0,
			    eff_canvas.valid ? eff_canvas.h : 0);

			// Transition: atlas COMMON→PSR, shared texture RT→COMMON
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			c->cmd_list->ResourceBarrier(2, barriers);

			// #727 dual tap: DXR_WEAVE_TAP=N dumps the shared texture right
			// after the DP weave (pre zone-mask composite) and again after
			// the composite, for the first N DP frames — splits a mono
			// verdict between the weave and the composite. Costly (full
			// GPU flush + readback + PNG per tap); diagnostics only.
			// %TEMP%\dxr_tap_fNNN_{a_postweave,b_postcomposite}.png
			static long tap_total = -1;
			if (tap_total < 0) {
				const char *tv = getenv("DXR_WEAVE_TAP");
				tap_total = (tv != nullptr) ? atol(tv) : 0;
			}
			static long tap_frame = 0;
			const bool tap_this = tap_frame < tap_total;
			const long tap_idx = tap_frame;
			tap_frame++;
			const char *tap_tmp = getenv("TEMP");

			if (tap_this && tap_tmp != nullptr) {
				// Flush the recorded weave so the texture holds its output.
				c->cmd_list->Close();
				ID3D12CommandList *tap_lists[] = {c->cmd_list};
				c->command_queue->ExecuteCommandLists(1, tap_lists);
				gpu_wait_idle(c);
				char tap_path[MAX_PATH];
				snprintf(tap_path, sizeof(tap_path),
				         "%s\\dxr_tap_f%03ld_a_postweave.png", tap_tmp, tap_idx);
				d3d12_capture_backbuffer_to_png(c, c->shared_texture,
				                                D3D12_RESOURCE_STATE_COMMON, tap_path);
				// Re-arm the cmd list for the composite below (the capture
				// helper leaves it closed+executed).
				c->cmd_allocator->Reset();
				c->cmd_list->Reset(c->cmd_allocator, nullptr);
			}

			// #439 / ADR-027: an authored zone mask or Local2D layers
			// composite the 2D/3D regions of the shared texture. dst is in
			// COMMON (just transitioned above); leave it in COMMON after.
			// No-op when this frame carries no zones / Local2D / explicit
			// mask, leaving the woven texture as-is.
			d3d12_composite_zone_mask(
			    c, false, false, c->shared_texture, st_rtv.ptr,
			    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
			    dp_target_w, dp_target_h, &eff_canvas);

			// #727 dual tap, second point: after the composite lands (needs
			// the close/execute/fence below to have run — defer via flag).
			if (tap_this && tap_tmp != nullptr) {
				c->tap_postcomposite_pending = true;
				c->tap_postcomposite_idx = tap_idx;
			}

		} else if (atlas_resource != nullptr) {
			// No DP: raw copy atlas to shared texture (2D mode fallback)
			D3D12_RESOURCE_BARRIER barriers[2] = {};
			barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[0].Transition.pResource = c->shared_texture;
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

			barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[1].Transition.pResource = atlas_resource;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

			c->cmd_list->ResourceBarrier(2, barriers);
			c->cmd_list->CopyResource(c->shared_texture, atlas_resource);

			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			c->cmd_list->ResourceBarrier(2, barriers);
		}

		// Close and execute command list
		c->cmd_list->Close();
		ID3D12CommandList *lists[] = {c->cmd_list};
		c->command_queue->ExecuteCommandLists(1, lists);

		// Signal fence and wait for frame completion
		c->fence_value++;
		c->command_queue->Signal(c->fence, c->fence_value);
		if (c->fence->GetCompletedValue() < c->fence_value) {
			c->fence->SetEventOnCompletion(c->fence_value, c->fence_event);
			WaitForSingleObject(c->fence_event, INFINITE);
		}

		// #727 dual tap, second point: composite is GPU-complete here.
		if (c->tap_postcomposite_pending) {
			c->tap_postcomposite_pending = false;
			const char *tap_tmp2 = getenv("TEMP");
			if (tap_tmp2 != nullptr) {
				char tap_path[MAX_PATH];
				snprintf(tap_path, sizeof(tap_path),
				         "%s\\dxr_tap_f%03ld_b_postcomposite.png", tap_tmp2,
				         c->tap_postcomposite_idx);
				d3d12_capture_backbuffer_to_png(c, c->shared_texture,
				                                D3D12_RESOURCE_STATE_COMMON, tap_path);
			}
		}

		// #224 / ADR-027 P4: sideband-sync this client's zone state with
		// the DP — runs post-fence so the staged wish / sticky mask is
		// GPU-complete when the DP samples it during the call.
		d3d12_sync_zone_mask_to_dp(c);

		return XRT_SUCCESS;
	}

	// Display processor path: the D3D12 weaver renders to whatever render
	// target is bound on the command list. We bind the swapchain back buffer
	// as RT, call weave, then present.
	if (c->display_processor != NULL && c->target != nullptr) {
		static bool dp_logged = false;
		if (!dp_logged) {
			U_LOG_W("D3D12 weaving via display processor (swapchain RT)");
			dp_logged = true;
		}

		// #875 DEPOSIT half: mask resolve + Local2D flatten, recorded into the
		// list that is closed/executed/synced immediately below.
		d3d12_composite_zone_mask(c, /*reuse_mask=*/false, /*prepare_only=*/true, nullptr, 0,
		                          D3D12_RESOURCE_STATE_RENDER_TARGET,
		                          D3D12_RESOURCE_STATE_RENDER_TARGET, tgt_width, tgt_height,
		                          &eff_canvas);

		// Execute atlas copy so the texture is ready for the weaver
		c->cmd_list->Close();
		ID3D12CommandList *copy_lists[] = {c->cmd_list};
		c->command_queue->ExecuteCommandLists(1, copy_lists);
		gpu_wait_idle(c);

		// Give the weaver a fresh command list
		c->cmd_allocator->Reset();
		c->cmd_list->Reset(c->cmd_allocator, nullptr);

		// #542: the DP gets the frame's EFFECTIVE content layout (== the
		// mode layout for matched submissions), not the mode layout.
		uint32_t view_width = c->eff_layout.tile_w;
		uint32_t view_height = c->eff_layout.tile_h;
		// The back buffer this frame actually wove into. Captured by the
		// helper BEFORE the present, because presenting advances the
		// swapchain's current index — the post-present diagnostics below want
		// the image that just went out, not the next one.
		ID3D12Resource *back_buffer = nullptr;
		ID3D12Resource *atlas_resource = zero_copy
		    ? static_cast<ID3D12Resource *>(zc_resource)
		    : static_cast<ID3D12Resource *>(comp_d3d12_renderer_get_atlas_resource(c->renderer));

		if (atlas_resource != nullptr) {
			uint32_t tile_columns = c->eff_layout.cols;
			uint32_t tile_rows = c->eff_layout.rows;

			// #868: publish everything the weave needs so the repaint thread
			// can replay it against fresh eyes. Arm only off the zero-copy
			// path — there the atlas IS the app's swapchain image, which the
			// app reacquires and overwrites, so a replay would race the app
			// for a half-drawn frame.
			c->repaint.tgt_w = tgt_width;
			c->repaint.tgt_h = tgt_height;
			c->repaint.view_w = view_width;
			c->repaint.view_h = view_height;
			c->repaint.cols = tile_columns;
			c->repaint.rows = tile_rows;
			c->repaint.canvas = eff_canvas;
			c->repaint.eye_pos = eye_pos;
			c->repaint.atlas = atlas_resource;
			c->repaint.content_w = tile_columns * view_width;
			c->repaint.content_h = tile_rows * view_height;
			c->repaint.armed = !zero_copy;

			// Atlas barrier, crop, 2D-under flatten, weave, composite, HUD and
			// present all happen in here — one code path, so a repaint is
			// constructed exactly the way the app frame it stands in for was.
			d3d12_dp_weave_and_present(c, false, &back_buffer);
		} else {
			c->repaint.armed = false;

			// No atlas to weave — nothing was bound as a render target, so
			// the back buffer is still in PRESENT. Just flush and present.
			c->cmd_list->Close();
			ID3D12CommandList *weave_lists[] = {c->cmd_list};
			c->command_queue->ExecuteCommandLists(1, weave_lists);
			comp_d3d12_target_present(c->target, 1);
			gpu_wait_idle(c);
		}

		// Post-compose capture (#210) — fully composed atlas as DP saw it.
		// DP path returns early; mirror the fallback path's call site so the
		// capture surface works regardless of which weave path ran.
		d3d12_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_POST_COMPOSE);

		// #672 woven back-buffer capture now lives in d3d12_dp_weave_and_present
		// so it can capture repaints as well as app frames (#868).

		return XRT_SUCCESS;
	}

	// Target path (no display processor, or mono fallback)
	if (c->target != nullptr) {
		uint32_t bb_index = comp_d3d12_target_get_current_index(c->target);
		ID3D12Resource *back_buffer = static_cast<ID3D12Resource *>(
		    comp_d3d12_target_get_back_buffer(c->target, bb_index));

		if (back_buffer != nullptr) {
			static bool fallback_warned = false;
			if (!fallback_warned) {
				U_LOG_W("Display processing not available, using fallback copy (3d=%d)", c->hardware_display_3d);
				fallback_warned = true;
			}

			ID3D12Resource *atlas_resource = static_cast<ID3D12Resource *>(
			    comp_d3d12_renderer_get_atlas_resource(c->renderer));

			if (atlas_resource != nullptr) {
				// Barrier: back buffer PRESENT -> COPY_DEST
				D3D12_RESOURCE_BARRIER barriers[2] = {};
				barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[0].Transition.pResource = back_buffer;
				barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
				barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
				barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

				barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[1].Transition.pResource = atlas_resource;
				barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
				barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

				c->cmd_list->ResourceBarrier(2, barriers);

				c->cmd_list->CopyResource(back_buffer, atlas_resource);

				// Barrier: back to original states
				barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
				barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
				barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

				c->cmd_list->ResourceBarrier(2, barriers);
			}
		}

		// Close and execute command list
		c->cmd_list->Close();
		ID3D12CommandList *lists[] = {c->cmd_list};
		c->command_queue->ExecuteCommandLists(1, lists);

		// Present with VSync
		xret = comp_d3d12_target_present(c->target, 1);

		// Signal WM_PAINT for modal drag loop
		if (c->owns_window && c->own_window != nullptr) {
			comp_d3d11_window_signal_paint_done(c->own_window);
		}

		// Signal fence and wait for frame completion (frame pacing)
		c->fence_value++;
		c->command_queue->Signal(c->fence, c->fence_value);
		if (c->fence->GetCompletedValue() < c->fence_value) {
			c->fence->SetEventOnCompletion(c->fence_value, c->fence_event);
			WaitForSingleObject(c->fence_event, INFINITE);
		}

		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to present");
			return xret;
		}
	}

	// #224 / ADR-027 P4: sideband-sync this client's zone state with the DP
	// — runs after the present path's fence wait so the staged wish /
	// sticky mask is GPU-complete when the DP samples it during the call.
	d3d12_sync_zone_mask_to_dp(c);

	// Post-compose capture (#210) — runs after the existing fence wait so
	// the GPU is idle when we reset the compositor's cmd allocator/list
	// for the readback.
	d3d12_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_POST_COMPOSE);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                              struct xrt_compositor_semaphore *xcsem,
                                              uint64_t value)
{
	return d3d12_compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}


static void
d3d12_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	U_LOG_I("Destroying D3D12 compositor");

	// #868: stop the repaint loop FIRST. It touches the command list, the
	// target and the display processor under c->mutex, so it has to be joined
	// before any of that is torn down — not merely signalled.
	c->repaint_quit.store(true);
	if (c->repaint_thread.joinable()) {
		c->repaint_thread.join();
	}
	if (c->repaint.ticks > 0) {
		U_LOG_W("#868: repaints=%llu ticks=%llu bail{armed=%llu gate=%llu race=%llu}",
		        (unsigned long long)c->repaint.count, (unsigned long long)c->repaint.ticks,
		        (unsigned long long)c->repaint.bail_armed, (unsigned long long)c->repaint.bail_gate,
		        (unsigned long long)c->repaint.bail_race);
	}

	// Uninstall MCP capture hook before the GPU resources go away.
	u_capture_dims_set_provider(NULL, c);
	mcp_capture_uninstall();
	mcp_capture_fini(&c->mcp_capture);

	// Wait for GPU idle
	if (c->fence != nullptr && c->command_queue != nullptr) {
		gpu_wait_idle(c);
	}

	// Destroy DP input crop resource
	if (c->dp_input_resource != nullptr) {
		c->dp_input_resource->Release();
		c->dp_input_resource = nullptr;
	}

	// Destroy display processor
	// #224 P4: withdraw this client's zone contribution from the vendor's
	// union before the DP goes away (clear-on-teardown edge).
	if (c->zone_published && c->display_processor != nullptr) {
		xrt_display_processor_d3d12_clear_local_zone_mask(c->display_processor);
		c->zone_published = false;
	}
	xrt_display_processor_d3d12_destroy(&c->display_processor);

	if (c->dp_srv_heap != nullptr) {
		c->dp_srv_heap->Release();
	}

	if (c->shared_texture_rtv_heap != nullptr) {
		c->shared_texture_rtv_heap->Release();
		c->shared_texture_rtv_heap = nullptr;
	}

	// #439: release the zone-mask scratches + detach any active mask (the
	// oxr handle owns the mask object itself).
	d3d12_release_zone_state(c);

	if (c->shared_texture != nullptr) {
		c->shared_texture->Release();
		c->shared_texture = nullptr;
	}

	if (c->renderer != nullptr) {
		comp_d3d12_renderer_destroy(&c->renderer);
	}

	if (c->target != nullptr) {
		comp_d3d12_target_destroy(&c->target);
	}

	if (c->fence_event != nullptr) {
		CloseHandle(c->fence_event);
	}
	if (c->fence != nullptr) {
		c->fence->Release();
	}
	if (c->cmd_list != nullptr) {
		c->cmd_list->Release();
	}
	if (c->cmd_allocator != nullptr) {
		c->cmd_allocator->Release();
	}

	if (c->command_queue != nullptr) {
		c->command_queue->Release();
	}
	if (c->device != nullptr) {
		c->device->Release();
	}

	// Destroy HUD resources
	if (c->hud != NULL) {
		u_hud_destroy(&c->hud);
	}
	if (c->hud_texture != nullptr) {
		c->hud_texture->Release();
	}
	if (c->hud_upload_buffer != nullptr) {
		c->hud_upload_buffer->Release();
	}

	// Destroy self-created window
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_destroy(&c->own_window);
	}

	delete c;
}

/*
 *
 * Exported functions
 *
 */

extern "C" xrt_result_t
comp_d3d12_compositor_create(struct xrt_device *xdev,
                             void *hwnd,
                             void *shared_texture_handle,
                             void *d3d12_device,
                             void *d3d12_command_queue,
                             void *dp_factory_d3d12,
                             bool transparent_background,
                             int32_t display_screen_left,
                             int32_t display_screen_top,
                             struct xrt_compositor_native **out_xc)
{
	if (d3d12_device == nullptr) {
		U_LOG_E("D3D12 device is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	if (d3d12_command_queue == nullptr) {
		U_LOG_E("D3D12 command queue is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_I("Creating D3D12 native compositor");

	comp_d3d12_compositor *c = new comp_d3d12_compositor();
	memset(&c->base, 0, sizeof(c->base));

	c->xdev = xdev;
	c->own_window = nullptr;
	c->owns_window = false;
	c->hardware_display_3d = true;
	c->last_3d_mode_index = 1;
	c->transparent_background = transparent_background;
	c->hud = NULL;
	c->hud_texture = nullptr;
	c->hud_upload_buffer = nullptr;
	c->hud_upload_pitch = 0;
	c->hud_initialized = false;
	c->last_frame_time_ns = 0;
	c->smoothed_frame_time_ms = 16.67f;

	// Handle window
	c->app_hwnd = nullptr;
	if (shared_texture_handle != nullptr) {
		// Shared texture mode: compositor doesn't own a swapchain.
		// Store app HWND separately for display processor position tracking.
		c->hwnd = nullptr;
		if (hwnd != nullptr) {
			c->app_hwnd = static_cast<HWND>(hwnd);
			U_LOG_I("Shared texture mode with app HWND for position tracking: %p", hwnd);
		} else {
			U_LOG_I("Shared texture mode (offscreen) — no window");
		}
	} else if (hwnd != nullptr) {
		c->hwnd = static_cast<HWND>(hwnd);
		U_LOG_I("Using app-provided window handle: %p", hwnd);
	} else {
		uint32_t win_w = xdev->hmd->screens[0].w_pixels;
		uint32_t win_h = xdev->hmd->screens[0].h_pixels;
		if (win_w == 0 || win_h == 0) {
			win_w = 1920;
			win_h = 1080;
		}
		U_LOG_I("No window handle provided, creating self-owned window (%ux%u)", win_w, win_h);
		xrt_result_t xret = comp_d3d11_window_create(
		    win_w, win_h, display_screen_left, display_screen_top, &c->own_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create self-owned window");
			delete c;
			return xret;
		}
		c->hwnd = static_cast<HWND>(comp_d3d11_window_get_hwnd(c->own_window));
		c->owns_window = true;
		U_LOG_I("Created self-owned window: %p", (void *)c->hwnd);
	}

	// Create HUD overlay for self-owned windows
	if (c->owns_window) {
		u_hud_create(&c->hud, xdev->hmd->screens[0].w_pixels);
	}

	// Get D3D12 device and command queue
	c->device = static_cast<ID3D12Device *>(d3d12_device);
	c->device->AddRef();

	c->command_queue = static_cast<ID3D12CommandQueue *>(d3d12_command_queue);
	c->command_queue->AddRef();

	// Create command allocator and command list
	HRESULT hr = c->device->CreateCommandAllocator(
	    D3D12_COMMAND_LIST_TYPE_DIRECT,
	    __uuidof(ID3D12CommandAllocator),
	    reinterpret_cast<void **>(&c->cmd_allocator));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create command allocator: 0x%08x", hr);
		d3d12_compositor_destroy(&c->base.base);
		return XRT_ERROR_D3D;
	}

	hr = c->device->CreateCommandList(
	    0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->cmd_allocator, nullptr,
	    __uuidof(ID3D12GraphicsCommandList),
	    reinterpret_cast<void **>(&c->cmd_list));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create command list: 0x%08x", hr);
		d3d12_compositor_destroy(&c->base.base);
		return XRT_ERROR_D3D;
	}
	// #747: name the list, not just the resources. The compositor SHARES the
	// app's device, so the debug layer's complaints interleave ours with the
	// app's — and with everything unnamed there is no way to tell whose barrier
	// is at fault. The list name attributes the barrier; the resource name
	// identifies the target. Debug-layer-only metadata.
	c->cmd_list->SetName(L"DXR.compositor_cmd_list");
	// Command list is created in recording state, close it
	c->cmd_list->Close();

	// Create fence
	hr = c->device->CreateFence(
	    0, D3D12_FENCE_FLAG_NONE,
	    __uuidof(ID3D12Fence),
	    reinterpret_cast<void **>(&c->fence));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create fence: 0x%08x", hr);
		d3d12_compositor_destroy(&c->base.base);
		return XRT_ERROR_D3D;
	}
	c->fence_value = 0;
	c->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	// Open shared texture if handle provided
	c->shared_texture = nullptr;
	c->shared_texture_rtv_heap = nullptr;
	c->has_shared_texture = false;
	c->tap_postcomposite_pending = false;
	c->tap_postcomposite_idx = 0;
	if (shared_texture_handle != nullptr) {
		HANDLE st_handle = static_cast<HANDLE>(shared_texture_handle);
		hr = c->device->OpenSharedHandle(
		    st_handle, __uuidof(ID3D12Resource),
		    reinterpret_cast<void **>(&c->shared_texture));
		if (FAILED(hr)) {
			U_LOG_E("Failed to open shared texture handle: 0x%08x", hr);
			d3d12_compositor_destroy(&c->base.base);
			return XRT_ERROR_D3D;
		}
		c->has_shared_texture = true;

		// #747: name it. The D3D12 debug layer identifies resources by name, and
		// with none set every barrier complaint reads "Unnamed ID3D12Resource
		// Object" — which is why the id-527 spam could be seen but not
		// attributed. Names are debug-layer-only metadata (ignored without it).
		c->shared_texture->SetName(L"DXR.app_shared_texture");

		// Query shared texture dimensions
		D3D12_RESOURCE_DESC st_desc = c->shared_texture->GetDesc();
		U_LOG_W("Opened shared texture handle: %p -> resource %p (%llux%llu)",
		        shared_texture_handle, (void *)c->shared_texture,
		        (unsigned long long)st_desc.Width, (unsigned long long)st_desc.Height);

		// Create RTV for shared texture so the display processor can weave into it
		D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
		rtv_heap_desc.NumDescriptors = 1;
		rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hr = c->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&c->shared_texture_rtv_heap));
		if (FAILED(hr)) {
			U_LOG_E("Failed to create shared texture RTV heap: 0x%08x", hr);
			d3d12_compositor_destroy(&c->base.base);
			return XRT_ERROR_D3D;
		}
		D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
		rtv_desc.Format = st_desc.Format;
		rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		c->device->CreateRenderTargetView(c->shared_texture, &rtv_desc,
		    c->shared_texture_rtv_heap->GetCPUDescriptorHandleForHeapStart());
		U_LOG_I("Created RTV for shared texture (weaver target)");
	}

	// Initialize settings
	memset(&c->settings, 0, sizeof(c->settings));
	c->settings.preferred.width = xdev->hmd->screens[0].w_pixels;
	c->settings.preferred.height = xdev->hmd->screens[0].h_pixels;
	if (c->settings.preferred.width == 0 || c->settings.preferred.height == 0) {
		c->settings.preferred.width = 1920;
		c->settings.preferred.height = 1080;
	}
	c->settings.nominal_frame_interval_ns = xdev->hmd->screens[0].nominal_frame_interval_ns;
	if (c->settings.nominal_frame_interval_ns == 0) {
		c->settings.nominal_frame_interval_ns = (1000 * 1000 * 1000) / 60;
	}

	// Get actual dimensions — from window or shared texture
	if (c->has_shared_texture && c->shared_texture != nullptr) {
		D3D12_RESOURCE_DESC st_desc = c->shared_texture->GetDesc();
		c->settings.preferred.width = static_cast<uint32_t>(st_desc.Width);
		c->settings.preferred.height = static_cast<uint32_t>(st_desc.Height);
	} else if (c->hwnd != nullptr) {
		RECT rect;
		if (GetClientRect(c->hwnd, &rect)) {
			c->settings.preferred.width = rect.right - rect.left;
			c->settings.preferred.height = rect.bottom - rect.top;
		}
	}

	// Create output target (DXGI swapchain).
	// The D3D12 weaver renders to whatever render target is bound on the
	// command list — it does NOT create its own swapchain. So we always
	// need a swapchain when we have a window, even with a display processor.
	// Skip only for shared texture offscreen mode (no window to present to).
	xrt_result_t xret;
	if (c->has_shared_texture) {
		c->target = nullptr;
		U_LOG_I("Skipping DXGI swapchain (shared texture mode — compositor renders to shared texture)");
	} else if (c->hwnd != nullptr) {
		xret = comp_d3d12_target_create(c, c->hwnd,
		                                              c->settings.preferred.width,
		                                              c->settings.preferred.height,
		                                              transparent_background,
		                                              &c->target);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create D3D12 target");
			d3d12_compositor_destroy(&c->base.base);
			return xret;
		}
		if (comp_d3d12_target_has_child_window(c->target)) {
			U_LOG_I("D3D12 target using child window fallback (parent HWND: %p)", (void *)c->hwnd);
		}
	} else {
		c->target = nullptr;
		U_LOG_I("No window — skipping DXGI swapchain");
	}

	// Current mode of the monitor this session's window lives on. Hardcoding
	// 60 Hz here handed the display processor a frame period 2.75× too long
	// on a 165 Hz panel — and this is the Unity/Unreal path.
	c->display_refresh_rate = 60.0f;
	{
		HWND rate_hwnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
		const float hz = comp_display_refresh_hz_win(rate_hwnd);
		if (hz > 0.0f) {
			c->display_refresh_rate = hz;
		}
	}
	U_LOG_W("Display refresh rate: %.2f Hz (frame period %.2f ms)", c->display_refresh_rate,
	        1000.0 / c->display_refresh_rate);
	if (c->target != nullptr) {
		comp_d3d12_target_set_display_period(
		    c->target, (uint64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate));
	}

	// #868: start the repaint loop. It arms itself off the first non-zero-copy
	// DP frame and is inert until then, so starting it here (before the display
	// processor exists) is safe.
	{
		const char *e = getenv("DXR_WEAVE_REPAINT");
		c->repaint.enabled = (e != nullptr && e[0] == '0') ? 0 : 1;
		const char *fe = getenv("DXR_WEAVE_REPAINT_FORCE");
		c->repaint.force = (fe != nullptr && fe[0] == '1') ? 1 : 0;
		if (c->repaint.force == 1) {
			U_LOG_W("#868: DXR_WEAVE_REPAINT_FORCE=1 — repainting every refresh regardless of app "
			        "rate. This is a correctness probe and WILL cost frame rate.");
		}
		if (c->repaint.enabled == 1 && c->target != nullptr) {
			c->repaint_quit.store(false);
			c->repaint_thread = std::thread(d3d12_repaint_thread, c);
		} else if (c->repaint.enabled == 0) {
			U_LOG_W("#868: weave repaint disabled (DXR_WEAVE_REPAINT=0)");
		}
	}

	// Determine view dimensions
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

	// Create display processor via factory
	if (dp_factory_d3d12 != NULL) {
		auto factory = (xrt_dp_factory_d3d12_fn_t)dp_factory_d3d12;
		HWND dp_hwnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
		xrt_result_t dp_ret = factory(c->device, c->command_queue, dp_hwnd, &c->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("D3D12 display processor factory failed (error %d), continuing without", (int)dp_ret);
			c->display_processor = nullptr;
		} else {
			U_LOG_W("D3D12 display processor created via factory");

			// Tell the weaver the output render target format so it can
			// create its internal pipeline state. Without this, the weaver's
			// pipeline state stays null and weave() silently no-ops.
			// Use the shared texture format when available (texture apps),
			// otherwise fall back to the swapchain format (handle apps).
			DXGI_FORMAT output_fmt = c->has_shared_texture
			    ? c->shared_texture->GetDesc().Format
			    : DXGI_FORMAT_R8G8B8A8_UNORM;
			xrt_display_processor_d3d12_set_output_format(
			    c->display_processor,
			    output_fmt);
			U_LOG_W("D3D12 display processor: output format set to %u (target=%p)",
			        (unsigned)output_fmt, (void *)c->target);

			// Forward session-level transparency (#573 — chroma-key-free).
			// client_presents=false — DELIBERATELY; #904's true was reverted
			// after a hardware eyeball. The de-occlusion band (partial-alpha
			// parallax fringe) cannot come from DWM blending: the weaver
			// destroys per-pixel alpha and the gate reconstructs only the
			// binary all-views-transparent mask, so the band is either the
			// DP's ~1-frame bake (the product spec) or BLACK. WGC cost is
			// attacked via capture throttling, not by dropping the band.
			// See comp_d3d11_compositor.cpp for the full rationale.
			xrt_display_processor_d3d12_set_transparent_background(
			    c->display_processor, transparent_background, false);

			// #68: tell the DP whether the app self-presents only the canvas
			// (texture app) vs the runtime presenting the full target (handle).
			// Used + zones state to skip the compose-under-bg desktop-UV remap.
			xrt_display_processor_d3d12_set_shared_texture_present(
			    c->display_processor, c->has_shared_texture);
		}
	} else {
		U_LOG_W("No D3D12 display processor factory provided");
	}

	// If display processor is available, query display pixel info to compute
	// optimal view dimensions (scaled to window size, matching D3D11 model).
	// Do NOT resize the app's window — _ext apps own their window.
	if (c->display_processor != nullptr) {
		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (xrt_display_processor_d3d12_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h, &disp_left, &disp_top) &&
		    disp_px_w > 0 && disp_px_h > 0) {
			// Use half display width as base view dims
			uint32_t base_vw = disp_px_w / 2;
			uint32_t base_vh = disp_px_h;

			U_LOG_W("Display pixel info: %ux%u, base view dims: %ux%u per eye",
			        disp_px_w, disp_px_h, base_vw, base_vh);

			// Scale by window/display pixel ratio (same as D3D11 resize path)
			float ratio = fminf(
			    (float)c->settings.preferred.width / (float)disp_px_w,
			    (float)c->settings.preferred.height / (float)disp_px_h);
			if (ratio > 1.0f) {
				ratio = 1.0f;
			}
			view_width = (uint32_t)((float)base_vw * ratio);
			view_height = (uint32_t)((float)base_vh * ratio);
			U_LOG_W("Scaled to window ratio %.3f: %ux%u per eye", ratio, view_width, view_height);
		}
	}

	// Create SRV descriptor heap for display processor (shader-visible, reuses renderer's SRV)
	{
		D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
		heap_desc.NumDescriptors = 1;
		heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		hr = c->device->CreateDescriptorHeap(
		    &heap_desc, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&c->dp_srv_heap));
		if (FAILED(hr)) {
			U_LOG_W("Failed to create DP SRV heap: 0x%08x", hr);
		}
	}

	// Create renderer — when a DP is present, atlas height must match view height
	// so the DP's UV 0..1 maps exactly to content. The per-frame resize path
	// (resize_target_h above) must apply the same guard.
	uint32_t target_height = (c->display_processor != NULL) ? view_height : c->settings.preferred.height;
	xret = comp_d3d12_renderer_create(c, view_width, view_height, target_height, &c->renderer);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create D3D12 renderer");
		d3d12_compositor_destroy(&c->base.base);
		return xret;
	}

	// Expose current window-scaled capture dims to xrCaptureAtlasDXR (#431).
	u_capture_dims_set_provider(d3d12_compositor_capture_dims_provider, c);

	// Initialize layer accumulator
	memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Populate supported swapchain formats
	uint32_t format_count = 0;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R8G8B8A8_UNORM;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_B8G8R8A8_UNORM;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R16G16B16A16_UNORM;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_D24_UNORM_S8_UINT;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_D32_FLOAT;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_D16_UNORM;
	c->base.base.info.format_count = format_count;

	c->base.base.info.initial_visible = true;
	c->base.base.info.initial_focused = true;

	// Set up compositor interface
	c->base.base.get_swapchain_create_properties = d3d12_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = d3d12_compositor_create_swapchain;
	c->base.base.import_swapchain = d3d12_compositor_import_swapchain;
	c->base.base.import_fence = d3d12_compositor_import_fence;
	c->base.base.create_semaphore = d3d12_compositor_create_semaphore;
	c->base.base.begin_session = d3d12_compositor_begin_session;
	c->base.base.end_session = d3d12_compositor_end_session;
	c->base.base.wait_frame = d3d12_compositor_wait_frame;
	c->base.base.predict_frame = d3d12_compositor_predict_frame;
	c->base.base.mark_frame = d3d12_compositor_mark_frame;
	c->base.base.begin_frame = d3d12_compositor_begin_frame;
	c->base.base.discard_frame = d3d12_compositor_discard_frame;
	c->base.base.layer_begin = d3d12_compositor_layer_begin;
	c->base.base.layer_projection = d3d12_compositor_layer_projection;
	c->base.base.layer_projection_depth = d3d12_compositor_layer_projection_depth;
	c->base.base.layer_quad = d3d12_compositor_layer_quad;
	c->base.base.layer_cube = d3d12_compositor_layer_cube;
	c->base.base.layer_cylinder = d3d12_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = d3d12_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = d3d12_compositor_layer_equirect2;
	c->base.base.layer_passthrough = d3d12_compositor_layer_passthrough;
	c->base.base.layer_window_space = d3d12_compositor_layer_window_space;
	c->base.base.layer_local_2d = d3d12_compositor_layer_local_2d;
	c->base.base.layer_zone_3d = d3d12_compositor_layer_zone_3d;
	c->base.base.layer_commit = d3d12_compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = d3d12_compositor_layer_commit_with_semaphore;
	c->base.base.destroy = d3d12_compositor_destroy;

	// Install MCP capture_frame hook + arm the trigger-file path (#210).
	mcp_capture_init(&c->mcp_capture);
	mcp_capture_install(&c->mcp_capture);

	*out_xc = &c->base;

	U_LOG_IFL_I(U_LOGGING_INFO, "D3D12 native compositor created successfully (%ux%u)",
	            c->settings.preferred.width, c->settings.preferred.height);

	return XRT_SUCCESS;
}

extern "C" bool
comp_d3d12_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_eye_positions *out_eye_pos)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	if (c->display_processor != nullptr) {
		if (xrt_display_processor_d3d12_get_predicted_eye_positions(c->display_processor, out_eye_pos) &&
		    out_eye_pos->valid) {
			return true;
		}
	}

	return false;
}

extern "C" bool
comp_d3d12_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                              float *out_width_m,
                                              float *out_height_m)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	return xrt_display_processor_d3d12_get_display_dimensions(
	    c->display_processor, out_width_m, out_height_m);
}

extern "C" bool
comp_d3d12_compositor_get_window_metrics(struct xrt_compositor *xc,
                                          struct xrt_window_metrics *out_metrics)
{
	if (xc == nullptr || out_metrics == nullptr) {
		if (out_metrics != nullptr) {
			out_metrics->valid = false;
		}
		return false;
	}

	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	// Prefer a DP-provided window metrics implementation if one exists.
	bool ok = xrt_display_processor_d3d12_get_window_metrics(c->display_processor, out_metrics);
	if (!ok) {
		// No DP implementation (the in-tree sim_display DP and the Leia
		// plug-in delegate window placement to the runtime). Compute the
		// metrics directly from the HWND — same construction as the d3d11
		// native compositor. Without this, d3d12 handle/texture sessions
		// had NO window metrics and the runtime-side Kooima (rig path, raw
		// channel, legacy-2D fovs) ran display-scoped, so window-relative
		// 3D and the rig's rotation pivot were wrong (#396 W7 dogfood).
		memset(out_metrics, 0, sizeof(*out_metrics));

		// Shared-texture (texture-app) sessions carry the app's window in
		// app_hwnd (c->hwnd stays null); their metrics come from that window.
		HWND metrics_hwnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
		if (c->display_processor == nullptr || metrics_hwnd == nullptr) {
			return false;
		}

		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (!xrt_display_processor_d3d12_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h, &disp_left, &disp_top)) {
			return false;
		}
		if (disp_px_w == 0 || disp_px_h == 0) {
			return false;
		}

		float disp_w_m = 0.0f, disp_h_m = 0.0f;
		if (!xrt_display_processor_d3d12_get_display_dimensions(
		        c->display_processor, &disp_w_m, &disp_h_m)) {
			return false;
		}

		RECT rect;
		if (!GetClientRect(metrics_hwnd, &rect)) {
			return false;
		}
		uint32_t win_px_w = static_cast<uint32_t>(rect.right - rect.left);
		uint32_t win_px_h = static_cast<uint32_t>(rect.bottom - rect.top);
		if (win_px_w == 0 || win_px_h == 0) {
			return false;
		}

		POINT client_origin = {0, 0};
		ClientToScreen(metrics_hwnd, &client_origin);

		float pixel_size_x = disp_w_m / (float)disp_px_w;
		float pixel_size_y = disp_h_m / (float)disp_px_h;

		float win_w_m = (float)win_px_w * pixel_size_x;
		float win_h_m = (float)win_px_h * pixel_size_y;

		float win_center_px_x = (float)(client_origin.x - disp_left) + (float)win_px_w / 2.0f;
		float win_center_px_y = (float)(client_origin.y - disp_top) + (float)win_px_h / 2.0f;
		float disp_center_px_x = (float)disp_px_w / 2.0f;
		float disp_center_px_y = (float)disp_px_h / 2.0f;

		// X: +right (screen and eye coords agree). Y: negated (screen
		// Y-down, eye Y-up).
		float offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
		float offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

		out_metrics->display_width_m = disp_w_m;
		out_metrics->display_height_m = disp_h_m;
		out_metrics->display_pixel_width = disp_px_w;
		out_metrics->display_pixel_height = disp_px_h;
		out_metrics->display_screen_left = disp_left;
		out_metrics->display_screen_top = disp_top;

		out_metrics->window_pixel_width = win_px_w;
		out_metrics->window_pixel_height = win_px_h;
		out_metrics->window_screen_left = static_cast<int32_t>(client_origin.x);
		out_metrics->window_screen_top = static_cast<int32_t>(client_origin.y);

		out_metrics->window_width_m = win_w_m;
		out_metrics->window_height_m = win_h_m;
		out_metrics->window_center_offset_x_m = offset_x_m;
		out_metrics->window_center_offset_y_m = offset_y_m;

		out_metrics->valid = true;
	}

	return true;
}

extern "C" bool
comp_d3d12_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	// Ensure GPU is fully idle before switching display mode.
	// The SR SDK's lens_hint enable/disable may interact with the D3D12
	// device internally. If the GPU has pending work (e.g. DXGI Present
	// scan-out), this can cause DXGI_ERROR_DEVICE_REMOVED on some GPUs
	// (observed on Intel Iris Xe with hosted D3D12 apps).
	gpu_wait_idle(c);

	return xrt_display_processor_d3d12_request_display_mode(c->display_processor, enable_3d);
}

extern "C" void
comp_d3d12_compositor_set_eye_tracking_mode(struct xrt_compositor *xc, uint32_t mode)
{
	if (xc == nullptr) {
		return;
	}

	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d12_set_eye_tracking_mode(c->display_processor, mode);
	}
}

extern "C" void
comp_d3d12_compositor_set_system_devices(struct xrt_compositor *xc,
                                          struct xrt_system_devices *xsysd)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	c->xsysd = xsysd;

	// Pass xsysd to self-owned window for direct qwerty input (WASD, TAB HUD, V mode toggle)
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_set_system_devices(c->own_window, xsysd);
	}
}

void
comp_d3d12_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc,
                                                   bool legacy,
                                                   float scale_x,
                                                   float scale_y,
                                                   uint32_t view_w,
                                                   uint32_t view_h)
{
	if (xc == nullptr) {
		return;
	}
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	c->legacy_app_tile_scaling = legacy;
	c->legacy_view_scale_x = scale_x;
	c->legacy_view_scale_y = scale_y;
	if (c->renderer != nullptr) {
		comp_d3d12_renderer_set_legacy_app_tile_scaling(c->renderer, legacy);
	}

	// Fix view dims at the actual recommended size the app was told to render at.
	if (legacy && c->renderer != nullptr && view_w > 0 && view_h > 0) {
		uint32_t target_h = (c->display_processor != nullptr) ? view_h : c->settings.preferred.height;
		comp_d3d12_renderer_resize(c->renderer, view_w, view_h, target_h);
	}
}

/*
 *
 * XR_DXR_local_3d_zone — authored 2D/3D mask consumer (#439 cross-API leg).
 *
 * Port of the D3D11 Phase 1+2 consumer (comp_d3d11_compositor.cpp). The oxr
 * handlers (oxr_local_3d_zone.c) forward here. The mask selects an
 * arbitrary scalar 2D/3D region: the
 * masked-composite shader's use_rect_mask = 0 path lerps
 * M·weave + (1−M)·twod per pixel. Authoring happens on the app's thread,
 * consumption inside d3d12_compositor_layer_commit — both serialize on
 * c->mutex (the entry points lock it; layer_commit already holds it), which
 * also makes submit atomic against an in-flight frame (spec §9 Q3).
 *
 * D3D12 specifics vs the D3D11 reference:
 *  - No immediate context: each authoring op re-arms c->cmd_allocator /
 *    c->cmd_list (Reset → record → Close → Execute → gpu_wait_idle) under
 *    c->mutex — the same pattern d3d12_compositor_capture_atlas_to_png uses;
 *    the list is provably idle whenever an entry point holds the mutex
 *    (every layer_commit exit closes + executes + fence-waits).
 *  - Tier 3 hands the app the ID3D12Resource* (descriptor heaps are
 *    app-owned); the resource is in RENDER_TARGET state and must be returned
 *    to RENDER_TARGET before xrSubmitLocal3DZoneDXR. Same device AND queue
 *    (in-process), so submission order is the sync — no fence.
 *  - Tier 2 uses ClearRenderTargetView's native rect array (one call).
 *
 * #464: the mask + 2D layer are window-sized (client-window pixels, matching
 * XrLocal3DZoneMaskCreateInfoDXR); the composite operates on the window rect
 * at the top-left anchor of the worst-case surface, never beyond it.
 *
 */

/*!
 * Compositor-side state for one authored zone mask. Owned by the oxr handle
 * (oxr_local_3d_zone_ext::comp_mask); the compositor only borrows the
 * pointer in active_zone_mask while the mask is submitted.
 */
struct comp_d3d12_zone_mask
{
	//! Authoring texture: R8_UNORM, M in [0,1] (1 = 3D / keep the weave).
	//! Steady state RENDER_TARGET (clears need it; Tier-3 contract returns it).
	ID3D12Resource *tex;
	//! 1-descriptor RTV heap for tex — used for Tier 1/2 fills (Tier 3 apps
	//! create their own RTV on the returned resource).
	ID3D12DescriptorHeap *rtv_heap;
	//! Staged snapshot sampled by the composite (decouples in-progress
	//! authoring from the frame; refreshed by zone_mask_submit). Steady
	//! state PIXEL_SHADER_RESOURCE.
	ID3D12Resource *staged;
	//! Mask dimensions in client-window pixels.
	uint32_t w, h;
	//! True once submitted at least once (an unsubmitted mask is invisible).
	bool submitted;
};

// #224 / ADR-027 hardware-DP zone leg (P4) — one-time DP zone-capability
// probe, cached on the compositor (caller holds c->mutex). Returns true when
// the DP consumes published zone masks; caps are then in c->zone_dp_caps.
static bool
d3d12_zone_dp_supported(struct comp_d3d12_compositor *c)
{
	if (c->display_processor == nullptr) {
		return false;
	}
	if (c->zone_dp_state == 0) { // 0 = unqueried, 1 = supported, 2 = legacy
		struct xrt_dp_local_zone_caps caps = {};
		caps.struct_size = sizeof(caps);
		bool ok = xrt_display_processor_d3d12_get_local_zone_caps(c->display_processor, &caps);
		c->zone_dp_state = (ok && caps.supported != 0) ? 1 : 2;
		if (c->zone_dp_state == 1) {
			c->zone_dp_caps = caps;
			U_LOG_W("D3D12 zone DP: local zones supported, grid %ux%u max_mask %ux%u max_hz %u "
			        "wish_fractional=%u granularity=%u",
			        caps.zone_grid_width, caps.zone_grid_height, caps.max_mask_width,
			        caps.max_mask_height, caps.max_update_hz, caps.wish_fractional,
			        caps.switch_granularity);
		}
	}
	return c->zone_dp_state == 1;
}

// Keep the DP's view of this client's zone mask in sync with the
// compositor's — the D3D12 clone of d3d11_sync_zone_mask_to_dp. Called once
// per layer_commit AFTER the path's ExecuteCommandLists + fence wait, so
// whatever staged resource we hand over is GPU-complete and in its steady
// PIXEL_SHADER_RESOURCE state (the publish contract). Zones frame: the WISH
// this frame's composite resolved (explicit staged or the auto raster);
// legacy frame: the sticky submitted mask. No resolvable source drives the
// clear-on-deactivate edge, once. Caller holds c->mutex.
static void
d3d12_sync_zone_mask_to_dp(struct comp_d3d12_compositor *c)
{
	if (!d3d12_zone_dp_supported(c)) {
		return; // legacy DP — tier-1 global fallback path unchanged.
	}

	ID3D12Resource *res = nullptr;
	uint32_t mask_w = 0;
	uint32_t mask_h = 0;
	if (c->zones_frame) {
		res = c->zone_publish_res;
		mask_w = c->zone_publish_w;
		mask_h = c->zone_publish_h;
	} else {
		struct comp_d3d12_zone_mask *mask = c->active_zone_mask;
		if (mask != nullptr && mask->submitted && mask->staged != nullptr) {
			res = mask->staged;
			mask_w = mask->w;
			mask_h = mask->h;
		}
	}

	if (res == nullptr) {
		if (c->zone_published) {
			xrt_display_processor_d3d12_clear_local_zone_mask(c->display_processor);
			c->zone_published = false;
		}
		return;
	}

	// Screen-anchor the mask: client-area origin in physical screen pixels.
	// No HWND (pure offscreen) → nothing to anchor to; skip the publish.
	HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	RECT r;
	POINT origin = {0, 0};
	if (wnd == nullptr || !GetClientRect(wnd, &r) || r.right <= 0 || r.bottom <= 0 ||
	    !ClientToScreen(wnd, &origin)) {
		return;
	}

	bool ok = xrt_display_processor_d3d12_publish_local_zone_mask(c->display_processor, res, mask_w, mask_h,
	                                                              (int32_t)origin.x, (int32_t)origin.y,
	                                                              (uint32_t)r.right, (uint32_t)r.bottom,
	                                                              c->zone_publish_seq);
	if (ok) {
		c->zone_published = true;
	}

	d3d12_phase_debug_dump(c, "publish_zone_mask");
}

// Release the compositor-owned zone consumables (scratches) and detach any
// active mask (the oxr handle owns the mask object itself). Idempotent;
// called from d3d12_compositor_destroy only.
static void
d3d12_release_zone_state(struct comp_d3d12_compositor *c)
{
	c->active_zone_mask = nullptr;
	// XR_DXR_display_zones: drop the frame-wish borrow + frame state
	// (+ the P4 publish-source borrow and seq-dedup cache).
	c->frame_wish = nullptr;
	c->zone_frame_wish_last = nullptr;
	c->zone_publish_res = nullptr;
	c->zones_frame = false;
	if (c->weave_scratch != nullptr) {
		c->weave_scratch->Release();
		c->weave_scratch = nullptr;
	}
	// #439 Phase 3 — Local2D consumer scratches + implicit mask.
	if (c->local2d_scratch != nullptr) {
		c->local2d_scratch->Release();
		c->local2d_scratch = nullptr;
	}
	if (c->local2d_scratch_rtv_heap != nullptr) {
		c->local2d_scratch_rtv_heap->Release();
		c->local2d_scratch_rtv_heap = nullptr;
	}
	c->local2d_scratch_w = 0;
	c->local2d_scratch_h = 0;
	// #491 part 3 — 2D-under backdrop scratch.
	if (c->backdrop_scratch != nullptr) {
		c->backdrop_scratch->Release();
		c->backdrop_scratch = nullptr;
	}
	if (c->backdrop_scratch_rtv_heap != nullptr) {
		c->backdrop_scratch_rtv_heap->Release();
		c->backdrop_scratch_rtv_heap = nullptr;
	}
	c->backdrop_scratch_w = 0;
	c->backdrop_scratch_h = 0;
	if (c->implicit_mask_staged != nullptr) {
		c->implicit_mask_staged->Release();
		c->implicit_mask_staged = nullptr;
	}
	if (c->implicit_mask_rtv_heap != nullptr) {
		c->implicit_mask_rtv_heap->Release();
		c->implicit_mask_rtv_heap = nullptr;
	}
	if (c->implicit_mask_tex != nullptr) {
		c->implicit_mask_tex->Release();
		c->implicit_mask_tex = nullptr;
	}
	c->implicit_mask_w = 0;
	c->implicit_mask_h = 0;
	c->implicit_rect_count = 0;
	// #800/#803 — per-zone opt-in feather composite mask.
	if (c->feather_mask_staged != nullptr) {
		c->feather_mask_staged->Release();
		c->feather_mask_staged = nullptr;
	}
	if (c->feather_mask_rtv_heap != nullptr) {
		c->feather_mask_rtv_heap->Release();
		c->feather_mask_rtv_heap = nullptr;
	}
	if (c->feather_mask_tex != nullptr) {
		c->feather_mask_tex->Release();
		c->feather_mask_tex = nullptr;
	}
	c->feather_mask_w = 0;
	c->feather_mask_h = 0;
}

// (Re)allocate a DEFAULT-heap committed scratch texture at the given
// dims/format (no-op when it already matches). D3D12 textures are SRV-able
// without bind flags; created in COMMON (the steady state between frames).
// Returns false on allocation failure (with *res released and nulled).
static bool
d3d12_ensure_scratch(struct comp_d3d12_compositor *c,
                     ID3D12Resource **res,
                     uint32_t w,
                     uint32_t h,
                     DXGI_FORMAT fmt,
                     const char *what)
{
	bool need_alloc = *res == nullptr;
	if (!need_alloc) {
		D3D12_RESOURCE_DESC cur = (*res)->GetDesc();
		need_alloc = (cur.Width != w || cur.Height != h || cur.Format != fmt);
	}
	if (!need_alloc) {
		return true;
	}
	if (*res != nullptr) {
		(*res)->Release();
		*res = nullptr;
	}

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = w;
	desc.Height = h;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	HRESULT hr = c->device->CreateCommittedResource(
	    &heap, D3D12_HEAP_FLAG_NONE, &desc,
	    D3D12_RESOURCE_STATE_COMMON, nullptr,
	    IID_PPV_ARGS(res));
	if (FAILED(hr) || *res == nullptr) {
		U_LOG_W("%s: scratch alloc (%ux%u fmt=%u) failed: 0x%08x", what, w, h, fmt, hr);
		*res = nullptr;
		return false;
	}
	return true;
}

// Re-arm the compositor's command list for a zone-authoring op. Caller holds
// c->mutex; the list is closed + the GPU idle whenever that's true (see the
// section comment), so the allocator Reset is safe.
static void
d3d12_zone_cmd_begin(struct comp_d3d12_compositor *c)
{
	c->cmd_allocator->Reset();
	c->cmd_list->Reset(c->cmd_allocator, nullptr);
}

// Close + execute the zone-authoring command list and wait for completion,
// restoring the "closed list, idle GPU" invariant before the mutex releases.
// The CPU wait also makes zone_mask_submit's staged copy atomic against the
// next frame (spec §9 Q3).
static void
d3d12_zone_cmd_execute(struct comp_d3d12_compositor *c)
{
	c->cmd_list->Close();
	ID3D12CommandList *lists[] = {c->cmd_list};
	c->command_queue->ExecuteCommandLists(1, lists);
	gpu_wait_idle(c);
}

// #439 Phase 3 — (re)allocate the dedicated Local2D flatten scratch
// (R8G8B8A8_UNORM, ALLOW_RENDER_TARGET, steady COMMON) + its RTV heap.
// Returns false on allocation failure.
static bool
d3d12_ensure_local2d_scratch(struct comp_d3d12_compositor *c, uint32_t w, uint32_t h)
{
	if (c->local2d_scratch != nullptr && c->local2d_scratch_w == w && c->local2d_scratch_h == h) {
		return true;
	}
	if (c->local2d_scratch != nullptr) {
		c->local2d_scratch->Release();
		c->local2d_scratch = nullptr;
	}
	c->local2d_scratch_w = 0;
	c->local2d_scratch_h = 0;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = w;
	desc.Height = h;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // transparent (flatten clears to 0,0,0,0)

	HRESULT hr = c->device->CreateCommittedResource(
	    &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(&c->local2d_scratch));
	if (FAILED(hr) || c->local2d_scratch == nullptr) {
		U_LOG_W("local2d scratch alloc (%ux%u) failed: 0x%08x", w, h, hr);
		c->local2d_scratch = nullptr;
		return false;
	}
	c->local2d_scratch->SetName(L"DXR.local2d_scratch"); // #747: debug-layer attribution

	if (c->local2d_scratch_rtv_heap == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
		rtv_desc.NumDescriptors = 1;
		rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hr = c->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&c->local2d_scratch_rtv_heap));
		if (FAILED(hr) || c->local2d_scratch_rtv_heap == nullptr) {
			U_LOG_W("local2d scratch RTV heap failed: 0x%08x", hr);
			c->local2d_scratch->Release();
			c->local2d_scratch = nullptr;
			return false;
		}
	}
	c->device->CreateRenderTargetView(c->local2d_scratch, nullptr,
	                                  c->local2d_scratch_rtv_heap->GetCPUDescriptorHandleForHeapStart());
	c->local2d_scratch_w = w;
	c->local2d_scratch_h = h;
	return true;
}

// #439 Phase 3 — (re)rasterize the runtime-owned IMPLICIT zone mask from this
// frame's Local2D layer rects. Inverse of zone_mask_set_rects: M=1 (keep the
// weave / 3D) everywhere, then M=0 (show the flattened 2D) inside each layer
// rect. The masked-composite shader lerps M*weave + (1-M)*twod, so M=0 in the
// rects is what surfaces the 2D content there. Records the raster + staging
// copy into the OPEN c->cmd_list (mid-frame). Re-rasters
// only when the rect set or dims change (steady-state frames reuse the staged
// snapshot). Returns the staged R8 resource (sampled by the composite) or
// nullptr on failure. Caller holds c->mutex.
static ID3D12Resource *
d3d12_update_implicit_mask(struct comp_d3d12_compositor *c,
                           const struct xrt_rect *rects,
                           uint32_t rect_count,
                           uint32_t w,
                           uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return nullptr;
	}

	bool dirty = c->implicit_mask_tex == nullptr || c->implicit_mask_staged == nullptr ||
	             c->implicit_mask_w != w || c->implicit_mask_h != h || c->implicit_rect_count != rect_count;
	for (uint32_t i = 0; !dirty && i < rect_count; i++) {
		if (memcmp(&c->implicit_rects[i], &rects[i], sizeof(rects[i])) != 0) {
			dirty = true;
		}
	}
	if (!dirty) {
		return c->implicit_mask_staged; // steady PSR, reuse
	}

	// (Re)allocate the R8 RT + staged copy on dims change (mirrors
	// zone_mask_create — tex steady RENDER_TARGET, staged steady PSR).
	if (c->implicit_mask_tex == nullptr || c->implicit_mask_w != w || c->implicit_mask_h != h) {
		if (c->implicit_mask_staged != nullptr) {
			c->implicit_mask_staged->Release();
			c->implicit_mask_staged = nullptr;
		}
		if (c->implicit_mask_rtv_heap != nullptr) {
			c->implicit_mask_rtv_heap->Release();
			c->implicit_mask_rtv_heap = nullptr;
		}
		if (c->implicit_mask_tex != nullptr) {
			c->implicit_mask_tex->Release();
			c->implicit_mask_tex = nullptr;
		}
		c->implicit_mask_w = 0;
		c->implicit_mask_h = 0;

		D3D12_RESOURCE_DESC td = {};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = w;
		td.Height = h;
		td.DepthOrArraySize = 1;
		td.MipLevels = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_CLEAR_VALUE clear = {};
		clear.Format = DXGI_FORMAT_R8_UNORM;
		clear.Color[0] = 1.0f;

		HRESULT hr = c->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
		                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
		                                                IID_PPV_ARGS(&c->implicit_mask_tex));
	if (c->implicit_mask_tex != nullptr) c->implicit_mask_tex->SetName(L"DXR.implicit_mask_tex"); // #747 attribution
		if (SUCCEEDED(hr) && c->implicit_mask_tex != nullptr) {
			D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
			rtv_desc.NumDescriptors = 1;
			rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			hr = c->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&c->implicit_mask_rtv_heap));
		}
		if (SUCCEEDED(hr) && c->implicit_mask_rtv_heap != nullptr) {
			c->device->CreateRenderTargetView(c->implicit_mask_tex, nullptr,
			                                  c->implicit_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart());
			td.Flags = D3D12_RESOURCE_FLAG_NONE;
			hr = c->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
			                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
			                                        IID_PPV_ARGS(&c->implicit_mask_staged));
	if (c->implicit_mask_staged != nullptr) c->implicit_mask_staged->SetName(L"DXR.implicit_mask_staged"); // #747 attribution
		}
		if (FAILED(hr) || c->implicit_mask_staged == nullptr) {
			U_LOG_E("implicit zone mask: D3D12 resource creation failed: 0x%08x", hr);
			if (c->implicit_mask_rtv_heap != nullptr) {
				c->implicit_mask_rtv_heap->Release();
				c->implicit_mask_rtv_heap = nullptr;
			}
			if (c->implicit_mask_tex != nullptr) {
				c->implicit_mask_tex->Release();
				c->implicit_mask_tex = nullptr;
			}
			return nullptr;
		}
		c->implicit_mask_w = w;
		c->implicit_mask_h = h;
	}

	// Clamp the layer rects → D3D12_RECT (client-window px); skip degenerate
	// / fully-outside ones.
	D3D12_RECT drs[XRT_MAX_LAYERS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < rect_count && n < XRT_MAX_LAYERS; i++) {
		int32_t left = rects[i].offset.w;
		int32_t top = rects[i].offset.h;
		int32_t right = left + rects[i].extent.w;
		int32_t bottom = top + rects[i].extent.h;
		if (left < 0) {
			left = 0;
		}
		if (top < 0) {
			top = 0;
		}
		if (right > (int32_t)w) {
			right = (int32_t)w;
		}
		if (bottom > (int32_t)h) {
			bottom = (int32_t)h;
		}
		if (right <= left || bottom <= top) {
			continue;
		}
		drs[n].left = left;
		drs[n].top = top;
		drs[n].right = right;
		drs[n].bottom = bottom;
		n++;
	}

	// Raster onto the open cmd-list: M=1 everywhere, then M=0 inside the rects
	// (tex is in its steady RENDER_TARGET state). D3D12's ClearRenderTargetView
	// takes the rect array natively (one call vs D3D11's per-rect ClearView).
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = c->implicit_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart();
	const float all_3d[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(rtv, all_3d, 0, nullptr);
	if (n > 0) {
		const float all_2d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		c->cmd_list->ClearRenderTargetView(rtv, all_2d, n, drs);
	}

	// Stage the snapshot the composite samples (RT≠SRV; decouple as the
	// explicit mask's submit does). Leaves tex in RENDER_TARGET, staged in PSR.
	D3D12_RESOURCE_BARRIER to_copy[2] = {};
	to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[0].Transition.pResource = c->implicit_mask_tex;
	to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[1].Transition.pResource = c->implicit_mask_staged;
	to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, to_copy);

	c->cmd_list->CopyResource(c->implicit_mask_staged, c->implicit_mask_tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	c->cmd_list->ResourceBarrier(2, to_copy);

	memcpy(c->implicit_rects, rects, sizeof(rects[0]) * rect_count);
	c->implicit_rect_count = rect_count;

	// One-off-ish lifecycle event (fires only on a rect/dims change).
	U_LOG_W("implicit zone mask: %ux%u, %u Local2D rect(s)", w, h, rect_count);
	return c->implicit_mask_staged;
}

// XR_DXR_display_zones (ADR-027) — (re)rasterize the AUTO wish: union of the
// frame's zone rects, BINARY (#800/#801 — the wish is HARDWARE-only and
// hard-edged by default; the old implicit 16px ring feather leaked cosmetic
// fractional M into the published wish and vignetted the composite at window
// edges). M=1 inside every zone rect, 0 outside. The staged resource is the
// MODE_ZONES composite's weave gate and the published wish when no explicit
// wish is staged; cosmetic feather (XrDisplayZoneFeatherDXR, #803) rasters
// into its own resources and never enters this one. Reuses the
// implicit-mask R8 resources (the implicit rule is inert in zones frames) and
// re-rasters every zones frame, VK-style — a handful of rect clears — while
// invalidating the implicit rect cache so a later legacy frame re-rasters.
// Records onto the OPEN c->cmd_list. Caller holds c->mutex. Returns the
// staged R8 resource (steady PIXEL_SHADER_RESOURCE) or nullptr on failure.
static ID3D12Resource *
d3d12_update_zone_wish_mask(struct comp_d3d12_compositor *c,
                            const struct xrt_rect *rects,
                            uint32_t rect_count,
                            uint32_t w,
                            uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return nullptr;
	}

	// (Re)allocate the R8 RT + staged copy on dims change (same block as
	// d3d12_update_implicit_mask — tex steady RENDER_TARGET, staged steady
	// PIXEL_SHADER_RESOURCE).
	if (c->implicit_mask_tex == nullptr || c->implicit_mask_staged == nullptr || c->implicit_mask_w != w ||
	    c->implicit_mask_h != h) {
		if (c->implicit_mask_staged != nullptr) {
			c->implicit_mask_staged->Release();
			c->implicit_mask_staged = nullptr;
		}
		if (c->implicit_mask_rtv_heap != nullptr) {
			c->implicit_mask_rtv_heap->Release();
			c->implicit_mask_rtv_heap = nullptr;
		}
		if (c->implicit_mask_tex != nullptr) {
			c->implicit_mask_tex->Release();
			c->implicit_mask_tex = nullptr;
		}
		c->implicit_mask_w = 0;
		c->implicit_mask_h = 0;

		D3D12_RESOURCE_DESC td = {};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = w;
		td.Height = h;
		td.DepthOrArraySize = 1;
		td.MipLevels = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_CLEAR_VALUE clear = {};
		clear.Format = DXGI_FORMAT_R8_UNORM;
		clear.Color[0] = 1.0f;

		HRESULT hr = c->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
		                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
		                                                IID_PPV_ARGS(&c->implicit_mask_tex));
	if (c->implicit_mask_tex != nullptr) c->implicit_mask_tex->SetName(L"DXR.implicit_mask_tex"); // #747 attribution
		if (SUCCEEDED(hr) && c->implicit_mask_tex != nullptr) {
			D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
			rtv_desc.NumDescriptors = 1;
			rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			hr = c->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&c->implicit_mask_rtv_heap));
		}
		if (SUCCEEDED(hr) && c->implicit_mask_rtv_heap != nullptr) {
			c->device->CreateRenderTargetView(c->implicit_mask_tex, nullptr,
			                                  c->implicit_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart());
			td.Flags = D3D12_RESOURCE_FLAG_NONE;
			hr = c->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
			                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
			                                        IID_PPV_ARGS(&c->implicit_mask_staged));
	if (c->implicit_mask_staged != nullptr) c->implicit_mask_staged->SetName(L"DXR.implicit_mask_staged"); // #747 attribution
		}
		if (FAILED(hr) || c->implicit_mask_staged == nullptr) {
			U_LOG_E("zone wish mask: D3D12 resource creation failed: 0x%08x", hr);
			if (c->implicit_mask_rtv_heap != nullptr) {
				c->implicit_mask_rtv_heap->Release();
				c->implicit_mask_rtv_heap = nullptr;
			}
			if (c->implicit_mask_tex != nullptr) {
				c->implicit_mask_tex->Release();
				c->implicit_mask_tex = nullptr;
			}
			return nullptr;
		}
		c->implicit_mask_w = w;
		c->implicit_mask_h = h;
	}

	// The wish raster replaces whatever the implicit rule cached.
	c->implicit_rect_count = 0;

	// BINARY raster: M=0 everywhere, then one rect-array clear at 1.0 over
	// every zone rect (D3D12's ClearRenderTargetView takes the rect array
	// natively).
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = c->implicit_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart();
	const float all_off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(rtv, all_off, 0, nullptr);
	{
		const float all_on[4] = {1.0f, 0.0f, 0.0f, 0.0f};
		D3D12_RECT drs[XRT_MAX_LAYERS];
		uint32_t n = 0;
		for (uint32_t i = 0; i < rect_count && n < XRT_MAX_LAYERS; i++) {
			int32_t left = rects[i].offset.w;
			int32_t top = rects[i].offset.h;
			int32_t right = rects[i].offset.w + rects[i].extent.w;
			int32_t bottom = rects[i].offset.h + rects[i].extent.h;
			if (left < 0) {
				left = 0;
			}
			if (top < 0) {
				top = 0;
			}
			if (right > (int32_t)w) {
				right = (int32_t)w;
			}
			if (bottom > (int32_t)h) {
				bottom = (int32_t)h;
			}
			if (right <= left || bottom <= top) {
				continue;
			}
			drs[n].left = left;
			drs[n].top = top;
			drs[n].right = right;
			drs[n].bottom = bottom;
			n++;
		}
		if (n > 0) {
			c->cmd_list->ClearRenderTargetView(rtv, all_on, n, drs);
		}
	}

	// Stage the snapshot the composite samples (same barrier dance as the
	// implicit raster — leaves tex in RENDER_TARGET, staged in PSR).
	D3D12_RESOURCE_BARRIER to_copy[2] = {};
	to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[0].Transition.pResource = c->implicit_mask_tex;
	to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[1].Transition.pResource = c->implicit_mask_staged;
	to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, to_copy);

	c->cmd_list->CopyResource(c->implicit_mask_staged, c->implicit_mask_tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	c->cmd_list->ResourceBarrier(2, to_copy);

	static bool wish_logged = false;
	if (!wish_logged) {
		wish_logged = true;
		U_LOG_W("zone wish mask (auto): %ux%u, %u zone rect(s), binary", w, h, rect_count);
	}
	return c->implicit_mask_staged;
}

// XR_DXR_display_zones (#800/#803) — (re)rasterize the zones COMPOSITE mask
// with PER-ZONE opt-in feather (XrDisplayZoneFeatherDXR) into its own R8
// resources. Clear M=0, then each zone draws hard (one clear at 1.0,
// feather_px[i] <= 0 — the default) or with its own inward 0->1 ramp over
// feather_px[i] window pixels (the rings idiom: ascending value WITH
// ascending inset, 2px steps, ramp-width cap 64px then wider steps; small
// zones clamp the inset so the center still reaches 1). Per zone so radii
// can differ. Only called when a frame's zones request feather — all-hard
// frames sample the binary raster instead, and the published wish stays
// binary regardless (cosmetics never enter the wish). Re-rasters every
// feathered zones frame (VK-style); records onto the OPEN c->cmd_list.
// Caller holds c->mutex. Returns the staged R8 resource (steady
// PIXEL_SHADER_RESOURCE) or nullptr on failure (caller falls back to the
// binary mask — hard edges, never a lost frame).
static ID3D12Resource *
d3d12_update_zone_feather_mask(struct comp_d3d12_compositor *c,
                               const struct xrt_rect *rects,
                               const float *feather_px,
                               uint32_t rect_count,
                               uint32_t w,
                               uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return nullptr;
	}

	// (Re)allocate the R8 RT + staged copy on dims change (same block as the
	// wish raster — tex steady RENDER_TARGET, staged steady
	// PIXEL_SHADER_RESOURCE).
	if (c->feather_mask_tex == nullptr || c->feather_mask_staged == nullptr || c->feather_mask_w != w ||
	    c->feather_mask_h != h) {
		if (c->feather_mask_staged != nullptr) {
			c->feather_mask_staged->Release();
			c->feather_mask_staged = nullptr;
		}
		if (c->feather_mask_rtv_heap != nullptr) {
			c->feather_mask_rtv_heap->Release();
			c->feather_mask_rtv_heap = nullptr;
		}
		if (c->feather_mask_tex != nullptr) {
			c->feather_mask_tex->Release();
			c->feather_mask_tex = nullptr;
		}
		c->feather_mask_w = 0;
		c->feather_mask_h = 0;

		D3D12_RESOURCE_DESC td = {};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = w;
		td.Height = h;
		td.DepthOrArraySize = 1;
		td.MipLevels = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_CLEAR_VALUE clear = {};
		clear.Format = DXGI_FORMAT_R8_UNORM;
		clear.Color[0] = 1.0f;

		HRESULT hr = c->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
		                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
		                                                IID_PPV_ARGS(&c->feather_mask_tex));
		if (c->feather_mask_tex != nullptr) c->feather_mask_tex->SetName(L"DXR.feather_mask_tex"); // #747 attribution
		if (SUCCEEDED(hr) && c->feather_mask_tex != nullptr) {
			D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
			rtv_desc.NumDescriptors = 1;
			rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			hr = c->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&c->feather_mask_rtv_heap));
		}
		if (SUCCEEDED(hr) && c->feather_mask_rtv_heap != nullptr) {
			c->device->CreateRenderTargetView(c->feather_mask_tex, nullptr,
			                                  c->feather_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart());
			td.Flags = D3D12_RESOURCE_FLAG_NONE;
			hr = c->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
			                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
			                                        IID_PPV_ARGS(&c->feather_mask_staged));
			if (c->feather_mask_staged != nullptr) c->feather_mask_staged->SetName(L"DXR.feather_mask_staged"); // #747 attribution
		}
		if (FAILED(hr) || c->feather_mask_staged == nullptr) {
			U_LOG_E("zone feather mask: D3D12 resource creation failed: 0x%08x", hr);
			if (c->feather_mask_rtv_heap != nullptr) {
				c->feather_mask_rtv_heap->Release();
				c->feather_mask_rtv_heap = nullptr;
			}
			if (c->feather_mask_tex != nullptr) {
				c->feather_mask_tex->Release();
				c->feather_mask_tex = nullptr;
			}
			return nullptr;
		}
		c->feather_mask_w = w;
		c->feather_mask_h = h;
	}

	// Per-zone raster: a hard zone is one full-rect clear at 1.0; a feathered
	// zone ramps 0->1 over its OWN radius via the rings idiom — ascending
	// value WITH ascending inset, later (deeper, higher-value) clears
	// overwriting the inner part of earlier ones so the edge keeps the low
	// values and the core reaches 1.
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = c->feather_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart();
	const float all_off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(rtv, all_off, 0, nullptr);
	for (uint32_t i = 0; i < rect_count; i++) {
		const float radius = feather_px[i];
		const bool feathered = radius > 0.0f;
		int32_t steps = 1;
		int32_t step_px = 0;
		if (feathered) {
			step_px = 2;
			steps = (int32_t)(radius / (float)step_px + 0.5f);
			if (steps < 1) {
				steps = 1;
			}
			if (steps > 32) { // beyond a 64px ramp, widen the step instead
				step_px = (int32_t)(radius / 32.0f + 0.5f);
				steps = 32;
			}
		}
		for (int32_t s = 1; s <= steps; s++) {
			const float v = (float)s / (float)steps; // 1.0 for the hard single step
			int32_t min_ext = rects[i].extent.w < rects[i].extent.h ? rects[i].extent.w
			                                                        : rects[i].extent.h;
			int32_t max_inset = (min_ext - 1) / 2;
			if (max_inset < 0) {
				max_inset = 0;
			}
			int32_t inset = feathered ? s * step_px : 0;
			if (inset > max_inset) {
				inset = max_inset;
			}
			int32_t left = rects[i].offset.w + inset;
			int32_t top = rects[i].offset.h + inset;
			int32_t right = rects[i].offset.w + rects[i].extent.w - inset;
			int32_t bottom = rects[i].offset.h + rects[i].extent.h - inset;
			if (left < 0) {
				left = 0;
			}
			if (top < 0) {
				top = 0;
			}
			if (right > (int32_t)w) {
				right = (int32_t)w;
			}
			if (bottom > (int32_t)h) {
				bottom = (int32_t)h;
			}
			if (right <= left || bottom <= top) {
				continue;
			}
			const float val[4] = {v, 0.0f, 0.0f, 0.0f};
			D3D12_RECT dr = {left, top, right, bottom};
			c->cmd_list->ClearRenderTargetView(rtv, val, 1, &dr);
		}
	}

	// Stage the snapshot the composite samples (same barrier dance as the
	// wish raster — leaves tex in RENDER_TARGET, staged in PSR).
	D3D12_RESOURCE_BARRIER to_copy[2] = {};
	to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[0].Transition.pResource = c->feather_mask_tex;
	to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[1].Transition.pResource = c->feather_mask_staged;
	to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, to_copy);

	c->cmd_list->CopyResource(c->feather_mask_staged, c->feather_mask_tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	c->cmd_list->ResourceBarrier(2, to_copy);

	static bool feather_logged = false;
	if (!feather_logged) {
		feather_logged = true;
		U_LOG_W("zone feather mask: %ux%u, %u zone rect(s) (composite-only, wish stays binary)", w, h,
		        rect_count);
	}
	return c->feather_mask_staged;
}

// Resolve the zones frame's wish/composite state (called from the composite,
// mid-recording; caller holds c->mutex). The BINARY auto raster is ALWAYS
// maintained and returned — it is the MODE_ZONES composite's weave gate
// (#801: an explicit wish is PUBLISH-ONLY, never a compositor blend gate)
// and doubles as the published wish when no explicit wish is staged.
// Explicit frame wish: stage the authoring texture in-cmd
// (referenced-at-frame-end = consume current state — no
// xrSubmitLocal3DZoneDXR required), mirroring zone_mask_submit's body, and
// route it to the publish (zone_publish_res) only.
// Returns the staged BINARY R8 resource (the composite mask) or nullptr.
static ID3D12Resource *
d3d12_update_zone_wish_state(struct comp_d3d12_compositor *c, uint32_t region_w, uint32_t region_h)
{
	// Binary auto raster first — the composite gate regardless of the
	// publish source.
	struct xrt_rect rects[XRT_MAX_LAYERS];
	uint32_t rect_count = 0;
	for (uint32_t i = 0; i < c->layer_accum.layer_count && rect_count < XRT_MAX_LAYERS; i++) {
		if (c->layer_accum.layers[i].data.type != XRT_LAYER_ZONE_3D) {
			continue;
		}
		rects[rect_count++] = c->layer_accum.layers[i].data.zone_3d.rect;
	}
	ID3D12Resource *staged = d3d12_update_zone_wish_mask(c, rects, rect_count, region_w, region_h);

	if (c->frame_wish != nullptr && c->frame_wish->tex != nullptr && c->frame_wish->staged != nullptr) {
		struct comp_d3d12_zone_mask *fw = c->frame_wish;

		// tex steady RENDER_TARGET, staged steady PIXEL_SHADER_RESOURCE
		// (see comp_d3d12_zone_mask) — same dance as zone_mask_submit,
		// recorded into the open frame cmd list.
		D3D12_RESOURCE_BARRIER to_copy[2] = {};
		to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		to_copy[0].Transition.pResource = fw->tex;
		to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		to_copy[1].Transition.pResource = fw->staged;
		to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		c->cmd_list->ResourceBarrier(2, to_copy);

		c->cmd_list->CopyResource(fw->staged, fw->tex);

		std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
		std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
		c->cmd_list->ResourceBarrier(2, to_copy);

		// P4 publish source + seq: the staged explicit wish. Bump the
		// generation on a source change (pointer flip; D3D12 masks carry
		// no author generation, so a same-pointer re-author keeps its seq
		// — vendors treat same-seq as anchor-only updates).
		c->zone_publish_res = fw->staged;
		c->zone_publish_w = fw->w;
		c->zone_publish_h = fw->h;
		if (c->zone_frame_wish_last != fw) {
			c->zone_frame_wish_last = fw;
			c->zone_publish_seq++;
		}
		// NOTE: the composite mask stays the binary raster — the explicit
		// wish publishes verbatim but never gates blending (#801).
		return staged;
	}

	if (staged != nullptr) {
		// P4 publish source + seq: the auto raster. It re-records every
		// zones frame, but identical rect set + dims = identical content —
		// bump the generation only when something actually changed (or the
		// source flipped explicit -> auto).
		bool wish_dirty = c->zone_frame_wish_last != nullptr || c->zone_wish_rect_count != rect_count ||
		                  c->zone_publish_w != region_w || c->zone_publish_h != region_h;
		for (uint32_t i = 0; !wish_dirty && i < rect_count; i++) {
			if (memcmp(&c->zone_wish_rects[i], &rects[i], sizeof(rects[i])) != 0) {
				wish_dirty = true;
			}
		}
		if (wish_dirty) {
			c->zone_frame_wish_last = nullptr;
			memcpy(c->zone_wish_rects, rects, sizeof(rects[0]) * rect_count);
			c->zone_wish_rect_count = rect_count;
			c->zone_publish_seq++;
		}
		c->zone_publish_res = staged;
		c->zone_publish_w = region_w;
		c->zone_publish_h = region_h;
	}
	return staged;
}

// #439 Phase 3 — flatten this frame's Local2D layers into local2d_scratch (the
// `twod` source the masked composite reads). Records into the OPEN c->cmd_list:
// transition the scratch to RENDER_TARGET, clear transparent, draw each layer
// in list order (later = on top) with premultiplied (or straight) "over", then
// transition to PIXEL_SHADER_RESOURCE. Dest rects clip to the window region;
// the clip fractions, the layer's norm_rect, and flip_y are carried into the
// source UVs (matches d3d11_flatten_local_2d_layers). Caller holds c->mutex and
// has ensured local2d_scratch at (region_w, region_h). Returns false on error.
static bool
d3d12_flatten_local_2d_layers(struct comp_d3d12_compositor *c, uint32_t region_w, uint32_t region_h, int32_t proj_idx)
{
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = c->local2d_scratch_rtv_heap->GetCPUDescriptorHandleForHeapStart();

	// Scratch COMMON → RENDER_TARGET, clear transparent. Where a pixel is M=0
	// (2D) but no layer covers it, twod stays (0,0,0,0) → final.a → 0 → the
	// desktop shows through (the §4.2 output-alpha rule).
	D3D12_RESOURCE_BARRIER to_rt = {};
	to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_rt.Transition.pResource = c->local2d_scratch;
	to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(1, &to_rt);

	const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(rtv, transparent, 0, nullptr);

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		// #491 part 3 — under-layers (before the projection) are the DP backdrop.
		if (proj_idx >= 0 && (int32_t)i < proj_idx) {
			continue;
		}
		struct xrt_swapchain *sc = layer->sc_array[0];
		if (sc == nullptr) {
			continue;
		}
		uint32_t img_idx = layer->data.local_2d.sub.image_index;
		ID3D12Resource *src_res = static_cast<ID3D12Resource *>(comp_d3d12_swapchain_get_resource(sc, img_idx));
		if (src_res == nullptr) {
			continue;
		}

		// Dest rect (client-window px), clipped to the window region.
		const struct xrt_rect *dr = &layer->data.local_2d.rect;
		int32_t dx = dr->offset.w;
		int32_t dy = dr->offset.h;
		int32_t dw = dr->extent.w;
		int32_t dh = dr->extent.h;
		if (dw <= 0 || dh <= 0) {
			continue;
		}
		int32_t x0 = dx < 0 ? 0 : dx;
		int32_t y0 = dy < 0 ? 0 : dy;
		int32_t x1 = (dx + dw) > (int32_t)region_w ? (int32_t)region_w : (dx + dw);
		int32_t y1 = (dy + dh) > (int32_t)region_h ? (int32_t)region_h : (dy + dh);
		if (x1 <= x0 || y1 <= y0) {
			continue;
		}

		// Clip fractions within the original dest rect (carry into the UVs).
		float fx0 = (float)(x0 - dx) / (float)dw;
		float fy0 = (float)(y0 - dy) / (float)dh;
		float fx1 = (float)(x1 - dx) / (float)dw;
		float fy1 = (float)(y1 - dy) / (float)dh;

		// App sub-rect within the swapchain image (normalized). Default full.
		struct xrt_normalized_rect nr = layer->data.local_2d.sub.norm_rect;
		if (nr.w <= 0.0f || nr.h <= 0.0f) {
			nr.x = 0.0f;
			nr.y = 0.0f;
			nr.w = 1.0f;
			nr.h = 1.0f;
		}

		float src_x = nr.x + nr.w * fx0;
		float src_w = nr.w * (fx1 - fx0);
		float src_y, src_h;
		if (layer->data.flip_y) {
			src_y = nr.y + nr.h * (1.0f - fy0);
			src_h = -(nr.h * (fy1 - fy0));
		} else {
			src_y = nr.y + nr.h * fy0;
			src_h = nr.h * (fy1 - fy0);
		}

		bool unpremult = (layer->data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0;

		// #491 part 3 — use the layer's accum index as the flatten descriptor
		// slot (unique across the pre-weave backdrop + this post-weave overlay,
		// which share flatten_srv_heap within the one deferred cmd list).
		comp_d3d12_renderer_flatten_local_2d(c->renderer, c->cmd_list, rtv.ptr, src_res, i, x0, y0,
		                                     (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), src_x, src_y, src_w,
		                                     src_h, unpremult);
	}

	// Scratch → sampleable for the masked composite.
	D3D12_RESOURCE_BARRIER to_psr = {};
	to_psr.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_psr.Transition.pResource = c->local2d_scratch;
	to_psr.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_psr.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_psr.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(1, &to_psr);
	return true;
}

// #491 part 3 — ensure the 2D-under backdrop scratch (clone of
// d3d12_ensure_local2d_scratch; separate so a model switch can't dangle it).
static bool
d3d12_ensure_backdrop_scratch(struct comp_d3d12_compositor *c, uint32_t w, uint32_t h)
{
	if (c->backdrop_scratch != nullptr && c->backdrop_scratch_w == w && c->backdrop_scratch_h == h) {
		return true;
	}
	if (c->backdrop_scratch != nullptr) {
		c->backdrop_scratch->Release();
		c->backdrop_scratch = nullptr;
	}
	c->backdrop_scratch_w = 0;
	c->backdrop_scratch_h = 0;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = w;
	desc.Height = h;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	HRESULT hr = c->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
	                                                D3D12_RESOURCE_STATE_COMMON, &clear,
	                                                IID_PPV_ARGS(&c->backdrop_scratch));
	if (c->backdrop_scratch != nullptr) c->backdrop_scratch->SetName(L"DXR.backdrop_scratch"); // #747 attribution
	if (FAILED(hr) || c->backdrop_scratch == nullptr) {
		U_LOG_W("backdrop scratch alloc (%ux%u) failed: 0x%08x", w, h, hr);
		c->backdrop_scratch = nullptr;
		return false;
	}

	if (c->backdrop_scratch_rtv_heap == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
		rtv_desc.NumDescriptors = 1;
		rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hr = c->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&c->backdrop_scratch_rtv_heap));
		if (FAILED(hr) || c->backdrop_scratch_rtv_heap == nullptr) {
			U_LOG_W("backdrop scratch RTV heap failed: 0x%08x", hr);
			c->backdrop_scratch->Release();
			c->backdrop_scratch = nullptr;
			return false;
		}
	}
	c->device->CreateRenderTargetView(c->backdrop_scratch, nullptr,
	                                  c->backdrop_scratch_rtv_heap->GetCPUDescriptorHandleForHeapStart());
	c->backdrop_scratch_w = w;
	c->backdrop_scratch_h = h;
	return true;
}

// #491 part 3 — flatten this frame's 2D-UNDER Local2D layers (before the
// projection in list order) into backdrop_scratch PRE-weave and return the
// ID3D12Resource* (+ region dims) for set_background_2d (the DP creates its own
// shader-visible SRV; the compose then puts `backdrop over captured-desktop`
// under the 3D). Returns nullptr (out dims 0) when there are no under-layers.
// Records into the OPEN c->cmd_list; leaves backdrop_scratch in
// PIXEL_SHADER_RESOURCE (DP-sampleable, outlives process_atlas). Caller holds
// c->mutex. Uses each layer's accum index as the flatten slot (unique vs the
// post-weave overlay flatten that shares flatten_srv_heap in this cmd list).
static ID3D12Resource *
d3d12_flatten_backdrop_2d(struct comp_d3d12_compositor *c, uint32_t dst_w, uint32_t dst_h, uint32_t *out_w,
                          uint32_t *out_h)
{
	*out_w = 0;
	*out_h = 0;
	if (!c->local_2d_last_frame || c->renderer == nullptr) {
		return nullptr;
	}

	int32_t proj_idx = -1;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
		if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
			proj_idx = (int32_t)i;
			break;
		}
	}
	if (proj_idx < 0) {
		return nullptr;
	}
	bool have_under = false;
	for (int32_t i = 0; i < proj_idx; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			have_under = true;
			break;
		}
	}
	if (!have_under) {
		return nullptr;
	}

	uint32_t region_w = dst_w;
	uint32_t region_h = dst_h;
	HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	if (wnd != nullptr) {
		RECT r;
		if (GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
			region_w = ((uint32_t)r.right < dst_w) ? (uint32_t)r.right : dst_w;
			region_h = ((uint32_t)r.bottom < dst_h) ? (uint32_t)r.bottom : dst_h;
		}
	}
	if (region_w == 0 || region_h == 0) {
		return nullptr;
	}

	if (!d3d12_ensure_backdrop_scratch(c, region_w, region_h)) {
		return nullptr;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = c->backdrop_scratch_rtv_heap->GetCPUDescriptorHandleForHeapStart();

	D3D12_RESOURCE_BARRIER to_rt = {};
	to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_rt.Transition.pResource = c->backdrop_scratch;
	to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(1, &to_rt);

	const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(rtv, transparent, 0, nullptr);

	for (int32_t i = 0; i < proj_idx; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		struct xrt_swapchain *sc = layer->sc_array[0];
		if (sc == nullptr) {
			continue;
		}
		uint32_t img_idx = layer->data.local_2d.sub.image_index;
		ID3D12Resource *src_res = static_cast<ID3D12Resource *>(comp_d3d12_swapchain_get_resource(sc, img_idx));
		if (src_res == nullptr) {
			continue;
		}
		const struct xrt_rect *dr = &layer->data.local_2d.rect;
		int32_t dx = dr->offset.w, dy = dr->offset.h, dw = dr->extent.w, dh = dr->extent.h;
		if (dw <= 0 || dh <= 0) {
			continue;
		}
		int32_t x0 = dx < 0 ? 0 : dx;
		int32_t y0 = dy < 0 ? 0 : dy;
		int32_t x1 = (dx + dw) > (int32_t)region_w ? (int32_t)region_w : (dx + dw);
		int32_t y1 = (dy + dh) > (int32_t)region_h ? (int32_t)region_h : (dy + dh);
		if (x1 <= x0 || y1 <= y0) {
			continue;
		}
		float fx0 = (float)(x0 - dx) / (float)dw;
		float fy0 = (float)(y0 - dy) / (float)dh;
		float fx1 = (float)(x1 - dx) / (float)dw;
		float fy1 = (float)(y1 - dy) / (float)dh;
		struct xrt_normalized_rect nr = layer->data.local_2d.sub.norm_rect;
		if (nr.w <= 0.0f || nr.h <= 0.0f) {
			nr.x = 0.0f;
			nr.y = 0.0f;
			nr.w = 1.0f;
			nr.h = 1.0f;
		}
		float src_x = nr.x + nr.w * fx0;
		float src_w = nr.w * (fx1 - fx0);
		float src_y, src_h;
		if (layer->data.flip_y) {
			src_y = nr.y + nr.h * (1.0f - fy0);
			src_h = -(nr.h * (fy1 - fy0));
		} else {
			src_y = nr.y + nr.h * fy0;
			src_h = nr.h * (fy1 - fy0);
		}
		bool unpremult = (layer->data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0;
		comp_d3d12_renderer_flatten_local_2d(c->renderer, c->cmd_list, rtv.ptr, src_res, i, x0, y0,
		                                     (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), src_x, src_y, src_w,
		                                     src_h, unpremult);
	}

	D3D12_RESOURCE_BARRIER to_psr = {};
	to_psr.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_psr.Transition.pResource = c->backdrop_scratch;
	to_psr.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_psr.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_psr.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(1, &to_psr);

	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("D3D12 #491 part3: flattened 2D-under backdrop %ux%u (handed to DP set_background_2d)",
		        region_w, region_h);
	}

	*out_w = region_w;
	*out_h = region_h;
	return c->backdrop_scratch;
}

// #439 — composite the authored zone mask. Records into the OPEN c->cmd_list
// (both call sites are mid-recording in layer_commit). The mask-lerp writes
// every window pixel. Returns false (no-op) when this frame carries no
// zones / Local2D / explicit mask.
//
// dst_pre_state/dst_post_state parameterize the weave target's states
// (COMMON/COMMON on the shared-texture path, RENDER_TARGET/RENDER_TARGET on
// the window-DP path).
//
// #464 window clamping: all inputs are window-sized; the pass writes only the
// window region at the top-left anchor of the (worst-case-allocated) dst.
//
// #439 Phase 2: eff_canvas is the caller's per-frame effective canvas
// (d3d12_effective_canvas under c->mutex) — the window rect while the mask
// is active, so the composite region and the weave region share one
// authority.
/*!
 * #875 diag: name every exit path out of the zone/Local2D composite.
 *
 * Two structural attempts at the deposit/render split both lost the 2D, and both
 * times I reasoned about the cause and was wrong. The symptom — 2D absent,
 * everything else perfect — is what an early-out looks like, so make the function
 * say which guard it left through instead of guessing.
 */
#define ZC_BAIL(reason)                                                                                                	do {                                                                                                           		static bool _zc_logged = false;                                                                        		if (!_zc_logged) {                                                                                     			_zc_logged = true;                                                                             			U_LOG_W("#875 composite bail[%s] reuse=%d prepare=%d", reason, (int)reuse_mask,                			        (int)prepare_only);                                                                    		}                                                                                                      		return false;                                                                                          	} while (0)

static bool
d3d12_composite_zone_mask(struct comp_d3d12_compositor *c,
                          bool reuse_mask,
                          bool prepare_only,
                          ID3D12Resource *dst,
                          uint64_t dst_rtv,
                          D3D12_RESOURCE_STATES dst_pre_state,
                          D3D12_RESOURCE_STATES dst_post_state,
                          uint32_t dst_w,
                          uint32_t dst_h,
                          const struct u_canvas_rect *eff_canvas)
{
	// #439 Phase 3: run when EITHER an explicit submitted mask exists OR this
	// frame carries Local2D layers (the layers supply both the 2D pixels and
	// an implicit mask). Mirrors the D3D11 leg.
	// XR_DXR_display_zones: a zones frame ALWAYS runs the composite (the
	// MODE_ZONES pass gates the weave by the binary zone raster — pixels
	// outside every zone go to the 2D flatten / transparent even with zero
	// Local2D layers); the sticky mask + implicit-mask rules are inert.
	struct comp_d3d12_zone_mask *mask = c->active_zone_mask;
	const bool zones_frame = c->zones_frame;
	const bool have_explicit = !zones_frame && (mask != nullptr && mask->submitted);
	const bool have_local_2d = c->local_2d_last_frame;
	if (!prepare_only && (dst == nullptr || dst_rtv == 0)) {
		ZC_BAIL("dst");
	}
	if (c->renderer == nullptr) {
		ZC_BAIL("renderer");
	}
	// #868: a repaint composites from the mask the last app frame resolved, so
	// the per-frame predicates below do not apply to it — only the presence of
	// that cached mask does.
	if (!reuse_mask && !zones_frame && !have_explicit && !have_local_2d) {
		ZC_BAIL("g1");
	}
	if (reuse_mask && c->repaint.mask_res == nullptr) {
		ZC_BAIL("g2");
	}

	// The window region inside the worst-case surface (#464). No HWND →
	// the dst is the window-sized target already.
	uint32_t region_w = dst_w;
	uint32_t region_h = dst_h;
	HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	if (wnd != nullptr) {
		RECT r;
		if (GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
			region_w = ((uint32_t)r.right < dst_w) ? (uint32_t)r.right : dst_w;
			region_h = ((uint32_t)r.bottom < dst_h) ? (uint32_t)r.bottom : dst_h;
		}
	}

	// Validate the weave-target format up front (both paths): the composite
	// has PSOs only for RGBA8/BGRA8 UNORM (the lerp is channel-agnostic —
	// app shared textures are BGRA8 in the wild, DXGI targets RGBA8).
	D3D12_RESOURCE_DESC dd = prepare_only ? D3D12_RESOURCE_DESC{} : dst->GetDesc();
	if (!prepare_only && dd.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
	    dd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
		static bool dfmt_logged = false;
		if (!dfmt_logged) {
			U_LOG_W("D3D12 zone mask: target format %u unsupported "
			        "(composite PSOs cover R8G8B8A8/B8G8R8A8 UNORM) — mask ignored",
			        (unsigned)dd.Format);
			dfmt_logged = true;
		}
		ZC_BAIL("g3");
	}

	// Resolve the mask source: an explicit submitted mask wins; else
	// rasterize the implicit mask from the Local2D layer rects (M=0 inside the
	// rect union, M=1 elsewhere — records onto the open cmd-list).
	// #491 part 3 — split Local2D by list order vs the projection: under-layers
	// (before the projection) are the DP backdrop, excluded from the overlay.
	int32_t proj_idx = -1;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
		if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
			proj_idx = (int32_t)i;
			break;
		}
	}

	// Zones frame (XR_DXR_display_zones, #801): the mask is ALWAYS the
	// BINARY zone raster — an explicit frame wish is staged for the publish
	// only (never a compositor blend gate). #803: when any zone requests
	// feather, the composite samples a separately-rastered per-zone feather
	// mask instead; the published wish stays binary regardless (cosmetics
	// never enter the wish).
	ID3D12Resource *mask_res = nullptr;
	if (reuse_mask) {
		/*
		 * A repaint replays RENDERING, never STATE TRANSITIONS.
		 *
		 * d3d12_update_zone_wish_state() and d3d12_update_implicit_mask() are
		 * not queries: they raster, copy tex->staged on the command list, set
		 * the P4 publish source and bump zone_publish_seq. Running them from a
		 * repaint ticks a once-per-app-frame state machine at panel rate,
		 * out of band with the post-present sideband publish that layer_commit
		 * does and a repaint does not.
		 *
		 * Measured cost of getting this wrong: the resolution returned NULL on
		 * ~17% of repaints, the composite bailed, and those frames showed bare
		 * weave where the 2D was -- the desktop compose-under flickering on top
		 * of the 2D bubble at roughly five frames a second.
		 */
		mask_res = c->repaint.mask_res;
	} else if (zones_frame) {
		mask_res = d3d12_update_zone_wish_state(c, region_w, region_h);

		struct xrt_rect zrects[XRT_MAX_LAYERS];
		float zfeather[XRT_MAX_LAYERS];
		uint32_t zcount = 0;
		bool any_feather = false;
		for (uint32_t i = 0; i < c->layer_accum.layer_count && zcount < XRT_MAX_LAYERS; i++) {
			if (c->layer_accum.layers[i].data.type != XRT_LAYER_ZONE_3D) {
				continue;
			}
			zfeather[zcount] = c->layer_accum.layers[i].data.zone_3d.feather_px;
			if (zfeather[zcount] > 0.0f) {
				any_feather = true;
			}
			zrects[zcount++] = c->layer_accum.layers[i].data.zone_3d.rect;
		}
		if (any_feather) {
			ID3D12Resource *fres =
			    d3d12_update_zone_feather_mask(c, zrects, zfeather, zcount, region_w, region_h);
			if (fres != nullptr) {
				mask_res = fres;
			} // raster failure: binary fallback — hard edges, never a lost frame
		}
	} else if (have_explicit) {
		mask_res = mask->staged;
	} else {
		struct xrt_rect rects[XRT_MAX_LAYERS];
		uint32_t rect_count = 0;
		for (uint32_t i = 0; i < c->layer_accum.layer_count && rect_count < XRT_MAX_LAYERS; i++) {
			if (c->layer_accum.layers[i].data.type != XRT_LAYER_LOCAL_2D) {
				continue;
			}
			if (proj_idx >= 0 && (int32_t)i < proj_idx) {
				continue; // under-layer (backdrop) — not part of the overlay mask
			}
			rects[rect_count++] = c->layer_accum.layers[i].data.local_2d.rect;
		}
		mask_res = d3d12_update_implicit_mask(c, rects, rect_count, region_w, region_h);
	}
	if (mask_res == nullptr) {
		ZC_BAIL("g4");
	}
	if (!reuse_mask) {
		// Hand the repaint path a mask that is already resolved and published.
		c->repaint.mask_res = mask_res;
	}

	// Resolve the `twod` source + a window-sized weave snapshot scratch.
	// Local2D layers (Phase 3) / zone 2D bands flatten into the dedicated
	// local2d_scratch. With zero Local2D layers (e.g. an explicit mask with
	// no 2D content, or a zones frame whose 2D bands are empty) this is a
	// clear-only flatten — MODE_ZONES then writes M·weave (zone interior)
	// over transparent, so pixels outside every zone present alpha 0.
	ID3D12Resource *twod_res = nullptr;
	if (!d3d12_ensure_local2d_scratch(c, region_w, region_h)) {
		ZC_BAIL("g5");
	}
	// Zones frame: flatten ALL Local2D layers (no under/over split —
	// 2D-under is reserved in v1).
	if (!reuse_mask) {
		if (!d3d12_flatten_local_2d_layers(c, region_w, region_h, zones_frame ? -1 : proj_idx)) {
			ZC_BAIL("g7");
		}
	}
	/*
	 * #868: a repaint reuses the last app frame's flatten and must NOT re-run
	 * it. d3d12_flatten_local_2d_layers samples the APP'S OWN Local2D swapchain
	 * images; by the time a repaint runs, the app has reacquired those images
	 * and may be part-way through drawing the next frame into them. This is the
	 * same hazard that keeps repaints off the zero-copy atlas, and it applies to
	 * every app-owned texture the composite touches -- not just the atlas.
	 *
	 * Symptom when this is wrong: the 2D region samples a half-written app
	 * image, so the 2D content differs between an app weave and the repaint
	 * standing in for it, and the two alternate on screen. Measured as a
	 * whole-bubble difference in a pixel diff of an adjacent app/repaint pair,
	 * while the opaque 3D content differed only at interlace edges.
	 *
	 * local2d_scratch is compositor-owned and survives until the next app
	 * frame re-flattens it, which is exactly the content a repaint wants.
	 */
	twod_res = c->local2d_scratch;

	// #875: the DEPOSIT half ends here — every read of an app-owned resource is
	// done and its result lives in compositor-owned scratch.
	if (prepare_only) {
		return true;
	}

	// RENDER half only. This snapshot target is sized/typed from the render
	// target's format, which the deposit half has no business knowing — asking
	// for it during prepare requested DXGI_FORMAT_UNKNOWN and failed, which is
	// what silently swallowed the 2D in the first two attempts at this split.
	if (!d3d12_ensure_scratch(c, &c->weave_scratch, region_w, region_h, dd.Format, "local2d weave")) {
		ZC_BAIL("g6");
	}

	// Snapshot the window region of the weave (the DP wrote dst; the weave
	// target is RTV-only to the shader, so the lerp reads this copy).
	D3D12_RESOURCE_BARRIER weave_enter[2] = {};
	weave_enter[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	weave_enter[0].Transition.pResource = dst;
	weave_enter[0].Transition.StateBefore = dst_pre_state;
	weave_enter[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	weave_enter[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	weave_enter[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	weave_enter[1].Transition.pResource = c->weave_scratch;
	weave_enter[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	weave_enter[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	weave_enter[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, weave_enter);

	D3D12_TEXTURE_COPY_LOCATION weave_dst_loc = {};
	weave_dst_loc.pResource = c->weave_scratch;
	weave_dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	weave_dst_loc.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION weave_src_loc = {};
	weave_src_loc.pResource = dst;
	weave_src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	weave_src_loc.SubresourceIndex = 0;
	D3D12_BOX weave_box = {0, 0, 0, region_w, region_h, 1};
	c->cmd_list->CopyTextureRegion(&weave_dst_loc, 0, 0, 0, &weave_src_loc, &weave_box);

	// Weave scratch → sampleable; dst → RENDER_TARGET for the composite draw.
	D3D12_RESOURCE_BARRIER weave_exit[2] = {};
	weave_exit[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	weave_exit[0].Transition.pResource = dst;
	weave_exit[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	weave_exit[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	weave_exit[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	weave_exit[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	weave_exit[1].Transition.pResource = c->weave_scratch;
	weave_exit[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	weave_exit[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	weave_exit[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, weave_exit);

	// Effective canvas rect clamped to the window region (the shader ignores
	// it on the mask path; kept coherent for the constants anyway). Phase 2:
	// this is the window rect while the mask is active.
	int32_t cx = eff_canvas->valid ? eff_canvas->x : 0;
	int32_t cy = eff_canvas->valid ? eff_canvas->y : 0;
	uint32_t cw = eff_canvas->valid ? eff_canvas->w : region_w;
	uint32_t ch = eff_canvas->valid ? eff_canvas->h : region_h;
	uint32_t cx_u = (cx < 0) ? 0u : (uint32_t)cx;
	uint32_t cy_u = (cy < 0) ? 0u : (uint32_t)cy;
	if (cx_u > region_w)
		cx_u = region_w;
	if (cy_u > region_h)
		cy_u = region_h;
	uint32_t cright = (cx_u + cw > region_w) ? region_w : cx_u + cw;
	uint32_t cbottom = (cy_u + ch > region_h) ? region_h : cy_u + ch;

	// #491: the implicit (auto) Local2D mask composites the 2D over the weave by
	// its own premultiplied alpha (translucent 2D reveals the 3D scene). The
	// explicit authored mask keeps the hard M-lerp.
	// XR_DXR_display_zones: MODE_ZONES (twod + (1−a)·(M·weave)) — the binary
	// zone raster (or the #803 feather ramp) gates only the WEAVE; Local2D
	// content composites on top by its own alpha (ADR-027/#801: the wish is
	// hardware-only; composition follows zone geometry + alpha). Formerly
	// the hard M-lerp, which multiplied overlays away inside zones and
	// dimmed the feathered edge.
	uint32_t composite_mode;
	if (zones_frame) {
		composite_mode = COMP_D3D12_COMPOSITE_MODE_ZONES;
	} else if (have_explicit) {
		composite_mode = COMP_D3D12_COMPOSITE_MODE_LERP;
	} else {
		composite_mode = COMP_D3D12_COMPOSITE_MODE_ALPHA_OVER;
	}

	// One-shot proof-of-life (capture is pre-weave, #492 — this is how we
	// confirm the post-weave composite ran without a live eyeball).
	static bool composite_logged = false;
	if (!composite_logged) {
		U_LOG_W("D3D12 Local2D composite: %ux%u region, %s mask, twod=local2d layers (mode=%u)",
		        region_w, region_h, zones_frame ? "zone" : (have_explicit ? "explicit" : "implicit"),
		        composite_mode);
		composite_logged = true;
	}

	// #833/#116 — opaque present on a transparent session: DWM completes no
	// blends, so the composite flattens against the weave (which the DP's
	// flattened gate already completed against the captured desktop) and
	// emits α=1. Opaque sessions keep today's behavior even with the env set.
	const bool opaque_present = c->transparent_background && debug_get_bool_option_present_opaque_comp();
	xrt_result_t xret = comp_d3d12_renderer_composite_2d_masked(
	    c->renderer, c->cmd_list, dst_rtv, static_cast<uint32_t>(dd.Format), twod_res, mask_res,
	    c->weave_scratch, region_w, region_h, (int32_t)cx_u, (int32_t)cy_u, cright - cx_u, cbottom - cy_u,
	    composite_mode, opaque_present);

	// Restore steady states: dst → caller's post state, scratches → COMMON.
	// twod_res is the local2d scratch that supplied the 2D pixels; it sits in
	// PIXEL_SHADER_RESOURCE after its setup above.
	D3D12_RESOURCE_BARRIER restore[3] = {};
	uint32_t n = 0;
	if (dst_post_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
		restore[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		restore[n].Transition.pResource = dst;
		restore[n].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		restore[n].Transition.StateAfter = dst_post_state;
		restore[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		n++;
	}
	restore[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	restore[n].Transition.pResource = twod_res;
	restore[n].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	restore[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	restore[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	n++;
	restore[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	restore[n].Transition.pResource = c->weave_scratch;
	restore[n].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	restore[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	restore[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	n++;
	c->cmd_list->ResourceBarrier(n, restore);

	return xret == XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_create(struct xrt_compositor *xc, uint32_t w, uint32_t h, void **out_mask)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	if (out_mask == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// 0 → runtime chooses: the client-window dims (#464 — the mask is
	// window-sized by definition), falling back to the render surface.
	if (w == 0 || h == 0) {
		HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
		RECT r;
		if (wnd != nullptr && GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
			w = (uint32_t)r.right;
			h = (uint32_t)r.bottom;
		} else if (c->shared_texture != nullptr) {
			D3D12_RESOURCE_DESC td = c->shared_texture->GetDesc();
			w = (uint32_t)td.Width;
			h = td.Height;
		} else if (c->target != nullptr) {
			comp_d3d12_target_get_dimensions(c->target, &w, &h);
		}
	}
	if (w == 0 || h == 0) {
		U_LOG_E("zone_mask_create: no window/surface to derive mask dims from");
		return XRT_ERROR_ALLOCATION;
	}

	struct comp_d3d12_zone_mask *mask = U_TYPED_CALLOC(struct comp_d3d12_zone_mask);
	if (mask == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}
	mask->w = w;
	mask->h = h;

	// Authoring texture: committed R8_UNORM render target, steady state
	// RENDER_TARGET, optimized clear = all-3D (matches the default fill).
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = w;
	td.Height = h;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8_UNORM;
	td.SampleDesc.Count = 1;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_R8_UNORM;
	clear.Color[0] = 1.0f;

	HRESULT hr = c->device->CreateCommittedResource(
	    &heap, D3D12_HEAP_FLAG_NONE, &td,
	    D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
	    IID_PPV_ARGS(&mask->tex));
	if (mask->tex != nullptr) mask->tex->SetName(L"DXR.zone_mask_tex"); // #747 attribution

	if (SUCCEEDED(hr) && mask->tex != nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
		rtv_desc.NumDescriptors = 1;
		rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hr = c->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&mask->rtv_heap));
	}
	if (SUCCEEDED(hr) && mask->rtv_heap != nullptr) {
		c->device->CreateRenderTargetView(mask->tex, nullptr,
		                                  mask->rtv_heap->GetCPUDescriptorHandleForHeapStart());
		// Staged snapshot: plain texture, steady PIXEL_SHADER_RESOURCE.
		td.Flags = D3D12_RESOURCE_FLAG_NONE;
		hr = c->device->CreateCommittedResource(
		    &heap, D3D12_HEAP_FLAG_NONE, &td,
		    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
		    IID_PPV_ARGS(&mask->staged));
	if (mask->staged != nullptr) mask->staged->SetName(L"DXR.zone_mask_staged"); // #747 attribution
	}
	if (FAILED(hr) || mask->staged == nullptr) {
		U_LOG_E("zone_mask_create: D3D12 resource creation failed: 0x%08x", hr);
		if (mask->rtv_heap != nullptr) {
			mask->rtv_heap->Release();
		}
		if (mask->tex != nullptr) {
			mask->tex->Release();
		}
		free(mask);
		return XRT_ERROR_ALLOCATION;
	}

	// Default to all-3D (M=1): an unauthored-but-submitted mask degrades to
	// the full weave (the no-2D-declared analog), never a blanked canvas.
	// Also prime the staged copy so a create→submit with no authoring is
	// coherent. Recorded + executed via the zone-op re-arm pattern.
	d3d12_zone_cmd_begin(c);
	const float all_3d[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(mask->rtv_heap->GetCPUDescriptorHandleForHeapStart(), all_3d, 0, nullptr);

	D3D12_RESOURCE_BARRIER to_copy[2] = {};
	to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[0].Transition.pResource = mask->tex;
	to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[1].Transition.pResource = mask->staged;
	to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, to_copy);

	c->cmd_list->CopyResource(mask->staged, mask->tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	c->cmd_list->ResourceBarrier(2, to_copy);
	d3d12_zone_cmd_execute(c);

	// One-off lifecycle event (WARN per the debug-logging convention so it
	// survives the hot-path INFO filter).
	U_LOG_W("zone_mask_create: %ux%u (client-window px)", w, h);
	*out_mask = mask;
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_set_whole(struct xrt_compositor *xc, void *mask_ptr, bool enable_3d)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->rtv_heap == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// Tier 1: one full clear (mask->tex sits in RENDER_TARGET).
	d3d12_zone_cmd_begin(c);
	const float m[4] = {enable_3d ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(mask->rtv_heap->GetCPUDescriptorHandleForHeapStart(), m, 0, nullptr);
	d3d12_zone_cmd_execute(c);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                          void *mask_ptr,
                                          uint32_t count,
                                          const struct xrt_rect *rects)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->rtv_heap == nullptr || (count > 0 && rects == nullptr)) {
		return XRT_ERROR_ALLOCATION;
	}

	// Clamp the rects up-front (client-window px); skip fully-outside /
	// degenerate ones. D3D12's ClearRenderTargetView takes the rect array
	// natively — one call, vs D3D11's per-rect ClearView loop.
	D3D12_RECT *drs = nullptr;
	uint32_t n = 0;
	if (count > 0) {
		drs = U_TYPED_ARRAY_CALLOC(D3D12_RECT, count);
		if (drs == nullptr) {
			return XRT_ERROR_ALLOCATION;
		}
		for (uint32_t i = 0; i < count; i++) {
			int32_t left = rects[i].offset.w;
			int32_t top = rects[i].offset.h;
			int32_t right = left + rects[i].extent.w;
			int32_t bottom = top + rects[i].extent.h;
			if (left < 0) {
				left = 0;
			}
			if (top < 0) {
				top = 0;
			}
			if (right > (int32_t)mask->w) {
				right = (int32_t)mask->w;
			}
			if (bottom > (int32_t)mask->h) {
				bottom = (int32_t)mask->h;
			}
			if (right <= left || bottom <= top) {
				continue;
			}
			drs[n].left = left;
			drs[n].top = top;
			drs[n].right = right;
			drs[n].bottom = bottom;
			n++;
		}
	}

	// M=0 everywhere, then M=1 inside the surviving rects.
	d3d12_zone_cmd_begin(c);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = mask->rtv_heap->GetCPUDescriptorHandleForHeapStart();
	const float all_2d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(rtv, all_2d, 0, nullptr);
	if (n > 0) {
		const float all_3d[4] = {1.0f, 0.0f, 0.0f, 0.0f};
		c->cmd_list->ClearRenderTargetView(rtv, all_3d, n, drs);
	}
	d3d12_zone_cmd_execute(c);

	free(drs);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_acquire_rt(
    struct xrt_compositor *xc, void *mask_ptr, void **out_resource, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->tex == nullptr || out_resource == nullptr || out_w == nullptr ||
	    out_h == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// The runtime retains ownership of the resource (the app must not
	// Release it); valid until the mask handle is destroyed. The compositor
	// device + queue are the app's own in-process, so the app records its
	// own RTV (descriptor heaps are app-owned in D3D12) and draws directly;
	// submission order is the sync. State contract: handed out in
	// RENDER_TARGET, must be back in RENDER_TARGET before submit.
	*out_resource = mask->tex;
	*out_w = mask->w;
	*out_h = mask->h;
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->staged == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// Snapshot the authoring texture so in-progress Tier-3 drawing can never
	// tear into a frame, and make this the active mask. Sticky
	// last-submit-wins: it stays active across frames until re-submit or
	// destroy (destroy reverts to full-weave behavior). The same-queue
	// ExecuteCommandLists + CPU wait below orders the copy after any Tier-3
	// authoring the app already submitted (no fence — same queue).
	d3d12_zone_cmd_begin(c);

	D3D12_RESOURCE_BARRIER to_copy[2] = {};
	to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[0].Transition.pResource = mask->tex;
	to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[1].Transition.pResource = mask->staged;
	to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, to_copy);

	c->cmd_list->CopyResource(mask->staged, mask->tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	c->cmd_list->ResourceBarrier(2, to_copy);
	d3d12_zone_cmd_execute(c);

	mask->submitted = true;
	c->active_zone_mask = mask;
	c->zone_publish_seq++; // #224 P4: new content generation for the DP publish
	return XRT_SUCCESS;
}

extern "C" void
comp_d3d12_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr) {
		return;
	}
	// XR_DXR_display_zones: never leave a dangling frame-wish reference.
	if (c->frame_wish == mask) {
		c->frame_wish = nullptr;
	}
	if (c->active_zone_mask == mask) {
		c->active_zone_mask = nullptr; // revert to full-weave behavior
	}
	// #224 P4: drop the seq-dedup cache (pointer may be reused by a future
	// alloc) and any per-frame publish source borrowed from this mask.
	if (c->zone_frame_wish_last == mask) {
		c->zone_frame_wish_last = nullptr;
	}
	if (c->zone_publish_res == mask->staged) {
		c->zone_publish_res = nullptr;
	}
	// The frame that might still reference these resources has fence-waited
	// before layer_commit returned (the mutex we hold serializes us behind
	// it), so an immediate Release is safe.
	if (mask->staged != nullptr) {
		mask->staged->Release();
	}
	if (mask->rtv_heap != nullptr) {
		mask->rtv_heap->Release();
	}
	if (mask->tex != nullptr) {
		mask->tex->Release();
	}
	free(mask);
}

extern "C" void
comp_d3d12_compositor_zones_set_frame_wish(struct xrt_compositor *xc, void *mask)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	// Per-frame reference (XR_DXR_display_zones): oxr sets this on every
	// zones frame before layer_commit, NULL meaning auto-derive. Consumed
	// by the commit's composite; harmlessly stale on zero-zone frames (the
	// zones branch never reads it there).
	c->frame_wish = static_cast<struct comp_d3d12_zone_mask *>(mask);
}

// #439 Phase 3 Q4 — the compositor's current recommended per-view render size,
// polled at frame end by oxr to fire XrEventDataLocal3DZoneViewSizeChangedDXR
// on a change (mask/Local2D activation or window resize supersedes the canvas).
// Returns false if no renderer / zero dims. Mirrors the D3D11 getter.
extern "C" bool
comp_d3d12_compositor_get_recommended_view_size(struct xrt_compositor *xc, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	if (out_w == nullptr || out_h == nullptr || c->renderer == nullptr) {
		return false;
	}
	uint32_t vw = 0;
	uint32_t vh = 0;
	comp_d3d12_renderer_get_view_dimensions(c->renderer, &vw, &vh);
	if (vw == 0 || vh == 0) {
		return false;
	}
	*out_w = vw;
	*out_h = vh;
	return true;
}
