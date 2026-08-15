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
 * out-ranks it. The devices are self-contained (`sim_input_device.c`).
 *
 * **NOT a product path.** This provider synthesises a fixed controller
 * motion pattern; left registered and un-gated it silently displaces the
 * qwerty fallback and the user sees phantom controllers sweeping through
 * the scene with nothing driving them. It was a #825 debugging aid, so
 * `probe()` now declines unless `DXR_SIM_INPUT=1` is set in the
 * environment — being registered is no longer enough. (Env var rather
 * than the usual registry gate on purpose: this is a developer switch
 * flipped per-run, not machine configuration. Note the process-level
 * caveat — the runtime DLL has its own static-CRT environment block, so
 * set it before launching the host process, not from a run script.)
 *
 * @author David Fattal
 * @ingroup drv_sim_input
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "util/u_logging.h"

#include "sim_input_interface.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Vtable callbacks.
 *
 */

/*!
 * Is the `DXR_SIM_INPUT` opt-in set? Anything but unset / empty / "0" /
 * "false" / "no" / "off" counts as on.
 */
static bool
sim_input_opted_in(void)
{
	const char *v = getenv("DXR_SIM_INPUT");
	if (v == NULL || v[0] == '\0') {
		return false;
	}
	return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 && strcmp(v, "no") != 0 && strcmp(v, "off") != 0;
}

static xrt_result_t
sim_input_plugin_probe(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;

	/* Registration alone must NOT put synthetic controllers in front of
	 * the user — see the file comment. Decline cleanly so the loader
	 * moves on and, with no other provider, qwerty keeps the hand roles.
	 * No per-instance state; ProbeOrder=200 (set at registration) still
	 * ranks this after every real-hardware provider when it IS opted in. */
	if (!sim_input_opted_in()) {
		U_LOG_I("sim-input: declining — set DXR_SIM_INPUT=1 to enable the simulated input provider.");
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	U_LOG_W("sim-input: DXR_SIM_INPUT set — SIMULATED controllers will drive the hand roles.");
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

static enum xrt_input_provider_presence
sim_input_plugin_get_presence(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	/* Simulated hardware is present exactly when it was asked for, and
	 * probe() already enforced the opt-in — so once we are loaded at all,
	 * the answer is yes, and the roles stay with us rather than bouncing
	 * to qwerty. */
	return XRT_INPUT_PROVIDER_PRESENCE_PRESENT;
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

    .get_presence = sim_input_plugin_get_presence,
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
