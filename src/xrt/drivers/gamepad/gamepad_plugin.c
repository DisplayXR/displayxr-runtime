// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the gamepad input provider.
 *
 * Implements @ref xrt_input_plugin_negotiate_fn_t (`xrt/xrt_input_plugin.h`,
 * ADR-034 / ADR-054). The runtime loads `DisplayXR-Gamepad` via
 * `LoadLibraryExW` + `GetProcAddress("xrtInputPluginNegotiate")` (dlopen /
 * dlsym on POSIX) and dispatches through the vtable below.
 *
 * Probe: claims the system wherever the platform has a pad reader at all
 * (XInput on Windows, GameController on macOS); declines cleanly where it
 * has none (desktop Linux until the evdev follow-up), so a staged manifest
 * there costs nothing. Whether a pad is ATTACHED is `get_presence`'s
 * business, polled by the hub, so a pad plugged in mid-session takes its
 * roles without a restart.
 *
 * Settings (ADR-054 decision 7, through the host iface only —
 * `displayxr-cli input options gamepad`): `DEADZONE`, `NAV_SPEED`,
 * `NAV_TURN_SPEED`, `NAV`, `EMULATE_CONTROLLERS`, `INVERT_LOOK_Y`,
 * `IDLE_TIMEOUT_S` (`gamepad_settings.c`). Read at `create_devices`, not
 * per frame.
 *
 * Presence is "last-used input wins", not "plugged in": a pad is PRESENT
 * only while connected AND touched within `IDLE_TIMEOUT_S` (default 20 s).
 * Otherwise a pad that merely lives in a USB port would hold the rig and
 * (with EMULATE_CONTROLLERS) the hands forever and silently disable WASD /
 * mouse-look. Touch the pad and it takes the roles at the arbiter's next
 * presence poll; leave it and the floor comes back.
 *
 * @author David Fattal
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "util/u_logging.h"

#include "gamepad_backend.h"
#include "gamepad_interface.h"
#include "gamepad_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>



/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
gamepad_plugin_probe(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;

	if (!gamepad_backend_available()) {
		U_LOG_I("gamepad: declining — no pad reader on this platform (%s).", gamepad_backend_name());
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
gamepad_plugin_create_devices(struct xrt_input_plugin_instance *inst,
                              struct xrt_device **out_devices,
                              uint32_t max_count,
                              uint32_t *out_count)
{
	(void)inst;
	*out_count = 0;

	const bool emulate = gamepad_setting_on("EMULATE_CONTROLLERS", true);
	char profile[16];
	const bool touch = emulate && gamepad_setting("EMULATE_PROFILE", profile, sizeof(profile)) &&
	                   strcmp(profile, "touch") == 0;
	// Touch hands take the sticks, so the rig would fight them: off unless asked for.
	const bool nav = gamepad_setting_on("NAV", !touch);
	const uint32_t needed = 1 + (nav ? 1 : 0) + (emulate ? 2 : 0);
	if (max_count < needed) {
		return XRT_ERROR_ALLOCATION;
	}

	struct gamepad_hub_config cfg = {
	    .table = &gamepad_map_default_table,
	    .map = GAMEPAD_MAP_CONFIG_DEFAULT,
	};
	cfg.map.deadzone = gamepad_setting_float("DEADZONE", cfg.map.deadzone, 0.0f, 0.9f);
	cfg.map.nav_speed_mps = gamepad_setting_float("NAV_SPEED", cfg.map.nav_speed_mps, 0.01f, 20.0f);
	cfg.map.nav_turn_dps = gamepad_setting_float("NAV_TURN_SPEED", cfg.map.nav_turn_dps, 1.0f, 720.0f);
	cfg.map.invert_look_y = gamepad_setting_on("INVERT_LOOK_Y", false);
	cfg.idle_timeout_s = gamepad_setting_float("IDLE_TIMEOUT_S", 20.0f, 0.0f, 3600.0f);

	struct gamepad_hub *hub = gamepad_hub_create(&cfg);
	if (hub == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct xrt_device *devs[4] = {0};
	uint32_t n = 0;
	devs[n++] = gamepad_create_pad_device(hub);
	if (nav) {
		devs[n++] = gamepad_create_navigation(hub);
	}
	if (touch) {
		struct gamepad_touch_config tcfg = {
		    .deadzone = cfg.map.deadzone,
		    .hand_speed_mps = gamepad_setting_float("HAND_SPEED", 0.25f, 0.01f, 5.0f),
		    .right_home = {0.08f, -0.04f, 0.12f},
		};
		char home[64];
		if (gamepad_setting("HAND_HOME", home, sizeof(home))) {
			struct xrt_vec3 v;
			if (sscanf(home, "%f,%f,%f", &v.x, &v.y, &v.z) == 3) {
				tcfg.right_home = v;
			} else {
				U_LOG_W("gamepad: HAND_HOME '%s' is not x,y,z — keeping the default.", home);
			}
		}
		devs[n++] = gamepad_create_touch_controller(hub, XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, &tcfg);
		devs[n++] = gamepad_create_touch_controller(hub, XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, &tcfg);
	} else if (emulate) {
		devs[n++] = gamepad_create_emulated_controller(hub, XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER);
		devs[n++] = gamepad_create_emulated_controller(hub, XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER);
	}

	bool ok = true;
	for (uint32_t i = 0; i < n; i++) {
		ok &= devs[i] != NULL;
	}
	if (!ok) {
		for (uint32_t i = 0; i < n; i++) {
			if (devs[i] != NULL) {
				devs[i]->destroy(devs[i]);
			}
		}
		gamepad_hub_release(hub);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	for (uint32_t i = 0; i < n; i++) {
		out_devices[i] = devs[i];
	}
	*out_count = n;

	// The devices now hold the hub; drop the creation reference. The hub
	// stays the "current" one (gamepad_hub_current) until the last device's
	// destroy frees it.
	gamepad_hub_release(hub);

	U_LOG_W("gamepad: %u device(s) — pad%s%s; a present pad takes the gamepad role%s%s.", n,
	        nav ? ", navigation" : "",
	        touch ? ", emulated left/right Touch controllers" : emulate ? ", emulated left/right controllers" : "",
	        nav ? " and the rig" : "", emulate ? " and the hand roles" : "");
	return XRT_SUCCESS;
}

static void
gamepad_plugin_destroy(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	// No instance state — the hub is owned by the devices, which the
	// runtime destroys via their own xrt_device::destroy.
}

static enum xrt_input_provider_presence
gamepad_plugin_get_presence(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	// Owned by the devices; NULL before create_devices (UNKNOWN = not present).
	return gamepad_hub_presence(gamepad_hub_current());
}


/*
 *
 * Vtable.
 *
 */

static struct xrt_input_plugin_iface g_gamepad_iface = {
    .struct_size = sizeof(struct xrt_input_plugin_iface),
    .reserved_0 = 0,

    .id = GAMEPAD_ID,
    .display_name = "DisplayXR Gamepad",
    .vendor = "DisplayXR",
    .version = NULL, /* matches the runtime's release tag at install time */

    .probe = gamepad_plugin_probe,
    .create_devices = gamepad_plugin_create_devices,
    .destroy = gamepad_plugin_destroy,

    .get_presence = gamepad_plugin_get_presence,
    .describe_settings = gamepad_describe_settings,
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
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	gamepad_settings_bind(host);

	*out_iface = &g_gamepad_iface;
	return XRT_SUCCESS;
}
