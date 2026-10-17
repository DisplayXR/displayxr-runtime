// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The view rig's canvas frame and point mapping (XR_DXR_view_rig, #2189).
 *
 * The view math runs in the CANVAS frame: a display-space point (physical
 * metres, display-centre origin, +X right +Y up +Z toward the viewer — the
 * frame the display processor reports eyes in) is re-expressed relative to the
 * canvas centre and orientation (@ref m_view_rig_canvas_rebase), then the rig
 * maps it into the app's frame. xrLocateViews does this to eyes; spec 4 of
 * XR_DXR_view_rig does it to hand joints, so joints land in the same frame as
 * the views.
 *
 * ONE implementation: the rebase below is the one the view locate itself
 * calls, and the point mapping calls the shared displayxr-common core's
 * single-eye path — an eye at the point, with the eye-pair-only factors (ipd,
 * parallax) neutral — so a joint and an eye at the same place map
 * identically by construction (pinned bit-for-bit by tests_view_rig_point).
 *
 * TO BE FOLDED into displayxr-common as point functions (a point mapping
 * without the frustum the core computes alongside each eye):
 * DisplayXR/displayxr-common issue "expose rig point mapping so the runtime's
 * hand-joint path can share the view math". Until then this file is the one
 * runtime-side caller.
 *
 * @ingroup aux_math
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The canvas a view locate resolved: its physical size (after the portrait
 * swap) and where it sits on the panel. For a window / zone / segment canvas
 * @ref center and @ref orientation are the window metrics' centre offset and
 * orientation; for a full-display canvas they are zero / identity.
 */
struct m_view_rig_canvas
{
	bool valid;
	float width_m;
	float height_m;
	//! Canvas centre in display space (metres).
	struct xrt_vec3 center;
	//! Canvas orientation in display space; identity = flat on the panel.
	struct xrt_quat orientation;
	//! @ref orientation is non-identity (the rebase rotates only then).
	bool has_orientation;
	//! The nominal viewer (display space) — the camera rig's z reference.
	float nominal_y_m;
	float nominal_z_m;
};

/*!
 * Display space → canvas frame: subtract the canvas centre, then (when the
 * canvas is rotated) apply the inverse canvas orientation. Exactly the step
 * xrLocateViews applies to every DP eye before the rig math.
 */
void
m_view_rig_canvas_rebase(const struct m_view_rig_canvas *canvas, const struct xrt_vec3 *p_display, struct xrt_vec3 *out);

#ifdef __cplusplus
}
#endif
