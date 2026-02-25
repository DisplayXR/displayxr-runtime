// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia 3D display builder.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_instance.h"

#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_builders.h"
#include "util/u_system_helpers.h"

#include "target_builder_interface.h"

#include "leia/leia_interface.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>


/*
 *
 * Helper functions.
 *
 */

static const char *driver_list[] = {
    "leia",
};


/*
 *
 * Member functions.
 *
 */

static xrt_result_t
leia_estimate_system(struct xrt_builder *xb,
                     cJSON *config,
                     struct xrt_prober *xp,
                     struct xrt_builder_estimate *estimate)
{
	// Check if the app uses XR_EXT_win32_window_binding (extension app).
	bool is_ext_app = false;
	if (xp->instance_info != NULL) {
		is_ext_app = xp->instance_info->app_info.ext_win32_window_binding_enabled;
	}

	estimate->certain.head = true;

	if (is_ext_app) {
		// Extension apps: Leia is preferred head device.
		estimate->priority = 0;
	} else {
		// Non-extension apps: lower priority than qwerty (keyboard/mouse).
		estimate->priority = -15;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
leia_open_system_impl(struct xrt_builder *xb,
                      cJSON *config,
                      struct xrt_prober *xp,
                      struct xrt_tracking_origin *origin,
                      struct xrt_system_devices *xsysd,
                      struct xrt_frame_context *xfctx,
                      struct u_builder_roles_helper *ubrh)
{
	struct xrt_device *head = leia_hmd_create();
	if (head == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Add to device list.
	xsysd->xdevs[xsysd->xdev_count++] = head;

	// Assign to role(s).
	ubrh->head = head;

	return XRT_SUCCESS;
}

static void
leia_destroy(struct xrt_builder *xb)
{
	free(xb);
}


/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_builder *
t_builder_leia_create(void)
{
	struct u_builder *ub = U_TYPED_CALLOC(struct u_builder);

	// xrt_builder fields.
	ub->base.estimate_system = leia_estimate_system;
	ub->base.open_system = u_builder_open_system_static_roles;
	ub->base.destroy = leia_destroy;
	ub->base.identifier = "leia";
	ub->base.name = "Leia 3D Display";
	ub->base.driver_identifiers = driver_list;
	ub->base.driver_identifier_count = ARRAY_SIZE(driver_list);
	ub->base.exclude_from_automatic_discovery = false;

	// u_builder fields.
	ub->open_system_static_roles = leia_open_system_impl;

	return &ub->base;
}
