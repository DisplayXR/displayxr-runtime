// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Emulated `khr/simple_controller` pair driven by the pad.
 *
 * Why these exist: a HANDLE app's own window owns the keyboard and mouse,
 * so the runtime withholds the qwerty pair from it (`oxr_input.c`,
 * `has_external_window`) and such an app has never had an `xrSyncActions`
 * controller. A pad is focus-independent, so a pad-driven pair has no such
 * conflict. The shape mirrors `sim_input_device.c` with the script replaced
 * by the mapping table: select / menu per hand come from
 * @ref gamepad_map_action_pressed, the poses are FIXED display-plane-relative
 * points (`XRT_TRACKING_TYPE_RIG_LOCAL`, §4b.4 — a navigating provider's
 * controllers follow the rig it drives), and vibration lands on the pad's
 * motor of that side.
 *
 * Gated by the provider's `EMULATE_CONTROLLERS` setting (default on). With a
 * pad attached the pair outranks qwerty for the hand roles, so a hosted app
 * also sees it; unplug the pad and qwerty takes the hands back (presence
 * arbitration, ADR-034 Amendment 1).
 *
 * @ingroup drv_gamepad
 */

#include "gamepad_interface.h"

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "util/u_device.h"
#include "util/u_grip_surface.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_var.h"

#include <stdio.h>


#define EMU_SELECT 0
#define EMU_MENU 1
#define EMU_GRIP 2
#define EMU_AIM 3
#define EMU_PALM 4

struct gamepad_emu_device
{
	struct xrt_device base;
	struct gamepad_hub *hub;

	bool is_left;

	//! Where this hand sits in front of the display (rig-local, metres).
	struct xrt_pose pose;

	//! Diagnostics (u_var).
	uint64_t haptic_event_count;
	float last_haptic_amplitude;
};

static const enum xrt_input_name emu_inputs_array[] = {
    XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_SIMPLE_GRIP_POSE,
    XRT_INPUT_SIMPLE_AIM_POSE,     XRT_INPUT_GENERIC_PALM_POSE, // `grip_surface` / `palm_ext` (#1633)
};

static const enum xrt_output_name emu_outputs_array[] = {
    XRT_OUTPUT_NAME_SIMPLE_VIBRATION,
};

static inline struct gamepad_emu_device *
emu_device(struct xrt_device *xdev)
{
	return (struct gamepad_emu_device *)xdev;
}

static void
emu_destroy(struct xrt_device *xdev)
{
	struct gamepad_emu_device *ed = emu_device(xdev);
	struct gamepad_hub *hub = ed->hub;

	u_var_remove_root(ed);
	u_device_free(&ed->base);
	gamepad_hub_release(hub);
}

static xrt_result_t
emu_update_inputs(struct xrt_device *xdev)
{
	struct gamepad_emu_device *ed = emu_device(xdev);

	struct gamepad_state s;
	gamepad_hub_snapshot(ed->hub, &s);

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

	const struct gamepad_map_table *table = &gamepad_map_default_table;
	xdev->inputs[EMU_SELECT].value.boolean =
	    gamepad_map_action_pressed(&s, table, ed->is_left ? GAMEPAD_ACT_LEFT_SELECT : GAMEPAD_ACT_RIGHT_SELECT);
	xdev->inputs[EMU_MENU].value.boolean =
	    gamepad_map_action_pressed(&s, table, ed->is_left ? GAMEPAD_ACT_LEFT_MENU : GAMEPAD_ACT_RIGHT_MENU);

	return XRT_SUCCESS;
}

static xrt_result_t
emu_get_tracked_pose(struct xrt_device *xdev,
                     enum xrt_input_name name,
                     int64_t at_timestamp_ns,
                     struct xrt_space_relation *out_relation)
{
	struct gamepad_emu_device *ed = emu_device(xdev);
	(void)at_timestamp_ns;

	switch (name) {
	case XRT_INPUT_SIMPLE_GRIP_POSE:
	case XRT_INPUT_SIMPLE_AIM_POSE:
	case XRT_INPUT_GENERIC_PALM_POSE: break;
	default:
		U_LOG_E("gamepad: unsupported controller input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation zero = XRT_SPACE_RELATION_ZERO;
	*out_relation = zero;
	out_relation->pose = ed->pose;

	struct gamepad_state s;
	gamepad_hub_snapshot(ed->hub, &s);
	if (!s.connected) {
		return XRT_SUCCESS; // flags clear: nothing to point with
	}

	// A fixed point is VALID and, for the apps' purposes, "tracked" — a
	// controller model that only renders TRACKED poses must still show up.
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

	if (name == XRT_INPUT_GENERIC_PALM_POSE) {
		u_grip_surface_from_grip(ed->is_left, out_relation, out_relation);
	}
	return XRT_SUCCESS;
}

static xrt_result_t
emu_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct gamepad_emu_device *ed = emu_device(xdev);

	if (name != XRT_OUTPUT_NAME_SIMPLE_VIBRATION) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	ed->haptic_event_count++;
	ed->last_haptic_amplitude = value->vibration.amplitude;

	int64_t duration = value->vibration.duration_ns;
	if (duration <= 0) {
		duration = 50LL * 1000 * 1000;
	}
	// Left hand → the low-frequency motor, right hand → the high-frequency one.
	gamepad_hub_rumble(ed->hub, ed->is_left ? value->vibration.amplitude : 0.0f,
	                   ed->is_left ? 0.0f : value->vibration.amplitude, duration);
	return XRT_SUCCESS;
}

struct xrt_device *
gamepad_create_emulated_controller(struct gamepad_hub *hub, enum xrt_device_type type)
{
	bool is_left = false;
	switch (type) {
	case XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER: is_left = true; break;
	case XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER: is_left = false; break;
	default: return NULL;
	}
	if (hub == NULL) {
		return NULL;
	}

	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	const uint32_t input_count = ARRAY_SIZE(emu_inputs_array);
	const uint32_t output_count = ARRAY_SIZE(emu_outputs_array);

	struct gamepad_emu_device *ed = U_DEVICE_ALLOCATE(struct gamepad_emu_device, flags, input_count, output_count);
	if (ed == NULL) {
		return NULL;
	}
	ed->hub = hub;
	gamepad_hub_retain(hub);
	ed->is_left = is_left;

	ed->base.update_inputs = emu_update_inputs;
	ed->base.get_tracked_pose = emu_get_tracked_pose;
	ed->base.get_view_poses = u_device_ni_get_view_poses;
	ed->base.set_output = emu_set_output;
	ed->base.destroy = emu_destroy;
	ed->base.supported.orientation_tracking = true;
	ed->base.supported.position_tracking = true;
	ed->base.name = XRT_DEVICE_SIMPLE_CONTROLLER;
	ed->base.device_type = type;

	// RIG_LOCAL: display-plane-relative (origin at the display centre, +Z
	// toward the viewer); the builder anchors it at the initial rig so the
	// hands follow the navigation this same pad drives (§4b.4). Shared by
	// name between the two hands, like sim_input's.
	ed->base.tracking_origin->type = XRT_TRACKING_TYPE_RIG_LOCAL;
	snprintf(ed->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Gamepad Rig-Local Volume");

	snprintf(ed->base.str, sizeof(ed->base.str), "Gamepad %s Controller", is_left ? "Left" : "Right");
	snprintf(ed->base.serial, sizeof(ed->base.serial), "GAMEPAD-%s", is_left ? "L" : "R");

	for (uint32_t i = 0; i < input_count; i++) {
		ed->base.inputs[i].active = false;
		ed->base.inputs[i].name = emu_inputs_array[i];
	}
	for (uint32_t i = 0; i < output_count; i++) {
		ed->base.outputs[i].name = emu_outputs_array[i];
	}

	// A third of a metre out of the screen, a hand's width either side,
	// slightly below centre — the same placement sim_input uses rig-local,
	// so the two providers' hands land in the same spot.
	ed->pose = (struct xrt_pose){
	    .orientation = {0.0f, 0.0f, 0.0f, 1.0f},
	    .position = {is_left ? -0.20f : 0.20f, -0.15f, 0.35f},
	};

	u_var_add_root(ed, ed->base.str, true);
	u_var_add_pose(ed, &ed->pose, "pose");
	u_var_add_ro_u64(ed, &ed->haptic_event_count, "haptic_event_count");
	u_var_add_ro_f32(ed, &ed->last_haptic_amplitude, "last_haptic_amplitude");

	return &ed->base;
}
