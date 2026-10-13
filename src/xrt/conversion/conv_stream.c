// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion stream lane: stream objects, one dispatcher per host.
 * @ingroup aux_conversion
 */

#include "conv_core_internal.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CONV_REG_NONE 0xFFFFFFFFu
#define CONV_RING_VIEWS 0u
#define CONV_RING_DEPTH 1u
#define CONV_RING_CONFIDENCE 2u
#define CONV_RING_DISPARITY 3u
//! Lane wait when nothing is plannable (READY transitions also wake it).
#define CONV_LANE_IDLE_WAIT_NS (50ull * 1000000ull)
//! Throttle of the per-stream INFO stats line (discovery spec §7.4: 10 s).
#define CONV_STATS_LOG_NS (10ull * 1000000000ull)

static uint64_t
now_ns(void)
{
	return os_monotonic_get_ns();
}

static struct conv_stream_lane *
lane_of(struct conv_core *core, const struct conv_stream *st)
{
	return &core->hosts[st->bind.provider].lane;
}

static void
signal_lane_locked(struct conv_core *core, uint32_t provider)
{
	struct conv_stream_lane *l = &core->hosts[provider].lane;
	if (l->started) {
		os_cond_broadcast(&l->cond);
	}
}

static struct conv_stream *
find_locked(struct conv_core *core, uint64_t id)
{
	for (struct conv_stream *st = core->streams; st != NULL; st = st->next) {
		if (st->id == id && !st->dead) {
			return st;
		}
	}
	return NULL;
}

static void
unlink_locked(struct conv_core *core, struct conv_stream *st)
{
	struct conv_stream **pp = &core->streams;
	while (*pp != NULL && *pp != st) {
		pp = &(*pp)->next;
	}
	if (*pp == st) {
		*pp = st->next;
	}
	st->next = NULL;
}

static bool
pinned(const struct conv_stream *st)
{
	for (uint32_t i = 0; i < U_LIFT_RING_SIZE; i++) {
		if (st->mb.out_pins[i] != 0) {
			return true;
		}
	}
	return false;
}

static void
free_image(struct conv_core *core, struct xrt_conv_image *img)
{
	if (img->memory_kind != 0 && core->cfg.alloc.free != NULL) {
		core->cfg.alloc.free(core->cfg.alloc.ud, img);
	}
	memset(img, 0, sizeof(*img));
}

//! The bound capability's import kinds (no lock held on entry).
static void
bound_import_kinds(struct conv_core *core, const struct conv_binding *b, uint32_t *mem, uint32_t *sync)
{
	*mem = 0;
	*sync = 0;
	os_mutex_lock(&core->registry_mtx);
	struct xrt_conv_capability cap;
	if (!b->composed && conv_core_row_cap_locked(core, b->provider, b->cap_id, &cap)) {
		*mem = cap.import_memory_kinds;
		*sync = cap.import_sync_kinds;
	}
	os_mutex_unlock(&core->registry_mtx);
}

static bool
alloc_image(struct conv_core *core,
            const struct conv_stream *st,
            uint32_t w,
            uint32_t h,
            uint32_t format,
            uint32_t layers,
            uint32_t role,
            uint32_t usage,
            struct xrt_conv_image *out)
{
	memset(out, 0, sizeof(*out));
	if (w == 0 || h == 0) {
		return false;
	}
	struct conv_image_request req;
	memset(&req, 0, sizeof(req));
	req.width = w;
	req.height = h;
	req.format = format;
	req.array_layers = layers > 0 ? layers : 1;
	req.role = role;
	req.usage = usage;
	req.stream_id = st->id;
	struct conv_binding b = st->bind;
	bound_import_kinds(core, &b, &req.import_memory_kinds, &req.import_sync_kinds);
	return core->cfg.alloc.alloc(core->cfg.alloc.ud, &req, out) && out->memory_kind != 0;
}


/*
 *
 * Teardown.
 *
 */

static void
unregister_in(struct conv_in_slot *s)
{
	if (s->reg_host != NULL) {
		for (uint32_t i = 0; i < CONV_STREAM_MAX_SUBMIT_IMAGES; i++) {
			if (s->reg_idx[i] != CONV_REG_NONE) {
				conv_host_unregister_image(s->reg_host, s->reg_idx[i]);
			}
			s->reg_idx[i] = CONV_REG_NONE;
		}
	}
	s->reg_host = NULL;
}

static void
unregister_out(struct conv_ring_slot *o)
{
	if (o->reg_host != NULL) {
		for (uint32_t i = 0; i < CONV_RING_TARGETS; i++) {
			if (o->reg_idx[i] != CONV_REG_NONE) {
				conv_host_unregister_image(o->reg_host, o->reg_idx[i]);
			}
			o->reg_idx[i] = CONV_REG_NONE;
		}
	}
	o->reg_host = NULL;
}

/*! Destroy the provider-side stream and drop host registrations. Dispatcher
 *  (or shutdown) thread, no lock held. */
static void
release_provider_side(struct conv_core *core, struct conv_stream *st)
{
	struct conv_host *host = st->pstream_host;
	if (st->pstream_created && host != NULL && conv_core_host(core, st->bind.provider) == host) {
		struct conv_host_cmd cmd;
		memset(&cmd, 0, sizeof(cmd));
		cmd.op = CONV_OP_STREAM_DESTROY;
		cmd.cap_id = st->bind.cap_id;
		cmd.stream = st->pstream;
		conv_host_call(host, &cmd);
	}
	st->pstream_created = false;
	st->pstream_failed = false;
	st->pstream_host = NULL;
	for (uint32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		unregister_in(&st->in[i]);
	}
	for (uint32_t i = 0; i < U_LIFT_RING_SIZE; i++) {
		unregister_out(&st->out[i]);
	}
}

static void
teardown(struct conv_core *core, struct conv_stream *st)
{
	release_provider_side(core, st);
	for (uint32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		for (uint32_t k = 0; k < CONV_STREAM_MAX_SUBMIT_IMAGES; k++) {
			free_image(core, &st->in[i].images[k]);
		}
	}
	for (uint32_t i = 0; i < U_LIFT_RING_SIZE; i++) {
		for (uint32_t k = 0; k < CONV_RING_TARGETS; k++) {
			free_image(core, &st->out[i].targets[k]);
		}
	}
	if (st->has_observer && st->observer.ops != NULL && st->observer.ops->destroy != NULL) {
		st->observer.ops->destroy(st->observer.ctx);
	}
	U_LOG_W("[conv] stream %llu destroyed: submitted=%llu converted=%llu dropped=%llu failed=%llu",
	        (unsigned long long)st->id, (unsigned long long)st->mb.submitted, (unsigned long long)st->mb.converted,
	        (unsigned long long)st->mb.dropped, (unsigned long long)st->mb.failed);
	free(st);
}


/*
 *
 * Rebinding.
 *
 */

static void
copy_provider_id_locked(struct conv_core *core, struct conv_stream *st)
{
	memset(st->stats.provider_id, 0, sizeof(st->stats.provider_id));
	if (st->bind.provider < core->reg.provider_count) {
		strncpy(st->stats.provider_id, core->reg.providers[st->bind.provider].id,
		        sizeof(st->stats.provider_id) - 1);
	}
	st->stats.cap_id = st->bind.cap_id;
	st->stats.composed = st->bind.composed;
}

/*! streams_mtx held on entry and exit; dropped around provider calls. */
static void
do_rebind(struct conv_core *core, struct conv_stream *st)
{
	const struct conv_binding from = st->bind;
	const struct conv_binding to = st->rebind;
	st->rebind_pending = false;
	st->converting = true; // keeps the stream ours while unlocked
	os_mutex_unlock(&core->streams_mtx);

	release_provider_side(core, st);

	struct conv_pipeline pipe;
	bool ok = false;
	os_mutex_lock(&core->registry_mtx);
	struct xrt_conv_capability cap;
	if (conv_core_row_cap_locked(core, to.provider, to.cap_id, &cap)) {
		const bool ready = conv_core_row_state_locked(core, to.provider, to.cap_id) == XRT_CONV_STATE_READY;
		ok = conv_pipeline_compose(&st->req, &cap, to.composed, ready, &pipe) == XRT_SUCCESS;
		conv_core_request_warm_locked(core, to.provider, to.cap_id, CONV_EV_CREATE_REQUESTED);
	}
	const char *from_id = from.provider < core->reg.provider_count ? core->reg.providers[from.provider].id : "?";
	const char *to_id = to.provider < core->reg.provider_count ? core->reg.providers[to.provider].id : "?";
	U_LOG_W("[conv] stream %llu rebound: %s cap %u -> %s cap %u%s (terminal failure; discontinuity)",
	        (unsigned long long)st->id, from_id, from.cap_id, to_id, to.cap_id, to.composed ? " composed" : "");
	os_mutex_unlock(&core->registry_mtx);

	os_mutex_lock(&core->streams_mtx);
	st->converting = false;
	if (ok) {
		st->bind = to;
		st->pipe = pipe;
		st->unsatisfied = false;
	} else {
		st->unsatisfied = true;
	}
	// The one rebinding rule: signalled by discontinuity, temporal state dropped.
	st->reset_pending = true;
	st->discontinuity_pending = true;
	st->has_last_depth = false;
	st->stats.rebinds++;
	os_mutex_lock(&core->registry_mtx);
	copy_provider_id_locked(core, st);
	os_mutex_unlock(&core->registry_mtx);
	signal_lane_locked(core, from.provider);
	signal_lane_locked(core, st->bind.provider);
}

void
conv_streams_rebind_check(struct conv_core *core)
{
	os_mutex_lock(&core->streams_mtx);
	for (struct conv_stream *st = core->streams; st != NULL; st = st->next) {
		if (st->dead || st->rebind_pending) {
			continue;
		}
		os_mutex_lock(&core->registry_mtx);
		const uint32_t state = conv_core_row_state_locked(core, st->bind.provider, st->bind.cap_id);
		const bool gone = core->hosts[st->bind.provider].hung;
		if (conv_state_is_terminal(state) || gone) {
			struct conv_binding nb;
			uint32_t ns = 0;
			const uint32_t r =
			    conv_core_select_locked(core, st->req.task, XRT_CONV_LANE_STREAM, st->req.input_kind,
			                            st->pipe.view_count, NULL, &nb, &ns);
			if (r <= CONV_SEL_OK_FALLTHROUGH &&
			    (nb.provider != st->bind.provider || nb.cap_id != st->bind.cap_id ||
			     nb.composed != st->bind.composed)) {
				st->rebind = nb;
				st->rebind_pending = true;
			}
		}
		os_mutex_unlock(&core->registry_mtx);
		if (st->rebind_pending) {
			signal_lane_locked(core, st->bind.provider);
		}
	}
	os_mutex_unlock(&core->streams_mtx);
}


/*
 *
 * One conversion.
 *
 */

static bool
ensure_pstream(struct conv_core *core, struct conv_stream *st, struct conv_host *host, uint32_t w, uint32_t h)
{
	if (st->pstream_created && st->pstream_host == host) {
		return true;
	}
	if (st->pstream_failed) {
		return false;
	}
	struct xrt_conv_stream_info info;
	memset(&info, 0, sizeof(info));
	info.struct_size = (uint32_t)sizeof(info);
	info.input_kind = st->req.input_kind != 0 ? st->req.input_kind : XRT_CONV_INPUT_MONO;
	info.outputs = st->pipe.provider_outputs;
	info.content_hint = st->content_hint;
	info.view_count = st->pipe.composed ? 0 : st->pipe.view_count;
	info.views_layout = st->pipe.views_layout;
	info.max_input_width = st->max_input_w != 0 ? st->max_input_w : w;
	info.max_input_height = st->max_input_h != 0 ? st->max_input_h : h;

	struct conv_host_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.op = CONV_OP_STREAM_CREATE;
	cmd.cap_id = st->bind.cap_id;
	cmd.stream_info = &info;
	const xrt_result_t r = conv_host_call(host, &cmd);
	if (r != XRT_SUCCESS) {
		st->pstream_failed = true;
		U_LOG_W("[conv] stream %llu: provider refused stream_create (cap %u, result %d)",
		        (unsigned long long)st->id, st->bind.cap_id, (int)r);
		return false;
	}
	st->pstream = cmd.stream;
	st->pstream_created = true;
	st->pstream_host = host;
	return true;
}

static bool
register_in(struct conv_stream *st, struct conv_in_slot *s, struct conv_host *host, struct xrt_conv_frame_in *fin)
{
	if (s->reg_host != host) {
		unregister_in(s);
		for (uint32_t i = 0; i < s->image_count; i++) {
			if (conv_host_register_image(host, &s->images[i], &s->reg_idx[i]) != XRT_SUCCESS) {
				unregister_in(s);
				return false;
			}
		}
		s->reg_host = host;
	}
	fin->image_count = s->image_count;
	for (uint32_t i = 0; i < s->image_count; i++) {
		fin->image_role[i] = s->role[i];
		if (!conv_host_lookup_image(host, s->reg_idx[i], &fin->images[i])) {
			return false;
		}
	}
	(void)st;
	return true;
}

/*! Size the ring slot's targets for a w x h input; (re)allocate + register. */
static bool
ensure_targets(struct conv_core *core,
               struct conv_stream *st,
               struct conv_ring_slot *o,
               struct conv_host *host,
               uint32_t w,
               uint32_t h)
{
	const struct conv_pipeline *p = &st->pipe;
	const uint32_t key = p->outputs | (p->provider_outputs << 8) | (p->view_count << 16) | (p->views_layout << 24);
	if (o->in_w == w && o->in_h == h && o->reg_host == host && o->sized_key == key) {
		return true;
	}
	unregister_out(o);
	for (uint32_t k = 0; k < CONV_RING_TARGETS; k++) {
		free_image(core, &o->targets[k]);
	}

	struct xrt_conv_output_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.struct_size = (uint32_t)sizeof(desc);
	const bool have_desc = conv_host_get_output_desc(host, st->pstream, w, h, &desc) == XRT_SUCCESS;

	const uint32_t n = p->view_count > 0 ? p->view_count : 1;
	const bool array = p->views_layout == XRT_CONV_VIEWS_ARRAY;
	bool ok = true;
	const uint32_t out_usage = CONV_IMAGE_USAGE_EXPORT;
	if ((p->outputs & XRT_CONV_OUT_VIEWS) != 0) {
		uint32_t vw = array ? w : n * w, vh = h, vf = XRT_CONV_FMT_RGBA8_UNORM;
		uint32_t usage = out_usage;
		if (p->composed) {
			usage |= CONV_IMAGE_USAGE_RENDER_TARGET;
		} else {
			usage |= CONV_IMAGE_USAGE_PROVIDER_OUT;
			if (have_desc && desc.views_width != 0) {
				vw = desc.views_width;
				vh = desc.views_height;
				vf = desc.views_format != 0 ? desc.views_format : vf;
			}
		}
		ok = ok && alloc_image(core, st, vw, vh, vf, array ? n : 1, XRT_CONV_OUT_VIEWS, usage,
		                       &o->targets[CONV_RING_VIEWS]);
	}
	if ((p->provider_outputs & XRT_CONV_OUT_DEPTH) != 0 || (p->outputs & XRT_CONV_OUT_DEPTH) != 0) {
		uint32_t dw = w, dh = h, df = XRT_CONV_FMT_R32_FLOAT;
		if (have_desc && desc.depth_width != 0) {
			dw = desc.depth_width;
			dh = desc.depth_height;
			df = desc.depth_format != 0 ? desc.depth_format : df;
		}
		ok = ok && alloc_image(core, st, dw, dh, df, 1, XRT_CONV_OUT_DEPTH,
		                       out_usage | CONV_IMAGE_USAGE_PROVIDER_OUT, &o->targets[CONV_RING_DEPTH]);
	}
	if ((p->provider_outputs & XRT_CONV_OUT_CONFIDENCE) != 0 && have_desc && desc.confidence_width != 0) {
		ok = ok && alloc_image(core, st, desc.confidence_width, desc.confidence_height, desc.confidence_format,
		                       1, XRT_CONV_OUT_CONFIDENCE, out_usage | CONV_IMAGE_USAGE_PROVIDER_OUT,
		                       &o->targets[CONV_RING_CONFIDENCE]);
	}
	if ((p->provider_outputs & XRT_CONV_OUT_DISPARITY) != 0) {
		uint32_t dw = w, dh = h, df = XRT_CONV_FMT_R32_FLOAT;
		if (have_desc && desc.disparity_width != 0) {
			dw = desc.disparity_width;
			dh = desc.disparity_height;
			df = desc.disparity_format != 0 ? desc.disparity_format : df;
		}
		ok = ok && alloc_image(core, st, dw, dh, df, 1, XRT_CONV_OUT_DISPARITY,
		                       out_usage | CONV_IMAGE_USAGE_PROVIDER_OUT, &o->targets[CONV_RING_DISPARITY]);
	}
	// Register what the PROVIDER writes (the composed views are runtime-rendered).
	for (uint32_t k = 0; ok && k < CONV_RING_TARGETS; k++) {
		o->reg_idx[k] = CONV_REG_NONE;
		if (o->targets[k].memory_kind == 0 || (k == CONV_RING_VIEWS && p->composed)) {
			continue;
		}
		ok = conv_host_register_image(host, &o->targets[k], &o->reg_idx[k]) == XRT_SUCCESS;
	}
	o->reg_host = host;
	if (!ok) {
		unregister_out(o);
		for (uint32_t k = 0; k < CONV_RING_TARGETS; k++) {
			free_image(core, &o->targets[k]);
		}
		o->in_w = o->in_h = 0;
		return false;
	}
	o->in_w = w;
	o->in_h = h;
	o->sized_key = key;
	o->alloc_gen++;
	return true;
}

static void
provider_targets(struct conv_ring_slot *o, struct conv_host *host, struct xrt_conv_frame_out *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	struct xrt_conv_image *dst[CONV_RING_TARGETS] = {&out->views, &out->depth, &out->confidence, &out->disparity};
	for (uint32_t k = 0; k < CONV_RING_TARGETS; k++) {
		if (o->reg_idx[k] != CONV_REG_NONE) {
			conv_host_lookup_image(host, o->reg_idx[k], dst[k]);
		}
	}
}

/*
 *
 * Still-image dedupe (#2030). A PHOTO stream's producer resubmits an unchanged
 * picture at page rate; converting it again buys nothing. When the input's
 * content and every S3 decision equal what the latest result was converted
 * from, that result is republished under the new frame id instead.
 *
 */

static uint64_t
mix64(uint64_t h, uint64_t v)
{
	h ^= v;
	h *= 0x9E3779B97F4A7C15ull;
	return h ^ (h >> 31);
}

//! The cheap content check for a CPU-memory input: every byte, 8 at a time.
static uint64_t
hash_cpu_image(uint64_t h, const struct xrt_conv_image *img)
{
	const uint8_t *p = (const uint8_t *)img->cpu_ptr;
	uint64_t n = img->byte_size;
	while (n >= 8) {
		uint64_t v;
		memcpy(&v, p, 8);
		h = mix64(h, v);
		p += 8;
		n -= 8;
	}
	uint64_t tail = 0;
	for (uint64_t i = 0; i < n; i++) {
		tail |= (uint64_t)p[i] << (8 * i);
	}
	return mix64(h, tail ^ (n << 56));
}

/*! The identity of a committed input's content: the producer's key, else the
 *  CPU pixels themselves. 0 = unknown (a GPU input without a key). */
static uint64_t
input_content_key(const struct conv_in_slot *in)
{
	const struct conv_frame_submit *fr = &in->frame;
	uint64_t h = mix64(0xCBF29CE484222325ull, fr->content_key);
	for (uint32_t i = 0; i < in->image_count; i++) {
		const struct xrt_conv_image *img = &in->images[i];
		h = mix64(h, ((uint64_t)img->width << 32) | img->height);
		h = mix64(h, ((uint64_t)img->format << 32) | in->role[i]);
		if (fr->content_key != 0) {
			continue;
		}
		if (img->memory_kind != XRT_CONV_MEM_CPU || img->cpu_ptr == NULL || img->byte_size == 0) {
			return 0;
		}
		h = hash_cpu_image(h, img);
	}
	return in->image_count == 0 ? 0 : (h != 0 ? h : 1);
}

static bool
near_eq(float a, float b)
{
	return fabsf(a - b) <= 1e-5f + 1e-5f * fabsf(b);
}

//! S3's decisions equal (frame id / time / flags excluded).
static bool
still_params_equal(const struct xrt_conv_params *a, const struct xrt_conv_params *b)
{
	if (a->convergence_mode != b->convergence_mode || a->view_count != b->view_count ||
	    a->content_hint != b->content_hint || a->viewpoint_frame != b->viewpoint_frame ||
	    a->viewpoint_source != b->viewpoint_source || a->axis_mode != b->axis_mode ||
	    a->viewpoint_count != b->viewpoint_count) {
		return false;
	}
	if (!near_eq(a->convergence, b->convergence) || !near_eq(a->strength, b->strength) ||
	    !near_eq(a->input_scale, b->input_scale) || !near_eq(a->focal_px, b->focal_px) ||
	    !near_eq(a->rect_width_m, b->rect_width_m) || !near_eq(a->rect_height_m, b->rect_height_m) ||
	    !near_eq(a->baseline_m, b->baseline_m) || !near_eq(a->max_offset_m, b->max_offset_m) ||
	    !near_eq(a->nominal_z_m, b->nominal_z_m)) {
		return false;
	}
	const uint32_t n = a->viewpoint_count < XRT_CONV_MAX_VIEWS ? a->viewpoint_count : XRT_CONV_MAX_VIEWS;
	for (uint32_t i = 0; i < n; i++) {
		for (uint32_t k = 0; k < 3; k++) {
			if (!near_eq(a->viewpoints_xyz[i][k], b->viewpoints_xyz[i][k])) {
				return false;
			}
		}
	}
	return memcmp(&a->stereo, &b->stereo, sizeof(a->stereo)) == 0;
}

static void
still_params_key(const struct xrt_conv_params *in, struct xrt_conv_params *out)
{
	memcpy(out, in, sizeof(*out));
	out->frame_flags = 0;
	out->source_time_ns = 0;
	out->frame_id = 0;
}

/*! streams_mtx held. Republish the latest result under @p meta's frame id
 *  (the new input's): consumers see a newer frame, the provider sees nothing. */
static void
republish_latest_locked(struct conv_stream *st,
                        const struct u_lift_frame_meta *meta,
                        const struct conv_frame_submit *fr)
{
	const int32_t l = st->mb.latest;
	const uint64_t t = now_ns();
	struct u_lift_frame_meta *m = &st->mb.out_meta[l];
	*m = *meta;
	m->done_ns = t;
	st->mb.last_publish_ns = t;
	// The echoes follow the new frame (a moved rect with the same picture).
	struct conv_ring_slot *o = &st->out[l];
	memcpy(o->active, fr->active, sizeof(o->active));
	if (fr->rect_valid) {
		memcpy(o->rect_center, fr->rect_center, sizeof(o->rect_center));
		o->rect_size[0] = fr->rect_w_m;
		o->rect_size[1] = fr->rect_h_m;
	}
}

struct convert_snapshot
{
	struct conv_binding bind;
	struct conv_pipeline pipe;
	struct conv_frame_submit frame;
	bool reset;
	bool discont;
	bool validated;
	uint32_t cap_flags;
	bool legacy_units; //!< CONV_PROV_LEGACY_UNITS (the dp-lift adapter)
	uint64_t eyes_dp;
	int32_t latest_slot; //!< the published result (dedupe); -1 = none
};

/*! streams_mtx held on entry and exit. */
static void
convert_one(struct conv_stream_lane *lane, struct conv_stream *st)
{
	struct conv_core *core = lane->core;
	int32_t in_slot = -1;
	struct u_lift_frame_meta meta;
	memset(&meta, 0, sizeof(meta));
	if (!u_lift_mailbox_take_pending(&st->mb, now_ns(), &in_slot, &meta)) {
		return;
	}
	st->converting = true;

	// Wait while a consumer pins the only writable ring slot (one copy long).
	int32_t out_slot = -1;
	bool ok = true;
	while (!u_lift_mailbox_begin_output(&st->mb, &out_slot)) {
		if (st->dead || lane->stop) {
			ok = false;
			break;
		}
		os_cond_wait_timeout_ns(&lane->cond, &core->streams_mtx, 5ull * 1000000ull);
	}

	struct convert_snapshot s;
	memset(&s, 0, sizeof(s));
	s.bind = st->bind;
	s.pipe = st->pipe;
	s.frame = st->in[in_slot].frame;
	s.reset = st->reset_pending;
	s.discont = st->discontinuity_pending;
	s.eyes_dp = st->opts.eyes_dp;
	s.latest_slot = -1;
	if (st->mb.latest >= 0 && st->mb.latest < (int32_t)U_LIFT_RING_SIZE &&
	    st->mb.out_state[st->mb.latest] == U_LIFT_OUT_READY) {
		s.latest_slot = st->mb.latest;
	}
	st->reset_pending = false;
	st->discontinuity_pending = false;
	os_mutex_lock(&core->registry_mtx);
	struct xrt_conv_capability cap;
	if (conv_core_row_cap_locked(core, s.bind.provider, s.bind.cap_id, &cap)) {
		s.cap_flags = cap.flags;
	}
	s.validated = conv_core_row_validated_locked(core, s.bind.provider, s.bind.cap_id);
	s.legacy_units = s.bind.provider < core->reg.provider_count &&
	                 (core->reg.providers[s.bind.provider].flags & CONV_PROV_LEGACY_UNITS) != 0;
	os_mutex_unlock(&core->registry_mtx);
	os_mutex_unlock(&core->streams_mtx);

	const uint64_t t0 = now_ns();
	struct conv_host *host = conv_core_host(core, s.bind.provider);
	struct conv_pipeline_frame *pf = (struct conv_pipeline_frame *)calloc(1, sizeof(*pf));
	xrt_result_t pr = XRT_SUCCESS;
	ok = ok && host != NULL && pf != NULL;
	ok = ok && ensure_pstream(core, st, host, meta.width, meta.height);
	ok = ok && register_in(st, &st->in[in_slot], host, &pf->in);
	struct conv_ring_slot *o = out_slot >= 0 ? &st->out[out_slot] : NULL;
	ok = ok && o != NULL && ensure_targets(core, st, o, host, meta.width, meta.height);

	// Dedupe applies to PHOTO only, and never across a reset / rebind (which
	// still record what they converted, for the frames after them).
	const bool still = ok && st->content_hint == XRT_CONV_CONTENT_PHOTO;
	const uint64_t still_key = still ? input_content_key(&st->in[in_slot]) : 0;
	bool dedupe = false;
	struct xrt_conv_params still_params;
	memset(&still_params, 0, sizeof(still_params));

	if (ok) {
		// S3: policy (runtime-owned; the provider only receives its results).
		struct xrt_conv_params *pa = &pf->params;
		pa->struct_size = (uint32_t)sizeof(*pa);
		pa->frame_flags = (s.frame.frame_flags & ~XRT_CONV_FRAME_RESET) | (s.reset ? XRT_CONV_FRAME_RESET : 0u);
		pa->source_time_ns = (uint64_t)s.frame.source_time;
		pa->frame_id = meta.frame_id;
		pa->view_count = s.pipe.view_count;
		pa->content_hint = st->content_hint;
		pa->input_scale = 1.0f;
		pa->focal_px = s.frame.focal_px > 0.0f ? s.frame.focal_px : 0.0f;
		pa->strength = 1.0f;
		pa->stereo = s.frame.stereo;
		pf->rear_cap_m = core->cfg.rear_cap_m;
		if (s.reset) {
			conv_policy_observer_reset(st->has_observer ? &st->observer : NULL);
		}
		if ((s.pipe.stages & CONV_STAGE_S3_POLICY) != 0) {
			struct conv_eyes eyes;
			conv_core_eyes_read(core, s.eyes_dp, t0, &eyes);
			struct conv_policy_view_in vin;
			memset(&vin, 0, sizeof(vin));
			vin.view_count = s.pipe.view_count;
			vin.explicit_count = s.frame.explicit_count;
			memcpy(vin.explicit_xyz, s.frame.explicit_xyz, sizeof(vin.explicit_xyz));
			if (s.frame.has_view_control) {
				vin.vc = s.frame.vc;
			} else {
				u_lift_view_control_default(&vin.vc);
			}
			vin.rig = s.frame.rig;
			vin.recenter_off = core->cfg.recenter_off;
			vin.rect_valid = s.frame.rect_valid;
			memcpy(vin.rect_center, s.frame.rect_center, sizeof(vin.rect_center));
			vin.rect_w_m = s.frame.rect_w_m;
			vin.rect_h_m = s.frame.rect_h_m;
			vin.nominal_z_m = core->cfg.nominal_z_m;
			vin.eyes = &eyes;
			vin.now_ns = t0;
			conv_policy_viewpoints(&vin, &st->vstate, pa, NULL);

			float obs = 0.0f;
			const bool obs_valid = st->has_observer && st->observer.ops->value != NULL &&
			                       st->observer.ops->value(st->observer.ctx, t0, &obs);
			struct conv_policy_conv_in cin;
			memset(&cin, 0, sizeof(cin));
			cin.app_set = s.frame.convergence_set;
			cin.app_rel01 = s.frame.convergence_rel01;
			cin.observer_valid = obs_valid;
			cin.observer_rel01 = obs;
			cin.hint_allowed = s.validated && (s.cap_flags & XRT_CONV_CAP_SUGGESTS_CONVERGENCE) != 0;
			cin.last = st->has_last_depth ? &st->last_depth : NULL;
			conv_policy_convergence(&cin, pa, NULL);
			if (s.legacy_units) {
				// The legacy contract's RELATIVE plane, straight through: the
				// app's, else the observer's, else AUTO (the module's own).
				if (cin.app_set) {
					pa->convergence_mode = CONV_CONVERGENCE_MODE_LEGACY_RELATIVE;
					pa->convergence =
					    cin.app_rel01 < 0.0f ? 0.0f : (cin.app_rel01 > 1.0f ? 1.0f : cin.app_rel01);
				} else if (obs_valid) {
					pa->convergence_mode = CONV_CONVERGENCE_MODE_LEGACY_RELATIVE;
					pa->convergence = obs < 0.0f ? 0.0f : (obs > 1.0f ? 1.0f : obs);
				} else {
					pa->convergence_mode = XRT_CONV_CONVERGENCE_AUTO;
					pa->convergence = 0.0f;
				}
			}

			struct conv_policy_budget_in bin;
			memset(&bin, 0, sizeof(bin));
			bin.depth_kind = s.pipe.depth_kind;
			bin.last = cin.last;
			bin.strength = s.frame.strength;
			bin.default_budget = core->cfg.default_budget;
			bin.rear_cap_m = core->cfg.rear_cap_m;
			bin.viewer_distance_m = pa->nominal_z_m;
			bin.display_width_m = pa->rect_width_m;
			bin.corpus_validated = s.validated;
			struct conv_policy_budget_out bout;
			conv_policy_budget(&bin, &bout);
			pa->strength = bout.strength;
			pf->budget = bout.budget;
			pf->rear_cap_m = bout.rear_cap_m;
			if (s.legacy_units) {
				// "1 = the module's calibrated budget": the app's strength as is.
				pa->strength = s.frame.strength > 0.0f ? s.frame.strength : 1.0f;
			}
		}

		still_params_key(pa, &still_params);
		dedupe = still_key != 0 && !s.reset && !s.discont && st->still.valid && st->still.content_key == still_key &&
		         s.latest_slot >= 0 && s.latest_slot == st->still.out_slot &&
		         st->still.bind.provider == s.bind.provider && st->still.bind.cap_id == s.bind.cap_id &&
		         st->still.bind.composed == s.bind.composed && st->still.outputs == s.pipe.outputs &&
		         st->still.provisional == s.pipe.provisional &&
		         still_params_equal(&still_params, &st->still.params) &&
		         near_eq(pf->budget, st->still.budget) && near_eq(pf->rear_cap_m, st->still.rear_cap_m);
	}

	if (ok && !dedupe) {
		pf->host = host;
		pf->cap_id = s.bind.cap_id;
		pf->provider_stream = st->pstream;
		pf->in.depth_in_info = s.frame.depth_in_info;
		provider_targets(o, host, &pf->provider_out);
		pf->views_target = o->targets[CONV_RING_VIEWS];
		pf->new_content = true;
		const xrt_result_t r = conv_pipeline_execute(&s.pipe, &core->cfg.viewsyn, &core->cfg.readback, pf);
		pr = pf->provider_result;
		ok = r == XRT_SUCCESS;

		// S7: observers, on the same inference only.
		if (ok && (s.pipe.stages & CONV_STAGE_S7_OBSERVERS) != 0 && st->has_observer) {
			conv_policy_observe(&st->observer, &pf->result.depth_info, &pf->samples, s.frame.active, t0);
		}
		conv_pipeline_frame_release(&core->cfg.readback, pf);
		// The provider's depth, published or observer-only (#2003).
		if (ok && (pf->provider_out.written & XRT_CONV_OUT_DEPTH) != 0) {
			st->last_depth = pf->result.depth_info;
			st->has_last_depth = true;
		}
	}

	// S8: publish.
	os_mutex_lock(&core->streams_mtx);
	u_lift_mailbox_finish_input(&st->mb, in_slot);
	if (pf != NULL) {
		if ((pf->blocked & CONV_CMD_BLOCKED_BY_JOB) != 0) {
			st->stats.blocked_by_job++;
		}
		if ((pf->blocked & CONV_CMD_BLOCKED_BY_WARM) != 0) {
			st->stats.blocked_by_warm++;
		}
	}
	if (out_slot >= 0) {
		if (dedupe) {
			u_lift_mailbox_abort_output(&st->mb, out_slot);
			st->mb.failed--; // the slot was never written: not a failed conversion
			republish_latest_locked(st, &meta, &s.frame);
			st->stats.deduped++;
		} else if (ok) {
			o->written = pf->result.written;
			o->views_info = pf->result.views_info;
			o->depth_info = pf->result.depth_info;
			memcpy(o->active, s.frame.active, sizeof(o->active));
			if (s.frame.rect_valid) {
				memcpy(o->rect_center, s.frame.rect_center, sizeof(o->rect_center));
				o->rect_size[0] = s.frame.rect_w_m;
				o->rect_size[1] = s.frame.rect_h_m;
			} else {
				memset(o->rect_center, 0, sizeof(o->rect_center));
				memset(o->rect_size, 0, sizeof(o->rect_size));
			}
			o->discontinuity = s.discont;
			o->provisional = s.pipe.provisional;
			u_lift_mailbox_publish_output(&st->mb, out_slot, &meta, now_ns());
			st->still.valid = still_key != 0;
			if (st->still.valid) {
				st->still.content_key = still_key;
				st->still.out_slot = out_slot;
				st->still.bind = s.bind;
				st->still.outputs = s.pipe.outputs;
				st->still.provisional = s.pipe.provisional;
				st->still.params = still_params;
				st->still.budget = pf->budget;
				st->still.rear_cap_m = pf->rear_cap_m;
			}
		} else {
			u_lift_mailbox_abort_output(&st->mb, out_slot);
		}
	} else if (!ok) {
		st->mb.failed++;
	}
	if (!ok) {
		st->still.valid = false;
		// A failed conversion keeps the previous result and owes the flags.
		st->reset_pending = st->reset_pending || s.reset;
		st->discontinuity_pending = st->discontinuity_pending || s.discont;
	}
	st->converting = false;
	const uint64_t t = now_ns();
	if (t - st->last_stats_log_ns > CONV_STATS_LOG_NS) {
		st->last_stats_log_ns = t;
		U_LOG_I(
		    "[conv] stream %llu: submitted=%llu converted=%llu deduped=%llu dropped=%llu failed=%llu "
		    "blocked_by_job=%llu blocked_by_warm=%llu latency ema=%.1fms",
		    (unsigned long long)st->id, (unsigned long long)st->mb.submitted,
		    (unsigned long long)st->mb.converted, (unsigned long long)st->stats.deduped,
		    (unsigned long long)st->mb.dropped, (unsigned long long)st->mb.failed,
		    (unsigned long long)st->stats.blocked_by_job,
		    (unsigned long long)st->stats.blocked_by_warm, st->mb.lat_ema_ns / 1e6);
	}
	os_cond_broadcast(&lane->cond);
	free(pf);

	if (!ok && pr != XRT_SUCCESS && host != NULL) {
		os_mutex_unlock(&core->streams_mtx);
		conv_core_call_failed(core, s.bind.provider, s.bind.cap_id, pr);
		os_mutex_lock(&core->streams_mtx);
	}
}


/*
 *
 * Dispatcher.
 *
 */

//! Drop every pending frame (unsatisfied streams can never convert).
static void
drain_pending_locked(struct conv_stream *st)
{
	int32_t slot = -1;
	while (u_lift_mailbox_take_pending(&st->mb, now_ns(), &slot, NULL)) {
		u_lift_mailbox_finish_input(&st->mb, slot);
		st->mb.failed++;
	}
}

/*! Streams_mtx held. Returns true when the stream may convert now. */
static bool
gate_locked(struct conv_core *core, struct conv_stream *st)
{
	os_mutex_lock(&core->registry_mtx);
	const uint32_t state = conv_core_row_state_locked(core, st->bind.provider, st->bind.cap_id);
	st->stats.row_state = state;
	bool ready = state == XRT_CONV_STATE_READY;
	// Refine the provisional pipeline against the READY capability. An
	// unsatisfied stream is re-tried on every gate pass: a provider's describe
	// is refined asynchronously (the control thread re-describes when the
	// host's generation moves), so the first READY pass may still see the
	// pre-READY capability and the row can gain the output a moment later.
	// Never latch for the stream's life (#2025, lifted-rect cursor).
	if (ready && (st->pipe.provisional || st->unsatisfied)) {
		struct xrt_conv_capability cap;
		if (conv_core_row_cap_locked(core, st->bind.provider, st->bind.cap_id, &cap)) {
			const bool was_unsatisfied = st->unsatisfied;
			if (conv_pipeline_refine(&st->pipe, st->req.required_outputs, &cap)) {
				st->unsatisfied = false;
				if (was_unsatisfied) {
					U_LOG_W("[conv] stream %llu: capability now declares every required output (0x%x): "
					        "converting",
					        (unsigned long long)st->id, st->req.required_outputs);
				}
			} else {
				st->unsatisfied = true;
				if (!was_unsatisfied) {
					U_LOG_W("[conv] stream %llu: capability READY but lacks a required output (0x%x of "
					        "0x%x): frames are dropped until the capability is refined",
					        (unsigned long long)st->id, st->req.required_outputs & ~cap.outputs,
					        st->req.required_outputs);
				}
			}
		}
	}
	os_mutex_unlock(&core->registry_mtx);
	if (st->unsatisfied) {
		drain_pending_locked(st);
		return false;
	}
	return ready;
}

static void *
lane_main(void *ptr)
{
	struct conv_stream_lane *lane = (struct conv_stream_lane *)ptr;
	struct conv_core *core = lane->core;
	struct u_lift_sched_entry entries[64];
	uint64_t plan[64];

	os_mutex_lock(&core->streams_mtx);
	while (!lane->stop) {
		// Reap dead streams and move rebinding ones (provider calls unlocked).
		bool again = false;
		for (struct conv_stream *st = core->streams; st != NULL; st = st->next) {
			if (st->bind.provider != lane->provider || st->converting) {
				continue;
			}
			if (st->dead && !pinned(st)) {
				unlink_locked(core, st);
				os_mutex_unlock(&core->streams_mtx);
				teardown(core, st);
				os_mutex_lock(&core->streams_mtx);
				again = true;
				break;
			}
			if (!st->dead && st->rebind_pending) {
				do_rebind(core, st);
				again = true;
				break;
			}
		}
		if (again) {
			continue;
		}

		uint32_t count = 0;
		for (struct conv_stream *st = core->streams; st != NULL && count < 64; st = st->next) {
			if (st->bind.provider != lane->provider || st->dead || st->rebind_pending) {
				continue;
			}
			if (!gate_locked(core, st)) {
				continue; // frames wait, latest-wins, until READY
			}
			// The planner wants ascending ids; the list is newest-first.
			uint32_t pos = count;
			while (pos > 0 && entries[pos - 1].id > st->id) {
				entries[pos] = entries[pos - 1];
				pos--;
			}
			entries[pos].id = st->id;
			entries[pos].priority = st->priority;
			entries[pos].pending = u_lift_mailbox_has_pending(&st->mb);
			count++;
		}
		const uint32_t n = u_lift_sched_plan(&lane->sched, entries, count, plan, 64);
		if (n == 0) {
			os_cond_wait_timeout_ns(&lane->cond, &core->streams_mtx, CONV_LANE_IDLE_WAIT_NS);
			continue;
		}
		for (uint32_t i = 0; i < n && !lane->stop; i++) {
			struct conv_stream *st = find_locked(core, plan[i]);
			if (st == NULL || st->bind.provider != lane->provider || st->rebind_pending) {
				continue;
			}
			convert_one(lane, st);
		}
	}
	os_mutex_unlock(&core->streams_mtx);
	return NULL;
}

xrt_result_t
conv_stream_lane_start(struct conv_core *core, uint32_t provider)
{
	struct conv_stream_lane *lane = &core->hosts[provider].lane;
	os_mutex_lock(&core->streams_mtx);
	if (lane->started) {
		os_mutex_unlock(&core->streams_mtx);
		return XRT_SUCCESS;
	}
	lane->core = core;
	lane->provider = provider;
	lane->stop = false;
	u_lift_sched_init(&lane->sched);
	if (os_cond_init(&lane->cond) != 0 || os_thread_init(&lane->thread) != 0) {
		os_mutex_unlock(&core->streams_mtx);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	if (os_thread_start(&lane->thread, lane_main, lane) != 0) {
		os_cond_destroy(&lane->cond);
		os_mutex_unlock(&core->streams_mtx);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	lane->started = true;
	os_mutex_unlock(&core->streams_mtx);
	return XRT_SUCCESS;
}

void
conv_stream_lane_stop(struct conv_core *core, uint32_t provider)
{
	struct conv_stream_lane *lane = &core->hosts[provider].lane;
	os_mutex_lock(&core->streams_mtx);
	if (!lane->started) {
		os_mutex_unlock(&core->streams_mtx);
		return;
	}
	lane->stop = true;
	os_cond_broadcast(&lane->cond);
	os_mutex_unlock(&core->streams_mtx);
	os_thread_join(&lane->thread);
	os_thread_destroy(&lane->thread);
	os_mutex_lock(&core->streams_mtx);
	lane->started = false;
	os_cond_destroy(&lane->cond);
	os_mutex_unlock(&core->streams_mtx);
}

void
conv_streams_wake(struct conv_core *core)
{
	os_mutex_lock(&core->streams_mtx);
	for (uint32_t p = 0; p < CONV_REGISTRY_MAX_PROVIDERS; p++) {
		signal_lane_locked(core, p);
	}
	os_mutex_unlock(&core->streams_mtx);
}

uint32_t
conv_streams_bound_count(struct conv_core *core, uint32_t provider, uint32_t cap_id, bool any_cap)
{
	uint32_t n = 0;
	os_mutex_lock(&core->streams_mtx);
	for (struct conv_stream *st = core->streams; st != NULL; st = st->next) {
		if (!st->dead && st->bind.provider == provider && (any_cap || st->bind.cap_id == cap_id)) {
			n++;
		}
	}
	os_mutex_unlock(&core->streams_mtx);
	return n;
}

void
conv_streams_destroy_all(struct conv_core *core)
{
	os_mutex_lock(&core->streams_mtx);
	struct conv_stream *list = core->streams;
	core->streams = NULL;
	os_mutex_unlock(&core->streams_mtx);
	while (list != NULL) {
		struct conv_stream *next = list->next;
		if (!core->hosts[list->bind.provider].hung) {
			teardown(core, list);
		}
		list = next;
	}
}

void
conv_streams_client_gone(struct conv_core *core, uint64_t client_id)
{
	os_mutex_lock(&core->streams_mtx);
	for (struct conv_stream *st = core->streams; st != NULL; st = st->next) {
		if (st->client_id == client_id && !st->dead) {
			st->dead = true;
			signal_lane_locked(core, st->bind.provider);
		}
	}
	os_mutex_unlock(&core->streams_mtx);
}


/*
 *
 * Public stream API.
 *
 */

xrt_result_t
conv_core_stream_create(struct conv_core *core,
                        uint64_t client_id,
                        const struct xrt_conv_wire_stream_desc *desc,
                        const struct conv_stream_options *opts,
                        struct xrt_conv_wire_stream_created *out)
{
	if (desc == NULL || out == NULL || !conv_task_is_served(desc->task)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct conv_stream_options o;
	memset(&o, 0, sizeof(o));
	o.eyes_dp = CONV_EYES_DP_ANY;
	if (opts != NULL) {
		o = *opts;
	}
	struct conv_pipeline_request req;
	memset(&req, 0, sizeof(req));
	req.task = desc->task;
	req.input_kind = desc->input_kind != 0 ? desc->input_kind : XRT_CONV_INPUT_MONO;
	req.required_outputs = desc->outputs;
	req.view_count = conv_task_produces_views(desc->task) ? (desc->view_count != 0 ? desc->view_count : 2u) : 0u;
	req.views_layout = desc->views_layout;
	req.source_format = o.source_format != 0 ? o.source_format : XRT_CONV_FMT_RGBA8_UNORM;
	req.weave_rect_source = o.weave_rect_source;
	req.observers = o.auto_convergence && core->cfg.create_convergence_observer != NULL;

	struct conv_binding b;
	struct conv_pipeline pipe;
	uint32_t max_streams = 4;
	os_mutex_lock(&core->registry_mtx);
	uint32_t state = 0;
	const uint32_t sel = conv_core_select_locked(core, req.task, XRT_CONV_LANE_STREAM, req.input_kind,
	                                             req.view_count, NULL, &b, &state);
	if (sel > CONV_SEL_OK_FALLTHROUGH) {
		if (sel == CONV_SEL_NONE_ALL_TERMINAL) {
			// The "create for an unserved task" retry trigger (FAILED > 30 s ago).
			for (uint32_t i = 0; i < core->reg.row_count; i++) {
				const struct conv_cap_row *row = &core->reg.rows[i];
				if (row->cap.task == req.task && row->rd.state == XRT_CONV_STATE_FAILED) {
					conv_core_request_warm_locked(core, row->provider, row->cap.cap_id,
					                              CONV_EV_CREATE_REQUESTED);
				}
			}
		}
		os_mutex_unlock(&core->registry_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_conv_capability cap;
	conv_core_row_cap_locked(core, b.provider, b.cap_id, &cap);
	const xrt_result_t cr = conv_pipeline_compose(&req, &cap, b.composed, state == XRT_CONV_STATE_READY, &pipe);
	if (cr != XRT_SUCCESS) {
		os_mutex_unlock(&core->registry_mtx);
		return cr;
	}
	if (cap.max_streams != 0) {
		max_streams = cap.max_streams;
	}
	if (sel == CONV_SEL_OK_FALLTHROUGH) {
		U_LOG_W("[conv] stream create for %s fell through to %s cap %u", conv_task_name(req.task),
		        core->reg.providers[b.provider].id, b.cap_id);
	}
	if (!core->first_create_seen) {
		core->first_create_seen = true;
		if (core->cfg.prewarm_on == CONV_PREWARM_FIRST_CREATE) {
			const uint64_t m =
			    core->cfg.prewarm_task_mask != 0 ? core->cfg.prewarm_task_mask : CONV_PREWARM_DEFAULT_MASK;
			for (uint32_t task = XRT_CONV_TASK_DEPTH_MONO; task < XRT_CONV_TASK_RESERVED_FIRST; task++) {
				struct conv_binding pb;
				if ((m & (1ull << task)) != 0 &&
				    conv_core_select_locked(core, task, 0, 0, conv_task_produces_views(task) ? 2u : 0u,
				                            NULL, &pb, NULL) <= CONV_SEL_OK_FALLTHROUGH) {
					conv_core_request_warm_locked(core, pb.provider, pb.cap_id,
					                              CONV_EV_WARM_REQUESTED);
				}
			}
		}
	}
	conv_core_request_warm_locked(core, b.provider, b.cap_id, CONV_EV_CREATE_REQUESTED);
	os_mutex_unlock(&core->registry_mtx);

	struct conv_stream *st = (struct conv_stream *)calloc(1, sizeof(*st));
	if (st == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	st->client_id = client_id;
	st->priority = o.priority != 0 ? o.priority : U_LIFT_PRIORITY_NORMAL;
	st->quality_hint = desc->quality_hint;
	st->content_hint = desc->content_hint;
	st->max_input_w = desc->max_input_width;
	st->max_input_h = desc->max_input_height;
	st->opts = o;
	st->req = req;
	st->pipe = pipe;
	st->bind = b;
	u_lift_mailbox_init(&st->mb);
	for (uint32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		for (uint32_t k = 0; k < CONV_STREAM_MAX_SUBMIT_IMAGES; k++) {
			st->in[i].reg_idx[k] = CONV_REG_NONE;
		}
	}
	for (uint32_t i = 0; i < U_LIFT_RING_SIZE; i++) {
		for (uint32_t k = 0; k < CONV_RING_TARGETS; k++) {
			st->out[i].reg_idx[k] = CONV_REG_NONE;
		}
	}
	if (req.observers) {
		st->has_observer = core->cfg.create_convergence_observer(core->cfg.observer_ud, &st->observer) &&
		                   st->observer.ops != NULL;
		if (st->has_observer) {
			conv_policy_observer_reset(&st->observer);
		}
	}

	os_mutex_lock(&core->streams_mtx);
	uint32_t bound = 0;
	for (struct conv_stream *it = core->streams; it != NULL; it = it->next) {
		if (!it->dead && it->bind.provider == b.provider && it->bind.cap_id == b.cap_id) {
			bound++;
		}
	}
	if (bound >= max_streams) {
		os_mutex_unlock(&core->streams_mtx);
		if (st->has_observer && st->observer.ops->destroy != NULL) {
			st->observer.ops->destroy(st->observer.ctx);
		}
		free(st);
		return XRT_ERROR_CLIENT_LIMIT_REACHED;
	}
	st->id = core->next_stream_id++;
	st->next = core->streams;
	core->streams = st;
	os_mutex_lock(&core->registry_mtx);
	copy_provider_id_locked(core, st);
	os_mutex_unlock(&core->registry_mtx);
	st->stats.provisional = pipe.provisional;
	signal_lane_locked(core, b.provider);
	os_mutex_unlock(&core->streams_mtx);

	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	out->stream_id = st->id;
	out->task = req.task;
	out->outputs = pipe.outputs;
	out->view_count = pipe.view_count;
	out->views_layout = pipe.views_layout;
	out->desc_flags = b.composed ? XRT_CONV_WIRE_DESC_COMPOSED : 0u;
	U_LOG_W("[conv] stream %llu created: task=%s provider=%s cap=%u%s views=%u outputs=0x%x%s",
	        (unsigned long long)st->id, conv_task_name(req.task), st->stats.provider_id, b.cap_id,
	        b.composed ? " (composed: depth + reference view synthesis)" : "", pipe.view_count, pipe.outputs,
	        pipe.provisional ? " (provisional until READY)" : "");
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_stream_destroy(struct conv_core *core, uint64_t stream_id)
{
	os_mutex_lock(&core->streams_mtx);
	struct conv_stream *st = find_locked(core, stream_id);
	if (st == NULL) {
		os_mutex_unlock(&core->streams_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	st->dead = true;
	signal_lane_locked(core, st->bind.provider);
	os_mutex_unlock(&core->streams_mtx);
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_stream_reset(struct conv_core *core, uint64_t stream_id)
{
	os_mutex_lock(&core->streams_mtx);
	struct conv_stream *st = find_locked(core, stream_id);
	if (st != NULL) {
		st->reset_pending = true;
		st->discontinuity_pending = true;
	}
	os_mutex_unlock(&core->streams_mtx);
	return st != NULL ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t
conv_core_stream_set_priority(struct conv_core *core, uint64_t stream_id, uint32_t priority)
{
	if (priority > U_LIFT_PRIORITY_HIGH) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&core->streams_mtx);
	struct conv_stream *st = find_locked(core, stream_id);
	if (st != NULL) {
		st->priority = priority;
		signal_lane_locked(core, st->bind.provider);
	}
	os_mutex_unlock(&core->streams_mtx);
	return st != NULL ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t
conv_core_stream_begin_submit(struct conv_core *core,
                              uint64_t stream_id,
                              const struct conv_submit_layout *layout,
                              struct conv_submit_slot *out)
{
	if (layout == NULL || out == NULL || layout->image_count == 0 ||
	    layout->image_count > CONV_STREAM_MAX_SUBMIT_IMAGES) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&core->streams_mtx);
	struct conv_stream *st = find_locked(core, stream_id);
	int32_t slot = -1;
	if (st == NULL || !u_lift_mailbox_begin_submit(&st->mb, &slot)) {
		os_mutex_unlock(&core->streams_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_unlock(&core->streams_mtx);

	// The slot is WRITING: the producer owns it. Allocation happens unlocked.
	struct conv_in_slot *s = &st->in[slot];
	bool same = s->image_count == layout->image_count;
	for (uint32_t i = 0; same && i < layout->image_count; i++) {
		same = s->images[i].width == layout->width[i] && s->images[i].height == layout->height[i] &&
		       s->images[i].format == layout->format[i];
	}
	bool ok = true;
	if (!same) {
		unregister_in(s);
		for (uint32_t i = 0; i < CONV_STREAM_MAX_SUBMIT_IMAGES; i++) {
			free_image(core, &s->images[i]);
		}
		s->image_count = 0;
		for (uint32_t i = 0; ok && i < layout->image_count; i++) {
			ok = alloc_image(core, st, layout->width[i], layout->height[i], layout->format[i], 1, 0,
			                 CONV_IMAGE_USAGE_INPUT, &s->images[i]);
			s->images[i].color_space = layout->color_space[i];
		}
		s->image_count = ok ? layout->image_count : 0;
	}
	for (uint32_t i = 0; i < layout->image_count; i++) {
		s->role[i] = layout->role[i];
	}
	if (!ok) {
		os_mutex_lock(&core->streams_mtx);
		u_lift_mailbox_abort_submit(&st->mb, slot);
		os_mutex_unlock(&core->streams_mtx);
		return XRT_ERROR_ALLOCATION;
	}
	memset(out, 0, sizeof(*out));
	out->slot = slot;
	out->image_count = s->image_count;
	memcpy(out->images, s->images, sizeof(out->images));
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_stream_commit_submit(struct conv_core *core,
                               uint64_t stream_id,
                               int32_t slot,
                               const struct conv_frame_submit *frame,
                               uint64_t *out_frame_id)
{
	if (frame == NULL || slot < 0 || slot >= U_LIFT_INPUT_SLOTS) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&core->streams_mtx);
	struct conv_stream *st = find_locked(core, stream_id);
	if (st == NULL || st->mb.in_state[slot] != U_LIFT_IN_WRITING) {
		os_mutex_unlock(&core->streams_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	st->in[slot].frame = *frame;
	if (frame->eyes_dp_set) {
		st->opts.eyes_dp = frame->eyes_dp; // the rect moved to another DP's screen: follow it
	}
	if ((frame->frame_flags & XRT_CONV_FRAME_RESET) != 0) {
		// Applies even when this frame is superseded before conversion.
		st->reset_pending = true;
		st->discontinuity_pending = true;
	}
	const struct xrt_conv_image *img = &st->in[slot].images[0];
	const uint64_t fid =
	    u_lift_mailbox_commit_submit(&st->mb, slot, frame->source_time, now_ns(), img->width, img->height);
	signal_lane_locked(core, st->bind.provider);
	os_mutex_unlock(&core->streams_mtx);
	if (out_frame_id != NULL) {
		*out_frame_id = fid;
	}
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_stream_bind_eyes(struct conv_core *core, uint64_t stream_id, uint64_t dp_key)
{
	os_mutex_lock(&core->streams_mtx);
	struct conv_stream *st = find_locked(core, stream_id);
	if (st != NULL) {
		st->opts.eyes_dp = dp_key;
	}
	os_mutex_unlock(&core->streams_mtx);
	return st != NULL ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

void
conv_core_stream_abort_submit(struct conv_core *core, uint64_t stream_id, int32_t slot)
{
	os_mutex_lock(&core->streams_mtx);
	for (struct conv_stream *st = core->streams; st != NULL; st = st->next) {
		if (st->id == stream_id && slot >= 0 && slot < U_LIFT_INPUT_SLOTS &&
		    st->mb.in_state[slot] == U_LIFT_IN_WRITING) {
			u_lift_mailbox_abort_submit(&st->mb, slot);
			break;
		}
	}
	os_mutex_unlock(&core->streams_mtx);
}

xrt_result_t
conv_core_stream_acquire(struct conv_core *core, uint64_t stream_id, bool only_newer, struct conv_result *out)
{
	os_mutex_lock(&core->streams_mtx);
	struct conv_stream *st = find_locked(core, stream_id);
	int32_t slot = -1;
	struct u_lift_frame_meta meta;
	if (st == NULL) {
		os_mutex_unlock(&core->streams_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (!u_lift_mailbox_pin_latest(&st->mb, only_newer, &slot, &meta)) {
		os_mutex_unlock(&core->streams_mtx);
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}
	const struct conv_ring_slot *o = &st->out[slot];
	memset(out, 0, sizeof(*out));
	out->frame_id = meta.frame_id;
	out->source_time = meta.source_time;
	out->latency_ns = meta.done_ns > meta.submit_ns ? meta.done_ns - meta.submit_ns : 0;
	out->ring_slot = (uint32_t)slot;
	out->written = o->written;
	out->views = o->targets[CONV_RING_VIEWS];
	out->depth = o->targets[CONV_RING_DEPTH];
	out->confidence = o->targets[CONV_RING_CONFIDENCE];
	out->disparity = o->targets[CONV_RING_DISPARITY];
	out->views_info = o->views_info;
	out->depth_info = o->depth_info;
	memcpy(out->active, o->active, sizeof(out->active));
	memcpy(out->rect_center, o->rect_center, sizeof(out->rect_center));
	memcpy(out->rect_size, o->rect_size, sizeof(out->rect_size));
	out->discontinuity = o->discontinuity;
	out->provisional = o->provisional;
	out->output_realloc = o->alloc_gen != st->consumer_alloc_gen;
	st->consumer_alloc_gen = o->alloc_gen;
	os_mutex_unlock(&core->streams_mtx);
	return XRT_SUCCESS;
}

void
conv_core_stream_release(struct conv_core *core, uint64_t stream_id, uint32_t ring_slot)
{
	os_mutex_lock(&core->streams_mtx);
	for (struct conv_stream *st = core->streams; st != NULL; st = st->next) {
		if (st->id == stream_id && ring_slot < U_LIFT_RING_SIZE && st->mb.out_pins[ring_slot] > 0) {
			u_lift_mailbox_unpin(&st->mb, (int32_t)ring_slot);
			os_cond_broadcast(&lane_of(core, st)->cond);
			break;
		}
	}
	os_mutex_unlock(&core->streams_mtx);
}

xrt_result_t
conv_core_stream_get_stats(struct conv_core *core, uint64_t stream_id, struct conv_stream_stats *out)
{
	os_mutex_lock(&core->streams_mtx);
	struct conv_stream *st = find_locked(core, stream_id);
	if (st == NULL) {
		os_mutex_unlock(&core->streams_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	*out = st->stats;
	out->submitted = st->mb.submitted;
	out->converted = st->mb.converted;
	out->dropped = st->mb.dropped;
	out->failed = st->mb.failed;
	out->lat_last_ns = st->mb.lat_last_ns;
	out->lat_ema_ns = st->mb.lat_ema_ns;
	out->rate_hz = u_lift_mailbox_rate_hz(&st->mb);
	out->provisional = st->pipe.provisional;
	out->unsatisfied = st->unsatisfied;
	os_mutex_lock(&core->registry_mtx);
	out->row_state = conv_core_row_state_locked(core, st->bind.provider, st->bind.cap_id);
	os_mutex_unlock(&core->registry_mtx);
	os_mutex_unlock(&core->streams_mtx);
	return XRT_SUCCESS;
}
