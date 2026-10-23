// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tracking-override settings shared by the prober, the config file and
 *         the debug GUI.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"

// XRT_DEVICE_NAME_LEN
#include "xrt/xrt_device.h"

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * @addtogroup xrt_iface
 * @{
 */

#define XRT_MAX_TRACKING_OVERRIDES 16

/*!
 * Tracking override type.
 */
enum xrt_tracking_override_type
{
	XRT_TRACKING_OVERRIDE_DIRECT = 0,
	XRT_TRACKING_OVERRIDE_ATTACHED,
};

struct xrt_tracking_override
{
	char target_device_serial[XRT_DEVICE_NAME_LEN];
	char tracker_device_serial[XRT_DEVICE_NAME_LEN];
	enum xrt_input_name input_name;
	struct xrt_pose offset;
	enum xrt_tracking_override_type override_type;
};

/*!
 * @}
 */


#ifdef __cplusplus
}
#endif
