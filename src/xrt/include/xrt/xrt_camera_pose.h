// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Where a camera sits relative to a screen — the one answer every
 *         component class reads through its host iface.
 *
 * A camera's pose relative to a screen is a property of the CAMERA and the
 * SCREEN, not of whichever provider happens to read that camera. The runtime
 * owns it: one calibration (done once, for head tracking) serves every
 * provider that uses the same camera on the same screen, whatever its class —
 * a viewer-tracking provider (`xrt_tracking_plugin.h`) and a hand-tracking
 * INPUT provider (`xrt_input_plugin.h`) get the identical answer. Both host
 * ifaces carry one `get_camera_pose` slot of the type below, carved from
 * their `reserved[]` (ADR-020: size and every other offset unchanged, no API
 * major bump; an older runtime leaves the slot NULL).
 *
 * **The answer is never "not found".** The runtime resolves, highest first:
 *
 *  1. `display-declared` — the display says where its own camera is (the
 *     planned ADR-043 field, runtime issue #2296; not wired yet);
 *  2. `user-calibration` — `displayxr-cli tracking calibrate apply` (the
 *     mirror-alignment solve);
 *  3. `placement-hint` — the user said which bezel edge the camera sits on
 *     (`displayxr-cli camera calibration set-placement`), turned into a pose
 *     from the panel size;
 *  4. `default-prior` — R = I, on the top centre of the bezel:
 *     T = (0, H/2 + 8 mm, 0), H = the panel's physical height.
 *
 * Every answer says which one it is (@ref xrt_camera_pose::source), so a
 * provider and the Dashboard can tell "uncalibrated (assumed top centre)"
 * from "calibrated (rms 2.1 mm)".
 *
 * Store and precedence: `docs/specs/runtime/tracking-provider-discovery.md` §6.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @name Where an answer came from (@ref xrt_camera_pose::source). The numeric
 * order IS the precedence: a higher value wins over a lower one.
 * @{
 */
#define XRT_CAMERA_POSE_SOURCE_NONE 0             //!< nothing (an error return; never in a success)
#define XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR 1    //!< top centre of the bezel, R = I (uncalibrated)
#define XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT 2   //!< the user named the bezel edge (uncalibrated)
#define XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION 3 //!< mirror-alignment calibration
#define XRT_CAMERA_POSE_SOURCE_DISPLAY_DECLARED 4 //!< the display declares its camera (ADR-043, #2296)
/*! @} */

/*!
 * @name Bezel edges of a placement hint (@ref xrt_camera_pose::placement_edge).
 * @{
 */
#define XRT_CAMERA_PLACEMENT_EDGE_TOP 0
#define XRT_CAMERA_PLACEMENT_EDGE_BOTTOM 1
#define XRT_CAMERA_PLACEMENT_EDGE_LEFT 2
#define XRT_CAMERA_PLACEMENT_EDGE_RIGHT 3
/*! @} */

//! The bezel margin of the default prior and of a placement hint: the lens sits this far outside the active area.
#define XRT_CAMERA_POSE_BEZEL_MARGIN_M 0.008

/*!
 * The camera's pose in the display space of a screen (the eye-position
 * convention of `xrt_display_metrics.h`: metres, origin at the centre of the
 * display surface, +X right, +Y up, +Z out of the glass toward the viewer).
 * The camera frame is the one `xrt_tracking_camera_sample` uses (+X right as
 * the user sees it, +Y up, +Z out of the lens toward the viewer), and
 * display point = position + R(orientation) · camera point.
 *
 * `struct_size` first (ADR-020): the CALLER sets it; the runtime writes no
 * further than it, and appends fields only.
 */
struct xrt_camera_pose
{
	uint32_t struct_size; //!< caller sets to sizeof before the call
	uint32_t source;      //!< XRT_CAMERA_POSE_SOURCE_*
	float position_m[3];  //!< the camera's optical centre, display space, metres
	float orientation[4]; //!< x, y, z, w; camera frame -> display frame
	//! The camera's true focal length / the one its directions are computed with (1 = none; mode c solve).
	float focal_scale;
	float rms_mm; //!< user-calibration: the solve's in-plane RMS residual; 0 otherwise
	/*!
	 * The VIEWER's iris scale (true / estimated eye distance; 1 = none): a
	 * property of the user, not of the camera — one per-user value any
	 * face-based provider multiplies its iris-size depth by. Carried here so
	 * a provider needs one call; stored apart from every camera record.
	 */
	float viewer_iris_scale;
	uint32_t sample_count;    //!< user-calibration: samples the solve used; 0 otherwise
	int64_t timestamp_unix_s; //!< when the record was written (UTC seconds); 0 for the default prior
	/*!
	 * Moves whenever this answer changes (a new calibration, a cleared one, a
	 * placement edit, a new iris scale). Compare for equality, never order: a
	 * provider that re-reads it (on `start()`, or now and then) re-applies the
	 * pose without a restart.
	 */
	uint64_t generation;
	char solver_mode[8];       //!< user-calibration: "a" / "b" / "c"; "" otherwise
	uint32_t placement_edge;   //!< placement-hint: XRT_CAMERA_PLACEMENT_EDGE_*
	float placement_offset_mm; //!< placement-hint: along the edge (+X right on top/bottom, +Y up on left/right)
	float placement_margin_mm; //!< placement-hint: extra margin beyond the bezel margin
	uint32_t reserved[8];      //!< 0
	                           /* Append-only below this line (ADR-020). */
};

/*!
 * The host-iface slot type (both the viewer-tracking and the input host
 * ifaces carry one): the pose of the camera @p camera_key relative to the
 * screen @p screen_key, resolved by the precedence above.
 *
 * @param host       the host iface the slot was read from (the caller's own `host` pointer)
 * @param camera_key the camera's stable key (`xrt_tracking_camera_sample::camera_key`)
 * @param screen_key the screen's key (`displayxr-cli displays --claims`); NULL or "" = the
 *                   screen the provider serves (the primary screen in this release), and a
 *                   stored record for this camera on ANY screen matches (the newest wins)
 * @param inout      `struct_size` preset by the caller; filled up to it on success
 *
 * @return XRT_SUCCESS (always with a source: never "not found");
 *         XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY when no record exists and the panel size
 *         the prior needs is not known yet (struct untouched);
 *         XRT_ERROR_INPUT_UNSUPPORTED on a NULL / too-small argument.
 *
 * Thread-safe, non-blocking apart from a small registry / file read: call it at
 * `start()` (or on a slow timer to follow `generation`), never per frame.
 */
typedef xrt_result_t (*xrt_camera_get_pose_fn_t)(const void *host,
                                                 const char *camera_key,
                                                 const char *screen_key,
                                                 struct xrt_camera_pose *inout);

#ifdef __cplusplus
}
#endif
