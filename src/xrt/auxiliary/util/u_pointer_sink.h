// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Process-wide pointer-event queue behind
 *         @ref xrt_input_plugin_host_iface::poll_pointer_events.
 *
 * The runtime owns the surfaces it renders into, so it is the only side that
 * ever sees their pointer input: the hosted Android `SurfaceView`
 * (`MonadoView.onTouchEvent` → `android_custom_surface.cpp`), an app-owned
 * surface fed through `xrSubmitAndroidInputEventDXR`
 * (`oxr_android_surface.c`), and — later — a workspace controller forwarding
 * what it hit-tests (#1996). Every one of those PUSHES here; the input-provider
 * loader hands providers a POLL on it through the host iface, and the touch
 * provider turns the stream into a navigation device + an emulated controller
 * on its own side of the ABI (ADR-034 Amendment 4: no gesture policy enters
 * the runtime).
 *
 * A fixed ring under one mutex: pushes come from a UI thread at touch rate,
 * polls from `xrSyncActions` at frame rate, and neither may block the other
 * for long or allocate. Overflow drops the OLDEST event — a stale MOVE is
 * worth less than the UP that follows it, and a provider that missed a MOVE
 * simply sees a larger delta on the next one.
 *
 * Platform-free on purpose (compiled into the host unit test): the sources
 * are platform-specific, the queue is not.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_input_plugin.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Ring capacity. ~a quarter second of 4-finger MOVEs at 120 Hz between two polls.
#define U_POINTER_SINK_CAPACITY 128

/*!
 * Append one event. Copies @p ev (which must have `struct_size` set); when the
 * ring is full the oldest event is dropped and the drop is counted. Never
 * blocks beyond the mutex, never allocates, safe from any thread.
 *
 * @ingroup aux_util
 */
void
u_pointer_sink_push(const struct xrt_input_pointer_event *ev);

/*!
 * Drain up to @p max events, oldest first, into @p out. Returns the count.
 * This is exactly the @ref xrt_input_plugin_host_iface::poll_pointer_events
 * slot; the loader points the slot straight at it.
 *
 * @ingroup aux_util
 */
uint32_t
u_pointer_sink_poll(struct xrt_input_pointer_event *out, uint32_t max);

/*!
 * Diagnostics: events pushed / dropped-on-overflow since process start. Either
 * out-pointer may be NULL.
 *
 * @ingroup aux_util
 */
void
u_pointer_sink_get_stats(uint64_t *out_pushed, uint64_t *out_dropped);

/*!
 * Empty the ring and zero the counters. Tests only — a session never clears
 * what another thread may be mid-way through publishing.
 *
 * @ingroup aux_util
 */
void
u_pointer_sink_reset(void);

#ifdef __cplusplus
}
#endif
