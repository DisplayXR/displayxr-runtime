// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: conv_host_wire.h framing over P2-1's
 *         os_channel (os/os_process.h), used by the host for its end and by
 *         tests for the service's end.
 *
 * A frame is a conv_host_wire_frame then exactly payload_bytes, sent with
 * ONE os_channel_send. Handles:
 *  - Windows: frame.handles[] carry values valid in the RECEIVER's process
 *    (the service duplicates into the host and pulls out of it with
 *    os_process_dup_*); nothing but bytes rides the pipe.
 *  - POSIX: the descriptors ride the frame's first byte as SCM_RIGHTS;
 *    frame.handles[] are 0 on the wire and filled locally on receive.
 *
 * `--channel` forms accepted by @ref conv_host_chan_open: Windows the pipe
 * path `\\.\pipe\<name>` (what os_channel_create_for_child hands out) or the
 * bare `<name>`; POSIX the inherited descriptor number (OS_CHANNEL_CHILD_FD).
 *
 * os_channel_send is not safe against a concurrent send, so sends take a
 * lock; a receive is safe against a send. Receives poll in short slices so
 * @ref conv_host_chan_shutdown can stop a blocked receiver.
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

struct os_channel;
struct conv_host_chan;

//! Child side: open the channel named by @p token. NULL on failure.
struct conv_host_chan *
conv_host_chan_open(const char *token, char *out_reason, uint32_t reason_size);

//! Wrap an already-open os_channel (takes ownership), e.g. the service's end.
struct conv_host_chan *
conv_host_chan_wrap(struct os_channel *ch);

/*!
 * Blocking receive of one frame. @p payload must hold
 * CONV_HOST_WIRE_MAX_PAYLOAD bytes; @p f->handles[0..handle_count) are local
 * values on return (POSIX: received descriptors, owned by the caller).
 * false = the peer closed, an I/O error, a malformed frame, or shutdown.
 */
bool
conv_host_chan_recv(struct conv_host_chan *c, struct conv_host_wire_frame *f, void *payload);

/*!
 * Send one frame with @p handle_count handles (Windows: values for the
 * receiver; POSIX: this process's fds, borrowed). Thread-safe.
 */
bool
conv_host_chan_send(struct conv_host_chan *c,
                    uint32_t type,
                    uint32_t request_id,
                    const void *payload,
                    uint32_t payload_bytes,
                    const uint64_t *handles,
                    uint32_t handle_count);

//! Make a blocked and every later receive / send fail. Any thread.
void
conv_host_chan_shutdown(struct conv_host_chan *c);

void
conv_host_chan_destroy(struct conv_host_chan **c_ptr);

#ifdef __cplusplus
}
#endif
