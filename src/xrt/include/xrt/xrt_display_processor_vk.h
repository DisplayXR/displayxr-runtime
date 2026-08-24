// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor_vk interface.
 *
 * Vulkan/Android variant of the display processor abstraction. Unlike the
 * standalone @ref xrt_display_processor_d3d11 (D3D11 has no shared base vtable),
 * the *generic* @ref xrt_display_processor IS already the Vulkan interface —
 * its @ref xrt_display_processor::process_atlas records into a VkCommandBuffer.
 * So this variant simply **embeds** the generic base at offset 0 and **appends**
 * one slot: @ref set_transparent_background.
 *
 * A plug-in opts into the variant by setting @ref xrt_display_processor::struct_size
 * (the embedded base's header) to `sizeof(struct xrt_display_processor_vk)`. The
 * runtime casts a @ref xrt_display_processor* it received from a Vulkan factory
 * to this type and uses the reported struct_size to decide whether the appended
 * slot is present (ADR-020 — exactly the per-API struct_size gate used by
 * @ref xrt_display_processor_d3d11). A DP that reports only
 * `sizeof(struct xrt_display_processor)` (sim_display, an older Leia plug-in) is
 * transparently treated as *not* the variant: the appended slot's bytes fall
 * past struct_size, so the helper below reports it absent.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_display_processor.h" // the embedded base + the XRT_DP_ABI_ASSERT/XRT_DP_ABI_MSG macros

#include <stdbool.h>
#include <stddef.h> // offsetof — used by the ABI tripwire at the end of this header

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @interface xrt_display_processor_vk
 *
 * Vulkan/Android display output processor — the generic @ref xrt_display_processor
 * plus the transparency-enable slot. The compositor calls the base vtable
 * (process_atlas, …) exactly as for any Vulkan DP; the appended slot is the
 * policy signal that turns on transparent-background output (the alpha-gate
 * post-weave pass on Leia).
 *
 * @ingroup xrt_iface
 */
struct xrt_display_processor_vk
{
	/*!
	 * The generic Vulkan display-processor vtable. MUST be the first member
	 * (offset 0) so a @ref xrt_display_processor* and a
	 * @ref xrt_display_processor_vk* are interchangeable. @ref base.struct_size
	 * is the 8-byte struct_size header that gates this variant's appended slot
	 * (set it to `sizeof(struct xrt_display_processor_vk)` to advertise the
	 * variant; see ADR-020 and @ref XRT_DP_HAS_SLOT).
	 */
	struct xrt_display_processor base;

	/*!
	 * Enable/disable transparent-background output for this client (#568, the
	 * Android port of the D3D11 ADR-029 slot). When enabled, the DP
	 * reconstructs per-pixel alpha *after* the weave (which destroys alpha): a
	 * fullscreen alpha-gate pass samples the original premultiplied atlas and
	 * writes (0,0,0,0) wherever every view tile is α==0 (so the platform
	 * compositor — SurfaceFlinger on Android — shows the live screen through
	 * the holes), else the woven RGB at α=1. This is the sole transparency
	 * enable (#573 removed the legacy chroma-key path everywhere).
	 *
	 * @p client_presents mirrors the D3D11 slot for symmetry, but on Android it
	 * is **always true**: SurfaceFlinger composites the runtime's translucent
	 * surface over the home screen for free (the in-process model, ADR-025), so
	 * the final present already blends the live screen into the α=0 holes. The
	 * DP therefore only reconstructs alpha (the alpha-gate) — it never composes
	 * its own captured-desktop background (which would bake a stale frame and
	 * add latency). The 2-arg signature is kept so the variant matches
	 * @ref xrt_display_processor_d3d11::set_transparent_background; callers on
	 * Android pass true.
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL ⟹ the DP
	 * doesn't support transparent output. Appended per ADR-020 (append-only
	 * within a major).
	 *
	 * @param xdp             Pointer to self.
	 * @param enabled         true to produce transparent output for see-through.
	 * @param client_presents true ⟹ the platform present blends the live screen
	 *                        into the α=0 holes (always true on Android). The DP
	 *                        then only reconstructs alpha, never composes its own
	 *                        background.
	 */
	void (*set_transparent_background)(struct xrt_display_processor_vk *xdp, bool enabled, bool client_presents);

	/*!
	 * Notify the DP that the compositor's target image set was (re)created
	 * (#602) — e.g. a window resize rebuilt the swapchain / DComp-bridge ring.
	 * Any cache the DP keys by the target VkImage handle MUST be invalidated
	 * here: Vulkan recycles freed image handles, so a cache entry keyed by a
	 * now-destroyed image can alias a fresh image of the same handle and fault
	 * the device when rendered through (observed as VK_ERROR_DEVICE_LOST in the
	 * compositor's next queue submit). Leia's post-weave alpha-gate "strip"
	 * framebuffer cache is exactly such a cache.
	 *
	 * Called on the compositor's frame thread immediately after the resize, with
	 * the device idle (the compositor `vkDeviceWaitIdle`s as part of the target
	 * rebuild), so the DP may destroy VkFramebuffer / VkImageView objects
	 * synchronously inside this call. @p generation is monotonic; a DP that
	 * already flushed for it can no-op (the call is idempotent).
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL ⟹ the DP
	 * keeps no target-handle-keyed cache and needs no notification. Appended
	 * after @ref set_transparent_background per ADR-020 (append-only within a
	 * major; no version bump — gated by the variant's `base.struct_size`).
	 *
	 * @param xdp         Pointer to self.
	 * @param generation  Monotonic target image-set generation.
	 */
	void (*notify_target_recreated)(struct xrt_display_processor_vk *xdp, uint32_t generation);

	/*!
	 * Tell the DP whether the on-screen presentation shows ONLY the canvas (the
	 * app self-presents its shared texture's canvas sub-region to its own window)
	 * vs the whole weave target (handle apps, where the runtime presents the full
	 * swapchain). The Vulkan port of the D3D11 slot (#68): a `_texture` app blits
	 * the shared texture and Presents itself, and for a zones frame the canvas IS
	 * the whole window — so the DP's compose-under-bg desktop-UV remap (which
	 * assumes the window shows the whole panel-sized target) must be skipped for a
	 * `shared_texture_present && zone_active` frame, or the captured desktop is
	 * magnified. Handle apps (full-window present) keep the remap. Set once at DP
	 * setup from the compositor's `has_shared_texture`.
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL ⟹ the DP
	 * assumes the full target is presented (legacy behavior). Appended after
	 * @ref notify_target_recreated per ADR-020 (append-only within a major; no
	 * version bump — gated by the variant's `base.struct_size`).
	 *
	 * @param xdp      Pointer to self.
	 * @param enabled  true ⟹ the app self-presents a shared texture (canvas-only).
	 */
	void (*set_shared_texture_present)(struct xrt_display_processor_vk *xdp, bool enabled);

	/*!
	 * Set the app window's client-area top-left in **panel-relative pixels**
	 * (origin = the SR display's top-left), so the DP can anchor its interlacing
	 * phase to where the woven window physically sits on the 3D panel — the
	 * enabling signal for **windowed weaving** (runtime#757 / LeiaSR#85).
	 *
	 * Background: the interlacing phase must align to the drawn region's absolute
	 * position on the panel. A vendor weaver derives that from the OS window
	 * position on Windows, but on desktop Linux the window's absolute position is
	 * not available to the weaver (the SR SDK's screen-rect query returns (0,0),
	 * and under Wayland a client cannot know its position at all). The compositor
	 * — which owns window placement and already queries the window's on-screen
	 * rect for the Kooima window metrics — is the single source of truth, so it
	 * supplies the origin here. The DP combines it with the per-atlas canvas
	 * (viewport) offset: phase = present_origin + canvas_offset.
	 *
	 * Sticky: applies to every subsequent @ref xrt_display_processor::process_atlas
	 * until changed. Set (0,0) — or never call — for display-scoped weaving (a
	 * full-panel window anchored at the panel top-left), which is the default and
	 * exactly today's behavior. The compositor should call this per frame (cheap)
	 * or whenever the window moves.
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL ⟹ the DP
	 * has no windowed-phase support and weaves display-scoped. Appended after
	 * @ref set_shared_texture_present per ADR-020 (append-only within a major; no
	 * version bump — gated by the variant's `base.struct_size`).
	 *
	 * @param xdp      Pointer to self.
	 * @param panel_x  Window client-area left edge, panel-relative pixels.
	 * @param panel_y  Window client-area top edge, panel-relative pixels.
	 */
	void (*set_present_origin)(struct xrt_display_processor_vk *xdp, int32_t panel_x, int32_t panel_y);

	/*!
	 * Per-frame presentation-timing feedback for the weave-latency control
	 * loop. The runtime reports the MEASURED weave→scanout residual of the
	 * most recently completed frame (VK_KHR_present_wait glass time minus
	 * that frame's weave-record time) plus the display refresh period, so
	 * the DP can feed the vendor eye predictor an exact motion-to-photon
	 * horizon (setLatency) instead of a heuristic. Under late-weave pacing
	 * the residual sits at ~1 refresh; unpaced paths report their true
	 * (larger) value — the whole point of measuring rather than assuming.
	 * Called at most once per frame, before @ref process_atlas.
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL ⟹ the
	 * DP keeps its own horizon heuristic. Appended after
	 * @ref set_present_origin per ADR-020 (append-only within a major; no
	 * version bump — gated by the variant's `base.struct_size`).
	 *
	 * @param xdp                  Pointer to self.
	 * @param weave_to_scanout_ns  Measured residual of the last completed
	 *                             frame; 0 = unknown / not yet measured.
	 * @param frame_period_ns      Display refresh period; 0 = unknown.
	 */
	void (*set_frame_timing)(struct xrt_display_processor_vk *xdp,
	                         uint64_t weave_to_scanout_ns,
	                         uint64_t frame_period_ns);

	/*!
	 * Tell the DP that the command buffer carrying this frame's weave has
	 * been SUBMITTED, and on which queue.
	 *
	 * This exists because the weave is recorded by the DP but submitted by
	 * the compositor: the DP records into the `cmd` handed to
	 * @ref xrt_display_processor::process_atlas and never sees the
	 * `vkQueueSubmit`, so it cannot know when the frame went to the GPU.
	 *
	 * Vendor late latching needs exactly that moment. It re-runs the weave's
	 * vertex attributes with the CURRENT predicted eye position and patches
	 * the mapped vertex buffer of frames already queued but not yet executed
	 * — so the pose is sampled at submit time rather than at record time.
	 * Without this call the vendor weaver cannot distinguish a submitted
	 * frame from an unsubmitted one, silently declines to latch, and still
	 * reports success from its enable call. A latch that never runs is worse
	 * than no latch, because a horizon predictor may stand down in its favour.
	 *
	 * MUST be called with the SAME queue the weave command buffer went to:
	 * the vendor submits an empty fence-carrying submit on it to count frames
	 * in flight, so a different queue tracks the wrong thing. Call once per
	 * weave, immediately after the submit; two weaves without this call
	 * between them degrade that frame to non-late-latched.
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL ⟹ the
	 * DP has no late-latching support and the compositor simply does not call
	 * it. Appended after @ref set_frame_timing per ADR-020 (append-only within
	 * a major; no version bump — gated by the variant's `base.struct_size`).
	 *
	 * @param xdp    Pointer to self.
	 * @param queue  The `VkQueue` the weave command buffer was submitted to.
	 */
	void (*weave_submitted)(struct xrt_display_processor_vk *xdp, VkQueue queue);

	/*!
	 * Report **this window's on-panel rectangle** — the origin AND size of the
	 * surface this DP weaves into, in the platform's screen pixels for the
	 * display named by @p display_id (ADR-036 D6, runtime#1033, LeiaSR#150).
	 *
	 * One compositor instance weaves into one window, so the interlacing phase
	 * must be referenced to where that window physically sits on the panel. The
	 * compositor — the party that knows the placement — reports geometry; the
	 * weaver still owns everything phase, including snapping. ADR-033 is
	 * unchanged by this slot.
	 *
	 * Android is what forces it. A pure window **move** raises no resize
	 * (`WindowFrames.didFrameSizeChange` compares w/h only): it goes out as a
	 * `oneway IWindow.moved` that surfaces no public callback, and
	 * SurfaceFlinger repositions the layer with the *old* buffer — so a
	 * window-relative weave keeps a stale phase for the whole drag. The client
	 * therefore samples `View.getLocationOnScreen()` from a `Choreographer`
	 * callback (opting out of OEM view-bounds sandboxing) and the value reaches
	 * the compositor, which forwards it here. Windows solves the same problem
	 * inside the vendor weaver (it subclasses the HWND and polls the screen
	 * rect); Android exposes no such hook, which is why the runtime reports.
	 *
	 * Coordinates are the platform's own screen coordinates for that display —
	 * on Android, *current*-orientation screen space exactly as
	 * `getLocationOnScreen` returns it. A vendor SDK that rotates into the
	 * panel's natural orientation internally (CNSDK does) takes them unchanged.
	 * @p x / @p y may be negative for a partially off-panel window. Any
	 * per-atlas canvas (zone) offset is *added to* this origin by the DP:
	 * phase = window origin + canvas offset.
	 *
	 * Sticky: applies to every subsequent @ref xrt_display_processor::process_atlas
	 * until changed. The compositor calls it once per frame immediately before
	 * process_atlas whenever it knows the rect; never calling it ⟹ display-scoped
	 * weaving (window anchored at the panel top-left) — exactly today's
	 * behaviour. DPs should cache the last rect and skip the vendor call when
	 * unchanged.
	 *
	 * Relationship to @ref set_present_origin (Linux windowed weaving,
	 * runtime#757): this is its platform-neutral successor — same meaning for
	 * the origin, plus the size and the display id. A DP implementing both takes
	 * whichever it was called with most recently as authoritative.
	 *
	 * Why the *variant* and not the base vtable: the base cannot grow without
	 * moving every slot of this variant (which embeds the base by value), which
	 * would misdispatch calls into any already-built VK-variant plug-in — a
	 * silent break of exactly the kind ADR-020 exists to prevent. The base *is*
	 * the Vulkan interface, so appending here costs no reach; D3D11 carries its
	 * own placement slot (`xrt_display_processor_d3d11::set_window`, #1008).
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL ⟹ no
	 * per-window phase support; the DP weaves display-scoped. Appended after
	 * @ref weave_submitted per ADR-020 (append-only within a major; no version
	 * bump — gated by the variant's `base.struct_size`).
	 *
	 * @param xdp         Pointer to self.
	 * @param x           Window left edge in physical screen pixels.
	 * @param y           Window top edge in physical screen pixels.
	 * @param w           Window width in physical screen pixels.
	 * @param h           Window height in physical screen pixels.
	 * @param display_id  Platform display id the rect is expressed in (Android
	 *                    `Display.getDisplayId()`, 0 = default panel); -1 =
	 *                    unknown / single-display.
	 */
	void (*set_window_screen_rect)(struct xrt_display_processor_vk *xdp,
	                               int32_t x,
	                               int32_t y,
	                               uint32_t w,
	                               uint32_t h,
	                               int32_t display_id);

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
	 * Why the *variant* and not the base vtable: the base cannot grow
	 * without moving every slot of this variant (which embeds the base by
	 * value), which would misdispatch calls into any already-built
	 * VK-variant plug-in — see @ref set_window_screen_rect. The base *is*
	 * the Vulkan interface, so appending here costs no reach.
	 *
	 * Optional — an absent slot (older plug-in `base.struct_size`) or NULL,
	 * or a false return, means "unknown": the runtime treats that as OK.
	 * Appended after @ref set_window_screen_rect per ADR-020.
	 *
	 * @param      xdp        Pointer to self.
	 * @param[out] out_state  One of the XRT_DP_BACKEND_STATE_* values.
	 * @return true when @p out_state was written.
	 */
	bool (*get_backend_state)(struct xrt_display_processor_vk *xdp, uint32_t *out_state);

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
	 * Optional — absent slot (the plug-in's `base.struct_size` doesn't cover
	 * it), NULL, or a false return all mean @ref XRT_DP_WEAVE_SCOPE_CANVAS,
	 * which is exactly the behaviour every existing plug-in already has.
	 * Appended per ADR-020 (append-only within a major; no version bump).
	 *
	 * @param      xdp       Pointer to self.
	 * @param[out] out_caps  Filled by the DP (struct_size pre-set by caller).
	 * @return true if @p out_caps was filled.
	 */
	bool (*get_scanout_caps)(struct xrt_display_processor_vk *xdp, struct xrt_dp_scanout_caps *out_caps);

};

/*!
 * Defined when this header carries the @ref xrt_display_processor_vk::set_present_origin
 * slot, so a plug-in built against an older runtime (which lacks the slot) can
 * `#ifdef`-guard its implementation and still compile — the coupled-ABI-addition
 * pattern for a pre-GA feature that lands across the runtime + a vendor plug-in
 * (runtime#757 / LeiaSR#85).
 */
#define XRT_DP_VK_HAS_PRESENT_ORIGIN 1

/*!
 * Defined when this header carries the @ref xrt_display_processor_vk::set_frame_timing
 * slot — same coupled-ABI-addition pattern as @ref XRT_DP_VK_HAS_PRESENT_ORIGIN,
 * for the weave-latency control loop landing across runtime + vendor plug-in.
 */
#define XRT_DP_VK_HAS_FRAME_TIMING 1

/*!
 * Defined when this header carries the @ref xrt_display_processor_vk::weave_submitted
 * slot — same coupled-ABI-addition pattern as @ref XRT_DP_VK_HAS_PRESENT_ORIGIN,
 * for Vulkan late latching landing across runtime + vendor plug-in.
 */
#define XRT_DP_VK_HAS_WEAVE_SUBMITTED 1

/*!
 * Defined when this header carries the
 * @ref xrt_display_processor_vk::set_window_screen_rect slot — same coupled-ABI-
 * addition pattern as @ref XRT_DP_VK_HAS_PRESENT_ORIGIN, for the per-window
 * weave-phase contract landing across runtime + vendor plug-in (ADR-036 D6,
 * runtime#1033 / LeiaSR#150).
 */
#define XRT_DP_VK_HAS_WINDOW_SCREEN_RECT 1

/*!
 * Defined when this header carries the
 * @ref xrt_display_processor_vk::get_backend_state slot — same coupled-ABI-
 * addition pattern as @ref XRT_DP_VK_HAS_PRESENT_ORIGIN, for the vendor-backend
 * health report landing across runtime + vendor plug-in.
 */
#define XRT_DP_VK_HAS_BACKEND_STATE 1


/*!
 * Defined when this header carries the
 * @ref xrt_display_processor_vk::get_scanout_caps slot — same coupled-ABI-
 * addition pattern as @ref XRT_DP_VK_HAS_PRESENT_ORIGIN, for the weave-scope
 * declaration a hardware-weaving plug-in needs in order to be routed correctly.
 */
#define XRT_DP_VK_HAS_SCANOUT_CAPS 1

/*
 * ── Plug-in ABI tripwire (ADR-020) ─────────────────────────────────────────
 *
 * This variant is part of the same versioned plug-in ABI as the base
 * @ref xrt_display_processor. Because it embeds the base at offset 0, the base's
 * own 22-slot tripwire (in xrt_display_processor.h) already pins the embedded
 * layout — any base reorder fails there. Here we only need to pin that the base
 * really is at offset 0 and that @ref set_transparent_background is appended
 * immediately after it (so the struct_size gate discriminates the variant).
 * Appending a method WITHOUT a major bump means appending after this slot, with
 * its own assert and a bumped size assert; any other change is breaking and must
 * bump XRT_PLUGIN_API_VERSION_CURRENT (xrt_plugin.h) + re-pin every plug-in.
 *
 * NOTE the appended slot lands at sizeof(struct xrt_display_processor) — i.e.
 * right after the base's last slot (clear_local_zone_mask). This is NOT the
 * D3D11 "+17" offset: the Vulkan base carries more slots than the D3D11 vtable.
 *
 * XRT_DP_ABI_ASSERT / XRT_DP_ABI_MSG / XRT_DP_HAS_SLOT are defined (guarded) by
 * xrt_display_processor.h, included above.
 */
// clang-format off
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, base) == 0, XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, set_transparent_background) == sizeof(struct xrt_display_processor) + 0 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, notify_target_recreated)    == sizeof(struct xrt_display_processor) + 1 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, set_shared_texture_present) == sizeof(struct xrt_display_processor) + 2 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, set_present_origin)          == sizeof(struct xrt_display_processor) + 3 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, set_frame_timing)            == sizeof(struct xrt_display_processor) + 4 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, weave_submitted)             == sizeof(struct xrt_display_processor) + 5 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, set_window_screen_rect)      == sizeof(struct xrt_display_processor) + 6 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, get_backend_state)          == sizeof(struct xrt_display_processor) + 7 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_vk, get_scanout_caps)           == sizeof(struct xrt_display_processor) + 8 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(sizeof(struct xrt_display_processor_vk) == sizeof(struct xrt_display_processor) + 9 * sizeof(void *), XRT_DP_ABI_MSG);
// clang-format on

/*!
 * @copydoc xrt_display_processor_vk::set_transparent_background
 *
 * Returns false if not supported (the plug-in's `base.struct_size` doesn't cover
 * the slot, or the pointer is NULL) — the caller then leaves the DP opaque.
 *
 * Unlike @ref XRT_DP_HAS_SLOT (which assumes a direct `struct_size` member), the
 * presence check here reads `xdp->base.struct_size` because the variant embeds
 * the base — see ADR-020.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline bool
xrt_display_processor_vk_set_transparent_background(struct xrt_display_processor_vk *xdp,
                                                    bool enabled,
                                                    bool client_presents)
{
	if (xdp == NULL) {
		return false;
	}
	const char *slot_end =
	    (const char *)&xdp->set_transparent_background + sizeof(xdp->set_transparent_background);
	if (slot_end > (const char *)xdp + xdp->base.struct_size || xdp->set_transparent_background == NULL) {
		return false;
	}
	xdp->set_transparent_background(xdp, enabled, client_presents);
	return true;
}

/*!
 * @copydoc xrt_display_processor_vk::notify_target_recreated
 *
 * No-op if not supported (the plug-in's `base.struct_size` doesn't cover the
 * slot, or the pointer is NULL). Like @ref
 * xrt_display_processor_vk_set_transparent_background, the presence check reads
 * `xdp->base.struct_size` because the variant embeds the base — see ADR-020.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline void
xrt_display_processor_vk_notify_target_recreated(struct xrt_display_processor_vk *xdp, uint32_t generation)
{
	if (xdp == NULL) {
		return;
	}
	const char *slot_end = (const char *)&xdp->notify_target_recreated + sizeof(xdp->notify_target_recreated);
	if (slot_end > (const char *)xdp + xdp->base.struct_size || xdp->notify_target_recreated == NULL) {
		return;
	}
	xdp->notify_target_recreated(xdp, generation);
}

/*!
 * @copydoc xrt_display_processor_vk::set_shared_texture_present
 *
 * Returns false if not supported (the plug-in's `base.struct_size` doesn't cover
 * the slot, or the pointer is NULL) — the DP then keeps its full-target
 * assumption (no remap skip). Like the wrappers above, the presence check reads
 * `xdp->base.struct_size` because the variant embeds the base — see ADR-020.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline bool
xrt_display_processor_vk_set_shared_texture_present(struct xrt_display_processor_vk *xdp, bool enabled)
{
	if (xdp == NULL) {
		return false;
	}
	const char *slot_end =
	    (const char *)&xdp->set_shared_texture_present + sizeof(xdp->set_shared_texture_present);
	if (slot_end > (const char *)xdp + xdp->base.struct_size || xdp->set_shared_texture_present == NULL) {
		return false;
	}
	xdp->set_shared_texture_present(xdp, enabled);
	return true;
}

/*!
 * @copydoc xrt_display_processor_vk::set_present_origin
 *
 * Returns false if not supported (the plug-in's `base.struct_size` doesn't cover
 * the slot, or the pointer is NULL) — the caller then leaves the DP weaving
 * display-scoped (today's behavior). Like the wrappers above, the presence check
 * reads `xdp->base.struct_size` because the variant embeds the base — see ADR-020.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline bool
xrt_display_processor_vk_set_present_origin(struct xrt_display_processor_vk *xdp, int32_t panel_x, int32_t panel_y)
{
	if (xdp == NULL) {
		return false;
	}
	const char *slot_end = (const char *)&xdp->set_present_origin + sizeof(xdp->set_present_origin);
	if (slot_end > (const char *)xdp + xdp->base.struct_size || xdp->set_present_origin == NULL) {
		return false;
	}
	xdp->set_present_origin(xdp, panel_x, panel_y);
	return true;
}

/*!
 * @copydoc xrt_display_processor_vk::set_frame_timing
 *
 * No-op if not supported (the plug-in's `base.struct_size` doesn't cover the
 * slot, or the pointer is NULL) — the DP then keeps its own horizon heuristic.
 * Like the wrappers above, the presence check reads `xdp->base.struct_size`
 * because the variant embeds the base — see ADR-020.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline void
xrt_display_processor_vk_set_frame_timing(struct xrt_display_processor_vk *xdp,
                                          uint64_t weave_to_scanout_ns,
                                          uint64_t frame_period_ns)
{
	if (xdp == NULL) {
		return;
	}
	const char *slot_end = (const char *)&xdp->set_frame_timing + sizeof(xdp->set_frame_timing);
	if (slot_end > (const char *)xdp + xdp->base.struct_size || xdp->set_frame_timing == NULL) {
		return;
	}
	xdp->set_frame_timing(xdp, weave_to_scanout_ns, frame_period_ns);
}

/*!
 * @copydoc xrt_display_processor_vk::weave_submitted
 *
 * Returns true if the DP was told, false if the slot is absent (older plug-in
 * `struct_size`) or NULL — the caller can use that to decide whether late
 * latching is drivable at all, since enabling it without this hook produces a
 * latch that silently never runs.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline bool
xrt_display_processor_vk_weave_submitted(struct xrt_display_processor_vk *xdp, VkQueue queue)
{
	if (xdp == NULL) {
		return false;
	}
	const char *slot_end = (const char *)&xdp->weave_submitted + sizeof(xdp->weave_submitted);
	if (slot_end > (const char *)xdp + xdp->base.struct_size || xdp->weave_submitted == NULL) {
		return false;
	}
	xdp->weave_submitted(xdp, queue);
	return true;
}


/*!
 * @copydoc xrt_display_processor_vk::set_window_screen_rect
 *
 * Returns false if not supported (the plug-in's `base.struct_size` doesn't cover
 * the slot, or the pointer is NULL) — the caller then leaves the DP weaving
 * display-scoped. Like the wrappers above, the presence check reads
 * `xdp->base.struct_size` because the variant embeds the base — see ADR-020.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline bool
xrt_display_processor_vk_set_window_screen_rect(struct xrt_display_processor_vk *xdp,
                                                int32_t x,
                                                int32_t y,
                                                uint32_t w,
                                                uint32_t h,
                                                int32_t display_id)
{
	if (xdp == NULL) {
		return false;
	}
	const char *slot_end = (const char *)&xdp->set_window_screen_rect + sizeof(xdp->set_window_screen_rect);
	if (slot_end > (const char *)xdp + xdp->base.struct_size || xdp->set_window_screen_rect == NULL) {
		return false;
	}
	xdp->set_window_screen_rect(xdp, x, y, w, h, display_id);
	return true;
}

/*!
 * @copydoc xrt_display_processor_vk::get_backend_state
 *
 * Returns false if not supported (the plug-in's `base.struct_size` doesn't cover
 * the slot, or the pointer is NULL, or the DP declined) — the caller then treats
 * the backend state as unknown, i.e. @ref XRT_DP_BACKEND_STATE_OK. Like the
 * wrappers above, the presence check reads `xdp->base.struct_size` because the
 * variant embeds the base — see ADR-020.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline bool
xrt_display_processor_vk_get_backend_state(struct xrt_display_processor_vk *xdp, uint32_t *out_state)
{
	if (xdp == NULL) {
		return false;
	}
	const char *slot_end = (const char *)&xdp->get_backend_state + sizeof(xdp->get_backend_state);
	if (slot_end > (const char *)xdp + xdp->base.struct_size || xdp->get_backend_state == NULL) {
		return false;
	}
	return xdp->get_backend_state(xdp, out_state);
}

/*!
 * @copydoc xrt_display_processor_vk::get_scanout_caps
 *
 * Returns false when the slot is absent, NULL, or the DP declined — in every
 * one of those cases the caller must read the scope as
 * @ref XRT_DP_WEAVE_SCOPE_CANVAS. Prefer
 * @ref xrt_display_processor_vk_get_weave_scope, which does that for you. Like
 * the wrappers above, the presence check reads `xdp->base.struct_size` because
 * the variant embeds the base — see ADR-020.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline bool
xrt_display_processor_vk_get_scanout_caps(struct xrt_display_processor_vk *xdp, struct xrt_dp_scanout_caps *out_caps)
{
	if (xdp == NULL) {
		return false;
	}
	const char *slot_end = (const char *)&xdp->get_scanout_caps + sizeof(xdp->get_scanout_caps);
	if (slot_end > (const char *)xdp + xdp->base.struct_size || xdp->get_scanout_caps == NULL) {
		return false;
	}
	return xdp->get_scanout_caps(xdp, out_caps);
}

/*!
 * The DP's @ref xrt_dp_weave_scope, with every "didn't answer" case resolved to
 * @ref XRT_DP_WEAVE_SCOPE_CANVAS. This is the accessor call sites should use.
 *
 * @public @memberof xrt_display_processor_vk
 */
static inline enum xrt_dp_weave_scope
xrt_display_processor_vk_get_weave_scope(struct xrt_display_processor_vk *xdp)
{
	struct xrt_dp_scanout_caps caps;
	xrt_dp_scanout_caps_init(&caps);
	if (!xrt_display_processor_vk_get_scanout_caps(xdp, &caps)) {
		return XRT_DP_WEAVE_SCOPE_CANVAS;
	}
	return xrt_dp_weave_scope_clamp(caps.weave_scope);
}

#ifdef __cplusplus
}
#endif
