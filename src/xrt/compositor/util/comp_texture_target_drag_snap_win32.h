// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Texture target drag phase-snap (ADR-060, #2725): while a texture
 *         target is bound to a WINDOWLESS display processor, user drags of the
 *         app's reference window are snapped through the DP's
 *         `snap_window_rect` slot, evaluated at the target's present origin.
 *
 * Why: a windowless DP is phased for the target's on-screen origin when the
 * frame is woven, and the APP presents that frame later. During a drag the
 * window has moved by then, so every in-flight frame lands off-phase and the
 * 3D breaks until the window stops. Snapping each drag step to a position with
 * the phase the window had when the drag started keeps every already-woven
 * frame correctly phased wherever it lands — the same treatment runtime-owned
 * windows get (#625 / #2405), applied to a window the APP owns.
 *
 * Mechanism. The reference window's top-level (GA_ROOT, the window that runs
 * the modal move loop) is subclassed with comctl32 `SetWindowSubclass`, which
 * has to run on the window's own thread: directly when the caller is that
 * thread, else through a one-shot thread-local `WH_GETMESSAGE` hook kicked by
 * a posted `WM_NULL` — never a blocking cross-thread call on the frame path.
 * The subclass:
 *  - WM_ENTERSIZEMOVE: records the frame top-left and the reference client
 *    origin's offset from it (the window is only snapped when it is per-monitor
 *    DPI aware, so its message coordinates are the physical px the DP works in);
 *  - WM_WINDOWPOSCHANGING (after the app's own handling, so the app's proposal
 *    is what gets snapped): for a pure MOVE inside that loop, asks the DP for
 *    the snap of the target's present origin (frame + client offset + target
 *    offset) and applies the resulting delta to the window position.
 * Resizes are never snapped. Nothing is snapped while the compositor reports
 * the target inactive (off the 3D screen, filled flat, segmented, or not woven
 * through the windowless DP's present origin at all).
 *
 * Lifetime / locking. Each reference window gets a refcounted binding: one
 * reference for the session, one for the window side (pending hook or live
 * subclass). The snap provider and the per-weave state live under the
 * binding's SRW lock: the window thread reads them shared for the duration of
 * a snap; detaching takes it exclusive, so once a detach returns no DP call is
 * in flight and none will start — detach BEFORE destroying the DP. The
 * subclass removes itself on WM_NCDESTROY (the app destroyed its window
 * first), on a detached binding's next message, or synchronously at destroy.
 * If the window thread cannot be reached at destroy (a short
 * SendMessageTimeout), the binding stays inert (detached) and this module is
 * pinned so the window can never call into unloaded code.
 *
 * Only for a DP that HAS the slot: the caller arms this only when the slot is
 * present, and a DP that declines leaves the window exactly where the user
 * puts it — today's behaviour.
 *
 * @ingroup comp_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The DP's snap: absolute desktop device px, drag-start top-left and proposed
 * top-left in, phase-snapped top-left out. Same contract as every
 * `snap_window_rect` slot: false ⟹ no snap, the caller keeps the target.
 * Called on the reference window's thread inside its move loop, and once
 * (the prewarm) on the thread that calls @ref comp_tt_drag_snap_create.
 */
typedef bool (*comp_tt_drag_snap_fn)(void *userdata,
                                     int32_t origin_x,
                                     int32_t origin_y,
                                     int32_t target_x,
                                     int32_t target_y,
                                     int32_t *out_x,
                                     int32_t *out_y);

struct comp_tt_drag_snap;

/*!
 * Arm the drag snap for a texture target whose primary DP is windowless and
 * has a `snap_window_rect` slot. Prewarms one snap on the calling thread at
 * the target's current present origin and logs the result once. Starts
 * inactive until the first @ref comp_tt_drag_snap_update says otherwise.
 *
 * @param ref_window The reference window (HWND) the target rect is in; NULL
 *                   (a desktop-px target) ⟹ nothing is armed until an update
 *                   names a window.
 * @param target_x   Target origin in the reference window's client area.
 * @param target_y   ditto.
 * @param fn         The DP snap.
 * @param userdata   Handed back to @p fn.
 * @param tier       Short tier name for log lines ("d3d12", "vk", ...).
 * @return the object, or NULL on allocation failure (the window moves freely).
 */
struct comp_tt_drag_snap *
comp_tt_drag_snap_create(
    void *ref_window, int32_t target_x, int32_t target_y, comp_tt_drag_snap_fn fn, void *userdata, const char *tier);

/*!
 * Per weave: the target the frame was woven for, and whether it was woven
 * through the windowless DP's present origin (true) or not (false: filled
 * flat, segmented, off the 3D screen — no snap). A change of reference window
 * (dock / undock) moves the subclass to the new window. Cheap: one brief lock
 * when nothing changed. NULL @p s is a no-op.
 */
void
comp_tt_drag_snap_update(
    struct comp_tt_drag_snap *s, void *ref_window, int32_t target_x, int32_t target_y, bool active);

/*!
 * Detach every binding (no DP call in flight or to come once this returns),
 * remove the subclasses where the window thread can be reached, and free.
 * Call before the DP is destroyed. NULL / *NULL is a no-op.
 */
void
comp_tt_drag_snap_destroy(struct comp_tt_drag_snap **s_ptr);

#ifdef __cplusplus
}
#endif
