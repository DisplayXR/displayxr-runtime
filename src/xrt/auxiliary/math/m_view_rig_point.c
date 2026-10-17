// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The view rig's canvas frame and point mapping (XR_DXR_view_rig, #2189).
 * @ingroup aux_math
 */

#include "math/m_view_rig_point.h"
#include "math/m_api.h"

void
m_view_rig_canvas_rebase(const struct m_view_rig_canvas *canvas, const struct xrt_vec3 *p_display, struct xrt_vec3 *out)
{
	struct xrt_vec3 delta = {
	    p_display->x - canvas->center.x,
	    p_display->y - canvas->center.y,
	    p_display->z - canvas->center.z,
	};
	if (canvas->has_orientation) {
		struct xrt_quat inv_q;
		math_quat_invert(&canvas->orientation, &inv_q);
		math_quat_rotate_vec3(&inv_q, &delta, out);
	} else {
		*out = delta;
	}
}
