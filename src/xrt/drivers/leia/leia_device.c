// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia 3D display HMD device.
 *
 * Creates an xrt_device representing a Leia light field display.
 * When SR SDK is available (Windows), queries the hardware for
 * pixel dimensions, refresh rate, and physical size.  Otherwise
 * falls back to sensible defaults.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_interface.h"

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
 * Structs and helpers.
 *
 */

/*!
 * Leia 3D display device.
 * @implements xrt_device
 * @ingroup drv_leia
 */
struct leia_hmd
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

static inline struct leia_hmd *
leia_hmd(struct xrt_device *xdev)
{
	return (struct leia_hmd *)xdev;
}


/*
 *
 * xrt_device interface methods.
 *
 */

static xrt_result_t
leia_hmd_get_tracked_pose(struct xrt_device *xdev,
                          enum xrt_input_name name,
                          int64_t at_timestamp_ns,
                          struct xrt_space_relation *out_relation)
{
	struct leia_hmd *hmd = leia_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		U_LOG_E("Unknown input name: 0x%08x", name);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Delegate to external pose source (e.g. qwerty HMD for WASD/mouse).
	if (hmd->pose_source != NULL) {
		struct xrt_space_relation src_rel;
		hmd->pose_source->get_tracked_pose(hmd->pose_source, name, at_timestamp_ns, &src_rel);

		out_relation->pose = src_rel.pose;
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
leia_hmd_destroy(struct xrt_device *xdev)
{
	struct leia_hmd *hmd = leia_hmd(xdev);

	u_var_remove_root(hmd);
	u_device_free(&hmd->base);
}

void
leia_hmd_set_pose_source(struct xrt_device *leia_dev, struct xrt_device *source)
{
	struct leia_hmd *hmd = leia_hmd(leia_dev);
	hmd->pose_source = source;
}


/*
 *
 * Creation function.
 *
 */

struct xrt_device *
leia_hmd_create(void)
{
	// Default values — used when SR SDK is not available.
	int pixel_w = 3840;
	int pixel_h = 2160;
	float refresh_hz = 60.0f;
	float display_w_m = 0.344f;
	float display_h_m = 0.194f;
	float nominal_z = 0.65f;

	// Use cached probe results from leiasr_probe_display() if available.
	{
		struct leiasr_probe_result probe;
		if (leiasr_get_probe_results(&probe) && probe.hw_found) {
			pixel_w = (int)probe.pixel_w;
			pixel_h = (int)probe.pixel_h;
			if (probe.refresh_hz > 0.0f) {
				refresh_hz = probe.refresh_hz;
			}
			display_w_m = probe.display_w_m;
			display_h_m = probe.display_h_m;
			if (probe.nominal_z_m > 0.0f) {
				nominal_z = probe.nominal_z_m;
			}
		}
	}

	enum u_device_alloc_flags flags =
	    (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
	struct leia_hmd *hmd = U_DEVICE_ALLOCATE(struct leia_hmd, flags, 1, 0);

	// Store config.
	hmd->display_width_m = display_w_m;
	hmd->display_height_m = display_h_m;
	hmd->nominal_z_m = nominal_z;
	hmd->log_level = U_LOGGING_INFO;

	// xrt_device methods.
	hmd->base.update_inputs = u_device_noop_update_inputs;
	hmd->base.get_tracked_pose = leia_hmd_get_tracked_pose;
	hmd->base.get_view_poses = u_device_get_view_poses;
	hmd->base.get_visibility_mask = u_device_get_visibility_mask;
	hmd->base.destroy = leia_hmd_destroy;
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;

	// Pose is delegated to the qwerty HMD (via pose_source), which already
	// includes the Y=1.6 standing height.  Mark our tracking origin as
	// OTHER so u_builder_setup_tracking_origins does NOT add a redundant
	// Y=1.6 offset — that would double-count the height and place
	// controllers (which share the qwerty origin) 1.6 m below the head.
	hmd->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;

	// Static pose: centered, at nominal viewing distance.
	hmd->pose.orientation.w = 1.0f;
	hmd->pose.position.z = -nominal_z; // Negative Z = looking at display

	hmd->base.hmd->view_count = 2;

	snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Leia 3D Display");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "leia_display_0");

	// Rendering modes: Leia exposes 2 modes (2D + LeiaSR).
	hmd->base.rendering_mode_count = 2;

	// Mode 0: 2D (mono, full resolution, 1×1 tile)
	hmd->base.rendering_modes[0].mode_index = 0;
	snprintf(hmd->base.rendering_modes[0].mode_name, XRT_DEVICE_NAME_LEN, "2D");
	hmd->base.rendering_modes[0].view_count = 1;
	hmd->base.rendering_modes[0].view_scale_x = 1.0f;
	hmd->base.rendering_modes[0].view_scale_y = 1.0f;
	hmd->base.rendering_modes[0].hardware_display_3d = false;
	hmd->base.rendering_modes[0].tile_columns = 1;
	hmd->base.rendering_modes[0].tile_rows = 1;

	// Mode 1: LeiaSR (stereo, 2×1 = SBS, scale from SR SDK — set later by target_instance)
	hmd->base.rendering_modes[1].mode_index = 1;
	snprintf(hmd->base.rendering_modes[1].mode_name, XRT_DEVICE_NAME_LEN, "LeiaSR");
	hmd->base.rendering_modes[1].view_count = 2;
	hmd->base.rendering_modes[1].view_scale_x = 0.5f; // Default, overridden by SR SDK
	hmd->base.rendering_modes[1].view_scale_y = 0.5f;
	hmd->base.rendering_modes[1].hardware_display_3d = true;
	hmd->base.rendering_modes[1].tile_columns = 2;
	hmd->base.rendering_modes[1].tile_rows = 1;

	hmd->base.hmd->active_rendering_mode_index = 1; // Default to LeiaSR (3D)

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
	float half_fov_h = atanf((display_w_m / 2.0f) / nominal_z);
	float half_fov_v = atanf((display_h_m / 2.0f) / nominal_z);
	info.fov[0] = half_fov_h * 2.0f;
	info.fov[1] = half_fov_h * 2.0f;

	(void)half_fov_v; // Used implicitly by u_device_setup_split_side_by_side

	bool setup_ok = u_device_setup_split_side_by_side(&hmd->base, &info);
	if (!setup_ok) {
		U_LOG_E("Failed to setup Leia display device info");
		leia_hmd_destroy(&hmd->base);
		return NULL;
	}

	// Advertise ALPHA_BLEND alongside the default OPAQUE that the device
	// helper installed. The Leia DPs deliver alpha-correct output two ways:
	//   - Standalone (XR_EXT_win32_window_binding + transparentBackgroundEnabled):
	//     WGC compose-under-bg + post-weave alpha-gate on D3D11/D3D12/VK.
	//   - Workspace (IPC): service compositor honours
	//     XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT per layer.
	// Advertising ALPHA_BLEND lets well-behaved apps discover transparency
	// support via the standard xrEnumerateEnvironmentBlendModes path instead
	// of relying on extension detection alone.
	hmd->base.hmd->blend_modes[hmd->base.hmd->blend_mode_count++] = XRT_BLEND_MODE_ALPHA_BLEND;

	// No distortion for Leia display.
	u_distortion_mesh_set_none(&hmd->base);

	// Debug variables.
	u_var_add_root(hmd, "Leia 3D Display", true);
	u_var_add_pose(hmd, &hmd->pose, "pose");
	u_var_add_f32(hmd, &hmd->display_width_m, "display_width_m");
	u_var_add_f32(hmd, &hmd->display_height_m, "display_height_m");
	u_var_add_f32(hmd, &hmd->nominal_z_m, "nominal_z_m");
	u_var_add_log_level(hmd, &hmd->log_level, "log_level");

	U_LOG_W("Created Leia 3D display: %dx%d px, %.3fx%.3f m, nominal Z=%.2f m, %.1f Hz",
	        pixel_w, pixel_h, display_w_m, display_h_m, nominal_z, refresh_hz);

	(void)refresh_hz; // Logged above; will be used for frame timing later.

	return &hmd->base;
}
