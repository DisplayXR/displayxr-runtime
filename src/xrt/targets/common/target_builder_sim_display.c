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
#include "target_builder_qwerty_input.h"

#include "sim_display/sim_display_interface.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
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
	estimate->priority = -20; // Higher than qwerty fallback (-25), lower than leia (-15)

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

	// Add qwerty keyboard/mouse input devices (controllers + HMD for pose).
	struct xrt_device *qwerty_hmd = NULL;
	t_builder_add_qwerty_input(xsysd, ubrh, U_LOGGING_INFO, &qwerty_hmd);

#ifdef XRT_BUILD_DRIVER_QWERTY
	// sim_display-specific: configure qwerty HMD pose and delegate sim_display
	// pose to qwerty for WASD/mouse camera control.
	if (qwerty_hmd != NULL) {
		struct qwerty_device *qd = qwerty_device(qwerty_hmd);

		struct sim_display_info info;
		if (sim_display_get_display_info(head, &info)) {
			qd->sys->screen_height_m = info.display_height_m;
			qd->sys->nominal_viewer_z = info.nominal_z_m;
		}

		// Share sim_display head's tracking origin with all qwerty devices
		// so they occupy the same node in the space overseer graph.
		qwerty_hmd->tracking_origin = head->tracking_origin;
		if (qd->sys->lctrl)
			qd->sys->lctrl->base.base.tracking_origin = head->tracking_origin;
		if (qd->sys->rctrl)
			qd->sys->rctrl->base.base.tracking_origin = head->tracking_origin;

		sim_display_hmd_set_pose_source(head, qwerty_hmd);
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
