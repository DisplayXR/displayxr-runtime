// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display FAKE stereo camera provider (ADR-043). See the header.
 *
 * Paces itself on os_monotonic_get_ns (no thread of its own): wait_frame
 * sleeps until the next frame's due time, renders the scene with the frame
 * number burned in, and hands the runtime a pointer into its own buffer —
 * exactly the shape a real plug-in has (a tracker publishes, the plug-in
 * copies/decodes, the runtime's camera thread consumes).
 *
 * @ingroup drv_sim_display
 */

#include "sim_display_stereo_camera.h"
#include "sim_display_stereo_camera_pattern.h"

#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_stereo_camera.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_HFOV_DEG 68.0
#define FAKE_BASELINE_MM 50.0
#define FAKE_BG_DEPTH_M 2.0
#define FAKE_BAR_DEPTH_M 0.6

struct fake_config
{
	bool enabled;
	uint32_t eye_w, eye_h;
	float fps;
	uint32_t format;
	int64_t suspend_period_ns;
};

//! Completes the plug-in-owned opaque type.
struct xrt_plugin_stereo_camera
{
	struct fake_config cfg;
	struct sim_stereo_camera_scene scene;
	uint8_t *gray;
	uint32_t gray_pitch;
	uint8_t *converted;
	struct u_stereo_camera_planes conv_layout;
	int64_t t0_ns;   //!< pacing anchor (re-anchored after a pause)
	int64_t open_ns; //!< suspend square-wave origin (never moves)
	int64_t period_ns;
	uint64_t seq;
};

static const struct fake_config *
fake_config(void)
{
	static struct fake_config cfg;
	static bool read = false;
	if (read) {
		return &cfg;
	}
	read = true;
	const char *e = getenv("SIM_DISPLAY_FAKE_STEREO_CAMERA");
	cfg.enabled = e != NULL && e[0] != '\0' && e[0] != '0';
	cfg.eye_w = 640;
	cfg.eye_h = 480;
	cfg.fps = 30.0f;
	cfg.format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8;
	const char *sz = getenv("SIM_DISPLAY_FAKE_STEREO_CAMERA_SIZE");
	unsigned w = 0, h = 0;
	if (sz != NULL && sscanf(sz, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 4096 && h <= 4096) {
		cfg.eye_w = w & ~1u; // NV12 needs even extents
		cfg.eye_h = h & ~1u;
	}
	const char *fps = getenv("SIM_DISPLAY_FAKE_STEREO_CAMERA_FPS");
	if (fps != NULL && atof(fps) >= 1.0 && atof(fps) <= 240.0) {
		cfg.fps = (float)atof(fps);
	}
	const char *fmt = getenv("SIM_DISPLAY_FAKE_STEREO_CAMERA_FORMAT");
	if (fmt != NULL) {
		if (strcmp(fmt, "nv12") == 0) {
			cfg.format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
		} else if (strcmp(fmt, "bgra") == 0 || strcmp(fmt, "bgra8") == 0) {
			cfg.format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_BGRA8;
		}
	}
	const char *sp = getenv("SIM_DISPLAY_FAKE_STEREO_CAMERA_SUSPEND_PERIOD_MS");
	if (sp != NULL && atol(sp) > 0) {
		cfg.suspend_period_ns = (int64_t)atol(sp) * 1000000;
	}
	if (cfg.enabled) {
		U_LOG_W("sim_display: FAKE stereo camera ON — %ux%u per eye @ %.1f Hz, format %u%s", cfg.eye_w,
		        cfg.eye_h, cfg.fps, cfg.format, cfg.suspend_period_ns > 0 ? ", suspend square-wave" : "");
	}
	return &cfg;
}

static double
fake_fx(uint32_t eye_w)
{
	return (eye_w / 2.0) / tan(FAKE_HFOV_DEG * 0.5 * 3.14159265358979323846 / 180.0);
}

uint32_t
sim_display_stereo_camera_enumerate(struct xrt_plugin_instance *inst,
                                    uint32_t capacity,
                                    struct xrt_plugin_stereo_camera_info *out)
{
	(void)inst;
	const struct fake_config *cfg = fake_config();
	if (!cfg->enabled) {
		return 0;
	}
	if (capacity >= 1 && out != NULL) {
		uint32_t sz = out->struct_size;
		struct xrt_plugin_stereo_camera_info info;
		memset(&info, 0, sizeof(info));
		info.struct_size = (uint32_t)sizeof(info);
		snprintf(info.display_name, sizeof(info.display_name), "Sim 3D camera (fake)");
		snprintf(info.device_identity, sizeof(info.device_identity), "sim-display-fake-stereo-camera-0001");
		info.flags = XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING | XRT_PLUGIN_STEREO_CAMERA_USER_FACING |
		             XRT_PLUGIN_STEREO_CAMERA_CALIBRATED | XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED;
		if (cfg->format == XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8) {
			info.flags |= XRT_PLUGIN_STEREO_CAMERA_MONOCHROME;
		}
		info.eye_width = cfg->eye_w;
		info.eye_height = cfg->eye_h;
		info.max_frame_rate = cfg->fps;
		info.native_format = cfg->format;
		// Never write past the runtime's struct_size (ADR-020).
		memcpy(out, &info, sz < sizeof(info) ? sz : sizeof(info));
		out->struct_size = sz;
	}
	return 1;
}

xrt_result_t
sim_display_stereo_camera_get_calibration(struct xrt_plugin_instance *inst,
                                          uint32_t index,
                                          struct xrt_plugin_stereo_camera_calibration *out)
{
	(void)inst;
	const struct fake_config *cfg = fake_config();
	if (!cfg->enabled || index != 0 || out == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	uint32_t sz = out->struct_size;
	struct xrt_plugin_stereo_camera_calibration c;
	memset(&c, 0, sizeof(c));
	c.struct_size = (uint32_t)sizeof(c);
	c.image_width = cfg->eye_w;
	c.image_height = cfg->eye_h;
	double fx = fake_fx(cfg->eye_w);
	for (int e = 0; e < 2; e++) {
		c.k[e][0] = fx;
		c.k[e][1] = fx;
		c.k[e][2] = (cfg->eye_w - 1) * 0.5;
		c.k[e][3] = (cfg->eye_h - 1) * 0.5;
	}
	c.distortion_model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE;
	c.rotation_right_from_left[0][0] = 1.0;
	c.rotation_right_from_left[1][1] = 1.0;
	c.rotation_right_from_left[2][2] = 1.0;
	// OpenCV convention (x_R = R x_L + T): a right camera at +x of the left
	// one has T = (-B, 0, 0).
	c.translation_right_from_left_mm[0] = -FAKE_BASELINE_MM;
	memcpy(out, &c, sz < sizeof(c) ? sz : sizeof(c));
	out->struct_size = sz;
	return XRT_SUCCESS;
}

xrt_result_t
sim_display_stereo_camera_open(struct xrt_plugin_instance *inst,
                               uint32_t index,
                               struct xrt_plugin_stereo_camera **out_cam)
{
	(void)inst;
	const struct fake_config *cfg = fake_config();
	if (!cfg->enabled || index != 0 || out_cam == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_plugin_stereo_camera *cam = calloc(1, sizeof(*cam));
	if (cam == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	cam->cfg = *cfg;
	sim_stereo_camera_scene_init(&cam->scene, cfg->eye_w, cfg->eye_h, fake_fx(cfg->eye_w), FAKE_BASELINE_MM,
	                             FAKE_BG_DEPTH_M, FAKE_BAR_DEPTH_M);
	cam->gray_pitch = 2 * cfg->eye_w;
	cam->gray = calloc((size_t)cam->gray_pitch * cfg->eye_h, 1);
	if (cfg->format != XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8) {
		u_stereo_camera_layout(cfg->format, 2 * cfg->eye_w, cfg->eye_h, &cam->conv_layout);
		cam->converted = calloc((size_t)cam->conv_layout.size, 1);
	}
	if (cam->gray == NULL || (cfg->format != XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8 && cam->converted == NULL)) {
		free(cam->gray);
		free(cam->converted);
		free(cam);
		return XRT_ERROR_ALLOCATION;
	}
	cam->period_ns = (int64_t)(1e9 / cfg->fps);
	cam->t0_ns = os_monotonic_get_ns();
	cam->open_ns = cam->t0_ns;
	U_LOG_W("sim_display: FAKE stereo camera opened (bg disparity %u px, bar disparity %u px)",
	        cam->scene.bg_disparity, cam->scene.bar_disparity);
	*out_cam = cam;
	return XRT_SUCCESS;
}

static void
sleep_ns(int64_t ns)
{
	if (ns > 0) {
		os_nanosleep(ns);
	}
}

uint32_t
sim_display_stereo_camera_wait_frame(struct xrt_plugin_stereo_camera *cam,
                                     int64_t timeout_ns,
                                     struct xrt_plugin_stereo_camera_frame *out)
{
	if (cam == NULL || out == NULL) {
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR;
	}
	int64_t now = os_monotonic_get_ns();

	// SUSPEND square wave: odd half-periods are "tracker stopped".
	if (cam->cfg.suspend_period_ns > 0 && ((now - cam->open_ns) / cam->cfg.suspend_period_ns) % 2 == 1) {
		sleep_ns(timeout_ns < 20000000 ? timeout_ns : 20000000);
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_SUSPENDED;
	}

	int64_t due = cam->t0_ns + (int64_t)cam->seq * cam->period_ns;
	if (now - due > cam->period_ns) {
		// Back from a pause: re-anchor instead of bursting the missed frames.
		cam->t0_ns = now - (int64_t)cam->seq * cam->period_ns;
		due = now;
	}
	if (due - now > timeout_ns) {
		sleep_ns(timeout_ns);
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT;
	}
	sleep_ns(due - now);

	cam->seq++;
	sim_stereo_camera_render_gray(&cam->scene, cam->seq, cam->gray, cam->gray_pitch);

	memset(out, 0, sizeof(*out));
	out->sequence = cam->seq;
	out->time_ns = due; // the synthetic "exposure"
	out->time_is_exposure = true;
	out->width = 2 * cam->cfg.eye_w;
	out->height = cam->cfg.eye_h;
	out->format = cam->cfg.format;
	if (cam->cfg.format == XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8) {
		out->planes[0] = cam->gray;
		out->pitches[0] = cam->gray_pitch;
	} else {
		const uint8_t *src[2] = {cam->gray, NULL};
		const uint32_t pitches[2] = {cam->gray_pitch, 0};
		u_stereo_camera_convert(XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8, src, pitches, cam->cfg.format,
		                        cam->converted, &cam->conv_layout, out->width, out->height);
		for (uint32_t p = 0; p < cam->conv_layout.plane_count; p++) {
			out->planes[p] = cam->converted + cam->conv_layout.offset[p];
			out->pitches[p] = cam->conv_layout.pitch[p];
		}
	}
	return XRT_PLUGIN_STEREO_CAMERA_WAIT_OK;
}

void
sim_display_stereo_camera_release_frame(struct xrt_plugin_stereo_camera *cam)
{
	(void)cam; // the buffer is ours and is only rewritten by the next wait_frame
}

void
sim_display_stereo_camera_close(struct xrt_plugin_stereo_camera *cam)
{
	if (cam == NULL) {
		return;
	}
	U_LOG_W("sim_display: FAKE stereo camera closed after %llu frames", (unsigned long long)cam->seq);
	free(cam->gray);
	free(cam->converted);
	free(cam);
}
