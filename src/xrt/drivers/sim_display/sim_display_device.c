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


/*
 *
 * Environment variable configuration.
 *
 */

DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_width_m, "SIM_DISPLAY_WIDTH_M", 0.344f)
DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_height_m, "SIM_DISPLAY_HEIGHT_M", 0.194f)
DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_nominal_z_m, "SIM_DISPLAY_NOMINAL_Z_M", 0.65f)
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
		return XRT_ERROR_DEVICE_NOT_FOUND;
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


/*
 *
 * Creation function.
 *
 */

struct xrt_device *
sim_display_hmd_create(void)
{
	// Read configuration from environment.
	float display_w_m = debug_get_float_option_sim_display_width_m();
	float display_h_m = debug_get_float_option_sim_display_height_m();
	float nominal_z = debug_get_float_option_sim_display_nominal_z_m();
	int pixel_w = (int)debug_get_num_option_sim_display_pixel_w();
	int pixel_h = (int)debug_get_num_option_sim_display_pixel_h();

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

	// Static pose: centered, at nominal viewing distance.
	hmd->pose.orientation.w = 1.0f;
	hmd->pose.position.z = -nominal_z; // Negative Z = looking at display

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
	info.lens_horizontal_separation_meters = 0.063f; // ~63mm IPD
	info.lens_vertical_position_meters = display_h_m / 2.0f;

	// Compute FOV from display geometry and viewing distance.
	// half_angle = atan(half_display_width / distance)
	float half_fov_h = atanf((display_w_m / 2.0f) / nominal_z);
	float half_fov_v = atanf((display_h_m / 2.0f) / nominal_z);
	info.fov[0] = half_fov_h * 2.0f;
	info.fov[1] = half_fov_h * 2.0f;

	(void)half_fov_v; // Used implicitly by u_device_setup_split_side_by_side

	bool setup_ok = u_device_setup_split_side_by_side(&hmd->base, &info);
	if (!setup_ok) {
		U_LOG_E("Failed to setup sim display device info");
		sim_display_hmd_destroy(&hmd->base);
		return NULL;
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

	U_LOG_W("Created sim 3D display: %dx%d px, %.3fx%.3f m, nominal Z=%.2f m",
	        pixel_w, pixel_h, display_w_m, display_h_m, nominal_z);

	return &hmd->base;
}
