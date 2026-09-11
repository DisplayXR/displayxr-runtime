// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The rig composer: the head device's pose source (#1380, ADR-034
 *         Amendment 4).
 * @ingroup xrt_target_common
 *
 * The state machine, in five lines:
 *
 *   - The rig role sits on the qwerty floor  -> `rig(t)` IS the qwerty pose.
 *   - A provider takes the role              -> align once: `T_world_F = rig_last o inv(N)`.
 *   - Aligned and N valid                    -> `rig(t) = T_world_F o N(t)`.
 *   - N invalid                              -> hold `rig_last`, re-align on the next valid N.
 *   - A newer recenter timestamp             -> `rig := rig_initial` and re-align there.
 *
 * Continuity is by construction: at a handover, at an invalid->valid
 * transition and at any non-recenter re-alignment the first composed pose is
 * exactly `rig_last`, because `T_world_F o N = (rig_last o inv(N)) o N`. The
 * single deliberate jump is a recenter, which lands on `rig_initial` — the rig
 * pose at system build, which @ref u_space_overseer also captures as its
 * `rig_initial` when it arms the rig source on this very head device
 * (`u_builders.c`, rig-source arming), up to the head's tracking-origin offset
 * (see the seeding comment in @ref t_rig_composer_create). Hands follow a
 * recenter, which is correct: a recenter is voluntary (ADR-034 Amendment 2).
 */

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"

#include "math/m_api.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include "target_rig_composer.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty/qwerty_interface.h"
#endif

#include <stdlib.h>
#include <string.h>


/*
 *
 * Struct and helpers.
 *
 */

/*!
 * The composer. Every mutable field below is under @ref rig_composer::mutex:
 * @ref rig_composer_get_tracked_pose is called concurrently by the in-process
 * app thread and by one thread per IPC client.
 */
struct rig_composer
{
	struct xrt_device base;

	struct os_mutex mutex;

	//! Not owned. The qwerty HMD — the rig floor, reseeded on handback.
	struct xrt_device *qwerty_hmd;
	//! Not owned. Read for roles and for the navigation device.
	struct xrt_system_devices *xsysd;
	//! Not owned. Logging only.
	struct xrt_device *head;

	//! Cached `roles.rig`; -1 = the qwerty floor holds the rig.
	int32_t holder;
	//! Last roles generation seen. Diagnostic only — decisions key on @ref holder.
	uint64_t roles_generation;

	//! The rig at composer creation (the qwerty seed). The recenter "home".
	struct xrt_pose rig_initial;
	//! Last `rig(t)` returned; also the value held while the rig cannot advance.
	struct xrt_pose rig_last;
	//! Alignment: world <- the provider's navigation frame F.
	struct xrt_pose T_world_F;
	//! Is @ref T_world_F valid for the current holder?
	bool aligned;

	//! Last consumed NAVIGATION_RECENTER timestamp (0 = none).
	int64_t recenter_last_ts;
	//! > 0 = a recenter with this timestamp is pending alignment.
	int64_t recenter_pending;
	//! Validity of N at the previous poll (diagnostic).
	bool nav_was_valid;
};

static inline struct rig_composer *
rig_composer(struct xrt_device *xdev)
{
	return (struct rig_composer *)xdev;
}

//! The flag shape qwerty returns, so the head's pose reads the same either way.
static const enum xrt_space_relation_flags rig_composer_flags =
    (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |   //
                                    XRT_SPACE_RELATION_POSITION_VALID_BIT |      //
                                    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | //
                                    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

/*!
 * Find an input by name on a device. @ref u_device.h has no such helper, and
 * the navigation device's RECENTER input is optional, so absence is normal.
 */
static struct xrt_input *
rig_composer_find_input(struct xrt_device *xdev, enum xrt_input_name name)
{
	if (xdev == NULL || xdev->inputs == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < xdev->input_count; i++) {
		if (xdev->inputs[i].name == name) {
			return &xdev->inputs[i];
		}
	}
	return NULL;
}

//! Write @p pose out with the standard flags and a zero velocity.
static void
rig_composer_emit(struct xrt_space_relation *out_relation, const struct xrt_pose *pose)
{
	struct xrt_space_relation zero = XRT_SPACE_RELATION_ZERO;
	*out_relation = zero;
	out_relation->pose = *pose;
	out_relation->relation_flags = rig_composer_flags;
}

/*!
 * Resolve `roles.rig` to a usable index into `xsysd->xdevs`, or -1 for the
 * qwerty floor. An out-of-range or NULL entry is treated as the floor rather
 * than trusted — a bad index here would be a dangling call every frame.
 */
static int32_t
rig_composer_resolve_holder(struct rig_composer *rc)
{
	if (rc->xsysd == NULL || rc->xsysd->get_roles == NULL) {
		return -1;
	}

	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	if (xrt_system_devices_get_roles(rc->xsysd, &roles) != XRT_SUCCESS) {
		return -1;
	}

	rc->roles_generation = roles.generation_id;

	int32_t rig = roles.rig;
	if (rig < 0 || (uint32_t)rig >= rc->xsysd->xdev_count || rc->xsysd->xdevs[rig] == NULL) {
		return -1;
	}
	return rig;
}


/*
 *
 * Device functions.
 *
 */

static xrt_result_t
rig_composer_update_inputs(struct xrt_device *xdev)
{
	(void)xdev;
	return XRT_SUCCESS;
}

static xrt_result_t
rig_composer_get_tracked_pose(struct xrt_device *xdev,
                              enum xrt_input_name name,
                              int64_t at_timestamp_ns,
                              struct xrt_space_relation *out_relation)
{
	struct rig_composer *rc = rig_composer(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		U_LOG_XDEV_UNSUPPORTED_INPUT(xdev, U_LOGGING_WARN, name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	os_mutex_lock(&rc->mutex);

	/*
	 * 1. Roles. A handover is a change of HOLDER, not every generation
	 *    bump: hand-role churn shares the counter and must not re-align.
	 */
	int32_t want = rig_composer_resolve_holder(rc);
	if (want != rc->holder) {
		// The rig continues from where it IS, never from the new
		// holder's frame. rig_last is deliberately left untouched.
		const struct xrt_pose rig_h = rc->rig_last;
		const int32_t old = rc->holder;
		const char *new_name = "qwerty (floor)";

		rc->holder = want;
		rc->aligned = false;

		// A pending recenter belongs to the tenure it was raised in. If
		// that provider disappears before the recenter could land, the
		// request dies with it — it must never fire into the NEXT
		// holder's frame. recenter_last_ts is deliberately kept, so a
		// stale timestamp republished by a later provider is still
		// ignored; only a strictly newer one recenters.
		rc->recenter_pending = 0;

		if (want >= 0) {
			// Alignment happens below, on the first valid N.
			rc->nav_was_valid = false;
			if (rc->xsysd->xdevs[want]->str[0] != '\0') {
				new_name = rc->xsysd->xdevs[want]->str;
			}
		} else {
#ifdef XRT_BUILD_DRIVER_QWERTY
			// So WASD continues from the current rig instead of
			// from wherever qwerty's own integrator drifted to
			// while a provider was driving.
			if (rc->qwerty_hmd != NULL) {
				qwerty_set_hmd_pose(rc->qwerty_hmd, &rig_h);
			}
#endif
		}

		// One WARN per handover: a lifecycle event, not a frame event.
		U_LOG_W("Rig composer: rig role %d -> %d ('%s'), continuing at (%.3f, %.3f, %.3f).", old, want,
		        new_name, (double)rig_h.position.x, (double)rig_h.position.y, (double)rig_h.position.z);
	}

	/*
	 * 2. The qwerty floor holds. WASD/mouse-look act only here; while a
	 *    provider holds, qwerty is not read at all and is reseeded on
	 *    handback.
	 */
	if (rc->holder < 0) {
		xrt_result_t xret = XRT_ERROR_INPUT_UNSUPPORTED;
		if (rc->qwerty_hmd != NULL) {
			xret = xrt_device_get_tracked_pose(rc->qwerty_hmd, XRT_INPUT_GENERIC_HEAD_POSE, at_timestamp_ns,
			                                   out_relation);
		}
		if (xret == XRT_SUCCESS) {
			rc->rig_last = out_relation->pose;
		} else {
			rig_composer_emit(out_relation, &rc->rig_last);
		}
		os_mutex_unlock(&rc->mutex);
		return XRT_SUCCESS;
	}

	/*
	 * 3. A provider holds.
	 */
	struct xrt_device *nav = rc->xsysd->xdevs[rc->holder];

	// The composer does NOT rely on being the only caller: xrSyncActions
	// sweeps update_inputs over every device too. That is exactly why
	// RECENTER is a durable level-with-timestamp and not a pulse — extra
	// calls in between are harmless.
	xrt_device_update_inputs(nav);

	// Recenter read (durable). Two resets between polls collapse to one:
	// only the latest timestamp is ever seen, and recenter is idempotent.
	struct xrt_input *recenter = rig_composer_find_input(nav, XRT_INPUT_GENERIC_NAVIGATION_RECENTER);
	if (recenter != NULL && recenter->active && recenter->value.boolean &&
	    recenter->timestamp > rc->recenter_last_ts) {
		rc->recenter_last_ts = recenter->timestamp;
		rc->recenter_pending = recenter->timestamp;
	}

	// Navigation sample. TRACKED bits are ignored for this input: the
	// VALID bits are the provider's "I have navigation authority" say-so.
	struct xrt_space_relation nav_rel = XRT_SPACE_RELATION_ZERO;
	bool valid = false;
	if (xrt_device_get_tracked_pose(nav, XRT_INPUT_GENERIC_NAVIGATION_POSE, at_timestamp_ns, &nav_rel) ==
	    XRT_SUCCESS) {
		valid = (nav_rel.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0 &&
		        (nav_rel.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0;
	}

	/*
	 * 4. Epochs / alignment. Recenter wins when both are due in one poll.
	 */
	bool compose = false;
	struct xrt_pose inv_nav = XRT_POSE_IDENTITY;

	if (rc->recenter_pending > 0) {
		if (valid && at_timestamp_ns >= rc->recenter_pending) {
			math_pose_invert(&nav_rel.pose, &inv_nav);
			math_pose_transform(&rc->rig_initial, &inv_nav, &rc->T_world_F);
			rc->aligned = true;
			rc->recenter_pending = 0;
			rc->rig_last = rc->rig_initial;
			rc->nav_was_valid = valid;

			// Exactly rig_initial: the one deliberate jump.
			rig_composer_emit(out_relation, &rc->rig_initial);
			os_mutex_unlock(&rc->mutex);
			return XRT_SUCCESS;
		}
		// Hold; the recenter stays pending.
	} else if (!rc->aligned) {
		if (valid) {
			math_pose_invert(&nav_rel.pose, &inv_nav);
			math_pose_transform(&rc->rig_last, &inv_nav, &rc->T_world_F);
			rc->aligned = true;
			compose = true;
		}
		// Else hold.
	} else if (!valid) {
		// Drop the alignment so the next valid sample re-aligns at the
		// held rig — the invalid->valid epoch.
		rc->aligned = false;
	} else {
		compose = true;
	}

	/*
	 * 5. Compose. T_world_F on the LEFT: a provider-local step
	 *    `N -> N o T(0,0,-1)` moves the rig along its OWN forward.
	 */
	if (compose) {
		math_pose_transform(&rc->T_world_F, &nav_rel.pose, &rc->rig_last);
	}

	/*
	 * 6. Diagnostics.
	 */
	rc->nav_was_valid = valid;

	rig_composer_emit(out_relation, &rc->rig_last);
	os_mutex_unlock(&rc->mutex);
	return XRT_SUCCESS;
}

static void
rig_composer_destroy(struct xrt_device *xdev)
{
	struct rig_composer *rc = rig_composer(xdev);

	os_mutex_destroy(&rc->mutex);
	u_device_free(&rc->base);
}


/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_device *
t_rig_composer_create(struct xrt_device *qwerty_hmd, struct xrt_system_devices *xsysd, struct xrt_device *head)
{
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_TRACKING_NONE);

	struct rig_composer *rc = U_DEVICE_ALLOCATE(struct rig_composer, flags, 1, 0);
	if (rc == NULL) {
		return NULL;
	}

	rc->base.name = XRT_DEVICE_GENERIC_HMD;
	rc->base.device_type = XRT_DEVICE_TYPE_HMD;
	rc->base.update_inputs = rig_composer_update_inputs;
	rc->base.get_tracked_pose = rig_composer_get_tracked_pose;
	rc->base.destroy = rig_composer_destroy;
	rc->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	rc->base.inputs[0].active = true;
	snprintf(rc->base.str, sizeof(rc->base.str), "Rig composer");
	snprintf(rc->base.serial, sizeof(rc->base.serial), "Rig composer");

	rc->qwerty_hmd = qwerty_hmd;
	rc->xsysd = xsysd;
	rc->head = head;

	rc->holder = -1;
	rc->roles_generation = 0;
	rc->aligned = false;
	rc->recenter_last_ts = 0;
	rc->recenter_pending = 0;
	rc->nav_was_valid = false;
	rc->T_world_F = (struct xrt_pose)XRT_POSE_IDENTITY;

	// Seed the rig at the qwerty pose. This is "home": what a recenter
	// returns to. @ref u_space_overseer captures its own rig_initial when it
	// arms the rig source on this head device, but in a DIFFERENT frame: the
	// composer's rig_initial is in the head DEVICE frame, while the overseer
	// resolves its copy through the head's tracking-origin offset (the root
	// frame). On the sim_display / Leia targets that offset is identity
	// (origin type OTHER), so the two log lines print the same numbers; with
	// a non-identity origin offset they differ by exactly that constant
	// offset, which cancels in every composition — so recenter and
	// `world = rig(t) o L` stay exact either way. The overseer logs its copy
	// ("Rig source ... armed at"); this WARN is the other half of the pair.
	struct xrt_pose seed = XRT_POSE_IDENTITY;
	if (qwerty_hmd != NULL) {
		struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
		if (xrt_device_get_tracked_pose(qwerty_hmd, XRT_INPUT_GENERIC_HEAD_POSE, os_monotonic_get_ns(), &rel) ==
		    XRT_SUCCESS) {
			seed = rel.pose;
		}
	}
	rc->rig_initial = seed;
	rc->rig_last = seed;

	if (os_mutex_init(&rc->mutex) != 0) {
		u_device_free(&rc->base);
		return NULL;
	}

	U_LOG_W("Rig composer created for head '%s': rig_initial = (%.3f, %.3f, %.3f).",
	        head != NULL ? head->str : "(none)", (double)seed.position.x, (double)seed.position.y,
	        (double)seed.position.z);

	return &rc->base;
}

void
t_rig_composer_destroy(struct xrt_device **inout_xdev)
{
	if (inout_xdev == NULL || *inout_xdev == NULL) {
		return;
	}
	xrt_device_destroy(inout_xdev);
}
