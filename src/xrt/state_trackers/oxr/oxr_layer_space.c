// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1594 — composition-layer pose into the compositor's frame.
 * @ingroup oxr_main
 */

#include "math/m_api.h"
#include "math/m_space.h"

#include "oxr_layer_space.h"


bool
oxr_layer_pose_in_xdev_frame(const struct xrt_space_relation *T_space_xdev,
                             const struct xrt_pose *T_space_layer,
                             struct xrt_pose *out_pose)
{
	if (T_space_xdev->relation_flags == 0) {
		return false;
	}

	struct xrt_space_relation T_xdev_layer;
	struct xrt_relation_chain xrc = {0};
	m_relation_chain_push_pose_if_not_identity(&xrc, T_space_layer);
	m_relation_chain_push_inverted_relation(&xrc, T_space_xdev); // T_xdev_space
	m_relation_chain_resolve(&xrc, &T_xdev_layer);

	*out_pose = T_xdev_layer.pose;

	return true;
}
