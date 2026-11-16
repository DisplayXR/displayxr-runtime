// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulated 6-DoF stylus device.
 *
 * Every input of `/interaction_profiles/logitech/mx_ink_stylus_logitech`
 * (tip / middle force, front / back click + double tap, system, dock; grip /
 * aim / tip poses) plus its haptic output, driven by the pure script in
 * `sim_stylus_script.c`. Pose and buttons are functions of the requested
 * monotonic time, so `get_tracked_pose(at_time_ns)` is exact for any time
 * and runs are reproducible.
 *
 * `MODE=mouse` replaces the script with the desktop mouse
 * (`sim_stylus_mouse.h`): every query samples the cursor NOW (a mouse cannot
 * be predicted), through one process-wide sampler that serialises
 * `update_inputs` against `get_tracked_pose` on other threads.
 *
 * Haptics: `set_output` counts vibration events and logs them (the first one
 * as a WARN, the rest at INFO), with the last amplitude / frequency visible
 * through u_var — enough to see an app's `xrApplyHapticFeedback` arrive.
 *
 * @ingroup drv_sim_stylus
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_tracking.h"

#include "os/os_time.h"

#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_var.h"

#include "sim_stylus_interface.h"
#include "sim_stylus_mouse.h"
#include "sim_stylus_script.h"

#include <stdio.h>


// Indices into base.inputs, matching g_inputs below.
enum sim_stylus_input_index
{
	SIM_STYLUS_IN_SYSTEM_CLICK = 0,
	SIM_STYLUS_IN_TIP_FORCE,
	SIM_STYLUS_IN_FRONT_CLICK,
	SIM_STYLUS_IN_FRONT_DOUBLE_TAP,
	SIM_STYLUS_IN_MIDDLE_FORCE,
	SIM_STYLUS_IN_BACK_CLICK,
	SIM_STYLUS_IN_BACK_DOUBLE_TAP,
	SIM_STYLUS_IN_DOCKED,
	SIM_STYLUS_IN_GRIP_POSE,
	SIM_STYLUS_IN_AIM_POSE,
	SIM_STYLUS_IN_TIP_POSE,
	SIM_STYLUS_IN_COUNT,
};

static const enum xrt_input_name g_inputs[SIM_STYLUS_IN_COUNT] = {
    XRT_INPUT_MX_INK_SYSTEM_CLICK,
    XRT_INPUT_MX_INK_TIP_FORCE,
    XRT_INPUT_MX_INK_CLUSTER_FRONT_CLICK,
    XRT_INPUT_MX_INK_CLUSTER_FRONT_DOUBLE_TAP,
    XRT_INPUT_MX_INK_CLUSTER_MIDDLE_FORCE,
    XRT_INPUT_MX_INK_CLUSTER_BACK_CLICK,
    XRT_INPUT_MX_INK_CLUSTER_BACK_DOUBLE_TAP,
    XRT_INPUT_MX_INK_DOCKED,
    XRT_INPUT_MX_INK_GRIP_POSE,
    XRT_INPUT_MX_INK_AIM_POSE,
    XRT_INPUT_MX_INK_TIP_POSE,
};

struct sim_stylus_device
{
	struct xrt_device base;

	struct sim_stylus_script_config cfg;

	//! Last evaluated state (u_var).
	float last_tip_force;
	bool last_docked;

	//! Haptics bookkeeping.
	uint64_t haptic_event_count;
	float last_haptic_amplitude;
	float last_haptic_frequency;
};

static inline struct sim_stylus_device *
sim_stylus_device(struct xrt_device *xdev)
{
	return (struct sim_stylus_device *)xdev;
}

static void
sim_stylus_destroy(struct xrt_device *xdev)
{
	struct sim_stylus_device *sd = sim_stylus_device(xdev);
	u_var_remove_root(sd);
	u_device_free(&sd->base);
}

//! The pen at @p t_ns: the script's, or (mouse) the desktop's as of now.
static void
eval_pen(const struct sim_stylus_device *sd, int64_t t_ns, struct sim_stylus_state *out)
{
	if (sd->cfg.mode == SIM_STYLUS_MODE_MOUSE) {
		sim_stylus_mouse_os_eval((int64_t)os_monotonic_get_ns(), out);
		return;
	}
	sim_stylus_script_eval(&sd->cfg, t_ns, out);
}

static xrt_result_t
sim_stylus_update_inputs(struct xrt_device *xdev)
{
	struct sim_stylus_device *sd = sim_stylus_device(xdev);
	const int64_t now = (int64_t)os_monotonic_get_ns();

	struct sim_stylus_state s;
	eval_pen(sd, now, &s);

	for (uint32_t i = 0; i < xdev->input_count; i++) {
		xdev->inputs[i].active = s.tracked;
		xdev->inputs[i].timestamp = now;
	}
	struct xrt_input *in = xdev->inputs;
	in[SIM_STYLUS_IN_SYSTEM_CLICK].value.boolean = s.system_click;
	in[SIM_STYLUS_IN_TIP_FORCE].value.vec1.x = s.tip_force;
	in[SIM_STYLUS_IN_FRONT_CLICK].value.boolean = s.front_click;
	in[SIM_STYLUS_IN_FRONT_DOUBLE_TAP].value.boolean = s.front_double_tap;
	in[SIM_STYLUS_IN_MIDDLE_FORCE].value.vec1.x = s.middle_force;
	in[SIM_STYLUS_IN_BACK_CLICK].value.boolean = s.back_click;
	in[SIM_STYLUS_IN_BACK_DOUBLE_TAP].value.boolean = s.back_double_tap;
	in[SIM_STYLUS_IN_DOCKED].value.boolean = s.docked;

	sd->last_tip_force = s.tip_force;
	sd->last_docked = s.docked;
	return XRT_SUCCESS;
}

static xrt_result_t
sim_stylus_get_tracked_pose(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            int64_t at_timestamp_ns,
                            struct xrt_space_relation *out_relation)
{
	struct sim_stylus_device *sd = sim_stylus_device(xdev);

	if (name != XRT_INPUT_MX_INK_GRIP_POSE && name != XRT_INPUT_MX_INK_AIM_POSE &&
	    name != XRT_INPUT_MX_INK_TIP_POSE) {
		// Plain message (the u_pp helpers are not on the runtime DLL's export surface).
		U_LOG_E("sim_stylus: unsupported input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct sim_stylus_state s;
	eval_pen(sd, at_timestamp_ns, &s);

	U_ZERO(out_relation);
	if (!s.tracked) {
		out_relation->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
		out_relation->relation_flags = (enum xrt_space_relation_flags)0;
		return XRT_SUCCESS;
	}

	out_relation->pose = name == XRT_INPUT_MX_INK_GRIP_POSE ? s.grip : s.tip;
	// The pen translates without turning, so every point of it shares the tip's velocity.
	out_relation->linear_velocity = s.tip_linear_velocity;
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	    XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
	return XRT_SUCCESS;
}

static xrt_result_t
sim_stylus_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct sim_stylus_device *sd = sim_stylus_device(xdev);

	if (name != XRT_OUTPUT_NAME_MX_INK_HAPTIC) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	// A stop request (amplitude 0) is not an event worth counting.
	if (value->vibration.amplitude <= 0.0f) {
		return XRT_SUCCESS;
	}

	sd->haptic_event_count++;
	sd->last_haptic_amplitude = value->vibration.amplitude;
	sd->last_haptic_frequency = value->vibration.frequency;

	if (sd->haptic_event_count == 1) {
		U_LOG_W("sim-stylus: first haptic event: amplitude=%.2f frequency=%.1f duration=%lld ns",
		        (double)value->vibration.amplitude, (double)value->vibration.frequency,
		        (long long)value->vibration.duration_ns);
	} else {
		U_LOG_I("sim-stylus: haptic event #%llu: amplitude=%.2f frequency=%.1f duration=%lld ns",
		        (unsigned long long)sd->haptic_event_count, (double)value->vibration.amplitude,
		        (double)value->vibration.frequency, (long long)value->vibration.duration_ns);
	}
	return XRT_SUCCESS;
}

struct xrt_device *
sim_stylus_create(enum xrt_device_type device_type, const struct sim_stylus_script_config *cfg)
{
	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	struct sim_stylus_device *sd = U_DEVICE_ALLOCATE(struct sim_stylus_device, flags, SIM_STYLUS_IN_COUNT, 1);
	if (sd == NULL) {
		return NULL;
	}

	sd->cfg = *cfg;

	sd->base.update_inputs = sim_stylus_update_inputs;
	sd->base.get_tracked_pose = sim_stylus_get_tracked_pose;
	sd->base.get_view_poses = u_device_ni_get_view_poses;
	sd->base.set_output = sim_stylus_set_output;
	sd->base.destroy = sim_stylus_destroy;
	sd->base.supported.orientation_tracking = true;
	sd->base.supported.position_tracking = true;
	sd->base.name = XRT_DEVICE_MX_INK_STYLUS;
	sd->base.device_type = device_type;

	// Glass-registered (ADR-034 Amendment 8, input-provider-discovery.md
	// §4b.4a): physical display-plane metres, screen-anchored, never
	// rescaled onto a session's canvas. Never leave a 6-DoF origin at
	// TRACKING_TYPE_NONE: the builder would inject arm-model offsets.
	sd->base.tracking_origin->type = XRT_TRACKING_TYPE_RIG_LOCAL_PHYSICAL;
	snprintf(sd->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Sim Stylus Glass Volume");

	snprintf(sd->base.str, sizeof(sd->base.str), "%s", "Simulated Stylus");
	snprintf(sd->base.serial, sizeof(sd->base.serial), "%s", "SIM-STYLUS-0");

	for (uint32_t i = 0; i < SIM_STYLUS_IN_COUNT; i++) {
		sd->base.inputs[i].active = true;
		sd->base.inputs[i].name = g_inputs[i];
	}
	sd->base.outputs[0].name = XRT_OUTPUT_NAME_MX_INK_HAPTIC;

	u_var_add_root(sd, sd->base.str, true);
	u_var_add_ro_f32(sd, &sd->last_tip_force, "tip_force");
	u_var_add_bool(sd, &sd->last_docked, "docked");
	u_var_add_ro_u64(sd, &sd->haptic_event_count, "haptic_event_count");
	u_var_add_ro_f32(sd, &sd->last_haptic_amplitude, "last_haptic_amplitude");
	u_var_add_ro_f32(sd, &sd->last_haptic_frequency, "last_haptic_frequency");

	return &sd->base;
}
