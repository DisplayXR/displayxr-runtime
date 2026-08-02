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
 * Motion script (period 4 s, radius 8 cm, circle centered per hand):
 *   position: center + r*(cos θ, sin θ, 0), θ = 2π·t/period + phase.
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

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_device.h"
#include "util/u_logging.h"

#include "sim_input_interface.h"

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

#define SIM_INPUT_CIRCLE_RADIUS_M 0.08f
#define SIM_INPUT_CIRCLE_PERIOD_S 4.0

struct sim_input_device
{
	struct xrt_device base;

	//! Circle center for this hand.
	struct xrt_pose center;

	//! Per-hand phase offset in radians (interleaves the two hands).
	double phase_rad;

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
	case XRT_INPUT_SIMPLE_AIM_POSE: break;
	default:
		U_LOG_XDEV_UNSUPPORTED_INPUT(&sd->base, u_log_get_global_level(), name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	if (!sd->active) {
		out_relation->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
		out_relation->relation_flags = 0;
		return XRT_SUCCESS;
	}

	/*
	 * The pose is analytic in the requested timestamp, so prediction for
	 * future frame times is exact — the property a real provider gets
	 * from m_relation_history + m_predict, delivered here by
	 * construction.
	 */
	const double omega = 2.0 * M_PI / SIM_INPUT_CIRCLE_PERIOD_S; // rad/s
	double t = (double)at_timestamp_ns * 1e-9;
	double theta = omega * t + sd->phase_rad;

	float r = SIM_INPUT_CIRCLE_RADIUS_M;
	float c = (float)cos(theta);
	float s = (float)sin(theta);

	struct xrt_pose pose = sd->center;
	pose.position.x += r * c;
	pose.position.y += r * s;

	out_relation->pose = pose;
	out_relation->linear_velocity = (struct xrt_vec3){
	    (float)(-(double)r * omega) * s,
	    (float)((double)r * omega) * c,
	    0.0f,
	};
	out_relation->angular_velocity = (struct xrt_vec3)XRT_VEC3_ZERO;

	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	    XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);

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
    XRT_INPUT_SIMPLE_SELECT_CLICK,
    XRT_INPUT_SIMPLE_MENU_CLICK,
    XRT_INPUT_SIMPLE_GRIP_POSE,
    XRT_INPUT_SIMPLE_AIM_POSE,
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
sim_input_create_controller(enum xrt_device_type type)
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
	sd->base.get_hand_tracking = u_device_ni_get_hand_tracking;
	sd->base.get_view_poses = u_device_ni_get_view_poses;
	sd->base.set_output = sim_input_set_output;
	sd->base.destroy = sim_input_destroy;
	sd->base.supported.orientation_tracking = true;
	sd->base.supported.position_tracking = true;
	sd->base.supported.hand_tracking = false;
	sd->base.name = XRT_DEVICE_SIMPLE_CONTROLLER;
	sd->base.device_type = type;

	snprintf(sd->base.str, sizeof(sd->base.str), "Simulated %s Motion Controller", is_left ? "Left" : "Right");
	snprintf(sd->base.serial, sizeof(sd->base.serial), "SIM-INPUT-%s", is_left ? "L" : "R");

	for (uint32_t i = 0; i < input_count; i++) {
		sd->base.inputs[i].active = true;
		sd->base.inputs[i].name = sim_input_inputs_array[i];
	}
	for (uint32_t i = 0; i < output_count; i++) {
		sd->base.outputs[i].name = sim_input_outputs_array[i];
	}

	// Circle centers mirror qwerty's initial controller placement:
	// ±0.2 m lateral, chest height in front of the display.
	sd->center = (struct xrt_pose){
	    .orientation = {0.0f, 0.0f, 0.0f, 1.0f},
	    .position = {is_left ? -0.2f : 0.2f, 1.3f, -0.5f},
	};
	// Half a revolution apart, so the hands interleave visibly (and the
	// button scripts alternate).
	sd->phase_rad = is_left ? M_PI : 0.0;
	sd->active = true;

	u_var_add_root(sd, sd->base.str, true);
	u_var_add_pose(sd, &sd->center, "center");
	u_var_add_bool(sd, &sd->active, "active");
	u_var_add_ro_u64(sd, &sd->haptic_event_count, "haptic_event_count");
	u_var_add_ro_f32(sd, &sd->last_haptic_amplitude, "last_haptic_amplitude");
	u_var_add_ro_f32(sd, &sd->last_haptic_frequency, "last_haptic_frequency");

	return &sd->base;
}
