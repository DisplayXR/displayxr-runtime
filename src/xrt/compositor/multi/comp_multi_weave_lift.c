// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lifted weave rects on the macOS / desktop-Linux weave engines —
 *         comp_multi's side; see comp_multi_weave_lift.h.
 * @ingroup comp_multi
 */

#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_dp_lift.h"

#include "comp_multi_weave_lift.h"
#include "comp_multi_private.h"

#include "util/u_logging.h"

#ifdef XRT_OS_MACOS
#include "comp_multi_weave_macos_backend.h"
#endif

#include <pthread.h>
#include <string.h>

#if defined(XRT_OS_MACOS) || defined(XRT_OS_LINUX_DESKTOP)

/*
 * The helper's ops. g_lift_mtx is held across every call into them, so
 * clearing the table (detach) waits for calls in flight. Engines are
 * independent; the serialisation is a few microseconds of bookkeeping per
 * lifted rect.
 */
static pthread_mutex_t g_lift_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct vk_conv_weave_ops g_ops;
static bool g_have_ops;
static uint64_t g_gen = 1;

static pthread_mutex_t g_eyes_mtx = PTHREAD_MUTEX_INITIALIZER;
static void (*g_eyes_publish)(void *ud, uint64_t dp_key, const struct xrt_eye_positions *eyes);
static void (*g_eyes_retire)(void *ud, uint64_t dp_key);
static void *g_eyes_ud;

void
comp_multi_weave_set_lift_ops(const struct vk_conv_weave_ops *ops)
{
	pthread_mutex_lock(&g_lift_mtx);
	if (ops != NULL) {
		g_ops = *ops;
		g_have_ops = true;
	} else {
		memset(&g_ops, 0, sizeof(g_ops));
		g_have_ops = false;
	}
	g_gen++; // every client of the old table is stale
	pthread_mutex_unlock(&g_lift_mtx);
}

void
comp_multi_set_conv_eyes_sink(void (*publish)(void *ud, uint64_t dp_key, const struct xrt_eye_positions *eyes),
                              void (*retire)(void *ud, uint64_t dp_key),
                              void *ud)
{
	pthread_mutex_lock(&g_eyes_mtx);
	g_eyes_publish = publish;
	g_eyes_retire = retire;
	g_eyes_ud = ud;
	pthread_mutex_unlock(&g_eyes_mtx);
}

void
comp_multi_conv_eyes_publish(uint64_t dp_key, const struct xrt_eye_positions *eyes)
{
	if (dp_key == 0 || eyes == NULL) {
		return;
	}
	pthread_mutex_lock(&g_eyes_mtx);
	if (g_eyes_publish != NULL) {
		g_eyes_publish(g_eyes_ud, dp_key, eyes);
	}
	pthread_mutex_unlock(&g_eyes_mtx);
}

void
comp_multi_conv_eyes_retire(uint64_t dp_key)
{
	if (dp_key == 0) {
		return;
	}
	pthread_mutex_lock(&g_eyes_mtx);
	if (g_eyes_retire != NULL) {
		g_eyes_retire(g_eyes_ud, dp_key);
	}
	pthread_mutex_unlock(&g_eyes_mtx);
}

//! g_lift_mtx held: the engine's client, dropped if it belongs to an old table.
static struct vk_conv_weave_client *
client_locked(struct comp_multi_weave_lift *l)
{
	if (l->client != NULL && l->client_gen != g_gen) {
		l->client = NULL; // freed by the helper that made it
		l->in_flight = false;
	}
	return l->client;
}

bool
comp_multi_weave_lift_set_rects(struct xrt_compositor *xc,
                                uint64_t owner,
                                uint32_t count,
                                const struct xrt_lift_weave_rect *rects)
{
	if (xc == NULL || !multi_compositor_is(xc) || count > XRT_LIFT_WEAVE_RECTS_MAX ||
	    (count > 0 && rects == NULL)) {
		return false;
	}
	struct multi_compositor *mc = multi_compositor(xc);
	struct comp_multi_weave_lift *l = &mc->weave.lift;
#ifdef XRT_OS_MACOS
	// The backend is picked at the first submit; one that cannot lift refuses.
	if (count > 0 && mc->weave.backend != NULL && !mc->weave.backend->lifts_rects) {
		return false;
	}
#endif
	if (count > 0) {
		pthread_mutex_lock(&g_lift_mtx);
		bool ok = g_have_ops && g_ops.stream_mode != NULL;
		// Only this owner's live SBS / NVIEW streams can be woven.
		for (uint32_t i = 0; ok && i < count; i++) {
			const uint32_t mode = g_ops.stream_mode(g_ops.ud, owner, rects[i].stream_id);
			ok = mode == XRT_DP_LIFT_MODE_SBS || mode == XRT_DP_LIFT_MODE_NVIEW;
		}
		pthread_mutex_unlock(&g_lift_mtx);
		if (!ok) {
			return false;
		}
		memcpy(l->rects, rects, count * sizeof(rects[0]));
	}
	// Set and consumed on the client's own IPC thread, in order (the IPC call
	// right before its submit): no lock needed against the submit.
	l->owner = owner;
	l->count = count;
	return true;
}

uint32_t
comp_multi_weave_lift_take(struct comp_multi_weave_lift *l,
                           uint64_t *out_owner,
                           struct xrt_lift_weave_rect out[XRT_LIFT_WEAVE_RECTS_MAX])
{
	uint32_t n = l->count > XRT_LIFT_WEAVE_RECTS_MAX ? XRT_LIFT_WEAVE_RECTS_MAX : l->count;
	if (n > 0) {
		memcpy(out, l->rects, n * sizeof(out[0]));
	}
	*out_owner = l->owner;
	l->count = 0;
	return n;
}

bool
comp_multi_weave_lift_begin(struct comp_multi_weave_lift *l, struct vk_bundle *vk, uint32_t count)
{
	pthread_mutex_lock(&g_lift_mtx);
	struct vk_conv_weave_client *c = client_locked(l);
	if (c != NULL && l->in_flight && g_ops.begin != NULL) {
		g_ops.begin(g_ops.ud, c); // the last weave never completed: it was never submitted
		l->in_flight = false;
	}
	bool ok = false;
	if (count > 0 && g_have_ops) {
		if (c == NULL && g_ops.client_create != NULL) {
			c = g_ops.client_create(g_ops.ud, vk);
			l->client = c;
			l->client_gen = g_gen;
		}
		ok = c != NULL;
		if (ok) {
			g_ops.begin(g_ops.ud, c);
			l->in_flight = true;
		}
	}
	pthread_mutex_unlock(&g_lift_mtx);
	return ok;
}

bool
comp_multi_weave_lift_record(struct comp_multi_weave_lift *l, VkCommandBuffer cmd, const struct vk_conv_weave_rect *r)
{
	pthread_mutex_lock(&g_lift_mtx);
	struct vk_conv_weave_client *c = client_locked(l);
	const bool ok = c != NULL && l->in_flight && g_ops.record != NULL && g_ops.record(g_ops.ud, c, cmd, r);
	pthread_mutex_unlock(&g_lift_mtx);
	return ok;
}

bool
comp_multi_weave_lift_host_plan(struct comp_multi_weave_lift *l,
                                const struct vk_conv_weave_rect *r,
                                struct vk_conv_weave_host_plan *out)
{
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&g_lift_mtx);
	struct vk_conv_weave_client *c = client_locked(l);
	const bool ok = c != NULL && l->in_flight && g_ops.host_plan != NULL && g_ops.host_plan(g_ops.ud, c, r, out);
	pthread_mutex_unlock(&g_lift_mtx);
	return ok;
}

void
comp_multi_weave_lift_host_done(struct comp_multi_weave_lift *l,
                                uint64_t owner,
                                uint64_t wire_id,
                                const struct vk_conv_weave_host_done *d)
{
	pthread_mutex_lock(&g_lift_mtx);
	struct vk_conv_weave_client *c = client_locked(l);
	if (c != NULL && l->in_flight && g_ops.host_done != NULL) {
		g_ops.host_done(g_ops.ud, c, owner, wire_id, d);
	}
	pthread_mutex_unlock(&g_lift_mtx);
}

void
comp_multi_weave_lift_complete(struct comp_multi_weave_lift *l)
{
	if (!l->in_flight) {
		return;
	}
	pthread_mutex_lock(&g_lift_mtx);
	struct vk_conv_weave_client *c = client_locked(l);
	if (c != NULL && l->in_flight && g_ops.complete != NULL) {
		g_ops.complete(g_ops.ud, c);
	}
	l->in_flight = false;
	pthread_mutex_unlock(&g_lift_mtx);
}

void
comp_multi_weave_lift_fini(struct comp_multi_weave_lift *l)
{
	pthread_mutex_lock(&g_lift_mtx);
	struct vk_conv_weave_client *c = client_locked(l);
	if (c != NULL && g_ops.client_destroy != NULL) {
		g_ops.client_destroy(g_ops.ud, c);
	}
	l->client = NULL;
	l->in_flight = false;
	l->count = 0;
	pthread_mutex_unlock(&g_lift_mtx);
}

void
comp_multi_weave_lift_rect_frame(struct multi_compositor *mc,
                                 const struct xrt_rect *rect,
                                 uint32_t win_w,
                                 uint32_t win_h,
                                 uint64_t dp_key,
                                 struct xrt_lift_rect_frame *out)
{
	memset(out, 0, sizeof(*out));
	out->eyes_dp_key = dp_key;
	if (mc->msc != NULL) {
		out->width_m = mc->msc->base.info.display_width_m;
		out->height_m = mc->msc->base.info.display_height_m;
	}
	struct xrt_window_metrics wm;
	memset(&wm, 0, sizeof(wm));
	if (!multi_compositor_get_window_metrics(mc, &wm) || !wm.valid || wm.window_width_m <= 0.0f ||
	    wm.window_height_m <= 0.0f) {
		return;
	}
	out->center[0] = wm.window_center_offset_x_m;
	out->center[1] = wm.window_center_offset_y_m;
	out->center[2] = wm.window_center_offset_z_m;
	out->width_m = wm.window_width_m;
	out->height_m = wm.window_height_m;
	if (rect != NULL && win_w > 0 && win_h > 0) {
		// Rect fractions of the window (window pixels, y down) -> metres (y up).
		const float fx = ((float)rect->offset.w + 0.5f * (float)rect->extent.w) / (float)win_w - 0.5f;
		const float fy = ((float)rect->offset.h + 0.5f * (float)rect->extent.h) / (float)win_h - 0.5f;
		out->center[0] += fx * wm.window_width_m;
		out->center[1] -= fy * wm.window_height_m;
		out->width_m = (float)rect->extent.w / (float)win_w * wm.window_width_m;
		out->height_m = (float)rect->extent.h / (float)win_h * wm.window_height_m;
	}
	out->valid = true;
}

#endif // XRT_OS_MACOS || XRT_OS_LINUX_DESKTOP
