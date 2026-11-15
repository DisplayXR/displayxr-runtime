// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Resolve a provider device's DOMINANT_HAND_CONTROLLER type (ADR-034
 *         Amendment 8). Pure: no I/O, so the rule is unit-tested on its own.
 * @ingroup targets_common
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_input_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The device type a provider device holds once the builder has resolved it:
 * `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER` becomes RIGHT or LEFT by
 * @p handedness (anything but LEFT is right — the policy's default); every
 * other type is returned unchanged. Never ANY: a single pen must not inherit
 * ANY's "left first".
 */
static inline enum xrt_device_type
t_input_resolve_dominant_hand(enum xrt_device_type type, enum xrt_input_handedness handedness)
{
	if (type != XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER) {
		return type;
	}
	return handedness == XRT_INPUT_HANDEDNESS_LEFT ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER
	                                               : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
}

#ifdef __cplusplus
}
#endif
