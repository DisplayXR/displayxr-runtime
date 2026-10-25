// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The D3D11 hub (fill arm): weave + present on a D3D11 device for any producer.
 * @ingroup comp_hub_d3d11
 *
 * Born as VK-1, the in-process Vulkan output-device split (#918 / #1178), in
 * comp_vk_native (as comp_vk_native_split.h); extracted into its own library by
 * #2317 P1a because it was never Vulkan code: it consumes only the pure-D3D11
 * @ref comp_hub_d3d11_handoff (comp_hub_d3d11_handoff.h). Today's producers are
 * the Vulkan deposit (VK-0) and the D3D12 tier's #1264 same-adapter reroute.
 * Symbols renamed comp_vk_split_* -> comp_hub_d3d11_*; the design prose below
 * is the VK-1 original and still describes the mechanism.
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
 * @ref comp_hub_d3d11_submit_atlas therefore takes the bridge's wait and then
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
struct comp_hub_d3d11;
struct xrt_screen_list;
struct xrt_segment_metrics;
struct xrt_system_compositor_info;
struct comp_segments_content;
struct comp_hub_d3d11_handoff;
//! XR_DXR_depth_budget - taken by pointer only; see xrt/xrt_display_processor.h.
struct xrt_dp_background_preview;

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
struct comp_hub_d3d11_info
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

	/*!
	 * #2317 P1b: the producer's atlas is ONE NT-shared texture, rendered on the
	 * handoff's immediate context and re-created only on a generation bump (the
	 * D3D11 tier's renderer atlas), not a per-frame ring slot.
	 *
	 * The hub then reads it IN PLACE. On the SAME adapter it opens the texture
	 * on its own device and GPU-waits on the producer's shared fence: DIRECT
	 * ingress, no transport copy at all (@ref comp_hub_d3d11_is_direct). Across
	 * adapters it binds it as comp_xbridge ingress Option I, as the D3D11 tier's
	 * own split does (two copies, no staging). Either way the producer must call
	 * @ref comp_hub_d3d11_pre_render before it rewrites the atlas.
	 *
	 * False (every ring deposit: the VK and D3D12 tiers): on the SAME adapter
	 * the hub reads each ring slot DIRECT (#2317 P2a — see
	 * @ref comp_hub_d3d11_take_slot_release); across adapters, or with
	 * `DXR_HUB_INGRESS=xbridge`, the forced staged ingress exactly as before.
	 */
	bool stable_atlas;

	/*!
	 * #2317 P2a: a RING producer that honours the DIRECT contract — it skips
	 * its deposit's note_consumed calls while @ref comp_hub_d3d11_is_direct,
	 * hands every @ref comp_hub_d3d11_take_slot_release to its deposit, and
	 * opens @ref comp_hub_d3d11_get_consumer_fence_handle. A ring producer that
	 * leaves it false stays on xbridge (`ingress_reason=producer_unsupported`),
	 * because nothing would order its slot rewrites after the hub's reads.
	 */
	bool ring_direct;

	/*!
	 * #2317 P2c: a RING producer whose 2D PLANES (Local2D over-flatten,
	 * 2D-under backdrop, authored mask) may ride the DIRECT ingress. It
	 * promises the plane twin of the atlas contract: it writes its
	 * single-buffered plane surfaces on the queue that signals the deposit
	 * fence, BEFORE the signal the handoff's `fence_value` names (so the
	 * hub's producer wait covers them), and it GPU-waits for
	 * @ref comp_hub_d3d11_get_plane_release_value on the consumer fence before
	 * any submit that rewrites a plane (so a rewrite never overlaps a weave or
	 * repaint still sampling it). Without it — and always for a KEYED-MUTEX
	 * deposit, which has no consumer fence — a ring session moves to xbridge on
	 * its first plane frame (`reason=plane_content`), as before P2c.
	 */
	bool ring_direct_planes;

	/*!
	 * #2317 step 2: decline a SAME-ADAPTER engage with
	 * @ref COMP_HUB_REASON_SAME_ADAPTER_OPT_IN. The D3D11 shim sets it when the
	 * hub engages by default (no explicit `DXR_HUB=1`): the default-on scope is
	 * the cross-adapter (hybrid) topology until the single-dGPU desktop run of
	 * ADR-059's acceptance lands. Zero for every other caller (unchanged).
	 */
	bool same_adapter_needs_opt_in;
};

/*!
 * @name #2317 P1b: the `hub placement:` reason tokens
 *
 * The closed set the D3D11 tier's one-per-session `hub placement: hub=0
 * reason=<token>` line names, beside the `COMP_SPLIT_REASON_*` tokens the hub's
 * own Stage A can return (`killed_by_env`, `software_scanout`, `stage_a_failed`,
 * `dp_refused_scanout`, ...). Fixed parse targets; never reformatted.
 * @{
 */
/*!
 * RETIRED as an emitted token by #2317 step 2, kept on purpose. It meant
 * "`DXR_HUB` is not `1`" while the D3D11 shim was opt-in (P1b..P2c); since step 2
 * the in-process D3D11 leg weaves through the hub BY DEFAULT and the only way to
 * opt out is the kill switch, @ref COMP_HUB_REASON_DISABLED_BY_ENV. Field logs
 * from builds before step 2 still carry this string; nothing emits it today.
 */
#define COMP_HUB_REASON_NOT_REQUESTED "not_requested"
/*!
 * `DXR_HUB=0` (or another false spelling): the kill switch (#2317 step 2, the
 * #1264 / ADR-039 pattern). The tier runs its own legs exactly as before the
 * default flipped. Distinct from the split gate's `killed_by_env`, which names
 * `DXR_WEAVE_ON_SCANOUT=0` — that one refuses the hub too, through the gate.
 */
#define COMP_HUB_REASON_DISABLED_BY_ENV "disabled_by_env"
//! The runtime owns the window (`_hosted`): its drag phase-snap provider is the
//! tier's own display processor, which the hub would replace. Not carried yet.
#define COMP_HUB_REASON_HOSTED_WINDOW "hosted_window"
//! A transparent session (DComp chain + compose-under): not validated on the hub.
#define COMP_HUB_REASON_TRANSPARENT "transparent_background"
//! The producer could not create or share the atlas fence the hub waits on.
#define COMP_HUB_REASON_NO_SHARED_FENCE "no_shared_fence"
//! Stage A2 (bridge or direct wiring) failed after Stage A1 succeeded.
#define COMP_HUB_REASON_WIRE_FAILED "wire_failed"
/*!
 * #2317 step 2: the session asked for a PQ atlas (`DXR_ATLAS_ENCODING=pq`,
 * #2273, experimental). The encoding is negotiated with the tier's OWN display
 * processor (`accepts_atlas_encoding` / `set_atlas_encoding`), which the hub
 * replaces; until the hub forwards that negotiation to its weaver the session
 * stays native rather than silently weave an sRGB-declared PQ atlas or drop PQ.
 */
#define COMP_HUB_REASON_ATLAS_ENCODING "atlas_encoding_requested"
/*!
 * #2317 step 2 review: render == scanout and the hub was not explicitly
 * requested (`DXR_HUB=1`). The default-on scope is the cross-adapter (hybrid)
 * topology; a single adapter — every single-dGPU desktop — keeps the tier's own
 * legs (its ADR-039 same-adapter engage) until the desktop run of ADR-059's
 * acceptance has been made. `DXR_HUB=1` engages it there.
 */
#define COMP_HUB_REASON_SAME_ADAPTER_OPT_IN "same_adapter_opt_in"
/*!
 * Mid-session RETIRE (#2317 P2b): a frame needed a plane — the Local2D
 * over-flatten, the 2D-under backdrop or an authored mask — and the hub could not
 * carry it (the producer's NT share, the hub's open or bind, its composite unit
 * or its mask raster failed). Every one of those is an allocation-class failure
 * that would recur, so the tier's own legs take the session rather than ship it
 * without its 2D. Fires on the frame AFTER the failure (that one frame runs
 * without its 2D band).
 *
 * The P1b plane retires (`local2d_layer`, `zone_layer`, `authored_mask`) are
 * gone: Local2D, zones, authored masks and the backdrop ride the hub on both
 * ingress routes.
 *
 * So is `segments_planes` (P2b review; retired by #2342 P2c): a zones frame or
 * an authored mask while the window was segmented across screens used to retire
 * to the native legs, because the hub published the zone mask to its primary DP
 * only. The hub now publishes it to every segment DP cropped to its screen and
 * clears them with it (#2218 #4, the native rule), on every hub route.
 */
#define COMP_HUB_REASON_PLANE_UNAVAILABLE "plane_unavailable"

/*
 * #2317 P3: the OpenGL shim's refusals (comp_gl_compositor + comp_gl_deposit).
 * GL cannot be told which adapter to use (ADR-037 §5), so every one of these
 * is a statement about the adapter and driver GL already landed on.
 */
//! The GL context's adapter could not be identified: no `GL_DEVICE_LUID_EXT`
//! and no unambiguous GL_VENDOR / GL_RENDERER match. The deposit device would
//! have to be guessed, which the shim never does.
#define COMP_HUB_REASON_GL_NO_LUID "gl_no_luid"
//! Neither import path exists on this driver: no `GL_EXT_memory_object_win32` +
//! `GL_EXT_semaphore_win32` and no `WGL_NV_DX_interop2`.
#define COMP_HUB_REASON_GL_NO_INTEROP "gl_no_interop"
//! No `glClipControl` (GL 4.5 / ARB_clip_control): the atlas cannot be painted
//! top-down into the D3D11 deposit.
#define COMP_HUB_REASON_GL_NO_CLIP_CONTROL "gl_no_clip_control"
//! The deposit device, texture, fences or every GL import failed.
#define COMP_HUB_REASON_GL_DEPOSIT_FAILED "gl_deposit_failed"
//! The import succeeded but the create-time round-trip probe (GL clears a
//! sentinel, D3D11 reads one texel back) did not see GL's bytes in the D3D11
//! texture: weaving would show black or stale content.
#define COMP_HUB_REASON_GL_DEPOSIT_UNVERIFIED "gl_deposit_unverified"
/*!
 * Mid-session RETIRE: a frame carried a Local2D layer, a display-zones layer or
 * an authored zone mask. The GL leg's flatten scratches and masks are plain GL
 * textures, not D3D11-shareable surfaces, so the hub cannot carry them; the
 * GL compositor's own legs take the session and compose that frame.
 */
#define COMP_HUB_REASON_PLANES_UNSUPPORTED_GL "planes_unsupported_gl"
/*! @} */

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
 * @ref comp_hub_d3d11_retire restores exactly the state A1 skipped, through the
 * same single recovery path. See @ref comp_hub_d3d11_wire_bridge.
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
comp_hub_d3d11_stage_a(const struct comp_hub_d3d11_info *info,
                       struct comp_hub_d3d11 **out_split,
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
 *        slot is nominated per frame by @ref comp_hub_d3d11_submit_atlas.
 *
 * @return true when the bridge is live. On false the caller must
 *         @ref comp_hub_d3d11_retire immediately; nothing has presented yet, so
 *         that is a cold recovery.
 */
bool
comp_hub_d3d11_wire_bridge(struct comp_hub_d3d11 *split, const struct comp_hub_d3d11_handoff *handoff);

/*!
 * Release everything, in the reverse of the create order, and NULL the caller's
 * pointer. Quiesces the bridge first (a bounded CPU wait, teardown only) because
 * its egress slots are what the display processor is still sampling.
 */
void
comp_hub_d3d11_destroy(struct comp_hub_d3d11 **split_ptr);

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
comp_hub_d3d11_retire(struct comp_hub_d3d11 **split_ptr, const char *why, const char *short_reason);

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
 * OUTPUT device inside @ref comp_hub_d3d11_weave_and_present.
 *
 * @param handoff This frame's deposit slot, after `comp_vk_deposit_advance`.
 * @param cols,rows,view_w,view_h The frame's effective tile layout. A change in
 *        the GRID (not the tile size — that moves continuously through a resize)
 *        bumps the layout generation stamped on the slot, which is the #918 R1
 *        refusal test.
 */
void
comp_hub_d3d11_submit_atlas(struct comp_hub_d3d11 *split,
                            const struct comp_hub_d3d11_handoff *handoff,
                            uint32_t cols,
                            uint32_t rows,
                            uint32_t view_w,
                            uint32_t view_h);

/*!
 * VK-1b (#1178) — publish this frame's 2D-under BACKDROP plane.
 *
 * Call once per app frame, BEFORE @ref comp_hub_d3d11_submit_atlas: that call
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
 *
 * On the DIRECT ingress (#2317 P2b) the surface is not transported at all: the
 * hub opens @p nt_handle on its own device and reads it in place, ordered by the
 * atlas's two fences (the producer writes it before signalling).
 *
 * @return false only when a plane that was ASKED FOR could not be bound (the
 *         plane is then un-staged); a NULL / 0 drop is not a failure.
 */
bool
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
                              uint32_t bd_h);

/*!
 * @name VK-1b-2 — the output-device masked composite.
 *
 * The composite's destination is the WEAVE TARGET, which the split moved to the
 * scanout adapter, so the pass has to run there. @ref comp_d3d11_outcomp is the
 * unit for exactly that and is reused verbatim, as both D3D legs do; what this
 * rung supplies are its three inputs on that device — the `twod` layer (the
 * LOCAL2D bridge plane), the mask (rastered here) and the weave snapshot
 * (unit-owned).
 * @{
 */
//! No mask this frame — the composite runs Phase 0's analytic canvas-rect path.
#define COMP_HUB_D3D11_MASK_NONE 0u
//! #439 — M=1 everywhere, M=0 inside the OVER Local2D rects (ALPHA_OVER).
#define COMP_HUB_D3D11_MASK_IMPLICIT 1u
//! ADR-027 auto wish — M=0 everywhere, M=1 inside each zone rect. ALSO the
//! published wish: cosmetics never enter it (#800/#801).
#define COMP_HUB_D3D11_MASK_ZONE_BINARY 2u
//! #803 per-zone opt-in inward ramp. COMPOSITE-only; the wish stays binary.
#define COMP_HUB_D3D11_MASK_ZONE_FEATHER 3u

/*!
 * Raster this app frame's zone/Local2D mask **on the scanout device**.
 *
 * Every input is CPU-side — rects, radii, dims — which is the whole reason the
 * mask is REBUILT on the device that consumes it rather than transported. Only
 * an app-AUTHORED (Tier-3) mask has pixels the app drew, and that one is a bridge
 * plane (VK-1b-3).
 *
 * **Why this runs in the deposit half and not, as on the D3D12 leg, from a
 * captured request in the consume half.** D12-4 could not raster at deposit
 * because it records into a command list and `layer_commit` `Reset()`s the
 * out-device list *after* the deposit half runs, so the work would be thrown
 * away; hence its `out_mask_req`. This leg's out device is a **D3D11 device with
 * an immediate context** (@ref comp_hub_d3d11_stage_a creates it), so work is
 * submitted as it is written and there is no Reset to lose it to. The raster runs
 * inline, exactly as the shipped D3D11 leg's does. Do not "align" this with the
 * D3D12 shape — the difference is real and it is on this side.
 *
 * Dirty-checked on the rect set, the radii and the dims, so a steady-state frame
 * costs a compare. The result is cached as the REPAINT mask: a repaint replays
 * rendering and never re-rasters (#868), because rastering re-runs a
 * once-per-app-frame state machine at panel rate.
 *
 * @return true when a mask is available for this frame's composite.
 */
bool
comp_hub_d3d11_raster_mask(struct comp_hub_d3d11 *split,
                           uint32_t kind,
                           const struct xrt_rect *rects,
                           const float *feather_px,
                           uint32_t rect_count,
                           uint32_t region_w,
                           uint32_t region_h);

/*!
 * VK-1b-2 — publish this frame's LOCAL2D plane and the COMPOSITE recipe.
 *
 * Call once per app frame from the deposit half, before
 * @ref comp_hub_d3d11_submit_atlas. @p nt_handle / @p generation / @p content_seq
 * and the dirty box carry the meanings documented on
 * @ref comp_hub_d3d11_stage_backdrop.
 *
 * The rest is the recipe (#1140): the consume half reads every composite
 * parameter FROM THE SLOT, never from live CPU state, because the slot it is
 * finishing was filled by an earlier frame.
 *
 * @param composite_mode A `COMP_D3D11_COMPOSITE_MODE_*`.
 * @param mask_is_plane VK-1b-3 — the composite's mask is the bridged Tier-3
 *        plane rather than an out-device raster. A ZONES frame passes false even
 *        with a live plane: per ADR-027/#801 an explicit wish is PUBLISH-only and
 *        never a blend gate, so the composite there always gates on the binary
 *        zone raster.
 *
 * @return false when this frame could not stage a composite (no plane surface,
 *         a bind/open failure, or no composite unit) — the frame then runs
 *         `composite=false`. The D3D11 tier names that rung
 *         (@ref COMP_HUB_REASON_PLANE_UNAVAILABLE); the VK and D3D12 legs
 *         degrade the 2D band and ignore it.
 */
bool
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
                             bool mask_is_plane);

/*!
 * This frame runs NO composite. Stamps `composite=false` on the slot and
 * un-stages the Local2D plane, so a consume half can never read a slot claiming
 * a composite it has no pixels for.
 */
void
comp_hub_d3d11_stage_no_composite(struct comp_hub_d3d11 *split);

/*!
 * #1264 — declare the DXGI format of the caller's 2D plane surfaces (Local2D +
 * backdrop). The plane chains must share the source's typeless family — a
 * cross-family copy is DROPPED in silence, which is why the bind refuses
 * instead. Default (never calling this) is `DXGI_FORMAT_B8G8R8A8_UNORM`, the
 * VK deposit's family; the d3d12 reroute passes `R8G8B8A8`. Call once, after
 * @ref comp_hub_d3d11_wire_bridge and BEFORE the first backdrop/Local2D stage —
 * changing an already-bound chain's family is not supported. The mask plane is
 * R8 regardless.
 */
void
comp_hub_d3d11_set_plane_format(struct comp_hub_d3d11 *split, uint32_t dxgi_format);

/*!
 * True when the scanout-adapter display processor advertises hardware zone
 * slots. Probed once; the answer is the D3D11 weaver's, not the Vulkan one's,
 * which is why the compositor cannot answer it for a split session.
 */
bool
comp_hub_d3d11_zone_dp_supported(struct comp_hub_d3d11 *split);

/*!
 * #2317 P2b — the scanout weaver's hardware zone grid (what
 * `xrGetLocalZoneCapsDXR`-class queries report), for a producer whose own
 * display processor the hub replaced. False when the weaver has no zones.
 */
bool
comp_hub_d3d11_get_zone_grid(struct comp_hub_d3d11 *split, uint32_t *out_grid_w, uint32_t *out_grid_h);

/*!
 * ADR-027 P4 — publish the last @ref COMP_HUB_D3D11_MASK_ZONE_BINARY raster as
 * this client's hardware zone WISH, screen-anchored to the split's own HWND.
 *
 * The BINARY raster, never the feather one: feather is a cosmetic composite
 * opt-in and never enters the wish (#800/#803).
 *
 * This exists because the compositor's own sideband
 * (`xrt_display_processor_publish_local_zone_mask`) takes the **Vulkan** display
 * processor, which a split session does not have — leaving the publish silently
 * inert, which is the shape #1175 found on the D3D12 leg.
 *
 * @param seq The client's publish generation; a vendor treats an unchanged seq
 *        as an anchor-only update.
 */
void
comp_hub_d3d11_publish_zone_wish(struct comp_hub_d3d11 *split, uint64_t seq);

/*! Withdraw this client's zone contribution (the clear edge). */
void
comp_hub_d3d11_clear_zone_wish(struct comp_hub_d3d11 *split);

/*!
 * VK-1b-3 — bind the app-AUTHORED (Tier-3) zone mask's transport.
 *
 * The one mask whose pixels the APP drew, and therefore the one that cannot be
 * rebuilt from CPU rects on the scanout device the way every other kind is. It
 * has to cross, as `COMP_XBRIDGE_PLANE_MASK`.
 *
 * Sized at the **MASK**, not the panel (#918 review F5): the composite and the
 * display processor both stretch the whole mask over the region, so a
 * panel-sized mask plane would leave them stretching a never-written band. That
 * makes a dims change a real reallocation — but an ON-CHANGE one, never
 * per-frame, because the steady-state call is the handle+generation early-out
 * inside the bridge.
 *
 * Call once per app frame from `layer_commit`, BEFORE any command is recorded,
 * with the frame's authoritative mask. A false return is the ONE remaining
 * reason this leg retires the split (`authored_mask`): with no transport there is
 * no mask on the scanout adapter at all, and the choice is the app device or
 * wrong pixels. Retiring there is honest; a silent difference would put different
 * pixels on a hybrid box than on a single-GPU one.
 *
 * @param nt_handle The mask's staging surface, or NULL to drop the plane (which
 *        is not a failure — a frame with no authored mask passes NULL).
 * @return false only when a bind that was ASKED FOR could not be made.
 */
bool
comp_hub_d3d11_bind_mask_plane(
    struct comp_hub_d3d11 *split, void *nt_handle, uint64_t generation, uint32_t w, uint32_t h);

/*!
 * VK-1b-3 — stage the authored mask plane for the next submit.
 *
 * Deliberately separate from the bind, and in that order, for the two properties
 * the D3D legs learned:
 *
 *  1. **One producer.** Binding one plane from two sites with two generations
 *     re-opens it on alternating frames, each re-open a full re-transport.
 *  2. **Staging does not depend on consumption.** The transport is set up before
 *     anything asks whether the pixels have landed, so the frame that AUTHORS a
 *     mask transports it and consumption starts on whichever later frame the slot
 *     lands. Gating this on the composite instead is what makes a Tier-3 mask
 *     unable to bootstrap: no deposit, no recipe, no transport, no mask next
 *     frame either, for ever.
 *
 * @param content_seq 0 ⟹ this frame does not use the plane; the recipe then
 *        stamps it invalid rather than lending an older frame's pixels.
 */
void
comp_hub_d3d11_stage_mask_plane(struct comp_hub_d3d11 *split, uint64_t content_seq, uint32_t w, uint32_t h);

/*!
 * VK-1b-2 — hand the split this frame's diagnostic HUD bitmap.
 *
 * `u_hud` rasterises to a CPU pixel buffer, so the HUD belongs to no graphics
 * device: there is nothing to transport and no bridge plane involved. The
 * out-device half uploads @p pixels to a scanout-adapter texture and copies it
 * onto the back buffer after the composite — which is exactly what both D3D legs
 * already do (`d3d11_render_hud_overlay(c, d3d11_out_device(c), …)`). The HUD is
 * therefore never a reason to retire.
 *
 * @param pixels RGBA8, @p w x @p h, or NULL to stop drawing the HUD.
 * @param dirty The bitmap changed since the last call and must be re-uploaded.
 */
void
comp_hub_d3d11_set_hud(struct comp_hub_d3d11 *split, const void *pixels, uint32_t w, uint32_t h, bool dirty);
/*! @} */

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
comp_hub_d3d11_invalidate_plane(struct comp_hub_d3d11 *split, uint32_t plane);

/*!
 * #1178 — make the scanout swapchain follow the window.
 *
 * Under the split there is no VkSwapchain: the surface the panel actually shows
 * is a DXGI chain on the SCANOUT adapter that this unit owns and the Vulkan side
 * never sees. Nothing else can resize it, so this is not an optional leg of the
 * caller's resize path — it IS the resize path whenever the split is up, and
 * omitting it leaves a frozen-size back buffer that DXGI scales or clips into the
 * window while every counter reads healthy. That was #1178.
 *
 * Idempotent: a call at the size the chain already has does nothing and succeeds.
 *
 * @return true when the chain is at @p width x @p height afterwards.
 */
bool
comp_hub_d3d11_resize_target(struct comp_hub_d3d11 *split, uint32_t width, uint32_t height);

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
comp_hub_d3d11_weave_and_present(struct comp_hub_d3d11 *split, bool is_repaint, const struct xrt_rect *canvas);

/*!
 * #2317 P1c: the predicted display time xrWaitFrame handed the app for the
 * frame about to be woven (the tier's `last_display_time_ns`). Call before
 * the APP weave; the weave hands it to the target's weave mark, which is what
 * makes the `DXR_WEAVE_LATENCY_CSV` site emit D (prediction error) rows —
 * the same rows the tiers' own sites emit, so native and hub arms compare.
 * Repaints never carry one (they are not the frame the app was promised).
 */
void
comp_hub_d3d11_set_predicted_display_time(struct comp_hub_d3d11 *split, uint64_t predicted_display_time_ns);

/*!
 * True when the last app frame published a slot a repaint could replay. The
 * repaint tick tests this BEFORE acquiring, so a warmup tick costs no acquire.
 */
bool
comp_hub_d3d11_has_weave_slot(struct comp_hub_d3d11 *split);

/*!
 * #1339: @ref comp_d3d11_target_repaint_admit on the arm's own target — the
 * fill arm presents through its d3d11 flip chain, so THAT queue is the one a
 * repaint must not deepen. false = skip this repaint tick.
 */
bool
comp_hub_d3d11_repaint_admit(struct comp_hub_d3d11 *split);

/*!
 * @name #2342 P2c: the hub's ONE repaint pacer and admission
 *
 * Every hub route (the D3D11 shim, the D3D12 #1264 reroute, the Vulkan split)
 * replays through these, so the pacing and the admission are the hub's, read
 * once, and identical across routes: the native D3D11 leg's shape, which is
 * the reference (`[WITNESS]` repaint rates under the frame divisor and under
 * `DXR_WEAVE_REPAINT_FORCE=1`).
 *
 *  1. **Pace, unlocked** (skipped by the caller under an ENGAGED #1257
 *     partition, whose schedule IS the pacing — as natively):
 *     @ref comp_hub_d3d11_repaint_pace_hold under the compositor lock, then
 *     @ref comp_hub_d3d11_repaint_pace_wait without it. That is
 *     comp_d3d11_target_repaint_pace on the hub's target — the scanout wait
 *     plus the #884 late-weave sleep to `period - R` — made safe against a
 *     hub retire that runs while the repaint thread is sleeping. Before P2c
 *     the three hub routes skipped pacing altogether ("no app-side target"),
 *     so a forced repaint tried at the loop's free-running tick and the #1339
 *     admission refused it every time: the 0/s of #2321 surprise 6.
 *  2. **Fire, under the lock:** @ref comp_hub_d3d11_repaint — the
 *     has-a-frame test, the #1339 frame-latency admission (skipped under an
 *     engaged partition, as natively), then the replay weave.
 *
 * The tier keeps the parts that are its own state: armed / app-frame-in-
 * progress, the #1257 cadence gate and DXR_WEAVE_REPAINT_FORCE, and its trace
 * counters. The late-weave knob itself (`DXR_LATE_WEAVE`) is read once, by the
 * hub's target (comp_d3d11_target): no tier reads its own copy on a hub route.
 * @{
 */
void *
comp_hub_d3d11_repaint_pace_hold(struct comp_hub_d3d11 *split);

void
comp_hub_d3d11_repaint_pace_wait(void *hold);

//! What @ref comp_hub_d3d11_repaint did — a closed set the tiers map onto
//! their own trace counters.
enum comp_hub_d3d11_repaint_result
{
	//! The replay was woven and presented.
	COMP_HUB_D3D11_REPAINT_FIRED = 0,
	//! Nothing woven yet to replay (warmup, or an app frame that refused).
	COMP_HUB_D3D11_REPAINT_NO_FRAME,
	//! #1339: the present queue is at the frame-latency cap; skip this tick.
	COMP_HUB_D3D11_REPAINT_NOT_ADMITTED,
	//! A fire that presented nothing (#918 F4/F7: no ready slot); the panel
	//! holds the last frame. Counts as a fire for spacing.
	COMP_HUB_D3D11_REPAINT_NOT_WOVEN,
};

/*!
 * One replay attempt. Call under the compositor mutex, after the tier's own
 * armed / in-progress / gate tests. @p partition_engaged skips the admission
 * exactly as the native leg does (the #1257 schedule owns the slots).
 */
enum comp_hub_d3d11_repaint_result
comp_hub_d3d11_repaint(struct comp_hub_d3d11 *split, bool partition_engaged, const struct xrt_rect *canvas);
/*! @} */

/*!
 * #1482: @ref comp_d3d11_target_set_app_paced on the arm's own target — the
 * fill arm's app frames block on THAT chain's frame-latency waitable, so it is
 * that target which must stop draining surplus tokens while the #1257 grid is
 * the pacer. Pushed from the VK compositor's `u_app_partition_throttle` call,
 * keyed on the same compositor's own grid.
 */
void
comp_hub_d3d11_set_app_paced(struct comp_hub_d3d11 *split, bool paced);

/*!
 * The once-every-10-s `[RENDER] split=… xb_kb=… ingress=… ing_leak=…` line, in
 * the same shape the D3D12 leg emits so one grep covers every in-process leg.
 * Cheap to call every frame; rate-limits itself.
 */
void
comp_hub_d3d11_render_diag(struct comp_hub_d3d11 *split);

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
comp_hub_d3d11_get_predicted_eye_positions(struct comp_hub_d3d11 *split, struct xrt_eye_positions *out_eye_pos);

//! N6 Part 1 (#2144): the hub DP's target-time eye sampling slot, forwarded.
bool
comp_hub_d3d11_has_eye_positions_at(struct comp_hub_d3d11 *split);

bool
comp_hub_d3d11_get_eye_positions_at(struct comp_hub_d3d11 *split,
                                    int64_t target_ns,
                                    struct xrt_eye_positions *out_eye_pos);

bool
comp_hub_d3d11_get_display_dimensions(struct comp_hub_d3d11 *split, float *out_width_m, float *out_height_m);

bool
comp_hub_d3d11_get_display_pixel_info(
    struct comp_hub_d3d11 *split, uint32_t *out_width_px, uint32_t *out_height_px, int32_t *out_left, int32_t *out_top);

bool
comp_hub_d3d11_request_display_mode(struct comp_hub_d3d11 *split, bool enable_3d);

void
comp_hub_d3d11_set_eye_tracking_mode(struct comp_hub_d3d11 *split, uint32_t mode);

/*!
 * XR_DXR_depth_budget - one background preview from the split's weaver.
 *
 * The rear-depth policy needs the desktop UNDER the app window, and under the
 * split the display processor that has those pixels is the D3D11 one on the
 * scanout adapter: it is the weaver for this session, and the vendor plug-in
 * produces the preview as a downsample of the capture it already runs for
 * compose-under transparency. The Vulkan display processor - if a session even
 * still has one - is not weaving and is not capturing.
 *
 * Pixels only: the split forwards the vtable call and forms no opinion. The
 * policy stays in the compositor's @ref comp_rear_budget, so a session that
 * retires the split mid-flight keeps its state and simply starts polling the
 * Vulkan slot instead.
 *
 * @return false when there is no source right now, which the policy treats as
 *         the conservative case.
 */
bool
comp_hub_d3d11_get_background_preview(struct comp_hub_d3d11 *split, struct xrt_dp_background_preview *out_preview);
/*! @} */

/*!
 * @name #2317 P1b: the stable-atlas producer (the D3D11 tier's shim)
 * @{
 */

/*!
 * True when the hub reads the producer's atlas DIRECTLY on its own device
 * (same adapter: the stable atlas, or since #2317 P2a a ring deposit's slot): no
 * comp_xbridge, no transport copy. A stable-atlas producer must then signal the
 * handoff's fence after the passes that write the atlas, and flush; a ring
 * producer skips its deposit's note_consumed calls and takes the owed releases
 * from @ref comp_hub_d3d11_take_slot_release instead. Can turn false mid-session
 * (a ring-contract or slot-open failure, or the first 2D-plane frame of a ring
 * producer without comp_hub_d3d11_info::ring_direct_planes), never back.
 */
bool
comp_hub_d3d11_is_direct(struct comp_hub_d3d11 *split);

/*!
 * The producer's back-fence, called on the producer's thread before the first
 * pass that rewrites the atlas. GPU-side only (`ID3D11DeviceContext4::Wait` on
 * the producer's immediate context), never a CPU wait.
 *
 * DIRECT: waits for the hub's last READ of the atlas (every app weave and every
 * repaint signals the hub's consumer fence once it has recorded the reads).
 * Option I through comp_xbridge: @ref comp_xbridge_pre_render. A no-op for the
 * ring deposits.
 *
 * The caller serialises this against @ref comp_hub_d3d11_weave_and_present (the
 * compositor mutex), so the value waited for is the last one any weave recorded.
 */
void
comp_hub_d3d11_pre_render(struct comp_hub_d3d11 *split);

/*!
 * The display period of the hub's target (the late-weave governor and the
 * forward horizon), as the tier's own legs set it on their target. Optional: a
 * caller that never sets it leaves the target on its default.
 */
void
comp_hub_d3d11_set_display_period(struct comp_hub_d3d11 *split, uint64_t period_ns);

/*!
 * #867 forwarders on the hub's target, so a tier whose wait_frame used its own
 * target's measured lookahead keeps doing so under the hub. 0 = no measurement.
 */
uint64_t
comp_hub_d3d11_get_predicted_lookahead_ns(struct comp_hub_d3d11 *split);

void
comp_hub_d3d11_mark_wait_frame(struct comp_hub_d3d11 *split);

//! Packed LUID (HighPart<<32 | LowPart) of the hub's own device, for the placement line.
uint64_t
comp_hub_d3d11_get_hub_luid(struct comp_hub_d3d11 *split);

/*!
 * @name #2317 P2a: DIRECT ingress for the RING producers (the D3D12 #1264
 * reroute and the Vulkan deposit)
 *
 * On ONE adapter the hub opens each deposit ring slot on its own device and
 * weaves it in place: no comp_xbridge, no transport copy (xbridge's staged
 * ingress paid three: staging, cross-adapter heap, egress). OPT-IN for now:
 * `DXR_HUB_INGRESS=direct` (the default stays xbridge,
 * `ingress_reason=default_xbridge`, until a single-dGPU desktop has run VK
 * fence-mode DIRECT; the flip is tracked in #2317). `DXR_HUB_INGRESS=xbridge`
 * forces the bridge for every producer, the D3D11 shim included.
 * Cross-adapter sessions always take xbridge.
 *
 * Synchronisation, all GPU-side (ADR-055):
 * - **write -> read:** the producer signals the deposit's shared fence after
 *   the atlas (as it always did); every hub weave of that slot waits for it on
 *   the hub's immediate context.
 * - **read -> rewrite:** the hub signals its OWN consumer fence after every
 *   weave's reads (app and repaint). When the next app frame moves the hub off
 *   a slot, that slot's last read value is OWED to the producer
 *   (@ref comp_hub_d3d11_take_slot_release); the producer's next write into
 *   the slot GPU-waits for it on the consumer fence it opened from
 *   @ref comp_hub_d3d11_get_consumer_fence_handle. A separate fence rather than
 *   the deposit's own, so each timeline has exactly one signaller and can never
 *   step backwards.
 * - **KEYED-MUTEX deposits** (no D3D12_FENCE import, e.g. Intel UHD Vulkan):
 *   no fences at all. The hub brackets each weave's reads with the slot's own
 *   keyed mutex (key 0, bounded acquire, skip-on-timeout — the deposit's
 *   documented contract), which carries both edges. Uncontended by
 *   construction: the producer only re-enters a slot after the hub has moved
 *   off it.
 *
 * **Planes (#2317 P2c).** A producer that declares
 * @ref comp_hub_d3d11_info::ring_direct_planes keeps its planes on DIRECT: the
 * hub opens each plane surface on its own device (as it does the stable atlas's)
 * and reads it in place, and the frame's recipe is snapshotted beside the slot it
 * nominates (a DIRECT weave only ever weaves the newest nominated frame, so one
 * snapshot is the whole ring's). The planes are single-buffered, so their two
 * edges are:
 * - **write -> read:** the producer writes them before the deposit signal the
 *   handoff names; the weave's producer wait already covers them.
 * - **read -> rewrite:** EVERY weave and repaint samples them, not just the one
 *   that moved onto a slot, so the producer GPU-waits for the consumer fence's
 *   LATEST value (@ref comp_hub_d3d11_get_plane_release_value) before a submit
 *   that rewrites a plane. The caller holds the compositor mutex from that wait
 *   through @ref comp_hub_d3d11_submit_atlas, so no repaint is recorded between
 *   the two.
 * - **keyed-mutex deposits** have no consumer fence to order the rewrite on, and
 *   DIRECT samples the planes in place on every repaint, so they keep the P2a
 *   move to xbridge on their first plane frame (`reason=plane_content`), where
 *   the planes are copied into hub-owned slots under the deposit's documented
 *   timing-only contract (#1274).
 *
 * A producer that does NOT declare it keeps the P2a rule: the first frame that
 * stages real 2D-plane content moves the session to the xbridge ingress for good
 * (`hub placement: CHANGED ... ingress=xbridge reason=plane_content`) BEFORE that
 * frame is submitted, so no frame ever loses its planes.
 * @{
 */

//! `DXR_HUB_INGRESS=xbridge`: the A/B control / kill switch.
#define COMP_HUB_INGRESS_REASON_FORCED "forced_by_env"
//! A ring producer on one adapter without `DXR_HUB_INGRESS=direct`: DIRECT is
//! opt-in for the ring producers until the single-GPU desktop gate (#2317).
#define COMP_HUB_INGRESS_REASON_DEFAULT_XBRIDGE "default_xbridge"
//! Render and hub devices are on different adapters: xbridge is the transport.
#define COMP_HUB_INGRESS_REASON_CROSS_ADAPTER "cross_adapter"
//! DIRECT was eligible but a fence/texture/interface it needs was unavailable.
#define COMP_HUB_INGRESS_REASON_DIRECT_FAILED "direct_wire_failed"
//! Mid-session: a frame staged 2D-plane content, which only xbridge carries.
#define COMP_HUB_INGRESS_REASON_PLANES "plane_content"
//! A ring producer that does not declare @ref comp_hub_d3d11_info::ring_direct.
#define COMP_HUB_INGRESS_REASON_PRODUCER "producer_unsupported"
//! Mid-session backstop: the producer re-nominated the slot the hub is reading
//! with a newer fence value (it rewrote it without handing over another slot).
#define COMP_HUB_INGRESS_REASON_RING_CONTRACT "ring_contract"

/*!
 * NT handle of the hub's consumer fence (`ID3D11Fence`, created SHARED on the
 * hub's device), or NULL when no ring producer runs DIRECT in fence mode. The
 * producer opens it ONCE after @ref comp_hub_d3d11_wire_bridge; the handle stays
 * valid for the hub's lifetime (it survives a demotion to xbridge, because
 * releases owed from the DIRECT era are still waited on).
 */
void *
comp_hub_d3d11_get_consumer_fence_handle(struct comp_hub_d3d11 *split);

/*!
 * #2317 P2c: the consumer-fence value a ring producer's next PLANE rewrite must
 * GPU-wait for (on the fence it opened from
 * @ref comp_hub_d3d11_get_consumer_fence_handle): the value the hub's latest
 * weave or repaint signalled after its reads, so every read of the
 * single-buffered plane surfaces recorded so far. 0 = nothing to wait for (no
 * DIRECT fence-mode ring session, or no weave yet). Stays valid after a move to
 * xbridge, so the first bridged plane write still waits out the DIRECT era's
 * last reads.
 */
uint64_t
comp_hub_d3d11_get_plane_release_value(struct comp_hub_d3d11 *split);

/*!
 * Pop one owed slot release: the hub will never read @p out_slot again until
 * the producer re-nominates it, and every read it recorded completes when the
 * consumer fence reaches @p out_value. Call after every
 * @ref comp_hub_d3d11_submit_atlas, in a loop until false, and hand each to the
 * deposit (the producer's next write into that slot waits for it, GPU-side).
 * Always false on xbridge-only and keyed-mutex sessions.
 */
bool
comp_hub_d3d11_take_slot_release(struct comp_hub_d3d11 *split, uint32_t *out_slot, uint64_t *out_value);

/*!
 * Move a ring producer's session off DIRECT onto the xbridge ingress for good
 * (the same one-time move 2D-plane content triggers), naming @p reason — for a
 * producer that cannot honour the DIRECT contract, e.g. one whose API would not
 * open the consumer fence. Call before the first frame is submitted, or at any
 * app-frame boundary. No-op unless a ring producer runs DIRECT.
 */
void
comp_hub_d3d11_leave_direct(struct comp_hub_d3d11 *split, const char *reason);

/*!
 * Why this session's ingress is xbridge (one of `COMP_HUB_INGRESS_REASON_*`),
 * or NULL when it is DIRECT.
 */
const char *
comp_hub_d3d11_ingress_reason(struct comp_hub_d3d11 *split);

/*!
 * The ONE `hub placement: hub=1 render=<luid> hub_dev=<luid> ingress=direct|xbridge
 * [ingress_reason=<token>]` WARN for a ring producer's session, in the D3D11
 * shim's format. Call once, after @ref comp_hub_d3d11_wire_bridge succeeded.
 *
 * @param producer "d3d12" or "vk" — prose only, after the parse fields.
 */
void
comp_hub_d3d11_log_placement(struct comp_hub_d3d11 *split, const char *producer);
/*! @} */

/*!
 * Called on the weaving thread after every weave (app and repaint) and BEFORE
 * the present, with the hub's device, immediate context and woven back buffer
 * (`ID3D11Device *`, `ID3D11DeviceContext *`, `ID3D11Texture2D *`). For the
 * tier's capture triggers, which need the post-weave pixels the hub owns. Keep
 * it cheap: it runs at panel rate.
 */
typedef void (*comp_hub_d3d11_post_weave_fn)(
    void *userdata, void *device, void *context, void *back_buffer, uint32_t width, uint32_t height, bool is_repaint);

void
comp_hub_d3d11_set_post_weave_hook(struct comp_hub_d3d11 *split, comp_hub_d3d11_post_weave_fn fn, void *userdata);
/*! @} */

/*!
 * @name #2342 P2c: the ONE capture site for every hub route
 *
 * The shared atlas trigger (u_capture_intent: `%TEMP%\displayxr_atlas_trigger`,
 * its `.projection` and per-exe variants, and MCP `capture_frame`) is polled
 * by the tier as before. Exactly one consumer per trigger:
 *
 * - **POST_COMPOSE (full) triggers** — the tier hands them to the hub at the
 *   top of the frame (its own post-compose point, which on the D3D12 reroute
 *   never ran, then stays idle). The hub writes, on the next APP frame it
 *   weaves, with the service twin's names (#2207 / #2289): the DP input — the
 *   content-cropped atlas handed to process_atlas (ADR-030), pre-weave — to
 *   `displayxr_atlas[.<exe>].png`, and the post-weave back buffer (weave +
 *   masked composite + HUD, what is presented) to
 *   `displayxr_atlas[.<exe>].woven.png`.
 * - **PROJECTION_ONLY triggers** stay with the tier's own projection-done
 *   point, which reads the renderer atlas BEFORE the window-space pass (the
 *   HUD-free capture `/make-app-logos` depends on). Only if that point was
 *   skipped (zero-copy) and the intent is still pending at the handoff does the
 *   tier hand it to the hub, which then writes the DP input alone to
 *   `displayxr_atlas[.<exe>].projection.png`.
 *
 * Alpha is forced opaque unless `DXR_ATLAS_CAPTURE_RAW_ALPHA=1`. One WARN per
 * completed capture names the files. A readback (one staging copy + a Map) runs
 * only on the frame that captures.
 * @{
 */

/*!
 * Arm a capture for the next APP-frame weave (never a repaint). @p path is the
 * intent's output path and @p woven_path its post-weave sibling (the tier
 * derives it with u_capture_intent_sibling_path; ignored for projection-only).
 * Always arms, even with an empty path: the write then fails and the intent
 * completes as failed, so an MCP waiter is never left re-arming. A second arm
 * before the first was taken replaces it.
 */
void
comp_hub_d3d11_capture_arm(struct comp_hub_d3d11 *split,
                           bool projection_only,
                           const char *path,
                           const char *woven_path);

/*!
 * After the app-frame weave: true when a capture was armed (it is now
 * disarmed, whatever happened), with @p out_ok whether its DP input was
 * written. A frame that wove nothing (#918 F4) consumes the arm with
 * @p out_ok false, so the tier can complete its intent (and an MCP waiter)
 * rather than leave it pending.
 */
bool
comp_hub_d3d11_capture_take(struct comp_hub_d3d11 *split, bool *out_ok);
/*! @} */

/*!
 * @name Multi-screen M6 under the split (ADR-047 Amendment 1, Vulkan)
 *
 * Under the split the weave is the D3D11 one on the scanout adapter, so a
 * window spanning screens is segmented the way the in-process D3D11
 * compositor segments it under ITS split: the D3D11 segment manager
 * (comp_d3d11_segments) lives on the OUTPUT device, its DPs come from each
 * screen's `create_dp_d3d11_for_screen`, they weave the egress slot, and the
 * window handle follows the majority screen (ADR-047 Amendment 2) by swapping
 * the split's own DP. The Vulkan segment manager is not used while the split
 * is up.
 * @{
 */

/*!
 * Hand over the system's screens and DP registry (copied). The segment
 * manager is (re)built on the next weave. Caller holds the compositor lock.
 *
 * @param list               The screens (xrEnumerateDisplaysDXR's list).
 * @param info               System compositor info: the DP registry.
 * @param pinned_display_id  The session's display binding, 0 = none (a pinned
 *                           session is never segmented).
 */
void
comp_hub_d3d11_set_screens(struct comp_hub_d3d11 *split,
                           const struct xrt_screen_list *list,
                           const struct xrt_system_compositor_info *info,
                           uint64_t pinned_display_id);

/*!
 * The segment table the last weave took, as per-segment view metrics, with
 * each segment's eyes predicted NOW (the primary from the split's DP, every
 * other screen from its segment DP). App thread (xrLocateViews).
 *
 * @return false when the last weave did not split the window.
 */
bool
comp_hub_d3d11_get_segment_metrics(struct comp_hub_d3d11 *split, struct xrt_segment_metrics *out);

/*!
 * #1883: the segment partition the atlas about to be submitted was PAINTED
 * with (the renderer's per-segment mosaic; NULL / count 0 = unrouted). Call
 * right before @ref comp_hub_d3d11_submit_atlas: the submit records it under the
 * slot's sequence, so the weave crops each segment's views out of the egress
 * slot where THAT frame painted them — the slot lags the committed frame.
 */
void
comp_hub_d3d11_stage_segment_content(struct comp_hub_d3d11 *split, const struct comp_segments_content *content);
/*! @} */

#ifdef __cplusplus
}
#endif
