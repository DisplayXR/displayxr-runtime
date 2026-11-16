// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the simulated stylus input provider.
 *
 * Implements @ref xrt_input_plugin_negotiate_fn_t (`xrt/xrt_input_plugin.h`,
 * ADR-034). The reference for a 6-DoF stylus provider (ADR-034 Amendment 8):
 *
 *   - one device on `XRT_DEVICE_MX_INK_STYLUS`
 *     (`/interaction_profiles/logitech/mx_ink_stylus_logitech`), every input
 *     of the profile plus its haptic output;
 *   - declared `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER`, so the builder puts
 *     it on the user's dominant hand (`InputProviders\Policy\Handedness`); the
 *     `HAND` setting can force a side, and a runtime without the
 *     `get_handedness` host slot (which would not resolve DOMINANT) gets an
 *     explicit RIGHT / LEFT instead;
 *   - poses on an `XRT_TRACKING_TYPE_RIG_LOCAL_PHYSICAL` origin (physical
 *     display-plane metres, screen-anchored, glass-registered);
 *   - presence follows the scripted dock (`DOCK_MS` / `DOCK_SLEEP_MS`): a pen
 *     asleep in its dock reports ABSENT, so the dominant hand's role goes back
 *     to the fallback and returns when it undocks;
 *   - `MODE=mouse` (Windows): the desktop mouse drives the pen while its gate
 *     is open (`GATE=scrolllock`, the default: Scroll Lock on), and presence
 *     follows the gate (`sim_stylus_mouse.h`). Elsewhere it falls back to
 *     `draw` with one WARN.
 *
 * **NOT a product path.** `probe()` declines unless its `ENABLE` setting is 1:
 * `displayxr-cli input set sim-stylus ENABLE 1` (per user; `--machine` =
 * HKLM), or `DXR_INPUT_SIM_STYLUS_ENABLE=1` in the HOSTING process's
 * environment (set it before launching that process). Every other knob is
 * listed by `displayxr-cli input options sim-stylus`.
 *
 * @ingroup drv_sim_stylus
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "os/os_time.h"

#include "util/u_logging.h"

#include "sim_stylus_interface.h"
#include "sim_stylus_mouse.h"
#include "sim_stylus_script.h"
#include "sim_stylus_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>


//! The host iface (process-lifetime runtime storage); set at negotiate.
static const struct xrt_input_plugin_host_iface *g_host = NULL;

//! The script the device was created with — presence reads the same dock schedule.
static struct sim_stylus_script_config g_cfg;
static bool g_have_cfg = false;

static bool
host_has(size_t offset, size_t size)
{
	return g_host != NULL && g_host->struct_size >= offset + size;
}

static void
display_size(float *out_w, float *out_h)
{
	*out_w = 0.0f;
	*out_h = 0.0f;
	if (!host_has(offsetof(struct xrt_input_plugin_host_iface, get_display_geometry),
	              sizeof(g_host->get_display_geometry)) ||
	    g_host->get_display_geometry == NULL) {
		return;
	}
	struct xrt_input_host_display_geometry geo = {0};
	geo.struct_size = (uint32_t)sizeof(geo);
	if (g_host->get_display_geometry(&geo) != XRT_SUCCESS) {
		return;
	}
	*out_w = geo.display_width_m;
	*out_h = geo.display_height_m;
}

//! The host's handedness, or -1 when the host has no get_handedness slot.
static int
host_handedness(void)
{
#ifdef XRT_INPUT_PLUGIN_HOST_HAS_GET_HANDEDNESS
	if (host_has(offsetof(struct xrt_input_plugin_host_iface, get_handedness), sizeof(g_host->get_handedness)) &&
	    g_host->get_handedness != NULL) {
		return g_host->get_handedness() == XRT_INPUT_HANDEDNESS_LEFT ? 1 : 0;
	}
#endif
	return -1;
}

static const char *
mode_str(enum sim_stylus_mode mode)
{
	switch (mode) {
	case SIM_STYLUS_MODE_HOVER: return "hover";
	case SIM_STYLUS_MODE_MOUSE: return "mouse";
	default: return "draw";
	}
}

//! Resolve the mouse-mode settings and start the sampler; false = no sampler on this platform.
static bool
start_mouse(const struct sim_stylus_script_config *cfg)
{
	struct sim_stylus_mouse_config mc;
	memset(&mc, 0, sizeof(mc));
	mc.display_width_m = cfg->display_width_m;
	mc.display_height_m = cfg->display_height_m;
	mc.hover_height_m = cfg->hover_height_m;
	mc.left_handed = cfg->left_handed;
	mc.tilt_deg = sim_stylus_setting_float("TILT_DEG", SIM_STYLUS_MOUSE_TILT_DEFAULT_DEG);
	mc.ramp_ms = sim_stylus_setting_ms("RAMP_MS", SIM_STYLUS_MOUSE_RAMP_DEFAULT_MS);
	mc.double_tap_ms = sim_stylus_mouse_os_double_click_ms();
	mc.gate = SIM_STYLUS_MOUSE_GATE_SCROLLLOCK;

	char v[64];
	if (sim_stylus_setting("GATE", v, sizeof(v)) && !sim_stylus_mouse_parse_gate(v, &mc.gate)) {
		U_LOG_W("sim-stylus: GATE='%s' is not scrolllock|none - using scrolllock.", v);
	}
	char screen[64];
	sim_stylus_setting("SCREEN", screen, sizeof(screen));

	if (!sim_stylus_mouse_os_init(&mc, screen)) {
		return false;
	}
	U_LOG_W("sim-stylus: mouse: gate=%s tilt=%.1f deg ramp=%lld ms double-tap=%lld ms. %s",
	        mc.gate == SIM_STYLUS_MOUSE_GATE_NONE ? "none" : "scrolllock", (double)mc.tilt_deg,
	        (long long)mc.ramp_ms, (long long)mc.double_tap_ms,
	        mc.gate == SIM_STYLUS_MOUSE_GATE_NONE ? "The mouse is the pen whenever it is over the panel."
	                                              : "Turn Scroll Lock on to pick the pen up.");
	return true;
}

static xrt_result_t
sim_stylus_plugin_probe(struct xrt_input_plugin_instance **out_inst)
{
	*out_inst = NULL;

	// Registration alone must NOT put a phantom pen on the user's hand.
	if (!sim_stylus_setting_bool("ENABLE", false)) {
		U_LOG_I(
		    "sim-stylus: declining — 'displayxr-cli input set sim-stylus ENABLE 1' (or "
		    "DXR_INPUT_SIM_STYLUS_ENABLE=1 in the hosting process) enables the simulated stylus.");
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	U_LOG_W("sim-stylus: ENABLE is set — a SIMULATED stylus will drive the dominant hand's role.");
	return XRT_SUCCESS;
}

static xrt_result_t
sim_stylus_plugin_create_devices(struct xrt_input_plugin_instance *inst,
                                 struct xrt_device **out_devices,
                                 uint32_t max_count,
                                 uint32_t *out_count)
{
	(void)inst;
	*out_count = 0;
	if (max_count < 1) {
		return XRT_ERROR_ALLOCATION;
	}

	char v[32];
	struct sim_stylus_script_config cfg;
	memset(&cfg, 0, sizeof(cfg));

	cfg.mode = SIM_STYLUS_MODE_DRAW;
	if (sim_stylus_setting("MODE", v, sizeof(v)) && !sim_stylus_parse_mode(v, &cfg.mode)) {
		U_LOG_W("sim-stylus: MODE='%s' is not draw|hover|mouse - using draw.", v);
	}
	cfg.period_s = sim_stylus_setting_float("PERIOD_S", SIM_STYLUS_PERIOD_DEFAULT_S);
	cfg.hover_height_m = sim_stylus_setting_float("HOVER_HEIGHT_M", SIM_STYLUS_HOVER_HEIGHT_DEFAULT_M);
	cfg.buttons = sim_stylus_setting_bool("BUTTONS", true);
	cfg.dock_ms = sim_stylus_setting_ms("DOCK_MS", 0);
	cfg.dock_sleep_ms = sim_stylus_setting_ms("DOCK_SLEEP_MS", SIM_STYLUS_DOCK_SLEEP_DEFAULT_MS);
	display_size(&cfg.display_width_m, &cfg.display_height_m);

	// Which hand: DOMINANT lets the builder decide from the class policy; a
	// runtime without the get_handedness slot does not resolve DOMINANT, so
	// fall back to an explicit side there.
	const int policy = host_handedness();
	enum xrt_device_type type = XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
	const char *hand_why = "default right (host has no get_handedness)";
	bool left = false;
	if (sim_stylus_setting("HAND", v, sizeof(v)) && strcmp(v, "dominant") != 0) {
		if (strcmp(v, "left") == 0) {
			left = true;
			type = XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER;
			hand_why = "HAND=left";
		} else if (strcmp(v, "right") == 0) {
			hand_why = "HAND=right";
		} else {
			U_LOG_W("sim-stylus: HAND='%s' is not dominant|right|left - using dominant.", v);
			v[0] = '\0';
		}
	} else {
		v[0] = '\0';
	}
	if (v[0] == '\0' && policy >= 0) {
#ifdef XRT_INPUT_PLUGIN_HAS_STYLUS
		type = XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER;
#endif
		left = policy == 1;
		hand_why = left ? "dominant (policy: left)" : "dominant (policy: right)";
	}
	cfg.left_handed = left;

	if (cfg.mode == SIM_STYLUS_MODE_MOUSE && !start_mouse(&cfg)) {
		U_LOG_W("sim-stylus: MODE=mouse needs Windows - using draw.");
		cfg.mode = SIM_STYLUS_MODE_DRAW;
	}

	U_LOG_W(
	    "sim-stylus: mode=%s period=%.2f s hover=%.3f m buttons=%d dock_ms=%lld dock_sleep_ms=%lld; display "
	    "%.3f x %.3f m (%s); hand %s.",
	    mode_str(cfg.mode), (double)cfg.period_s, (double)cfg.hover_height_m, cfg.buttons ? 1 : 0,
	    (long long)cfg.dock_ms, (long long)cfg.dock_sleep_ms,
	    (double)(cfg.display_width_m > 0.0f ? cfg.display_width_m : SIM_STYLUS_DISPLAY_WIDTH_FALLBACK_M),
	    (double)(cfg.display_height_m > 0.0f ? cfg.display_height_m : SIM_STYLUS_DISPLAY_HEIGHT_FALLBACK_M),
	    cfg.display_height_m > 0.0f ? "host display geometry" : "fallback", hand_why);

	struct xrt_device *pen = sim_stylus_create(type, &cfg);
	if (pen == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	g_cfg = cfg;
	g_have_cfg = true;

	out_devices[0] = pen;
	*out_count = 1;
	return XRT_SUCCESS;
}

static void
sim_stylus_plugin_destroy(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	// No instance state: the device is destroyed by the runtime.
	g_have_cfg = false;
}

static enum xrt_input_provider_presence
sim_stylus_plugin_get_presence(struct xrt_input_plugin_instance *inst)
{
	(void)inst;
	// Mouse: the pen exists while its gate is open.
	if (g_have_cfg && g_cfg.mode == SIM_STYLUS_MODE_MOUSE) {
		return sim_stylus_mouse_os_gate_open() ? XRT_INPUT_PROVIDER_PRESENCE_PRESENT
		                                       : XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
	}
	// A pen asleep in its dock is gone; otherwise it is here. Non-blocking:
	// the same pure schedule the device evaluates.
	if (g_have_cfg && !sim_stylus_script_tracked(&g_cfg, (int64_t)os_monotonic_get_ns())) {
		return XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
	}
	return XRT_INPUT_PROVIDER_PRESENCE_PRESENT;
}

static struct xrt_input_plugin_iface g_sim_stylus_iface = {
    .struct_size = sizeof(struct xrt_input_plugin_iface),
    .reserved_0 = 0,

    .id = SIM_STYLUS_ID,
    .display_name = "DisplayXR Sim Stylus",
    .vendor = "DisplayXR",
    .version = NULL,

    .probe = sim_stylus_plugin_probe,
    .create_devices = sim_stylus_plugin_create_devices,
    .destroy = sim_stylus_plugin_destroy,

    .get_presence = sim_stylus_plugin_get_presence,

    .describe_settings = sim_stylus_describe_settings,
};

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

	sim_stylus_settings_bind(host);
	g_host = host;

	*out_iface = &g_sim_stylus_iface;
	return XRT_SUCCESS;
}
