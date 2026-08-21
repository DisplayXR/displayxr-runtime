// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cross-adapter atlas bridge for the D3D11 output-device split (#918).
 * @ingroup comp_d3d11
 *
 * On a non-MUX hybrid laptop the 3D panel is scanned out by the iGPU while the
 * app renders on the dGPU. Today's placement weaves and presents on the app's
 * adapter, so every woven frame (and every repaint tick) crosses the adapter
 * boundary inside `Present` — measured at ~42 ms present-to-display and
 * ~330 ms/s of dGPU copy engine under a forced repaint
 * (docs/investigations/hybrid-igpu-weave.md).
 *
 * The output-device split moves the target, the display processor, the HUD and
 * the repaint loop onto the scanout adapter and sends only the *atlas* across —
 * once per app frame, never per repaint. This unit is that transport.
 *
 * D3D11 has **no** cross-adapter texture path on this stack (all six share
 * flavours fail at the open call); D3D12 cross-adapter heaps do work. So the
 * bridge is D3D12 in the middle with D3D11 on both ends:
 *
 * @verbatim
 *   app D3D11 atlas  --(same-adapter NT share)-->  producer D3D12 (app adapter)
 *        |                                              | COPY queue
 *        | f_app (D3D11 fence, NT-shared)               v
 *        |                                   cross-adapter placed ring x2
 *        |                                   (SHARED|SHARED_CROSS_ADAPTER heap,
 *        |                                    ROW_MAJOR ALLOW_CROSS_ADAPTER)
 *        |                                              | f_xa
 *        |                                              v
 *        |                                   consumer D3D12 (scanout adapter)
 *        |                                              | COPY queue
 *        |                                              v
 *        +----------------------------->  egress ring x3: output-D3D11 textures
 *                                          (NT-shared, opened by the consumer),
 *                                          each with an SRV the DP samples.
 * @endverbatim
 *
 * **INVARIANT: no CPU wait anywhere on the app render thread or the weave
 * path.** The only cross-adapter wait belongs to the consumer's own copy queue.
 * The weave picks the newest egress slot whose completion fence has already
 * fired (`comp_d3d11_xbridge_pick_slot`) and takes a *GPU-side* wait on the
 * output context. Bounded CPU waits exist only in teardown.
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

struct comp_d3d11_xbridge;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Everything the bridge needs to stand itself up. All pointers are borrowed —
 * the bridge AddRefs what it keeps and never outlives its creator.
 */
struct comp_d3d11_xbridge_info
{
	void *app_device;  //!< `ID3D11Device *` on the app (render) adapter.
	void *app_context; //!< `ID3D11DeviceContext *`, the app's immediate context.
	void *app_adapter; //!< `IDXGIAdapter *` the app device lives on.
	void *out_device;  //!< `ID3D11Device *` on the scanout adapter.
	void *out_context; //!< `ID3D11DeviceContext *` of @ref out_device.
	void *out_adapter; //!< `IDXGIAdapter *` that scans out the panel.

	//! Worst-case system atlas (u_tiling_compute_system_atlas) — the
	//! cross-adapter ring is sized for this once, so a mode switch never
	//! reallocates the heap mid-flight.
	uint32_t max_width;
	uint32_t max_height;

	/*!
	 * Panel extent (`xdev->hmd->screens[0]`). The two 2D PLANES are sized from
	 * this ONCE and never resized — a window can never exceed the panel, so a
	 * plane allocated here fits every region the session will ever composite.
	 * That is deliberate: it puts them structurally outside the R2 churn
	 * hysteresis and the #1091 resize path, where a per-size realloc of three
	 * NT-shared textures on the frame path is exactly what cost 21 of 50 frames.
	 *
	 * The authored-MASK plane is the exception and is sized at the MASK — see
	 * @ref comp_d3d11_xbridge_bind_plane.
	 */
	uint32_t panel_width;
	uint32_t panel_height;
};

/*!
 * #918 Phase 2a — the bridge PLANES. Beside the atlas, the masked composite
 * needs three more app-device images on the scanout adapter. Each rides the
 * SAME egress slot as the atlas (parallel per-slot arrays, copies recorded into
 * the same producer/consumer command lists), so atlas + planes land atomically
 * under one seq and one fence pair — which makes the layout-generation refusal,
 * the slot-readiness check and the repaint slot republish cover them for free.
 * @{
 */
//! Local2D OVER flatten (RGBA). Per frame, dirty-box + change-skip.
#define COMP_D3D11_XBRIDGE_PLANE_LOCAL2D 0u
//! 2D-under backdrop flatten (RGBA), handed to the DP's set_background_2d.
#define COMP_D3D11_XBRIDGE_PLANE_BACKDROP 1u
//! Tier-3 app-authored zone mask (R8). On `author_seq` change only.
#define COMP_D3D11_XBRIDGE_PLANE_MASK 2u
#define COMP_D3D11_XBRIDGE_PLANE_COUNT 3u
/*! @} */

//! @ref comp_d3d11_xbridge_recipe::mask_kind
#define COMP_D3D11_XBRIDGE_MASK_NONE 0u
//! An output-device raster (auto wish / implicit / feather / Tier-1-2 shadow).
#define COMP_D3D11_XBRIDGE_MASK_OUT_RASTER 1u
//! The bridged Tier-3 authored mask plane.
#define COMP_D3D11_XBRIDGE_MASK_PLANE 2u

/*!
 * #918 Phase 2a — the COMPOSITE RECIPE stamped on an egress slot.
 *
 * Same defect class as the Phase-1 offset-cube fix, closed pre-emptively. Under
 * the split the weave consumes a slot some EARLIER frame filled, so reading the
 * composite's parameters from live CPU state can pair one frame's pixels with
 * the next frame's recipe — exactly what `eg_gen` already forbids for the atlas.
 * The consume half therefore reads these FROM THE SLOT.
 */
struct comp_d3d11_xbridge_recipe
{
	//! True when the frame that filled this slot ran the masked composite at
	//! all. A projection-only frame stamps false and the consume half skips.
	bool composite;
	//! COMP_D3D11_COMPOSITE_MODE_* (LERP / ALPHA_OVER / ZONES).
	uint32_t composite_mode;
	//! COMP_D3D11_XBRIDGE_MASK_*.
	uint32_t mask_kind;
	//! #833/#116 opaque present on a transparent session.
	bool opaque_present;
	//! Composite region (window px inside the worst-case surface, #464).
	uint32_t region_w, region_h;
	/*!
	 * The BACKDROP plane's own extent, which is NOT the composite region: a
	 * frame can flatten a backdrop and then fail (or skip) the composite, and the
	 * DP's `set_background_2d` contract takes the backdrop's real width/height.
	 * 0 when the frame produced no backdrop.
	 */
	uint32_t bd_w, bd_h;
	//! Effective canvas sub-rect, already clamped to the region.
	int32_t cx, cy;
	uint32_t cw, ch;
	/*!
	 * Bit i set ⟹ plane i's pixels in this slot ARE the generation the frame
	 * staged. A change-skip satisfies that by construction; a half-rate or
	 * empty-box skip does not, and leaves the bit clear (#918 review F3).
	 */
	uint32_t plane_valid;
	/*!
	 * Content generation of each plane's pixels in this slot — the generation
	 * the slot actually HOLDS, which for a skipped plane is the older one it
	 * still carries. Passed back to @ref comp_d3d11_xbridge_get_plane_srv as
	 * `want_seq`, so the consume half proves the pixels it is about to sample
	 * are still the ones its recipe describes rather than a later frame's.
	 */
	uint64_t plane_seq[COMP_D3D11_XBRIDGE_PLANE_COUNT];
};

/*!
 * Stage A: devices, queues, the cross-adapter heap + placed ring, and every
 * fence — plus one probe egress round-trip, so an unshareable egress is
 * discovered before the caller commits to the split. The egress RING itself is
 * not allocated here; the caller must follow with
 * @ref comp_d3d11_xbridge_set_content_size (or
 * @ref comp_d3d11_xbridge_alloc_worstcase_egress) and only activate the split
 * once that succeeded, so a later re-size failure can never leave the session
 * with nothing to weave.
 *
 * @param out_reason On failure, receives a static string naming the exact
 *        fallback reason for the caller's one WARN. Never NULL on failure.
 */
xrt_result_t
comp_d3d11_xbridge_create(const struct comp_d3d11_xbridge_info *info,
                          struct comp_d3d11_xbridge **out_xb,
                          const char **out_reason);

/*!
 * Allocate the egress ring at the WORST-CASE system atlas. Part of Stage A, so
 * the split only ever activates with a working egress ring in hand — a later
 * Stage-B failure can then never leave the session with nothing to weave.
 */
bool
comp_d3d11_xbridge_alloc_worstcase_egress(struct comp_d3d11_xbridge *xb);

/*!
 * Stage B: (re)allocate the egress ring at the frame's real content size, so
 * the consumer copy lands content-sized and the compositor's crop pass can be
 * skipped entirely. Cheap to call every frame — no-op when the size is
 * unchanged. Returns false if the allocation failed, in which case the previous
 * (worst-case) ring is restored and the caller crops on the output device.
 *
 * **#918 R2 hysteresis.** A size that keeps CHANGING — an interactive resize
 * drag moves the content box on every mouse event — does not realloc per size:
 * the ring switches to worst-case once and stays there until the size has held
 * still for half a second, and the caller crops instead (as it already does for
 * any worst-case ring). Measured on the reference panel: a 1.2 s edge drag cost
 * 12 ring rebuilds, and the split delivered 29 frames against the non-split
 * path's 50 with a 161 ms worst gap. A rebuild is not free — it drains the
 * consumer fence, then releases and recreates three NT-shared textures, their
 * SRVs and their D3D12 opens, on the frame path. A window MOVE changes no size,
 * rebuilds nothing, and already matched the non-split path; this makes a resize
 * behave the same way.
 *
 * @param layout_gen The caller's layout generation. A size change carrying a NEW
 *        generation is a mode switch — one step, then still — and is deliberately
 *        NOT treated as churn.
 */
bool
comp_d3d11_xbridge_set_content_size(struct comp_d3d11_xbridge *xb, uint32_t w, uint32_t h, uint64_t layout_gen);

/*!
 * The layout that produced @p slot's pixels: the generation passed to
 * @ref comp_d3d11_xbridge_submit and the content box that frame painted.
 *
 * Both matter to the weave. The generation is a REFUSAL test — a slot composited
 * under a layout the DP has since switched away from must never be woven (#918
 * R1). The content box is the geometry to weave a surviving slot WITH: during a
 * resize it is a frame behind the window, and cropping it to the current frame's
 * box would slice the tiles at the wrong stride.
 *
 * @return false when the slot holds nothing (empty ring, or out of range).
 */
bool
comp_d3d11_xbridge_slot_layout(
    struct comp_d3d11_xbridge *xb, int32_t slot, uint64_t *out_gen, uint32_t *out_w, uint32_t *out_h);

/*!
 * Ingress Option I: bind the renderer's NT-shared atlas directly, so the
 * producer copies out of the app's own atlas with no extra app-device copy.
 *
 * @param nt_handle `HANDLE` from `comp_d3d11_renderer_get_atlas_shared_handle`.
 * @param generation Renderer atlas generation; a change re-opens the handle.
 *
 * Returns false if the D3D12 open failed — the bridge then latches Option II
 * (an app-device NT-shared ingress ring filled by one CopySubresourceRegion in
 * layer_commit) automatically and keeps working.
 */
bool
comp_d3d11_xbridge_bind_atlas(struct comp_d3d11_xbridge *xb, void *nt_handle, uint64_t generation);

/*!
 * Select ingress Option II — the app-device NT-shared staging ring — UP FRONT,
 * instead of reaching it only as the Option-I failure fallback.
 *
 * Option I binds ONE app-device atlas by NT handle and lets the producer read it
 * in place, which is right for a compositor that owns its renderer atlas for the
 * life of the session. A caller whose source texture changes IDENTITY between
 * frames — the service's atlas is whichever focused client's surface it is
 * compositing this tick — would instead re-open a shared handle per frame, and
 * each re-open drains the producer queue (see
 * @ref comp_d3d11_xbridge_bind_atlas). Such a caller wants the staging ring from
 * the start: one extra same-adapter copy per frame, and the source texture may
 * then be anything, re-created whenever.
 *
 * **Call right after @ref comp_d3d11_xbridge_create, before the first
 * @ref comp_d3d11_xbridge_submit.** It allocates the staging ring, so calling it
 * mid-flight would change ingress flavour under an in-flight producer copy.
 * Idempotent, and a no-op returning true if the bridge already latched Option II
 * on its own.
 *
 * @return false when the staging ring could not be allocated — the bridge is
 *         then inoperative and the caller must not activate the split.
 */
bool
comp_d3d11_xbridge_force_staged_ingress(struct comp_d3d11_xbridge *xb);

/*!
 * Back-pressure for ingress Option I (#918 review F6). The producer's copy of
 * frame N-1 reads the app's atlas directly, so the renderer passes of frame N
 * must not start overwriting it until that copy has retired. Call at the TOP of
 * the compositor's renderer section, before the first pass writes the atlas.
 *
 * Issues a **GPU-side** wait on the app's immediate context only — the
 * no-CPU-wait-on-the-app-thread invariant stands. No-op in Option II (whose
 * staging ring already gives the copy a private slot) and no-op when the app
 * device could not open the producer fence (that case latches Option II).
 */
void
comp_d3d11_xbridge_pre_render(struct comp_d3d11_xbridge *xb);

/*!
 * #918 Phase 2a — stand up the plane transport (panel-sized, once).
 *
 * Per-plane LAZY allocation: each plane gets its OWN cross-adapter heap + placed
 * ring and its own egress textures, created on the first bind. A plane that
 * cannot be allocated degrades THAT FEATURE with one WARN — Local2D stops
 * compositing, say — and never the split, which is why the heaps are not one
 * shared allocation.
 *
 * A plane that has failed once is never retried, so these calls are safe to make
 * unconditionally on the frame path.
 *
 * Bind the APP-DEVICE source texture for @p plane. The producer opens it
 * directly (the same Option-I shape as the atlas — no extra app-device copy),
 * so the texture must have been created `SHARED | SHARED_NTHANDLE`.
 *
 * @param nt_handle `HANDLE` from `IDXGIResource1::CreateSharedHandle`, or NULL
 *        to drop the plane.
 * @param generation Bumped by the caller whenever the texture is REALLOCATED; a
 *        change re-opens the handle (and drains the producer first).
 * @param dxgi_format The plane's DXGI format — RGBA8 for the 2D planes, R8 for
 *        the authored mask. Probed against the cross-adapter heap at first use;
 *        a format the stack refuses disables that plane alone.
 * @param w,h Extent to allocate the plane's chain at. The 2D planes pass the
 *        PANEL and so never resize — that is what keeps them outside the R2
 *        churn path. The authored MASK passes the MASK's own dims: it maps
 *        stretch-to-region, so a panel-sized mask plane would leave the
 *        composite stretching a never-written band over the region (#918 review
 *        F5). A dims change rebuilds the chain, drained; for the mask that is an
 *        on-change event, never a per-frame one.
 *
 * @return true when the plane is live.
 */
bool
comp_d3d11_xbridge_bind_plane(struct comp_d3d11_xbridge *xb,
                              uint32_t plane,
                              void *nt_handle,
                              uint64_t generation,
                              uint32_t dxgi_format,
                              uint32_t w,
                              uint32_t h);

/*!
 * Stage @p plane for the NEXT @ref comp_d3d11_xbridge_submit.
 *
 * @param content_seq A hash of the content the source texture now holds. The
 *        copy is SKIPPED when the write slot already carries this exact seq —
 *        mandatory, not an optimisation: a full-window RGBA plane at 4K60 is
 *        1.99 GB/s on its own.
 * @param x,y,w,h The DIRTY BOX in source pixels. The bridge accumulates it per
 *        slot, so a slot that missed several changes is refreshed with the union
 *        of all of them and never sees a partially-updated image.
 */
void
comp_d3d11_xbridge_stage_plane(
    struct comp_d3d11_xbridge *xb, uint32_t plane, uint64_t content_seq, int32_t x, int32_t y, uint32_t w, uint32_t h);

/*!
 * #918 review F4 — drop every slot's pixels for @p plane and make each owe a
 * FULL refresh of the plane extent.
 *
 * For the caller whose SOURCE content moved without the source texture being
 * reallocated: a composite region change leaves stale pixels outside the new
 * region in a panel-sized scratch, and the dirty-box union that follows would
 * carry them across as if they were content. The caller clears its whole scratch
 * and calls this, so the next copy is a full refresh of known-good pixels.
 */
void
comp_d3d11_xbridge_invalidate_plane(struct comp_d3d11_xbridge *xb, uint32_t plane);

/*!
 * #918 review, R1-adjacent — order an app-device write to @p plane's SOURCE
 * behind the producer's in-flight read of it.
 *
 * @ref comp_d3d11_xbridge_pre_render covers everything layer_commit writes, but
 * the authored mask is staged from an OpenXR entry point of the app's, before
 * layer_commit runs — so that write preceded the frame's back-fence in the
 * command stream and raced the producer's copy of the previous seq. Call
 * immediately before any such out-of-band write.
 *
 * GPU-side wait on the app's immediate context only; this thread never waits,
 * and the wait is free after the first call for a given producer seq.
 */
void
comp_d3d11_xbridge_pre_plane_write(struct comp_d3d11_xbridge *xb, uint32_t plane);

//! Stage the composite recipe the next submit stamps on its slot.
void
comp_d3d11_xbridge_stage_recipe(struct comp_d3d11_xbridge *xb, const struct comp_d3d11_xbridge_recipe *recipe);

//! The recipe stamped on @p slot. False when the slot holds nothing.
bool
comp_d3d11_xbridge_slot_recipe(struct comp_d3d11_xbridge *xb, int32_t slot, struct comp_d3d11_xbridge_recipe *out);

/*!
 * `ID3D11ShaderResourceView *` of @p plane for @p slot (NULL when invalid).
 *
 * @param want_seq The content generation the caller's recipe says this slot
 *        carries (`comp_d3d11_xbridge_recipe::plane_seq[plane]`), or 0 to skip
 *        the test. A mismatch means a later submit has rewritten the plane under
 *        this weave and returns NULL rather than mismatched pixels (#918 review
 *        F3).
 */
void *
comp_d3d11_xbridge_get_plane_srv(struct comp_d3d11_xbridge *xb, int32_t slot, uint32_t plane, uint64_t want_seq);

//! Allocated extent of @p plane's chain. False when the plane is not live.
bool
comp_d3d11_xbridge_plane_extent(struct comp_d3d11_xbridge *xb, uint32_t plane, uint32_t *out_w, uint32_t *out_h);

/*!
 * Per-plane transport counters for the once-a-second `#918 DIAG` line: bytes
 * copied and copies skipped since the last call, plus whether the plane has been
 * latched to half rate by the bandwidth gate.
 */
void
comp_d3d11_xbridge_take_plane_stats(struct comp_d3d11_xbridge *xb,
                                    uint32_t plane,
                                    uint64_t *out_bytes,
                                    uint64_t *out_copies,
                                    uint64_t *out_skips,
                                    bool *out_half_rate);

//! Atlas bytes copied since the last call — the DIAG line's denominator.
uint64_t
comp_d3d11_xbridge_take_atlas_bytes(struct comp_d3d11_xbridge *xb);

//! Short name of @p plane for the DIAG line, so adding a plane cannot leave the
//! log naming it by a stale index (#918 review D8).
const char *
comp_d3d11_xbridge_plane_label(uint32_t plane);

/*!
 * Record and execute the three legs for one app frame. Returns immediately —
 * nothing on this thread ever waits.
 *
 * @param seq Monotonic app frame counter; the one sequence every fence uses.
 * @param layout_gen The caller's layout generation — bumped whenever the
 *        effective layout (the DP's recipe) changes. Stamped on the egress slot
 *        so a later weave can tell whether the slot's pixels still belong to the
 *        recipe the DP is running (#918 R1).
 * @param atlas_texture `ID3D11Texture2D *` of the renderer atlas. Only read in
 *        Option II (the ingress staging copy); ignored in Option I.
 * @param content_w/content_h The content box actually painted this frame.
 */
void
comp_d3d11_xbridge_submit(struct comp_d3d11_xbridge *xb,
                          uint64_t seq,
                          uint64_t layout_gen,
                          void *atlas_texture,
                          uint32_t content_w,
                          uint32_t content_h);

/*!
 * Newest egress slot whose consumer copy has already completed AND whose stamped
 * layout generation is still @p want_gen, or -1 when there is none (warmup, or
 * every landed slot predates a mode switch). Opportunistic by default;
 * `DXR_WEAVE_ON_SCANOUT_DEPTH=1` forces the seq-1 slot deterministically —
 * subject to the same generation test, which is never relaxed.
 */
int32_t
comp_d3d11_xbridge_pick_slot(struct comp_d3d11_xbridge *xb, uint64_t want_gen);

/*!
 * #918 R1 — the TRANSITION pick. The newest slot carrying @p want_gen content
 * whether or not its consumer copy has landed yet; -1 when there is none, or
 * when the output device could not open the consumer fence.
 *
 * Only for the frame where @ref comp_d3d11_xbridge_pick_slot came back empty
 * because a layout change just rebuilt the ring. Presenting nothing there is not
 * neutral: with FLIP_DISCARD the panel keeps showing the last frame it WAS
 * given, which was woven for the mode the display has just left — an interlaced
 * frame under a disabled lens is the offset cube the maintainer sees on 3D->2D.
 * Weaving the in-flight slot instead costs a GPU-side wait on the output queue
 * (never a CPU one: the caller must follow with
 * @ref comp_d3d11_xbridge_gpu_wait_slot) and the frame goes out correct.
 */
int32_t
comp_d3d11_xbridge_pick_inflight_slot(struct comp_d3d11_xbridge *xb, uint64_t want_gen);

/*!
 * Queue a GPU-side wait for @p slot's completion on the output context (cached
 * at create). Free when the slot was picked opportunistically (its fence
 * already fired); the ordering that makes the forced-depth mode correct.
 */
void
comp_d3d11_xbridge_gpu_wait_slot(struct comp_d3d11_xbridge *xb, int32_t slot);

/*!
 * True when @p slot may be woven right now: either the output device took the
 * GPU-side wait above (so the ordering is on the queue), or the consumer copy
 * that filled the slot is CPU-verified complete.
 *
 * The repaint tick re-weaves a slot it did not pick itself, and without the
 * GPU-side wait nothing else orders it against a consumer copy that is
 * rewriting that same slot — so the repaint must check and bail (#918 F7).
 */
bool
comp_d3d11_xbridge_slot_ready(struct comp_d3d11_xbridge *xb, int32_t slot);

//! `ID3D11ShaderResourceView *` the DP samples for @p slot (NULL if invalid).
void *
comp_d3d11_xbridge_get_srv(struct comp_d3d11_xbridge *xb, int32_t slot);

//! Allocated extent of the egress ring (content-sized after Stage B).
void
comp_d3d11_xbridge_get_egress_dims(struct comp_d3d11_xbridge *xb, uint32_t *out_w, uint32_t *out_h);

/*!
 * Publish the slot the last app frame wove, so a repaint re-weaves exactly that
 * one with zero bridge traffic. Caller holds `c->mutex`.
 */
void
comp_d3d11_xbridge_set_weave_slot(struct comp_d3d11_xbridge *xb, int32_t slot);

//! The slot published by @ref comp_d3d11_xbridge_set_weave_slot (-1 if none).
int32_t
comp_d3d11_xbridge_get_weave_slot(struct comp_d3d11_xbridge *xb);

/*!
 * True once the watchdog has given up on the cross-adapter fence (5 s without
 * advance while frames are submitted). Submissions stop; the last good egress
 * slot keeps being woven so the panel never goes black.
 */
bool
comp_d3d11_xbridge_is_degraded(struct comp_d3d11_xbridge *xb);

/*!
 * Stop submitting, join the watchdog and drain the queues under a bounded CPU
 * wait. Call before tearing down the DP / target / devices.
 */
void
comp_d3d11_xbridge_quiesce(struct comp_d3d11_xbridge *xb);

//! Release everything, in the reverse of the create order.
void
comp_d3d11_xbridge_destroy(struct comp_d3d11_xbridge **xb_ptr);

#ifdef __cplusplus
}
#endif
