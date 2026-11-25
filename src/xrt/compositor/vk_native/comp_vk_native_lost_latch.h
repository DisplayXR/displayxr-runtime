// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Latch that turns persistently failing queue submits into "the
 *         Vulkan device is lost".
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_vulkan_includes.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Consecutive failed submits (no success in between) after which the device
 * is treated as lost. Half a second at 60 Hz: long enough that a transient
 * failure (the Android shared-GSL-context race returns -3 once and then
 * succeeds, #1905) never trips it.
 */
#define COMP_VK_LOST_LATCH_THRESHOLD 30

/*!
 * One per submit site family (the renderer's compose submits, the target's
 * readback submit). Written by the thread that submits; `lost` is read by the
 * app thread in wait_frame without a lock, which is safe because it only ever
 * goes false -> true.
 *
 * Why a count and not just VK_ERROR_DEVICE_LOST: after a Windows GPU engine
 * timeout the Adreno driver returns VK_ERROR_INITIALIZATION_FAILED (-3) from
 * every vkQueueSubmit, never -4, so the compositor used to log an error per
 * frame forever while the window showed a frozen image.
 */
struct comp_vk_lost_latch
{
	uint32_t consecutive_failures;
	bool lost;
};

/*!
 * Record one submit result. Returns true when the caller should log this
 * failure: the first of a run, and the one that trips the latch. Every other
 * failure in the run is silent, so a dead device cannot flood the log.
 */
static inline bool
comp_vk_lost_latch_note(struct comp_vk_lost_latch *l, VkResult res)
{
	if (res == VK_SUCCESS) {
		l->consecutive_failures = 0;
		return false;
	}
	if (l->lost) {
		return false;
	}
	l->consecutive_failures++;
	if (res == VK_ERROR_DEVICE_LOST || l->consecutive_failures >= COMP_VK_LOST_LATCH_THRESHOLD) {
		l->lost = true;
		return true;
	}
	return l->consecutive_failures == 1;
}

#ifdef __cplusplus
}
#endif
