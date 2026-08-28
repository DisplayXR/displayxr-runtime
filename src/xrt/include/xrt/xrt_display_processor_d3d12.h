// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor_d3d12 interface.
 *
 * D3D12 variant of the display processor abstraction for vendor-specific
 * atlas-to-display output processing (interlacing, SBS, anaglyph, etc.).
 *
 * Unlike the D3D11 variant, this interface operates on D3D12 resources:
 * - Input is an atlas texture SRV (GPU descriptor handle)
 * - Output goes to a render target (CPU descriptor handle for RTV)
 * - Commands are recorded onto a provided command list (deferred execution)
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_display_color.h"
#include "xrt/xrt_display_zones.h"
#include "xrt/xrt_display_scanout.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h> // offsetof — used by the ABI tripwire at the end of this header

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for types used by optional vtable methods.
struct xrt_eye_positions;
struct xrt_window_metrics;

/*
 * ── DP vendor-backend health states ────────────────────────────────────────
 *
 * Values reported through the per-API `get_backend_state` slot. Guarded so
 * every DP header can define them independently of include order (no MSVC
 * C4005 redefinition).
 */
#ifndef XRT_DP_BACKEND_STATE_OK
//! Backend connected and healthy.
#define XRT_DP_BACKEND_STATE_OK 0u
//! Backend down or reconnecting — the DP is handling it; surface only.
#define XRT_DP_BACKEND_STATE_DEGRADED 1u
//! Backend connection dead and unrecoverable in place — recreate the DP.
#define XRT_DP_BACKEND_STATE_STALE 2u
#endif

/*!
 * @interface xrt_display_processor_d3d12
 *
 * D3D12 display output processor that converts an atlas
 * texture into the final display output format.
 *
 * The compositor calls process_atlas() after rendering the view
 * pair into an atlas texture. The display processor records draw commands
 * onto the provided command list to write the final output.
 *
 * @ingroup xrt_iface
 */
struct xrt_display_processor_d3d12
{
	/*!
	 * `sizeof(struct xrt_display_processor_d3d12)` at the plug-in's compile
	 * time. Set by the plug-in factory; the runtime treats any slot at/past
	 * this offset as absent (NULL). 8-byte header (with @ref reserved_0) so
	 * @ref process_atlas lands at offset 8 on 64- and 32-bit. See ADR-020.
	 */
	uint32_t struct_size;

	/*! Reserved for alignment / future flags. Must be 0. */
	uint32_t reserved_0;

	/*!
	 * Process an atlas texture into the final display output.
	 *
	 * Records draw commands onto the provided command list. The caller
	 * is responsible for executing the command list and synchronization.
	 *
	 * @param      xdp                   Pointer to self.
	 * @param      d3d12_command_list     Command list (ID3D12GraphicsCommandList*).
	 * @param      atlas_texture_resource Atlas texture resource (ID3D12Resource*), may be NULL.
	 * @param      atlas_srv_gpu_handle  Atlas texture SRV (D3D12_GPU_DESCRIPTOR_HANDLE as uint64_t).
	 * @param      target_rtv_cpu_handle  Output render target RTV (D3D12_CPU_DESCRIPTOR_HANDLE as uint64_t).
	 * @param      view_width            Width of one eye view in pixels.
	 * @param      view_height           Height of one eye view in pixels.
	 * @param      tile_columns          Number of tile columns in the atlas layout.
	 * @param      tile_rows             Number of tile rows in the atlas layout.
	 * @param      format                DXGI format of the atlas texture (DXGI_FORMAT as
	 *                                   uint32_t). Atlas *encoding state* (ADR-021) is
	 *                                   conveyed via @ref set_atlas_encoding, not this
	 *                                   format; the output RT format is set_output_format.
	 * @param      target_width          Width of the output render target in pixels.
	 * @param      target_height         Height of the output render target in pixels.
	 * @param      canvas_offset_x       Canvas left edge in window client-area pixels (0 = no offset).
	 * @param      canvas_offset_y       Canvas top edge in window client-area pixels (0 = no offset).
	 * @param      canvas_width          Canvas width in pixels (0 = fills full window/target).
	 * @param      canvas_height         Canvas height in pixels (0 = fills full window/target).
	 */
	void (*process_atlas)(struct xrt_display_processor_d3d12 *xdp,
	                       void *d3d12_command_list,
	                       void *atlas_texture_resource,
	                       uint64_t atlas_srv_gpu_handle,
	                       uint64_t target_rtv_cpu_handle,
	                       void *target_resource,
	                       uint32_t view_width,
	                       uint32_t view_height,
	                       uint32_t tile_columns,
	                       uint32_t tile_rows,
	                       uint32_t format,
	                       uint32_t target_width,
	                       uint32_t target_height,
	                       int32_t canvas_offset_x,
	                       int32_t canvas_offset_y,
	                       uint32_t canvas_width,
	                       uint32_t canvas_height);

	/*!
	 * Set the output render target format.
	 *
	 * Must be called before the first process_atlas() call so the
	 * display processor can create its internal pipeline state.
	 * Optional — NULL means format is set during creation.
	 *
	 * @param xdp    Pointer to self.
	 * @param format DXGI format of the output render target (DXGI_FORMAT as uint32_t).
	 */
	void (*set_output_format)(struct xrt_display_processor_d3d12 *xdp, uint32_t format);

	/*!
	 * Get predicted eye positions from vendor eye tracking SDK.
	 * Optional — NULL means not supported.
	 */
	bool (*get_predicted_eye_positions)(struct xrt_display_processor_d3d12 *xdp,
	                                    struct xrt_eye_positions *out_eye_pos);

	/*!
	 * Get window metrics for adaptive FOV calculation.
	 * Optional — NULL means not supported.
	 */
	bool (*get_window_metrics)(struct xrt_display_processor_d3d12 *xdp,
	                           struct xrt_window_metrics *out_metrics);

	/*!
	 * Request a display mode switch (2D/3D).
	 * Optional — NULL means not supported.
	 */
	bool (*request_display_mode)(struct xrt_display_processor_d3d12 *xdp,
	                             bool enable_3d);

	/*!
	 * Query hardware 3D display state from vendor SDK.
	 * Optional — NULL means not supported.
	 */
	bool (*get_hardware_3d_state)(struct xrt_display_processor_d3d12 *xdp,
	                              bool *out_is_3d);

	/*!
	 * Get physical display dimensions in meters.
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_dimensions)(struct xrt_display_processor_d3d12 *xdp,
	                               float *out_width_m,
	                               float *out_height_m);

	/*!
	 * Get native display pixel info (resolution and screen position).
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_pixel_info)(struct xrt_display_processor_d3d12 *xdp,
	                               uint32_t *out_pixel_width,
	                               uint32_t *out_pixel_height,
	                               int32_t *out_screen_left,
	                               int32_t *out_screen_top);

	/*!
	 * Whether this display processor passes per-pixel alpha through to its
	 * output stage. true for sim_display-style processors; false (or NULL)
	 * for Leia-style weavers.
	 * Optional — NULL means false.
	 */
	bool (*is_alpha_native)(struct xrt_display_processor_d3d12 *xdp);

	/*!
	 * Destroy this display processor and free all resources.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*destroy)(struct xrt_display_processor_d3d12 *xdp);

	/*!
	 * Declare which atlas encoding state(s) this DP accepts at handoff
	 * (ADR-021 §3, @ref xrt_dp_color_capability). Optional — absent slot or
	 * NULL ⟹ @ref XRT_DP_COLOR_ENCODED. Appended per ADR-020.
	 */
	enum xrt_dp_color_capability (*get_handoff_color_capability)(struct xrt_display_processor_d3d12 *xdp);

	/*!
	 * Declare the atlas encoding for the next process_atlas (ADR-021 per-frame
	 * runtime intent; out-of-band so the format arg stays real). Optional —
	 * absent slot or NULL ⟹ DP assumes @ref XRT_ATLAS_ENCODING_ENCODED.
	 */
	void (*set_atlas_encoding)(struct xrt_display_processor_d3d12 *xdp, enum xrt_atlas_encoding atlas_encoding);

	/*!
	 * Hand the DP this frame's flattened 2D-under backdrop (#491 part 3).
	 * Called once per frame, immediately before @ref process_atlas, when the
	 * frame carries Local2D layers before the projection (the "under" layers).
	 * The runtime flattens them into a single premultiplied-RGBA texture in the
	 * client-window pixel space / canvas rect and passes the resource here. The
	 * DP composites it OVER its captured desktop background and uses the result
	 * as the under-3D background for the NEXT process_atlas. The resource must
	 * outlive that call and be left in a shader-readable state.
	 *
	 * Pass NULL (or width/height 0) to clear — desktop-only background.
	 *
	 * Optional — absent slot or NULL ⟹ no-op (part-1-only behavior). Appended
	 * per ADR-020 (append-only within a major).
	 *
	 * @param xdp                  Pointer to self.
	 * @param background_resource  ID3D12Resource* of the flattened backdrop (or NULL to clear).
	 * @param width                Backdrop width in pixels.
	 * @param height               Backdrop height in pixels.
	 */
	void (*set_background_2d)(struct xrt_display_processor_d3d12 *xdp,
	                          void *background_resource,
	                          uint32_t width,
	                          uint32_t height);

	/*!
	 * Select the eye-tracking control mode (MANAGED=0 / MANUAL=1) — the policy
	 * counterpart to @ref request_display_mode. See the base
	 * @ref xrt_display_processor::set_eye_tracking_mode and
	 * docs/specs/vendor/eye-tracking-modes.md. Optional — absent slot or NULL
	 * means the DP doesn't react. Appended per ADR-020 (append-only).
	 *
	 * @param xdp   Pointer to self.
	 * @param mode  0 = MANAGED, 1 = MANUAL.
	 */
	void (*set_eye_tracking_mode)(struct xrt_display_processor_d3d12 *xdp, uint32_t mode);

	/*!
	 * Query the DP's local 2D/3D-zone capability (#224 Phase 0, ADR-027
	 * Decision 5 — D3D12 port of the D3D11 slot). The caller pre-sets
	 * @ref xrt_dp_local_zone_caps::struct_size; the DP writes only fields
	 * within it.
	 *
	 * Optional — absent slot (older plug-in `struct_size`) or NULL ⟹ legacy
	 * DP: the runtime keeps the global request_display_mode path (tier-1
	 * fallback) and never calls the zone publish methods. Appended per
	 * ADR-020 (append-only within a major).
	 *
	 * @param      xdp       Pointer to self.
	 * @param[out] out_caps  Filled by the DP (struct_size pre-set by caller).
	 * @return true if @p out_caps was filled.
	 */
	bool (*get_local_zone_caps)(struct xrt_display_processor_d3d12 *xdp,
	                            struct xrt_dp_local_zone_caps *out_caps);

	/*!
	 * Publish this client's screen-anchored 3D-zone mask (#224 Phase 0;
	 * wish semantics per ADR-027 Decision 5 — D3D12 port of the D3D11
	 * slot, mask handle following this header's @ref set_background_2d
	 * ID3D12Resource* convention).
	 *
	 * The runtime owns the mask resource (R8_UNORM, client-window pixels)
	 * and passes it here in PIXEL_SHADER_RESOURCE state with all producing
	 * GPU work fence-completed — the DP samples or copies it DURING this
	 * call (on its own list/queue if it needs GPU access) and must not hold
	 * the pointer past return. @p screen_x/y/w/h anchor the mask's pixel
	 * space on the panel in physical screen pixels (post-DPI client rect).
	 * @p seq is the mask CONTENT generation — monotonic, bumped only when
	 * the published content changes (xrSubmitLocal3DZoneDXR, a wish
	 * re-raster, an explicit-wish change); same-seq publishes differ only
	 * in the screen anchor, so a vendor evaluates content once per
	 * generation.
	 *
	 * Wish semantics (ADR-027): the published R8 mask is the WISH — per-
	 * pixel M ∈ [0,1], 1 = panel physically 3D, 0 = flat, intermediate =
	 * fractional 3D-ness at the DP's discretion (declared via
	 * @ref xrt_dp_local_zone_caps::wish_fractional). The existing
	 * downsample-and-arbitrate rule (any non-zero mask pixel overlapping a
	 * hardware cell ⟹ cell 3D, OR union across clients) is the DEFAULT
	 * (conformant) quantization of the wish. The runtime republishes every
	 * frame while a mask is active; vendors coalesce per max_update_hz.
	 *
	 * Optional — absent slot or NULL ⟹ not supported (see
	 * @ref get_local_zone_caps). Appended per ADR-020.
	 *
	 * @param xdp            Pointer to self.
	 * @param mask_resource  Mask resource (ID3D12Resource*, R8_UNORM, PIXEL_SHADER_RESOURCE).
	 * @param mask_width     Mask width in client-window pixels.
	 * @param mask_height    Mask height in client-window pixels.
	 * @param screen_x       Client-area left edge in physical screen pixels.
	 * @param screen_y       Client-area top edge in physical screen pixels.
	 * @param screen_w       Client-area width in physical screen pixels.
	 * @param screen_h       Client-area height in physical screen pixels.
	 * @param seq            Mask content generation.
	 * @return true if the publish was accepted.
	 */
	bool (*publish_local_zone_mask)(struct xrt_display_processor_d3d12 *xdp,
	                                void *mask_resource,
	                                uint32_t mask_width,
	                                uint32_t mask_height,
	                                int32_t screen_x,
	                                int32_t screen_y,
	                                uint32_t screen_w,
	                                uint32_t screen_h,
	                                uint64_t seq);

	/*!
	 * Withdraw this client's zone contribution (#224 Phase 0) — equivalent
	 * to (and cheaper than) publishing an all-zero mask. Called when the
	 * active mask is destroyed or the session ends.
	 *
	 * Optional — absent slot or NULL ⟹ not supported. Appended per ADR-020.
	 *
	 * @param xdp Pointer to self.
	 * @return true if the clear was accepted.
	 */
	bool (*clear_local_zone_mask)(struct xrt_display_processor_d3d12 *xdp);

	/*!
	 * Enable/disable transparent-background output for this client (#573 — the
	 * D3D12 counterpart of @ref xrt_display_processor_d3d11::set_transparent_background).
	 * When enabled, the DP composites its weave OVER the captured desktop behind
	 * the app window (compose-under-background, from the atlas's premultiplied
	 * alpha) so transparent atlas regions show the desktop. This is the policy
	 * signal; the runtime separately guarantees the app window is excluded from
	 * the DP's desktop capture (SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE),
	 * set from the window-owning process).
	 *
	 * This is the sole transparency enable (#573 removed the legacy chroma-key
	 * path, which on D3D12 had been overloaded as the enable signal). Optional —
	 * absent slot or NULL ⟹ the DP doesn't support transparent output. Appended
	 * per ADR-020.
	 *
	 * @param xdp             Pointer to self.
	 * @param enabled         true to produce transparent output for see-through.
	 * @param client_presents true ⟹ the runtime owns a transparent present that
	 *                        DWM-blends the live desktop into alpha=0 holes; the DP
	 *                        then only reconstructs alpha (the alpha-gate), never
	 *                        composing its own captured-desktop background. false ⟹
	 *                        the DP owns see-through itself (compose-under-bg).
	 */
	void (*set_transparent_background)(struct xrt_display_processor_d3d12 *xdp, bool enabled, bool client_presents);

	/*!
	 * Tell the DP whether the on-screen presentation shows ONLY the canvas
	 * (the app self-presents its shared texture's canvas sub-region to its own
	 * window) vs the whole weave target (handle apps). A `_texture` app blits the
	 * shared texture and Presents itself, and for a zones frame the canvas IS the
	 * whole window — so the DP's compose-under-bg desktop-UV remap (which assumes
	 * the window shows the whole panel-sized target) must be skipped or the
	 * captured desktop is magnified (#68). Set once at DP setup from the
	 * compositor's `has_shared_texture`. Optional — absent slot or NULL ⟹ the DP
	 * assumes the full target is presented. Appended per ADR-020 (append-only).
	 */
	void (*set_shared_texture_present)(struct xrt_display_processor_d3d12 *xdp, bool enabled);
	/*!
	 * Per-frame presentation-timing feedback for the weave-latency control
	 * loop. The runtime reports the MEASURED weave→scanout residual of the
	 * most recently completed frame (DXGI frame statistics: SyncQPCTime of
	 * the last flipped present minus that frame's weave-record time) plus
	 * the display refresh period, so the DP can feed the vendor eye
	 * predictor an exact motion-to-photon horizon (setLatency) instead of a
	 * heuristic. Under late-weave pacing the residual sits at ~1 refresh;
	 * unpaced paths report their true (larger) value — the point of
	 * measuring rather than assuming. Called at most once per frame, before
	 * process_atlas.
	 *
	 * Optional — absent slot (older plug-in `struct_size`) or NULL ⇒ the DP
	 * keeps its own horizon heuristic. Appended per ADR-020.
	 *
	 * @param xdp                  Pointer to self.
	 * @param weave_to_scanout_ns  Measured residual of the last completed
	 *                             frame; 0 = unknown / not yet measured.
	 * @param frame_period_ns      Display refresh period; 0 = unknown.
	 */
	void (*set_frame_timing)(struct xrt_display_processor_d3d12 *xdp,
	                         uint64_t weave_to_scanout_ns,
	                         uint64_t frame_period_ns);

	/*!
	 * Report the health of the DP's vendor backend — the vendor platform
	 * service/session this DP is connected to — so the runtime can react
	 * when that backend restarts or degrades underneath a long-lived
	 * process (a service upgrade, a driver reset, a session teardown).
	 *
	 * States written to @p out_state:
	 * - @ref XRT_DP_BACKEND_STATE_OK — connected and healthy.
	 * - @ref XRT_DP_BACKEND_STATE_DEGRADED — backend down or reconnecting;
	 *   the DP is handling it and output may be temporarily untracked / 2D.
	 *   No action is needed beyond surfacing it.
	 * - @ref XRT_DP_BACKEND_STATE_STALE — the DP's backend connection is
	 *   dead and the DP cannot recover in place; destroying and recreating
	 *   the DP is the remedy.
	 *
	 * Must be cheap and non-blocking — the runtime polls it at ~1 Hz from
	 * the RENDER THREAD.
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL, or a
	 * false return, means "unknown": the runtime treats that as OK.
	 * Appended per ADR-020.
	 *
	 * @param      xdp        Pointer to self.
	 * @param[out] out_state  One of the XRT_DP_BACKEND_STATE_* values.
	 * @return true when @p out_state was written.
	 */
	bool (*get_backend_state)(struct xrt_display_processor_d3d12 *xdp, uint32_t *out_state);

	/*!
	 * Declare how much of the panel this DP's output transform covers
	 * (@ref xrt_dp_weave_scope) — i.e. whether the runtime may present it in
	 * a window at all.
	 *
	 * A GPU weaver produces final pixels for the canvas it was handed and
	 * leaves the rest of the screen alone, so it is windowable by
	 * construction: it leaves this slot NULL and the runtime reads
	 * @ref XRT_DP_WEAVE_SCOPE_CANVAS. A DP driving a display that weaves in
	 * its own hardware is instead emitting a PACKED frame (the atlas repacked
	 * into the layout its chip expects, per the tile geometry the plug-in
	 * already declares in @ref xrt_rendering_mode) and must say whether that
	 * chip can be pointed at a rectangle (@ref XRT_DP_WEAVE_SCOPE_REGION —
	 * windowed output is fine) or transforms the whole scanout
	 * (@ref XRT_DP_WEAVE_SCOPE_SCANOUT — only a panel-scoped, fullscreen
	 * presentation can be correct).
	 *
	 * The caller pre-sets @ref xrt_dp_scanout_caps::struct_size; the DP writes
	 * only fields within it and MUST zero @ref xrt_dp_scanout_caps::reserved.
	 * Cheap — queried once at DP setup, never per frame.
	 *
	 * Optional — absent slot (older plug-in `struct_size`), NULL, or a false
	 * return all mean @ref XRT_DP_WEAVE_SCOPE_CANVAS, which is exactly the
	 * behaviour every existing plug-in already has. Appended per ADR-020
	 * (append-only within a major; no version bump).
	 *
	 * @param      xdp       Pointer to self.
	 * @param[out] out_caps  Filled by the DP (struct_size pre-set by caller).
	 * @return true if @p out_caps was filled.
	 */
	bool (*get_scanout_caps)(struct xrt_display_processor_d3d12 *xdp, struct xrt_dp_scanout_caps *out_caps);

	/*!
	 * #206: the FORWARD-computed weave→scanout time of THIS weave, from the
	 * runtime's vsync-locked vblank grid — the exact per-weave horizon for
	 * the vendor eye predictor, to be fed RAW (no smoothing, no deadband).
	 * 0 = no trusted grid this frame ⟹ the DP keeps its retrospective
	 * heuristic. Called after @ref set_frame_timing, before
	 * @ref xrt_display_processor::process_atlas. Full contract: the D3D11
	 * variant's doc. Appended after @ref get_scanout_caps per ADR-020
	 * (append-only within a major; no version bump).
	 *
	 * @param xdp                            Pointer to self.
	 * @param predicted_weave_to_scanout_ns  Forward horizon; 0 = unknown.
	 */
	void (*set_predicted_scanout)(struct xrt_display_processor_d3d12 *xdp,
	                              uint64_t predicted_weave_to_scanout_ns);

};


/*!
 * Defined when this header carries the @ref xrt_display_processor_d3d12::get_scanout_caps
 * slot, so a plug-in that must also compile against an older runtime header can
 * `#ifdef`-guard its implementation — the coupled-ABI-addition pattern used by
 * the other appended slots.
 */
#define XRT_DP_D3D12_HAS_SCANOUT_CAPS 1

/*
 * ── Plug-in ABI tripwire (ADR-020) ─────────────────────────────────────────
 *
 * Per-API DP vtable; part of the same versioned plug-in ABI as the base
 * @ref xrt_display_processor. As of ABI major v2 it carries the 8-byte
 * struct_size header, so appending a method at the END is compatible within a
 * major; any other layout change is breaking and must bump
 * XRT_PLUGIN_API_VERSION_CURRENT (xrt_plugin.h) + re-pin every plug-in. Note
 * D3D12 has an extra `set_output_format` slot (index 1) the other APIs omit.
 */
#ifndef XRT_DP_ABI_ASSERT
#if defined(__cplusplus)
#define XRT_DP_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define XRT_DP_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#endif
#ifndef XRT_DP_ABI_MSG
#define XRT_DP_ABI_MSG                                                                                                  \
	"xrt_display_processor ABI changed — see ADR-020: bump XRT_PLUGIN_API_VERSION_CURRENT and re-pin every plug-in."
#endif
#ifndef XRT_DP_HAS_SLOT
#define XRT_DP_HAS_SLOT(xdp, field)                                                                                    \
	((xdp) != NULL && ((const char *)&(xdp)->field + sizeof((xdp)->field)) <=                                       \
	                      ((const char *)(xdp) + (xdp)->struct_size))
#endif

#define XRT_DP_D3D12_BASE_OFF offsetof(struct xrt_display_processor_d3d12, process_atlas)
// clang-format off
XRT_DP_ABI_ASSERT(XRT_DP_D3D12_BASE_OFF == 8, XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, process_atlas)               == XRT_DP_D3D12_BASE_OFF +  0 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, set_output_format)           == XRT_DP_D3D12_BASE_OFF +  1 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, get_predicted_eye_positions) == XRT_DP_D3D12_BASE_OFF +  2 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, get_window_metrics)          == XRT_DP_D3D12_BASE_OFF +  3 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, request_display_mode)        == XRT_DP_D3D12_BASE_OFF +  4 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, get_hardware_3d_state)       == XRT_DP_D3D12_BASE_OFF +  5 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, get_display_dimensions)      == XRT_DP_D3D12_BASE_OFF +  6 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, get_display_pixel_info)      == XRT_DP_D3D12_BASE_OFF +  7 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, is_alpha_native)             == XRT_DP_D3D12_BASE_OFF +  8 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, destroy)                     == XRT_DP_D3D12_BASE_OFF +  9 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, get_handoff_color_capability) == XRT_DP_D3D12_BASE_OFF + 10 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, set_atlas_encoding)           == XRT_DP_D3D12_BASE_OFF + 11 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, set_background_2d)            == XRT_DP_D3D12_BASE_OFF + 12 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, set_eye_tracking_mode)        == XRT_DP_D3D12_BASE_OFF + 13 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, get_local_zone_caps)          == XRT_DP_D3D12_BASE_OFF + 14 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, publish_local_zone_mask)      == XRT_DP_D3D12_BASE_OFF + 15 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, clear_local_zone_mask)        == XRT_DP_D3D12_BASE_OFF + 16 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, set_transparent_background)   == XRT_DP_D3D12_BASE_OFF + 17 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, set_shared_texture_present)   == XRT_DP_D3D12_BASE_OFF + 18 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, set_frame_timing)            == XRT_DP_D3D12_BASE_OFF + 19 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, get_backend_state)           == XRT_DP_D3D12_BASE_OFF + 20 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, get_scanout_caps)            == XRT_DP_D3D12_BASE_OFF + 21 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d12, set_predicted_scanout)       == XRT_DP_D3D12_BASE_OFF + 22 * sizeof(void *), XRT_DP_ABI_MSG);

/*!
 * Defined when this header carries the set_predicted_scanout slot (#206) —
 * same coupled-ABI-addition pattern as @ref XRT_DP_D3D12_HAS_FRAME_TIMING.
 */
#define XRT_DP_D3D12_HAS_PREDICTED_SCANOUT 1
XRT_DP_ABI_ASSERT(sizeof(struct xrt_display_processor_d3d12)                                == XRT_DP_D3D12_BASE_OFF + 22 * sizeof(void *), XRT_DP_ABI_MSG);

/*!
 * Defined when this header carries the set_frame_timing slot, so a plug-in
 * built against an older runtime can #ifdef-guard its implementation — the
 * coupled-ABI-addition pattern (see XRT_DP_VK_HAS_PRESENT_ORIGIN).
 */
#define XRT_DP_D3D12_HAS_FRAME_TIMING 1

/*!
 * Defined when this header carries the get_backend_state slot, so a plug-in
 * built against an older runtime can #ifdef-guard its implementation — the
 * coupled-ABI-addition pattern (see XRT_DP_VK_HAS_PRESENT_ORIGIN).
 */
#define XRT_DP_D3D12_HAS_BACKEND_STATE 1

// clang-format on

/*!
 * @copydoc xrt_display_processor_d3d12::process_atlas
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_process_atlas(struct xrt_display_processor_d3d12 *xdp,
                                           void *d3d12_command_list,
                                           void *atlas_texture_resource,
                                           uint64_t atlas_srv_gpu_handle,
                                           uint64_t target_rtv_cpu_handle,
                                           void *target_resource,
                                           uint32_t view_width,
                                           uint32_t view_height,
                                           uint32_t tile_columns,
                                           uint32_t tile_rows,
                                           uint32_t format,
                                           uint32_t target_width,
                                           uint32_t target_height,
                                           int32_t canvas_offset_x,
                                           int32_t canvas_offset_y,
                                           uint32_t canvas_width,
                                           uint32_t canvas_height)
{
	xdp->process_atlas(xdp, d3d12_command_list, atlas_texture_resource, atlas_srv_gpu_handle,
	                    target_rtv_cpu_handle, target_resource,
	                    view_width, view_height, tile_columns, tile_rows, format,
	                    target_width, target_height, canvas_offset_x, canvas_offset_y, canvas_width,
	                    canvas_height);
}

/*!
 * @copydoc xrt_display_processor_d3d12::set_output_format
 * No-op if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_set_output_format(struct xrt_display_processor_d3d12 *xdp, uint32_t format)
{
	if (XRT_DP_HAS_SLOT(xdp, set_output_format) && xdp->set_output_format != NULL) {
		xdp->set_output_format(xdp, format);
	}
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_predicted_eye_positions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_predicted_eye_positions(struct xrt_display_processor_d3d12 *xdp,
                                                        struct xrt_eye_positions *out_eye_pos)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_predicted_eye_positions) || xdp->get_predicted_eye_positions == NULL) {
		return false;
	}
	return xdp->get_predicted_eye_positions(xdp, out_eye_pos);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_window_metrics
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_window_metrics(struct xrt_display_processor_d3d12 *xdp,
                                               struct xrt_window_metrics *out_metrics)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_window_metrics) || xdp->get_window_metrics == NULL) {
		return false;
	}
	return xdp->get_window_metrics(xdp, out_metrics);
}

/*!
 * @copydoc xrt_display_processor_d3d12::request_display_mode
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_request_display_mode(struct xrt_display_processor_d3d12 *xdp, bool enable_3d)
{
	if (!XRT_DP_HAS_SLOT(xdp, request_display_mode) || xdp->request_display_mode == NULL) {
		return false;
	}
	return xdp->request_display_mode(xdp, enable_3d);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_hardware_3d_state
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_hardware_3d_state(struct xrt_display_processor_d3d12 *xdp,
                                                  bool *out_is_3d)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_hardware_3d_state) || xdp->get_hardware_3d_state == NULL) {
		return false;
	}
	return xdp->get_hardware_3d_state(xdp, out_is_3d);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_display_dimensions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_display_dimensions(struct xrt_display_processor_d3d12 *xdp,
                                                   float *out_width_m,
                                                   float *out_height_m)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_display_dimensions) || xdp->get_display_dimensions == NULL) {
		return false;
	}
	return xdp->get_display_dimensions(xdp, out_width_m, out_height_m);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_display_pixel_info
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_display_pixel_info(struct xrt_display_processor_d3d12 *xdp,
                                                   uint32_t *out_pixel_width,
                                                   uint32_t *out_pixel_height,
                                                   int32_t *out_screen_left,
                                                   int32_t *out_screen_top)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_display_pixel_info) || xdp->get_display_pixel_info == NULL) {
		return false;
	}
	return xdp->get_display_pixel_info(xdp, out_pixel_width, out_pixel_height, out_screen_left, out_screen_top);
}

/*!
 * @copydoc xrt_display_processor_d3d12::is_alpha_native
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_is_alpha_native(struct xrt_display_processor_d3d12 *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, is_alpha_native) || xdp->is_alpha_native == NULL) {
		return false;
	}
	return xdp->is_alpha_native(xdp);
}

/*!
 * @copydoc xrt_display_processor_d3d12::set_transparent_background
 * Returns false if not supported (slot absent or NULL) — the caller then
 * leaves the DP opaque.
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_set_transparent_background(struct xrt_display_processor_d3d12 *xdp,
                                                       bool enabled,
                                                       bool client_presents)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_transparent_background) || xdp->set_transparent_background == NULL) {
		return false;
	}
	xdp->set_transparent_background(xdp, enabled, client_presents);
	return true;
}

/*!
 * @copydoc xrt_display_processor_d3d12::set_shared_texture_present
 *
 * Helper for calling through the function pointer. Returns false if the slot is
 * absent (older plug-in) or NULL — the DP then keeps its full-target assumption.
 *
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_set_shared_texture_present(struct xrt_display_processor_d3d12 *xdp, bool enabled)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_shared_texture_present) || xdp->set_shared_texture_present == NULL) {
		return false;
	}
	xdp->set_shared_texture_present(xdp, enabled);
	return true;
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_handoff_color_capability
 * Returns @ref XRT_DP_COLOR_ENCODED if not supported (slot absent or NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline enum xrt_dp_color_capability
xrt_display_processor_d3d12_get_handoff_color_capability(struct xrt_display_processor_d3d12 *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_handoff_color_capability) || xdp->get_handoff_color_capability == NULL) {
		return XRT_DP_COLOR_ENCODED;
	}
	return xdp->get_handoff_color_capability(xdp);
}

/*!
 * @copydoc xrt_display_processor_d3d12::set_atlas_encoding
 * No-op if not supported (slot absent or NULL) — the DP then assumes ENCODED.
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_set_atlas_encoding(struct xrt_display_processor_d3d12 *xdp,
                                               enum xrt_atlas_encoding atlas_encoding)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_atlas_encoding) || xdp->set_atlas_encoding == NULL) {
		return;
	}
	xdp->set_atlas_encoding(xdp, atlas_encoding);
}

/*!
 * @copydoc xrt_display_processor_d3d12::set_background_2d
 * No-op when the DP doesn't expose the slot (older plug-in) or leaves it NULL.
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_set_background_2d(struct xrt_display_processor_d3d12 *xdp,
                                              void *background_resource,
                                              uint32_t width,
                                              uint32_t height)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_background_2d) || xdp->set_background_2d == NULL) {
		return;
	}
	xdp->set_background_2d(xdp, background_resource, width, height);
}

/*!
 * @copydoc xrt_display_processor_d3d12::set_eye_tracking_mode
 * No-op when the DP doesn't expose the slot (older plug-in) or leaves it NULL.
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_set_eye_tracking_mode(struct xrt_display_processor_d3d12 *xdp, uint32_t mode)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_eye_tracking_mode) || xdp->set_eye_tracking_mode == NULL) {
		return;
	}
	xdp->set_eye_tracking_mode(xdp, mode);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_local_zone_caps
 * Returns false (legacy DP — caps untouched) if not supported (slot absent or
 * NULL). The caller must zero @p out_caps and pre-set out_caps->struct_size.
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_local_zone_caps(struct xrt_display_processor_d3d12 *xdp,
                                                struct xrt_dp_local_zone_caps *out_caps)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_local_zone_caps) || xdp->get_local_zone_caps == NULL) {
		return false;
	}
	return xdp->get_local_zone_caps(xdp, out_caps);
}

/*!
 * @copydoc xrt_display_processor_d3d12::publish_local_zone_mask
 * Returns false if not supported (slot absent or NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_publish_local_zone_mask(struct xrt_display_processor_d3d12 *xdp,
                                                    void *mask_resource,
                                                    uint32_t mask_width,
                                                    uint32_t mask_height,
                                                    int32_t screen_x,
                                                    int32_t screen_y,
                                                    uint32_t screen_w,
                                                    uint32_t screen_h,
                                                    uint64_t seq)
{
	if (!XRT_DP_HAS_SLOT(xdp, publish_local_zone_mask) || xdp->publish_local_zone_mask == NULL) {
		return false;
	}
	return xdp->publish_local_zone_mask(xdp, mask_resource, mask_width, mask_height, screen_x, screen_y, screen_w,
	                                    screen_h, seq);
}

/*!
 * @copydoc xrt_display_processor_d3d12::clear_local_zone_mask
 * Returns false if not supported (slot absent or NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_clear_local_zone_mask(struct xrt_display_processor_d3d12 *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, clear_local_zone_mask) || xdp->clear_local_zone_mask == NULL) {
		return false;
	}
	return xdp->clear_local_zone_mask(xdp);
}

/*!
 * Factory function type for creating a D3D12 display processor.
 *
 * Called by the compositor to create a display processor for a session.
 * The factory is set by the target builder at init time and stored in
 * xrt_system_compositor_info.
 *
 * @param d3d12_device        D3D12 device (ID3D12Device*).
 * @param d3d12_command_queue D3D12 command queue (ID3D12CommandQueue*).
 * @param window_handle       Native window handle (HWND), may be NULL.
 * @param[out] out_xdp        Created display processor on success.
 * @return XRT_SUCCESS on success.
 */
typedef xrt_result_t (*xrt_dp_factory_d3d12_fn_t)(void *d3d12_device,
                                                   void *d3d12_command_queue,
                                                   void *window_handle,
                                                   struct xrt_display_processor_d3d12 **out_xdp);

/*!
 * Destroy an xrt_display_processor_d3d12 — helper function.
 *
 * @param[in,out] xdp_ptr  A pointer to your display processor pointer.
 *
 * Will destroy the processor if *xdp_ptr is not NULL.
 * Will then set *xdp_ptr to NULL.
 *
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_destroy(struct xrt_display_processor_d3d12 **xdp_ptr)
{
	struct xrt_display_processor_d3d12 *xdp = *xdp_ptr;
	if (xdp == NULL) {
		return;
	}

	xdp->destroy(xdp);
	*xdp_ptr = NULL;
}



/*!
 * @copydoc xrt_display_processor_d3d12::set_frame_timing
 * No-op if not supported (slot absent or NULL) — the DP then keeps its own
 * horizon heuristic.
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_set_frame_timing(struct xrt_display_processor_d3d12 *xdp,
                                           uint64_t weave_to_scanout_ns,
                                           uint64_t frame_period_ns)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_frame_timing) || xdp->set_frame_timing == NULL) {
		return;
	}
	xdp->set_frame_timing(xdp, weave_to_scanout_ns, frame_period_ns);
}

/*!
 * @copydoc xrt_display_processor_d3d12::set_predicted_scanout
 * No-op if not supported (slot absent or NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_set_predicted_scanout(struct xrt_display_processor_d3d12 *xdp,
                                                  uint64_t predicted_weave_to_scanout_ns)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_predicted_scanout) || xdp->set_predicted_scanout == NULL) {
		return;
	}
	xdp->set_predicted_scanout(xdp, predicted_weave_to_scanout_ns);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_backend_state
 * Returns false if not supported (slot absent or NULL) — the caller then
 * treats the backend state as unknown, i.e. @ref XRT_DP_BACKEND_STATE_OK.
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_backend_state(struct xrt_display_processor_d3d12 *xdp, uint32_t *out_state)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_backend_state) || xdp->get_backend_state == NULL) {
		return false;
	}
	return xdp->get_backend_state(xdp, out_state);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_scanout_caps
 *
 * Returns false when the slot is absent (older plug-in `struct_size`), NULL, or
 * the DP declined — in every one of those cases the caller must read the scope
 * as @ref XRT_DP_WEAVE_SCOPE_CANVAS. Prefer
 * @ref xrt_display_processor_d3d12_get_weave_scope, which does that for you.
 *
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_scanout_caps(struct xrt_display_processor_d3d12 *xdp,
                                             struct xrt_dp_scanout_caps *out_caps)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_scanout_caps) || xdp->get_scanout_caps == NULL) {
		return false;
	}
	return xdp->get_scanout_caps(xdp, out_caps);
}

/*!
 * The DP's @ref xrt_dp_weave_scope, with every "didn't answer" case resolved to
 * @ref XRT_DP_WEAVE_SCOPE_CANVAS. This is the accessor call sites should use.
 *
 * @public @memberof xrt_display_processor_d3d12
 */
static inline enum xrt_dp_weave_scope
xrt_display_processor_d3d12_get_weave_scope(struct xrt_display_processor_d3d12 *xdp)
{
	struct xrt_dp_scanout_caps caps;
	xrt_dp_scanout_caps_init(&caps);
	if (!xrt_display_processor_d3d12_get_scanout_caps(xdp, &caps)) {
		return XRT_DP_WEAVE_SCOPE_CANVAS;
	}
	return xrt_dp_weave_scope_clamp(caps.weave_scope);
}

#ifdef __cplusplus
}
#endif
