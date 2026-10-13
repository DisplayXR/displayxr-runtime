// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: conv_host_wire.h framing over os_channel.
 * @ingroup conv_host
 */

#include "conv_host_chan.h"

#include "xrt/xrt_config_os.h"
#include "os/os_process.h"
#include "os/os_threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! Receive slice: how often a blocked receiver looks at the shutdown flag.
#define CHAN_POLL_MS 200u
//! A frame that started must finish within this long (else the stream is desynced).
#define CHAN_FRAME_MS 5000u

struct conv_host_chan
{
	struct os_channel *ch;
	struct os_mutex send_mtx;
	volatile bool dead;
};

struct conv_host_chan *
conv_host_chan_wrap(struct os_channel *ch)
{
	if (ch == NULL) {
		return NULL;
	}
	struct conv_host_chan *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		os_channel_destroy(&ch);
		return NULL;
	}
	c->ch = ch;
	(void)os_mutex_init(&c->send_mtx);
	return c;
}

struct conv_host_chan *
conv_host_chan_open(const char *token, char *out_reason, uint32_t reason_size)
{
	if (token == NULL || token[0] == '\0') {
		return NULL;
	}
	char full[300];
#ifdef XRT_OS_WINDOWS
	// os_channel_create_for_child hands out the full pipe path; accept the
	// bare name too.
	if (strncmp(token, "\\\\.\\pipe\\", 9) != 0) {
		(void)snprintf(full, sizeof(full), "\\\\.\\pipe\\%s", token);
		token = full;
	}
#else
	(void)full;
#endif
	struct os_channel *ch = NULL;
	const int r = os_channel_open_child(token, &ch);
	if (r != 0) {
		if (out_reason != NULL && reason_size > 0) {
			(void)snprintf(out_reason, reason_size, "os_channel_open_child failed (%d)", r);
		}
		return NULL;
	}
	return conv_host_chan_wrap(ch);
}

//! 1 = got it all, 0 = nothing yet (only when @p allow_idle), < 0 = broken.
static int
recv_exact(struct conv_host_chan *c, void *buf, size_t len, uint64_t *handles, uint32_t *inout_count, bool allow_idle)
{
	for (;;) {
		if (c->dead) {
			return -1;
		}
		uint32_t cap = inout_count != NULL ? *inout_count : 0;
		const int r = os_channel_recv(c->ch, buf, len, handles, inout_count != NULL ? &cap : NULL,
		                              allow_idle ? CHAN_POLL_MS : CHAN_FRAME_MS);
		if (inout_count != NULL) {
			*inout_count = cap;
		}
		if (r == 1) {
			return 1;
		}
		if (r == 0 && allow_idle) {
			continue; // nothing arrived: look at the shutdown flag again
		}
		return -1; // closed, failed, or a started message that never finished
	}
}

bool
conv_host_chan_recv(struct conv_host_chan *c, struct conv_host_wire_frame *f, void *payload)
{
	if (c == NULL) {
		return false;
	}
	uint64_t fds[CONV_HOST_WIRE_MAX_HANDLES] = {0};
	uint32_t nfd = CONV_HOST_WIRE_MAX_HANDLES;
	if (recv_exact(c, f, sizeof(*f), fds, &nfd, true) != 1) {
		return false;
	}
	bool ok = conv_host_wire_frame_valid(f);
	if (ok && f->payload_bytes > 0) {
		ok = recv_exact(c, payload, f->payload_bytes, NULL, NULL, false) == 1;
	}
#ifdef XRT_OS_WINDOWS
	(void)fds;
	(void)nfd;
	// Values are already valid in this process (duplicated in by the sender).
#else
	if (ok && nfd != f->handle_count) {
		ok = false;
	}
	for (uint32_t i = 0; i < CONV_HOST_WIRE_MAX_HANDLES; i++) {
		f->handles[i] = ok && i < nfd ? fds[i] : 0;
	}
	if (!ok) {
		for (uint32_t i = 0; i < nfd; i++) {
			os_native_handle_close(fds[i]);
		}
	}
#endif
	return ok;
}

bool
conv_host_chan_send(struct conv_host_chan *c,
                    uint32_t type,
                    uint32_t request_id,
                    const void *payload,
                    uint32_t payload_bytes,
                    const uint64_t *handles,
                    uint32_t handle_count)
{
	if (c == NULL || c->dead || payload_bytes > CONV_HOST_WIRE_MAX_PAYLOAD ||
	    handle_count > CONV_HOST_WIRE_MAX_HANDLES) {
		return false;
	}
	uint8_t buf[sizeof(struct conv_host_wire_frame) + CONV_HOST_WIRE_MAX_PAYLOAD];
	struct conv_host_wire_frame f;
	memset(&f, 0, sizeof(f));
	f.magic = CONV_HOST_WIRE_MAGIC;
	f.type = type;
	f.payload_bytes = payload_bytes;
	f.request_id = request_id;
	f.handle_count = handle_count;
#ifdef XRT_OS_WINDOWS
	for (uint32_t i = 0; i < handle_count; i++) {
		f.handles[i] = handles[i];
	}
	const uint64_t *os_handles = NULL;
	const uint32_t os_count = 0;
#else
	const uint64_t *os_handles = handles;
	const uint32_t os_count = handle_count;
#endif
	memcpy(buf, &f, sizeof(f));
	if (payload_bytes > 0) {
		memcpy(buf + sizeof(f), payload, payload_bytes);
	}
	os_mutex_lock(&c->send_mtx);
	const int r = os_channel_send(c->ch, buf, sizeof(f) + payload_bytes, os_handles, os_count);
	os_mutex_unlock(&c->send_mtx);
	return r == 0;
}

void
conv_host_chan_shutdown(struct conv_host_chan *c)
{
	if (c != NULL) {
		c->dead = true;
	}
}

void
conv_host_chan_destroy(struct conv_host_chan **c_ptr)
{
	if (c_ptr == NULL || *c_ptr == NULL) {
		return;
	}
	struct conv_host_chan *c = *c_ptr;
	os_channel_destroy(&c->ch);
	os_mutex_destroy(&c->send_mtx);
	free(c);
	*c_ptr = NULL;
}
