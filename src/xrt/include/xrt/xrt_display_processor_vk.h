// Copyright 2026, Leia Inc.
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
};

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
XRT_DP_ABI_ASSERT(sizeof(struct xrt_display_processor_vk) == sizeof(struct xrt_display_processor) + 2 * sizeof(void *), XRT_DP_ABI_MSG);
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

#ifdef __cplusplus
}
#endif
