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
 * `probe()` declines unless its `ENABLE` setting is 1 — being registered
 * is not enough. Settings reach it ONLY through the host iface (ADR-054
 * decision 7, the one options contract — N3c): `displayxr-cli input set
 * sim-input ENABLE 1` (per user; `--machine` = HKLM), or
 * `DXR_INPUT_SIM_INPUT_ENABLE=1` in the HOSTING process's environment,
 * which always wins. The pre-N3c `DXR_SIM_INPUT=1` keeps working for one
 * release as an alias (`sim_input_settings.c`, with a deprecation WARN).
 * Process-level caveat unchanged: the runtime DLL has its own static-CRT
 * environment block, so set a variable before launching the host process,
 * not from a run script.
 *
 * A second, independent opt-in adds the scripted NAVIGATION device that
 * drives the rig role (ADR-034 Amendment 4 / #1380):
 *
 *   - `NAV` = 1             (alias `DXR_SIM_INPUT_NAV=1`) — create it (default
 *                           OFF, so every pre-existing run is unchanged). It
 *                           also switches the controllers to
 *                           display-plane-relative poses, which is what the
 *                           contract requires of a provider that navigates.
 *   - `NAV_HOLD_MS` = n     (alias `DXR_SIM_INPUT_NAV_HOLD_MS`) — every n ms
 *                           the navigation pose reports invalid for 500 ms
 *                           (composer hold / re-align path). Default 0 = never.
 *   - `NAV_RECENTER_MS` = n (alias `DXR_SIM_INPUT_NAV_RECENTER_MS`) — fire a
 *                           scripted recenter every n ms. Default 0 = never.
 *
 * @author David Fattal
 * @ingroup drv_sim_input
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "util/u_logging.h"

#include "sim_input_interface.h"
#include "sim_input_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
sim_input_plugin_probe(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;

	/* Registration alone must NOT put synthetic controllers in front of
	 * the user — see the file comment. Decline cleanly so the loader
	 * moves on and, with no other provider, qwerty keeps the hand roles.
	 * No per-instance state; ProbeOrder=200 (set at registration) still
	 * ranks this after every real-hardware provider when it IS opted in. */
	if (!sim_input_setting_on("ENABLE")) {
		U_LOG_I(
		    "sim-input: declining — 'displayxr-cli input set sim-input ENABLE 1' (or "
		    "DXR_INPUT_SIM_INPUT_ENABLE=1 in the hosting process) enables the simulated input provider.");
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	U_LOG_W("sim-input: ENABLE is set — SIMULATED controllers will drive the hand roles.");
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

	/* #1380: the scripted NAVIGATION device is a SECOND opt-in on top of
	 * ENABLE. Default OFF, so every existing selftest / CI run that only
	 * asks for simulated hands keeps exactly the behaviour it had — the
	 * rig stays on the qwerty floor and the controllers keep their
	 * stage-anchored circle. With it on, this provider drives the rig
	 * role end-to-end with no hardware. */
	const bool nav = sim_input_setting_on("NAV");
	const uint32_t needed = nav ? 3 : 2;
	if (max_count < needed) {
		return XRT_ERROR_ALLOCATION;
	}

	/* A provider that navigates publishes its controllers
	 * display-plane-relative (ADR-034 Amendment 4) — so the knob moves the
	 * controllers' frame too, which is the whole point: the reference
	 * provider must model the contract, not half of it. */
	struct xrt_device *left = sim_input_create_controller(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, nav);
	struct xrt_device *right = sim_input_create_controller(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, nav);
	struct xrt_device *navdev = NULL;
	if (nav) {
		navdev = sim_input_create_navigation(sim_input_setting_ms("NAV_HOLD_MS"),
		                                     sim_input_setting_ms("NAV_RECENTER_MS"));
	}

	if (left == NULL || right == NULL || (nav && navdev == NULL)) {
		if (left != NULL) {
			left->destroy(left);
		}
		if (right != NULL) {
			right->destroy(right);
		}
		if (navdev != NULL) {
			navdev->destroy(navdev);
		}
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	out_devices[0] = left;
	out_devices[1] = right;
	if (navdev != NULL) {
		out_devices[2] = navdev;
	}
	*out_count = needed;
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

    /* N3c: what `displayxr-cli input options sim-input` lists. */
    .describe_settings = sim_input_describe_settings,
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
	*out_plugin_api_version = XRT_INPUT_PLUGIN_API_VERSION_CURRENT;

	if (runtime_api_version != XRT_INPUT_PLUGIN_API_VERSION_CURRENT) {
		/* Different ABI generation — decline cleanly; the runtime
		 * logs the mismatch and skips us. */
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	/* N3c: settings come through the host from now on (the pointer lives
	 * in the runtime for the life of the process; a provider may keep it). */
	sim_input_settings_bind(host);

	*out_iface = &g_sim_input_iface;
	return XRT_SUCCESS;
}
