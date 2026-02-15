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

/*!
 * Parse the SIM_DISPLAY_OUTPUT env var into an output mode.
 */
static enum sim_display_output_mode
get_output_mode(void)
{
	const char *mode = getenv("SIM_DISPLAY_OUTPUT");
	if (mode == NULL || strcmp(mode, "sbs") == 0) {
		return SIM_DISPLAY_OUTPUT_SBS;
	} else if (strcmp(mode, "anaglyph") == 0) {
		return SIM_DISPLAY_OUTPUT_ANAGLYPH;
	} else if (strcmp(mode, "blend") == 0) {
		return SIM_DISPLAY_OUTPUT_BLEND;
	}
	return SIM_DISPLAY_OUTPUT_SBS;
}


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
		return XRT_ERROR_DEVICE_NOT_FOUND;
	}

	// Add to device list.
	xsysd->xdevs[xsysd->xdev_count++] = head;

	// Assign to role(s).
	ubrh->head = head;

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
