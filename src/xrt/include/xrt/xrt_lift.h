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
};

#ifdef __cplusplus
}
#endif
