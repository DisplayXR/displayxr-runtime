// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_stylus's settings: the host iface's get_setting, nothing else.
 * @ingroup drv_sim_stylus
 */

#include "sim_stylus_settings.h"

#include <stdlib.h>
#include <string.h>


static const struct xrt_component_setting_desc g_descs[] = {
    {"ENABLE", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
     "probe() declines unless 1: being registered is not enough (a simulated pen would take the dominant "
     "hand's role from the qwerty fallback)"},
    {"MODE", XRT_COMPONENT_SETTING_TYPE_ENUM, "draw",
     "what the pen does: draw (the tip traces a figure-eight ON the glass with a rising-and-falling pressure "
     "ramp, lifting briefly between strokes) | hover (the same figure held HOVER_HEIGHT_M above the glass, no "
     "pressure)"},
    {"PERIOD_S", XRT_COMPONENT_SETTING_TYPE_FLOAT, "6.0", "seconds per lap of the figure-eight (one stroke)"},
    {"HOVER_HEIGHT_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.02",
     "MODE=hover: the tip's height above the glass (metres, physical)"},
    {"BUTTONS", XRT_COMPONENT_SETTING_TYPE_BOOL, "1",
     "scripted button pulses: front click every 4 s, back click every 6 s, front / back double tap every 8 / "
     "12 s, a middle-cluster force ramp every 10 s"},
    {"DOCK_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0",
     "dock the pen every n ms for n ms (docked_logitech true, no pressure or buttons, the pen standing in a "
     "dock beside the panel); 0 = never docks"},
    {"DOCK_SLEEP_MS", XRT_COMPONENT_SETTING_TYPE_INT, "1000",
     "docked this long, the pen stops tracking and the provider reports itself ABSENT (the hand role goes "
     "back to the fallback until it undocks); 0 = stays present while docked"},
    {"HAND", XRT_COMPONENT_SETTING_TYPE_ENUM, "dominant",
     "which hand role the pen takes: dominant (the runtime's InputProviders policy value Handedness: 0 = "
     "right, 1 = left) | right | left"},
};

static xrt_component_get_setting_fn_t g_get_setting = NULL;

uint32_t
sim_stylus_describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_descs;
	}
	return (uint32_t)(sizeof(g_descs) / sizeof(g_descs[0]));
}

void
sim_stylus_settings_bind(const struct xrt_input_plugin_host_iface *host)
{
	g_get_setting = NULL;
	if (host == NULL) {
		return;
	}
	if (host->struct_size >=
	    offsetof(struct xrt_input_plugin_host_iface, get_setting) + sizeof(host->get_setting)) {
		g_get_setting = host->get_setting;
	}
}

bool
sim_stylus_setting(const char *key, char *buf, size_t cap)
{
	if (buf == NULL || cap == 0) {
		return false;
	}
	buf[0] = '\0';
	if (key == NULL || g_get_setting == NULL) {
		return false;
	}
	if (g_get_setting(SIM_STYLUS_ID, key, buf, (uint32_t)cap) && buf[0] != '\0') {
		return true;
	}
	buf[0] = '\0';
	return false;
}

bool
sim_stylus_setting_bool(const char *key, bool fallback)
{
	char v[32];
	if (!sim_stylus_setting(key, v, sizeof(v))) {
		return fallback;
	}
	return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 && strcmp(v, "no") != 0 && strcmp(v, "off") != 0;
}

float
sim_stylus_setting_float(const char *key, float fallback)
{
	char v[32];
	if (!sim_stylus_setting(key, v, sizeof(v))) {
		return fallback;
	}
	char *end = NULL;
	const float f = strtof(v, &end);
	return end == v ? fallback : f;
}

int64_t
sim_stylus_setting_ms(const char *key, int64_t fallback)
{
	char v[32];
	if (!sim_stylus_setting(key, v, sizeof(v))) {
		return fallback;
	}
	char *end = NULL;
	const long long ms = strtoll(v, &end, 10);
	if (end == v) {
		return fallback;
	}
	return ms > 0 ? (int64_t)ms : 0;
}
