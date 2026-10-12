// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift v3 handlers -> the conversion core (see ipc_server_conv.h).
 * @ingroup ipc_server
 */

#include "server/ipc_server_conv.h"

#include "shared/ipc_protocol.h"

#include "conversion/conv_core.h"
#include "conversion/conv_readiness.h"
#include "conversion/conv_select.h"

#include "os/os_threading.h"
#include "util/u_logging.h"
#include "util/u_lift_mailbox.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// The v3 mode bits ARE the core's legacy-mode bits (conv_legacy_mode_task).
_Static_assert(XRT_DP_LIFT_MODE_DEPTH == CONV_V3_MODE_DEPTH && XRT_DP_LIFT_MODE_SBS == CONV_V3_MODE_SBS &&
                   XRT_DP_LIFT_MODE_NVIEW == CONV_V3_MODE_NVIEW &&
                   XRT_DP_LIFT_MODE_GAUSSIANS == CONV_V3_MODE_GAUSSIANS,
               "v3 mode bits != CONV_V3_MODE_*");
_Static_assert(XRT_DP_LIFT_STATE_UNAVAILABLE == CONV_V3_STATE_UNAVAILABLE &&
                   XRT_DP_LIFT_STATE_ACTIVATING == CONV_V3_STATE_ACTIVATING &&
                   XRT_DP_LIFT_STATE_READY == CONV_V3_STATE_READY,
               "v3 state values != CONV_V3_STATE_*");
_Static_assert(XRT_LIFT_RIG_NONE == U_LIFT_RIG_NONE && XRT_LIFT_RIG_DISPLAY == U_LIFT_RIG_DISPLAY &&
                   XRT_LIFT_RIG_CAMERA == U_LIFT_RIG_CAMERA,
               "lift rig type values differ");
_Static_assert(IPC_LIFT_MAX_VIEWS <= XRT_CONV_MAX_VIEWS, "v3 views exceed the conversion ABI's views");
// A v3 submit carries ONE input image; the conversion wire allows two (+ sync).
_Static_assert(1 <= CONV_STREAM_MAX_SUBMIT_IMAGES, "a v3 submit needs one input image slot");

//! v3 NVIEW default view count (lift_params_from_xr: viewCount 0 -> 4).
#define SC_NVIEW_DEFAULT_VIEWS 4u

struct sc_stream
{
	bool used;
	uint64_t wire_id;
	uint64_t core_id;
	uint64_t client_id;
	uint32_t mode;
	uint32_t content_hint;
	uint32_t view_count;
	uint32_t max_views;
	bool aux_depth;
	uint32_t priority;
	bool has_last_params;
	struct ipc_lift_params last_params;
};

struct ipc_server_conv
{
	struct conv_core *core;
	struct os_mutex mtx; //!< leaf: the table, the io ops copy
	struct ipc_server_conv_io_ops io;
	bool io_set;
	uint64_t next_wire;
	struct sc_stream streams[IPC_SERVER_CONV_MAX_STREAMS];
};

static struct ipc_server_conv *g_sc;


/*
 *
 * Helpers.
 *
 */

static bool
io_complete(const struct ipc_server_conv_io_ops *io)
{
	return io->import_input != NULL && io->release_input != NULL && io->publish_result != NULL &&
	       io->export_output != NULL && io->export_fence != NULL && io->stream_gone != NULL;
}

//! A copy of the io ops, or false when the route is off.
static bool
get_io(struct ipc_server_conv *sc, struct ipc_server_conv_io_ops *out)
{
	if (sc == NULL) {
		return false;
	}
	os_mutex_lock(&sc->mtx);
	const bool ok = sc->io_set;
	if (ok) {
		*out = sc->io;
	}
	os_mutex_unlock(&sc->mtx);
	return ok;
}

//! Copy of the stream @p wire_id owned by @p client_id; false = not ours.
static bool
find_stream(struct ipc_server_conv *sc, uint64_t client_id, uint64_t wire_id, struct sc_stream *out)
{
	if (sc == NULL || (wire_id & IPC_SERVER_CONV_STREAM_TAG) == 0) {
		return false;
	}
	bool found = false;
	os_mutex_lock(&sc->mtx);
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
		const struct sc_stream *s = &sc->streams[i];
		if (s->used && s->wire_id == wire_id && s->client_id == client_id) {
			*out = *s;
			found = true;
			break;
		}
	}
	os_mutex_unlock(&sc->mtx);
	return found;
}

static void
store_stream(struct ipc_server_conv *sc, const struct sc_stream *st)
{
	os_mutex_lock(&sc->mtx);
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
		struct sc_stream *s = &sc->streams[i];
		if (s->used && s->wire_id == st->wire_id) {
			*s = *st;
			break;
		}
	}
	os_mutex_unlock(&sc->mtx);
}

static bool
remove_stream(struct ipc_server_conv *sc, uint64_t client_id, uint64_t wire_id, struct sc_stream *out)
{
	bool found = false;
	os_mutex_lock(&sc->mtx);
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
		struct sc_stream *s = &sc->streams[i];
		if (s->used && s->wire_id == wire_id && s->client_id == client_id) {
			*out = *s;
			memset(s, 0, sizeof(*s));
			found = true;
			break;
		}
	}
	os_mutex_unlock(&sc->mtx);
	return found;
}

static bool
depth_kind_is_metric(uint32_t kind)
{
	return kind == XRT_CONV_DEPTH_METRIC || kind == XRT_CONV_DEPTH_METRIC_INVERSE;
}

static bool
depth_kind_is_inverse(uint32_t kind)
{
	return kind == XRT_CONV_DEPTH_METRIC_INVERSE || kind == XRT_CONV_DEPTH_AFFINE_INVERSE ||
	       kind == XRT_CONV_DEPTH_AFFINE_DISPARITY || kind == XRT_CONV_DEPTH_STEREO_DISPARITY_PX;
}

//! The task desc of @p task when selection has a candidate for it.
static bool
task_candidate(struct conv_core *core, uint32_t task, struct xrt_conv_wire_task_desc *out)
{
	if (task == XRT_CONV_TASK_NONE || !conv_core_task_has_candidate(core, task)) {
		return false;
	}
	return conv_core_get_task_desc(core, task, out);
}

//! The task desc of @p task when its candidate is non-terminal (the mode can be advertised).
static bool
task_advertised(struct conv_core *core, uint32_t task, struct xrt_conv_wire_task_desc *out)
{
	return task_candidate(core, task, out) && !conv_state_is_terminal(out->state);
}

static uint32_t
mode_task(uint32_t mode, uint32_t content_hint)
{
	return conv_legacy_mode_task(mode, content_hint == XRT_CONV_CONTENT_PHOTO);
}

static bool
mode_routable(uint32_t mode)
{
	return mode == XRT_DP_LIFT_MODE_DEPTH || mode == XRT_DP_LIFT_MODE_SBS || mode == XRT_DP_LIFT_MODE_NVIEW;
}

static xrt_result_t
core_create(struct ipc_server_conv *sc, struct sc_stream *st, uint64_t *out_core_id)
{
	const uint32_t task = mode_task(st->mode, st->content_hint);
	struct xrt_conv_wire_task_desc d;
	if (!task_candidate(sc->core, task, &d)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_conv_wire_stream_desc sd;
	memset(&sd, 0, sizeof(sd));
	sd.struct_size = (uint32_t)sizeof(sd);
	sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	sd.task = task;
	sd.input_kind = XRT_CONV_INPUT_MONO;
	sd.content_hint = st->content_hint == XRT_CONV_CONTENT_PHOTO ? XRT_CONV_CONTENT_PHOTO : XRT_CONV_CONTENT_VIDEO;
	sd.views_layout = XRT_CONV_VIEWS_ROW; // today's SBS / NVIEW texture
	if (st->mode == XRT_DP_LIFT_MODE_DEPTH) {
		sd.outputs = XRT_CONV_OUT_DEPTH;
	} else {
		sd.outputs = XRT_CONV_OUT_VIEWS;
		// Aux depth is advisory in v3 (roadmap §7.3): required only when declared.
		if (st->aux_depth && (d.outputs & XRT_CONV_OUT_DEPTH) != 0) {
			sd.outputs |= XRT_CONV_OUT_DEPTH;
		}
		sd.view_count = st->view_count;
	}

	struct conv_stream_options opts;
	memset(&opts, 0, sizeof(opts));
	opts.priority = st->priority != 0 ? st->priority : U_LIFT_PRIORITY_NORMAL;
	opts.eyes_dp = 0; // the primary display processor's eyes (#2003 publishes them; stale = nominal)
	opts.source_format = XRT_CONV_FMT_RGBA8_UNORM;

	struct xrt_conv_wire_stream_created created;
	memset(&created, 0, sizeof(created));
	const xrt_result_t xret = conv_core_stream_create(sc->core, st->client_id, &sd, &opts, &created);
	if (xret == XRT_SUCCESS) {
		*out_core_id = created.stream_id;
	}
	return xret;
}

static void
view_control_from_ipc(const struct xrt_lift_view_control *in, struct u_lift_view_control *out)
{
	u_lift_view_control_default(out);
	out->ipd_factor = in->ipd_factor;
	out->parallax_factor = in->parallax_factor;
	out->axis_mode = in->axis_mode;
	out->max_offset_m = in->max_offset_m;
	out->recenter_mode = in->recenter_mode;
	if (in->hold_s >= 0.0f) {
		out->hold_s = in->hold_s;
	}
	if (in->tau_s > 0.0f) {
		out->tau_s = in->tau_s;
	}
}

static void
frame_from_params(const struct ipc_lift_params *p, uint32_t view_count, struct conv_frame_submit *fr)
{
	if (p->viewpoint_count > 0) {
		uint32_t n = p->viewpoint_count;
		if (n > IPC_LIFT_MAX_VIEWS) {
			n = IPC_LIFT_MAX_VIEWS;
		}
		if (view_count > 0 && n > view_count) {
			n = view_count;
		}
		fr->explicit_count = n;
		for (uint32_t i = 0; i < n; i++) {
			fr->explicit_xyz[i][0] = p->viewpoints[3 * i + 0];
			fr->explicit_xyz[i][1] = p->viewpoints[3 * i + 1];
			fr->explicit_xyz[i][2] = p->viewpoints[3 * i + 2];
		}
	}
	if (p->has_view_control != 0) {
		fr->has_view_control = true;
		view_control_from_ipc(&p->view_control, &fr->vc);
	}
	if (p->rig.type != XRT_LIFT_RIG_NONE) {
		fr->rig.type = p->rig.type;
		fr->rig.ipd_factor = p->rig.ipd_factor;
		fr->rig.parallax_factor = p->rig.parallax_factor;
		fr->rig.perspective_factor = p->rig.perspective_factor;
		fr->rig.inv_convergence_distance = p->rig.inv_convergence_distance;
		fr->rig.half_tan_vfov = p->rig.half_tan_vfov;
		fr->rig.m2v = p->rig.m2v;
		u_lift_rig_sanitize(&fr->rig);
	}
	// v3: convergence < 0 = auto; else the app's plane, [0,1] relative depth.
	if (p->convergence >= 0.0f) {
		fr->convergence_set = true;
		fr->convergence_rel01 = p->convergence > 1.0f ? 1.0f : p->convergence;
	}
	fr->strength = p->strength;
	fr->focal_px = p->focal_px;
}


/*
 *
 * Lifetime + registration.
 *
 */

xrt_result_t
ipc_server_conv_create(struct conv_core *core, struct ipc_server_conv **out_sc)
{
	if (core == NULL || out_sc == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	struct ipc_server_conv *sc = calloc(1, sizeof(*sc));
	if (sc == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	if (os_mutex_init(&sc->mtx) != 0) {
		free(sc);
		return XRT_ERROR_ALLOCATION;
	}
	sc->core = core;
	sc->next_wire = 1;
	*out_sc = sc;
	return XRT_SUCCESS;
}

void
ipc_server_conv_destroy(struct ipc_server_conv **sc_ptr)
{
	if (sc_ptr == NULL || *sc_ptr == NULL) {
		return;
	}
	struct ipc_server_conv *sc = *sc_ptr;
	*sc_ptr = NULL;
	if (g_sc == sc) {
		g_sc = NULL;
	}
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
		struct sc_stream *s = &sc->streams[i];
		if (!s->used) {
			continue;
		}
		(void)conv_core_stream_destroy(sc->core, s->core_id);
		if (sc->io_set) {
			sc->io.stream_gone(sc->io.ud, s->wire_id);
		}
		s->used = false;
	}
	os_mutex_destroy(&sc->mtx);
	free(sc);
}

void
ipc_server_conv_set_io(struct ipc_server_conv *sc, const struct ipc_server_conv_io_ops *ops)
{
	if (sc == NULL) {
		return;
	}
	os_mutex_lock(&sc->mtx);
	if (ops != NULL && io_complete(ops)) {
		sc->io = *ops;
		sc->io_set = true;
	} else {
		if (ops != NULL) {
			U_LOG_W("[conv] v3 glue: incomplete io ops refused (the core route stays off)");
		}
		memset(&sc->io, 0, sizeof(sc->io));
		sc->io_set = false;
	}
	os_mutex_unlock(&sc->mtx);
}

struct conv_core *
ipc_server_conv_get_core(struct ipc_server_conv *sc)
{
	return sc != NULL ? sc->core : NULL;
}

void
ipc_server_set_conv(struct ipc_server_conv *sc)
{
	g_sc = sc;
}

struct ipc_server_conv *
ipc_server_get_conv(void)
{
	return g_sc;
}


/*
 *
 * Routing.
 *
 */

bool
ipc_server_conv_routes_properties(struct ipc_server_conv *sc)
{
	struct ipc_server_conv_io_ops io;
	if (!get_io(sc, &io)) {
		return false;
	}
	if (conv_core_task_has_candidate(sc->core, XRT_CONV_TASK_VIEWS_MONO) ||
	    conv_core_task_has_candidate(sc->core, XRT_CONV_TASK_DEPTH_VIDEO) ||
	    conv_core_task_has_candidate(sc->core, XRT_CONV_TASK_DEPTH_MONO)) {
		return true;
	}
	return !conv_core_discovery_settled(sc->core);
}

bool
ipc_server_conv_routes_create(struct ipc_server_conv *sc, uint32_t mode, uint32_t content_hint)
{
	struct ipc_server_conv_io_ops io;
	if (!mode_routable(mode) || !get_io(sc, &io)) {
		return false;
	}
	return conv_core_task_has_candidate(sc->core, mode_task(mode, content_hint));
}

bool
ipc_server_conv_owns_stream(struct ipc_server_conv *sc, uint64_t stream_id)
{
	if (sc == NULL || (stream_id & IPC_SERVER_CONV_STREAM_TAG) == 0) {
		return false;
	}
	bool found = false;
	os_mutex_lock(&sc->mtx);
	// No glue (removed after the create): the handler falls through to
	// svc_lift, which refuses the unknown id and disposes of any handle.
	for (uint32_t i = 0; sc->io_set && i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
		if (sc->streams[i].used && sc->streams[i].wire_id == stream_id) {
			found = true;
			break;
		}
	}
	os_mutex_unlock(&sc->mtx);
	return found;
}


/*
 *
 * The v3 adapter.
 *
 */

void
ipc_server_conv_lift_properties(struct ipc_server_conv *sc, struct xrt_dp_lift_caps *out)
{
	xrt_dp_lift_caps_init(out);
	if (sc == NULL) {
		return;
	}
	struct conv_core *core = sc->core;
	const uint32_t v3_modes = CONV_V3_MODE_DEPTH | CONV_V3_MODE_SBS | CONV_V3_MODE_NVIEW;
	out->state = conv_core_v3_state(core, v3_modes, false);

	struct xrt_conv_wire_task_desc views, depth;
	const bool have_views = task_advertised(core, XRT_CONV_TASK_VIEWS_MONO, &views);
	bool have_depth = task_advertised(core, XRT_CONV_TASK_DEPTH_VIDEO, &depth);
	if (!have_depth) {
		have_depth = task_advertised(core, XRT_CONV_TASK_DEPTH_MONO, &depth);
	}
	if (have_depth) {
		out->modes |= XRT_DP_LIFT_MODE_DEPTH;
		out->depth_semantics =
		    depth_kind_is_metric(depth.depth_kind) ? XRT_DP_LIFT_DEPTH_METRIC : XRT_DP_LIFT_DEPTH_RELATIVE;
	}
	const struct xrt_conv_wire_task_desc *main = have_views ? &views : (have_depth ? &depth : NULL);
	if (have_views) {
		out->modes |= XRT_DP_LIFT_MODE_SBS;
		uint32_t mv = views.max_views > IPC_LIFT_MAX_VIEWS ? IPC_LIFT_MAX_VIEWS : views.max_views;
		out->max_views = mv;
		if (mv > 2) {
			out->modes |= XRT_DP_LIFT_MODE_NVIEW;
		}
		if ((views.outputs & XRT_CONV_OUT_DEPTH) != 0) {
			out->aux_outputs = XRT_DP_LIFT_AUX_DEPTH;
			// AFFINE kinds (e.g. AFFINE_DISPARITY) read RELATIVE (+ INVERSE
			// encoding per result), so auxDepthSupported stays true (§7.3 shift 3).
			out->aux_depth_semantics =
			    depth_kind_is_metric(views.depth_kind) ? XRT_DP_LIFT_DEPTH_METRIC : XRT_DP_LIFT_DEPTH_RELATIVE;
		}
	}
	if (main != NULL) {
		out->max_streams = main->max_streams;
		out->typical_latency_ns = (uint64_t)main->est_latency_ms * 1000000ull;
	}
	// No provider identity crosses the app API (ADR-053).
	memcpy(out->backend, "conversion", sizeof("conversion"));
}

xrt_result_t
ipc_server_conv_lift_stream_create(struct ipc_server_conv *sc,
                                   uint64_t client_id,
                                   uint32_t mode,
                                   uint32_t content_hint,
                                   float input_scale,
                                   uint32_t aux_outputs,
                                   uint32_t aux_depth_format,
                                   uint64_t *out_stream_id)
{
	*out_stream_id = 0;
	(void)input_scale;      // the glue's S0 snapshot may downscale; the core sees the snapshot extent
	(void)aux_depth_format; // advisory in v3; the provider's depth_output_format wins
	struct ipc_server_conv_io_ops io;
	if (!mode_routable(mode) || !get_io(sc, &io)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct sc_stream st;
	memset(&st, 0, sizeof(st));
	st.used = true;
	st.client_id = client_id;
	st.mode = mode;
	st.content_hint = content_hint;
	st.aux_depth = (aux_outputs & XRT_DP_LIFT_AUX_DEPTH) != 0;
	st.priority = U_LIFT_PRIORITY_NORMAL;

	struct xrt_conv_wire_task_desc d;
	if (!task_candidate(sc->core, mode_task(mode, content_hint), &d)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	st.max_views = d.max_views > IPC_LIFT_MAX_VIEWS ? IPC_LIFT_MAX_VIEWS : d.max_views;
	if (mode == XRT_DP_LIFT_MODE_SBS) {
		st.view_count = 2;
	} else if (mode == XRT_DP_LIFT_MODE_NVIEW) {
		if (st.max_views <= 2) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED; // NVIEW is advertised only when maxViews > 2
		}
		st.view_count = st.max_views < SC_NVIEW_DEFAULT_VIEWS ? st.max_views : SC_NVIEW_DEFAULT_VIEWS;
	}

	xrt_result_t xret = core_create(sc, &st, &st.core_id);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	bool stored = false;
	os_mutex_lock(&sc->mtx);
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
		if (!sc->streams[i].used) {
			st.wire_id = IPC_SERVER_CONV_STREAM_TAG | sc->next_wire++;
			sc->streams[i] = st;
			stored = true;
			break;
		}
	}
	os_mutex_unlock(&sc->mtx);
	if (!stored) {
		(void)conv_core_stream_destroy(sc->core, st.core_id);
		return XRT_ERROR_CLIENT_LIMIT_REACHED;
	}
	*out_stream_id = st.wire_id;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_conv_lift_stream_destroy(struct ipc_server_conv *sc, uint64_t client_id, uint64_t stream_id)
{
	struct sc_stream st;
	if (sc == NULL || !remove_stream(sc, client_id, stream_id, &st)) {
		return XRT_SUCCESS; // v3: destroy of an unknown stream is a no-op
	}
	(void)conv_core_stream_destroy(sc->core, st.core_id);
	struct ipc_server_conv_io_ops io;
	if (get_io(sc, &io)) {
		io.stream_gone(io.ud, stream_id);
	}
	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_conv_lift_submit(struct ipc_server_conv *sc,
                            uint64_t client_id,
                            const struct ipc_arg_lift_submit *args,
                            xrt_graphics_buffer_handle_t handle,
                            bool is_dxgi,
                            uint64_t *out_frame_id)
{
	*out_frame_id = 0;
	struct ipc_server_conv_io_ops io;
	if (!get_io(sc, &io)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // the caller still owns the handle: route checked first
	}
	struct sc_stream st;
	if (!find_stream(sc, client_id, args->stream_id, &st)) {
		io.release_input(io.ud, handle, is_dxgi);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream: non-fatal, like svc_lift
	}
	if (args->width == 0 || args->height == 0) {
		io.release_input(io.ud, handle, is_dxgi);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}

	if (args->has_params != 0) {
		st.last_params = args->params;
		st.has_last_params = true;
	}
	struct ipc_lift_params p;
	memset(&p, 0, sizeof(p));
	p.convergence = -1.0f;
	p.strength = 1.0f;
	if (st.has_last_params) {
		p = st.last_params;
	}

	// #2005 G2: the view count is bounded by the TASK's max_views, here too.
	if (st.mode == XRT_DP_LIFT_MODE_NVIEW && p.view_count != 0 && p.view_count != st.view_count) {
		if (p.view_count < 2 || p.view_count > st.max_views) {
			io.release_input(io.ud, handle, is_dxgi);
			U_LOG_I("[conv] v3 submit: viewCount %u outside [2, %u] for this NVIEW stream", p.view_count,
			        st.max_views);
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
		// A new view count is a new core stream behind the same v3 handle (the
		// glue reallocates the export texture and says output_realloc).
		const uint64_t old_core = st.core_id;
		st.view_count = p.view_count;
		xrt_result_t cr = core_create(sc, &st, &st.core_id);
		if (cr != XRT_SUCCESS) {
			io.release_input(io.ud, handle, is_dxgi);
			return cr;
		}
		(void)conv_core_stream_destroy(sc->core, old_core);
	}
	store_stream(sc, &st);

	struct conv_submit_layout lay;
	memset(&lay, 0, sizeof(lay));
	lay.image_count = 1;
	lay.width[0] = args->width;
	lay.height[0] = args->height;
	lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
	lay.color_space[0] = XRT_CONV_CS_SRGB;
	lay.role[0] = XRT_CONV_IMG_COLOR;
	struct conv_submit_slot slot;
	memset(&slot, 0, sizeof(slot));
	xrt_result_t xret = conv_core_stream_begin_submit(sc->core, st.core_id, &lay, &slot);
	if (xret != XRT_SUCCESS) {
		io.release_input(io.ud, handle, is_dxgi);
		return xret;
	}
	xret = io.import_input(io.ud, st.wire_id, handle, is_dxgi, args->width, args->height, &slot);
	if (xret != XRT_SUCCESS) {
		conv_core_stream_abort_submit(sc->core, st.core_id, slot.slot);
		return xret;
	}

	struct conv_frame_submit fr;
	memset(&fr, 0, sizeof(fr));
	fr.source_time = args->source_time;
	frame_from_params(&p, st.view_count, &fr);
	return conv_core_stream_commit_submit(sc->core, st.core_id, slot.slot, &fr, out_frame_id);
}

xrt_result_t
ipc_server_conv_lift_acquire(struct ipc_server_conv *sc,
                             uint64_t client_id,
                             uint64_t stream_id,
                             bool *out_ready,
                             struct ipc_lift_result *out)
{
	*out_ready = false;
	memset(out, 0, sizeof(*out));
	struct ipc_server_conv_io_ops io;
	if (!get_io(sc, &io)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct sc_stream st;
	if (!find_stream(sc, client_id, stream_id, &st)) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	struct conv_result r;
	memset(&r, 0, sizeof(r));
	xrt_result_t xret = conv_core_stream_acquire(sc->core, st.core_id, true, &r);
	if (xret == XRT_ERROR_NO_IMAGE_AVAILABLE) {
		return XRT_SUCCESS; // nothing newer: NOT READY
	}
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	struct ipc_server_conv_published pub;
	memset(&pub, 0, sizeof(pub));
	xret = io.publish_result(io.ud, stream_id, &r, &pub);
	conv_core_stream_release(sc->core, st.core_id, r.ring_slot);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	*out_ready = true;
	out->frame_id = r.frame_id;
	out->source_time = r.source_time;
	out->fence_value = pub.fence_value;
	out->latency_ns = r.latency_ns;
	out->width = pub.width;
	out->height = pub.height;
	out->format = pub.format;
	out->output_realloc = (pub.realloc || r.output_realloc) ? 1u : 0u;
	if ((r.written & XRT_CONV_OUT_VIEWS) != 0) {
		uint32_t n = r.views_info.view_count;
		if (n > IPC_LIFT_MAX_VIEWS) {
			n = IPC_LIFT_MAX_VIEWS;
		}
		out->view_count = n;
		out->viewpoint_count = n;
		for (uint32_t i = 0; i < n; i++) {
			out->viewpoints[3 * i + 0] = r.views_info.eye_xyz[i][0];
			out->viewpoints[3 * i + 1] = r.views_info.eye_xyz[i][1];
			out->viewpoints[3 * i + 2] = r.views_info.eye_xyz[i][2];
		}
	}
	if ((r.written & XRT_CONV_OUT_DEPTH) != 0) {
		const struct xrt_conv_depth_info *di = &r.depth_info;
		struct xrt_lift_depth_info *d = &out->depth;
		d->valid = 1;
		d->realloc = pub.depth_realloc ? 1u : 0u;
		d->width = pub.depth_width;
		d->height = pub.depth_height;
		d->format = pub.depth_format;
		d->units = depth_kind_is_metric(di->kind) ? XRT_DP_LIFT_DEPTH_METRIC : XRT_DP_LIFT_DEPTH_RELATIVE;
		d->encoding =
		    depth_kind_is_inverse(di->kind) ? XRT_DP_LIFT_DEPTH_ENCODING_INVERSE : XRT_DP_LIFT_DEPTH_ENCODING_LINEAR;
		d->same_inference = di->same_inference;
		d->value_scale = di->value_scale != 0.0f ? di->value_scale : 1.0f;
		d->value_offset = di->value_shift;
		d->near_depth = di->near_value;
		d->far_depth = di->far_value;
		if (di->fx > 0.0f && di->fy > 0.0f) {
			d->intrinsics_valid = 1;
			d->focal[0] = di->fx;
			d->focal[1] = di->fy;
			d->principal[0] = di->cx;
			d->principal[1] = di->cy;
		}
		d->convergence_depth = (di->suggest_flags & 4u) != 0 ? di->convergence_value : 0.0f;
	}
	return XRT_SUCCESS;
}

bool
ipc_server_conv_lift_export_output(struct ipc_server_conv *sc,
                                   uint64_t client_id,
                                   uint64_t stream_id,
                                   bool depth,
                                   xrt_graphics_buffer_handle_t *out_handle,
                                   uint32_t *out_width,
                                   uint32_t *out_height,
                                   uint32_t *out_format)
{
	struct ipc_server_conv_io_ops io;
	struct sc_stream st;
	if (!get_io(sc, &io) || !find_stream(sc, client_id, stream_id, &st)) {
		return false;
	}
	return io.export_output(io.ud, stream_id, depth, out_handle, out_width, out_height, out_format);
}

bool
ipc_server_conv_lift_export_fence(struct ipc_server_conv *sc,
                                  uint64_t client_id,
                                  uint64_t stream_id,
                                  xrt_graphics_sync_handle_t *out_handle)
{
	struct ipc_server_conv_io_ops io;
	struct sc_stream st;
	if (!get_io(sc, &io) || !find_stream(sc, client_id, stream_id, &st)) {
		return false;
	}
	return io.export_fence(io.ud, stream_id, out_handle);
}

xrt_result_t
ipc_server_conv_lift_set_priority(struct ipc_server_conv *sc,
                                  uint64_t client_id,
                                  uint64_t stream_id,
                                  uint32_t priority)
{
	struct sc_stream st;
	if (!find_stream(sc, client_id, stream_id, &st)) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (priority > U_LIFT_PRIORITY_HIGH) {
		return XRT_ERROR_IPC_FAILURE; // untrusted wire (oxr validated the enum)
	}
	xrt_result_t xret = conv_core_stream_set_priority(sc->core, st.core_id, priority);
	if (xret == XRT_SUCCESS) {
		st.priority = priority;
		store_stream(sc, &st);
	}
	return xret;
}

xrt_result_t
ipc_server_conv_lift_stats(struct ipc_server_conv *sc,
                           uint64_t client_id,
                           uint64_t stream_id,
                           struct xrt_lift_stream_stats *out)
{
	memset(out, 0, sizeof(*out));
	struct sc_stream st;
	if (!find_stream(sc, client_id, stream_id, &st)) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	struct conv_stream_stats s;
	memset(&s, 0, sizeof(s));
	xrt_result_t xret = conv_core_stream_get_stats(sc->core, st.core_id, &s);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	out->priority = st.priority;
	out->submitted = s.submitted;
	out->converted = s.converted;
	out->dropped = s.dropped;
	out->failed = s.failed;
	out->latency_last_ns = s.lat_last_ns;
	out->latency_avg_ns = s.lat_ema_ns;
	out->rate_hz = s.rate_hz;
	return XRT_SUCCESS;
}

void
ipc_server_conv_hint_interest(struct ipc_server_conv *sc, uint64_t client_id, uint64_t task_mask)
{
	if (sc == NULL) {
		return;
	}
	conv_core_hint_interest(sc->core, client_id, task_mask);
}

void
ipc_server_conv_client_gone(struct ipc_server_conv *sc, uint64_t client_id)
{
	if (sc == NULL) {
		return;
	}
	uint64_t gone[IPC_SERVER_CONV_MAX_STREAMS];
	uint32_t n = 0;
	os_mutex_lock(&sc->mtx);
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
		struct sc_stream *s = &sc->streams[i];
		if (s->used && s->client_id == client_id) {
			gone[n++] = s->wire_id;
			memset(s, 0, sizeof(*s));
		}
	}
	os_mutex_unlock(&sc->mtx);
	// The core drops this connection's streams and jobs itself.
	conv_core_client_gone(sc->core, client_id);
	struct ipc_server_conv_io_ops io;
	if (n > 0 && get_io(sc, &io)) {
		for (uint32_t i = 0; i < n; i++) {
			io.stream_gone(io.ud, gone[i]);
		}
	}
}
