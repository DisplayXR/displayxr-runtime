// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Generic service-side input-event queue (macOS shell Tier 1, #48 / #61).
 *
 * The Windows D3D11 service compositor owns its output window and enqueues
 * keyboard/mouse events from its WndProc, routing them to either the focused
 * app's HWND (native OS focus) or the workspace controller. The macOS
 * null+comp_multi service has no such monolith: the service main thread owns the
 * NSWindow and pumps NSEvents (ipc_server_macos_appkit.m), so this is the
 * vendor-neutral analogue — a set of small thread-safe rings of @ref
 * ipc_workspace_input_event keyed by an opaque *target*.
 *
 * #61 input routing: the AppKit pump hit-tests each event against the placed
 * window rects (comp_multi_workspace) and pushes it to a per-target queue — the
 * controller's queue (@ref IPC_INPUT_TARGET_CONTROLLER, a NULL key) for
 * workspace/chrome events, or a content client's own queue (keyed by its
 * per-session `xrt_compositor *` == `ics->xc`) for events over its content.
 * Each client drains only its own queue
 * (ipc_handle_workspace_enumerate_input_events), so a content app sees the input
 * forwarded to it while the controller keeps the workspace gestures — mirroring
 * the Windows split without OS-level window focus.
 *
 * @ingroup ipc_server
 */

#pragma once

#include "shared/ipc_protocol.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Target key for workspace/controller-bound events (NULL). The workspace
 * controller drains this queue; content clients drain queues keyed by their own
 * compositor pointer.
 */
#define IPC_INPUT_TARGET_CONTROLLER NULL

/*!
 * Push one input event onto @p target's queue. @p target is either
 * @ref IPC_INPUT_TARGET_CONTROLLER or a content client's per-session
 * `xrt_compositor *`. Thread-safe; called from the service main thread (AppKit
 * router). Drops the oldest event if that target's ring is full — input is
 * latency-sensitive, a stale event is worse than a dropped one.
 */
void
ipc_server_input_queue_push(void *target, const struct ipc_workspace_input_event *event);

/*!
 * Drain up to @p capacity queued events for @p target into @p out_batch (FIFO).
 * Thread-safe; called from a per-client IPC handler thread. @p capacity is
 * clamped to IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX. Always sets out_batch->count
 * (0 if @p target has no queue / no events).
 */
void
ipc_server_input_queue_drain(void *target, uint32_t capacity, struct ipc_workspace_input_event_batch *out_batch);

/*!
 * Drop @p target's queue (client disconnect). The compositor pointer is reused
 * across sessions, so its queue must be cleared on destroy or a new client could
 * inherit stale events. No-op if @p target has no queue.
 */
void
ipc_server_input_queue_drop(void *target);

/*!
 * Input-grab state (workspace launcher band open, via xrSetWorkspaceInputGrabEXT).
 * While grabbed, the router sends every event to the controller queue only — no
 * content forwarding — so the band owns all input. The macOS analogue of the
 * Windows SetForegroundWindow grab.
 */
void
ipc_server_input_queue_set_input_grab(bool grabbed);
bool
ipc_server_input_queue_input_grabbed(void);

/*!
 * Pointer-capture state (a controller drag/resize/rotate gesture is active, via
 * xrEnableWorkspacePointerCaptureEXT). While captured, the router sends all
 * pointer/scroll/motion to the controller only, so a gesture started on chrome
 * keeps receiving motion even when the cursor passes over content.
 */
void
ipc_server_input_queue_set_pointer_capture(bool captured);
bool
ipc_server_input_queue_pointer_captured(void);

#ifdef __cplusplus
}
#endif
