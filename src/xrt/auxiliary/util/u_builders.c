// Copyright 2022-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Helpers for @ref xrt_builder implementations.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_util
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_tracking.h"

#include "math/m_api.h"

#include "os/os_time.h"

#include "util/u_debug.h"
#include "util/u_builders.h"
#include "util/u_logging.h"
#include "util/u_system_helpers.h"
#include "util/u_space_overseer.h"


DEBUG_GET_ONCE_FLOAT_OPTION(tracking_origin_offset_x, "XRT_TRACKING_ORIGIN_OFFSET_X", 0.0f)
DEBUG_GET_ONCE_FLOAT_OPTION(tracking_origin_offset_y, "XRT_TRACKING_ORIGIN_OFFSET_Y", 0.0f)
DEBUG_GET_ONCE_FLOAT_OPTION(tracking_origin_offset_z, "XRT_TRACKING_ORIGIN_OFFSET_Z", 0.0f)


/*
 *
 * Helper functions.
 *
 */

static void
apply_offset(struct xrt_vec3 *position, struct xrt_vec3 *offset)
{
	position->x += offset->x;
	position->y += offset->y;
	position->z += offset->z;
}

/*!
 * Anchor every `XRT_TRACKING_TYPE_RIG_LOCAL` tracking origin at the INITIAL
 * rig (#1380 / ADR-034 Amendment 4).
 *
 * A provider that navigates publishes its controllers and joints
 * display-plane-relative — origin at the display centre, +X right, +Y up, +Z
 * toward the viewer. Those poses only become world poses once something says
 * where the display plane was when the system was built. That is exactly
 * `u_space_overseer`'s `rig_initial`, so setting it as the origin's
 * `initial_offset` makes the existing rig delta collapse to the identity the
 * contract asks for:
 *
 *     world = rig(t) o inv(rig_initial) o rig_initial o L = rig(t) o L
 *
 * `rig_initial` is read here the same way @ref u_space_overseer_set_rig_source
 * reads it a moment later: the head device's pose right now, resolved through
 * its tracking origin's offset (the one level `u_space_overseer_legacy_setup`
 * builds), and the identity when the head has no valid pose. It has to happen
 * HERE — after @ref u_builder_setup_tracking_origins has settled the head's
 * own offset, and before `legacy_setup` snapshots each origin's
 * `initial_offset` into a space — because the overseer never re-reads it.
 *
 * Origins of any other type (OTHER, NONE, …) are left exactly as they were:
 * a stage-anchored provider keeps its own mount offset.
 */
static void
anchor_rig_local_origins(struct xrt_device *head, struct xrt_device **xdevs, uint32_t xdev_count)
{
	if (head == NULL || head->tracking_origin == NULL || xdevs == NULL) {
		return;
	}

	// Cheap pre-pass: most systems have no RIG_LOCAL origin at all, and
	// polling the head for a pose is not free.
	bool any = false;
	for (uint32_t i = 0; i < xdev_count; i++) {
		if (xdevs[i] != NULL && xdevs[i]->tracking_origin != NULL &&
		    xdevs[i]->tracking_origin->type == XRT_TRACKING_TYPE_RIG_LOCAL) {
			any = true;
			break;
		}
	}
	if (!any) {
		return;
	}

	struct xrt_pose rig_initial = XRT_POSE_IDENTITY;

	struct xrt_space_relation head_rel = XRT_SPACE_RELATION_ZERO;
	xrt_device_get_tracked_pose(head, XRT_INPUT_GENERIC_HEAD_POSE, os_monotonic_get_ns(), &head_rel);

	const enum xrt_space_relation_flags needed =
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT;
	if ((head_rel.relation_flags & needed) == needed) {
		// The head's space hangs off its tracking origin's offset
		// space, so the world pose is offset o pose.
		math_pose_transform(&head->tracking_origin->initial_offset, &head_rel.pose, &rig_initial);
	}

	for (uint32_t i = 0; i < xdev_count; i++) {
		struct xrt_device *xdev = xdevs[i];
		if (xdev == NULL || xdev->tracking_origin == NULL ||
		    xdev->tracking_origin->type != XRT_TRACKING_TYPE_RIG_LOCAL) {
			continue;
		}

		// Several devices routinely share one origin (a provider's
		// left and right controllers do); anchor it once.
		bool already = false;
		for (uint32_t k = 0; k < i; k++) {
			if (xdevs[k] != NULL && xdevs[k]->tracking_origin == xdev->tracking_origin) {
				already = true;
				break;
			}
		}
		if (already) {
			continue;
		}

		xdev->tracking_origin->initial_offset = rig_initial;

		U_LOG_W("Rig-local origin '%s' anchored at the initial rig (%.3f, %.3f, %.3f).",
		        xdev->tracking_origin->name, rig_initial.position.x, rig_initial.position.y,
		        rig_initial.position.z);
	}
}


/*
 *
 * 'Exported' function.
 *
 */

struct xrt_prober_device *
u_builder_find_prober_device(struct xrt_prober_device *const *xpdevs,
                             size_t xpdev_count,
                             uint16_t vendor_id,
                             uint16_t product_id,
                             enum xrt_bus_type bus_type)
{
	for (size_t i = 0; i < xpdev_count; i++) {
		struct xrt_prober_device *xpdev = xpdevs[i];
		if (xpdev->product_id != product_id || //
		    xpdev->vendor_id != vendor_id ||   //
		    xpdev->bus != bus_type) {
			continue;
		}

		return xpdev;
	}

	return NULL;
}

void
u_builder_search(struct xrt_prober *xp,
                 struct xrt_prober_device *const *xpdevs,
                 size_t xpdev_count,
                 const struct u_builder_search_filter *filters,
                 size_t filter_count,
                 struct u_builder_search_results *results)
{
	for (size_t i = 0; i < xpdev_count; i++) {
		struct xrt_prober_device *xpdev = xpdevs[i];
		bool match = false;

		for (size_t k = 0; k < filter_count; k++) {
			struct u_builder_search_filter f = filters[k];

			if (xpdev->product_id != f.product_id || //
			    xpdev->vendor_id != f.vendor_id ||   //
			    xpdev->bus != f.bus_type) {          //
				continue;
			}

			match = true;
			break;
		}

		if (!match) {
			continue;
		}

		results->xpdevs[results->xpdev_count++] = xpdev;

		// Exit if full.
		if (results->xpdev_count >= ARRAY_SIZE(results->xpdevs)) {
			return;
		}
	}
}

void
u_builder_setup_tracking_origins(struct xrt_device *head,
                                 struct xrt_device *left,
                                 struct xrt_device *right,
                                 struct xrt_device *gamepad,
                                 struct xrt_vec3 *global_tracking_origin_offset)
{
	struct xrt_tracking_origin *head_origin = head ? head->tracking_origin : NULL;
	struct xrt_tracking_origin *left_origin = left ? left->tracking_origin : NULL;
	struct xrt_tracking_origin *right_origin = right ? right->tracking_origin : NULL;
	struct xrt_tracking_origin *gamepad_origin = gamepad ? gamepad->tracking_origin : NULL;

	if (left_origin != NULL && left_origin->type == XRT_TRACKING_TYPE_NONE) {
		left_origin->initial_offset.position.x = -0.2f;
		left_origin->initial_offset.position.y = 1.3f;
		left_origin->initial_offset.position.z = -0.5f;
	}

	if (right_origin != NULL && right_origin->type == XRT_TRACKING_TYPE_NONE) {
		right_origin->initial_offset.position.x = 0.2f;
		right_origin->initial_offset.position.y = 1.3f;
		right_origin->initial_offset.position.z = -0.5f;
	}

	if (gamepad_origin != NULL && gamepad_origin->type == XRT_TRACKING_TYPE_NONE) {
		gamepad_origin->initial_offset.position.x = 0.0f;
		gamepad_origin->initial_offset.position.y = 1.3f;
		gamepad_origin->initial_offset.position.z = -0.5f;
	}

	// Head comes last, because left and right may share tracking origin.
	if (head_origin != NULL && head_origin->type == XRT_TRACKING_TYPE_NONE) {
		// "nominal height" 1.6m
		head_origin->initial_offset.position.x = 0.0f;
		head_origin->initial_offset.position.y = 1.6f;
		head_origin->initial_offset.position.z = 0.0f;
	}

	if (head_origin) {
		apply_offset(&head_origin->initial_offset.position, global_tracking_origin_offset);
	}
	if (left_origin && left_origin != head_origin) {
		apply_offset(&left->tracking_origin->initial_offset.position, global_tracking_origin_offset);
	}
	if (right_origin && right_origin != head_origin && right_origin != left_origin) {
		apply_offset(&right->tracking_origin->initial_offset.position, global_tracking_origin_offset);
	}
	if (gamepad_origin && gamepad_origin != head_origin && gamepad_origin != right_origin &&
	    gamepad_origin != left_origin) {
		apply_offset(&gamepad->tracking_origin->initial_offset.position, global_tracking_origin_offset);
	}
}

void
u_builder_create_space_overseer_legacy(struct xrt_session_event_sink *broadcast,
                                       struct xrt_device *head,
                                       struct xrt_device *left,
                                       struct xrt_device *right,
                                       struct xrt_device *gamepad,
                                       struct xrt_device **xdevs,
                                       uint32_t xdev_count,
                                       bool root_is_unbounded,
                                       bool per_app_local_spaces,
                                       struct xrt_space_overseer **out_xso)
{
	/*
	 * Tracking origins.
	 */

	struct xrt_vec3 global_tracking_origin_offset = {
	    debug_get_float_option_tracking_origin_offset_x(),
	    debug_get_float_option_tracking_origin_offset_y(),
	    debug_get_float_option_tracking_origin_offset_z(),
	};

	u_builder_setup_tracking_origins(    //
	    head,                            //
	    left,                            //
	    right,                           //
	    gamepad,                         //
	    &global_tracking_origin_offset); //

	// #1380: display-plane-relative origins are anchored at the initial
	// rig, which is only knowable once the head's own origin is settled
	// and must be known before the overseer turns each origin offset
	// into a space.
	anchor_rig_local_origins(head, xdevs, xdev_count);


	/*
	 * Space overseer.
	 */

	struct u_space_overseer *uso = u_space_overseer_create(broadcast);

	struct xrt_pose T_stage_local = XRT_POSE_IDENTITY;
	T_stage_local.position.y = 1.6;

	u_space_overseer_legacy_setup( //
	    uso,                       // uso
	    xdevs,                     // xdevs
	    xdev_count,                // xdev_count
	    head,                      // head
	    &T_stage_local,            // local_offset
	    root_is_unbounded,         // root_is_unbounded
	    per_app_local_spaces       // per_app_local_spaces
	);

	*out_xso = (struct xrt_space_overseer *)uso;
}

xrt_result_t
u_builder_roles_helper_open_system(struct xrt_builder *xb,
                                   cJSON *config,
                                   struct xrt_prober *xp,
                                   struct xrt_session_event_sink *broadcast,
                                   struct xrt_system_devices **out_xsysd,
                                   struct xrt_space_overseer **out_xso,
                                   u_builder_open_system_fn fn)
{
	struct u_builder_roles_helper ubrh = XRT_STRUCT_INIT;
	xrt_result_t xret;

	// Use the static system devices helper, no dynamic roles.
	struct u_system_devices_static *usysds = u_system_devices_static_allocate();
	struct xrt_tracking_origin *origin = &usysds->base.origin;
	struct xrt_system_devices *xsysd = &usysds->base.base;
	struct xrt_frame_context *xfctx = &usysds->base.xfctx;

	xret = fn(  //
	    xb,     // xb
	    config, // config
	    xp,     // xp
	    origin, // origin
	    xsysd,  // xsysd
	    xfctx,  // xfctx
	    &ubrh); // ubrh
	if (xret != XRT_SUCCESS) {
		xrt_system_devices_destroy(&xsysd);
		return xret;
	}

	/*
	 * Assign to role(s).
	 */

	xsysd->static_roles.head = ubrh.head;
#define U_SET_HT_ROLE(SRC)                                                                                             \
	xsysd->static_roles.hand_tracking.SRC.left = ubrh.hand_tracking.SRC.left;                                      \
	xsysd->static_roles.hand_tracking.SRC.right = ubrh.hand_tracking.SRC.right;
	U_SET_HT_ROLE(unobstructed)
	U_SET_HT_ROLE(conforming)
#undef U_SET_HT_ROLE

	u_system_devices_static_finalize( //
	    usysds,                       // usysds
	    ubrh.left,                    // left
	    ubrh.right,                   // right
	    ubrh.gamepad);                // gamepad


	/*
	 * Create the space overseer.
	 */

	*out_xsysd = xsysd;
	u_builder_create_space_overseer_legacy( //
	    broadcast,                          // broadcast
	    ubrh.head,                          // head
	    ubrh.left,                          // left
	    ubrh.right,                         // right
	    ubrh.gamepad,                       // gamepad
	    xsysd->xdevs,                       // xdevs
	    xsysd->xdev_count,                  // xdev_count
	    false,                              // root_is_unbounded
	    true,                               // per_app_local_spaces
	    out_xso);                           // out_xso

	/*
	 * Rig composition. Devices the builder flagged as bolted to the rig
	 * follow the head *device* pose — the voluntary fly camera — and not
	 * eye-tracked parallax, which is applied later at view-pose level.
	 */
	if (ubrh.rig_relative_count > 0 && ubrh.head != NULL) {
		struct u_space_overseer *uso = (struct u_space_overseer *)*out_xso;

		u_space_overseer_set_rig_source(uso, ubrh.head, XRT_INPUT_GENERIC_HEAD_POSE);
		for (uint32_t i = 0; i < ubrh.rig_relative_count; i++) {
			u_space_overseer_set_device_rig_relative(uso, ubrh.rig_relative[i]);
		}
	}

	return XRT_SUCCESS;
}

xrt_result_t
u_builder_open_system_static_roles(struct xrt_builder *xb,
                                   cJSON *config,
                                   struct xrt_prober *xp,
                                   struct xrt_session_event_sink *broadcast,
                                   struct xrt_system_devices **out_xsysd,
                                   struct xrt_space_overseer **out_xso)
{
	struct u_builder *ub = (struct u_builder *)xb;

	return u_builder_roles_helper_open_system( //
	    xb,                                    //
	    config,                                //
	    xp,                                    //
	    broadcast,                             //
	    out_xsysd,                             //
	    out_xso,                               //
	    ub->open_system_static_roles);         //
}
