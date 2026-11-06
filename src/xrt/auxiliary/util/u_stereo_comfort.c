// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo comfort zone (XR_DXR_stereo_comfort).
 * @ingroup aux_util
 */

#include "u_stereo_comfort.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

void
u_stereo_comfort_compute(enum u_stereo_comfort_zone zone,
                         float custom_diopters,
                         float viewer_m,
                         float user_scale,
                         struct u_stereo_comfort *out)
{
	if (!isfinite(viewer_m) || viewer_m <= 0.0f) {
		viewer_m = U_STEREO_COMFORT_DEFAULT_VIEWER_M;
	}
	if (!isfinite(user_scale) || user_scale <= 0.0f) {
		user_scale = 1.0f;
	}
	if (user_scale > 1.0f) {
		user_scale = 1.0f;
	}
	if (zone == U_STEREO_COMFORT_ZONE_CUSTOM && (!isfinite(custom_diopters) || custom_diopters <= 0.0f)) {
		zone = U_STEREO_COMFORT_ZONE_SHIBATA;
	}
	if (zone != U_STEREO_COMFORT_ZONE_PERCIVAL && zone != U_STEREO_COMFORT_ZONE_CUSTOM) {
		zone = U_STEREO_COMFORT_ZONE_SHIBATA;
	}

	float front;
	float behind;
	if (zone == U_STEREO_COMFORT_ZONE_SHIBATA) {
		// Shibata et al. 2011: near vergence Dv/1.035 + 0.626, far Dv/1.129 - 0.442.
		const double dv = 1.0 / (double)viewer_m;
		front = (float)(dv / 1.035 + 0.626 - dv);
		const double far = dv / 1.129 - 0.442;
		behind = far > 0.0 ? (float)(dv - far) : INFINITY;
	} else if (zone == U_STEREO_COMFORT_ZONE_PERCIVAL) {
		front = behind = 1.0f / 3.0f;
	} else {
		front = behind = custom_diopters;
	}

	front *= user_scale;
	behind *= user_scale; // inf * scale stays inf

	const float k = U_STEREO_COMFORT_EYE_SEPARATION_M * viewer_m;
	out->zone = zone;
	out->viewer_m = viewer_m;
	out->front_diopters = front;
	out->behind_diopters = behind;
	out->front_disparity_m = k * front;
	out->behind_disparity_m = isinf(behind) ? INFINITY : k * behind;
	out->user_scale = user_scale;
}

bool
u_stereo_comfort_parse_zone(const char *s, enum u_stereo_comfort_zone *out_zone, float *out_custom_diopters)
{
	if (s == NULL || s[0] == '\0') {
		return false;
	}
	if (strcasecmp(s, "shibata") == 0) {
		*out_zone = U_STEREO_COMFORT_ZONE_SHIBATA;
		return true;
	}
	if (strcasecmp(s, "percival") == 0) {
		*out_zone = U_STEREO_COMFORT_ZONE_PERCIVAL;
		return true;
	}
	char *end = NULL;
	const float d = strtof(s, &end);
	if (end != s && *end == '\0' && isfinite(d) && d > 0.0f) {
		*out_zone = U_STEREO_COMFORT_ZONE_CUSTOM;
		*out_custom_diopters = d;
		return true;
	}
	return false;
}
