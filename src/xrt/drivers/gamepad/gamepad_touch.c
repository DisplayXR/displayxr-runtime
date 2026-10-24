// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Emulated `oculus/touch_controller` pair with pad-driven hands.
 *
 * Why this exists next to @ref gamepad_emu.c: a fixed-pose
 * `khr/simple_controller` pair is enough to press a button in a viewer,
 * but VR creation tools (sculpting, painting — Substance 3D Modeler is the
 * motivating title) bind only the full Touch layout and need hands that
 * MOVE through the work volume. With `EMULATE_PROFILE` = touch each stick
 * moves its hand (in the display plane, or along depth while the stick is
 * clicked in), and the buttons map onto the Touch layout per
 * @ref gamepad_map_touch_hand. A View tap brings both hands home.
 *
 * Poses are `XRT_TRACKING_TYPE_RIG_LOCAL` like the simple pair's (§4b.4),
 * pointing into the screen (−Z), so an aim ray starts at the hand and
 * goes away from the viewer.
 *
 * @ingroup drv_gamepad
 */

#include "gamepad_interface.h"

#include "xrt/xrt_device.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_device.h"
#include "util/u_grip_surface.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_var.h"

#include <stdio.h>


//! Input slots shared by both hands; the names differ per hand (X/Y/menu vs A/B/system).
enum touch_slot
{
	T_LOWER_CLICK = 0,
	T_LOWER_TOUCH,
	T_UPPER_CLICK,
	T_UPPER_TOUCH,
	T_MENU_CLICK,
	T_SQUEEZE,
	T_TRIGGER_TOUCH,
	T_TRIGGER,
	T_STICK_CLICK,
	T_STICK_TOUCH,
	T_STICK,
	T_THUMBREST_TOUCH,
	T_GRIP,
	T_AIM,
	T_PALM,
	T_COUNT,
};

static const enum xrt_input_name touch_left_inputs[T_COUNT] = {
    XRT_INPUT_TOUCH_X_CLICK,          XRT_INPUT_TOUCH_X_TOUCH,       XRT_INPUT_TOUCH_Y_CLICK,
    XRT_INPUT_TOUCH_Y_TOUCH,          XRT_INPUT_TOUCH_MENU_CLICK,    XRT_INPUT_TOUCH_SQUEEZE_VALUE,
    XRT_INPUT_TOUCH_TRIGGER_TOUCH,    XRT_INPUT_TOUCH_TRIGGER_VALUE, XRT_INPUT_TOUCH_THUMBSTICK_CLICK,
    XRT_INPUT_TOUCH_THUMBSTICK_TOUCH, XRT_INPUT_TOUCH_THUMBSTICK,    XRT_INPUT_TOUCH_THUMBREST_TOUCH,
    XRT_INPUT_TOUCH_GRIP_POSE,        XRT_INPUT_TOUCH_AIM_POSE,      XRT_INPUT_GENERIC_PALM_POSE,
};

static const enum xrt_input_name touch_right_inputs[T_COUNT] = {
    XRT_INPUT_TOUCH_A_CLICK,          XRT_INPUT_TOUCH_A_TOUCH,       XRT_INPUT_TOUCH_B_CLICK,
    XRT_INPUT_TOUCH_B_TOUCH,          XRT_INPUT_TOUCH_SYSTEM_CLICK,  XRT_INPUT_TOUCH_SQUEEZE_VALUE,
    XRT_INPUT_TOUCH_TRIGGER_TOUCH,    XRT_INPUT_TOUCH_TRIGGER_VALUE, XRT_INPUT_TOUCH_THUMBSTICK_CLICK,
    XRT_INPUT_TOUCH_THUMBSTICK_TOUCH, XRT_INPUT_TOUCH_THUMBSTICK,    XRT_INPUT_TOUCH_THUMBREST_TOUCH,
    XRT_INPUT_TOUCH_GRIP_POSE,        XRT_INPUT_TOUCH_AIM_POSE,      XRT_INPUT_GENERIC_PALM_POSE,
};

//! A hand never strays further than this from home on any axis, metres.
#define TOUCH_HAND_RANGE_M 0.6f

//! Longest step the integrator takes, so a stalled app does not fling the hands.
#define TOUCH_MAX_DT_S 0.1

struct gamepad_touch_device
{
	struct xrt_device base;
	struct gamepad_hub *hub;
	struct gamepad_touch_config cfg;

	bool is_left;

	//! Guards @ref offset, written by update_inputs and read by get_tracked_pose.
	struct os_mutex lock;
	struct xrt_vec3 home;
	struct xrt_vec3 offset;
	int64_t last_update_ns;
	struct gamepad_tap tap;

	//! Diagnostics (u_var).
	uint64_t haptic_event_count;
	uint64_t recenter_count;
};

static inline struct gamepad_touch_device *
touch_device(struct xrt_device *xdev)
{
	return (struct gamepad_touch_device *)xdev;
}

static float
clamp_range(float v)
{
	if (v > TOUCH_HAND_RANGE_M) {
		return TOUCH_HAND_RANGE_M;
	}
	if (v < -TOUCH_HAND_RANGE_M) {
		return -TOUCH_HAND_RANGE_M;
	}
	return v;
}

static void
touch_destroy(struct xrt_device *xdev)
{
	struct gamepad_touch_device *td = touch_device(xdev);
	struct gamepad_hub *hub = td->hub;

	u_var_remove_root(td);
	os_mutex_destroy(&td->lock);
	u_device_free(&td->base);
	gamepad_hub_release(hub);
}

static xrt_result_t
touch_update_inputs(struct xrt_device *xdev)
{
	struct gamepad_touch_device *td = touch_device(xdev);

	struct gamepad_state s;
	gamepad_hub_snapshot(td->hub, &s);

	const int64_t now = (int64_t)os_monotonic_get_ns();
	for (uint32_t i = 0; i < xdev->input_count; i++) {
		xdev->inputs[i].active = s.connected;
		xdev->inputs[i].timestamp = now;
		U_ZERO(&xdev->inputs[i].value);
	}

	struct gamepad_touch_hand h;
	gamepad_map_touch_hand(&s, td->is_left, td->cfg.deadzone, td->cfg.hand_speed_mps, &h);
	const bool recenter = gamepad_map_view_tap(&td->tap, &s, now);

	os_mutex_lock(&td->lock);
	double dt = td->last_update_ns > 0 ? (double)(now - td->last_update_ns) * 1e-9 : 0.0;
	if (dt < 0.0) {
		dt = 0.0;
	}
	if (dt > TOUCH_MAX_DT_S) {
		dt = TOUCH_MAX_DT_S;
	}
	td->last_update_ns = now;
	if (recenter) {
		td->offset = (struct xrt_vec3){0.0f, 0.0f, 0.0f};
		td->recenter_count++;
	} else {
		td->offset.x = clamp_range(td->offset.x + h.velocity_mps.x * (float)dt);
		td->offset.y = clamp_range(td->offset.y + h.velocity_mps.y * (float)dt);
		td->offset.z = clamp_range(td->offset.z + h.velocity_mps.z * (float)dt);
	}
	os_mutex_unlock(&td->lock);

	if (!s.connected) {
		return XRT_SUCCESS;
	}

	struct xrt_input *in = xdev->inputs;
	in[T_LOWER_CLICK].value.boolean = h.lower;
	in[T_LOWER_TOUCH].value.boolean = h.lower;
	in[T_UPPER_CLICK].value.boolean = h.upper;
	in[T_UPPER_TOUCH].value.boolean = h.upper;
	in[T_MENU_CLICK].value.boolean = h.menu;
	in[T_SQUEEZE].value.vec1.x = h.squeeze;
	in[T_TRIGGER_TOUCH].value.boolean = h.trigger > GAMEPAD_MAP_TRIGGER_ACTIVITY;
	in[T_TRIGGER].value.vec1.x = h.trigger;
	in[T_STICK].value.vec2.x = h.thumbstick_x;
	in[T_STICK].value.vec2.y = h.thumbstick_y;
	in[T_STICK_TOUCH].value.boolean = h.thumbstick_x != 0.0f || h.thumbstick_y != 0.0f;
	// The thumb rests on the controller whenever it is not on a button or the stick.
	in[T_THUMBREST_TOUCH].value.boolean = !h.lower && !h.upper && !in[T_STICK_TOUCH].value.boolean;

	return XRT_SUCCESS;
}

static xrt_result_t
touch_get_tracked_pose(struct xrt_device *xdev,
                       enum xrt_input_name name,
                       int64_t at_timestamp_ns,
                       struct xrt_space_relation *out_relation)
{
	struct gamepad_touch_device *td = touch_device(xdev);
	(void)at_timestamp_ns;

	switch (name) {
	case XRT_INPUT_TOUCH_GRIP_POSE:
	case XRT_INPUT_TOUCH_AIM_POSE:
	case XRT_INPUT_GENERIC_PALM_POSE: break;
	default:
		U_LOG_E("gamepad: unsupported touch controller input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation zero = XRT_SPACE_RELATION_ZERO;
	*out_relation = zero;

	os_mutex_lock(&td->lock);
	out_relation->pose.orientation = (struct xrt_quat){0.0f, 0.0f, 0.0f, 1.0f};
	out_relation->pose.position.x = td->home.x + td->offset.x;
	out_relation->pose.position.y = td->home.y + td->offset.y;
	out_relation->pose.position.z = td->home.z + td->offset.z;
	os_mutex_unlock(&td->lock);

	struct gamepad_state s;
	gamepad_hub_snapshot(td->hub, &s);
	if (!s.connected) {
		return XRT_SUCCESS; // flags clear: nothing to point with
	}

	// Valid and "tracked", as for the simple pair: controller models that
	// only render TRACKED poses must still show up.
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

	if (name == XRT_INPUT_GENERIC_PALM_POSE) {
		u_grip_surface_from_grip(td->is_left, out_relation, out_relation);
	}
	return XRT_SUCCESS;
}

static xrt_result_t
touch_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct gamepad_touch_device *td = touch_device(xdev);

	if (name != XRT_OUTPUT_NAME_TOUCH_HAPTIC) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	td->haptic_event_count++;

	int64_t duration = value->vibration.duration_ns;
	if (duration <= 0) {
		duration = 50LL * 1000 * 1000;
	}
	// Left hand → the low-frequency motor, right hand → the high-frequency one.
	gamepad_hub_rumble(td->hub, td->is_left ? value->vibration.amplitude : 0.0f,
	                   td->is_left ? 0.0f : value->vibration.amplitude, duration);
	return XRT_SUCCESS;
}

struct xrt_device *
gamepad_create_touch_controller(struct gamepad_hub *hub,
                                enum xrt_device_type type,
                                const struct gamepad_touch_config *cfg)
{
	bool is_left = false;
	switch (type) {
	case XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER: is_left = true; break;
	case XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER: is_left = false; break;
	default: return NULL;
	}
	if (hub == NULL || cfg == NULL) {
		return NULL;
	}

	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	struct gamepad_touch_device *td = U_DEVICE_ALLOCATE(struct gamepad_touch_device, flags, T_COUNT, 1);
	if (td == NULL) {
		return NULL;
	}
	if (os_mutex_init(&td->lock) != 0) {
		u_device_free(&td->base);
		return NULL;
	}
	td->hub = hub;
	gamepad_hub_retain(hub);
	td->cfg = *cfg;
	td->is_left = is_left;
	// The configured home is the right hand's; the left one mirrors it.
	td->home = cfg->right_home;
	if (is_left) {
		td->home.x = -td->home.x;
	}

	td->base.update_inputs = touch_update_inputs;
	td->base.get_tracked_pose = touch_get_tracked_pose;
	td->base.get_view_poses = u_device_ni_get_view_poses;
	td->base.set_output = touch_set_output;
	td->base.destroy = touch_destroy;
	td->base.supported.orientation_tracking = true;
	td->base.supported.position_tracking = true;
	td->base.name = XRT_DEVICE_TOUCH_CONTROLLER;
	td->base.device_type = type;

	// Same rig-local volume as the simple pair (gamepad_emu.c).
	td->base.tracking_origin->type = XRT_TRACKING_TYPE_RIG_LOCAL;
	snprintf(td->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Gamepad Rig-Local Volume");

	snprintf(td->base.str, sizeof(td->base.str), "Gamepad %s Touch Controller", is_left ? "Left" : "Right");
	snprintf(td->base.serial, sizeof(td->base.serial), "GAMEPAD-TOUCH-%s", is_left ? "L" : "R");

	const enum xrt_input_name *names = is_left ? touch_left_inputs : touch_right_inputs;
	for (uint32_t i = 0; i < T_COUNT; i++) {
		td->base.inputs[i].active = false;
		td->base.inputs[i].name = names[i];
	}
	td->base.outputs[0].name = XRT_OUTPUT_NAME_TOUCH_HAPTIC;

	u_var_add_root(td, td->base.str, true);
	u_var_add_ro_vec3_f32(td, &td->home, "home");
	u_var_add_ro_vec3_f32(td, &td->offset, "offset");
	u_var_add_ro_u64(td, &td->haptic_event_count, "haptic_event_count");
	u_var_add_ro_u64(td, &td->recenter_count, "recenter_count");

	return &td->base;
}
