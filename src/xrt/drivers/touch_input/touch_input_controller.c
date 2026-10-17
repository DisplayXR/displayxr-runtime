// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The emulated touch controller (#132): `khr/simple_controller` with
 *         the aim ray under the finger.
 *
 * `XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER` so it fills whichever hand role is
 * empty (left first, per the builder). Poses are RIG-LOCAL — display-plane
 * relative, which a provider that also navigates must publish (ADR-034
 * Amendment 4) so the runtime anchors them at the initial rig and
 * `world = rig(t) ∘ L` carries the ray with every orbit:
 *
 *   - AIM:  origin at the nominal viewer, pointing through the finger's point
 *           on the display plane — what an app ray-casts with.
 *   - GRIP: at that point on the plane, same orientation — where a rendered
 *           hand marker sits, i.e. under the finger.
 *   - SELECT: a tap pulses it, a press-and-hold keeps it (touch_gesture.h).
 *   - MENU: never — there is no second button on a finger.
 *
 * Haptics are accepted and dropped: a tablet's vibrator is a platform
 * service the provider has no handle to (follow-up, not a defect).
 *
 * @ingroup drv_touch_input
 */

#include "xrt/xrt_device.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_var.h"

#include "touch_gesture.h"
#include "touch_input_interface.h"
#include "touch_input_provider.h"

#include <stdio.h>


// Indices into base.inputs, matching touch_input_ctl_inputs_array below.
#define TOUCH_INPUT_CTL_SELECT 0
#define TOUCH_INPUT_CTL_MENU 1
#define TOUCH_INPUT_CTL_GRIP 2
#define TOUCH_INPUT_CTL_AIM 3

struct touch_input_ctl_device
{
	struct xrt_device base;
	struct touch_input_provider *tp;

	//! Diagnostics (u_var).
	bool select;
	struct xrt_pose last_aim;
	uint64_t haptic_event_count;
};

static inline struct touch_input_ctl_device *
touch_input_ctl_device(struct xrt_device *xdev)
{
	return (struct touch_input_ctl_device *)xdev;
}

static void
touch_input_ctl_destroy(struct xrt_device *xdev)
{
	struct touch_input_ctl_device *cd = touch_input_ctl_device(xdev);
	u_var_remove_root(cd);
	u_device_free(&cd->base);
}

static xrt_result_t
touch_input_ctl_update_inputs(struct xrt_device *xdev)
{
	struct touch_input_ctl_device *cd = touch_input_ctl_device(xdev);
	const int64_t now = (int64_t)os_monotonic_get_ns();

	touch_input_pump(cd->tp, now);

	os_mutex_lock(&cd->tp->mutex);
	const bool select = touch_gesture_select(&cd->tp->gesture, now);
	struct xrt_pose aim;
	const bool have_aim = touch_gesture_aim(&cd->tp->gesture, &aim, NULL);
	os_mutex_unlock(&cd->tp->mutex);

	for (uint32_t i = 0; i < xdev->input_count; i++) {
		xdev->inputs[i].timestamp = now;
	}
	// The buttons are live from the first touch on; the poses too (an app
	// that ray-casts before any touch gets the ray through the panel centre
	// — see get_tracked_pose — but it is INACTIVE until a finger has been
	// seen, so a pointer that never existed is not drawn).
	xdev->inputs[TOUCH_INPUT_CTL_SELECT].active = true;
	xdev->inputs[TOUCH_INPUT_CTL_SELECT].value.boolean = select;
	xdev->inputs[TOUCH_INPUT_CTL_MENU].active = true;
	xdev->inputs[TOUCH_INPUT_CTL_MENU].value.boolean = false;
	xdev->inputs[TOUCH_INPUT_CTL_GRIP].active = have_aim;
	xdev->inputs[TOUCH_INPUT_CTL_AIM].active = have_aim;

	cd->select = select;
	if (have_aim) {
		cd->last_aim = aim;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
touch_input_ctl_get_tracked_pose(struct xrt_device *xdev,
                                 enum xrt_input_name name,
                                 int64_t at_timestamp_ns,
                                 struct xrt_space_relation *out_relation)
{
	struct touch_input_ctl_device *cd = touch_input_ctl_device(xdev);
	(void)at_timestamp_ns;

	switch (name) {
	case XRT_INPUT_SIMPLE_GRIP_POSE:
	case XRT_INPUT_SIMPLE_AIM_POSE: break;
	default:
		// Plain message — see the u_pp note in touch_input_nav.c.
		U_LOG_E("touch_input: unsupported controller input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation zero = XRT_SPACE_RELATION_ZERO;
	*out_relation = zero;

	struct xrt_pose aim;
	struct xrt_pose grip;
	os_mutex_lock(&cd->tp->mutex);
	const bool have = touch_gesture_aim(&cd->tp->gesture, &aim, &grip);
	os_mutex_unlock(&cd->tp->mutex);

	if (!have) {
		// Nothing has touched the surface yet: a valid-but-inactive ray
		// through the panel centre, so an app that locates the aim space
		// before the first tap gets a pose rather than an error.
		out_relation->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
		out_relation->relation_flags = (enum xrt_space_relation_flags)0;
		return XRT_SUCCESS;
	}

	out_relation->pose = name == XRT_INPUT_SIMPLE_AIM_POSE ? aim : grip;
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	return XRT_SUCCESS;
}

static xrt_result_t
touch_input_ctl_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct touch_input_ctl_device *cd = touch_input_ctl_device(xdev);
	(void)value;
	if (name != XRT_OUTPUT_NAME_SIMPLE_VIBRATION) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	cd->haptic_event_count++; // accepted, not actuated (see the file comment)
	return XRT_SUCCESS;
}

static enum xrt_input_name touch_input_ctl_inputs_array[] = {
    XRT_INPUT_SIMPLE_SELECT_CLICK,
    XRT_INPUT_SIMPLE_MENU_CLICK,
    XRT_INPUT_SIMPLE_GRIP_POSE,
    XRT_INPUT_SIMPLE_AIM_POSE,
};

static enum xrt_output_name touch_input_ctl_outputs_array[] = {
    XRT_OUTPUT_NAME_SIMPLE_VIBRATION,
};

struct xrt_device *
touch_input_create_controller(struct touch_input_provider *tp)
{
	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	const uint32_t input_count = ARRAY_SIZE(touch_input_ctl_inputs_array);
	const uint32_t output_count = ARRAY_SIZE(touch_input_ctl_outputs_array);

	struct touch_input_ctl_device *cd =
	    U_DEVICE_ALLOCATE(struct touch_input_ctl_device, flags, input_count, output_count);
	if (cd == NULL) {
		return NULL;
	}
	cd->tp = tp;

	cd->base.update_inputs = touch_input_ctl_update_inputs;
	cd->base.get_tracked_pose = touch_input_ctl_get_tracked_pose;
	cd->base.get_view_poses = u_device_ni_get_view_poses;
	cd->base.set_output = touch_input_ctl_set_output;
	cd->base.destroy = touch_input_ctl_destroy;
	cd->base.supported.orientation_tracking = true;
	cd->base.supported.position_tracking = true;
	cd->base.name = XRT_DEVICE_SIMPLE_CONTROLLER;
	cd->base.device_type = XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER;

	// RIG_LOCAL (ADR-034 Amendment 4): display-plane-relative, anchored at the
	// initial rig by the builder so the ray follows every orbit. Never
	// TRACKING_TYPE_NONE — the builder injects per-hand arm-model offsets
	// into NONE-typed origins (same rule as qwerty / sim_input).
	cd->base.tracking_origin->type = XRT_TRACKING_TYPE_RIG_LOCAL;
	snprintf(cd->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Touch Rig-Local Volume");

	snprintf(cd->base.str, sizeof(cd->base.str), "%s", "Touch Controller");
	snprintf(cd->base.serial, sizeof(cd->base.serial), "%s", "TOUCH-INPUT-CTL");

	for (uint32_t i = 0; i < input_count; i++) {
		cd->base.inputs[i].active = false;
		cd->base.inputs[i].name = touch_input_ctl_inputs_array[i];
	}
	for (uint32_t i = 0; i < output_count; i++) {
		cd->base.outputs[i].name = touch_input_ctl_outputs_array[i];
	}

	u_var_add_root(cd, cd->base.str, true);
	u_var_add_bool(cd, &cd->select, "select");
	u_var_add_pose(cd, &cd->last_aim, "aim_rig_local");
	u_var_add_ro_u64(cd, &cd->haptic_event_count, "haptic_event_count");

	U_LOG_W("touch-input: emulated khr/simple_controller created — aim ray from the nominal viewer through the "
	        "finger, select = tap / press-and-hold.");
	return &cd->base;
}
