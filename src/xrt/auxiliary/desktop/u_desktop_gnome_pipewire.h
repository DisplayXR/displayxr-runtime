// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME screen capture: one frame from each portal ScreenCast stream,
 *         through libpipewire-0.3 (dlopen'd, no DT_NEEDED — like libei) and
 *         pw_stream directly (no GStreamer). XR_DXR_desktop_control spec §10.5.
 */

#pragma once

#include "u_desktop.h"
#include "u_desktop_capture.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One monitor stream of the portal session: its PipeWire node and where it is.
struct gnome_pw_stream
{
	uint32_t node;
	struct u_desktop_rect rect; //!< stage px (the Start response's position / size)
};

//! One grabbed frame (malloc'd pixels, caller frees with gnome_pw_frame_free).
struct gnome_pw_frame
{
	uint8_t *data;
	uint32_t w, h, stride;
	enum u_desktop_pixfmt fmt;
};

//! True when libpipewire-0.3.so.0 loads with every symbol used here.
bool
gnome_pw_available(void);

/*!
 * One frame from each of @p n streams, connected through a dup of @p pw_fd (the
 * portal's OpenPipeWireRemote fd; it stays the caller's). A new pw_stream per
 * stream, the first buffer awaited (mutter sends one when a stream starts, idle
 * screen or not), all torn down before returning — no stream outlives the call.
 * Bounded by @p timeout_ms in total (TIMEOUT). SHM buffers only. The frames never
 * go anywhere but @p out.
 */
enum u_desktop_result
gnome_pw_grab(int pw_fd,
              const struct gnome_pw_stream *streams,
              uint32_t n,
              uint32_t timeout_ms,
              struct gnome_pw_frame *out,
              char *msg);

void
gnome_pw_frame_free(struct gnome_pw_frame *f);

#ifdef __cplusplus
}
#endif
