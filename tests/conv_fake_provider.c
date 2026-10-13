// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A scriptable CPU conversion provider for tests_conv_lanes.
 */

#include "conv_fake_provider.h"

#include "os/os_time.h"

#include <string.h>

static struct conv_fake_provider *
fp(struct xrt_conversion_plugin_instance *inst)
{
	return (struct conv_fake_provider *)(void *)inst;
}

static struct conv_fake_cap *
find_cap(struct conv_fake_provider *f, uint32_t cap_id)
{
	for (uint32_t i = 0; i < f->cap_count; i++) {
		if (f->caps[i].cap.cap_id == cap_id) {
			return &f->caps[i];
		}
	}
	return NULL;
}

struct conv_fake_cap *
conv_fake_find(struct conv_fake_provider *f, uint32_t cap_id)
{
	return find_cap(f, cap_id);
}

static void
enter_s(struct conv_fake_provider *f)
{
	os_mutex_lock(&f->mtx);
	f->in_flight++;
	if (f->in_flight > f->max_in_flight) {
		f->max_in_flight = f->in_flight;
	}
	os_mutex_unlock(&f->mtx);
}

static void
leave_s(struct conv_fake_provider *f)
{
	os_mutex_lock(&f->mtx);
	f->in_flight--;
	os_mutex_unlock(&f->mtx);
}

static void
sleep_ms(uint32_t ms)
{
	os_nanosleep((int64_t)ms * 1000000);
}

/* [C] */

static bool
fake_status(struct xrt_conv_provider_status *out)
{
	out->state = 0;
	out->flags = 0;
	return true;
}

static xrt_result_t
fake_probe(struct xrt_conversion_plugin_instance **out_inst)
{
	(void)out_inst;
	return XRT_SUCCESS;
}

static xrt_result_t
fake_describe(struct xrt_conversion_plugin_instance *inst,
              uint32_t index,
              struct xrt_conv_capability *out,
              uint32_t *out_count)
{
	struct conv_fake_provider *f = fp(inst);
	os_mutex_lock(&f->mtx);
	const uint32_t n = f->cap_count;
	if (out_count != NULL) {
		*out_count = n;
	}
	if (out == NULL) {
		os_mutex_unlock(&f->mtx);
		return XRT_SUCCESS;
	}
	if (index >= n) {
		os_mutex_unlock(&f->mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const uint32_t sz = out->struct_size;
	*out = f->caps[index].cap;
	out->struct_size = sz;
	os_mutex_unlock(&f->mtx);
	return XRT_SUCCESS;
}

static uint64_t
fake_generation(struct xrt_conversion_plugin_instance *inst)
{
	struct conv_fake_provider *f = fp(inst);
	os_mutex_lock(&f->mtx);
	const uint64_t g = f->generation;
	os_mutex_unlock(&f->mtx);
	return g;
}

static xrt_result_t
fake_get_state(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, struct xrt_conv_state *out)
{
	struct conv_fake_provider *f = fp(inst);
	os_mutex_lock(&f->mtx);
	struct conv_fake_cap *c = find_cap(f, cap_id);
	if (c == NULL) {
		os_mutex_unlock(&f->mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	out->state = c->state;
	out->failure_reason = c->failure_reason;
	out->retryable = c->retryable;
	out->progress = c->progress;
	os_mutex_unlock(&f->mtx);
	return XRT_SUCCESS;
}

static xrt_result_t
fake_output_desc(struct xrt_conversion_plugin_instance *inst,
                 uint64_t stream,
                 uint32_t w,
                 uint32_t h,
                 struct xrt_conv_output_desc *out)
{
	(void)inst;
	const uint32_t views = (uint32_t)(stream >> 32);
	const uint32_t layout = (uint32_t)((stream >> 24) & 0xFF);
	if (views > 0) {
		out->views_width = layout == XRT_CONV_VIEWS_ARRAY ? w : views * w;
		out->views_height = h;
		out->views_format = XRT_CONV_FMT_RGBA8_UNORM;
	}
	out->depth_width = w;
	out->depth_height = h;
	out->depth_format = XRT_CONV_FMT_R32_FLOAT;
	return XRT_SUCCESS;
}

/* [S] */

static xrt_result_t
fake_warm(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, bool (*is_cancelled)(void *), void *ctx)
{
	struct conv_fake_provider *f = fp(inst);
	enter_s(f);
	os_mutex_lock(&f->mtx);
	struct conv_fake_cap *c = find_cap(f, cap_id);
	const uint32_t ms = c != NULL ? c->warm_ms : 0;
	if (c != NULL && c->state != XRT_CONV_STATE_FAILED) {
		c->state = XRT_CONV_STATE_WARMING;
		c->progress = 0.0f;
	}
	f->warms++;
	const uint32_t poll = f->cancel_poll_ms != 0 ? f->cancel_poll_ms : 10;
	os_mutex_unlock(&f->mtx);
	xrt_result_t r = XRT_SUCCESS;
	for (uint32_t t = 0; t < ms; t += poll) {
		if (is_cancelled != NULL && is_cancelled(ctx)) {
			r = XRT_TIMEOUT;
			break;
		}
		sleep_ms(poll);
		os_mutex_lock(&f->mtx);
		if (c != NULL) {
			c->progress = (float)(t + poll) / (float)ms;
		}
		os_mutex_unlock(&f->mtx);
	}
	os_mutex_lock(&f->mtx);
	if (c != NULL && r == XRT_SUCCESS && c->state == XRT_CONV_STATE_WARMING) {
		c->state = c->warm_end_state != 0 ? c->warm_end_state : XRT_CONV_STATE_READY;
		c->progress = c->state == XRT_CONV_STATE_READY ? 1.0f : -1.0f;
		if (c->state != XRT_CONV_STATE_READY) {
			c->retryable = 0;
			r = XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
	}
	os_mutex_unlock(&f->mtx);
	leave_s(f);
	return r;
}

static xrt_result_t
fake_evict(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id)
{
	struct conv_fake_provider *f = fp(inst);
	os_mutex_lock(&f->mtx);
	struct conv_fake_cap *c = find_cap(f, cap_id);
	if (c != NULL && c->state == XRT_CONV_STATE_READY) {
		c->state = XRT_CONV_STATE_EVICTED;
	}
	os_mutex_unlock(&f->mtx);
	return XRT_SUCCESS;
}

static xrt_result_t
fake_stream_create(struct xrt_conversion_plugin_instance *inst,
                   uint32_t cap_id,
                   const struct xrt_conv_stream_info *info,
                   uint64_t *out_stream)
{
	struct conv_fake_provider *f = fp(inst);
	enter_s(f);
	os_mutex_lock(&f->mtx);
	f->streams_created++;
	f->first_frame_pending = true;
	// Encode view count + layout in the handle: output_desc is pure.
	*out_stream = ((uint64_t)info->view_count << 32) | ((uint64_t)(info->views_layout & 0xFF) << 24) |
	              ((uint64_t)cap_id << 8) | (f->streams_created & 0xFF);
	os_mutex_unlock(&f->mtx);
	leave_s(f);
	return XRT_SUCCESS;
}

static void
fill_depth_ramp(const struct xrt_conv_image *img, float lo, float hi)
{
	if (img->memory_kind != XRT_CONV_MEM_CPU || img->cpu_ptr == NULL) {
		return;
	}
	const uint32_t stride = img->row_stride != 0 ? img->row_stride / 4u : img->width;
	float *p = (float *)img->cpu_ptr;
	for (uint32_t y = 0; y < img->height; y++) {
		const float v = img->height > 1 ? lo + (hi - lo) * (float)y / (float)(img->height - 1) : lo;
		for (uint32_t x = 0; x < img->width; x++) {
			p[(size_t)y * stride + x] = v;
		}
	}
}

static xrt_result_t
fake_stream_convert(struct xrt_conversion_plugin_instance *inst,
                    uint64_t stream,
                    const struct xrt_conv_frame_in *in,
                    const struct xrt_conv_params *params,
                    struct xrt_conv_frame_out *out)
{
	struct conv_fake_provider *f = fp(inst);
	enter_s(f);
	const uint32_t cap_id = (uint32_t)((stream >> 8) & 0xFFFF);
	os_mutex_lock(&f->mtx);
	struct conv_fake_cap *c = find_cap(f, cap_id);
	const uint32_t ms = c != NULL ? c->convert_ms : 0;
	const bool fail = c != NULL && c->fail_converts;
	f->converts++;
	f->last_params = *params;
	if ((params->frame_flags & XRT_CONV_FRAME_RESET) != 0) {
		f->resets_seen++;
	}
	if (f->first_frame_pending) {
		f->first_frame_pending = false;
		f->first_frame_flags = params->frame_flags;
	}
	if (fail && c != NULL) {
		c->state = XRT_CONV_STATE_FAILED;
		c->failure_reason = XRT_CONV_FAIL_DEVICE_LOST;
		c->retryable = 0;
	}
	const uint32_t task = c != NULL ? c->cap.task : 0;
	const uint32_t kind = c != NULL ? c->cap.depth_kind : 0;
	os_mutex_unlock(&f->mtx);
	if (ms > 0) {
		sleep_ms(ms);
	}
	if (fail) {
		leave_s(f);
		return XRT_ERROR_D3D11; // a device-lost-class error
	}
	out->written = 0;
	if (out->views.memory_kind == XRT_CONV_MEM_CPU && out->views.cpu_ptr != NULL) {
		memset(out->views.cpu_ptr, (int)(params->frame_id & 0xFF), (size_t)out->views.byte_size);
		out->written |= XRT_CONV_OUT_VIEWS;
		out->views_info.view_count = params->view_count;
		memcpy(out->views_info.eye_xyz, params->viewpoints_xyz, sizeof(out->views_info.eye_xyz));
	}
	if (out->depth.memory_kind == XRT_CONV_MEM_CPU) {
		fill_depth_ramp(&out->depth, 1.0f, 2.0f); // decoded 1..2 (metres when METRIC)
		out->written |= XRT_CONV_OUT_DEPTH;
		out->depth_info.kind = kind;
		out->depth_info.value_scale = 1.0f;
		out->depth_info.invalid_sample = -1.0f;
		out->depth_info.same_inference = task == XRT_CONV_TASK_VIEWS_MONO ? 1u : 0u;
		out->depth_info.source_time_ns = params->source_time_ns;
	}
	(void)in;
	leave_s(f);
	return XRT_SUCCESS;
}

static void
fake_stream_destroy(struct xrt_conversion_plugin_instance *inst, uint64_t stream)
{
	struct conv_fake_provider *f = fp(inst);
	(void)stream;
	enter_s(f);
	os_mutex_lock(&f->mtx);
	f->streams_destroyed++;
	os_mutex_unlock(&f->mtx);
	leave_s(f);
}

static xrt_result_t
fake_job(struct xrt_conversion_plugin_instance *inst,
         uint32_t cap_id,
         const struct xrt_conv_job_in *in,
         const struct xrt_conv_params *params,
         const struct xrt_conv_job_control *ctl,
         struct xrt_conv_job_out *out)
{
	struct conv_fake_provider *f = fp(inst);
	(void)in;
	(void)params;
	enter_s(f);
	os_mutex_lock(&f->mtx);
	struct conv_fake_cap *c = find_cap(f, cap_id);
	const uint32_t ms = c != NULL ? c->job_ms : 0;
	const uint64_t bytes = c != NULL ? c->job_bytes : 0;
	const uint32_t blob_format = c != NULL && c->job_blob_format != 0 ? c->job_blob_format : XRT_CONV_BLOB_SPZ;
	const uint32_t poll = f->cancel_poll_ms != 0 ? f->cancel_poll_ms : 10;
	f->jobs++;
	f->job_running = true;
	f->job_started_ns = os_monotonic_get_ns();
	f->job_cancel_seen_ns = 0;
	os_mutex_unlock(&f->mtx);

	xrt_result_t r = XRT_SUCCESS;
	// Wall-clock deadline, not an iteration count: a 10 ms sleep rounds up to the
	// OS timer tick (15.6 ms on Windows by default), so counting polls stretched a
	// 3 s job to ~4.7 s and timed out the lane-isolation test on a busy box.
	const uint64_t start_ns = os_monotonic_get_ns();
	const uint64_t span_ns = (uint64_t)ms * 1000000ull;
	for (uint64_t el = 0; el < span_ns; el = os_monotonic_get_ns() - start_ns) {
		if (ctl->is_cancelled(ctl->ctx)) {
			os_mutex_lock(&f->mtx);
			f->job_cancel_seen_ns = os_monotonic_get_ns();
			os_mutex_unlock(&f->mtx);
			r = XRT_TIMEOUT;
			break;
		}
		if (ctl->progress != NULL) {
			ctl->progress(ctl->ctx, (float)((double)el / (double)span_ns), "fake");
		}
		sleep_ms(poll);
	}
	if (r == XRT_SUCCESS && ctl->sink_count > 0 && bytes > 0) {
		struct xrt_conv_blob_sink *s = &ctl->sinks[0];
		uint8_t buf[4096];
		if (!s->begin(s->ctx, blob_format, bytes)) {
			r = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
		for (uint64_t off = 0; r == XRT_SUCCESS && off < bytes; off += sizeof(buf)) {
			const uint64_t n = bytes - off < sizeof(buf) ? bytes - off : sizeof(buf);
			for (uint64_t k = 0; k < n; k++) {
				buf[k] = (uint8_t)((off + k) * 7u + 3u);
			}
			if (!s->write(s->ctx, buf, n)) {
				r = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
			}
		}
		out->written = XRT_CONV_OUT_SPLAT_BLOB;
		out->splat_info.blob_format = blob_format;
		out->splat_info.splat_count = bytes / 32u;
	}
	os_mutex_lock(&f->mtx);
	f->job_running = false;
	os_mutex_unlock(&f->mtx);
	leave_s(f);
	return r;
}

static void
fake_destroy(struct xrt_conversion_plugin_instance *inst)
{
	struct conv_fake_provider *f = fp(inst);
	os_mutex_lock(&f->mtx);
	f->destroyed = true;
	os_mutex_unlock(&f->mtx);
}

void
conv_fake_init(struct conv_fake_provider *f, const char *id)
{
	memset(f, 0, sizeof(*f));
	strncpy(f->id, id, sizeof(f->id) - 1);
	os_mutex_init(&f->mtx);
	f->cancel_poll_ms = 10;
	f->generation = 1;
	struct xrt_conversion_plugin_iface *i = &f->iface;
	i->struct_size = (uint32_t)sizeof(*i);
	i->id = f->id;
	i->display_name = f->id;
	i->get_provider_status = fake_status;
	i->probe = fake_probe;
	i->describe_cap = fake_describe;
	i->get_generation = fake_generation;
	i->get_state = fake_get_state;
	i->warm = fake_warm;
	i->evict = fake_evict;
	i->stream_create = fake_stream_create;
	i->stream_get_output_desc = fake_output_desc;
	i->stream_convert = fake_stream_convert;
	i->stream_destroy = fake_stream_destroy;
	i->job_execute = fake_job;
	i->destroy = fake_destroy;
}

void
conv_fake_fini(struct conv_fake_provider *f)
{
	os_mutex_destroy(&f->mtx);
}

struct conv_fake_cap *
conv_fake_add_cap(
    struct conv_fake_provider *f, uint32_t cap_id, uint32_t task, uint32_t lanes, uint32_t outputs, uint32_t flags)
{
	if (f->cap_count >= CONV_FAKE_MAX_CAPS) {
		return NULL;
	}
	struct conv_fake_cap *c = &f->caps[f->cap_count++];
	memset(c, 0, sizeof(*c));
	struct xrt_conv_capability *k = &c->cap;
	k->struct_size = (uint32_t)sizeof(*k);
	k->cap_id = cap_id;
	k->task = task;
	k->lanes = lanes;
	k->input_kinds = XRT_CONV_INPUT_MONO;
	k->outputs = outputs;
	k->flags = flags;
	if (task == XRT_CONV_TASK_VIEWS_MONO || task == XRT_CONV_TASK_VIEWS_FROM_DEPTH) {
		k->flags |= XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;
	}
	k->import_memory_kinds = XRT_CONV_MEM_CPU;
	k->import_sync_kinds = 1u << XRT_CONV_SYNC_NONE;
	k->input_formats = 1u << XRT_CONV_FMT_RGBA8_UNORM;
	k->depth_output_format = XRT_CONV_FMT_R32_FLOAT;
	k->min_views = 2;
	k->max_views = 8;
	k->depth_kind = XRT_CONV_DEPTH_METRIC;
	k->licence_class = XRT_CONV_LICENCE_PERMISSIVE;
	k->commercial_ok = 1;
	strncpy(k->model_id, "fake", sizeof(k->model_id) - 1);
	c->state = XRT_CONV_STATE_INSTALLED;
	c->progress = -1.0f;
	return c;
}

void
conv_fake_set_state(struct conv_fake_provider *f, uint32_t cap_id, uint32_t state, uint32_t reason, bool bump)
{
	os_mutex_lock(&f->mtx);
	struct conv_fake_cap *c = find_cap(f, cap_id);
	if (c != NULL) {
		c->state = state;
		c->failure_reason = reason;
	}
	if (bump) {
		f->generation++;
	}
	os_mutex_unlock(&f->mtx);
}
