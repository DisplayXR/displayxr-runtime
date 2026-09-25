// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display-processor 2D→3D conversion ("lift") contract types (ADR-042).
 *
 * A vendor plug-in may ship a conversion module (monocular depth, stereo or
 * N-view synthesis, photo → Gaussian splats). The runtime reaches it through
 * optional appended slots on the per-API DP vtable — today only
 * @ref xrt_display_processor_d3d11 (lift_get_caps … lift_convert_blob, guarded
 * by XRT_DP_D3D11_HAS_LIFT) — and exposes it to apps as XR_DXR_lift.
 *
 * The split of labour (ADR-042, ADR-007):
 *  - the PLUG-IN converts, synchronously, one frame per call. It never weaves
 *    the result, never queues, never timestamps.
 *  - the RUNTIME owns everything around the call: the lift thread, the
 *    latest-wins mailbox, the output ring (a slot's output is valid only until
 *    that slot's next call), fences, timestamps, IPC, and weaving the SBS /
 *    N-view result on the ordinary weave path.
 *
 * Every struct starts with @c struct_size, set by the CALLER (the runtime for
 * caps / params / stream info), so either side may grow its struct by
 * appending fields without an ABI bump (ADR-020).
 *
 * @ingroup xrt_iface
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @name Lift mode bits (xrt_dp_lift_caps::modes, xrt_dp_lift_stream_info::mode)
 * Values match XR_LIFT_MODE_*_BIT_DXR.
 * @{
 */
#define XRT_DP_LIFT_MODE_DEPTH 1u     //!< monocular depth map
#define XRT_DP_LIFT_MODE_SBS 2u       //!< stereo, side by side, NOT woven
#define XRT_DP_LIFT_MODE_NVIEW 4u     //!< N views side by side in one row, NOT woven
#define XRT_DP_LIFT_MODE_GAUSSIANS 8u //!< photo → Gaussian splats, via lift_convert_blob
/*! @} */

/*!
 * @name Lift module state (xrt_dp_lift_caps::state)
 * Values are the DP-side encoding; XR_LIFT_STATE_*_DXR maps them 1:1 in name
 * (not in value — the OpenXR enum orders UNAVAILABLE=0, ACTIVATING=1, READY=2
 * too, so they do coincide; keep it that way).
 * @{
 */
#define XRT_DP_LIFT_STATE_UNAVAILABLE 0u
#define XRT_DP_LIFT_STATE_ACTIVATING 1u
#define XRT_DP_LIFT_STATE_READY 2u
/*! @} */

//! xrt_dp_lift_caps::depth_semantics
#define XRT_DP_LIFT_DEPTH_RELATIVE 0u
#define XRT_DP_LIFT_DEPTH_METRIC 1u

//! xrt_dp_lift_stream_info::content_hint
#define XRT_DP_LIFT_CONTENT_VIDEO 0u
#define XRT_DP_LIFT_CONTENT_PHOTO 1u

/*!
 * @name Blob formats (lift_convert_blob's out_format)
 * Values match XR_LIFT_BLOB_FORMAT_*_DXR.
 * @{
 */
#define XRT_DP_LIFT_BLOB_PLY_3DGS 1u //!< binary little-endian PLY, reference 3DGS layout
#define XRT_DP_LIFT_BLOB_SOG 2u      //!< PlayCanvas SOG container
/*! @} */

/*!
 * What the plug-in's conversion module can do. The runtime pre-sets
 * @c struct_size (zeroing the rest); the DP writes only fields within it.
 * Cheap: the runtime polls it from its lift thread (≤ 1 Hz while not READY).
 */
struct xrt_dp_lift_caps
{
	uint32_t struct_size;
	uint32_t modes; //!< bits: 1 DEPTH, 2 SBS, 4 NVIEW, 8 GAUSSIANS
	uint32_t max_streams;
	uint32_t max_views;
	uint32_t depth_semantics;    //!< 0 relative, 1 metric
	uint32_t state;              //!< 0 unavailable, 1 activating, 2 ready
	uint64_t typical_latency_ns; //!< submit→result as the module expects it; 0 = unknown
	char backend[32];            //!< NUL-terminated module name (informational)
};

//! Stream creation parameters (runtime-filled).
struct xrt_dp_lift_stream_info
{
	uint32_t struct_size;
	uint32_t mode;         //!< ONE XRT_DP_LIFT_MODE_* bit
	uint32_t content_hint; //!< 0 video, 1 photo
	float input_scale;     //!< (0,1]: convert at reduced resolution; 1 = native
};

//! Upper bound on explicit viewpoints the runtime passes to lift_convert.
#define XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS 8

/*!
 * Per-frame conversion parameters (runtime-filled).
 *
 * @c convergence is the RELATIVE depth placed at the display plane (zero
 * disparity), normalised to [0, 1] over the frame's depth range: 0 = the
 * nearest content sits on the glass (everything else behind it), 1 = the
 * farthest does (everything pops out), 0.5 = the middle of the range. Any
 * negative value = AUTO: the module picks it. The plug-in maps this to its
 * model's own units and calibrates it on its panel; the runtime never
 * interprets it beyond clamping to [0, 1] (or passing a negative through).
 */
struct xrt_dp_lift_params
{
	uint32_t struct_size;
	float convergence;   //!< relative depth at the display plane, [0,1]; < 0 = AUTO
	float strength;      //!< disparity scale; 1 = the module's calibrated budget
	uint32_t inpaint;    //!< non-zero = fill disocclusions
	uint32_t view_count; //!< views to produce (2 for SBS, N for NVIEW; ignored otherwise)
};

/*!
 * Pre-set @p caps for a lift_get_caps call: zero it and stamp struct_size.
 */
static inline void
xrt_dp_lift_caps_init(struct xrt_dp_lift_caps *caps)
{
	uint8_t *p = (uint8_t *)caps;
	for (uint32_t i = 0; i < (uint32_t)sizeof(*caps); i++) {
		p[i] = 0;
	}
	caps->struct_size = (uint32_t)sizeof(*caps);
}

#ifdef __cplusplus
}
#endif
