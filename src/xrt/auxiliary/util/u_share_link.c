// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2: compositor share tap <-> session service link
 *         hand-off (see u_share_link.h).
 * @ingroup aux_util
 */

#include "util/u_share_link.h"
#include "util/u_logging.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct u_share_link_sink g_sink;
static void *g_ctx = NULL;
static bool g_installed = false;

static bool g_tap_available = false;
static uint32_t g_tap_reason = 0;
static uint8_t g_tap_luid[XRT_LUID_SIZE];
static bool g_tap_luid_valid = false;

void
u_share_link_install(const struct u_share_link_sink *sink, void *ctx)
{
	if (sink == NULL || sink->set_tap == NULL || sink->poll == NULL || sink->publish_ring == NULL ||
	    sink->frame_ready == NULL) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	if (g_installed && g_ctx != ctx) {
		U_LOG_W("app share: a second in-process session installed its share link; the first one's is replaced");
	}
	g_sink = *sink;
	g_ctx = ctx;
	g_installed = true;
	g_sink.set_tap(g_ctx, g_tap_available, g_tap_reason, g_tap_luid, g_tap_luid_valid);
	pthread_mutex_unlock(&g_lock);
}

void
u_share_link_uninstall(void *ctx)
{
	pthread_mutex_lock(&g_lock);
	if (g_installed && g_ctx == ctx) {
		memset(&g_sink, 0, sizeof(g_sink));
		g_ctx = NULL;
		g_installed = false;
	}
	pthread_mutex_unlock(&g_lock);
}

bool
u_share_link_installed(void)
{
	pthread_mutex_lock(&g_lock);
	const bool r = g_installed;
	pthread_mutex_unlock(&g_lock);
	return r;
}

void
u_share_link_set_tap(bool available, uint32_t unavailable_reason, const uint8_t luid[XRT_LUID_SIZE], bool luid_valid)
{
	pthread_mutex_lock(&g_lock);
	g_tap_available = available;
	g_tap_reason = available ? 0u : unavailable_reason;
	if (luid_valid && luid != NULL) {
		memcpy(g_tap_luid, luid, XRT_LUID_SIZE);
		g_tap_luid_valid = true;
	} else {
		// No adapter now: a stale one must not be reported (or replayed) any more.
		memset(g_tap_luid, 0, sizeof(g_tap_luid));
		g_tap_luid_valid = false;
	}
	if (g_installed) {
		g_sink.set_tap(g_ctx, g_tap_available, g_tap_reason, g_tap_luid, g_tap_luid_valid);
	}
	pthread_mutex_unlock(&g_lock);
}

void
u_share_link_get_tap(bool *out_available, uint32_t *out_unavailable_reason)
{
	pthread_mutex_lock(&g_lock);
	if (out_available != NULL) {
		*out_available = g_tap_available;
	}
	if (out_unavailable_reason != NULL) {
		*out_unavailable_reason = g_tap_reason;
	}
	pthread_mutex_unlock(&g_lock);
}

bool
u_share_link_poll(struct xrt_share_link_poll *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&g_lock);
	const bool r = g_installed && g_sink.poll(g_ctx, out);
	pthread_mutex_unlock(&g_lock);
	return r;
}

xrt_result_t
u_share_link_publish_ring(const struct xrt_share_link_ring_desc *desc,
                          const xrt_graphics_buffer_handle_t *textures,
                          uint32_t texture_count,
                          xrt_graphics_sync_handle_t fence)
{
	pthread_mutex_lock(&g_lock);
	xrt_result_t r = g_installed ? g_sink.publish_ring(g_ctx, desc, textures, texture_count, fence)
	                             : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	pthread_mutex_unlock(&g_lock);
	return r;
}

xrt_result_t
u_share_link_frame_ready(const struct xrt_share_link_frame *frame, struct xrt_share_link_poll *out_poll)
{
	if (out_poll != NULL) {
		memset(out_poll, 0, sizeof(*out_poll));
	}
	pthread_mutex_lock(&g_lock);
	xrt_result_t r = g_installed ? g_sink.frame_ready(g_ctx, frame, out_poll) : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	pthread_mutex_unlock(&g_lock);
	return r;
}

void
u_share_link_reset(void)
{
	pthread_mutex_lock(&g_lock);
	memset(&g_sink, 0, sizeof(g_sink));
	g_ctx = NULL;
	g_installed = false;
	g_tap_available = false;
	g_tap_reason = 0;
	memset(g_tap_luid, 0, sizeof(g_tap_luid));
	g_tap_luid_valid = false;
	pthread_mutex_unlock(&g_lock);
}

void
u_share_link_poll_cache_reset(struct u_share_link_poll_cache *c)
{
	memset(c, 0, sizeof(*c));
}

void
u_share_link_poll_cache_merge(struct u_share_link_poll_cache *c, const struct xrt_share_link_poll *in)
{
	if (in->state == XRT_SHARE_LINK_STATE_SHARE_STOPPED && c->stopped_taken &&
	    in->stream_generation <= c->stopped_taken_generation) {
		return; // that stop was already reported
	}
	const bool unread_stop = c->valid && c->poll.state == XRT_SHARE_LINK_STATE_SHARE_STOPPED;
	if (unread_stop && in->state == XRT_SHARE_LINK_STATE_NONE) {
		c->valid = true; // keep the STOPPED until the consumer took it
		return;
	}
	c->poll = *in;
	c->valid = true;
}

bool
u_share_link_poll_cache_take(struct u_share_link_poll_cache *c, struct xrt_share_link_poll *out)
{
	memset(out, 0, sizeof(*out));
	if (!c->valid) {
		return false;
	}
	*out = c->poll;
	if (c->poll.state == XRT_SHARE_LINK_STATE_SHARE_STOPPED) {
		c->stopped_taken = true;
		c->stopped_taken_generation = c->poll.stream_generation;
		c->poll.state = XRT_SHARE_LINK_STATE_NONE; // reported once
	}
	return true;
}
