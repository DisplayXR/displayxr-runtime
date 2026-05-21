// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the simulation 3D display driver.
 *
 * Implements the @ref xrt_plugin_negotiate_fn_t signature defined in
 * `xrt/xrt_plugin.h`. The runtime DLL loads this plug-in via
 * `LoadLibraryExW` + `GetProcAddress("xrtPluginNegotiate")` and
 * dispatches through the returned @ref xrt_plugin_iface vtable. See
 * `docs/roadmap/vendor-plugin-architecture.md` §4.5 (the sim_display
 * plug-in section) and `docs/adr/ADR-019-vendor-plugin-aux-boundary.md`.
 *
 * sim_display has no hardware to probe — it is the vendor-neutral
 * fallback. The probe vtable entry therefore always returns
 * `XRT_SUCCESS` with a NULL instance handle; no per-instance state is
 * needed (the device, output mode, and view count atomics in
 * `sim_display_device.c` are process-singleton and used directly).
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"

#include "sim_display_interface.h"

#include <stddef.h>


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
sim_display_plugin_probe(struct xrt_plugin_instance **out_inst)
{
	/*
	 * sim_display is the vendor-neutral fallback — it always claims the
	 * system. Per ADR-019, ProbeOrder=200 (set at registration time) ranks
	 * it after every real-hardware plug-in.
	 *
	 * There is no per-instance state: the device, output-mode atomic, and
	 * view-count atomic live in sim_display_device.c as process-singletons.
	 */
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
sim_display_plugin_create_device(struct xrt_plugin_instance *inst, struct xrt_device **out_dev)
{
	(void)inst;
	struct xrt_device *xdev = sim_display_hmd_create();
	if (xdev == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	*out_dev = xdev;
	return XRT_SUCCESS;
}

static void
sim_display_plugin_destroy(struct xrt_plugin_instance *inst)
{
	(void)inst;
	/* No instance state — nothing to free. */
}

static bool
sim_display_plugin_get_display_info(struct xrt_plugin_instance *inst,
                                    struct xrt_device *xdev,
                                    struct xrt_plugin_display_info *out_info)
{
	(void)inst;

	struct sim_display_info sd_info;
	if (!sim_display_get_display_info(xdev, &sd_info)) {
		return false;
	}

	/*
	 * v1: runtime + plug-in are built against the same xrt_plugin.h,
	 * so out_info->struct_size always equals
	 * sizeof(struct xrt_plugin_display_info). A future struct
	 * extension will gate per-field writes on the reported
	 * struct_size for forward compat.
	 */
	(void)out_info->struct_size;

	out_info->display_width_m = sd_info.display_width_m;
	out_info->display_height_m = sd_info.display_height_m;
	out_info->nominal_viewer_x_m = 0.0f;
	out_info->nominal_viewer_y_m = sd_info.nominal_y_m;
	out_info->nominal_viewer_z_m = sd_info.nominal_z_m;
	out_info->display_pixel_width = sd_info.display_pixel_width;
	out_info->display_pixel_height = sd_info.display_pixel_height;
	/* sim_display has no SR-style recommended scale; runtime derives
	 * one from the worst-case rendering mode when these stay zero. */
	out_info->recommended_view_scale_x = 0.0f;
	out_info->recommended_view_scale_y = 0.0f;
	/* No EDID screen position for the simulated display. */
	out_info->display_screen_left = 0;
	out_info->display_screen_top = 0;
	/* MANUAL eye tracking only — the simulated device is always
	 * "tracking" and eye positions come from app input. */
	out_info->supported_eye_tracking_modes = 2u; /* MANUAL_BIT */
	out_info->default_eye_tracking_mode = 1u;    /* MANUAL */

	return true;
}


/*
 *
 * Vtable.
 *
 */

static struct xrt_plugin_iface g_sim_display_iface = {
    .struct_size = sizeof(struct xrt_plugin_iface),
    .reserved_0 = 0,

    .id = "sim-display",
    .display_name = "DisplayXR Sim Display",
    .vendor = "DisplayXR",
    .version = NULL, /* matches the runtime's release tag at install time */

    .probe = sim_display_plugin_probe,
    .create_device = sim_display_plugin_create_device,

    /*
     * Per-graphics-API DP factories. sim_display ships factories for
     * every API the platform supports; the runtime picks one at session
     * creation based on the app's graphics binding. Each function
     * pointer is the existing factory from sim_display_interface.h —
     * the signatures already match the xrt_dp_factory_*_fn_t typedefs.
     */
#if defined(XRT_HAVE_VULKAN) || !defined(_WIN32)
    .create_dp_vk = sim_display_dp_factory_vk,
#else
    .create_dp_vk = NULL,
#endif

#if defined(_WIN32)
    .create_dp_d3d11 = sim_display_dp_factory_d3d11,
#else
    .create_dp_d3d11 = NULL,
#endif

#if defined(_WIN32)
    .create_dp_d3d12 = sim_display_dp_factory_d3d12,
#else
    .create_dp_d3d12 = NULL,
#endif

    .create_dp_gl = sim_display_dp_factory_gl,

#if defined(__APPLE__)
    .create_dp_metal = sim_display_dp_factory_metal,
#else
    .create_dp_metal = NULL,
#endif

    .destroy = sim_display_plugin_destroy,

    .get_display_info = sim_display_plugin_get_display_info,
};


/*
 *
 * Entry point.
 *
 */

XRT_PLUGIN_EXPORT xrt_result_t
xrtPluginNegotiate(uint32_t runtime_api_version,
                   const struct xrt_plugin_host_iface *host,
                   struct xrt_plugin_iface **out_iface,
                   uint32_t *out_plugin_api_version)
{
	(void)host;

	*out_plugin_api_version = XRT_PLUGIN_API_VERSION_CURRENT;

	if (runtime_api_version != XRT_PLUGIN_API_VERSION_CURRENT) {
		/*
		 * The runtime is from a different ABI generation. Decline
		 * cleanly; the runtime will log the mismatch and skip us.
		 */
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	*out_iface = &g_sim_display_iface;
	return XRT_SUCCESS;
}
