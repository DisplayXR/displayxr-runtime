// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared helper for adding qwerty keyboard/mouse input devices.
 * @author David Fattal
 * @ingroup drv_qwerty
 */

#include "xrt/xrt_config_drivers.h"

#include "target_builder_qwerty_input.h"

#include "util/u_system_helpers.h"
#include "util/u_builders.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty/qwerty_interface.h"
#endif


xrt_result_t
t_builder_add_qwerty_input(struct xrt_system_devices *xsysd,
                           struct u_builder_roles_helper *ubrh,
                           enum u_logging_level log_level,
                           struct xrt_device **out_qwerty_hmd)
{
#ifdef XRT_BUILD_DRIVER_QWERTY
	struct xrt_device *qwerty_head = NULL;
	struct xrt_device *left = NULL;
	struct xrt_device *right = NULL;

	xrt_result_t xret = qwerty_create_devices(log_level, &qwerty_head, &left, &right);
	if (xret != XRT_SUCCESS) {
		return XRT_SUCCESS; // Non-fatal: display builder continues without input devices.
	}

	if (qwerty_head != NULL) {
		xsysd->xdevs[xsysd->xdev_count++] = qwerty_head;
	}
	if (left != NULL) {
		xsysd->xdevs[xsysd->xdev_count++] = left;
		ubrh->left = left;
	}
	if (right != NULL) {
		xsysd->xdevs[xsysd->xdev_count++] = right;
		ubrh->right = right;
	}

	if (out_qwerty_hmd != NULL) {
		*out_qwerty_hmd = qwerty_head;
	}
#else
	(void)xsysd;
	(void)ubrh;
	(void)log_level;
	if (out_qwerty_hmd != NULL) {
		*out_qwerty_hmd = NULL;
	}
#endif

	return XRT_SUCCESS;
}
