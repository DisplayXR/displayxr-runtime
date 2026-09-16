// Copyright 2024-2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 DXGI swapchain target for display output.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations (C++ structs)
struct comp_d3d11_target;
struct comp_d3d11_compositor;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a D3D11 output target (DXGI swapchain).
 *
 * The device/context/factory the target presents on are passed explicitly — the
 * target does NOT own them (no AddRef/Release), it only borrows them for the
 * lifetime of the caller-owned swapchain.
 *
 * @param c The D3D11 compositor.
 * @param hwnd The window handle to present to.
 * @param device The ID3D11Device* the swapchain and its RTV are created on.
 * @param context The ID3D11DeviceContext* used to bind/clear the back buffer.
 * @param dxgi_factory The IDXGIFactory4* the swapchain is created from.
 * @param width Preferred width.
 * @param height Preferred height.
 * @param transparent When true (and hwnd != NULL), use BitBlt swap effect so DWM
 *                    consults WS_EX_LAYERED + LWA_COLORKEY on the bound HWND.
 *                    Otherwise use flip-model + ALPHA_MODE_IGNORE (#163 default).
 * @param out_target Pointer to receive the created target.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_target_create(struct comp_d3d11_compositor *c,
                         void *hwnd,
                         void *device,
                         void *context,
                         void *dxgi_factory,
                         uint32_t width,
                         uint32_t height,
                         bool transparent,
                         struct comp_d3d11_target **out_target);

/*!
 * Destroy a D3D11 output target.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_target_destroy(struct comp_d3d11_target **target_ptr);

/*!
 * Acquire the next image for rendering.
 *
 * @param target The target.
 * @param out_index Index of the acquired image.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_target_acquire(struct comp_d3d11_target *target, uint32_t *out_index);

/*!
 * Present the rendered image.
 *
 * @param target The target.
 * @param sync_interval VSync interval (1 for VSync, 0 for immediate).
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_target_present(struct comp_d3d11_target *target, uint32_t sync_interval);

/*!
 * Late-weave pacing + weave-latency harness mark. Call immediately before the
 * DP weave on the window path: paces on the previous present's actual scanout
 * when DXR_LATE_WEAVE=1, and timestamps T_weave when DXR_WEAVE_LATENCY_CSV is
 * set. No-op otherwise. @p mode_3d feeds the DXR_FRAME_WITNESS counters (was
 * this a 3D weave or a 2D blit).
 */
void
comp_d3d11_target_weave_mark(struct comp_d3d11_target *target, uint64_t predicted_display_time_ns, bool mode_3d);

/*!
 * #868: pace a repaint to the panel. Runs WITHOUT the compositor lock, and never
 * waits on the frame-latency waitable (a semaphore — see the implementation).
 */
void
comp_d3d11_target_repaint_pace(struct comp_d3d11_target *target);

/*!
 * #1339: may a repaint present NOW without deepening the present queue?
 *
 * Takes one frame-latency token non-blockingly (timeout 0). A repaint is a
 * fill for a vblank the app missed; one that queues BEHIND a pending present
 * is not a fill, it is latency — and on an arm where the join cannot see the
 * pipeline (#1435 refused) that latency is invisible to the eye predictor.
 * Measured on the reference box (fill arm, forced repaints): the flip queue
 * ran 4-5 deep and every weave landed 3 periods after the horizon it was
 * handed, at max frame latency 1 — the cap only binds presenters that wait
 * on it, and repaints never did. Returns false = skip this tick (the token
 * stays with the app). A token taken here is kept until the repaint presents
 * (settled in @ref comp_d3d11_target_present on the repaint thread), and a
 * repaint that bails after admission, or whose present is dropped, keeps it
 * for the next tick — or, if the loop disarms for good, abandons it: the app
 * reclaims it at its next wait (one 100 ms timeout, then the chain re-syncs). Applies to DXR_WEAVE_REPAINT_FORCE=1 too: the probe now
 * fills every FREE slot rather than every refresh (DXR_WEAVE_REPAINT_QUEUE_CAP=0
 * restores the old fill-the-queue behaviour). Call it UNDER the compositor
 * lock: the app frame path waits on the same waitable while holding that lock,
 * and a token taken outside it can be the one the app is about to block on.
 * Runs on the repaint thread only.
 */
bool
comp_d3d11_target_repaint_admit(struct comp_d3d11_target *target);

/*!
 * #1339 instrumentation: the longest the APP has blocked on the frame-latency
 * waitable inside @ref comp_d3d11_target_weave_mark since the last reset.
 *
 * That wait happens on the app thread WITH the compositor lock held, so it is
 * one of the two candidate reasons an app commit can overrun a whole
 * partition stride and forfeit a grid slot. Read by the repaint loop for the
 * DXR_WEAVE_REPAINT_TRACE row; diagnostics only, never a control input.
 */
uint64_t
comp_d3d11_target_app_wait_take_max_ns(void);

/*!
 * #1339: clear the peak above (and its last-sample companion) so the trace
 * row reports a PER-WINDOW maximum. Called once per trace window.
 */
void
comp_d3d11_target_app_wait_reset(void);

/*!
 * #1482 instrumentation: the longest STAGE 2 of @ref comp_d3d11_target_weave_mark
 * since the last read — the composed chain's compositor-clock align or the
 * opaque chain's scanout wait, whichever ran.
 *
 * Stage 1 (the frame-latency wait) is the peak above; this is the other half of
 * the same span under the compositor lock, and the scanout wait is bounded at
 * three panel periods — a whole partition stride at D=3 — so it can forfeit a
 * slot on its own. Same destructive read; diagnostics only.
 */
uint64_t
comp_d3d11_target_app_stage2_take_max_ns(void);

/*!
 * @name #1482 stage-1 tallies for the trace row's `drain=` / `to=` / `inst=`.
 *
 * Destructive reads, like the peaks above: each returns the count since the
 * last call and zeroes it.
 *
 *  - drained: frame-latency tokens the #868 surplus drain removed. Always 0
 *    while @ref comp_d3d11_target_set_app_paced is true, because the drain does
 *    not run there.
 *  - wait_timeouts: stage-1 waits that ran to the full 100 ms bound — the #1482
 *    signature, where the drain deleted the very token the app then waited for.
 *  - wait_instant: stage-1 waits that returned inside 2 ms on an already-banked
 *    token — the #868 signature the drain exists to suppress.
 * @{
 */
uint32_t
comp_d3d11_target_app_take_drained(void);

uint32_t
comp_d3d11_target_app_take_wait_timeouts(void);

uint32_t
comp_d3d11_target_app_take_wait_instant(void);
/*! @} */

/*!
 * #868: repaint counterpart of @ref comp_d3d11_target_weave_mark — stamps
 * T_weave only, staying out of the saturation governor and the #867 ledger.
 */
void
comp_d3d11_target_weave_mark_repaint(struct comp_d3d11_target *target, bool mode_3d);

/*!
 * Note that xrWaitFrame just returned, so the span to this frame's weave can
 * be measured (#867).
 */
void
comp_d3d11_target_mark_wait_frame(struct comp_d3d11_target *target);

/*!
 * App-visible wait_frame->scanout lookahead in ns from measured frame cost +
 * measured weave->scanout residual (#867). 0 when unmeasured, in which case
 * the caller keeps its own estimate.
 */
uint64_t
comp_d3d11_target_get_predicted_lookahead_ns(struct comp_d3d11_target *target);

/*!
 * Seed the late-weave governor's panel period from the compositor's queried
 * refresh rate, so saturation is judged correctly before DXGI frame
 * statistics have produced their first period sample. No-op once measured.
 */
void
comp_d3d11_target_set_display_period(struct comp_d3d11_target *target, uint64_t period_ns);

/*!
 * #1482: tell the target whether a #1257 partition grid is pacing the app's
 * frame releases. Pushed by the compositor at its `u_app_partition_throttle`
 * call and keyed on that compositor's OWN grid (`next_release_ns != 0`), which
 * is how every other partition decision on this path is keyed.
 *
 * While true, @ref comp_d3d11_target_weave_mark skips the #868 surplus-token
 * drain: the grid is the pacer, so an instant stage-1 return is exactly what
 * the app's slot wants — and the drain, which cannot tell the app's own credit
 * from a repaint's surplus, would otherwise delete the token the app is about
 * to block on while it holds the compositor lock.
 */
void
comp_d3d11_target_set_app_paced(struct comp_d3d11_target *target, bool paced);

/*!
 * The display refresh period this target is pacing against, or 0 if it has not
 * been established yet (the DP contract's "unknown").
 */
uint64_t
comp_d3d11_target_get_display_period_ns(struct comp_d3d11_target *target);

/*!
 * Measured weave→scanout residual of the last completed frame in ns (0 =
 * unknown), from DXGI frame statistics. Feeds the DP's set_frame_timing
 * control loop.
 */
uint64_t
comp_d3d11_target_get_measured_weave_ns(struct comp_d3d11_target *target);

/*!
 * #206: FORWARD-computed weave→scanout for a weave recorded now, from the
 * vsync-locked vblank grid (0 = no trusted grid — DP falls back to the
 * retrospective heuristic). Feeds the DP's set_predicted_scanout slot.
 */
uint64_t
comp_d3d11_target_predict_weave_to_scanout_ns(struct comp_d3d11_target *target);

/*!
 * Get target dimensions.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_target_get_dimensions(struct comp_d3d11_target *target,
                                 uint32_t *out_width,
                                 uint32_t *out_height);

/*!
 * Re-bind the target's render target view and viewport (without clearing).
 *
 * Call this after operations that change the bound RTV (e.g. renderer draw)
 * and before the display processor writes to the target.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_target_bind(struct comp_d3d11_target *target);

/*!
 * Get the back buffer texture (ID3D11Texture2D*) for direct pixel copy.
 *
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_target_get_back_buffer(struct comp_d3d11_target *target);

/*!
 * Resize the target swapchain.
 *
 * @param target The target.
 * @param width New width.
 * @param height New height.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_target_resize(struct comp_d3d11_target *target,
                         uint32_t width,
                         uint32_t height);

#ifdef __cplusplus
}
#endif
