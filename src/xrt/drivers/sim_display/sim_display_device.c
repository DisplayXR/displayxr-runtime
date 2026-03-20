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
static xrt_atomic_s32_t g_sim_display_view_count = 2;

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
		        mode == SIM_DISPLAY_OUTPUT_SBS           ? "SBS" :
		        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH       ? "Anaglyph" :
		        mode == SIM_DISPLAY_OUTPUT_SQUEEZED_SBS   ? "Squeezed SBS" :
		        mode == SIM_DISPLAY_OUTPUT_QUAD           ? "Quad" :
		        mode == SIM_DISPLAY_OUTPUT_PASSTHROUGH    ? "Passthrough" : "Blend");
	}
}

uint32_t
sim_display_get_view_count(void)
{
	return (uint32_t)xrt_atomic_s32_load(&g_sim_display_view_count);
}

void
sim_display_set_view_count(uint32_t count)
{
	xrt_atomic_s32_exchange(&g_sim_display_view_count, (int32_t)count);
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

	//! EXT app mode: return raw hmd->pose without qwerty compose.
	//! Set when the session has an external window handle.
	bool ext_app_mode;

	//! Physical display dimensions in meters.
	float display_width_m;
	float display_height_m;

	//! Native display panel resolution in pixels.
	uint32_t display_pixel_width;
	uint32_t display_pixel_height;

	//! Nominal viewer distance in meters.
	float nominal_z_m;

	//! Inter-pupillary distance in meters.
	float ipd_m;

	//! Zoom scale factor (1.0 = no zoom). Divides both eye position
	//! and screen dimensions for Kooima projection. Ready for future
	//! scroll-wheel zoom support.
	float zoom_scale;

	//! Per-view eye position offsets for Kooima FOV computation.
	struct xrt_vec3 view_eye_offsets[8]; // XRT_MAX_VIEWS

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

	// EXT app mode: return raw eye position in display space (no qwerty compose).
	// The external app owns the virtual display model and expects raw positions.
	if (hmd->ext_app_mode) {
		out_relation->pose = hmd->pose;
		out_relation->relation_flags = (enum xrt_space_relation_flags)(
		    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		    XRT_SPACE_RELATION_POSITION_VALID_BIT);
		return XRT_SUCCESS;
	}

	// Display pose from external source (e.g. qwerty HMD via WASD/mouse).
	// Returns the display center position and orientation in world space.
	// Eye offsets are NOT applied here — the Kooima block in oxr_session.c
	// computes world-space eye positions from display-space data.
	if (hmd->pose_source != NULL) {
		struct xrt_space_relation src_rel;
		hmd->pose_source->get_tracked_pose(hmd->pose_source, name, at_timestamp_ns, &src_rel);

		out_relation->pose = src_rel.pose;
		out_relation->relation_flags = (enum xrt_space_relation_flags)(
		    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		    XRT_SPACE_RELATION_POSITION_VALID_BIT);

		return XRT_SUCCESS;
	}

	// Static pose: viewer at nominal position in front of the display.
	out_relation->pose = hmd->pose;
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	    XRT_SPACE_RELATION_POSITION_VALID_BIT);

	return XRT_SUCCESS;
}

static void
sim_display_hmd_destroy(struct xrt_device *xdev)
{
	struct sim_display_hmd *hmd = sim_display_hmd(xdev);

	u_var_remove_root(hmd);
	u_device_free(&hmd->base);
}

/*!
 * Per-frame Kooima asymmetric frustum FOV.
 *
 * Matches the ext app's calculation: eye position and screen dimensions are
 * both divided by zoom_scale, and screen width is halved in SBS mode.
 * For fullscreen non-ext apps, the viewport-scaling step (vs = minDisp/minWin)
 * reduces to 1.0 since window == display.
 */
static xrt_result_t
sim_display_get_view_poses(struct xrt_device *xdev,
                           const struct xrt_vec3 *default_eye_relation,
                           int64_t at_timestamp_ns,
                           uint32_t view_count,
                           struct xrt_space_relation *out_head_relation,
                           struct xrt_fov *out_fovs,
                           struct xrt_pose *out_poses)
{
	struct sim_display_hmd *hmd = sim_display_hmd(xdev);

	// Get head tracking pose (display-space: origin at display center).
	xrt_result_t xret =
	    xrt_device_get_tracked_pose(xdev, XRT_INPUT_GENERIC_HEAD_POSE, at_timestamp_ns, out_head_relation);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// Per-view head-relative offset poses from stored eye offsets.
	for (uint32_t i = 0; i < view_count && i < ARRAY_SIZE(xdev->hmd->views); i++) {
		out_poses[i].orientation = (struct xrt_quat){0, 0, 0, 1};
		// View offset relative to head center
		out_poses[i].position.x = hmd->view_eye_offsets[i].x;
		out_poses[i].position.y = hmd->view_eye_offsets[i].y - hmd->pose.position.y;
		out_poses[i].position.z = hmd->view_eye_offsets[i].z - hmd->pose.position.z;
	}

	// Kooima projection: recompute FOV each frame from tracked eye position.
	float zs = hmd->zoom_scale;
	if (zs < 0.001f) {
		zs = 1.0f;
	}

	// Screen dims depend on current output mode (atomic, can change mid-frame via 1/2/3 keys).
	bool sbs_mode = (sim_display_get_output_mode() == SIM_DISPLAY_OUTPUT_SBS);
	float screen_w = (sbs_mode ? hmd->display_width_m / 2.0f : hmd->display_width_m) / zs;
	float screen_h = hmd->display_height_m / zs;
	float half_w = screen_w / 2.0f;
	float half_h = screen_h / 2.0f;

	for (uint32_t i = 0; i < view_count && i < ARRAY_SIZE(xdev->hmd->views); i++) {
		// Eye world position = head + per-eye offset, divided by zoom.
		float eye_x = (out_head_relation->pose.position.x + out_poses[i].position.x) / zs;
		float eye_y = (out_head_relation->pose.position.y + out_poses[i].position.y) / zs;
		float eye_z = (out_head_relation->pose.position.z + out_poses[i].position.z) / zs;

		if (eye_z <= 0.001f) {
			eye_z = hmd->nominal_z_m / zs;
		}

		out_fovs[i].angle_left = atanf((-half_w - eye_x) / eye_z);
		out_fovs[i].angle_right = atanf((half_w - eye_x) / eye_z);
		out_fovs[i].angle_up = atanf((half_h - eye_y) / eye_z);
		out_fovs[i].angle_down = atanf((-half_h - eye_y) / eye_z);
	}

	return XRT_SUCCESS;
}

void
sim_display_hmd_set_pose_source(struct xrt_device *sim_hmd, struct xrt_device *source)
{
	struct sim_display_hmd *hmd = sim_display_hmd(sim_hmd);
	hmd->pose_source = source;
}

void
sim_display_hmd_set_ext_app_mode(struct xrt_device *xdev, bool enabled)
{
	struct sim_display_hmd *hmd = sim_display_hmd(xdev);
	hmd->ext_app_mode = enabled;
}

bool
sim_display_get_display_info(struct xrt_device *xdev, struct sim_display_info *out_info)
{
	if (xdev == NULL || out_info == NULL) {
		return false;
	}
	if (strcmp(xdev->serial, "sim_display_0") != 0) {
		return false;
	}
	struct sim_display_hmd *hmd = sim_display_hmd(xdev);
	out_info->display_width_m = hmd->display_width_m;
	out_info->display_height_m = hmd->display_height_m;
	out_info->nominal_y_m = hmd->pose.position.y;
	out_info->nominal_z_m = hmd->nominal_z_m;
	out_info->display_pixel_width = hmd->display_pixel_width;
	out_info->display_pixel_height = hmd->display_pixel_height;
	out_info->zoom_scale = hmd->zoom_scale;
	return true;
}


/*
 *
 * Generic property interface.
 *
 */

static xrt_result_t
sim_display_hmd_set_property(struct xrt_device *xdev,
                              enum xrt_device_property_id property,
                              int32_t value)
{
	if (property == XRT_DEVICE_PROPERTY_OUTPUT_MODE) {
		if ((uint32_t)value >= xdev->rendering_mode_count) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}

		// Always update active rendering mode index and view count
		xdev->hmd->active_rendering_mode_index = (uint32_t)value;
		sim_display_set_view_count(xdev->rendering_modes[value].view_count);

		if (value == 0) {
			// 2D — switch weaver to passthrough mode
			sim_display_set_output_mode(SIM_DISPLAY_OUTPUT_PASSTHROUGH);
			return XRT_SUCCESS;
		}

		enum sim_display_output_mode internal_mode;
		switch (value) {
		case 1:
			internal_mode = SIM_DISPLAY_OUTPUT_ANAGLYPH;
			break;
		case 2:
			internal_mode = SIM_DISPLAY_OUTPUT_SBS;
			break;
		case 3:
			internal_mode = SIM_DISPLAY_OUTPUT_SQUEEZED_SBS;
			break;
		case 4:
			internal_mode = SIM_DISPLAY_OUTPUT_QUAD;
			break;
		default:
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		sim_display_set_output_mode(internal_mode);
		return XRT_SUCCESS;
	}
	if (property == XRT_DEVICE_PROPERTY_EXT_APP_MODE) {
		sim_display_hmd_set_ext_app_mode(xdev, value != 0);
		return XRT_SUCCESS;
	}
	return XRT_ERROR_NOT_IMPLEMENTED;
}

static xrt_result_t
sim_display_hmd_get_property(struct xrt_device *xdev,
                              enum xrt_device_property_id property,
                              int32_t *out_value)
{
	if (property == XRT_DEVICE_PROPERTY_OUTPUT_MODE) {
		// Return unified mode index: internal SBS(0)→2, Anaglyph(1)→1, Blend(2)→3
		enum sim_display_output_mode internal = sim_display_get_output_mode();
		switch (internal) {
		case SIM_DISPLAY_OUTPUT_SBS:          *out_value = 2; break;
		case SIM_DISPLAY_OUTPUT_ANAGLYPH:     *out_value = 1; break;
		case SIM_DISPLAY_OUTPUT_BLEND:        *out_value = 3; break;
		case SIM_DISPLAY_OUTPUT_SQUEEZED_SBS: *out_value = 3; break;
		case SIM_DISPLAY_OUTPUT_QUAD:         *out_value = 4; break;
		default:                              *out_value = 1; break;
		}
		return XRT_SUCCESS;
	}
	if (property == XRT_DEVICE_PROPERTY_SBS_MODE) {
		*out_value = (sim_display_get_output_mode() == SIM_DISPLAY_OUTPUT_SBS) ? 1 : 0;
		return XRT_SUCCESS;
	}
	return XRT_ERROR_NOT_IMPLEMENTED;
}


/*
 *
 * Creation function.
 *
 */

struct xrt_device *
sim_display_hmd_create(void)
{
	// Parse output mode from env var early so it's available when
	// target_instance.c queries sim_display_get_output_mode() for scale values.
	{
		const char *mode_str = getenv("SIM_DISPLAY_OUTPUT");
		if (mode_str) {
			if (strcmp(mode_str, "anaglyph") == 0)
				sim_display_set_output_mode(SIM_DISPLAY_OUTPUT_ANAGLYPH);
			else if (strcmp(mode_str, "blend") == 0)
				sim_display_set_output_mode(SIM_DISPLAY_OUTPUT_BLEND);
			else
				sim_display_set_output_mode(SIM_DISPLAY_OUTPUT_SBS);
		}
	}

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

		// Use visible frame (excluding menu bar, dock, title bar) so
		// the recommended swapchain matches the actual window size.
		extern void sim_display_macos_get_visible_frame(uint32_t *out_w, uint32_t *out_h);
		uint32_t vis_w = 0, vis_h = 0;
		sim_display_macos_get_visible_frame(&vis_w, &vis_h);
		if (vis_w > 0 && vis_h > 0) {
			pixel_w = (int)vis_w;
			pixel_h = (int)vis_h;
		}
	}
#endif

#ifdef XRT_OS_MACOS
	// Physical pixel dimensions for Retina displays.
	// display_pixel_width/height drive the render resolution and swapchain/atlas
	// sizing, while pixel_w/pixel_h (logical points) control the NSWindow size.
	extern float sim_display_macos_get_backing_scale(void);
	float backing_scale = sim_display_macos_get_backing_scale();

	// Full display resolution (not visible frame) for swapchain/atlas sizing.
	// The visible frame controls the initial window, but the render resolution
	// should match the full display so content doesn't degrade when the window
	// is resized to fill the screen.
	size_t full_display_w = CGDisplayPixelsWide(CGMainDisplayID());
	size_t full_display_h = CGDisplayPixelsHigh(CGMainDisplayID());
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
#ifdef XRT_OS_MACOS
	hmd->display_pixel_width = (uint32_t)(full_display_w * backing_scale);
	hmd->display_pixel_height = (uint32_t)(full_display_h * backing_scale);
#else
	hmd->display_pixel_width = (uint32_t)pixel_w;
	hmd->display_pixel_height = (uint32_t)pixel_h;
#endif
	hmd->ipd_m = ipd;
	hmd->zoom_scale = 1.0f;
	hmd->nominal_z_m = nominal_z;
	hmd->log_level = U_LOGGING_INFO;

	// xrt_device methods.
	hmd->base.update_inputs = u_device_noop_update_inputs;
	hmd->base.get_tracked_pose = sim_display_hmd_get_tracked_pose;
	hmd->base.get_view_poses = sim_display_get_view_poses;
	hmd->base.get_visibility_mask = u_device_get_visibility_mask;
	hmd->base.set_property = sim_display_hmd_set_property;
	hmd->base.get_property = sim_display_hmd_get_property;
	hmd->base.destroy = sim_display_hmd_destroy;
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;

	// Pose is delegated to the qwerty HMD (via pose_source), which already
	// includes the Y=1.6 standing height.  Mark our tracking origin as
	// OTHER so u_builder_setup_tracking_origins does NOT add a redundant
	// Y=1.6 offset — that would double-count the height and place
	// controllers (which share the qwerty origin) in a different space.
	hmd->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;

	// Static pose: center of eyes, slightly above scene, at viewing distance.
	hmd->pose.orientation.w = 1.0f;
	hmd->pose.position.x = 0.0f;
	hmd->pose.position.y = eye_y;
	hmd->pose.position.z = eye_z;

	snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Sim 3D Display");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "sim_display_0");

	// Rendering modes: sim_display supports 5 modes (2D + 3 stereo + quad).
	// Order: 0=2D, 1=Anaglyph (default 3D), 2=Cropped SBS, 3=Squeezed SBS, 4=Quad
	hmd->base.rendering_mode_count = 5;

	// Mode 0: 2D (mono, full resolution, 1×1 tile)
	hmd->base.rendering_modes[0].mode_index = 0;
	snprintf(hmd->base.rendering_modes[0].mode_name, XRT_DEVICE_NAME_LEN, "2D");
	hmd->base.rendering_modes[0].view_count = 1;
	hmd->base.rendering_modes[0].view_scale_x = 1.0f;
	hmd->base.rendering_modes[0].view_scale_y = 1.0f;
	hmd->base.rendering_modes[0].hardware_display_3d = false;
	hmd->base.rendering_modes[0].tile_columns = 1;
	hmd->base.rendering_modes[0].tile_rows = 1;

	// Mode 1: Anaglyph (stereo, default 3D, 2×1 tiles = side-by-side half-res)
	hmd->base.rendering_modes[1].mode_index = 1;
	snprintf(hmd->base.rendering_modes[1].mode_name, XRT_DEVICE_NAME_LEN, "Anaglyph");
	hmd->base.rendering_modes[1].view_count = 2;
	hmd->base.rendering_modes[1].view_scale_x = 0.5f;
	hmd->base.rendering_modes[1].view_scale_y = 0.5f;
	hmd->base.rendering_modes[1].hardware_display_3d = true;
	hmd->base.rendering_modes[1].tile_columns = 2;
	hmd->base.rendering_modes[1].tile_rows = 1;

	// Mode 2: Cropped SBS (stereo, 1×2 tiles = top-bottom half-res)
	hmd->base.rendering_modes[2].mode_index = 2;
	snprintf(hmd->base.rendering_modes[2].mode_name, XRT_DEVICE_NAME_LEN, "Cropped SBS");
	hmd->base.rendering_modes[2].view_count = 2;
	hmd->base.rendering_modes[2].view_scale_x = 0.5f;
	hmd->base.rendering_modes[2].view_scale_y = 0.5f;
	hmd->base.rendering_modes[2].hardware_display_3d = true;
	hmd->base.rendering_modes[2].tile_columns = 1;
	hmd->base.rendering_modes[2].tile_rows = 2;

	// Mode 3: Squeezed SBS (stereo, 2×1 tiles = side-by-side full-height)
	hmd->base.rendering_modes[3].mode_index = 3;
	snprintf(hmd->base.rendering_modes[3].mode_name, XRT_DEVICE_NAME_LEN, "Squeezed SBS");
	hmd->base.rendering_modes[3].view_count = 2;
	hmd->base.rendering_modes[3].view_scale_x = 0.5f;
	hmd->base.rendering_modes[3].view_scale_y = 1.0f;
	hmd->base.rendering_modes[3].hardware_display_3d = true;
	hmd->base.rendering_modes[3].tile_columns = 2;
	hmd->base.rendering_modes[3].tile_rows = 1;

	// Mode 4: Quad (4 views, 2x2 grid, each quadrant at half-res)
	hmd->base.rendering_modes[4].mode_index = 4;
	snprintf(hmd->base.rendering_modes[4].mode_name, XRT_DEVICE_NAME_LEN, "Quad");
	hmd->base.rendering_modes[4].view_count = 4;
	hmd->base.rendering_modes[4].view_scale_x = 0.5f;
	hmd->base.rendering_modes[4].view_scale_y = 0.5f;
	hmd->base.rendering_modes[4].hardware_display_3d = true;
	hmd->base.rendering_modes[4].tile_columns = 2;
	hmd->base.rendering_modes[4].tile_rows = 2;

	// view_count = max across all rendering modes
	{
		uint32_t max_views = 1;
		for (uint32_t m = 0; m < hmd->base.rendering_mode_count; m++) {
			if (hmd->base.rendering_modes[m].view_count > max_views)
				max_views = hmd->base.rendering_modes[m].view_count;
		}
		hmd->base.hmd->view_count = max_views;
	}

	// Set default active mode from env var
	{
		enum sim_display_output_mode sd_mode = sim_display_get_output_mode();
		// Map internal mode to unified index: SBS→2, Anaglyph→1, SqueezedSBS→3
		uint32_t default_mode;
		switch (sd_mode) {
		case SIM_DISPLAY_OUTPUT_SBS:          default_mode = 2; break;
		case SIM_DISPLAY_OUTPUT_ANAGLYPH:     default_mode = 1; break;
		case SIM_DISPLAY_OUTPUT_SQUEEZED_SBS: default_mode = 3; break;
		default:                              default_mode = 1; break;
		}
		hmd->base.hmd->active_rendering_mode_index = default_mode;
		sim_display_set_view_count(hmd->base.rendering_modes[default_mode].view_count);
	}

	// Initialize per-view eye offsets based on max view_count
	{
		float half_ipd = ipd / 2.0f;
		// Views 0-1: standard stereo layout
		hmd->view_eye_offsets[0] = (struct xrt_vec3){-half_ipd, eye_y, eye_z};
		hmd->view_eye_offsets[1] = (struct xrt_vec3){ half_ipd, eye_y, eye_z};
		// Views 2-3: quad mode upper row (offset Y positions)
		hmd->view_eye_offsets[2] = (struct xrt_vec3){-half_ipd, eye_y + ipd, eye_z};
		hmd->view_eye_offsets[3] = (struct xrt_vec3){ half_ipd, eye_y + ipd, eye_z};
		// Pad remaining with center position
		for (uint32_t v = 4; v < 8; v++) {
			hmd->view_eye_offsets[v] = (struct xrt_vec3){0, eye_y, eye_z};
		}
	}

	// Head pose input.
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	// Manual display setup (replaces u_device_setup_split_side_by_side which only handles 2 views)
	hmd->base.hmd->blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	hmd->base.hmd->blend_mode_count = 1;
	hmd->base.hmd->distortion.models = XRT_DISTORTION_MODEL_NONE;
	hmd->base.hmd->distortion.preferred = XRT_DISTORTION_MODEL_NONE;
	hmd->base.hmd->screens[0].w_pixels = pixel_w;
	hmd->base.hmd->screens[0].h_pixels = pixel_h;
	hmd->base.hmd->screens[0].nominal_frame_interval_ns = (uint64_t)time_s_to_ns(1.0f / 60.0f);

	// Override display/viewport dimensions: all modes use the display processor
	// for final output, so each eye gets full-resolution swapchains.
	for (uint32_t i = 0; i < hmd->base.hmd->view_count; i++) {
		hmd->base.hmd->views[i].display.w_pixels = pixel_w;
		hmd->base.hmd->views[i].display.h_pixels = pixel_h;
		hmd->base.hmd->views[i].viewport.x_pixels = 0;
		hmd->base.hmd->views[i].viewport.w_pixels = pixel_w;
	}

	// Kooima off-axis asymmetric frustum per view.
	// Display plane at Z=0, centered at origin. Each view computes frustum
	// angles from its eye position to the display edges.
	// In SBS mode each eye sees half the display width.
	{
		bool sbs_mode = (sim_display_get_output_mode() == SIM_DISPLAY_OUTPUT_SBS);
		const float half_w = (sbs_mode ? display_w_m / 2.0f : display_w_m) / 2.0f;
		const float half_h = display_h_m / 2.0f;

		for (uint32_t i = 0; i < hmd->base.hmd->view_count; i++) {
			float eye_x = hmd->view_eye_offsets[i].x;
			float eye_y_fov = hmd->view_eye_offsets[i].y;
			float eye_z_fov = hmd->view_eye_offsets[i].z;
			if (eye_z_fov <= 0.001f) eye_z_fov = nominal_z;

			hmd->base.hmd->distortion.fov[i].angle_left = atanf((-half_w - eye_x) / eye_z_fov);
			hmd->base.hmd->distortion.fov[i].angle_right = atanf((half_w - eye_x) / eye_z_fov);
			hmd->base.hmd->distortion.fov[i].angle_down = atanf((-half_h - eye_y_fov) / eye_z_fov);
			hmd->base.hmd->distortion.fov[i].angle_up = atanf((half_h - eye_y_fov) / eye_z_fov);
		}

		U_LOG_W("Kooima FOV (view 0): L=%.1f R=%.1f U=%.1f D=%.1f deg",
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

	U_LOG_W("Created sim 3D display: logical=%dx%d, physical=%ux%u, %.3fx%.3f m, eye=(0,%.2f,%.2f) IPD=%.0fmm",
	        pixel_w, pixel_h, hmd->display_pixel_width, hmd->display_pixel_height,
	        display_w_m, display_h_m, eye_y, eye_z, ipd * 1000.0f);

	return &hmd->base;
}
