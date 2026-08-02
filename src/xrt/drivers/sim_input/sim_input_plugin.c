// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the simulated input provider.
 *
 * Implements the @ref xrt_input_plugin_negotiate_fn_t signature defined
 * in `xrt/xrt_input_plugin.h` (ADR-034 / #823). The runtime loads this
 * provider via `LoadLibraryExW` + `GetProcAddress("xrtInputPluginNegotiate")`
 * (dlopen/dlsym on POSIX) and dispatches through the returned
 * @ref xrt_input_plugin_iface vtable.
 *
 * sim_input has no hardware to probe — it is the vendor-neutral test
 * provider, registered at ProbeOrder 200 so any real tracking vendor
 * out-ranks it. Probe therefore always succeeds with a NULL instance
 * handle; the devices are self-contained (`sim_input_device.c`).
 *
 * @author David Fattal
 * @ingroup drv_sim_input
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "sim_input_interface.h"

#include <stddef.h>


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
sim_input_plugin_probe(struct xrt_input_plugin_instance **out_inst)
{
	/* Always claims the system — the vendor-neutral test provider. No
	 * per-instance state; ProbeOrder=200 (set at registration) ranks it
	 * after every real-hardware provider. */
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
sim_input_plugin_create_devices(struct xrt_input_plugin_instance *inst,
                                struct xrt_device **out_devices,
                                uint32_t max_count,
                                uint32_t *out_count)
{
	(void)inst;

	*out_count = 0;
	if (max_count < 2) {
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_device *left = sim_input_create_controller(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER);
	struct xrt_device *right = sim_input_create_controller(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER);
	if (left == NULL || right == NULL) {
		if (left != NULL) {
			left->destroy(left);
		}
		if (right != NULL) {
			right->destroy(right);
		}
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	out_devices[0] = left;
	out_devices[1] = right;
	*out_count = 2;
	return XRT_SUCCESS;
}

static void
sim_input_plugin_destroy(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	/* No instance state — devices are destroyed by the runtime via
	 * their own xrt_device::destroy. */
}


/*
 *
 * Vtable.
 *
 */

static struct xrt_input_plugin_iface g_sim_input_iface = {
    .struct_size = sizeof(struct xrt_input_plugin_iface),
    .reserved_0 = 0,

    .id = "sim-input",
    .display_name = "DisplayXR Sim Input",
    .vendor = "DisplayXR",
    .version = NULL, /* matches the runtime's release tag at install time */

    .probe = sim_input_plugin_probe,
    .create_devices = sim_input_plugin_create_devices,
    .destroy = sim_input_plugin_destroy,
};


/*
 *
 * Entry point.
 *
 */

XRT_INPUT_PLUGIN_EXPORT xrt_result_t
xrtInputPluginNegotiate(uint32_t runtime_api_version,
                        const struct xrt_input_plugin_host_iface *host,
                        struct xrt_input_plugin_iface **out_iface,
                        uint32_t *out_plugin_api_version)
{
	(void)host;

	*out_plugin_api_version = XRT_INPUT_PLUGIN_API_VERSION_CURRENT;

	if (runtime_api_version != XRT_INPUT_PLUGIN_API_VERSION_CURRENT) {
		/* Different ABI generation — decline cleanly; the runtime
		 * logs the mismatch and skips us. */
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	*out_iface = &g_sim_input_iface;
	return XRT_SUCCESS;
}
