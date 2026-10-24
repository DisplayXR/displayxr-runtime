// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  gamepad settings shim: host iface only. See the header.
 * @ingroup drv_gamepad
 */

#include "gamepad_settings.h"

#include <stdlib.h>
#include <string.h>


static const struct xrt_component_setting_desc g_descs[] = {
    {"DEADZONE", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.15",
     "radial stick deadzone as a fraction of full deflection (0..0.9); below it a stick reads 0"},
    {"NAV_SPEED", XRT_COMPONENT_SETTING_TYPE_FLOAT, "1.0",
     "rig translation speed at full stick / trigger deflection, metres per second (0.01..20)"},
    {"NAV_TURN_SPEED", XRT_COMPONENT_SETTING_TYPE_FLOAT, "90",
     "rig yaw / pitch rate at full right-stick deflection, degrees per second (1..720)"},
    {"NAV", XRT_COMPONENT_SETTING_TYPE_BOOL, "1",
     "create the navigation device: a present pad then drives the rig role (left stick = move, right stick = "
     "look, triggers = up / down, right-stick click = recenter). 0 = the pad is only a gamepad"},
    {"EMULATE_CONTROLLERS", XRT_COMPONENT_SETTING_TYPE_BOOL, "1",
     "also publish an emulated left / right khr/simple_controller pair driven by the pad (shoulders / X / A = "
     "select, View / Menu = menu), so handle apps get xrSyncActions controllers; a present pad then holds the "
     "hand roles over qwerty. 0 = hands untouched"},
    {"EMULATE_PROFILE", XRT_COMPONENT_SETTING_TYPE_ENUM, "simple",
     "which controllers EMULATE_CONTROLLERS publishes: simple = fixed khr/simple_controller pair; touch = an "
     "oculus/touch_controller pair whose hands the sticks move (stick click + stick = depth, triggers / shoulders "
     "= trigger / squeeze, A B X Y = A B X Y, D-pad = right thumbstick or left with View held, View tap = "
     "recenter hands) for VR creation tools. touch turns NAV off unless NAV is set"},
    {"HAND_SPEED", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.25",
     "touch profile: hand speed at full stick deflection, metres per second (0.01..5)"},
    {"HAND_HOME", XRT_COMPONENT_SETTING_TYPE_STRING, "0.08,-0.04,0.12",
     "touch profile: the right hand's rest point x,y,z in display-plane metres (+Z toward the viewer); the left "
     "hand mirrors x"},
    {"INVERT_LOOK_Y", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "flip the right stick's pitch direction"},
    {"IDLE_TIMEOUT_S", XRT_COMPONENT_SETTING_TYPE_FLOAT, "20",
     "last-used input wins: the pad holds its roles (gamepad, rig, hands) only while connected AND touched within "
     "this many seconds; untouched longer, the floor (fly camera / qwerty) takes them back until the next touch. "
     "0 = connected-only (a plugged-in pad holds them indefinitely)"},
};

static xrt_component_get_setting_fn_t g_get_setting = NULL;

uint32_t
gamepad_describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_descs;
	}
	return (uint32_t)(sizeof(g_descs) / sizeof(g_descs[0]));
}

void
gamepad_settings_bind(const struct xrt_input_plugin_host_iface *host)
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
gamepad_setting(const char *key, char *buf, size_t cap)
{
	if (buf == NULL || cap == 0) {
		return false;
	}
	buf[0] = '\0';
	if (key == NULL || g_get_setting == NULL) {
		return false;
	}
	if (g_get_setting(GAMEPAD_ID, key, buf, (uint32_t)cap) && buf[0] != '\0') {
		return true;
	}
	buf[0] = '\0';
	return false;
}

bool
gamepad_setting_on(const char *key, bool default_on)
{
	char v[32];
	if (!gamepad_setting(key, v, sizeof(v))) {
		return default_on;
	}
	return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 && strcmp(v, "no") != 0 && strcmp(v, "off") != 0;
}

float
gamepad_setting_float(const char *key, float default_value, float lo, float hi)
{
	char v[32];
	if (!gamepad_setting(key, v, sizeof(v))) {
		return default_value;
	}
	char *end = NULL;
	double d = strtod(v, &end);
	if (end == v || !(d == d)) {
		return default_value;
	}
	if (d < (double)lo) {
		d = lo;
	}
	if (d > (double)hi) {
		d = hi;
	}
	return (float)d;
}
