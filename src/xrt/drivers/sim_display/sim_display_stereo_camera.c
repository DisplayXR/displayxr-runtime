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
#include "sim_display_settings.h"
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
	float advertised_fps; //!< what enumerate claims; 0 = "unknown" (the service measures)
	double baseline_mm;   //!< 50 mm default; 120 mm reproduces the Leia SR tracking camera's disparities
	uint32_t format;
	int64_t suspend_period_ns;
	//! R2: raw, distorted, misaligned pair with known ground truth (not
	//! NATIVELY_RECTIFIED; the service's rectifier does the work).
	bool distort;
	//! R2 refinement: an UNCALIBRATED extra vertical offset (px) + slope (px
	//! per 100 px) of the right eye on the distorted pair.
	double extra_dy_px, extra_dy_slope_100;
};

//! Completes the plug-in-owned opaque type.
struct xrt_plugin_stereo_camera
{
	struct fake_config cfg;
	struct sim_stereo_camera_scene scene;
	struct sim_stereo_camera_distorted distorted; //!< cfg.distort only
	uint8_t *gray;
	uint32_t gray_pitch;
	uint8_t *converted;
	struct u_stereo_camera_planes conv_layout;
	int64_t t0_ns;   //!< pacing anchor (re-anchored after a pause)
	int64_t open_ns; //!< suspend square-wave origin (never moves)
	int64_t period_ns;
	uint64_t seq;

	// Spec v8: the running mode (cfg.eye_w x eye_h is the default, mode 0).
	uint32_t eye_w, eye_h;
	// Spec v8 fake controls: a lens that follows its target with a lag.
	struct xrt_plugin_stereo_camera_controls ctl;
	uint32_t trigger_seen;
	double focus_d;      //!< where the lens is, dioptres
	double focus_target; //!< where it is going
	bool searching;
	int64_t focus_ns; //!< last lens update
	struct xrt_plugin_stereo_camera_capture_result result;
	// Spec v8 fake still: rendered at the still size after a capture latency,
	// with the frames paused meanwhile (a real source reconfiguring).
	bool still_pending, still_ready;
	int64_t still_due_ns;
	uint32_t still_w, still_h;
	uint64_t still_request;
	uint8_t *still_gray;
	uint8_t *still_conv;
	struct u_stereo_camera_planes still_layout;
	struct xrt_plugin_stereo_camera_capture_result still_result;
	int64_t still_time_ns;
};

#define FAKE_MIN_FOCUS_D 10.0       //!< nearest focus: 10 cm
#define FAKE_FOCUS_TAU_NS 150e6     //!< the lens's time constant
#define FAKE_STILL_LATENCY_NS 200e6 //!< capture latency; frames pause meanwhile
#define FAKE_BASE_EXPOSURE_NS 16666667.0
#define FAKE_MODE_COUNT_MAX 3

static void
fake_render_still(struct xrt_plugin_stereo_camera *cam);
static void
fake_update_capture_state(struct xrt_plugin_stereo_camera *cam, int64_t t_ns);

static const struct fake_config *
fake_config(void)
{
	static struct fake_config cfg;
	static bool read = false;
	if (read) {
		return &cfg;
	}
	read = true;
	// Every knob through the host iface (N3c, sim_display_settings.h: `dp set
	// sim-display STEREO_CAMERA* ...`; the SIM_DISPLAY_FAKE_STEREO_CAMERA*
	// names stay aliases for one release).
	char sz[32], fps[32], bl[32], afps[32], fmt[16], sp[32], edy[32], eds[32];
	cfg.enabled = sim_display_setting_on("STEREO_CAMERA");
	cfg.eye_w = 640;
	cfg.eye_h = 480;
	cfg.fps = 30.0f;
	cfg.format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8;
	unsigned w = 0, h = 0;
	if (sim_display_setting("STEREO_CAMERA_SIZE", sz, sizeof(sz)) && sscanf(sz, "%ux%u", &w, &h) == 2 && w >= 64 &&
	    h >= 64 && w <= 4096 && h <= 4096) {
		cfg.eye_w = w & ~1u; // NV12 needs even extents
		cfg.eye_h = h & ~1u;
	}
	if (sim_display_setting("STEREO_CAMERA_FPS", fps, sizeof(fps)) && atof(fps) >= 1.0 && atof(fps) <= 240.0) {
		cfg.fps = (float)atof(fps);
	}
	cfg.advertised_fps = cfg.fps;
	cfg.baseline_mm = FAKE_BASELINE_MM;
	if (sim_display_setting("STEREO_CAMERA_BASELINE_MM", bl, sizeof(bl)) && atof(bl) >= 10.0 && atof(bl) <= 300.0) {
		cfg.baseline_mm = atof(bl);
	}
	if (sim_display_setting("STEREO_CAMERA_ADVERTISED_FPS", afps, sizeof(afps)) && atof(afps) >= 0.0 &&
	    atof(afps) <= 240.0) {
		cfg.advertised_fps = (float)atof(afps);
	}
	if (sim_display_setting("STEREO_CAMERA_FORMAT", fmt, sizeof(fmt))) {
		if (strcmp(fmt, "nv12") == 0) {
			cfg.format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
		} else if (strcmp(fmt, "bgra") == 0 || strcmp(fmt, "bgra8") == 0) {
			cfg.format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_BGRA8;
		}
	}
	if (sim_display_setting("STEREO_CAMERA_SUSPEND_PERIOD_MS", sp, sizeof(sp)) && atol(sp) > 0) {
		cfg.suspend_period_ns = (int64_t)atol(sp) * 1000000;
	}
	cfg.distort = sim_display_setting_on("STEREO_CAMERA_DISTORT");
	if (sim_display_setting("STEREO_CAMERA_DY", edy, sizeof(edy)) && fabs(atof(edy)) <= 20.0) {
		cfg.extra_dy_px = atof(edy);
	}
	if (sim_display_setting("STEREO_CAMERA_DY_SLOPE", eds, sizeof(eds)) && fabs(atof(eds)) <= 5.0) {
		cfg.extra_dy_slope_100 = atof(eds);
	}
	if (cfg.enabled) {
		U_LOG_W("sim_display: FAKE stereo camera ON — %ux%u per eye @ %.1f Hz, baseline %.1f mm, format %u%s%s",
		        cfg.eye_w, cfg.eye_h, cfg.fps, cfg.baseline_mm, cfg.format,
		        cfg.suspend_period_ns > 0 ? ", suspend square-wave" : "",
		        cfg.distort ? ", DISTORTED raw pair (rectified by the service)" : "");
		if (cfg.distort && (cfg.extra_dy_px != 0.0 || cfg.extra_dy_slope_100 != 0.0)) {
			U_LOG_W(
			    "sim_display: FAKE stereo camera — UNCALIBRATED vertical misalignment of the right eye: "
			    "%+.2f px %+.3f px/100 px (the service's refinement should remove it)",
			    cfg.extra_dy_px, cfg.extra_dy_slope_100);
		}
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
		             XRT_PLUGIN_STEREO_CAMERA_CALIBRATED;
		if (!cfg->distort) {
			info.flags |= XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED;
		}
		if (cfg->format == XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8) {
			info.flags |= XRT_PLUGIN_STEREO_CAMERA_MONOCHROME;
		}
		info.eye_width = cfg->eye_w;
		info.eye_height = cfg->eye_h;
		info.max_frame_rate = cfg->advertised_fps;
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
	if (cfg->distort) {
		// The raw pair's ground truth (sim_display_stereo_camera_pattern.h).
		struct sim_stereo_camera_truth t;
		sim_stereo_camera_truth_init(&t, cfg->eye_w, cfg->eye_h, fake_fx(cfg->eye_w), cfg->baseline_mm,
		                             FAKE_BG_DEPTH_M, FAKE_BAR_DEPTH_M);
		for (int e = 0; e < 2; e++) {
			c.k[e][0] = t.fx[e];
			c.k[e][1] = t.fy[e];
			c.k[e][2] = t.cx[e];
			c.k[e][3] = t.cy[e];
			for (int k = 0; k < 5; k++) {
				c.distortion[e][k] = t.dist[e][k];
			}
		}
		c.distortion_model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN5;
		memcpy(c.rotation_right_from_left, t.R, sizeof(t.R));
		memcpy(c.translation_right_from_left_mm, t.T_mm, sizeof(t.T_mm));
		memcpy(out, &c, sz < sizeof(c) ? sz : sizeof(c));
		out->struct_size = sz;
		return XRT_SUCCESS;
	}
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
	c.translation_right_from_left_mm[0] = -cfg->baseline_mm;
	memcpy(out, &c, sz < sizeof(c) ? sz : sizeof(c));
	out->struct_size = sz;
	return XRT_SUCCESS;
}

/*!
 * (Re)build the scene + buffers for frames of @p w x @p h per eye (open, and
 * spec v8 set_mode). The scene is defined in eye-normalised terms through fx
 * (which scales with the width), so every mode images the same field of view
 * and the one calibration, rescaled, describes it.
 */
static bool
fake_resize(struct xrt_plugin_stereo_camera *cam, uint32_t w, uint32_t h)
{
	const struct fake_config *cfg = &cam->cfg;
	sim_stereo_camera_scene_init(&cam->scene, w, h, fake_fx(w), cfg->baseline_mm, FAKE_BG_DEPTH_M,
	                             FAKE_BAR_DEPTH_M);
	if (cfg->distort) {
		sim_stereo_camera_distorted_fini(&cam->distorted);
		if (!sim_stereo_camera_distorted_init(&cam->distorted, w, h, fake_fx(w), cfg->baseline_mm,
		                                      FAKE_BG_DEPTH_M, FAKE_BAR_DEPTH_M)) {
			return false;
		}
		// The uncalibrated misalignment is given in pixels of the default size.
		const double s = (double)w / (double)cfg->eye_w;
		sim_stereo_camera_distorted_set_vmisalign(&cam->distorted, cfg->extra_dy_px * s,
		                                          cfg->extra_dy_slope_100 / 100.0);
	}
	uint8_t *gray = realloc(cam->gray, (size_t)2 * w * h);
	if (gray == NULL) {
		return false;
	}
	cam->gray = gray;
	cam->gray_pitch = 2 * w;
	if (cfg->format != XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8) {
		struct u_stereo_camera_planes lay;
		if (!u_stereo_camera_layout(cfg->format, 2 * w, h, &lay)) {
			return false;
		}
		uint8_t *conv = realloc(cam->converted, (size_t)lay.size);
		if (conv == NULL) {
			return false;
		}
		cam->converted = conv;
		cam->conv_layout = lay;
	}
	cam->eye_w = w;
	cam->eye_h = h;
	return true;
}

/*!
 * Spec v8 modes of the fake: 0 = the configured default (STREAM + STILL), a
 * half-size STREAM mode, and a double-size STREAM + STILL mode (the "full
 * resolution" a camera app takes stills at). Even sizes only.
 */
static uint32_t
fake_modes(const struct fake_config *cfg, struct xrt_plugin_stereo_camera_mode out[FAKE_MODE_COUNT_MAX])
{
	uint32_t n = 0;
	out[n++] = (struct xrt_plugin_stereo_camera_mode){
	    (uint32_t)sizeof(out[0]), cfg->eye_w, cfg->eye_h, cfg->advertised_fps,
	    XRT_PLUGIN_STEREO_CAMERA_MODE_STREAM | XRT_PLUGIN_STEREO_CAMERA_MODE_STILL};
	const uint32_t hw = (cfg->eye_w / 2) & ~1u, hh = (cfg->eye_h / 2) & ~1u;
	if (hw >= 64 && hh >= 64) {
		out[n++] = (struct xrt_plugin_stereo_camera_mode){(uint32_t)sizeof(out[0]), hw, hh, cfg->advertised_fps,
		                                                  XRT_PLUGIN_STEREO_CAMERA_MODE_STREAM};
	}
	if (cfg->eye_w * 2 <= 4096 && cfg->eye_h * 2 <= 4096) {
		out[n++] = (struct xrt_plugin_stereo_camera_mode){
		    (uint32_t)sizeof(out[0]), cfg->eye_w * 2, cfg->eye_h * 2, cfg->advertised_fps,
		    XRT_PLUGIN_STEREO_CAMERA_MODE_STREAM | XRT_PLUGIN_STEREO_CAMERA_MODE_STILL};
	}
	return n;
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
	// Spec v8: a camera opens in its mode 0 (the default); set_mode may follow.
	if (!fake_resize(cam, cfg->eye_w, cfg->eye_h)) {
		free(cam->gray);
		free(cam->converted);
		sim_stereo_camera_distorted_fini(&cam->distorted);
		free(cam);
		return XRT_ERROR_ALLOCATION;
	}
	cam->ctl.struct_size = (uint32_t)sizeof(cam->ctl);
	cam->focus_d = 1.0 / FAKE_BG_DEPTH_M; // the source's own default: focused on the scene
	cam->focus_target = cam->focus_d;
	cam->period_ns = (int64_t)(1e9 / cfg->fps);
	cam->t0_ns = os_monotonic_get_ns();
	cam->open_ns = cam->t0_ns;
	U_LOG_W("sim_display: FAKE stereo camera opened (%s; bg disparity %.2f px, bar disparity %.2f px in the %s)",
	        cfg->distort ? "DISTORTED raw pair" : "parallel pair", cam->scene.bg_disparity_f,
	        cam->scene.bar_disparity_f, cfg->distort ? "virtual parallel frame" : "frame");
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

	// Spec v8: a still in capture pauses the frames (a real source reconfigures);
	// at its due time it is rendered at the still size and frames resume.
	if (cam->still_pending) {
		if (now < cam->still_due_ns) {
			int64_t left = cam->still_due_ns - now;
			sleep_ns(left < timeout_ns ? left : timeout_ns);
			return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT;
		}
		fake_render_still(cam);
		now = os_monotonic_get_ns();
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
	if (cam->cfg.distort) {
		sim_stereo_camera_render_gray_distorted(&cam->distorted, cam->seq, cam->gray, cam->gray_pitch);
	} else {
		sim_stereo_camera_render_gray(&cam->scene, cam->seq, cam->gray, cam->gray_pitch);
	}

	fake_update_capture_state(cam, due);

	memset(out, 0, sizeof(*out));
	out->sequence = cam->seq;
	out->time_ns = due; // the synthetic "exposure"
	out->time_is_exposure = true;
	out->width = 2 * cam->eye_w;
	out->height = cam->eye_h;
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
	free(cam->still_gray);
	free(cam->still_conv);
	sim_stereo_camera_distorted_fini(&cam->distorted);
	free(cam);
}


/*
 *
 * Spec v8 (§9b): modes, controls + capture state, stills.
 *
 */

uint32_t
sim_display_stereo_camera_enumerate_modes(struct xrt_plugin_instance *inst,
                                          uint32_t index,
                                          uint32_t capacity,
                                          struct xrt_plugin_stereo_camera_mode *out)
{
	(void)inst;
	const struct fake_config *cfg = fake_config();
	if (!cfg->enabled || index != 0) {
		return 0;
	}
	struct xrt_plugin_stereo_camera_mode modes[FAKE_MODE_COUNT_MAX];
	const uint32_t n = fake_modes(cfg, modes);
	for (uint32_t i = 0; i < n && i < capacity && out != NULL; i++) {
		const uint32_t sz = out[i].struct_size;
		memcpy(&out[i], &modes[i], sz < sizeof(modes[i]) ? sz : sizeof(modes[i])); // never past struct_size
		out[i].struct_size = sz;
	}
	return n;
}

xrt_result_t
sim_display_stereo_camera_set_mode(struct xrt_plugin_stereo_camera *cam, uint32_t mode_index)
{
	struct xrt_plugin_stereo_camera_mode modes[FAKE_MODE_COUNT_MAX];
	const uint32_t n = fake_modes(&cam->cfg, modes);
	if (mode_index >= n || (modes[mode_index].flags & XRT_PLUGIN_STEREO_CAMERA_MODE_STREAM) == 0) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	const uint32_t w = modes[mode_index].eye_width, h = modes[mode_index].eye_height;
	if (w == cam->eye_w && h == cam->eye_h) {
		return XRT_SUCCESS;
	}
	if (!fake_resize(cam, w, h)) {
		return XRT_ERROR_ALLOCATION;
	}
	U_LOG_W("sim_display: FAKE stereo camera now runs mode %u (%ux%u per eye)", mode_index, w, h);
	return XRT_SUCCESS;
}

xrt_result_t
sim_display_stereo_camera_get_control_caps(struct xrt_plugin_instance *inst,
                                           uint32_t index,
                                           struct xrt_plugin_stereo_camera_control_caps *out)
{
	(void)inst;
	const struct fake_config *cfg = fake_config();
	if (!cfg->enabled || index != 0 || out == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const uint32_t sz = out->struct_size;
	struct xrt_plugin_stereo_camera_control_caps c;
	memset(&c, 0, sizeof(c));
	c.supported_controls =
	    XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_MODE | XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE |
	    XRT_PLUGIN_STEREO_CAMERA_CONTROL_METERING_POINT | XRT_PLUGIN_STEREO_CAMERA_CONTROL_EXPOSURE_COMPENSATION;
	c.supported_focus_modes =
	    (1u << XRT_PLUGIN_STEREO_CAMERA_FOCUS_CONTINUOUS_AUTO) | (1u << XRT_PLUGIN_STEREO_CAMERA_FOCUS_AUTO_ONCE) |
	    (1u << XRT_PLUGIN_STEREO_CAMERA_FOCUS_MANUAL) | (1u << XRT_PLUGIN_STEREO_CAMERA_FOCUS_LOCKED);
	c.min_focus_distance_diopters = (float)FAKE_MIN_FOCUS_D;
	c.focus_distance_calibration = XRT_PLUGIN_STEREO_CAMERA_FOCUS_APPROXIMATE;
	c.exposure_compensation_min_ev = -2.0f;
	c.exposure_compensation_max_ev = 2.0f;
	c.exposure_compensation_step_ev = 1.0f / 3.0f;
	memcpy(out, &c, sz < sizeof(c) ? sz : sizeof(c));
	out->struct_size = sz;
	return XRT_SUCCESS;
}

/*!
 * The scene's depth under the metering point (normalised in the raw left eye):
 * the near bar or the background. The lens focuses there on AUTO_ONCE.
 */
static double
fake_depth_at(const struct xrt_plugin_stereo_camera *cam, float nx, float ny)
{
	const struct sim_stereo_camera_scene *s = &cam->scene;
	const double x = nx * s->eye_width, y = ny * s->eye_height;
	const bool bar = x >= s->bar_x0 && x < s->bar_x1 && y >= s->bar_y0 && y < s->bar_y1;
	return bar ? FAKE_BAR_DEPTH_M : FAKE_BG_DEPTH_M;
}

xrt_result_t
sim_display_stereo_camera_set_controls(struct xrt_plugin_stereo_camera *cam,
                                       const struct xrt_plugin_stereo_camera_controls *controls)
{
	if (cam == NULL || controls == NULL) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	struct xrt_plugin_stereo_camera_controls c;
	memset(&c, 0, sizeof(c));
	memcpy(&c, controls, controls->struct_size < sizeof(c) ? controls->struct_size : sizeof(c));
	const bool trigger = c.focus_trigger != cam->trigger_seen;
	cam->trigger_seen = c.focus_trigger;
	cam->ctl = c;
	// Where the lens goes. Unset focus mode = the source's own default (continuous AF on the scene).
	const uint32_t mode = (c.valid_controls & XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_MODE)
	                          ? c.focus_mode
	                          : (uint32_t)XRT_PLUGIN_STEREO_CAMERA_FOCUS_CONTINUOUS_AUTO;
	const bool point = (c.valid_controls & XRT_PLUGIN_STEREO_CAMERA_CONTROL_METERING_POINT) != 0;
	switch (mode) {
	case XRT_PLUGIN_STEREO_CAMERA_FOCUS_AUTO_ONCE:
		if (trigger) {
			cam->focus_target =
			    1.0 / fake_depth_at(cam, point ? c.metering_x : 0.5f, point ? c.metering_y : 0.5f);
		}
		break;
	case XRT_PLUGIN_STEREO_CAMERA_FOCUS_MANUAL:
		cam->focus_target = (c.valid_controls & XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE)
		                        ? c.focus_distance_diopters
		                        : cam->focus_d;
		break;
	case XRT_PLUGIN_STEREO_CAMERA_FOCUS_LOCKED: cam->focus_target = cam->focus_d; break;
	default: // CONTINUOUS_AUTO: what the metering point (else the centre) looks at
		cam->focus_target = 1.0 / fake_depth_at(cam, point ? c.metering_x : 0.5f, point ? c.metering_y : 0.5f);
		break;
	}
	return XRT_SUCCESS;
}

//! Move the lens towards its target (first-order lag) and record the frame's capture state.
static void
fake_update_capture_state(struct xrt_plugin_stereo_camera *cam, int64_t t_ns)
{
	if (cam->focus_ns > 0 && t_ns > cam->focus_ns) {
		const double a = 1.0 - exp(-(double)(t_ns - cam->focus_ns) / FAKE_FOCUS_TAU_NS);
		cam->focus_d += (cam->focus_target - cam->focus_d) * a;
	}
	cam->focus_ns = t_ns;
	const bool locked_mode = (cam->ctl.valid_controls & XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_MODE) != 0 &&
	                         cam->ctl.focus_mode == XRT_PLUGIN_STEREO_CAMERA_FOCUS_LOCKED;
	cam->searching = !locked_mode && fabs(cam->focus_target - cam->focus_d) > 0.01;
	const double ev = (cam->ctl.valid_controls & XRT_PLUGIN_STEREO_CAMERA_CONTROL_EXPOSURE_COMPENSATION)
	                      ? cam->ctl.exposure_compensation_ev
	                      : 0.0;
	struct xrt_plugin_stereo_camera_capture_result *r = &cam->result;
	memset(r, 0, sizeof(*r));
	r->struct_size = (uint32_t)sizeof(*r);
	r->valid_fields = XRT_PLUGIN_STEREO_CAMERA_RESULT_FOCUS_DISTANCE | XRT_PLUGIN_STEREO_CAMERA_RESULT_FOCUS_STATE |
	                  XRT_PLUGIN_STEREO_CAMERA_RESULT_EXPOSURE_TIME | XRT_PLUGIN_STEREO_CAMERA_RESULT_SENSITIVITY |
	                  XRT_PLUGIN_STEREO_CAMERA_RESULT_EYE_TIMES;
	r->focus_distance_diopters = (float)cam->focus_d;
	r->focus_state = cam->searching ? XRT_PLUGIN_STEREO_CAMERA_FOCUS_STATE_SEARCHING
	                                : XRT_PLUGIN_STEREO_CAMERA_FOCUS_STATE_LOCKED;
	r->exposure_time_ns = (int64_t)(FAKE_BASE_EXPOSURE_NS * pow(2.0, ev));
	r->sensitivity_iso = 100;
	r->eye_time_ns[0] = t_ns;
	r->eye_time_ns[1] = t_ns + 200000; // the right sensor starts 0.2 ms later
}

xrt_result_t
sim_display_stereo_camera_get_capture_result(struct xrt_plugin_stereo_camera *cam,
                                             struct xrt_plugin_stereo_camera_capture_result *out)
{
	if (cam == NULL || out == NULL || cam->result.struct_size == 0) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const uint32_t sz = out->struct_size;
	memcpy(out, &cam->result, sz < sizeof(cam->result) ? sz : sizeof(cam->result));
	out->struct_size = sz;
	return XRT_SUCCESS;
}

xrt_result_t
sim_display_stereo_camera_request_still(struct xrt_plugin_stereo_camera *cam,
                                        const struct xrt_plugin_stereo_camera_still_request *req)
{
	if (cam == NULL || req == NULL || cam->still_pending || cam->still_ready) {
		return XRT_ERROR_STEREO_CAMERA_BUSY;
	}
	struct xrt_plugin_stereo_camera_mode modes[FAKE_MODE_COUNT_MAX];
	const uint32_t n = fake_modes(&cam->cfg, modes);
	bool listed = false;
	for (uint32_t i = 0; i < n; i++) {
		listed |= modes[i].eye_width == req->eye_width && modes[i].eye_height == req->eye_height &&
		          (modes[i].flags & XRT_PLUGIN_STEREO_CAMERA_MODE_STILL) != 0;
	}
	if (!listed) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	cam->still_w = req->eye_width;
	cam->still_h = req->eye_height;
	cam->still_request = req->request_id;
	cam->still_due_ns = os_monotonic_get_ns() + (int64_t)FAKE_STILL_LATENCY_NS;
	cam->still_pending = true;
	return XRT_SUCCESS;
}

//! Render the pending still at its size (camera thread, from wait_frame).
static void
fake_render_still(struct xrt_plugin_stereo_camera *cam)
{
	cam->still_pending = false;
	const uint32_t w = cam->still_w, h = cam->still_h;
	struct sim_stereo_camera_scene scene;
	sim_stereo_camera_scene_init(&scene, w, h, fake_fx(w), cam->cfg.baseline_mm, FAKE_BG_DEPTH_M, FAKE_BAR_DEPTH_M);
	uint8_t *gray = realloc(cam->still_gray, (size_t)2 * w * h);
	if (gray == NULL) {
		return; // poll reports FAILED (not ready, not pending)
	}
	cam->still_gray = gray;
	if (cam->cfg.distort) {
		// The distorted fake's still goes through the same raw lenses, at the
		// still size, so the service rectifies it with the rescaled calibration.
		struct sim_stereo_camera_distorted d;
		memset(&d, 0, sizeof(d));
		if (!sim_stereo_camera_distorted_init(&d, w, h, fake_fx(w), cam->cfg.baseline_mm, FAKE_BG_DEPTH_M,
		                                      FAKE_BAR_DEPTH_M)) {
			return;
		}
		const double s = (double)w / (double)cam->cfg.eye_w;
		sim_stereo_camera_distorted_set_vmisalign(&d, cam->cfg.extra_dy_px * s,
		                                          cam->cfg.extra_dy_slope_100 / 100.0);
		sim_stereo_camera_render_gray_distorted(&d, cam->seq + 1, cam->still_gray, 2 * w);
		sim_stereo_camera_distorted_fini(&d);
	} else {
		sim_stereo_camera_render_gray(&scene, cam->seq + 1, cam->still_gray, 2 * w);
	}
	if (cam->cfg.format != XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8) {
		struct u_stereo_camera_planes lay;
		if (!u_stereo_camera_layout(cam->cfg.format, 2 * w, h, &lay)) {
			return;
		}
		uint8_t *conv = realloc(cam->still_conv, (size_t)lay.size);
		if (conv == NULL) {
			return;
		}
		cam->still_conv = conv;
		cam->still_layout = lay;
		const uint8_t *src[2] = {cam->still_gray, NULL};
		const uint32_t pitches[2] = {2 * w, 0};
		u_stereo_camera_convert(XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8, src, pitches, cam->cfg.format,
		                        cam->still_conv, &lay, 2 * w, h);
	}
	cam->still_time_ns = os_monotonic_get_ns();
	fake_update_capture_state(cam, cam->still_time_ns);
	cam->still_result = cam->result;
	cam->still_ready = true;
	U_LOG_W("sim_display: FAKE stereo camera still %llu captured (%ux%u per eye)",
	        (unsigned long long)cam->still_request, w, h);
}

uint32_t
sim_display_stereo_camera_poll_still(struct xrt_plugin_stereo_camera *cam,
                                     struct xrt_plugin_stereo_camera_still *out,
                                     struct xrt_plugin_stereo_camera_capture_result *out_result)
{
	if (cam == NULL) {
		return XRT_PLUGIN_STEREO_CAMERA_STILL_FAILED;
	}
	if (cam->still_pending) {
		return XRT_PLUGIN_STEREO_CAMERA_STILL_NONE;
	}
	if (!cam->still_ready) {
		return XRT_PLUGIN_STEREO_CAMERA_STILL_FAILED; // nothing pending: it failed to render
	}
	const uint32_t w = cam->still_w, h = cam->still_h;
	const uint32_t sz = out->struct_size;
	struct xrt_plugin_stereo_camera_still st;
	memset(&st, 0, sizeof(st));
	st.width = 2 * w;
	st.height = h;
	st.format = cam->cfg.format;
	if (cam->cfg.format == XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8) {
		st.planes[0] = cam->still_gray;
		st.pitches[0] = 2 * w;
	} else {
		for (uint32_t p = 0; p < cam->still_layout.plane_count; p++) {
			st.planes[p] = cam->still_conv + cam->still_layout.offset[p];
			st.pitches[p] = cam->still_layout.pitch[p];
		}
	}
	st.time_ns = cam->still_time_ns;
	st.time_is_exposure = 1;
	memcpy(out, &st, sz < sizeof(st) ? sz : sizeof(st));
	out->struct_size = sz;
	if (out_result != NULL) {
		const uint32_t rs = out_result->struct_size;
		memcpy(out_result, &cam->still_result, rs < sizeof(cam->still_result) ? rs : sizeof(cam->still_result));
		out_result->struct_size = rs;
	}
	return XRT_PLUGIN_STEREO_CAMERA_STILL_READY;
}

void
sim_display_stereo_camera_release_still(struct xrt_plugin_stereo_camera *cam)
{
	if (cam != NULL) {
		cam->still_ready = false; // the buffers stay allocated for the next still
	}
}
