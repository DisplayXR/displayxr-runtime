// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime's `get_camera_pose` answer (see target_camera_pose.h).
 * @ingroup target_common
 */

#include "target_camera_pose.h"

#include "os/os_threading.h"

#include <stdio.h>
#include <string.h>

static struct os_mutex g_mutex;
static bool g_mutex_ready = false;
static float g_home_w_m = 0.0f;
static float g_home_h_m = 0.0f;
static char g_home_screen[U_CAMERA_CALIB_KEY_MAX];
static target_camera_pose_declared_fn g_declared = NULL;

static void
lock(void)
{
	// First use is on a loader's single-threaded init path (negotiate / publish).
	if (!g_mutex_ready) {
		g_mutex_ready = os_mutex_init(&g_mutex) == 0;
	}
	if (g_mutex_ready) {
		os_mutex_lock(&g_mutex);
	}
}

static void
unlock(void)
{
	if (g_mutex_ready) {
		os_mutex_unlock(&g_mutex);
	}
}

void
target_camera_pose_set_home_display(float width_m, float height_m, const char *screen_key)
{
	lock();
	if (width_m > 0.0f && height_m > 0.0f) {
		g_home_w_m = width_m;
		g_home_h_m = height_m;
	}
	if (screen_key != NULL && screen_key[0] != '\0') {
		snprintf(g_home_screen, sizeof(g_home_screen), "%s", screen_key);
	}
	unlock();
}

void
target_camera_pose_set_declared_hook(target_camera_pose_declared_fn fn)
{
	lock();
	g_declared = fn;
	unlock();
}

bool
target_camera_pose_resolve(const char *camera_key,
                           const char *screen_key,
                           float panel_w_m,
                           float panel_h_m,
                           struct u_camera_calib_record *out,
                           double *out_iris_scale)
{
	lock();
	const float home_w = g_home_w_m, home_h = g_home_h_m;
	char home_screen[U_CAMERA_CALIB_KEY_MAX];
	snprintf(home_screen, sizeof(home_screen), "%s", g_home_screen);
	const target_camera_pose_declared_fn declared_fn = g_declared;
	unlock();

	const bool explicit_screen = screen_key != NULL && screen_key[0] != '\0';
	const char *report_screen = explicit_screen ? screen_key : home_screen;
	const double w = panel_w_m > 0.0f ? panel_w_m : home_w;
	const double h = panel_h_m > 0.0f ? panel_h_m : home_h;

	struct u_camera_calib_record declared, cal, hint;
	const bool has_declared = declared_fn != NULL && declared_fn(camera_key, report_screen, &declared);
	// "" = the screen served: any stored screen for this camera matches (newest wins).
	const char *find_screen = explicit_screen ? screen_key : NULL;
	const bool has_cal = !has_declared && u_camera_calib_store_find(camera_key, find_screen,
	                                                                XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, &cal);
	const bool has_hint =
	    !has_declared && !has_cal &&
	    u_camera_calib_store_find(camera_key, find_screen, XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT, &hint);
	double iris = 1.0;
	(void)u_camera_calib_viewer_iris_scale(&iris);
	if (out_iris_scale != NULL) {
		*out_iris_scale = iris;
	}
	return u_camera_calib_resolve(has_declared ? &declared : NULL, has_cal ? &cal : NULL, has_hint ? &hint : NULL,
	                              camera_key, report_screen, w, h, out);
}

xrt_result_t
target_camera_pose_host_get(const void *host,
                            const char *camera_key,
                            const char *screen_key,
                            struct xrt_camera_pose *inout)
{
	(void)host;
	if (inout == NULL || inout->struct_size < sizeof(uint32_t) || camera_key == NULL || camera_key[0] == '\0') {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	struct u_camera_calib_record r;
	double iris = 1.0;
	if (!target_camera_pose_resolve(camera_key, screen_key, 0.0f, 0.0f, &r, &iris)) {
		return XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY;
	}
	struct xrt_camera_pose a;
	memset(&a, 0, sizeof(a));
	a.struct_size = (uint32_t)sizeof(a);
	a.source = r.source;
	for (int i = 0; i < 3; i++) {
		a.position_m[i] = (float)r.t_m[i];
	}
	for (int i = 0; i < 4; i++) {
		a.orientation[i] = (float)r.q[i];
	}
	a.focal_scale = (float)(r.focal_scale > 0.0 ? r.focal_scale : 1.0);
	a.rms_mm = (float)r.rms_mm;
	a.viewer_iris_scale = (float)iris;
	a.sample_count = r.sample_count;
	a.timestamp_unix_s = r.timestamp_unix;
	a.generation = u_camera_calib_answer_generation(&r, iris);
	snprintf(a.solver_mode, sizeof(a.solver_mode), "%s", r.solver_mode);
	a.placement_edge = r.edge;
	a.placement_offset_mm = (float)r.offset_mm;
	a.placement_margin_mm = (float)r.margin_mm;

	// Prefix semantics: a provider built against an older (shorter) struct gets what it knows.
	const uint32_t caller = inout->struct_size;
	const size_t copy = caller < sizeof(a) ? caller : sizeof(a);
	memcpy(inout, &a, copy);
	inout->struct_size = caller;
	return XRT_SUCCESS;
}
