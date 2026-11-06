// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo comfort zone (XR_DXR_stereo_comfort): the diopter bounds for a
 *         viewer at a distance, the same zones the web SDK's comfortZone() states.
 *
 * Pure: no locks, no clock, no GPU. The state tracker fills XrStereoComfortDXR
 * from it; the zone arithmetic matches displayxr-web js/inline3d-depth-budget.js.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_STEREO_COMFORT_EYE_SEPARATION_M 0.063f
#define U_STEREO_COMFORT_DEFAULT_VIEWER_M 0.6f

enum u_stereo_comfort_zone
{
	U_STEREO_COMFORT_ZONE_SHIBATA = 1,
	U_STEREO_COMFORT_ZONE_PERCIVAL = 2,
	U_STEREO_COMFORT_ZONE_CUSTOM = 3,
};

struct u_stereo_comfort
{
	enum u_stereo_comfort_zone zone;
	float viewer_m;           //!< V used
	float front_diopters;     //!< after the user scale
	float behind_diopters;    //!< after the user scale; may be +inf
	float front_disparity_m;  //!< e * V * front
	float behind_disparity_m; //!< e * V * behind; may be +inf
	float user_scale;         //!< applied, (0, 1]
};

/*!
 * The comfort zone for a viewer at @p viewer_m.
 *
 * @param zone            SHIBATA, PERCIVAL or CUSTOM (anything else: SHIBATA)
 * @param custom_diopters CUSTOM only: symmetric bound (<= 0 or non-finite: SHIBATA)
 * @param viewer_m        viewing distance, metres (<= 0 or non-finite: 0.6)
 * @param user_scale      the user's depth preference, scales both bounds
 *                        (clamped to (0, 1]; <= 0 or non-finite: 1)
 */
void
u_stereo_comfort_compute(enum u_stereo_comfort_zone zone,
                         float custom_diopters,
                         float viewer_m,
                         float user_scale,
                         struct u_stereo_comfort *out);

/*!
 * Parse a zone setting: "shibata", "percival", or a positive number of
 * diopters (CUSTOM). Returns false (and leaves the outputs alone) otherwise.
 */
bool
u_stereo_comfort_parse_zone(const char *s, enum u_stereo_comfort_zone *out_zone, float *out_custom_diopters);

#ifdef __cplusplus
}
#endif
