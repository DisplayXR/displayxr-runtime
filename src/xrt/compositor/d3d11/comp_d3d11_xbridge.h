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
 */
bool
comp_d3d11_xbridge_set_content_size(struct comp_d3d11_xbridge *xb, uint32_t w, uint32_t h);

/*!
 * True when the egress ring is content-sized (Stage B succeeded), i.e. the
 * caller may skip its own crop-before-DP pass.
 */
bool
comp_d3d11_xbridge_egress_is_content_sized(struct comp_d3d11_xbridge *xb);

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
 * Record and execute the three legs for one app frame. Returns immediately —
 * nothing on this thread ever waits.
 *
 * @param seq Monotonic app frame counter; the one sequence every fence uses.
 * @param atlas_texture `ID3D11Texture2D *` of the renderer atlas. Only read in
 *        Option II (the ingress staging copy); ignored in Option I.
 * @param content_w/content_h The content box actually painted this frame.
 */
void
comp_d3d11_xbridge_submit(
    struct comp_d3d11_xbridge *xb, uint64_t seq, void *atlas_texture, uint32_t content_w, uint32_t content_h);

/*!
 * Newest egress slot whose consumer copy has already completed, or -1 when
 * nothing has landed yet (warmup). Opportunistic by default;
 * `DXR_WEAVE_ON_SCANOUT_DEPTH=1` forces the seq-1 slot deterministically.
 */
int32_t
comp_d3d11_xbridge_pick_slot(struct comp_d3d11_xbridge *xb);

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
