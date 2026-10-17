// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  touch_input's settings shim: host iface only, no legacy aliases.
 * @ingroup drv_touch_input
 */

#include "touch_input_settings.h"

#include <stdlib.h>
#include <string.h>


static const struct xrt_component_setting_desc g_descs[] = {
    {"ENABLE", XRT_COMPONENT_SETTING_TYPE_BOOL, "1 on Android, 0 elsewhere",
     "probe() claims the system only when 1. Android: the touch screen is always there, so the default is on "
     "and 0 withdraws the provider; desktop: nothing feeds pointer events yet, so it is off unless asked for"},
    {"CONTROLLER", XRT_COMPONENT_SETTING_TYPE_BOOL, "1",
     "also publish the emulated khr/simple_controller (aim ray from the nominal viewer through the finger, "
     "SELECT = tap / press-and-hold); 0 = navigation device only"},
    {"SENSITIVITY", XRT_COMPONENT_SETTING_TYPE_FLOAT, "1.0",
     "multiplies the orbit rate: 1.0 = a full-width drag is half a turn, a full-height drag a quarter turn"},
};

static xrt_component_get_setting_fn_t g_get_setting = NULL;

uint32_t
touch_input_describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_descs;
	}
	return (uint32_t)(sizeof(g_descs) / sizeof(g_descs[0]));
}

void
touch_input_settings_bind(const struct xrt_input_plugin_host_iface *host)
{
	g_get_setting = NULL;
	if (host == NULL) {
		return;
	}
	// Carved from reserved[]: an older runtime leaves it NULL, and a runtime
	// older than the slot has a struct_size that does not even cover it.
	if (host->struct_size >=
	    offsetof(struct xrt_input_plugin_host_iface, get_setting) + sizeof(host->get_setting)) {
		g_get_setting = host->get_setting;
	}
}

bool
touch_input_setting(const char *key, char *buf, size_t cap)
{
	if (buf == NULL || cap == 0) {
		return false;
	}
	buf[0] = '\0';
	if (key == NULL || g_get_setting == NULL) {
		return false;
	}
	if (g_get_setting(TOUCH_INPUT_ID, key, buf, (uint32_t)cap) && buf[0] != '\0') {
		return true;
	}
	buf[0] = '\0';
	return false;
}

static bool
str_ieq(const char *a, const char *b)
{
	for (;; a++, b++) {
		const char ca = (char)((*a >= 'A' && *a <= 'Z') ? *a + ('a' - 'A') : *a);
		const char cb = (char)((*b >= 'A' && *b <= 'Z') ? *b + ('a' - 'A') : *b);
		if (ca != cb) {
			return false;
		}
		if (ca == '\0') {
			return true;
		}
	}
}

bool
touch_input_setting_on(const char *key, bool dflt)
{
	char v[32];
	if (!touch_input_setting(key, v, sizeof(v))) {
		return dflt;
	}
	return !(strcmp(v, "0") == 0 || str_ieq(v, "false") || str_ieq(v, "no") || str_ieq(v, "off"));
}

float
touch_input_setting_float(const char *key, float dflt)
{
	char v[32];
	if (!touch_input_setting(key, v, sizeof(v))) {
		return dflt;
	}
	char *end = NULL;
	const double d = strtod(v, &end);
	if (end == v || d <= 0.0) {
		return dflt;
	}
	return (float)d;
}
