// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulated NAVIGATION device — the hardware-free reference rig driver.
 *
 * The input-side sibling of the scripted controllers (`sim_input_device.c`),
 * for the rig (navigation) role of ADR-034 Amendment 4 / #1380: one
 * `XRT_DEVICE_TYPE_NAVIGATION` device publishing
 * @ref XRT_INPUT_GENERIC_NAVIGATION_POSE and
 * @ref XRT_INPUT_GENERIC_NAVIGATION_RECENTER, so the whole role walk —
 * arbitration, alignment, hold, recenter — runs in CI with nothing plugged in.
 *
 * **N(t) is ABSOLUTE in the provider's own navigation frame F**, never a
 * delta: a slow figure-eight in F's XZ plane (period
 * @ref SIM_INPUT_NAV_PERIOD_S, radius @ref SIM_INPUT_NAV_RADIUS_M) plus a
 * gentle yaw about +Y. Like the controllers' circle it is a PURE FUNCTION of
 * the requested timestamp, so prediction for a future frame time is exact with
 * no relation-history buffer and CI runs are bit-reproducible. F's origin is
 * private to the provider — the runtime never interprets it, it aligns against
 * it (`T_world_F = rig_last o inv(N)`), which is exactly what makes a
 * non-identity N a real test of the composer rather than a no-op.
 *
 * Two optional scripted faults, both off by default, both pure functions of
 * the timestamp:
 *
 *   - **Hold** (`DXR_SIM_INPUT_NAV_HOLD_MS`): every n ms the pose reports
 *     INVALID for @ref SIM_INPUT_NAV_HOLD_WINDOW_NS, driving the composer's
 *     hold / re-align path (rig freezes, then continues from where it froze).
 *   - **Recenter** (`DXR_SIM_INPUT_NAV_RECENTER_MS`): every n ms a scripted
 *     reset fires. Per the contract the input is a DURABLE
 *     level-with-timestamp — `value.boolean` stays true and `timestamp` is the
 *     time of the most recent reset, never cleared — because `xrSyncActions`
 *     sweeps `update_inputs` over every device and a pulse would be eaten
 *     before the composer ever polled. The reset ALSO jumps the scripted pose
 *     (the script's phase steps by @ref SIM_INPUT_NAV_RESET_PHASE_RAD) in the
 *     SAME publication, because both are derived from the same reset index —
 *     which is the pairing the composer's recenter alignment is specified
 *     against.
 *
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

#include <stdio.h>


/*
 *
 * Structs and defines.
 *
 */

// Indices into base.inputs, matching sim_input_nav_inputs_array below.
#define SIM_INPUT_NAV_POSE 0
#define SIM_INPUT_NAV_RECENTER 1

//! One lap of the figure-eight. Slow on purpose: a rig that moves this far in
//! one frame would make "did it change?" indistinguishable from noise.
#define SIM_INPUT_NAV_PERIOD_S 20.0

//! Half-width of the figure-eight in F, metres.
#define SIM_INPUT_NAV_RADIUS_M 0.5

//! Peak yaw about +Y, radians — enough that a rotation-blind composer fails.
#define SIM_INPUT_NAV_YAW_RAD 0.35

/*!
 * Phase step applied at every scripted reset. Deliberately NOT a multiple of
 * 2π: the whole point is that the post-reset N is somewhere else, so the
 * composer must re-align rather than coast.
 */
#define SIM_INPUT_NAV_RESET_PHASE_RAD (2.0 * M_PI / 3.0)

//! How long the pose reports invalid once a hold window opens.
#define SIM_INPUT_NAV_HOLD_WINDOW_NS (500 * 1000 * 1000LL)

struct sim_input_nav_device
{
	struct xrt_device base;

	//! Hold-window period in ns; 0 = the pose is always valid.
	int64_t hold_period_ns;
	//! Scripted-reset period in ns; 0 = never recenter.
	int64_t recenter_period_ns;

	bool active;

	//! Diagnostics (u_var / tests): last published reset time and index.
	int64_t last_recenter_ns;
	uint64_t recenter_index;
};


/*
 *
 * Helper functions.
 *
 */

static inline struct sim_input_nav_device *
sim_input_nav_device(struct xrt_device *xdev)
{
	return (struct sim_input_nav_device *)xdev;
}

/*!
 * How many scripted resets have fired by @p at_timestamp_ns. Pure function of
 * the timestamp — which is what pairs the RECENTER timestamp published from
 * `update_inputs` with the phase jump seen by `get_tracked_pose`.
 */
static int64_t
nav_reset_index(const struct sim_input_nav_device *nd, int64_t at_timestamp_ns)
{
	if (nd->recenter_period_ns <= 0 || at_timestamp_ns <= 0) {
		return 0;
	}
	return at_timestamp_ns / nd->recenter_period_ns;
}

/*!
 * Is the navigation pose valid at @p at_timestamp_ns? False inside a scripted
 * hold window — the provider saying "hold the rig", not "I am gone"
 * (`get_presence` is unaffected; validity is not presence).
 */
static bool
nav_valid_at(const struct sim_input_nav_device *nd, int64_t at_timestamp_ns)
{
	if (nd->hold_period_ns <= 0) {
		return true;
	}

	int64_t window = SIM_INPUT_NAV_HOLD_WINDOW_NS;
	if (window > nd->hold_period_ns / 2) {
		// A period shorter than two windows would leave the pose
		// invalid most of the time; keep it a fault, not a floor.
		window = nd->hold_period_ns / 2;
	}

	int64_t phase = at_timestamp_ns % nd->hold_period_ns;
	if (phase < 0) {
		phase += nd->hold_period_ns;
	}
	return phase >= window;
}

/*!
 * N(t): the absolute rig pose in the provider's navigation frame F. A
 * figure-eight in the XZ plane (x = R sin θ, z = R sin θ cos θ) plus a yaw
 * about +Y, with θ advanced by @ref SIM_INPUT_NAV_RESET_PHASE_RAD per scripted
 * reset.
 */
static void
nav_script_pose(const struct sim_input_nav_device *nd, int64_t at_timestamp_ns, struct xrt_pose *out_pose)
{
	double t = (double)at_timestamp_ns * 1e-9;
	double theta = 2.0 * M_PI * t / SIM_INPUT_NAV_PERIOD_S +
	               (double)nav_reset_index(nd, at_timestamp_ns) * SIM_INPUT_NAV_RESET_PHASE_RAD;

	double s = sin(theta);
	double c = cos(theta);

	out_pose->position.x = (float)(SIM_INPUT_NAV_RADIUS_M * s);
	out_pose->position.y = 0.0f;
	out_pose->position.z = (float)(SIM_INPUT_NAV_RADIUS_M * s * c);

	// Yaw about +Y, built inline rather than through math_quat_* so this
	// unit stays free of the aux_math link (it is compiled straight into
	// the unit test).
	double half = 0.5 * SIM_INPUT_NAV_YAW_RAD * s;
	out_pose->orientation.x = 0.0f;
	out_pose->orientation.y = (float)sin(half);
	out_pose->orientation.z = 0.0f;
	out_pose->orientation.w = (float)cos(half);
}


/*
 *
 * Member functions.
 *
 */

static void
sim_input_nav_destroy(struct xrt_device *xdev)
{
	struct sim_input_nav_device *nd = sim_input_nav_device(xdev);

	u_var_remove_root(nd);
	u_device_free(&nd->base);
}

static xrt_result_t
sim_input_nav_update_inputs(struct xrt_device *xdev)
{
	struct sim_input_nav_device *nd = sim_input_nav_device(xdev);

	int64_t now = (int64_t)os_monotonic_get_ns();

	nd->base.inputs[SIM_INPUT_NAV_POSE].active = nd->active;
	nd->base.inputs[SIM_INPUT_NAV_POSE].timestamp = now;

	nd->base.inputs[SIM_INPUT_NAV_RECENTER].active = nd->active;

	if (!nd->active || nd->recenter_period_ns <= 0) {
		// Never-cleared is only meaningful once a reset has happened;
		// with the knob off there is nothing to report.
		nd->base.inputs[SIM_INPUT_NAV_RECENTER].value.boolean = false;
		nd->base.inputs[SIM_INPUT_NAV_RECENTER].timestamp = now;
		return XRT_SUCCESS;
	}

	// DURABLE level-with-timestamp: `true` forever, timestamp = the time of
	// the most recent scripted reset. The composer consumes a recenter as
	// "timestamp > last consumed", so it is immaterial how many other
	// callers (xrSyncActions, the IPC server) swept update_inputs in
	// between, and two resets between polls collapse to one.
	int64_t index = nav_reset_index(nd, now);
	int64_t reset_ns = index * nd->recenter_period_ns;

	nd->base.inputs[SIM_INPUT_NAV_RECENTER].value.boolean = true;
	nd->base.inputs[SIM_INPUT_NAV_RECENTER].timestamp = reset_ns;

	nd->last_recenter_ns = reset_ns;
	nd->recenter_index = (uint64_t)index;

	return XRT_SUCCESS;
}

static xrt_result_t
sim_input_nav_get_tracked_pose(struct xrt_device *xdev,
                               enum xrt_input_name name,
                               int64_t at_timestamp_ns,
                               struct xrt_space_relation *out_relation)
{
	struct sim_input_nav_device *nd = sim_input_nav_device(xdev);

	if (name != XRT_INPUT_GENERIC_NAVIGATION_POSE) {
		// Plain message, not U_LOG_XDEV_UNSUPPORTED_INPUT: the u_pp
		// helpers it uses aren't on the runtime DLL's aux export
		// surface (see sim_input_device.c).
		U_LOG_E("sim_input: unsupported navigation input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation zero = XRT_SPACE_RELATION_ZERO;
	*out_relation = zero;

	if (!nd->active) {
		out_relation->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
		return XRT_SUCCESS;
	}

	// The scripted pose is published either way; the FLAGS are the
	// provider's "I have navigation authority" say-so, and clearing them
	// is how a provider asks the runtime to hold the rig.
	nav_script_pose(nd, at_timestamp_ns, &out_relation->pose);

	if (!nav_valid_at(nd, at_timestamp_ns)) {
		out_relation->relation_flags = (enum xrt_space_relation_flags)0;
		return XRT_SUCCESS;
	}

	// VALID but not TRACKED: the composer ignores the TRACKED bits for this
	// input by contract, and a scripted pose has no tracker behind it.
	out_relation->relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                               XRT_SPACE_RELATION_POSITION_VALID_BIT);

	return XRT_SUCCESS;
}


/*
 *
 * Data arrays.
 *
 */

static enum xrt_input_name sim_input_nav_inputs_array[] = {
    XRT_INPUT_GENERIC_NAVIGATION_POSE,
    XRT_INPUT_GENERIC_NAVIGATION_RECENTER,
};


/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_device *
sim_input_create_navigation(int64_t hold_period_ms, int64_t recenter_period_ms)
{
	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	const uint32_t input_count = ARRAY_SIZE(sim_input_nav_inputs_array);

	struct sim_input_nav_device *nd = U_DEVICE_ALLOCATE(struct sim_input_nav_device, flags, input_count, 0);
	if (nd == NULL) {
		return NULL;
	}

	nd->base.update_inputs = sim_input_nav_update_inputs;
	nd->base.get_tracked_pose = sim_input_nav_get_tracked_pose;
	nd->base.get_view_poses = u_device_ni_get_view_poses;
	nd->base.destroy = sim_input_nav_destroy;
	nd->base.supported.orientation_tracking = true;
	nd->base.supported.position_tracking = true;

	// XRT_DEVICE_INVALID on purpose: `xrt_device_name` selects an
	// INTERACTION PROFILE, and a navigation device binds none — it is read
	// by the rig composer, never by the action system. Only the hand-role
	// devices need a profile-bearing name.
	nd->base.name = XRT_DEVICE_INVALID;
	nd->base.device_type = XRT_DEVICE_TYPE_NAVIGATION;

	// TYPE_OTHER, NOT RIG_LOCAL: this device's poses live in the
	// provider's private navigation frame F, which the runtime aligns
	// against rather than anchors. RIG_LOCAL is for the CONTROLLERS, whose
	// poses are display-plane-relative and do get anchored at the initial
	// rig (u_builders.c, anchor_rig_local_origins).
	nd->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	snprintf(nd->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Sim Navigation Frame");

	snprintf(nd->base.str, sizeof(nd->base.str), "%s", "Sim navigation");
	snprintf(nd->base.serial, sizeof(nd->base.serial), "%s", "SIM-INPUT-NAV");

	for (uint32_t i = 0; i < input_count; i++) {
		nd->base.inputs[i].active = true;
		nd->base.inputs[i].name = sim_input_nav_inputs_array[i];
	}

	nd->hold_period_ns = hold_period_ms > 0 ? hold_period_ms * 1000 * 1000 : 0;
	nd->recenter_period_ns = recenter_period_ms > 0 ? recenter_period_ms * 1000 * 1000 : 0;
	nd->active = true;

	u_var_add_root(nd, nd->base.str, true);
	u_var_add_bool(nd, &nd->active, "active");
	u_var_add_ro_i64(nd, &nd->hold_period_ns, "hold_period_ns");
	u_var_add_ro_i64(nd, &nd->recenter_period_ns, "recenter_period_ns");
	u_var_add_ro_i64(nd, &nd->last_recenter_ns, "last_recenter_ns");
	u_var_add_ro_u64(nd, &nd->recenter_index, "recenter_index");

	U_LOG_W("sim-input: scripted navigation device — hold=%lld ms, recenter=%lld ms (0 = off).",
	        (long long)hold_period_ms, (long long)recenter_period_ms);

	return &nd->base;
}
