// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  In-service conversion host: exec threads + admission rules.
 * @ingroup aux_conversion
 */

#include "conv_host_inproc.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

/*! The error every command completes with when its host is gone (abandoned or
 *  shutting down) — the same code a satellite reports on a lost pipe. */
#define CONV_HOST_ERR_GONE XRT_ERROR_IPC_FAILURE

struct cap_threading
{
	uint32_t cap_id;
	uint32_t flags;
	uint32_t max_concurrent; //!< ≥ 1
};

struct inflight
{
	bool used;
	uint32_t op;
	uint32_t cap_id;
	uint64_t since_ns;
};

struct image_slot
{
	bool used;
	struct xrt_conv_image img;
};

struct host_inproc
{
	struct conv_host base;

	const struct xrt_conversion_plugin_iface *iface;
	struct xrt_conversion_plugin_instance *inst;

	struct os_mutex mtx;
	struct os_cond cond;

	struct conv_host_cmd *head, *tail;
	uint32_t queued;

	struct inflight infl[CONV_HOST_INPROC_MAX_THREADS];
	uint32_t in_flight;

	struct cap_threading caps[XRT_CONV_MAX_CAPABILITIES];
	uint32_t cap_count;

	struct os_thread threads[CONV_HOST_INPROC_MAX_THREADS];
	uint32_t thread_count;

	bool stopping;
	bool hung;
	uint64_t calls_total;

	struct image_slot images[CONV_HOST_INPROC_MAX_IMAGES];
};

static inline struct host_inproc *
hi(struct conv_host *h)
{
	return (struct host_inproc *)h;
}


/*
 *
 * Admission.
 *
 */

static const struct cap_threading *
cap_of(const struct host_inproc *h, uint32_t cap_id)
{
	for (uint32_t i = 0; i < h->cap_count; i++) {
		if (h->caps[i].cap_id == cap_id) {
			return &h->caps[i];
		}
	}
	return NULL;
}

static uint32_t
flags_of(const struct host_inproc *h, uint32_t cap_id)
{
	const struct cap_threading *c = cap_of(h, cap_id);
	return c != NULL ? c->flags : 0u;
}

static uint32_t
max_conc_of(const struct host_inproc *h, uint32_t cap_id)
{
	const struct cap_threading *c = cap_of(h, cap_id);
	return c != NULL && c->max_concurrent > 1 ? c->max_concurrent : 1u;
}

static bool
is_stream_op(uint32_t op)
{
	return op == CONV_OP_STREAM_CREATE || op == CONV_OP_STREAM_DESTROY || op == CONV_OP_CONVERT;
}

/*!
 * May @p a and @p b run at the same time on this provider (discovery spec
 * §2.3)? Everything is serialised unless a capability opts in.
 */
static bool
compatible(const struct host_inproc *h, uint32_t op_a, uint32_t cap_a, uint32_t op_b, uint32_t cap_b)
{
	if (op_a == CONV_OP_EVICT || op_b == CONV_OP_EVICT) {
		return false;
	}
	if (op_a == CONV_OP_CONVERT && op_b == CONV_OP_CONVERT) {
		return cap_a == cap_b && max_conc_of(h, cap_a) > 1;
	}
	if (is_stream_op(op_a) && op_b == CONV_OP_JOB) {
		return (flags_of(h, cap_b) & XRT_CONV_CAP_LANES_CONCURRENT) != 0;
	}
	if (is_stream_op(op_b) && op_a == CONV_OP_JOB) {
		return (flags_of(h, cap_a) & XRT_CONV_CAP_LANES_CONCURRENT) != 0;
	}
	if (op_a == CONV_OP_WARM && (op_b == CONV_OP_CONVERT || op_b == CONV_OP_JOB)) {
		return (flags_of(h, cap_a) & XRT_CONV_CAP_WARM_CONCURRENT) != 0;
	}
	if (op_b == CONV_OP_WARM && (op_a == CONV_OP_CONVERT || op_a == CONV_OP_JOB)) {
		return (flags_of(h, cap_b) & XRT_CONV_CAP_WARM_CONCURRENT) != 0;
	}
	return false;
}

static uint32_t
blocked_reason(uint32_t inflight_op)
{
	switch (inflight_op) {
	case CONV_OP_JOB: return CONV_CMD_BLOCKED_BY_JOB;
	case CONV_OP_WARM: return CONV_CMD_BLOCKED_BY_WARM;
	default: return CONV_CMD_BLOCKED_BY_OTHER;
	}
}

/*! First queued command that may start now; unlinks it. Lock held. */
static struct conv_host_cmd *
pick_locked(struct host_inproc *h)
{
	struct conv_host_cmd *prev = NULL;
	for (struct conv_host_cmd *c = h->head; c != NULL; prev = c, c = c->next_) {
		bool ok = true;
		uint32_t blocked = 0;
		uint32_t same_cap_converts = 0;
		for (uint32_t i = 0; i < CONV_HOST_INPROC_MAX_THREADS; i++) {
			const struct inflight *f = &h->infl[i];
			if (!f->used) {
				continue;
			}
			if (!compatible(h, f->op, f->cap_id, c->op, c->cap_id)) {
				ok = false;
				blocked |= blocked_reason(f->op);
			}
			if (f->op == CONV_OP_CONVERT && c->op == CONV_OP_CONVERT && f->cap_id == c->cap_id) {
				same_cap_converts++;
			}
		}
		if (ok && c->op == CONV_OP_CONVERT && same_cap_converts >= max_conc_of(h, c->cap_id)) {
			ok = false;
			blocked |= CONV_CMD_BLOCKED_BY_OTHER;
		}
		// Keep submission order between conflicting commands: nothing may
		// overtake an earlier queued command it could not run beside.
		for (struct conv_host_cmd *q = h->head; ok && q != c; q = q->next_) {
			if (!compatible(h, q->op, q->cap_id, c->op, c->cap_id)) {
				ok = false;
			}
		}
		c->blocked |= blocked;
		if (!ok) {
			continue;
		}
		if (prev == NULL) {
			h->head = c->next_;
		} else {
			prev->next_ = c->next_;
		}
		if (h->tail == c) {
			h->tail = prev;
		}
		c->next_ = NULL;
		h->queued--;
		return c;
	}
	return NULL;
}


/*
 *
 * Cancel / progress wrappers.
 *
 */

struct cancel_wrap
{
	struct host_inproc *h;
	bool (*fn)(void *ctx);
	void *ctx;
	const struct xrt_conv_job_control *job_ctl; //!< JOB: the caller's control block
};

static bool
host_stopping(struct host_inproc *h)
{
	os_mutex_lock(&h->mtx);
	bool s = h->stopping || h->hung;
	os_mutex_unlock(&h->mtx);
	return s;
}

static bool
wrap_is_cancelled(void *ctx)
{
	struct cancel_wrap *w = (struct cancel_wrap *)ctx;
	if (host_stopping(w->h)) {
		return true;
	}
	return w->fn != NULL && w->fn(w->ctx);
}

static void
wrap_job_progress(void *ctx, float fraction, const char *stage)
{
	struct cancel_wrap *w = (struct cancel_wrap *)ctx;
	if (w->job_ctl != NULL && w->job_ctl->progress != NULL) {
		w->job_ctl->progress(w->job_ctl->ctx, fraction, stage);
	}
}


/*
 *
 * Exec threads.
 *
 */

static xrt_result_t
run_cmd(struct host_inproc *h, struct conv_host_cmd *c)
{
	const struct xrt_conversion_plugin_iface *f = h->iface;
	switch (c->op) {
	case CONV_OP_WARM: {
		if (!XRT_CONV_IFACE_HAS(f, warm)) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		struct cancel_wrap w = {h, c->is_cancelled, c->cancel_ctx, NULL};
		return f->warm(h->inst, c->cap_id, wrap_is_cancelled, &w);
	}
	case CONV_OP_EVICT:
		if (!XRT_CONV_IFACE_HAS(f, evict)) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		return f->evict(h->inst, c->cap_id);
	case CONV_OP_STREAM_CREATE:
		if (!XRT_CONV_IFACE_HAS(f, stream_create) || c->stream_info == NULL) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		return f->stream_create(h->inst, c->cap_id, c->stream_info, &c->stream);
	case CONV_OP_STREAM_DESTROY:
		if (XRT_CONV_IFACE_HAS(f, stream_destroy)) {
			f->stream_destroy(h->inst, c->stream);
		}
		return XRT_SUCCESS;
	case CONV_OP_CONVERT:
		if (!XRT_CONV_IFACE_HAS(f, stream_convert) || c->frame_in == NULL || c->params == NULL ||
		    c->frame_out == NULL) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		return f->stream_convert(h->inst, c->stream, c->frame_in, c->params, c->frame_out);
	case CONV_OP_JOB: {
		if (!XRT_CONV_IFACE_HAS(f, job_execute) || c->job_in == NULL || c->params == NULL ||
		    c->job_ctl == NULL || c->job_out == NULL) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		struct cancel_wrap w = {h, c->job_ctl->is_cancelled, c->job_ctl->ctx, c->job_ctl};
		struct xrt_conv_job_control ctl = *c->job_ctl;
		ctl.struct_size = (uint32_t)sizeof(ctl);
		ctl.ctx = &w;
		ctl.is_cancelled = wrap_is_cancelled;
		ctl.progress = wrap_job_progress;
		return f->job_execute(h->inst, c->cap_id, c->job_in, c->params, &ctl, c->job_out);
	}
	default: return XRT_ERROR_NOT_IMPLEMENTED;
	}
}

static int32_t
claim_inflight_locked(struct host_inproc *h, const struct conv_host_cmd *c, uint64_t now)
{
	for (uint32_t i = 0; i < CONV_HOST_INPROC_MAX_THREADS; i++) {
		if (!h->infl[i].used) {
			h->infl[i].used = true;
			h->infl[i].op = c->op;
			h->infl[i].cap_id = c->cap_id;
			h->infl[i].since_ns = now;
			h->in_flight++;
			return (int32_t)i;
		}
	}
	return -1; // impossible: at most thread_count ≤ MAX in flight
}

static void *
exec_main(void *ptr)
{
	struct host_inproc *h = (struct host_inproc *)ptr;
	os_mutex_lock(&h->mtx);
	for (;;) {
		if (h->stopping || h->hung) {
			break;
		}
		struct conv_host_cmd *c = pick_locked(h);
		if (c == NULL) {
			os_cond_wait(&h->cond, &h->mtx);
			continue;
		}
		const uint64_t now = os_monotonic_get_ns();
		const int32_t slot = claim_inflight_locked(h, c, now);
		c->start_ns = now;
		os_mutex_unlock(&h->mtx);

		c->result = run_cmd(h, c);

		os_mutex_lock(&h->mtx);
		c->end_ns = os_monotonic_get_ns();
		if (slot >= 0) {
			h->infl[slot].used = false;
			h->in_flight--;
		}
		h->calls_total++;
		os_cond_broadcast(&h->cond); // something may be admissible now
		os_mutex_unlock(&h->mtx);

		c->on_complete(c); // may free c

		os_mutex_lock(&h->mtx);
	}
	os_mutex_unlock(&h->mtx);
	return NULL;
}

static uint32_t
wanted_threads(const struct host_inproc *h)
{
	bool lanes = false, warm = false;
	uint32_t maxc = 1;
	for (uint32_t i = 0; i < h->cap_count; i++) {
		lanes = lanes || (h->caps[i].flags & XRT_CONV_CAP_LANES_CONCURRENT) != 0;
		warm = warm || (h->caps[i].flags & XRT_CONV_CAP_WARM_CONCURRENT) != 0;
		if (h->caps[i].max_concurrent > maxc) {
			maxc = h->caps[i].max_concurrent;
		}
	}
	uint32_t n = 1u + (lanes ? 1u : 0u) + (warm ? 1u : 0u) + (maxc - 1u);
	return n > CONV_HOST_INPROC_MAX_THREADS ? CONV_HOST_INPROC_MAX_THREADS : n;
}

/*! Lock held. */
static bool
grow_threads_locked(struct host_inproc *h, uint32_t want)
{
	while (h->thread_count < want) {
		struct os_thread *t = &h->threads[h->thread_count];
		if (os_thread_init(t) != 0) {
			return false;
		}
		if (os_thread_start(t, exec_main, h) != 0) {
			os_thread_destroy(t);
			return false;
		}
		h->thread_count++;
	}
	return true;
}


/*
 *
 * Ops.
 *
 */

static xrt_result_t
inproc_describe_cap(struct conv_host *base, uint32_t index, struct xrt_conv_capability *out, uint32_t *out_count)
{
	struct host_inproc *h = hi(base);
	if (!XRT_CONV_IFACE_HAS(h->iface, describe_cap)) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	return h->iface->describe_cap(h->inst, index, out, out_count);
}

static uint64_t
inproc_get_generation(struct conv_host *base)
{
	struct host_inproc *h = hi(base);
	return XRT_CONV_IFACE_HAS(h->iface, get_generation) ? h->iface->get_generation(h->inst) : 0;
}

static xrt_result_t
inproc_get_state(struct conv_host *base, uint32_t cap_id, struct xrt_conv_state *out)
{
	struct host_inproc *h = hi(base);
	if (!XRT_CONV_IFACE_HAS(h->iface, get_state)) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	return h->iface->get_state(h->inst, cap_id, out);
}

static xrt_result_t
inproc_get_output_desc(
    struct conv_host *base, uint64_t stream, uint32_t w, uint32_t hgt, struct xrt_conv_output_desc *out)
{
	struct host_inproc *h = hi(base);
	if (!XRT_CONV_IFACE_HAS(h->iface, stream_get_output_desc)) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	return h->iface->stream_get_output_desc(h->inst, stream, w, hgt, out);
}

static xrt_result_t
inproc_submit(struct conv_host *base, struct conv_host_cmd *cmd)
{
	struct host_inproc *h = hi(base);
	if (cmd == NULL || cmd->on_complete == NULL || cmd->op == CONV_OP_NONE || cmd->op > CONV_OP_JOB) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	os_mutex_lock(&h->mtx);
	if (h->stopping || h->hung) {
		os_mutex_unlock(&h->mtx);
		return CONV_HOST_ERR_GONE;
	}
	cmd->next_ = NULL;
	cmd->blocked = 0;
	// Report what it waits behind now (the busy threads cannot re-examine it).
	for (uint32_t i = 0; i < CONV_HOST_INPROC_MAX_THREADS; i++) {
		const struct inflight *f = &h->infl[i];
		if (f->used && !compatible(h, f->op, f->cap_id, cmd->op, cmd->cap_id)) {
			cmd->blocked |= blocked_reason(f->op);
		}
	}
	cmd->result = XRT_SUCCESS;
	cmd->queued_ns = os_monotonic_get_ns();
	cmd->start_ns = cmd->end_ns = 0;
	if (h->tail == NULL) {
		h->head = h->tail = cmd;
	} else {
		h->tail->next_ = cmd;
		h->tail = cmd;
	}
	h->queued++;
	os_cond_broadcast(&h->cond);
	os_mutex_unlock(&h->mtx);
	return XRT_SUCCESS;
}

static void
inproc_refresh_caps(struct conv_host *base)
{
	struct host_inproc *h = hi(base);
	struct cap_threading caps[XRT_CONV_MAX_CAPABILITIES];
	uint32_t n = 0, count = 0;
	if (XRT_CONV_IFACE_HAS(h->iface, describe_cap) &&
	    h->iface->describe_cap(h->inst, 0, NULL, &count) == XRT_SUCCESS) {
		if (count > XRT_CONV_MAX_CAPABILITIES) {
			count = XRT_CONV_MAX_CAPABILITIES;
		}
		for (uint32_t i = 0; i < count; i++) {
			struct xrt_conv_capability cap;
			memset(&cap, 0, sizeof(cap));
			cap.struct_size = (uint32_t)sizeof(cap);
			if (h->iface->describe_cap(h->inst, i, &cap, NULL) != XRT_SUCCESS || cap.cap_id == 0) {
				continue;
			}
			caps[n].cap_id = cap.cap_id;
			caps[n].flags = cap.flags;
			caps[n].max_concurrent = cap.max_concurrent > 1 ? cap.max_concurrent : 1u;
			n++;
		}
	}
	os_mutex_lock(&h->mtx);
	memcpy(h->caps, caps, sizeof(caps[0]) * n);
	h->cap_count = n;
	if (!h->stopping && !h->hung && !grow_threads_locked(h, wanted_threads(h))) {
		U_LOG_W("[conv] provider %s: could not start every exec thread (%u running)", h->base.provider_id,
		        h->thread_count);
	}
	os_cond_broadcast(&h->cond);
	os_mutex_unlock(&h->mtx);
}

static xrt_result_t
inproc_register_image(struct conv_host *base, const struct xrt_conv_image *img, uint32_t *out_index)
{
	struct host_inproc *h = hi(base);
	if (img == NULL || out_index == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	os_mutex_lock(&h->mtx);
	for (uint32_t i = 0; i < CONV_HOST_INPROC_MAX_IMAGES; i++) {
		if (!h->images[i].used) {
			h->images[i].used = true;
			h->images[i].img = *img;
			*out_index = i;
			os_mutex_unlock(&h->mtx);
			return XRT_SUCCESS;
		}
	}
	os_mutex_unlock(&h->mtx);
	return XRT_ERROR_ALLOCATION;
}

static void
inproc_unregister_image(struct conv_host *base, uint32_t index)
{
	struct host_inproc *h = hi(base);
	if (index >= CONV_HOST_INPROC_MAX_IMAGES) {
		return;
	}
	os_mutex_lock(&h->mtx);
	h->images[index].used = false;
	os_mutex_unlock(&h->mtx);
}

static bool
inproc_lookup_image(struct conv_host *base, uint32_t index, struct xrt_conv_image *out)
{
	struct host_inproc *h = hi(base);
	if (index >= CONV_HOST_INPROC_MAX_IMAGES || out == NULL) {
		return false;
	}
	os_mutex_lock(&h->mtx);
	const bool ok = h->images[index].used;
	if (ok) {
		*out = h->images[index].img;
	}
	os_mutex_unlock(&h->mtx);
	return ok;
}

static void
inproc_heartbeat(struct conv_host *base, struct conv_host_heartbeat *out)
{
	struct host_inproc *h = hi(base);
	memset(out, 0, sizeof(*out));
	os_mutex_lock(&h->mtx);
	out->in_flight = h->in_flight;
	out->queued = h->queued;
	out->calls_total = h->calls_total;
	out->exec_threads = h->thread_count;
	out->hung = h->hung;
	for (uint32_t i = 0; i < CONV_HOST_INPROC_MAX_THREADS; i++) {
		const struct inflight *f = &h->infl[i];
		if (f->used && (out->oldest_since_ns == 0 || f->since_ns < out->oldest_since_ns)) {
			out->oldest_since_ns = f->since_ns;
			out->oldest_op = f->op;
			out->oldest_cap_id = f->cap_id;
		}
	}
	os_mutex_unlock(&h->mtx);
}

/*! Unlink every queued command (lock held); the caller completes them unlocked. */
static struct conv_host_cmd *
take_queue_locked(struct host_inproc *h)
{
	struct conv_host_cmd *list = h->head;
	h->head = h->tail = NULL;
	h->queued = 0;
	return list;
}

static void
fail_list(struct conv_host_cmd *list)
{
	while (list != NULL) {
		struct conv_host_cmd *next = list->next_;
		list->next_ = NULL;
		list->result = CONV_HOST_ERR_GONE;
		list->on_complete(list);
		list = next;
	}
}

static void
inproc_abandon(struct conv_host *base)
{
	struct host_inproc *h = hi(base);
	os_mutex_lock(&h->mtx);
	if (h->hung) {
		os_mutex_unlock(&h->mtx);
		return;
	}
	h->hung = true;
	struct conv_host_cmd *list = take_queue_locked(h);
	os_cond_broadcast(&h->cond);
	os_mutex_unlock(&h->mtx);
	U_LOG_W("[conv] provider %s abandoned by the watchdog: no further calls until the service restarts",
	        h->base.provider_id);
	fail_list(list);
}

static void
inproc_destroy(struct conv_host **h_ptr)
{
	struct host_inproc *h = hi(*h_ptr);
	*h_ptr = NULL;

	os_mutex_lock(&h->mtx);
	const bool hung = h->hung;
	h->stopping = true;
	struct conv_host_cmd *list = take_queue_locked(h);
	os_cond_broadcast(&h->cond);
	os_mutex_unlock(&h->mtx);
	fail_list(list);

	if (hung) {
		// A thread is stuck inside the provider and still points at us:
		// leak the host (ADR-053: in-service, a hang lasts until restart).
		return;
	}
	for (uint32_t i = 0; i < h->thread_count; i++) {
		os_thread_join(&h->threads[i]);
		os_thread_destroy(&h->threads[i]);
	}
	// [S] destroy, after every other call returned (it MAY never be called).
	if (XRT_CONV_IFACE_HAS(h->iface, destroy) && h->inst != NULL) {
		h->iface->destroy(h->inst);
	}
	os_cond_destroy(&h->cond);
	os_mutex_destroy(&h->mtx);
	free(h);
}

static const struct conv_host_ops k_inproc_ops = {
    .describe_cap = inproc_describe_cap,
    .get_generation = inproc_get_generation,
    .get_state = inproc_get_state,
    .get_output_desc = inproc_get_output_desc,
    .submit = inproc_submit,
    .refresh_caps = inproc_refresh_caps,
    .register_image = inproc_register_image,
    .unregister_image = inproc_unregister_image,
    .lookup_image = inproc_lookup_image,
    .heartbeat = inproc_heartbeat,
    .abandon = inproc_abandon,
    .destroy = inproc_destroy,
};

xrt_result_t
conv_host_inproc_create(const char *provider_id,
                        const struct xrt_conversion_plugin_iface *iface,
                        struct xrt_conversion_plugin_instance *inst,
                        struct conv_host **out_host)
{
	if (iface == NULL || out_host == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	struct host_inproc *h = (struct host_inproc *)calloc(1, sizeof(*h));
	if (h == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	h->base.ops = &k_inproc_ops;
	h->base.hosting = 1u; // CONV_HOSTING_IN_SERVICE
	if (provider_id != NULL) {
		strncpy(h->base.provider_id, provider_id, sizeof(h->base.provider_id) - 1);
	}
	h->iface = iface;
	h->inst = inst;
	if (os_mutex_init(&h->mtx) != 0 || os_cond_init(&h->cond) != 0) {
		free(h);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	inproc_refresh_caps(&h->base);
	if (h->thread_count == 0) {
		struct conv_host *b = &h->base;
		inproc_destroy(&b);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	*out_host = &h->base;
	return XRT_SUCCESS;
}


/*
 *
 * Synchronous helper (any host implementation).
 *
 */

struct call_waiter
{
	struct os_mutex m;
	struct os_cond c;
	bool done;
};

static void
call_done(struct conv_host_cmd *cmd)
{
	struct call_waiter *w = (struct call_waiter *)cmd->user;
	os_mutex_lock(&w->m);
	w->done = true;
	os_cond_signal(&w->c);
	os_mutex_unlock(&w->m);
}

xrt_result_t
conv_host_call(struct conv_host *h, struct conv_host_cmd *cmd)
{
	struct call_waiter w;
	memset(&w, 0, sizeof(w));
	if (os_mutex_init(&w.m) != 0 || os_cond_init(&w.c) != 0) {
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	cmd->on_complete = call_done;
	cmd->user = &w;
	xrt_result_t r = conv_host_submit(h, cmd);
	if (r == XRT_SUCCESS) {
		os_mutex_lock(&w.m);
		while (!w.done) {
			os_cond_wait(&w.c, &w.m);
		}
		os_mutex_unlock(&w.m);
		r = cmd->result;
	}
	os_cond_destroy(&w.c);
	os_mutex_destroy(&w.m);
	return r;
}
