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
#include "comp_d3d12_outcomp.h"

// #918 D12-3 — the output-device split: the activation decision and the
// cross-adapter transport, both shared with the D3D11 legs.
#include "comp_xbridge.h"
#include "comp_split_gate.h"
#include "comp_d3d12_deposit.h"
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
// #1264 heavy-d3d12 reroute: the same-adapter fill arm. Vulkan-free D3D11 code
// that lives in comp_vk_native; consumes only the pure-D3D11 handoff struct.
#include "vk_native/comp_vk_native_split.h"
#else
struct comp_vk_split; // the reroute fields exist either way; the code does not
#endif

#include "d3d11/comp_d3d11_window.h"

#include "util/comp_layer_accum.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_logging.h"
#include "util/u_setting.h"
#include "util/u_weave_scope.h"
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
#include "util/u_repaint_gate.h"
#include "util/u_fill_thread_win.h"
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
#include "d3d/d3d_scanout_helpers.hpp"
#include "d3d/d3d_weave_placement.h"
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

	//! DXGI factory the output target's swapchain is created from. Owned
	//! (released in destroy). Created here rather than inside the target
	//! because under the output-device split (#918) only the compositor knows
	//! which adapter scans out, and the target is handed the factory to use.
	//! CreateDXGIFactory2 is adapter-independent, so a session-lived factory
	//! is the same object the target used to make and destroy per create.
	IDXGIFactory4 *dxgi_factory;

	/*!
	 * #918 D12-3 / D12-4 — THE OUTPUT-DEVICE SPLIT.
	 *
	 * Everything in this block is NULL/false unless `DXR_WEAVE_ON_SCANOUT=1`
	 * resolved a scanout adapter that differs from the app's AND every step of
	 * Stage A succeeded. When it is live, the weave, the HUD, the 2D/3D
	 * composite, the repaint loop and the present all move to `out_dev` on the
	 * scanout adapter, and the only things that cross the adapter boundary are the
	 * ATLAS and — since D12-4 — the two 2D PLANES, once per app frame through
	 * @ref xbridge, never once per present and never per repaint.
	 *
	 * The app device keeps the renderer, the swapchains and the app's own
	 * command list. Nothing here is reachable from the app's frame path except
	 * the bridge's submit and its GPU-side back-fence, both of which are queue
	 * operations.
	 * @{
	 */
	//! Runtime-owned device on the scanout adapter.
	ID3D12Device *out_dev;
	//! Its DIRECT queue: the weave records here, the swapchain presents from it.
	ID3D12CommandQueue *out_queue;
	ID3D12CommandAllocator *out_cmd_allocator;
	ID3D12GraphicsCommandList *out_cmd_list;
	//! The out timeline's fence + event. See @ref d3d12_out_timeline.
	ID3D12Fence *out_fence;
	UINT64 out_fence_value;
	HANDLE out_fence_event;
	//! DXGI factory reached from the SCANOUT adapter, so the swapchain is
	//! created against a device that actually drives the panel.
	IDXGIFactory4 *out_factory;
	//! The DP's atlas SRV heap, on the out device (#854's heap, moved).
	ID3D12DescriptorHeap *out_dp_srv_heap;
	//! The cross-adapter transport, in its D3D12-ends flavour (#918 D12-3a).
	struct comp_xbridge *xbridge;
	//! True once Stage A completed. The single predicate every split branch reads.
	bool split_active;
	/*!
	 * #918 Phase 3 — the canonical short reason the `weave placement:` line
	 * prints while @ref split_active is false. One of the
	 * `COMP_SPLIT_REASON_*` tokens; NULL while the split is up. Written by
	 * Stage A and by @ref d3d12_split_retire, which is the only thing that can
	 * turn the split off after Stage A said yes.
	 */
	const char *split_off_reason;
	//! The scanout adapter, for the one canonical placement line.
	LUID out_luid;
	//! Panel extent Stage A resolved. A window can never exceed it, so it is
	//! the once-and-for-all bound the bridge's plane transports would be sized
	//! from — recorded here now, used by D12-4.
	uint32_t split_panel_w, split_panel_h;
	//! Monotonic app-frame counter handed to the bridge as its one seq.
	uint64_t split_seq;
	/*!
	 * #918 R1: the layout generation, bumped whenever the effective layout the
	 * DP is running changes. Stamped on the egress slot at submit, and tested
	 * at the weave — a slot composited under a layout the DP has since left
	 * must never be woven. "The atlas recipe IS the mode."
	 */
	uint64_t split_layout_gen;
	uint32_t split_gen_cols, split_gen_rows, split_gen_vw, split_gen_vh;
	//! #918 F4: weaves that produced nothing and whose Present was therefore
	//! SKIPPED, so the panel keeps the last good frame instead of flashing a
	//! cleared buffer. Reported on the periodic line, never per frame.
	uint64_t split_no_slot;
	//! Frames the output-device crop rescued from a worst-case egress ring.
	uint64_t split_out_crop;
	//! Periodic-diagnostic window start (the [RENDER] line's cadence).
	uint64_t split_diag_window_ns;
	/*!
	 * #1151 — the SRV-ring tripwire, carried from D12-1.
	 *
	 * @ref comp_d3d12_outcomp's descriptor ring wraps SILENTLY after
	 * `kSrvRingSets` composites, and the unit is a pure recorder: it cannot see
	 * where the caller's execution boundaries are, so only the caller can tell
	 * a safe wrap from an overwrite of a set still in flight. This counts
	 * composites recorded since the last known boundary. See
	 * @ref d3d12_outcomp_note_execute.
	 *
	 * #1151 — the output composite unit and its SRV-ring tripwire.
	 *
	 * The tripwire is SPLIT between the two files on purpose. The COUNT lives in
	 * `comp_d3d12_outcomp`, beside the ring depth it compares against, so a
	 * caller-side copy of that constant cannot drift from the ring it describes
	 * — drifting silently is the exact failure #1151 is about. The BOUNDARY is
	 * this file's, because only the caller knows where its `ExecuteCommandLists`
	 * are, and it is wired at every one of them (see
	 * @ref d3d12_outcomp_note_execute).
	 *
	 * D12-3 did not create the unit at all, and that was a warmup decision rather
	 * than an oversight: `comp_d3d12_outcomp_create` runs `D3DCompile` on the
	 * masked-composite shaders and builds a PSO per target format, and Stage A's
	 * explicit constraint is that it must not lengthen the session warmup the
	 * hybrid-iGPU investigation measured.
	 *
	 * #918 D12-4 creates it and KEEPS that constraint rather than relaxing it: the
	 * create is LAZY, on `out_dev` (the device-explicit create D12-1 exists for),
	 * taken from the DEPOSIT half of the first frame that actually composites. A
	 * session that never composites never pays it; one that does pays it once, on
	 * its own render thread, well past warmup. See @ref d3d12_ensure_outcomp.
	 */
	struct comp_d3d12_outcomp *outcomp;
	//! The lazy create was attempted and failed. Never retried, never re-WARNed —
	//! the deposit half runs every frame.
	bool outcomp_failed;
	//! One WARN, ever, for a session whose Local2D plane could not be bound.
	bool local2d_plane_warned;

	/*!
	 * #918 D12-5 — the app-authored (Tier-3) mask's transport state, written by
	 * the ONE per-frame publisher (@ref d3d12_bind_mask_plane) and read by
	 * everything that consumes an authored mask under the split.
	 *
	 * @ref mask_plane_live is "this frame's authoritative authored mask is
	 * riding @ref COMP_XBRIDGE_PLANE_MASK". @ref mask_plane_gen is the mask
	 * OBJECT generation it is riding for, so a session that hands the single
	 * plane two different masks cannot have one of them consume the other's
	 * pixels — the pointer alone is not enough, because a freed mask's address
	 * can be recycled.
	 * @{
	 */
	bool mask_plane_live;
	uint64_t mask_plane_gen;
	//! Monotonic source of @ref comp_d3d12_zone_mask::res_gen. Never reset.
	uint64_t zone_mask_gen_next;
	/*! @} */

	/*!
	 * #918 D12-4 — the mask raster the CONSUME half owes, captured at DEPOSIT.
	 *
	 * THE ORDERING THIS EXISTS FOR, because it is the one thing the D3D11 leg
	 * never had to solve. Both legs rebuild an auto/implicit/feather mask on the
	 * OUTPUT device rather than transporting it — it is pure CPU rects in, so it
	 * is built where the composite consumes it. D3D11 can do that inline in the
	 * deposit half, because it records onto an IMMEDIATE CONTEXT and the work is
	 * submitted as it is written. D3D12 records into a command list, and the
	 * out-device list is `Reset()` AFTER the deposit half runs (layer_commit closes
	 * and executes the APP list first, then arms the weave list) — so a raster
	 * recorded at deposit onto the weave list would be thrown away by that Reset,
	 * and one recorded onto the app list would be built on the wrong device.
	 *
	 * So the deposit half captures the raster REQUEST (rects and dims: CPU data,
	 * no device in sight) and the consume half records it onto the weave list, on
	 * the output device, immediately before the composite that samples it. Do not
	 * "tidy" this back to a deposit-time raster.
	 *
	 * Only an APP FRAME fills this. A repaint replays rendering and never state
	 * transitions, so it composites from @ref repaint.mask_res — the resource the
	 * last app frame's consume half rastered.
	 * @{
	 */
	//! @ref out_mask_req::kind
#define D3D12_OUT_MASK_NONE 0u
#define D3D12_OUT_MASK_IMPLICIT 1u     //!< #439 Local2D rect union (ALPHA_OVER).
#define D3D12_OUT_MASK_ZONE_BINARY 2u  //!< ADR-027 auto wish; ALSO the published wish.
#define D3D12_OUT_MASK_ZONE_FEATHER 3u //!< #803 per-zone ramp; the wish stays binary.
	struct
	{
		uint32_t kind;
		struct xrt_rect rects[XRT_MAX_LAYERS];
		float feather[XRT_MAX_LAYERS];
		uint32_t count;
		uint32_t w, h;
	} out_mask_req;
	/*! @} */
	/*! @} */

	//! The DP factory, kept so the display processor can be re-created on the
	//! APP device if the split has to be retired mid-session (#918 D12-3). Not
	//! owned — it is a function pointer supplied at create.
	void *dp_factory;

	//! #1264 heavy-d3d12 reroute: the D3D11 DP factory the d3d11 fill arm's
	//! Stage A asks for a weaver with. Not owned; NULL means the reroute
	//! refuses (`dp_refused_scanout`) and the tier keeps its own arm.
	void *dp_factory_d3d11;

	/*!
	 * #1264 / ADR-039 heavy-d3d12 reroute. When ACTIVE this session's fill,
	 * weave and present all belong to the d3d11 fill arm (`comp_vk_split` —
	 * the measured event-immune engine): the renderer's atlas is an imported
	 * D3D11 deposit slot, the compositor's own DP / target / out-device split
	 * machinery is never created (one weaver per HWND — the split took it),
	 * and the frame path exits through comp_vk_split_weave_and_present. The
	 * own-legs arm remains the hybrid path and the DXR_SPLIT_D3D12_ROUTE=own
	 * A/B control.
	 */
	struct
	{
		struct comp_vk_split *split;
		struct comp_d3d12_deposit *dep;
		bool active;
		//! The DP canvas rect the frame path last published (repaint replays it).
		struct xrt_rect canvas;
		//! Change-detection for the scratch->plane copies (planes are
		//! on-change surfaces; a steady frame costs a compare).
		uint64_t l2d_copied_hash;
		uint64_t bd_copied_hash;
		//! ADR-027 P4 wish publish sequence on this route.
		uint64_t wish_seq;
		//! Signature of the last wish actually published (dedupe).
		uint64_t wish_sig_published;
		//! Change-detection for the authored-mask staged->plane copy
		//! ((res_gen << 48) | author_seq — either half moving re-copies).
		uint64_t mask_copied_key;
	} reroute;

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

	/*!
	 * Flattened Local2D layers (the `twod` source). R8G8B8A8_UNORM render
	 * target — dedicated. Lazily (re)allocated window-sized.
	 *
	 * #918 D12-4: under the split this is a BRIDGE PLANE SOURCE and is allocated
	 * at the PANEL instead — once, and then never resized, which is what keeps it
	 * structurally outside the R2 resize-churn path (a per-size realloc of a plane
	 * chain on the frame path is what cost 21 of 50 frames in #1091). The flatten
	 * still writes only the window region at the top-left, exactly as #464 has it.
	 * @ref local2d_scratch_gen is bumped on every REALLOCATION so the bridge
	 * re-binds; a bare pointer compare is not enough, because an allocator that
	 * recycles an address would otherwise keep the producer reading the previous
	 * allocation (the trap `comp_xbridge_set_source`'s `source_key` documents).
	 */
	ID3D12Resource *local2d_scratch;
	ID3D12DescriptorHeap *local2d_scratch_rtv_heap;
	uint32_t local2d_scratch_w, local2d_scratch_h;
	uint64_t local2d_scratch_gen;

	//! #491 part 3 — 2D-under backdrop flatten target (same trio as
	//! local2d_scratch, and the same #918 D12-4 panel-sizing under the split).
	//! UNDER Local2D layers (before the projection in list order) flatten here
	//! PRE-weave, left in PIXEL_SHADER_RESOURCE; the ID3D12Resource* is handed to
	//! the DP via set_background_2d (the DP creates its own shader-visible SRV).
	//! Compositor-owned so it outlives process_atlas.
	ID3D12Resource *backdrop_scratch;
	ID3D12DescriptorHeap *backdrop_scratch_rtv_heap;
	uint32_t backdrop_scratch_w, backdrop_scratch_h;
	uint64_t backdrop_scratch_gen;

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

		/*
		 * (startup jerk) When the layer_begin -> layer_commit window opened.
		 *
		 * The comment above asserts this window is short because an app renders
		 * between xrWaitFrame and xrEndFrame. If a client holds it open instead,
		 * every repaint bails for the whole duration and the panel shows nothing --
		 * which is what ~211 ms present gaps during the Unity warm-up look like.
		 * Measured, not assumed: DXR_SLOW_SECTION_MS logs any window over the
		 * threshold (default 50 ms, 0 disables).
		 */
		uint64_t app_frame_begin_ns;

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

		//! #1257 interval-aware quiet gate.
		struct u_repaint_gate gate;

		//! DXR_WEAVE_REPAINT_TRACE=1 loop instrumentation (#1264 Phase B:
		//! this arm was the one fill loop with no trace rows — blind
		//! exactly where the same-adapter bring-up needed eyes).
		struct u_repaint_trace trace;
		//! #1264 event-absorption shed (DXR_FILL_SHED_FIRE_MS, opt-in).
		struct u_fill_shed shed;

		//! #1257 slot partition: xrWaitFrame throttle state.
		struct u_app_partition partition;

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
                          const struct u_canvas_rect *eff_canvas,
                          int32_t slot,
                          bool is_repaint);
// #918 D12-4 — the plane-transport helpers, defined with the Local2D helpers
// near the bottom and called from layer_commit + the backdrop flatten above them.
static void
d3d12_local2d_digest(struct comp_d3d12_compositor *c,
                     int32_t proj_idx,
                     bool over,
                     uint32_t region_w,
                     uint32_t region_h,
                     struct xrt_rect *out_box,
                     uint64_t *out_hash);
static void
d3d12_clamp_region_to_panel(struct comp_d3d12_compositor *c, uint32_t *w, uint32_t *h);
// #491 part 3 — pre-weave 2D-under backdrop flatten (defined with the Local2D
// helpers near the bottom; called from the process_atlas sites above).
static ID3D12Resource *d3d12_flatten_backdrop_2d(struct comp_d3d12_compositor *c, uint32_t dst_w, uint32_t dst_h,
                                                 uint32_t *out_w, uint32_t *out_h);
static void d3d12_release_zone_state(struct comp_d3d12_compositor *c);
// #224 / ADR-027 hardware-DP zone leg (P4): one-time caps probe + per-frame
// sideband publish of the wish / sticky mask. Defined with the zone helpers
// near the bottom; called after each path's fence wait in layer_commit.
static bool d3d12_zone_dp_supported(struct comp_d3d12_compositor *c);
// (startup jerk) sub-step timers; defined with the zone helpers near the bottom.
static void d3d12_zone_step(const char *nm);
static void d3d12_zone_step_begin(void);
static bool d3d12_zone_dp_supported_weaving_arm(struct comp_d3d12_compositor *c);
static void d3d12_sync_zone_mask_to_dp(struct comp_d3d12_compositor *c);
// #918 D12-5 — the app-authored (Tier-3) mask's bridge transport. Defined with
// the zone helpers near the bottom; both are called from layer_commit's DP+target
// path above them, and only from there (see d3d12_bind_mask_plane).
struct comp_d3d12_zone_mask;
static struct comp_d3d12_zone_mask *
d3d12_frame_authored_mask(struct comp_d3d12_compositor *c);
static bool
d3d12_bind_mask_plane(struct comp_d3d12_compositor *c, struct comp_d3d12_zone_mask *mask);
static void
d3d12_stage_mask_plane(struct comp_d3d12_compositor *c, struct comp_d3d12_zone_mask *mask);
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

/*
 *
 * GPU timelines — device-scoped idle waits (#918 D12-2).
 *
 */

/*!
 * ONE queue, ONE fence, ONE event, and the device they all belong to.
 *
 * There is no back-pointer to the compositor here, and no pointer to any other
 * timeline. That absence is the point of the type: a `comp_d3d12_timeline` names
 * a single GPU execution scope and gives its holder no way to reach a second
 * one. See @ref gpu_wait_idle_on for the invariant it exists to enforce.
 *
 * Passed BY VALUE (six words). `value` is a pointer into the owner's storage, so
 * a copy still advances the one real counter.
 */
struct comp_d3d12_timeline
{
	//! "app" / "out" — names the scope in the device-removed report.
	const char *name;
	//! The device the queue and the fence belong to. NOT owned.
	ID3D12Device *device;
	//! The queue signalled and waited on. NOT owned.
	ID3D12CommandQueue *queue;
	//! The fence signalled from @ref queue. NOT owned.
	ID3D12Fence *fence;
	//! Points at the owner's monotonic counter for @ref fence.
	UINT64 *value;
	//! Auto-reset event @ref fence completion is signalled onto.
	HANDLE event;
};

/*!
 * The app-adapter timeline: the device the application handed us, the queue the
 * renderer and every atlas-side command list execute on.
 */
static struct comp_d3d12_timeline
d3d12_app_timeline(struct comp_d3d12_compositor *c)
{
	return {"app", c->device, c->command_queue, c->fence, &c->fence_value, c->fence_event};
}

/*!
 * True when the output timeline is a DIFFERENT queue from the app timeline, so
 * a caller that must quiesce EVERYTHING (only teardown) knows to wait twice.
 *
 * #918 D12-3 made this reachable: it is exactly "the split is standing up and
 * running". Off the split it stays false and every wait routed through
 * @ref d3d12_out_timeline is bit-for-bit the wait that was there before.
 */
static bool
d3d12_out_timeline_is_separate(struct comp_d3d12_compositor *c)
{
	return c->split_active && c->out_queue != nullptr && c->out_fence != nullptr;
}

/*!
 * The output-adapter timeline: the device the weave records on, the swapchain
 * presents from, and the display processor talks to.
 *
 * Under the split this is the scanout device's own queue and fence, which is
 * what makes D12-2's invariant load-bearing rather than decorative — an
 * app-thread wait now genuinely cannot cover the panel-side work, because the
 * two scopes no longer share a queue. Off the split it resolves to the app
 * timeline, unchanged.
 */
static struct comp_d3d12_timeline
d3d12_out_timeline(struct comp_d3d12_compositor *c)
{
	if (!d3d12_out_timeline_is_separate(c)) {
		return d3d12_app_timeline(c);
	}
	return {"out", c->out_dev, c->out_queue, c->out_fence, &c->out_fence_value, c->out_fence_event};
}

/*
 *
 * #918 D12-3 — "which device?" accessors.
 *
 * Every out-device object has an app-device twin, and off the split the two are
 * the same object. Routing through these three (rather than testing
 * `split_active` at each use) is what keeps the weave path ONE code path: a
 * repaint is constructed exactly like the app frame it stands in for, on either
 * placement.
 *
 */

//! The device the weave target, the DP and the HUD staging all belong to.
static inline ID3D12Device *
d3d12_out_device(struct comp_d3d12_compositor *c)
{
	return c->split_active ? c->out_dev : c->device;
}

//! The queue the weave list executes on and the swapchain presents from.
static inline ID3D12CommandQueue *
d3d12_out_queue(struct comp_d3d12_compositor *c)
{
	return c->split_active ? c->out_queue : c->command_queue;
}

//! The list the weave records into.
static inline ID3D12GraphicsCommandList *
d3d12_weave_list(struct comp_d3d12_compositor *c)
{
	return c->split_active ? c->out_cmd_list : c->cmd_list;
}

//! The atlas SRV heap the DP samples through (#854), on the weave's device.
static inline ID3D12DescriptorHeap *
d3d12_weave_srv_heap(struct comp_d3d12_compositor *c)
{
	return c->split_active ? c->out_dp_srv_heap : c->dp_srv_heap;
}

/*!
 * #1151 — the caller half of the outcomp SRV-ring tripwire.
 *
 * The unit hands out one descriptor set per composite and wraps when it runs
 * out; a wrap is only safe once the set being reused has finished EXECUTING.
 * The unit is a pure recorder and cannot see a submission boundary — this file
 * can, and this is where every one of them is. The counting lives in the unit,
 * beside the ring depth it has to compare against, so the two cannot drift.
 *
 * Wired at every execute in this file, including the ones that happen with no
 * unit yet (the call is NULL-tolerant). That is deliberate: the boundary half
 * has to be complete BEFORE the first composite exists, or the first composite
 * lands against a tripwire that only knows about some of the boundaries.
 */
static inline void
d3d12_outcomp_note_execute(struct comp_d3d12_compositor *c)
{
	comp_d3d12_outcomp_note_execute(c->outcomp);
}

/*!
 * Signal @p tl's fence from @p tl's queue and CPU-wait for it. The ONLY CPU
 * idle wait in this compositor.
 *
 * THE INVARIANT — an app-thread wait must never cover out-queue work.
 * ---------------------------------------------------------------------------
 * Under the output-device split there are two queues: the app queue, driven by
 * the application's own render thread through xrEndFrame, and the out queue on
 * the scanout adapter, which weaves and presents. The app queue's waits sit
 * directly on the app's frame loop. If one of them could only complete after
 * the OUT queue had drained, then a slow or stalled panel-side present would
 * block the application inside an OpenXR call — a CPU wait on the render path
 * that can stall the workspace, which is the #925 wedge class exactly.
 *
 * WHY IT CANNOT HAPPEN HERE, rather than merely does not:
 *
 *  1. This function takes a `comp_d3d12_timeline`, not the compositor. A
 *     timeline holds one queue and one fence and NOTHING that names another
 *     timeline, so the wait below is reachable only by that queue's own Signal.
 *     There is no "wait for the GPU" entry point left to call: the old
 *     compositor-wide `gpu_wait_idle(c)` is gone, and every call site names its
 *     scope.
 *  2. A fence wait can only be delayed by work AHEAD of its Signal in the SAME
 *     queue. So the only way to make an app-side wait cover out-queue work is
 *     to record `app_queue->Wait(out_fence)` — a cross-queue GPU wait in the
 *     app→out direction. Nothing does, and nothing may: the data flows the
 *     other way (the app renders the atlas, the out side consumes it), so the
 *     legal direction is `out_queue->Wait(app_fence)`, and D12-3's cross-adapter
 *     ordering runs that way through the bridge. Adding the inverse means
 *     writing a new call against a fence this file hands to nobody — a
 *     reviewable act, not an accident at a call site.
 *
 * The wait is INFINITE, unchanged from the single wait it replaces. That is
 * defensible only because of the above: it is bounded in practice by the
 * completion of work this same queue already holds, and D3D12 has no
 * partial-timeout idle primitive that would leave the command allocator safe to
 * reset. Two of the four escalating present watchdogs (#1000, in the target)
 * cover the case where the OUT side is the thing that never returns; that is
 * the out timeline's problem to report, and — by (2) — never the app's to wait
 * for.
 */
static void
gpu_wait_idle_on(struct comp_d3d12_timeline tl)
{
	(*tl.value)++;
	tl.queue->Signal(tl.fence, *tl.value);

	if (tl.fence->GetCompletedValue() < *tl.value) {
		tl.fence->SetEventOnCompletion(*tl.value, tl.event);
		WaitForSingleObject(tl.event, INFINITE);
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
		if (!s_reported && tl.device != nullptr) {
			HRESULT rr = tl.device->GetDeviceRemovedReason();
			if (rr != S_OK) {
				s_reported = true;
				U_LOG_E(
				    "#747 DEVICE REMOVED observed by the compositor at gpu_wait_idle: "
				    "GetDeviceRemovedReason=0x%08x (%s timeline)",
				    (unsigned)rr, tl.name);
				comp_d3d12_log_dred_state(tl.device, "gpu_wait_idle/device-removed");
			}
		}
	}
}

/*!
 * Drain the APP adapter: everything this compositor recorded onto the
 * application's queue is complete on return. Use before reading an app-device
 * resource back, before resetting the shared command allocator, and before
 * handing an app-device texture to something that will read it without a fence.
 *
 * Never covers out-queue work — see @ref gpu_wait_idle_on.
 */
static void
gpu_wait_idle_app(struct comp_d3d12_compositor *c)
{
	gpu_wait_idle_on(d3d12_app_timeline(c));
}

/*!
 * Drain the OUTPUT adapter: the weave and the present are complete on return.
 * Use for frame pacing behind Present, and before anything that perturbs the
 * display (a 2D/3D mode switch, teardown).
 *
 * Today this resolves to the same queue as @ref gpu_wait_idle_app — see
 * @ref d3d12_out_timeline.
 */
static void
gpu_wait_idle_out(struct comp_d3d12_compositor *c)
{
	gpu_wait_idle_on(d3d12_out_timeline(c));
}

/*
 *
 * #918 D12-3 — Stage A: stand the output device and the bridge up.
 *
 */

//! The target and the display processor are created at session start AND, if the
//! split has to be retired mid-session, again on the app device. One definition
//! each, so the two placements cannot drift apart.
static xrt_result_t
d3d12_make_target(struct comp_d3d12_compositor *c, bool transparent)
{
	/*
	 * #918 D12-3: under the split the swapchain, its frame-latency waitable and
	 * its DXGI frame statistics all live on the SCANOUT adapter — which is what
	 * removes the cross-adapter present. The target takes its device, queue and
	 * factory explicitly (D12-2), so nothing inside it has to know.
	 */
	xrt_result_t xret = comp_d3d12_target_create(
	    c->hwnd, d3d12_out_device(c), d3d12_out_queue(c), c->split_active ? c->out_factory : c->dxgi_factory,
	    c->settings.preferred.width, c->settings.preferred.height, transparent, &c->target);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	if (comp_d3d12_target_has_child_window(c->target)) {
		U_LOG_I("D3D12 target using child window fallback (parent HWND: %p)", (void *)c->hwnd);
	}
	return XRT_SUCCESS;
}

/*!
 * THE OUTPUT-DEVICE SPLIT'S TWO DEV-ONLY FAULT-INJECTION ARMS (#918 D12-3).
 * Documented together because they walk the two HALVES of the same fallback
 * matrix, and finding one without the other is how a verification pass ends up
 * covering half of it.
 *
 *   `DXR_TEST_SPLIT_FAIL_STAGEA=1` — the PRE-activation half. Fails Stage A at
 *       the point the bridge would be created, so "one WARN, stock path" is
 *       walked without a machine that genuinely cannot allocate a cross-adapter
 *       heap. Read by @ref comp_split_gate_env_test_fail_stage_a (it is shared
 *       with the D3D11 legs); used in @ref d3d12_split_stage_a.
 *
 *   `DXR_TEST_FAKE_DP_REFUSE=1` — the POST-activation half, below. Makes the
 *       display processor decline a weaver on the SCANOUT adapter, which is the
 *       one fallback row that can only fire after the split is already up and
 *       the target already lives on the other device (ADR-037 §3a).
 *
 * Same shape as `DXR_TEST_FAKE_DEVICE_REMOVED` in the D3D11 service: opt-in
 * env, latched on first read, never anything but a test.
 *
 * The arm fires ONLY while the split is active — i.e. only on the OUT-device
 * create — and that asymmetry is the whole point. What is under test is the
 * RECOVERY: refuse on the scanout adapter, retire the split, and let the
 * app-device rebuild succeed, so the session ends up weaving. An arm that also
 * failed the rebuild would leave no display processor anywhere and would be
 * testing a different (and less interesting) thing.
 */
static bool
d3d12_test_fake_dp_refuse(void)
{
	static int want = -1;
	if (want < 0) {
		const char *e = getenv("DXR_TEST_FAKE_DP_REFUSE");
		want = (e != nullptr && e[0] == '1') ? 1 : 0;
	}
	return want == 1;
}

/*!
 * Create the display processor on the device the weave will run on, and apply
 * the three session-level settings that have to follow it there.
 *
 * #918 D12-3 — THE BIND KEY IS `(hwnd, device)`, NOT `hwnd`. A vendor weaver is
 * one-per-HWND and holds device-scoped state, so moving the weave to another
 * adapter is not "tell the DP about a new device", it is DESTROY THEN CREATE —
 * and in that order, because a second weaver on the same window is refused. The
 * service leg learned this the same way; @ref d3d12_split_retire is the only
 * place the device half of the key ever changes after session start.
 *
 * @return false when no display processor was created. The CALLER decides what
 *         that means, because the answer differs by placement: a refusal on the
 *         SCANOUT device is recoverable (retire the split and try the app
 *         device — ADR-037 §3a), a refusal on the app device is not.
 */
static bool
d3d12_make_dp(struct comp_d3d12_compositor *c)
{
	if (c->dp_factory == nullptr) {
		return false;
	}
	auto factory = (xrt_dp_factory_d3d12_fn_t)c->dp_factory;
	HWND dp_hwnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;

	// Dev-only injection — see d3d12_test_fake_dp_refuse. The factory is not
	// called at all on this path, so no weaver can be left behind on the HWND.
	const bool fake_refuse = c->split_active && d3d12_test_fake_dp_refuse();
	if (fake_refuse) {
		U_LOG_W("DXR_TEST_FAKE_DP_REFUSE=1: refusing the display processor on the SCANOUT device (test)");
	}

	xrt_result_t dp_ret = fake_refuse
	                          ? XRT_ERROR_DEVICE_CREATION_FAILED
	                          : factory(d3d12_out_device(c), d3d12_out_queue(c), dp_hwnd, &c->display_processor);
	if (dp_ret != XRT_SUCCESS) {
		U_LOG_W("D3D12 display processor factory failed (error %d) on the %s device", (int)dp_ret,
		        c->split_active ? "SCANOUT" : "app");
		c->display_processor = nullptr;
		return false;
	}
	U_LOG_W("D3D12 display processor created via factory on %s device (hwnd %p)",
	        c->split_active ? "the SCANOUT" : "the app", (void *)dp_hwnd);

	// Report the DP's weave scope once (no-op for every GPU weaver, which
	// reads as canvas). In-process output is a WINDOW's client area, so a
	// scanout-scoped hardware weaver gets told here that it cannot be correct
	// — instead of silently shredding the whole panel with nothing in the log.
	(void)u_weave_scope_report(xrt_display_processor_d3d12_get_weave_scope(c->display_processor), "D3D12",
	                           /* panel_scoped */ false);

	// Tell the weaver the output render target format so it can create its
	// internal pipeline state. Without this, the weaver's pipeline state stays
	// null and weave() silently no-ops. Use the shared texture format when
	// available (texture apps), otherwise the swapchain format (handle apps).
	DXGI_FORMAT output_fmt =
	    c->has_shared_texture ? c->shared_texture->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
	xrt_display_processor_d3d12_set_output_format(c->display_processor, output_fmt);
	U_LOG_W("D3D12 display processor: output format set to %u (target=%p)", (unsigned)output_fmt,
	        (void *)c->target);

	// Forward session-level transparency (#573 — chroma-key-free).
	// client_presents=false — DELIBERATELY; #904's true was reverted after a
	// hardware eyeball. See comp_d3d11_compositor.cpp for the full rationale.
	xrt_display_processor_d3d12_set_transparent_background(c->display_processor, c->transparent_background, false);

	// #68: tell the DP whether the app self-presents only the canvas (texture
	// app) vs the runtime presenting the full target (handle).
	xrt_display_processor_d3d12_set_shared_texture_present(c->display_processor, c->has_shared_texture);
	return true;
}

/*!
 * #918 D12-4 — create the output composite unit, LAZILY, on the first frame that
 * actually composites.
 *
 * WHERE THIS IS, AND WHY IT IS NOT IN STAGE A. `comp_d3d12_outcomp_create` runs
 * `D3DCompile` on the masked-composite shaders and builds a PSO per target
 * format. Stage A's explicit constraint is that it must not lengthen the session
 * warmup the hybrid-iGPU investigation measured, and D12-3 declined to create the
 * unit there for exactly that reason. That constraint has not changed, so the
 * create moved rather than being relaxed: it happens on the app's own render
 * thread, inside the first `xrEndFrame` that carries zones or Local2D, and costs
 * that one frame a shader compile. A session that never composites never pays it,
 * and a session that does pays it once and off the warmup path.
 *
 * Called from the DEPOSIT half rather than the consume half deliberately. The
 * deposit half is what stamps the slot's recipe, so a create failure there stamps
 * `composite=false` and the frame ships as projection-only; discovering it in the
 * consume half would mean a slot already stamped compositable that nothing can
 * composite, and the consume half would bail on it every frame afterwards.
 *
 * Never retried after a failure and never re-WARNed: the deposit half runs every
 * frame, and the logging law forbids a per-frame WARN outright.
 */
static bool
d3d12_ensure_outcomp(struct comp_d3d12_compositor *c)
{
	if (c->outcomp != nullptr) {
		return true;
	}
	if (c->outcomp_failed) {
		return false;
	}
	const uint64_t start_ns = os_monotonic_get_ns();
	if (comp_d3d12_outcomp_create(d3d12_out_device(c), &c->outcomp) != XRT_SUCCESS || c->outcomp == nullptr) {
		c->outcomp_failed = true;
		c->outcomp = nullptr;
		U_LOG_W(
		    "#918 D12-4: the output composite unit could not be created on the scanout device — 2D "
		    "content does not composite under the split for this session; the 3D weave is unaffected");
		return false;
	}
	U_LOG_W(
	    "#918 D12-4: output composite unit up on the %s device in %.1f ms (lazy — first compositing frame, off the "
	    "warmup path)",
	    c->split_active ? "SCANOUT" : "app", (double)(os_monotonic_get_ns() - start_ns) / 1.0e6);
	return true;
}

/*!
 * #918 D12-4 — drop the mask rasters that belong to whichever device the
 * composite last ran on.
 *
 * The implicit / binary-zone / feather rasters are created on the device the
 * COMPOSITE consumes them on, which the split moves. Retiring the split changes
 * that device, so these have to go with it — they are not merely stale, they are
 * resources of a device that is about to be released. The published wish borrows
 * one of them, so that borrow is dropped here too, as is the repaint's cached
 * mask.
 *
 * Deliberately NOT @ref d3d12_release_zone_state: that one also detaches the
 * app's active mask, which is session state a mid-session retire must not touch.
 */
static void
d3d12_release_out_device_masks(struct comp_d3d12_compositor *c)
{
	c->zone_publish_res = nullptr;
	c->repaint.mask_res = nullptr;
	// #918 D12-5: the authored mask's transport is bridge-scoped, and the bridge
	// goes with the placement. A retire re-resolves it on the app device, where
	// `mask->staged` is simply read directly.
	c->mask_plane_live = false;
	c->mask_plane_gen = 0;
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

//! Release everything Stage A built. Idempotent; leaves `split_active` alone
//! (the caller owns that flag and the ordering around it).
static void
d3d12_split_release_out(struct comp_d3d12_compositor *c)
{
	if (c->xbridge != nullptr) {
		comp_xbridge_quiesce(c->xbridge);
		comp_xbridge_destroy(&c->xbridge);
	}
	if (c->outcomp != nullptr) {
		comp_d3d12_outcomp_destroy(&c->outcomp);
	}
	if (c->out_dp_srv_heap != nullptr) {
		c->out_dp_srv_heap->Release();
		c->out_dp_srv_heap = nullptr;
	}
	if (c->out_cmd_list != nullptr) {
		c->out_cmd_list->Release();
		c->out_cmd_list = nullptr;
	}
	if (c->out_cmd_allocator != nullptr) {
		c->out_cmd_allocator->Release();
		c->out_cmd_allocator = nullptr;
	}
	if (c->out_fence_event != nullptr) {
		CloseHandle(c->out_fence_event);
		c->out_fence_event = nullptr;
	}
	if (c->out_fence != nullptr) {
		c->out_fence->Release();
		c->out_fence = nullptr;
	}
	if (c->out_factory != nullptr) {
		c->out_factory->Release();
		c->out_factory = nullptr;
	}
	if (c->out_queue != nullptr) {
		c->out_queue->Release();
		c->out_queue = nullptr;
	}
	if (c->out_dev != nullptr) {
		c->out_dev->Release();
		c->out_dev = nullptr;
	}
}

/*!
 * #918 D12-3 — retire the split for the rest of the session and put the weave
 * back on the app device.
 *
 * WHY THIS EXISTS RATHER THAN A PER-FRAME DEGRADE. A frame whose composite has an
 * input on the app device and a target on the scanout device has no honest
 * version to draw. The two dishonest options are both worse than this: presenting
 * the weave without its 2D regions is a silent visual regression, and
 * half-splitting the frame is the class of bug #918's whole recipe/slot machinery
 * exists to prevent.
 *
 * So the session goes back to the single-adapter path — target, display
 * processor, HUD staging, DP crop and the composite's own mask rasters all
 * rebuilt on the app device — and stays there. Once, guarded by `split_active`.
 *
 * **D12-4 NARROWED THIS AND D12-5 NARROWED IT AGAIN, neither deleted it.** D12-4
 * moved zones and Local2D out: their planes cross with the atlas, their masks
 * rebuild output-side, and their composite runs on the scanout adapter. D12-5
 * moved the last one out — the Tier-3 APP-AUTHORED mask, whose pixels the
 * application draws into a texture of its own on the render adapter, now crosses
 * as @ref COMP_XBRIDGE_PLANE_MASK. So the PRESENCE of an authored mask no longer
 * retires anything; only a mask whose plane this MACHINE could not allocate does,
 * and it keeps the same @ref COMP_SPLIT_REASON_AUTHORED_MASK token because the
 * token names what the session lost. A support case can still tell that apart
 * from the generic @ref COMP_SPLIT_REASON_LAYERS_UNSUPPORTED, which from a D3D12
 * session means an old build.
 *
 * THE SECOND TRIGGER is a display processor that DECLINES a weaver on the
 * scanout adapter (ADR-037 §3a). Same recovery, same reason it is a recovery
 * rather than a session failure — but note that it is the only trigger that
 * fires during session create rather than mid-frame.
 *
 * THE CPU WAITS BELOW ARE REACHED FROM THE FRAME PATH, AND THAT IS ALLOWED
 * HERE — AND ONLY HERE. `gpu_wait_idle_out` / `gpu_wait_idle_app` are bounded
 * CPU idle waits, and the mid-frame trigger reaches them from the application's
 * own render thread inside xrEndFrame, not from teardown. Three things make
 * that defensible in the IN-PROCESS compositor: the thread belongs to ONE
 * application (its own frame is the only thing that can be delayed), it happens
 * AT MOST ONCE per session (`split_active` latches false before the rebuild),
 * and no lock anyone else is queued behind is held across it — `c->mutex`
 * serialises this compositor's own repaint thread and nothing more.
 *
 * The D3D11 SERVICE must not copy this shape, and the difference is not a
 * matter of degree. Its frame thread holds `render_mutex`, which EVERY client
 * is queued behind, so the same two waits there would stall every app in the
 * workspace at once for as long as the slower adapter took to drain — the #925
 * wedge class exactly. If the service ever needs an equivalent retire it has to
 * be driven off the frame path entirely.
 *
 * Locking: the CALLER holds `c->mutex` — layer_commit already does, and the
 * create-time site takes it — so no repaint can be part-way through the old
 * placement. This function never takes it itself (`std::mutex` is not
 * recursive, and layer_commit's call would deadlock on the spot if it did).
 */
static void
d3d12_split_retire(struct comp_d3d12_compositor *c, const char *why, const char *short_reason)
{
	if (!c->split_active) {
		return;
	}

	U_LOG_W(
	    "#918 output-device split RETIRED (%s) — moving the weave back to the app adapter for the rest of "
	    "the session (#918 D12-5)",
	    why);

	/*
	 * #918 Phase 3 — CORRECT THE CANONICAL LINE. `weave placement:` was already
	 * emitted at session create, saying split=1, and it stopped being true one
	 * line ago. Support greps for that string and reads the LAST one, so a
	 * retire that left the old line standing alone would be a log that lies.
	 *
	 * Deliberately does NOT re-resolve the adapters: this can run on the frame
	 * path (layer_commit) under `c->mutex`, and a QueryDisplayConfig there is a
	 * cost with no new information — the adapters are the ones the create-time
	 * line already named. What changed is the REGIME, and that is what this says.
	 */
	c->split_off_reason = (short_reason != nullptr) ? short_reason : COMP_SPLIT_REASON_STAGE_A_FAILED;
	U_LOG_W(
	    "weave placement: CHANGED — weave/present move back to the RENDER adapter for the rest of this "
	    "session (split=0 reason=%s) (#918)",
	    c->split_off_reason);

	/*
	 * Quiesce BOTH scopes before anything is torn down — teardown is one of the
	 * only two places allowed to, and it does so as two INDEPENDENT waits, each
	 * on its own queue's fence. Out first: it is the consumer, and the app queue
	 * may still owe it a signal the bridge is waiting on.
	 */
	gpu_wait_idle_out(c);
	gpu_wait_idle_app(c);

	// The bridge goes first: its egress slots are what the DP is sampling.
	if (c->xbridge != nullptr) {
		comp_xbridge_quiesce(c->xbridge);
	}

	// (hwnd, device) bind key: DESTROY the weaver before creating its
	// replacement. One weaver per HWND — a create against a window that still
	// has one is refused, and the failure is silent weaving.
	if (c->zone_published && c->display_processor != nullptr) {
		xrt_display_processor_d3d12_clear_local_zone_mask(c->display_processor);
		c->zone_published = false;
	}
	xrt_display_processor_d3d12_destroy(&c->display_processor);

	if (c->target != nullptr) {
		comp_d3d12_target_destroy(&c->target);
	}

	// The DP crop and the HUD staging were allocated on the out device; drop
	// them so the app-device path re-creates them at their own device.
	if (c->dp_input_resource != nullptr) {
		c->dp_input_resource->Release();
		c->dp_input_resource = nullptr;
		c->dp_input_width = 0;
		c->dp_input_height = 0;
	}
	if (c->hud_texture != nullptr) {
		c->hud_texture->Release();
		c->hud_texture = nullptr;
	}
	if (c->hud_upload_buffer != nullptr) {
		c->hud_upload_buffer->Release();
		c->hud_upload_buffer = nullptr;
	}
	c->hud_initialized = false;

	/*
	 * #918 D12-4 — the composite's own device-scoped resources go with the
	 * placement. The mask rasters were built on the scanout device; the outcomp
	 * (released inside d3d12_split_release_out) was created there too. Dropping
	 * them here is not tidying: keeping a resource of a device that is about to be
	 * released and then sampling it from the app device is a use-after-free with a
	 * plausible-looking pointer.
	 */
	d3d12_release_out_device_masks(c);
	c->outcomp_failed = false;

	// Nothing above may run after this: every "which device?" accessor keys off
	// it, so the rebuild below must see the app placement.
	c->split_active = false;
	c->repaint.armed = false;
	c->repaint.atlas = nullptr;

	d3d12_split_release_out(c);

	if (d3d12_make_target(c, c->transparent_background) != XRT_SUCCESS) {
		U_LOG_E("#918: could not rebuild the D3D12 target on the app device after retiring the split");
		c->target = nullptr;
	}
	if (c->target != nullptr && c->display_refresh_rate > 1.0f) {
		comp_d3d12_target_set_display_period(c->target, (uint64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate));
	}
	if (!d3d12_make_dp(c)) {
		// End of the line: the app device is where the non-split path has
		// always created it, so there is nothing left to fall back to. ERROR
		// rather than WARN — a session with no display processor anywhere shows
		// unwoven content on a 3D panel, and that must not be inferred from the
		// absence of a line.
		U_LOG_E(
		    "#918: no display processor on the app device either, after retiring the split — this "
		    "session will not weave");
	}
}

/*!
 * Stage A. Resolve the scanout adapter and, if it differs from the app's, stand
 * up the runtime-owned output device plus the cross-adapter atlas bridge.
 *
 * Everything here is best-effort: on ANY failure we log the exact reason once
 * and fall through to the stock single-device path. Stage A must also stay
 * CHEAP — the session warmup already shows a brief black window (weaver
 * async-create plus the fullscreen swapchain resize) and the investigation's
 * explicit constraint is that the split must not lengthen it. Duration is
 * logged so a regression is visible rather than inferred.
 */
//! #1264: the reroute is live for this session (always-defined so panel-sizing
//! conditions read cleanly on builds without the arm).
static inline bool
d3d12_fill_arm_active(struct comp_d3d12_compositor *c)
{
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	return c->reroute.active;
#else
	(void)c;
	return false;
#endif
}

#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
/*!
 * #1264 heavy-d3d12: which arm does a SAME-ADAPTER engage run on? Default is
 * the d3d11 fill arm (the measured winner — 2.2–2.9 ms fires under real app
 * load where the d3d12 arm serializes at 8–9 ms; Intel preemption is
 * draw-granular). `DXR_SPLIT_D3D12_ROUTE=own` keeps the tier's own out-arm,
 * as the A/B control and for the cube-class record.
 */
/*!
 * PROTOTYPE (#1261 follow-up): route a HYBRID (cross-adapter) d3d12 session
 * onto the d3d11 fill arm too, instead of its own-legs D3D12 arm.
 *
 * The VK tier already does exactly this in BOTH topologies -- its partition
 * gate reads "the #918 split's d3d11 fill arm, hybrid OR same-adapter". The
 * d3d12 reroute was accepted same-adapter-only (#1264), so a hybrid d3d12
 * session falls through to the own-legs arm, whose partition record did not
 * pass -- which is why the #1257 divisor lever does not exist for Unity on a
 * hybrid box today.
 *
 * Nothing structural blocks it: the deposit ring is app-adapter-local in both
 * designs (D3D12 renders into NT-shared D3D11 textures on its OWN adapter),
 * and the cross-adapter hop belongs to the xbridge downstream -- the same
 * xbridge the VK tier already crosses on hybrid hardware.
 *
 * OPT-IN and default OFF: this is a bring-up flag, exactly as
 * DXR_SPLIT_SAME_ADAPTER_D3D12 was for Phase C. It does not become a default
 * without its own measured acceptance record.
 */
static bool
d3d12_reroute_hybrid_optin(void)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("DXR_SPLIT_D3D12_HYBRID_REROUTE");
		on = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return on == 1;
}

static bool
d3d12_reroute_route_d3d11(void)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("DXR_SPLIT_D3D12_ROUTE");
		on = (e != NULL && strcmp(e, "own") == 0) ? 0 : 1;
	}
	return on == 1;
}

/*!
 * #1264 heavy-d3d12 reroute Stage A: stand the d3d11 fill arm up for this
 * session — comp_vk_split's Stage A1 (out device, HWND target, D3D11 DP per
 * ADR-037 §3a), then the deposit ring, then wire the transport. Mirrors the VK
 * compositor's A1/A2 split. Best-effort: any failure leaves the session on its
 * stock path with the reason logged; nothing half-engages.
 */
static void
d3d12_reroute_stage_a(struct comp_d3d12_compositor *c,
                      struct xrt_device *xdev,
                      int32_t display_screen_left,
                      int32_t display_screen_top)
{
	const LUID app_luid = c->device->GetAdapterLuid();
	struct comp_vk_split_info si = {};
	si.xdev = xdev;
	si.hwnd = c->hwnd;
	si.dp_factory_d3d11 = c->dp_factory_d3d11;
	si.has_shared_texture = c->has_shared_texture;
	si.transparent_background = c->transparent_background;
	// The gate this bool exists for is Vulkan's (a timeline semaphore the
	// runtime cannot enable after the fact); D3D12 fences always interop.
	si.app_timeline_semaphores = true;
	si.render_packed_luid =
	    ((uint64_t)(uint32_t)app_luid.HighPart << 32) | (uint64_t)(uint32_t)app_luid.LowPart;
	si.display_screen_left = display_screen_left;
	si.display_screen_top = display_screen_top;
	si.preferred_width = c->settings.preferred.width;
	si.preferred_height = c->settings.preferred.height;

	const char *short_reason = NULL;
	if (comp_vk_split_stage_a(&si, &c->reroute.split, &short_reason) != XRT_SUCCESS ||
	    c->reroute.split == NULL) {
		c->split_off_reason = (short_reason != NULL) ? short_reason : COMP_SPLIT_REASON_STAGE_A_FAILED;
		U_LOG_W("#1264 reroute: d3d11 fill arm Stage A refused (%s) — stock path",
		        c->split_off_reason != NULL ? c->split_off_reason : "?");
		return;
	}

	// The deposit ring at the worst-case system atlas (ADR-010's sizing rule,
	// same numbers the own-legs arm bridges at).
	uint32_t sys_w = 0, sys_h = 0;
	if (xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(xdev->rendering_modes, xdev->rendering_mode_count, &sys_w, &sys_h);
	}
	if (sys_w == 0 || sys_h == 0) {
		sys_w = c->settings.preferred.width;
		sys_h = c->settings.preferred.height;
	}
	if (comp_d3d12_deposit_create(c->device, sys_w, sys_h, &c->reroute.dep) != XRT_SUCCESS ||
	    c->reroute.dep == NULL) {
		comp_vk_split_retire(&c->reroute.split, "deposit creation failed", COMP_SPLIT_REASON_STAGE_A_FAILED);
		c->split_off_reason = COMP_SPLIT_REASON_STAGE_A_FAILED;
		return;
	}

	struct comp_vk_deposit_handoff handoff = {};
	if (!comp_d3d12_deposit_get_handoff(c->reroute.dep, &handoff) ||
	    !comp_vk_split_wire_bridge(c->reroute.split, &handoff)) {
		comp_d3d12_deposit_destroy(&c->reroute.dep);
		comp_vk_split_retire(&c->reroute.split, "bridge wire failed", COMP_SPLIT_REASON_STAGE_A_FAILED);
		c->split_off_reason = COMP_SPLIT_REASON_STAGE_A_FAILED;
		return;
	}

	// The plane machinery's panel-sizing rule reads these (same source the
	// own-legs Stage A fills them from).
	if (xdev->hmd != NULL) {
		c->split_panel_w = xdev->hmd->screens[0].w_pixels;
		c->split_panel_h = xdev->hmd->screens[0].h_pixels;
	}

	// This tier's flatten family is RGBA (the atlas ring's, and the scratch
	// the plane copies promote from) — the arm's chains must match or the
	// bind refuses on the typeless-family check.
	comp_vk_split_set_plane_format(c->reroute.split, (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM);

	c->reroute.active = true;
	c->split_off_reason = NULL;
	U_LOG_W(
	    "#1264 heavy-d3d12 reroute ACTIVE: this session's fill/weave/present run on the d3d11 fill "
	    "arm (ADR-039 — one fill engine); the atlas renders into a D3D11 deposit ring opened on the "
	    "app's D3D12 device, %ux%u x%u. DXR_SPLIT_D3D12_ROUTE=own reverts to the tier's own arm.",
	    sys_w, sys_h, (unsigned)COMP_D3D12_DEPOSIT_RING);
}
/*!
 * Record a scratch -> deposit-plane copy on the app list. Both resources sit
 * in COMMON at rest (the scratch's deposit-half steady state; the imported
 * plane's cross-API decay state); the copy brackets its own states. Requires
 * identical extents — both are panel-sized by the widened alloc conditions.
 */
static void
d3d12_reroute_copy_to_plane(struct comp_d3d12_compositor *c, ID3D12Resource *scratch, ID3D12Resource *plane)
{
	D3D12_RESOURCE_BARRIER pre[2] = {};
	pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	pre[0].Transition.pResource = scratch;
	pre[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	pre[1] = pre[0];
	pre[1].Transition.pResource = plane;
	pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	c->cmd_list->ResourceBarrier(2, pre);

	c->cmd_list->CopyResource(plane, scratch);

	D3D12_RESOURCE_BARRIER post[2] = {};
	post[0] = pre[0];
	post[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	post[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	post[1] = pre[1];
	post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	post[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	c->cmd_list->ResourceBarrier(2, post);
}

/*!
 * #1264 — the reroute's LOCAL2D staging: the transport fork of the deposit
 * half. Everything semantic (the mask capture into out_mask_req, the Local2D
 * flatten into the panel-sized scratch, the guards) already ran exactly as the
 * own-legs split's; this copies the flatten into the deposit plane on content
 * change, rasters the captured mask on the d3d11 arm (whose immediate-context
 * device rasters inline, the shipped D3D11 leg's shape), and stamps the arm's
 * recipe. Mode and mask-kind vocabularies are numerically identical across the
 * legs (LERP/ALPHA_OVER/ZONES = 0/1/2; NONE/IMPLICIT/ZONE_BINARY/ZONE_FEATHER
 * = 0/1/2/3), so both pass through.
 *
 * @return false when this frame cannot composite on the route — the caller
 *         stamps no-composite, exactly like a failed own-legs deposit.
 */
// (startup jerk MITIGATION) Skip a zone-wish publish that would say the same thing.
//
// MEASURED: comp_vk_split_publish_zone_wish() -> the vendor plug-in's
// leia_dp_d3d11_publish_local_zone_mask() blocks 180-186 ms EVERY FRAME during the Unity
// warm-up, on the compositor thread, under c->mutex. That is the whole ~200 ms hold that
// locks out the repaint fill and freezes the panel; GPU idle 7-12%, CPU 1.6 of 22 cores.
//
// The avatar's wish is CONSTANT while it warms up (811x1421, 1x1 collapse), so publishing
// it once per frame re-asserts an unchanged mask at ~185 ms a go. This hashes what the
// publish would convey and skips the call when it is unchanged.
//
// The real fix belongs in the plug-in (async or rate-limited publish) -- a 184 ms
// synchronous call is a defect whatever the caller does. This bounds the damage.
//
// DXR_ZONE_WISH_DEDUPE=0 restores the publish-every-frame behaviour for A/B.
static bool
d3d12_zone_wish_dedupe_on(void)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("DXR_ZONE_WISH_DEDUPE");
		on = (e != NULL && e[0] == '0') ? 0 : 1;
	}
	return on == 1;
}

//! Cheap signature of everything the wish publish conveys.
static uint64_t
d3d12_zone_wish_sig(struct comp_d3d12_compositor *c, uint32_t region_w, uint32_t region_h)
{
	uint64_t h = 1469598103934665603ULL;
	const auto mix = [&h](uint64_t v) {
		h ^= v;
		h *= 1099511628211ULL;
	};
	mix((uint64_t)c->out_mask_req.kind);
	mix((uint64_t)c->out_mask_req.count);
	mix((uint64_t)c->out_mask_req.w);
	mix((uint64_t)c->out_mask_req.h);
	mix((uint64_t)region_w);
	mix((uint64_t)region_h);
	for (uint32_t i = 0; i < c->out_mask_req.count && i < XRT_MAX_LAYERS; i++) {
		const struct xrt_rect *r = &c->out_mask_req.rects[i];
		mix((uint64_t)(uint32_t)r->offset.w);
		mix((uint64_t)(uint32_t)r->offset.h);
		mix((uint64_t)(uint32_t)r->extent.w);
		mix((uint64_t)(uint32_t)r->extent.h);
	}
	return h;
}

static bool
d3d12_reroute_stage_local2d(struct comp_d3d12_compositor *c,
                            uint32_t region_w,
                            uint32_t region_h,
                            bool zones_frame,
                            bool bridged_authored,
                            bool have_explicit,
                            const struct u_canvas_rect *eff_canvas,
                            int32_t digest_proj_idx)
{
	d3d12_zone_step_begin();
	if (c->split_panel_w == 0 || c->split_panel_h == 0) {
		return false;
	}
	d3d12_zone_step("L2D:deposit_plane_ensure");
	if (!comp_d3d12_deposit_plane_ensure(c->reroute.dep, COMP_D3D12_DEPOSIT_PLANE_LOCAL2D, c->split_panel_w,
	                                     c->split_panel_h)) {
		return false;
	}
	struct comp_d3d12_deposit_plane pl = {};
	d3d12_zone_step("L2D:deposit_plane_get");
	if (!comp_d3d12_deposit_plane_get(c->reroute.dep, COMP_D3D12_DEPOSIT_PLANE_LOCAL2D, &pl)) {
		return false;
	}

	struct xrt_rect box = {};
	uint64_t hash = 0;
	d3d12_zone_step("L2D:local2d_digest");
	d3d12_local2d_digest(c, digest_proj_idx, /*over=*/true, region_w, region_h, &box, &hash);
	if (hash != c->reroute.l2d_copied_hash) {
		d3d12_zone_step("L2D:copy_to_plane");
		d3d12_reroute_copy_to_plane(c, c->local2d_scratch, (ID3D12Resource *)pl.resource12);
		c->reroute.l2d_copied_hash = hash;
	}

	// The captured mask request rasters on the ARM (out_mask_req kinds ==
	// COMP_VK_SPLIT_MASK_* numerically). A BRIDGED authored mask captured no
	// raster and gates the composite off its plane instead — the own-legs
	// capture block's exact exemption.
	bool mask_ok = false;
	if (c->out_mask_req.kind != D3D12_OUT_MASK_NONE && c->out_mask_req.count > 0) {
		d3d12_zone_step("L2D:split_raster_mask");
		mask_ok = comp_vk_split_raster_mask(c->reroute.split, c->out_mask_req.kind, c->out_mask_req.rects,
		                                    c->out_mask_req.feather, c->out_mask_req.count,
		                                    c->out_mask_req.w, c->out_mask_req.h);
	}
	if (!mask_ok && !bridged_authored) {
		return false;
	}

	// ADR-027/#801 — a ZONES frame's explicit wish is PUBLISH-only, never a
	// blend gate: the composite gates on the binary zone raster there. A
	// legacy (non-zones) authored mask IS the gate — the hard M-lerp.
	const bool mask_is_plane = bridged_authored && !zones_frame;
	const uint32_t mode = zones_frame ? COMP_D3D12_COMPOSITE_MODE_ZONES
	                     : mask_is_plane ? COMP_D3D12_COMPOSITE_MODE_LERP
	                                     : COMP_D3D12_COMPOSITE_MODE_ALPHA_OVER;
	const bool opaque = c->transparent_background && debug_get_bool_option_present_opaque_comp();
	const int32_t cx = eff_canvas->valid ? eff_canvas->x : 0;
	const int32_t cy = eff_canvas->valid ? eff_canvas->y : 0;
	const uint32_t cw = eff_canvas->valid ? eff_canvas->w : region_w;
	const uint32_t ch = eff_canvas->valid ? eff_canvas->h : region_h;

	d3d12_zone_step("L2D:split_stage_local2d");
	comp_vk_split_stage_local2d(c->reroute.split, pl.shared_handle, pl.generation, pl.width, pl.height, hash,
	                            box.offset.w, box.offset.h, (uint32_t)box.extent.w, (uint32_t)box.extent.h,
	                            region_w, region_h, cx, cy, cw, ch, mode, opaque, mask_is_plane);

	/*
	 * ADR-027 P4 — the hardware zone WISH on this route (the VK caller's
	 * exact pattern): a zones frame publishes the binary raster the arm just
	 * built; a non-zones frame clears.
	 */
	if (zones_frame) {
		// Dedupe: this call costs ~185 ms in the plug-in; do not pay it to repeat itself.
		const uint64_t wish_sig = d3d12_zone_wish_sig(c, region_w, region_h);
		if (d3d12_zone_wish_dedupe_on() && wish_sig == c->reroute.wish_sig_published &&
		    c->reroute.wish_seq != 0) {
			static uint64_t skipped = 0;
			skipped++;
			if (skipped == 1 || (skipped % 300) == 0) {
				U_LOG_W("[WISHDEDUPE] skipped %llu unchanged zone-wish publish(es)\n",
				        (unsigned long long)skipped);
			}
		} else {
		c->reroute.wish_sig_published = wish_sig;
		c->reroute.wish_seq++;
		d3d12_zone_step("L2D:publish_zone_wish");
		comp_vk_split_publish_zone_wish(c->reroute.split, c->reroute.wish_seq);
		}
	} else {
		d3d12_zone_step("L2D:clear_zone_wish");
		comp_vk_split_clear_zone_wish(c->reroute.split);
	}
	return true;
}
#endif // COMP_D3D12_HAVE_D3D11_FILL_ARM

static void
d3d12_split_stage_a(struct comp_d3d12_compositor *c,
                    struct xrt_device *xdev,
                    int32_t display_screen_left,
                    int32_t display_screen_top)
{
	const uint64_t stage_a_start_ns = os_monotonic_get_ns();
	double stage_a_bridge_ms = 0.0;
	HRESULT hr = S_OK;

	/*
	 * The gate's inputs are gathered in the ORIGINAL order and nothing is
	 * resolved past the first refusal — an ineligible session must not go near
	 * getScanoutAdapter. The DECISION itself is comp_split_gate's, shared with
	 * both D3D11 legs, so a fourth hand-rolled copy of "is this session eligible,
	 * do the adapters differ, which ingress" cannot drift from the other three.
	 */
	struct comp_split_gate_inputs gin = {};
	gin.requested = comp_split_gate_env_requested();
	if (!gin.requested) {
		// #918 Phase 3: the ONLY way to get here now. The placement line still
		// runs and names the kill switch.
		c->split_off_reason = COMP_SPLIT_REASON_KILLED_BY_ENV;
		return;
	}

	if (c->hwnd == nullptr) {
		gin.ineligible_reason = COMP_SPLIT_REASON_NO_HWND;
	} else if (c->has_shared_texture) {
		gin.ineligible_reason = COMP_SPLIT_REASON_SHARED_TEXTURE;
	} else if (xdev->hmd == NULL || xdev->hmd->screens[0].w_pixels == 0 || xdev->hmd->screens[0].h_pixels == 0) {
		gin.ineligible_reason = COMP_SPLIT_REASON_NO_PANEL_DIMS;
	}

	// The app device's adapter. D3D12 hands us the LUID directly — there is no
	// IDXGIDevice to walk, which is one fewer thing that can fail than the D3D11
	// leg has.
	LUID app_luid = {};
	if (gin.ineligible_reason == nullptr) {
		app_luid = c->device->GetAdapterLuid();
	}

	wil::com_ptr<IDXGIAdapter> scanout;
	DXGI_ADAPTER_DESC sdesc{};
	if (gin.ineligible_reason == nullptr) {
		scanout = xrt::auxiliary::d3d::getScanoutAdapter(display_screen_left, display_screen_top,
		                                                 xdev->hmd->screens[0].w_pixels,
		                                                 xdev->hmd->screens[0].h_pixels, U_LOGGING_INFO);
		gin.scanout_resolved = scanout && SUCCEEDED(scanout->GetDesc(&sdesc));
		gin.render_luid.low = app_luid.LowPart;
		gin.render_luid.high = app_luid.HighPart;
		gin.scanout_luid.low = sdesc.AdapterLuid.LowPart;
		gin.scanout_luid.high = sdesc.AdapterLuid.HighPart;
	}

	// ADR-039 ACCEPTED for this tier via the heavy-d3d12 reroute (#1264,
	// 2026-08-29: the full ladder — 8-9 ms own-arm fires to 1.4 ms through
	// the d3d11 arm at 60 flat, acceptance leg with events absorbed, planes
	// bound, and the eyeball incl. live resize — "ship it"). The tier now
	// consults the accepted default (on; DXR_SPLIT_SAME_ADAPTER=0 is the
	// shared kill switch); the DXR_SPLIT_SAME_ADAPTER_D3D12 bring-up env
	// retired with the acceptance, as its contract said it would. The
	// engage routes to the d3d11 fill arm by default
	// (DXR_SPLIT_D3D12_ROUTE=own keeps the own-legs arm — the A/B control
	// and the hybrid arm, whose partition record did NOT pass).
	gin.allow_same_adapter = comp_split_gate_env_same_adapter();

	struct comp_split_gate_result gate = {};
	comp_split_gate_evaluate(&gin, &gate);
	const char *reason = gate.reason;
	c->split_off_reason = gate.short_reason;
	// PROTOTYPE: the reroute is same-adapter by acceptance; the opt-in extends it
	// to the hybrid topology the VK tier already serves on this same arm.
	const bool reroute_topology_ok = gate.same_adapter || d3d12_reroute_hybrid_optin();
	if (reroute_topology_ok && gate.split_active) {
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
		if (d3d12_reroute_route_d3d11()) {
			// #1264 heavy-d3d12: the same-adapter engage runs on the d3d11
			// fill arm, not this tier's own out-device machinery — the
			// measured route for real app load. Nothing of the own-legs
			// Stage A below is created.
			U_LOG_W(
			    "#918 output-device split: ADR-039 %s ENGAGE via the d3d11 fill arm on "
			    "'%ls' LUID=%08lx:%08lx (#1264 heavy-d3d12 reroute%s; "
			    "DXR_SPLIT_SAME_ADAPTER=0 reverts, DXR_SPLIT_D3D12_ROUTE=own for the own-legs arm)",
			    gate.same_adapter ? "same-adapter" : "HYBRID (cross-adapter)", sdesc.Description,
			    (unsigned long)sdesc.AdapterLuid.HighPart, (unsigned long)sdesc.AdapterLuid.LowPart,
			    gate.same_adapter ? ", accepted default"
			                      : ", PROTOTYPE opt-in DXR_SPLIT_D3D12_HYBRID_REROUTE=1");
			d3d12_reroute_stage_a(c, xdev, display_screen_left, display_screen_top);
			return;
		}
#endif
		// ADR-039: same adapter, split ENGAGED anyway — the fill engine is
		// the point, not the copy. The "cross-adapter" heap below is simply
		// a shared heap when both LUIDs match.
		U_LOG_W(
		    "#918 output-device split: ADR-039 same-adapter ENGAGE on the OWN-LEGS arm on '%ls' "
		    "LUID=%08lx:%08lx (DXR_SPLIT_D3D12_ROUTE=own — the A/B control; its event record did "
		    "not pass the matrix, so the partition refuses on this arm)",
		    sdesc.Description, (unsigned long)sdesc.AdapterLuid.HighPart,
		    (unsigned long)sdesc.AdapterLuid.LowPart);
	} else if (gate.same_adapter) {
		// Not a failure: on a MUX'd / single-GPU box the weave is already local,
		// so the split has nothing to do.
		U_LOG_W(
		    "#918 output-device split: scanout adapter '%ls' LUID=%08lx:%08lx IS the app's adapter — "
		    "split is a no-op (#918)",
		    sdesc.Description, (unsigned long)sdesc.AdapterLuid.HighPart,
		    (unsigned long)sdesc.AdapterLuid.LowPart);
	}

	// --- the runtime-owned D3D12 device on the scanout adapter -------------
	if (reason == nullptr) {
		hr = D3D12CreateDevice(scanout.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
		                       reinterpret_cast<void **>(&c->out_dev));
		if (FAILED(hr) || c->out_dev == nullptr) {
			U_LOG_W("#918 output-device split: D3D12CreateDevice(scanout) failed 0x%08lx",
			        (unsigned long)hr);
			reason = "D3D12CreateDevice failed";
		}
	}
	if (reason == nullptr) {
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		/*
		 * #1264 Phase B: SAME-ADAPTER only, the out queue asks for
		 * scheduler-level priority. Measured (v2.14.11-4, Unity vs cube on
		 * one iGPU): with both direct queues at NORMAL the app's render
		 * work sits in front of every weave — fire= balloons 3.5-5x
		 * (7-10 ms vs the cube's 2 ms), which at 40 target fires/s is
		 * arithmetically unfillable; the app's own weaves suffer the same
		 * (13-16 of 20 granted). The contention pre-exists the split
		 * (stock in-process leg is statistically identical), so isolation
		 * is the fix class, and PRIORITY_HIGH is the documented,
		 * unprivileged rung (GLOBAL_REALTIME needs privilege; a compute-
		 * queue weave is the deeper fallback). Hybrid keeps NORMAL — its
		 * out queue owns a whole adapter and the accepted record was
		 * measured there at NORMAL. DXR_SPLIT_QUEUE_HIGH=0 forces NORMAL
		 * for A/B. Refusal falls back to NORMAL with one WARN.
		 */
		bool want_high = gate.same_adapter;
		{
			const char *e = getenv("DXR_SPLIT_QUEUE_HIGH");
			if (e != nullptr && e[0] == '0') {
				want_high = false;
			}
		}
		if (want_high) {
			qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
		}
		hr = c->out_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
		                                    reinterpret_cast<void **>(&c->out_queue));
		if (want_high && (FAILED(hr) || c->out_queue == nullptr)) {
			U_LOG_W(
			    "#1264: PRIORITY_HIGH out queue refused (0x%08lx) — falling back to NORMAL "
			    "(expect app-vs-fill queue contention on this adapter)",
			    (unsigned long)hr);
			qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
			hr = c->out_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
			                                    reinterpret_cast<void **>(&c->out_queue));
		} else if (want_high && SUCCEEDED(hr)) {
			U_LOG_W("#1264: same-adapter out queue at D3D12_COMMAND_QUEUE_PRIORITY_HIGH — "
			        "weave submissions preempt the app's NORMAL direct queue "
			        "(DXR_SPLIT_QUEUE_HIGH=0 reverts)");
		}
		if (SUCCEEDED(hr)) {
			hr = c->out_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			                                        __uuidof(ID3D12CommandAllocator),
			                                        reinterpret_cast<void **>(&c->out_cmd_allocator));
		}
		if (SUCCEEDED(hr)) {
			hr = c->out_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->out_cmd_allocator,
			                                   nullptr, __uuidof(ID3D12GraphicsCommandList),
			                                   reinterpret_cast<void **>(&c->out_cmd_list));
		}
		if (SUCCEEDED(hr)) {
			// #747: name the list. Under the split the debug layer sees two
			// devices' lists interleaved; without a name there is no way to tell
			// whose barrier is at fault.
			c->out_cmd_list->SetName(L"DXR.out_weave_cmd_list");
			c->out_cmd_list->Close();
			hr = c->out_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
			                             reinterpret_cast<void **>(&c->out_fence));
		}
		if (SUCCEEDED(hr)) {
			c->out_fence_value = 0;
			c->out_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (c->out_fence_event == nullptr) {
				hr = E_FAIL;
			}
		}
		if (FAILED(hr)) {
			U_LOG_W("#918 output-device split: out queue/allocator/list/fence failed 0x%08lx",
			        (unsigned long)hr);
			reason = "out device objects failed";
		}
	}
	if (reason == nullptr) {
		hr = scanout->GetParent(__uuidof(IDXGIFactory4), reinterpret_cast<void **>(&c->out_factory));
		if (FAILED(hr) || c->out_factory == nullptr) {
			U_LOG_W("#918 output-device split: scanout DXGI factory failed 0x%08lx", (unsigned long)hr);
			// Its own reason: the device was created fine, it is the adapter's
			// IDXGIFactory4 parent that could not be reached.
			reason = "scanout DXGI factory unavailable";
		}
	}
	if (reason == nullptr) {
		// The DP's atlas SRV heap (#854), on the device the DP will live on.
		D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
		heap_desc.NumDescriptors = 1;
		heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		hr = c->out_dev->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
		                                      reinterpret_cast<void **>(&c->out_dp_srv_heap));
		if (FAILED(hr)) {
			U_LOG_W("#918 output-device split: out DP SRV heap failed 0x%08lx", (unsigned long)hr);
			reason = "out DP SRV heap failed";
		}
	}

	// --- the bridge: the cross-adapter heap, the ring, every fence ---------
	if (reason == nullptr) {
		uint32_t sys_w = 0, sys_h = 0;
		if (xdev->rendering_mode_count > 0) {
			u_tiling_compute_system_atlas(xdev->rendering_modes, xdev->rendering_mode_count, &sys_w,
			                              &sys_h);
		}
		if (sys_w == 0 || sys_h == 0) {
			sys_w = c->settings.preferred.width;
			sys_h = c->settings.preferred.height;
		}

		struct comp_xbridge_info xbi = {};
		// #918 D12-3a: D3D12 ends. The producer IS the app's device and the
		// consumer IS the device created above, so nothing is NT-shared and both
		// GPU-side waits are direct queue waits.
		xbi.d3d12_ends = true;
		xbi.app_device = c->device;
		xbi.app_queue = c->command_queue;
		xbi.app_adapter = nullptr; // unused with D3D12 ends
		xbi.out_device = c->out_dev;
		xbi.out_queue = c->out_queue;
		xbi.out_adapter = nullptr;
		xbi.max_width = sys_w;
		xbi.max_height = sys_h;
		// #918 D12-4: the 2D planes are sized from the PANEL, once, and never
		// resized — that is what keeps them out of the R2 resize-churn path.
		xbi.panel_width = xdev->hmd->screens[0].w_pixels;
		xbi.panel_height = xdev->hmd->screens[0].h_pixels;

		const char *xb_reason = nullptr;
		const uint64_t xb_t0 = os_monotonic_get_ns();
		if (comp_split_gate_env_test_fail_stage_a()) {
			// DXR_TEST_SPLIT_FAIL_STAGEA=1 — exercise the "one WARN, stock path"
			// degrade without a machine that genuinely cannot allocate the heap.
			c->xbridge = nullptr;
			reason = "DXR_TEST_SPLIT_FAIL_STAGEA";
		} else if (comp_xbridge_create(&xbi, &c->xbridge, &xb_reason) != XRT_SUCCESS) {
			c->xbridge = nullptr;
			reason = (xb_reason != nullptr) ? xb_reason : "cross-adapter heap unsupported";
		} else {
			/*
			 * The egress ring is allocated HERE, not in stage B: the split must
			 * not be able to activate and THEN discover it has nothing to weave
			 * into. Size it at the ACTIVE MODE's nominal content box rather than
			 * the worst-case atlas — same fail-safe, but it avoids committing
			 * (and immediately freeing) a large iGPU allocation during the
			 * warmup we were asked not to lengthen.
			 */
			uint32_t eg_w = 0, eg_h = 0;
			uint32_t mi = xdev->hmd->active_rendering_mode_index;
			if (mi < xdev->rendering_mode_count) {
				const struct xrt_rendering_mode *m = &xdev->rendering_modes[mi];
				eg_w = m->tile_columns * m->view_width_pixels;
				eg_h = m->tile_rows * m->view_height_pixels;
			}
			bool eg_ok;
			if (eg_w > 0 && eg_h > 0) {
				comp_xbridge_set_content_size(c->xbridge, eg_w, eg_h, 0);
				uint32_t gw = 0, gh = 0;
				comp_xbridge_get_egress_dims(c->xbridge, &gw, &gh);
				eg_ok = (gw > 0 && gh > 0);
			} else {
				eg_ok = comp_xbridge_alloc_worstcase_egress(c->xbridge);
			}
			if (!eg_ok) {
				reason = "egress share failed";
			}
		}
		stage_a_bridge_ms = (double)(os_monotonic_get_ns() - xb_t0) / 1.0e6;
	}

	if (reason == nullptr) {
		c->split_active = true;
		c->split_off_reason = nullptr;
		c->out_luid.LowPart = gate.out_adapter_luid.low;
		c->out_luid.HighPart = gate.out_adapter_luid.high;
		c->split_panel_w = xdev->hmd->screens[0].w_pixels;
		c->split_panel_h = xdev->hmd->screens[0].h_pixels;
		c->split_diag_window_ns = os_monotonic_get_ns();
		U_LOG_W(
		    "#918 output-device split ACTIVE: weave/repaint/present move to '%ls' LUID=%08lx:%08lx "
		    "(app device on LUID=%08lx:%08lx); atlas crosses via a D3D12 cross-adapter heap once per "
		    "frame, D3D12 ends both sides (#918 D12-3)",
		    sdesc.Description, (unsigned long)sdesc.AdapterLuid.HighPart,
		    (unsigned long)sdesc.AdapterLuid.LowPart, (unsigned long)app_luid.HighPart,
		    (unsigned long)app_luid.LowPart);
	} else {
		d3d12_split_release_out(c);
		if (reason[0] != '\0') {
			// The gate's refusals already carry a canonical token; a Stage-A
			// failure carries prose, and `stage_a_failed` is its short form.
			if (gate.split_active) {
				c->split_off_reason = COMP_SPLIT_REASON_STAGE_A_FAILED;
			}
			U_LOG_W(
			    "#918 output-device split DISABLED (%s) — falling back to the stock single-device "
			    "path (#918)",
			    reason);
		}
	}

	// Warmup budget: both numbers logged so a regression is visible rather than
	// inferred — the bridge (the cross-adapter heap, the fences, the egress
	// ring) dominates.
	U_LOG_W("#918 output-device split: stage A took %.1f ms (bridge %.1f ms, active=%d)",
	        (double)(os_monotonic_get_ns() - stage_a_start_ns) / 1.0e6, stage_a_bridge_ms, (int)c->split_active);
}

/*!
 * #918 — ONE canonical weave-placement line per session, ALWAYS emitted, in
 * addition to (never instead of) the Stage-A detail lines above.
 *
 * The Stage-A lines exist only when Stage A ran, so without this a box that
 * killed the split, or one whose session was ineligible, produces a log
 * byte-identical to a single-adapter box that pays nothing — the same blind spot
 * #1000 closed for adapter SELECTION, now closed for weave PLACEMENT. The
 * formatting is aux_d3d's, so this is literally the same string the D3D11,
 * service, Vulkan and OpenGL paths emit. Costs one QueryDisplayConfig per call.
 *
 * Called TWICE in the worst case — once after Stage A, and again from
 * @ref d3d12_split_retire when a display processor declines the scanout adapter
 * (ADR-037 §3a) or a zones/Local2D/mask frame retires the split. That is
 * deliberate: the retire happens AFTER the first line was emitted, so without a
 * second line the log's only placement statement would be the one that stopped
 * being true. The LAST `weave placement:` line in a log is always the truth.
 */
static void
d3d12_log_weave_placement(struct comp_d3d12_compositor *c,
                          struct xrt_device *xdev,
                          int32_t display_screen_left,
                          int32_t display_screen_top)
{
	const LUID rl = c->device->GetAdapterLuid();
	uint64_t render_packed_luid = 0;
	memcpy(&render_packed_luid, &rl, sizeof(render_packed_luid));

	const uint32_t pw = (xdev->hmd != NULL) ? xdev->hmd->screens[0].w_pixels : 0;
	const uint32_t ph = (xdev->hmd != NULL) ? xdev->hmd->screens[0].h_pixels : 0;
	d3d_log_weave_placement(render_packed_luid, display_screen_left, display_screen_top, pw, ph, c->split_active,
	                        c->split_off_reason);
}

/*!
 * The split's periodic diagnostic window. Deliberately the SAME shape and the
 * same ten-second cadence the D3D11 service emits, so the measurement sessions
 * can read one parser across both legs. Emitted only when the split is up, so
 * an ordinary session's log is unchanged.
 */
static void
d3d12_split_render_diag(struct comp_d3d12_compositor *c)
{
	if (!c->split_active || c->xbridge == nullptr) {
		return;
	}
	const uint64_t now_ns = os_monotonic_get_ns();
	if (c->split_diag_window_ns != 0 && now_ns - c->split_diag_window_ns < 10ull * U_TIME_1S_IN_NS) {
		return;
	}
	c->split_diag_window_ns = now_ns;

	struct comp_xbridge_ingress_stats ing = {};
	comp_xbridge_take_ingress_stats(c->xbridge, &ing);
	const char *ing_name = ing.mode == COMP_XBRIDGE_INGRESS_ADAPTIVE ? "adaptive"
	                       : ing.mode == COMP_XBRIDGE_INGRESS_DIRECT ? "direct"
	                       : ing.mode == COMP_XBRIDGE_INGRESS_STAGED ? "staged"
	                                                                 : "none";
	const uint64_t xb_bytes = comp_xbridge_take_atlas_bytes(c->xbridge);
	const uint64_t no_slot = c->split_no_slot;
	const uint64_t out_crop = c->split_out_crop;
	c->split_no_slot = 0;
	c->split_out_crop = 0;

	U_LOG_W(
	    "[RENDER] split=%d xb_kb=%llu xb_degraded=%d no_slot=%llu out_crop=%llu ingress=%s "
	    "ing_direct=%llu ing_staged=%llu ing_rebind=%llu ing_churn=%llu ing_leak=%llu window_s=10",
	    (int)c->split_active, (unsigned long long)(xb_bytes / 1024u), (int)comp_xbridge_is_degraded(c->xbridge),
	    (unsigned long long)no_slot, (unsigned long long)out_crop, ing_name, (unsigned long long)ing.direct,
	    (unsigned long long)ing.staged, (unsigned long long)ing.rebind, (unsigned long long)ing.churn,
	    (unsigned long long)ing.leak);

	/*
	 * #918 D12-4 — one line per live PLANE, same shape and same field names as
	 * the D3D11 leg's, so one parser reads both. Emitted only for a plane that
	 * has actually transported something, so a projection-only session's log is
	 * byte-identical to what it was before the planes existed.
	 */
	for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
		uint64_t pb = 0, pc = 0, ps = 0;
		bool half = false;
		comp_xbridge_take_plane_stats(c->xbridge, p, &pb, &pc, &ps, &half);
		if (pc == 0 && ps == 0) {
			continue;
		}
		U_LOG_W("[RENDER] plane=%s kb=%llu copies=%llu skips=%llu half_rate=%d window_s=10",
		        comp_xbridge_plane_label(p), (unsigned long long)(pb / 1024u), (unsigned long long)pc,
		        (unsigned long long)ps, (int)half);
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
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	else if (c->reroute.active && c->reroute.split != NULL) {
		comp_vk_split_request_display_mode(c->reroute.split, true);
	}
#endif

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
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	else if (c->reroute.active && c->reroute.split != NULL) {
		comp_vk_split_request_display_mode(c->reroute.split, false);
	}
#endif

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
	// #1257 partition: panel period on purpose — see wait_frame's note on
	// the double-pacing failure.
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
		// #999: graceful exit request, not a lost session (see the d3d11 note).
		U_LOG_I("Window closed - requesting session exit");
		return XRT_ERROR_COMPOSITOR_WINDOW_CLOSED;
	}

	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->display_refresh_rate);

	// #1257 partition: block until the app's next slot BEFORE the lock —
	// the repaint loop keeps weaving the other slots underneath this
	// sleep. No-op unless DXR_APP_FRAME_DIVISOR >= 2. Supported tier =
	// the #1264 REROUTE only (the d3d11 fill arm, whose acceptance record
	// carried the tier) — keying on the reroute being ACTIVE couples this
	// gate to the accepted default by construction, the same structural
	// coupling the VK and d3d11 tiers ship. The own-legs arm (hybrid, or
	// DXR_SPLIT_D3D12_ROUTE=own) still refuses cleanly: its event record
	// (3-of-3 deep dips) never passed the matrix.
	u_app_partition_throttle(&c->repaint.partition, (uint64_t)period_ns, d3d12_fill_arm_active(c));

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
	// #1257 partition: deliberately still the PANEL period, never D x period
	// — the stretched period made well-behaved apps pace themselves on top
	// of the throttle (double pacing; measured 14/s on a 20/s schedule on
	// this tier). Pacing lives in the throttle alone; animation steps by
	// predictedDisplayTime deltas, which stride honestly.
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
	// The GPU is already idle here: layer_commit() ends every frame with a
	// scoped idle wait, so no additional GPU drain is needed.
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
	c->repaint.app_frame_begin_ns = os_monotonic_get_ns();
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

		// #918 D12-3: the HUD is copied INTO the weave target, so its staging
		// pair belongs to the weave target's device, not the app's.
		HRESULT hr = d3d12_out_device(c)->CreateCommittedResource(
		    &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		    __uuidof(ID3D12Resource), reinterpret_cast<void **>(&c->hud_texture));
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

		hr = d3d12_out_device(c)->CreateCommittedResource(
		    &upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		    __uuidof(ID3D12Resource), reinterpret_cast<void **>(&c->hud_upload_buffer));
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

		// #918 D12-3: the crop's destination is a DP input, so it belongs to the
		// device the DP lives on — the scanout device under the split. The
		// retire path releases it so a fallback re-creates it on the app device.
		HRESULT hr = d3d12_out_device(c)->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
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
	// #918 D12-3: the crop is OUTPUT-side work — it feeds the display processor,
	// so it records onto the weave list, which is the out device's under the split.
	ID3D12GraphicsCommandList *crop_list = d3d12_weave_list(c);
	crop_list->ResourceBarrier(1, &barrier);

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
	crop_list->ResourceBarrier(1, &atlas_rb);

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
	crop_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

	// Atlas COPY_SOURCE → COMMON: restore the entry state the callers assume.
	atlas_rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	atlas_rb.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	crop_list->ResourceBarrier(1, &atlas_rb);

	// Transition intermediate: COPY_DEST → COMMON (for DP)
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	crop_list->ResourceBarrier(1, &barrier);

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
// Caller must ensure the APP timeline is idle on entry (gpu_wait_idle_app has
// been called, or the existing layer_commit fence-waits before returning) —
// everything read back here is an app-device resource. On exit the
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
	gpu_wait_idle_app(c);

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
	// #918 D12-3: the back buffer belongs to the swapchain, which under the
	// split lives on the scanout device — so the readback resource, the list and
	// the queue that copies into it must all be that device's too. Off the split
	// every one of these resolves to the app device, unchanged.
	ID3D12Device *cap_dev = d3d12_out_device(c);
	ID3D12GraphicsCommandList *cap_list = d3d12_weave_list(c);
	ID3D12CommandAllocator *cap_alloc = c->split_active ? c->out_cmd_allocator : c->cmd_allocator;
	if (back_buffer == nullptr || cap_dev == nullptr || cap_list == nullptr) {
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
	if (FAILED(cap_dev->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rb_desc,
	                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	                                            IID_PPV_ARGS(&readback))) ||
	    readback == nullptr) {
		return false;
	}

	cap_alloc->Reset();
	cap_list->Reset(cap_alloc, nullptr);

	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = back_buffer;
	b.Transition.Subresource = 0;
	b.Transition.StateBefore = entry_state;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	cap_list->ResourceBarrier(1, &b);

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
	cap_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	cap_list->ResourceBarrier(1, &b);
	cap_list->Close();
	ID3D12CommandList *lists[] = {cap_list};
	d3d12_out_queue(c)->ExecuteCommandLists(1, lists);
	// Out scope: this drains the queue the copy above ran on. Diagnostic path,
	// never the frame path, so a CPU wait here is fine — and it is the OUT
	// queue's own wait, never an app wait covering out work (D12-2).
	gpu_wait_idle_out(c);
	d3d12_outcomp_note_execute(c);

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
	// #918 D12-3: the heap, the view and the list all belong to the device the
	// display processor lives on — the scanout device under the split.
	ID3D12DescriptorHeap *heap = d3d12_weave_srv_heap(c);
	if (heap == nullptr || dp_resource == nullptr) {
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
	d3d12_out_device(c)->CreateShaderResourceView(dp_resource, &srv_desc,
	                                              heap->GetCPUDescriptorHandleForHeapStart());

	// Every later pass in this cmd_list (renderer draw, zone composite,
	// Local2D flatten) binds its own heap before use, so this bind is scoped
	// to the DP call that follows.
	ID3D12DescriptorHeap *heaps[] = {heap};
	d3d12_weave_list(c)->SetDescriptorHeaps(1, heaps);
	return heap->GetGPUDescriptorHandleForHeapStart().ptr;
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
 * Entry contract: the WEAVE LIST (@ref d3d12_weave_list — the app's off the
 * split, the out device's under it) is open and freshly reset,
 * `c->repaint.atlas` is in PIXEL_SHADER_RESOURCE, and layer_accum holds a
 * COMPLETE frame's layers (the crop and the 2D-under flatten below are rebuilt
 * from it every weave).
 *
 * #918 D12-3 — under the split the atlas is NOT `c->repaint.atlas`. That
 * resource lives on the app's adapter and the display processor is on the
 * scanout adapter, so the weave reads an EGRESS SLOT instead: whichever slot the
 * bridge has landed, picked and generation-checked below. Everything downstream
 * of that substitution is the same code on both placements, which is what keeps
 * a repaint bit-identical in construction to the app frame it stands in for.
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

	ID3D12GraphicsCommandList *wl = d3d12_weave_list(c);

	/*
	 * #918 D12-3 — pick the egress slot FIRST, before a single command is
	 * recorded. Two invariants depend on that ordering:
	 *
	 *  F4: never present a cleared-unwoven buffer. If there is nothing to weave
	 *      the correct output is NO PRESENT — with FLIP_DISCARD the panel then
	 *      keeps the last good frame instead of flashing a cleared one. Deciding
	 *      after the back buffer had been bound and cleared would leave us
	 *      choosing between presenting black and abandoning a half-recorded list.
	 *
	 *  R1: a slot may only be woven under the generation that PRODUCED it. Across
	 *      a mode switch the newest landed slot can belong to the recipe the
	 *      display has just left, and weaving it is the interlaced-frame-under-a-
	 *      disabled-lens artifact. `pick_slot` refuses those; when it comes back
	 *      empty because a layout change just rebuilt the ring, the transition
	 *      pick takes the IN-FLIGHT slot of the NEW generation and orders the
	 *      weave behind it with a GPU-side wait — never a CPU one.
	 */
	ID3D12Resource *weave_atlas = c->repaint.atlas;
	uint32_t weave_content_w = c->repaint.content_w;
	uint32_t weave_content_h = c->repaint.content_h;
	uint32_t weave_view_w = c->repaint.view_w;
	uint32_t weave_view_h = c->repaint.view_h;
	uint32_t weave_cols = c->repaint.cols;
	uint32_t weave_rows = c->repaint.rows;

	// #918 D12-4: hoisted out of the block below — the composite and the DP
	// backdrop both need the slot the weave settled on, and both live past it.
	int32_t slot = -1;
	if (c->split_active) {
		const uint64_t want_gen = c->split_layout_gen;
		if (is_repaint) {
			// A repaint re-weaves exactly the slot the last app frame wove,
			// with zero bridge traffic — and must prove nothing is rewriting
			// it underneath (#918 F7).
			slot = comp_xbridge_get_weave_slot(c->xbridge);
			if (slot >= 0 && !comp_xbridge_slot_ready(c->xbridge, slot)) {
				slot = -1;
			}
		} else {
			slot = comp_xbridge_pick_slot(c->xbridge, want_gen);
			if (slot < 0) {
				slot = comp_xbridge_pick_inflight_slot(c->xbridge, want_gen);
			}
			if (slot >= 0) {
				comp_xbridge_set_weave_slot(c->xbridge, slot);
			}
		}

		uint64_t slot_gen = 0;
		uint32_t slot_cw = 0, slot_ch = 0;
		if (slot >= 0 && (!comp_xbridge_slot_layout(c->xbridge, slot, &slot_gen, &slot_cw, &slot_ch) ||
		                  slot_gen != want_gen || slot_cw == 0 || slot_ch == 0)) {
			slot = -1;
		}
		if (slot >= 0) {
			weave_atlas = static_cast<ID3D12Resource *>(comp_xbridge_get_egress_resource(c->xbridge, slot));
		} else {
			weave_atlas = nullptr;
		}

		if (weave_atlas == nullptr) {
			/*
			 * #918 F4. Close the list so the next Reset has a closed one, and
			 * return WITHOUT presenting. Counted, never logged per frame — the
			 * periodic [RENDER] line carries `no_slot`.
			 */
			c->split_no_slot++;
			wl->Close();
			if (out_back_buffer != nullptr) {
				*out_back_buffer = nullptr;
			}
			return XRT_SUCCESS;
		}

		// Order the weave behind the consumer copy that filled this slot. GPU
		// side, on the OUT queue — the weave thread never blocks.
		comp_xbridge_gpu_wait_slot(c->xbridge, slot);

		/*
		 * #1140 — the recipe travels with the pixels. The tile geometry the DP
		 * is handed comes from the LAYOUT GENERATION the slot was stamped with,
		 * never re-read from whatever the app has most recently submitted: the
		 * split's whole hazard is that the weave consumes a slot some EARLIER
		 * frame filled. `slot_gen == want_gen` was just proved above, and
		 * split_gen_* is the snapshot taken when that generation was minted, so
		 * these four numbers provably describe these pixels. The content box
		 * comes from the slot itself, because during a resize it is a frame
		 * behind the window and cropping to the current box would slice the
		 * tiles at the wrong stride.
		 */
		weave_view_w = c->split_gen_vw;
		weave_view_h = c->split_gen_vh;
		weave_cols = c->split_gen_cols;
		weave_rows = c->split_gen_rows;
		weave_content_w = slot_cw;
		weave_content_h = slot_ch;
	}

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
		U_LOG_W(
		    "D3D12 DP dims: back_buffer=%llux%u, viewport=%ux%u, "
		    "view=%ux%u, atlas=%ux%u (tile %ux%u) split=%d",
		    (unsigned long long)bb_desc.Width, (unsigned)bb_desc.Height, tgt_width, tgt_height, weave_view_w,
		    weave_view_h, weave_cols * weave_view_w, weave_rows * weave_view_h, weave_cols, weave_rows,
		    (int)c->split_active);
	}

	// Transition back buffer: PRESENT → RENDER_TARGET
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = back_buffer;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	wl->ResourceBarrier(1, &barrier);

	// Bind back buffer as render target
	wl->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

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
	//
	// #918 D12-3: under the split there is no atlas barrier to take. The egress
	// slot is an ALLOW_SIMULTANEOUS_ACCESS resource that is COMMON at every
	// submission boundary by construction (see comp_xbridge_get_egress_resource),
	// so it promotes implicitly for the DP read and decays back with no barrier
	// owed by anyone -- which is also the only reason the vendor plug-in being
	// free to leave states behind (#747) does not bite here.
	D3D12_RESOURCE_BARRIER atlas_barrier = {};
	atlas_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	atlas_barrier.Transition.pResource = weave_atlas;
	atlas_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	atlas_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	atlas_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	if (!c->split_active) {
		wl->ResourceBarrier(1, &atlas_barrier);
	}

	/*
	 * Crop before the DP is the law (ADR-030). Under the split the bridge
	 * usually sizes the egress ring AT the content box, so this early-outs; when
	 * the R2 resize hysteresis has it holding a worst-case ring it does not, and
	 * the crop runs HERE -- on the output device, out of the output list,
	 * exactly where the pixels are. Counted so a session paying it is visible.
	 */
	ID3D12Resource *dp_resource = d3d12_crop_atlas_for_dp(c, weave_atlas, weave_content_w, weave_content_h);
	if (c->split_active && dp_resource != weave_atlas) {
		c->split_out_crop++;
	}

	/*
	 * The 2D-under backdrop flatten samples the APP'S OWN Local2D swapchain
	 * images, exactly like the over-layer flatten in the composite — so a
	 * repaint must not re-run it either, and reuses what the last app frame
	 * produced. backdrop_scratch is compositor-owned and stable until the next
	 * layer_commit. (Invisible in the avatar scene, where the backdrop is NULL
	 * on every weave, but it is the same class of bug as the one that produced
	 * the 2D flicker — see the composite.)
	 */
	//
	// #918 D12-4: under the split the flatten does not run HERE — it reads the
	// app's own Local2D swapchain images and writes an app-device scratch, so it
	// belongs to the deposit half, where it now runs (see layer_commit). What the
	// display processor is handed here is the BACKDROP PLANE of the slot being
	// woven: the same pixels, on the device the DP actually lives on, and stamped
	// with the recipe that describes them.
	if (!is_repaint && !c->split_active) {
		uint32_t bd_w = 0, bd_h = 0;
		c->repaint.backdrop = d3d12_flatten_backdrop_2d(c, tgt_width, tgt_height, &bd_w, &bd_h);
		c->repaint.backdrop_w = bd_w;
		c->repaint.backdrop_h = bd_h;
	}
	if (c->split_active) {
		/*
		 * The backdrop's dims come from the SLOT's recipe, not from
		 * `c->repaint.backdrop_w/h`: the DP declares the backdrop's own extent
		 * separately from the composite region, and under the split the pixels
		 * being handed over belong to whichever frame filled this slot — which is
		 * not necessarily the one whose dims live in CPU state (#1140, the recipe
		 * travels with the pixels). A slot with no backdrop, or one whose plane is
		 * stale, clears the DP's background rather than showing an older frame's.
		 */
		ID3D12Resource *bd = nullptr;
		uint32_t bd_w = 0, bd_h = 0;
		struct comp_xbridge_recipe brec = {};
		if (slot >= 0 && comp_xbridge_slot_recipe(c->xbridge, slot, &brec) && brec.bd_w > 0 && brec.bd_h > 0 &&
		    (brec.plane_valid & (1u << COMP_XBRIDGE_PLANE_BACKDROP)) != 0) {
			bd = static_cast<ID3D12Resource *>(
			    comp_xbridge_get_plane_resource(c->xbridge, slot, COMP_XBRIDGE_PLANE_BACKDROP,
			                                    brec.plane_seq[COMP_XBRIDGE_PLANE_BACKDROP]));
			if (bd != nullptr) {
				bd_w = brec.bd_w;
				bd_h = brec.bd_h;
			}
		}
		xrt_display_processor_d3d12_set_background_2d(c->display_processor, bd, bd_w, bd_h);
	} else {
		xrt_display_processor_d3d12_set_background_2d(c->display_processor, c->repaint.backdrop,
		                                              c->repaint.backdrop_w, c->repaint.backdrop_h);
	}


	D3D12_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(tgt_width);
	viewport.Height = static_cast<float>(tgt_height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	wl->RSSetViewports(1, &viewport);

	D3D12_RECT scissor = {};
	scissor.left = 0;
	scissor.top = 0;
	scissor.right = static_cast<LONG>(tgt_width);
	scissor.bottom = static_cast<LONG>(tgt_height);
	wl->RSSetScissorRects(1, &scissor);

	// Late-weave pacing + weave-latency harness mark (env-gated no-ops
	// otherwise). A repaint already paced itself unlocked, and is deliberately
	// kept out of the governor's frame-interval EMA and out of the #867
	// prediction ledger — it has no app frame and no promised photon time
	// behind it.
	if (is_repaint) {
		comp_d3d12_target_weave_mark_repaint(c->target, c->hardware_display_3d);
	} else {
		comp_d3d12_target_weave_mark(c->target, c->last_display_time_ns, c->hardware_display_3d);
	}

	// Timing feedback: hand the DP last frame's MEASURED weave→scanout residual
	// so the vendor eye predictor runs with an exact horizon (0 = unknown ⟹ DP
	// heuristic).
	xrt_display_processor_d3d12_set_frame_timing(c->display_processor,
	                                             comp_d3d12_target_get_measured_weave_ns(c->target),
	                                             (uint64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate));

	// #206: forward-computed horizon for THIS weave, from the vsync-locked
	// vblank grid — exact per weave, no estimator lag under variable
	// cadence. 0 = no trusted grid ⟹ DP keeps the retrospective value.
	xrt_display_processor_d3d12_set_predicted_scanout(
	    c->display_processor, comp_d3d12_target_predict_weave_to_scanout_ns(c->target));

	// Pass actual backbuffer dimensions to the DP. Canvas offset and size are
	// passed separately — the DP uses them to set a viewport sub-rect for
	// correct interlacing phase.
	xrt_display_processor_d3d12_process_atlas(
	    c->display_processor, wl, dp_resource,
	    d3d12_bind_dp_atlas_srv(c, dp_resource), // #854: real SRV — sim DP binds it; SR weaver ignores it
	    rtv_handle.ptr, back_buffer, weave_view_w, weave_view_h, weave_cols, weave_rows,
	    static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM), tgt_width, tgt_height,
	    eff_canvas.valid ? eff_canvas.x : 0, eff_canvas.valid ? eff_canvas.y : 0,
	    eff_canvas.valid ? eff_canvas.w : 0, eff_canvas.valid ? eff_canvas.h : 0);

	// #439 / ADR-027: an authored zone mask or Local2D layers composite the
	// 2D/3D regions of the back buffer. Back buffer is still in RENDER_TARGET
	// from the DP; leave it in RENDER_TARGET so HUD's existing RT→COPY_DEST
	// transition (below) proceeds unchanged. No-op when this frame carries no
	// zones / Local2D / explicit mask.
	//
	// #918 D12-4: this now runs under the split too, and it is the rung. The
	// inputs it reads there are all output-device resources — the Local2D plane
	// the bridge landed beside this slot's atlas, a mask rastered on the out
	// device a few instructions earlier, and the composite unit's own weave
	// snapshot — and every parameter comes from the SLOT's recipe rather than
	// from live CPU state. `slot` is what carries both; -1 off the split, which
	// the callee reads as "not a bridged consume".
	d3d12_composite_zone_mask(c, /*reuse_mask=*/true, /*prepare_only=*/false, back_buffer, rtv_handle.ptr,
	                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, tgt_width,
	                          tgt_height, &eff_canvas, slot, is_repaint);


	// Transition atlas back: COMMON → PIXEL_SHADER_RESOURCE
	// (no-op under the split — see the entry barrier above.)
	atlas_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	atlas_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	if (!c->split_active) {
		wl->ResourceBarrier(1, &atlas_barrier);
	}

	// Transition back buffer for HUD overlay
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	wl->ResourceBarrier(1, &barrier);

	// HUD overlay
	d3d12_render_hud_overlay(c, wl, back_buffer, tgt_width, tgt_height, &c->repaint.eye_pos);

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
	wl->ResourceBarrier(1, &barrier);

	// Close and execute. #918 D12-3: on the OUT queue under the split — this
	// submission and the Present below it are the whole point of the rung.
	wl->Close();
	ID3D12CommandList *weave_lists[] = {wl};
	d3d12_out_queue(c)->ExecuteCommandLists(1, weave_lists);
	// #1151: an execution boundary — the outcomp SRV ring may safely wrap past
	// here. See d3d12_outcomp_note_record.
	d3d12_outcomp_note_execute(c);

	// Present with VSync
	xrt_result_t xret = comp_d3d12_target_present(c->target, 1);

	// Wait for frame completion (frame pacing)
	// Out scope: this wait paces the frame behind the Present above. Under
	// the split the weave list and the swapchain both live on the output
	// device, so the whole tail below the atlas is the out timeline's.
	gpu_wait_idle_out(c);

	// Only a real frame resets the quiet-gate; see d3d12_repaint_thread.
	if (!is_repaint) {
		c->repaint.last_app_frame_ns = os_monotonic_get_ns();
		u_repaint_gate_on_app_frame(&c->repaint.gate, c->repaint.last_app_frame_ns);
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
// (startup jerk) Report what happened DURING a long gap between submits.
//
// All four in-path sections measure <50 ms (pacing, weave, present, composite tail) and
// the GPU is idle (7-12%) while the present stream shows ~211 ms gaps -- so the time goes
// BETWEEN our calls. This says whether the repaint loop was even awake: ticks advancing
// with bail_armed climbing means it ran and refused; ticks flat means it never woke.
static void
d3d12_report_submit_gap(struct comp_d3d12_compositor *c, const char *who)
{
	static double thr_ms = -1.0;
	if (thr_ms < 0.0) {
		const char *e = getenv("DXR_SLOW_SECTION_MS");
		thr_ms = (e != NULL && e[0] != '\0') ? atof(e) : 50.0;
	}
	if (thr_ms <= 0.0) {
		return;
	}
	static uint64_t last_ns = 0;
	static uint64_t last_ticks = 0, last_armed = 0, last_gate = 0;
	const uint64_t now = os_monotonic_get_ns();
	if (last_ns != 0) {
		const double gap_ms = (double)(now - last_ns) / 1e6;
		if (gap_ms >= thr_ms) {
			U_LOG_W("[GAP] %.1f ms with no submit (next by %s) -- during it the repaint "
				        "loop: ticks+%llu bail_armed+%llu bail_gate+%llu\n",
				        gap_ms, who,
				        (unsigned long long)(c->repaint.ticks - last_ticks),
				        (unsigned long long)(c->repaint.bail_armed - last_armed),
				        (unsigned long long)(c->repaint.bail_gate - last_gate));
		}
	}
	last_ns = now;
	last_ticks = c->repaint.ticks;
	last_armed = c->repaint.bail_armed;
	last_gate = c->repaint.bail_gate;
}

static void
d3d12_repaint_thread(struct comp_d3d12_compositor *c)
{
#ifdef XRT_OS_WINDOWS
	// #1264 S2: real-time-media scheduling for the fill thread.
	u_fill_thread_join_mmcss("d3d12");
#endif

	while (!c->repaint_quit.load(std::memory_order_relaxed)) {
		const double hz = (c->display_refresh_rate > 1.0f) ? (double)c->display_refresh_rate : 60.0;
		const uint64_t period_ns = (uint64_t)(U_TIME_1S_IN_NS / hz);

		// Tick well inside a period so "the app went quiet" is noticed near the
		// refresh it matters for, rather than up to a full period late.
		// #1257 partition: with a known fill schedule the window segments
		// are only a few ms wide, so tick fine enough to land in them.
		// (startup jerk) The [GAP] reporter showed the loop ticking ONCE per ~200 ms gap
		// while the box is idle, with no bails -- i.e. the thread is not refusing, it is
		// not running. Measure the sleep itself: asked-vs-actual separates "os_nanosleep
		// overslept" (timer resolution) from "the thread was starved" (scheduling, e.g.
		// the MMCSS join above demoting it).
		const int64_t ask_ns =
		    (int64_t)((c->repaint.partition.next_release_ns != 0) ? period_ns / 12 : period_ns / 4);
		const uint64_t sleep_t0 = os_monotonic_get_ns();
		os_nanosleep(ask_ns);
		{
			static double thr_ms = -1.0;
			if (thr_ms < 0.0) {
				const char *e = getenv("DXR_SLOW_SECTION_MS");
				thr_ms = (e != NULL && e[0] != '\0') ? atof(e) : 50.0;
			}
			const double slept_ms = (double)(os_monotonic_get_ns() - sleep_t0) / 1e6;
			if (thr_ms > 0.0 && slept_ms >= thr_ms) {
				U_LOG_W("[SLEEP] repaint loop asked %.2f ms, slept %.1f ms\n",
				        (double)ask_ns / 1e6, slept_ms);
			}
		}

		if (c->repaint_quit.load(std::memory_order_relaxed)) {
			break;
		}

		c->repaint.ticks++;

		if (u_repaint_trace_enabled(&c->repaint.trace)) {
			const uint64_t tn = os_monotonic_get_ns();
			u_repaint_trace_tick(&c->repaint.trace, tn);
			u_repaint_trace_report(&c->repaint.trace, tn, "d3d12", &c->repaint.gate, period_ns,
			                       &c->repaint.partition);
		}

		// Cheap unlocked pre-check, re-tested under the lock below. Avoids
		// paying the pacing wait on every tick of an app that is making rate.
		if (!c->repaint.armed || c->repaint.app_frame_in_progress) {
			c->repaint.bail_armed++;
			u_repaint_trace_bail_armed(&c->repaint.trace);
			continue;
		}
		// Fire only once the app has ALREADY missed a full refresh — i.e. the
		// panel has shown this atlas for a whole period and is about to show it
		// again. Keyed on the last APP frame, never on the last repaint —
		// otherwise repaints would pace off their own timestamps and drift below
		// panel rate. Their cadence comes from the scanout wait instead.
		//
		// #1257: the fixed margin became cadence-aware. The old constant —
		// two periods, not one-and-a-bit — existed because an app whose
		// interval merely straddles a period (measured: the Unity avatar at
		// 46.7 fps on this 60 Hz panel, a 1.28-period interval) is about to
		// submit anyway: a repaint racing it steals the lock and the GPU to
		// fill a gap that was closing on its own. But the same constant made
		// panel rate unreachable for a present-capped app (hz20: the first
		// missed vblank of EVERY app frame is unrepaintable; hz30: interval
		// = exactly 2 periods, the gate never opens — measured repaints/s
		// 0.0). u_repaint_gate keeps the intent in vblank counts instead:
		// when the app provably presents every N vblanks, each app frame
		// gets a budget of N-1 repaints presented clear of the app's own
		// FIFO queue slot (the 46.7 fps case rounds to N=1 and therefore
		// still never repaints); without a trusted cadence it IS the legacy
		// 2-period constant.
		//
		// DXR_WEAVE_REPAINT_FORCE=1 bypasses the gate so the repaint path can be
		// exercised on hardware where no app is slow enough to trip it. It makes
		// the app SLOWER (it is meant to) — it is a correctness probe, never a
		// perf setting.
		if (c->repaint.force != 1 &&
		    !u_repaint_gate_open(&c->repaint.gate, os_monotonic_get_ns(), period_ns, &c->repaint.partition)) {
			c->repaint.bail_gate++;
			u_repaint_trace_bail_gate(&c->repaint.trace);
			continue;
		}

		// #1264 event-absorption shed (opt-in): an expensive fire opened a
		// shed window — skip this fill; app frames are untouched by
		// construction (this is the FILL loop).
		if (u_fill_shed_active(&c->repaint.shed, os_monotonic_get_ns())) {
			c->repaint.bail_gate++;
			u_repaint_trace_bail_gate(&c->repaint.trace);
			continue;
		}

		// Pace to the panel BEFORE taking the lock — this blocks for up to a
		// few periods, and holding the lock across it would stall an arriving
		// app frame for exactly that long.
		// #1257 partition: SKIPPED under the partition — this pacer is for
		// occasional repaints and its multi-period blocks were the measured
		// reason the d3d12 grid fill sat at ~8/s while VK's (whose pacer is
		// a no-op on Intel) reached 27-31/s. The grid IS the pacing.
		// #1264 reroute: no app-side target to pace against — the d3d11
		// arm's own flip chain paces its weave.
		if (c->repaint.partition.next_release_ns == 0 && c->target != nullptr) {
			const uint64_t pace_t0 = os_monotonic_get_ns();
			comp_d3d12_target_repaint_pace(c->target);
			u_repaint_trace_pace(&c->repaint.trace, pace_t0, os_monotonic_get_ns());
		}

		std::lock_guard<std::mutex> lock(c->mutex);

#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
		/*
		 * #1264 reroute: the fill IS the d3d11 arm's replay of the published
		 * egress slot — zero bridge traffic, no app-device command list, no
		 * DP or target on this side to test for. Same re-test discipline,
		 * then the arm's own weave.
		 */
		if (c->reroute.active) {
			if (c->repaint_quit.load(std::memory_order_relaxed) || !c->repaint.armed ||
			    c->repaint.app_frame_in_progress || c->reroute.split == NULL ||
			    !comp_vk_split_has_weave_slot(c->reroute.split)) {
				c->repaint.bail_armed++;
				u_repaint_trace_bail_armed(&c->repaint.trace);
				continue;
			}
			if (c->repaint.force != 1 &&
			    !u_repaint_gate_open(&c->repaint.gate, os_monotonic_get_ns(), period_ns,
			                         &c->repaint.partition)) {
				c->repaint.bail_race++;
				u_repaint_trace_bail_race(&c->repaint.trace);
				continue;
			}

			const uint64_t fire_t0 = os_monotonic_get_ns();
			d3d12_report_submit_gap(c, "repaint fill");
			comp_vk_split_weave_and_present(c->reroute.split, /*is_repaint=*/true,
			                                &c->reroute.canvas);
			c->repaint.count++;
			const uint64_t fire_t1 = os_monotonic_get_ns();
			u_repaint_gate_note_repaint(&c->repaint.gate, fire_t1);
			u_repaint_trace_fire(&c->repaint.trace, fire_t0, fire_t1);
			u_fill_shed_note_fire(&c->repaint.shed, fire_t0, fire_t1, period_ns);
			static bool rr_logged = false;
			if (!rr_logged) {
				rr_logged = true;
				U_LOG_W("#1264 reroute: filling via the d3d11 arm at %.1f Hz while the app is "
				        "between frames",
				        hz);
			}
			continue;
		}
#endif

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
			u_repaint_trace_bail_armed(&c->repaint.trace);
			continue;
		}
		// Re-run the gate under the lock (was a bare `quiet < period` floor;
		// the #1257 adaptive window opens at half a period).
		if (c->repaint.force != 1 &&
		    !u_repaint_gate_open(&c->repaint.gate, os_monotonic_get_ns(), period_ns, &c->repaint.partition)) {
			c->repaint.bail_race++;
			u_repaint_trace_bail_race(&c->repaint.trace);
			continue;
		}

		// #918 D12-3: the repaint replays the weave, so it resets the WEAVE
		// list — the out device's under the split. This is the arm the split
		// exists for: a repaint re-weaves a slot already on the scanout adapter
		// and costs no cross-adapter traffic at all.
		const uint64_t fire_t0 = os_monotonic_get_ns();
		if (c->split_active) {
			c->out_cmd_allocator->Reset();
			c->out_cmd_list->Reset(c->out_cmd_allocator, nullptr);
		} else {
			c->cmd_allocator->Reset();
			c->cmd_list->Reset(c->cmd_allocator, nullptr);
		}

		d3d12_dp_weave_and_present(c, true, nullptr);

		c->repaint.count++;
		const uint64_t fire_t1 = os_monotonic_get_ns();
		u_repaint_gate_note_repaint(&c->repaint.gate, fire_t1);
		u_repaint_trace_fire(&c->repaint.trace, fire_t0, fire_t1);
		u_fill_shed_note_fire(&c->repaint.shed, fire_t0, fire_t1, period_ns);
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

// (startup jerk) PROTOTYPE, default OFF: DXR_FILL_DURING_APP_WAIT=1.
//
// layer_commit holds c->mutex for the whole frame and waits INFINITE on the app GPU
// timeline inside it (gpu_wait_idle_on -> WaitForSingleObject(event, INFINITE)). Measured
// during the Unity warm-up: the mutex is held ~200 ms, thirteen times, and the repaint
// fill wakes, bumps its tick, then blocks on that mutex for all of it -- so the panel
// gets NOTHING for 200 ms while the GPU sits at 7-12%. app_frame_in_progress is already
// cleared by this point, so the fill is otherwise eligible to run.
//
// Releasing the lock across a pure fence wait lets the fill present in the hole. It is
// GATED because it also lets the fill present CONCURRENTLY with this frame, and the
// serialisation of the two present paths is exactly what this lock was providing --
// that has to be proven on hardware, not assumed.
static bool
d3d12_fill_during_app_wait(void)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("DXR_FILL_DURING_APP_WAIT");
		on = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return on == 1;
}

static xrt_result_t
d3d12_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::unique_lock<std::mutex> lock(c->mutex);
	// (startup jerk) This lock_guard is FUNCTION-SCOPED: c->mutex is held across the
	// whole frame -- deposit, submit to the arm, weave, present, gpu_wait_idle_app. The
	// repaint thread increments its tick, then blocks here, which is exactly the
	// signature measured: [GAP] ~200 ms closed by an app frame with ticks+1 and no
	// bails, while every individual section times under 80 ms and the GPU is idle.
	// So: how long is it actually held?
	const uint64_t lock_t0 = os_monotonic_get_ns();
	struct lock_hold_report
	{
		uint64_t t0;
		const char *names[24];
		uint64_t stamps[24];
		int n;
		void mark(const char *nm)
		{
			if (n < 24) {
				names[n] = nm;
				stamps[n] = os_monotonic_get_ns();
				n++;
			}
		}
		~lock_hold_report()
		{
			static double thr_ms = -1.0;
			if (thr_ms < 0.0) {
				const char *e = getenv("DXR_SLOW_SECTION_MS");
				thr_ms = (e != NULL && e[0] != '\0') ? atof(e) : 50.0;
			}
			const uint64_t now = os_monotonic_get_ns();
			const double held = (double)(now - t0) / 1e6;
			if (thr_ms <= 0.0 || held < thr_ms) {
				return;
			}
			// Which span inside the critical section owns the hold?
			const char *worst_from = "lock";
			const char *worst_to = "return";
			double worst = 0.0;
			uint64_t prev = t0;
			const char *prev_nm = "lock";
			for (int k = 0; k < n; k++) {
				const double d = (double)(stamps[k] - prev) / 1e6;
				if (d > worst) {
					worst = d;
					worst_from = prev_nm;
					worst_to = names[k];
				}
				prev = stamps[k];
				prev_nm = names[k];
			}
			{
				const double d = (double)(now - prev) / 1e6;
				if (d > worst) {
					worst = d;
					worst_from = prev_nm;
					worst_to = "return";
				}
			}
			U_LOG_W("[LOCK] layer_commit held c->mutex %.1f ms; worst span %.1f ms "
				        "between [%s] and [%s] (%d marks)\n",
				        held, worst, worst_from, worst_to, n);
		}
	} _lock_hold{lock_t0, {}, {}, 0};


	// #868: the submission window closes here. Cleared at the TOP rather than
	// on the way out so it cannot be leaked by one of this function's several
	// early returns — the lock is held throughout, so no repaint can interleave
	// with layer_commit itself regardless.
	{
		// How long were repaints locked out by this app frame?
		static double thr_ms = -1.0;
		if (thr_ms < 0.0) {
			const char *e = getenv("DXR_SLOW_SECTION_MS");
			thr_ms = (e != nullptr && e[0] != '\0') ? atof(e) : 50.0;
		}
		if (thr_ms > 0.0 && c->repaint.app_frame_begin_ns != 0) {
			const double held_ms =
				    (double)(os_monotonic_get_ns() - c->repaint.app_frame_begin_ns) / 1e6;
			if (held_ms >= thr_ms) {
				U_LOG_W("[SLOWSEC] layer_begin->layer_commit held %.1f ms "
				        "-- repaints were locked out for all of it\n", held_ms);
			}
		}
	}
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
	/*
	 * #918 D12-5 — THE AUTHORED MASK'S TRANSPORT, and the last fallback trigger.
	 *
	 * The scan above is the frame's one coherent answer to "does this frame
	 * composite?". D12-3 retired the split for ANY compositing frame, because the
	 * D3D12-ends bridge was projection-only. D12-4 transports the Local2D and
	 * backdrop planes and rebuilds the auto/implicit/feather masks output-side, so
	 * zones and Local2D no longer belong here — they composite on the scanout
	 * adapter, which is the whole point of the rung and what the shipping Unity
	 * avatar sample (zones + a Local2D band, auto wish) needs to see any benefit
	 * from the split at all.
	 *
	 * **D12-5 CLOSED THE LAST ONE.** The Tier-3 APP-AUTHORED mask — pixels the
	 * application draws into a texture of its own on the RENDER adapter — now
	 * crosses as COMP_XBRIDGE_PLANE_MASK, so "this frame has an authored mask" is
	 * no longer a reason to retire anything. What remains is the honest residue:
	 * a mask whose plane the machine could not ALLOCATE (an R8 cross-adapter heap
	 * the stack refuses, or an egress the driver will not share). That keeps the
	 * SAME token, because the token names the thing the session lost, not the
	 * line of ours that noticed — and there is no half-measure available: the
	 * D3D12 leg has no output-device shadow to fall back on the way the D3D11 one
	 * does, so the choice is the app device or wrong pixels.
	 *
	 * THE BIND IS MADE HERE, before any of this frame's work is recorded and under
	 * c->mutex, precisely because its failure branch is this retire — which
	 * quiesces both devices and rebuilds the target, and so cannot be taken from
	 * the middle of a recorded frame. Binding records nothing; the frame's staging
	 * happens later, in the deposit half (@ref d3d12_stage_mask_plane).
	 */
	struct comp_d3d12_zone_mask *const authored_mask = d3d12_frame_authored_mask(c);
	// #1264 reroute: the bind forks inside — a reroute bind failure degrades
	// the mask feature (returns true), never the session, so this retire is
	// own-legs-only by construction.
	_lock_hold.mark("before bind mask plane");
	if ((c->split_active || d3d12_fill_arm_active(c)) && !d3d12_bind_mask_plane(c, authored_mask)) {
		d3d12_split_retire(c, "the app-authored zone mask could not be transported to the scanout adapter",
		                   COMP_SPLIT_REASON_AUTHORED_MASK);
	}

	if (c->zones_frame && !c->zones_mode_requested && !d3d12_zone_dp_supported_weaving_arm(c)) {
		c->zones_mode_requested = true;
		comp_d3d12_compositor_request_display_mode(&c->base.base, true);
	} else if (!c->zones_frame) {
		c->zones_mode_requested = false;
	}

	// Reset this frame's resolved wish source — d3d12_note_auto_wish_publish
	// sets it in zones frames, from d3d12_update_zone_wish_state off the split
	// and from the consume half's out-device raster under it (#1175). A stale
	// pointer from an earlier frame must never publish. (zone_publish_w/h persist
	// as the previous raster's dims for the auto-wish seq dirty-check.)
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
	} else if (d3d12_fill_arm_active(c) && c->hwnd != nullptr) {
		/*
		 * #1264 reroute: there is no app-side target, so the WINDOW itself
		 * is the authority — and it moves: 3DLuma launches at its args size
		 * and Screen.SetResolution()s to its authored size within seconds,
		 * and users drag-resize. Falling back to settings.preferred here
		 * froze the weave target at the launch size and the composite
		 * clipped to the intersection (the rung-4 "bottom of the zone is
		 * cut" finding — the bottom 448 px of an 840x1448 window never
		 * composited). The arm's own chain follows via the per-frame
		 * comp_vk_split_resize_target below, which early-outs on same dims.
		 */
		RECT rr;
		if (GetClientRect(c->hwnd, &rr) && rr.right > 0 && rr.bottom > 0) {
			tgt_width = (uint32_t)rr.right;
			tgt_height = (uint32_t)rr.bottom;
		}
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

	/*
	 * Zero-copy check: can we pass the app's swapchain directly to the DP?
	 *
	 * #918 D12-3 — FORCED OFF under the split, for v1. Zero-copy hands the DP
	 * the APP's swapchain image, and under the split the DP is on the other
	 * adapter: there is no zero-copy to have, only a cross-adapter read the
	 * transport exists to replace. `u_tiling_can_zero_copy()` remains the sole
	 * eligibility gate (ADR-030) — this is not a second gate but a placement
	 * fact, so it is applied to the RESULT rather than folded into the test.
	 * Unreal's zero-copy measurement gates any later change.
	 */
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
						// #1264 reroute: zero-copy hands the DP the APP's
						// swapchain image; under the reroute the atlas MUST
						// be the deposit slot the d3d11 arm ingests — a
						// placement fact applied to the result, same as the
						// split's (ADR-030's gate stays the sole eligibility
						// test).
						if (!c->split_active && !c->reroute.active &&
						    u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs_arr, sw, sh, mode)) {
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
		/*
		 * #918 F6: the producer's copy of frame N-1 reads the app's atlas IN
		 * PLACE (ingress Option I), so the renderer passes of frame N must not
		 * start overwriting it until that copy has retired. GPU-side wait on the
		 * app's own queue — never a CPU one, and it can never cover out-queue
		 * work: what it waits on is the producer COPY queue, on this same
		 * adapter, which itself waits on nothing but this queue (D12-2).
		 */
		if (c->split_active) {
			comp_xbridge_pre_render(c->xbridge);
		}

#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
		if (c->reroute.active && c->reroute.dep != NULL) {
			/*
			 * #1264 reroute — this frame's atlas IS a deposit slot. Ring
			 * advance takes the GPU-side back-pressure wait on the app
			 * queue (the d3d11 arm's release of the slot), and the renderer
			 * is re-pointed at the slot's imported resource before any draw
			 * records. A content growth past the ring (a mode change) grows
			 * the ring first, under an app-queue idle — rare by
			 * construction (the ring is worst-case-sized, ADR-010).
			 */
			uint32_t dep_w = 0, dep_h = 0;
			comp_d3d12_deposit_get_dims(c->reroute.dep, &dep_w, &dep_h);
			const uint32_t need_w = c->eff_layout.cols * c->eff_layout.tile_w;
			const uint32_t need_h = c->eff_layout.rows * c->eff_layout.tile_h;
			if (need_w > dep_w || need_h > dep_h) {
				if (d3d12_fill_during_app_wait()) {
					// let the fill present in this hole; re-acquire after the fence
					lock.unlock();
					_lock_hold.mark("before gpu_wait_idle_app");
					gpu_wait_idle_app(c);
					lock.lock();
				} else {
					_lock_hold.mark("before gpu_wait_idle_app");
					gpu_wait_idle_app(c);
				}
				comp_d3d12_deposit_resize(c->reroute.dep, need_w > dep_w ? need_w : dep_w,
				                          need_h > dep_h ? need_h : dep_h);
				comp_d3d12_deposit_get_dims(c->reroute.dep, &dep_w, &dep_h);
			}
			comp_d3d12_deposit_advance(c->reroute.dep, c->command_queue);
			void *slot_res = comp_d3d12_deposit_current_resource(c->reroute.dep);
			if (slot_res == NULL ||
			    comp_d3d12_renderer_set_external_atlas(c->renderer, slot_res, dep_w, dep_h) !=
			        XRT_SUCCESS) {
				static bool bind_warned = false;
				if (!bind_warned) {
					bind_warned = true;
					U_LOG_W("#1264 reroute: external atlas bind failed — this frame renders "
					        "into the owned atlas and the arm weaves a stale slot");
				}
			}
		}
#endif
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

			_lock_hold.mark("before cmdlist Close");
			c->cmd_list->Close();
			ID3D12CommandList *flush_lists[] = {c->cmd_list};
			_lock_hold.mark("before ExecuteCommandLists");
			c->command_queue->ExecuteCommandLists(1, flush_lists);
			if (d3d12_fill_during_app_wait()) {
				// let the fill present in this hole; re-acquire after the fence
				lock.unlock();
				_lock_hold.mark("before gpu_wait_idle_app");
				gpu_wait_idle_app(c);
				lock.lock();
			} else {
				_lock_hold.mark("before gpu_wait_idle_app");
				gpu_wait_idle_app(c);
			}

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

#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	/*
	 * #1264 reroute — THE frame exit. Execute the atlas work on the app queue,
	 * signal the deposit fence past it, and hand the slot to the d3d11 fill
	 * arm; nothing below (this tier's weave, crop, HUD, present) runs — the
	 * arm owns all of it, exactly as it does for the VK tier. The CPU
	 * idle-wait mirrors the VK path's post-submit wait (#837's, to remove on
	 * both together): it is what lets the arm's ingress `Wait` always find
	 * the atlas complete.
	 */
	if (c->reroute.active && c->reroute.split != NULL && c->reroute.dep != NULL) {
		// Follow the window FIRST — the weave target, the composite region
		// and the canvas below all derive from this frame's dims, and the
		// call early-outs on same size (steady state costs a compare).
		(void)comp_vk_split_resize_target(c->reroute.split, tgt_width, tgt_height);

		/*
		 * #1264 plane transport — the same deposit half the own-legs split
		 * runs, with the transport forked to the arm inside it. Recorded onto
		 * the still-open app list so the flattens and the scratch->plane
		 * copies ride this frame's execute + fence signal, and one fence
		 * value covers planes AND atlas (the VK tier's exact pattern).
		 */
		{
			// The 2D-UNDER backdrop, first — its extent rides the recipe.
			uint32_t bd_w = 0, bd_h = 0;
			ID3D12Resource *bd = d3d12_flatten_backdrop_2d(c, tgt_width, tgt_height, &bd_w, &bd_h);
			c->repaint.backdrop = bd;
			c->repaint.backdrop_w = bd_w;
			c->repaint.backdrop_h = bd_h;
			bool bd_staged = false;
			if (bd != nullptr && bd_w > 0 && bd_h > 0 && c->split_panel_w > 0 &&
			    comp_d3d12_deposit_plane_ensure(c->reroute.dep, COMP_D3D12_DEPOSIT_PLANE_BACKDROP,
			                                    c->split_panel_w, c->split_panel_h)) {
				struct comp_d3d12_deposit_plane bp = {};
				if (comp_d3d12_deposit_plane_get(c->reroute.dep, COMP_D3D12_DEPOSIT_PLANE_BACKDROP,
				                                 &bp)) {
					// The flatten leaves the scratch in PSR for a DP that,
					// on this route, lives on the arm and never samples it —
					// COMMON is where the copy promotes from.
					D3D12_RESOURCE_BARRIER to_common = {};
					to_common.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					to_common.Transition.pResource = bd;
					to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
					to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
					to_common.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					c->cmd_list->ResourceBarrier(1, &to_common);

					int32_t bproj = -1;
					for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
						enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
						if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
							bproj = (int32_t)i;
							break;
						}
					}
					struct xrt_rect bd_box = {};
					uint64_t bd_hash = 0;
					d3d12_local2d_digest(c, bproj, /*over=*/false, bd_w, bd_h, &bd_box,
					                     &bd_hash);
					if (bd_hash != c->reroute.bd_copied_hash) {
						d3d12_reroute_copy_to_plane(c, bd,
						                            (ID3D12Resource *)bp.resource12);
						c->reroute.bd_copied_hash = bd_hash;
					}
					comp_vk_split_stage_backdrop(c->reroute.split, bp.shared_handle,
					                             bp.generation, bp.width, bp.height, bd_hash,
					                             bd_box.offset.w, bd_box.offset.h,
					                             (uint32_t)bd_box.extent.w,
					                             (uint32_t)bd_box.extent.h, bd_w, bd_h);
					bd_staged = true;
				}
			}
			if (!bd_staged) {
				// The NULL drop — un-stages the plane so a slot can never
				// claim a backdrop it has no pixels for.
				comp_vk_split_stage_backdrop(c->reroute.split, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
				c->repaint.backdrop_w = 0;
				c->repaint.backdrop_h = 0;
			}

			// The MASK plane, staged before the composite decision and
			// independently of it (#918 F1/D6 — a Tier-3 mask must be able
			// to bootstrap). Fork inside stages the arm.
			d3d12_stage_mask_plane(c, authored_mask);

			// The Local2D/mask deposit half (fork inside stages the arm).
			_lock_hold.mark("before composite zone_mask");
			const bool deposited = d3d12_composite_zone_mask(
			    c, /*reuse_mask=*/false, /*prepare_only=*/true, nullptr, 0,
			    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, tgt_width,
			    tgt_height, &eff_canvas, /*slot=*/-1, /*is_repaint=*/false);
			_lock_hold.mark("AFTER composite zone_mask call");
			if (!deposited) {
				comp_vk_split_stage_no_composite(c->reroute.split);
			}
		}

		// Plane back-pressure: nothing in this frame's execute may rewrite a
		// plane the arm's producer is still reading. GPU-side, usually
		// already satisfied.
		_lock_hold.mark("before deposit plane_wait");
		comp_d3d12_deposit_plane_wait(c->reroute.dep, c->command_queue);

		_lock_hold.mark("before cmdlist Close");
		c->cmd_list->Close();
		ID3D12CommandList *rr_lists[] = {c->cmd_list};
		_lock_hold.mark("before ExecuteCommandLists");
		c->command_queue->ExecuteCommandLists(1, rr_lists);
		_lock_hold.mark("before deposit signal");
		comp_d3d12_deposit_signal(c->reroute.dep, c->command_queue);
		if (d3d12_fill_during_app_wait()) {
			// let the fill present in this hole; re-acquire after the fence
			lock.unlock();
			_lock_hold.mark("before gpu_wait_idle_app");
			gpu_wait_idle_app(c);
			lock.lock();
		} else {
			_lock_hold.mark("before gpu_wait_idle_app");
			gpu_wait_idle_app(c);
		}

		struct comp_vk_deposit_handoff rr_handoff = {};
		if (comp_d3d12_deposit_get_handoff(c->reroute.dep, &rr_handoff)) {
			d3d12_report_submit_gap(c, "app frame");
			_lock_hold.mark("before split submit_atlas");
			comp_vk_split_submit_atlas(c->reroute.split, &rr_handoff, c->eff_layout.cols,
			                           c->eff_layout.rows, c->eff_layout.tile_w, c->eff_layout.tile_h);
			comp_d3d12_deposit_note_consumed(c->reroute.dep, rr_handoff.slot);
			// The PLANE release edge, behind the pre_plane_write wait
			// submit_atlas just recorded on the same D3D11 immediate
			// context — one ordered stream, one signal for every plane.
			comp_d3d12_deposit_note_planes_consumed(c->reroute.dep);
		}

		// The DP canvas: the effective canvas sub-rect when one is active,
		// else the full target — the same answer the own weave computes.
		struct xrt_rect rr_canvas = {};
		if (eff_canvas.valid) {
			rr_canvas.offset.w = eff_canvas.x;
			rr_canvas.offset.h = eff_canvas.y;
			rr_canvas.extent.w = (int32_t)eff_canvas.w;
			rr_canvas.extent.h = (int32_t)eff_canvas.h;
		} else {
			rr_canvas.extent.w = (int32_t)tgt_width;
			rr_canvas.extent.h = (int32_t)tgt_height;
		}
		c->reroute.canvas = rr_canvas;

		_lock_hold.mark("before split weave+present");
		const bool rr_wove = comp_vk_split_weave_and_present(c->reroute.split, /*is_repaint=*/false,
		                                                     &rr_canvas);

		// #868: arm the repaint replay exactly as the VK tier does — a frame
		// that wove nothing keeps the panel's last woven frame and stays
		// disarmed for the tick.
		c->repaint.tgt_w = tgt_width;
		c->repaint.tgt_h = tgt_height;
		c->repaint.view_w = c->eff_layout.tile_w;
		c->repaint.view_h = c->eff_layout.tile_h;
		c->repaint.cols = c->eff_layout.cols;
		c->repaint.rows = c->eff_layout.rows;
		c->repaint.armed = rr_wove;
		if (rr_wove) {
			c->repaint.last_app_frame_ns = os_monotonic_get_ns();
			u_repaint_gate_on_app_frame(&c->repaint.gate, c->repaint.last_app_frame_ns);
		}

		comp_vk_split_render_diag(c->reroute.split);
		return XRT_SUCCESS;
	}
#endif

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
			_lock_hold.mark("before cmdlist Close");
			c->cmd_list->Close();
			ID3D12CommandList *copy_lists[] = {c->cmd_list};
			_lock_hold.mark("before ExecuteCommandLists");
			c->command_queue->ExecuteCommandLists(1, copy_lists);
			if (d3d12_fill_during_app_wait()) {
				// let the fill present in this hole; re-acquire after the fence
				lock.unlock();
				_lock_hold.mark("before gpu_wait_idle_app");
				gpu_wait_idle_app(c);
				lock.lock();
			} else {
				_lock_hold.mark("before gpu_wait_idle_app");
				gpu_wait_idle_app(c);
			}

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
				_lock_hold.mark("before cmdlist Close");
				c->cmd_list->Close();
				ID3D12CommandList *tap_lists[] = {c->cmd_list};
				_lock_hold.mark("before ExecuteCommandLists");
				c->command_queue->ExecuteCommandLists(1, tap_lists);
				if (d3d12_fill_during_app_wait()) {
					// let the fill present in this hole; re-acquire after the fence
					lock.unlock();
					_lock_hold.mark("before gpu_wait_idle_app");
					gpu_wait_idle_app(c);
					lock.lock();
				} else {
					_lock_hold.mark("before gpu_wait_idle_app");
					gpu_wait_idle_app(c);
				}
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
			d3d12_composite_zone_mask(c, false, false, c->shared_texture, st_rtv.ptr,
			                          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, dp_target_w,
			                          dp_target_h, &eff_canvas, /*slot=*/-1,
			                          /*is_repaint=*/false);

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

		// Signal fence and wait for frame completion. App scope: the shared
		// texture and the list that wrote it are both on the app device, and
		// this path has no swapchain at all.
		if (d3d12_fill_during_app_wait()) {
			// let the fill present in this hole; re-acquire after the fence
			lock.unlock();
			gpu_wait_idle_app(c);
			lock.lock();
		} else {
			gpu_wait_idle_app(c);
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

		/*
		 * #918 D12-4 — the 2D-UNDER BACKDROP, under the split, is deposited HERE.
		 *
		 * Off the split it is flattened inside the weave, immediately before the
		 * display processor is told about it, and that is still where it happens.
		 * It cannot stay there under the split for two independent reasons: it
		 * samples the app's own Local2D swapchain images (app device), and the
		 * weave records onto the OUT device's list. So the flatten runs on the app
		 * list here, the pixels cross as the BACKDROP plane, and the weave hands
		 * the DP the egress plane of the slot it is weaving.
		 *
		 * Before the composite's deposit half, because that half stamps the
		 * recipe and the recipe carries the backdrop's own extent.
		 */
		if (c->split_active) {
			uint32_t bd_w = 0, bd_h = 0;
			ID3D12Resource *bd = d3d12_flatten_backdrop_2d(c, tgt_width, tgt_height, &bd_w, &bd_h);
			c->repaint.backdrop = bd;
			c->repaint.backdrop_w = bd_w;
			c->repaint.backdrop_h = bd_h;
			if (bd != nullptr && bd_w > 0 && bd_h > 0) {
				/*
				 * The flatten left it in PIXEL_SHADER_RESOURCE for a display
				 * processor that, under the split, lives on the other adapter and
				 * will never sample it. The producer's COPY queue reads it
				 * instead, and that requires COMMON.
				 */
				D3D12_RESOURCE_BARRIER to_common = {};
				to_common.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				to_common.Transition.pResource = bd;
				to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
				to_common.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				c->cmd_list->ResourceBarrier(1, &to_common);

				int32_t bproj = -1;
				for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
					enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
					if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
						bproj = (int32_t)i;
						break;
					}
				}
				struct xrt_rect box = {};
				uint64_t hash = 0;
				d3d12_local2d_digest(c, bproj, /*over=*/false, bd_w, bd_h, &box, &hash);
				// Panel-sized, always: never resized, so structurally outside the
				// R2 churn path (#918 Phase 2a).
				if (comp_xbridge_bind_plane_resource(
				        c->xbridge, COMP_XBRIDGE_PLANE_BACKDROP, bd, c->backdrop_scratch_gen,
				        (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM, c->split_panel_w, c->split_panel_h)) {
					comp_xbridge_stage_plane(c->xbridge, COMP_XBRIDGE_PLANE_BACKDROP, hash,
					                         box.offset.w, box.offset.h, (uint32_t)box.extent.w,
					                         (uint32_t)box.extent.h);
				} else {
					// The backdrop degrades on its own — a session without one
					// simply has no 2D-under band, and the weave is unaffected.
					// Stamped invalid so the weave clears the DP background
					// rather than showing an older frame's.
					c->repaint.backdrop_w = 0;
					c->repaint.backdrop_h = 0;
				}
			} else {
				comp_xbridge_stage_plane(c->xbridge, COMP_XBRIDGE_PLANE_BACKDROP, 0, 0, 0, 0, 0);
			}
		}

		/*
		 * #918 D12-5 — stage the authored-mask plane ONCE per frame, BEFORE the
		 * deposit half asks anything about it. This is the only site that stages
		 * PLANE_MASK, and it deliberately does not depend on whether the plane's
		 * pixels have landed: the frame that authors a mask transports it, and
		 * consumption starts on whichever later frame the slot lands.
		 *
		 * `authored_mask` was resolved and BOUND at the top of this function; this
		 * records the frame-wish snapshot onto the still-open app list and hands
		 * the bridge the content generation.
		 */
		d3d12_stage_mask_plane(c, authored_mask);

		// #875 DEPOSIT half: mask resolve + Local2D flatten, recorded into the
		// list that is closed/executed/synced immediately below.
		const bool deposited = d3d12_composite_zone_mask(
		    c, /*reuse_mask=*/false, /*prepare_only=*/true, nullptr, 0, D3D12_RESOURCE_STATE_RENDER_TARGET,
		    D3D12_RESOURCE_STATE_RENDER_TARGET, tgt_width, tgt_height, &eff_canvas, /*slot=*/-1,
		    /*is_repaint=*/false);

		/*
		 * #918 D12-4 — the ONE place "this frame does not composite" is decided.
		 *
		 * A frame whose deposit half did not complete (projection-only, or a plane
		 * that could not be bound) must stamp `composite = false` and UN-STAGE the
		 * Local2D plane, or the consume half reads a slot claiming a composite it
		 * has no pixels for. Mirrors the D3D11 leg's `!deposited` branch.
		 *
		 * #918 D12-5 (the D3D11 leg's #918 review F1) — the MASK plane is
		 * deliberately NOT un-staged here. It is staged above, before the deposit
		 * runs, and this branch fires on exactly the frame whose transport the
		 * NEXT frame needs; reverting it is half of what makes a Tier-3 mask
		 * unable to bootstrap. Its own publisher un-stages it when the frame
		 * genuinely has no authored mask.
		 */
		if (c->split_active && !deposited && c->xbridge != nullptr) {
			struct comp_xbridge_recipe r = {};
			r.composite = false;
			r.bd_w = c->repaint.backdrop_w;
			r.bd_h = c->repaint.backdrop_h;
			comp_xbridge_stage_recipe(c->xbridge, &r);
			comp_xbridge_stage_plane(c->xbridge, COMP_XBRIDGE_PLANE_LOCAL2D, 0, 0, 0, 0, 0);
		}

		// Execute atlas copy so the texture is ready for the weaver
		c->cmd_list->Close();
		ID3D12CommandList *copy_lists[] = {c->cmd_list};
		c->command_queue->ExecuteCommandLists(1, copy_lists);
		gpu_wait_idle_app(c);

		// #542: the DP gets the frame's EFFECTIVE content layout (== the
		// mode layout for matched submissions), not the mode layout.
		uint32_t view_width = c->eff_layout.tile_w;
		uint32_t view_height = c->eff_layout.tile_h;

		/*
		 * #918 D12-3 — ship this frame's atlas across the adapter boundary.
		 *
		 * Placed AFTER the app queue has executed the atlas work, because the
		 * bridge's first act is to signal that queue: the producer waits on that
		 * signal, so the ordering is "everything the app queue holds up to now,
		 * then the copy". Nothing here waits — submit returns immediately and the
		 * weave below consumes whichever slot has already landed.
		 *
		 * #918 R1 — the LAYOUT GENERATION. It counts changes to the recipe the DP
		 * is running, and it is what lets the weave tell a slot that still belongs
		 * to the current mode from one composited under a mode the display has
		 * since left. Bumped here, together with the geometry snapshot the weave
		 * reads back (#1140: the recipe travels with the pixels).
		 */
		if (c->split_active && c->xbridge != nullptr) {
			const uint32_t cols = c->eff_layout.cols;
			const uint32_t rows = c->eff_layout.rows;
			if (cols != c->split_gen_cols || rows != c->split_gen_rows || view_width != c->split_gen_vw ||
			    view_height != c->split_gen_vh) {
				c->split_layout_gen++;
				c->split_gen_cols = cols;
				c->split_gen_rows = rows;
				c->split_gen_vw = view_width;
				c->split_gen_vh = view_height;
				U_LOG_W("#918: layout generation %llu — %ux%u tiles of %ux%u (#918 R1)",
				        (unsigned long long)c->split_layout_gen, cols, rows, view_width, view_height);
			}

			const uint32_t bridge_w = cols * view_width;
			const uint32_t bridge_h = rows * view_height;
			ID3D12Resource *src =
			    static_cast<ID3D12Resource *>(comp_d3d12_renderer_get_atlas_resource(c->renderer));
			// Bound by POINTER — the producer IS this device (#918 D12-3a). A
			// renderer resize re-binds through the fence-deferred retire; no
			// share, no open, no drain.
			comp_xbridge_bind_atlas_resource(c->xbridge, src, c->split_layout_gen);
			comp_xbridge_set_content_size(c->xbridge, bridge_w, bridge_h, c->split_layout_gen);
			c->split_seq++;
			comp_xbridge_submit(c->xbridge, c->split_seq, c->split_layout_gen, nullptr, bridge_w, bridge_h);
		}

		// Give the weaver a fresh command list — the OUT device's under the
		// split, which is the list every step from here to Present records into.
		if (c->split_active) {
			c->out_cmd_allocator->Reset();
			c->out_cmd_list->Reset(c->out_cmd_allocator, nullptr);
		} else {
			c->cmd_allocator->Reset();
			c->cmd_list->Reset(c->cmd_allocator, nullptr);
		}
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
			// the back buffer is still in PRESENT. Just flush and present, on
			// whichever queue owns the swapchain (#918 D12-3).
			ID3D12GraphicsCommandList *wl = d3d12_weave_list(c);
			wl->Close();
			ID3D12CommandList *weave_lists[] = {wl};
			d3d12_out_queue(c)->ExecuteCommandLists(1, weave_lists);
			d3d12_outcomp_note_execute(c);
			comp_d3d12_target_present(c->target, 1);
			// Out scope: pacing behind Present, as in d3d12_dp_weave_and_present.
			gpu_wait_idle_out(c);
		}

		/*
		 * #224 / ADR-027 P4: sideband-sync this client's zone state with the DP.
		 *
		 * #1175 — THIS CALL SITE WAS MISSING, and this is the exit an active
		 * output-device split takes, so the zone wish publish was inert on
		 * exactly the path #918 built. Both weave branches above end in their own
		 * fence wait (d3d12_dp_weave_and_present's post-Present gpu_wait_idle_out,
		 * or the no-atlas branch's), so the resource resolved here is GPU-complete
		 * — the same publish contract the other two exits satisfy.
		 *
		 * The weave, not the deposit, is what has to have finished: under the
		 * split the auto wish is rastered by the CONSUME half onto the weave list,
		 * and an authored mask's egress plane is filled by the consumer copy the
		 * weave took its GPU-side wait on. Publishing before the weave would hand
		 * the DP a resource nothing had written yet.
		 *
		 * A REPAINT never reaches here — the publish is a once-per-app-frame state
		 * transition, and repaints replay rendering only (#868).
		 */
		d3d12_sync_zone_mask_to_dp(c);

		// Post-compose capture (#210) — fully composed atlas as DP saw it.
		// DP path returns early; mirror the fallback path's call site so the
		// capture surface works regardless of which weave path ran.
		d3d12_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_POST_COMPOSE);

		// #672 woven back-buffer capture now lives in d3d12_dp_weave_and_present
		// so it can capture repaints as well as app frames (#868).

		// #918: the split's own ten-second window, same shape as the service's.
		d3d12_split_render_diag(c);

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

		// Signal fence and wait for frame completion (frame pacing).
		// Out scope: pacing behind the Present above.
		gpu_wait_idle_out(c);

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

#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	// #1264 reroute teardown: the arm first (its consumer holds NT opens of
	// the deposit slots and quiesces its own device), then the deposit —
	// AFTER the repaint join above, since the fill ticks drive the arm.
	if (c->reroute.split != NULL) {
		comp_vk_split_destroy(&c->reroute.split);
	}
	if (c->reroute.dep != NULL) {
		comp_d3d12_deposit_destroy(&c->reroute.dep);
	}
	c->reroute.active = false;
#endif

	// Wait for GPU idle. Teardown is one of the only two places that must
	// quiesce BOTH scopes, and it does so as two INDEPENDENT waits, each on
	// its own queue's fence — never one wait covering both queues. Off the
	// frame path by definition; see gpu_wait_idle_on for why the frame path
	// may not do this.
	if (c->fence != nullptr && c->command_queue != nullptr) {
		gpu_wait_idle_app(c);
		if (d3d12_out_timeline_is_separate(c)) {
			gpu_wait_idle_out(c);
		}
	}

	/*
	 * #918 D12-3: quiesce the bridge NEXT — stop submitting, join its watchdog
	 * and drain both links under a bounded CPU wait — because its egress slots
	 * are exactly what the display processor below is still sampling. The bridge
	 * itself is released later, AFTER the target, for the same reason.
	 */
	if (c->xbridge != nullptr) {
		comp_xbridge_quiesce(c->xbridge);
		comp_xbridge_destroy(&c->xbridge);
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

	// After the target — the swapchain the factory made outlives nothing here,
	// but releasing the factory first would be releasing the maker before the
	// made.
	if (c->dxgi_factory != nullptr) {
		c->dxgi_factory->Release();
		c->dxgi_factory = nullptr;
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

	/*
	 * #918 D12-3: the output device goes LAST. Everything above that was
	 * allocated on it — the DP crop, the HUD staging pair, the swapchain's back
	 * buffers — has to be released while its device is still alive, which is the
	 * same rule the app device already follows two blocks up.
	 */
	d3d12_split_release_out(c);

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
                             void *dp_factory_d3d11,
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
	c->dxgi_factory = nullptr;
	c->own_window = nullptr;
	c->owns_window = false;
	c->hardware_display_3d = true;
	c->last_3d_mode_index = 1;
	c->transparent_background = transparent_background;
	c->dp_factory_d3d11 = dp_factory_d3d11;
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

	// #1000: record which adapter the app's device actually lives on. On a
	// hybrid iGPU/dGPU box a hung run's log was byte-identical to a healthy
	// one because placement was never written down. One-time, init-only —
	// never per frame. D3D12 has no IDXGIDevice, and there is no DXGI factory
	// in scope here, so log the LUID alone rather than spinning one up just
	// for a description string — the LUID is what identifies the adapter.
	{
		LUID log_luid = c->device->GetAdapterLuid();
		U_LOG_W("D3D12 compositor: app device on adapter LUID=%08lx:%08lx (#1000)",
		        (unsigned long)log_luid.HighPart, (unsigned long)log_luid.LowPart);
	}

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

	/*
	 * #918 D12-3 STAGE A — before the target exists, because the target belongs
	 * to whichever device wins here. Best-effort throughout: any failure logs one
	 * reason and leaves `split_active` false, which makes every accessor below
	 * resolve to the app device exactly as it did before this rung.
	 */
	d3d12_split_stage_a(c, xdev, display_screen_left, display_screen_top);
	d3d12_log_weave_placement(c, xdev, display_screen_left, display_screen_top);

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
		// The app-adapter factory is created unconditionally: it is what the
		// target uses off the split, and what a mid-session split RETIRE needs
		// to rebuild the swapchain with (d3d12_split_retire). Under the split
		// the target is handed `out_factory` instead (#918 D12-2 made all three
		// parameters explicit precisely so this is a parameter change, not a
		// change inside the target).
		HRESULT fhr =
		    CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void **>(&c->dxgi_factory));
		if (FAILED(fhr) || c->dxgi_factory == nullptr) {
			U_LOG_E("Failed to create DXGI factory: 0x%08x", fhr);
			d3d12_compositor_destroy(&c->base.base);
			return XRT_ERROR_D3D;
		}
		if (c->reroute.active) {
			// #1264 reroute: the d3d11 fill arm owns the HWND's swapchain
			// and present. The factory above stays — a future retire
			// rebuilds the app-side target with it, exactly as a split
			// retire does.
			c->target = nullptr;
			U_LOG_I("Skipping app-side DXGI swapchain (reroute — the d3d11 arm presents)");
		} else {
			xret = d3d12_make_target(c, transparent_background);
			if (xret != XRT_SUCCESS) {
				U_LOG_E("Failed to create D3D12 target");
				d3d12_compositor_destroy(&c->base.base);
				return xret;
			}
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
		// #1252: settings chain (env > per-user > machine) — the Control
		// Panel's Compatibility mode turns this off. Same parse as before.
		char rp_buf[64];
		const char *e = u_setting_get_raw("DXR_WEAVE_REPAINT", rp_buf, sizeof(rp_buf), nullptr);
		c->repaint.enabled = (e != nullptr && e[0] == '0') ? 0 : 1;
		const char *fe = getenv("DXR_WEAVE_REPAINT_FORCE");
		c->repaint.force = (fe != nullptr && fe[0] == '1') ? 1 : 0;
		if (c->repaint.force == 1) {
			U_LOG_W("#868: DXR_WEAVE_REPAINT_FORCE=1 — repainting every refresh regardless of app "
			        "rate. This is a correctness probe and WILL cost frame rate.");
		}
		// #1264 reroute: there is deliberately no app-side target — the fill
		// replays through the d3d11 arm — but the fill LOOP is still this
		// thread. Gating on the target alone left the reroute with a working
		// partition and zero fills: a 20 Hz panel, worse than every config
		// this campaign measured (found on the first Unity verdict run).
		bool have_fill_surface = c->target != nullptr;
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
		have_fill_surface = have_fill_surface || c->reroute.active;
#endif
		if (c->repaint.enabled == 1 && have_fill_surface) {
			c->repaint_quit.store(false);
			c->repaint_thread = std::thread(d3d12_repaint_thread, c);
		} else if (c->repaint.enabled == 0) {
			U_LOG_W("#868: weave repaint disabled (DXR_WEAVE_REPAINT=0)");
		}
	}

	// Determine view dimensions
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

	// Create display processor via factory. #918 D12-3: on the device the weave
	// will run on — the scanout device under the split — and through the shared
	// helper d3d12_make_dp, so the create-time placement and a mid-session retire
	// cannot drift apart. Everything the DP is told after creation (output
	// format, transparency, shared-texture-present) moved in there with it.
	c->dp_factory = dp_factory_d3d12;
	if (c->reroute.active) {
		// #1264 reroute: the d3d11 fill arm's Stage A already created the
		// session's ONE weaver on the HWND (ADR-037 §3a negotiated in
		// there); creating this tier's own DP too would put two weavers on
		// one window — the hard rule d3d12_make_dp's doc carries.
		U_LOG_I("Skipping app-side display processor (reroute — the d3d11 arm's weaver owns the HWND)");
	} else if (dp_factory_d3d12 != NULL) {
		if (!d3d12_make_dp(c) && c->split_active) {
			/*
			 * ADR-037 §3a — A WEAVER-CREATION FAILURE IS A FALLBACK TRIGGER,
			 * NEVER A SESSION FAILURE, and "never a session failure" has to
			 * mean the session still WEAVES. Continuing here without a display
			 * processor satisfies the letter and fails the effect: on a 3D
			 * panel that is a flat or black experience, which is worse than the
			 * single-adapter fallback the ADR mandates.
			 *
			 * The refusal is specifically about the SCANOUT adapter — the
			 * vendor may have no weaver for that GPU, or the panel may not be
			 * reachable from it — so retire the split and rebuild the display
			 * processor on the app device, where the non-split path has always
			 * created it. The `(hwnd, device)` bind key is honoured by the
			 * retire: the (absent) scanout weaver is destroyed before the app
			 * one is created, so the HWND is never asked to hold two.
			 *
			 * Under c->mutex for the same reason layer_commit's call is: the
			 * repaint thread is already running by this point (it starts before
			 * the display processor exists and idles until the first frame
			 * arms it), and retire's contract is that its caller holds the lock.
			 */
			std::lock_guard<std::mutex> retire_lock(c->mutex);
			d3d12_split_retire(c, "the display processor declined a weaver on the scanout adapter",
			                   COMP_SPLIT_REASON_DP_REFUSED_SCANOUT);
		}
	} else {
		U_LOG_W("No D3D12 display processor factory provided");
	}

	// If display processor is available, query display pixel info to compute
	// optimal view dimensions (scaled to window size, matching D3D11 model).
	// Do NOT resize the app's window — _ext apps own their window. Under the
	// #1264 reroute the session's weaver lives in the d3d11 arm; ask it.
	{
		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		bool px_ok = false;
		if (c->display_processor != nullptr) {
			px_ok = xrt_display_processor_d3d12_get_display_pixel_info(
			    c->display_processor, &disp_px_w, &disp_px_h, &disp_left, &disp_top);
		}
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
		else if (c->reroute.active && c->reroute.split != NULL) {
			px_ok = comp_vk_split_get_display_pixel_info(c->reroute.split, &disp_px_w, &disp_px_h,
			                                             &disp_left, &disp_top);
		}
#endif
		if (px_ok && disp_px_w > 0 && disp_px_h > 0) {
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
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	if (c->reroute.active && c->reroute.split != NULL) {
		return comp_vk_split_get_predicted_eye_positions(c->reroute.split, out_eye_pos) &&
		       out_eye_pos->valid;
	}
#endif

	return false;
}

extern "C" bool
comp_d3d12_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                              float *out_width_m,
                                              float *out_height_m)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	if (c->reroute.active && c->reroute.split != NULL) {
		return comp_vk_split_get_display_dimensions(c->reroute.split, out_width_m, out_height_m);
	}
#endif
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
		bool have_dp = c->display_processor != nullptr;
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
		const bool have_reroute_dp = c->reroute.active && c->reroute.split != NULL;
		have_dp = have_dp || have_reroute_dp;
#endif
		if (!have_dp || metrics_hwnd == nullptr) {
			return false;
		}

		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		bool px_ok = false;
		float disp_w_m = 0.0f, disp_h_m = 0.0f;
		bool dim_ok = false;
		if (c->display_processor != nullptr) {
			px_ok = xrt_display_processor_d3d12_get_display_pixel_info(
			    c->display_processor, &disp_px_w, &disp_px_h, &disp_left, &disp_top);
			dim_ok = xrt_display_processor_d3d12_get_display_dimensions(c->display_processor, &disp_w_m,
			                                                            &disp_h_m);
		}
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
		else if (have_reroute_dp) {
			px_ok = comp_vk_split_get_display_pixel_info(c->reroute.split, &disp_px_w, &disp_px_h,
			                                             &disp_left, &disp_top);
			dim_ok = comp_vk_split_get_display_dimensions(c->reroute.split, &disp_w_m, &disp_h_m);
		}
#endif
		if (!px_ok || disp_px_w == 0 || disp_px_h == 0 || !dim_ok) {
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
	//
	// The pending work that matters is the present, so the OUT scope leads;
	// and this is the second of the two places that quiesce both scopes (see
	// destroy), again as two independent waits and again off the frame path —
	// a mode switch is a user action, not a per-frame one.
	gpu_wait_idle_out(c);
	if (d3d12_out_timeline_is_separate(c)) {
		gpu_wait_idle_app(c);
	}

#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	if (c->reroute.active && c->reroute.split != NULL) {
		// The d3d11 arm owns the weaver; it quiesces its own device around
		// the switch, exactly as the VK tier's forward does.
		return comp_vk_split_request_display_mode(c->reroute.split, enable_3d) ? XRT_SUCCESS
		                                                                       : XRT_ERROR_D3D12;
	}
#endif
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
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	else if (c->reroute.active && c->reroute.split != NULL) {
		comp_vk_split_set_eye_tracking_mode(c->reroute.split, mode);
	}
#endif
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
 *    c->cmd_list (Reset → record → Close → Execute → gpu_wait_idle_app) under
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
	/*!
	 * #918 D12-5 — the CONTENT generation of @ref staged, bumped by every
	 * authoring op that can change what a later stage copies into it.
	 *
	 * This is the bridge plane's change-skip key, so it is deliberately
	 * CONSERVATIVE: Tier-1/2 write @ref tex and only a submit (or a zones
	 * frame's wish stage) reaches @ref staged, so a `set_rects` with no submit
	 * behind it bumps a generation whose pixels did not move. That costs one
	 * R8 copy the bridge could have skipped. The opposite error — a mask whose
	 * pixels changed under an unchanged seq — is a silently wrong weave, and
	 * the two are not comparable.
	 */
	uint64_t author_seq;
	/*!
	 * #918 D12-5 — this mask OBJECT's generation, fixed at create from
	 * @ref comp_d3d12_compositor::zone_mask_gen_next.
	 *
	 * The D3D12 mask never reallocates its resources, so this is not the
	 * "realloc counter" @ref comp_xbridge_bind_plane_resource documents for the
	 * 2D scratches — it is the OBJECT identity that a recycled address cannot
	 * forge. Binding compares pointer AND generation, which is exactly the trap
	 * `comp_xbridge_set_source`'s `source_key` exists for: destroy a mask,
	 * create another, and the allocator can hand back the same address for
	 * genuinely different pixels.
	 */
	uint64_t res_gen;
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

/*!
 * The ADR-027 tier-1 gate's real question: does the display processor that
 * actually weaves THIS session consume zone masks?
 *
 * @ref d3d12_zone_dp_supported can only answer for the tier's OWN DP, and on the
 * #1264 reroute there is no own DP -- the weaver lives on the d3d11 fill arm and
 * `c->display_processor` is NULL by construction, so it answers "legacy DP" for a
 * weaver that in fact advertises zone slots. The tier-1 fallback then fires on the
 * first zones frame and requests hardware 3D, which silently overrides an app's 2D
 * rendering mode for the rest of the session (the mask leg itself was never
 * affected: it publishes through the arm, @ref comp_vk_split_publish_zone_wish).
 *
 * So ask whichever arm owns the weaver. `comp_vk_split_zone_dp_supported` exists
 * for exactly this question -- its own doc says the compositor cannot answer it for
 * a split session.
 */
static bool
d3d12_zone_dp_supported_weaving_arm(struct comp_d3d12_compositor *c)
{
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	if (c->reroute.active && c->reroute.split != NULL) {
		return comp_vk_split_zone_dp_supported(c->reroute.split);
	}
#endif
	return d3d12_zone_dp_supported(c);
}

/*!
 * Record @p mask's authoring texture into its staged snapshot on @p list.
 *
 * The ONE place the tex(RENDER_TARGET) -> staged(PIXEL_SHADER_RESOURCE) copy is
 * written. Every consumer reads `staged` and never `tex` — the composite, the DP
 * publish, and since #918 D12-5 the bridge's mask plane — so in-progress Tier-3
 * drawing can never tear into a frame. Caller holds c->mutex.
 */
static void
d3d12_zone_mask_snapshot(struct comp_d3d12_zone_mask *mask, ID3D12GraphicsCommandList *list)
{
	if (mask == nullptr || mask->tex == nullptr || mask->staged == nullptr || list == nullptr) {
		return;
	}
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
	list->ResourceBarrier(2, to_copy);

	list->CopyResource(mask->staged, mask->tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	list->ResourceBarrier(2, to_copy);
}

/*!
 * #918 D12-5 — the frame's AUTHORITATIVE app-authored mask, or NULL.
 *
 * A zones frame's is its explicit frame wish; a legacy frame's is the sticky
 * submitted mask. They are mutually exclusive by construction, and that is
 * exactly why the single authored-mask plane needs one arbiter rather than two
 * independent binders (the D3D11 leg's #918 review D6, ported).
 *
 * Caller holds c->mutex, and @ref comp_d3d12_compositor::zones_frame must
 * already be resolved for this frame.
 */
static struct comp_d3d12_zone_mask *
d3d12_frame_authored_mask(struct comp_d3d12_compositor *c)
{
	if (c->zones_frame) {
		struct comp_d3d12_zone_mask *fw = c->frame_wish;
		return (fw != nullptr && fw->tex != nullptr && fw->staged != nullptr) ? fw : nullptr;
	}
	struct comp_d3d12_zone_mask *m = c->active_zone_mask;
	return (m != nullptr && m->submitted && m->staged != nullptr) ? m : nullptr;
}

/*!
 * #918 D12-5 — bind @p mask's staged snapshot as the bridge's MASK plane source.
 *
 * THE TRANSPORT DECISION, AND WHY IT IS MADE HERE. This runs at the TOP of
 * layer_commit, before a single command of the frame is recorded, because its
 * failure branch is a RETIRE and a retire quiesces both devices and rebuilds the
 * target — which cannot be done half way through a recorded frame. Nothing here
 * records: a bind is an allocation plus an AddRef.
 *
 * Sized at the MASK, never the panel (#918 review F5). Both consumers stretch the
 * whole mask over the composite region — the shader gives t1 no uv scale at all
 * (see comp_d3d12_outcomp) and the DP publish declares the mask's own dims — so a
 * panel-sized plane would leave both sampling a never-written band.
 *
 * @return false ONLY when @p mask is non-NULL and its plane could not be bound.
 *         A frame with no authored mask returns true, having declared the plane
 *         not live.
 */
static bool
d3d12_bind_mask_plane(struct comp_d3d12_compositor *c, struct comp_d3d12_zone_mask *mask)
{
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	if (d3d12_fill_arm_active(c)) {
		/*
		 * #1264 reroute — the mask travels as a DEPOSIT plane (R8, sized at
		 * the mask), bound on the arm by NT handle. A failure degrades THAT
		 * FEATURE (mask_plane_live=false; the stage un-stages, and the
		 * composite falls to the raster kinds), never the session — so this
		 * always returns true on the reroute.
		 */
		c->mask_plane_live = false;
		c->mask_plane_gen = 0;
		if (mask == nullptr || c->reroute.dep == NULL || c->reroute.split == NULL) {
			return true;
		}
		struct comp_d3d12_deposit_plane mp = {};
		if (comp_d3d12_deposit_plane_ensure(c->reroute.dep, COMP_D3D12_DEPOSIT_PLANE_MASK, mask->w,
		                                    mask->h) &&
		    comp_d3d12_deposit_plane_get(c->reroute.dep, COMP_D3D12_DEPOSIT_PLANE_MASK, &mp) &&
		    comp_vk_split_bind_mask_plane(c->reroute.split, mp.shared_handle, mp.generation, mp.width,
		                                  mp.height)) {
			c->mask_plane_live = true;
			c->mask_plane_gen = mask->res_gen;
		}
		return true;
	}
#endif
	if (!c->split_active || c->xbridge == nullptr) {
		c->mask_plane_live = false;
		c->mask_plane_gen = 0;
		return true;
	}
	if (mask == nullptr) {
		c->mask_plane_live = false;
		c->mask_plane_gen = 0;
		return true;
	}
	/*
	 * A dims change rebuilds the plane's chain and drains the consumer fence
	 * inside the bridge — a bounded CPU wait, reached here from the app's own
	 * render thread. It is an ON-CHANGE event (the app created a
	 * differently-sized mask), never a per-frame one: the steady-state call is
	 * the pointer+generation early-out in comp_xbridge_bind_plane_resource. The
	 * authoring entry points that produce such a mask already drain the app
	 * queue outright (d3d12_zone_cmd_execute), so this adds no new class of
	 * wait to the leg.
	 */
	if (!comp_xbridge_bind_plane_resource(c->xbridge, COMP_XBRIDGE_PLANE_MASK, mask->staged, mask->res_gen,
	                                      (uint32_t)DXGI_FORMAT_R8_UNORM, mask->w, mask->h)) {
		c->mask_plane_live = false;
		c->mask_plane_gen = 0;
		return false;
	}
	c->mask_plane_live = true;
	c->mask_plane_gen = mask->res_gen;
	return true;
}

/*!
 * #918 D12-5 — stage the MASK plane's content for the next submit, and refresh
 * the snapshot the plane transports. The ONE site that stages PLANE_MASK.
 *
 * DEPOSIT, NOT CONSUME — and for this plane the reason is the opposite of the
 * mask RASTER's. A raster is CPU rects in, so it is built where it is consumed
 * (the output device, in the consume half, on the weave list). An authored mask
 * is PIXELS THE APP DREW on the render adapter: the only device that can read
 * them is the app's, the only list that reaches the producer's copy in time is
 * the app list, and both are the deposit half's. The consume half then merely
 * SAMPLES the egress resource the bridge landed beside this slot's atlas.
 *
 * Two properties this ordering buys, neither of which survived being folded into
 * the composite (the D3D11 leg's #918 review D6/F1, and the same trap here):
 *
 *  1. **One producer.** Binding one plane from two sites with two generations
 *     re-opens it on alternating frames, each re-open a full re-transport.
 *  2. **Staging does not depend on consumption.** The transport is set up before
 *     anything asks whether the plane's pixels have landed — so the frame that
 *     authors a mask transports it, and consumption starts on whichever later
 *     frame the slot lands. Gating this on the composite instead is what makes a
 *     Tier-3 mask unable to bootstrap: no deposit, no recipe, no transport, no
 *     mask next frame either, for ever.
 *
 * Caller holds c->mutex and has the app command list OPEN.
 */
static void
d3d12_stage_mask_plane(struct comp_d3d12_compositor *c, struct comp_d3d12_zone_mask *mask)
{
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
	if (d3d12_fill_arm_active(c)) {
		if (mask == nullptr || !c->mask_plane_live || c->reroute.split == NULL ||
		    c->reroute.dep == NULL) {
			// 0 un-stages: the recipe stamps the plane invalid rather than
			// lending an older frame's pixels — the arm's exact contract.
			if (c->reroute.split != NULL) {
				comp_vk_split_stage_mask_plane(c->reroute.split, 0, 0, 0);
			}
			return;
		}
		// A zones frame's explicit wish snapshot — the same refresh the
		// own-legs stage does, for the same #1175 reason (the off-split
		// stager never runs here).
		if (c->zones_frame) {
			d3d12_zone_mask_snapshot(mask, c->cmd_list);
			if (c->zone_frame_wish_last != mask) {
				c->zone_frame_wish_last = mask;
			}
		}
		// The transport IS the copy: staged (PSR at rest) -> deposit plane
		// (COMMON at rest), on content change only.
		const uint64_t key = (mask->res_gen << 48) | (mask->author_seq & 0xFFFFFFFFFFFFull);
		struct comp_d3d12_deposit_plane mp = {};
		if (comp_d3d12_deposit_plane_get(c->reroute.dep, COMP_D3D12_DEPOSIT_PLANE_MASK, &mp) &&
		    mp.width == mask->w && mp.height == mask->h) {
			if (key != c->reroute.mask_copied_key) {
				D3D12_RESOURCE_BARRIER pre[2] = {};
				pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				pre[0].Transition.pResource = mask->staged;
				pre[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
				pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				pre[1] = pre[0];
				pre[1].Transition.pResource = (ID3D12Resource *)mp.resource12;
				pre[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
				pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
				c->cmd_list->ResourceBarrier(2, pre);
				c->cmd_list->CopyResource((ID3D12Resource *)mp.resource12, mask->staged);
				D3D12_RESOURCE_BARRIER post[2] = {};
				post[0] = pre[0];
				post[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
				post[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				post[1] = pre[1];
				post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				post[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
				c->cmd_list->ResourceBarrier(2, post);
				c->reroute.mask_copied_key = key;
			}
			comp_vk_split_stage_mask_plane(c->reroute.split, key, mask->w, mask->h);
		} else {
			comp_vk_split_stage_mask_plane(c->reroute.split, 0, 0, 0);
		}
		return;
	}
#endif
	if (!c->split_active || c->xbridge == nullptr) {
		return;
	}
	if (mask == nullptr || !c->mask_plane_live) {
		// 0 is the bridge's "this frame does not use the plane" — the recipe
		// then stamps it invalid instead of lending an older frame's pixels.
		comp_xbridge_stage_plane(c->xbridge, COMP_XBRIDGE_PLANE_MASK, 0, 0, 0, 0, 0);
		return;
	}

	/*
	 * Refresh the snapshot the plane carries. A SUBMITTED sticky mask already
	 * staged itself in zone_mask_submit; a zones frame's explicit wish did not,
	 * because the entry point that hands it in only stores a pointer
	 * (comp_d3d12_compositor_zones_set_frame_wish) and the frame path that
	 * normally stages it — d3d12_update_zone_wish_state — is exactly the
	 * function the split does not run (the consume half rasters on the out
	 * device instead; #1175). So it is staged HERE, on the app list, before the
	 * submit that transports it.
	 *
	 * Ordering against the producer's in-flight read is comp_xbridge_pre_render's
	 * back-fence, which layer_commit already took for everything it writes this
	 * frame. The out-of-band twin — an authoring call that stages OUTSIDE
	 * layer_commit — takes comp_xbridge_pre_plane_write itself.
	 */
	if (c->zones_frame) {
		d3d12_zone_mask_snapshot(mask, c->cmd_list);
		// P4 publish generation: bump on a SOURCE change (a different mask
		// object), the same rule d3d12_update_zone_wish_state applies off the
		// split. The authored pixels' own changes ride author_seq below.
		if (c->zone_frame_wish_last != mask) {
			c->zone_frame_wish_last = mask;
			c->zone_publish_seq++;
		}
	}

	/*
	 * Content generation, unique across mask OBJECTS as well as across authoring
	 * calls on one: a session can hand this single plane two different masks, and
	 * two masks both at author_seq 1 must not look like the same pixels.
	 */
	uint64_t seq = 1469598103934665603ull;
	seq = (seq ^ mask->res_gen) * 1099511628211ull;
	seq = (seq ^ mask->author_seq) * 1099511628211ull;
	if (seq == 0) {
		seq = 1; // 0 is reserved for "this frame does not use the plane"
	}
	comp_xbridge_stage_plane(c->xbridge, COMP_XBRIDGE_PLANE_MASK, seq, 0, 0, mask->w, mask->h);
}

/*!
 * #918 D12-5 — @p mask's pixels ON THE DEVICE THE DISPLAY PROCESSOR LIVES ON.
 *
 * Off the split that is the app-device staged snapshot, verbatim. Under the split
 * the DP was created on the SCANOUT device (see d3d12_make_dp), so handing it an
 * app-device resource would be a cross-device pointer the vendor plug-in cannot
 * sample — the mask has to come off the bridge, from the slot the weave settled
 * on, seq-tested against that slot's own recipe (#918 review F3).
 *
 * The plane is allocated at exactly (mask->w, mask->h), so the publish's declared
 * dims describe the egress resource verbatim and there is no rescale here.
 *
 * @return NULL when the mask's pixels have not landed on the DP's device yet.
 *         That is a NORMAL transient under the split, never a withdrawal — see
 *         the have_content rule in d3d12_sync_zone_mask_to_dp.
 */
static ID3D12Resource *
d3d12_zone_mask_publish_res(struct comp_d3d12_compositor *c, struct comp_d3d12_zone_mask *mask)
{
	if (mask == nullptr) {
		return nullptr;
	}
	if (!c->split_active) {
		return mask->staged;
	}
	if (!c->mask_plane_live || c->mask_plane_gen != mask->res_gen || c->xbridge == nullptr) {
		return nullptr;
	}
	const int32_t slot = comp_xbridge_get_weave_slot(c->xbridge);
	struct comp_xbridge_recipe rec = {};
	if (slot < 0 || !comp_xbridge_slot_recipe(c->xbridge, slot, &rec) ||
	    (rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_MASK)) == 0) {
		return nullptr;
	}
	return static_cast<ID3D12Resource *>(comp_xbridge_get_plane_resource(c->xbridge, slot, COMP_XBRIDGE_PLANE_MASK,
	                                                                     rec.plane_seq[COMP_XBRIDGE_PLANE_MASK]));
}

/*!
 * Keep the DP's view of this client's zone mask in sync with the compositor's —
 * the D3D12 clone of d3d11_sync_zone_mask_to_dp. Zones frame: the WISH (the app's
 * explicit one, or the auto raster); legacy frame: the sticky submitted mask.
 * Caller holds c->mutex.
 *
 * **Called once per layer_commit, from EACH of its three exits**, always AFTER
 * that path's ExecuteCommandLists + fence wait, so the resource handed over is
 * GPU-complete and in its steady PIXEL_SHADER_RESOURCE state — that is the
 * publish contract, and it is why this is not simply hoisted to the top of
 * layer_commit the way the D3D11 leg's twin is (D3D11 records onto an immediate
 * context, where "recorded" and "submitted" are the same instant).
 *
 * #1175 — THE DP+TARGET EXIT USED TO BE MISSING, and it is the one an active
 * output-device split takes. The wish publish was therefore inert on exactly the
 * path #918 built, so a transported mask would have crossed the adapter
 * boundary correctly and then reached nothing. The wish IS the weave mask; a
 * dropped publish is not a cosmetic loss. Do not remove any of the three.
 *
 * #918 D12-5 — under the split the display processor was created on the SCANOUT
 * device, so every resource published from here has to be one of ITS: the auto
 * raster is already rastered there by the consume half, and an authored mask
 * comes off the bridge (@ref d3d12_zone_mask_publish_res).
 */
static void
d3d12_sync_zone_mask_to_dp(struct comp_d3d12_compositor *c)
{
	if (!d3d12_zone_dp_supported(c)) {
		return; // legacy DP — tier-1 global fallback path unchanged.
	}

	ID3D12Resource *res = nullptr;
	uint32_t mask_w = 0;
	uint32_t mask_h = 0;
	/*
	 * #918 D12-5 (the D3D12 twin of the D3D11 leg's #918 review F9) — does this
	 * frame HAVE published content at all, per the authoritative CPU-side state?
	 * That question, and only that question, decides whether a CLEAR is right.
	 * Whether the content's PIXELS have finished crossing the bridge is a
	 * transient that decides nothing: an authored mask's plane lands a frame
	 * behind the call that authored it, and a slot-less frame (#918 F4) resolves
	 * nothing at all. Clearing on those tells the DP the app withdrew its mask
	 * and then republished it — a clear/republish cycle at frame rate, which is a
	 * visible 2D/3D flicker rather than a degradation.
	 */
	bool have_content = false;
	if (c->zones_frame) {
		// A zones frame always has a wish: the app's explicit one, or the auto
		// union of its zone rects. Both are content; only which one varies.
		have_content = true;
		struct comp_d3d12_zone_mask *fw = c->frame_wish;
		if (fw != nullptr) {
			// An explicit wish that has not landed must NOT silently fall back
			// to the auto raster — that publishes different geometry for one
			// frame, which is a flicker, not a degradation.
			res = d3d12_zone_mask_publish_res(c, fw);
			mask_w = fw->w;
			mask_h = fw->h;
		} else {
			res = c->zone_publish_res;
			mask_w = c->zone_publish_w;
			mask_h = c->zone_publish_h;
		}
	} else {
		struct comp_d3d12_zone_mask *mask = c->active_zone_mask;
		have_content = (mask != nullptr && mask->submitted && mask->staged != nullptr);
		if (have_content) {
			res = d3d12_zone_mask_publish_res(c, mask);
			mask_w = mask->w;
			mask_h = mask->h;
		}
	}

	if (res == nullptr) {
		if (have_content) {
			return; // not landed yet — keep the DP's previous publish, retry next frame
		}
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
	// #918 D12-5 — the authored mask's plane borrow.
	c->mask_plane_live = false;
	c->mask_plane_gen = 0;
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
	gpu_wait_idle_app(c);
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
	// #918 D12-4: a REALLOCATION, which the bridge must re-bind to. Bumped on
	// every one — never a pointer compare, because a recycled address would
	// otherwise leave the producer copying the previous allocation.
	c->local2d_scratch_gen++;
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
                           ID3D12Device *dev,
                           ID3D12GraphicsCommandList *list,
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

		HRESULT hr =
		    dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
		                                 &clear, IID_PPV_ARGS(&c->implicit_mask_tex));
		if (c->implicit_mask_tex != nullptr)
			c->implicit_mask_tex->SetName(L"DXR.implicit_mask_tex"); // #747 attribution
		if (SUCCEEDED(hr) && c->implicit_mask_tex != nullptr) {
			D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
			rtv_desc.NumDescriptors = 1;
			rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			hr = dev->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&c->implicit_mask_rtv_heap));
		}
		if (SUCCEEDED(hr) && c->implicit_mask_rtv_heap != nullptr) {
			dev->CreateRenderTargetView(c->implicit_mask_tex, nullptr,
			                            c->implicit_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart());
			td.Flags = D3D12_RESOURCE_FLAG_NONE;
			hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
			                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
			                                  IID_PPV_ARGS(&c->implicit_mask_staged));
			if (c->implicit_mask_staged != nullptr)
				c->implicit_mask_staged->SetName(L"DXR.implicit_mask_staged"); // #747 attribution
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
	list->ClearRenderTargetView(rtv, all_3d, 0, nullptr);
	if (n > 0) {
		const float all_2d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		list->ClearRenderTargetView(rtv, all_2d, n, drs);
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
	list->ResourceBarrier(2, to_copy);

	list->CopyResource(c->implicit_mask_staged, c->implicit_mask_tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	list->ResourceBarrier(2, to_copy);

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
// Records onto the OPEN list. Caller holds c->mutex. Returns the
// staged R8 resource (steady PIXEL_SHADER_RESOURCE) or nullptr on failure.
static ID3D12Resource *
d3d12_update_zone_wish_mask(struct comp_d3d12_compositor *c,
                            ID3D12Device *dev,
                            ID3D12GraphicsCommandList *list,
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

		HRESULT hr =
		    dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
		                                 &clear, IID_PPV_ARGS(&c->implicit_mask_tex));
		if (c->implicit_mask_tex != nullptr)
			c->implicit_mask_tex->SetName(L"DXR.implicit_mask_tex"); // #747 attribution
		if (SUCCEEDED(hr) && c->implicit_mask_tex != nullptr) {
			D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
			rtv_desc.NumDescriptors = 1;
			rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			hr = dev->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&c->implicit_mask_rtv_heap));
		}
		if (SUCCEEDED(hr) && c->implicit_mask_rtv_heap != nullptr) {
			dev->CreateRenderTargetView(c->implicit_mask_tex, nullptr,
			                            c->implicit_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart());
			td.Flags = D3D12_RESOURCE_FLAG_NONE;
			hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
			                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
			                                  IID_PPV_ARGS(&c->implicit_mask_staged));
			if (c->implicit_mask_staged != nullptr)
				c->implicit_mask_staged->SetName(L"DXR.implicit_mask_staged"); // #747 attribution
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
	list->ClearRenderTargetView(rtv, all_off, 0, nullptr);
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
			list->ClearRenderTargetView(rtv, all_on, n, drs);
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
	list->ResourceBarrier(2, to_copy);

	list->CopyResource(c->implicit_mask_staged, c->implicit_mask_tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	list->ResourceBarrier(2, to_copy);

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
// feathered zones frame (VK-style); records onto the OPEN list.
// Caller holds c->mutex. Returns the staged R8 resource (steady
// PIXEL_SHADER_RESOURCE) or nullptr on failure (caller falls back to the
// binary mask — hard edges, never a lost frame).
static ID3D12Resource *
d3d12_update_zone_feather_mask(struct comp_d3d12_compositor *c,
                               ID3D12Device *dev,
                               ID3D12GraphicsCommandList *list,
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

		HRESULT hr =
		    dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
		                                 &clear, IID_PPV_ARGS(&c->feather_mask_tex));
		if (c->feather_mask_tex != nullptr) c->feather_mask_tex->SetName(L"DXR.feather_mask_tex"); // #747 attribution
		if (SUCCEEDED(hr) && c->feather_mask_tex != nullptr) {
			D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
			rtv_desc.NumDescriptors = 1;
			rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			hr = dev->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&c->feather_mask_rtv_heap));
		}
		if (SUCCEEDED(hr) && c->feather_mask_rtv_heap != nullptr) {
			dev->CreateRenderTargetView(c->feather_mask_tex, nullptr,
			                            c->feather_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart());
			td.Flags = D3D12_RESOURCE_FLAG_NONE;
			hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
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
	list->ClearRenderTargetView(rtv, all_off, 0, nullptr);
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
			list->ClearRenderTargetView(rtv, val, 1, &dr);
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
	list->ResourceBarrier(2, to_copy);

	list->CopyResource(c->feather_mask_staged, c->feather_mask_tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	list->ResourceBarrier(2, to_copy);

	static bool feather_logged = false;
	if (!feather_logged) {
		feather_logged = true;
		U_LOG_W("zone feather mask: %ux%u, %u zone rect(s) (composite-only, wish stays binary)", w, h,
		        rect_count);
	}
	return c->feather_mask_staged;
}

/*!
 * #224 / ADR-027 P4 — note the AUTO wish raster as this frame's published wish
 * source, bumping the content generation only when the geometry actually changed
 * (the raster re-records every zones frame, but an identical rect set at
 * identical dims is identical content, and a vendor treats a new seq as new
 * pixels to upload).
 *
 * #1175 — extracted from @ref d3d12_update_zone_wish_state so the OUTPUT-DEVICE
 * raster can note it too. Under the split that function never runs: the consume
 * half rasters directly on the out device (see d3d12_composite_zone_mask), so
 * without this the split's published wish source was never set and the publish
 * was inert even once its call site existed. Both sites must agree on what "the
 * wish changed" means, or the seq the DP sees lies about the pixels.
 */
static void
d3d12_note_auto_wish_publish(struct comp_d3d12_compositor *c,
                             ID3D12Resource *staged,
                             const struct xrt_rect *rects,
                             uint32_t rect_count,
                             uint32_t region_w,
                             uint32_t region_h)
{
	if (staged == nullptr) {
		return;
	}
	bool wish_dirty = c->zone_frame_wish_last != nullptr || c->zone_wish_rect_count != rect_count ||
	                  c->zone_publish_w != region_w || c->zone_publish_h != region_h;
	for (uint32_t i = 0; !wish_dirty && i < rect_count; i++) {
		if (memcmp(&c->zone_wish_rects[i], &rects[i], sizeof(rects[i])) != 0) {
			wish_dirty = true;
		}
	}
	if (wish_dirty) {
		c->zone_frame_wish_last = nullptr;
		if (rect_count > 0) {
			memcpy(c->zone_wish_rects, rects, sizeof(rects[0]) * rect_count);
		}
		c->zone_wish_rect_count = rect_count;
		c->zone_publish_seq++;
	}
	c->zone_publish_res = staged;
	c->zone_publish_w = region_w;
	c->zone_publish_h = region_h;
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
d3d12_update_zone_wish_state(struct comp_d3d12_compositor *c,
                             ID3D12Device *dev,
                             ID3D12GraphicsCommandList *list,
                             uint32_t region_w,
                             uint32_t region_h)
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
	ID3D12Resource *staged = d3d12_update_zone_wish_mask(c, dev, list, rects, rect_count, region_w, region_h);

	if (c->frame_wish != nullptr && c->frame_wish->tex != nullptr && c->frame_wish->staged != nullptr) {
		struct comp_d3d12_zone_mask *fw = c->frame_wish;

		// tex steady RENDER_TARGET, staged steady PIXEL_SHADER_RESOURCE
		// (see comp_d3d12_zone_mask) — same dance as zone_mask_submit,
		// recorded into the open frame cmd list. Under the split the SAME
		// snapshot is taken by d3d12_stage_mask_plane instead, on the app list,
		// because this function does not run there (#1175).
		d3d12_zone_mask_snapshot(fw, list);

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

	d3d12_note_auto_wish_publish(c, staged, rects, rect_count, region_w, region_h);
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
	// #918 D12-4 — see d3d12_ensure_local2d_scratch.
	c->backdrop_scratch_gen++;
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

	d3d12_clamp_region_to_panel(c, &region_w, &region_h);

	/*
	 * #918 D12-4: under the split this is a BRIDGE PLANE SOURCE — panel-sized
	 * once (so it never enters the R2 churn path) and bound by pointer, with no
	 * NT share because the producer IS this device. The flatten below still
	 * writes only the region at the top-left, and the DP is told the region's
	 * dims separately, exactly as it is off the split.
	 */
	const uint32_t bd_alloc_w = (c->split_active || d3d12_fill_arm_active(c)) ? c->split_panel_w : region_w;
	const uint32_t bd_alloc_h = (c->split_active || d3d12_fill_arm_active(c)) ? c->split_panel_h : region_h;
	if (!d3d12_ensure_backdrop_scratch(c, bd_alloc_w, bd_alloc_h)) {
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

/*!
 * #918 D12-4 — CHANGE-SKIP key and DIRTY BOX for a Local2D flatten. Port of
 * `d3d11_local2d_digest`, and it must stay one: the two legs feed the same
 * bridge, so a divergence here is a divergence in what "the plane changed" means.
 *
 * The hash covers everything that can change the flattened pixels without
 * changing the rect: the swapchain a layer draws from, the image index inside
 * it, the source sub-rect, the flip and the blend flags. The image index is what
 * makes this safe rather than optimistic — the swapchain hands out indices
 * round-robin, so an app that redraws hashes differently every frame and an app
 * that stops acquiring keeps the index (and genuinely has not changed).
 *
 * @param over true for the OVER layers (the composite's `twod`), false for the
 *        2D-under backdrop.
 */
static void
d3d12_local2d_digest(struct comp_d3d12_compositor *c,
                     int32_t proj_idx,
                     bool over,
                     uint32_t region_w,
                     uint32_t region_h,
                     struct xrt_rect *out_box,
                     uint64_t *out_hash)
{
	uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
	auto mix = [&h](const void *ptr, size_t n) {
		const uint8_t *b = static_cast<const uint8_t *>(ptr);
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

/*!
 * #918 D12-4 — clamp a composite region to the panel.
 *
 * Under the split the flatten scratches are PANEL-sized once (that is what keeps
 * them out of the R2 churn path), so a region larger than the panel would index
 * outside them. A window cannot exceed the panel in practice; this is the
 * structural guarantee rather than the practical one. No-op off the split.
 */
static void
d3d12_clamp_region_to_panel(struct comp_d3d12_compositor *c, uint32_t *w, uint32_t *h)
{
	if ((!c->split_active && !d3d12_fill_arm_active(c)) || c->split_panel_w == 0 || c->split_panel_h == 0) {
		return;
	}
	if (*w > c->split_panel_w) {
		*w = c->split_panel_w;
	}
	if (*h > c->split_panel_h) {
		*h = c->split_panel_h;
	}
}

// (startup jerk) Sub-step timing inside the zone-mask composite.
//
// Measured: ~185 ms of a ~199 ms c->mutex hold in layer_commit lives between
// [before composite zone_mask] and [before deposit plane_wait], i.e. inside
// d3d12_composite_zone_mask -- with the GPU idle at 7-12%. This names the sub-step.
// Threshold shared with the rest: DXR_SLOW_SECTION_MS (default 50, 0 disables).
// Reset at each invocation: a single static prev would otherwise measure the span from
// the LAST mark of one frame to the FIRST of the next -- i.e. the frame interval, which
// is exactly the wrong answer (it reported ~390 ms and meant nothing).
static uint64_t g_zone_step_prev_ns;
static const char *g_zone_step_prev_nm = "entry";
static void
d3d12_zone_step_begin(void)
{
	g_zone_step_prev_ns = os_monotonic_get_ns();
	g_zone_step_prev_nm = "composite entry";
}

static void
d3d12_zone_step(const char *nm)
{
	static double thr_ms = -1.0;
	if (thr_ms < 0.0) {
		const char *e = getenv("DXR_SLOW_SECTION_MS");
		thr_ms = (e != NULL && e[0] != '\0') ? atof(e) : 50.0;
	}
	uint64_t &prev_ns = g_zone_step_prev_ns;
	const char *&prev_nm = g_zone_step_prev_nm;
	const uint64_t now = os_monotonic_get_ns();
	if (thr_ms > 0.0 && prev_ns != 0) {
		const double d = (double)(now - prev_ns) / 1e6;
		if (d >= thr_ms) {
			U_LOG_W("[ZONESTEP] %.1f ms spent after [%s], before [%s]\n",
				        d, prev_nm, nm);
		}
	}
	prev_ns = now;
	prev_nm = nm;
}

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
                          const struct u_canvas_rect *eff_canvas,
                          int32_t slot,
                          bool is_repaint)
{
	d3d12_zone_step_begin();
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

	/*
	 * #918 D12-4 — THE DEPOSIT / CONSUME SEAM IS ALSO THE DEVICE SEAM.
	 *
	 * The deposit half (prepare_only) reads app-owned resources — the Local2D
	 * swapchain images — and stays on the app device, recording into the app's
	 * command list, unchanged. The consume half writes `dst`, which the split
	 * moved to the scanout adapter, so it runs on the OUTPUT device against the
	 * plane pixels that crossed with this slot's atlas, and it reads its
	 * PARAMETERS from the slot's recipe stamp rather than from live CPU state.
	 * That is the Phase-1 offset-cube defect class closed one level down: the
	 * atlas generation stops a slot being woven under the wrong mode, this stops
	 * its pixels being composited under the wrong recipe.
	 */
	// #1264: the reroute runs the SAME deposit half (capture + flatten +
	// stage), with the transport forked to the d3d11 arm at the staging
	// site; the consume half stays own-legs-only (the arm composites on its
	// own device).
	const bool split_deposit = (c->split_active || d3d12_fill_arm_active(c)) && prepare_only;
	const bool split_consume = c->split_active && !prepare_only;
	struct comp_xbridge_recipe rec = {};
	if (split_consume) {
		d3d12_zone_step("xbridge_slot_recipe");
		if (slot < 0 || !comp_xbridge_slot_recipe(c->xbridge, slot, &rec)) {
			ZC_BAIL("slot");
		}
		if (!rec.composite) {
			// A projection-only frame filled this slot — nothing to composite,
			// and that is the correct answer, not a bail.
			d3d12_zone_step("about to return (line ~54)");
			return false;
		}
	}

	if (!prepare_only && (dst == nullptr || dst_rtv == 0)) {
		ZC_BAIL("dst");
	}
	if (c->renderer == nullptr) {
		ZC_BAIL("renderer");
	}
	// #868: a repaint composites from the mask the last app frame resolved, so
	// the per-frame predicates below do not apply to it — only the presence of
	// that cached mask does.
	if (!reuse_mask && !split_consume && !zones_frame && !have_explicit && !have_local_2d) {
		ZC_BAIL("g1");
	}
	if (reuse_mask && !split_consume && c->repaint.mask_res == nullptr) {
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
	d3d12_zone_step("clamp_region_to_panel");
	d3d12_clamp_region_to_panel(c, &region_w, &region_h);
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
			ZC_BAIL("region");
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

	// The frame's zone rects + feather radii, gathered once (both the raster
	// REQUEST under the split and the inline raster off it want them).
	struct xrt_rect zrects[XRT_MAX_LAYERS];
	float zfeather[XRT_MAX_LAYERS];
	uint32_t zcount = 0;
	bool any_feather = false;
	if (zones_frame) {
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
	}

	// Zones frame (XR_DXR_display_zones, #801): the mask is ALWAYS the
	// BINARY zone raster — an explicit frame wish is staged for the publish
	// only (never a compositor blend gate). #803: when any zone requests
	// feather, the composite samples a separately-rastered per-zone feather
	// mask instead; the published wish stays binary regardless (cosmetics
	// never enter the wish).
	ID3D12Resource *mask_res = nullptr;
	if (split_deposit) {
		/*
		 * #918 D12-4 — THE DEPOSIT HALF RESOLVES NO MASK, IT CAPTURES A REQUEST.
		 * See `out_mask_req`: the raster belongs on the OUTPUT device (it is pure
		 * CPU rects in, so it is built where the composite consumes it rather than
		 * transported), and the out-device command list does not exist yet at this
		 * point in layer_commit — it is Reset AFTER this half runs. The consume
		 * half records the raster from what is captured here.
		 *
		 * #918 D12-5 — AN APP-AUTHORED (Tier-3) MASK CAPTURES NOTHING HERE, and
		 * that asymmetry is the point. A raster is CPU rects in, so it is built
		 * where it is consumed; an authored mask is pixels the app DREW on the
		 * render adapter, so it is TRANSPORTED instead — bound and staged as
		 * COMP_XBRIDGE_PLANE_MASK by d3d12_stage_mask_plane, which ran before
		 * this half, on the app list, with the app device in hand. The recipe
		 * stamp below records which of the two this frame's composite gets.
		 */
		c->out_mask_req.kind = D3D12_OUT_MASK_NONE;
		c->out_mask_req.count = 0;
		c->out_mask_req.w = region_w;
		c->out_mask_req.h = region_h;
		const bool bridged_authored = have_explicit && c->mask_plane_live;
		if (zones_frame) {
			c->out_mask_req.kind = any_feather ? D3D12_OUT_MASK_ZONE_FEATHER : D3D12_OUT_MASK_ZONE_BINARY;
			c->out_mask_req.count = zcount;
			for (uint32_t i = 0; i < zcount; i++) {
				c->out_mask_req.rects[i] = zrects[i];
				c->out_mask_req.feather[i] = zfeather[i];
			}
		} else if (bridged_authored) {
			// Nothing to raster: the mask is riding the plane. Falls through to
			// the plane bind + recipe stamp below with kind == NONE, which is
			// why the guard that follows exempts it rather than bailing.
		} else if (have_local_2d) {
			uint32_t rect_count = 0;
			for (uint32_t i = 0; i < c->layer_accum.layer_count && rect_count < XRT_MAX_LAYERS; i++) {
				if (c->layer_accum.layers[i].data.type != XRT_LAYER_LOCAL_2D) {
					continue;
				}
				if (proj_idx >= 0 && (int32_t)i < proj_idx) {
					continue; // under-layer (backdrop) — not part of the overlay mask
				}
				c->out_mask_req.rects[rect_count++] = c->layer_accum.layers[i].data.local_2d.rect;
			}
			c->out_mask_req.kind = D3D12_OUT_MASK_IMPLICIT;
			c->out_mask_req.count = rect_count;
		}
		if (!bridged_authored && (c->out_mask_req.kind == D3D12_OUT_MASK_NONE || c->out_mask_req.count == 0)) {
			ZC_BAIL("g4");
		}
	} else if (split_consume) {
		/*
		 * #918 D12-4 — RECORD THE REQUESTED RASTER, HERE, ON THE OUTPUT DEVICE.
		 *
		 * This is the one place the D3D12 leg is structurally different from the
		 * D3D11 one, and the difference is the command list, not the algorithm.
		 * D3D11 rasters output-side inside its DEPOSIT half, because it records
		 * onto an immediate context; a D3D12 raster recorded there would go onto a
		 * list that layer_commit `Reset()`s before it ever executes. So it lands
		 * here instead, on the weave list, immediately before the composite that
		 * samples it and inside the same submission. Do not move it back.
		 *
		 * A REPAINT does not raster. Rastering re-runs a once-per-app-frame state
		 * machine at panel rate, which is the #868 rule; it composites from the
		 * resource the last app frame's consume half produced.
		 *
		 * #918 D12-5 — AND SOME MASKS ARE NOT RASTERED AT ALL. An app-authored
		 * (Tier-3) mask's pixels were drawn by the application on the render
		 * adapter, so the deposit half bound and staged them as
		 * COMP_XBRIDGE_PLANE_MASK and this half only SAMPLES the egress resource
		 * that landed beside this slot's atlas. Which of the two kinds this frame
		 * gets comes FROM THE SLOT'S RECIPE, never from live CPU state — a mask a
		 * later frame authored must not be composited over these pixels.
		 */
		if (rec.mask_kind == COMP_XBRIDGE_MASK_PLANE) {
			if ((rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_MASK)) == 0) {
				ZC_BAIL("mask_plane_invalid");
			}
			/*
			 * Resolved from the SLOT on repaints too, rather than cached in
			 * repaint.mask_res: a repaint re-weaves the slot the last app frame
			 * wove (comp_xbridge_get_weave_slot), so the same call returns the
			 * same resource under the same seq test — and a cached pointer would
			 * be the one thing that could outlive its seq.
			 */
			d3d12_zone_step("xbridge_get_plane_resource");
			mask_res = static_cast<ID3D12Resource *>(comp_xbridge_get_plane_resource(
			    c->xbridge, slot, COMP_XBRIDGE_PLANE_MASK, rec.plane_seq[COMP_XBRIDGE_PLANE_MASK]));
			if (mask_res == nullptr) {
				ZC_BAIL("mask_plane_stale");
			}
		} else if (!is_repaint && c->out_mask_req.kind != D3D12_OUT_MASK_NONE) {
			ID3D12Device *odev = d3d12_out_device(c);
			d3d12_zone_step("weave_list");
			ID3D12GraphicsCommandList *olist = d3d12_weave_list(c);
			ID3D12Resource *r = nullptr;
			const uint32_t mw = c->out_mask_req.w;
			const uint32_t mh = c->out_mask_req.h;
			if (c->out_mask_req.kind == D3D12_OUT_MASK_IMPLICIT) {
				d3d12_zone_step("update_implicit_mask");
				r = d3d12_update_implicit_mask(c, odev, olist, c->out_mask_req.rects,
				                               c->out_mask_req.count, mw, mh);
			} else {
				d3d12_zone_step("update_zone_wish_mask");
				ID3D12Resource *binary = d3d12_update_zone_wish_mask(
				    c, odev, olist, c->out_mask_req.rects, c->out_mask_req.count, mw, mh);
				r = binary;
				/*
				 * #1175 — THE PUBLISHED WISH, on the path an active split
				 * takes. Off the split d3d12_update_zone_wish_state does this;
				 * that function never runs here, so without this line the
				 * split's wish source stayed NULL and the publish had nothing
				 * to hand the DP. The BINARY raster, never the feather one:
				 * cosmetics never enter the wish (#803). Skipped when the app
				 * supplied an explicit wish — that publishes verbatim off the
				 * mask plane, and must not silently fall back to this geometry.
				 */
				if (c->frame_wish == nullptr) {
					d3d12_note_auto_wish_publish(c, binary, c->out_mask_req.rects,
					                             c->out_mask_req.count, mw, mh);
				}
				if (c->out_mask_req.kind == D3D12_OUT_MASK_ZONE_FEATHER) {
					d3d12_zone_step("update_zone_feather_mask");
					ID3D12Resource *fres = d3d12_update_zone_feather_mask(
					    c, odev, olist, c->out_mask_req.rects, c->out_mask_req.feather,
					    c->out_mask_req.count, mw, mh);
					if (fres != nullptr) {
						r = fres;
					} // raster failure: binary fallback — hard edges, never a lost frame
				}
			}
			if (r != nullptr) {
				c->repaint.mask_res = r;
			}
			mask_res = c->repaint.mask_res;
		} else {
			// A repaint, or a frame whose deposit half requested no raster:
			// composite from the resource the last app frame's consume half
			// produced. Only ever an OUT-DEVICE RASTER — a transported mask is
			// resolved from the slot above, on every frame kind.
			mask_res = c->repaint.mask_res;
		}
	} else if (reuse_mask) {
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
		d3d12_zone_step("update_zone_wish_state");
		mask_res = d3d12_update_zone_wish_state(c, c->device, c->cmd_list, region_w, region_h);
		if (any_feather) {
			d3d12_zone_step("update_zone_feather_mask");
			ID3D12Resource *fres = d3d12_update_zone_feather_mask(c, c->device, c->cmd_list, zrects,
			                                                      zfeather, zcount, region_w, region_h);
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
		d3d12_zone_step("update_implicit_mask");
		mask_res = d3d12_update_implicit_mask(c, c->device, c->cmd_list, rects, rect_count, region_w, region_h);
	}
	if (mask_res == nullptr && !split_deposit) {
		ZC_BAIL("g4");
	}
	if (!reuse_mask && !split_deposit && !split_consume) {
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
	if (!split_consume) {
		/*
		 * #918 D12-4: under the split this scratch is a BRIDGE PLANE SOURCE, and
		 * is allocated at the PANEL rather than the region — once, then never
		 * resized, which is what keeps it structurally outside the R2 churn path
		 * and #1091. The flatten still writes only the window region at the
		 * top-left, and the composite derives the plane's uv scale from its own
		 * extent, so a panel-sized plane samples the region 1:1.
		 */
		const uint32_t alloc_w = (c->split_active || d3d12_fill_arm_active(c)) ? c->split_panel_w : region_w;
		const uint32_t alloc_h = (c->split_active || d3d12_fill_arm_active(c)) ? c->split_panel_h : region_h;
		if (!d3d12_ensure_local2d_scratch(c, alloc_w, alloc_h)) {
			ZC_BAIL("g5");
		}
	}
	// Zones frame: flatten ALL Local2D layers (no under/over split —
	// 2D-under is reserved in v1).
	if (!reuse_mask && !split_consume) {
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
	//
	// #918 D12-4: this is ALSO the device seam. Everything above ran on the app
	// device; everything below runs on the output device, reading the plane
	// pixels the bridge landed beside this slot's atlas.
	if (prepare_only) {
		if (split_deposit) {
			/*
			 * The flatten left the scratch in PIXEL_SHADER_RESOURCE for a
			 * composite that, under the split, does not run on this device. The
			 * producer's COPY queue reads it instead, and a copy queue requires
			 * COMMON — so put it back where the next flatten expects to find it
			 * and where the copy can promote from. Off the split this is the
			 * composite's own `restore` barrier below; there is no third place
			 * the steady state is decided.
			 */
			D3D12_RESOURCE_BARRIER to_common = {};
			to_common.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			to_common.Transition.pResource = c->local2d_scratch;
			to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			to_common.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			d3d12_zone_step("ResourceBarrier");
			c->cmd_list->ResourceBarrier(1, &to_common);

			/*
			 * The unit the CONSUME half will need. Created here rather than there
			 * because this half stamps the recipe: a create failure stamps
			 * `composite = false` and the frame ships projection-only, where
			 * discovering it in the consume half would leave a slot marked
			 * compositable that nothing can composite. See d3d12_ensure_outcomp
			 * for why the create is lazy at all.
			 */
#ifdef COMP_D3D12_HAVE_D3D11_FILL_ARM
			if (d3d12_fill_arm_active(c)) {
				// #1264 — the transport fork: the arm rasters the captured
				// mask itself and composites on its own device, so the
				// own-legs outcomp/xbridge below never runs.
				if (!d3d12_reroute_stage_local2d(c, region_w, region_h, zones_frame,
				                                 have_explicit && c->mask_plane_live, have_explicit,
				                                 eff_canvas, zones_frame ? -1 : proj_idx)) {
				d3d12_zone_step("L2D:returned false");
					c->out_mask_req.kind = D3D12_OUT_MASK_NONE;
					d3d12_zone_step("about to return (line ~458)");
					return false;
				}
				d3d12_zone_step("about to return (line ~460)");
				return true;
			}
#endif
			if (!d3d12_ensure_outcomp(c)) {
				c->out_mask_req.kind = D3D12_OUT_MASK_NONE;
				d3d12_zone_step("about to return (line ~465)");
				return false;
			}

			struct xrt_rect box = {};
			uint64_t hash = 0;
			d3d12_local2d_digest(c, zones_frame ? -1 : proj_idx, /*over=*/true, region_w, region_h, &box,
			                     &hash);
			const bool twod_bound = comp_xbridge_bind_plane_resource(
			    c->xbridge, COMP_XBRIDGE_PLANE_LOCAL2D, c->local2d_scratch, c->local2d_scratch_gen,
			    (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM, c->split_panel_w, c->split_panel_h);
			if (!twod_bound) {
				/*
				 * #918 review D4: the Local2D plane IS the composite's `twod`
				 * under the split, so a frame that could not bind it has no
				 * composite to stamp. Claiming otherwise stamps `composite=true`
				 * on a slot whose plane the submit then marks invalid, and the
				 * consume half bails on every frame afterwards with no log.
				 */
				if (!c->local2d_plane_warned) {
					c->local2d_plane_warned = true;
					U_LOG_W(
					    "#918 D12-4: the Local2D plane could not be bound — 2D content does "
					    "not composite under the split for this session; the 3D weave is "
					    "unaffected");
				}
				c->out_mask_req.kind = D3D12_OUT_MASK_NONE;
				ZC_BAIL("plane");
			}
			comp_xbridge_stage_plane(c->xbridge, COMP_XBRIDGE_PLANE_LOCAL2D, hash, box.offset.w,
			                         box.offset.h, (uint32_t)box.extent.w, (uint32_t)box.extent.h);

			// Stamp the recipe this frame's slot will carry (#918 Phase 2a): the
			// consume half reads its parameters from HERE, never from live CPU
			// state, so a slot's pixels can never be composited under a later
			// frame's recipe.
			struct comp_xbridge_recipe r = {};
			r.composite = true;
			/*
			 * #918 D12-5 — WHICH KIND OF MASK THIS SLOT'S COMPOSITE GETS, decided
			 * here and read from the slot there. D12-4 stamped OUT_RASTER
			 * unconditionally because it transported no authored mask at all.
			 *
			 * Only a LEGACY frame's sticky authored mask rides the plane as a
			 * composite input. A ZONES frame's explicit wish deliberately does
			 * not: per ADR-027/#801 the wish is HARDWARE-only, so the composite's
			 * gate there is always the binary zone raster and the wish's own
			 * pixels reach the display processor through the publish instead —
			 * which is why the plane is still bound and staged for a zones frame
			 * even though this stamps OUT_RASTER.
			 */
			r.mask_kind = (have_explicit && c->mask_plane_live) ? COMP_XBRIDGE_MASK_PLANE
			                                                    : COMP_XBRIDGE_MASK_OUT_RASTER;
			r.region_w = region_w;
			r.region_h = region_h;
			if (zones_frame) {
				r.composite_mode = COMP_D3D12_COMPOSITE_MODE_ZONES;
			} else if (have_explicit) {
				r.composite_mode = COMP_D3D12_COMPOSITE_MODE_LERP;
			} else {
				r.composite_mode = COMP_D3D12_COMPOSITE_MODE_ALPHA_OVER;
			}
			r.opaque_present = c->transparent_background && debug_get_bool_option_present_opaque_comp();
			r.cx = eff_canvas->valid ? eff_canvas->x : 0;
			r.cy = eff_canvas->valid ? eff_canvas->y : 0;
			r.cw = eff_canvas->valid ? eff_canvas->w : region_w;
			r.ch = eff_canvas->valid ? eff_canvas->h : region_h;
			// The backdrop's OWN extent — the DP declares it separately from the
			// composite region, and the two can legitimately differ. Filled by
			// the pre-weave backdrop flatten, which ran before this.
			r.bd_w = c->repaint.backdrop_w;
			r.bd_h = c->repaint.backdrop_h;
			comp_xbridge_stage_recipe(c->xbridge, &r);
		}
		d3d12_zone_step("about to return (line ~538)");
		return true;
	}

	/*
	 * #918 D12-4: under the split `twod` is the LOCAL2D PLANE that landed with
	 * this slot — the app-device scratch above lives on the wrong adapter and is
	 * never what the composite samples here.
	 */
	if (split_consume) {
		if ((rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_LOCAL2D)) == 0) {
			ZC_BAIL("plane_invalid");
		}
		d3d12_zone_step("xbridge_get_plane_resource");
		twod_res = static_cast<ID3D12Resource *>(comp_xbridge_get_plane_resource(
		    c->xbridge, slot, COMP_XBRIDGE_PLANE_LOCAL2D, rec.plane_seq[COMP_XBRIDGE_PLANE_LOCAL2D]));
		if (twod_res == nullptr) {
			ZC_BAIL("plane_stale");
		}
		if (mask_res == nullptr) {
			ZC_BAIL("g4");
		}
	}

	// Effective canvas rect clamped to the window region (the shader ignores
	// it on the mask path; kept coherent for the constants anyway). Phase 2:
	// this is the window rect while the mask is active.
	// #918 D12-4: under the split these come FROM THE SLOT, not from live CPU
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
	if (split_consume) {
		composite_mode = rec.composite_mode;
	} else if (zones_frame) {
		composite_mode = COMP_D3D12_COMPOSITE_MODE_ZONES;
	} else if (have_explicit) {
		composite_mode = COMP_D3D12_COMPOSITE_MODE_LERP;
	} else {
		composite_mode = COMP_D3D12_COMPOSITE_MODE_ALPHA_OVER;
	}

	// #833/#116 — opaque present on a transparent session: DWM completes no
	// blends, so the composite flattens against the weave (which the DP's
	// flattened gate already completed against the captured desktop) and
	// emits α=1. Opaque sessions keep today's behavior even with the env set.
	const bool opaque_present = split_consume
	                                ? rec.opaque_present
	                                : (c->transparent_background && debug_get_bool_option_present_opaque_comp());

	// One-shot proof-of-life (capture is pre-weave, #492 — this is how we
	// confirm the post-weave composite ran without a live eyeball).
	static bool composite_logged = false;
	if (!composite_logged) {
		U_LOG_W("D3D12 Local2D composite: %ux%u region, %s mask, twod=%s (mode=%u split=%d)", region_w,
		        region_h, zones_frame ? "zone" : (have_explicit ? "explicit" : "implicit"),
		        split_consume ? "bridged Local2D plane" : "local2d layers", composite_mode, (int)split_consume);
		composite_logged = true;
	}

	if (split_consume) {
		/*
		 * #918 D12-4 — the whole tail on the OUTPUT device, through the unit
		 * D12-1 built for exactly this. Every resource handed in belongs to the
		 * out device: `dst` is the scanout swapchain's back buffer, `twod_res` is
		 * the egress plane, `mask_res` is the raster recorded a few lines up, and
		 * the weave snapshot is the unit's own scratch.
		 */
		d3d12_zone_step("weave_list");
		ID3D12GraphicsCommandList *olist = d3d12_weave_list(c);
		/*
		 * The deposit half creates the unit and stamps `composite = false` if it
		 * cannot, so reaching here without one should be impossible. Checked
		 * anyway, because "impossible" here means dereferencing a null on the
		 * weave thread rather than dropping one frame's 2D band.
		 */
		if (c->outcomp == nullptr) {
			ZC_BAIL("outcomp");
		}
		if (!comp_d3d12_outcomp_ensure_weave_scratch(c->outcomp, region_w, region_h, (uint32_t)dd.Format)) {
			ZC_BAIL("g6");
		}
		void *weave_res = comp_d3d12_outcomp_snapshot_weave(c->outcomp, olist, dst, (uint32_t)dst_pre_state,
		                                                    region_w, region_h);
		if (weave_res == nullptr) {
			ZC_BAIL("snapshot");
		}
		xrt_result_t oxret = comp_d3d12_outcomp_composite_2d_masked(
		    c->outcomp, olist, dst, (uint32_t)dst_pre_state, twod_res, mask_res, weave_res, region_w, region_h,
		    (int32_t)cx_u, (int32_t)cy_u, cright - cx_u, cbottom - cy_u, composite_mode, opaque_present);
		/*
		 * The unit round-trips `dst` back into `dst_pre_state`. The caller's
		 * post-state contract is honoured because both split call sites pass
		 * RENDER_TARGET for pre and post; assert that rather than emit a barrier
		 * from a state the unit has already restored.
		 */
		if (dst_post_state != dst_pre_state) {
			D3D12_RESOURCE_BARRIER fix = {};
			fix.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			fix.Transition.pResource = dst;
			fix.Transition.StateBefore = dst_pre_state;
			fix.Transition.StateAfter = dst_post_state;
			fix.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			d3d12_zone_step("ResourceBarrier");
			olist->ResourceBarrier(1, &fix);
		}
		return oxret == XRT_SUCCESS;
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
	d3d12_zone_step("ResourceBarrier");
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
	d3d12_zone_step("ResourceBarrier");
	c->cmd_list->ResourceBarrier(2, weave_exit);

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
	d3d12_zone_step("ResourceBarrier");
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
	// #918 D12-5 — the object identity the bridge binds against, and the first
	// content generation. Both monotonic; see comp_d3d12_zone_mask.
	mask->res_gen = ++c->zone_mask_gen_next;
	mask->author_seq = 1;

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
	// No comp_xbridge_pre_plane_write here: a mask this call is still building
	// cannot be the plane's bound source, so there is no in-flight producer read
	// of it to order behind.
	d3d12_zone_mask_snapshot(mask, c->cmd_list);
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
	mask->author_seq++; // #918 D12-5 — see comp_d3d12_zone_mask::author_seq
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
	mask->author_seq++; // #918 D12-5 — see comp_d3d12_zone_mask::author_seq

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
	/*
	 * #918 D12-5 — the app is about to draw pixels the runtime will never see
	 * being drawn, so THIS is the honest hook for "the authored content changed":
	 * a Tier-3 app makes no other runtime call between acquiring the target and
	 * submitting. The submit that follows bumps it again, which is one redundant
	 * transport at worst and never a stale one.
	 */
	mask->author_seq++;
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

	/*
	 * #918 D12-5 (the D3D11 leg's #918 review, R1-adjacent) — `mask->staged` is a
	 * bridge PLANE SOURCE the producer reads directly, and this write runs from an
	 * OpenXR entry point of the app's, BEFORE layer_commit and therefore before
	 * the frame's comp_xbridge_pre_render back-fence. Nothing else orders it
	 * against the producer's in-flight read of the previous seq. Same fence, same
	 * mechanism, applied at the write instead — GPU-side on the app queue, so this
	 * thread does not block, and it must precede the ExecuteCommandLists below.
	 */
	if (c->split_active && c->xbridge != nullptr) {
		comp_xbridge_pre_plane_write(c->xbridge, COMP_XBRIDGE_PLANE_MASK);
	}
	d3d12_zone_mask_snapshot(mask, c->cmd_list);
	d3d12_zone_cmd_execute(c);

	mask->submitted = true;
	mask->author_seq++; // #918 D12-5 — staged now holds new pixels
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
	const bool was_active = c->active_zone_mask == mask;
	if (was_active) {
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
	if (was_active) {
		// #224 / #1175: withdraw this client's DP zone contribution NOW. The
		// session may never commit another frame (a teardown-path destroy), and
		// the per-frame sync would then leave the panel pinned by a dead client.
		// The CPU-side state above already says the mask is gone, so the resolve
		// comes back empty and this takes the clear edge exactly once.
		d3d12_sync_zone_mask_to_dp(c);
	}
	/*
	 * #918 D12-5: the bridge's producer may be reading mask->staged as the
	 * authored-mask plane source. Drop the binding BEFORE releasing the texture
	 * under it — fence-deferred, so this costs no wait.
	 *
	 * Only when the plane is bound to THIS mask (`res_gen` is unique per mask
	 * object, so the test is exact). Without that, destroying any other mask
	 * would unbind the live one and cost a full re-transport for nothing.
	 */
	if (c->split_active && c->xbridge != nullptr && c->mask_plane_gen == mask->res_gen) {
		comp_xbridge_bind_plane_resource(c->xbridge, COMP_XBRIDGE_PLANE_MASK, nullptr, 0,
		                                 (uint32_t)DXGI_FORMAT_R8_UNORM, 0, 0);
		c->mask_plane_live = false;
		c->mask_plane_gen = 0;
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
