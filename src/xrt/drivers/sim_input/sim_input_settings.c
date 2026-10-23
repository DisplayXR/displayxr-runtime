// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_input's settings shim: host iface first, legacy env aliases
 *         second (one release). See the header. This is the one file under
 *         drivers/sim_input/ allowed to call getenv (lint.yml guard).
 * @ingroup drv_sim_input
 */

#include "sim_input_settings.h"

#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>


/*
 *
 * The declaration (describe_settings) + the legacy aliases.
 *
 */

static const struct xrt_component_setting_desc g_descs[] = {
    {"ENABLE", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
     "probe() declines unless 1: being registered is not enough (simulated controllers would displace the "
     "qwerty fallback). Replaces DXR_SIM_INPUT=1"},
    {"NAV", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
     "also create the scripted navigation device (rig role, ADR-034 Amendment 4) and put the controllers on "
     "display-plane-relative poses. Replaces DXR_SIM_INPUT_NAV=1"},
    {"NAV_HOLD_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0",
     "every n ms the navigation pose reports invalid for 500 ms (composer hold / re-align); 0 = never"},
    {"NAV_RECENTER_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0", "fire a scripted recenter every n ms; 0 = never"},
    {"HANDS", XRT_COMPONENT_SETTING_TYPE_ENUM, "both",
     "which simulated hands track: both | left | right | none (a hand left out stays a device but is never "
     "active)"},
    {"PINCH", XRT_COMPONENT_SETTING_TYPE_ENUM, "wave",
     "what the fingers do: wave (open-fist curl wave, 3 s) | hold (thumb + index tips held pinched, other "
     "fingers relaxed: with HANDS=right a sustained one-hand drag) | open (never pinches)"},
    {"HAND_DISTANCE_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.25",
     "how far in front of the display glass the hands' 4 cm circles are centred (metres; the hands sit 12 cm "
     "left / right of the display centre)"},
    {"HAND_SCALE_H_PER_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "2.5",
     "the hand's own size, in display heights per metre of real hand (joint offsets about the wrist/palm and "
     "joint radii), the Ultraleap relative mapping's gain: a 10 cm hand shows as 0.25 x the display height. "
     "Display-metric, never the window. <= 0 = 1:1 metres. The circle centres and radius do not change"},
};

//! KEY -> the DXR_SIM_INPUT* variable it replaces (one release; removed with the next major cleanup).
static const struct
{
	const char *key;
	const char *legacy_env;
} g_aliases[] = {
    {"ENABLE", "DXR_SIM_INPUT"},
    {"NAV", "DXR_SIM_INPUT_NAV"},
    {"NAV_HOLD_MS", "DXR_SIM_INPUT_NAV_HOLD_MS"},
    {"NAV_RECENTER_MS", "DXR_SIM_INPUT_NAV_RECENTER_MS"},
};
#define ALIAS_COUNT (sizeof(g_aliases) / sizeof(g_aliases[0]))

static xrt_component_get_setting_fn_t g_get_setting = NULL;
static bool g_alias_warned[ALIAS_COUNT];

uint32_t
sim_input_describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_descs;
	}
	return (uint32_t)(sizeof(g_descs) / sizeof(g_descs[0]));
}


/*
 *
 * Lookup.
 *
 */

void
sim_input_settings_bind(const struct xrt_input_plugin_host_iface *host)
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

static void
copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

bool
sim_input_setting(const char *key, char *buf, size_t cap)
{
	if (buf == NULL || cap == 0) {
		return false;
	}
	buf[0] = '\0';
	if (key == NULL) {
		return false;
	}
	if (g_get_setting != NULL && g_get_setting(SIM_INPUT_ID, key, buf, (uint32_t)cap) && buf[0] != '\0') {
		return true;
	}
	buf[0] = '\0';
	// Legacy alias: the pre-N3c DXR_SIM_INPUT* switches, read from THIS
	// process's environment (the only tier that ever existed for them).
	for (size_t i = 0; i < ALIAS_COUNT; i++) {
		if (strcmp(g_aliases[i].key, key) != 0) {
			continue;
		}
		const char *v = getenv(g_aliases[i].legacy_env);
		if (v == NULL || v[0] == '\0') {
			return false;
		}
		if (!g_alias_warned[i]) {
			g_alias_warned[i] = true;
			U_LOG_W(
			    "sim-input: %s is deprecated — use 'displayxr-cli input set sim-input %s %s' (or "
			    "DXR_INPUT_SIM_INPUT_%s in the hosting process's environment); the alias goes away one "
			    "release after N3c (ADR-054 decision 7).",
			    g_aliases[i].legacy_env, key, v, key);
		}
		copy_str(buf, cap, v);
		return true;
	}
	return false;
}

bool
sim_input_setting_on(const char *key)
{
	char v[32];
	if (!sim_input_setting(key, v, sizeof(v))) {
		return false;
	}
	return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 && strcmp(v, "no") != 0 && strcmp(v, "off") != 0;
}

float
sim_input_setting_float(const char *key, float fallback)
{
	char v[32];
	if (!sim_input_setting(key, v, sizeof(v))) {
		return fallback;
	}
	char *end = NULL;
	const float f = strtof(v, &end);
	if (end == v) {
		return fallback;
	}
	return f;
}

int64_t
sim_input_setting_ms(const char *key)
{
	char v[32];
	if (!sim_input_setting(key, v, sizeof(v))) {
		return 0;
	}
	const long long ms = strtoll(v, NULL, 10);
	return ms > 0 ? (int64_t)ms : 0;
}
