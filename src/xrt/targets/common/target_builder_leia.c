// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia 3D display builder.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "xrt/xrt_config_drivers.h"

#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_builders.h"
#include "util/u_logging.h"
#include "util/u_system_helpers.h"

#include "target_builder_interface.h"
#include "target_builder_qwerty_input.h"

#include "leia/leia_interface.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty/qwerty_device.h"
#endif

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
	bool hw_found = leiasr_probe_display(3.0);
	if (hw_found) {
		estimate->certain.head = true;
		U_LOG_I("SR hardware detected — Leia builder claims head device");
	} else {
		U_LOG_I("No SR hardware detected — Leia builder will not claim head device");
	}

	estimate->priority = -15;

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

	// Add qwerty keyboard/mouse input devices (controllers + HMD for pose).
	struct xrt_device *qwerty_hmd = NULL;
	t_builder_add_qwerty_input(xsysd, ubrh, U_LOGGING_INFO, &qwerty_hmd);

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (qwerty_hmd != NULL) {
		struct qwerty_device *qd = qwerty_device(qwerty_hmd);

		// Configure 3D params from Leia display info.
		struct leiasr_probe_result probe;
		if (leiasr_get_probe_results(&probe) && probe.hw_found) {
			qd->sys->screen_height_m = probe.display_h_m;
			qd->sys->nominal_viewer_z = (probe.nominal_z_m > 0.0f) ? probe.nominal_z_m : 0.65f;
		}

		// Camera mode default position (eye height, looking forward).
		qd->pose.position = (struct xrt_vec3){0, 1.6f, 0};
		qd->pose.orientation = (struct xrt_quat){0, 0, 0, 1};

		// Delegate head pose to qwerty HMD for WASD/mouse camera control.
		leia_hmd_set_pose_source(head, qwerty_hmd);
	}
#endif

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
