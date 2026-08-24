// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  VK-1 — the in-process Vulkan output-device split (#918 / #1178).
 * @ingroup comp_vk_native
 *
 * ## What this is
 *
 * The second rung of the in-process Vulkan weave-on-scanout ladder. VK-0 made
 * the Vulkan compositor's atlas live in a D3D11 texture (@ref comp_vk_deposit);
 * this unit takes that texture across the adapter boundary and moves the weave,
 * the repaint tick and the present onto the **scanout** adapter, exactly as the
 * shipped D3D11 and D3D12 in-process legs do (ADR-037 §1: the split is not a
 * mode to opt into, it is what the placement rule degenerates to whenever render
 * and scanout differ).
 *
 * ## Topology — Vulkan never touches a cross-adapter object
 *
 * @verbatim
 *   VK renders the atlas
 *        | (renders INTO, no copy — VK-0)
 *        v
 *   D3D11 deposit texture           <-- app end, RENDER adapter
 *        | comp_xbridge, D3D11 ENDS (producer/consumer D3D12 devices it owns,
 *        |               cross-adapter placed ring in the middle)
 *        v
 *   egress ring: output-D3D11 textures + SRVs   <-- SCANOUT adapter
 *        |
 *   D3D11 display processor weaves --> DXGI swapchain --> Present
 * @endverbatim
 *
 * **The "xbridge has no Vulkan ends" blocker does not exist.** Earlier #1178
 * analysis (and both D3D reference legs' own porting notes) assumed a Vulkan leg
 * would need a third @ref comp_xbridge_info ends flavour or a VK↔D3D12
 * external-memory ingest. VK-0 dissolved that: the deposit hands over a genuine
 * NT-shared `ID3D11Texture2D` with its own `ID3D11Device`, immediate context and
 * `IDXGIAdapter`, so the **existing D3D11-ends flavour applies verbatim** and the
 * transport needed no change at all for this rung.
 *
 * ## Why this is a separate unit, and why it is C++
 *
 * @ref comp_vk_native_compositor.c is C11. Everything here is COM — D3D11
 * devices, DXGI factories, `ID3D11Fence`, `ID3D11DeviceContext4` — which in C
 * means `p->lpVtbl->Method(p, ...)` at every call. The same reasoning already put
 * @ref comp_vk_native_target.cpp and @ref comp_vk_native_deposit.cpp in C++; this
 * follows it. The compositor sees an opaque handle and a dozen C entry points,
 * and the ~40 D3D call sites stay in one reviewable file.
 *
 * ## The PLANES — and why Vulkan is the only leg that had to build them
 *
 * Beside the atlas, the masked composite needs three more app-device images on
 * the scanout adapter: the Local2D over-flatten, the 2D-under backdrop and a
 * Tier-3 authored zone mask. @ref comp_xbridge has transported all three since
 * Phase 2a — but its D3D11-ends flavour binds a plane by NT handle to an
 * `ID3D11Texture2D` and its D3D12-ends flavour binds an `ID3D12Resource` by
 * pointer, and **both assume the compositor's flatten scratch already IS a D3D
 * resource**. The D3D11 leg's is (`local2d_scratch_share`); the D3D12 leg's is.
 * This compositor's is a plain `VkImage`, so there is nothing to bind.
 *
 * That is the asymmetry to keep in mind reading this file, and it is not
 * over-engineering: VK-1a needed no transport change only because VK-0 had
 * *already* made the atlas a D3D11 texture. The planes had no VK-0, so VK-1b is
 * VK-0 again — the flatten renders straight into a deposit-backed, NT-shared
 * `ID3D11Texture2D` (@ref comp_vk_deposit_plane_ensure), zero copies, same fence.
 *
 * Window-space layers need none of this: the Vulkan compositor composites those
 * INTO the atlas pre-weave, on the app device, so they ride across for free.
 *
 * ## The plane back-fence, which is NOT the bridge's
 *
 * A plane's ingress is Option I — the bridge's producer copy queue opens the
 * app-device texture and reads it in place. The bridge's own back-fence for that
 * (`comp_xbridge_pre_plane_write`) issues a GPU-side wait **on the app's D3D11
 * immediate context**, which is exactly right for the two D3D legs because the
 * immediate context is what writes their scratch. Here the plane is written by
 * the **Vulkan queue**, which that wait does not order at all.
 *
 * @ref comp_vk_split_submit_atlas therefore takes the bridge's wait and then
 * signals the deposit's shared fence on that same immediate context, so the value
 * is unreachable until the producer's read has resolved; Vulkan's next flatten
 * waits for it on the timeline. One queued signal, one queued wait, no deeper
 * ring and no change of ingress mode. See @ref comp_vk_deposit_note_planes_consumed.
 *
 * ## The HUD is NOT a plane, and is not skipped either
 *
 * `u_hud` rasterises to a CPU pixel buffer, so it belongs to no device: the
 * out-device half simply uploads it to a scanout-adapter texture and copies it
 * onto the back buffer, which is what the D3D11 and D3D12 legs already do
 * (`d3d11_render_hud_overlay(c, d3d11_out_device(c), …)`). Nothing crosses the
 * bridge for it and there is no reason token — the HUD is never a split obstacle.
 *
 * ## Synchronisation — no CPU wait on the app thread or the weave path
 *
 * The defining property of the ladder, and it is unchanged from VK-0. Ordering is
 * carried by the deposit's imported **timeline semaphore**, never by a CPU wait:
 *
 * - VK's atlas submit signals the timeline at value V.
 * - Before the bridge reads the deposit, the app-end D3D11 immediate context
 *   takes `ID3D11DeviceContext4::Wait(fence, V)` — a GPU-side wait.
 * - The bridge's own producer/consumer waits are queue operations it already owns.
 * - The weave takes `comp_xbridge_gpu_wait_slot` on the OUTPUT context.
 * - Back-pressure the other way (VK must not overwrite a deposit slot the
 *   producer is still reading) is the deposit's **consumer-release** value: the
 *   app-end context signals the same fence past V once the bridge has taken its
 *   copy, and VK's next submit into that slot waits for it. See
 *   @ref comp_vk_deposit_note_consumed.
 *
 * The only bounded CPU waits are in teardown, inside `comp_xbridge_quiesce`.
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

struct xrt_device;
struct comp_vk_split;
struct comp_vk_deposit_handoff;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * ADR-037 §3 — the RENDER adapter has no `VK_KHR_timeline_semaphore`, so the
 * deposit cannot exist and neither can the split.
 *
 * A new token, and deliberately not folded into
 * @ref COMP_SPLIT_REASON_API_UNSUPPORTED: "Vulkan cannot split" was true before
 * this rung and is a statement about the RUNTIME, whereas this is a statement
 * about **one app's device**, which the app itself chose when it created it. A
 * support case reading `api_unsupported` from a Vulkan session would conclude the
 * build predates VK-1 and stop looking; this one says the build is fine and the
 * device is not.
 *
 * The runtime cannot enable the feature after the fact, so this is a hard gate
 * checked FIRST — before any adapter is resolved and before any device is
 * created.
 */
#define COMP_SPLIT_REASON_NO_TIMELINE_SEMAPHORE "no_timeline_semaphore"

/*!
 * Everything Stage A needs. All pointers are borrowed; the split AddRefs what it
 * keeps and never outlives its creator.
 */
struct comp_vk_split_info
{
	struct xrt_device *xdev;

	//! The window the session presents to. NULL ⟹ ineligible (`no_hwnd`).
	void *hwnd;

	/*!
	 * `xrt_dp_factory_d3d11_fn_t`. The plug-in is asked for a **D3D11** weaver
	 * because that is what the scanout-adapter target is; a NULL factory is
	 * not an error but does mean the session would weave nothing, so Stage A
	 * refuses (`dp_refused_scanout`).
	 */
	void *dp_factory_d3d11;

	//! The app owns the present — ineligible (`shared_texture_session`).
	bool has_shared_texture;

	//! Forwarded to the out-device DP and the out-device swapchain.
	bool transparent_background;

	/*!
	 * The app's `VkDevice` has `VK_KHR_timeline_semaphore`. False is the hard
	 * gate above; checked before anything is resolved.
	 */
	bool app_timeline_semaphores;

	//! Packed LUID (HighPart<<32 | LowPart) of the `VkPhysicalDevice`, 0 if the
	//! driver would not report one (`render_unresolvable`).
	uint64_t render_packed_luid;

	//! Panel origin in desktop pixels — how the scanout adapter is located.
	int32_t display_screen_left;
	int32_t display_screen_top;

	//! The out-device swapchain's initial size.
	uint32_t preferred_width;
	uint32_t preferred_height;
};

/*!
 * **Stage A1** — decide, then stand up everything on the SCANOUT adapter that
 * does not depend on the app-side deposit: the runtime-owned `ID3D11Device` and
 * its DXGI factory, the display processor (ADR-037 §3a, negotiated BEFORE the
 * split commits — the D3D11 leg's #1169 design, so a refusal is just another
 * Stage-A failure with no teardown path of its own), and the DXGI swapchain.
 *
 * Split in two because of a Vulkan-specific ordering fact: the deposit is owned
 * by the renderer, and the renderer is created AFTER the point where the VK
 * display processor and the VK target would be. The decision has to be taken
 * before those, or the session ends up with a VK weaver bound to the HWND that
 * the split would then have to evict. So A1 decides and takes the HWND; A2 wires
 * the transport once the deposit exists.
 *
 * A2 failing is therefore **not** a weaker guarantee than the D3D legs': nothing
 * has been submitted or presented at that point, so
 * @ref comp_vk_split_retire restores exactly the state A1 skipped, through the
 * same single recovery path. See @ref comp_vk_split_wire_bridge.
 *
 * @param out_split On success, the active split. NULL on every other outcome.
 * @param out_short_reason ALWAYS set: one of the canonical `COMP_SPLIT_REASON_*`
 *        tokens naming the rung taken, or NULL when the split is active. Never a
 *        phrase — this is what `weave placement:` and `[RENDER] split=0 reason=`
 *        print, and what a support case is grepped for.
 *
 * @return XRT_SUCCESS when the split is active. Any other value means the caller
 *         proceeds on its stock single-device Vulkan path; no failure here is
 *         ever fatal to the session.
 */
xrt_result_t
comp_vk_split_stage_a(const struct comp_vk_split_info *info,
                      struct comp_vk_split **out_split,
                      const char **out_short_reason);

/*!
 * **Stage A2** — wire @ref comp_xbridge between the deposit and the output
 * device, and allocate the egress ring.
 *
 * Call once, as soon as the renderer's deposit exists. The ring is allocated
 * HERE rather than on the frame path so the split can never reach its first
 * weave with nothing to weave into.
 *
 * @param handoff The deposit's app end (device, immediate context, adapter). The
 *        split keeps the device/context/adapter, not the per-slot texture — the
 *        slot is nominated per frame by @ref comp_vk_split_submit_atlas.
 *
 * @return true when the bridge is live. On false the caller must
 *         @ref comp_vk_split_retire immediately; nothing has presented yet, so
 *         that is a cold recovery.
 */
bool
comp_vk_split_wire_bridge(struct comp_vk_split *split, const struct comp_vk_deposit_handoff *handoff);

/*!
 * Release everything, in the reverse of the create order, and NULL the caller's
 * pointer. Quiesces the bridge first (a bounded CPU wait, teardown only) because
 * its egress slots are what the display processor is still sampling.
 */
void
comp_vk_split_destroy(struct comp_vk_split **split_ptr);

/*!
 * ADR-037 §3 — give the scanout adapter back and move the weave home for the
 * rest of the session.
 *
 * Tears down the output half ONLY. The caller is then responsible for rebuilding
 * its Vulkan display processor and Vulkan target on the app device — which is the
 * same code it would have run had Stage A failed, so there is one recovery path
 * and not two.
 *
 * Emits the RETIRED WARN and the `weave placement: CHANGED …` correction line, so
 * the LAST `weave placement:` line in a log stays the truth.
 *
 * @param why Prose for the human-readable WARN.
 * @param short_reason The canonical token for the correction line.
 */
void
comp_vk_split_retire(struct comp_vk_split **split_ptr, const char *why, const char *short_reason);

/*!
 * Hand this app frame's deposit slot to the bridge.
 *
 * Takes the GPU-side `ID3D11DeviceContext4::Wait(fence, fence_value)` on the app
 * end so the producer copy cannot start before Vulkan's write has landed, then
 * nominates the slot, right-sizes the egress ring to the frame's CONTENT box and
 * submits the three copy legs. Returns immediately — nothing here waits.
 *
 * **This is where crop-before-DP happens (ADR-030).** The ring is sized at
 * @p content_w × @p content_h, so the consumer's copy lands content-sized and the
 * bridge delivers CROPPED pixels; the compositor's own `vk_crop_atlas_for_dp`
 * scratch is not involved and must not run. Only when the R2 resize hysteresis is
 * holding a worst-case ring does a crop remain owed, and that one runs on the
 * OUTPUT device inside @ref comp_vk_split_weave_and_present.
 *
 * @param handoff This frame's deposit slot, after `comp_vk_deposit_advance`.
 * @param cols,rows,view_w,view_h The frame's effective tile layout. A change in
 *        the GRID (not the tile size — that moves continuously through a resize)
 *        bumps the layout generation stamped on the slot, which is the #918 R1
 *        refusal test.
 */
void
comp_vk_split_submit_atlas(struct comp_vk_split *split,
                           const struct comp_vk_deposit_handoff *handoff,
                           uint32_t cols,
                           uint32_t rows,
                           uint32_t view_w,
                           uint32_t view_h);

/*!
 * VK-1b (#1178) — publish this frame's 2D-under BACKDROP plane.
 *
 * Call once per app frame, BEFORE @ref comp_vk_split_submit_atlas: that call
 * stamps the slot's recipe, and the recipe carries the backdrop's own extent.
 *
 * The plane's SOURCE is a deposit-backed surface — an NT-shared `ID3D11Texture2D`
 * on the app adapter that the Vulkan flatten renders straight into
 * (@ref comp_vk_deposit_plane_ensure). That is what this rung had to build and
 * the two D3D legs did not: their flatten scratch was already a D3D resource.
 *
 * @param nt_handle The plane surface's NT share, or NULL to drop the plane.
 * @param generation The surface's ALLOCATION generation. A change re-opens the
 *        handle inside the bridge (and drains the producer first), so it must
 *        never move per frame.
 * @param alloc_w,alloc_h The plane chain's extent — the PANEL, always, so the
 *        plane stays outside the R2 resize hysteresis.
 * @param content_seq A hash of the pixels the surface now holds; 0 means "this
 *        frame does not use the plane". The bridge SKIPS the copy when the write
 *        slot already carries this exact seq, which is mandatory rather than an
 *        optimisation — a full-window RGBA plane at 4K60 is ~2 GB/s on its own.
 * @param dirty_x,dirty_y,dirty_w,dirty_h This frame's dirty box in source pixels.
 * @param bd_w,bd_h The backdrop's OWN extent (the composite region), which is not
 *        the plane's extent. 0 ⟹ the frame produced no backdrop and the weave
 *        clears the display processor's background.
 */
void
comp_vk_split_stage_backdrop(struct comp_vk_split *split,
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
                             uint32_t bd_h);

/*!
 * #918 review F4 — the composite REGION moved, so drop every slot's pixels for
 * @p plane and make each owe a full refresh of the plane extent.
 *
 * The plane surfaces are panel-sized; a region that shrinks leaves stale pixels
 * outside the new one, and the per-slot dirty-box union would carry them across
 * as if they were content. The caller clears its whole surface and calls this.
 *
 * @param plane A `COMP_XBRIDGE_PLANE_*` index.
 */
void
comp_vk_split_invalidate_plane(struct comp_vk_split *split, uint32_t plane);

/*!
 * Weave one frame on the scanout adapter and present it.
 *
 * Picks the newest egress slot whose consumer copy has landed AND whose stamped
 * layout generation still matches (#918 R1), falling back to the in-flight slot
 * across a transition rather than letting the panel keep a frame woven for a mode
 * the display has left. Takes a GPU-side wait on the output context, crops on the
 * output device if the ring is oversized, weaves, and presents.
 *
 * **#918 F4** — when no slot is usable this presents NOTHING and returns false.
 * With FLIP_DISCARD the panel then holds the last woven frame, which is strictly
 * better than a cleared one.
 *
 * @param is_repaint The #868 replay tick: re-weaves the slot the last app frame
 *        published, with zero bridge traffic. Bails when the slot is not verified
 *        complete (#918 F7) — it did not pick that slot itself, so nothing else
 *        orders it against a consumer copy rewriting it.
 * @param canvas The DP canvas sub-rect, in window pixels.
 *
 * @return true when a frame was woven and presented.
 */
bool
comp_vk_split_weave_and_present(struct comp_vk_split *split, bool is_repaint, const struct xrt_rect *canvas);

/*!
 * True when the last app frame published a slot a repaint could replay. The
 * repaint tick tests this BEFORE acquiring, so a warmup tick costs no acquire.
 */
bool
comp_vk_split_has_weave_slot(struct comp_vk_split *split);

/*!
 * The once-every-10-s `[RENDER] split=… xb_kb=… ingress=… ing_leak=…` line, in
 * the same shape the D3D12 leg emits so one grep covers every in-process leg.
 * Cheap to call every frame; rate-limits itself.
 */
void
comp_vk_split_render_diag(struct comp_vk_split *split);

/*!
 * @name Display-processor forwarding
 *
 * Under the split the session's weaver is an `xrt_display_processor_d3d11`,
 * whose vtable is a different type from the Vulkan `xrt_display_processor` the
 * compositor holds everywhere else. These forward the handful of
 * graphics-API-free queries the compositor makes of "the display processor,
 * whichever one this session has" — eye positions, panel geometry, mode and
 * eye-tracking control. Everything Vulkan-shaped (render passes, self-submission,
 * target-recreate notifications) has no counterpart and no caller while the split
 * is up.
 * @{
 */
bool
comp_vk_split_get_predicted_eye_positions(struct comp_vk_split *split, struct xrt_eye_positions *out_eye_pos);

bool
comp_vk_split_get_display_dimensions(struct comp_vk_split *split, float *out_width_m, float *out_height_m);

bool
comp_vk_split_get_display_pixel_info(struct comp_vk_split *split,
                                     uint32_t *out_width_px,
                                     uint32_t *out_height_px,
                                     int32_t *out_left,
                                     int32_t *out_top);

bool
comp_vk_split_request_display_mode(struct comp_vk_split *split, bool enable_3d);

void
comp_vk_split_set_eye_tracking_mode(struct comp_vk_split *split, uint32_t mode);
/*! @} */

#ifdef __cplusplus
}
#endif
