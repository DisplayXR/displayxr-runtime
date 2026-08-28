// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 DXGI swapchain target for display output.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations (C++ structs)
struct comp_d3d12_target;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a D3D12 output target (DXGI swapchain).
 *
 * The device, the queue the swapchain is created against and the DXGI factory
 * are passed explicitly — the target does NOT own them (no AddRef/Release), it
 * only borrows them for the lifetime of the caller-owned swapchain. This is the
 * D3D12 twin of @ref comp_d3d11_target_create's post-#918 shape, and the reason
 * for it is the same: under the output-device split the presentation target
 * belongs on the SCANOUT adapter, so the target must be creatable from a
 * device/queue/factory triple the caller chooses, with no route back into the
 * app-device compositor. There is deliberately no compositor parameter.
 *
 * @param hwnd The window handle to present to.
 * @param device The ID3D12Device* the RTV heap and the back-buffer RTVs are
 *               created on.
 * @param command_queue The ID3D12CommandQueue* the swapchain is created against
 *                      (DXGI flushes and presents through it).
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
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_target_create(void *hwnd,
                         void *device,
                         void *command_queue,
                         void *dxgi_factory,
                         uint32_t width,
                         uint32_t height,
                         bool transparent,
                         struct comp_d3d12_target **out_target);

/*!
 * Destroy a D3D12 output target.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_target_destroy(struct comp_d3d12_target **target_ptr);

/*!
 * Present the rendered image.
 *
 * @param target The target.
 * @param sync_interval VSync interval (1 for VSync, 0 for immediate).
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_target_present(struct comp_d3d12_target *target, uint32_t sync_interval);

/*!
 * Late-weave pacing + weave-latency harness mark. Call immediately before
 * recording the DP weave: waits on the frame-latency waitable when
 * DXR_LATE_WEAVE=1 (vsync-locking weave+present) and timestamps T_weave when
 * DXR_WEAVE_LATENCY_CSV is set. No-op otherwise.
 */
void
comp_d3d12_target_weave_mark(struct comp_d3d12_target *target, uint64_t predicted_display_time_ns, bool mode_3d);

/*!
 * #868: pace a repaint to the panel — the wait half of
 * @ref comp_d3d12_target_weave_mark, split out so it can run WITHOUT the
 * compositor lock held.
 *
 * This blocks for up to ~3 panel periods. Doing it under the lock would let a
 * repaint stall an arriving app frame for that long; doing it here means the
 * lock only covers the record+submit+present, and it is also what limits the
 * repaint loop to panel rate.
 */
void
comp_d3d12_target_repaint_pace(struct comp_d3d12_target *target);

/*!
 * #868: the repaint counterpart of @ref comp_d3d12_target_weave_mark — stamps
 * T_weave and nothing else.
 *
 * A repaint carries no app frame, so it is kept out of the saturation
 * governor's frame-interval EMA and out of the #867 prediction ledger. Call
 * this (never the plain weave_mark) when re-weaving an unchanged atlas; pacing
 * is @ref comp_d3d12_target_repaint_pace, called earlier and unlocked.
 */
void
comp_d3d12_target_weave_mark_repaint(struct comp_d3d12_target *target, bool mode_3d);

/*!
 * Note that xrWaitFrame just returned, so the span to this frame's weave can
 * be measured (#867).
 */
void
comp_d3d12_target_mark_wait_frame(struct comp_d3d12_target *target);

/*!
 * App-visible wait_frame->scanout lookahead in ns from measured frame cost +
 * measured weave->scanout residual (#867). 0 when unmeasured, in which case
 * the caller keeps its own estimate.
 */
uint64_t
comp_d3d12_target_get_predicted_lookahead_ns(struct comp_d3d12_target *target);

/*!
 * Seed the late-weave governor's panel period from the compositor's queried
 * refresh rate, so saturation is judged correctly before DXGI frame
 * statistics have produced their first period sample. No-op once measured.
 */
void
comp_d3d12_target_set_display_period(struct comp_d3d12_target *target, uint64_t period_ns);

/*!
 * Measured weave→scanout residual of the last completed frame in ns (0 =
 * unknown), from DXGI frame statistics. Feeds the DP's set_frame_timing
 * control loop.
 */
uint64_t
comp_d3d12_target_get_measured_weave_ns(struct comp_d3d12_target *target);

/*!
 * #206: FORWARD-computed weave→scanout for a weave recorded now, from the
 * vsync-locked vblank grid (0 = no trusted grid — DP falls back to the
 * retrospective heuristic). Feeds the DP's set_predicted_scanout slot.
 */
uint64_t
comp_d3d12_target_predict_weave_to_scanout_ns(struct comp_d3d12_target *target);

/*!
 * Get target dimensions.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_target_get_dimensions(struct comp_d3d12_target *target,
                                 uint32_t *out_width,
                                 uint32_t *out_height);

/*!
 * Get the current back buffer index.
 *
 * @ingroup comp_d3d12
 */
uint32_t
comp_d3d12_target_get_current_index(struct comp_d3d12_target *target);

/*!
 * Get the back buffer resource at the given index.
 *
 * @ingroup comp_d3d12
 */
void *
comp_d3d12_target_get_back_buffer(struct comp_d3d12_target *target, uint32_t index);

/*!
 * Get the RTV CPU descriptor handle for the given back buffer index.
 *
 * @ingroup comp_d3d12
 */
uint64_t
comp_d3d12_target_get_rtv_handle(struct comp_d3d12_target *target, uint32_t index);

/*!
 * Resize the target swapchain.
 *
 * @param target The target.
 * @param width New width.
 * @param height New height.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_target_resize(struct comp_d3d12_target *target,
                         uint32_t width,
                         uint32_t height);

/*!
 * Check whether the target created a child window fallback.
 *
 * Returns true if the target had to create a WS_CHILD window because the
 * app's HWND already had a DXGI swapchain (E_ACCESSDENIED fallback).
 *
 * @ingroup comp_d3d12
 */
bool
comp_d3d12_target_has_child_window(struct comp_d3d12_target *target);

/*!
 * Resize the child window to match the parent's client area.
 *
 * No-op if the target does not have a child window fallback.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_target_resize_child_window(struct comp_d3d12_target *target,
                                      uint32_t width,
                                      uint32_t height);

#ifdef __cplusplus
}
#endif
