// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Ultraleap → display-plane mapping + settings. See the header.
 * @ingroup drv_ultraleap
 */

#include "ultraleap_map.h"

#include <stdlib.h>
#include <string.h>


/*
 *
 * Settings declaration.
 *
 */

static const struct xrt_component_setting_desc g_descs[] = {
    {"MAPPING", XRT_COMPONENT_SETTING_TYPE_ENUM, "relative",
     "relative | physical. relative: hands placed in display heights (ANCHOR_*_H, GAIN_H_PER_M), no measuring; "
     "physical: 1:1 metres from a measured mount (MOUNT_*_M)"},
    {"ANCHOR_X_H", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.0",
     "relative: Leap origin right of the display centre, in display heights"},
    {"ANCHOR_Y_H", XRT_COMPONENT_SETTING_TYPE_FLOAT, "-0.20",
     "relative: Leap origin above the display centre, in display heights (-0.20 = a fifth of the height below)"},
    {"ANCHOR_Z_H", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.0",
     "relative: Leap origin in front of the glass, in display heights (0 = on the display plane)"},
    {"GAIN_H_PER_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.81",
     "relative: display heights moved per metre of hand motion (hand size scales with it); <= 0 = default"},
    {"MOUNT_X_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.0", "physical: Leap origin right of the display centre, metres"},
    {"MOUNT_Y_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "-0.15", "physical: Leap origin above the display centre, metres"},
    {"MOUNT_Z_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.10",
     "physical: Leap origin in front of the display glass, metres"},
};

uint32_t
ul_map_describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_descs;
	}
	return (uint32_t)(sizeof(g_descs) / sizeof(g_descs[0]));
}


/*
 *
 * Config.
 *
 */

void
ul_map_config_defaults(struct ul_map_config *out)
{
	out->mode = UL_MAPPING_RELATIVE;
	out->anchor_h.x = UL_MAP_DEFAULT_ANCHOR_X_H;
	out->anchor_h.y = UL_MAP_DEFAULT_ANCHOR_Y_H;
	out->anchor_h.z = UL_MAP_DEFAULT_ANCHOR_Z_H;
	out->gain_h_per_m = UL_MAP_DEFAULT_GAIN_H_PER_M;
	out->mount_m.x = UL_MAP_DEFAULT_MOUNT_X_M;
	out->mount_m.y = UL_MAP_DEFAULT_MOUNT_Y_M;
	out->mount_m.z = UL_MAP_DEFAULT_MOUNT_Z_M;
}

static bool
read_key(xrt_component_get_setting_fn_t get_setting, const char *key, char *buf, uint32_t cap)
{
	buf[0] = '\0';
	return get_setting(UL_PROVIDER_ID, key, buf, cap) && buf[0] != '\0';
}

static void
read_float(xrt_component_get_setting_fn_t get_setting, const char *key, float *inout)
{
	char v[32];
	if (!read_key(get_setting, key, v, sizeof(v))) {
		return;
	}
	char *end = NULL;
	const float f = strtof(v, &end);
	if (end != v) {
		*inout = f;
	}
}

static bool
ieq(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++) {
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
		if (ca != cb) {
			return false;
		}
	}
	return *a == *b;
}

void
ul_map_config_from_settings(xrt_component_get_setting_fn_t get_setting, struct ul_map_config *out)
{
	ul_map_config_defaults(out);
	if (get_setting == NULL) {
		return;
	}

	char v[32];
	if (read_key(get_setting, "MAPPING", v, sizeof(v))) {
		if (ieq(v, "physical")) {
			out->mode = UL_MAPPING_PHYSICAL;
		} else {
			out->mode = UL_MAPPING_RELATIVE; // "relative" or anything unrecognised
		}
	}

	read_float(get_setting, "ANCHOR_X_H", &out->anchor_h.x);
	read_float(get_setting, "ANCHOR_Y_H", &out->anchor_h.y);
	read_float(get_setting, "ANCHOR_Z_H", &out->anchor_h.z);

	float gain = out->gain_h_per_m;
	read_float(get_setting, "GAIN_H_PER_M", &gain);
	if (gain > 0.0f) {
		out->gain_h_per_m = gain;
	}

	read_float(get_setting, "MOUNT_X_M", &out->mount_m.x);
	read_float(get_setting, "MOUNT_Y_M", &out->mount_m.y);
	read_float(get_setting, "MOUNT_Z_M", &out->mount_m.z);
}


/*
 *
 * Mapping.
 *
 */

void
ul_map_resolve(const struct ul_map_config *cfg, float display_height_m, struct ul_map *out)
{
	if (cfg->mode == UL_MAPPING_PHYSICAL) {
		out->offset_m = cfg->mount_m;
		out->scale = 1.0f;
		return;
	}

	const float h = display_height_m > 0.0f ? display_height_m : UL_MAP_FALLBACK_DISPLAY_HEIGHT_M;
	out->offset_m.x = cfg->anchor_h.x * h;
	out->offset_m.y = cfg->anchor_h.y * h;
	out->offset_m.z = cfg->anchor_h.z * h;
	out->scale = (cfg->gain_h_per_m > 0.0f ? cfg->gain_h_per_m : UL_MAP_DEFAULT_GAIN_H_PER_M) * h;
}

struct xrt_vec3
ul_map_position_mm(const struct ul_map *map, float x_mm, float y_mm, float z_mm)
{
	// Leap desktop frame and the display-plane frame share their axes
	// (+X right, +Y up, +Z toward the user/viewer): no axis swap.
	const float s = map->scale / 1000.0f;
	struct xrt_vec3 out;
	out.x = map->offset_m.x + s * x_mm;
	out.y = map->offset_m.y + s * y_mm;
	out.z = map->offset_m.z + s * z_mm;
	return out;
}

struct xrt_vec3
ul_map_velocity_mm_s(const struct ul_map *map, float x_mm_s, float y_mm_s, float z_mm_s)
{
	const float s = map->scale / 1000.0f;
	struct xrt_vec3 out;
	out.x = s * x_mm_s;
	out.y = s * y_mm_s;
	out.z = s * z_mm_s;
	return out;
}

float
ul_map_radius_from_width_mm(const struct ul_map *map, float width_mm)
{
	return (width_mm / 1000.0f) * 0.5f * map->scale;
}

struct xrt_quat
ul_map_orientation(float x, float y, float z, float w)
{
	struct xrt_quat out;
	out.x = x;
	out.y = y;
	out.z = z;
	out.w = w;
	return out;
}
