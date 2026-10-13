// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `dp-lift` row's static describe (discovery spec §4.5).
 * @ingroup aux_conversion
 */

#include "conv_dp_lift_describe.h"

#include <string.h>

static void
copy_str(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

bool
conv_dp_lift_describe_static(uint32_t index, struct xrt_conv_capability *c)
{
	static const uint32_t k_tasks[CONV_DP_LIFT_CAP_COUNT] = {
	    XRT_CONV_TASK_DEPTH_MONO,
	    XRT_CONV_TASK_DEPTH_VIDEO,
	    XRT_CONV_TASK_VIEWS_MONO,
	};
	if (c == NULL || index >= CONV_DP_LIFT_CAP_COUNT) {
		return false;
	}
	const uint32_t struct_size = c->struct_size; // the caller's
	memset(c, 0, sizeof(*c));
	c->struct_size = struct_size;
	c->cap_id = index + 1; // 1 DEPTH_MONO, 2 DEPTH_VIDEO, 3 VIEWS_MONO
	c->task = k_tasks[index];
	c->lanes = XRT_CONV_LANE_STREAM;
	c->input_kinds = XRT_CONV_INPUT_MONO;
	c->import_memory_kinds = XRT_CONV_MEM_CPU | XRT_CONV_MEM_WIN32_NT_HANDLE;
	c->import_sync_kinds = (1u << XRT_CONV_SYNC_NONE) | (1u << XRT_CONV_SYNC_KEYED_MUTEX);
	c->input_formats = 1u << XRT_CONV_FMT_RGBA8_UNORM;
	c->depth_output_format = XRT_CONV_FMT_R32_FLOAT;
	c->max_concurrent = 1;
	c->max_streams = CONV_DP_LIFT_DEFAULT_MAX_STREAMS;
	c->depth_measure = XRT_CONV_MEASURE_PLANAR;
	c->intrinsics_source = XRT_CONV_INTRINSICS_ASSUMED;
	c->est_warmup_ms = CONV_DP_LIFT_EST_WARMUP_MS;
	// Grandfathered (spec §4.5): UNKNOWN, never COMMERCIAL; exempt from the gate.
	c->licence_class = XRT_CONV_LICENCE_UNKNOWN;
	c->commercial_ok = 0;
	copy_str(c->backend, sizeof(c->backend), "dp-lift");
	copy_str(c->display_name, sizeof(c->display_name), CONV_DP_LIFT_DISPLAY_NAME);

	if (c->task == XRT_CONV_TASK_VIEWS_MONO) {
		c->min_views = 2;
		c->max_views = XRT_CONV_MAX_VIEWS; // refined from lift_get_caps by the live adapter
		// The runtime always passes the viewpoints it resolved, and a module
		// must give them precedence (the lift DP has no tracker session).
		// Required for every VIEWS_* row by validation.
		c->flags = XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;
		// Aux depth is provisional until the module is READY (#2025): a
		// module answers caps while still ACTIVATING, and only a READY one
		// says whether it really has AUX_DEPTH.
		c->outputs = XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH;
		c->depth_kind = XRT_CONV_DEPTH_AFFINE_INVERSE;
	} else {
		c->outputs = XRT_CONV_OUT_DEPTH;
		c->flags = c->task == XRT_CONV_TASK_DEPTH_VIDEO ? XRT_CONV_CAP_TEMPORAL : 0u;
		c->depth_kind = XRT_CONV_DEPTH_AFFINE;
	}
	return true;
}
