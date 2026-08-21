// Copyright 2024-2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native D3D11 compositor implementation.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_compositor.h"
#include "comp_d3d11_swapchain.h"
#include "comp_d3d11_target.h"
#include "util/comp_display_refresh_win.h"
#include "comp_d3d11_renderer.h"
#include "comp_d3d11_outcomp.h"
#include "comp_d3d11_window.h"
#include "comp_d3d11_state_guard.h"
#include "comp_d3d11_xbridge.h"

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

#ifdef XRT_FEATURE_DEBUG_GUI
#include "util/u_debug_gui.h"
#include "comp_d3d11_debug.h"
#endif

#include "xrt/xrt_display_processor_d3d11.h"

#include "util/u_hud.h"
#include <displayxr_mcp/mcp_capture.h>

// STB_IMAGE_WRITE_STATIC scopes stbi_write_* symbols to this TU so
// we don't clash with comp_d3d11_service.cpp's implementation (both
// link into ipc_server on Windows).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "math/m_api.h"
#include "d3d/d3d_dxgi_formats.h"
#include "d3d/d3d_scanout_helpers.hpp"
#include "util/u_tiling.h"
#include "util/u_canvas.h"
#include "util/u_capture_intent.h"
#include "util/u_capture_dims.h"
#include "util/u_image_capture.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <stddef.h> // offsetof — the prefix-mirror tripwire below
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <cmath>

/*!
 * Minimal settings struct for D3D11 compositor (replaces deleted main/comp_settings.h).
 */
struct comp_settings
{
	struct
	{
		uint32_t width;
		uint32_t height;
	} preferred;

	//! Nominal frame interval in nanoseconds.
	int64_t nominal_frame_interval_ns;
};

/*!
 * The D3D11 native compositor structure.
 */
// Decoupled presentation (#833): same env the target reads. On a transparent
// session the DP's alpha-gate flattens against the captured desktop (plugin
// #116), so the post-weave Local2D composite must flatten too instead of
// emitting DWM-dependent alpha.
DEBUG_GET_ONCE_BOOL_OPTION(present_opaque_comp, "DXR_PRESENT_OPAQUE", false)

/*!
 * #918 output-device split. On a non-MUX hybrid the panel is scanned out by the
 * iGPU while the app renders on the dGPU, so today's placement pays ~42 ms of
 * present-to-display and re-crosses the whole woven frame on every repaint tick
 * (docs/investigations/hybrid-igpu-weave.md). Opting in moves the target, the
 * display processor, the HUD and the repaint loop onto the SCANOUT adapter and
 * ships only the atlas across, once per app frame.
 *
 * Prototype, off by default. Silent no-op when the scanout adapter IS the app's
 * adapter; any setup failure falls back to the stock single-device path with one
 * WARN naming the reason — it must never brick a session.
 */
DEBUG_GET_ONCE_BOOL_OPTION(weave_on_scanout, "DXR_WEAVE_ON_SCANOUT", false)

struct comp_d3d11_compositor
{
	//! Base type - must be first!
	struct xrt_compositor_native base;

	//! The device we are rendering for.
	struct xrt_device *xdev;

	//! D3D11 device (from app's graphics binding, we add a reference).
	ID3D11Device *device;

	//! D3D11 immediate context.
	ID3D11DeviceContext *context;

	//! DXGI factory for swapchain creation.
	IDXGIFactory4 *dxgi_factory;

	/*!
	 * #868: the device's multithread lock, held for the LIFETIME of the
	 * compositor so a repaint can make its whole weave sequence atomic.
	 *
	 * `device`/`context` above belong to the APP — an immediate context is a
	 * single shared object, and SetMultithreadProtected(TRUE) only makes
	 * INDIVIDUAL calls thread-safe. A weave is a sequence (bind RTV, set
	 * viewport, draw), so the app's own render thread could call
	 * OMSetRenderTargets between our bind and the weaver's draw — the weave
	 * then landed on the app's render target and the swapchain stayed black.
	 * Enter()/Leave() takes that same internal lock across the whole
	 * sequence, which is the only way to make it atomic.
	 *
	 * D3D12 needs no equivalent: it records into per-thread command lists and
	 * allocators, so there is no shared immediate state to interleave with.
	 *
	 * MUST stay BELOW dxgi_factory: comp_d3d11_renderer.cpp mirrors the members
	 * above as `comp_d3d11_compositor_internals` to reach them from C. A field
	 * inserted into that prefix silently shifts the mirror and the renderer
	 * reads the wrong pointer. (comp_d3d11_target.cpp no longer mirrors it —
	 * the target is handed its device/context/factory explicitly, #918.)
	 */
	ID3D10Multithread *mt_lock;

	/*!
	 * #918 OUTPUT-DEVICE SPLIT. Everything below is NULL/false unless
	 * DXR_WEAVE_ON_SCANOUT=1 resolved a scanout adapter that differs from the
	 * app device's. When the split is active these — not `device`/`context`/
	 * `dxgi_factory` above — own the swapchain, the display processor, the HUD
	 * and the repaint loop; the renderer and the atlas stay on the app device
	 * and the composited atlas crosses once per frame through `xbridge`.
	 *
	 * Appended AFTER mt_lock deliberately: comp_d3d11_renderer.cpp still
	 * mirrors the prefix up to dxgi_factory as
	 * `comp_d3d11_compositor_internals`, so nothing may be inserted into it.
	 */
	ID3D11Device *out_dev;
	ID3D11DeviceContext *out_ctx;
	IDXGIFactory4 *out_factory;
	LUID out_luid;
	struct comp_d3d11_xbridge *xbridge;
	bool split_active;
	//! Monotonic app frame counter — the single seq every bridge fence uses.
	uint64_t split_seq;
	/*!
	 * #918 Phase 2a — the panel extent the bridge PLANES are sized from, and the
	 * dims the Local2D / backdrop scratches take under the split. Panel-sized
	 * once, never resized: a window can never exceed the panel, so this keeps the
	 * plane transport structurally out of the R2 churn hysteresis and the #1091
	 * resize path. 0 when the split is off (the scratches stay region-sized, byte
	 * for byte as before).
	 */
	uint32_t split_panel_w, split_panel_h;
	/*!
	 * #918 Phase 2a — composite recipes REFUSED because the slot's stamp
	 * disagreed with the frame the DP is now running. Same class as
	 * split_stale_refusals, one level down: the atlas generation guards the
	 * weave, this guards the composite on top of it. Expected to read ZERO —
	 * a mode change bumps the layout generation, so the slot is normally refused
	 * upstream before this can fire. It is the instrument that PROVES that.
	 */
	uint64_t split_recipe_refusals;
	uint64_t split_recipe_logged_gen;
	//! Per-plane DIAG deltas: the lifetime copy/skip totals as of the last line.
	uint64_t split_plane_copies_prev[COMP_D3D11_XBRIDGE_PLANE_COUNT];
	uint64_t split_plane_skips_prev[COMP_D3D11_XBRIDGE_PLANE_COUNT];
	/*!
	 * #918 Phase 2a — REGION-SIZED views of the planes the DISPLAY PROCESSOR
	 * consumes through its sideband entry points.
	 *
	 * The masked composite can sample a panel-sized plane over a sub-rect because
	 * its shader takes a uv scale. The DP's `set_background_2d` takes an SRV plus
	 * a width/height and no scale, so handing it the panel-sized backdrop plane
	 * would tell a vendor the backdrop is 1280x720 while giving it a 3840x2160
	 * texture — it would minify the whole panel into the region.
	 *
	 * #918 review F8: this copy is SEQ-GATED and METERED. It used to run on every
	 * weave AND every repaint tick, unconditionally, on the output device and
	 * invisible to the bandwidth gate — 4K RGBA8 at panel rate is GB/s of iGPU
	 * traffic, and re-copying an unchanged backdrop is precisely the per-tick
	 * cost the split exists to remove. It now runs only when the source slot's
	 * plane generation differs from the one the view already holds, and its bytes
	 * appear in the DIAG line.
	 *
	 * Since the authored-mask plane is now MASK-sized (#918 review F5) rather than
	 * panel-sized, the mask publish takes the passthrough path below and copies
	 * nothing at all.
	 */
	struct d3d11_dp_plane_view
	{
		ID3D11Texture2D *tex;
		ID3D11ShaderResourceView *srv;
		//! Allocated extent, and the content generation the view holds.
		uint32_t w, h;
		uint64_t seq;
		//! #918 review D7: one alloc-failure WARN per session, never per frame.
		bool warned;
	};
	struct d3d11_dp_plane_view dp_bd_view;
	struct d3d11_dp_plane_view dp_mask_view;
	//! #918 review F8 — sideband copy bytes and count since the last DIAG line.
	uint64_t split_sideband_bytes;
	uint64_t split_sideband_copies;
	uint64_t split_sideband_skips;
	//! Frames where the split had nothing woven to show, so the Present was
	//! skipped and the panel kept the previous frame (#918 F4). Reported at most
	//! once a second — a skip is a per-frame event and must never WARN per frame.
	uint64_t split_skip_present;
	uint64_t split_skip_logged_ns;
	/*!
	 * #918 R1: the layout generation, bumped whenever the effective layout the
	 * DP is handed changes (a mode switch, a submission whose view count
	 * changed). Every bridged frame is stamped with it and every weave demands
	 * it, so the split can never do what the non-split path cannot — weave one
	 * frame's pixels under the next frame's recipe.
	 */
	uint64_t split_layout_gen;
	//! Signature of the layout `split_layout_gen` currently stands for.
	uint64_t split_layout_sig;
	//! Weaves refused because the only landed content predates the current
	//! generation. One per mode switch is expected and correct.
	uint64_t split_stale_refusals;
	//! Generation whose first refusal has already been logged — the refusal WARN
	//! is per TRANSITION (a handful per session), never per frame.
	uint64_t split_stale_logged_gen;
	//! #918 R1: transition frames woven from a still-in-flight slot instead of
	//! being skipped. Each one is a mode switch that did NOT leave the panel
	//! holding a frame woven for the mode it just left.
	uint64_t split_inflight_weaves;
	//! Weave cadence, for the #918 R1/R2 evidence: the last weave that reached
	//! the DP and the longest gap between two of them.
	uint64_t split_weave_last_ns;
	uint64_t split_weave_gap_max_ns;
	//! DXR_XBRIDGE_DIAG=1 — a once-a-second cadence line while testing the
	//! split. Off by default; never per frame either way.
	bool split_diag;
	uint64_t split_diag_logged_ns;

	/*!
	 * #918 review D6/F1 — the authored-mask PLANE's per-frame publication,
	 * resolved once by @ref d3d11_stage_mask_plane and read everywhere else.
	 *
	 * There is exactly ONE authored-mask plane and two candidate producers (a
	 * zones frame's explicit wish, a legacy frame's sticky submitted mask). They
	 * used to bind it from their own code paths with globally-unique
	 * generations, so alternating frames re-bound it every frame — a blocking
	 * producer drain and a full re-transport each time — and the sticky path's
	 * bind sat inside a branch that returned before the recipe was ever stamped,
	 * which is why a sticky Tier-3 mask could never bootstrap at all. One
	 * publisher per frame, called from layer_commit before the deposit, replaces
	 * both.
	 * @{
	 */
	//! True when this frame's authored mask is genuinely riding the plane. When
	//! false the composite and the DP publish fall back to the output-device
	//! shadow, so a bind failure degrades Tier 3 and nothing else.
	bool mask_plane_live;
	//! `staged_gen` of the mask the plane currently carries (0 = none). The
	//! destroy hook drops the binding only for THIS mask (#918 review D2).
	uint64_t mask_plane_gen;
	/*! @} */

	//! #918 review D4 — one WARN per session when the Local2D plane cannot be
	//! bound, never one per frame.
	bool local2d_plane_warned;

	//! Output target (DXGI swapchain).
	struct comp_d3d11_target *target;

	//! Renderer for layer compositing.
	struct comp_d3d11_renderer *renderer;

	//! #918 Phase 2a — the masked 2D-over-3D composite, on an EXPLICIT device.
	//! Created on the app device here (behaviour-neutral); the split points it
	//! at the output device once the composite's inputs cross (Phase 2a-2).
	struct comp_d3d11_outcomp *outcomp;

	//! Accumulated layers for the current frame.
	struct comp_layer_accum layer_accum;

	//! Compositor settings.
	struct comp_settings settings;

	//! Window handle for rendering (either from app or self-created).
	HWND hwnd;

	//! App's window handle for position tracking in shared-texture mode.
	//! When non-NULL, the hidden weaver window is repositioned each frame
	//! to match this window's client rect on screen.
	HWND app_hwnd;

	//! Self-created window (NULL if app provided window).
	struct comp_d3d11_window *own_window;

	//! True if we created the window ourselves.
	bool owns_window;

	//! Shared texture opened from app's shared HANDLE (may be NULL).
	ID3D11Texture2D *shared_texture;

	//! Render target view for shared texture (may be NULL).
	ID3D11RenderTargetView *shared_rtv;

	//! True if shared texture mode is active.
	bool has_shared_texture;

	//! Transparent session (#573): the DP composes-under-bg. With
	//! DXR_PRESENT_OPAQUE (#833) this also flips the Local2D composite into
	//! its flatten mode (plugin #116 flattens the gate).
	bool transparent_background;


	//! Active authored zone mask (#439 Phase 1, XR_DXR_local_3d_zone). Set by
	//! comp_d3d11_compositor_zone_mask_submit (sticky, last-submit-wins),
	//! cleared when that mask is destroyed. NOT owned — the oxr handle owns
	//! the mask; lifetime is guaranteed by the destroy hook clearing this.
	struct comp_d3d11_zone_mask *active_zone_mask;

	//! #439 Phase 3 — runtime-owned flatten target (RT+SRV) the masked
	//! composite reads as `twod` when the frame carries
	//! XrCompositionLayerLocal2DDXR layers. The RT bind + RTV must not dangle
	//! across a 2D-model switch.
	//! Allocated as a trio by d3d11_ensure_rt_srv_scratch.
	ID3D11Texture2D *local2d_scratch;
	ID3D11ShaderResourceView *local2d_scratch_srv;
	ID3D11RenderTargetView *local2d_scratch_rtv;
	//! #918 Phase 2a: NT share handle + realloc generation, so the bridge's
	//! producer can open this app-device texture directly (Option-I shape — no
	//! extra app-device copy per frame). NULL/0 when the split is off.
	HANDLE local2d_scratch_share;
	uint64_t local2d_scratch_gen;
	//! #918 review F4 — the region this scratch was last cleared at. A CHANGE
	//! clears the whole (panel-sized) surface and invalidates the plane's slots,
	//! so pixels the content vacated cannot ride a later dirty-box union across.
	uint32_t local2d_scratch_region_w, local2d_scratch_region_h;

	//! #491 part 3 — 2D-under backdrop flatten target (RT+SRV), same trio as
	//! local2d_scratch. The frame's UNDER Local2D layers (before the projection
	//! in list order) flatten here PRE-weave; the SRV is handed to the DP via
	//! set_background_2d so it composites `backdrop over captured-desktop` under
	//! the 3D. Compositor-owned so it outlives process_atlas.
	ID3D11Texture2D *backdrop_scratch;
	ID3D11ShaderResourceView *backdrop_scratch_srv;
	ID3D11RenderTargetView *backdrop_scratch_rtv;
	//! #918 Phase 2a — same plane-source shape as local2d_scratch above.
	HANDLE backdrop_scratch_share;
	uint64_t backdrop_scratch_gen;
	//! #918 review F4 — same region-change bookkeeping as local2d_scratch above.
	uint32_t backdrop_scratch_region_w, backdrop_scratch_region_h;

	//! #439 Phase 3 — runtime-owned IMPLICIT zone mask, rasterized from the
	//! frame's Local2D layer rects (Q3): M=1 (keep the weave) everywhere,
	//! M=0 (show the 2D layers) inside the union of the layer rects. Used as
	//! the mask source when no explicit mask is submitted but Local2D layers
	//! are present. Mirrors the explicit comp_d3d11_zone_mask RTV+staged
	//! pattern (GPU raster via ClearView, then CopyResource to an SRV-capable
	//! staged sibling) — runtime lifetime, re-rasterized only when the rect
	//! set or dims change.
	ID3D11Texture2D *implicit_mask_tex;            //!< R8_UNORM, RTV-capable
	ID3D11RenderTargetView *implicit_mask_rtv;
	ID3D11Texture2D *implicit_mask_staged;         //!< R8_UNORM, SRV-capable
	ID3D11ShaderResourceView *implicit_mask_staged_srv;
	uint32_t implicit_mask_w, implicit_mask_h;     //!< 0 = unallocated
	struct xrt_rect implicit_rects[XRT_MAX_LAYERS]; //!< dirty-check cache
	uint32_t implicit_rect_count;

	//! #439 Phase 3 — true when the last committed frame carried Local2D
	//! layers. Makes the implicit mask's canvas-supersede effect visible to
	//! d3d11_effective_canvas (the per-frame canvas authority). Set once
	//! under c->mutex at the top of layer_commit (per-frame atomicity).
	bool local_2d_last_frame;

	//! #224 hardware-DP zone leg: cached get_local_zone_caps result.
	//! 0 = not queried yet (calloc default), 1 = supported, 2 = legacy DP
	//! (slot absent / caps unsupported — never publish).
	int zone_dp_state;
	//! DP zone caps when zone_dp_state == 1 (grid dims surface through
	//! comp_d3d11_compositor_zone_get_hw_caps → xrGetLocal3DZoneCapabilitiesDXR).
	struct xrt_dp_local_zone_caps zone_dp_caps;
	//! Zone-mask content generation: bumped on every zone_mask_submit (the
	//! only point the staged content can change). Per-frame publishes carry
	//! the current generation, so a vendor evaluates mask CONTENT once per
	//! generation and treats same-seq publishes as screen-anchor-only updates.
	uint64_t zone_publish_seq;
	//! True while this client's mask is published to the DP — drives the
	//! clear-on-deactivate edge (mask destroyed / compositor teardown).
	bool zone_published;

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
	//! handed in via comp_d3d11_compositor_zones_set_frame_wish before
	//! layer_commit and consumed by that commit (cleared at its end).
	//! NULL = auto-derive. Not owned — the mask handle owns the resources;
	//! handle destroy clears any dangling reference via zone_mask_destroy.
	struct comp_d3d11_zone_mask *frame_wish;
	//! Publish dedup for the explicit frame wish: last pointer + last
	//! author generation actually staged/published.
	struct comp_d3d11_zone_mask *frame_wish_last;
	uint64_t frame_wish_last_seq;

	//! Auto-wish raster (union of the frame's zone rects, BINARY — #800/#801:
	//! the wish is hardware-only and hard-edged by default), R8_UNORM — same
	//! RTV+staged pattern as the implicit mask. One staged SRV serves BOTH
	//! consumers (the MODE_ZONES composite's weave gate + the DP publish when
	//! no explicit wish is staged), the ADR's no-drift property.
	ID3D11Texture2D *wish_mask_tex;
	ID3D11RenderTargetView *wish_mask_rtv;
	ID3D11Texture2D *wish_mask_staged;
	ID3D11ShaderResourceView *wish_mask_staged_srv;
	uint32_t wish_mask_w, wish_mask_h; //!< 0 = unallocated
	struct xrt_rect wish_rects[XRT_MAX_LAYERS]; //!< dirty-check cache
	uint32_t wish_rect_count;

	//! Zones COMPOSITE mask with per-zone opt-in feather
	//! (XrDisplayZoneFeatherDXR, #800/#803). Allocated only when a frame's
	//! zones request feather — the published wish must stay binary
	//! (wish_mask above), so a feathered composite needs its own raster.
	//! All-hard frames sample the binary wish raster for the composite.
	ID3D11Texture2D *feather_mask_tex;
	ID3D11RenderTargetView *feather_mask_rtv;
	ID3D11Texture2D *feather_mask_staged;
	ID3D11ShaderResourceView *feather_mask_staged_srv;
	uint32_t feather_mask_w, feather_mask_h; //!< 0 = unallocated
	struct xrt_rect feather_rects[XRT_MAX_LAYERS]; //!< dirty-check cache
	float feather_radii[XRT_MAX_LAYERS];           //!< dirty-check cache
	uint32_t feather_rect_count;

	//! Generic D3D11 display processor (vendor-agnostic weaving).
	struct xrt_display_processor_d3d11 *display_processor;

	//! System devices (for qwerty driver keyboard input and display mode toggle).
	struct xrt_system_devices *xsysd;

#ifdef XRT_FEATURE_DEBUG_GUI
	//! Debug GUI window.
	struct u_debug_gui *debug_gui;

	//! Debug readback module.
	struct comp_d3d11_debug *debug;
#endif

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;



	//! HUD overlay (runtime-owned windows only).
	struct u_hud *hud;

	//! D3D11 staging texture for HUD pixel upload.
	ID3D11Texture2D *hud_texture;

	//! True if HUD GPU resources are initialized.
	bool hud_initialized;

	//! Last frame timestamp for FPS calculation.
	uint64_t last_frame_time_ns;

	//! Smoothed frame time for display.
	float smoothed_frame_time_ms;

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

	//! Per-frame effective CONTENT layout (#542): the atlas grid actually
	//! painted and handed to the DP this frame — submission-derived,
	//! decoupled from hardware_display_3d. views == 0 until the first
	//! layer commit computes it.
	struct comp_d3d11_eff_layout eff_layout;

	//! Last known 3D rendering mode index (for V-key toggle restore).
	uint32_t last_3d_mode_index;

	//! True when a legacy app is using a compromise view scale that doesn't
	//! match the 3D mode's native scale, so direct rendering mode selection
	//! via 1/2/3 keys should be disabled.
	bool legacy_app_tile_scaling;

	//! Compromise view scale for legacy apps. Only valid when legacy_app_tile_scaling is true.
	float legacy_view_scale_x;
	float legacy_view_scale_y;

	//! Lazily allocated intermediate texture for cropping atlas to content dims
	//! before passing to display processor. NULL when not needed (zero-copy case).
	ID3D11Texture2D *dp_input_texture;

	//! SRV for dp_input_texture.
	ID3D11ShaderResourceView *dp_input_srv;

	//! Cached dimensions for lazy reallocation.
	uint32_t dp_input_width, dp_input_height;

	//! Thread safety.
	std::mutex mutex;

	/*!
	 * #868 — weave-rate decoupling. Mirrors the D3D12 leg; the invariants are
	 * documented there and in [[repaint-never-touches-app-owned-state]]:
	 * a repaint replays RENDERING only. It never reads an app-owned texture,
	 * never ticks a per-frame state machine, and never runs while the app is
	 * part-way through submitting a frame.
	 */
	struct
	{
		int enabled;  //!< DXR_WEAVE_REPAINT=0 kill switch. -1 = unprobed.
		int force;    //!< DXR_WEAVE_REPAINT_FORCE=1 correctness probe.
		bool armed;   //!< Last frame was DP-woven and not zero-copy.
		bool app_frame_in_progress; //!< layer_begin .. layer_commit.
		uint64_t last_app_frame_ns;
		uint64_t count, ticks;

		//! #887 bail counters, mirroring the D3D12 leg: why a tick did not
		//! repaint. armed = not armed / app mid-submission; gate = app still
		//! making rate; race = an app frame landed while we paced.
		uint64_t bail_armed, bail_gate, bail_race;

		//! DXR_WEAVE_REPAINT_HASH=1 adjacent-pair probe.
		int hash_probe;
		uint64_t app_hash, app_seq, hash_seq;
		uint8_t *app_px; //!< retained pixels of the last app weave
		/*!
		 * #876 diag: why the last d3d11_composite_zone_mask call exited.
		 * 0 = composited normally; 1..5 = the numbered early return that fired.
		 * Sampled by the structural-mismatch detector so a black zone can be
		 * attributed to a specific bail instead of guessed at — reading the
		 * code already killed one plausible theory (repaint.mask_srv is only
		 * ever assigned, never cleared, so its null-guard cannot fire after the
		 * first frame).
		 */
		int composite_bail;

		//! #876: per-cell luminance baseline for the absolute dropout detector.
		double cell_ema[64];
		uint64_t dropouts;
		uint64_t detector_frames;

		//! #876 geometry detector: hash of the layout the last APP frame wove
		//! with, and how often a repaint disagreed with it.
		uint64_t geom_app;
		uint64_t geom_mismatches;
		uint64_t geom_app_changes;

		//! Set by the 8x8 grid scan when a cell lit in the app frame came out
		//! black in the repaint — keys the PNG dump on the DEFECT rather than
		//! on the first (usually benign) hash mismatch.
		bool saw_black_region;

		//! Resolved by the last app frame; replayed as-is.
		bool zero_copy;
		void *zc_srv;
		ID3D11ShaderResourceView *backdrop_srv;
		uint32_t backdrop_w, backdrop_h;
		ID3D11ShaderResourceView *atlas_srv;
		ID3D11ShaderResourceView *mask_srv;
		uint32_t view_w, view_h, cols, rows;
		struct u_canvas_rect canvas;
		struct xrt_eye_positions eye_pos;
	} repaint;

	std::thread repaint_thread;
	std::atomic<bool> repaint_quit;

	//! MCP capture_frame request box (serviced at end of layer_commit).
	struct mcp_capture_request mcp_capture;

	//! Per-frame capture intent populated at top of layer_commit. See
	//! u_capture_intent.h.
	struct u_capture_intent capture_intent;
};

/*
 * TRIPWIRE for the prefix mirror in comp_d3d11_renderer.cpp.
 *
 * That file re-declares this struct's first five members as
 * `comp_d3d11_compositor_internals` and reinterpret_casts a
 * comp_d3d11_compositor* onto it to reach device/context/dxgi_factory from a
 * TU that cannot see the real definition. Inserting ANY member into that
 * prefix would shift the mirror and hand the renderer the wrong pointer,
 * silently. The mirror itself cannot assert (it has no view of the real
 * struct), so the assertion lives here, where the real struct IS visible: the
 * prefix must be `base` followed by four naturally-packed pointers, in that
 * order. Adding a member anywhere AFTER dxgi_factory is fine and fires
 * nothing — that is the documented place to grow (#918 appended out_dev &co
 * below mt_lock for exactly this reason).
 */
static_assert(offsetof(struct comp_d3d11_compositor, xdev) == sizeof(struct xrt_compositor_native),
              "comp_d3d11_renderer.cpp's prefix mirror expects xdev right after base");
static_assert(offsetof(struct comp_d3d11_compositor, device) ==
                  offsetof(struct comp_d3d11_compositor, xdev) + sizeof(void *),
              "comp_d3d11_renderer.cpp's prefix mirror expects device right after xdev");
static_assert(offsetof(struct comp_d3d11_compositor, context) ==
                  offsetof(struct comp_d3d11_compositor, device) + sizeof(void *),
              "comp_d3d11_renderer.cpp's prefix mirror expects context right after device");
static_assert(offsetof(struct comp_d3d11_compositor, dxgi_factory) ==
                  offsetof(struct comp_d3d11_compositor, context) + sizeof(void *),
              "comp_d3d11_renderer.cpp's prefix mirror expects dxgi_factory right after context");

/*
 *
 * Helper functions
 *
 */

static inline struct comp_d3d11_compositor *
d3d11_comp(struct xrt_compositor *xc)
{
	return reinterpret_cast<struct comp_d3d11_compositor *>(xc);
}

/*!
 * #918: the device the OUTPUT side lives on — the swapchain, the display
 * processor, the HUD and every read-back of the woven back buffer. The app
 * device when the split is off, so every call site is written once.
 */
static inline ID3D11Device *
d3d11_out_device(struct comp_d3d11_compositor *c)
{
	return c->split_active ? c->out_dev : c->device;
}

static inline ID3D11DeviceContext *
d3d11_out_context(struct comp_d3d11_compositor *c)
{
	return c->split_active ? c->out_ctx : c->context;
}

/*
 * #918 Phase 2a: `d3d11_split_gate` and its per-site enum lived here.
 *
 * Phase 1 shipped the split projection-only, hard-gating the three
 * device-crossing features (the 2D-under backdrop, the zone-mask sideband
 * publish, the zones / Local2D / authored-mask composite) with one WARN each.
 * All three now cross for real — two as bridge planes riding the egress slot,
 * the masks by being rasterized on the output device in the first place — so
 * ZERO gates remain and the helper is gone rather than left as a dead branch.
 */

// #918 review D6/F1 — defined with the rest of the zone-mask code; layer_commit
// (far above it) is where the authored-mask plane's one publisher is called.
struct comp_d3d11_zone_mask;
static struct comp_d3d11_zone_mask *
d3d11_frame_authored_mask(struct comp_d3d11_compositor *c);
static void
d3d11_stage_mask_plane(struct comp_d3d11_compositor *c, struct comp_d3d11_zone_mask *mask);

/*!
 * #918 F4: count a frame whose weave produced nothing and whose Present was
 * therefore skipped, and report the running total at most once a second. Never
 * per frame — a black-flash transition is a burst of skips, and the repo law is
 * that nothing on the frame path may WARN per frame.
 */
static void
d3d11_split_note_skipped_present(struct comp_d3d11_compositor *c, const char *where)
{
	c->split_skip_present++;
	const uint64_t now = os_monotonic_get_ns();
	if (now - c->split_skip_logged_ns < U_TIME_1S_IN_NS) {
		return;
	}
	c->split_skip_logged_ns = now;
	U_LOG_W(
	    "D3D11 output-device split: %s had nothing woven — present SKIPPED, the panel holds the "
	    "last woven frame (%llu skips so far, %llu of them refusing a superseded layout) (#918)",
	    where, (unsigned long long)c->split_skip_present, (unsigned long long)c->split_stale_refusals);
}

/*!
 * #625: drag phase-snap provider for the self-owned window, routed through the
 * DP vtable (snap_window_rect, slot 18). Runs on the WINDOW thread inside
 * WM_WINDOWPOSCHANGING; safe because the v2 snap is a pure coordinate function
 * with no thread affinity, and the v1 fallback's own thread guard makes it
 * return false here (see the prewarm at DP creation). c->display_processor is
 * stable for the compositor's lifetime and the provider is detached (with the
 * setter's exclusive lock) before the DP is destroyed.
 */
static bool
d3d11_compositor_snap_window_rect_cb(void *userdata,
                                     int32_t origin_x,
                                     int32_t origin_y,
                                     int32_t target_x,
                                     int32_t target_y,
                                     int32_t *out_x,
                                     int32_t *out_y)
{
	auto *c = static_cast<struct comp_d3d11_compositor *>(userdata);
	if (c == nullptr || c->display_processor == nullptr) {
		return false;
	}
	return xrt_display_processor_d3d11_snap_window_rect(c->display_processor, origin_x, origin_y,
	                                                    target_x, target_y, out_x, out_y);
}

// #439 Phase 1 authored zone-mask helpers (XR_DXR_local_3d_zone). Defined
// near the bottom of the file alongside the comp_d3d11_compositor_zone_mask_*
// entry points, called from the layer-commit paths + destroy above them.
static bool
d3d11_composite_zone_mask(struct comp_d3d11_compositor *c,
                          bool is_repaint,
                          bool prepare_only,
                          ID3D11Texture2D *dst,
                          uint32_t dst_w,
                          uint32_t dst_h,
                          const struct u_canvas_rect *eff_canvas,
                          int32_t slot);
// #491 part 3 — pre-weave 2D-under backdrop flatten (defined with the other
// Local2D helpers near the bottom; called from the process_atlas sites above).
static ID3D11ShaderResourceView *
d3d11_flatten_backdrop_2d(struct comp_d3d11_compositor *c, uint32_t dst_w, uint32_t dst_h, uint32_t *out_w,
                          uint32_t *out_h);
static void
d3d11_release_zone_state(struct comp_d3d11_compositor *c);
// #918 Phase 2a — region-sized output-device view of a panel-sized bridge plane,
// for the DP sidebands that take dims but no uv scale. Defined with the other
// zone helpers near the bottom; called from d3d11_dp_weave above them.
static ID3D11ShaderResourceView *
d3d11_plane_dp_view(struct comp_d3d11_compositor *c,
                    struct comp_d3d11_compositor::d3d11_dp_plane_view *v,
                    void *plane_srv,
                    uint64_t content_seq,
                    uint32_t w,
                    uint32_t h,
                    DXGI_FORMAT fmt,
                    const char *what);
// #224 hardware-DP zone leg: per-frame sideband publish of the active mask to
// the display processor (and the clear-on-deactivate edge). Defined with the
// other zone helpers near the bottom.
static void
d3d11_sync_zone_mask_to_dp(struct comp_d3d11_compositor *c);
// XR_DXR_display_zones (ADR-027): resolve the zones frame's wish source —
// stage the explicit frame wish, or (re)rasterize the auto wish from the
// frame's zone rects. Defined with the other zone helpers near the bottom.
static void
d3d11_update_zone_wish_state(struct comp_d3d11_compositor *c);

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
// #439 Phase 3: a frame carrying Local2D layers raises an IMPLICIT mask
// (Q3), which supersedes the canvas by the same uniform rule — no third
// state. local_2d_last_frame is set under c->mutex at the top of
// layer_commit, so it reflects the current frame's accumulator.
static struct u_canvas_rect
d3d11_effective_canvas(struct comp_d3d11_compositor *c)
{
	// XR_DXR_display_zones: a zones frame spans the full client window by
	// definition (each zone rect is its own canvas; the output rect is
	// inert) — same supersede geometry as the mask/Local2D rules below.
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

/*
 *
 * xrt_compositor member functions
 *
 */

static xrt_result_t
d3d11_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                                  const struct xrt_swapchain_create_info *info,
                                                  struct xrt_swapchain_create_properties *xsccp)
{
	// D3D11 native compositor can handle all standard properties
	xsccp->image_count = 3; // Triple buffering
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_create_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_swapchain **out_xsc)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	return comp_d3d11_swapchain_create(c, info, out_xsc);
}

static xrt_result_t
d3d11_compositor_import_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_image_native *native_images,
                                   uint32_t image_count,
                                   struct xrt_swapchain **out_xsc)
{
	// For now, don't support importing external swapchains
	// The D3D11 client code should create swapchains directly
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
d3d11_compositor_import_fence(struct xrt_compositor *xc,
                               xrt_graphics_sync_handle_t handle,
                               struct xrt_compositor_fence **out_xcf)
{
	// D3D11 native compositor uses D3D11 synchronization primitives
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
d3d11_compositor_create_semaphore(struct xrt_compositor *xc,
                                   xrt_graphics_sync_handle_t *out_handle,
                                   struct xrt_compositor_semaphore **out_xcsem)
{
	// D3D11 native compositor doesn't expose semaphores
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
d3d11_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	U_LOG_I("D3D11 compositor session begin - hwnd=%p, owns_window=%d, target=%p, renderer=%p",
	        (void *)c->hwnd, c->owns_window, (void *)c->target, (void *)c->renderer);

#ifdef XRT_FEATURE_DEBUG_GUI
	// Start the debug GUI thread now that session is beginning
	// xsysd should have been set via comp_d3d11_compositor_set_system_devices
	if (c->debug_gui != nullptr) {
		u_debug_gui_start(c->debug_gui, NULL, c->xsysd);
	}
#endif

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_end_session(struct xrt_compositor *xc)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	(void)c;

	U_LOG_I("D3D11 compositor session end");

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_predict_frame(struct xrt_compositor *xc,
                                int64_t *out_frame_id,
                                int64_t *out_wake_time_ns,
                                int64_t *out_predicted_gpu_time_ns,
                                int64_t *out_predicted_display_time_ns,
                                int64_t *out_predicted_display_period_ns)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;

	*out_frame_id = c->frame_id;

	// Use queried display refresh rate
	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->display_refresh_rate);

	// #867: measured wait_frame->scanout lookahead when available; the
	// period*2 constant only holds for an app keeping up at queue depth 1.
	int64_t lookahead_ns = period_ns * 2;
	if (c->target != nullptr) {
		const uint64_t measured = comp_d3d11_target_get_predicted_lookahead_ns(c->target);
		if (measured != 0) {
			lookahead_ns = (int64_t)measured;
		}
		comp_d3d11_target_mark_wait_frame(c->target);
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
d3d11_compositor_wait_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	// Check if window was closed (user pressed ESC or closed window).
	// Skip for shared texture mode (hidden window) — session lifetime is
	// controlled by the app, not our hidden weaver window.
	if (c->owns_window && c->own_window != nullptr && c->hwnd != nullptr &&
	    !comp_d3d11_window_is_valid(c->own_window)) {
		// #999: NOT XRT_ERROR_IPC_FAILURE — that flags the session lost and
		// makes xrPollEvent / xrEndSession / xrRequestExitSession fail, so the
		// app can never be told to leave. This code arms the graceful exit.
		U_LOG_I("Window closed - requesting session exit");
		return XRT_ERROR_COMPOSITOR_WINDOW_CLOSED;
	}

	// During drag, synchronize with the window thread's WM_PAINT cycle.
	// This ensures the window position is stable between weave() and Present(),
	// so the interlacing pattern matches the actual displayed position.
	if (c->owns_window && c->own_window != nullptr && c->hwnd != nullptr &&
	    comp_d3d11_window_is_in_size_move(c->own_window)) {
		comp_d3d11_window_wait_for_paint(c->own_window);
	}

	// Use queried display refresh rate
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->display_refresh_rate);

	// Frame pacing is handled by Present(1) + SetMaximumFrameLatency(1).
	// Present(1) blocks until vsync, and the frame latency limit of 1
	// prevents burst/stall queuing. No additional sleep is needed here —
	// adding one would eat into the vsync margin and cause the pipeline
	// to miss vsync deadlines during window drag (dropping from 60→30Hz).

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;

	*out_frame_id = c->frame_id;

	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());

	// #867: measured wait_frame->scanout lookahead when available; the
	// period*2 constant only holds for an app keeping up at queue depth 1.
	int64_t lookahead_ns = period_ns * 2;
	if (c->target != nullptr) {
		const uint64_t measured = comp_d3d11_target_get_predicted_lookahead_ns(c->target);
		if (measured != 0) {
			lookahead_ns = (int64_t)measured;
		}
		comp_d3d11_target_mark_wait_frame(c->target);
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
d3d11_compositor_mark_frame(struct xrt_compositor *xc,
                             int64_t frame_id,
                             enum xrt_compositor_frame_point point,
                             int64_t when_ns)
{
	// Frame timing telemetry - optional
	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Check for window resize and handle it.
	// During a modal drag/resize (in_size_move), we still resize the swapchain target
	// to keep DXGI in sync, but defer the expensive atlas texture reallocation until
	// the drag ends. This avoids per-pixel texture churn that causes stutter.
	bool in_size_move = false;
	if (c->owns_window && c->own_window != nullptr) {
		in_size_move = comp_d3d11_window_is_in_size_move(c->own_window);
	}

	if (c->hwnd != nullptr) {
		RECT rect;
		if (GetClientRect(c->hwnd, &rect)) {
			uint32_t new_width = static_cast<uint32_t>(rect.right - rect.left);
			uint32_t new_height = static_cast<uint32_t>(rect.bottom - rect.top);

			// Only resize if dimensions actually changed and are valid
			if (new_width > 0 && new_height > 0) {
				uint32_t current_width, current_height;
				comp_d3d11_target_get_dimensions(c->target, &current_width, &current_height);

				if (new_width != current_width || new_height != current_height) {
					U_LOG_I("Window resized: %ux%u -> %ux%u",
					        current_width, current_height, new_width, new_height);

					/*
					 * Make the resize atomic on the immediate context, for the
					 * same reason the #868 repaint replay is (see the
					 * mt_lock rationale in d3d11_repaint_thread).
					 * comp_d3d11_target_resize issues OMSetRenderTargets +
					 * ResizeBuffers on the context the APP also renders its
					 * scene on, from its own thread, without ever taking
					 * c->mutex — so c->mutex keeps the repaint out but not the
					 * app. D3D11 survives the interleave where VK does not, but
					 * "survives" is not "correct": it is the same window that
					 * sent the weave to the app's render target and left the
					 * swapchain black.
					 */
					// #918: under the split the swapchain lives on the runtime's own
					// output device, which the app never touches — so this bracket
					// (whose sole purpose is excluding the app's render thread from
					// the APP's immediate context) is unnecessary and wrong to take.
					const bool resize_needs_lock = !c->split_active && c->mt_lock != nullptr;
					if (resize_needs_lock) {
						c->mt_lock->Enter();
					}
					// Deliberately NOT wrapped in app_state_guard: the guard would
					// AddRef whatever RTV is bound, and if that is our own back
					// buffer, ResizeBuffers fails (it needs zero outstanding refs).
					xrt_result_t xret = comp_d3d11_target_resize(c->target, new_width, new_height);
					if (resize_needs_lock) {
						c->mt_lock->Leave();
					}
					if (xret != XRT_SUCCESS) {
						U_LOG_E("Failed to resize target");
						// Continue anyway, rendering will just be wrong size
					} else {
						// Update settings to reflect new size
						c->settings.preferred.width = new_width;
						c->settings.preferred.height = new_height;

						// Renderer atlas sizing is handled in layer_commit based on
						// the active rendering mode — no resize needed here.
						// (Matches GL compositor's begin_frame which does nothing.)
					}
				}
			}
		}
	}

	// Reset layer accumulator for this frame
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Clear layers
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// #868: layer_accum is about to be rewritten and the lock released — keep
	// the repaint thread out until layer_commit closes the window.
	c->repaint.app_frame_in_progress = true;
	comp_layer_accum_begin(&c->layer_accum, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_projection(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                   const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Store the layer data
	comp_layer_accum_projection(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                         struct xrt_device *xdev,
                                         struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                         struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                         const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Store the layer data (ignore depth for now)
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_quad(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_quad(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_cube(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_cube(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_cylinder(struct xrt_compositor *xc,
                                 struct xrt_device *xdev,
                                 struct xrt_swapchain *xsc,
                                 const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_cylinder(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_equirect1(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_equirect1(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_equirect2(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_equirect2(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_passthrough(struct xrt_compositor *xc,
                                    struct xrt_device *xdev,
                                    const struct xrt_layer_data *data)
{
	// Passthrough not supported on D3D11 native compositor
	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_window_space(struct xrt_compositor *xc,
                                     struct xrt_device *xdev,
                                     struct xrt_swapchain *xsc,
                                     const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_window_space(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

/*!
 * Local-2D layer (XR_DXR_local_3d_zone v3, #439 Phase 3) — accumulate only;
 * the D3D11 consumer (flatten + masked composite) is the Windows leg 1 of
 * docs/roadmap/unified-2d-3d-phase3-impl.md §7.
 */
static xrt_result_t
d3d11_compositor_layer_local_2d(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc,
                                const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_local_2d(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

/*!
 * 3D display zone layer (XR_DXR_display_zones, ADR-027) — multi-swapchain
 * accumulate like projection; consumed by the zones-frame branch of
 * layer_commit (sub-tile viewport scaled-blit into the window-spanning atlas).
 */
static xrt_result_t
d3d11_compositor_layer_zone_3d(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                               const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_zone_3d(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

/*!
 * Render the HUD overlay onto the back buffer (post-weave).
 * Uses CopySubresourceRegion for zero-shader simplicity.
 *
 * @p device / @p context are the device the staging texture is created on and
 * the context the upload + back-buffer blit run on — explicit (#918) so the HUD
 * follows the output target rather than the compositor's own device.
 */
static void
d3d11_render_hud_overlay(struct comp_d3d11_compositor *c,
                         ID3D11Device *device,
                         ID3D11DeviceContext *context,
                         bool weaving_done,
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
		comp_d3d11_renderer_get_view_dimensions(c->renderer, &render_w, &render_h);
	}
	uint32_t win_w = c->settings.preferred.width;
	uint32_t win_h = c->settings.preferred.height;
	if (c->target != nullptr) {
		comp_d3d11_target_get_dimensions(c->target, &win_w, &win_h);
	}

	// Get display physical dimensions from display processor
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;
	comp_d3d11_compositor_get_display_dimensions(&c->base.base, &disp_w_m, &disp_h_m);
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

	// Lazy-create the D3D11 staging texture
	if (!c->hud_initialized) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = hud_w;
		desc.Height = hud_h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = 0; // No shader binding needed, just copy source

		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &c->hud_texture);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create HUD texture: 0x%08x", hr);
			return;
		}
		c->hud_initialized = true;
		dirty = true; // Force initial upload
	}

	// Upload pixels to staging texture if changed
	if (dirty && c->hud_texture != nullptr) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		context->UpdateSubresource(c->hud_texture, 0, nullptr, u_hud_get_pixels(c->hud), hud_w * 4, 0);
	}

	// Blit HUD texture to bottom-left of back buffer
	if (c->hud_texture != nullptr && c->target != nullptr) {
		ID3D11Texture2D *back_buffer =
		    static_cast<ID3D11Texture2D *>(comp_d3d11_target_get_back_buffer(c->target));
		if (back_buffer != nullptr) {
			uint32_t hud_w = u_hud_get_width(c->hud);
			uint32_t hud_h = u_hud_get_height(c->hud);

			// Position at bottom-left with 10px margin
			uint32_t dst_x = 10;
			uint32_t dst_y = (win_h > hud_h + 10) ? (win_h - hud_h - 10) : 0;

			D3D11_BOX src_box = {0, 0, 0, hud_w, hud_h, 1};
			context->CopySubresourceRegion(back_buffer, 0, dst_x, dst_y, 0, c->hud_texture, 0, &src_box);
		}
	}
}

/*!
 * Crop atlas to content dimensions before passing to display processor.
 * Returns the SRV to pass to process_atlas() — either the original (zero-copy)
 * or a cropped intermediate texture.
 */
static void *
d3d11_crop_atlas_for_dp(struct comp_d3d11_compositor *c,
                        void *atlas_srv,
                        uint32_t content_w,
                        uint32_t content_h)
{
	// Get atlas texture dimensions from the SRV's underlying resource
	ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(atlas_srv);
	ID3D11Resource *resource = nullptr;
	srv->GetResource(&resource);
	ID3D11Texture2D *atlas_tex = static_cast<ID3D11Texture2D *>(resource);
	D3D11_TEXTURE2D_DESC atlas_desc;
	atlas_tex->GetDesc(&atlas_desc);

	// Zero-copy: atlas already matches content dimensions
	if (content_w == atlas_desc.Width && content_h == atlas_desc.Height) {
		atlas_tex->Release();
		return atlas_srv;
	}

	// Lazily (re)create intermediate texture at content dimensions
	if (c->dp_input_width != content_w || c->dp_input_height != content_h) {
		if (c->dp_input_srv != nullptr) {
			c->dp_input_srv->Release();
			c->dp_input_srv = nullptr;
		}
		if (c->dp_input_texture != nullptr) {
			c->dp_input_texture->Release();
			c->dp_input_texture = nullptr;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = content_w;
		desc.Height = content_h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = atlas_desc.Format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		HRESULT hr = d3d11_out_device(c)->CreateTexture2D(&desc, nullptr, &c->dp_input_texture);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create DP input texture %ux%u: 0x%lx", content_w, content_h, hr);
			atlas_tex->Release();
			return atlas_srv; // fallback to original
		}

		// Build an explicit SRV desc: a null desc is only valid over a fully
		// typed resource, and Unity D3D11 swapchains are TYPELESS — the null
		// path failed E_INVALIDARG every frame (#431). Select the typed UNORM
		// sibling (NOT the _SRGB one): in-process is ADR-021 Model A
		// passthrough, so the DP must sample the raw bytes — an _SRGB view
		// would decode sRGB->linear on sample and re-introduce the ~2.2x
		// too-dark half-conversion fixed by #407/#408/#409.
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = d3d_dxgi_format_to_unorm_sample(atlas_desc.Format);
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MostDetailedMip = 0;
		srv_desc.Texture2D.MipLevels = 1;
		hr = d3d11_out_device(c)->CreateShaderResourceView(c->dp_input_texture, &srv_desc,
		                                                   &c->dp_input_srv);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create DP input SRV: 0x%lx", hr);
			c->dp_input_texture->Release();
			c->dp_input_texture = nullptr;
			atlas_tex->Release();
			return atlas_srv;
		}

		c->dp_input_width = content_w;
		c->dp_input_height = content_h;
		U_LOG_I("D3D11 crop: created DP input texture %ux%u (atlas %ux%u)",
		        content_w, content_h, atlas_desc.Width, atlas_desc.Height);
	}

	// Copy content region from atlas to intermediate
	D3D11_BOX src_box = {0, 0, 0, content_w, content_h, 1};
	d3d11_out_context(c)->CopySubresourceRegion(c->dp_input_texture, 0, 0, 0, 0,
	                                            atlas_tex, 0, &src_box);
	atlas_tex->Release();

	return c->dp_input_srv;
}

// Copy the content region of the renderer's atlas (tile_columns × view_width
// by tile_rows × view_height — what the app actually wrote, same region the
// compositor crops and sends to the DP) into a staging texture, then write
// @p path as PNG. D3D11 renderer uses DXGI_FORMAT_R8G8B8A8_UNORM so no
// channel swap is needed.
// Read back the @p content_w × @p content_h top-left region of @p atlas_tex
// (clamped to its actual size) and write @p path as an opaque RGBA8 PNG. The
// source may be the renderer's composited atlas OR, in zero-copy present, the
// app's own swapchain image — both already hold the laid-out multi-view atlas.
//
// When @p dst_w / @p dst_h are non-zero and differ from the read-back region,
// the buffer is bilinear-resampled to (dst_w × dst_h) before the PNG write.
// This is used by the zero-copy path: an app may present a multi-view atlas at
// the mode NOMINAL dims while the runtime renders at the window-scaled dims, so
// the readback (nominal) must be scaled down to the content dims the DP sees
// (#431). A uniform resample is correct for an equal-sized tile grid — the tile
// boundary at source view_w maps exactly to the target view_w. Pass 0 to write
// the read-back region verbatim (no resample).
//
// The device/context the readback runs on are explicit parameters (#918) — the
// capture is not tied to the compositor's own device.
static bool
d3d11_capture_texture_to_png(ID3D11Device *device,
                             ID3D11DeviceContext *context,
                             ID3D11Texture2D *atlas_tex,
                             uint32_t content_w,
                             uint32_t content_h,
                             uint32_t dst_w,
                             uint32_t dst_h,
                             const char *path)
{
	if (atlas_tex == nullptr || content_w == 0 || content_h == 0) {
		return false;
	}

	D3D11_TEXTURE2D_DESC adesc;
	atlas_tex->GetDesc(&adesc);
	if (content_w > adesc.Width)  content_w = adesc.Width;
	if (content_h > adesc.Height) content_h = adesc.Height;

	D3D11_TEXTURE2D_DESC sd = adesc;
	sd.Width = content_w;
	sd.Height = content_h;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;

	ID3D11Texture2D *staging = nullptr;
	if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) || staging == nullptr) {
		return false;
	}

	D3D11_BOX src_box = {0, 0, 0, content_w, content_h, 1};
	context->CopySubresourceRegion(staging, 0, 0, 0, 0, atlas_tex, 0, &src_box);

	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
		staging->Release();
		return false;
	}

	// Repack into a tightly-packed RGBA8 buffer (the mapped staging is
	// READ-only, so we can't fix alpha in place) and force opaque: swapchain
	// alpha is undefined for display output, and left as-is the PNG renders
	// fully transparent/black (issue #425).
	bool ok = false;
	size_t tight_pitch = (size_t)content_w * 4;
	uint8_t *tight = (uint8_t *)malloc(tight_pitch * content_h);
	if (tight != nullptr) {
		const uint8_t *src = (const uint8_t *)m.pData;
		for (uint32_t y = 0; y < content_h; y++) {
			memcpy(tight + (size_t)y * tight_pitch, src + (size_t)y * m.RowPitch, tight_pitch);
		}
		u_image_force_opaque_rgba8(tight, content_w, content_h, tight_pitch);

		// Optional resample to (dst_w × dst_h) — see header comment (#431).
		if (dst_w != 0 && dst_h != 0 && (dst_w != content_w || dst_h != content_h)) {
			size_t dst_pitch = (size_t)dst_w * 4;
			uint8_t *out = (uint8_t *)malloc(dst_pitch * dst_h);
			if (out != nullptr) {
				// Bilinear, RGBA8. Sample centers map source<->dst so the
				// outer edges align (no half-texel shift across the grid).
				float sx = (dst_w > 1) ? (float)(content_w - 1) / (float)(dst_w - 1) : 0.0f;
				float sy = (dst_h > 1) ? (float)(content_h - 1) / (float)(dst_h - 1) : 0.0f;
				for (uint32_t dy = 0; dy < dst_h; dy++) {
					float fy = dy * sy;
					uint32_t y0 = (uint32_t)fy;
					uint32_t y1 = (y0 + 1 < content_h) ? y0 + 1 : y0;
					float wy = fy - (float)y0;
					for (uint32_t dx = 0; dx < dst_w; dx++) {
						float fx = dx * sx;
						uint32_t x0 = (uint32_t)fx;
						uint32_t x1 = (x0 + 1 < content_w) ? x0 + 1 : x0;
						float wx = fx - (float)x0;
						const uint8_t *p00 = tight + (size_t)y0 * tight_pitch + (size_t)x0 * 4;
						const uint8_t *p01 = tight + (size_t)y0 * tight_pitch + (size_t)x1 * 4;
						const uint8_t *p10 = tight + (size_t)y1 * tight_pitch + (size_t)x0 * 4;
						const uint8_t *p11 = tight + (size_t)y1 * tight_pitch + (size_t)x1 * 4;
						uint8_t *d = out + (size_t)dy * dst_pitch + (size_t)dx * 4;
						for (int ch = 0; ch < 4; ch++) {
							float top = p00[ch] * (1.0f - wx) + p01[ch] * wx;
							float bot = p10[ch] * (1.0f - wx) + p11[ch] * wx;
							d[ch] = (uint8_t)(top * (1.0f - wy) + bot * wy + 0.5f);
						}
					}
				}
				ok = stbi_write_png(path, (int)dst_w, (int)dst_h, 4, out, (int)dst_pitch) != 0;
				free(out);
			}
		} else {
			ok = stbi_write_png(path, (int)content_w, (int)content_h, 4, tight, (int)tight_pitch) != 0;
		}
		free(tight);
	}

	context->Unmap(staging, 0);
	staging->Release();
	return ok;
}

// Resolve the active content region from the frame's effective layout (#542:
// what the passes actually painted — submission-derived), falling back to the
// renderer's mode layout before the first commit. Returns false if the
// renderer hasn't been sized yet.
static bool
d3d11_compositor_content_dims(struct comp_d3d11_compositor *c, uint32_t *out_w, uint32_t *out_h)
{
	if (c->renderer == nullptr) {
		return false;
	}
	if (c->eff_layout.views > 0 && c->eff_layout.tile_w > 0 && c->eff_layout.tile_h > 0) {
		*out_w = c->eff_layout.cols * c->eff_layout.tile_w;
		*out_h = c->eff_layout.rows * c->eff_layout.tile_h;
		return true;
	}
	uint32_t tile_columns = 1, tile_rows = 1;
	comp_d3d11_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);
	uint32_t view_w = 0, view_h = 0;
	comp_d3d11_renderer_get_view_dimensions(c->renderer, &view_w, &view_h);
	if (tile_columns == 0 || tile_rows == 0 || view_w == 0 || view_h == 0) {
		return false;
	}
	*out_w = tile_columns * view_w;
	*out_h = tile_rows * view_h;
	return true;
}

// u_capture_dims provider: report the renderer's CURRENT window-scaled per-view
// dims + tile layout so xrCaptureAtlasDXR can fill XrAtlasCaptureResultDXR with
// what the capture actually writes, not the nominal system info (#431).
static bool
d3d11_compositor_capture_dims_provider(void *userdata,
                                       uint32_t *out_view_w,
                                       uint32_t *out_view_h,
                                       uint32_t *out_tile_cols,
                                       uint32_t *out_tile_rows)
{
	struct comp_d3d11_compositor *c = static_cast<struct comp_d3d11_compositor *>(userdata);
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
	comp_d3d11_renderer_get_view_dimensions(c->renderer, &vw, &vh);
	comp_d3d11_renderer_get_tile_layout(c->renderer, &cols, &rows);
	if (vw == 0 || vh == 0) {
		return false;
	}
	*out_view_w = vw;
	*out_view_h = vh;
	*out_tile_cols = cols;
	*out_tile_rows = rows;
	return true;
}

// Capture from the renderer's composited atlas (non-zero-copy frames).
static bool
d3d11_compositor_capture_atlas_to_png(struct comp_d3d11_compositor *c, const char *path)
{
	ID3D11Texture2D *atlas_tex = static_cast<ID3D11Texture2D *>(
	    comp_d3d11_renderer_get_atlas_texture(c->renderer));
	uint32_t content_w = 0, content_h = 0;
	if (atlas_tex == nullptr || !d3d11_compositor_content_dims(c, &content_w, &content_h)) {
		return false;
	}
	return d3d11_capture_texture_to_png(c->device, c->context, atlas_tex, content_w, content_h, 0, 0, path);
}

// Service a pending MCP capture_frame request — thin wrapper around
// Run the capture readback if the per-frame intent matches @p mode_filter.
// Mirrors vk_native_dispatch_capture / gl_compositor_dispatch_capture.
static void
d3d11_compositor_dispatch_capture(struct comp_d3d11_compositor *c, uint32_t mode_filter)
{
	if (!u_capture_intent_should_capture(&c->capture_intent, mode_filter)) {
		return;
	}
	bool ok = d3d11_compositor_capture_atlas_to_png(c, c->capture_intent.path);
	if (ok) {
		U_LOG_I("Atlas captured (mode=%u) to %s",
		        c->capture_intent.mode, c->capture_intent.path);
	} else {
		U_LOG_W("Atlas capture failed (mode=%u path=%s)",
		        c->capture_intent.mode, c->capture_intent.path);
	}
	u_capture_intent_complete(&c->capture_intent, &c->mcp_capture, ok);
}

// Zero-copy capture: in zero-copy present the app's own swapchain image IS the
// laid-out multi-view atlas (that's why it qualifies), and the normal composite
// passes — and their PROJECTION_ONLY/POST_COMPOSE dispatch points — are skipped,
// so a pending capture would otherwise silently produce no PNG (issue #425: the
// Unity path submits a single double-wide projection layer that hits zero-copy,
// unlike the native/Unreal apps). Read directly from the same swapchain SRV the
// DP receives — NOT by forcing a re-composite, which re-projects the already-
// tiled texture and corrupts it. Stage selector is irrelevant here: zero-copy
// has a single projection layer and no window-space layers, so PROJECTION_ONLY
// and POST_COMPOSE are identical.
static void
d3d11_compositor_dispatch_capture_zerocopy(struct comp_d3d11_compositor *c, void *zc_srv)
{
	if (!c->capture_intent.pending || zc_srv == nullptr) {
		return;
	}
	ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(zc_srv);
	ID3D11Resource *resource = nullptr;
	srv->GetResource(&resource);
	ID3D11Texture2D *zc_tex = static_cast<ID3D11Texture2D *>(resource);

	// Read back the whole swapchain texture: zero-copy is only eligible when the
	// app's swapchain laid-out rects tile it exactly (u_tiling_can_zero_copy), so
	// the entire texture IS the multi-view atlas.
	//
	// For an EXTENSION app, the swapchain may be presented at the mode NOMINAL
	// dims (e.g. a Unity backend composes a double-wide 3840×1080 atlas) while the
	// runtime renders at the window-scaled content dims (e.g. 2400×1080) — so we
	// resample the readback down to the renderer's current content dims, matching
	// the PNG D3D12 produces for the same window (#431). A plain crop would slice
	// the larger tiles; a uniform resample preserves the equal-sized tile grid.
	//
	// For a LEGACY app, the renderer view dims are the 2D/3D compromise size
	// (smaller than the app's real per-view render), so resampling to them would
	// needlessly downscale — keep the verbatim full-swapchain dump (#425).
	bool ok = false;
	if (zc_tex != nullptr) {
		D3D11_TEXTURE2D_DESC zdesc;
		zc_tex->GetDesc(&zdesc);
		uint32_t dst_w = 0, dst_h = 0;
		if (!c->legacy_app_tile_scaling) {
			uint32_t content_w = 0, content_h = 0;
			if (d3d11_compositor_content_dims(c, &content_w, &content_h)) {
				dst_w = content_w;
				dst_h = content_h;
			}
		}
		ok = d3d11_capture_texture_to_png(c->device, c->context, zc_tex, zdesc.Width, zdesc.Height, dst_w,
		                                  dst_h, c->capture_intent.path);
	}
	if (resource != nullptr) {
		resource->Release();
	}
	if (ok) {
		U_LOG_I("Atlas captured (zero-copy, mode=%u) to %s",
		        c->capture_intent.mode, c->capture_intent.path);
	} else {
		U_LOG_W("Atlas capture failed (zero-copy, mode=%u path=%s)",
		        c->capture_intent.mode, c->capture_intent.path);
	}
	u_capture_intent_complete(&c->capture_intent, &c->mcp_capture, ok);
}

/*!
 * #868 diag (DXR_WEAVE_REPAINT_HASH=1): cheap content hash of a rendered target.
 *
 * The PNG-pair instrument cannot settle whether a repaint reproduces the app
 * frame, because encoding a PNG takes 70-150 ms and the scene moves in the gap —
 * on D3D11 the residual difference is the same size as that animation artifact.
 * This hashes on the frame path instead, so an app weave and the repaint that
 * follows it can be compared with NOTHING in between.
 *
 * Subsamples every 4th row and every 4th pixel: enough to catch a structurally
 * different frame, cheap enough to run every weave while probing. Still a
 * staging copy + Map, so it is env-gated and never on by default.
 */
static uint64_t
d3d11_hash_target(ID3D11Device *device,
                  ID3D11DeviceContext *context,
                  ID3D11Texture2D *tex,
                  uint32_t w,
                  uint32_t h,
                  uint8_t **out_rgba)
{
	if (tex == nullptr || w == 0 || h == 0) {
		return 0;
	}
	D3D11_TEXTURE2D_DESC td;
	tex->GetDesc(&td);
	if (w > td.Width)  w = td.Width;
	if (h > td.Height) h = td.Height;

	D3D11_TEXTURE2D_DESC sd = td;
	sd.Width = w;
	sd.Height = h;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;

	ID3D11Texture2D *staging = nullptr;
	if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) || staging == nullptr) {
		return 0;
	}
	D3D11_BOX box = {0, 0, 0, w, h, 1};
	context->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);

	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
		staging->Release();
		return 0;
	}
	uint64_t hsh = 1469598103934665603ull; // FNV-1a offset basis
	const uint8_t *base = (const uint8_t *)m.pData;
	if (out_rgba != nullptr) {
		// Keep the pixels so a mismatch can dump BOTH sides of the pair. The
		// app frame is gone by the time the repaint discovers the mismatch, so
		// it has to be retained up front.
		free(*out_rgba);
		*out_rgba = (uint8_t *)malloc((size_t)w * h * 4);
		if (*out_rgba != nullptr) {
			for (uint32_t y = 0; y < h; y++) {
				memcpy(*out_rgba + (size_t)y * w * 4, base + (size_t)y * m.RowPitch,
				       (size_t)w * 4);
			}
		}
	}
	for (uint32_t y = 0; y < h; y += 4) {
		const uint8_t *row = base + (size_t)y * m.RowPitch;
		for (uint32_t x = 0; x < w; x += 4) {
			// RGB only — swapchain alpha is undefined for display output.
			for (int k = 0; k < 3; k++) {
				hsh ^= (uint64_t)row[(size_t)x * 4 + k];
				hsh *= 1099511628211ull;
			}
		}
	}
	context->Unmap(staging, 0);
	staging->Release();
	return hsh;
}

/*!
 * #868: record the display-processor weave for the current frame state.
 * Shared by layer_commit and the repaint thread, so a repaint is constructed
 * exactly like the app frame it stands in for.
 *
 * The replay reads ONLY compositor-owned resources: the renderer atlas (or its
 * crop), the cached 2D-under backdrop and the cached zone/Local2D mask. It never
 * re-reads an app-owned texture and never ticks a per-frame state machine — see
 * d3d11_composite_zone_mask, and the D3D12 leg for what happens when it does.
 */
static bool
d3d11_dp_weave(struct comp_d3d11_compositor *c, bool is_repaint)
{
	const bool zero_copy = c->repaint.zero_copy;
	void *zc_srv = c->repaint.zc_srv;
	const struct u_canvas_rect eff_canvas = c->repaint.canvas;

	// Re-bind target RTV before DP — the renderer may have changed it to the atlas RTV.
	// The DP writes to the currently bound render target (see xrt_display_processor_d3d11.h).
	comp_d3d11_target_bind(c->target);

	// #918: under the split the atlas the DP samples is an OUTPUT-device egress
	// texture, and which slot it is can only be decided after the late-weave
	// pace below — so it is resolved further down rather than here.
	void *atlas_srv = c->split_active ? nullptr
	                                  : (zero_copy ? zc_srv : comp_d3d11_renderer_get_atlas_srv(c->renderer));

	// #542: the DP gets the frame's EFFECTIVE content layout — the grid
	// the passes above actually painted (== the mode layout for matched
	// submissions) — not the mode layout.
	uint32_t view_width = c->eff_layout.tile_w;
	uint32_t view_height = c->eff_layout.tile_h;
	uint32_t tile_columns = c->eff_layout.cols;
	uint32_t tile_rows = c->eff_layout.rows;

	/*
	 * #876 GEOMETRY detector.
	 *
	 * The reported artefact is a DOUBLE / zoomed cube, which is a weave
	 * geometry or interlace-phase error — the weave ran with the wrong tile
	 * stride, not with wrong pixels. Neither previous instrument could see it:
	 * the adjacent-pair hash is blind when the app frame and its repaint AGREE
	 * (measured 2399/2400), and a luminance detector is blind because a doubled
	 * image has essentially unchanged brightness.
	 *
	 * So compare the geometry ITSELF. A repaint must weave with exactly the
	 * layout of the app frame it stands in for; any difference is the defect,
	 * and it shows up here whether or not the resulting pixels differ.
	 */
	{
		const struct u_canvas_rect ec = c->repaint.canvas;
		const uint64_t geom = ((uint64_t)view_width << 48) ^ ((uint64_t)view_height << 32) ^
		                      ((uint64_t)tile_columns << 24) ^ ((uint64_t)tile_rows << 16) ^
		                      ((uint64_t)(uint32_t)ec.w << 8) ^ (uint64_t)(uint32_t)ec.h ^
		                      ((uint64_t)(uint32_t)ec.x << 40) ^ ((uint64_t)(uint32_t)ec.y << 20) ^
		                      ((uint64_t)(ec.valid ? 1 : 0) << 60) ^ ((uint64_t)(zero_copy ? 1 : 0) << 61);
		if (!is_repaint) {
			// Baseline: what the app frame just wove with.
			if (c->repaint.geom_app != 0 && geom != c->repaint.geom_app) {
				c->repaint.geom_app_changes++;
				U_LOG_W("#876 GEOM: APP layout changed -> %ux%u tiles %ux%u canvas %d,%d %ux%u "
				        "valid=%d zc=%d (app-side changes %llu)",
				        view_width, view_height, tile_columns, tile_rows, ec.x, ec.y, ec.w, ec.h,
				        (int)ec.valid, (int)zero_copy,
				        (unsigned long long)c->repaint.geom_app_changes);
			}
			c->repaint.geom_app = geom;
		} else if (c->repaint.geom_app != 0 && geom != c->repaint.geom_app) {
			c->repaint.geom_mismatches++;
			U_LOG_W("#876 GEOM MISMATCH: repaint wove %ux%u tiles %ux%u canvas %d,%d %ux%u "
			        "valid=%d zc=%d — differs from the app frame it replays (total %llu)",
			        view_width, view_height, tile_columns, tile_rows, ec.x, ec.y, ec.w, ec.h,
			        (int)ec.valid, (int)zero_copy,
			        (unsigned long long)c->repaint.geom_mismatches);
		}
	}


	// Crop the renderer's atlas to content dims before passing to the DP;
	// never crop a zero-copy atlas — the app's swapchain is already a
	// complete, correctly-strided multi-view atlas, and a top-left crop to
	// the window-scaled width would slice the tiles and weave them
	// misaligned (big disparity). See the offscreen path above (#431).
	uint32_t content_w = tile_columns * view_width;
	uint32_t content_h = tile_rows * view_height;
	if (!zero_copy && !c->split_active) {
		atlas_srv = d3d11_crop_atlas_for_dp(c, atlas_srv, content_w, content_h);
	}

	uint32_t target_width, target_height;
	comp_d3d11_target_get_dimensions(c->target, &target_width, &target_height);

	// #491 part 3 — flatten the 2D-under layers PRE-weave and hand them to
	// the DP (it composites `backdrop over captured-desktop` under the 3D).
	// The flatten binds/unbinds its own RTV, so re-bind the target RTV after
	// (the DP writes to the currently bound target). NULL ⟹ no under-layers.
	// The backdrop flatten samples the APP'S OWN Local2D under-layer swapchain
	// images, so a repaint reuses what the last app frame produced rather than
	// re-reading textures the app has since reacquired.
	//
	//
	// #918 Phase 2a: under the split the flatten happens in the DEPOSIT half
	// (before the bridge submit, so the plane rides this frame's slot) and the
	// SRV handed to the DP is the BACKDROP PLANE that landed with the slot being
	// woven — resolved below, once the slot is known. The app-device SRV cached
	// in c->repaint.backdrop_srv lives on the wrong adapter and is never used
	// there; it stays exactly as-is on the non-split path.
	if (!c->split_active) {
		if (!is_repaint) {
			uint32_t bd_w = 0, bd_h = 0;
			c->repaint.backdrop_srv =
			    d3d11_flatten_backdrop_2d(c, target_width, target_height, &bd_w, &bd_h);
			c->repaint.backdrop_w = bd_w;
			c->repaint.backdrop_h = bd_h;
		}
		xrt_display_processor_d3d11_set_background_2d(c->display_processor, c->repaint.backdrop_srv,
		                                              c->repaint.backdrop_w, c->repaint.backdrop_h);
		if (c->repaint.backdrop_srv != nullptr) {
			comp_d3d11_target_bind(c->target);
		}
	}

	// Late-weave pacing + weave-latency harness mark (env-gated no-ops). A
	// repaint paced itself unlocked and stays out of the governor EMA and the
	// #867 prediction ledger.
	if (is_repaint) {
		comp_d3d11_target_weave_mark_repaint(c->target);
	} else {
		comp_d3d11_target_weave_mark(c->target, c->last_display_time_ns);
	}

	/*
	 * #918: pick the egress slot the DP will sample, AFTER the late-weave pace
	 * above — a slot that landed during the sleep is the freshest one available
	 * and picking before the sleep would throw it away.
	 *
	 * An app frame picks OPPORTUNISTICALLY: the newest slot whose consumer copy
	 * has already completed, CPU-verified, so this thread never waits. A repaint
	 * re-weaves the slot the last app frame published — zero bridge traffic per
	 * tick, which is the whole point of the split (today's placement re-crosses
	 * the entire woven frame on every repaint).
	 */
	int32_t weave_slot = -1;
	if (c->split_active) {
		const uint64_t want_gen = c->split_layout_gen;
		int32_t slot;
		if (is_repaint) {
			slot = comp_d3d11_xbridge_get_weave_slot(c->xbridge);
		} else {
			slot = comp_d3d11_xbridge_pick_slot(c->xbridge, want_gen);
			if (slot < 0) {
				/*
				 * #918 R1 — the transition frame. A layout change rebuilt
				 * the ring, so nothing of the CURRENT generation has landed
				 * yet, and holding is not the neutral choice it looks like:
				 * FLIP_DISCARD leaves the panel showing the frame woven for
				 * the mode the display has just left. Take the slot whose
				 * copy is still in flight and let the GPU-side wait below
				 * order the weave behind it — the frame goes out one
				 * transition-only wait late instead of one mode wrong.
				 */
				slot = comp_d3d11_xbridge_pick_inflight_slot(c->xbridge, want_gen);
				if (slot >= 0) {
					c->split_inflight_weaves++;
				}
			}
			if (slot >= 0) {
				comp_d3d11_xbridge_set_weave_slot(c->xbridge, slot);
			} else {
				// Warmup: nothing has crossed yet. Keep weaving whatever the
				// last good slot was rather than handing the DP a null atlas.
				// (The generation test below vets it just the same.)
				slot = comp_d3d11_xbridge_get_weave_slot(c->xbridge);
			}
		}
		/*
		 * #918 R1 — a slot may only be woven under the generation that produced
		 * it. "The atlas recipe IS the mode": non-split the weave consumes the
		 * atlas its own frame composited, so layout and recipe change together
		 * by construction; under the split the weave consumes a slot some frame
		 * filled, and the two can differ across a mode switch.
		 *
		 * Today the ring happens to be rebuilt at every switch (the content box
		 * really does change — 2 x 640x360 in 3D against 1 x 1280x720 in 2D on
		 * the reference panel) which empties it and hides the exposure. The R2
		 * hysteresis below deliberately STOPS rebuilding per size, so surviving
		 * slots become reachable across a layout change and this test is what
		 * keeps a superseded recipe from ever reaching the DP.
		 *
		 * The degrade of last resort is the F4 one — weave nothing, skip the
		 * present. But note what that costs, because it is the actual R1 defect:
		 * a skipped present leaves FLIP_DISCARD showing the frame woven for the
		 * mode the display has just LEFT. On 3D->2D that is an interlaced frame
		 * under a disabled lens, whose two eye-column sets become visible side by
		 * side — the offset cube. (2D->3D holds a flat frame under an enabled
		 * lens, which is why only one direction was ever reported.) So the pick
		 * above prefers the in-flight slot of the CURRENT generation and skips
		 * only when there is genuinely nothing of this recipe anywhere.
		 */
		if (slot >= 0) {
			uint64_t slot_gen = 0;
			uint32_t slot_cw = 0, slot_ch = 0;
			if (!comp_d3d11_xbridge_slot_layout(c->xbridge, slot, &slot_gen, &slot_cw, &slot_ch) ||
			    slot_gen != want_gen) {
				c->split_stale_refusals++;
				// Once per TRANSITION — this names the exact frame that used
				// to go out with the wrong geometry.
				if (c->split_stale_logged_gen != want_gen) {
					c->split_stale_logged_gen = want_gen;
					U_LOG_W(
					    "#918 R1: %s refused egress slot %d — its %ux%u content was "
					    "composited under layout generation %llu, the DP is now on "
					    "generation %llu (%ux%u tiles of %ux%u). Weaving it is the "
					    "one-frame offset; presenting nothing holds the last good frame.",
					    is_repaint ? "the repaint tick" : "the app frame", (int)slot, slot_cw,
					    slot_ch, (unsigned long long)slot_gen, (unsigned long long)want_gen,
					    tile_columns, tile_rows, view_width, view_height);
				}
				slot = -1;
			} else if (slot_cw > 0 && slot_ch > 0 && tile_columns > 0 && tile_rows > 0 &&
			           (slot_cw != content_w || slot_ch != content_h)) {
				/*
				 * #918 R2: same recipe, but the slot was painted at a
				 * different SCALE — the resize hysteresis keeps a
				 * worst-case ring through a drag, so what landed is a
				 * frame behind the window. Weave it with ITS OWN content
				 * box: the DP derives its tile stride from the atlas
				 * width, so cropping to the CURRENT box would slice every
				 * tile at the wrong offset. The canvas stays the live one,
				 * so the weave phase tracks the window; only the content
				 * scale lags, by the one frame the bridge always costs.
				 */
				view_width = slot_cw / tile_columns;
				view_height = slot_ch / tile_rows;
				content_w = view_width * tile_columns;
				content_h = view_height * tile_rows;
			}
		}
		if (slot < 0) {
			// Pre-first-crossing, or the ring was just reallocated. The target
			// was acquired (== cleared to black); returning false is what tells
			// layer_commit / the repaint tick to SKIP the present, so the panel
			// holds the last shown frame instead of flashing black (#918 F4).
			return false;
		}
		/*
		 * #918 F7 (b): a repaint re-weaves a slot it did not pick, so nothing
		 * has verified that slot's consumer copy landed. The GPU-side wait does
		 * it when the output device could open the consumer fence; when it could
		 * not, check CPU-side and bail rather than weave a slot the consumer is
		 * mid-way through rewriting. (This runs on the repaint thread, never on
		 * the app's render thread, and it is a single poll — not a wait.)
		 */
		if (is_repaint && !comp_d3d11_xbridge_slot_ready(c->xbridge, slot)) {
			return false;
		}
		comp_d3d11_xbridge_gpu_wait_slot(c->xbridge, slot);
		atlas_srv = comp_d3d11_xbridge_get_srv(c->xbridge, slot);
		if (atlas_srv == nullptr) {
			return false;
		}
		weave_slot = slot;

		/*
		 * #918 Phase 2a — the 2D-under backdrop, from the slot. Same GPU wait,
		 * same fence, same seq as the atlas above, so the DP composites this
		 * frame's backdrop under this frame's 3D and never last frame's. A
		 * repaint tick reads it out of the published slot with ZERO bridge
		 * traffic, which is the whole point of the split.
		 */
		{
			struct comp_d3d11_xbridge_recipe brec = {};
			ID3D11ShaderResourceView *bd_srv = nullptr;
			if (comp_d3d11_xbridge_slot_recipe(c->xbridge, slot, &brec) && brec.bd_w > 0 && brec.bd_h > 0 &&
			    (brec.plane_valid & (1u << COMP_D3D11_XBRIDGE_PLANE_BACKDROP)) != 0) {
				// Region-sized view: the DP's contract is an SRV plus the
				// backdrop's real dims, and the plane is panel-sized. The copy
				// is seq-gated on the slot's plane generation (#918 review F8),
				// so an unchanged backdrop under a repaint costs nothing.
				const uint64_t bd_seq = brec.plane_seq[COMP_D3D11_XBRIDGE_PLANE_BACKDROP];
				bd_srv = d3d11_plane_dp_view(
				    c, &c->dp_bd_view,
				    comp_d3d11_xbridge_get_plane_srv(c->xbridge, slot,
				                                     COMP_D3D11_XBRIDGE_PLANE_BACKDROP, bd_seq),
				    bd_seq, brec.bd_w, brec.bd_h, DXGI_FORMAT_R8G8B8A8_UNORM, "2D-under backdrop");
			}
			xrt_display_processor_d3d11_set_background_2d(c->display_processor, bd_srv,
			                                              bd_srv != nullptr ? brec.bd_w : 0,
			                                              bd_srv != nullptr ? brec.bd_h : 0);
			if (bd_srv != nullptr) {
				comp_d3d11_target_bind(c->target);
			}
		}
		/*
		 * Crop only when the slot's content does not already fill it. A
		 * content-sized ring (the steady state) needs none — cropping an
		 * already-cropped image would be a second crop. A worst-case ring — the
		 * Stage-B fallback, and every frame of a resize drag under the R2
		 * hysteresis — needs one, on the OUTPUT device, to the box that slot was
		 * painted at, which the block above already resolved.
		 */
		uint32_t eg_w = 0, eg_h = 0;
		comp_d3d11_xbridge_get_egress_dims(c->xbridge, &eg_w, &eg_h);
		if (content_w != eg_w || content_h != eg_h) {
			atlas_srv = d3d11_crop_atlas_for_dp(c, atlas_srv, content_w, content_h);
		}

		// Weave cadence — the #918 R2 evidence. A gap far past a frame period
		// means weaves are STARVING (the panel is holding), which is what a
		// per-size egress realloc used to do to every frame of a resize.
		{
			const uint64_t now = os_monotonic_get_ns();
			if (c->split_weave_last_ns != 0) {
				const uint64_t gap = now - c->split_weave_last_ns;
				if (gap > c->split_weave_gap_max_ns) {
					c->split_weave_gap_max_ns = gap;
				}
			}
			c->split_weave_last_ns = now;
			if (c->split_diag && now - c->split_diag_logged_ns >= U_TIME_1S_IN_NS) {
				const double secs = (c->split_diag_logged_ns != 0)
				                        ? (double)(now - c->split_diag_logged_ns) / 1.0e9
				                        : 1.0;
				c->split_diag_logged_ns = now;
				/*
				 * #918 Phase 2a — the BANDWIDTH evidence. Per-plane bytes/s and
				 * change-skips beside the atlas, because the acceptance number
				 * ("< 2.0 GB/s sustained on the 4K full-screen backdrop case") is
				 * a per-plane question and a whole-transport total answers it
				 * without saying which plane to fix.
				 */
				const double atlas_gbs =
				    (double)comp_d3d11_xbridge_take_atlas_bytes(c->xbridge) / secs / 1.0e9;
				/*
				 * #918 review D8: sized by the constant, not by the 3 that
				 * happened to be true. A fourth plane used to compile clean and
				 * vanish from the line — and since take_plane_stats is the only
				 * thing that drains a plane's rolling byte window, an unvisited
				 * index would never drain it either.
				 */
				char planes[512];
				size_t off = 0;
				double total_gbs = atlas_gbs;
				for (uint32_t p = 0; p < COMP_D3D11_XBRIDGE_PLANE_COUNT; p++) {
					uint64_t pb = 0, pc = 0, ps = 0;
					bool half = false;
					comp_d3d11_xbridge_take_plane_stats(c->xbridge, p, &pb, &pc, &ps, &half);
					const double gbs = (double)pb / secs / 1.0e9;
					total_gbs += gbs;
					const int n = snprintf(planes + off, sizeof(planes) - off,
					                       "%s %.3f GB/s (%llu copies, %llu skips%s) |",
					                       comp_d3d11_xbridge_plane_label(p), gbs,
					                       (unsigned long long)(pc - c->split_plane_copies_prev[p]),
					                       (unsigned long long)(ps - c->split_plane_skips_prev[p]),
					                       half ? " HALF-RATE" : "");
					if (n > 0 && (size_t)n < sizeof(planes) - off) {
						off += (size_t)n;
					}
					c->split_plane_copies_prev[p] = pc;
					c->split_plane_skips_prev[p] = ps;
				}
				/*
				 * #918 review F8 — the DP SIDEBAND leg, which used to be
				 * invisible here. It runs on the output device (so it is not
				 * bridge traffic) but it is real per-tick bandwidth, and a
				 * repaint with static content must show ~0 copies.
				 */
				const double side_gbs = (double)c->split_sideband_bytes / secs / 1.0e9;
				const uint64_t side_copies = c->split_sideband_copies;
				const uint64_t side_skips = c->split_sideband_skips;
				c->split_sideband_bytes = 0;
				c->split_sideband_copies = 0;
				c->split_sideband_skips = 0;
				U_LOG_W(
				    "#918 DIAG: weave %ux%u tiles %ux%u (ring %ux%u) gap_max=%.1f ms "
				    "stale-refusals=%llu recipe-refusals=%llu skipped-presents=%llu | "
				    "atlas %.3f GB/s |%s sideband %.3f GB/s (%llu copies, %llu skips) | "
				    "TOTAL %.3f GB/s (+sideband %.3f)",
				    view_width, view_height, tile_columns, tile_rows, eg_w, eg_h,
				    (double)c->split_weave_gap_max_ns / 1.0e6,
				    (unsigned long long)c->split_stale_refusals,
				    (unsigned long long)c->split_recipe_refusals,
				    (unsigned long long)c->split_skip_present, atlas_gbs, planes, side_gbs,
				    (unsigned long long)side_copies, (unsigned long long)side_skips, total_gbs,
				    total_gbs + side_gbs);
			}
		}
	}

	// Timing feedback: hand the DP last frame's MEASURED weave→scanout
	// residual so the vendor eye predictor runs with an exact horizon
	// (0 = unknown ⟹ DP falls back to its heuristic).
	{
		const uint64_t wl_measured = comp_d3d11_target_get_measured_weave_ns(c->target);
		static bool wl_logged = false;
		if (!wl_logged && wl_measured > 0) {
			wl_logged = true;
			U_LOG_W("Weave timing feedback engaged: measured=%llu us, period=%llu us",
			        (unsigned long long)(wl_measured / 1000),
			        (unsigned long long)((uint64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate) / 1000));
		}
		xrt_display_processor_d3d11_set_frame_timing(
		    c->display_processor, wl_measured,
		    (uint64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate));
	}

	xrt_display_processor_d3d11_process_atlas(
	    c->display_processor, d3d11_out_context(c), atlas_srv, view_width, view_height,
	    tile_columns, tile_rows, DXGI_FORMAT_R8G8B8A8_UNORM, target_width, target_height,
	    eff_canvas.valid ? eff_canvas.x : 0,
	    eff_canvas.valid ? eff_canvas.y : 0,
	    eff_canvas.valid ? eff_canvas.w : 0,
	    eff_canvas.valid ? eff_canvas.h : 0);

	// #439 Phase 1: an authored zone mask (XR_DXR_local_3d_zone) or a
	// Local2D layer composites the 2D/3D regions of the DXGI back buffer.
	// The downstream CopyResource/CopySubresourceRegion (line ~1500)
	// propagates the composite into c->shared_texture for _texture-mode
	// read-back. No-op when the frame carries neither.
	ID3D11Texture2D *back_buffer_2d = static_cast<ID3D11Texture2D *>(
	    comp_d3d11_target_get_back_buffer(c->target));
	/*
	 * DXR_WEAVE_PROBE=1 — one-shot dump of the back buffer as the DP left it,
	 * BEFORE the masked composite. The composite writes every pixel of the
	 * region, so a wrong final frame is ambiguous between "the weave produced
	 * nothing" and "the composite blended it away"; this separates the two in one
	 * run. Env-gated and one-shot — nothing on the frame path when unset.
	 */
	{
		static int weave_probe = -1;
		if (weave_probe < 0) {
			const char *e = getenv("DXR_WEAVE_PROBE");
			weave_probe = (e != nullptr && e[0] == '1') ? 1 : 0;
		}
		const char *tmpp = getenv("TEMP");
		if (weave_probe == 1 && back_buffer_2d != nullptr && tmpp != nullptr) {
			/*
			 * Armed by the SAME `dxr_woven_trigger` file as the post-composite
			 * capture below, and deliberately does not consume it — the post
			 * block does that. The pair is then the same frame either side of the
			 * composite, which is the only way to read it: two dumps seconds
			 * apart would differ by scene motion and prove nothing.
			 */
			char trig[512];
			snprintf(trig, sizeof(trig), "%s\\dxr_woven_trigger", tmpp);
			FILE *tf = fopen(trig, "rb");
			if (tf != nullptr) {
				fclose(tf);
				char out[512];
				snprintf(out, sizeof(out), "%s\\dxr_weave_precomposite_%s.png", tmpp,
				         is_repaint ? "repaint" : "app");
				bool ok = d3d11_capture_texture_to_png(d3d11_out_device(c), d3d11_out_context(c),
				                                       back_buffer_2d, target_width, target_height, 0,
				                                       0, out);
				U_LOG_W("#918 WEAVE PROBE: pre-composite back buffer %s -> %s", ok ? "OK" : "FAILED",
				        out);
			}
		}
	}
	d3d11_composite_zone_mask(c, /*is_repaint=*/true, /*prepare_only=*/false, back_buffer_2d, target_width,
	                          target_height, &eff_canvas, weave_slot);

	/*
	 * #868 diag: adjacent-pair hash comparison. A repaint should reproduce the
	 * app frame it stands in for, pixel for pixel, whenever the viewer has not
	 * moved. Comparing ONLY when no app weave has intervened (app_seq unchanged)
	 * is what makes a mismatch meaningful — otherwise a newer atlas, which is
	 * correct behaviour, reads as a defect.
	 */
	if (c->repaint.hash_probe == 1 && back_buffer_2d != nullptr) {
		uint8_t *rp_px = nullptr;
		const uint64_t hsh =
		    d3d11_hash_target(d3d11_out_device(c), d3d11_out_context(c), back_buffer_2d, target_width,
		                      target_height, is_repaint ? &rp_px : &c->repaint.app_px);

		/*
		 * #876 ABSOLUTE dropout detector — runs on EVERY weave, app or repaint.
		 *
		 * The adjacent-pair hash above can only see the app frame and its
		 * repaint DISAGREEING. If a repaint faithfully replays a frame that is
		 * already wrong, both are equally wrong and the pair reports
		 * "reproduced" — measured at 2399/2400 agreement while the panel was
		 * visibly flickering. So pair-comparison is blind to this class by
		 * construction, and only an absolute measure can see it.
		 *
		 * Track a per-cell luminance EMA on an 8x8 grid and flag any cell that
		 * was reliably lit and then collapses. That catches a zone dropping out
		 * regardless of which path produced the frame.
		 */
		{
			/*
			 * Warm-up gate. The first couple of seconds are content fading in
			 * while the EMA has no baseline yet — that produced 437 "dropouts"
			 * in 2 s and then silence for the next 58, i.e. pure noise that
			 * buried the once-per-10s event actually being hunted. Arm only
			 * once the baseline means something.
			 */
			c->repaint.detector_frames++;
			const uint8_t *px = is_repaint ? rp_px : c->repaint.app_px;
			const bool armed = c->repaint.detector_frames > 300;
			if (px != nullptr) {
				const uint32_t GX = 8, GY = 8;
				const uint32_t stride = target_width * 4;
				for (uint32_t gj = 0; gj < GY; gj++) {
					for (uint32_t gi = 0; gi < GX; gi++) {
						const uint32_t y0 = gj * target_height / GY;
						const uint32_t y1 = (gj + 1) * target_height / GY;
						const uint32_t x0 = gi * target_width / GX;
						const uint32_t x1 = (gi + 1) * target_width / GX;
						uint64_t sum = 0;
						uint32_t n = 0;
						for (uint32_t y = y0; y < y1; y += 4) {
							for (uint32_t x = x0; x < x1; x += 4) {
								const uint32_t o = y * stride + x * 4;
								sum += (uint64_t)px[o] + px[o + 1] + px[o + 2];
								n++;
							}
						}
						if (n == 0) {
							continue;
						}
						const double mean = (double)sum / (n * 3.0);
						const uint32_t ci = gj * GX + gi;
						double *ema = &c->repaint.cell_ema[ci];
						if (*ema <= 0.0) {
							*ema = mean;
							continue;
						}
						// Lit cell that collapsed: the dropout we are hunting.
						if (armed && *ema > 8.0 && mean < *ema * 0.50) {
							c->repaint.dropouts++;
							U_LOG_W("#876 DROPOUT: cell(%u,%u) [%s] lum %.1f -> %.1f (%.0f%%) "
							        "(%s weave, composite_bail=%d, total %llu)",
							        gi, gj, gi < 4 ? "zoneA" : "zoneB", *ema, mean,
							        100.0 * mean / *ema,
							        is_repaint ? "REPAINT" : "APP",
							        c->repaint.composite_bail,
							        (unsigned long long)c->repaint.dropouts);
						}
						// Slow EMA so a one-frame dropout cannot drag the
						// baseline down and mask the next one.
						*ema = *ema * 0.95 + mean * 0.05;
					}
				}
			}
		}
		if (!is_repaint) {
			c->repaint.app_hash = hsh;
			c->repaint.app_seq++;
			c->repaint.hash_seq = c->repaint.app_seq;
		} else if (c->repaint.app_hash != 0 && c->repaint.hash_seq == c->repaint.app_seq) {
			static uint64_t same = 0, diff = 0, black = 0;
			if (hsh == c->repaint.app_hash) {
				same++;
			} else {
				diff++;
				/*
				 * Classify the mismatch. The hash alone cannot separate the
				 * two populations, and only one of them is a defect: a
				 * repaint that wove with FRESHER EYES differs by design (that
				 * is the whole feature), whereas a repaint that produced
				 * NOTHING is the flicker. Subsampled mean luminance splits
				 * them for free.
				 */
				if (rp_px != nullptr && c->repaint.app_px != nullptr) {
					/*
					 * Trigger on STRUCTURAL difference, not on darkness.
					 *
					 * Two defects have shown up on the panel and neither is what
					 * a naive detector finds. A whole-frame mean missed one zone
					 * going black (frame mean stayed ~44). And the zones flicker
					 * is often not dark at all — the cube appears "zoomed in,
					 * double image", i.e. the weave ran with wrong tile geometry.
					 * Both are large-area pixel differences, so count HOW MUCH of
					 * the frame changed and localise it on an 8x8 grid.
					 *
					 * A repaint that merely wove with fresher eyes shifts the
					 * interlace phase: many pixels change a little. A geometry or
					 * dropout fault changes a lot of pixels a lot.
					 */
					const uint32_t GX = 8, GY = 8;
					const uint32_t stride = target_width * 4;
					uint64_t sa_all = 0, sr_all = 0;
					uint32_t n_all = 0, big_all = 0;
					uint32_t cell_big[64] = {0}, cell_n[64] = {0};
					for (uint32_t y = 0; y < target_height; y += 4) {
						const uint32_t gj = (y * GY) / target_height;
						for (uint32_t x = 0; x < target_width; x += 4) {
							const uint32_t gi = (x * GX) / target_width;
							const uint32_t o = y * stride + x * 4;
							const int d0 = (int)c->repaint.app_px[o] - (int)rp_px[o];
							const int d1 = (int)c->repaint.app_px[o + 1] - (int)rp_px[o + 1];
							const int d2 = (int)c->repaint.app_px[o + 2] - (int)rp_px[o + 2];
							const int ad = abs(d0) + abs(d1) + abs(d2);
							sa_all += (uint64_t)c->repaint.app_px[o] + c->repaint.app_px[o + 1] +
							          c->repaint.app_px[o + 2];
							sr_all += (uint64_t)rp_px[o] + rp_px[o + 1] + rp_px[o + 2];
							n_all++;
							const uint32_t ci = gj * GX + gi;
							if (ci < GX * GY) {
								cell_n[ci]++;
								if (ad > 48) {
									big_all++;
									cell_big[ci]++;
								}
							}
						}
					}
					const double ma = n_all != 0 ? (double)sa_all / (n_all * 3.0) : 0.0;
					const double mr = n_all != 0 ? (double)sr_all / (n_all * 3.0) : 0.0;
					const double big_pct = n_all != 0 ? 100.0 * (double)big_all / (double)n_all : 0.0;
					int worst_i = -1, worst_j = -1;
					double worst_pct = 0.0;
					for (uint32_t ci = 0; ci < GX * GY; ci++) {
						if (cell_n[ci] == 0) {
							continue;
						}
						const double cp = 100.0 * (double)cell_big[ci] / (double)cell_n[ci];
						if (cp > worst_pct) {
							worst_pct = cp;
							worst_i = (int)(ci % GX);
							worst_j = (int)(ci / GX);
						}
					}
					// >12% of the frame is far beyond an interlace-phase shift.
					const bool structural = (big_pct > 12.0);
					c->repaint.saw_black_region = structural;
					if (structural) {
						black++;
						U_LOG_W("#868 hash: STRUCTURAL repaint mismatch — %.1f%% of frame differs "
						        ">48; worst cell(%d,%d) %.0f%%; lum app=%.1f repaint=%.1f "
						        "(structural %llu / %llu mismatches)",
						        big_pct, worst_i, worst_j, worst_pct, ma, mr,
						        (unsigned long long)black, (unsigned long long)diff);
					} else if ((diff % 20) == 0) {
						U_LOG_W("#868 hash: mismatch #%llu big=%.2f%% lum app=%.1f repaint=%.1f "
						        "(structural %llu)",
						        (unsigned long long)diff, big_pct, ma, mr,
						        (unsigned long long)black);
					}
				}
			}
			if (((same + diff) % 120) == 0) {
				U_LOG_W("#868 hash: adjacent repaint reproduced the app frame %llu/%llu "
				        "(mismatches %llu)",
				        (unsigned long long)same, (unsigned long long)(same + diff),
				        (unsigned long long)diff);
			}
			/*
			 * Dump the first BLACK-REGION pair, not merely the first mismatch.
			 * Most mismatches are the feature working (the repaint wove with
			 * fresher eyes) and dumping one of those wastes the single shot —
			 * it did exactly that on the zones app, capturing a pair whose
			 * worst pixel differed by 21.
			 */
			static bool dumped = false;
			const char *tmp2 = getenv("TEMP");
			if (!dumped && c->repaint.saw_black_region && tmp2 != nullptr && rp_px != nullptr &&
			    c->repaint.app_px != nullptr) {
				dumped = true;
				char pa[512], pr[512];
				snprintf(pa, sizeof(pa), "%s\\hash_app.png", tmp2);
				snprintf(pr, sizeof(pr), "%s\\hash_repaint.png", tmp2);
				stbi_write_png(pa, (int)target_width, (int)target_height, 4, c->repaint.app_px,
				               (int)target_width * 4);
				stbi_write_png(pr, (int)target_width, (int)target_height, 4, rp_px,
				               (int)target_width * 4);
				U_LOG_W("#868 hash: dumped first mismatching pair -> %s / %s", pa, pr);
			}
		}
		free(rp_px);
	}

	/*
	 * #868 diag: paired woven back-buffer capture. ONE trigger arms BOTH weave
	 * kinds so the pair is adjacent in time — comparing an app weave against a
	 * repaint captured seconds apart would drown the signal in scene motion.
	 * This is the instrument that localised the equivalent D3D12 bug after
	 * three wrong hypotheses.
	 */
	{
		const char *tmp = getenv("TEMP");
		if (tmp != nullptr && back_buffer_2d != nullptr) {
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
				snprintf(out, sizeof(out), "%s\\dxr_woven_%s.png", tmp,
				         is_repaint ? "repaint" : "app");
				bool wok = d3d11_capture_texture_to_png(d3d11_out_device(c), d3d11_out_context(c),
				                                        back_buffer_2d, target_width,
				                                        target_height, 0, 0, out);
				U_LOG_W("#868 d3d11 woven capture %s -> %s", wok ? "OK" : "FAILED", out);
			}
		}
	}

	// Only a REAL frame resets the quiet-gate; a repaint must not, or repaints
	// would pace off their own timestamps and drift below panel rate.
	if (!is_repaint) {
		c->repaint.last_app_frame_ns = os_monotonic_get_ns();
	}

	return true;
}

/*!
 * #868 repaint loop — see the D3D12 leg for the full rationale.
 *
 * Fires only once the app has already missed a full refresh (>= 2 panel
 * periods), paces itself unlocked, then holds the compositor lock across the
 * replay so the display processor still only ever sees one caller.
 */
static void
d3d11_repaint_thread(struct comp_d3d11_compositor *c)
{
	while (!c->repaint_quit.load(std::memory_order_relaxed)) {
		const double hz = (c->display_refresh_rate > 1.0f) ? (double)c->display_refresh_rate : 60.0;
		const uint64_t period_ns = (uint64_t)(U_TIME_1S_IN_NS / hz);

		os_nanosleep((int64_t)(period_ns / 4));
		if (c->repaint_quit.load(std::memory_order_relaxed)) {
			break;
		}
		c->repaint.ticks++;

		if (!c->repaint.armed || c->repaint.app_frame_in_progress) {
			c->repaint.bail_armed++;
			continue;
		}
		if (c->repaint.force != 1 &&
		    os_monotonic_get_ns() - c->repaint.last_app_frame_ns < period_ns * 2) {
			c->repaint.bail_gate++;
			continue;
		}

		comp_d3d11_target_repaint_pace(c->target);

		std::lock_guard<std::mutex> lock(c->mutex);

		// app_frame_in_progress is load-bearing and is NOT bypassed by the
		// force probe: replaying into a half-written layer_accum does not
		// exercise the feature, it corrupts the frame.
		if (c->repaint_quit.load(std::memory_order_relaxed) || !c->repaint.armed ||
		    c->repaint.app_frame_in_progress || c->display_processor == NULL || c->target == nullptr) {
			c->repaint.bail_armed++;
			continue;
		}
		if (c->repaint.force != 1 && os_monotonic_get_ns() - c->repaint.last_app_frame_ns < period_ns) {
			c->repaint.bail_race++;
			continue;
		}

		// Acquire exactly as the app frame does. This is NOT bookkeeping: the
		// swapchain is FLIP_DISCARD, so the back buffer's contents are
		// UNDEFINED after every Present, and acquire is what clears it to
		// black. d3d11_dp_weave's comp_d3d11_target_bind() re-binds the RTV
		// and viewport but does NOT clear — so a repaint that skipped acquire
		// wove over discarded garbage wherever the weave and the zone
		// composite do not write every pixel (outside the canvas sub-rect, and
		// any 2D band). App frames started from black, repaints did not, so
		// those regions alternated at the repaint rate — the D3D11 flicker.
		//
		// D3D12 has no equivalent exposure: its shared weave function
		// re-queries the current back-buffer index and RTV on every weave, so
		// both paths are constructed identically by design there. This is that
		// same property, restored on D3D11.
		uint32_t rp_index = 0;

		/*
		 * Make the whole replay atomic on the immediate context. c->mutex only
		 * excludes the compositor's OWN callers; the app renders its scene on
		 * its own thread through this same context and never takes it. Without
		 * this, an app OMSetRenderTargets landing mid-sequence sent the weave
		 * to the app's render target and left the swapchain black — measured
		 * at ~19% of repaints, which is the flicker.
		 */
		/*
		 * #918: under the output-device split the replay runs entirely on the
		 * RUNTIME-OWNED output context, whose only two callers are layer_commit
		 * and this loop — and both already hold c->mutex. The mt_lock bracket
		 * and the app_state_guard exist solely to defend against the APP's
		 * render thread interleaving on the APP's immediate context, which this
		 * replay no longer touches; taking them here would add contention on a
		 * device the app is still rendering with, for nothing. Note also that
		 * the replay costs ZERO bridge traffic: it re-weaves the egress slot the
		 * last app frame published, already resident on this adapter.
		 */
		if (c->split_active) {
			// #918 F4: nothing published to re-weave (warmup, or the egress ring
			// was just reallocated by a mode switch / resize). Bail BEFORE the
			// acquire — acquire clears the back buffer, and a present of that
			// clear is exactly the black flash this guard exists to remove.
			if (comp_d3d11_xbridge_get_weave_slot(c->xbridge) < 0) {
				c->repaint.bail_armed++;
				continue;
			}
			comp_d3d11_target_acquire(c->target, &rp_index);
			if (d3d11_dp_weave(c, true)) {
				d3d11_render_hud_overlay(c, c->out_dev, c->out_ctx, true, &c->repaint.eye_pos);
				comp_d3d11_target_present(c->target, 1);
			} else {
				// Cleared but unwoven — hold the last shown frame (FLIP_DISCARD
				// keeps it on screen precisely because we do not present).
				d3d11_split_note_skipped_present(c, "the repaint tick");
			}
		} else {
			if (c->mt_lock != nullptr) {
				c->mt_lock->Enter();
			}

			{
				// The replay lands BETWEEN the app's own draw calls on this
				// context. Enter()/Leave() keeps our sequence atomic against the
				// app; this guard keeps the app's cached state truthful once we
				// Leave(). Scoped so the restore runs before Leave().
				xrt::compositor::d3d11::app_state_guard app_state(c->context);

				comp_d3d11_target_acquire(c->target, &rp_index);
				d3d11_dp_weave(c, true);
				d3d11_render_hud_overlay(c, c->device, c->context, true, &c->repaint.eye_pos);
				comp_d3d11_target_present(c->target, 1);
			}

			if (c->mt_lock != nullptr) {
				c->mt_lock->Leave();
			}
		}

		c->repaint.count++;
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("#868: repainting last atlas at %.1f Hz while the app is between frames "
			        "(set DXR_WEAVE_REPAINT=0 to disable)",
			        hz);
		}
		// #887 periodic counters: apps are usually killed rather than shut
		// down cleanly, so a destroy-time dump alone is rarely seen.
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
d3d11_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Everything below renders on the APP's immediate context. Hand its pipeline
	// state back on every exit path, or a state-caching engine (Unity's D3D11
	// backend) draws its next frame against what WE left bound: see-through
	// geometry, degenerate triangles, shredded text. See comp_d3d11_state_guard.h.
	xrt::compositor::d3d11::app_state_guard app_state(c->context);

	// #868: the app's submission window closes here. Cleared at the TOP so no
	// early return can leak it; the lock is held throughout layer_commit, so no
	// repaint can interleave with it regardless.
	c->repaint.app_frame_in_progress = false;

	// Capture-intent poll — see u_capture_intent.h. Consumed at the
	// projection-done boundary (PROJECTION_ONLY, once renderer split
	// lands) or end of frame (POST_COMPOSE).
	u_capture_intent_poll(&c->capture_intent, &c->mcp_capture);

	// Phase 1 diagnostic — env-gated per-client commit interval. Mirrors
	// the same `[CLIENT_FRAME_NS]` line emitted by the SERVICE compositor's
	// `compositor_layer_commit` so per-app frame rates are directly
	// comparable across workspace mode (service compositor) and standalone
	// (this in-process compositor). One client per process here so a
	// static cache is fine; HWND tags the line for log demuxing.
	{
		static int log_client_frame_ns = -1;
		if (log_client_frame_ns < 0) {
			const char *e = getenv("DISPLAYXR_LOG_PRESENT_NS");
			log_client_frame_ns = (e != nullptr && e[0] == '1') ? 1 : 0;
		}
		if (log_client_frame_ns) {
			static int64_t last_commit_ns = 0;
			int64_t now_ns = os_monotonic_get_ns();
			if (last_commit_ns != 0) {
				U_LOG_W("[CLIENT_FRAME_NS] client=%p dt_ns=%lld",
				        (void *)c->hwnd,
				        (long long)(now_ns - last_commit_ns));
			}
			last_commit_ns = now_ns;
		}
	}

	// Get predicted eye positions
	struct xrt_eye_positions eye_pos = {};
	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d11_get_predicted_eye_positions(c->display_processor, &eye_pos);
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
				comp_d3d11_renderer_set_tile_layout(
				    c->renderer, mode->tile_columns, mode->tile_rows);
			}
		}
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
					// Save current 3D mode index before switching to 2D
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						c->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					// Restore last 3D mode index
					head->hmd->active_rendering_mode_index = c->last_3d_mode_index;
				}
			}
			comp_d3d11_compositor_request_display_mode(&c->base.base, !force_2d);
		}

		// Rendering mode change from qwerty 1/2/3 keys.
		// Legacy apps (no XR_DXR_display_info) only support V toggle between
		// mode 0 (2D) and mode 1 (default 3D) — skip direct mode selection.
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

	// #439 Phase 3: frame mask state. Any XrCompositionLayerLocal2DDXR layer
	// this frame raises an implicit mask (Q3) which, like an explicit one,
	// supersedes the canvas (uniform Phase-2 rule). Compute once under
	// c->mutex (held here) so d3d11_effective_canvas below and the composite
	// downstream see one coherent decision for the frame.
	bool have_local_2d = false;
	bool zones_frame = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			have_local_2d = true;
		} else if (c->layer_accum.layers[i].data.type == XRT_LAYER_ZONE_3D) {
			zones_frame = true;
		}
	}
	c->local_2d_last_frame = have_local_2d;
	// XR_DXR_display_zones (ADR-027): one coherent per-frame decision —
	// effective canvas, the wish raster/publish, and the visual composite
	// all read this under the same c->mutex hold.
	c->zones_frame = zones_frame;

	// #439 Phase 2: the one canvas authority for this frame. While a zone
	// mask is active this is the client-window rect (the mask supersedes
	// the output rect); otherwise it is an invalid rect (readers fall back
	// to full-window/target dims). Computed once
	// under c->mutex (held for this whole function) so the weave region,
	// view dims, and composite all see the same rect even if submit/destroy
	// race the frame.
	const struct u_canvas_rect eff_canvas = d3d11_effective_canvas(c);

	// XR_DXR_display_zones: resolve the frame's wish source BEFORE the DP
	// sideband sync and the post-weave lerp — both consume the same staged
	// SRV (the ADR's no-drift property).
	if (c->zones_frame) {
		d3d11_update_zone_wish_state(c);
	}

	// #224 hardware-DP zone leg: sideband-publish the active mask (or clear
	// it on the inactive edge) so switchable-lens cells track the authored
	// zones. Same mutex scope as the composite — the DP sees the same mask
	// state as this frame's weave/composite (the two consumers must agree).
	// In a zones frame the published content is the WISH (explicit frame
	// wish or the auto raster), not the sticky legacy mask.
	d3d11_sync_zone_mask_to_dp(c);

	// Get target (window) dimensions for mono viewport sizing.
	// In shared texture mode (no target), use canvas dims if available —
	// the DP weaves at canvas resolution, not full shared texture size.
	uint32_t tgt_width = c->settings.preferred.width;
	uint32_t tgt_height = c->settings.preferred.height;
	if (c->target != nullptr) {
		comp_d3d11_target_get_dimensions(c->target, &tgt_width, &tgt_height);
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
				comp_d3d11_renderer_get_view_dimensions(c->renderer, &cur_vw, &cur_vh);
				if (cur_vw != new_vw || cur_vh != new_vh) {
					uint32_t resize_target_h = (c->display_processor != NULL) ? new_vh : tgt_height;
					comp_d3d11_renderer_resize(
					    c->renderer,
					    new_vw,
					    new_vh,
					    resize_target_h);
				}
			}
		}
	}

	// Per-frame effective CONTENT layout (#542): the mode's recipe, with the
	// submission clamped to it. Feeds both renderer passes, the DP handoff,
	// and the capture providers — they must all agree on the frame's
	// geometry.
	comp_d3d11_renderer_compute_effective_layout(c->renderer, &c->layer_accum, &c->eff_layout);

	// Zero-copy check: can we pass the app's swapchain directly to the DP?
	//
	// #918: never under the output-device split. The app's swapchain image lives
	// on the app's adapter and the DP is on the scanout adapter, so there is
	// nothing to hand over zero-copy — the atlas has to cross regardless. The
	// probe is skipped entirely rather than computed and discarded.
	bool zero_copy = false;
	void *zc_srv = nullptr;
	if (!c->split_active) {
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
						comp_d3d11_swapchain_get_dimensions(layer->sc_array[0], &sw, &sh);
						int32_t rxs[XRT_MAX_VIEWS], rys[XRT_MAX_VIEWS];
						uint32_t rws[XRT_MAX_VIEWS], rhs_arr[XRT_MAX_VIEWS];
						for (uint32_t v = 0; v < vc; v++) {
							rxs[v] = layer->data.proj.v[v].sub.rect.offset.w;
							rys[v] = layer->data.proj.v[v].sub.rect.offset.h;
							rws[v] = layer->data.proj.v[v].sub.rect.extent.w;
							rhs_arr[v] = layer->data.proj.v[v].sub.rect.extent.h;
						}
						if (u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs_arr, sw, sh, mode)) {
							zc_srv = comp_d3d11_swapchain_get_srv(layer->sc_array[0], img_idx);
							if (zc_srv != nullptr)
								zero_copy = true;
						}
					}
				}
			}
		}
	}


	// No CPU-side wait for the app's GPU work here, by design: the app renders
	// on this same immediate context, and D3D11 executes it in submission order,
	// so every read of an app swapchain image below is already ordered after
	// the app's writes. A wait would only stall the app's render thread and
	// serialise CPU against GPU. (A previous fence+event wait here was, in
	// practice, a no-op that consumed a stale auto-reset signal; making it
	// "work" cost ~1 ms/frame on the app thread and fixed nothing.)

	// Verify app renders at the expected resolution (not stretched)
	{
		static int rect_check_log = 0;
		uint32_t expected_vw, expected_vh;
		comp_d3d11_renderer_get_view_dimensions(c->renderer, &expected_vw, &expected_vh);
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

	// Zero-copy capture: the app's swapchain (synced by the GPU-completion
	// wait above) already holds the atlas the DP will present, and the normal
	// dispatch points below are gated behind !zero_copy — so service a pending
	// capture here, reading the swapchain directly (issue #425).
	if (zero_copy) {
		d3d11_compositor_dispatch_capture_zerocopy(c, zc_srv);
	}

	// Render layers to atlas texture (skip if zero-copy). Split into a
	// projection pass + window-space pass so a projection-only capture
	// can read the atlas in between.
	xrt_result_t xret = XRT_SUCCESS;
	if (!zero_copy) {
		/*
		 * #918 F6: the passes below overwrite the atlas the producer copies OUT
		 * of. Take the GPU-queue back-fence first, so the app's adapter cannot
		 * start frame N's writes until the producer's copy of the last bridged
		 * frame retired. GPU-side only — this thread does not wait.
		 */
		if (c->split_active) {
			comp_d3d11_xbridge_pre_render(c->xbridge);
		}
		xret = comp_d3d11_renderer_draw_projection_pass(
		    c->renderer, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, &c->eff_layout);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render projection pass");
			return xret;
		}

		// Projection-only capture point — atlas now contains projection-
		// class layers (projection, projection-depth, quad) for every
		// tile; window-space layers haven't been composed yet.
		d3d11_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_PROJECTION_ONLY);

		xret = comp_d3d11_renderer_draw_window_space_pass(
		    c->renderer, &c->layer_accum, tgt_width, tgt_height, &c->eff_layout);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render window-space pass");
			return xret;
		}
	}

	/*
	 * #875 DEPOSIT: resolve the mask and flatten the Local2D layers while the app
	 * still owns the swapchain images they read. The weave below — and every
	 * repaint after it — then composite from compositor-owned state alone, so both
	 * produce the same pixels by construction.
	 *
	 * #918 Phase 2a moved this ABOVE the bridge submit (it used to sit after the
	 * target acquire). The plane copies have to be recorded into the same command
	 * lists as the atlas copy, so the pixels they transport must already exist
	 * when submit runs. Nothing here writes the target — the deposit only reads
	 * its descriptor for the scratch format — and `comp_d3d11_target_acquire` does
	 * not block, so this reorder costs the copy legs no window at all.
	 */
	if (c->target != nullptr && c->display_processor != NULL) {
		uint32_t dep_w = 0, dep_h = 0;
		comp_d3d11_target_get_dimensions(c->target, &dep_w, &dep_h);
		ID3D11Texture2D *dep_bb = static_cast<ID3D11Texture2D *>(comp_d3d11_target_get_back_buffer(c->target));

		/*
		 * #918 review D6/F1 — publish the authored-mask plane ONCE per frame,
		 * BEFORE anything asks whether its pixels have landed. This is the only
		 * site that binds or stages PLANE_MASK.
		 */
		d3d11_stage_mask_plane(c, d3d11_frame_authored_mask(c));

		// #918 Phase 2a: the 2D-under backdrop flatten is app-device work reading
		// app-owned swapchain images, so it belongs in the deposit half beside the
		// overlay flatten — and under the split that is also what stages it as a
		// bridge plane in time for the submit below.
		if (c->split_active) {
			uint32_t bd_w = 0, bd_h = 0;
			c->repaint.backdrop_srv = d3d11_flatten_backdrop_2d(c, dep_w, dep_h, &bd_w, &bd_h);
			c->repaint.backdrop_w = bd_w;
			c->repaint.backdrop_h = bd_h;
			if (c->repaint.backdrop_srv == nullptr && c->xbridge != nullptr) {
				// No under-layers this frame — un-stage, so the recipe stamps
				// the plane invalid instead of carrying stale pixels forward.
				comp_d3d11_xbridge_stage_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_BACKDROP, 0, 0, 0,
				                               0, 0);
			}
		}

		const bool deposited = d3d11_composite_zone_mask(c, /*is_repaint=*/false, /*prepare_only=*/true, dep_bb,
		                                                 dep_w, dep_h, &eff_canvas, -1);
		if (c->split_active && !deposited && c->xbridge != nullptr) {
			/*
			 * A projection-only frame. Stamp a recipe that says so and un-stage
			 * the LOCAL2D plane, so a slot from a Local2D frame that is still in
			 * the ring cannot lend its pixels to this one.
			 *
			 * #918 review F1: the MASK plane is deliberately NOT un-staged here.
			 * It is published above, before the deposit runs, and this branch
			 * fires on exactly the frame whose transport the NEXT frame needs —
			 * reverting it here is half of what made a Tier-3 mask unable to
			 * bootstrap. The mask plane's own publisher already un-stages it when
			 * the frame genuinely has no authored mask.
			 */
			struct comp_d3d11_xbridge_recipe r = {};
			r.composite = false;
			// The backdrop is independent of the composite: a frame can have
			// 2D-under layers and still not run the masked pass.
			r.bd_w = c->repaint.backdrop_w;
			r.bd_h = c->repaint.backdrop_h;
			comp_d3d11_xbridge_stage_recipe(c->xbridge, &r);
			comp_d3d11_xbridge_stage_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_LOCAL2D, 0, 0, 0, 0, 0);
		}
	}

	/*
	 * #918: ship this frame's atlas — and, since Phase 2a, its planes — across the
	 * adapter boundary.
	 *
	 * Placed here deliberately — after both renderer passes (so the copy sources
	 * a COMPLETE atlas) and before the target acquire + late-weave sleep below,
	 * which is the window the copy legs get to run in. Nothing on this thread
	 * waits: the app-side cost is one fence signal plus a flush.
	 */
	if (c->split_active) {
		const uint32_t bridge_w = c->eff_layout.cols * c->eff_layout.tile_w;
		const uint32_t bridge_h = c->eff_layout.rows * c->eff_layout.tile_h;
		if (bridge_w > 0 && bridge_h > 0) {
			/*
			 * #918 R1: the layout GENERATION. It counts changes to the DP's
			 * recipe — the tile grid and the submitted view count — and NOT
			 * the tile dimensions, which move continuously through a resize
			 * while the recipe stays put (those are handled per slot, by
			 * weaving each slot at the box it was painted with).
			 *
			 * A 3D<->2D switch is precisely a grid change that leaves the
			 * content box identical, so nothing else in the pipeline marks
			 * it; this is the only signal the weave gets.
			 */
			const uint64_t sig = ((uint64_t)c->eff_layout.cols << 40) |
			                     ((uint64_t)c->eff_layout.rows << 20) | (uint64_t)c->eff_layout.views;
			if (sig != c->split_layout_sig) {
				c->split_layout_sig = sig;
				c->split_layout_gen++;
				U_LOG_W(
				    "#918: layout generation %llu — %ux%u tiles of %ux%u, %u view(s) "
				    "submitted; slots from generation %llu are no longer weavable",
				    (unsigned long long)c->split_layout_gen, c->eff_layout.cols, c->eff_layout.rows,
				    c->eff_layout.tile_w, c->eff_layout.tile_h, c->eff_layout.views,
				    (unsigned long long)(c->split_layout_gen - 1));
			}
			// The atlas is only reallocated on a genuine grow (mode switch,
			// window resize); the generation is what tells the producer to
			// re-open, and it never moves on the #602 fits-early-out.
			comp_d3d11_xbridge_bind_atlas(c->xbridge,
			                              comp_d3d11_renderer_get_atlas_shared_handle(c->renderer),
			                              comp_d3d11_renderer_get_atlas_generation(c->renderer));
			// Live egress re-size: the content box follows the window and the
			// active mode, so a V-key mode switch or a resize lands here.
			comp_d3d11_xbridge_set_content_size(c->xbridge, bridge_w, bridge_h, c->split_layout_gen);
			c->split_seq++;
			comp_d3d11_xbridge_submit(c->xbridge, c->split_seq, c->split_layout_gen,
			                          comp_d3d11_renderer_get_atlas_texture(c->renderer), bridge_w,
			                          bridge_h);
		}
	}

#ifdef XRT_FEATURE_DEBUG_GUI
	// Update debug GUI preview with the rendered atlas texture
	if (comp_d3d11_debug_is_active(c->debug)) {
		ID3D11Texture2D *atlas_texture =
		    static_cast<ID3D11Texture2D *>(comp_d3d11_renderer_get_atlas_texture(c->renderer));
		comp_d3d11_debug_update_preview(c->debug, c->context, atlas_texture);
	}
#endif

	bool weaving_done = false;

	// Offscreen shared-texture-only path: no DXGI target
	if (c->target == nullptr) {
		// Weave/blit directly into the shared texture
		if (c->display_processor != NULL && c->shared_rtv != nullptr) {
			void *atlas_srv = zero_copy ? zc_srv : comp_d3d11_renderer_get_atlas_srv(c->renderer);
			// #542: the DP gets the frame's EFFECTIVE content layout —
			// the grid the passes above actually painted (== the mode
			// layout for matched submissions) — not the mode layout.
			uint32_t view_width = c->eff_layout.tile_w;
			uint32_t view_height = c->eff_layout.tile_h;
			uint32_t tile_columns = c->eff_layout.cols;
			uint32_t tile_rows = c->eff_layout.rows;

			// Crop the renderer's atlas to content dims before passing to the
			// DP (the renderer allocates a worst-case atlas; content sits
			// top-left at the content stride). Do NOT crop a zero-copy atlas:
			// the app's swapchain is already a complete, correctly-strided
			// multi-view atlas (the DP derives tile stride from atlas_width /
			// tile_columns), so a top-left crop to the window-scaled width
			// would slice the tiles and weave them misaligned — big disparity
			// (#431 follow-up: the crop must not touch the zero-copy path).
			uint32_t content_w = tile_columns * view_width;
			uint32_t content_h = tile_rows * view_height;
			if (!zero_copy) {
				atlas_srv = d3d11_crop_atlas_for_dp(c, atlas_srv, content_w, content_h);
			}

			// Weave directly into the shared texture at the canvas sub-rect.
			// The SR weaver handles backbuffer > HWND correctly as long as the
			// viewport position inside the backbuffer matches the eventual
			// screen-space position inside the HWND (verified via Leia
			// ST-5465-test-scenario). The app's blit reads back from
			// (canvas.x, canvas.y) in the shared texture — see ADR-010.
			D3D11_TEXTURE2D_DESC st_desc;
			c->shared_texture->GetDesc(&st_desc);
			uint32_t dp_target_w = st_desc.Width;
			uint32_t dp_target_h = st_desc.Height;

			// #491 part 3 — flatten the 2D-under layers PRE-weave and hand them
			// to the DP (it composites `backdrop over captured-desktop` under
			// the 3D). The flatten unbinds its RTV; the shared RTV is bound
			// after. NULL ⟹ no under-layers (DP clears its backdrop).
			uint32_t bd_w = 0, bd_h = 0;
			ID3D11ShaderResourceView *bd_srv =
			    d3d11_flatten_backdrop_2d(c, dp_target_w, dp_target_h, &bd_w, &bd_h);
			xrt_display_processor_d3d11_set_background_2d(c->display_processor, bd_srv, bd_w, bd_h);

			c->context->OMSetRenderTargets(1, &c->shared_rtv, nullptr);

			xrt_display_processor_d3d11_process_atlas(
			    c->display_processor, c->context, atlas_srv, view_width, view_height,
			    tile_columns, tile_rows, DXGI_FORMAT_R8G8B8A8_UNORM, dp_target_w, dp_target_h,
			    eff_canvas.valid ? eff_canvas.x : 0,
			    eff_canvas.valid ? eff_canvas.y : 0,
			    eff_canvas.valid ? eff_canvas.w : 0,
			    eff_canvas.valid ? eff_canvas.h : 0);

			// #439 Phase 1: an authored zone mask (XR_DXR_local_3d_zone) or
			// a Local2D layer composites the 2D/3D regions of the shared
			// texture. No-op when the frame carries neither.
			d3d11_composite_zone_mask(c, false, false, c->shared_texture, dp_target_w, dp_target_h,
			                          &eff_canvas, -1);

			weaving_done = true;
		}

		c->context->Flush();
		return XRT_SUCCESS;
	}

	// Normal path: acquire DXGI target image
	uint32_t target_index;
	xret = comp_d3d11_target_acquire(c->target, &target_index);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to acquire target");
		return xret;
	}

	// Use generic display processor for weaving/blit (vendor-agnostic path)
	// The DP may be a no-op in 2D mode (returns without rendering).
	if (c->display_processor != NULL) {
		static bool dp_logged = false;
		if (!dp_logged) {
			U_LOG_W("D3D11 weaving via display processor interface");
			dp_logged = true;
		}

		/*
		 * #868: publish what the repaint thread needs to replay this weave.
		 * Armed only off the zero-copy path — there the atlas IS the app's own
		 * swapchain image, which it reacquires and overwrites.
		 *
		 * #918 Phase 2a moved the #875 DEPOSIT above the bridge submit (the plane
		 * copies ride the same command lists as the atlas, so their pixels must
		 * exist before submit records them) but deliberately left THIS here, after
		 * a successful acquire: arming the repaint thread for a frame the acquire
		 * then failed to produce is a behaviour change the split has no business
		 * making, and the deposit reads none of these fields.
		 */
		c->repaint.canvas = eff_canvas;
		c->repaint.eye_pos = eye_pos;
		c->repaint.armed = !zero_copy;
		c->repaint.zero_copy = zero_copy;
		c->repaint.zc_srv = zc_srv;

		weaving_done = d3d11_dp_weave(c, false);
	}

	/*
	 * #918 F4: under the split the weave can legitimately produce NOTHING — the
	 * first frames before anything has crossed, and the frame after an egress
	 * realloc (a mode switch or a resize) that reset the ring. The back buffer
	 * was already acquired, and acquire CLEARS it, so presenting here would put
	 * a black frame on the panel; the repaint tick would then do it again, which
	 * is the mode-switch black flash.
	 *
	 * With FLIP_DISCARD the correct degrade is simply not to present: the panel
	 * keeps showing the last frame that WAS woven, for the one or two frames the
	 * transition takes. The skip is counted, never logged per frame.
	 */
	if (c->split_active && !weaving_done) {
		d3d11_split_note_skipped_present(c, "the app frame");
		if (c->owns_window && c->own_window != nullptr) {
			comp_d3d11_window_signal_paint_done(c->own_window);
		}
		return XRT_SUCCESS;
	}

	// HUD overlay (post-processing, always readable). #918: the staging texture
	// and the back-buffer blit follow the target onto the output device.
	d3d11_render_hud_overlay(c, d3d11_out_device(c), d3d11_out_context(c), weaving_done, &eye_pos);

	// Note: transparency (compose-under-bg / alpha-gate) lives inside the
	// vendor display processor, enabled via set_transparent_background. The
	// compositor is vendor-agnostic for transparency.

	// Copy composited output into shared texture if active (dual output: window + shared)
	if (c->has_shared_texture && c->shared_texture != nullptr) {
		ID3D11Texture2D *back_buffer = static_cast<ID3D11Texture2D *>(
		    comp_d3d11_target_get_back_buffer(c->target));
		if (back_buffer != nullptr) {
			D3D11_TEXTURE2D_DESC src_desc, dst_desc;
			back_buffer->GetDesc(&src_desc);
			c->shared_texture->GetDesc(&dst_desc);

			if (src_desc.Width == dst_desc.Width && src_desc.Height == dst_desc.Height) {
				c->context->CopyResource(c->shared_texture, back_buffer);
			} else {
				UINT copy_width = (src_desc.Width < dst_desc.Width) ? src_desc.Width : dst_desc.Width;
				UINT copy_height = (src_desc.Height < dst_desc.Height) ? src_desc.Height : dst_desc.Height;
				D3D11_BOX src_box = {0, 0, 0, copy_width, copy_height, 1};
				c->context->CopySubresourceRegion(c->shared_texture, 0, 0, 0, 0,
				                                   back_buffer, 0, &src_box);
			}
			c->context->Flush();
		}
	}

	// Present with VSync
	xret = comp_d3d11_target_present(c->target, 1);

	// Signal WM_PAINT that the frame is done (unblocks modal drag loop)
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_signal_paint_done(c->own_window);
	}

	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to present");
		return xret;
	}

	// Post-compose capture (#210) — fully composed atlas as DP saw it.
	// PROJECTION_ONLY intent is consumed earlier in this function once
	// the renderer split lands; until then both modes hit this point.
	d3d11_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_POST_COMPOSE);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                              struct xrt_compositor_semaphore *xcsem,
                                              uint64_t value)
{
	// Use the same implementation as layer_commit
	return d3d11_compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}

static void
d3d11_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	// #868: join the repaint loop FIRST — it touches the device context, the
	// target and the display processor under c->mutex, so it must be stopped
	// before any of that is torn down, not merely signalled.
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

	/*
	 * #918: with the repaint loop joined, quiesce the bridge NEXT — stop
	 * submitting, join its watchdog, and drain the copy queues under a bounded
	 * (2 s) CPU wait. Nothing below may run while a cross-adapter copy is still
	 * reading the renderer's atlas or writing an egress slot.
	 */
	if (c->xbridge != nullptr) {
		U_LOG_W(
		    "D3D11 output-device split: %llu presents skipped for want of a woven frame "
		    "(%llu refusing a superseded layout, #918 R1); %llu transition frames woven from an "
		    "in-flight slot rather than held; %llu composites refused for a stale recipe stamp "
		    "(#918 Phase 2a — expected 0); longest gap between weaves %.1f ms (#918 F4/R1/R2)",
		    (unsigned long long)c->split_skip_present, (unsigned long long)c->split_stale_refusals,
		    (unsigned long long)c->split_inflight_weaves, (unsigned long long)c->split_recipe_refusals,
		    (double)c->split_weave_gap_max_ns / 1.0e6);
		comp_d3d11_xbridge_quiesce(c->xbridge);
	}

	U_LOG_I("Destroying D3D11 compositor");

	u_capture_dims_set_provider(NULL, c);
	mcp_capture_uninstall();
	mcp_capture_fini(&c->mcp_capture);

#ifdef XRT_FEATURE_DEBUG_GUI
	// Stop debug GUI first (before destroying resources it may reference)
	if (c->debug != nullptr) {
		comp_d3d11_debug_destroy(&c->debug);
	}
	if (c->debug_gui != nullptr) {
		u_debug_gui_stop(&c->debug_gui);
	}
#endif

	// Destroy shared texture resources
	if (c->shared_rtv != nullptr) {
		c->shared_rtv->Release();
		c->shared_rtv = nullptr;
	}
	if (c->shared_texture != nullptr) {
		c->shared_texture->Release();
		c->shared_texture = nullptr;
	}

	// #439 Phase 1: release the weave scratch + detach any active zone mask
	// (the oxr handle owns the mask object itself).
	d3d11_release_zone_state(c);

	// Destroy HUD
	if (c->hud_texture != nullptr) {
		c->hud_texture->Release();
		c->hud_texture = nullptr;
	}
	u_hud_destroy(&c->hud);

	// Destroy DP input crop texture
	if (c->dp_input_srv != nullptr) {
		c->dp_input_srv->Release();
		c->dp_input_srv = nullptr;
	}
	if (c->dp_input_texture != nullptr) {
		c->dp_input_texture->Release();
		c->dp_input_texture = nullptr;
	}

	// Detach the window's phase-snap provider BEFORE the DP dies: the
	// setter's exclusive lock waits out any snap call in flight on the
	// window thread, so the callback can never race the destroy below.
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_set_snap_provider(c->own_window, NULL, NULL);
	}

	// Destroy display processor (handles all vendor cleanup internally)
	xrt_display_processor_d3d11_destroy(&c->display_processor);

	if (c->renderer != nullptr) {
		comp_d3d11_renderer_destroy(&c->renderer);
	}

	// #918 Phase 2a: after the renderer (nothing else references it) and
	// before the devices below — it borrows one of them.
	comp_d3d11_outcomp_destroy(&c->outcomp);

	if (c->target != nullptr) {
		comp_d3d11_target_destroy(&c->target);
	}

	/*
	 * #918: the bridge dies AFTER the target — its egress textures are the
	 * atlas the DP was sampling and the target's back buffer is the thing the DP
	 * wrote, so both consumers must be gone first. Inside, the release order is
	 * egress -> consumer -> cross-adapter heap -> producer -> fences.
	 */
	if (c->xbridge != nullptr) {
		comp_d3d11_xbridge_destroy(&c->xbridge);
	}

	if (c->out_factory != nullptr) {
		c->out_factory->Release();
		c->out_factory = nullptr;
	}
	if (c->out_ctx != nullptr) {
		c->out_ctx->Release();
		c->out_ctx = nullptr;
	}
	if (c->out_dev != nullptr) {
		c->out_dev->Release();
		c->out_dev = nullptr;
	}
	c->split_active = false;

	if (c->dxgi_factory != nullptr) {
		c->dxgi_factory->Release();
	}

	// #868: released after the repaint thread has been joined (above), so no
	// replay can still be inside Enter()/Leave().
	if (c->mt_lock != nullptr) {
		c->mt_lock->Release();
		c->mt_lock = nullptr;
	}

	if (c->context != nullptr) {
		c->context->Release();
	}

	if (c->device != nullptr) {
		c->device->Release();
	}

	// Destroy self-created window if we own it
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_destroy(&c->own_window);
	}

	// layer_accum doesn't need special cleanup - it's just a struct

	delete c;
}

/*
 *
 * Exported functions
 *
 */

extern "C" xrt_result_t
comp_d3d11_compositor_create(struct xrt_device *xdev,
                             void *hwnd,
                             void *d3d11_device,
                             void *dp_factory_d3d11,
                             void *shared_texture_handle,
                             bool transparent_background,
                             int32_t display_screen_left,
                             int32_t display_screen_top,
                             struct xrt_compositor_native **out_xc)
{
	if (d3d11_device == nullptr) {
		U_LOG_E("D3D11 device is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_I("Creating D3D11 native compositor");

	// Allocate compositor
	comp_d3d11_compositor *c = new comp_d3d11_compositor();
	memset(&c->base, 0, sizeof(c->base));

	c->xdev = xdev;

	mcp_capture_init(&c->mcp_capture);
	mcp_capture_install(&c->mcp_capture);
	c->own_window = nullptr;
	c->owns_window = false;
	c->app_hwnd = nullptr;
	c->hardware_display_3d = true;
	c->last_3d_mode_index = 1;
	c->transparent_background = transparent_background;

	// Handle window: use provided HWND, create our own, or go offscreen (shared texture)
	if (shared_texture_handle != nullptr) {
		// Shared texture mode: compositor doesn't own a swapchain.
		// Store app HWND separately for display processor position tracking.
		// No hidden window — the DP gets the app's real HWND directly.
		c->hwnd = nullptr;
		if (hwnd != nullptr) {
			c->app_hwnd = static_cast<HWND>(hwnd);
			U_LOG_I("Shared texture mode with app HWND for position tracking: %p", hwnd);
		} else {
			U_LOG_I("Shared texture mode (offscreen) — no window");
		}
	} else if (hwnd != nullptr) {
		// App provided window via XR_DXR_win32_window_binding (ext mode)
		c->hwnd = static_cast<HWND>(hwnd);
		U_LOG_I("Using app-provided window handle: %p", hwnd);
	} else {
		// No window provided - create our own at native display resolution
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

	// Get D3D11 device - just use the base interface, we don't need Device5 features
	c->device = static_cast<ID3D11Device *>(d3d11_device);
	c->device->AddRef();

	// Get immediate context
	c->device->GetImmediateContext(&c->context);

	// #1000: record which adapter the app's device actually lives on. On a
	// hybrid iGPU/dGPU box a hung run's log was byte-identical to a healthy
	// one because placement was never written down. One-time, init-only —
	// never per frame. Purely diagnostic: every failure path skips the log
	// and continues, logging must never fail the create.
	{
		IDXGIDevice *log_dxgi_dev = nullptr;
		if (SUCCEEDED(c->device->QueryInterface(__uuidof(IDXGIDevice),
		                                        reinterpret_cast<void **>(&log_dxgi_dev))) &&
		    log_dxgi_dev != nullptr) {
			IDXGIAdapter *log_adapter = nullptr;
			if (SUCCEEDED(log_dxgi_dev->GetAdapter(&log_adapter)) && log_adapter != nullptr) {
				DXGI_ADAPTER_DESC log_desc{};
				if (SUCCEEDED(log_adapter->GetDesc(&log_desc))) {
					U_LOG_W("D3D11 compositor: app device on adapter '%ls' "
					        "LUID=%08lx:%08lx (#1000)",
					        log_desc.Description,
					        (unsigned long)log_desc.AdapterLuid.HighPart,
					        (unsigned long)log_desc.AdapterLuid.LowPart);
				}
				log_adapter->Release();
			}
			log_dxgi_dev->Release();
		}
	}

	// Enable D3D11 multithread protection for cross-thread window/compositor access.
	// The HWND lives on a dedicated window thread while D3D11 rendering happens here.
	HRESULT hr;
	c->mt_lock = nullptr;
	{
		ID3D10Multithread *mt = nullptr;
		hr = c->device->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt);
		if (SUCCEEDED(hr) && mt != nullptr) {
			mt->SetMultithreadProtected(TRUE);
			// #868: kept (not released) — the repaint thread needs
			// Enter()/Leave() to make its weave sequence atomic against the
			// app's own use of this same immediate context.
			c->mt_lock = mt;
			U_LOG_W("D3D11 multithread protection enabled");
		}
	}

	// Limit DXGI frame queue to 1 to prevent burst/stall frame pacing.
	// The default is 3, which lets the app submit multiple frames before
	// Present(1) blocks, causing micro-stutter in windowed mode.
	{
		IDXGIDevice1 *dxgi_dev1 = nullptr;
		hr = c->device->QueryInterface(__uuidof(IDXGIDevice1), (void **)&dxgi_dev1);
		if (SUCCEEDED(hr) && dxgi_dev1 != nullptr) {
			dxgi_dev1->SetMaximumFrameLatency(1);
			dxgi_dev1->Release();
			U_LOG_W("DXGI maximum frame latency set to 1");
		}
	}

	// Get DXGI factory
	IDXGIDevice *dxgi_device;
	hr = c->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device));
	if (SUCCEEDED(hr)) {
		IDXGIAdapter *adapter;
		dxgi_device->GetAdapter(&adapter);

		hr = adapter->GetParent(__uuidof(IDXGIFactory4), reinterpret_cast<void **>(&c->dxgi_factory));
		adapter->Release();
		dxgi_device->Release();
	}

	if (c->dxgi_factory == nullptr) {
		U_LOG_E("Failed to get DXGI factory");
		d3d11_compositor_destroy(&c->base.base);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Initialize settings with defaults (simplified for D3D11 native compositor)
	memset(&c->settings, 0, sizeof(c->settings));
	c->settings.preferred.width = xdev->hmd->screens[0].w_pixels;
	c->settings.preferred.height = xdev->hmd->screens[0].h_pixels;
	if (c->settings.preferred.width == 0 || c->settings.preferred.height == 0) {
		c->settings.preferred.width = 1920;
		c->settings.preferred.height = 1080;
	}
	c->settings.nominal_frame_interval_ns = xdev->hmd->screens[0].nominal_frame_interval_ns;
	if (c->settings.nominal_frame_interval_ns == 0) {
		c->settings.nominal_frame_interval_ns = (1000 * 1000 * 1000) / 60; // 60Hz default
	}

	// Get actual window size if we have a window
	if (c->hwnd != nullptr) {
		RECT rect;
		if (GetClientRect(c->hwnd, &rect)) {
			c->settings.preferred.width = rect.right - rect.left;
			c->settings.preferred.height = rect.bottom - rect.top;
		}
	}

	// Open shared texture from app's HANDLE if provided
	c->shared_texture = nullptr;
	c->shared_rtv = nullptr;
	c->has_shared_texture = false;
	if (shared_texture_handle != nullptr) {
		HANDLE h = static_cast<HANDLE>(shared_texture_handle);
		hr = c->device->OpenSharedResource(h, __uuidof(ID3D11Texture2D),
		                                    reinterpret_cast<void **>(&c->shared_texture));
		if (FAILED(hr) || c->shared_texture == nullptr) {
			U_LOG_E("Failed to open shared texture handle %p: 0x%08x", shared_texture_handle, hr);
			d3d11_compositor_destroy(&c->base.base);
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		D3D11_TEXTURE2D_DESC st_desc;
		c->shared_texture->GetDesc(&st_desc);
		U_LOG_W("Opened shared texture: %ux%u format=%u", st_desc.Width, st_desc.Height, st_desc.Format);

		// Use shared texture dimensions for settings when no window
		if (c->hwnd == nullptr) {
			c->settings.preferred.width = st_desc.Width;
			c->settings.preferred.height = st_desc.Height;
		}

		// Create RTV on shared texture
		hr = c->device->CreateRenderTargetView(c->shared_texture, nullptr, &c->shared_rtv);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create RTV on shared texture: 0x%08x", hr);
			d3d11_compositor_destroy(&c->base.base);
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}
		c->has_shared_texture = true;
	}

	/*
	 * #918 STAGE A — resolve the scanout adapter and, if it differs from the
	 * app's, stand up the runtime-owned output device plus the cross-adapter
	 * atlas bridge. Everything here is best-effort: on ANY failure we log the
	 * exact reason once and fall through to the stock single-device path.
	 *
	 * Stage A must stay cheap — the session warmup already shows a brief black
	 * window (weaver async-create + the fullscreen swapchain resize) and the
	 * investigation's explicit constraint is that the split must not lengthen
	 * it. Duration is logged so a regression is visible, not inferred.
	 */
	if (debug_get_bool_option_weave_on_scanout()) {
		const uint64_t stage_a_start_ns = os_monotonic_get_ns();
		double stage_a_bridge_ms = 0.0;
		const char *reason = nullptr;

		if (c->hwnd == nullptr) {
			reason = "no HWND";
		} else if (c->has_shared_texture) {
			reason = "shared-texture session";
		}

		// The app device's adapter — the LUID the scanout adapter is compared
		// against, and the producer side of the bridge.
		IDXGIAdapter *app_adapter = nullptr;
		LUID app_luid = {};
		if (reason == nullptr) {
			IDXGIDevice *dd = nullptr;
			if (SUCCEEDED(c->device->QueryInterface(__uuidof(IDXGIDevice),
			                                        reinterpret_cast<void **>(&dd))) &&
			    dd != nullptr) {
				dd->GetAdapter(&app_adapter);
				dd->Release();
			}
			DXGI_ADAPTER_DESC ad{};
			if (app_adapter == nullptr || FAILED(app_adapter->GetDesc(&ad))) {
				reason = "scanout unresolvable";
			} else {
				app_luid = ad.AdapterLuid;
			}
		}

		wil::com_ptr<IDXGIAdapter> scanout;
		DXGI_ADAPTER_DESC sdesc{};
		if (reason == nullptr) {
			scanout = xrt::auxiliary::d3d::getScanoutAdapter(
			    display_screen_left, display_screen_top, xdev->hmd->screens[0].w_pixels,
			    xdev->hmd->screens[0].h_pixels, U_LOGGING_INFO);
			if (!scanout || FAILED(scanout->GetDesc(&sdesc))) {
				reason = "scanout unresolvable";
			} else if (sdesc.AdapterLuid.LowPart == app_luid.LowPart &&
			           sdesc.AdapterLuid.HighPart == app_luid.HighPart) {
				// Not a failure: on a MUX'd / single-GPU box the weave is
				// already local, so the split has nothing to do. One INFO,
				// no WARN.
				U_LOG_W("D3D11 output-device split: scanout adapter '%ls' LUID=%08lx:%08lx IS the "
				        "app's adapter — split is a no-op (#918)",
				        sdesc.Description, (unsigned long)sdesc.AdapterLuid.HighPart,
				        (unsigned long)sdesc.AdapterLuid.LowPart);
				reason = ""; // handled; suppress the fallback WARN
			}
		}

		// Runtime-owned D3D11 device on the scanout adapter.
		if (reason == nullptr) {
			UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
			D3D_FEATURE_LEVEL got = {};
			static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
			hr = D3D11CreateDevice(scanout.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
			                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &c->out_dev, &got,
			                       &c->out_ctx);
			if (FAILED(hr) || c->out_dev == nullptr || c->out_ctx == nullptr) {
				U_LOG_W("D3D11 output-device split: D3D11CreateDevice failed 0x%08lx",
				        (unsigned long)hr);
				reason = "D3D11CreateDevice failed";
			}
		}
		if (reason == nullptr) {
			// The output context has exactly two callers (layer_commit and the
			// repaint loop, both under c->mutex), but the vendor DP may touch it
			// from its own threads — keep the same protection the app device has.
			ID3D10Multithread *omt = nullptr;
			if (SUCCEEDED(c->out_dev->QueryInterface(__uuidof(ID3D10Multithread),
			                                         reinterpret_cast<void **>(&omt))) &&
			    omt != nullptr) {
				omt->SetMultithreadProtected(TRUE);
				omt->Release();
			}
			// Same one-deep DXGI queue the app device gets: the present now
			// happens HERE, so this is where the pacing lever lives.
			IDXGIDevice1 *od1 = nullptr;
			if (SUCCEEDED(c->out_dev->QueryInterface(__uuidof(IDXGIDevice1),
			                                         reinterpret_cast<void **>(&od1))) &&
			    od1 != nullptr) {
				od1->SetMaximumFrameLatency(1);
				od1->Release();
			}
			hr = scanout->GetParent(__uuidof(IDXGIFactory4),
			                        reinterpret_cast<void **>(&c->out_factory));
			if (FAILED(hr) || c->out_factory == nullptr) {
				U_LOG_W("D3D11 output-device split: scanout DXGI factory failed 0x%08lx",
				        (unsigned long)hr);
				// Its own reason: the device was created fine, it is the
				// adapter's IDXGIFactory4 parent that could not be reached,
				// and the fallback WARN must not misname that.
				reason = "scanout DXGI factory unavailable";
			}
		}

		// The bridge: both D3D12 devices, both COPY queues, the cross-adapter
		// heap + placed ring, every fence, and one probe egress round-trip.
		if (reason == nullptr) {
			uint32_t sys_w = 0, sys_h = 0;
			if (xdev->rendering_mode_count > 0) {
				u_tiling_compute_system_atlas(xdev->rendering_modes, xdev->rendering_mode_count,
				                              &sys_w, &sys_h);
			}
			if (sys_w == 0 || sys_h == 0) {
				sys_w = c->settings.preferred.width;
				sys_h = c->settings.preferred.height;
			}

			struct comp_d3d11_xbridge_info xbi = {};
			xbi.app_device = c->device;
			xbi.app_context = c->context;
			xbi.app_adapter = app_adapter;
			xbi.out_device = c->out_dev;
			xbi.out_context = c->out_ctx;
			xbi.out_adapter = scanout.get();
			xbi.max_width = sys_w;
			xbi.max_height = sys_h;
			/*
			 * #918 Phase 2a: the PANEL extent sizes the bridge planes (and the
			 * Local2D / backdrop scratches that feed them) once, for good. A
			 * window can never exceed the panel, so nothing here ever has to
			 * resize — which is exactly what keeps the planes out of the R2 churn
			 * hysteresis and the #1091 resize path.
			 */
			xbi.panel_width = xdev->hmd->screens[0].w_pixels;
			xbi.panel_height = xdev->hmd->screens[0].h_pixels;

			const char *xb_reason = nullptr;
			const uint64_t xb_t0 = os_monotonic_get_ns();
			if (comp_d3d11_xbridge_create(&xbi, &c->xbridge, &xb_reason) != XRT_SUCCESS) {
				c->xbridge = nullptr;
				reason = (xb_reason != nullptr) ? xb_reason : "cross-adapter heap unsupported";
			} else {
				/*
				 * The egress ring is allocated HERE, not in stage B: the split
				 * must not be able to activate and THEN discover it has nothing
				 * to weave into. Size it at the ACTIVE MODE's nominal content
				 * box rather than the worst-case atlas — same fail-safe, but it
				 * avoids committing (and immediately freeing) ~100 MB of iGPU
				 * memory during the session warmup the investigation asked us
				 * not to lengthen. Stage B refines it to the real content dims.
				 */
				uint32_t eg_w = 0, eg_h = 0;
				if (xdev->hmd != NULL) {
					uint32_t mi = xdev->hmd->active_rendering_mode_index;
					if (mi < xdev->rendering_mode_count) {
						const struct xrt_rendering_mode *m = &xdev->rendering_modes[mi];
						eg_w = m->tile_columns * m->view_width_pixels;
						eg_h = m->tile_rows * m->view_height_pixels;
					}
				}
				bool eg_ok;
				if (eg_w > 0 && eg_h > 0) {
					// Falls back to a worst-case ring internally if the
					// content-sized allocation fails.
					comp_d3d11_xbridge_set_content_size(c->xbridge, eg_w, eg_h, 0);
					uint32_t gw = 0, gh = 0;
					comp_d3d11_xbridge_get_egress_dims(c->xbridge, &gw, &gh);
					eg_ok = (gw > 0 && gh > 0);
				} else {
					eg_ok = comp_d3d11_xbridge_alloc_worstcase_egress(c->xbridge);
				}
				if (!eg_ok) {
					reason = "egress share failed";
				}
			}
			stage_a_bridge_ms = (double)(os_monotonic_get_ns() - xb_t0) / 1.0e6;
		}

		if (reason == nullptr) {
			c->split_active = true;
			c->out_luid = sdesc.AdapterLuid;
			c->split_panel_w = xdev->hmd->screens[0].w_pixels;
			c->split_panel_h = xdev->hmd->screens[0].h_pixels;
			{
				const char *d = getenv("DXR_XBRIDGE_DIAG");
				c->split_diag = (d != nullptr && d[0] == '1');
			}
			U_LOG_W("D3D11 output-device split ACTIVE: weave/repaint/present move to '%ls' "
			        "LUID=%08lx:%08lx (app device on LUID=%08lx:%08lx); atlas crosses via a D3D12 "
			        "cross-adapter heap once per frame (#918)",
			        sdesc.Description, (unsigned long)sdesc.AdapterLuid.HighPart,
			        (unsigned long)sdesc.AdapterLuid.LowPart, (unsigned long)app_luid.HighPart,
			        (unsigned long)app_luid.LowPart);
		} else {
			if (c->xbridge != nullptr) {
				comp_d3d11_xbridge_destroy(&c->xbridge);
			}
			if (c->out_factory != nullptr) {
				c->out_factory->Release();
				c->out_factory = nullptr;
			}
			if (c->out_ctx != nullptr) {
				c->out_ctx->Release();
				c->out_ctx = nullptr;
			}
			if (c->out_dev != nullptr) {
				c->out_dev->Release();
				c->out_dev = nullptr;
			}
			if (reason[0] != '\0') {
				U_LOG_W("D3D11 output-device split DISABLED (%s) — falling back to the stock "
				        "single-device path (#918)",
				        reason);
			}
		}
		if (app_adapter != nullptr) {
			app_adapter->Release();
		}
		// Warmup budget: the session already shows a brief black window while the
		// vendor weaver async-creates, and the investigation's constraint is that
		// the split must not lengthen it. Both numbers are logged so a regression
		// is visible rather than inferred — the bridge (two D3D12 devices, the
		// cross-adapter heap, the fences, the egress ring) dominates.
		U_LOG_W("D3D11 output-device split: stage A took %.1f ms (bridge %.1f ms, active=%d)",
		        (double)(os_monotonic_get_ns() - stage_a_start_ns) / 1.0e6, stage_a_bridge_ms,
		        (int)c->split_active);
	}

	/*
	 * #918 — ONE canonical weave-placement line per session, ALWAYS emitted,
	 * in addition to (never instead of) the Stage-A detail lines above.
	 *
	 * The Stage-A lines only exist when DXR_WEAVE_ON_SCANOUT is set, so
	 * without this a hybrid box silently paying the cross-adapter present
	 * produced a log byte-identical to a single-adapter box that pays
	 * nothing — the same blind spot #1000 closed for adapter SELECTION, now
	 * closed for weave PLACEMENT. Costs one QueryDisplayConfig per session.
	 */
	{
		DXGI_ADAPTER_DESC rdesc = {};
		bool render_ok = false;
		{
			IDXGIDevice *dd = nullptr;
			IDXGIAdapter *ra = nullptr;
			if (SUCCEEDED(
			        c->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dd))) &&
			    dd != nullptr) {
				dd->GetAdapter(&ra);
				dd->Release();
			}
			if (ra != nullptr) {
				render_ok = SUCCEEDED(ra->GetDesc(&rdesc));
				ra->Release();
			}
		}

		DXGI_ADAPTER_DESC pdesc = {};
		bool scanout_ok = false;
		{
			const uint32_t pw = (xdev->hmd != NULL) ? xdev->hmd->screens[0].w_pixels : 0;
			const uint32_t ph = (xdev->hmd != NULL) ? xdev->hmd->screens[0].h_pixels : 0;
			wil::com_ptr<IDXGIAdapter> panel = xrt::auxiliary::d3d::getScanoutAdapter(
			    display_screen_left, display_screen_top, pw, ph, U_LOGGING_INFO);
			scanout_ok = panel != nullptr && SUCCEEDED(panel->GetDesc(&pdesc));
		}

		const WCHAR *rname = render_ok ? rdesc.Description : L"<unknown>";
		const unsigned long rhi = (unsigned long)rdesc.AdapterLuid.HighPart;
		const unsigned long rlo = (unsigned long)rdesc.AdapterLuid.LowPart;

		if (!scanout_ok) {
			// Say so — never guess. Without the scanout adapter there is no
			// way to know whether this session crosses adapters at all.
			U_LOG_W(
			    "weave placement: render='%ls' LUID=%08lx:%08lx, panel scanout=UNRESOLVED — cannot "
			    "tell whether the weave crosses adapters (#918)",
			    rname, rhi, rlo);
		} else if (render_ok && pdesc.AdapterLuid.LowPart == rdesc.AdapterLuid.LowPart &&
		           pdesc.AdapterLuid.HighPart == rdesc.AdapterLuid.HighPart) {
			U_LOG_W(
			    "weave placement: render='%ls' LUID=%08lx:%08lx, panel scanout='%ls' "
			    "LUID=%08lx:%08lx — render and scanout share one adapter; weave is local (#918)",
			    rname, rhi, rlo, pdesc.Description, (unsigned long)pdesc.AdapterLuid.HighPart,
			    (unsigned long)pdesc.AdapterLuid.LowPart);
		} else if (c->split_active) {
			U_LOG_W(
			    "weave placement: render='%ls' LUID=%08lx:%08lx, panel scanout='%ls' "
			    "LUID=%08lx:%08lx — weave/present on the SCANOUT adapter (DXR_WEAVE_ON_SCANOUT "
			    "split active) (#918)",
			    rname, rhi, rlo, pdesc.Description, (unsigned long)pdesc.AdapterLuid.HighPart,
			    (unsigned long)pdesc.AdapterLuid.LowPart);
		} else {
			U_LOG_W(
			    "weave placement: render='%ls' LUID=%08lx:%08lx, panel scanout='%ls' "
			    "LUID=%08lx:%08lx — weave on the RENDER adapter; every present crosses adapters to "
			    "reach scanout (set DXR_WEAVE_ON_SCANOUT=1 to move the weave) (#918)",
			    rname, rhi, rlo, pdesc.Description, (unsigned long)pdesc.AdapterLuid.HighPart,
			    (unsigned long)pdesc.AdapterLuid.LowPart);
		}
	}

	// Create output target (DXGI swapchain) — skip if offscreen (no HWND).
	// #918: under the split the swapchain, its waitable and its frame statistics
	// all live on the SCANOUT adapter, which is what removes the cross-adapter
	// present (~26 ms of the measured 42 ms present-to-display).
	if (c->hwnd != nullptr) {
		xrt_result_t xret = comp_d3d11_target_create(c, c->hwnd, d3d11_out_device(c), d3d11_out_context(c),
		                                             c->split_active ? c->out_factory : c->dxgi_factory,
		                                             c->settings.preferred.width, c->settings.preferred.height,
		                                             transparent_background, &c->target);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create D3D11 target");
			d3d11_compositor_destroy(&c->base.base);
			return xret;
		}
	} else {
		c->target = nullptr;
		U_LOG_I("No DXGI target — offscreen shared texture mode");
	}

	// Current mode of the monitor this session's window lives on. This used to
	// walk the output's mode LIST and take the highest supported rate, which
	// reports 165 Hz for a 165 Hz-capable panel running at 60 — the frame
	// period handed to the display processor was then wrong by that ratio.
	c->display_refresh_rate = 60.0f; // Default to 60Hz
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
		comp_d3d11_target_set_display_period(
		    c->target, (uint64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate));
	}

	/*
	 * #868: the repaint loop, ON by default (DXR_WEAVE_REPAINT=0 disables).
	 *
	 * Hardware-verified on D3D11 as well as D3D12. Getting here took two fixes
	 * that the frame-path hash probe (DXR_WEAVE_REPAINT_HASH=1) localised only
	 * once a mean-luminance classifier split the mismatches into the two
	 * populations they actually contained — a repaint weaving with FRESHER
	 * EYES differs by design and is the feature, whereas ~19% of them were
	 * coming out fully BLACK, which is the flicker:
	 *
	 *   1. The replay skipped comp_d3d11_target_acquire, which is what CLEARS
	 *      the FLIP_DISCARD back buffer. App frames started from black,
	 *      repaints started from discarded garbage.
	 *   2. The replay ran unserialised against the APP's immediate context.
	 *      See mt_lock — SetMultithreadProtected only protects individual
	 *      calls, and a weave is a sequence.
	 *
	 * Measured after both: adjacent repaints reproduce the app frame 1320/1320
	 * with zero black frames, against 500/1320 before.
	 */
	{
		/*
		 * OPT-IN on D3D11 (DXR_WEAVE_REPAINT=1) — see runtime#876.
		 *
		 * The repaint is correct on the handle cube but has an intermittent,
		 * visible defect on zones apps: roughly once every 10 s a zone blinks,
		 * sometimes as a black frame and sometimes as a doubled/zoomed cube.
		 * It is repaint-caused (0 occurrences with the loop off, measured and
		 * eyeballed both ways) and the cause is NOT yet found — three
		 * detectors, each blind to the artefact in a different way, and four
		 * hypotheses eliminated. Details and what has been ruled out are on
		 * #876.
		 *
		 * A visible artefact is worse than a stale interlace, so this stays off
		 * by default until the cause is understood. D3D12 and VK are verified
		 * and remain default-ON.
		 */
		/*
		 * Default ON again (DXR_WEAVE_REPAINT=0 disables), matching D3D12/VK.
		 *
		 * The opt-in was based on #876, which turned out NOT to be a repaint
		 * bug at all: the zones test app's GetAsyncKeyState hotkeys are
		 * GLOBAL, and another session typing 'o'/'m' anywhere on the box was
		 * genuinely moving zone B onto zone A (the "double cube") and
		 * republishing the wish mask (the black transients). Reproduced on
		 * demand with injected keypresses; identical with the repaint
		 * disabled (134 vs 141 dropouts). The app is now focus-gated and the
		 * repaint exonerated.
		 */
		const char *e = getenv("DXR_WEAVE_REPAINT");
		c->repaint.enabled = (e != nullptr && e[0] == '0') ? 0 : 1;
		const char *fe = getenv("DXR_WEAVE_REPAINT_FORCE");
		c->repaint.force = (fe != nullptr && fe[0] == '1') ? 1 : 0;
		const char *he = getenv("DXR_WEAVE_REPAINT_HASH");
		c->repaint.hash_probe = (he != nullptr && he[0] == '1') ? 1 : 0;
		if (c->repaint.force == 1) {
			U_LOG_W("#868: DXR_WEAVE_REPAINT_FORCE=1 — repainting every refresh regardless of app "
			        "rate. Correctness probe; it WILL cost frame rate.");
		}
		if (c->repaint.enabled == 1 && c->target != nullptr) {
			c->repaint_quit.store(false);
			c->repaint_thread = std::thread(d3d11_repaint_thread, c);
		}
	}

	// Determine view dimensions for the atlas texture.
	// Default: derive from window size (half width for 2-view atlas)
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

	// Create display processor via factory (set by the target builder at init time).
	if (dp_factory_d3d11 != NULL) {
		auto factory = (xrt_dp_factory_d3d11_fn_t)dp_factory_d3d11;
		// Use app HWND for position tracking in shared texture mode,
		// otherwise use the compositor's own HWND
		HWND dp_hwnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
		// #918: under the split the weaver is created on the OUTPUT device, so
		// it weaves and presents local to the panel. One weaver per HWND — this
		// factory call is the single creation point and it runs exactly once,
		// after out_dev exists.
		xrt_result_t dp_ret =
		    factory(d3d11_out_device(c), d3d11_out_context(c), dp_hwnd, &c->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("D3D11 display processor factory failed (error %d), continuing without",
			        (int)dp_ret);
			c->display_processor = nullptr;
		} else {
			U_LOG_W("D3D11 display processor created via factory");
			// Forward session-level transparency (#573 — chroma-key-free).
			// client_presents=false — DELIBERATELY, and #904's true here was
			// wrong for in-process (reverted after a hardware eyeball): the
			// DE-OCCLUSION BAND (pixels where SOME but not all views are
			// transparent — the parallax fringe around 3D content) cannot
			// come from DWM live blending. The SR weaver destroys per-pixel
			// alpha and the alpha-gate reconstructs only the binary
			// all-views-transparent mask, so the band is either filled by
			// the DP's compose-under-bg (~1-frame-stale bake, the product
			// spec: live desktop in the holes, bake only in the band) or it
			// is BLACK. #904 disabled the compose calling it wasted work —
			// the dwm saving it measured partly bought black de-occlusions.
			// The WGC cost is attacked by capture throttling instead, not by
			// dropping the band. client_presents=true remains correct for
			// true client-side presents (#551 IPC) and for content with no
			// partial-alpha band.
			xrt_display_processor_d3d11_set_transparent_background(
			    c->display_processor, transparent_background, false);
			// #68: tell the DP whether the app self-presents only the canvas (a
			// _texture app blits its shared texture's canvas to its own window)
			// vs the runtime presenting the full target (handle apps). The DP
			// uses this + its zones state to skip the compose-under-bg desktop-UV
			// remap that would otherwise magnify the captured desktop. Replaces
			// the DISPLAYXR_ZONES_TEXTURE_FIX prototype env flag.
			xrt_display_processor_d3d11_set_shared_texture_present(
			    c->display_processor, c->has_shared_texture);
		}
	} else {
		U_LOG_W("No D3D11 display processor factory provided");
	}

	// #625 (v2): let the runtime-owned window phase-snap its own drags via
	// the DP's snap_window_rect (vtable slot 18). Handle apps don't need
	// this — the app owns the window and the vendor SDK handles its drags.
	// A DP that leaves the slot NULL (no interlace lattice, FPGA weave,
	// older plug-in) simply never snaps: the wrapper returns false and the
	// window moves freely, exactly as before.
	if (c->display_processor != nullptr && c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_set_snap_provider(c->own_window, d3d11_compositor_snap_window_rect_cb, c);

		// Prewarm one snap from THIS thread, before any rendering runs.
		// Load-bearing for the Leia v1 (pre-1.37) fallback: its snap drives
		// a probe window that is thread-affine and shares our immediate
		// context, so it must be created here — after this, window-thread
		// snap calls hit the plugin's own thread guard and return false
		// (v1 keeps the SDK's in-window subclass behaviour, no double
		// snap). On v2 the snap is a pure function and this is just a
		// probe of availability.
		RECT wr = {};
		if (c->hwnd != nullptr && GetWindowRect(c->hwnd, &wr)) {
			int32_t sx = wr.left;
			int32_t sy = wr.top;
			bool ok = xrt_display_processor_d3d11_snap_window_rect(
			    c->display_processor, wr.left, wr.top, wr.left, wr.top, &sx, &sy);
			U_LOG_W("D3D11: window drag phase-snap provider armed (prewarm: %s)",
			        ok ? "snap available" : "declined — window will move unsnapped");
		}
	}

	// If display processor is available, query display pixel info to compute
	// optimal view dimensions (scaled to window size).
	if (c->display_processor != nullptr) {
		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (xrt_display_processor_d3d11_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h,
		        &disp_left, &disp_top) &&
		    disp_px_w > 0 && disp_px_h > 0) {
			// Use half display width as base view dims
			uint32_t base_vw = disp_px_w / 2;
			uint32_t base_vh = disp_px_h;

			U_LOG_W("Display pixel info: %ux%u, base view dims: %ux%u per eye",
			        disp_px_w, disp_px_h, base_vw, base_vh);

			// Scale by window/display pixel ratio (same as resize path)
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

	// Create renderer with the correct view dimensions.
	// When a display processor is present, the atlas texture height must match
	// view_height so the display processor's UV 0..1 maps exactly to the rendered content.
	// Without a display processor, use the window height so the atlas texture is tall enough
	// for mono fallback blitting.  Mono/2D mode uses a GPU stretch blit to fill the full
	// window regardless of atlas texture height.
	// NOTE: the per-frame resize path (renderer_resize) must apply the same guard —
	// see resize_target_h in the mode-switch handler above.
	uint32_t target_height = (c->display_processor != NULL) ? view_height : c->settings.preferred.height;
	// #918: only the split asks for an NT-shareable atlas. The non-split
	// allocation shape is unchanged, byte for byte.
	xrt_result_t xret = comp_d3d11_renderer_create(c, view_width, view_height, target_height, c->split_active,
	                                               &c->renderer);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create D3D11 renderer");
		d3d11_compositor_destroy(&c->base.base);
		return xret;
	}

	/*
	 * #918 Phase 2a — the masked 2D-over-3D composite unit, on the OUTPUT device.
	 *
	 * The composite's destination is the weave target, which Phase 1 moved to the
	 * scanout adapter, so the pass has to run there no matter what — that is the
	 * reframe the whole phase turns on. Every input now arrives on that device:
	 * the mask rasterizers are constructed there, the Local2D flatten and the
	 * authored Tier-3 mask cross as bridge planes, and the weave scratch is unit-
	 * owned. `d3d11_out_device` collapses to the app device when the split is off,
	 * so the non-split path is byte-identical to PR 2a-1.
	 *
	 * Fatal on failure, like the renderer's shader compile it replaces.
	 */
	xret = comp_d3d11_outcomp_create(d3d11_out_device(c), d3d11_out_context(c), &c->outcomp);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create the D3D11 output composite unit");
		d3d11_compositor_destroy(&c->base.base);
		return xret;
	}

	/*
	 * #918 STAGE B — right-size the egress ring and bind the ingress, now that
	 * the DP factory has returned and the vendor's ~200 ms async weaver create
	 * is running in the background. A failure here is NOT fatal: Stage A already
	 * allocated a worst-case ring, so the fallback is simply that the consumer
	 * copy lands worst-case-sized and d3d11_crop_atlas_for_dp runs on the OUTPUT
	 * device, exactly as the non-split path crops on the app device.
	 */
	if (c->split_active) {
		uint32_t cols = 2, rows = 1;
		comp_d3d11_renderer_get_tile_layout(c->renderer, &cols, &rows);
		uint32_t cw = cols * view_width;
		uint32_t ch = rows * view_height;
		if (cw == 0 || ch == 0) {
			cw = c->settings.preferred.width;
			ch = c->settings.preferred.height;
		}
		if (!comp_d3d11_xbridge_set_content_size(c->xbridge, cw, ch, c->split_layout_gen)) {
			U_LOG_W("D3D11 output-device split: stage B could not size the egress ring to %ux%u — "
			        "keeping the worst-case ring and cropping on the output device (#918)",
			        cw, ch);
		}
		// Ingress Option I: the producer reads the renderer's atlas directly.
		// A NULL handle (or a failed D3D12 open) latches Option II inside the
		// bridge, with its own one-off WARN.
		comp_d3d11_xbridge_bind_atlas(c->xbridge, comp_d3d11_renderer_get_atlas_shared_handle(c->renderer),
		                              comp_d3d11_renderer_get_atlas_generation(c->renderer));
	}

	// Expose current window-scaled capture dims to xrCaptureAtlasDXR (#431).
	u_capture_dims_set_provider(d3d11_compositor_capture_dims_provider, c);

#ifdef XRT_FEATURE_DEBUG_GUI
	// Create debug GUI (controlled by XRT_DEBUG_GUI env var)
	struct u_debug_gui_create_info udgci = {};
	udgci.open = U_DEBUG_GUI_OPEN_AUTO;
	strncpy(udgci.window_title, "Monado D3D11 Debug", U_DEBUG_GUI_WINDOW_TITLE_MAX - 1);

	int gui_ret = u_debug_gui_create(&udgci, &c->debug_gui);
	if (gui_ret == 0 && c->debug_gui != nullptr) {
		// Debug GUI was created, now create the readback module
		// Stereo texture is 2x view width
		xrt_result_t xret2 = comp_d3d11_debug_create(c->device, view_width * 2, view_height, &c->debug);
		if (xret2 != XRT_SUCCESS) {
			U_LOG_W("Failed to create D3D11 debug readback, debug GUI preview disabled");
			c->debug = nullptr;
		} else {
			// Add debug variables to u_var
			comp_d3d11_debug_add_vars(c->debug);
		}

		// Note: u_debug_gui_start() is called later when xsysd is available
		// For now, we just create the resources
		U_LOG_I("D3D11 debug GUI created (set XRT_DEBUG_GUI=1 to enable window)");
	}
#endif

	// Initialize layer accumulator - just zero it
	memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Create HUD overlay for runtime-owned windows
	c->hud = NULL;
	c->hud_texture = nullptr;
	c->hud_initialized = false;
	c->last_frame_time_ns = 0;
	c->smoothed_frame_time_ms = 16.67f;
	if (c->owns_window) {
		u_hud_create(&c->hud, c->settings.preferred.width);
	}

	// Populate supported swapchain formats (DXGI formats for D3D11)
	// These are the common formats that D3D11 applications can use
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

	U_LOG_I("D3D11 native compositor supports %u swapchain formats", format_count);

	// Set initial visibility/focus state for session state machine
	// Native in-process compositor is always visible and focused
	c->base.base.info.initial_visible = true;
	c->base.base.info.initial_focused = true;

	// Set up compositor interface
	c->base.base.get_swapchain_create_properties = d3d11_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = d3d11_compositor_create_swapchain;
	c->base.base.import_swapchain = d3d11_compositor_import_swapchain;
	c->base.base.import_fence = d3d11_compositor_import_fence;
	c->base.base.create_semaphore = d3d11_compositor_create_semaphore;
	c->base.base.begin_session = d3d11_compositor_begin_session;
	c->base.base.end_session = d3d11_compositor_end_session;
	c->base.base.wait_frame = d3d11_compositor_wait_frame;
	c->base.base.predict_frame = d3d11_compositor_predict_frame;
	c->base.base.mark_frame = d3d11_compositor_mark_frame;
	c->base.base.begin_frame = d3d11_compositor_begin_frame;
	c->base.base.discard_frame = d3d11_compositor_discard_frame;
	c->base.base.layer_begin = d3d11_compositor_layer_begin;
	c->base.base.layer_projection = d3d11_compositor_layer_projection;
	c->base.base.layer_projection_depth = d3d11_compositor_layer_projection_depth;
	c->base.base.layer_quad = d3d11_compositor_layer_quad;
	c->base.base.layer_cube = d3d11_compositor_layer_cube;
	c->base.base.layer_cylinder = d3d11_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = d3d11_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = d3d11_compositor_layer_equirect2;
	c->base.base.layer_passthrough = d3d11_compositor_layer_passthrough;
	c->base.base.layer_window_space = d3d11_compositor_layer_window_space;
	c->base.base.layer_local_2d = d3d11_compositor_layer_local_2d;
	c->base.base.layer_zone_3d = d3d11_compositor_layer_zone_3d;
	c->base.base.layer_commit = d3d11_compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = d3d11_compositor_layer_commit_with_semaphore;
	c->base.base.destroy = d3d11_compositor_destroy;

	*out_xc = &c->base;

	U_LOG_IFL_I(U_LOGGING_INFO, "D3D11 native compositor created successfully (%ux%u)",
	            c->settings.preferred.width, c->settings.preferred.height);

	return XRT_SUCCESS;
}


/*
 *
 * XR_DXR_local_3d_zone — authored 2D/3D mask consumer (#439 Phase 1).
 *
 * The oxr handlers (oxr_local_3d_zone.c) forward here. The mask expresses an
 * arbitrary scalar 2D/3D region:
 * the masked-composite shader's use_rect_mask = 0 path lerps
 * M·weave + (1−M)·twod per pixel. Authoring happens on the app's thread,
 * consumption inside d3d11_compositor_layer_commit — both serialize on
 * c->mutex (the entry points lock it; layer_commit already holds it), which
 * also makes submit atomic against an in-flight frame (spec §9 Q3).
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
struct comp_d3d11_zone_mask
{
	//! Authoring texture: R8_UNORM, M in [0,1] (1 = 3D / keep the weave).
	ID3D11Texture2D *tex;
	//! RTV on tex — handed to the app for Tier 3, used for Tier 1/2 fills.
	ID3D11RenderTargetView *rtv;
	//! Staged snapshot sampled by the composite (decouples in-progress
	//! authoring from the frame; refreshed by zone_mask_submit).
	ID3D11Texture2D *staged;
	ID3D11ShaderResourceView *staged_srv;
	/*!
	 * #918 Phase 2a — OUTPUT-DEVICE SHADOW.
	 *
	 * Tier-1 (whole) and Tier-2 (rects) authoring are pure CPU-rect ClearView
	 * rasters — they sample nothing — so under the split they are simply RUN
	 * AGAIN on the output device and the composite reads the shadow. Nothing
	 * crosses the bridge for them, which is the single largest simplification in
	 * this phase. NULL when the split is off.
	 */
	ID3D11Texture2D *out_tex;
	ID3D11RenderTargetView *out_rtv;
	ID3D11Texture2D *out_staged;
	ID3D11ShaderResourceView *out_staged_srv;
	/*!
	 * Tier 3 is the exception: the APP draws into @ref rtv on the APP device, so
	 * those pixels genuinely have to cross. Set the first time
	 * xrAcquireLocal3DZoneMaskRenderTargetDXR hands the RTV out, and never
	 * cleared — a mask that has ever been app-authored keeps crossing, because a
	 * later Tier-1/2 fill on the shadow could not reproduce what the app drew.
	 */
	bool app_authored;
	//! #918 Phase 2a: NT share handle of @ref staged + its realloc generation,
	//! so the bridge's producer can open it as the authored-mask plane source.
	HANDLE staged_share;
	uint64_t staged_gen;
	//! Mask dimensions in client-window pixels.
	uint32_t w, h;
	//! True once submitted at least once (an unsubmitted mask is invisible).
	bool submitted;
	//! Author generation: bumped on every authoring call (Tier 1/2 fills,
	//! Tier-3 acquire, submit). The zones frame-wish publish dedups on this
	//! (XR_DXR_display_zones — referenced per-frame, no submit required).
	uint64_t author_seq;
};

// Release the compositor-owned zone consumables (weave scratch) and detach
// any active mask (the oxr handle owns the mask object itself). Idempotent;
// called from d3d11_compositor_destroy only.
static void
d3d11_release_zone_state(struct comp_d3d11_compositor *c)
{
	c->active_zone_mask = nullptr;
	c->frame_wish = nullptr;
	c->frame_wish_last = nullptr;
	c->zones_frame = false;
	d3d11_sync_zone_mask_to_dp(c); // withdraw this client's DP zone contribution (#224)

	// XR_DXR_display_zones — auto-wish raster set.
	if (c->wish_mask_staged_srv != nullptr) {
		c->wish_mask_staged_srv->Release();
		c->wish_mask_staged_srv = nullptr;
	}
	if (c->wish_mask_staged != nullptr) {
		c->wish_mask_staged->Release();
		c->wish_mask_staged = nullptr;
	}
	if (c->wish_mask_rtv != nullptr) {
		c->wish_mask_rtv->Release();
		c->wish_mask_rtv = nullptr;
	}
	if (c->wish_mask_tex != nullptr) {
		c->wish_mask_tex->Release();
		c->wish_mask_tex = nullptr;
	}
	c->wish_mask_w = 0;
	c->wish_mask_h = 0;
	c->wish_rect_count = 0;
	// #800/#803 — per-zone opt-in feather composite mask.
	if (c->feather_mask_staged_srv != nullptr) {
		c->feather_mask_staged_srv->Release();
		c->feather_mask_staged_srv = nullptr;
	}
	if (c->feather_mask_staged != nullptr) {
		c->feather_mask_staged->Release();
		c->feather_mask_staged = nullptr;
	}
	if (c->feather_mask_rtv != nullptr) {
		c->feather_mask_rtv->Release();
		c->feather_mask_rtv = nullptr;
	}
	if (c->feather_mask_tex != nullptr) {
		c->feather_mask_tex->Release();
		c->feather_mask_tex = nullptr;
	}
	c->feather_mask_w = 0;
	c->feather_mask_h = 0;
	c->feather_rect_count = 0;
	// (#918 Phase 2a: the weave scratch moved to comp_d3d11_outcomp, which
	// owns it on the composite's device and releases it in its own destroy.)

	// #439 Phase 3 — runtime-owned Local2D flatten scratch + implicit mask.
	if (c->local2d_scratch_rtv != nullptr) {
		c->local2d_scratch_rtv->Release();
		c->local2d_scratch_rtv = nullptr;
	}
	if (c->local2d_scratch_srv != nullptr) {
		c->local2d_scratch_srv->Release();
		c->local2d_scratch_srv = nullptr;
	}
	if (c->local2d_scratch != nullptr) {
		c->local2d_scratch->Release();
		c->local2d_scratch = nullptr;
	}
	// #918 Phase 2a — the plane-source share handles.
	if (c->local2d_scratch_share != nullptr) {
		CloseHandle(c->local2d_scratch_share);
		c->local2d_scratch_share = nullptr;
	}
	// #491 part 3 — 2D-under backdrop flatten scratch.
	if (c->backdrop_scratch_rtv != nullptr) {
		c->backdrop_scratch_rtv->Release();
		c->backdrop_scratch_rtv = nullptr;
	}
	if (c->backdrop_scratch_srv != nullptr) {
		c->backdrop_scratch_srv->Release();
		c->backdrop_scratch_srv = nullptr;
	}
	if (c->backdrop_scratch != nullptr) {
		c->backdrop_scratch->Release();
		c->backdrop_scratch = nullptr;
	}
	if (c->backdrop_scratch_share != nullptr) {
		CloseHandle(c->backdrop_scratch_share);
		c->backdrop_scratch_share = nullptr;
	}
	// #918 Phase 2a — the region-sized DP views of the bridge planes.
	for (struct comp_d3d11_compositor::d3d11_dp_plane_view *v : {&c->dp_bd_view, &c->dp_mask_view}) {
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
	if (c->implicit_mask_staged_srv != nullptr) {
		c->implicit_mask_staged_srv->Release();
		c->implicit_mask_staged_srv = nullptr;
	}
	if (c->implicit_mask_staged != nullptr) {
		c->implicit_mask_staged->Release();
		c->implicit_mask_staged = nullptr;
	}
	if (c->implicit_mask_rtv != nullptr) {
		c->implicit_mask_rtv->Release();
		c->implicit_mask_rtv = nullptr;
	}
	if (c->implicit_mask_tex != nullptr) {
		c->implicit_mask_tex->Release();
		c->implicit_mask_tex = nullptr;
	}
	c->implicit_mask_w = 0;
	c->implicit_mask_h = 0;
	c->implicit_rect_count = 0;
}

// #224 hardware-DP zone leg — keep the DP's view of this client's zone mask in
// sync with the compositor's. While a submitted mask is active, republish it
// every frame (the screen rect tracks window moves/resizes; vendors coalesce
// per their max_update_hz). On the active→inactive edge (mask destroyed,
// compositor teardown), clear once so this client's contribution drops out of
// the vendor's OR-union. Caller holds c->mutex (layer_commit / the zone entry
// points), matching the rest of the zone state machine.
//
// Occlusion subtraction (Z-order walk, local-3d-zones.md "runtime per-frame
// work" step 2-3) is deferred — Phase 0 publishes the un-occluded mask; on
// 1×1-grid hardware the union result is identical.
// One-time DP zone-capability probe, cached on the compositor (caller holds
// c->mutex). Returns true when the DP consumes published zone masks; caps are
// then in c->zone_dp_caps.
static bool
d3d11_zone_dp_supported(struct comp_d3d11_compositor *c)
{
	if (c->display_processor == nullptr) {
		return false;
	}
	if (c->zone_dp_state == 0) { // 0 = unqueried, 1 = supported, 2 = legacy
		struct xrt_dp_local_zone_caps caps = {};
		caps.struct_size = sizeof(caps);
		bool ok = xrt_display_processor_d3d11_get_local_zone_caps(c->display_processor, &caps);
		c->zone_dp_state = (ok && caps.supported != 0) ? 1 : 2;
		if (c->zone_dp_state == 1) {
			c->zone_dp_caps = caps;
			// ADR-027 appends (wish_fractional / switch_granularity) read 0
			// from pre-append plug-ins — the conservative defaults.
			U_LOG_W("D3D11 zone DP: local zones supported, grid %ux%u max_mask %ux%u max_hz %u "
			        "wish_fractional=%u granularity=%u",
			        caps.zone_grid_width, caps.zone_grid_height, caps.max_mask_width, caps.max_mask_height,
			        caps.max_update_hz, caps.wish_fractional, caps.switch_granularity);
		}
	}
	return c->zone_dp_state == 1;
}

/*!
 * #918 Phase 2a — a REGION-SIZED output-device view of a bridge plane, for the
 * DP sideband entry points that take dims but no uv scale.
 *
 * Three properties the first cut did not have:
 *
 * - **Passthrough when the plane already IS the requested extent.** No texture,
 *   no copy, no bytes. That is now the mask publish's normal path, because the
 *   authored-mask plane is sized at the mask (#918 review F5).
 * - **SEQ-GATED (#918 review F8).** The copy re-runs only when the slot's plane
 *   generation differs from the one the view already holds. An unchanged
 *   backdrop under a forced repaint copies NOTHING; the old code re-copied a
 *   full 4K region on every weave AND every repaint tick, which at panel rate is
 *   GB/s of output-device traffic that no meter could see.
 * - **CLAMPED and METERED.** The box is min(src, dst) — an oversized
 *   `CopySubresourceRegion` is silently DROPPED by D3D11, which would have left
 *   a vendor sampling an uninitialised texture (#918 review F6) — and the bytes
 *   land in the DIAG line.
 *
 * Returns NULL on any failure, which the callers already treat as "no backdrop"
 * / "keep the previous publish" — a degraded feature, never a broken frame.
 *
 * @param content_seq The plane generation @p plane_srv carries, from the slot's
 *        recipe. 0 means "unknown", which forces the copy.
 */
static ID3D11ShaderResourceView *
d3d11_plane_dp_view(struct comp_d3d11_compositor *c,
                    struct comp_d3d11_compositor::d3d11_dp_plane_view *v,
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
	if (FAILED(pres->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&ptex))) ||
	    ptex == nullptr) {
		pres->Release();
		return nullptr;
	}
	ptex->GetDesc(&pd);
	ptex->Release();

	// Passthrough: the plane is exactly what the DP asked for.
	if (pd.Width == w && pd.Height == h) {
		pres->Release();
		return psrv;
	}

	bool need_alloc = (v->tex == nullptr) || (v->srv == nullptr) || v->w != w || v->h != h;
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
		HRESULT hr = d3d11_out_device(c)->CreateTexture2D(&td, nullptr, &v->tex);
		if (SUCCEEDED(hr) && v->tex != nullptr) {
			hr = d3d11_out_device(c)->CreateShaderResourceView(v->tex, nullptr, &v->srv);
		}
		if (FAILED(hr) || v->srv == nullptr) {
			// #918 review D7: latched. This runs on the frame path (and on
			// every repaint tick), and the repo forbids a per-frame WARN.
			if (!v->warned) {
				v->warned = true;
				U_LOG_W(
				    "%s: DP-view alloc (%ux%u) failed 0x%08x — that sideband degrades for this "
				    "session (#918)",
				    what, w, h, hr);
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
		// #918 review F8: same pixels, already copied. This is the steady state
		// under a repaint — the whole reason the split is worth having.
		c->split_sideband_skips++;
		pres->Release();
		return v->srv;
	}

	// #918 review F6: min(src, dst). D3D11 drops an oversized box silently.
	uint32_t cw = (w < pd.Width) ? w : pd.Width;
	uint32_t ch = (h < pd.Height) ? h : pd.Height;
	if (cw == 0 || ch == 0) {
		pres->Release();
		return nullptr;
	}
	D3D11_BOX box = {0, 0, 0, cw, ch, 1};
	d3d11_out_context(c)->CopySubresourceRegion(v->tex, 0, 0, 0, 0, pres, 0, &box);
	pres->Release();
	v->seq = content_seq;
	c->split_sideband_copies++;
	c->split_sideband_bytes += (uint64_t)cw * ch * dxgi_format_bytes_per_pixel(fmt);
	return v->srv;
}

/*!
 * The frame's AUTHORITATIVE authored mask, or NULL when it has none.
 *
 * A zones frame's is its explicit frame wish; a legacy frame's is the sticky
 * submitted mask. They are mutually exclusive by construction — which is exactly
 * why the single authored-mask plane needs one arbiter rather than two
 * independent binders (#918 review D6).
 */
static struct comp_d3d11_zone_mask *
d3d11_frame_authored_mask(struct comp_d3d11_compositor *c)
{
	if (c->zones_frame) {
		return c->frame_wish;
	}
	struct comp_d3d11_zone_mask *m = c->active_zone_mask;
	return (m != nullptr && m->submitted) ? m : nullptr;
}

/*!
 * #918 review D6/F1 — the ONE per-frame publisher of the authored-mask bridge
 * plane. Called from layer_commit before the deposit half, with the frame's
 * authoritative mask (which may be NULL).
 *
 * Two properties matter and neither survived the scattered binds:
 *
 * 1. **One producer.** Binding the same plane from two call sites with two
 *    globally-unique generations made alternating frames re-open it every frame,
 *    each re-open a blocking producer drain plus a full re-transport.
 *
 * 2. **Staging does not depend on consumption.** The transport is set up here,
 *    before anything asks whether the plane's pixels have landed. That breaks
 *    the F1 cycle: the frame that authors a mask transports it, and consumption
 *    starts on whichever later frame the slot lands — instead of the composite
 *    refusing to deposit because the plane it was about to fill was still empty.
 */
static void
d3d11_stage_mask_plane(struct comp_d3d11_compositor *c, struct comp_d3d11_zone_mask *mask)
{
	if (!c->split_active || c->xbridge == nullptr) {
		c->mask_plane_live = false;
		c->mask_plane_gen = 0;
		return;
	}

	/*
	 * Tier 1/2 need no transport at all — their rasters are pure CPU rects and
	 * were simply run again on the output-device shadow.
	 *
	 * #918 review D5: unless that shadow could not be allocated. The WARN at
	 * create said "Tier-1/2 masks degrade under the split" and nothing degraded —
	 * the composite simply had no mask on the output device, bailed for ever, and
	 * the DP's mask was cleared. A mask with no shadow rides the plane instead,
	 * whatever tier authored it: the transport already exists, the pixels are the
	 * same, and it costs one R8 copy per authoring call.
	 */
	const bool needs_plane = mask != nullptr && (mask->app_authored || mask->out_staged_srv == nullptr);
	if (mask == nullptr || !needs_plane || mask->staged_share == nullptr) {
		comp_d3d11_xbridge_stage_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_MASK, 0, 0, 0, 0, 0);
		c->mask_plane_live = false;
		c->mask_plane_gen = 0;
		return;
	}

	/*
	 * Sized at the MASK, not the panel (#918 review F5) — the composite and the
	 * DP both stretch the whole mask over the region, so the whole mask is what
	 * has to cross.
	 */
	if (!comp_d3d11_xbridge_bind_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_MASK, mask->staged_share,
	                                   mask->staged_gen, (uint32_t)DXGI_FORMAT_R8_UNORM, mask->w, mask->h)) {
		// Transport unavailable (no share handle, or the stack refused an R8
		// cross-adapter heap). Tier-1/2 content still composites through the
		// shadow; the Tier-3 strokes do not.
		comp_d3d11_xbridge_stage_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_MASK, 0, 0, 0, 0, 0);
		c->mask_plane_live = false;
		c->mask_plane_gen = 0;
		return;
	}

	/*
	 * Content generation: unique across mask OBJECTS as well as across authoring
	 * calls on one, since a session can hand this single plane two different
	 * masks and two masks both at author_seq 1 must not look like the same
	 * pixels. Mixed rather than bit-tagged, so no bit budget is spent on which
	 * call site produced it — there is only one now.
	 */
	uint64_t seq = 1469598103934665603ull;
	seq = (seq ^ mask->staged_gen) * 1099511628211ull;
	seq = (seq ^ mask->author_seq) * 1099511628211ull;
	if (seq == 0) {
		seq = 1; // 0 is reserved for "this frame does not use the plane"
	}
	comp_d3d11_xbridge_stage_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_MASK, seq, 0, 0, mask->w, mask->h);
	c->mask_plane_live = true;
	c->mask_plane_gen = mask->staged_gen;
}

/*!
 * #918 Phase 2a — the SRV of @p mask ON THE COMPOSITE DEVICE, plus how it got
 * there (a COMP_D3D11_XBRIDGE_MASK_* kind for the recipe stamp).
 *
 * Three cases, and only one of them costs bandwidth:
 *   - split off → the app-device staged snapshot, verbatim.
 *   - split on, Tier-1/2 only → the OUTPUT-DEVICE SHADOW. The rasters are pure
 *     CPU rects, so they were simply run again over there; nothing crossed.
 *   - split on, ever Tier-3 → the bridge plane for @p slot. The app drew those
 *     pixels, so they genuinely have to be transported.
 *
 * @param slot Egress slot to read a bridged mask from; -1 asks for the published
 *        weave slot (what the DP sideband publish and a repaint want).
 * @param out_seq Receives the plane generation the returned SRV carries (0 when
 *        the mask did not come off the bridge), for the sideband's copy gate.
 */
static ID3D11ShaderResourceView *
d3d11_zone_mask_consume_srv(struct comp_d3d11_compositor *c,
                            struct comp_d3d11_zone_mask *mask,
                            int32_t slot,
                            uint32_t *out_kind,
                            uint64_t *out_seq)
{
	if (out_kind != nullptr) {
		*out_kind = COMP_D3D11_XBRIDGE_MASK_OUT_RASTER;
	}
	if (out_seq != nullptr) {
		*out_seq = 0;
	}
	if (mask == nullptr) {
		if (out_kind != nullptr) {
			*out_kind = COMP_D3D11_XBRIDGE_MASK_NONE;
		}
		return nullptr;
	}
	if (!c->split_active) {
		return mask->staged_srv;
	}
	/*
	 * #918 review D6: the KIND follows this frame's single publisher, not a
	 * per-site guess. `mask_plane_live` is false whenever the plane could not be
	 * bound (no share handle, a stack that refuses R8 cross-adapter) or the mask
	 * does not need it — and then the output-device shadow is both the right
	 * answer and the honest one to stamp on the recipe.
	 */
	if (!c->mask_plane_live || c->mask_plane_gen != mask->staged_gen) {
		return mask->out_staged_srv; // NULL when the shadow itself failed (#918 review D5)
	}
	if (out_kind != nullptr) {
		*out_kind = COMP_D3D11_XBRIDGE_MASK_PLANE;
	}
	if (slot < 0) {
		slot = comp_d3d11_xbridge_get_weave_slot(c->xbridge);
	}
	/*
	 * #918 review F3 — the slot's own recipe says which generation its mask
	 * plane holds, and whether those pixels are the ones its frame staged. Pass
	 * that back into the bridge so mismatched pixels come back as NULL (the
	 * callers' "not yet / keep the previous publish" path) rather than being
	 * composited or published under the wrong recipe.
	 */
	struct comp_d3d11_xbridge_recipe rec = {};
	if (!comp_d3d11_xbridge_slot_recipe(c->xbridge, slot, &rec) ||
	    (rec.plane_valid & (1u << COMP_D3D11_XBRIDGE_PLANE_MASK)) == 0) {
		return nullptr;
	}
	const uint64_t want = rec.plane_seq[COMP_D3D11_XBRIDGE_PLANE_MASK];
	if (out_seq != nullptr) {
		*out_seq = want;
	}
	return static_cast<ID3D11ShaderResourceView *>(
	    comp_d3d11_xbridge_get_plane_srv(c->xbridge, slot, COMP_D3D11_XBRIDGE_PLANE_MASK, want));
}

//! Refresh @p mask's staged snapshot on every device that consumes it.
static void
d3d11_zone_mask_stage(struct comp_d3d11_compositor *c, struct comp_d3d11_zone_mask *mask)
{
	if (mask == nullptr || mask->staged == nullptr || mask->tex == nullptr) {
		return;
	}
	/*
	 * #918 review, R1-adjacent: `mask->staged` is a bridge PLANE SOURCE the
	 * producer opened directly, so this write has to be ordered behind the
	 * producer's in-flight read of the previous seq — the same back-fence
	 * layer_commit takes for the atlas, applied here because this copy can run
	 * from an OpenXR entry point BEFORE layer_commit does. GPU-side only.
	 */
	if (c->split_active && c->xbridge != nullptr) {
		comp_d3d11_xbridge_pre_plane_write(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_MASK);
	}
	c->context->CopyResource(mask->staged, mask->tex);
	if (c->split_active && mask->out_staged != nullptr && mask->out_tex != nullptr) {
		c->out_ctx->CopyResource(mask->out_staged, mask->out_tex);
	}
}

static void
d3d11_sync_zone_mask_to_dp(struct comp_d3d11_compositor *c)
{
	if (!d3d11_zone_dp_supported(c)) {
		return; // legacy DP — global request_display_mode path unchanged.
	}

	// Resolve this frame's published content. Zones frame
	// (XR_DXR_display_zones): the WISH — explicit frame wish or the auto
	// raster — and the sticky legacy mask is inert. Legacy frame: the
	// sticky submitted mask, unchanged.
	ID3D11ShaderResourceView *srv = nullptr;
	uint32_t mask_w = 0;
	uint32_t mask_h = 0;
	// #918 Phase 2a: every SRV below is resolved ON THE DP'S DEVICE — the auto
	// wish / feather rasters are built there in the first place, and an authored
	// mask resolves to its output-device shadow (Tier 1/2) or its bridge plane
	// (Tier 3). The publish's contract already took the context and the SRV as
	// parameters, so there is no vtable change here at all.
	uint32_t pub_kind = COMP_D3D11_XBRIDGE_MASK_NONE;
	uint64_t pub_seq = 0;
	/*
	 * #918 review F9 — does this frame HAVE published mask content at all, per
	 * the authoritative CPU-side state? That question, and only that question,
	 * decides whether a clear is right. Whether the content's PIXELS have
	 * finished crossing the bridge is a transient that decides nothing.
	 */
	bool have_content = false;
	if (c->zones_frame) {
		have_content = (c->frame_wish != nullptr) || (c->wish_mask_staged_srv != nullptr);
		ID3D11ShaderResourceView *fw_srv =
		    (c->frame_wish != nullptr) ? d3d11_zone_mask_consume_srv(c, c->frame_wish, -1, &pub_kind, &pub_seq)
		                               : nullptr;
		if (fw_srv != nullptr) {
			srv = fw_srv;
			mask_w = c->frame_wish->w;
			mask_h = c->frame_wish->h;
		} else if (c->frame_wish == nullptr && c->wish_mask_staged_srv != nullptr) {
			// No explicit wish: the auto raster IS the published wish. (An
			// explicit wish whose plane has not landed must NOT silently fall
			// back to the auto raster — that would publish different geometry
			// for one frame, which is a flicker, not a degradation.)
			pub_kind = COMP_D3D11_XBRIDGE_MASK_OUT_RASTER;
			srv = c->wish_mask_staged_srv;
			mask_w = c->wish_mask_w;
			mask_h = c->wish_mask_h;
		}
	} else {
		struct comp_d3d11_zone_mask *mask = c->active_zone_mask;
		have_content = (mask != nullptr && mask->submitted);
		if (have_content) {
			srv = d3d11_zone_mask_consume_srv(c, mask, -1, &pub_kind, &pub_seq);
			mask_w = mask->w;
			mask_h = mask->h;
		}
	}
	/*
	 * #918 Phase 2a: a BRIDGED (Tier-3) mask arrives as an egress plane while the
	 * publish declares the mask's own dims and the vtable has no scale. Since the
	 * plane is now sized AT the mask (#918 review F5) this is normally a
	 * passthrough that copies nothing; the helper still handles the case where a
	 * publish asks for dims the plane does not have, seq-gated and clamped.
	 */
	if (srv != nullptr && pub_kind == COMP_D3D11_XBRIDGE_MASK_PLANE) {
		srv = d3d11_plane_dp_view(c, &c->dp_mask_view, srv, pub_seq, mask_w, mask_h, DXGI_FORMAT_R8_UNORM,
		                          "zone-mask publish");
	}

	if (srv == nullptr) {
		/*
		 * #918 review F9 — a NULL consume SRV is a NORMAL not-yet-landed state
		 * under the split: a Tier-3 mask's plane crosses a frame behind the
		 * authoring call, and any transport skip leaves it NULL for a frame.
		 * Clearing on that told the DP the app had withdrawn its mask and then
		 * republished it — a clear/republish cycle at frame rate, which is a
		 * visible 2D/3D flicker. Only the CPU-side state can say the mask is
		 * GONE; until it does, keep the DP's previous publish and try again.
		 */
		if (have_content) {
			return;
		}
		if (c->zone_published) {
			xrt_display_processor_d3d11_clear_local_zone_mask(c->display_processor);
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

	// seq is the content GENERATION (bumped on zone_mask_submit, the auto-
	// wish re-raster, and explicit-frame-wish changes), not a frame counter —
	// same-seq publishes differ only in the screen anchor, so a vendor's
	// content evaluation (e.g. the 1×1 any-nonzero check) runs once per
	// generation instead of once per frame.
	bool ok = xrt_display_processor_d3d11_publish_local_zone_mask(
	    c->display_processor, d3d11_out_context(c), srv, mask_w, mask_h, (int32_t)origin.x, (int32_t)origin.y,
	    (uint32_t)r.right, (uint32_t)r.bottom, c->zone_publish_seq);
	if (ok) {
		c->zone_published = true;
	}
}

// #439 Phase 3 — like comp_d3d11_outcomp's SRV-only scratch but
// RENDER_TARGET-capable, so the Local2D flatten can draw into it before the
// masked composite samples it. Keeps the SRV and RTV in lockstep (both
// (re)created on dims/format change). Kept separate from the weave snapshot's
// allocator so that one stays SRV-only (no wasted RT bind on a pure snapshot).
//
// #918: the device is explicit — this scratch is written by the Local2D
// flatten (app device today) and read by the composite, so which device owns
// it is a decision, not a detail. Callers pass c->device.
// #918 Phase 2a: @p share/@p gen are non-NULL when this scratch is a BRIDGE
// PLANE SOURCE. The texture is then created NT-shareable so the bridge's
// producer D3D12 device can open it directly (no extra app-device copy per
// frame), the handle is handed back for that open, and the generation is bumped
// on every REALLOCATION so the bridge knows to re-open. Under the split these
// are allocated at the PANEL extent exactly once, so the generation moves during
// warmup and never again.
static bool
d3d11_ensure_rt_srv_scratch(ID3D11Device *device,
                            ID3D11Texture2D **tex,
                            ID3D11ShaderResourceView **srv,
                            ID3D11RenderTargetView **rtv,
                            uint32_t w,
                            uint32_t h,
                            DXGI_FORMAT fmt,
                            const char *what,
                            HANDLE *share = nullptr,
                            uint64_t *gen = nullptr)
{
	bool need_alloc = *tex == nullptr || *rtv == nullptr;
	if (!need_alloc) {
		D3D11_TEXTURE2D_DESC cur;
		(*tex)->GetDesc(&cur);
		need_alloc = (cur.Width != w || cur.Height != h || cur.Format != fmt);
	}
	if (!need_alloc) {
		return true;
	}
	if (*rtv != nullptr) {
		(*rtv)->Release();
		*rtv = nullptr;
	}
	if (*srv != nullptr) {
		(*srv)->Release();
		*srv = nullptr;
	}
	if (*tex != nullptr) {
		(*tex)->Release();
		*tex = nullptr;
	}
	if (share != nullptr && *share != nullptr) {
		CloseHandle(*share);
		*share = nullptr;
	}
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	if (share != nullptr) {
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	}
	HRESULT hr = device->CreateTexture2D(&td, nullptr, tex);
	if (FAILED(hr) || *tex == nullptr) {
		U_LOG_W("%s: RT scratch alloc (%ux%u fmt=%u) failed: 0x%08x", what, w, h, fmt, hr);
		return false;
	}
	if (share != nullptr) {
		IDXGIResource1 *dr = nullptr;
		if (SUCCEEDED((*tex)->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&dr))) &&
		    dr != nullptr) {
			if (FAILED(dr->CreateSharedHandle(
			        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, share))) {
				*share = nullptr;
			}
			dr->Release();
		}
		if (*share == nullptr) {
			// Not fatal here: the caller's bind_plane sees a NULL handle and
			// degrades that ONE plane with its own WARN.
			U_LOG_W("%s: NT share handle unavailable — this bridge plane will not transport (#918)", what);
		}
		if (gen != nullptr) {
			(*gen)++;
		}
	}
	hr = device->CreateShaderResourceView(*tex, nullptr, srv);
	if (FAILED(hr) || *srv == nullptr) {
		U_LOG_W("%s: RT scratch SRV failed: 0x%08x", what, hr);
		(*tex)->Release();
		*tex = nullptr;
		return false;
	}
	hr = device->CreateRenderTargetView(*tex, nullptr, rtv);
	if (FAILED(hr) || *rtv == nullptr) {
		U_LOG_W("%s: RT scratch RTV failed: 0x%08x", what, hr);
		(*srv)->Release();
		*srv = nullptr;
		(*tex)->Release();
		*tex = nullptr;
		return false;
	}
	return true;
}

// #439 Phase 3 — (re)rasterize the runtime-owned IMPLICIT zone mask from this
// frame's Local2D layer rects (Q3). Inverse of zone_mask_set_rects: clear M=1
// (keep the weave / 3D) everywhere, then ClearView M=0 (show the 2D layers)
// inside each layer rect. The masked-composite shader lerps M·weave+(1−M)·twod,
// so M=0 inside the rects is what surfaces the flattened Local2D content there.
// Re-rasters only when the rect set or dims change (the common steady-state
// frame reuses the staged SRV). Returns the staged SRV (sampled by the
// composite) or nullptr on failure. Caller holds c->mutex.
// #918: device/context are EXPLICIT. Every input of this raster is CPU-side
// (rects, dims), so the mask can be built on whichever device consumes it —
// which is the property Phase 2a-2 relies on. Callers pass c->device/c->context.
static ID3D11ShaderResourceView *
d3d11_update_implicit_mask(struct comp_d3d11_compositor *c,
                           ID3D11Device *device,
                           ID3D11DeviceContext *context,
                           const struct xrt_rect *rects,
                           uint32_t rect_count,
                           uint32_t w,
                           uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return nullptr;
	}

	// Dirty-check: reuse the existing rasterization unless dims or the rect
	// set changed.
	bool dirty = c->implicit_mask_tex == nullptr || c->implicit_mask_staged_srv == nullptr ||
	             c->implicit_mask_w != w || c->implicit_mask_h != h || c->implicit_rect_count != rect_count;
	for (uint32_t i = 0; !dirty && i < rect_count; i++) {
		if (memcmp(&c->implicit_rects[i], &rects[i], sizeof(rects[i])) != 0) {
			dirty = true;
		}
	}
	if (!dirty) {
		return c->implicit_mask_staged_srv;
	}

	// (Re)allocate the R8 RTV + staged SRV textures on dims change (mirrors
	// zone_mask_create). Reused across re-rasters at the same size.
	if (c->implicit_mask_tex == nullptr || c->implicit_mask_w != w || c->implicit_mask_h != h) {
		if (c->implicit_mask_staged_srv != nullptr) {
			c->implicit_mask_staged_srv->Release();
			c->implicit_mask_staged_srv = nullptr;
		}
		if (c->implicit_mask_staged != nullptr) {
			c->implicit_mask_staged->Release();
			c->implicit_mask_staged = nullptr;
		}
		if (c->implicit_mask_rtv != nullptr) {
			c->implicit_mask_rtv->Release();
			c->implicit_mask_rtv = nullptr;
		}
		if (c->implicit_mask_tex != nullptr) {
			c->implicit_mask_tex->Release();
			c->implicit_mask_tex = nullptr;
		}
		c->implicit_mask_w = 0;
		c->implicit_mask_h = 0;

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &c->implicit_mask_tex);
		if (SUCCEEDED(hr) && c->implicit_mask_tex != nullptr) {
			hr = device->CreateRenderTargetView(c->implicit_mask_tex, nullptr, &c->implicit_mask_rtv);
		}
		if (SUCCEEDED(hr) && c->implicit_mask_rtv != nullptr) {
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			hr = device->CreateTexture2D(&td, nullptr, &c->implicit_mask_staged);
		}
		if (SUCCEEDED(hr) && c->implicit_mask_staged != nullptr) {
			hr = device->CreateShaderResourceView(c->implicit_mask_staged, nullptr,
			                                      &c->implicit_mask_staged_srv);
		}
		if (FAILED(hr) || c->implicit_mask_staged_srv == nullptr) {
			U_LOG_E("implicit zone mask: D3D resource creation failed: 0x%08x", hr);
			if (c->implicit_mask_staged != nullptr) {
				c->implicit_mask_staged->Release();
				c->implicit_mask_staged = nullptr;
			}
			if (c->implicit_mask_rtv != nullptr) {
				c->implicit_mask_rtv->Release();
				c->implicit_mask_rtv = nullptr;
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

	// Raster: M=1 (keep weave) everywhere, then M=0 (show 2D) inside each
	// clamped layer rect — the inverse of zone_mask_set_rects.
	const float all_3d[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	context->ClearRenderTargetView(c->implicit_mask_rtv, all_3d);

	ID3D11DeviceContext1 *ctx1 = nullptr;
	HRESULT hr = context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx1));
	if (FAILED(hr) || ctx1 == nullptr) {
		U_LOG_E("implicit zone mask: ID3D11DeviceContext1 unavailable (hr=0x%08x)", hr);
		return nullptr;
	}
	const float all_2d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	for (uint32_t i = 0; i < rect_count; i++) {
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
		D3D11_RECT dr = {left, top, right, bottom};
		ctx1->ClearView(c->implicit_mask_rtv, all_2d, &dr, 1);
	}
	ctx1->Release();

	// Stage the snapshot the composite samples (RT≠SRV; same decouple as the
	// explicit mask's submit).
	context->CopyResource(c->implicit_mask_staged, c->implicit_mask_tex);

	// Cache the rect set for the next frame's dirty-check.
	memcpy(c->implicit_rects, rects, sizeof(rects[0]) * rect_count);
	c->implicit_rect_count = rect_count;

	// One-off-ish lifecycle event (only fires on a rect/dims change, not
	// per-frame). WARN per the debug-logging convention.
	U_LOG_W("implicit zone mask: %ux%u, %u Local2D rect(s)", w, h, rect_count);
	return c->implicit_mask_staged_srv;
}

// XR_DXR_display_zones (ADR-027) — (re)rasterize the AUTO wish: union of the
// frame's zone rects, BINARY (#800/#801 — the wish is HARDWARE-only and
// hard-edged by default; the old implicit 16px ring feather leaked cosmetic
// fractional M into the published wish and vignetted the composite at window
// edges). M=1 inside every zone rect, 0 outside. The same staged SRV is the
// MODE_ZONES composite's weave gate and the published wish when no explicit
// wish is staged; cosmetic feather (XrDisplayZoneFeatherDXR, #803) rasters
// into its own texture and never enters this one.
// Same R8 RTV + staged-SRV pattern as the implicit mask (separate textures —
// the two never coexist in one frame but lifetimes differ). Dirty-checked on
// the rect set + dims; bumps c->zone_publish_seq on re-raster. Caller holds
// c->mutex. Returns the staged SRV or nullptr on failure.
// #918: device/context are EXPLICIT. Every input of this raster is CPU-side
// (rects, dims), so the mask can be built on whichever device consumes it —
// which is the property Phase 2a-2 relies on. Callers pass c->device/c->context.
static ID3D11ShaderResourceView *
d3d11_update_zone_wish_mask(struct comp_d3d11_compositor *c,
                            ID3D11Device *device,
                            ID3D11DeviceContext *context,
                            const struct xrt_rect *rects,
                            uint32_t rect_count,
                            uint32_t w,
                            uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return nullptr;
	}

	bool dirty = c->wish_mask_tex == nullptr || c->wish_mask_staged_srv == nullptr || c->wish_mask_w != w ||
	             c->wish_mask_h != h || c->wish_rect_count != rect_count;
	/*
	 * #876 diag: NAME the condition that tripped. Mid-run re-rasters correlate
	 * 1:1 with the dropout bursts, but every logged re-raster shows identical
	 * inputs ("2 zone rect(s), 1280x720") — so WHICH term goes dirty is the
	 * question, and guessing has been wrong five times on this bug.
	 */
	if (dirty && c->wish_mask_tex != nullptr) {
		U_LOG_W("#876 wish dirty: srv_null=%d w %u!=%u h %u!=%u count %u!=%u",
		        (int)(c->wish_mask_staged_srv == nullptr), c->wish_mask_w, w, c->wish_mask_h, h,
		        c->wish_rect_count, rect_count);
	}
	for (uint32_t i = 0; !dirty && i < rect_count; i++) {
		if (memcmp(&c->wish_rects[i], &rects[i], sizeof(rects[i])) != 0) {
			dirty = true;
			U_LOG_W("#876 wish dirty: rect[%u] %d,%d %ux%u -> %d,%d %ux%u", i,
			        c->wish_rects[i].offset.w, c->wish_rects[i].offset.h,
			        c->wish_rects[i].extent.w, c->wish_rects[i].extent.h,
			        rects[i].offset.w, rects[i].offset.h,
			        rects[i].extent.w, rects[i].extent.h);
		}
	}
	if (!dirty) {
		return c->wish_mask_staged_srv;
	}

	if (c->wish_mask_tex == nullptr || c->wish_mask_w != w || c->wish_mask_h != h) {
		if (c->wish_mask_staged_srv != nullptr) {
			c->wish_mask_staged_srv->Release();
			c->wish_mask_staged_srv = nullptr;
		}
		if (c->wish_mask_staged != nullptr) {
			c->wish_mask_staged->Release();
			c->wish_mask_staged = nullptr;
		}
		if (c->wish_mask_rtv != nullptr) {
			c->wish_mask_rtv->Release();
			c->wish_mask_rtv = nullptr;
		}
		if (c->wish_mask_tex != nullptr) {
			c->wish_mask_tex->Release();
			c->wish_mask_tex = nullptr;
		}
		c->wish_mask_w = 0;
		c->wish_mask_h = 0;

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &c->wish_mask_tex);
		if (SUCCEEDED(hr) && c->wish_mask_tex != nullptr) {
			hr = device->CreateRenderTargetView(c->wish_mask_tex, nullptr, &c->wish_mask_rtv);
		}
		if (SUCCEEDED(hr) && c->wish_mask_rtv != nullptr) {
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			hr = device->CreateTexture2D(&td, nullptr, &c->wish_mask_staged);
		}
		if (SUCCEEDED(hr) && c->wish_mask_staged != nullptr) {
			hr = device->CreateShaderResourceView(c->wish_mask_staged, nullptr, &c->wish_mask_staged_srv);
		}
		if (FAILED(hr) || c->wish_mask_staged_srv == nullptr) {
			U_LOG_E("zone wish mask: D3D resource creation failed: 0x%08x", hr);
			if (c->wish_mask_staged != nullptr) {
				c->wish_mask_staged->Release();
				c->wish_mask_staged = nullptr;
			}
			if (c->wish_mask_rtv != nullptr) {
				c->wish_mask_rtv->Release();
				c->wish_mask_rtv = nullptr;
			}
			if (c->wish_mask_tex != nullptr) {
				c->wish_mask_tex->Release();
				c->wish_mask_tex = nullptr;
			}
			return nullptr;
		}
		c->wish_mask_w = w;
		c->wish_mask_h = h;
	}

	const float all_off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	context->ClearRenderTargetView(c->wish_mask_rtv, all_off);

	ID3D11DeviceContext1 *ctx1 = nullptr;
	HRESULT hr = context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx1));
	if (FAILED(hr) || ctx1 == nullptr) {
		U_LOG_E("zone wish mask: ID3D11DeviceContext1 unavailable (hr=0x%08x)", hr);
		return nullptr;
	}
	const float all_on[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	for (uint32_t i = 0; i < rect_count; i++) {
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
		D3D11_RECT dr = {left, top, right, bottom};
		ctx1->ClearView(c->wish_mask_rtv, all_on, &dr, 1);
	}
	ctx1->Release();

	context->CopyResource(c->wish_mask_staged, c->wish_mask_tex);

	memcpy(c->wish_rects, rects, sizeof(rects[0]) * rect_count);
	c->wish_rect_count = rect_count;
	c->zone_publish_seq++; // new wish content generation

	U_LOG_W("zone wish mask (auto): %ux%u, %u zone rect(s), binary", w, h, rect_count);
	return c->wish_mask_staged_srv;
}

// XR_DXR_display_zones (#800/#803) — (re)rasterize the zones COMPOSITE mask
// with PER-ZONE opt-in feather (XrDisplayZoneFeatherDXR). Clear M=0, then
// each zone draws hard (one full-rect ClearView at 1.0, feather_px[i] <= 0 —
// the default) or with its own inward 0->1 ramp over feather_px[i] window
// pixels (the rings idiom: ascending value WITH ascending inset, 2px steps,
// ramp-width cap 64px then wider steps; later, deeper, higher-value clears
// overwrite the inner part of earlier ones so the edge keeps the low values
// and the core reaches 1; small zones clamp the inset so the center still
// reaches 1). Per zone so radii can differ. Only called when a frame's zones
// request feather — all-hard frames sample the binary wish raster instead,
// and the published wish stays binary regardless (cosmetics never enter the
// wish). NO zone_publish_seq bump: composite-only, never published.
// Dirty-checked on the rect set + radii + dims. Caller holds c->mutex.
// Returns the staged SRV or nullptr on failure (caller falls back to the
// binary mask — hard edges, never a lost frame).
// #918: device/context are EXPLICIT. Every input of this raster is CPU-side
// (rects, dims), so the mask can be built on whichever device consumes it —
// which is the property Phase 2a-2 relies on. Callers pass c->device/c->context.
static ID3D11ShaderResourceView *
d3d11_update_zone_feather_mask(struct comp_d3d11_compositor *c,
                               ID3D11Device *device,
                               ID3D11DeviceContext *context,
                               const struct xrt_rect *rects,
                               const float *feather_px,
                               uint32_t rect_count,
                               uint32_t w,
                               uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return nullptr;
	}

	bool dirty = c->feather_mask_tex == nullptr || c->feather_mask_staged_srv == nullptr ||
	             c->feather_mask_w != w || c->feather_mask_h != h || c->feather_rect_count != rect_count;
	for (uint32_t i = 0; !dirty && i < rect_count; i++) {
		if (memcmp(&c->feather_rects[i], &rects[i], sizeof(rects[i])) != 0 ||
		    c->feather_radii[i] != feather_px[i]) {
			dirty = true;
		}
	}
	if (!dirty) {
		return c->feather_mask_staged_srv;
	}

	if (c->feather_mask_tex == nullptr || c->feather_mask_w != w || c->feather_mask_h != h) {
		if (c->feather_mask_staged_srv != nullptr) {
			c->feather_mask_staged_srv->Release();
			c->feather_mask_staged_srv = nullptr;
		}
		if (c->feather_mask_staged != nullptr) {
			c->feather_mask_staged->Release();
			c->feather_mask_staged = nullptr;
		}
		if (c->feather_mask_rtv != nullptr) {
			c->feather_mask_rtv->Release();
			c->feather_mask_rtv = nullptr;
		}
		if (c->feather_mask_tex != nullptr) {
			c->feather_mask_tex->Release();
			c->feather_mask_tex = nullptr;
		}
		c->feather_mask_w = 0;
		c->feather_mask_h = 0;

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &c->feather_mask_tex);
		if (SUCCEEDED(hr) && c->feather_mask_tex != nullptr) {
			hr = device->CreateRenderTargetView(c->feather_mask_tex, nullptr, &c->feather_mask_rtv);
		}
		if (SUCCEEDED(hr) && c->feather_mask_rtv != nullptr) {
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			hr = device->CreateTexture2D(&td, nullptr, &c->feather_mask_staged);
		}
		if (SUCCEEDED(hr) && c->feather_mask_staged != nullptr) {
			hr = device->CreateShaderResourceView(c->feather_mask_staged, nullptr,
			                                      &c->feather_mask_staged_srv);
		}
		if (FAILED(hr) || c->feather_mask_staged_srv == nullptr) {
			U_LOG_E("zone feather mask: D3D resource creation failed: 0x%08x", hr);
			if (c->feather_mask_staged != nullptr) {
				c->feather_mask_staged->Release();
				c->feather_mask_staged = nullptr;
			}
			if (c->feather_mask_rtv != nullptr) {
				c->feather_mask_rtv->Release();
				c->feather_mask_rtv = nullptr;
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

	const float all_off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	context->ClearRenderTargetView(c->feather_mask_rtv, all_off);

	ID3D11DeviceContext1 *ctx1 = nullptr;
	HRESULT hr = context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx1));
	if (FAILED(hr) || ctx1 == nullptr) {
		U_LOG_E("zone feather mask: ID3D11DeviceContext1 unavailable (hr=0x%08x)", hr);
		return nullptr;
	}
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
			D3D11_RECT dr = {left, top, right, bottom};
			ctx1->ClearView(c->feather_mask_rtv, val, &dr, 1);
		}
	}
	ctx1->Release();

	context->CopyResource(c->feather_mask_staged, c->feather_mask_tex);

	memcpy(c->feather_rects, rects, sizeof(rects[0]) * rect_count);
	memcpy(c->feather_radii, feather_px, sizeof(feather_px[0]) * rect_count);
	c->feather_rect_count = rect_count;

	U_LOG_W("zone feather mask: %ux%u, %u zone rect(s) (composite-only, wish stays binary)", w, h, rect_count);
	return c->feather_mask_staged_srv;
}

// Resolve the zones frame's wish source (called from layer_commit before the
// DP sync + composite; caller holds c->mutex). The BINARY auto raster is
// ALWAYS maintained — it is the MODE_ZONES composite's weave gate (#801: an
// explicit wish is PUBLISH-ONLY, never a compositor blend gate) and doubles
// as the published wish when no explicit wish is staged. Explicit frame
// wish: stage the authoring texture (referenced-at-frame-end = consume
// current state — no xrSubmitLocal3DZoneDXR required) and dedup the publish
// generation on the mask's author_seq.
static void
d3d11_update_zone_wish_state(struct comp_d3d11_compositor *c)
{
	// Window region for the raster (same clamp as the composite).
	uint32_t w = 0;
	uint32_t h = 0;
	HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	RECT r;
	if (wnd != nullptr && GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
		w = (uint32_t)r.right;
		h = (uint32_t)r.bottom;
	} else if (c->shared_texture != nullptr) {
		D3D11_TEXTURE2D_DESC td;
		c->shared_texture->GetDesc(&td);
		w = td.Width;
		h = td.Height;
	}

	struct xrt_rect rects[XRT_MAX_LAYERS];
	uint32_t rect_count = 0;
	for (uint32_t i = 0; i < c->layer_accum.layer_count && rect_count < XRT_MAX_LAYERS; i++) {
		if (c->layer_accum.layers[i].data.type != XRT_LAYER_ZONE_3D) {
			continue;
		}
		rects[rect_count++] = c->layer_accum.layers[i].data.zone_3d.rect;
	}
	// #918 Phase 2a: on the OUTPUT device. Every input of this raster is CPU-side
	// (zone rects, window dims), so it is built where it is consumed — the DP
	// publish and the MODE_ZONES composite both live on the scanout adapter now —
	// and the wish never touches the bridge.
	d3d11_update_zone_wish_mask(c, d3d11_out_device(c), d3d11_out_context(c), rects, rect_count, w, h);

	if (c->frame_wish != nullptr) {
		struct comp_d3d11_zone_mask *fw = c->frame_wish;
		d3d11_zone_mask_stage(c, fw);
		/*
		 * #918 Phase 2a: an EXPLICIT frame wish the app drew itself (Tier 3) is
		 * the one wish that cannot be reproduced on the output device, so it
		 * rides the authored-mask plane — staged by d3d11_stage_mask_plane, the
		 * single per-frame publisher (#918 review D6), not from here. The publish
		 * below reads the plane from the LAST published weave slot, so a freshly
		 * authored wish reaches the DP one frame later; the publish is a
		 * per-frame sideband that republishes every frame, so that lag is
		 * invisible (and the vendor dedups on zone_publish_seq anyway).
		 */
		if (c->frame_wish_last != fw || c->frame_wish_last_seq != fw->author_seq) {
			U_LOG_W("#876 wish seq: EXPLICIT bump (fw %p->%p author_seq %llu->%llu)",
			        (void *)c->frame_wish_last, (void *)fw,
			        (unsigned long long)c->frame_wish_last_seq,
			        (unsigned long long)fw->author_seq);
			c->zone_publish_seq++;
			c->frame_wish_last = fw;
			c->frame_wish_last_seq = fw->author_seq;
		}
		return;
	}

	// No explicit wish this frame; d3d11_stage_mask_plane un-stages the plane
	// for exactly that case, so there is nothing to do here (#918 review D6 —
	// this used to un-stage from here, which is a second producer of the same
	// plane guarded only by prose about which frames reach this function).

	// Source flip explicit -> auto: even an unchanged auto raster is new
	// content at the DP.
	if (c->frame_wish_last != nullptr) {
		U_LOG_W("#876 wish seq: EXPLICIT->AUTO flip bump (fw_last %p)", (void *)c->frame_wish_last);
		c->frame_wish_last = nullptr;
		c->zone_publish_seq++;
	}
}

// #439 Phase 3 — flatten one Local2D layer into @p rtv (premultiplied or
// straight-alpha over), clipped to the window region with clip/norm_rect/flip_y
// carried into the source UVs. Shared by the post-weave overlay flatten and the
// pre-weave 2D-under backdrop flatten (#491 part 3). Caller holds c->mutex.
static void
d3d11_flatten_one_local2d_layer(struct comp_d3d11_compositor *c,
                                ID3D11RenderTargetView *rtv,
                                struct comp_layer *layer,
                                uint32_t region_w,
                                uint32_t region_h)
{
	struct xrt_swapchain *sc = layer->sc_array[0];
	if (sc == nullptr) {
		return;
	}
	uint32_t img_idx = layer->data.local_2d.sub.image_index;
	// sRGB-passthrough: get_srv returns the swapchain's UNORM sibling SRV
	// (no auto-decode), the same SRV the projection draws sample.
	ID3D11ShaderResourceView *src_srv =
	    static_cast<ID3D11ShaderResourceView *>(comp_d3d11_swapchain_get_srv(sc, img_idx));
	if (src_srv == nullptr) {
		return; // swapchain not SAMPLED — nothing to flatten
	}

	// Dest rect (client-window px), clipped to the window region.
	const struct xrt_rect *dr = &layer->data.local_2d.rect;
	int32_t dx = dr->offset.w;
	int32_t dy = dr->offset.h;
	int32_t dw = dr->extent.w;
	int32_t dh = dr->extent.h;
	if (dw <= 0 || dh <= 0) {
		return;
	}
	int32_t x0 = dx < 0 ? 0 : dx;
	int32_t y0 = dy < 0 ? 0 : dy;
	int32_t x1 = (dx + dw) > (int32_t)region_w ? (int32_t)region_w : (dx + dw);
	int32_t y1 = (dy + dh) > (int32_t)region_h ? (int32_t)region_h : (dy + dh);
	if (x1 <= x0 || y1 <= y0) {
		return; // clipped fully out
	}

	// Clip fractions within the original dest rect (carry into the UVs so
	// the correct source region is sampled when the rect is clipped).
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

	// Source rect (normalized) carrying clip + flip_y. flip_y: start at
	// the bottom of the sub-rect and sample upward (negative height).
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

	comp_d3d11_renderer_flatten_local_2d(c->renderer, rtv, src_srv, x0, y0, (uint32_t)(x1 - x0),
	                                     (uint32_t)(y1 - y0), src_x, src_y, src_w, src_h, unpremult);
}

/*!
 * #918 Phase 2a — clamp a composite region to the PANEL under the split.
 *
 * The plane scratches and their egress textures are panel-sized once, and the
 * header's claim that "a window can never exceed the panel" is an assumption
 * about the window manager, not something the code enforces. If it were ever
 * false the flatten would be clipped while the uv scale stayed 1.0 and the
 * clipped content would be stretched across the region — silently. Clamp, and
 * say so once.
 */
static void
d3d11_clamp_region_to_panel(struct comp_d3d11_compositor *c, uint32_t *w, uint32_t *h)
{
	if (!c->split_active || c->split_panel_w == 0 || c->split_panel_h == 0) {
		return;
	}
	if (*w <= c->split_panel_w && *h <= c->split_panel_h) {
		return;
	}
	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W(
		    "D3D11 output-device split: composite region %ux%u exceeds the panel %ux%u — clamping; "
		    "the bridge planes are panel-sized by construction (#918 Phase 2a)",
		    *w, *h, c->split_panel_w, c->split_panel_h);
	}
	if (*w > c->split_panel_w) {
		*w = c->split_panel_w;
	}
	if (*h > c->split_panel_h) {
		*h = c->split_panel_h;
	}
}

/*!
 * #918 Phase 2a — clear the flatten scratch's REGION, not the whole texture.
 *
 * Under the split the scratch is panel-sized (so the bridge plane never resizes)
 * while the content is window-sized, and a full-surface clear would write the
 * whole panel every frame — 33 MB at 4K, on the app device, for nothing. Outside
 * the region nothing is ever sampled, so clearing the region is not an
 * approximation: it is the same set of authoritative pixels the region-sized
 * scratch used to hold.
 *
 * **#918 review F4 — except on the frame the region CHANGES.** "Outside the
 * region nothing is sampled" is true of the scratch and false of its egress
 * plane. Shrink the window and the pixels the content vacated are still in the
 * scratch, un-cleared, and the dirty-box union that follows copies them across;
 * grow it back and the plane keeps them for ever, because the union only ever
 * covers where content IS, never where it WAS. That is a ghost overlay at its
 * old position, flickering with the ring rotation. So a region change clears the
 * WHOLE scratch and makes every egress slot owe a full refresh. It is a
 * per-change cost, not a per-frame one, and it is the only thing that makes the
 * plane's pixels a function of the scratch's.
 *
 * @param last_w,last_h The region this scratch was last cleared at, owned by the
 *        caller (one pair per scratch).
 * @param plane The bridge plane this scratch feeds.
 */
static void
d3d11_clear_scratch_region(struct comp_d3d11_compositor *c,
                           ID3D11RenderTargetView *rtv,
                           uint32_t region_w,
                           uint32_t region_h,
                           uint32_t *last_w,
                           uint32_t *last_h,
                           uint32_t plane)
{
	// Where a pixel is M=0 (2D) but no layer covers it, twod stays (0,0,0,0) →
	// final.a → 0 → the desktop shows through (Q2, §4.2 output-alpha rule).
	const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	const bool region_changed = (*last_w != region_w) || (*last_h != region_h);
	*last_w = region_w;
	*last_h = region_h;
	if (region_changed) {
		c->context->ClearRenderTargetView(rtv, transparent);
		if (c->split_active && c->xbridge != nullptr) {
			comp_d3d11_xbridge_invalidate_plane(c->xbridge, plane);
		}
		return;
	}

	ID3D11DeviceContext1 *ctx1 = nullptr;
	if (SUCCEEDED(c->context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx1))) &&
	    ctx1 != nullptr) {
		D3D11_RECT dr = {0, 0, (LONG)region_w, (LONG)region_h};
		ctx1->ClearView(rtv, transparent, &dr, 1);
		ctx1->Release();
		return;
	}
	c->context->ClearRenderTargetView(rtv, transparent);
}

/*!
 * #918 Phase 2a — CHANGE-SKIP key and DIRTY BOX for a Local2D flatten.
 *
 * The hash covers everything that can change the flattened pixels without
 * changing the rect: the swapchain a layer draws from, the image index inside
 * it, the source sub-rect, the flip and the blend flags. The image index is what
 * makes this safe rather than optimistic — `d3d11_swapchain_acquire_image` hands
 * out indices round-robin, so an app that redraws hashes differently every frame
 * and an app that stops acquiring keeps the index (and genuinely has not
 * changed). An app that redraws into ONE image without re-acquiring would defeat
 * it, which OpenXR's acquire/wait/release cycle does not permit.
 *
 * @param over true for the OVER layers (the composite's `twod`), false for the
 *        2D-under backdrop.
 */
static void
d3d11_local2d_digest(struct comp_d3d11_compositor *c,
                     int32_t proj_idx,
                     bool over,
                     uint32_t region_w,
                     uint32_t region_h,
                     struct xrt_rect *out_box,
                     uint64_t *out_hash)
{
	uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
	auto mix = [&h](const void *p, size_t n) {
		const uint8_t *b = static_cast<const uint8_t *>(p);
		for (size_t i = 0; i < n; i++) {
			h ^= b[i];
			h *= 1099511628211ull;
		}
	};
	mix(&region_w, sizeof(region_w));
	mix(&region_h, sizeof(region_h));

	int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		const bool is_under = (proj_idx >= 0 && (int32_t)i < proj_idx);
		if (is_under == over) {
			continue;
		}
		mix(&i, sizeof(i));
		void *sc = layer->sc_array[0];
		mix(&sc, sizeof(sc));
		mix(&layer->data.local_2d.sub.image_index, sizeof(layer->data.local_2d.sub.image_index));
		mix(&layer->data.local_2d.sub.rect, sizeof(layer->data.local_2d.sub.rect));
		mix(&layer->data.local_2d.sub.norm_rect, sizeof(layer->data.local_2d.sub.norm_rect));
		mix(&layer->data.local_2d.rect, sizeof(layer->data.local_2d.rect));
		mix(&layer->data.flip_y, sizeof(layer->data.flip_y));
		mix(&layer->data.flags, sizeof(layer->data.flags));

		const struct xrt_rect *dr = &layer->data.local_2d.rect;
		if (dr->extent.w <= 0 || dr->extent.h <= 0) {
			continue;
		}
		if (dr->offset.w < x0) {
			x0 = dr->offset.w;
		}
		if (dr->offset.h < y0) {
			y0 = dr->offset.h;
		}
		if (dr->offset.w + dr->extent.w > x1) {
			x1 = dr->offset.w + dr->extent.w;
		}
		if (dr->offset.h + dr->extent.h > y1) {
			y1 = dr->offset.h + dr->extent.h;
		}
	}

	if (x1 <= x0 || y1 <= y0) {
		// No layers on this side: the plane is a cleared region, and the clear
		// itself is what has to reach the other adapter.
		x0 = 0;
		y0 = 0;
		x1 = (int32_t)region_w;
		y1 = (int32_t)region_h;
	}
	if (x0 < 0) {
		x0 = 0;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (x1 > (int32_t)region_w) {
		x1 = (int32_t)region_w;
	}
	if (y1 > (int32_t)region_h) {
		y1 = (int32_t)region_h;
	}
	out_box->offset.w = x0;
	out_box->offset.h = y0;
	out_box->extent.w = (x1 > x0) ? (x1 - x0) : 0;
	out_box->extent.h = (y1 > y0) ? (y1 - y0) : 0;
	// 0 is reserved for "the frame does not use this plane".
	*out_hash = (h != 0) ? h : 1ull;
}

// #439 Phase 3 — flatten this frame's OVER Local2D layers into local2d_scratch
// (the `twod` source the masked composite reads). Under-layers (#491 part 3,
// before the projection in list order) are the DP backdrop and are skipped here.
// Caller holds c->mutex and has already (re)allocated local2d_scratch (+RTV).
static bool
d3d11_flatten_local_2d_layers(struct comp_d3d11_compositor *c, uint32_t region_w, uint32_t region_h, int32_t proj_idx)
{
	d3d11_clear_scratch_region(c, c->local2d_scratch_rtv, region_w, region_h, &c->local2d_scratch_region_w,
	                           &c->local2d_scratch_region_h, COMP_D3D11_XBRIDGE_PLANE_LOCAL2D);

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		// #491 part 3 — under-layers are the DP backdrop (handled pre-weave).
		if (proj_idx >= 0 && (int32_t)i < proj_idx) {
			continue;
		}
		d3d11_flatten_one_local2d_layer(c, c->local2d_scratch_rtv, layer, region_w, region_h);
	}

	// Unbind the scratch RTV so the masked composite can sample it as an SRV
	// without a read/write-hazard warning.
	ID3D11RenderTargetView *null_rtv = nullptr;
	c->context->OMSetRenderTargets(1, &null_rtv, nullptr);
	return true;
}

// #491 part 3 — flatten this frame's 2D-UNDER Local2D layers (before the
// projection in list order) into backdrop_scratch PRE-weave and return its SRV
// (+ region dims) so the caller hands it to the DP via set_background_2d (the DP
// composites `backdrop over captured-desktop` under the 3D). Returns nullptr
// (out dims 0) when there are no under-layers. Caller holds c->mutex; the
// immediate context orders these draws before the subsequent process_atlas.
static ID3D11ShaderResourceView *
d3d11_flatten_backdrop_2d(struct comp_d3d11_compositor *c, uint32_t dst_w, uint32_t dst_h, uint32_t *out_w,
                          uint32_t *out_h)
{
	*out_w = 0;
	*out_h = 0;
	if (!c->local_2d_last_frame || c->renderer == nullptr) {
		return nullptr;
	}

	// Under = Local2D layers BEFORE the projection. No projection ⟹ no backdrop.
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

	// Window region inside the worst-case dst (#464).
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
	d3d11_clamp_region_to_panel(c, &region_w, &region_h);

	/*
	 * Premultiplied RGBA, UNORM (sRGB-passthrough) like local2d_scratch.
	 *
	 * #918 Phase 2a: under the split this is a BRIDGE PLANE SOURCE — panel-sized
	 * once (so it never enters the R2 churn path) and NT-shareable (so the
	 * producer opens it directly, with no extra app-device copy per frame).
	 */
	const bool split = c->split_active;
	const uint32_t alloc_w = split ? c->split_panel_w : region_w;
	const uint32_t alloc_h = split ? c->split_panel_h : region_h;
	if (!d3d11_ensure_rt_srv_scratch(c->device, &c->backdrop_scratch, &c->backdrop_scratch_srv,
	                                 &c->backdrop_scratch_rtv, alloc_w, alloc_h, DXGI_FORMAT_R8G8B8A8_UNORM,
	                                 "backdrop scratch", split ? &c->backdrop_scratch_share : nullptr,
	                                 split ? &c->backdrop_scratch_gen : nullptr)) {
		return nullptr;
	}

	d3d11_clear_scratch_region(c, c->backdrop_scratch_rtv, region_w, region_h, &c->backdrop_scratch_region_w,
	                           &c->backdrop_scratch_region_h, COMP_D3D11_XBRIDGE_PLANE_BACKDROP);

	for (int32_t i = 0; i < proj_idx; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		d3d11_flatten_one_local2d_layer(c, c->backdrop_scratch_rtv, layer, region_w, region_h);
	}

	ID3D11RenderTargetView *null_rtv = nullptr;
	c->context->OMSetRenderTargets(1, &null_rtv, nullptr);

	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("D3D11 #491 part3: flattened 2D-under backdrop %ux%u (handed to DP set_background_2d)",
		        region_w, region_h);
	}

	/*
	 * #918 Phase 2a — stage the backdrop as a bridge plane. The SRV returned here
	 * is the APP-device one; under the split the caller never hands THAT to the
	 * DP (it lives on the wrong adapter) — it hands the egress plane SRV of the
	 * slot it wove. Returning it anyway keeps the non-split path byte-identical
	 * and gives the caller its dims.
	 */
	if (split && c->xbridge != nullptr) {
		struct xrt_rect box = {};
		uint64_t hash = 0;
		d3d11_local2d_digest(c, proj_idx, /*over=*/false, region_w, region_h, &box, &hash);
		// Panel-sized, always: never resized, so structurally outside the R2
		// churn path (#918 Phase 2a).
		if (comp_d3d11_xbridge_bind_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_BACKDROP,
		                                  c->backdrop_scratch_share, c->backdrop_scratch_gen,
		                                  (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM, c->split_panel_w,
		                                  c->split_panel_h)) {
			comp_d3d11_xbridge_stage_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_BACKDROP, hash,
			                               box.offset.w, box.offset.h, (uint32_t)box.extent.w,
			                               (uint32_t)box.extent.h);
		}
	}

	*out_w = region_w;
	*out_h = region_h;
	return c->backdrop_scratch_srv;
}

// #439 Phase 1 — composite the authored zone mask. Runs when an active
// submitted mask, a zones frame, or Local2D layers are present (the mask-lerp
// writes every window pixel). Returns false → no-op for this frame.
//
// #464 window clamping: all inputs are window-sized; the pass writes only the
// window region at the top-left anchor of the (worst-case-allocated) dst.
//
// #439 Phase 2: eff_canvas is the caller's per-frame effective canvas
// (d3d11_effective_canvas under c->mutex) — the window rect while the mask
// is active, so the composite region and the weave region share one
// authority.
static bool
d3d11_composite_zone_mask(struct comp_d3d11_compositor *c,
                          bool is_repaint,
                          bool prepare_only,
                          ID3D11Texture2D *dst,
                          uint32_t dst_w,
                          uint32_t dst_h,
                          const struct u_canvas_rect *eff_canvas,
                          int32_t slot)
{
	// #439 Phase 3: run when EITHER an explicit submitted mask exists OR this
	// frame carries Local2D layers (the layers supply the 2D pixels + an
	// implicit mask).
	// XR_DXR_display_zones: a zones frame ALWAYS runs the composite (the
	// MODE_ZONES pass gates the weave by the binary zone raster — pixels
	// outside every zone go to the 2D flatten / transparent even with zero
	// Local2D layers); the sticky mask + implicit-mask rules are inert.
	struct comp_d3d11_zone_mask *mask = c->active_zone_mask;
	const bool zones_frame = c->zones_frame;
	const bool have_explicit = !zones_frame && (mask != nullptr && mask->submitted);
	const bool have_local_2d = c->local_2d_last_frame;

	/*
	 * #918 Phase 2a — the DEPOSIT / CONSUME seam is also the DEVICE seam.
	 *
	 * The deposit half (prepare_only) reads app-owned resources — the Local2D
	 * swapchain images — and stays on the app device, unchanged. The consume half
	 * writes `dst`, which the split moved to the scanout adapter, so it runs on
	 * the OUTPUT device against the plane pixels that crossed with this slot's
	 * atlas, and it reads its PARAMETERS from the slot's recipe stamp rather than
	 * from live CPU state. That is the Phase-1 offset-cube defect class closed one
	 * level down: the atlas generation stops a slot being woven under the wrong
	 * mode, this stops its pixels being composited under the wrong recipe.
	 */
	const bool split_consume = c->split_active && !prepare_only;
	struct comp_d3d11_xbridge_recipe rec = {};
	if (split_consume) {
		if (slot < 0 || !comp_d3d11_xbridge_slot_recipe(c->xbridge, slot, &rec)) {
			c->repaint.composite_bail = 2;
			return false;
		}
		if (!rec.composite) {
			// A projection-only frame filled this slot — nothing to composite,
			// and that is the correct answer, not a bail.
			c->repaint.composite_bail = 0;
			return false;
		}
	}
	if (is_repaint && !split_consume && c->repaint.mask_srv == nullptr) {
		c->repaint.composite_bail = 1;
		return false;
	}
	if ((!is_repaint && !split_consume && !zones_frame && !have_explicit && !have_local_2d) || dst == nullptr ||
	    c->renderer == nullptr || c->outcomp == nullptr) {
		c->repaint.composite_bail = 2;
		return false;
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
	d3d11_clamp_region_to_panel(c, &region_w, &region_h);
	if (split_consume) {
		/*
		 * The recipe's region is the one the plane pixels were flattened at. It
		 * lags the window by at most the one frame the bridge always costs — the
		 * same lag the R2 hysteresis already accepts for the atlas — and using it
		 * is what keeps every input sampled at the scale it was drawn at.
		 *
		 * A region LAG is deliberately not a refusal. The uv scales are derived
		 * per input from that input's own extent, so an out-device mask rasterized
		 * at the live region and a plane flattened at the slot's region are BOTH
		 * addressed by window pixel and both come out geometrically right; only
		 * the 2D content's scale trails by a frame, exactly as the 3D content's
		 * does. Refusing here instead would drop the 2D band on every frame of a
		 * resize drag, which is strictly worse and buys no correctness.
		 */
		region_w = rec.region_w;
		region_h = rec.region_h;
		if (region_w > dst_w) {
			region_w = dst_w;
		}
		if (region_h > dst_h) {
			region_h = dst_h;
		}
		if (region_w == 0 || region_h == 0) {
			c->repaint.composite_bail = 2;
			return false;
		}
	}

	D3D11_TEXTURE2D_DESC dd;
	dst->GetDesc(&dd);

	// #491 part 3 — split Local2D by list order vs the projection: layers BEFORE
	// the projection are the 2D-under backdrop (handled pre-weave by the DP) and
	// are excluded from the overlay mask + flatten here. is_over == !under.
	int32_t proj_idx = -1;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
		if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
			proj_idx = (int32_t)i;
			break;
		}
	}

	// Resolve the frame's mask source. Zones frame (#801): ALWAYS the BINARY
	// zone raster (staged by d3d11_update_zone_wish_state earlier this
	// commit) — per ADR-027 the wish is HARDWARE-only and composition
	// follows zone geometry + alpha, so an explicit frame wish never gates
	// blending (it publishes via d3d11_sync_zone_mask_to_dp, verbatim).
	// #803: when any zone requests feather, the composite samples a
	// separately-rastered per-zone feather mask instead; the published wish
	// stays binary regardless (cosmetics never enter the wish). Legacy: an
	// explicit submitted mask wins; else rasterize the implicit Tier-2-style
	// mask from the OVER Local2D layer rects (Q3 — M=0 inside the rect
	// union, M=1 elsewhere).
	ID3D11ShaderResourceView *mask_srv = nullptr;
	uint32_t mask_kind = COMP_D3D11_XBRIDGE_MASK_OUT_RASTER;
	if (split_consume) {
		/*
		 * #918 Phase 2a — the consume half never re-derives the mask; it takes
		 * the KIND from the slot. A bridged (Tier-3, app-drawn) mask reads the
		 * plane that landed with this slot's atlas; every other kind is an
		 * output-device raster the compositor owns. That raster is region-sized
		 * and may have been re-rastered at a NEWER region than this slot's, which
		 * is harmless: the composite addresses every input by window pixel through
		 * a uv scale derived from that input's own extent, so the sub-rect sampled
		 * is the right one either way.
		 */
		if (rec.mask_kind == COMP_D3D11_XBRIDGE_MASK_PLANE) {
			if ((rec.plane_valid & (1u << COMP_D3D11_XBRIDGE_PLANE_MASK)) == 0) {
				c->repaint.composite_bail = 3;
				return false;
			}
			mask_srv = static_cast<ID3D11ShaderResourceView *>(
			    comp_d3d11_xbridge_get_plane_srv(c->xbridge, slot, COMP_D3D11_XBRIDGE_PLANE_MASK,
			                                     rec.plane_seq[COMP_D3D11_XBRIDGE_PLANE_MASK]));
		} else {
			mask_srv = c->repaint.mask_srv;
		}
	} else if (is_repaint) {
		// #868: a repaint replays RENDERING, never STATE TRANSITIONS.
		// d3d11_update_implicit_mask() rasters and republishes; the zones
		// branch reads a wish staged once per app frame by
		// d3d11_update_zone_wish_state(). Driving either at panel rate ticks a
		// once-per-app-frame machine out of band with the post-present sideband
		// publish that only layer_commit performs.
		mask_srv = c->repaint.mask_srv;
	} else if (zones_frame) {
		mask_srv = c->wish_mask_staged_srv;

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
			ID3D11ShaderResourceView *fsrv = d3d11_update_zone_feather_mask(
			    c, d3d11_out_device(c), d3d11_out_context(c), zrects, zfeather, zcount, region_w, region_h);
			if (fsrv != nullptr) {
				mask_srv = fsrv;
			} // raster failure: binary fallback — hard edges, never a lost frame
		}
	} else if (have_explicit) {
		// #918 Phase 2a: the shadow (Tier 1/2) or the bridge plane (Tier 3).
		// The plane was BOUND AND STAGED before this function ran, by
		// d3d11_stage_mask_plane — the transport does not depend on whether the
		// pixels have landed yet (#918 review F1/D6).
		mask_srv = d3d11_zone_mask_consume_srv(c, mask, slot, &mask_kind, nullptr);
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
		// #918 Phase 2a: rasterized on the OUTPUT device — pure CPU rects in,
		// so it is built where the composite consumes it and never transported.
		mask_srv = d3d11_update_implicit_mask(c, d3d11_out_device(c), d3d11_out_context(c), rects, rect_count,
		                                      region_w, region_h);
	}
	/*
	 * #918 review F1 — the DEPOSIT half under the split does not need the mask
	 * resolved. The consume half reads its kind and its pixels FROM THE SLOT, and
	 * a Tier-3 mask's plane has legitimately not landed on the frame that authors
	 * it. Bailing here is what made a sticky Tier-3 mask permanently dead: no
	 * deposit → no recipe stamp → no transport → still no SRV next frame, for
	 * ever. The first authoring frame now transports, and consumption starts
	 * whenever the slot lands.
	 */
	const bool deposit_bridged_mask = prepare_only && c->split_active && mask_kind == COMP_D3D11_XBRIDGE_MASK_PLANE;
	if (mask_srv == nullptr && !deposit_bridged_mask) {
		c->repaint.composite_bail = 3;
		return false;
	}
	if (!is_repaint && !split_consume && mask_srv != nullptr) {
		c->repaint.mask_srv = mask_srv;
	}


	// Resolve the `twod` source + a window-sized weave snapshot scratch.
	ID3D11ShaderResourceView *twod_srv = nullptr;
	if (zones_frame || have_local_2d || have_explicit || split_consume) {
		// #439 Phase 3: flatten the Local2D layers into a runtime-owned RT
		// scratch. The flatten write, the weave snapshot, and the lerp all
		// operate on plain UNORM bytes (sRGB-passthrough), so both scratches
		// use the UNORM sibling of the dst format — no implicit gamma.
		DXGI_FORMAT unorm_fmt = d3d_dxgi_format_srgb_to_unorm(dd.Format);
		if (!split_consume) {
			/*
			 * #918 Phase 2a: under the split this scratch is a BRIDGE PLANE
			 * SOURCE — panel-sized once (outside the R2 churn path and #1091)
			 * and NT-shareable so the producer opens it directly.
			 */
			const uint32_t alloc_w = c->split_active ? c->split_panel_w : region_w;
			const uint32_t alloc_h = c->split_active ? c->split_panel_h : region_h;
			if (!d3d11_ensure_rt_srv_scratch(c->device, &c->local2d_scratch, &c->local2d_scratch_srv,
			                                 &c->local2d_scratch_rtv, alloc_w, alloc_h, unorm_fmt,
			                                 "local2d scratch",
			                                 c->split_active ? &c->local2d_scratch_share : nullptr,
			                                 c->split_active ? &c->local2d_scratch_gen : nullptr)) {
				c->repaint.composite_bail = 4;
				return false;
			}
		}
		/*
		 * #918 F10: ONE size authority per frame. The weave snapshot belongs to
		 * the CONSUME half alone — the deposit half never reads it — and under
		 * the split the two halves legitimately run at two different regions
		 * during a resize (live window vs the slot's stamped region). Sizing it
		 * from both is what made the two halves ping-pong the allocation; the
		 * consume half's region is the one that matters, so only it asks.
		 */
		if (!prepare_only &&
		    !comp_d3d11_outcomp_ensure_weave_scratch(c->outcomp, region_w, region_h, (uint32_t)unorm_fmt)) {
			c->repaint.composite_bail = 5;
			return false;
		}
		// Zones frame: flatten ALL Local2D layers (no under/over split —
		// 2D-under is reserved in v1); with zero Local2D layers this is a
		// clear-only flatten and MODE_ZONES writes M·weave (zone interior)
		// over transparent — pixels outside every zone present alpha 0.
		/*
		 * #868: a repaint must NOT re-run this. d3d11_flatten_local_2d_layers
		 * samples the APP'S OWN Local2D swapchain images, and by the time a
		 * repaint runs the app has reacquired them and may be drawing its next
		 * frame into them — the same hazard that keeps repaints off the
		 * zero-copy atlas. local2d_scratch is compositor-owned and still holds
		 * the last app frame's flatten, which is what a repaint wants.
		 *
		 * Getting this wrong on the D3D12 leg produced a 2D region that
		 * differed between an app weave and its repaint, alternating on screen
		 * as a flicker confined to semi-transparent content.
		 */
		if (!is_repaint && !split_consume) {
			if (!d3d11_flatten_local_2d_layers(c, region_w, region_h,
			                                   zones_frame ? -1 : proj_idx)) {
				return false;
			}
		}
		twod_srv = c->local2d_scratch_srv;
		/*
		 * #875: DEPOSIT half ends here. Everything above reads app-owned
		 * resources (the Local2D layer swapchain images) or ticks per-frame
		 * state; everything below only touches compositor-owned scratch and the
		 * render target. dst is still passed in during prepare because the
		 * scratch formats derive from the target's format — prepare reads that
		 * descriptor but never writes the target. (On the D3D12 leg, stubbing
		 * the descriptor out instead requested DXGI_FORMAT_UNKNOWN and silently
		 * swallowed the 2D.)
		 *
		 * #918 Phase 2a: this is ALSO the device seam. Everything above ran on
		 * the app device; everything below runs on the output device, reading the
		 * plane pixels the bridge landed beside this slot's atlas.
		 */
		if (prepare_only) {
			if (c->split_active && c->xbridge != nullptr) {
				struct xrt_rect box = {};
				uint64_t hash = 0;
				d3d11_local2d_digest(c, zones_frame ? -1 : proj_idx, /*over=*/true, region_w, region_h,
				                     &box, &hash);
				const bool twod_bound = comp_d3d11_xbridge_bind_plane(
				    c->xbridge, COMP_D3D11_XBRIDGE_PLANE_LOCAL2D, c->local2d_scratch_share,
				    c->local2d_scratch_gen, (uint32_t)unorm_fmt, c->split_panel_w, c->split_panel_h);
				if (twod_bound) {
					comp_d3d11_xbridge_stage_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_LOCAL2D,
					                               hash, box.offset.w, box.offset.h,
					                               (uint32_t)box.extent.w, (uint32_t)box.extent.h);
				} else {
					/*
					 * #918 review D4: the Local2D plane IS the composite's
					 * `twod` under the split, so a frame that could not bind it
					 * has no composite to stamp. Claiming otherwise stamped
					 * `composite=true` on a slot whose plane the submit would
					 * then mark invalid, and the consume half bailed on every
					 * frame afterwards with no log outside the #876 diag.
					 */
					if (!c->local2d_plane_warned) {
						c->local2d_plane_warned = true;
						U_LOG_W(
						    "D3D11 output-device split: the Local2D plane could not be bound "
						    "— 2D content does not composite under the split for this "
						    "session; the 3D weave is unaffected (#918 Phase 2a)");
					}
					// Returning false routes through layer_commit's
					// `!deposited` branch, which stamps composite=false and
					// un-stages the plane — the one place that decision lives.
					c->repaint.composite_bail = 4;
					return false;
				}

				// Stamp the recipe this frame's slot will carry.
				struct comp_d3d11_xbridge_recipe r = {};
				r.composite = true;
				r.mask_kind = mask_kind;
				r.region_w = region_w;
				r.region_h = region_h;
				if (zones_frame) {
					r.composite_mode = COMP_D3D11_COMPOSITE_MODE_ZONES;
				} else if (have_explicit) {
					r.composite_mode = COMP_D3D11_COMPOSITE_MODE_LERP;
				} else {
					r.composite_mode = COMP_D3D11_COMPOSITE_MODE_ALPHA_OVER;
				}
				r.opaque_present =
				    c->transparent_background && debug_get_bool_option_present_opaque_comp();
				r.cx = eff_canvas->valid ? eff_canvas->x : 0;
				r.cy = eff_canvas->valid ? eff_canvas->y : 0;
				r.cw = eff_canvas->valid ? eff_canvas->w : region_w;
				r.ch = eff_canvas->valid ? eff_canvas->h : region_h;
				// The backdrop's OWN extent — the DP declares it separately from
				// the composite region, and the two can legitimately differ.
				r.bd_w = c->repaint.backdrop_w;
				r.bd_h = c->repaint.backdrop_h;
				comp_d3d11_xbridge_stage_recipe(c->xbridge, &r);
			}
			c->repaint.composite_bail = 0; // deposit half completed
			return true;
		}
	} else {
		// Unreachable: the early-out gate already returns false unless one of
		// zones_frame / have_local_2d / have_explicit is set. Defensive only.
		return false;
	}

	/*
	 * #918 Phase 2a: under the split `twod` is the LOCAL2D PLANE that landed with
	 * this slot — the app-device scratch above lives on the wrong adapter and is
	 * never what the composite samples here.
	 */
	if (split_consume) {
		if ((rec.plane_valid & (1u << COMP_D3D11_XBRIDGE_PLANE_LOCAL2D)) == 0) {
			c->repaint.composite_bail = 4;
			return false;
		}
		twod_srv = static_cast<ID3D11ShaderResourceView *>(
		    comp_d3d11_xbridge_get_plane_srv(c->xbridge, slot, COMP_D3D11_XBRIDGE_PLANE_LOCAL2D,
		                                     rec.plane_seq[COMP_D3D11_XBRIDGE_PLANE_LOCAL2D]));
		if (twod_srv == nullptr) {
			c->repaint.composite_bail = 4;
			return false;
		}
	}

	// Snapshot the window region of the weave (the DP wrote dst; RT≠SRV, so
	// the lerp reads this copy — impl doc §3 step 2). Owned by the composite
	// unit: source and destination are both its device's (#918 Phase 2a).
	ID3D11ShaderResourceView *weave_srv = static_cast<ID3D11ShaderResourceView *>(
	    comp_d3d11_outcomp_snapshot_weave(c->outcomp, dst, region_w, region_h));

	// Effective canvas rect clamped to the window region (the shader ignores
	// it on the mask path; kept coherent for the CB anyway). #439 Phase 2:
	// this is the window rect while the mask is active.
	// #918 Phase 2a: under the split these come FROM THE SLOT, not from live CPU
	// state — see split_consume above.
	int32_t cx = split_consume ? rec.cx : (eff_canvas->valid ? eff_canvas->x : 0);
	int32_t cy = split_consume ? rec.cy : (eff_canvas->valid ? eff_canvas->y : 0);
	uint32_t cw = split_consume ? rec.cw : (eff_canvas->valid ? eff_canvas->w : region_w);
	uint32_t ch = split_consume ? rec.ch : (eff_canvas->valid ? eff_canvas->h : region_h);
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
		composite_mode = COMP_D3D11_COMPOSITE_MODE_ZONES;
	} else if (have_explicit) {
		composite_mode = COMP_D3D11_COMPOSITE_MODE_LERP;
	} else {
		composite_mode = COMP_D3D11_COMPOSITE_MODE_ALPHA_OVER;
	}

	// #833/#116 — opaque present on a transparent session: DWM completes no
	// blends, so the composite flattens against the weave (which the DP's
	// flattened gate already completed against the captured desktop) and
	// emits α=1. Opaque sessions keep today's behavior even with the env set.
	bool opaque_present = c->transparent_background && debug_get_bool_option_present_opaque_comp();

	if (split_consume) {
		/*
		 * #918 Phase 2a — the RECIPE REFUSAL, and the counter that proves it
		 * never has to fire.
		 *
		 * A slot composited under one blend recipe must never be finished under
		 * another. In practice a recipe change (zones on/off, 2D<->3D, a mask
		 * appearing) also changes the tile grid, so `eg_gen` refuses the slot one
		 * level up and this is unreachable — that is the DESIGN, and this test is
		 * what turns "should be unreachable" into a number in the log. Refusing
		 * costs the 2D band for one frame; compositing under the wrong recipe
		 * would put visibly wrong pixels on the panel.
		 */
		if (rec.composite_mode != composite_mode || rec.opaque_present != opaque_present) {
			c->split_recipe_refusals++;
			if (c->split_recipe_logged_gen != c->split_layout_gen) {
				c->split_recipe_logged_gen = c->split_layout_gen;
				U_LOG_W(
				    "#918 Phase 2a: refused the composite on egress slot %d — its recipe "
				    "(mode=%u opaque=%d) is not the one this frame runs (mode=%u opaque=%d). "
				    "The weave still went out; only the 2D band is held for this frame. "
				    "(total %llu)",
				    (int)slot, rec.composite_mode, (int)rec.opaque_present, composite_mode,
				    (int)opaque_present, (unsigned long long)c->split_recipe_refusals);
			}
			c->repaint.composite_bail = 7;
			return false;
		}
		composite_mode = rec.composite_mode;
		opaque_present = rec.opaque_present;
	}
	xrt_result_t xret = comp_d3d11_outcomp_composite_2d_masked(
	    c->outcomp, dst, twod_srv, mask_srv, weave_srv, region_w, region_h, (int32_t)cx_u, (int32_t)cy_u,
	    cright - cx_u, cbottom - cy_u, composite_mode, opaque_present);

	// One-shot lifecycle log (NOT per-frame): proves the masked composite ran +
	// which mask source and mode resolved. WARN so it survives the hot-path
	// INFO filter.
	if (xret == XRT_SUCCESS) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("D3D11 Local2D composite: %ux%u region, %s mask (mode=%u)", region_w, region_h,
			        zones_frame ? "zone" : (have_explicit ? "explicit" : "implicit"), composite_mode);
		}
	}
	// #876 diag: 0 = composited, 6 = the draw itself failed.
	c->repaint.composite_bail = (xret == XRT_SUCCESS) ? 0 : 6;
	return xret == XRT_SUCCESS;
}

// #918: the authored-mask resource set, allocated on an EXPLICIT device.
// A Tier-3 mask is drawn by the APP through the returned RTV, so that one is
// pinned to the app device — but naming the device at the call site is what
// makes that a stated rule instead of an accident of which struct member was
// in scope. Callers pass c->device / c->context.
//
// #918 Phase 2a: @p out_device/@p out_context non-NULL (i.e. the split is on)
// additionally allocate the OUTPUT-DEVICE SHADOW that Tier-1/2 authoring
// mirrors onto, and make the app-side staged texture NT-shareable so a Tier-3
// mask can ride the bridge as a plane.
static xrt_result_t
d3d11_zone_mask_alloc(ID3D11Device *device,
                      ID3D11DeviceContext *context,
                      ID3D11Device *out_device,
                      ID3D11DeviceContext *out_context,
                      uint32_t w,
                      uint32_t h,
                      struct comp_d3d11_zone_mask **out_mask)
{
	struct comp_d3d11_zone_mask *mask = U_TYPED_CALLOC(struct comp_d3d11_zone_mask);
	if (mask == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}
	mask->w = w;
	mask->h = h;
	const bool split = out_device != nullptr && out_device != device;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	HRESULT hr = device->CreateTexture2D(&td, nullptr, &mask->tex);
	if (SUCCEEDED(hr) && mask->tex != nullptr) {
		hr = device->CreateRenderTargetView(mask->tex, nullptr, &mask->rtv);
	}
	bool staged_shared = false;
	if (SUCCEEDED(hr) && mask->rtv != nullptr) {
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (split) {
			/*
			 * #918 review D1: the share flags are an OPTIMISATION (they let the
			 * Tier-3 mask ride the bridge as a plane), never a requirement. A
			 * stack that refuses a shared R8 texture must not take
			 * xrCreateLocal3DZoneMaskDXR down with it — every other Phase-2a
			 * failure on this path degrades, and so does this one: retry
			 * unshared, and Tier 1/2 keep working through the output-device
			 * shadow.
			 */
			td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
			hr = device->CreateTexture2D(&td, nullptr, &mask->staged);
			staged_shared = SUCCEEDED(hr);
			if (!staged_shared) {
				U_LOG_W(
				    "zone_mask_create: this stack refuses a shared R8 staging texture 0x%08x — "
				    "Tier-3 (app-drawn) masks will not cross the output-device split this "
				    "session; the mask itself is created normally (#918 Phase 2a)",
				    hr);
				td.MiscFlags = 0;
				hr = device->CreateTexture2D(&td, nullptr, &mask->staged);
			}
		} else {
			hr = device->CreateTexture2D(&td, nullptr, &mask->staged);
		}
		td.MiscFlags = 0;
	}
	if (SUCCEEDED(hr) && mask->staged != nullptr) {
		hr = device->CreateShaderResourceView(mask->staged, nullptr, &mask->staged_srv);
	}
	if (FAILED(hr) || mask->staged_srv == nullptr) {
		U_LOG_E("zone_mask_create: D3D resource creation failed: 0x%08x", hr);
		if (mask->staged != nullptr) {
			mask->staged->Release();
		}
		if (mask->rtv != nullptr) {
			mask->rtv->Release();
		}
		if (mask->tex != nullptr) {
			mask->tex->Release();
		}
		free(mask);
		return XRT_ERROR_ALLOCATION;
	}

	// Default to all-3D (M=1): an unauthored-but-submitted mask degrades to
	// the full weave (the no-2D-declared analog), never a blanked canvas.
	const float all_3d[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	context->ClearRenderTargetView(mask->rtv, all_3d);
	context->CopyResource(mask->staged, mask->tex);

	if (split) {
		// Plane source handle. A failure here only costs the TIER-3 path (the
		// shadow below still serves Tier 1/2), so it is a WARN, not an error.
		IDXGIResource1 *dr = nullptr;
		if (staged_shared &&
		    SUCCEEDED(mask->staged->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&dr))) &&
		    dr != nullptr) {
			if (FAILED(dr->CreateSharedHandle(nullptr,
			                                  DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			                                  nullptr, &mask->staged_share))) {
				mask->staged_share = nullptr;
			}
			dr->Release();
		}
		/*
		 * #918 Phase 2a: the generation must be unique ACROSS mask objects, not
		 * just across reallocations of one. The bridge re-opens a plane's source
		 * only when the generation changes, and a session can hand the single
		 * authored-mask plane two different mask objects (the sticky composite
		 * mask on a legacy frame, the explicit frame wish on a zones frame) — two
		 * masks both numbered 1 would leave the producer reading the first one's
		 * texture for the second one's pixels.
		 */
		static uint64_t s_zone_mask_gen = 0;
		mask->staged_gen = ++s_zone_mask_gen;
		if (mask->staged_share == nullptr) {
			U_LOG_W(
			    "zone_mask_create: no NT share handle for the authored mask — Tier-3 (app-drawn) "
			    "masks will not cross the output-device split this session (#918 Phase 2a)");
		}

		// The output-device shadow Tier-1/2 authoring mirrors onto.
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		hr = out_device->CreateTexture2D(&td, nullptr, &mask->out_tex);
		if (SUCCEEDED(hr) && mask->out_tex != nullptr) {
			hr = out_device->CreateRenderTargetView(mask->out_tex, nullptr, &mask->out_rtv);
		}
		if (SUCCEEDED(hr) && mask->out_rtv != nullptr) {
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			hr = out_device->CreateTexture2D(&td, nullptr, &mask->out_staged);
		}
		if (SUCCEEDED(hr) && mask->out_staged != nullptr) {
			hr = out_device->CreateShaderResourceView(mask->out_staged, nullptr, &mask->out_staged_srv);
		}
		if (FAILED(hr) || mask->out_staged_srv == nullptr) {
			/*
			 * #918 review D5: name the actual consequence. With no shadow this
			 * mask rides the bridge plane at every tier (d3d11_stage_mask_plane),
			 * which costs one R8 transport per authoring call instead of zero —
			 * a real degradation with a real fallback behind it. Without a share
			 * handle either, there is nothing left and the mask is inert under
			 * the split.
			 */
			U_LOG_W("zone_mask_create: output-device shadow failed 0x%08x — %s (#918 Phase 2a)", hr,
			        mask->staged_share != nullptr
			            ? "every tier of this mask rides the bridge plane instead, at one R8 transport "
			              "per authoring call"
			            : "and there is no share handle either, so this mask cannot composite under the "
			              "output-device split at all");
		} else {
			out_context->ClearRenderTargetView(mask->out_rtv, all_3d);
			out_context->CopyResource(mask->out_staged, mask->out_tex);
		}
	}

	*out_mask = mask;
	return XRT_SUCCESS;
}

// #918 — Tier-1 authoring (whole mask 3D or 2D) on an EXPLICIT context.
// Phase 2a: mirrored onto the output-device shadow when one exists. The raster
// samples nothing, so running it twice is exact — not an approximation of a
// transport.
static void
d3d11_zone_mask_fill_whole(ID3D11DeviceContext *context,
                           ID3D11DeviceContext *out_context,
                           struct comp_d3d11_zone_mask *mask,
                           bool enable_3d)
{
	const float m[4] = {enable_3d ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
	context->ClearRenderTargetView(mask->rtv, m);
	if (out_context != nullptr && mask->out_rtv != nullptr) {
		out_context->ClearRenderTargetView(mask->out_rtv, m);
	}
	mask->author_seq++;
}

// #918 — Tier-2 authoring (M=1 inside each rect) on an EXPLICIT context. The
// fourth mask rasterizer; like the other three its inputs are pure CPU rects,
// which is exactly why Phase 2a can mirror it onto the output device instead of
// transporting its result.
static xrt_result_t
d3d11_zone_mask_fill_rects_one(ID3D11DeviceContext *context,
                               ID3D11RenderTargetView *rtv,
                               struct comp_d3d11_zone_mask *mask,
                               uint32_t count,
                               const struct xrt_rect *rects)
{
	// M=0 everywhere, then M=1 inside each rect (client-window px, clamped).
	const float all_2d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	context->ClearRenderTargetView(rtv, all_2d);

	// ID3D11DeviceContext1::ClearView with rects — D3D11.1, present on every
	// Win10/11 driver this runtime targets.
	ID3D11DeviceContext1 *ctx1 = nullptr;
	HRESULT hr = context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx1));
	if (FAILED(hr) || ctx1 == nullptr) {
		U_LOG_E("zone_mask_set_rects: ID3D11DeviceContext1 unavailable (hr=0x%08x)", hr);
		return XRT_ERROR_D3D;
	}

	const float all_3d[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	for (uint32_t i = 0; i < count; i++) {
		int32_t left = rects[i].offset.w;
		int32_t top = rects[i].offset.h;
		int32_t right = left + rects[i].extent.w;
		int32_t bottom = top + rects[i].extent.h;
		// Clamp to the mask; skip fully-outside / degenerate rects.
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
		D3D11_RECT dr = {left, top, right, bottom};
		ctx1->ClearView(rtv, all_3d, &dr, 1);
	}
	ctx1->Release();
	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_zone_mask_fill_rects(ID3D11DeviceContext *context,
                           ID3D11DeviceContext *out_context,
                           struct comp_d3d11_zone_mask *mask,
                           uint32_t count,
                           const struct xrt_rect *rects)
{
	xrt_result_t xret = d3d11_zone_mask_fill_rects_one(context, mask->rtv, mask, count, rects);
	if (xret == XRT_SUCCESS && out_context != nullptr && mask->out_rtv != nullptr) {
		xret = d3d11_zone_mask_fill_rects_one(out_context, mask->out_rtv, mask, count, rects);
	}
	if (xret == XRT_SUCCESS) {
		mask->author_seq++;
	}
	return xret;
}

extern "C" xrt_result_t
comp_d3d11_compositor_zone_mask_create(struct xrt_compositor *xc, uint32_t w, uint32_t h, void **out_mask)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
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
			D3D11_TEXTURE2D_DESC td;
			c->shared_texture->GetDesc(&td);
			w = td.Width;
			h = td.Height;
		} else if (c->target != nullptr) {
			comp_d3d11_target_get_dimensions(c->target, &w, &h);
		}
	}
	if (w == 0 || h == 0) {
		U_LOG_E("zone_mask_create: no window/surface to derive mask dims from");
		return XRT_ERROR_ALLOCATION;
	}

	struct comp_d3d11_zone_mask *mask = nullptr;
	xrt_result_t xret = d3d11_zone_mask_alloc(c->device, c->context, c->split_active ? c->out_dev : nullptr,
	                                          c->split_active ? c->out_ctx : nullptr, w, h, &mask);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// One-off lifecycle event (WARN per the debug-logging convention so it
	// survives the hot-path INFO filter).
	U_LOG_W("zone_mask_create: %ux%u (client-window px)", w, h);
	*out_mask = mask;
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_compositor_zone_mask_set_whole(struct xrt_compositor *xc, void *mask_ptr, bool enable_3d)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d11_zone_mask *mask = static_cast<struct comp_d3d11_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->rtv == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	d3d11_zone_mask_fill_whole(c->context, c->split_active ? c->out_ctx : nullptr, mask, enable_3d);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                          void *mask_ptr,
                                          uint32_t count,
                                          const struct xrt_rect *rects)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d11_zone_mask *mask = static_cast<struct comp_d3d11_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->rtv == nullptr || (count > 0 && rects == nullptr)) {
		return XRT_ERROR_ALLOCATION;
	}

	return d3d11_zone_mask_fill_rects(c->context, c->split_active ? c->out_ctx : nullptr, mask, count, rects);
}

extern "C" xrt_result_t
comp_d3d11_compositor_zone_mask_acquire_rt(struct xrt_compositor *xc,
                                           void *mask_ptr,
                                           void **out_rtv,
                                           uint32_t *out_w,
                                           uint32_t *out_h)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d11_zone_mask *mask = static_cast<struct comp_d3d11_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->rtv == nullptr || out_rtv == nullptr || out_w == nullptr || out_h == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// The runtime retains ownership of the RTV (the app must not Release
	// it); valid until the mask handle is destroyed. The compositor device
	// is the app's own D3D11 device in-process, so the app draws directly.
	*out_rtv = mask->rtv;
	*out_w = mask->w;
	*out_h = mask->h;
	mask->author_seq++;
	/*
	 * #918 Phase 2a: from here on this mask's authoritative pixels live on the
	 * APP device — the app is about to draw into them and no CPU-rect replay on
	 * the output-device shadow can reproduce that. Latch it so the submit path
	 * routes this mask through the bridge plane instead of the shadow, for the
	 * rest of its life (a later Tier-1/2 fill does not un-author it: the shadow
	 * would then be missing whatever the app had already drawn).
	 */
	mask->app_authored = true;
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d11_zone_mask *mask = static_cast<struct comp_d3d11_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->staged == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// Snapshot the authoring texture so in-progress Tier-3 drawing can never
	// tear into a frame, and make this the active mask. Sticky
	// last-submit-wins: it stays active across frames until re-submit or
	// destroy.
	//
	// #918 review, R1-adjacent: order the write behind the producer's read of
	// the previous seq — see d3d11_zone_mask_stage.
	if (c->split_active && c->xbridge != nullptr) {
		comp_d3d11_xbridge_pre_plane_write(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_MASK);
	}
	c->context->CopyResource(mask->staged, mask->tex);
	// #918 Phase 2a: stage the shadow too, so a Tier-1/2 mask is consumable on
	// the output device without any transport at all.
	if (c->split_active && mask->out_staged != nullptr && mask->out_tex != nullptr) {
		c->out_ctx->CopyResource(mask->out_staged, mask->out_tex);
	}
	mask->submitted = true;
	mask->author_seq++;
	c->active_zone_mask = mask;
	c->zone_publish_seq++; // #224: new content generation for the DP publish
	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d11_zone_mask *mask = static_cast<struct comp_d3d11_zone_mask *>(mask_ptr);
	if (mask == nullptr) {
		return;
	}
	// XR_DXR_display_zones: never leave a dangling frame-wish reference.
	if (c->frame_wish == mask) {
		c->frame_wish = nullptr;
	}
	if (c->frame_wish_last == mask) {
		c->frame_wish_last = nullptr;
	}
	if (c->active_zone_mask == mask) {
		c->active_zone_mask = nullptr; // no active mask
		// #224: withdraw the DP zone contribution now — the session may not
		// commit another frame (teardown-path destroy), and the per-frame
		// sync would otherwise leave the panel pinned by a dead client.
		d3d11_sync_zone_mask_to_dp(c);
	}
	/*
	 * #918 Phase 2a: the bridge's producer may hold an open of mask->staged as
	 * the authored-mask plane source. Drop the binding (which drains the producer
	 * link, or leaks on timeout) BEFORE releasing the texture underneath it.
	 *
	 * #918 review D2: only when the plane is actually bound to THIS mask.
	 * `staged_gen` is unique per mask object, so the test is exact — and without
	 * it, destroying any other mask that merely HAS a share handle unbound the
	 * live one, costing a blocking drain plus a full re-transport for nothing.
	 */
	if (c->split_active && c->xbridge != nullptr && mask->staged_share != nullptr &&
	    c->mask_plane_gen == mask->staged_gen) {
		comp_d3d11_xbridge_bind_plane(c->xbridge, COMP_D3D11_XBRIDGE_PLANE_MASK, nullptr, 0,
		                              (uint32_t)DXGI_FORMAT_R8_UNORM, 0, 0);
		c->mask_plane_live = false;
		c->mask_plane_gen = 0;
	}
	if (mask->out_staged_srv != nullptr) {
		mask->out_staged_srv->Release();
	}
	if (mask->out_staged != nullptr) {
		mask->out_staged->Release();
	}
	if (mask->out_rtv != nullptr) {
		mask->out_rtv->Release();
	}
	if (mask->out_tex != nullptr) {
		mask->out_tex->Release();
	}
	if (mask->staged_share != nullptr) {
		CloseHandle(mask->staged_share);
	}
	if (mask->staged_srv != nullptr) {
		mask->staged_srv->Release();
	}
	if (mask->staged != nullptr) {
		mask->staged->Release();
	}
	if (mask->rtv != nullptr) {
		mask->rtv->Release();
	}
	if (mask->tex != nullptr) {
		mask->tex->Release();
	}
	free(mask);
}

extern "C" void
comp_d3d11_compositor_zones_set_frame_wish(struct xrt_compositor *xc, void *mask)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	// Per-frame reference (XR_DXR_display_zones): oxr sets this on every
	// zones frame before layer_commit, NULL meaning auto-derive. Consumed
	// by the commit's wish-state resolve; harmlessly stale on zero-zone
	// frames (the zones branch never reads it there).
	c->frame_wish = static_cast<struct comp_d3d11_zone_mask *>(mask);
}

extern "C" bool
comp_d3d11_compositor_zone_get_hw_caps(struct xrt_compositor *xc,
                                       uint32_t *out_grid_w,
                                       uint32_t *out_grid_h)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	if (out_grid_w != nullptr) {
		*out_grid_w = 0;
	}
	if (out_grid_h != nullptr) {
		*out_grid_h = 0;
	}
	if (!d3d11_zone_dp_supported(c)) {
		return false;
	}
	if (out_grid_w != nullptr) {
		*out_grid_w = c->zone_dp_caps.zone_grid_width;
	}
	if (out_grid_h != nullptr) {
		*out_grid_h = c->zone_dp_caps.zone_grid_height;
	}
	return true;
}

// #439 Phase 3 Q4 — current recommended per-view render size. The renderer's
// view dims are recomputed each frame from the effective canvas (window rect
// while a mask/Local2D is active, canvas otherwise — see layer_commit's
// u_tiling_compute_canvas_view + resize), so this reflects the renegotiated
// size and oxr fires XrEventDataLocal3DZoneViewSizeChangedDXR when it changes.
extern "C" bool
comp_d3d11_compositor_get_recommended_view_size(struct xrt_compositor *xc, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	if (out_w == nullptr || out_h == nullptr || c->renderer == nullptr) {
		return false;
	}
	uint32_t vw = 0;
	uint32_t vh = 0;
	comp_d3d11_renderer_get_view_dimensions(c->renderer, &vw, &vh);
	if (vw == 0 || vh == 0) {
		return false;
	}
	*out_w = vw;
	*out_h = vh;
	return true;
}

extern "C" bool
comp_d3d11_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_eye_positions *out_eye_pos)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	if (c->display_processor != nullptr) {
		if (xrt_display_processor_d3d11_get_predicted_eye_positions(c->display_processor, out_eye_pos) &&
		    out_eye_pos->valid) {
			return true;
		}
	}

	return false;
}

extern "C" bool
comp_d3d11_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                              float *out_width_m,
                                              float *out_height_m)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	if (c->display_processor != nullptr) {
		return xrt_display_processor_d3d11_get_display_dimensions(
		    c->display_processor, out_width_m, out_height_m);
	}

	// Default display dimensions (typical laptop display size)
	*out_width_m = 0.3f;
	*out_height_m = 0.2f;

	return false;
}

extern "C" bool
comp_d3d11_compositor_get_window_metrics(struct xrt_compositor *xc,
                                          struct xrt_window_metrics *out_metrics)
{
	if (xc == nullptr || out_metrics == nullptr) {
		if (out_metrics != nullptr) {
			out_metrics->valid = false;
		}
		return false;
	}

	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	memset(out_metrics, 0, sizeof(*out_metrics));

	// Shared-texture (texture-app) sessions carry the app's window in
	// app_hwnd (c->hwnd stays null — no swapchain on it). Their metrics
	// come from that window — the documented model (swapchain-model.md:
	// view dims + Kooima projection use canvas size, not display size).
	// Without this, texture sessions had NO window metrics at all and the
	// runtime-side Kooima (rig path, raw channel, legacy-2D fovs) ran
	// display-scoped (#396 W7).
	HWND metrics_hwnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	if (c->display_processor == nullptr || metrics_hwnd == nullptr) {
		return false;
	}

	// Get display pixel info from display processor
	uint32_t disp_px_w = 0, disp_px_h = 0;
	int32_t disp_left = 0, disp_top = 0;
	if (!xrt_display_processor_d3d11_get_display_pixel_info(
	        c->display_processor, &disp_px_w, &disp_px_h,
	        &disp_left, &disp_top)) {
		return false;
	}

	if (disp_px_w == 0 || disp_px_h == 0) {
		return false;
	}

	// Get physical display dimensions from display processor
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!xrt_display_processor_d3d11_get_display_dimensions(
	        c->display_processor, &disp_w_m, &disp_h_m)) {
		return false;
	}

	// Get window client rect
	RECT rect;
	if (!GetClientRect(metrics_hwnd, &rect)) {
		return false;
	}
	uint32_t win_px_w = static_cast<uint32_t>(rect.right - rect.left);
	uint32_t win_px_h = static_cast<uint32_t>(rect.bottom - rect.top);
	if (win_px_w == 0 || win_px_h == 0) {
		return false;
	}

	// Get window screen position
	POINT client_origin = {0, 0};
	ClientToScreen(metrics_hwnd, &client_origin);

	// Compute pixel size (meters per pixel)
	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	// Window physical size
	float win_w_m = (float)win_px_w * pixel_size_x;
	float win_h_m = (float)win_px_h * pixel_size_y;

	// Window center in pixels (relative to display origin in screen coords)
	float win_center_px_x = (float)(client_origin.x - disp_left) + (float)win_px_w / 2.0f;
	float win_center_px_y = (float)(client_origin.y - disp_top) + (float)win_px_h / 2.0f;

	// Display center in pixels
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;

	// Window center offset in meters
	// X: +right (screen coords and eye coords both +right)
	// Y: negated because screen coords Y-down, eye coords Y-up
	float offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	float offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	// Fill output
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

	return true;
}

extern "C" bool
comp_d3d11_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	if (xc == nullptr) {
		return false;
	}

	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	if (c->display_processor != nullptr) {
		return xrt_display_processor_d3d11_request_display_mode(c->display_processor, enable_3d);
	}

	return false;
}

extern "C" void
comp_d3d11_compositor_set_eye_tracking_mode(struct xrt_compositor *xc, uint32_t mode)
{
	if (xc == nullptr) {
		return;
	}

	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d11_set_eye_tracking_mode(c->display_processor, mode);
	}
}

extern "C" void
comp_d3d11_compositor_set_system_devices(struct xrt_compositor *xc,
                                          struct xrt_system_devices *xsysd)
{
	if (xc == nullptr) {
		return;
	}

	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	c->xsysd = xsysd;

	if (xsysd != nullptr) {
		U_LOG_I("D3D11 compositor: system devices set for qwerty support");
	}

	// Pass xsysd to self-owned window for direct qwerty input from main window
	// This enables WASDQE controls without requiring the SDL debug window
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_set_system_devices(c->own_window, xsysd);
	}
}

void
comp_d3d11_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc,
                                                   bool legacy,
                                                   float scale_x,
                                                   float scale_y,
                                                   uint32_t view_w,
                                                   uint32_t view_h)
{
	if (xc == nullptr) {
		return;
	}
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	c->legacy_app_tile_scaling = legacy;
	c->legacy_view_scale_x = scale_x;
	c->legacy_view_scale_y = scale_y;
	if (c->renderer != nullptr) {
		comp_d3d11_renderer_set_legacy_app_tile_scaling(c->renderer, legacy);
	}

	// Fix view dims at the actual recommended size the app was told to render at.
	if (legacy && c->renderer != nullptr && view_w > 0 && view_h > 0) {
		uint32_t target_h = (c->display_processor != nullptr) ? view_h : c->settings.preferred.height;
		comp_d3d11_renderer_resize(c->renderer, view_w, view_h, target_h);
	}
}
