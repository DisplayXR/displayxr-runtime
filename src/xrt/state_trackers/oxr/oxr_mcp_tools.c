// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MCP tool implementations for the OpenXR state tracker.
 *
 * In Phase A the DisplayXR runtime supports exactly one handle-app session
 * at a time, so a single attached @c oxr_session pointer is sufficient.
 * Access is synchronized with a mutex — tool handlers run on the MCP
 * server thread, the session pointer is written from the app thread.
 *
 * Tools are registered in two phases:
 *   @ref oxr_mcp_tools_register_all — called from xrCreateInstance.
 *   @ref oxr_mcp_tools_attach_session — called from xrCreateSession.
 * Handlers read-check the session pointer and return an error result when
 * no session is attached yet.
 *
 * @ingroup oxr_main
 */

#include "oxr_mcp_tools.h"
#include "oxr_objects.h"

#include <displayxr_mcp/mcp_server.h>
#include <displayxr_mcp/mcp_transport.h>
#include <displayxr_mcp/mcp_capture.h>
#include <displayxr_mcp/mcp_log_ring.h>

#include "util/u_logging.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "os/os_time.h"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_limits.h"

#include <cjson/cJSON.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static pthread_mutex_t g_sess_lock = PTHREAD_MUTEX_INITIALIZER;
static struct oxr_session *g_sess = NULL;

// ---------- Per-view snapshot ----------
//
// Three signals per spec §4.C, captured per view (1..XRT_MAX_VIEWS) so the
// same machinery handles mono / stereo / quilted displays:
//   recommended  = runtime's xrLocateViews output (Kooima from display geometry)
//   declared     = app's XrCompositionLayerProjectionView at frame submit
//   actual       = pixels (slice 6, not here)
//
// Published via atomic pointer swap: writer fills the off-buffer, swaps in.
// Reader does an atomic load + memcpy, so the whole snapshot is self-consistent.

struct mcp_view_data
{
	struct xrt_pose pose;
	struct xrt_fov fov;
	// subImage fields are only meaningful on declared views.
	struct
	{
		uint32_t width_pixels, height_pixels;
		int32_t x_pixels, y_pixels;
		uint32_t array_index;
		uint32_t image_index;
	} subimage;
};

struct mcp_frame_snapshot
{
	uint64_t seq;
	int64_t predicted_display_time_ns; //!< xrLocateViews::displayTime, monotonic ns.
	uint64_t frame_id;                 //!< sess->frame_id.begun.

	// xrLocateViews reports the system max view_count across all modes
	// (e.g. 4 on a 3D display that supports Quad), while submit reports
	// the active mode's actual view count (e.g. 2 for Anaglyph). Keep
	// both so get_kooima_params / get_submitted_projection report the
	// correct per-signal count, and diff_projection pairs declared[i]
	// with recommended[i] for i in 0..min(rec,dec).
	uint32_t recommended_view_count;
	uint32_t declared_view_count;

	// Recommended (runtime) — written by record_recommended.
	struct mcp_view_data recommended[XRT_MAX_VIEWS];
	bool have_recommended;

	// Declared (app) — written by record_submitted.
	struct mcp_view_data declared[XRT_MAX_VIEWS];
	bool have_declared;

	// Display context — copied in at publish so the snapshot is self-contained.
	float display_width_m, display_height_m;
	uint32_t atlas_width_pixels, atlas_height_pixels;
	uint32_t panel_width_pixels, panel_height_pixels;
	float nominal_viewer_x_m, nominal_viewer_y_m, nominal_viewer_z_m;
};

// Originally an _Atomic pointer swap, but MSVC's <stdatomic.h> requires
// /experimental:c11atomics and even then doesn't accept _Atomic(T*).
// A tiny mutex is correct everywhere and uncontended in our case
// (single writer per frame, occasional reader on the MCP thread).
static pthread_mutex_t g_published_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mcp_frame_snapshot *g_published = NULL;
static struct mcp_frame_snapshot g_scratch;
static uint64_t g_next_seq = 1;

static struct oxr_session *
lock_session(void)
{
	pthread_mutex_lock(&g_sess_lock);
	return g_sess;
}

static void
unlock_session(void)
{
	pthread_mutex_unlock(&g_sess_lock);
}

static cJSON *
error_object(const char *message)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "error", message);
	return o;
}

static const char *
gfx_api_name(const struct oxr_session *sess)
{
	if (sess->is_d3d11_native_compositor)
		return "d3d11";
	if (sess->is_d3d12_native_compositor)
		return "d3d12";
	if (sess->is_metal_native_compositor)
		return "metal";
	if (sess->is_gl_native_compositor)
		return "opengl";
	if (sess->is_vk_native_compositor)
		return "vulkan";
	return "unknown";
}

// ---------- list_sessions ----------

static cJSON *
tool_list_sessions(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	cJSON *result = cJSON_CreateObject();
	cJSON *arr = cJSON_CreateArray();

	struct oxr_session *sess = lock_session();
	if (sess != NULL) {
		cJSON *e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "pid", (double)mcp_self_pid());
		// Phase A is single-session; display_id is 0.
		cJSON_AddNumberToObject(e, "display_id", 0);
		cJSON_AddStringToObject(e, "api", gfx_api_name(sess));
		cJSON_AddNumberToObject(e, "session_state", sess->state);
		cJSON_AddNumberToObject(e, "view_count", sess->sys != NULL ? sess->sys->view_count : 0);
		cJSON_AddItemToArray(arr, e);
	}
	unlock_session();

	cJSON_AddItemToObject(result, "sessions", arr);
	return result;
}

static const struct mcp_tool TOOL_LIST_SESSIONS = {
    .name = "list_sessions",
    .description = "List MCP-introspectable DisplayXR sessions visible from this process.",
    .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
    .fn = tool_list_sessions,
};

// ---------- get_display_info ----------

static cJSON *
tool_get_display_info(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	struct oxr_session *sess = lock_session();
	if (sess == NULL || sess->sys == NULL || sess->sys->xsysc == NULL) {
		unlock_session();
		return error_object("no active session");
	}
	const struct xrt_system_compositor_info *info = &sess->sys->xsysc->info;
	uint32_t view_count = sess->sys->view_count;

	cJSON *r = cJSON_CreateObject();

	cJSON *phys = cJSON_CreateObject();
	cJSON_AddNumberToObject(phys, "width_m", info->display_width_m);
	cJSON_AddNumberToObject(phys, "height_m", info->display_height_m);
	cJSON_AddItemToObject(r, "physical_size", phys);

	cJSON *panel = cJSON_CreateObject();
	cJSON_AddNumberToObject(panel, "width_pixels", info->display_pixel_width);
	cJSON_AddNumberToObject(panel, "height_pixels", info->display_pixel_height);
	cJSON_AddItemToObject(r, "panel", panel);

	cJSON *atlas = cJSON_CreateObject();
	cJSON_AddNumberToObject(atlas, "width_pixels", info->atlas_width_pixels);
	cJSON_AddNumberToObject(atlas, "height_pixels", info->atlas_height_pixels);
	cJSON_AddItemToObject(r, "atlas", atlas);

	cJSON *nom = cJSON_CreateObject();
	cJSON_AddNumberToObject(nom, "x_m", info->nominal_viewer_x_m);
	cJSON_AddNumberToObject(nom, "y_m", info->nominal_viewer_y_m);
	cJSON_AddNumberToObject(nom, "z_m", info->nominal_viewer_z_m);
	cJSON_AddItemToObject(r, "nominal_viewer", nom);

	cJSON *scale = cJSON_CreateObject();
	cJSON_AddNumberToObject(scale, "x", info->recommended_view_scale_x);
	cJSON_AddNumberToObject(scale, "y", info->recommended_view_scale_y);
	cJSON_AddItemToObject(r, "recommended_view_scale", scale);

	cJSON_AddNumberToObject(r, "view_count", view_count);
	cJSON_AddBoolToObject(r, "hardware_display_3d", sess->hardware_display_3d);

	cJSON *views = cJSON_CreateArray();
	for (uint32_t i = 0; i < view_count && i < XRT_MAX_VIEWS; i++) {
		const XrViewConfigurationView *v = &sess->sys->views[i];
		cJSON *vo = cJSON_CreateObject();
		cJSON_AddNumberToObject(vo, "recommended_width", v->recommendedImageRectWidth);
		cJSON_AddNumberToObject(vo, "recommended_height", v->recommendedImageRectHeight);
		cJSON_AddNumberToObject(vo, "max_width", v->maxImageRectWidth);
		cJSON_AddNumberToObject(vo, "max_height", v->maxImageRectHeight);
		cJSON_AddItemToArray(views, vo);
	}
	cJSON_AddItemToObject(r, "views", views);

	unlock_session();
	return r;
}

static const struct mcp_tool TOOL_GET_DISPLAY_INFO = {
    .name = "get_display_info",
    .description =
        "Return the DisplayXR display dimensions, panel size, atlas size, nominal viewer position, "
        "and per-view recommended/max image rects. Wraps XR_EXT_display_info.",
    .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
    .fn = tool_get_display_info,
};

// ---------- get_runtime_metrics ----------

static cJSON *
tool_get_runtime_metrics(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	struct oxr_session *sess = lock_session();
	if (sess == NULL) {
		unlock_session();
		return error_object("no active session");
	}
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "gfx_api", gfx_api_name(sess));
	cJSON_AddNumberToObject(r, "session_state", sess->state);
	cJSON_AddNumberToObject(r, "frames_begun", (double)sess->frame_id.begun);
	cJSON_AddNumberToObject(r, "frames_waited", (double)sess->frame_id.waited);
	cJSON_AddNumberToObject(r, "active_wait_frames", sess->active_wait_frames);
	cJSON_AddBoolToObject(r, "compositor_visible", sess->compositor_visible);
	cJSON_AddBoolToObject(r, "compositor_focused", sess->compositor_focused);
	cJSON_AddBoolToObject(r, "has_external_window", sess->has_external_window);
	cJSON_AddNumberToObject(r, "ipd_meters", sess->ipd_meters);
	unlock_session();
	return r;
}

static const struct mcp_tool TOOL_GET_RUNTIME_METRICS = {
    .name = "get_runtime_metrics",
    .description =
        "Return cheap runtime counters: frames begun/waited, compositor visibility/focus, "
        "graphics API, current session state.",
    .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
    .fn = tool_get_runtime_metrics,
};

// ---------- Snapshot helpers ----------

static void
copy_display_context(struct mcp_frame_snapshot *dst, const struct oxr_session *sess)
{
	if (sess->sys == NULL || sess->sys->xsysc == NULL) {
		return;
	}
	const struct xrt_system_compositor_info *info = &sess->sys->xsysc->info;
	dst->display_width_m = info->display_width_m;
	dst->display_height_m = info->display_height_m;
	dst->atlas_width_pixels = info->atlas_width_pixels;
	dst->atlas_height_pixels = info->atlas_height_pixels;
	dst->panel_width_pixels = info->display_pixel_width;
	dst->panel_height_pixels = info->display_pixel_height;
	dst->nominal_viewer_x_m = info->nominal_viewer_x_m;
	dst->nominal_viewer_y_m = info->nominal_viewer_y_m;
	dst->nominal_viewer_z_m = info->nominal_viewer_z_m;
}

static cJSON *
pose_to_json(const struct xrt_pose *p)
{
	cJSON *o = cJSON_CreateObject();
	cJSON *pos = cJSON_CreateObject();
	cJSON_AddNumberToObject(pos, "x", p->position.x);
	cJSON_AddNumberToObject(pos, "y", p->position.y);
	cJSON_AddNumberToObject(pos, "z", p->position.z);
	cJSON_AddItemToObject(o, "position", pos);
	cJSON *q = cJSON_CreateObject();
	cJSON_AddNumberToObject(q, "x", p->orientation.x);
	cJSON_AddNumberToObject(q, "y", p->orientation.y);
	cJSON_AddNumberToObject(q, "z", p->orientation.z);
	cJSON_AddNumberToObject(q, "w", p->orientation.w);
	cJSON_AddItemToObject(o, "orientation", q);
	return o;
}

static cJSON *
fov_to_json(const struct xrt_fov *f)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "angle_left", f->angle_left);
	cJSON_AddNumberToObject(o, "angle_right", f->angle_right);
	cJSON_AddNumberToObject(o, "angle_up", f->angle_up);
	cJSON_AddNumberToObject(o, "angle_down", f->angle_down);
	return o;
}

// Load the published snapshot into @p out. Returns false if no snapshot
// has been published yet. Mutex-guarded; uncontended in practice (one
// frame writer + one MCP-thread reader).
static bool
load_snapshot(struct mcp_frame_snapshot *out)
{
	bool ok = false;
	pthread_mutex_lock(&g_published_lock);
	if (g_published != NULL) {
		*out = *g_published;
		ok = true;
	}
	pthread_mutex_unlock(&g_published_lock);
	return ok;
}

// ---------- get_kooima_params ----------
//
// Returns the runtime's recommended per-view projection + the display
// geometry that Kooima used to derive it. Per-view arrays size to
// view_count so mono/stereo/quilted displays all report correctly.

static cJSON *
tool_get_kooima_params(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	struct mcp_frame_snapshot snap;
	if (!load_snapshot(&snap) || !snap.have_recommended) {
		return error_object("no recommended views yet — ensure xrLocateViews has been called");
	}

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "seq", (double)snap.seq);
	cJSON_AddNumberToObject(r, "predicted_display_time_ns", (double)snap.predicted_display_time_ns);
	cJSON_AddNumberToObject(r, "frame_id", (double)snap.frame_id);
	cJSON_AddNumberToObject(r, "view_count", snap.recommended_view_count);

	cJSON *disp = cJSON_CreateObject();
	cJSON_AddNumberToObject(disp, "width_m", snap.display_width_m);
	cJSON_AddNumberToObject(disp, "height_m", snap.display_height_m);
	cJSON_AddNumberToObject(disp, "panel_width_pixels", snap.panel_width_pixels);
	cJSON_AddNumberToObject(disp, "panel_height_pixels", snap.panel_height_pixels);
	cJSON_AddNumberToObject(disp, "atlas_width_pixels", snap.atlas_width_pixels);
	cJSON_AddNumberToObject(disp, "atlas_height_pixels", snap.atlas_height_pixels);
	cJSON *nom = cJSON_CreateObject();
	cJSON_AddNumberToObject(nom, "x_m", snap.nominal_viewer_x_m);
	cJSON_AddNumberToObject(nom, "y_m", snap.nominal_viewer_y_m);
	cJSON_AddNumberToObject(nom, "z_m", snap.nominal_viewer_z_m);
	cJSON_AddItemToObject(disp, "nominal_viewer", nom);
	cJSON_AddItemToObject(r, "display", disp);

	cJSON *views = cJSON_CreateArray();
	for (uint32_t i = 0; i < snap.recommended_view_count && i < XRT_MAX_VIEWS; i++) {
		cJSON *v = cJSON_CreateObject();
		cJSON_AddItemToObject(v, "recommended_pose", pose_to_json(&snap.recommended[i].pose));
		cJSON_AddItemToObject(v, "recommended_fov", fov_to_json(&snap.recommended[i].fov));
		cJSON_AddItemToArray(views, v);
	}
	cJSON_AddItemToObject(r, "views", views);
	return r;
}

static const struct mcp_tool TOOL_GET_KOOIMA_PARAMS = {
    .name = "get_kooima_params",
    .description =
        "Return the runtime's per-view recommended projection (pose + FoV) as computed from "
        "display geometry and viewer pose, plus the display context used. Array sized to "
        "view_count (1=mono, 2=stereo, 4/8=quilted).",
    .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
    .fn = tool_get_kooima_params,
};

// ---------- get_submitted_projection ----------
//
// Returns the per-view projection the app actually submitted via
// XrCompositionLayerProjectionView at the most recent xrEndFrame.

static cJSON *
tool_get_submitted_projection(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	struct mcp_frame_snapshot snap;
	if (!load_snapshot(&snap) || !snap.have_declared) {
		return error_object("no submitted projection yet — waiting for xrEndFrame");
	}

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "seq", (double)snap.seq);
	cJSON_AddNumberToObject(r, "predicted_display_time_ns", (double)snap.predicted_display_time_ns);
	cJSON_AddNumberToObject(r, "frame_id", (double)snap.frame_id);
	cJSON_AddNumberToObject(r, "view_count", snap.declared_view_count);

	cJSON *views = cJSON_CreateArray();
	for (uint32_t i = 0; i < snap.declared_view_count && i < XRT_MAX_VIEWS; i++) {
		cJSON *v = cJSON_CreateObject();
		cJSON_AddItemToObject(v, "declared_pose", pose_to_json(&snap.declared[i].pose));
		cJSON_AddItemToObject(v, "declared_fov", fov_to_json(&snap.declared[i].fov));
		cJSON *sub = cJSON_CreateObject();
		cJSON_AddNumberToObject(sub, "x_pixels", snap.declared[i].subimage.x_pixels);
		cJSON_AddNumberToObject(sub, "y_pixels", snap.declared[i].subimage.y_pixels);
		cJSON_AddNumberToObject(sub, "width_pixels", snap.declared[i].subimage.width_pixels);
		cJSON_AddNumberToObject(sub, "height_pixels", snap.declared[i].subimage.height_pixels);
		cJSON_AddNumberToObject(sub, "array_index", snap.declared[i].subimage.array_index);
		cJSON_AddNumberToObject(sub, "image_index", snap.declared[i].subimage.image_index);
		cJSON_AddItemToObject(v, "subimage", sub);
		cJSON_AddItemToArray(views, v);
	}
	cJSON_AddItemToObject(r, "views", views);
	return r;
}

static const struct mcp_tool TOOL_GET_SUBMITTED_PROJECTION = {
    .name = "get_submitted_projection",
    .description =
        "Return the per-view projection the app submitted via XrCompositionLayerProjectionView "
        "at the most recent xrEndFrame, plus the swapchain sub-image rect for each view.",
    .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
    .fn = tool_get_submitted_projection,
};

// ---------- diff_projection ----------
//
// Classifies the delta between recommended and declared per view. See
// docs/roadmap/mcp-spec-v0.2.md §4.C for the bug-class taxonomy.

#include <math.h>

#define EPS_FOV_RAD 0.0005f     // ~0.03°
#define EPS_ASPECT 0.01f        // ~1% aspect mismatch tolerance
#define EPS_POSITION_M 0.001f   // 1 mm
#define EPS_QUAT_DOT 0.9999f    // ≈0.8° rotation tolerance

static float
fov_aspect_wh(const struct xrt_fov *f)
{
	float w = tanf(f->angle_right) + tanf(-f->angle_left);
	float h = tanf(f->angle_up) + tanf(-f->angle_down);
	return h > 1e-6f ? (w / h) : 0.0f;
}

static bool
fov_equal(const struct xrt_fov *a, const struct xrt_fov *b)
{
	return fabsf(a->angle_left - b->angle_left) < EPS_FOV_RAD &&
	       fabsf(a->angle_right - b->angle_right) < EPS_FOV_RAD &&
	       fabsf(a->angle_up - b->angle_up) < EPS_FOV_RAD &&
	       fabsf(a->angle_down - b->angle_down) < EPS_FOV_RAD;
}

static float
quat_dot(const struct xrt_quat *a, const struct xrt_quat *b)
{
	return a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w;
}

static float
pos_delta(const struct xrt_vec3 *a, const struct xrt_vec3 *b)
{
	float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static cJSON *
tool_diff_projection(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	struct mcp_frame_snapshot snap;
	if (!load_snapshot(&snap)) {
		return error_object("no snapshot yet");
	}
	if (!snap.have_recommended || !snap.have_declared) {
		return error_object("snapshot missing recommended or declared — call xrLocateViews + xrEndFrame first");
	}

	// Pair declared[i] with recommended[i] for i in 0..min(counts). The
	// active 3D mode (e.g. Anaglyph=2) often submits fewer views than
	// xrLocateViews reports (system max, e.g. 4).
	uint32_t diff_count = snap.declared_view_count < snap.recommended_view_count
	                          ? snap.declared_view_count
	                          : snap.recommended_view_count;

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "seq", (double)snap.seq);
	cJSON_AddNumberToObject(r, "recommended_view_count", snap.recommended_view_count);
	cJSON_AddNumberToObject(r, "declared_view_count", snap.declared_view_count);
	cJSON_AddNumberToObject(r, "view_count", diff_count);

	float display_aspect =
	    (snap.display_height_m > 0.f) ? (snap.display_width_m / snap.display_height_m) : 0.f;

	cJSON *views = cJSON_CreateArray();
	cJSON *flags_set = cJSON_CreateArray();
	bool have_any_flag = false;

	for (uint32_t i = 0; i < diff_count && i < XRT_MAX_VIEWS; i++) {
		const struct mcp_view_data *rec = &snap.recommended[i];
		const struct mcp_view_data *dec = &snap.declared[i];
		cJSON *v = cJSON_CreateObject();
		cJSON *vflags = cJSON_CreateArray();

		// (1) declared fov != recommended fov — neutral observation, not
		// a bug (some apps deliberately compute their own Kooima with
		// different tunables). Use fov_aspect_mismatch_* to detect real
		// pipeline breaks.
		bool fov_differs = !fov_equal(&rec->fov, &dec->fov);
		if (fov_differs) {
			cJSON_AddItemToArray(vflags, cJSON_CreateString("app_not_forwarding_locate_views_fov"));
		}

		// (2) fov_aspect_mismatch: declared fov aspect vs. subImage aspect,
		// and vs. display aspect when available.
		float dec_fov_aspect = fov_aspect_wh(&dec->fov);
		float sub_aspect = 0.0f;
		if (dec->subimage.height_pixels > 0) {
			sub_aspect = (float)dec->subimage.width_pixels / (float)dec->subimage.height_pixels;
		}
		bool sub_aspect_ok =
		    (sub_aspect > 0.f && fabsf(dec_fov_aspect - sub_aspect) < EPS_ASPECT * dec_fov_aspect);
		bool disp_aspect_ok =
		    (display_aspect > 0.f &&
		     fabsf(dec_fov_aspect - display_aspect) < EPS_ASPECT * dec_fov_aspect);
		if (sub_aspect > 0.f && !sub_aspect_ok) {
			cJSON_AddItemToArray(vflags, cJSON_CreateString("fov_aspect_mismatch_subimage"));
		}
		if (display_aspect > 0.f && !disp_aspect_ok && diff_count == 1) {
			// Only flag display-aspect mismatch for mono; stereo halves the fov aspect.
			cJSON_AddItemToArray(vflags, cJSON_CreateString("fov_aspect_mismatch_display"));
		}

		// (3) declared vs. recommended pose delta — same caveat as (1):
		// apps doing client-side Kooima will also submit a different
		// pose. Not inherently a bug. Phase B may add a cross-frame
		// staleness check (true "pose froze for N frames") which is a
		// real bug class.
		float dp = pos_delta(&dec->pose.position, &rec->pose.position);
		float qdot = quat_dot(&dec->pose.orientation, &rec->pose.orientation);
		if (qdot < 0.f) qdot = -qdot; // double-cover
		bool pose_stale = (dp > EPS_POSITION_M) || (qdot < EPS_QUAT_DOT);
		if (pose_stale) {
			cJSON_AddItemToArray(vflags, cJSON_CreateString("app_not_forwarding_locate_views_pose"));
		}

		// (4) wrong_disparity: reserved for slice 6 when capture_frame lands.

		cJSON_AddNumberToObject(v, "view_index", i);
		cJSON_AddItemToObject(v, "flags", vflags);

		cJSON *metrics = cJSON_CreateObject();
		cJSON_AddNumberToObject(metrics, "declared_fov_aspect_wh", dec_fov_aspect);
		cJSON_AddNumberToObject(metrics, "subimage_aspect_wh", sub_aspect);
		cJSON_AddNumberToObject(metrics, "position_delta_m", dp);
		cJSON_AddNumberToObject(metrics, "orientation_dot", qdot);
		cJSON_AddItemToObject(v, "metrics", metrics);

		cJSON *expected = cJSON_CreateObject();
		cJSON_AddItemToObject(expected, "recommended_fov", fov_to_json(&rec->fov));
		cJSON_AddItemToObject(expected, "declared_fov", fov_to_json(&dec->fov));
		cJSON_AddItemToObject(v, "fovs", expected);

		cJSON_AddItemToArray(views, v);

		// Only aspect-class mismatches are considered pipeline bugs.
		// app_not_forwarding_locate_views_{fov,pose} are neutral
		// observations — many apps deliberately compute their own
		// Kooima with custom tunables.
		if ((sub_aspect > 0.f && !sub_aspect_ok) ||
		    (display_aspect > 0.f && !disp_aspect_ok && diff_count == 1)) {
			have_any_flag = true;
		}
		(void)fov_differs;
		(void)pose_stale;
	}

	if (have_any_flag || cJSON_GetArraySize(views) > 0) {
		// Unioned flag set across all views, for quick triage.
		for (int i = 0; i < cJSON_GetArraySize(views); i++) {
			cJSON *v = cJSON_GetArrayItem(views, i);
			cJSON *vflags = cJSON_GetObjectItemCaseSensitive(v, "flags");
			for (int j = 0; j < cJSON_GetArraySize(vflags); j++) {
				const char *s = cJSON_GetArrayItem(vflags, j)->valuestring;
				bool seen = false;
				for (int k = 0; k < cJSON_GetArraySize(flags_set); k++) {
					if (strcmp(cJSON_GetArrayItem(flags_set, k)->valuestring, s) == 0) {
						seen = true;
						break;
					}
				}
				if (!seen) {
					cJSON_AddItemToArray(flags_set, cJSON_CreateString(s));
				}
			}
		}
	}

	cJSON_AddItemToObject(r, "flags", flags_set);
	cJSON_AddItemToObject(r, "views", views);
	// Renamed from "ok" to make the semantics explicit: this asks
	// whether the declared projection is internally consistent
	// (subImage / display aspect), not whether it matches the runtime's
	// recommendation. Many apps deliberately diverge from the
	// recommendation with their own Kooima — that's expected, not a bug.
	cJSON_AddBoolToObject(r, "pipeline_consistent", !have_any_flag);
	return r;
}

// ---------- capture_frame ----------
//
// Per-API readback of the compositor's most recent submitted frame or
// atlas, returned as a PNG (base64) or a file path in /tmp. The actual
// GPU→CPU copy lives in the compositor that owns the swapchain; this
// tool registers a stub path until per-compositor hooks land.
//
// Hook registration pattern: each compositor calls oxr_mcp_tools_set
// _capture_handler() at creation time; the tool then dispatches to
// whichever compositor the current session uses.

static pthread_mutex_t g_capture_lock = PTHREAD_MUTEX_INITIALIZER;
static oxr_mcp_capture_fn g_capture_fn = NULL;
static void *g_capture_userdata = NULL;

void
oxr_mcp_tools_set_capture_handler(oxr_mcp_capture_fn fn, void *userdata)
{
	pthread_mutex_lock(&g_capture_lock);
	g_capture_fn = fn;
	g_capture_userdata = userdata;
	pthread_mutex_unlock(&g_capture_lock);
}

static cJSON *
tool_capture_frame(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	pthread_mutex_lock(&g_capture_lock);
	oxr_mcp_capture_fn fn = g_capture_fn;
	void *ud = g_capture_userdata;
	pthread_mutex_unlock(&g_capture_lock);

	if (fn == NULL) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(
		    o, "error",
		    "capture_frame not wired: compositor has not registered a handler. "
		    "Rebuild with the MCP capture hooks enabled for this graphics API.");
		return o;
	}

	char path[256];
	long pid = (long)mcp_self_pid();
	struct oxr_session *sess = lock_session();
	int64_t begun = sess ? sess->frame_id.begun : 0;
	uint64_t seq = begun >= 0 ? (uint64_t)begun : 0;
	unlock_session();
	snprintf(path, sizeof(path), "/tmp/displayxr-mcp-capture-%ld-%llu.png", pid, (unsigned long long)seq);

	bool ok = fn(path, ud);

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "frame_id", (double)seq);
	cJSON_AddStringToObject(r, "path", path);
	// Absorb any brief fclose→stat lag before reporting size.
	struct stat st = {0};
	for (int attempt = 0; attempt < 20; attempt++) {
		if (stat(path, &st) == 0 && st.st_size > 0) {
			break;
		}
		os_nanosleep(10 * 1000 * 1000); // 10 ms
	}
	cJSON_AddNumberToObject(r, "size_bytes", (double)st.st_size);
	if (!ok || st.st_size == 0) {
		cJSON_AddStringToObject(r, "error", "compositor capture handler reported failure");
	}
	return r;
}

static const struct mcp_tool TOOL_CAPTURE_FRAME = {
    .name = "capture_frame",
    .description =
        "Capture the compositor's most recent composited frame — the content region of the "
        "atlas that the compositor crops and hands to the display processor (i.e. what the "
        "app actually wrote to its swapchain). Returns the PNG path and byte size.",
    .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
    .fn = tool_capture_frame,
};

static const struct mcp_tool TOOL_DIFF_PROJECTION = {
    .name = "diff_projection",
    .description =
        "Compare the runtime's recommended per-view projection against the app's declared "
        "projection and flag mismatches per spec §4.C: app_ignores_recommended, "
        "fov_aspect_mismatch_{subimage,display}, stale_head_pose. "
        "wrong_disparity is reserved for slice 6 (capture_frame).",
    .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
    .fn = tool_diff_projection,
};

// ---------- Public API ----------

static void
notify_capture_install(void *req)
{
	oxr_mcp_tools_set_capture_handler(
	    (oxr_mcp_capture_fn)mcp_capture_blocking_handler, req);
}

static void
notify_capture_uninstall(void *req)
{
	(void)req;
	oxr_mcp_tools_set_capture_handler(NULL, NULL);
}

// Bridge u_logging → mcp_log_ring. The displayxr_mcp framework owns the
// ring buffer but does not hook any logging system itself — embedders
// install a sink that pushes lines into it. tail_log readers see what
// the runtime's U_LOG_* macros emit.
static enum mcp_log_level
xlate_log_level(enum u_logging_level level)
{
	switch (level) {
	case U_LOGGING_TRACE: return MCP_LOG_LEVEL_TRACE;
	case U_LOGGING_DEBUG: return MCP_LOG_LEVEL_DEBUG;
	case U_LOGGING_INFO: return MCP_LOG_LEVEL_INFO;
	case U_LOGGING_WARN: return MCP_LOG_LEVEL_WARN;
	case U_LOGGING_ERROR: return MCP_LOG_LEVEL_ERROR;
	default: return MCP_LOG_LEVEL_RAW;
	}
}

static void
oxr_mcp_log_sink(const char *file,
                 int line,
                 const char *func,
                 enum u_logging_level level,
                 const char *fmt,
                 va_list args,
                 void *data)
{
	(void)file;
	(void)line;
	(void)func;
	(void)data;
	mcp_log_ring_append(xlate_log_level(level), fmt, args);
}

// Reads HKLM\Software\DisplayXR\Capabilities\MCP\Enabled (DWORD) under
// the 64-bit registry view. The MCP installer (DisplayXRMCPSetup-*.exe)
// from displayxr-mcp writes this key. Non-Windows builds always return
// false — there's no Capabilities key on POSIX; rely on the env var.
bool
oxr_mcp_capability_enabled(void)
{
#ifdef _WIN32
	HKEY key = NULL;
	LSTATUS rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
	                           "Software\\DisplayXR\\Capabilities\\MCP",
	                           0,
	                           KEY_READ | KEY_WOW64_64KEY,
	                           &key);
	if (rc != ERROR_SUCCESS) {
		return false;
	}
	DWORD value = 0;
	DWORD value_size = sizeof(value);
	DWORD value_type = 0;
	rc = RegQueryValueExA(key, "Enabled", NULL, &value_type,
	                      (LPBYTE)&value, &value_size);
	RegCloseKey(key);
	return rc == ERROR_SUCCESS && value_type == REG_DWORD && value == 1;
#else
	return false;
#endif
}

void
oxr_mcp_tools_register_all(void)
{
	mcp_server_register_tool(&TOOL_LIST_SESSIONS);
	mcp_server_register_tool(&TOOL_GET_DISPLAY_INFO);
	mcp_server_register_tool(&TOOL_GET_RUNTIME_METRICS);
	mcp_server_register_tool(&TOOL_GET_KOOIMA_PARAMS);
	mcp_server_register_tool(&TOOL_GET_SUBMITTED_PROJECTION);
	mcp_server_register_tool(&TOOL_DIFF_PROJECTION);
	mcp_server_register_tool(&TOOL_CAPTURE_FRAME);
	mcp_capture_set_notify(notify_capture_install, notify_capture_uninstall);
	// Only hijack the global log sink when MCP is actually enabled; otherwise
	// leave whatever sink the runtime had already installed (typically
	// `file_logging_sink` so per-process .log files keep growing past
	// xrCreateInstance). Without this gate the MCP ring sink silently swallows
	// every U_LOG_* call once the OXR instance is created, even with MCP off,
	// breaking standalone diagnostics like [CLIENT_FRAME_NS].
	if (mcp_check_env_or(oxr_mcp_capability_enabled())) {
		u_log_set_sink(oxr_mcp_log_sink, NULL);
	}
}

void
oxr_mcp_tools_record_recommended(struct oxr_session *sess,
                                 uint32_t view_count,
                                 const XrView *views,
                                 uint64_t display_time_ns)
{
	if (sess == NULL || views == NULL || view_count == 0 || view_count > XRT_MAX_VIEWS) {
		return;
	}
	g_scratch.recommended_view_count = view_count;
	g_scratch.predicted_display_time_ns = (int64_t)display_time_ns;
	for (uint32_t i = 0; i < view_count; i++) {
		// XrPosef layout matches xrt_pose; same for XrFovf and xrt_fov.
		memcpy(&g_scratch.recommended[i].pose, &views[i].pose, sizeof(struct xrt_pose));
		memcpy(&g_scratch.recommended[i].fov, &views[i].fov, sizeof(struct xrt_fov));
	}
	g_scratch.have_recommended = true;
	copy_display_context(&g_scratch, sess);
	// Not publishing here — declared data comes from frame_end; publish there.
	// Make recommended visible on its own so a diff can proceed even if no
	// projection layer has been submitted yet.
	g_scratch.seq = g_next_seq++;
	g_scratch.frame_id = (uint64_t)sess->frame_id.begun;
	static struct mcp_frame_snapshot a, b;
	pthread_mutex_lock(&g_published_lock);
	struct mcp_frame_snapshot *next = (g_published == &a) ? &b : &a;
	*next = g_scratch;
	g_published = next;
	pthread_mutex_unlock(&g_published_lock);
}

void
oxr_mcp_tools_record_submitted(struct oxr_session *sess, const struct xrt_layer_data *data)
{
	if (sess == NULL || data == NULL || data->view_count == 0 || data->view_count > XRT_MAX_VIEWS) {
		return;
	}
	if (data->type != XRT_LAYER_PROJECTION && data->type != XRT_LAYER_PROJECTION_DEPTH) {
		return;
	}
	g_scratch.declared_view_count = data->view_count;
	for (uint32_t i = 0; i < data->view_count; i++) {
		const struct xrt_layer_projection_view_data *v = &data->proj.v[i];
		g_scratch.declared[i].pose = v->pose;
		g_scratch.declared[i].fov = v->fov;
		g_scratch.declared[i].subimage.x_pixels = v->sub.rect.offset.w;
		g_scratch.declared[i].subimage.y_pixels = v->sub.rect.offset.h;
		g_scratch.declared[i].subimage.width_pixels = v->sub.rect.extent.w;
		g_scratch.declared[i].subimage.height_pixels = v->sub.rect.extent.h;
		g_scratch.declared[i].subimage.array_index = v->sub.array_index;
		g_scratch.declared[i].subimage.image_index = v->sub.image_index;
	}
	g_scratch.have_declared = true;
	copy_display_context(&g_scratch, sess);
	g_scratch.seq = g_next_seq++;
	g_scratch.frame_id = (uint64_t)sess->frame_id.begun;

	// Double-buffer publish. Two static slots alternate; we never free.
	static struct mcp_frame_snapshot a, b;
	pthread_mutex_lock(&g_published_lock);
	struct mcp_frame_snapshot *next = (g_published == &a) ? &b : &a;
	*next = g_scratch;
	g_published = next;
	pthread_mutex_unlock(&g_published_lock);
}

void
oxr_mcp_tools_attach_session(struct oxr_session *sess)
{
	pthread_mutex_lock(&g_sess_lock);
	g_sess = sess;
	pthread_mutex_unlock(&g_sess_lock);
}

void
oxr_mcp_tools_detach_session(struct oxr_session *sess)
{
	pthread_mutex_lock(&g_sess_lock);
	if (g_sess == sess) {
		g_sess = NULL;
	}
	pthread_mutex_unlock(&g_sess_lock);
}
