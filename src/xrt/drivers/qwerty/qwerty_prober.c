// Copyright 2021, Mateo de Mayo.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Qwerty devices @ref xrt_auto_prober "autoprober".
 * @author Mateo de Mayo <mateodemayo@gmail.com>
 * @ingroup drv_qwerty
 */

#include "xrt/xrt_prober.h"

#include "util/u_logging.h"

#include "qwerty_device.h"
#include "qwerty_interface.h"


/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
qwerty_create_devices_ex(enum u_logging_level log_level,
                         bool publish_controllers,
                         struct xrt_device **out_hmd,
                         struct xrt_device **out_left,
                         struct xrt_device **out_right)
{
	struct qwerty_hmd *qhmd = qwerty_hmd_create();
	struct qwerty_controller *qleft = qwerty_controller_create(true, qhmd);
	struct qwerty_controller *qright = qwerty_controller_create(false, qhmd);

	struct qwerty_system *qs = qwerty_system_create(qhmd, qleft, qright, log_level);
	if (qs == NULL) {
		struct xrt_device *devices[] = {&qright->base.base, &qleft->base.base, &qhmd->base.base};
		for (size_t i = 0; i < 3; i++) {
			xrt_device_destroy(&devices[i]);
		}
		*out_hmd = *out_left = *out_right = NULL;
		return XRT_ERROR_ALLOCATION;
	}

	*out_hmd = &qhmd->base.base;
	if (publish_controllers) {
		*out_left = &qleft->base.base;
		*out_right = &qright->base.base;
	} else {
		// ADR-054 N7: the caller gets the look-around head only. The
		// controller objects still exist — every input front-end addresses
		// `lctrl` / `rctrl` and the system asserts on them — but they are
		// the driver's private state now: in no device list, never a role
		// candidate, destroyed by the system when the HMD is.
		qs->controllers_private = true;
		*out_left = NULL;
		*out_right = NULL;
	}

	return XRT_SUCCESS;
}

xrt_result_t
qwerty_create_devices(enum u_logging_level log_level,
                      struct xrt_device **out_hmd,
                      struct xrt_device **out_left,
                      struct xrt_device **out_right)
{
	return qwerty_create_devices_ex(log_level, true, out_hmd, out_left, out_right);
}
