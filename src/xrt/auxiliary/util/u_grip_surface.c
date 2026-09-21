// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Derive the OpenXR `grip_surface` pose from a controller's `grip` pose.
 * @see    u_grip_surface.h for the convention and where the numbers come from.
 * @ingroup aux_util
 */

#include "util/u_grip_surface.h"

#include "math/m_api.h"

#include <stdint.h>

void
u_grip_surface_offset(bool is_left, struct xrt_pose *out_pose)
{
	if (out_pose == NULL) {
		return;
	}

	*out_pose = (struct xrt_pose)XRT_POSE_IDENTITY;

	// Towards the palm along the grip's X axis: grip +X points away from
	// the palm on the left hand and into it on the right.
	out_pose->position.x = is_left ? -U_GRIP_SURFACE_OFFSET_M : U_GRIP_SURFACE_OFFSET_M;
}

void
u_grip_surface_from_grip(bool is_left, const struct xrt_space_relation *grip, struct xrt_space_relation *out_relation)
{
	if (out_relation == NULL) {
		return;
	}

	if (grip == NULL) {
		*out_relation = (struct xrt_space_relation)XRT_SPACE_RELATION_ZERO;
		return;
	}

	// Copy first so the caller may pass the same relation for both.
	const struct xrt_space_relation in = *grip;

	struct xrt_pose offset;
	u_grip_surface_offset(is_left, &offset);

	*out_relation = in;
	math_pose_transform(&in.pose, &offset, &out_relation->pose);

	/*
	 * A point rigidly attached at r moves with v + ω × r. Only claim that
	 * when the source claims BOTH velocities; otherwise the inherited
	 * linear velocity stands, exactly as valid as the flags say it is.
	 */
	const uint32_t both = (uint32_t)XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT |
	                      (uint32_t)XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT;
	if (((uint32_t)in.relation_flags & both) == both) {
		struct xrt_vec3 lever = XRT_VEC3_ZERO;
		math_quat_rotate_vec3(&in.pose.orientation, &offset.position, &lever);

		struct xrt_vec3 tangential = XRT_VEC3_ZERO;
		math_vec3_cross(&in.angular_velocity, &lever, &tangential);

		out_relation->linear_velocity.x = in.linear_velocity.x + tangential.x;
		out_relation->linear_velocity.y = in.linear_velocity.y + tangential.y;
		out_relation->linear_velocity.z = in.linear_velocity.z + tangential.z;
	}
}
