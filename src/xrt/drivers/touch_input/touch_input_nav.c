// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The touch NAVIGATION device — the rig role from gestures (ADR-034
 *         Amendment 4, #132).
 *
 * One `XRT_DEVICE_TYPE_NAVIGATION` device publishing
 * @ref XRT_INPUT_GENERIC_NAVIGATION_POSE — the absolute rig pose `N(t)` in the
 * provider's own frame F, which the runtime aligns to the world once and then
 * composes (`T_world_F ∘ N(t)`) — and @ref XRT_INPUT_GENERIC_NAVIGATION_RECENTER,
 * the durable level-with-timestamp a double tap raises.
 *
 * Latest-state, not predicted: a finger reports its position, not a velocity
 * the runtime could usefully extrapolate, and the composer reads this once per
 * frame right after `xrSyncActions` pumped the queue. `at_timestamp_ns` is
 * therefore ignored, which the contract allows for a source with no sensor
 * behind it (sim_input's scripted device does the same for the opposite
 * reason).
 *
 * @ingroup drv_touch_input
 */

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_var.h"

#include "touch_gesture.h"
#include "touch_input_interface.h"
#include "touch_input_provider.h"

#include <stdio.h>


// Indices into base.inputs, matching touch_input_nav_inputs_array below.
#define TOUCH_INPUT_NAV_POSE 0
#define TOUCH_INPUT_NAV_RECENTER 1

struct touch_input_nav_device
{
	struct xrt_device base;
	struct touch_input_provider *tp;

	//! Diagnostics (u_var): the last published rig pose and recenter time.
	struct xrt_pose last_pose;
	int64_t last_recenter_ns;
};

static inline struct touch_input_nav_device *
touch_input_nav_device(struct xrt_device *xdev)
{
	return (struct touch_input_nav_device *)xdev;
}

static void
touch_input_nav_destroy(struct xrt_device *xdev)
{
	struct touch_input_nav_device *nd = touch_input_nav_device(xdev);
	u_var_remove_root(nd);
	u_device_free(&nd->base);
}

static xrt_result_t
touch_input_nav_update_inputs(struct xrt_device *xdev)
{
	struct touch_input_nav_device *nd = touch_input_nav_device(xdev);
	const int64_t now = (int64_t)os_monotonic_get_ns();

	touch_input_pump(nd->tp, now);

	os_mutex_lock(&nd->tp->mutex);
	const int64_t recenter = touch_gesture_recenter_timestamp(&nd->tp->gesture);
	os_mutex_unlock(&nd->tp->mutex);

	nd->base.inputs[TOUCH_INPUT_NAV_POSE].active = true;
	nd->base.inputs[TOUCH_INPUT_NAV_POSE].timestamp = now;

	// DURABLE level-with-timestamp: true from the first double tap on,
	// timestamp = the most recent one, never cleared. The composer acts on
	// `timestamp > last consumed`, so however many update_inputs sweeps land
	// in between, a recenter is never eaten and two collapse to one.
	nd->base.inputs[TOUCH_INPUT_NAV_RECENTER].active = true;
	nd->base.inputs[TOUCH_INPUT_NAV_RECENTER].value.boolean = recenter != 0;
	nd->base.inputs[TOUCH_INPUT_NAV_RECENTER].timestamp = recenter != 0 ? recenter : now;
	nd->last_recenter_ns = recenter;

	return XRT_SUCCESS;
}

static xrt_result_t
touch_input_nav_get_tracked_pose(struct xrt_device *xdev,
                                 enum xrt_input_name name,
                                 int64_t at_timestamp_ns,
                                 struct xrt_space_relation *out_relation)
{
	struct touch_input_nav_device *nd = touch_input_nav_device(xdev);
	(void)at_timestamp_ns;

	if (name != XRT_INPUT_GENERIC_NAVIGATION_POSE) {
		// Plain message, not U_LOG_XDEV_UNSUPPORTED_INPUT: the u_pp helpers
		// are not on the runtime DLL's aux export surface (sim_input note).
		U_LOG_E("touch_input: unsupported navigation input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation zero = XRT_SPACE_RELATION_ZERO;
	*out_relation = zero;

	os_mutex_lock(&nd->tp->mutex);
	touch_gesture_nav_pose(&nd->tp->gesture, &out_relation->pose);
	os_mutex_unlock(&nd->tp->mutex);
	nd->last_pose = out_relation->pose;

	// VALID but not TRACKED: the composer ignores the TRACKED bits for this
	// input by contract, and a gesture has no tracker behind it. Always
	// valid — an untouched rig is at rest, not lost.
	out_relation->relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                               XRT_SPACE_RELATION_POSITION_VALID_BIT);
	return XRT_SUCCESS;
}

static enum xrt_input_name touch_input_nav_inputs_array[] = {
    XRT_INPUT_GENERIC_NAVIGATION_POSE,
    XRT_INPUT_GENERIC_NAVIGATION_RECENTER,
};

struct xrt_device *
touch_input_create_navigation(struct touch_input_provider *tp)
{
	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	const uint32_t input_count = ARRAY_SIZE(touch_input_nav_inputs_array);

	struct touch_input_nav_device *nd = U_DEVICE_ALLOCATE(struct touch_input_nav_device, flags, input_count, 0);
	if (nd == NULL) {
		return NULL;
	}
	nd->tp = tp;

	nd->base.update_inputs = touch_input_nav_update_inputs;
	nd->base.get_tracked_pose = touch_input_nav_get_tracked_pose;
	nd->base.get_view_poses = u_device_ni_get_view_poses;
	nd->base.destroy = touch_input_nav_destroy;
	nd->base.supported.orientation_tracking = true;
	nd->base.supported.position_tracking = true;

	// XRT_DEVICE_INVALID on purpose: `xrt_device_name` selects an interaction
	// profile and a navigation device binds none — the rig composer reads it,
	// never the action system.
	nd->base.name = XRT_DEVICE_INVALID;
	nd->base.device_type = XRT_DEVICE_TYPE_NAVIGATION;

	// TYPE_OTHER, not RIG_LOCAL: this device's poses live in the provider's
	// private navigation frame F, which the runtime aligns against rather than
	// anchors (sim_input_nav.c has the same note). RIG_LOCAL is for the
	// controller.
	nd->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	snprintf(nd->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Touch Navigation Frame");

	snprintf(nd->base.str, sizeof(nd->base.str), "%s", "Touch navigation");
	snprintf(nd->base.serial, sizeof(nd->base.serial), "%s", "TOUCH-INPUT-NAV");

	for (uint32_t i = 0; i < input_count; i++) {
		nd->base.inputs[i].active = true;
		nd->base.inputs[i].name = touch_input_nav_inputs_array[i];
	}

	u_var_add_root(nd, nd->base.str, true);
	u_var_add_pose(nd, &nd->last_pose, "rig_pose_F");
	u_var_add_ro_i64(nd, &nd->last_recenter_ns, "last_recenter_ns");

	U_LOG_W("touch-input: navigation device created — one-finger drag orbits, pinch dollies, two-finger drag "
	        "pans, double tap recenters.");
	return &nd->base;
}
