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
#include "os/os_time.h"
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

	/* Spec 4 (#2054): created by conv_stream_create (a task stream). */
	bool v4;
	uint32_t task;   //!< XRT_CONV_TASK_*
	bool export_cpu;  //!< results leave as CPU bytes (io.export_cpu)
	bool match_weave; //!< viewCount 0: follow the weave's view count on a lifted rect
};

//! Pending event ids per connection (tasks + the providers pseudo-id, each once).
#define SC_EV_FIFO_MAX 16u

//! One connection's spec-4 event state (opt-in latch, FIFO, last delivered).
struct sc_client_ev
{
	bool used;
	bool latched;
	uint64_t client_id;
	uint64_t status_gen;
	uint32_t fifo_count;
	uint32_t fifo[SC_EV_FIFO_MAX];
	uint32_t last_state[XRT_CONV_TASK_RESERVED_FIRST];
	uint32_t last_reason[XRT_CONV_TASK_RESERVED_FIRST];
	float last_progress[XRT_CONV_TASK_RESERVED_FIRST];
	uint64_t last_progress_ns[XRT_CONV_TASK_RESERVED_FIRST];
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

	//! Spec-4 events: a leaf lock of its own, because the core calls
	//! ipc_server_conv_on_event with its locks held while this module may hold
	//! mtx across a core call (install / removal).
	struct os_mutex ev_mtx;
	struct sc_client_ev ev[IPC_SERVER_CONV_MAX_CLIENTS];
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

/*!
 * #2029: the view-count bound of a v3 stream. Once the task's capability is
 * READY it is its max_views; while it is not (the describe may still be the
 * conservative 2 views), any count up to IPC_LIFT_MAX_VIEWS is accepted and
 * the READY refine decides (the core stream goes unsatisfied if the module
 * lacks the views).
 */
static uint32_t
views_bound(const struct xrt_conv_wire_task_desc *d)
{
	if (d->state != XRT_CONV_STATE_READY) {
		return IPC_LIFT_MAX_VIEWS;
	}
	return d->max_views > IPC_LIFT_MAX_VIEWS ? IPC_LIFT_MAX_VIEWS : d->max_views;
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
	if (io->import_input_cpu == NULL || !task_advertised(sc->core, XRT_CONV_TASK_SPLATS_MONO, &d) ||
	    (d.lanes & XRT_CONV_LANE_JOB) == 0) {
		return false;
	}
	// Spec 3 knows PLY and SOG only: a provider that declares neither (e.g.
	// SPZ only) is not offered to it. 0 = undeclared (the blob says).
	const uint32_t v3_formats = (1u << XRT_CONV_BLOB_PLY_3DGS) | (1u << XRT_CONV_BLOB_SOG);
	return d.splat_formats == 0 || (d.splat_formats & v3_formats) != 0;
}

//! A blob format spec 3 can name, one to one (XRT_CONV_BLOB_* 1, 2 == XRT_DP_LIFT_BLOB_*); 0 = none.
static uint32_t
v3_blob_format(uint32_t conv_format)
{
	switch (conv_format) {
	case XRT_CONV_BLOB_PLY_3DGS: return XRT_DP_LIFT_BLOB_PLY_3DGS;
	case XRT_CONV_BLOB_SOG: return XRT_DP_LIFT_BLOB_SOG;
	default: return 0;
	}
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
	// Unbound until the first submit names the DP that owns the rect (roadmap
	// §5.2); until then the most recent eyes any DP published (stale = nominal).
	opts.eyes_dp = CONV_EYES_DP_ANY;
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
	if (os_mutex_init(&sc->ev_mtx) != 0) {
		os_mutex_destroy(&sc->mtx);
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
	os_mutex_destroy(&sc->ev_mtx);
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
	if (!get_io(sc, &io) || io.cpu_only) {
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
	if (!get_io(sc, &io) || io.cpu_only) {
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
		if (gpu->io.cpu_only) {
			// ADR-053 P5: a GPU side (POSIX Vulkan glue) under the CPU I/O table.
			U_LOG_W(
			    "[conv] GPU glue installed under the CPU route (cpu_only): spec-4 CPU streams, composed "
			    "pipelines on; no v3 route");
		} else {
			U_LOG_W("[conv] GPU glue installed: the v3 lift route goes through the conversion core");
		}
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
	st.max_views = views_bound(&d);
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

	// #2005 G2: the view count is bounded by the TASK's max_views, here too
	// (#2029: re-read, so a bound taken while provisional is refined).
	if (st.mode == XRT_DP_LIFT_MODE_NVIEW && p.view_count != 0 && p.view_count != st.view_count) {
		struct xrt_conv_wire_task_desc vd;
		if (task_candidate(sc->core, mode_task(st.mode, st.content_hint), &vd)) {
			st.max_views = views_bound(&vd);
		}
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
	// #2030: still-image dedupe for PHOTO streams (the glue's GPU content
	// check; lift spec 4 will let the client name its frame instead).
	if (st.content_hint == XRT_CONV_CONTENT_PHOTO && io.input_content_key != NULL) {
		fr.content_key = io.input_content_key(io.ud, st.wire_id, &slot);
	}
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
	// The eyes of the DP that owns the session's window (multi-screen M6).
	if (frame != NULL && frame->eyes_dp_key != 0) {
		fr.eyes_dp_set = true;
		fr.eyes_dp = frame->eyes_dp_key;
	}
	return conv_core_stream_commit_submit(sc->core, st.core_id, slot.slot, &fr, out_frame_id);
}

/*! The v3 acquire; @p keep (optional) receives the result's metadata for the
 *  spec-4 acquire (its image pointers are stale once this returns). */
static xrt_result_t
lift_acquire_common(struct ipc_server_conv *sc,
                    uint64_t client_id,
                    uint64_t stream_id,
                    bool *out_ready,
                    struct ipc_lift_result *out,
                    struct conv_result *keep)
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
	if (keep != NULL) {
		*keep = r;
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
		// convergence_value is DECODED (an inverse depth for an inverse kind);
		// spec 3's convergenceDepth is the depth reading of it (the ray
		// fraction d / dc of the display transform), XR_DXR_lift.h's table.
		d->convergence_depth = 0.0f;
		if ((di->suggest_flags & 4u) != 0 && di->convergence_value > 0.0f && isfinite(di->convergence_value)) {
			d->convergence_depth =
			    depth_kind_is_inverse(di->kind) ? 1.0f / di->convergence_value : di->convergence_value;
		}
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

xrt_result_t
ipc_server_conv_lift_acquire(struct ipc_server_conv *sc,
                             uint64_t client_id,
                             uint64_t stream_id,
                             bool *out_ready,
                             struct ipc_lift_result *out)
{
	return lift_acquire_common(sc, client_id, stream_id, out_ready, out, NULL);
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
					const uint32_t v3_format = v3_blob_format(jr.blob_format);
					if (buf != NULL && off == jr.byte_size && v3_format != 0) {
						st.blob = buf;
						st.blob_size = jr.byte_size;
						st.blob_format = v3_format;
						st.blob_frame_id = st.job_frame_id;
						st.blob_source_time = st.job_source_time;
					} else {
						if (v3_format == 0) {
							// Never relabelled: a v3 caller would parse the
							// bytes as the wrong format. Dropped (NOT READY).
							U_LOG_W("[conv] GAUSSIANS: the provider returned blob format %u, "
							        "which spec 3 cannot name; dropped",
							        jr.blob_format);
						}
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

xrt_result_t
ipc_server_conv_warm(struct ipc_server_conv *sc, uint32_t task, uint32_t *out_prev_state, uint32_t *out_state)
{
	*out_prev_state = 0;
	*out_state = 0;
	if (sc == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	return conv_core_warm_task_explicit(sc->core, task, out_prev_state, out_state);
}

static void
v4_client_gone(struct ipc_server_conv *sc, uint64_t client_id);

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
	v4_client_gone(sc, client_id); // its event latch and queue
}


/*
 *
 * Spec 4 (ADR-053 P3, #2054): the conv_* family.
 *
 */

//! Pseudo-task id of a queued PROVIDERS_CHANGED in a connection's event FIFO.
#define SC_EV_PROVIDERS 0u
//! Progress events: at least this much movement, at most this often.
#define SC_EV_PROGRESS_STEP 0.05f
#define SC_EV_PROGRESS_MIN_NS (500ull * 1000ull * 1000ull)

static bool
v4_task_is_stream_task(uint32_t task)
{
	return task == XRT_CONV_TASK_DEPTH_MONO || task == XRT_CONV_TASK_DEPTH_VIDEO || task == XRT_CONV_TASK_VIEWS_MONO;
}

//! Spec-4 tasks an app may name at all (spec-5 and reserved ones are a validation failure).
static bool
v4_task_is_spec4(uint32_t task)
{
	return v4_task_is_stream_task(task) || task == XRT_CONV_TASK_SPLATS_MONO;
}

static bool
v4_state_is_waiting(uint32_t state)
{
	return state == XRT_CONV_STATE_WARMING || state == XRT_CONV_STATE_DOWNLOADING;
}

//! The connection's event slot; @p create makes one. Called with ev_mtx held.
static struct sc_client_ev *
ev_slot_locked(struct ipc_server_conv *sc, uint64_t client_id, bool create)
{
	struct sc_client_ev *free_slot = NULL;
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_CLIENTS; i++) {
		struct sc_client_ev *e = &sc->ev[i];
		if (e->used && e->client_id == client_id) {
			return e;
		}
		if (!e->used && free_slot == NULL) {
			free_slot = e;
		}
	}
	if (!create || free_slot == NULL) {
		return NULL;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	free_slot->client_id = client_id;
	return free_slot;
}

//! Queue @p id (a task, or SC_EV_PROVIDERS) once: a FIFO without duplicates.
static void
ev_queue_locked(struct sc_client_ev *e, uint32_t id)
{
	for (uint32_t i = 0; i < e->fifo_count; i++) {
		if (e->fifo[i] == id) {
			return; // already pending: the delivery reads the state then, so a repeat adds nothing
		}
	}
	if (e->fifo_count < SC_EV_FIFO_MAX) {
		e->fifo[e->fifo_count++] = id;
	}
}

void
ipc_server_conv_v4_latch(struct ipc_server_conv *sc, uint64_t client_id)
{
	if (sc == NULL || client_id == 0) {
		return;
	}
	os_mutex_lock(&sc->ev_mtx);
	struct sc_client_ev *e = ev_slot_locked(sc, client_id, false);
	const bool latched = e != NULL && e->latched;
	os_mutex_unlock(&sc->ev_mtx);
	if (latched) {
		return;
	}

	// The baseline: what the app's first spec-4 query sees. Events report
	// changes FROM it (no core call under ev_mtx).
	uint32_t state[XRT_CONV_TASK_RESERVED_FIRST];
	uint32_t reason[XRT_CONV_TASK_RESERVED_FIRST];
	float progress[XRT_CONV_TASK_RESERVED_FIRST];
	for (uint32_t t = 0; t < XRT_CONV_TASK_RESERVED_FIRST; t++) {
		struct xrt_conv_wire_task_desc d;
		state[t] = XRT_CONV_STATE_ABSENT;
		reason[t] = 0;
		progress[t] = -1.0f;
		if (conv_core_get_task_desc(sc->core, t, &d)) {
			state[t] = d.state;
			reason[t] = d.failure_reason;
			progress[t] = d.progress;
		}
	}
	struct xrt_conv_wire_generation gen;
	conv_core_get_generation(sc->core, &gen);

	os_mutex_lock(&sc->ev_mtx);
	e = ev_slot_locked(sc, client_id, true);
	if (e != NULL && !e->latched) {
		e->latched = true;
		e->status_gen = gen.status_gen;
		for (uint32_t t = 0; t < XRT_CONV_TASK_RESERVED_FIRST; t++) {
			e->last_state[t] = state[t];
			e->last_reason[t] = reason[t];
			e->last_progress[t] = progress[t];
		}
	} else if (e == NULL) {
		U_LOG_W("[conv] spec 4: more than %u connections opted into lift events; this one gets none",
		        IPC_SERVER_CONV_MAX_CLIENTS);
	}
	os_mutex_unlock(&sc->ev_mtx);
}

bool
ipc_server_conv_v4_latched(struct ipc_server_conv *sc, uint64_t client_id)
{
	if (sc == NULL) {
		return false;
	}
	os_mutex_lock(&sc->ev_mtx);
	struct sc_client_ev *e = ev_slot_locked(sc, client_id, false);
	const bool latched = e != NULL && e->latched;
	os_mutex_unlock(&sc->ev_mtx);
	return latched;
}

void
ipc_server_conv_on_event(struct ipc_server_conv *sc, uint64_t client_id, const struct xrt_conv_wire_event *ev)
{
	// Any core thread, core locks possibly held: queue only (ev_mtx is a leaf).
	if (sc == NULL || ev == NULL) {
		return;
	}
	uint32_t id;
	if (ev->kind == XRT_CONV_WIRE_EVENT_PROVIDERS_CHANGED) {
		id = SC_EV_PROVIDERS;
	} else if (ev->kind == XRT_CONV_WIRE_EVENT_TASK_STATE_CHANGED && ev->task > XRT_CONV_TASK_NONE &&
	           ev->task < XRT_CONV_TASK_RESERVED_FIRST) {
		id = ev->task;
	} else {
		return; // job events are spec 5
	}
	os_mutex_lock(&sc->ev_mtx);
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_CLIENTS; i++) {
		struct sc_client_ev *e = &sc->ev[i];
		// Opt-in: a connection that never made a spec-4 call never gets one.
		if (e->used && e->latched && (client_id == 0 || e->client_id == client_id)) {
			ev_queue_locked(e, id);
		}
	}
	os_mutex_unlock(&sc->ev_mtx);
}

xrt_result_t
ipc_server_conv_v4_poll_event(struct ipc_server_conv *sc,
                              uint64_t client_id,
                              bool *out_has,
                              struct xrt_conv_wire_event *out)
{
	*out_has = false;
	memset(out, 0, sizeof(*out));
	if (sc == NULL) {
		return XRT_SUCCESS;
	}

	// Progress moves without a state change: when the readiness generation
	// moved, re-check every task this connection last saw warming.
	struct xrt_conv_wire_generation gen;
	conv_core_get_generation(sc->core, &gen);
	os_mutex_lock(&sc->ev_mtx);
	struct sc_client_ev *e = ev_slot_locked(sc, client_id, false);
	if (e == NULL || !e->latched) {
		os_mutex_unlock(&sc->ev_mtx);
		return XRT_SUCCESS;
	}
	if (gen.status_gen != e->status_gen) {
		e->status_gen = gen.status_gen;
		for (uint32_t t = 1; t < XRT_CONV_TASK_RESERVED_FIRST; t++) {
			if (v4_state_is_waiting(e->last_state[t])) {
				ev_queue_locked(e, t);
			}
		}
	}
	os_mutex_unlock(&sc->ev_mtx);

	for (;;) {
		os_mutex_lock(&sc->ev_mtx);
		e = ev_slot_locked(sc, client_id, false);
		if (e == NULL || e->fifo_count == 0) {
			os_mutex_unlock(&sc->ev_mtx);
			return XRT_SUCCESS;
		}
		const uint32_t id = e->fifo[0];
		memmove(&e->fifo[0], &e->fifo[1], (e->fifo_count - 1) * sizeof(e->fifo[0]));
		e->fifo_count--;
		os_mutex_unlock(&sc->ev_mtx);

		out->struct_size = (uint32_t)sizeof(*out);
		out->family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
		out->progress = -1.0f;
		if (id == SC_EV_PROVIDERS) {
			out->kind = XRT_CONV_WIRE_EVENT_PROVIDERS_CHANGED;
			*out_has = true;
			return XRT_SUCCESS;
		}

		// The EFFECTIVE state now (the selected provider's), never a row's:
		// one provider's transition behind another's selection is not news.
		struct xrt_conv_wire_task_desc d;
		uint32_t state = XRT_CONV_STATE_ABSENT, reason = 0;
		float progress = -1.0f;
		if (conv_core_get_task_desc(sc->core, id, &d)) {
			state = d.state;
			reason = d.failure_reason;
			progress = d.progress;
		}
		const uint64_t now = os_monotonic_get_ns();
		bool deliver = false;
		uint32_t prev = 0;
		os_mutex_lock(&sc->ev_mtx);
		e = ev_slot_locked(sc, client_id, false);
		if (e != NULL) {
			prev = e->last_state[id];
			if (state != prev || reason != e->last_reason[id]) {
				deliver = true;
			} else if (v4_state_is_waiting(state) && progress >= 0.0f &&
			           fabsf(progress - e->last_progress[id]) >= SC_EV_PROGRESS_STEP &&
			           now - e->last_progress_ns[id] >= SC_EV_PROGRESS_MIN_NS) {
				deliver = true;
			}
			if (deliver) {
				e->last_state[id] = state;
				e->last_reason[id] = reason;
				e->last_progress[id] = progress;
				e->last_progress_ns[id] = now;
			}
		}
		os_mutex_unlock(&sc->ev_mtx);
		if (!deliver) {
			continue; // deduplicated: nothing the app has not seen
		}
		out->kind = XRT_CONV_WIRE_EVENT_TASK_STATE_CHANGED;
		out->task = id;
		out->state = state;
		out->previous_state = prev;
		out->failure_reason = reason;
		out->progress = progress;
		*out_has = true;
		return XRT_SUCCESS;
	}
}

void
ipc_server_conv_v4_get_generation(struct ipc_server_conv *sc,
                                  uint64_t client_id,
                                  struct xrt_conv_wire_generation *out)
{
	memset(out, 0, sizeof(*out));
	if (sc == NULL) {
		return;
	}
	ipc_server_conv_v4_latch(sc, client_id);
	conv_core_get_generation(sc->core, out);
}

uint64_t
ipc_server_conv_v4_image_kinds(struct ipc_server_conv *sc, bool export_kinds)
{
	struct ipc_server_conv_io_ops io;
	if (!get_io(sc, &io)) {
		return 0; // no GPU glue: no task is reachable yet
	}
	if (io.cpu_only) {
		// The POSIX CPU glue: CPU bytes in, CPU bytes out, nothing else.
		const bool ok = export_kinds ? io.export_cpu != NULL : io.import_input_ex != NULL;
		return ok ? 1ull << IPC_SERVER_CONV_IMAGE_KIND_CPU : 0;
	}
	// The v3 glue imports D3D11 NT / legacy DXGI shared textures and exports
	// D3D11 NT textures + a shared fence: the kinds every spec-4 stream gets.
	uint64_t kinds = 1ull << IPC_SERVER_CONV_IMAGE_KIND_D3D11_NT;
	if (!export_kinds) {
		kinds |= 1ull << IPC_SERVER_CONV_IMAGE_KIND_D3D11_DXGI;
		if (io.import_input_ex != NULL) {
			kinds |= (1ull << IPC_SERVER_CONV_IMAGE_KIND_D3D12_NT) | (1ull << IPC_SERVER_CONV_IMAGE_KIND_CPU);
		}
	} else if (io.export_cpu != NULL) {
		kinds |= 1ull << IPC_SERVER_CONV_IMAGE_KIND_CPU;
	}
	return kinds;
}

xrt_result_t
ipc_server_conv_v4_enum_task(struct ipc_server_conv *sc,
                             uint64_t client_id,
                             uint32_t task,
                             struct xrt_conv_wire_task_desc *out,
                             uint64_t *out_image_kinds,
                             uint64_t *out_export_kinds)
{
	memset(out, 0, sizeof(*out));
	*out_image_kinds = 0;
	*out_export_kinds = 0;
	out->struct_size = (uint32_t)sizeof(*out);
	out->family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	out->task = task;
	out->state = XRT_CONV_STATE_ABSENT;
	out->progress = -1.0f;
	if (sc == NULL) {
		return XRT_SUCCESS;
	}
	ipc_server_conv_v4_latch(sc, client_id);
	if (!v4_task_is_spec4(task) || !conv_core_get_task_desc(sc->core, task, out)) {
		return XRT_SUCCESS; // reserved / spec-5 / unserved: ABSENT, never listed
	}
	if ((out->lanes & XRT_CONV_LANE_STREAM) == 0) {
		out->max_streams = 0; // no stream lane: no stream limit to report
	}
	if (v4_task_is_stream_task(task) && (out->lanes & XRT_CONV_LANE_STREAM) != 0) {
		*out_image_kinds = ipc_server_conv_v4_image_kinds(sc, false);
		*out_export_kinds = ipc_server_conv_v4_image_kinds(sc, true);
	}
	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_conv_v4_warm(struct ipc_server_conv *sc, uint64_t client_id, uint32_t task)
{
	if (sc == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	ipc_server_conv_v4_latch(sc, client_id);
	if (!v4_task_is_spec4(task)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	return conv_core_warm_task(sc->core, task);
}

//! XR_LIFT_OUTPUT_* bits a task desc offers (the app's requiredOutputs vocabulary).
uint64_t
ipc_server_conv_v4_output_flags(const struct xrt_conv_wire_task_desc *d)
{
	uint64_t f = 0;
	const bool views = d->task == XRT_CONV_TASK_VIEWS_MONO;
	if (views && (d->outputs & XRT_CONV_OUT_DEPTH) != 0) {
		f |= IPC_SERVER_CONV_OUTPUT_AUX_DEPTH;
	}
	if ((d->outputs & XRT_CONV_OUT_DEPTH) != 0 &&
	    (d->depth_kind == XRT_CONV_DEPTH_METRIC || d->depth_kind == XRT_CONV_DEPTH_METRIC_INVERSE)) {
		f |= IPC_SERVER_CONV_OUTPUT_METRIC;
	}
	if ((d->outputs & XRT_CONV_OUT_DEPTH) != 0 && d->intrinsics_source != XRT_CONV_INTRINSICS_NONE) {
		f |= IPC_SERVER_CONV_OUTPUT_INTRINSICS;
	}
	if ((d->flags & XRT_CONV_CAP_TEMPORAL) != 0) {
		f |= IPC_SERVER_CONV_OUTPUT_TEMPORAL;
	}
	if (views && (d->flags & XRT_CONV_CAP_EXPLICIT_VIEWPOINTS) != 0) {
		f |= IPC_SERVER_CONV_OUTPUT_EXPLICIT_VIEWPOINTS;
	}
	if ((d->flags & XRT_CONV_CAP_SUGGESTS_CONVERGENCE) != 0) {
		f |= IPC_SERVER_CONV_OUTPUT_CONVERGENCE_HINT;
	}
	if ((d->outputs & XRT_CONV_OUT_CONFIDENCE) != 0) {
		f |= IPC_SERVER_CONV_OUTPUT_CONFIDENCE;
	}
	if ((d->desc_flags & XRT_CONV_WIRE_DESC_COMPOSED) != 0) {
		f |= IPC_SERVER_CONV_OUTPUT_COMPOSED;
	}
	return f;
}

xrt_result_t
ipc_server_conv_v4_stream_create(struct ipc_server_conv *sc,
                                 uint64_t client_id,
                                 const struct xrt_conv_wire_stream_desc *in,
                                 uint64_t required_outputs,
                                 uint32_t legacy_mode,
                                 float input_scale,
                                 struct xrt_conv_wire_stream_created *out)
{
	memset(out, 0, sizeof(*out));
	if (sc == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	ipc_server_conv_v4_latch(sc, client_id);

	/* VALIDATION_FAILURE (XRT_ERROR_INPUT_UNSUPPORTED on this family). */
	const uint32_t task = in->task;
	if (!v4_task_is_spec4(task)) {
		return XRT_ERROR_INPUT_UNSUPPORTED; // reserved, spec-5 or out of range
	}
	const bool views = task == XRT_CONV_TASK_VIEWS_MONO;
	if (legacy_mode != 0) {
		const bool match = (legacy_mode == XRT_DP_LIFT_MODE_DEPTH && !views && task != XRT_CONV_TASK_SPLATS_MONO) ||
		                   ((legacy_mode == XRT_DP_LIFT_MODE_SBS || legacy_mode == XRT_DP_LIFT_MODE_NVIEW) && views) ||
		                   (legacy_mode == XRT_DP_LIFT_MODE_GAUSSIANS && task == XRT_CONV_TASK_SPLATS_MONO);
		if (!match) {
			return XRT_ERROR_INPUT_UNSUPPORTED;
		}
	}
	const uint32_t input_kind = in->input_kind != 0 ? in->input_kind : XRT_CONV_INPUT_MONO;
	if (input_kind != XRT_CONV_INPUT_MONO) {
		return XRT_ERROR_INPUT_UNSUPPORTED; // stereo / image+depth input is spec 5
	}
	struct ipc_server_conv_io_ops io;
	const bool have_io = get_io(sc, &io);
	// A CPU-only glue (POSIX) exports CPU bytes by default and nothing else.
	const bool cpu_only = have_io && io.cpu_only;
	const bool cpu_export = in->export_memory_kind == XRT_CONV_MEM_CPU || (cpu_only && in->export_memory_kind == 0);
	if (in->export_memory_kind != 0 && (cpu_only || in->export_memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE) &&
	    !(cpu_export && have_io && io.export_cpu != NULL)) {
		return XRT_ERROR_INPUT_UNSUPPORTED; // an export kind no candidate offers
	}

	/* FEATURE_UNSUPPORTED: no non-terminal candidate, no stream lane, no route. */
	if (!have_io) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_conv_wire_task_desc d;
	if (!conv_core_get_task_desc(sc->core, task, &d)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (!conv_core_task_has_candidate(sc->core, task)) {
		// UNKNOWN (discovery not settled, nothing described yet) is a waiting
		// state, but there is nothing to bind a stream to: transient.
		return d.state == XRT_CONV_STATE_UNKNOWN ? XRT_ERROR_OUTPUT_REQUEST_FAILURE
		                                         : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if ((d.lanes & XRT_CONV_LANE_STREAM) == 0 || !v4_task_is_stream_task(task)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // a job-lane task (spec 5)
	}
	if ((d.input_kinds & input_kind) == 0 && d.input_kinds != 0) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	uint32_t max_views = d.max_views > IPC_LIFT_MAX_VIEWS ? IPC_LIFT_MAX_VIEWS : d.max_views;
	uint32_t view_count = 0;
	if (views) {
		view_count = in->view_count != 0 ? in->view_count : 2u;
		if (view_count < 2 || view_count > max_views) {
			return XRT_ERROR_INPUT_UNSUPPORTED; // fixes v3's unenforced maxViews
		}
	}
	// requiredOutputs refuse only against a READY provider; before READY the
	// declared outputs may be provisional, so they are not grounds to refuse.
	const uint64_t offered = ipc_server_conv_v4_output_flags(&d);
	if (d.state == XRT_CONV_STATE_READY && (required_outputs & ~offered) != 0) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	struct sc_stream st;
	memset(&st, 0, sizeof(st));
	st.used = true;
	st.client_id = client_id;
	st.v4 = true;
	st.task = task;
	st.export_cpu = cpu_export;
	st.match_weave = views && in->view_count == 0;
	st.content_hint = task == XRT_CONV_TASK_DEPTH_MONO ? XRT_CONV_CONTENT_PHOTO
	                  : task == XRT_CONV_TASK_DEPTH_VIDEO ? XRT_CONV_CONTENT_VIDEO
	                                                      : in->content_hint;
	st.mode = !views ? XRT_DP_LIFT_MODE_DEPTH : (view_count > 2 ? XRT_DP_LIFT_MODE_NVIEW : XRT_DP_LIFT_MODE_SBS);
	st.view_count = view_count;
	st.max_views = max_views;
	st.aux_depth = views && (required_outputs & IPC_SERVER_CONV_OUTPUT_AUX_DEPTH) != 0;
	st.priority = U_LIFT_PRIORITY_NORMAL;
	st.input_scale = input_scale > 0.0f && input_scale <= 1.0f ? input_scale : 1.0f;

	struct xrt_conv_wire_stream_desc sd = *in;
	sd.struct_size = (uint32_t)sizeof(sd);
	sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	sd.input_kind = input_kind;
	sd.content_hint = st.content_hint;
	sd.views_layout = XRT_CONV_VIEWS_ROW;
	sd.view_count = view_count;
	sd.outputs = views ? XRT_CONV_OUT_VIEWS : XRT_CONV_OUT_DEPTH;
	if (st.aux_depth) {
		sd.outputs |= XRT_CONV_OUT_DEPTH; // required: the core refuses a READY provider without it
	}
	sd.export_memory_kind = 0;

	struct conv_stream_options opts;
	memset(&opts, 0, sizeof(opts));
	opts.priority = U_LIFT_PRIORITY_NORMAL;
	opts.eyes_dp = 0;
	opts.source_format = XRT_CONV_FMT_RGBA8_UNORM;
	opts.auto_convergence = views;
	struct xrt_conv_wire_stream_created created;
	memset(&created, 0, sizeof(created));
	xrt_result_t xret = conv_core_stream_create(sc->core, client_id, &sd, &opts, &created);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	st.core_id = created.stream_id;

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

	*out = created;
	out->struct_size = (uint32_t)sizeof(*out);
	out->family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	out->stream_id = st.wire_id;
	out->task = task;
	out->view_count = view_count;
	out->views_layout = XRT_CONV_VIEWS_ROW;
	out->desc_flags |= d.desc_flags & XRT_CONV_WIRE_DESC_COMPOSED;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_conv_v4_stream_reset(struct ipc_server_conv *sc, uint64_t client_id, uint64_t stream_id)
{
	struct sc_stream st;
	if (sc == NULL || !find_stream(sc, client_id, stream_id, &st) || st.core_id == 0) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	return conv_core_stream_reset(sc->core, st.core_id);
}

xrt_result_t
ipc_server_conv_v4_acquire(struct ipc_server_conv *sc,
                           uint64_t client_id,
                           uint64_t stream_id,
                           bool *out_ready,
                           bool *out_discontinuity,
                           struct ipc_lift_result *out_v3,
                           struct xrt_conv_wire_result *out)
{
	memset(out, 0, sizeof(*out));
	*out_discontinuity = false;
	struct conv_result r;
	memset(&r, 0, sizeof(r));
	const xrt_result_t xret = lift_acquire_common(sc, client_id, stream_id, out_ready, out_v3, &r);
	if (xret != XRT_SUCCESS || !*out_ready) {
		return xret;
	}
	struct sc_stream st;
	struct ipc_server_conv_io_ops io;
	if (find_stream(sc, client_id, stream_id, &st) && st.export_cpu && get_io(sc, &io) && io.export_cpu != NULL) {
		// CPU export: the result just published, read back into the mapping
		// the caller reads (valid until its next acquire on this stream).
		xrt_graphics_buffer_handle_t h = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		uint32_t w = 0, hh = 0, f = 0;
		uint64_t bytes = 0;
		bool realloc_cpu = false;
		if (!io.export_cpu(io.ud, stream_id, true, &h, &w, &hh, &f, &bytes, &realloc_cpu)) {
			*out_ready = false;
			return XRT_ERROR_WEAVE_REFUSED;
		}
		if (realloc_cpu) {
			out_v3->output_realloc = 1;
		}
	}
	xrt_conv_wire_begin(out, (uint32_t)sizeof(*out));
	out->stream_id = stream_id;
	out->frame_id = r.frame_id;
	out->source_time_ns = (uint64_t)r.source_time;
	out->latency_ns = r.latency_ns;
	out->written = r.written;
	out->output_realloc = out_v3->output_realloc;
	out->views_width = r.views.width;
	out->views_height = r.views.height;
	out->views_format = r.views.format;
	out->depth_width = r.depth.width;
	out->depth_height = r.depth.height;
	out->depth_format = r.depth.format;
	xrt_conv_wire_pack_views_info(&r.views_info, &out->views_info);
	xrt_conv_wire_pack_depth_info(&r.depth_info, &out->depth_info);
	*out_discontinuity = r.discontinuity;
	return XRT_SUCCESS;
}

static void
v4_client_gone(struct ipc_server_conv *sc, uint64_t client_id)
{
	if (sc == NULL) {
		return;
	}
	os_mutex_lock(&sc->ev_mtx);
	struct sc_client_ev *e = ev_slot_locked(sc, client_id, false);
	if (e != NULL) {
		memset(e, 0, sizeof(*e));
	}
	os_mutex_unlock(&sc->ev_mtx);
}

static void
v4_release_input(const struct ipc_server_conv_io_ops *io, const struct conv_input_import *in)
{
	io->release_input(io->ud, in->handle, in->kind == CONV_INPUT_DXGI);
	if (in->sync_handle != XRT_GRAPHICS_BUFFER_HANDLE_INVALID) {
		io->release_input(io->ud, in->sync_handle, false);
	}
}

xrt_result_t
ipc_server_conv_v4_submit(struct ipc_server_conv *sc,
                          uint64_t client_id,
                          const struct ipc_arg_lift_submit *args,
                          const struct conv_input_import *in,
                          const struct xrt_lift_rect_frame *frame,
                          uint64_t *out_frame_id)
{
	*out_frame_id = 0;
	struct ipc_server_conv_io_ops io;
	if (!get_io(sc, &io)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // the caller still owns the handles: route checked first
	}
	if (io.import_input_ex == NULL) {
		v4_release_input(&io, in);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct sc_stream st;
	if (!find_stream(sc, client_id, args->stream_id, &st) || st.core_id == 0) {
		v4_release_input(&io, in);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream (or the GAUSSIANS facade): non-fatal
	}
	if (args->width == 0 || args->height == 0) {
		v4_release_input(&io, in);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}

	// The sticky params, exactly as the v3 submit keeps them.
	if (args->has_params != 0) {
		st.last_params = args->params;
		st.has_last_params = true;
	}
	struct ipc_lift_params p;
	memset(&p, 0, sizeof(p));
	p.convergence = -1.0f;
	p.strength = 1.0f;
	p.inpaint = 1;
	if (st.has_last_params) {
		p = st.last_params;
	}
	if (st.mode == XRT_DP_LIFT_MODE_NVIEW && p.view_count != 0 && p.view_count != st.view_count) {
		if (p.view_count < 2 || p.view_count > st.max_views) {
			v4_release_input(&io, in);
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
		const uint64_t old_core = st.core_id;
		st.view_count = p.view_count;
		xrt_result_t cr = core_create(sc, &st, &st.core_id);
		if (cr != XRT_SUCCESS) {
			v4_release_input(&io, in);
			return cr;
		}
		(void)conv_core_stream_destroy(sc->core, old_core);
	}
	store_stream(sc, &st);

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
		v4_release_input(&io, in);
		return xret;
	}
	struct conv_input_import region = *in;
	region.w = args->width;
	region.h = args->height;
	xret = io.import_input_ex(io.ud, st.wire_id, &region, &slot); // owns the handles from here
	if (xret != XRT_SUCCESS) {
		conv_core_stream_abort_submit(sc->core, st.core_id, slot.slot);
		return xret;
	}

	struct conv_frame_submit fr;
	memset(&fr, 0, sizeof(fr));
	fr.source_time = args->source_time;
	frame_from_params(&p, st.view_count, &fr);
	if (st.input_scale < 1.0f && fr.focal_px > 0.0f) {
		fr.focal_px *= (float)sw / (float)args->width;
	}
	// ADR-053 P5: a windowless submit converts against a rect of its extent's aspect.
	struct xrt_lift_rect_frame rect;
	if (ipc_server_conv_submit_rect(frame, args->width, args->height, &rect)) {
		fr.rect_valid = true;
		memcpy(fr.rect_center, rect.center, sizeof(fr.rect_center));
		fr.rect_w_m = rect.width_m;
		fr.rect_h_m = rect.height_m;
	}
	return conv_core_stream_commit_submit(sc->core, st.core_id, slot.slot, &fr, out_frame_id);
}

bool
ipc_server_conv_submit_rect(const struct xrt_lift_rect_frame *frame,
                            uint32_t extent_w,
                            uint32_t extent_h,
                            struct xrt_lift_rect_frame *out)
{
	if (frame == NULL) {
		return false;
	}
	*out = *frame;
	if (frame->valid) {
		return true; // a window / weave rect: unchanged
	}
	const float pw = frame->width_m, ph = frame->height_m;
	if (!(pw > 0.0f) || !(ph > 0.0f) || extent_w == 0 || extent_h == 0) {
		return false;
	}
	// The extent's aspect, fitted inside the panel, centred on it.
	const float a = (float)extent_w / (float)extent_h;
	if (a >= pw / ph) {
		out->width_m = pw;
		out->height_m = pw / a;
	} else {
		out->height_m = ph;
		out->width_m = ph * a;
	}
	memset(out->center, 0, sizeof(out->center));
	out->valid = true;
	return true;
}

bool
ipc_server_conv_v4_cpu_output(struct ipc_server_conv *sc,
                              uint64_t client_id,
                              uint64_t stream_id,
                              xrt_graphics_buffer_handle_t *out_handle,
                              uint32_t *out_width,
                              uint32_t *out_height,
                              uint32_t *out_format,
                              uint64_t *out_bytes)
{
	struct sc_stream st;
	struct ipc_server_conv_io_ops io;
	if (sc == NULL || !find_stream(sc, client_id, stream_id, &st) || !st.export_cpu || !get_io(sc, &io) ||
	    io.export_cpu == NULL) {
		return false;
	}
	bool realloc_unused = false;
	return io.export_cpu(io.ud, stream_id, false, out_handle, out_width, out_height, out_format, out_bytes,
	                     &realloc_unused);
}

void
ipc_server_conv_v4_match_weave(struct ipc_server_conv *sc, uint64_t client_id, uint64_t stream_id, uint32_t views)
{
	struct sc_stream st;
	if (sc == NULL || !find_stream(sc, client_id, stream_id, &st) || !st.match_weave || st.core_id == 0 ||
	    views == st.view_count || views < 2 || views > st.max_views) {
		return;
	}
	// A new view count is a new core stream behind the same handle, exactly
	// as the v3 NVIEW resize (the glue reallocates the export, output_realloc).
	const uint64_t old_core = st.core_id;
	const uint32_t old_views = st.view_count;
	st.view_count = views;
	st.mode = views > 2 ? XRT_DP_LIFT_MODE_NVIEW : XRT_DP_LIFT_MODE_SBS;
	if (core_create(sc, &st, &st.core_id) != XRT_SUCCESS) {
		return; // keeps serving the old count
	}
	(void)conv_core_stream_destroy(sc->core, old_core);
	store_stream(sc, &st);
	U_LOG_I("[conv] spec-4 stream 0x%llx follows the weave: %u -> %u views", (unsigned long long)stream_id,
	        old_views, views);
}
