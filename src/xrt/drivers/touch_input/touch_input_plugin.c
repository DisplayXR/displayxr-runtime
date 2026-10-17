// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the touch input provider (ADR-034 / #132).
 *
 * Implements @ref xrt_input_plugin_negotiate_fn_t. The provider state is one
 * process-wide recogniser fed from the host's pointer queue: both devices
 * share it (the controller's aim ray and the navigation pose come from the
 * same finger), so it lives here, under one mutex, and the devices hold a
 * pointer.
 *
 * Where the events come from is the RUNTIME's business (it owns the surfaces
 * — the hosted SurfaceView, an app surface fed through
 * `xrSubmitAndroidInputEventDXR`, later the workspace controller's forwarded
 * events, #1996). Where they go — orbit, dolly, pan, select, recenter — is
 * decided entirely in `touch_gesture.c`, on this side of the ABI.
 *
 * Presence: the touch screen is the device, and it is there for as long as
 * the process runs — so `get_presence` says PRESENT whenever the provider
 * loaded at all. Validity of the navigation pose is likewise unconditional:
 * an untouched rig is a rig at rest, not a lost one.
 *
 * @ingroup drv_touch_input
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include "touch_gesture.h"
#include "touch_input_interface.h"
#include "touch_input_provider.h"
#include "touch_input_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Provider state.
 *
 */

//! How many events one pump drains at most: the sink holds 128, a poll never needs more.
#define TOUCH_INPUT_DRAIN_MAX 128

//! Process-wide: the loader hands every provider the same persistent host iface, and there is one touch screen.
static struct touch_input_provider g_provider;
static bool g_provider_ready = false;

/*!
 * The display geometry is published by the builder AFTER negotiate and before
 * create_devices, so the first chance to read it is create_devices — which is
 * where this runs. A runtime without the slot keeps the recogniser's defaults.
 */
static void
touch_input_apply_geometry(struct touch_input_provider *tp)
{
	if (tp->geometry_applied || tp->get_geometry == NULL) {
		return;
	}
	struct xrt_input_host_display_geometry geo = {0};
	geo.struct_size = (uint32_t)sizeof(geo);
	if (tp->get_geometry(&geo) != XRT_SUCCESS) {
		return;
	}
	struct touch_gesture_config cfg = tp->gesture.cfg;
	cfg.display_width_m = geo.display_width_m;
	cfg.display_height_m = geo.display_height_m;
	cfg.viewer_x_m = geo.nominal_viewer_x_m;
	cfg.viewer_y_m = geo.nominal_viewer_y_m;
	cfg.viewer_z_m = geo.nominal_viewer_z_m;
	touch_gesture_init(&tp->gesture, &cfg);
	tp->geometry_applied = true;

	U_LOG_W("touch-input: display geometry %.3f x %.3f m, nominal viewer (%.3f, %.3f, %.3f) m — orbit pivot at "
	        "rest = the display plane.",
	        (double)cfg.display_width_m, (double)cfg.display_height_m, (double)cfg.viewer_x_m,
	        (double)cfg.viewer_y_m, (double)cfg.viewer_z_m);
}

void
touch_input_pump(struct touch_input_provider *tp, int64_t now_ns)
{
	if (tp == NULL) {
		return;
	}
	os_mutex_lock(&tp->mutex);
	if (tp->poll != NULL) {
		struct xrt_input_pointer_event evs[TOUCH_INPUT_DRAIN_MAX];
		uint32_t n = 0;
		// Drain until empty: a burst larger than one batch is still one poll.
		do {
			n = tp->poll(evs, TOUCH_INPUT_DRAIN_MAX);
			for (uint32_t i = 0; i < n; i++) {
				touch_gesture_feed(&tp->gesture, &evs[i]);
			}
			tp->events_total += n;
		} while (n == TOUCH_INPUT_DRAIN_MAX);
	}
	touch_gesture_tick(&tp->gesture, now_ns);
	tp->last_pump_ns = now_ns;
	os_mutex_unlock(&tp->mutex);
}


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
touch_input_plugin_probe(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;

	/* Android: the touch screen is always there, so the provider claims by
	 * default and ENABLE=0 withdraws it. Elsewhere nothing feeds the pointer
	 * queue yet, so the default is to decline — a registered-but-idle
	 * navigation device would take the rig role from the fly camera and
	 * freeze WASD/mouse-look for nothing. */
#ifdef XRT_OS_ANDROID
	const bool dflt = true;
#else
	const bool dflt = false;
#endif
	if (!touch_input_setting_on("ENABLE", dflt)) {
		U_LOG_I("touch-input: declining — 'displayxr-cli input set touch-input ENABLE 1' (or "
		        "DXR_INPUT_TOUCH_INPUT_ENABLE=1 in the hosting process) enables the touch provider here.");
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	if (!g_provider_ready) {
		memset(&g_provider, 0, sizeof(g_provider));
		if (os_mutex_init(&g_provider.mutex) != 0) {
			return XRT_ERROR_ALLOCATION;
		}
		touch_gesture_init(&g_provider.gesture, NULL);
		g_provider_ready = true;
	}

	*out_inst = (struct xrt_input_plugin_instance *)&g_provider;
	U_LOG_W("touch-input: claiming the system — touch drives the rig (orbit / pinch / pan / double-tap "
	        "recenter) and an emulated controller (aim = finger, select = tap or hold).");
	return XRT_SUCCESS;
}

static xrt_result_t
touch_input_plugin_create_devices(struct xrt_input_plugin_instance *inst,
                                  struct xrt_device **out_devices,
                                  uint32_t max_count,
                                  uint32_t *out_count)
{
	struct touch_input_provider *tp = (struct touch_input_provider *)inst;
	*out_count = 0;
	if (tp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	touch_input_apply_geometry(tp);

	const float sens = touch_input_setting_float("SENSITIVITY", 1.0f);
	if (sens != tp->gesture.cfg.sensitivity) {
		struct touch_gesture_config cfg = tp->gesture.cfg;
		cfg.sensitivity = sens;
		touch_gesture_init(&tp->gesture, &cfg);
	}

	const bool with_controller = touch_input_setting_on("CONTROLLER", true);
	const uint32_t needed = with_controller ? 2 : 1;
	if (max_count < needed) {
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_device *nav = touch_input_create_navigation(tp);
	struct xrt_device *ctl = with_controller ? touch_input_create_controller(tp) : NULL;
	if (nav == NULL || (with_controller && ctl == NULL)) {
		if (nav != NULL) {
			nav->destroy(nav);
		}
		if (ctl != NULL) {
			ctl->destroy(ctl);
		}
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	out_devices[0] = nav;
	if (ctl != NULL) {
		out_devices[1] = ctl;
	}
	*out_count = needed;
	return XRT_SUCCESS;
}

static void
touch_input_plugin_destroy(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	/* The provider state is process-static: the devices the runtime still
	 * owns may be destroyed after this call, and they only ever read it
	 * under its mutex. Nothing to free. */
}

static enum xrt_input_provider_presence
touch_input_plugin_get_presence(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	/* The touch screen is the hardware, and it is attached for as long as
	 * the process runs. Presence is transport, not "a finger is down"
	 * (ADR-034 Amendment 1): an idle screen must NOT bounce the rig role. */
	return XRT_INPUT_PROVIDER_PRESENCE_PRESENT;
}


/*
 *
 * Vtable.
 *
 */

static struct xrt_input_plugin_iface g_touch_input_iface = {
    .struct_size = sizeof(struct xrt_input_plugin_iface),
    .reserved_0 = 0,

    .id = TOUCH_INPUT_ID,
    .display_name = "DisplayXR Touch Input",
    .vendor = "DisplayXR",
    .version = NULL, /* matches the runtime's release tag at install time */

    .probe = touch_input_plugin_probe,
    .create_devices = touch_input_plugin_create_devices,
    .destroy = touch_input_plugin_destroy,

    .get_presence = touch_input_plugin_get_presence,
    .describe_settings = touch_input_describe_settings,
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

	touch_input_settings_bind(host);

	// The two host slots this provider lives on. Both carved from reserved[]
	// (API major 1 unchanged), so cover-check the struct_size before reading.
	g_provider.poll = NULL;
	g_provider.get_geometry = NULL;
	if (host != NULL) {
		if (host->struct_size >= offsetof(struct xrt_input_plugin_host_iface, poll_pointer_events) +
		                             sizeof(host->poll_pointer_events)) {
			g_provider.poll = host->poll_pointer_events;
		}
		if (host->struct_size >= offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
		                             sizeof(host->get_display_geometry)) {
			g_provider.get_geometry = host->get_display_geometry;
		}
	}
	if (g_provider.poll == NULL) {
		U_LOG_W("touch-input: this runtime has no pointer-event queue (host iface predates "
		        "poll_pointer_events) — the provider loads but nothing will ever move.");
	}

	*out_iface = &g_touch_input_iface;
	return XRT_SUCCESS;
}
