// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `dp-lift` row's static describe (discovery spec §4.5): the ONE
 *         source shared by the in-service adapter (conv_provider_legacy_dp,
 *         which refines it from the live module) and the headless status
 *         (target_conv_status.c: selftest, `conv list`, the headless status
 *         snapshot), so the two can never describe the row differently.
 *
 * Pure C: no threads, no GPU, no logging (part of conv_core).
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONV_DP_LIFT_ID "dp-lift"
#define CONV_DP_LIFT_PROBE_ORDER 150u
#define CONV_DP_LIFT_DISPLAY_NAME "Display plug-in lift module"

/*! @name Capability ids (spec §4.5) @{ */
#define CONV_DP_LIFT_CAP_DEPTH_MONO 1u
#define CONV_DP_LIFT_CAP_DEPTH_VIDEO 2u
#define CONV_DP_LIFT_CAP_VIEWS_MONO 3u
#define CONV_DP_LIFT_CAP_COUNT 3u
/*! @} */

//! Module activation is slow (licence check, engine build): the warm-up estimate.
#define CONV_DP_LIFT_EST_WARMUP_MS 60000u
//! max_streams before the module answered lift_get_caps.
#define CONV_DP_LIFT_DEFAULT_MAX_STREAMS 4u

/*!
 * Fill capability @p index (0 .. CONV_DP_LIFT_CAP_COUNT - 1) as it is
 * described before the module is instantiated. @p out is cleared first
 * (its struct_size is kept).
 * Returns false for an index out of range.
 */
bool
conv_dp_lift_describe_static(uint32_t index, struct xrt_conv_capability *out);

#ifdef __cplusplus
}
#endif
