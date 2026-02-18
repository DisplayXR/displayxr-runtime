// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulation 3D display builder.
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "xrt/xrt_config_drivers.h"

#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_builders.h"
#include "util/u_system_helpers.h"

#include "target_builder_interface.h"

#include "sim_display/sim_display_interface.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty/qwerty_interface.h"
#include "qwerty/qwerty_device.h"
#endif

#include <assert.h>
#include <string.h>
#include <stdlib.h>


DEBUG_GET_ONCE_BOOL_OPTION(enable_sim_display, "SIM_DISPLAY_ENABLE", false)


/*
 *
 * Helper functions.
 *
 */

static const char *driver_list[] = {
    "sim_display",
};

/*
 *
 * Member functions.
 *
 */

static xrt_result_t
sim_display_estimate_system(struct xrt_builder *xb,
                            cJSON *config,
                            struct xrt_prober *xp,
                            struct xrt_builder_estimate *estimate)
{
	if (!debug_get_bool_option_enable_sim_display()) {
		return XRT_SUCCESS;
	}

	estimate->certain.head = true;
	estimate->priority = -20; // Lower than qwerty (-25) but higher than real hardware

	return XRT_SUCCESS;
}

static xrt_result_t
sim_display_open_system_impl(struct xrt_builder *xb,
                             cJSON *config,
                             struct xrt_prober *xp,
                             struct xrt_tracking_origin *origin,
                             struct xrt_system_devices *xsysd,
                             struct xrt_frame_context *xfctx,
                             struct u_builder_roles_helper *ubrh)
{
	struct xrt_device *head = sim_display_hmd_create();
	if (head == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Add to device list.
	xsysd->xdevs[xsysd->xdev_count++] = head;

	// Assign to role(s).
	ubrh->head = head;

#ifdef XRT_BUILD_DRIVER_QWERTY
	// Create qwerty devices for keyboard/mouse input.
	// The sim_display HMD stays as head (keeps Kooima FOV + display processor),
	// but delegates its pose to the qwerty HMD for WASD/mouse camera control.
	{
		struct xrt_device *qwerty_head = NULL;
		struct xrt_device *left = NULL;
		struct xrt_device *right = NULL;
		enum u_logging_level log_level = U_LOGGING_INFO;

		xrt_result_t xret = qwerty_create_devices(log_level, &qwerty_head, &left, &right);
		if (xret == XRT_SUCCESS) {
			if (qwerty_head != NULL) {
				xsysd->xdevs[xsysd->xdev_count++] = qwerty_head;

				// Set qwerty HMD initial pose to match sim_display's
				// nominal viewer position so the scene starts correctly.
				struct xrt_space_relation rel;
				head->get_tracked_pose(head, XRT_INPUT_GENERIC_HEAD_POSE, 0, &rel);
				struct qwerty_device *qd = qwerty_device(qwerty_head);
				qd->pose = rel.pose;

				// Delegate sim_display pose to qwerty HMD.
				sim_display_hmd_set_pose_source(head, qwerty_head);
			}
			if (left != NULL) {
				xsysd->xdevs[xsysd->xdev_count++] = left;
				ubrh->left = left;
			}
			if (right != NULL) {
				xsysd->xdevs[xsysd->xdev_count++] = right;
				ubrh->right = right;
			}
		}
	}
#endif

	return XRT_SUCCESS;
}

static void
sim_display_destroy(struct xrt_builder *xb)
{
	free(xb);
}


/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_builder *
t_builder_sim_display_create(void)
{
	struct u_builder *ub = U_TYPED_CALLOC(struct u_builder);

	// xrt_builder fields.
	ub->base.estimate_system = sim_display_estimate_system;
	ub->base.open_system = u_builder_open_system_static_roles;
	ub->base.destroy = sim_display_destroy;
	ub->base.identifier = "sim_display";
	ub->base.name = "Simulated 3D Display";
	ub->base.driver_identifiers = driver_list;
	ub->base.driver_identifier_count = ARRAY_SIZE(driver_list);
	ub->base.exclude_from_automatic_discovery = !debug_get_bool_option_enable_sim_display();

	// u_builder fields.
	ub->open_system_static_roles = sim_display_open_system_impl;

	return &ub->base;
}
