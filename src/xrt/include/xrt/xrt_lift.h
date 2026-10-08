// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-internal XR_DXR_lift (ADR-042) value types shared by the
 *         state tracker, the IPC bridges and the service compositor.
 *
 * The DP-facing contract is xrt_dp_lift.h; these are the runtime's own
 * plumbing types between xrAcquireLiftResultDXR & co. and the service.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_dp_lift.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Max lift-flagged rects per weave submit (mirrors XR_WEAVE_SUBMIT_MAX_LIFT_RECTS_DXR).
#define XRT_LIFT_WEAVE_RECTS_MAX 8
//! Max explicit viewpoints per submit (mirrors XR_LIFT_MAX_VIEWS_DXR).
#define XRT_LIFT_MAX_VIEWS 8

/*!
 * The viewpoint policy of a stream (XrLiftViewControlDXR, ADR-048). Values
 * are the XR_DXR_lift enum values (axis 1..3, recenter 0 off / 1 ease-back); the service
 * sanitizes them (u_lift_view_control_sanitize).
 */
struct xrt_lift_view_control
{
	float ipd_factor;
	float parallax_factor;
	uint32_t axis_mode;
	float max_offset_m;
	uint32_t recenter_mode;
	float hold_s; //!< < 0 = default
	float tau_s;  //!< <= 0 = default
};

//! xrt_lift_rig::type (ADR-048 Addendum A).
#define XRT_LIFT_RIG_NONE 0u
#define XRT_LIFT_RIG_DISPLAY 1u
#define XRT_LIFT_RIG_CAMERA 2u

/*!
 * An app rig chained on a lift (XrDisplayRigDXR / XrCameraRigDXR,
 * XR_DXR_lift v3), boundary-converted like xrLocateViews' rig: verticalFov ->
 * half-tangent, convergenceDiopters -> inverse convergence distance. The
 * service sanitizes it (u_lift_rig_sanitize). Layout mirrors u_lift_rig.
 */
struct xrt_lift_rig
{
	uint32_t type; //!< XRT_LIFT_RIG_*
	float ipd_factor;
	float parallax_factor;
	float perspective_factor;       //!< display rig
	float inv_convergence_distance; //!< camera rig
	float half_tan_vfov;            //!< camera rig
	float m2v;                      //!< camera rig
};

/*!
 * The auxiliary depth of one acquired result (XrLiftDepthResultDXR,
 * XR_DXR_lift v3). Plain values; the texture travels separately.
 */
struct xrt_lift_depth_info
{
	uint32_t valid;   //!< non-zero = this result carries depth
	uint32_t realloc; //!< the depth export texture changed: re-export it
	uint32_t width;
	uint32_t height;
	uint32_t format;   //!< DXGI_FORMAT
	uint32_t units;    //!< XRT_DP_LIFT_DEPTH_RELATIVE / _METRIC
	uint32_t encoding; //!< XRT_DP_LIFT_DEPTH_ENCODING_*
	uint32_t same_inference;
	float value_scale;
	float value_offset;
	float near_depth;
	float far_depth;
	uint32_t intrinsics_valid;
	float focal[2];     //!< depth-map pixels
	float principal[2]; //!< depth-map pixels
	float convergence_depth;
	float viewpoint[3]; //!< rect-relative metres
	uint32_t transform_valid;
	float depth_to_display[16]; //!< column-major
};

/*!
 * The physical region a lift frame came from (ADR-048): its centre in display
 * space (metres, panel centre = 0, +y up) and its size. TRACKED and EXPLICIT
 * viewpoints are rebased to @c center before the module sees them.
 */
struct xrt_lift_rect_frame
{
	float center[3];
	float width_m;
	float height_m;
	bool valid; //!< false = the panel centre, size unknown
};

//! One acquired texture result.
struct xrt_lift_result
{
	uint64_t frame_id;
	int64_t source_time;
	uint64_t fence_value;
	uint64_t latency_ns;
	uint32_t width;
	uint32_t height;
	uint32_t format; //!< DXGI_FORMAT
	uint32_t view_count;
	bool output_realloc; //!< export texture changed: re-export it to the caller
	//! ADR-048 echo: the viewpoints this result was synthesized for, relative
	//! to @c rect_center (display axes, metres). 0 = none known.
	uint32_t viewpoint_count;
	float viewpoints[3 * XRT_LIFT_MAX_VIEWS];
	float rect_center[3]; //!< display space, metres
	float rect_size[2];   //!< metres
	//! XR_DXR_lift v3: the auxiliary depth of this result.
	struct xrt_lift_depth_info depth;
};

//! One acquired blob result.
struct xrt_lift_blob_info
{
	uint64_t frame_id;
	int64_t source_time;
	uint32_t format; //!< XRT_DP_LIFT_BLOB_*
	uint64_t byte_count;
};

//! A lift-flagged rect of the NEXT weave submit (XrWeaveRectLiftDXR).
struct xrt_lift_weave_rect
{
	uint64_t stream_id;
	uint32_t rect_index;
	bool has_params;
	struct xrt_dp_lift_params params;
	bool has_view_control; //!< XrLiftViewControlDXR chained (ADR-048)
	struct xrt_lift_view_control view_control;
	//! XR_DXR_lift v3: an app rig rode on this rect (type NONE = none). Like
	//! the explicit viewpoints it rides with the options: applied whenever
	//! has_params or a rig is present.
	struct xrt_lift_rig rig;
	//! XR_DXR_lift v3: EXPLICIT viewpoints on a lifted rect, display space
	//! metres (3 floats each); 0 = tracked.
	uint32_t viewpoint_floats;
	float viewpoints[3 * XRT_LIFT_MAX_VIEWS];
};

//! One stream's counters + effective rate (XrLiftStreamStatsDXR).
struct xrt_lift_stream_stats
{
	uint32_t priority; //!< XrLiftPriorityDXR value (0 paused .. 3 high)
	uint32_t reserved;
	uint64_t submitted;
	uint64_t converted;
	uint64_t dropped;
	uint64_t failed;
	uint64_t latency_last_ns;
	uint64_t latency_avg_ns;
	uint64_t latency_min_ns;
	uint64_t latency_max_ns;
	float rate_hz;
	uint32_t reserved2;
};

#ifdef __cplusplus
}
#endif
