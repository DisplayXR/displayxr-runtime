// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the Ultraleap input provider.
 *
 * Implements @ref xrt_input_plugin_negotiate_fn_t (ADR-034 / #825) for
 * the Ultraleap hand-as-motion-controller provider.
 *
 * `probe()` deliberately always succeeds: it answers "should this
 * provider be loaded", which is a registration decision, not a hardware
 * one. Whether a Leap Motion is actually plugged in is answered
 * separately and continuously by `get_presence()`, because the loader is
 * one-shot — a probe that declined on an unplugged device could never
 * notice it being plugged in later, and the user would be stuck on the
 * qwerty fallback for the rest of the process. With presence reported
 * honestly, the runtime moves the hand roles between this provider and
 * qwerty as the hardware comes and goes.
 *
 * Register with `register_dev_plugin.bat input <path>\DisplayXR-Ultraleap.dll`
 * (Windows) or an `NNN-ultraleap-input-provider.json` manifest (POSIX).
 *
 * @author David Fattal
 * @ingroup drv_ultraleap
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "ultraleap_interface.h"

#include <stddef.h>


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
ul_plugin_probe(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
ul_plugin_create_devices(struct xrt_input_plugin_instance *inst,
                         struct xrt_device **out_devices,
                         uint32_t max_count,
                         uint32_t *out_count)
{
	(void)inst;

	*out_count = 0;
	if (max_count < 2) {
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_device *left = NULL;
	struct xrt_device *right = NULL;
	xrt_result_t xret = ul_create_devices(&left, &right);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	out_devices[0] = left;
	out_devices[1] = right;
	*out_count = 2;
	return XRT_SUCCESS;
}

static void
ul_plugin_destroy(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	/* Hub teardown rides the devices' refcounted destroy. */
}

static enum xrt_input_provider_presence
ul_plugin_get_presence(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	return ul_get_presence();
}


/*
 *
 * Vtable.
 *
 */

static struct xrt_input_plugin_iface g_ul_iface = {
    .struct_size = sizeof(struct xrt_input_plugin_iface),
    .reserved_0 = 0,

    .id = "ultraleap",
    .display_name = "Ultraleap Hand Tracking (controller emulation)",
    .vendor = "DisplayXR",
    .version = NULL,

    .probe = ul_plugin_probe,
    .create_devices = ul_plugin_create_devices,
    .destroy = ul_plugin_destroy,

    .get_presence = ul_plugin_get_presence,
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
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	*out_iface = &g_ul_iface;
	return XRT_SUCCESS;
}
