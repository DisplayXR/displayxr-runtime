// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the network-fed input provider.
 *
 * Implements @ref xrt_input_plugin_negotiate_fn_t (ADR-034 / #823) for
 * net_input. Probe always succeeds (the transport is a loopback listen
 * socket we can only meaningfully claim at create time); if binding the
 * port fails at `create_devices`, the builder falls back to qwerty.
 *
 * net_input is opt-in: nothing registers it by default. Register it
 * explicitly (`register_dev_plugin.bat input <dll>` on Windows, an
 * `NNN-net-input-input-provider.json` manifest on POSIX) when an
 * external feeder process supplies the tracking.
 *
 * @author David Fattal
 * @ingroup drv_net_input
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "net_input_interface.h"

#include <stddef.h>


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
net_input_plugin_probe(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
net_input_plugin_create_devices(struct xrt_input_plugin_instance *inst,
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
	xrt_result_t xret = net_input_create_devices(0 /* default port */, &left, &right);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	out_devices[0] = left;
	out_devices[1] = right;
	*out_count = 2;
	return XRT_SUCCESS;
}

static void
net_input_plugin_destroy(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	/* Hub teardown rides the devices' refcounted destroy. */
}

static enum xrt_input_provider_presence
net_input_plugin_get_presence(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	return net_input_get_presence();
}


/*
 *
 * Vtable.
 *
 */

static struct xrt_input_plugin_iface g_net_input_iface = {
    .struct_size = sizeof(struct xrt_input_plugin_iface),
    .reserved_0 = 0,

    .id = "net-input",
    .display_name = "DisplayXR Network Input",
    .vendor = "DisplayXR",
    .version = NULL,

    .probe = net_input_plugin_probe,
    .create_devices = net_input_plugin_create_devices,
    .destroy = net_input_plugin_destroy,

    .get_presence = net_input_plugin_get_presence,
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

	*out_iface = &g_net_input_iface;
	return XRT_SUCCESS;
}
