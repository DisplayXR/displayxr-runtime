// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_input's hand/controller script (#2191) — see the header.
 * @ingroup drv_sim_input
 */

#include "sim_input_hand_script.h"

#include "math/m_mathinclude.h"

#include <ctype.h>
#include <string.h>


static bool
word_eq(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
			return false;
		}
	}
	return *a == '\0' && *b == '\0';
}

bool
sim_input_parse_hands(const char *value, enum sim_input_hands_mode *out)
{
	if (value == NULL || out == NULL) {
		return false;
	}
	static const struct
	{
		const char *word;
		enum sim_input_hands_mode mode;
	} k[] = {
	    {"both", SIM_INPUT_HANDS_BOTH},
	    {"left", SIM_INPUT_HANDS_LEFT},
	    {"right", SIM_INPUT_HANDS_RIGHT},
	    {"none", SIM_INPUT_HANDS_NONE},
	};
	for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		if (word_eq(value, k[i].word)) {
			*out = k[i].mode;
			return true;
		}
	}
	return false;
}

bool
sim_input_parse_pinch(const char *value, enum sim_input_pinch_mode *out)
{
	if (value == NULL || out == NULL) {
		return false;
	}
	static const struct
	{
		const char *word;
		enum sim_input_pinch_mode mode;
	} k[] = {
	    {"wave", SIM_INPUT_PINCH_WAVE},
	    {"hold", SIM_INPUT_PINCH_HOLD},
	    {"open", SIM_INPUT_PINCH_OPEN},
	};
	for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		if (word_eq(value, k[i].word)) {
			*out = k[i].mode;
			return true;
		}
	}
	return false;
}

bool
sim_input_hand_enabled(enum sim_input_hands_mode mode, bool left)
{
	switch (mode) {
	case SIM_INPUT_HANDS_BOTH: return true;
	case SIM_INPUT_HANDS_LEFT: return left;
	case SIM_INPUT_HANDS_RIGHT: return !left;
	case SIM_INPUT_HANDS_NONE:
	default: return false;
	}
}

static float
display_height_or_fallback(float display_height_m)
{
	if (!(display_height_m > 0.0f) || !isfinite(display_height_m)) {
		return SIM_INPUT_DISPLAY_HEIGHT_FALLBACK_M;
	}
	return display_height_m;
}

struct xrt_pose
sim_input_hand_center(bool left, float distance_m, float display_height_m)
{
	if (!(distance_m > 0.0f) || !isfinite(distance_m)) {
		distance_m = SIM_INPUT_HAND_DISTANCE_DEFAULT_M;
	}
	const float h = display_height_or_fallback(display_height_m);
	const float lateral = SIM_INPUT_HAND_LATERAL_FRAC * h;
	struct xrt_pose p = {
	    .orientation = {0.0f, 0.0f, 0.0f, 1.0f},
	    .position = {left ? -lateral : lateral, SIM_INPUT_HAND_HEIGHT_FRAC * h, distance_m},
	};
	return p;
}

float
sim_input_hand_radius(float display_height_m)
{
	const float r = SIM_INPUT_CIRCLE_RADIUS_FRAC * display_height_or_fallback(display_height_m);
	return r < SIM_INPUT_CIRCLE_RADIUS_M ? r : SIM_INPUT_CIRCLE_RADIUS_M;
}

void
sim_input_hand_relation(const struct xrt_pose *center,
                        float radius_m,
                        double phase_rad,
                        int64_t at_timestamp_ns,
                        struct xrt_space_relation *out_relation)
{
	const double omega = 2.0 * M_PI / SIM_INPUT_CIRCLE_PERIOD_S; // rad/s
	double t = (double)at_timestamp_ns * 1e-9;
	double theta = omega * t + phase_rad;

	float r = radius_m;
	float c = (float)cos(theta);
	float s = (float)sin(theta);

	struct xrt_pose pose = *center;
	pose.position.x += r * c;
	pose.position.y += r * s;

	out_relation->pose = pose;
	out_relation->linear_velocity = (struct xrt_vec3){
	    (float)(-(double)r * omega) * s,
	    (float)((double)r * omega) * c,
	    0.0f,
	};
	out_relation->angular_velocity = (struct xrt_vec3)XRT_VEC3_ZERO;

	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	    XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
}

void
sim_input_hand_curls(enum sim_input_pinch_mode mode,
                     double phase_rad,
                     int64_t at_timestamp_ns,
                     struct u_hand_tracking_curl_values *out_curls)
{
	switch (mode) {
	case SIM_INPUT_PINCH_HOLD:
		*out_curls = (struct u_hand_tracking_curl_values){
		    .little = SIM_INPUT_RELAXED_CURL,
		    .ring = SIM_INPUT_RELAXED_CURL,
		    .middle = SIM_INPUT_RELAXED_CURL,
		    .index = SIM_INPUT_PINCH_HOLD_INDEX_CURL,
		    .thumb = SIM_INPUT_PINCH_HOLD_THUMB_CURL,
		};
		return;
	case SIM_INPUT_PINCH_OPEN:
		*out_curls = (struct u_hand_tracking_curl_values){
		    .little = SIM_INPUT_RELAXED_CURL,
		    .ring = SIM_INPUT_RELAXED_CURL,
		    .middle = SIM_INPUT_RELAXED_CURL,
		    .index = SIM_INPUT_RELAXED_CURL,
		    .thumb = SIM_INPUT_RELAXED_CURL,
		};
		return;
	case SIM_INPUT_PINCH_WAVE:
	default: break;
	}

	// Scripted curl wave: a full open→fist→open cycle every
	// SIM_INPUT_CURL_PERIOD_S, fingers phase-staggered so the hand visibly
	// ripples rather than pumping as one block.
	double t = (double)at_timestamp_ns * 1e-9 + phase_rad / (2.0 * M_PI) * SIM_INPUT_CIRCLE_PERIOD_S;
	const double omega = 2.0 * M_PI / SIM_INPUT_CURL_PERIOD_S;
#define SIM_CURL(i) (float)(0.5 - 0.5 * cos(omega * t - (double)(i)*0.6))
	*out_curls = (struct u_hand_tracking_curl_values){
	    .little = SIM_CURL(4),
	    .ring = SIM_CURL(3),
	    .middle = SIM_CURL(2),
	    .index = SIM_CURL(1),
	    .thumb = SIM_CURL(0),
	};
#undef SIM_CURL
}
