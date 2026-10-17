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
 * DisplayXR/displayxr-common#84 ("expose rig point mapping so the runtime's
 * hand-joint path can share the view math"). Until then this file is the one
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
m_view_rig_canvas_rebase(const struct m_view_rig_canvas *canvas,
                         const struct xrt_vec3 *p_display,
                         struct xrt_vec3 *out);

/*!
 * Which rig a mapping applies; NONE = identity (no descriptor chained).
 */
enum m_view_rig_kind
{
	M_VIEW_RIG_NONE = 0,
	M_VIEW_RIG_DISPLAY = 1,
	M_VIEW_RIG_CAMERA = 2,
};

/*!
 * A rig descriptor, post-clamp, in the shape the view math consumes (the
 * XrDisplayRigDXR / XrCameraRigDXR values after the runtime's boundary
 * conversions). @ref pose is in the OUTPUT space — the space the mapped point
 * is wanted in (for joints: XrHandJointsLocateInfoEXT::baseSpace).
 */
struct m_view_rig
{
	enum m_view_rig_kind kind;
	struct xrt_pose pose;
	// Display rig.
	float virtual_display_height; //!< app units; m2v = this / canvas height
	float perspective_factor;
	// Camera rig.
	float m2v;                      //!< meters -> world units (<= 0 treated as 1)
	float inv_convergence_distance; //!< frustum only: does not move a point
	float half_tan_vfov;            //!< frustum only: does not move a point
};

/*!
 * Canvas frame -> rig frame for ONE point: the shared core's eye mapping for an
 * eye at @p p_canvas with the eye-pair factors (ipd, parallax) neutral —
 * `dxr_xrt_display3d_compute_views` / `dxr_xrt_camera3d_compute_views` with a
 * count of one. So a point maps exactly as an eye there would.
 * M_VIEW_RIG_NONE copies the point.
 */
void
m_view_rig_map_canvas_point(const struct m_view_rig_canvas *canvas,
                            const struct m_view_rig *rig,
                            const struct xrt_vec3 *p_canvas,
                            struct xrt_vec3 *out);

/*!
 * The uniform length scale the rig applies to a point (radii, linear
 * velocities): display rig `perspectiveFactor x virtualDisplayHeight / canvas
 * height`, camera rig `metersToVirtual` (<= 0 = 1). 1 for NONE.
 */
float
m_view_rig_length_scale(const struct m_view_rig_canvas *canvas, const struct m_view_rig *rig);

/*!
 * Map one tracked relation (a hand joint) from @p base into the rig frame:
 * base -> physical display plane (`inverse(plane_base)`) -> canvas
 * (@ref m_view_rig_canvas_rebase) -> rig (@ref m_view_rig_map_canvas_point).
 * Orientation and angular velocity are rotated by
 * `rig.pose.orientation * inverse(canvas.orientation) * inverse(plane.orientation)`;
 * linear velocity is rotated the same way and scaled by
 * @ref m_view_rig_length_scale, as is @p radius. Flags are kept. For
 * M_VIEW_RIG_NONE (or an invalid canvas) the relation and radius are copied
 * unchanged.
 */
void
m_view_rig_map_relation(const struct m_view_rig_canvas *canvas,
                        const struct m_view_rig *rig,
                        const struct xrt_pose *plane_base,
                        const struct xrt_space_relation *in_base,
                        float radius,
                        struct xrt_space_relation *out,
                        float *out_radius);

#ifdef __cplusplus
}
#endif
