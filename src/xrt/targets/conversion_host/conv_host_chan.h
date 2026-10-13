// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: the host end of the control channel and
 *         the OS primitives the data lanes need (wake signals, shm, handles).
 *
 * Framing follows conv_host_wire.h: a conv_host_wire_frame, then exactly
 * payload_bytes. Handles:
 *  - Windows: frame.handles[] are values already valid in THIS process (the
 *    service duplicated them in); the host sends its own values and the
 *    service pulls them. Nothing rides the pipe but bytes.
 *  - POSIX: the descriptors ride the frame as SCM_RIGHTS ancillary data, in
 *    order; frame.handles[] are 0 on the wire and filled locally on receive.
 *
 * `--channel` is either the inherited channel token (a decimal handle value /
 * file descriptor, os_channel_child_token) or a NAME: Windows connects to
 * `\\.\pipe\<name>`, POSIX to the AF_UNIX socket at `<name>` (an absolute
 * path) or, on Linux, the abstract socket `@<name>`.
 *
 * This is the host's private transport. When P2-1's os_process.h channel
 * layer lands with the same framing, these entry points become thin wrappers.
 *
 * @ingroup conv_host
 */

#pragma once

#include "conversion/conv_host_wire.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct conv_host_chan;

/*!
 * Open the channel named by @p token (see above). @p out_reason (optional)
 * says why not. NULL on failure.
 */
struct conv_host_chan *
conv_host_chan_open(const char *token, char *out_reason, uint32_t reason_size);

/*!
 * Blocking receive of one frame. @p payload must hold
 * CONV_HOST_WIRE_MAX_PAYLOAD bytes; @p f->handles[0..handle_count) are local
 * values on return (POSIX: received descriptors; any surplus is closed).
 * false = the peer closed, an I/O error, or a malformed frame.
 */
bool
conv_host_chan_recv(struct conv_host_chan *c, struct conv_host_wire_frame *f, void *payload);

/*!
 * Send one frame: @p type, @p request_id, @p payload of @p payload_bytes and
 * @p handle_count local handles (Windows: their values go in frame.handles;
 * POSIX: SCM_RIGHTS). Thread-safe (one lock per channel). false = broken.
 */
bool
conv_host_chan_send(struct conv_host_chan *c,
                    uint32_t type,
                    uint32_t request_id,
                    const void *payload,
                    uint32_t payload_bytes,
                    const uint64_t *handles,
                    uint32_t handle_count);

/*!
 * Unblock a receiver and make every later call fail (the channel stays
 * allocated until @ref conv_host_chan_destroy). Safe from any thread.
 */
void
conv_host_chan_shutdown(struct conv_host_chan *c);

void
conv_host_chan_destroy(struct conv_host_chan **c_ptr);


/*
 *
 * OS primitives for the data lanes. A "handle" is a HANDLE value on Windows
 * and a file descriptor on POSIX, widened to u64.
 *
 */

//! Close a received / created handle. 0 and invalid values are ignored.
void
conv_host_handle_close(uint64_t h);

/*!
 * Wait for a wake signal (Windows: an event; POSIX: an eventfd or pipe /
 * socket read end) for up to @p timeout_ms. Consumes the signal. true =
 * signalled, false = timed out or error.
 */
bool
conv_host_wake_wait(uint64_t h, uint32_t timeout_ms);

//! Raise a wake signal (Windows: SetEvent; POSIX: write an 8-byte 1).
void
conv_host_wake_signal(uint64_t h);

/*!
 * Map @p bytes of the shm section @p h (read + write). NULL on failure.
 * Windows: a file-mapping handle; POSIX: an fd (mmap MAP_SHARED).
 */
void *
conv_host_shm_map(uint64_t h, uint64_t bytes);

void
conv_host_shm_unmap(void *p, uint64_t bytes);

/*!
 * Create a shm section of @p bytes, mapped at *@p out_map. Returns its handle
 * (0 on failure). The host sends it to the service, which pulls / receives it.
 */
uint64_t
conv_host_shm_create(uint64_t bytes, void **out_map);

#ifdef __cplusplus
}
#endif
