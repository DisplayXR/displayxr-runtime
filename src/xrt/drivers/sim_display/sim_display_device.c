// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulated 3D display HMD device.
 *
 * Creates an xrt_device that simulates a tracked 3D display with
 * configurable physical dimensions. Used for development/testing
 * without real 3D display hardware.
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_var.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#endif


/*
 *
 * Global runtime-switchable output mode.
 *
 */

static xrt_atomic_s32_t g_sim_display_output_mode = SIM_DISPLAY_OUTPUT_SBS;

/*!
 * Cross-platform atomic load for xrt_atomic_s32_t.
 */
static inline int32_t
xrt_atomic_s32_load(xrt_atomic_s32_t *p)
{
#if defined(__GNUC__)
	return __sync_add_and_fetch(p, 0);
#elif defined(_MSC_VER)
	return InterlockedCompareExchange((volatile LONG *)p, 0, 0);
#else
#error "compiler not supported"
#endif
}

/*!
 * Cross-platform atomic exchange for xrt_atomic_s32_t.
 */
static inline int32_t
xrt_atomic_s32_exchange(xrt_atomic_s32_t *p, int32_t val)
{
#if defined(__GNUC__)
	int32_t old;
	do {
		old = *p;
	} while (__sync_val_compare_and_swap(p, old, val) != old);
	return old;
#elif defined(_MSC_VER)
	return InterlockedExchange((volatile LONG *)p, val);
#else
#error "compiler not supported"
#endif
}

enum sim_display_output_mode
sim_display_get_output_mode(void)
{
	return (enum sim_display_output_mode)xrt_atomic_s32_load(&g_sim_display_output_mode);
}

void
sim_display_set_output_mode(enum sim_display_output_mode mode)
{
	enum sim_display_output_mode old = (enum sim_display_output_mode)xrt_atomic_s32_exchange(&g_sim_display_output_mode, (int)mode);
	if (old != mode) {
		U_LOG_W("Sim display mode changed: %s",
		        mode == SIM_DISPLAY_OUTPUT_SBS ? "SBS" :
		        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH ? "Anaglyph" : "Blend");
	}
}


/*
 *
 * Environment variable configuration.
 *
 */

DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_width_m, "SIM_DISPLAY_WIDTH_M", 0.344f)
DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_height_m, "SIM_DISPLAY_HEIGHT_M", 0.194f)
DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_nominal_z_m, "SIM_DISPLAY_NOMINAL_Z_M", 0.60f)
DEBUG_GET_ONCE_NUM_OPTION(sim_display_pixel_w, "SIM_DISPLAY_PIXEL_W", 1920)
DEBUG_GET_ONCE_NUM_OPTION(sim_display_pixel_h, "SIM_DISPLAY_PIXEL_H", 1080)


/*
 *
 * Structs and helpers.
 *
 */

/*!
 * Simulated 3D display device.
 * @implements xrt_device
 * @ingroup drv_sim_display
 */
struct sim_display_hmd
{
	struct xrt_device base;

	//! Stationary pose (looking at the display from nominal distance).
	struct xrt_pose pose;

	//! Optional external device providing pose (e.g. qwerty HMD).
	//! When set, get_tracked_pose delegates to this device.
	struct xrt_device *pose_source;

	//! Physical display dimensions in meters.
	float display_width_m;
	float display_height_m;

	//! Nominal viewer distance in meters.
	float nominal_z_m;

	enum u_logging_level log_level;
};

static inline struct sim_display_hmd *
sim_display_hmd(struct xrt_device *xdev)
{
	return (struct sim_display_hmd *)xdev;
}


/*
 *
 * xrt_device interface methods.
 *
 */

static xrt_result_t
sim_display_hmd_get_tracked_pose(struct xrt_device *xdev,
                                 enum xrt_input_name name,
                                 int64_t at_timestamp_ns,
                                 struct xrt_space_relation *out_relation)
{
	struct sim_display_hmd *hmd = sim_display_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		U_LOG_E("Unknown input name: 0x%08x", name);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Orbit camera model using external pose source (e.g. qwerty HMD).
	// Rotation orbits the eye around the display center (origin) rather than
	// rotating in place (FPS-style). This matches the Windows app's camera
	// model in sr_cube_openxr_ext/LocateViews:
	//   worldPos = rotate(initialOffset / zoom, playerOri) + playerPos
	if (hmd->pose_source != NULL) {
		struct xrt_space_relation src_rel;
		hmd->pose_source->get_tracked_pose(hmd->pose_source, name, at_timestamp_ns, &src_rel);

		// initial_offset = nominal eye position relative to display center
		struct xrt_vec3 initial_offset = hmd->pose.position;

		// WASD displacement = how far the user has moved from the starting position
		struct xrt_vec3 wasd_offset = {
		    src_rel.pose.position.x - initial_offset.x,
		    src_rel.pose.position.y - initial_offset.y,
		    src_rel.pose.position.z - initial_offset.z,
		};

		// Orbit: rotate the initial offset around the display center (origin)
		struct xrt_vec3 orbit_pos;
		math_quat_rotate_vec3(&src_rel.pose.orientation, &initial_offset, &orbit_pos);

		// Final position = orbited eye position + WASD displacement
		out_relation->pose.position.x = orbit_pos.x + wasd_offset.x;
		out_relation->pose.position.y = orbit_pos.y + wasd_offset.y;
		out_relation->pose.position.z = orbit_pos.z + wasd_offset.z;
		out_relation->pose.orientation = src_rel.pose.orientation;
		out_relation->relation_flags = (enum xrt_space_relation_flags)(
		    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		    XRT_SPACE_RELATION_POSITION_VALID_BIT |
		    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

		return XRT_SUCCESS;
	}

	// Static pose: viewer at nominal position in front of the display.
	out_relation->pose = hmd->pose;
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	    XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

	return XRT_SUCCESS;
}

static void
sim_display_hmd_destroy(struct xrt_device *xdev)
{
	struct sim_display_hmd *hmd = sim_display_hmd(xdev);

	u_var_remove_root(hmd);
	u_device_free(&hmd->base);
}

void
sim_display_hmd_set_pose_source(struct xrt_device *sim_hmd, struct xrt_device *source)
{
	struct sim_display_hmd *hmd = sim_display_hmd(sim_hmd);
	hmd->pose_source = source;
}


/*
 *
 * Creation function.
 *
 */

struct xrt_device *
sim_display_hmd_create(void)
{
	// Read configuration from environment (used as defaults / overrides).
	float display_w_m = debug_get_float_option_sim_display_width_m();
	float display_h_m = debug_get_float_option_sim_display_height_m();
	float nominal_z = debug_get_float_option_sim_display_nominal_z_m();
	int pixel_w = (int)debug_get_num_option_sim_display_pixel_w();
	int pixel_h = (int)debug_get_num_option_sim_display_pixel_h();

#ifdef XRT_OS_MACOS
	// Auto-detect active Mac display physical size and resolution.
	{
		CGDirectDisplayID main_display = CGMainDisplayID();
		CGSize screen_mm = CGDisplayScreenSize(main_display);

		if (screen_mm.width > 0 && screen_mm.height > 0) {
			display_w_m = (float)screen_mm.width / 1000.0f;
			display_h_m = (float)screen_mm.height / 1000.0f;
		}

		uint32_t cg_w = CGDisplayPixelsWide(main_display);
		uint32_t cg_h = CGDisplayPixelsHigh(main_display);
		if (cg_w > 0 && cg_h > 0) {
			pixel_w = (int)cg_w;
			pixel_h = (int)cg_h;
		}
	}
#endif

	// Eye/camera configuration.
	const float ipd = 0.06f;       // 60mm inter-pupillary distance
	const float eye_y = 0.1f;      // 10cm above display center
	const float eye_z = nominal_z; // Viewing distance from display plane (Z=0)

	enum u_device_alloc_flags flags =
	    (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
	struct sim_display_hmd *hmd = U_DEVICE_ALLOCATE(struct sim_display_hmd, flags, 1, 0);

	// Store config.
	hmd->display_width_m = display_w_m;
	hmd->display_height_m = display_h_m;
	hmd->nominal_z_m = nominal_z;
	hmd->log_level = U_LOGGING_INFO;

	// xrt_device methods.
	hmd->base.update_inputs = u_device_noop_update_inputs;
	hmd->base.get_tracked_pose = sim_display_hmd_get_tracked_pose;
	hmd->base.get_view_poses = u_device_get_view_poses;
	hmd->base.get_visibility_mask = u_device_get_visibility_mask;
	hmd->base.destroy = sim_display_hmd_destroy;
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;

	// Static pose: center of eyes, slightly above scene, at viewing distance.
	hmd->pose.orientation.w = 1.0f;
	hmd->pose.position.x = 0.0f;
	hmd->pose.position.y = eye_y;
	hmd->pose.position.z = eye_z;

	hmd->base.hmd->view_count = 2;

	snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Sim 3D Display");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "sim_display_0");

	// Head pose input.
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	// Display geometry using helper struct.
	struct u_device_simple_info info;
	info.display.w_pixels = pixel_w;
	info.display.h_pixels = pixel_h;
	info.display.w_meters = display_w_m;
	info.display.h_meters = display_h_m;
	info.lens_horizontal_separation_meters = ipd;
	info.lens_vertical_position_meters = display_h_m / 2.0f;

	// Temporary symmetric FOV for u_device_setup_split_side_by_side
	// (will be overridden with Kooima asymmetric frustum below).
	float half_fov_h = atanf((display_w_m / 2.0f) / eye_z);
	info.fov[0] = half_fov_h * 2.0f;
	info.fov[1] = half_fov_h * 2.0f;

	bool setup_ok = u_device_setup_split_side_by_side(&hmd->base, &info);
	if (!setup_ok) {
		U_LOG_E("Failed to setup sim display device info");
		sim_display_hmd_destroy(&hmd->base);
		return NULL;
	}

	// Override display/viewport dimensions: all modes use the display processor
	// for final output, so each eye gets full-resolution swapchains.
	for (uint32_t i = 0; i < hmd->base.hmd->view_count; i++) {
		hmd->base.hmd->views[i].display.w_pixels = pixel_w;
		hmd->base.hmd->views[i].display.h_pixels = pixel_h;
		hmd->base.hmd->views[i].viewport.x_pixels = 0;
		hmd->base.hmd->views[i].viewport.w_pixels = pixel_w;
	}

	// Kooima off-axis asymmetric frustum per eye.
	// Display plane at Z=0, centered at origin. Each eye computes frustum
	// angles from its position to the display edges.
	{
		const float half_w = display_w_m / 2.0f;
		const float half_h = display_h_m / 2.0f;

		for (uint32_t i = 0; i < hmd->base.hmd->view_count; i++) {
			// Eye X offset: left eye negative, right eye positive.
			float eye_x = (i == 0) ? -ipd / 2.0f : ipd / 2.0f;

			// Frustum angles from eye to display edges (Kooima projection).
			// Display left edge at x = -half_w, right edge at x = +half_w.
			// Display bottom at y = -half_h, top at y = +half_h.
			hmd->base.hmd->distortion.fov[i].angle_left = atanf((-half_w - eye_x) / eye_z);
			hmd->base.hmd->distortion.fov[i].angle_right = atanf((half_w - eye_x) / eye_z);
			hmd->base.hmd->distortion.fov[i].angle_down = atanf((-half_h - eye_y) / eye_z);
			hmd->base.hmd->distortion.fov[i].angle_up = atanf((half_h - eye_y) / eye_z);
		}

		U_LOG_W("Kooima FOV (left eye): L=%.1f R=%.1f U=%.1f D=%.1f deg",
		        hmd->base.hmd->distortion.fov[0].angle_left * 180.0f / M_PI,
		        hmd->base.hmd->distortion.fov[0].angle_right * 180.0f / M_PI,
		        hmd->base.hmd->distortion.fov[0].angle_up * 180.0f / M_PI,
		        hmd->base.hmd->distortion.fov[0].angle_down * 180.0f / M_PI);
	}

	// No distortion for simulated display.
	u_distortion_mesh_set_none(&hmd->base);

	// Debug variables.
	u_var_add_root(hmd, "Sim 3D Display", true);
	u_var_add_pose(hmd, &hmd->pose, "pose");
	u_var_add_f32(hmd, &hmd->display_width_m, "display_width_m");
	u_var_add_f32(hmd, &hmd->display_height_m, "display_height_m");
	u_var_add_f32(hmd, &hmd->nominal_z_m, "nominal_z_m");
	u_var_add_log_level(hmd, &hmd->log_level, "log_level");

	U_LOG_W("Created sim 3D display: %dx%d px, %.3fx%.3f m, eye=(0,%.2f,%.2f) IPD=%.0fmm",
	        pixel_w, pixel_h, display_w_m, display_h_m, eye_y, eye_z, ipd * 1000.0f);

	return &hmd->base;
}
