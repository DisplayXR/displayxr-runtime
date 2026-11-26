// Copyright 2025, The DisplayXR Project
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
#include "sim_display_modes.h"
#include "sim_display_eyes.h"
#include "sim_display_settings.h"
#include "sim_display_monitor_geometry.h"

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_config_os.h"
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

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


/*
 *
 * Global runtime-switchable output mode.
 *
 */

//! The output the DP draws right now: PASSTHROUGH while slot 0 (2D) is active,
//! the slot-1 entry's kind while slot 1 is (ADR-065, ADR-066 C1). Written by
//! the device at creation and on every OUTPUT_MODE request; read by the DPs
//! each frame.
//!
//! The default config (OUTPUT unset) is SAME AS 2D: slot 1 is one sharp,
//! full-resolution view (project lead, 2026-11-23 - an anaglyph on an ordinary
//! screen looks broken, so a 3D look is chosen on purpose). It is
//! position-preserving, as anaglyph, interlaced and blend are, so it composes
//! with display zones. The rearranging kinds (SBS / Squeezed / Quad) are
//! view-inspection tools. The DEVICE starts in slot 1 (the app picks its
//! mode; ADR-065 Amendment 1).
static xrt_atomic_s32_t g_sim_display_output_mode = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
static xrt_atomic_s32_t g_sim_display_view_count = 1;
//! The slot-1 entry's kind (ADR-066 C1), published for the DPs'
//! sim_display_output_for_tiles(). Follows a live config switch (written on
//! every CONFIGURED_3D_MODE answer and every OUTPUT_MODE request for slot 1).
//! Same as 2D until a device exists.
static xrt_atomic_s32_t g_sim_display_kind = SIM_DISPLAY_DEFAULT_KIND;
//! The live device's hmd parts (ADR-065, F6), so every consumer resolves the
//! output from the ACTIVE MODE - which any runtime writer may set directly,
//! without an OUTPUT_MODE request - rather than from the last request. Set at
//! creation, cleared at destroy; one sim head device per process.
static struct xrt_hmd_parts *volatile g_sim_live_hmd = NULL;

//! Panel metrics published for the DP variants (#856). Written once in
//! sim_display_hmd_create() before any display processor exists, then read-only
//! — the DP needs these to answer get_display_dimensions/get_display_pixel_info,
//! which is what lets the compositor build WINDOW-scoped (not display-scoped)
//! Kooima. Without them the compositor's get_window_metrics bails and every
//! non-display-aspect viewport (e.g. a tall display-zone) renders stretched.
static float g_sim_panel_w_m = 0.0f;
static float g_sim_panel_h_m = 0.0f;
static uint32_t g_sim_panel_px_w = 0;
static uint32_t g_sim_panel_px_h = 0;

void
sim_display_get_panel_metrics(float *out_w_m, float *out_h_m, uint32_t *out_px_w, uint32_t *out_px_h)
{
	if (out_w_m != NULL) *out_w_m = g_sim_panel_w_m;
	if (out_h_m != NULL) *out_h_m = g_sim_panel_h_m;
	if (out_px_w != NULL) *out_px_w = g_sim_panel_px_w;
	if (out_px_h != NULL) *out_px_h = g_sim_panel_px_h;
}

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
		        mode == SIM_DISPLAY_OUTPUT_PASSTHROUGH    ? "2D" :
		        mode == SIM_DISPLAY_OUTPUT_SBS            ? "SBS" :
		        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH       ? "Anaglyph" :
		        mode == SIM_DISPLAY_OUTPUT_SQUEEZED_SBS   ? "Squeezed SBS" :
		        mode == SIM_DISPLAY_OUTPUT_QUAD           ? "Quad" :
		        mode == SIM_DISPLAY_OUTPUT_INTERLACED     ? "Interlaced" : "Blend");
	}
}

enum sim_display_output_mode
sim_display_resolved_output(void)
{
	struct xrt_hmd_parts *hmd = g_sim_live_hmd;
	return sim_display_output_resolve(hmd != NULL, hmd != NULL ? hmd->active_rendering_mode_index : 0,
	                                  (enum sim_display_output_mode)xrt_atomic_s32_load(&g_sim_display_kind),
	                                  sim_display_get_output_mode());
}

enum sim_display_output_mode
sim_display_output_for_tiles(uint32_t tile_count)
{
	return sim_display_output_draw(tile_count, sim_display_resolved_output(),
	                               (enum sim_display_output_mode)xrt_atomic_s32_load(&g_sim_display_kind));
}

uint32_t
sim_display_get_view_count(void)
{
	return (uint32_t)xrt_atomic_s32_load(&g_sim_display_view_count);
}

bool
sim_display_fake_tracking_enabled(void)
{
	// Dev-only knob (#441), read through the host iface since N3c
	// (`dp set sim-display TRACKING 1`, or DXR_DP_SIM_DISPLAY_TRACKING=1 in
	// the hosting process; SIM_DISPLAY_FAKE_TRACKING stays an alias for one
	// release). The shim caches it per process.
	return sim_display_setting_on("TRACKING");
}

bool
sim_display_fake_tracking_is_tracking(void)
{
	return sim_display_fake_tracking_sample(NULL);
}

bool
sim_display_fake_tracking_sample(uint64_t *out_edges)
{
	return sim_display_fake_tracking_sample_at((int64_t)os_monotonic_get_ns(), out_edges);
}

bool
sim_display_fake_tracking_sample_at(int64_t target_ns, uint64_t *out_edges)
{
	if (out_edges != NULL) {
		*out_edges = 0;
	}
	if (!sim_display_fake_tracking_enabled()) {
		return false; // Honest: sim_display never really tracks.
	}

	static int64_t period_ms = -1;
	if (period_ms < 0) {
		period_ms = (int64_t)sim_display_setting_num("TRACKING_PERIOD_MS", 0);
		if (period_ms < 0) {
			period_ms = 0;
		}
	}
	// Toggle on, no period: always tracking; else the square wave at the target.
	return sim_fake_tracking_at(period_ms, target_ns, out_edges);
}

bool
sim_display_eyes_at(struct xrt_eye_positions *out,
                    uint32_t view_count,
                    float nominal_x_m,
                    float nominal_y_m,
                    float nominal_z_m,
                    float ipd_m,
                    int64_t target_ns,
                    bool *webcam_dp_ref)
{
	// The sway knobs (N6 Part 1): read once through the host iface
	// (`dp set sim-display TRACKING_SWAY_MM 20`). Only a fake-tracked viewer
	// moves — with TRACKING off the eyes are the static nominal viewer
	// sim_display always reported.
	static float sway_m = -1.0f;
	static int64_t sway_period_ns = 0;
	if (sway_m < 0.0f) {
		float mm = sim_display_setting_float("TRACKING_SWAY_MM", 0.0f);
		if (!(mm >= 0.0f) || mm > 500.0f) {
			mm = 0.0f;
		}
		long period_ms = sim_display_setting_num("TRACKING_SWAY_PERIOD_MS", 4000);
		if (period_ms < 100) {
			period_ms = 100;
		}
		sway_period_ns = (int64_t)period_ms * 1000000;
		sway_m = mm / 1000.0f;
	}

	struct sim_fake_eyes_params p = {
	    .nominal_x_m = nominal_x_m,
	    .nominal_y_m = nominal_y_m,
	    .nominal_z_m = nominal_z_m,
	    .ipd_m = ipd_m,
	    .sway_m = sim_display_fake_tracking_enabled() ? sway_m : 0.0f,
	    .sway_period_ns = sway_period_ns,
	};
	sim_fake_eyes_at(&p, view_count, target_ns, out);
	out->is_tracking = sim_display_fake_tracking_sample_at(target_ns, NULL); // false unless TRACKING (#441)
	// #1855: opt-in webcam tracking overrides the synthetic eyes (no-op unless
	// WEBCAM_TRACKING; a screen-bound segment DP passes NULL and stays nominal).
	sim_display_webcam_tracking_apply_at(out, nominal_x_m, nominal_y_m, nominal_z_m, ipd_m, target_ns,
	                                     webcam_dp_ref);
	return true;
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

/*
 * Every knob below is read through the host iface (N3c, `sim_display_settings.h`:
 * `dp set sim-display <KEY> <value>` / DXR_DP_SIM_DISPLAY_<KEY>; the former
 * SIM_DISPLAY_<KEY> environment names stay aliases for one release), cached
 * per process exactly like the DEBUG_GET_ONCE options they replaced.
 */
#define debug_get_float_option_sim_display_width_m() sim_display_setting_float("WIDTH_M", 0.344f)
#define debug_get_float_option_sim_display_height_m() sim_display_setting_float("HEIGHT_M", 0.194f)
#define debug_get_float_option_sim_display_nominal_z_m() sim_display_setting_float("NOMINAL_Z_M", 0.60f)
#define debug_get_num_option_sim_display_pixel_w() sim_display_setting_num("PIXEL_W", 1920)
#define debug_get_num_option_sim_display_pixel_h() sim_display_setting_num("PIXEL_H", 1080)
//! Dev/test knob: pin the active rendering mode — 0 = 2D, 1 = the 3D kind that
//! OUTPUT names — overriding the start-up mode AND every later mode request
//! (see the guard in sim_display_hmd_set_property). -1 (unset) = normal
//! behaviour; any other value is ignored with a WARN. This is plug-in-wide: it
//! holds ANY sim_display app in the chosen view layout, so e.g. a zones app can
//! be forced to the 4-view Quad kind (OUTPUT=quad FORCE_MODE=1) for atlas
//! inspection despite the session-begin floor that would otherwise select 2D.
#define debug_get_num_option_sim_display_force_mode() sim_display_setting_num("FORCE_MODE", -1)

//! FORCE_MODE as the device applies it: a pin only for 0 or 1 (ADR-065).
static bool
sim_display_force_mode_pins(void)
{
	const long f = debug_get_num_option_sim_display_force_mode();
	return f == 0 || f == 1;
}


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

	//! The kind slot 1 draws (ADR-066 C1): the entry OUTPUT names. Follows a
	//! live config switch. Private driver state - the runtime maps slot 1 to
	//! the declared entry itself.
	enum sim_display_output_mode kind;

	//! The listed kinds, in the driver's own numbering (entry 0 = the 2D
	//! entry), and the count (8; Quad is START_UP_ONLY unless OUTPUT=quad at start).
	enum sim_display_output_mode entry_kinds[XRT_MAX_RENDERING_MODES];
	uint32_t entry_count;

	//! The last OUTPUT value WARNed about (unknown, or Quad not declared), so a
	//! polled config WARNs once per value.
	char warned_output[32];

	//! Quad was declared at start-up (OUTPUT=quad), not just listed.
	bool quad_declared;

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

	if (g_sim_live_hmd == hmd->base.hmd) {
		g_sim_live_hmd = NULL;
	}
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
		out_poses[i].position.x = xdev->hmd->view_eye_offsets[i].x;
		out_poses[i].position.y = xdev->hmd->view_eye_offsets[i].y - hmd->pose.position.y;
		out_poses[i].position.z = xdev->hmd->view_eye_offsets[i].z - hmd->pose.position.z;
	}

	// Kooima projection: recompute FOV each frame from tracked eye position.
	float zs = hmd->zoom_scale;
	if (zs < 0.001f) {
		zs = 1.0f;
	}

	// Screen dims depend on current output mode (atomic, can change mid-frame: V / 0 / 1 switch 2D <-> the 3D
	// kind).
	bool sbs_mode = (sim_display_resolved_output() == SIM_DISPLAY_OUTPUT_SBS);
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
		// SIM_DISPLAY_FORCE_MODE pins the active mode: swallow every later
		// request so the forced view layout survives the session-begin default
		// (oxr_session.c), the compositor, app xrRequestDisplayModeDXR calls,
		// and zone reconciliation. Report success so callers don't treat it as
		// an error; one-shot WARN so the override is visible in the log.
		if (sim_display_force_mode_pins()) {
			static bool warned_force = false;
			if (!warned_force) {
				warned_force = true;
				U_LOG_W("sim_display: ignoring OUTPUT_MODE=%d request — "
				        "SIM_DISPLAY_FORCE_MODE pins the active mode",
				        value);
			}
			return XRT_SUCCESS;
		}

		// ADR-066 C1: the value is an entry of the DECLARED list, in this
		// driver's numbering (the runtime's wrapper translates slot 1 to the
		// entry the config maps it to). 0 = 2D.
		struct sim_display_hmd *sd = sim_display_hmd(xdev);
		if (value < 0 || (uint32_t)value >= sd->entry_count) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}

		const struct sim_display_kind_desc d = sim_display_kind_desc_get(sd->entry_kinds[value]);
		xdev->hmd->active_rendering_mode_index = (uint32_t)value;
		if (value >= 1) {
			sd->kind = sd->entry_kinds[value];
			xrt_atomic_s32_exchange(&g_sim_display_kind, (int32_t)sd->kind);
		}
		sim_display_set_view_count(value == 0 ? 1u : d.view_count);
		sim_display_set_output_mode(value == 0 ? SIM_DISPLAY_OUTPUT_PASSTHROUGH : sd->kind);
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
	struct sim_display_hmd *sd = sim_display_hmd(xdev);
	if (property == XRT_DEVICE_PROPERTY_OUTPUT_MODE) {
		// The entry in effect, in this driver's numbering: 0 in slot 0, the
		// slot-1 entry in slot 1 (the wrapper translates it back).
		const uint32_t slot1 = sim_display_entry_for_kind(sd->kind, sd->entry_count);
		*out_value = (xdev->hmd->active_rendering_mode_index == 0 || slot1 == 0) ? 0 : (int32_t)slot1;
		return XRT_SUCCESS;
	}
	if (property == XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE) {
		// ADR-066 C1.5: the entry the OUTPUT config names NOW - re-read, not
		// the value cached at creation, so `dp set sim-display OUTPUT <x>` (and
		// the Dashboard) applies live. The runtime polls this and remaps slot 1.
		char buf[32];
		const char *v = sim_display_setting_reread("OUTPUT", buf, sizeof(buf)) ? buf : NULL;
		enum sim_display_output_mode want = SIM_DISPLAY_DEFAULT_KIND;
		const bool known = sim_display_kind_parse(v, &want);
		uint32_t entry = known ? sim_display_entry_for_kind(want, sd->entry_count) : 0;
		if (entry != 0 && want == SIM_DISPLAY_OUTPUT_QUAD && !sd->quad_declared) {
			entry = 0; // listed START_UP_ONLY: not selectable on this run
		}
		const char *shown = v != NULL ? v : "";
		if (entry == 0) {
			if (strcmp(sd->warned_output, shown) != 0) {
				snprintf(sd->warned_output, sizeof(sd->warned_output), "%s", shown);
				if (!known) {
					U_LOG_W("sim_display: unknown OUTPUT '%s' - slot 1 stays '%s'", shown,
					        sim_display_kind_desc_get(sd->kind).mode_name);
				} else {
					U_LOG_W(
					    "sim_display: OUTPUT=%s applies at the next start - Quad (4 views) is "
					    "declared "
					    "only when selected at start-up, so PRIMARY_MULTIVIEW_DXR sessions locate "
					    "2 views "
					    "otherwise (ADR-066 C1, interim rule); slot 1 stays '%s'",
					    shown, sim_display_kind_desc_get(sd->kind).mode_name);
				}
			}
			entry = sim_display_entry_for_kind(sd->kind, sd->entry_count);
		} else {
			sd->warned_output[0] = '\0';
			sd->kind = want;
			xrt_atomic_s32_exchange(&g_sim_display_kind, (int32_t)want);
		}
		if (entry == 0) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		*out_value = (int32_t)entry;
		return XRT_SUCCESS;
	}
	if (property == XRT_DEVICE_PROPERTY_SBS_MODE) {
		*out_value = (sim_display_resolved_output() == SIM_DISPLAY_OUTPUT_SBS) ? 1 : 0;
		return XRT_SUCCESS;
	}
	if (property == XRT_DEVICE_PROPERTY_OUTPUT_MODE_PINNED) {
		// SIM_DISPLAY_FORCE_MODE pins the active mode; the state tracker reads
		// this to honor the held mode over an app OUTPUT_MODE request.
		*out_value = sim_display_force_mode_pins() ? 1 : 0;
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
	// ADR-066 C1: the config (OUTPUT) names slot 1's entry; it is read here for
	// the start-up selection and re-read live through CONFIGURED_3D_MODE. The
	// start-up pin (FORCE_MODE) is read once, here.
	enum sim_display_output_mode kind = SIM_DISPLAY_DEFAULT_KIND;
	struct sim_display_start start;
	{
		char mode_buf[32];
		const char *mode_str = sim_display_setting("OUTPUT", mode_buf, sizeof(mode_buf)) ? mode_buf : NULL;
		if (!sim_display_kind_parse(mode_str, &kind)) {
			U_LOG_W(
			    "sim_display: unknown OUTPUT '%s' (want "
			    "default|same_as_2d|anaglyph|sbs|squeezed|quad|blend|"
			    "interlaced) - keeping the Same-as-2D default",
			    mode_str);
		}
		const long force = debug_get_num_option_sim_display_force_mode();
		start = sim_display_resolve_start(force);
		if (sim_display_setting_on("START_3D")) {
			U_LOG_W(
			    "sim_display: START_3D is deprecated and has no effect - the device starts in slot 1 "
			    "and the app picks its mode (ADR-065 Amendment 1); FORCE_MODE is the developer pin");
		}
		if (start.force_ignored) {
			U_LOG_W(
			    "sim_display: SIM_DISPLAY_FORCE_MODE=%ld ignored — it takes 0 (2D) or 1 (the 3D slot "
			    "OUTPUT names); -1 = off",
			    force);
		}
		sim_display_set_output_mode(start.active == 1 ? kind : SIM_DISPLAY_OUTPUT_PASSTHROUGH);
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

#ifdef XRT_OS_WINDOWS
	// Auto-detect the primary monitor's NATIVE pixel resolution so the
	// simulated panel matches the real display the workspace window is
	// created on. The compositor sizes the atlas to this reported
	// resolution but assumes atlas == panel (true for real vendor plug-ins
	// like Leia SR, which report the physical 4K size). Without this,
	// sim_display's hardcoded 1920x1080 default makes the runtime compose a
	// half-size atlas onto the full panel, leaving workspace content in the
	// top-left quadrant (#369). EnumDisplaySettingsW returns true pixels
	// regardless of this process's DPI-awareness. Explicit env overrides
	// (SIM_DISPLAY_PIXEL_W/H) still win.
	{
		DEVMODEW dm = {0};
		dm.dmSize = sizeof(dm);
		if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth > 0 &&
		    dm.dmPelsHeight > 0) {
			if (!sim_display_setting_is_set("PIXEL_W")) {
				pixel_w = (int)dm.dmPelsWidth;
			}
			if (!sim_display_setting_is_set("PIXEL_H")) {
				pixel_h = (int)dm.dmPelsHeight;
			}
		}
	}
#endif

#ifdef XRT_OS_LINUX_DESKTOP
	// Desktop-Linux twin of the Windows block above (#369 class): adopt the
	// real monitor's DEVICE pixels and EDID physical size, from the same
	// RandR <-> DRM-sysfs records the plug-in loader logs as "monitor N
	// output=... connector=..." (aux_os, statically linked into this
	// plug-in, so no runtime internals). Device pixels, not the scaled
	// logical desktop size: the service window presents 1:1 device pixels,
	// so a 2880x1800 eDP at 167% is a 2880x1800 panel, not 3456x2160 (and
	// never the fixed 1920x1080). Explicit env overrides still win, per
	// variable. The nominal viewer (SIM_DISPLAY_NOMINAL_Z_M, 10 cm eye
	// height) is left alone, as on Windows and macOS.
	{
		struct os_display_edid_list mons;
		struct sim_display_monitor_geometry geo;
		if (os_display_edid_enumerate(&mons) && sim_display_pick_monitor_geometry(&mons, &geo)) {
			const struct os_display_edid_monitor *m = &mons.monitors[geo.index];
			if (!sim_display_setting_is_set("PIXEL_W")) {
				pixel_w = (int)geo.pixel_width;
			}
			if (!sim_display_setting_is_set("PIXEL_H")) {
				pixel_h = (int)geo.pixel_height;
			}
			if (geo.physical_width_mm > 0 && !sim_display_setting_is_set("WIDTH_M")) {
				display_w_m = (float)geo.physical_width_mm / 1000.0f;
			}
			if (geo.physical_height_mm > 0 && !sim_display_setting_is_set("HEIGHT_M")) {
				display_h_m = (float)geo.physical_height_mm / 1000.0f;
			}
			U_LOG_W(
			    "sim_display: adopting monitor %u (%s) output='%s' connector='%s' %ux%u px (%s) %ux%u mm "
			    "-> panel %dx%d px, %.3fx%.3f m (SIM_DISPLAY_* env overrides win)",
			    geo.index, geo.reason, m->output_name, m->connector, geo.pixel_width, geo.pixel_height,
			    geo.pixel_is_native ? "connector mode" : "desktop mode", geo.physical_width_mm,
			    geo.physical_height_mm, pixel_w, pixel_h, display_w_m, display_h_m);
		} else {
			U_LOG_W("sim_display: no monitor enumerated, keeping %dx%d px, %.3fx%.3f m", pixel_w, pixel_h,
			        display_w_m, display_h_m);
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
	// #856: publish the same geometry to the DP variants (see the globals).
	g_sim_panel_w_m = hmd->display_width_m;
	g_sim_panel_h_m = hmd->display_height_m;
	g_sim_panel_px_w = hmd->display_pixel_width;
	g_sim_panel_px_h = hmd->display_pixel_height;

	hmd->ipd_m = ipd;
	hmd->zoom_scale = 1.0f;
	hmd->kind = kind;
	xrt_atomic_s32_exchange(&g_sim_display_kind, (int32_t)kind);
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

	// Rendering modes (ADR-066 C1): the FULL declared list (sim_display_modes.h
	// has the table). The runtime keeps all of it as the sizing envelope and
	// shows apps two slots: 0 = 2D, 1 = the entry OUTPUT names.
	//
	// Per-mode tracking capability (#441): sim_display has no real eye
	// tracker, so every entry stays untracked unless a source is on: under the
	// dev-only TRACKING toggle the 3D entries claim HAS_TRACKING so the MANUAL
	// path + tracking-state event are exercisable without hardware (matches
	// the MANUAL_BIT the plug-in then advertises); opt-in webcam tracking
	// (#1855) makes them tracked for real (MANAGED). Per entry through
	// sim_display_entry_tracked(), the seam the per-screen tracking setting
	// will drive (ADR-066 Part B).
	hmd->entry_count =
	    sim_display_fill_declared(kind == SIM_DISPLAY_OUTPUT_QUAD,
	                              sim_display_fake_tracking_enabled() || sim_display_webcam_tracking_enabled(),
	                              hmd->base.rendering_modes, hmd->entry_kinds);
	hmd->base.rendering_mode_count = hmd->entry_count;
	hmd->quad_declared = kind == SIM_DISPLAY_OUTPUT_QUAD;
	const uint32_t kind_entry = sim_display_entry_for_kind(kind, hmd->entry_count);

	// view_count = max across the declared list (the envelope)
	{
		uint32_t max_views = 1;
		for (uint32_t m = 0; m < hmd->base.rendering_mode_count; m++) {
			if ((hmd->base.rendering_modes[m].mode_flags & XRT_RENDERING_MODE_FLAG_START_UP_ONLY) == 0 &&
			    hmd->base.rendering_modes[m].view_count > max_views)
				max_views = hmd->base.rendering_modes[m].view_count;
		}
		hmd->base.hmd->view_count = max_views;
	}

	// Start-up mode (ADR-065 Amendment 1): slot 1; FORCE_MODE (0/1) pins it and
	// the set_property guard above then holds it against every later request
	// (plug-in-wide, any app).
	if (start.pinned) {
		U_LOG_W(
		    "sim_display: SIM_DISPLAY_FORCE_MODE=%u — pinning active mode "
		    "'%s' (view_count=%u); later mode requests are ignored",
		    start.active, hmd->base.rendering_modes[start.active == 1 ? kind_entry : 0].mode_name,
		    hmd->base.rendering_modes[start.active == 1 ? kind_entry : 0].view_count);
	}
	// The driver's own numbering: slot 1 at start is the configured entry, so
	// a 3D start marks that entry active (the runtime's selection translates it).
	const uint32_t start_entry = start.active == 1 ? kind_entry : 0u;
	hmd->base.hmd->active_rendering_mode_index = start_entry;
	g_sim_live_hmd = hmd->base.hmd;
	sim_display_set_view_count(hmd->base.rendering_modes[start_entry].view_count);
	U_LOG_W(
	    "sim_display: %u declared rendering modes (ADR-066 C1); config OUTPUT -> slot 1 = '%s' (%u views)%s; "
	    "starting in slot %u",
	    hmd->entry_count, hmd->base.rendering_modes[kind_entry].mode_name,
	    hmd->base.rendering_modes[kind_entry].view_count,
	    hmd->quad_declared ? ", Quad declared (OUTPUT=quad at start-up)" : ", Quad listed start-up-only",
	    start.active);

	// Initialize per-view eye offsets based on max view_count.
	// Stored on xrt_hmd_parts so the state tracker can read them generically
	// to synthesize per-tile viewer positions for Quad/N-view modes.
	{
		float half_ipd = ipd / 2.0f;
		struct xrt_vec3 *off = hmd->base.hmd->view_eye_offsets;
		// Views 0-1: standard stereo layout
		off[0] = (struct xrt_vec3){-half_ipd, eye_y, eye_z};
		off[1] = (struct xrt_vec3){ half_ipd, eye_y, eye_z};
		// Views 2-3: quad mode upper row (offset Y positions)
		off[2] = (struct xrt_vec3){-half_ipd, eye_y + ipd, eye_z};
		off[3] = (struct xrt_vec3){ half_ipd, eye_y + ipd, eye_z};
		// Pad remaining with center position
		for (uint32_t v = 4; v < XRT_MAX_VIEWS; v++) {
			off[v] = (struct xrt_vec3){0, eye_y, eye_z};
		}
	}

	// Head pose input.
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	// Manual display setup (replaces u_device_setup_split_side_by_side which only handles 2 views)
	// Advertise both OPAQUE (default) and ALPHA_BLEND so apps that want
	// per-pixel transparency (sim_display is alpha-native end-to-end on
	// Metal/D3D11/D3D12/GL via the cocoa_window_binding/win32_window_binding
	// transparentBackgroundEnabled bit) can opt-in via
	// xrEndFrame's environmentBlendMode. OPAQUE stays first so it's the
	// default for apps that don't pick.
	hmd->base.hmd->blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	hmd->base.hmd->blend_modes[1] = XRT_BLEND_MODE_ALPHA_BLEND;
	hmd->base.hmd->blend_mode_count = 2;
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
		bool sbs_mode = (sim_display_resolved_output() == SIM_DISPLAY_OUTPUT_SBS);
		const float half_w = (sbs_mode ? display_w_m / 2.0f : display_w_m) / 2.0f;
		const float half_h = display_h_m / 2.0f;

		for (uint32_t i = 0; i < hmd->base.hmd->view_count; i++) {
			float eye_x = hmd->base.hmd->view_eye_offsets[i].x;
			float eye_y_fov = hmd->base.hmd->view_eye_offsets[i].y;
			float eye_z_fov = hmd->base.hmd->view_eye_offsets[i].z;
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

	// These two are NOT one rectangle in two units, and calling them
	// logical/physical implied they were. On macOS `pixel_w/h` is the VISIBLE
	// FRAME in logical points (menu bar and Dock excluded — it sizes the
	// NSWindow and the advertised screen/view dims) while display_pixel_* is
	// the FULL panel in physical pixels. Dividing one by the other to recover
	// the backing scale yields 2.0 horizontally and ~2.39 vertically on a
	// Retina display, i.e. a phantom. Label each by what it is.
#ifdef XRT_OS_MACOS
	const char *screen_units = "pt (visible frame)";
#else
	const char *screen_units = "px";
#endif
	U_LOG_W("Created sim 3D display: screen=%dx%d %s, panel=%ux%u px, %.3fx%.3f m, eye=(0,%.2f,%.2f) IPD=%.0fmm",
	        pixel_w, pixel_h, screen_units, hmd->display_pixel_width, hmd->display_pixel_height,
	        display_w_m, display_h_m, eye_y, eye_z, ipd * 1000.0f);

	return &hmd->base;
}


/*
 *
 * Per-monitor defaults (multi-screen M1).
 *
 */

void
sim_display_get_default_viewer(float *out_w_m, float *out_h_m, float *out_nominal_y_m, float *out_nominal_z_m)
{
	// The same env-configurable defaults sim_display_hmd_create() starts from,
	// and the same 10 cm eye height it gives its static pose.
	if (out_w_m != NULL)
		*out_w_m = debug_get_float_option_sim_display_width_m();
	if (out_h_m != NULL)
		*out_h_m = debug_get_float_option_sim_display_height_m();
	if (out_nominal_y_m != NULL)
		*out_nominal_y_m = 0.1f;
	if (out_nominal_z_m != NULL)
		*out_nominal_z_m = debug_get_float_option_sim_display_nominal_z_m();
}
