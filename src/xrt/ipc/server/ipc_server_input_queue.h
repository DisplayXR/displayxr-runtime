// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Generic service-side input-event queue (macOS shell Tier 1, #48).
 *
 * The Windows D3D11 service compositor owns its output window and enqueues
 * keyboard/mouse events from its WndProc, drained over IPC by the workspace
 * controller (comp_d3d11_service_workspace_drain_input_events). The macOS
 * null+comp_multi service has no such monolith: the service main thread owns the
 * NSWindow and pumps NSEvents (ipc_server_macos_appkit.m), so this is the
 * vendor-neutral analogue — a small thread-safe ring of @ref
 * ipc_workspace_input_event that the AppKit pump pushes into and the IPC handler
 * (ipc_handle_workspace_enumerate_input_events) drains. One process-global queue;
 * the service is a single process.
 *
 * @ingroup ipc_server
 */

#pragma once

#include "shared/ipc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Push one input event onto the global queue. Thread-safe; called from the
 * service main thread (AppKit pump). Drops the oldest event if the ring is full
 * — input is latency-sensitive, a stale event is worse than a dropped one.
 */
void
ipc_server_input_queue_push(const struct ipc_workspace_input_event *event);

/*!
 * Drain up to @p capacity queued events into @p out_batch (FIFO). Thread-safe;
 * called from a per-client IPC handler thread. @p capacity is clamped to
 * IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX. Always sets out_batch->count.
 */
void
ipc_server_input_queue_drain(uint32_t capacity, struct ipc_workspace_input_event_batch *out_batch);

#ifdef __cplusplus
}
#endif
