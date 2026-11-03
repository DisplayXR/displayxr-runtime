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
	/*!
	 * Height of the PHYSICAL display the canvas sits on (metres, in the
	 * canvas's orientation — after the same portrait swap). The RIG_LOCAL
	 * scale is `height_m / display_height_m` (@ref m_view_rig_canvas_scale);
	 * 0 = unknown = scale 1.
	 */
	float display_height_m;
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
	float m2v;                      //!< meters -> world units (<= 0 treated as 1); eyes only
	float inv_convergence_distance; //!< 1 / convergence distance (world units): where the glass lands
	float half_tan_vfov;            //!< tan(vFOV/2): sizes the virtual screen at the glass
};

/*!
 * Where a camera rig puts the physical glass (XR_DXR_view_rig spec 5): the
 * camera rig's own virtual screen, from the shared core's camera -> display conversion
 * (`dxr_view_rig_camera_to_display`, the one rig toggles use) — the plane at
 * `pose` moved `1 / inv_convergence_distance` along the camera's forward axis
 * (zero parallax), height `V = 2 half_tan_vfov / inv_convergence_distance`.
 * @p out_scale = V / canvas height: the metres -> rig-units factor a point at
 * the glass gets, so a hand on the glass lands on the pixel the viewer sees
 * it over. False when @p rig is not a camera rig, the convergence is at
 * infinity (inv_convergence_distance <= 0) or the canvas is degenerate; the
 * camera rig then keeps the viewer-relative mapping (x metersToVirtual about
 * the nominal viewer plane).
 */
bool
m_view_rig_camera_glass(const struct m_view_rig_canvas *canvas,
                        const struct m_view_rig *rig,
                        struct xrt_pose *out_glass_pose,
                        float *out_scale);

/*!
 * Canvas frame -> rig frame for ONE point.
 *
 * Display rig: the shared core's eye mapping for an eye at @p p_canvas with the
 * eye-pair factors (ipd, parallax) neutral — `dxr_xrt_display3d_compute_views`
 * with a count of one, so a point maps exactly as an eye there would.
 *
 * Camera rig (spec 5): the same display-core mapping for the camera rig's
 * glass (@ref m_view_rig_camera_glass) with perspectiveFactor 1 — the glass lands
 * on the convergence plane, scaled by virtual screen height / canvas height,
 * exactly as a display rig's glass lands on its plane. (Before spec 5 a camera
 * rig mapped a point as an eye — x metersToVirtual about the nominal viewer —
 * which put the glass a nominal viewer distance in front of the camera at
 * metric scale, not at zero parallax; that remains the fallback when the
 * convergence is at infinity.)
 *
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
 * height`, camera rig its virtual screen height / canvas height
 * (@ref m_view_rig_camera_glass; `metersToVirtual`, <= 0 = 1, when the
 * convergence is at infinity). 1 for NONE.
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

/*
 *
 * RIG_LOCAL is canvas-relative (input-provider-discovery.md §4b.4, ADR-034
 * Amendment 7): a rig-local pose L (metres relative to the PHYSICAL display
 * plane) is read as fractions of the physical display height and applied to
 * the app's canvas — origin at the canvas centre, positions, radii and linear
 * velocities scaled by s = canvas height / physical display height,
 * orientations untouched. A full-display canvas has s = 1 and its centre at
 * the display centre, so it is unchanged.
 *
 */

/*!
 * The RIG_LOCAL scale of @p canvas: `height_m / display_height_m`. 1 when the
 * canvas is invalid, either height is unknown, or the ratio is within 1e-4 of
 * 1 (a full-display canvas whose two heights come from different sources must
 * not move by float noise).
 */
float
m_view_rig_canvas_scale(const struct m_view_rig_canvas *canvas);

/*!
 * The canvas as a pose in display space: @ref m_view_rig_canvas::center and,
 * when @ref m_view_rig_canvas::has_orientation, its orientation (else identity).
 */
void
m_view_rig_canvas_pose(const struct m_view_rig_canvas *canvas, struct xrt_pose *out);

/*!
 * Where the canvas centre lands: the pose (in the frame @p plane_origin is
 * given in) that a rig-local frame is placed on for a session that owns its
 * canvas. The canvas pose on the physical plane @p plane_origin
 * (`plane_origin o canvas_pose`), mapped through @p rig the way a joint is
 * (@ref m_view_rig_map_relation: the canvas point 0 through the shared core,
 * orientation `rig.pose.orientation`). @p rig NULL / NONE = no rig.
 */
void
m_view_rig_canvas_plane(const struct m_view_rig_canvas *canvas,
                        const struct m_view_rig *rig,
                        const struct xrt_pose *plane_origin,
                        struct xrt_pose *out);

/*!
 * Scale one relation uniformly about @p origin (same frame as the relation):
 * `p' = origin + s (p - origin)`, linear velocity and @p radius times @p s;
 * orientation, angular velocity and flags kept. This is the step that turns a
 * rigid RIG_LOCAL placement `plane o L` into `plane o (s L)`. @p s == 1 copies
 * the relation and radius bit for bit.
 */
void
m_view_rig_scale_relation(const struct xrt_vec3 *origin,
                          float s,
                          const struct xrt_space_relation *in,
                          float radius,
                          struct xrt_space_relation *out,
                          float *out_radius);

#ifdef __cplusplus
}
#endif
