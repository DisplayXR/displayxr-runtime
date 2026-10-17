// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `microsoft/xbox_controller` gamepad device.
 *
 * Every input of the profile (`bindings.json`:
 * `/interaction_profiles/microsoft/xbox_controller`, `/user/gamepad`) is
 * published from the hub's latched sample; the four haptic outputs route to
 * the pad's two motors (left / left-trigger → low-frequency motor, right /
 * right-trigger → high-frequency). The device has no pose — it is a
 * `XRT_DEVICE_TYPE_GAMEPAD`, never a hand — so `get_tracked_pose` rejects
 * every name. `XR_EXT_dpad_binding` needs nothing here: the runtime's dpad
 * emulation (`oxr_api_action.c`) works over the thumbstick vec2 inputs this
 * device already exposes.
 *
 * @ingroup drv_gamepad
 */

#include "gamepad_interface.h"

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_var.h"

#include <stdio.h>


struct gamepad_pad_device
{
	struct xrt_device base;
	struct gamepad_hub *hub;

	//! Diagnostics (u_var).
	struct gamepad_state last;
	uint64_t haptic_event_count;
};

//! Order of base.inputs; the enum is the index, the array below the name.
enum pad_input
{
	PAD_MENU = 0,
	PAD_VIEW,
	PAD_A,
	PAD_B,
	PAD_X,
	PAD_Y,
	PAD_DPAD_DOWN,
	PAD_DPAD_RIGHT,
	PAD_DPAD_UP,
	PAD_DPAD_LEFT,
	PAD_SHOULDER_LEFT,
	PAD_SHOULDER_RIGHT,
	PAD_THUMB_LEFT_CLICK,
	PAD_THUMB_LEFT,
	PAD_THUMB_RIGHT_CLICK,
	PAD_THUMB_RIGHT,
	PAD_TRIGGER_LEFT,
	PAD_TRIGGER_RIGHT,
	PAD_INPUT_COUNT,
};

static const enum xrt_input_name pad_inputs_array[PAD_INPUT_COUNT] = {
    XRT_INPUT_XBOX_MENU_CLICK,
    XRT_INPUT_XBOX_VIEW_CLICK,
    XRT_INPUT_XBOX_A_CLICK,
    XRT_INPUT_XBOX_B_CLICK,
    XRT_INPUT_XBOX_X_CLICK,
    XRT_INPUT_XBOX_Y_CLICK,
    XRT_INPUT_XBOX_DPAD_DOWN_CLICK,
    XRT_INPUT_XBOX_DPAD_RIGHT_CLICK,
    XRT_INPUT_XBOX_DPAD_UP_CLICK,
    XRT_INPUT_XBOX_DPAD_LEFT_CLICK,
    XRT_INPUT_XBOX_SHOULDER_LEFT_CLICK,
    XRT_INPUT_XBOX_SHOULDER_RIGHT_CLICK,
    XRT_INPUT_XBOX_THUMBSTICK_LEFT_CLICK,
    XRT_INPUT_XBOX_THUMBSTICK_LEFT,
    XRT_INPUT_XBOX_THUMBSTICK_RIGHT_CLICK,
    XRT_INPUT_XBOX_THUMBSTICK_RIGHT,
    XRT_INPUT_XBOX_LEFT_TRIGGER_VALUE,
    XRT_INPUT_XBOX_RIGHT_TRIGGER_VALUE,
};

static const enum xrt_output_name pad_outputs_array[] = {
    XRT_OUTPUT_NAME_XBOX_HAPTIC_LEFT,
    XRT_OUTPUT_NAME_XBOX_HAPTIC_RIGHT,
    XRT_OUTPUT_NAME_XBOX_HAPTIC_LEFT_TRIGGER,
    XRT_OUTPUT_NAME_XBOX_HAPTIC_RIGHT_TRIGGER,
};

static inline struct gamepad_pad_device *
pad_device(struct xrt_device *xdev)
{
	return (struct gamepad_pad_device *)xdev;
}

static void
pad_destroy(struct xrt_device *xdev)
{
	struct gamepad_pad_device *pd = pad_device(xdev);
	struct gamepad_hub *hub = pd->hub;

	u_var_remove_root(pd);
	u_device_free(&pd->base);
	gamepad_hub_release(hub);
}

static xrt_result_t
pad_update_inputs(struct xrt_device *xdev)
{
	struct gamepad_pad_device *pd = pad_device(xdev);

	struct gamepad_state s;
	gamepad_hub_snapshot(pd->hub, &s);
	pd->last = s;

	const int64_t now = (int64_t)os_monotonic_get_ns();
	for (uint32_t i = 0; i < xdev->input_count; i++) {
		xdev->inputs[i].active = s.connected;
		xdev->inputs[i].timestamp = now;
	}
	if (!s.connected) {
		for (uint32_t i = 0; i < xdev->input_count; i++) {
			U_ZERO(&xdev->inputs[i].value);
		}
		return XRT_SUCCESS;
	}

#define BTN(IDX, BIT) xdev->inputs[IDX].value.boolean = (s.buttons & (BIT)) != 0
	BTN(PAD_MENU, GAMEPAD_BTN_MENU);
	BTN(PAD_VIEW, GAMEPAD_BTN_VIEW);
	BTN(PAD_A, GAMEPAD_BTN_A);
	BTN(PAD_B, GAMEPAD_BTN_B);
	BTN(PAD_X, GAMEPAD_BTN_X);
	BTN(PAD_Y, GAMEPAD_BTN_Y);
	BTN(PAD_DPAD_DOWN, GAMEPAD_BTN_DPAD_DOWN);
	BTN(PAD_DPAD_RIGHT, GAMEPAD_BTN_DPAD_RIGHT);
	BTN(PAD_DPAD_UP, GAMEPAD_BTN_DPAD_UP);
	BTN(PAD_DPAD_LEFT, GAMEPAD_BTN_DPAD_LEFT);
	BTN(PAD_SHOULDER_LEFT, GAMEPAD_BTN_SHOULDER_LEFT);
	BTN(PAD_SHOULDER_RIGHT, GAMEPAD_BTN_SHOULDER_RIGHT);
	BTN(PAD_THUMB_LEFT_CLICK, GAMEPAD_BTN_THUMB_LEFT);
	BTN(PAD_THUMB_RIGHT_CLICK, GAMEPAD_BTN_THUMB_RIGHT);
#undef BTN

	// The profile's sticks are raw (no deadzone): apps apply their own, and
	// XR_EXT_dpad_binding carries its own thresholds. The deadzone setting
	// is a NAVIGATION knob (gamepad_map.c), not a profile one.
	xdev->inputs[PAD_THUMB_LEFT].value.vec2.x = s.left_x;
	xdev->inputs[PAD_THUMB_LEFT].value.vec2.y = s.left_y;
	xdev->inputs[PAD_THUMB_RIGHT].value.vec2.x = s.right_x;
	xdev->inputs[PAD_THUMB_RIGHT].value.vec2.y = s.right_y;
	xdev->inputs[PAD_TRIGGER_LEFT].value.vec1.x = s.left_trigger;
	xdev->inputs[PAD_TRIGGER_RIGHT].value.vec1.x = s.right_trigger;

	return XRT_SUCCESS;
}

static xrt_result_t
pad_get_tracked_pose(struct xrt_device *xdev,
                     enum xrt_input_name name,
                     int64_t at_timestamp_ns,
                     struct xrt_space_relation *out_relation)
{
	(void)xdev;
	(void)at_timestamp_ns;
	(void)out_relation;
	// A gamepad has no pose. Plain message, not U_LOG_XDEV_UNSUPPORTED_INPUT:
	// the u_pp helpers it uses aren't on the runtime DLL's aux export
	// surface (same rule as sim_input).
	U_LOG_E("gamepad: unsupported pose input name: 0x%08x", name);
	return XRT_ERROR_INPUT_UNSUPPORTED;
}

static xrt_result_t
pad_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct gamepad_pad_device *pd = pad_device(xdev);

	// Two motors, four outputs: the "left" ones drive the low-frequency
	// motor, the "right" ones the high-frequency one; the two trigger
	// outputs share them (a plain Xbox pad has no trigger motors).
	float low = 0.0f, high = 0.0f;
	switch (name) {
	case XRT_OUTPUT_NAME_XBOX_HAPTIC_LEFT:
	case XRT_OUTPUT_NAME_XBOX_HAPTIC_LEFT_TRIGGER: low = value->vibration.amplitude; break;
	case XRT_OUTPUT_NAME_XBOX_HAPTIC_RIGHT:
	case XRT_OUTPUT_NAME_XBOX_HAPTIC_RIGHT_TRIGGER: high = value->vibration.amplitude; break;
	default: return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	pd->haptic_event_count++;

	// XR_MIN_HAPTIC_DURATION (-1) means "the runtime's shortest pulse".
	int64_t duration = value->vibration.duration_ns;
	if (duration <= 0) {
		duration = 50LL * 1000 * 1000;
	}
	gamepad_hub_rumble(pd->hub, low, high, duration);
	return XRT_SUCCESS;
}

struct xrt_device *
gamepad_create_pad_device(struct gamepad_hub *hub)
{
	if (hub == NULL) {
		return NULL;
	}
	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	const uint32_t input_count = ARRAY_SIZE(pad_inputs_array);
	const uint32_t output_count = ARRAY_SIZE(pad_outputs_array);

	struct gamepad_pad_device *pd = U_DEVICE_ALLOCATE(struct gamepad_pad_device, flags, input_count, output_count);
	if (pd == NULL) {
		return NULL;
	}
	pd->hub = hub;
	gamepad_hub_retain(hub);

	pd->base.update_inputs = pad_update_inputs;
	pd->base.get_tracked_pose = pad_get_tracked_pose;
	pd->base.get_view_poses = u_device_ni_get_view_poses;
	pd->base.set_output = pad_set_output;
	pd->base.destroy = pad_destroy;
	pd->base.supported.orientation_tracking = false;
	pd->base.supported.position_tracking = false;
	pd->base.name = XRT_DEVICE_XBOX_CONTROLLER;
	pd->base.device_type = XRT_DEVICE_TYPE_GAMEPAD;

	// No pose, so the origin type is immaterial; name it anyway for the
	// debug UI, and leave it NONE (u_builder_setup_tracking_origins' gamepad
	// offset is harmless on a device that never reports a relation).
	snprintf(pd->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Gamepad (no pose)");

	snprintf(pd->base.str, sizeof(pd->base.str), "%s", "Gamepad");
	snprintf(pd->base.serial, sizeof(pd->base.serial), "%s", "GAMEPAD-0");

	for (uint32_t i = 0; i < input_count; i++) {
		pd->base.inputs[i].active = false;
		pd->base.inputs[i].name = pad_inputs_array[i];
	}
	for (uint32_t i = 0; i < output_count; i++) {
		pd->base.outputs[i].name = pad_outputs_array[i];
	}

	u_var_add_root(pd, pd->base.str, true);
	u_var_add_bool(pd, &pd->last.connected, "connected");
	u_var_add_ro_f32(pd, &pd->last.left_x, "left_x");
	u_var_add_ro_f32(pd, &pd->last.left_y, "left_y");
	u_var_add_ro_f32(pd, &pd->last.right_x, "right_x");
	u_var_add_ro_f32(pd, &pd->last.right_y, "right_y");
	u_var_add_ro_f32(pd, &pd->last.left_trigger, "left_trigger");
	u_var_add_ro_f32(pd, &pd->last.right_trigger, "right_trigger");
	u_var_add_ro_u64(pd, &pd->haptic_event_count, "haptic_event_count");

	return &pd->base;
}
