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
#include "target_plugin_loader.h"

#include "sim_display/sim_display_interface.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty/qwerty_device.h"
#endif

#include <assert.h>
#include <string.h>
#include <stdlib.h>


DEBUG_GET_ONCE_BOOL_OPTION(force_sim_display, "FORCE_SIM_DISPLAY", false)


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
	estimate->certain.head = true;

	if (debug_get_bool_option_force_sim_display()) {
		estimate->priority = -10; // Forced: override vendor drivers (leia is -15)
	} else {
		estimate->priority = -20; // Fallback: below vendor drivers, above qwerty (-25)
	}

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
	/*
	 * Prefer the sim_display plug-in DLL when present. The loader caches
	 * the negotiated iface across calls; failure → NULL, which falls
	 * through to the statically-linked sim_display_hmd_create() — same
	 * device shape, just an in-process construction. Issue #256.
	 */
	struct xrt_device *head = NULL;
	const struct xrt_plugin_iface *plugin = target_plugin_get_active();
	if (plugin != NULL && plugin->create_device != NULL) {
		if (plugin->create_device(NULL, &head) != XRT_SUCCESS) {
			head = NULL;
		}
	}
	if (head == NULL) {
		head = sim_display_hmd_create();
	}
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
	// Configure qwerty HMD pose and delegate head's pose to qwerty for
	// WASD/mouse camera control. Vendor-agnostic via the plug-in iface
	// (issue #256) — historically this code called
	// `sim_display_hmd_set_pose_source(head, qwerty_hmd)` directly,
	// which downcasts `head` to a sim_display container struct. With
	// iface->create_device potentially returning a non-sim device
	// (e.g. Leia), the direct call corrupted the head's vtable
	// backing and broke shell rendering. Route through the iface
	// instead; the plug-in owns the vendor-private cast.
	if (qwerty_hmd != NULL) {
		struct qwerty_device *qd = qwerty_device(qwerty_hmd);
		qd->pose.position = (struct xrt_vec3){0, 1.6f, 0};
		qd->pose.orientation = (struct xrt_quat){0, 0, 0, 1};

		// Dims for the qwerty system: screen height + nominal viewer
		// Z drive the WASD camera scaling. Iface-provided when
		// available; fall back to sim_display's typed query (only
		// returns true for sim_display devices, so this is safe even
		// when the head is some other vendor).
		float screen_height_m = 0.0f;
		float nominal_z_m = 0.0f;
		if (plugin != NULL &&
		    plugin->struct_size > offsetof(struct xrt_plugin_iface, get_display_info) &&
		    plugin->get_display_info != NULL) {
			struct xrt_plugin_display_info pdi = {0};
			pdi.struct_size = (uint32_t)sizeof(pdi);
			if (plugin->get_display_info(target_plugin_get_active_instance(), head, &pdi)) {
				screen_height_m = pdi.display_height_m;
				nominal_z_m = pdi.nominal_viewer_z_m;
			}
		}
		if (screen_height_m == 0.0f) {
			struct sim_display_info info;
			if (sim_display_get_display_info(head, &info)) {
				screen_height_m = info.display_height_m;
				nominal_z_m = info.nominal_z_m;
			}
		}
		if (screen_height_m > 0.0f) {
			qd->sys->screen_height_m = screen_height_m;
		}
		if (nominal_z_m > 0.0f) {
			qd->sys->nominal_viewer_z = nominal_z_m;
		}

		// Bind the qwerty HMD as the head's external pose source.
		// Iface-routed (plug-in owns the vendor-private cast);
		// falls back to the in-tree sim_display call when no
		// iface is active (in-proc fallback build, or a developer
		// configuration with no plug-ins).
		if (plugin != NULL &&
		    plugin->struct_size > offsetof(struct xrt_plugin_iface, set_pose_source) &&
		    plugin->set_pose_source != NULL) {
			plugin->set_pose_source(target_plugin_get_active_instance(), head, qwerty_hmd);
		} else {
			sim_display_hmd_set_pose_source(head, qwerty_hmd);
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
	ub->base.exclude_from_automatic_discovery = false; // Always available as fallback

	// u_builder fields.
	ub->open_system_static_roles = sim_display_open_system_impl;

	return &ub->base;
}
