// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The gamepad NAVIGATION device — the rig driver (ADR-034 Amendment 4).
 *
 * One `XRT_DEVICE_TYPE_NAVIGATION` device whose
 * `XRT_INPUT_GENERIC_NAVIGATION_POSE` is N(t), the hub's integrated pose in
 * the provider's frame F (absolute, never a delta, never a viewer pose —
 * eye-tracked parallax is applied by the runtime at view level and never
 * enters N, Amendment 2), and whose `XRT_INPUT_GENERIC_NAVIGATION_RECENTER`
 * is the durable level-with-timestamp the composer consumes (§4b.3).
 *
 * Presence is not validity (§4b.2): a detached pad flips PRESENCE and hands
 * the rig to the fly camera; the pose here stays VALID whenever the backend
 * produced a sample, so a pad that is attached but idle simply holds the
 * rig where it is.
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


#define NAV_POSE 0
#define NAV_RECENTER 1

struct gamepad_nav_device
{
	struct xrt_device base;
	struct gamepad_hub *hub;

	//! Diagnostics (u_var).
	struct xrt_pose last_pose;
	int64_t last_recenter_ns;
};

static const enum xrt_input_name nav_inputs_array[] = {
    XRT_INPUT_GENERIC_NAVIGATION_POSE,
    XRT_INPUT_GENERIC_NAVIGATION_RECENTER,
};

static inline struct gamepad_nav_device *
nav_device(struct xrt_device *xdev)
{
	return (struct gamepad_nav_device *)xdev;
}

static void
nav_destroy(struct xrt_device *xdev)
{
	struct gamepad_nav_device *nd = nav_device(xdev);
	struct gamepad_hub *hub = nd->hub;

	u_var_remove_root(nd);
	u_device_free(&nd->base);
	gamepad_hub_release(hub);
}

static xrt_result_t
nav_update_inputs(struct xrt_device *xdev)
{
	struct gamepad_nav_device *nd = nav_device(xdev);

	const int64_t now = (int64_t)os_monotonic_get_ns();
	struct xrt_pose pose;
	bool valid = false;
	int64_t recenter_ns = 0;
	gamepad_hub_nav_sample(nd->hub, now, &pose, &valid, &recenter_ns);

	xdev->inputs[NAV_POSE].active = true;
	xdev->inputs[NAV_POSE].timestamp = now;

	// DURABLE level-with-timestamp: true forever once a recenter happened,
	// timestamp = the time of the most recent one, never cleared — immune
	// to however many update_inputs sweeps land between composer polls.
	xdev->inputs[NAV_RECENTER].active = true;
	xdev->inputs[NAV_RECENTER].value.boolean = recenter_ns != 0;
	xdev->inputs[NAV_RECENTER].timestamp = recenter_ns != 0 ? recenter_ns : now;
	nd->last_recenter_ns = recenter_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
nav_get_tracked_pose(struct xrt_device *xdev,
                     enum xrt_input_name name,
                     int64_t at_timestamp_ns,
                     struct xrt_space_relation *out_relation)
{
	struct gamepad_nav_device *nd = nav_device(xdev);

	if (name != XRT_INPUT_GENERIC_NAVIGATION_POSE) {
		U_LOG_E("gamepad: unsupported navigation input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation zero = XRT_SPACE_RELATION_ZERO;
	*out_relation = zero;

	bool valid = false;
	int64_t recenter_ns = 0;
	gamepad_hub_nav_sample(nd->hub, at_timestamp_ns, &out_relation->pose, &valid, &recenter_ns);
	nd->last_pose = out_relation->pose;

	if (!valid) {
		return XRT_SUCCESS; // flags clear = "hold the rig"
	}

	// VALID but not TRACKED: the composer ignores the TRACKED bits for this
	// input by contract, and an integrated stick has no tracker behind it.
	out_relation->relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                               XRT_SPACE_RELATION_POSITION_VALID_BIT);
	return XRT_SUCCESS;
}

struct xrt_device *
gamepad_create_navigation(struct gamepad_hub *hub)
{
	if (hub == NULL) {
		return NULL;
	}
	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	const uint32_t input_count = ARRAY_SIZE(nav_inputs_array);

	struct gamepad_nav_device *nd = U_DEVICE_ALLOCATE(struct gamepad_nav_device, flags, input_count, 0);
	if (nd == NULL) {
		return NULL;
	}
	nd->hub = hub;
	gamepad_hub_retain(hub);

	nd->base.update_inputs = nav_update_inputs;
	nd->base.get_tracked_pose = nav_get_tracked_pose;
	nd->base.get_view_poses = u_device_ni_get_view_poses;
	nd->base.destroy = nav_destroy;
	nd->base.supported.orientation_tracking = true;
	nd->base.supported.position_tracking = true;

	// XRT_DEVICE_INVALID on purpose: a navigation device binds no
	// interaction profile — it is read by the rig composer, never by the
	// action system (same as sim_input's).
	nd->base.name = XRT_DEVICE_INVALID;
	nd->base.device_type = XRT_DEVICE_TYPE_NAVIGATION;

	// TYPE_OTHER, not RIG_LOCAL: these poses live in the provider's private
	// frame F, which the runtime aligns against rather than anchors.
	nd->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	snprintf(nd->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Gamepad Navigation Frame");

	snprintf(nd->base.str, sizeof(nd->base.str), "%s", "Gamepad navigation");
	snprintf(nd->base.serial, sizeof(nd->base.serial), "%s", "GAMEPAD-NAV");

	for (uint32_t i = 0; i < input_count; i++) {
		nd->base.inputs[i].active = true;
		nd->base.inputs[i].name = nav_inputs_array[i];
	}
	nd->last_pose = (struct xrt_pose)XRT_POSE_IDENTITY;

	u_var_add_root(nd, nd->base.str, true);
	u_var_add_pose(nd, &nd->last_pose, "nav_pose");
	u_var_add_ro_i64(nd, &nd->last_recenter_ns, "last_recenter_ns");

	return &nd->base;
}
