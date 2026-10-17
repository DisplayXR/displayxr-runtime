// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The touch provider's shared state — private to drivers/touch_input.
 *
 * One recogniser per process, fed from the host's pointer queue, read by both
 * devices under its mutex. Defined once here so the three units that touch it
 * agree on the layout.
 *
 * @ingroup drv_touch_input
 */

#pragma once

#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "os/os_threading.h"

#include "touch_gesture.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct touch_input_provider
{
	struct os_mutex mutex;
	struct touch_gesture gesture;

	//! The host's queue poll (NULL on a runtime older than the slot: devices stay at rest).
	uint32_t (*poll)(struct xrt_input_pointer_event *out, uint32_t max);
	//! The host's geometry read, for the aim plane + the rest pivot distance.
	xrt_result_t (*get_geometry)(struct xrt_input_host_display_geometry *inout);

	bool geometry_applied;
	int64_t last_pump_ns;
	uint64_t events_total;
};

#ifdef __cplusplus
}
#endif
