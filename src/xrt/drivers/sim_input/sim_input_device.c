// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulated motion-controller device.
 *
 * Adapted from Monado's `simulated_controller.c` (restored from the
 * pre-strip history, #823): trimmed to the `khr/simple_controller`
 * profile and given deterministic motion — each controller sweeps a
 * circle in front of the display and plays a scripted button pattern,
 * both computed as pure functions of the monotonic clock. That makes
 * `get_tracked_pose` timestamp-correct for ANY requested time (past or
 * future) with no relation-history buffer, and makes CI runs
 * reproducible.
 *
 * Motion script (`sim_input_hand_script.c`, #2191): period 4 s, radius
 * 4 cm, one circle per hand in the DISPLAY PLANE's XY, centred
 * `HAND_DISTANCE_M` (0.25 m) in front of the glass and 12 cm to its side:
 *   position: center + r*(cos θ, sin θ, 0), θ = 2π·t/period + phase.
 * Published on a `XRT_TRACKING_TYPE_RIG_LOCAL` origin (display-plane
 * relative), so the hands land in front of the panel whatever the stage /
 * LOCAL convention (input-provider-discovery.md §4b.4).
 * Button script:
 *   select: 1 s pressed / 1 s released square wave (left offset by half
 *           a cycle so the hands interleave);
 *   menu:   pressed for the first 0.5 s of every 5 s.
 *
 * Haptics: `set_output` counts vibration events and records the last
 * amplitude/frequency (visible via u_var) — enough for an end-to-end
 * `xrApplyHapticFeedback` assertion without hardware.
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author David Fattal
 * @ingroup drv_sim_input
 */

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_mathinclude.h"

#include "util/u_grip_surface.h"
#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_hand_tracking.h"
#include "util/u_hand_simulation.h"

#include "sim_input_interface.h"
#include "sim_input_hand_script.h"

#include <assert.h>
#include <stdio.h>


/*
 *
 * Structs and defines.
 *
 */

// Indices into base.inputs, matching sim_input_inputs_array below.
#define SIM_INPUT_SELECT 0
#define SIM_INPUT_MENU 1
#define SIM_INPUT_GRIP 2
#define SIM_INPUT_AIM 3
#define SIM_INPUT_HT 4
//! #1633: `grip_surface` / `palm_ext`, derived from SIM_INPUT_GRIP.
#define SIM_INPUT_PALM 5

struct sim_input_device
{
	struct xrt_device base;

	//! Circle center for this hand.
	struct xrt_pose center;

	//! Per-hand phase offset in radians (interleaves the two hands).
	double phase_rad;

	//! Which hand this device serves (drives the simulated joint set).
	enum xrt_hand hand;

	//! #2191 `PINCH`: what the fingers of the simulated hand do.
	enum sim_input_pinch_mode pinch;

	bool active;

	//! Haptics bookkeeping — asserted by tests, browsable via u_var.
	uint64_t haptic_event_count;
	float last_haptic_amplitude;
	float last_haptic_frequency;
};


/*
 *
 * Helper functions.
 *
 */

static inline struct sim_input_device *
sim_input_device(struct xrt_device *xdev)
{
	return (struct sim_input_device *)xdev;
}

/*!
 * Scripted pose, a pure function of the requested timestamp — so
 * prediction for future frame times is exact, the property a real
 * provider gets from m_relation_history + m_predict, delivered here by
 * construction. Shared by the grip/aim poses and the hand-tracking root.
 */
static void
script_relation(const struct sim_input_device *sd, int64_t at_timestamp_ns, struct xrt_space_relation *out_relation)
{
	sim_input_hand_relation(&sd->center, sd->phase_rad, at_timestamp_ns, out_relation);
}

//! Scripted button state, a pure function of the monotonic time.
static void
script_buttons(const struct sim_input_device *sd, int64_t now_ns, bool *out_select, bool *out_menu)
{
	double t = (double)now_ns * 1e-9 + sd->phase_rad / (2.0 * M_PI) * SIM_INPUT_CIRCLE_PERIOD_S;

	// select: 1 s pressed / 1 s released.
	*out_select = fmod(t, 2.0) < 1.0;
	// menu: first 0.5 s of every 5 s.
	*out_menu = fmod(t, 5.0) < 0.5;
}


/*
 *
 * Member functions.
 *
 */

static void
sim_input_destroy(struct xrt_device *xdev)
{
	struct sim_input_device *sd = sim_input_device(xdev);

	u_var_remove_root(sd);
	u_device_free(&sd->base);
}

static xrt_result_t
sim_input_update_inputs(struct xrt_device *xdev)
{
	struct sim_input_device *sd = sim_input_device(xdev);

	int64_t now = (int64_t)os_monotonic_get_ns();

	if (!sd->active) {
		for (uint32_t i = 0; i < xdev->input_count; i++) {
			xdev->inputs[i].active = false;
			xdev->inputs[i].timestamp = now;
			U_ZERO(&xdev->inputs[i].value);
		}
		return XRT_SUCCESS;
	}

	bool select = false;
	bool menu = false;
	script_buttons(sd, now, &select, &menu);

	for (uint32_t i = 0; i < xdev->input_count; i++) {
		xdev->inputs[i].active = true;
		xdev->inputs[i].timestamp = now;
	}
	xdev->inputs[SIM_INPUT_SELECT].value.boolean = select;
	xdev->inputs[SIM_INPUT_MENU].value.boolean = menu;

	return XRT_SUCCESS;
}

static xrt_result_t
sim_input_get_tracked_pose(struct xrt_device *xdev,
                           enum xrt_input_name name,
                           int64_t at_timestamp_ns,
                           struct xrt_space_relation *out_relation)
{
	struct sim_input_device *sd = sim_input_device(xdev);

	switch (name) {
	case XRT_INPUT_SIMPLE_GRIP_POSE:
	case XRT_INPUT_SIMPLE_AIM_POSE:
	case XRT_INPUT_GENERIC_PALM_POSE: break;
	default:
		// Plain message, not U_LOG_XDEV_UNSUPPORTED_INPUT: the u_pp
		// helpers it uses aren't on the runtime DLL's aux export
		// surface, and pulling them statically drags u_logging.c into
		// the plug-in link where it collides with the exported
		// logging symbols (LNK2005). Same style as sim_display.
		U_LOG_E("sim_input: unsupported input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	if (!sd->active) {
		out_relation->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
		out_relation->relation_flags = 0;
		return XRT_SUCCESS;
	}

	script_relation(sd, at_timestamp_ns, out_relation);

	// #1633: the palm pose is the grip pose plus a fixed hand-specific
	// offset — one shared helper, never a per-driver constant.
	if (name == XRT_INPUT_GENERIC_PALM_POSE) {
		u_grip_surface_from_grip(sd->hand == XRT_HAND_LEFT, out_relation, out_relation);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
sim_input_get_hand_tracking(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            int64_t at_timestamp_ns,
                            struct xrt_hand_joint_set *out_value,
                            int64_t *out_timestamp_ns)
{
	struct sim_input_device *sd = sim_input_device(xdev);

	const enum xrt_input_name expected =
	    sd->hand == XRT_HAND_LEFT ? XRT_INPUT_HT_UNOBSTRUCTED_LEFT : XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;
	if (name != expected) {
		// Plain message — see the u_pp comment in get_tracked_pose.
		U_LOG_E("sim_input: unsupported hand-tracking input: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	U_ZERO(out_value);
	*out_timestamp_ns = at_timestamp_ns;

	if (!sd->active) {
		out_value->is_active = false;
		return XRT_SUCCESS;
	}

	// The hand root rides the same analytic circle as the grip pose, so
	// joints stay timestamp-correct for any requested time — and the
	// marker cubes an app renders from the grip action land inside the
	// rendered hand.
	struct xrt_space_relation root;
	script_relation(sd, at_timestamp_ns, &root);

	// #2191 `PINCH`: wave (the #825 curl wave), hold (thumb + index tips
	// held together — a sustained pinch whose root rides the circle, i.e.
	// a one-hand drag) or open.
	struct u_hand_tracking_curl_values curls;
	sim_input_hand_curls(sd->pinch, sd->phase_rad, at_timestamp_ns, &curls);

	u_hand_sim_simulate_for_valve_index_knuckles(&curls, sd->hand, &root, out_value);

	return XRT_SUCCESS;
}

static xrt_result_t
sim_input_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct sim_input_device *sd = sim_input_device(xdev);

	if (name != XRT_OUTPUT_NAME_SIMPLE_VIBRATION) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	sd->haptic_event_count++;
	sd->last_haptic_amplitude = value->vibration.amplitude;
	sd->last_haptic_frequency = value->vibration.frequency;

	U_LOG_XDEV_IFL_D(&sd->base, u_log_get_global_level(),
	                 "haptic event #%llu: amplitude=%.2f frequency=%.1f duration=%lld ns",
	                 (unsigned long long)sd->haptic_event_count, (double)value->vibration.amplitude,
	                 (double)value->vibration.frequency, (long long)value->vibration.duration_ns);

	return XRT_SUCCESS;
}


/*
 *
 * khr/simple_controller data arrays.
 *
 */

static enum xrt_input_name sim_input_inputs_array[] = {
    XRT_INPUT_SIMPLE_SELECT_CLICK,  XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_SIMPLE_AIM_POSE,
    XRT_INPUT_HT_UNOBSTRUCTED_LEFT, // SIM_INPUT_HT — patched to _RIGHT per hand in create.
    XRT_INPUT_GENERIC_PALM_POSE,    // SIM_INPUT_PALM — `grip_surface` / `palm_ext` (#1633).
};

static enum xrt_output_name sim_input_outputs_array[] = {
    XRT_OUTPUT_NAME_SIMPLE_VIBRATION,
};


/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_device *
sim_input_create_controller(enum xrt_device_type type, const struct sim_input_controller_config *config)
{
	bool is_left = false;
	switch (type) {
	case XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER: is_left = true; break;
	case XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER: is_left = false; break;
	default: assert(false && "sim_input: only left/right motion controllers"); return NULL;
	}

	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	const uint32_t input_count = ARRAY_SIZE(sim_input_inputs_array);
	const uint32_t output_count = ARRAY_SIZE(sim_input_outputs_array);

	struct sim_input_device *sd = U_DEVICE_ALLOCATE(struct sim_input_device, flags, input_count, output_count);
	sd->base.update_inputs = sim_input_update_inputs;
	sd->base.get_tracked_pose = sim_input_get_tracked_pose;
	sd->base.get_hand_tracking = sim_input_get_hand_tracking;
	sd->base.get_view_poses = u_device_ni_get_view_poses;
	sd->base.set_output = sim_input_set_output;
	sd->base.destroy = sim_input_destroy;
	sd->base.supported.orientation_tracking = true;
	sd->base.supported.position_tracking = true;
	// #825 Tier 2: a scripted 26-joint set (curl wave over the same
	// analytic circle) makes sim_input the hardware-free test vehicle
	// for the whole XR_EXT_hand_tracking path.
	sd->base.supported.hand_tracking = true;
	sd->base.name = XRT_DEVICE_SIMPLE_CONTROLLER;
	sd->base.device_type = type;
	sd->hand = is_left ? XRT_HAND_LEFT : XRT_HAND_RIGHT;

	// 6DOF-tracked, so the origin type must NOT stay TRACKING_TYPE_NONE:
	// u_builder_setup_tracking_origins injects per-hand "arm model"
	// offsets (±0.2, 1.3, −0.5) into NONE-typed origins, silently
	// shifting every reported pose. Same rule as qwerty.
	//
	// #2191: always DISPLAY-PLANE-relative (RIG_LOCAL, §4b.4). The builder
	// anchors the origin at the initial rig and the overseer composes the
	// rig delta, so `world = rig(t) o L` — the hands sit HAND_DISTANCE_M in
	// front of the glass in every reference space, with no stage height or
	// mount offset to guess. (#1380 introduced this frame for the NAV knob;
	// the stage-anchored circle it kept for NAV=0 sat behind the glass on
	// some setups and ~1.45 m off on others, #2191.)
	sd->base.tracking_origin->type = XRT_TRACKING_TYPE_RIG_LOCAL;
	snprintf(sd->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Sim Input Rig-Local Volume");

	snprintf(sd->base.str, sizeof(sd->base.str), "Simulated %s Motion Controller", is_left ? "Left" : "Right");
	snprintf(sd->base.serial, sizeof(sd->base.serial), "SIM-INPUT-%s", is_left ? "L" : "R");

	for (uint32_t i = 0; i < input_count; i++) {
		sd->base.inputs[i].active = true;
		sd->base.inputs[i].name = sim_input_inputs_array[i];
	}
	if (!is_left) {
		sd->base.inputs[SIM_INPUT_HT].name = XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;
	}
	for (uint32_t i = 0; i < output_count; i++) {
		sd->base.outputs[i].name = sim_input_outputs_array[i];
	}

	// #2191: circle centre in the display-plane frame — HAND_DISTANCE_M out
	// of the glass, 12 cm to this hand's side, slightly below centre. The
	// grip / aim / palm poses ride the same circle as the hand root, so
	// marker cubes an app draws from the grip action land inside the hand.
	sd->center = sim_input_hand_center(is_left, config != NULL ? config->hand_distance_m : 0.0f);
	sd->pinch = config != NULL ? config->pinch : SIM_INPUT_PINCH_WAVE;
	// Half a revolution apart, so the hands interleave visibly (and the
	// button scripts alternate).
	sd->phase_rad = is_left ? M_PI : 0.0;
	// #2191 `HANDS`: a hand left out stays a device (so role assignment and
	// the button-script phase are unchanged) but never tracks — inactive
	// inputs, invalid poses, `isActive` false on its joint set.
	sd->active = config != NULL ? config->active : true;

	u_var_add_root(sd, sd->base.str, true);
	u_var_add_pose(sd, &sd->center, "center");
	u_var_add_bool(sd, &sd->active, "active");
	u_var_add_ro_u64(sd, &sd->haptic_event_count, "haptic_event_count");
	u_var_add_ro_f32(sd, &sd->last_haptic_amplitude, "last_haptic_amplitude");
	u_var_add_ro_f32(sd, &sd->last_haptic_frequency, "last_haptic_frequency");

	return &sd->base;
}
