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
#include "util/u_lift_depth.h"
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
	float input_scale; //!< (0,1]: the S0 snapshot is downscaled by it (#2003)

	/* GAUSSIANS facade (#2003, G8): a latest-wins photo mailbox over jobs. */
	uint64_t job_id;       //!< the newest submitted job; 0 = none
	uint64_t job_frame_id; //!< its frame id
	int64_t job_source_time;
	uint64_t next_frame;
	uint8_t *blob; //!< latched result (two-call idiom)
	uint64_t blob_size;
	uint32_t blob_format;
	uint64_t blob_frame_id;
	int64_t blob_source_time;
};

struct ipc_server_conv
{
	struct conv_core *core;
	struct os_mutex mtx; //!< leaf: the table, the io ops copy
	struct ipc_server_conv_io_ops io;
	bool io_set;
	ipc_server_conv_gpu_hook_fn gpu_hook;
	void *gpu_hook_ud;
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

//! The GAUSSIANS facade is possible: the glue reads inputs back to the CPU,
//! and SPLATS_MONO has a non-terminal candidate (a job lane provider).
static bool
gaussians_routable(struct ipc_server_conv *sc, const struct ipc_server_conv_io_ops *io)
{
	struct xrt_conv_wire_task_desc d;
	return io->import_input_cpu != NULL && task_advertised(sc->core, XRT_CONV_TASK_SPLATS_MONO, &d) &&
	       (d.lanes & XRT_CONV_LANE_JOB) != 0;
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
	// rt#1978's runtime auto-convergence on every view stream whose app leaves
	// the plane AUTO (the core runs it only when an observer factory is configured).
	opts.auto_convergence = st->mode != XRT_DP_LIFT_MODE_DEPTH;

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
	if (p->inpaint != 0) {
		fr->frame_flags |= XRT_CONV_FRAME_INPAINT;
	}
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
		if (s->core_id != 0) {
			(void)conv_core_stream_destroy(sc->core, s->core_id);
		}
		if (s->job_id != 0) {
			(void)conv_core_job_destroy(sc->core, s->job_id);
		}
		free(s->blob);
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
	if (!get_io(sc, &io)) {
		return false;
	}
	if (mode == XRT_DP_LIFT_MODE_GAUSSIANS) {
		return gaussians_routable(sc, &io);
	}
	if (!mode_routable(mode)) {
		return false;
	}
	return conv_core_task_has_candidate(sc->core, mode_task(mode, content_hint));
}

bool
ipc_server_conv_resolve_stream(
    struct ipc_server_conv *sc, uint64_t client_id, uint64_t stream_id, uint64_t *out_core_id, uint32_t *out_mode)
{
	struct sc_stream st;
	if (!find_stream(sc, client_id, stream_id, &st) || st.core_id == 0) {
		return false;
	}
	*out_core_id = st.core_id;
	*out_mode = st.mode;
	return true;
}

void
ipc_server_conv_set_gpu_hook(struct ipc_server_conv *sc, ipc_server_conv_gpu_hook_fn fn, void *ud)
{
	if (sc == NULL) {
		return;
	}
	os_mutex_lock(&sc->mtx);
	sc->gpu_hook = fn;
	sc->gpu_hook_ud = ud;
	os_mutex_unlock(&sc->mtx);
}

void
ipc_server_conv_install_gpu(struct ipc_server_conv *sc, const struct ipc_server_conv_gpu *gpu)
{
	if (sc == NULL) {
		return;
	}
	os_mutex_lock(&sc->mtx);
	ipc_server_conv_gpu_hook_fn hook = sc->gpu_hook;
	void *hook_ud = sc->gpu_hook_ud;
	os_mutex_unlock(&sc->mtx);

	if (gpu != NULL) {
		struct conv_core_gpu cg;
		memset(&cg, 0, sizeof(cg));
		cg.viewsyn = gpu->viewsyn;
		cg.readback = gpu->readback;
		cg.nominal_z_m = gpu->nominal_z_m;
		conv_core_set_gpu(sc->core, &cg);
		if (hook != NULL) {
			hook(hook_ud, gpu); // allocator + the dp-lift row
		}
		ipc_server_conv_set_io(sc, &gpu->io); // the route engages last
		U_LOG_W("[conv] GPU glue installed: the v3 lift route goes through the conversion core");
		return;
	}

	// Removal: the route first, then every core-routed stream (their images
	// belong to the GPU side), then the service half and the stages.
	ipc_server_conv_set_io(sc, NULL);
	os_mutex_lock(&sc->mtx);
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
		struct sc_stream *s = &sc->streams[i];
		if (!s->used) {
			continue;
		}
		if (s->core_id != 0) {
			(void)conv_core_stream_destroy(sc->core, s->core_id);
		}
		if (s->job_id != 0) {
			(void)conv_core_job_destroy(sc->core, s->job_id);
		}
		free(s->blob);
		memset(s, 0, sizeof(*s));
	}
	os_mutex_unlock(&sc->mtx);
	if (hook != NULL) {
		hook(hook_ud, NULL);
	}
	conv_core_set_gpu(sc->core, NULL);
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
	struct ipc_server_conv_io_ops io;
	const bool splats = get_io(sc, &io) && gaussians_routable(sc, &io);
	uint32_t v3_modes = CONV_V3_MODE_DEPTH | CONV_V3_MODE_SBS | CONV_V3_MODE_NVIEW;
	if (splats) {
		// "Any", not "all": a splat model downloading never holds back live video.
		v3_modes |= CONV_V3_MODE_GAUSSIANS;
		out->modes |= XRT_DP_LIFT_MODE_GAUSSIANS;
	}
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
	(void)aux_depth_format; // advisory in v3; the provider's depth_output_format wins
	struct ipc_server_conv_io_ops io;
	if (!get_io(sc, &io)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const bool gaussians = mode == XRT_DP_LIFT_MODE_GAUSSIANS;
	if (gaussians ? !gaussians_routable(sc, &io) : !mode_routable(mode)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct sc_stream st;
	memset(&st, 0, sizeof(st));
	st.used = true;
	st.client_id = client_id;
	st.mode = mode;
	st.content_hint = gaussians ? XRT_CONV_CONTENT_PHOTO : content_hint;
	st.aux_depth = (aux_outputs & XRT_DP_LIFT_AUX_DEPTH) != 0;
	st.priority = U_LIFT_PRIORITY_NORMAL;
	// inputScale (0,1]: the S0 snapshot is converted at reduced resolution.
	st.input_scale = input_scale > 0.0f && input_scale <= 1.0f ? input_scale : 1.0f;
	if (gaussians) {
		// The photo facade: no core stream; each submit is one job (latest wins).
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
			return XRT_ERROR_CLIENT_LIMIT_REACHED;
		}
		*out_stream_id = st.wire_id;
		return XRT_SUCCESS;
	}

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
	if (st.core_id != 0) {
		(void)conv_core_stream_destroy(sc->core, st.core_id);
	}
	if (st.job_id != 0) {
		(void)conv_core_job_destroy(sc->core, st.job_id);
	}
	free(st.blob);
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
	return ipc_server_conv_lift_submit_framed(sc, client_id, args, handle, is_dxgi, NULL, out_frame_id);
}

/*! GAUSSIANS facade submit: the photo as decoded CPU pixels, one SPLATS_MONO
 *  job; a newer photo cancels the previous job (latest wins). */
static xrt_result_t
gaussians_submit(struct ipc_server_conv *sc,
                 const struct ipc_server_conv_io_ops *io,
                 struct sc_stream *st,
                 const struct ipc_arg_lift_submit *args,
                 const struct ipc_lift_params *p,
                 xrt_graphics_buffer_handle_t handle,
                 bool is_dxgi,
                 uint64_t *out_frame_id)
{
	uint8_t *px = NULL;
	xrt_result_t xret = io->import_input_cpu(io->ud, st->wire_id, handle, is_dxgi, args->width, args->height, &px);
	if (xret != XRT_SUCCESS || px == NULL) {
		return xret != XRT_SUCCESS ? xret : XRT_ERROR_WEAVE_REFUSED;
	}
	struct conv_job_create_info ji;
	memset(&ji, 0, sizeof(ji));
	ji.task = XRT_CONV_TASK_SPLATS_MONO;
	ji.priority = CONV_JOB_PRIORITY_INTERACTIVE;
	ji.input_kind = XRT_CONV_INPUT_MONO;
	ji.requested_outputs = XRT_CONV_OUT_SPLAT_BLOB;
	ji.requested_blob_format = XRT_CONV_BLOB_PLY_3DGS; // v3 formats only
	ji.image_count = 1;
	ji.image_role[0] = XRT_CONV_IMG_COLOR;
	struct xrt_conv_image *im = &ji.images[0];
	im->memory_kind = XRT_CONV_MEM_CPU;
	im->cpu_ptr = px;
	im->byte_size = (uint64_t)args->width * args->height * 4u;
	im->width = args->width;
	im->height = args->height;
	im->format = XRT_CONV_FMT_RGBA8_UNORM;
	im->color_space = XRT_CONV_CS_SRGB;
	im->row_stride = args->width * 4u;
	ji.params.struct_size = (uint32_t)sizeof(ji.params);
	ji.params.content_hint = XRT_CONV_CONTENT_PHOTO;
	ji.params.focal_px = p->focal_px > 0.0f ? p->focal_px : 0.0f;
	ji.params.source_time_ns = (uint64_t)args->source_time;
	ji.params.input_scale = 1.0f;
	uint64_t job_id = 0;
	xret = conv_core_job_submit(sc->core, st->client_id, &ji, &job_id); // CPU inputs are copied at submit
	free(px);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	if (st->job_id != 0) {
		(void)conv_core_job_destroy(sc->core, st->job_id); // latest wins: cancels a still-running one
	}
	st->job_id = job_id;
	st->job_frame_id = ++st->next_frame;
	st->job_source_time = args->source_time;
	*out_frame_id = st->job_frame_id;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_conv_lift_submit_framed(struct ipc_server_conv *sc,
                                   uint64_t client_id,
                                   const struct ipc_arg_lift_submit *args,
                                   xrt_graphics_buffer_handle_t handle,
                                   bool is_dxgi,
                                   const struct xrt_lift_rect_frame *frame,
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
	p.inpaint = 1; // svc_lift's stream default
	if (st.has_last_params) {
		p = st.last_params;
	}

	if (st.mode == XRT_DP_LIFT_MODE_GAUSSIANS) {
		if (io.import_input_cpu == NULL) {
			io.release_input(io.ud, handle, is_dxgi);
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		const xrt_result_t gr = gaussians_submit(sc, &io, &st, args, &p, handle, is_dxgi, out_frame_id);
		store_stream(sc, &st);
		return gr;
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

	// inputScale: the snapshot is box-filtered down into a smaller slot.
	uint32_t sw = args->width, sh = args->height;
	if (st.input_scale < 1.0f) {
		sw = (uint32_t)((float)args->width * st.input_scale + 0.5f);
		sh = (uint32_t)((float)args->height * st.input_scale + 0.5f);
		sw = sw < 1 ? 1 : sw;
		sh = sh < 1 ? 1 : sh;
	}
	struct conv_submit_layout lay;
	memset(&lay, 0, sizeof(lay));
	lay.image_count = 1;
	lay.width[0] = sw;
	lay.height[0] = sh;
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
	if (st.input_scale < 1.0f && fr.focal_px > 0.0f) {
		fr.focal_px *= (float)sw / (float)args->width; // focal is in INPUT pixels
	}
	// ADR-048: the session's window is the lifted rect (svc_lift's frame).
	if (frame != NULL && frame->valid) {
		fr.rect_valid = true;
		memcpy(fr.rect_center, frame->center, sizeof(fr.rect_center));
		fr.rect_w_m = frame->width_m;
		fr.rect_h_m = frame->height_m;
	}
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
	memcpy(out->rect_center, r.rect_center, sizeof(out->rect_center));
	memcpy(out->rect_size, r.rect_size, sizeof(out->rect_size));
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
	// A DEPTH-mode stream's depth IS the main export (svc_lift reported no aux
	// depth for it); aux depth rides SBS / NVIEW only.
	if ((r.written & XRT_CONV_OUT_DEPTH) != 0 && st.mode != XRT_DP_LIFT_MODE_DEPTH) {
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
		const bool metric = d->units == XRT_DP_LIFT_DEPTH_METRIC;
		// RELATIVE depth: no intrinsics, no transform — never a faked scale.
		if (metric && di->fx > 0.0f && di->fy > 0.0f) {
			d->intrinsics_valid = 1;
			d->focal[0] = di->fx;
			d->focal[1] = di->fy;
			d->principal[0] = di->cx;
			d->principal[1] = di->cy;
		}
		d->convergence_depth = (di->suggest_flags & 4u) != 0 ? di->convergence_value : 0.0f;
		// The depth refers to the source camera = the midpoint of the
		// synthesised viewpoints (the straight-on viewer when none).
		const uint32_t vn = out->viewpoint_count;
		if (vn > 0) {
			for (uint32_t i = 0; i < vn; i++) {
				d->viewpoint[0] += out->viewpoints[3 * i + 0] / (float)vn;
				d->viewpoint[1] += out->viewpoints[3 * i + 1] / (float)vn;
				d->viewpoint[2] += out->viewpoints[3 * i + 2] / (float)vn;
			}
		} else {
			d->viewpoint[2] = U_LIFT_NOMINAL_Z_DEFAULT_M;
		}
		d->transform_valid =
		    metric && u_lift_depth_to_display(d->viewpoint, r.rect_center, r.rect_size[0], r.rect_size[1],
		                                      d->convergence_depth, d->depth_to_display)
		        ? 1u
		        : 0u;
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

xrt_result_t
ipc_server_conv_lift_acquire_blob(struct ipc_server_conv *sc,
                                  uint64_t client_id,
                                  uint64_t stream_id,
                                  uint64_t capacity,
                                  bool *out_ready,
                                  struct xrt_lift_blob_info *out_info,
                                  uint8_t **out_bytes)
{
	*out_ready = false;
	memset(out_info, 0, sizeof(*out_info));
	*out_bytes = NULL;
	struct sc_stream st;
	if (sc == NULL || !find_stream(sc, client_id, stream_id, &st)) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (st.mode != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (st.blob == NULL && st.job_id != 0) {
		// Latch the newest job's blob once it succeeded (chunked read, no cap).
		struct conv_job_status js;
		memset(&js, 0, sizeof(js));
		if (conv_core_job_status(sc->core, st.job_id, &js) == XRT_SUCCESS) {
			if (js.status == CONV_JOB_SUCCEEDED) {
				struct xrt_conv_wire_job_result jr;
				memset(&jr, 0, sizeof(jr));
				if (conv_core_job_result(sc->core, st.job_id, 0, &jr) == XRT_SUCCESS &&
				    jr.byte_size > 0 && jr.byte_size <= ((uint64_t)1 << 31)) {
					uint8_t *buf = (uint8_t *)malloc((size_t)jr.byte_size);
					uint64_t off = 0;
					while (buf != NULL && off < jr.byte_size) {
						uint64_t got = 0, total = 0;
						if (conv_core_job_read(sc->core, st.job_id, 0, off, buf + off,
						                       jr.byte_size - off, &got,
						                       &total) != XRT_SUCCESS ||
						    got == 0) {
							break;
						}
						off += got;
					}
					if (buf != NULL && off == jr.byte_size) {
						st.blob = buf;
						st.blob_size = jr.byte_size;
						st.blob_format = jr.blob_format == XRT_CONV_BLOB_SOG
						                     ? XRT_DP_LIFT_BLOB_SOG
						                     : XRT_DP_LIFT_BLOB_PLY_3DGS;
						st.blob_frame_id = st.job_frame_id;
						st.blob_source_time = st.job_source_time;
					} else {
						free(buf);
					}
				}
				(void)conv_core_job_destroy(sc->core, st.job_id);
				st.job_id = 0;
			} else if (js.status == CONV_JOB_FAILED || js.status == CONV_JOB_CANCELLED) {
				(void)conv_core_job_destroy(sc->core, st.job_id);
				st.job_id = 0;
			}
			store_stream(sc, &st);
		}
	}
	if (st.blob == NULL) {
		return XRT_SUCCESS; // nothing newer
	}
	*out_ready = true;
	out_info->frame_id = st.blob_frame_id;
	out_info->source_time = st.blob_source_time;
	out_info->format = st.blob_format;
	out_info->byte_count = st.blob_size;
	if (capacity >= st.blob_size) {
		// Delivered: the latch is consumed (the caller frees the bytes).
		*out_bytes = st.blob;
		st.blob = NULL;
		st.blob_size = 0;
		store_stream(sc, &st);
	}
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
			free(s->blob); // its jobs die with the connection in the core
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
