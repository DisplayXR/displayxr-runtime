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
};

/*!
 * Defined when this header carries the @ref xrt_display_processor_vk::set_present_origin
 * slot, so a plug-in built against an older runtime (which lacks the slot) can
 * `#ifdef`-guard its implementation and still compile — the coupled-ABI-addition
 * pattern for a pre-GA feature that lands across the runtime + a vendor plug-in
 * (runtime#757 / LeiaSR#85).
 */
#define XRT_DP_VK_HAS_PRESENT_ORIGIN 1

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
XRT_DP_ABI_ASSERT(sizeof(struct xrt_display_processor_vk) == sizeof(struct xrt_display_processor) + 4 * sizeof(void *), XRT_DP_ABI_MSG);
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

#ifdef __cplusplus
}
#endif
