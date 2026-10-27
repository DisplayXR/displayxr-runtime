// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The gamepad mapping table and its pure functions. See the header.
 * @ingroup drv_gamepad
 */

#include "gamepad_map.h"

#include <math.h>

//! Local so this unit needs neither m_mathinclude.h nor MSVC's _USE_MATH_DEFINES.
#define GAMEPAD_PI 3.14159265358979323846


/*
 *
 * The default table.
 *
 */

/*!
 * Left stick = translate, right stick = look, triggers = up / down. Stick
 * +Y is "up" on the stick, which is "forward" for the left stick and "look
 * up" for the right one. Right on the right stick must turn the rig RIGHT,
 * i.e. a NEGATIVE rotation about +Y, hence the −1 on yaw.
 */
// clang-format off
static const struct gamepad_axis_binding g_default_axes[] = {
    {GAMEPAD_NAV_STRAFE,  GAMEPAD_AXIS_LEFT_X,        1.0f},
    {GAMEPAD_NAV_FORWARD, GAMEPAD_AXIS_LEFT_Y,        1.0f},
    {GAMEPAD_NAV_UP,      GAMEPAD_AXIS_RIGHT_TRIGGER, 1.0f},
    {GAMEPAD_NAV_UP,      GAMEPAD_AXIS_LEFT_TRIGGER, -1.0f},
    {GAMEPAD_NAV_YAW,     GAMEPAD_AXIS_RIGHT_X,      -1.0f},
    {GAMEPAD_NAV_PITCH,   GAMEPAD_AXIS_RIGHT_Y,       1.0f},
};
// clang-format on

/*!
 * Shoulders are the "select" of the emulated `khr/simple_controller` pair
 * (the triggers are spent on navigation); X / A double as left / right
 * select for a one-handed grip. View / Menu are the two "menu"s. Recenter
 * is the right stick click — it never conflicts with a look, and both
 * stick clicks are otherwise unused by the emulation.
 */
// clang-format off
static const struct gamepad_button_binding g_default_buttons[] = {
    {GAMEPAD_ACT_LEFT_SELECT,  GAMEPAD_BTN_SHOULDER_LEFT | GAMEPAD_BTN_X},
    {GAMEPAD_ACT_LEFT_MENU,    GAMEPAD_BTN_VIEW},
    {GAMEPAD_ACT_RIGHT_SELECT, GAMEPAD_BTN_SHOULDER_RIGHT | GAMEPAD_BTN_A},
    {GAMEPAD_ACT_RIGHT_MENU,   GAMEPAD_BTN_MENU},
    {GAMEPAD_ACT_RECENTER,     GAMEPAD_BTN_THUMB_RIGHT},
};
// clang-format on

const struct gamepad_map_table gamepad_map_default_table = {
    .axes = g_default_axes,
    .axis_count = sizeof(g_default_axes) / sizeof(g_default_axes[0]),
    .buttons = g_default_buttons,
    .button_count = sizeof(g_default_buttons) / sizeof(g_default_buttons[0]),
};


/*
 *
 * Helpers.
 *
 */

static float
clamp_unit(float v)
{
	if (!(v == v)) { // NaN
		return 0.0f;
	}
	if (v > 1.0f) {
		return 1.0f;
	}
	if (v < -1.0f) {
		return -1.0f;
	}
	return v;
}

static float
clamp_01(float v)
{
	if (!(v == v)) {
		return 0.0f;
	}
	if (v > 1.0f) {
		return 1.0f;
	}
	if (v < 0.0f) {
		return 0.0f;
	}
	return v;
}


/*
 *
 * 'Exported' functions.
 *
 */

void
gamepad_map_apply_deadzone(float x, float y, float deadzone, float *out_x, float *out_y)
{
	x = clamp_unit(x);
	y = clamp_unit(y);
	float dz = deadzone;
	if (!(dz == dz) || dz < 0.0f) {
		dz = 0.0f;
	}
	if (dz > 0.9f) {
		dz = 0.9f;
	}

	float mag = sqrtf(x * x + y * y);
	if (mag <= dz || mag <= 0.0f) {
		*out_x = 0.0f;
		*out_y = 0.0f;
		return;
	}
	// Remap (dz, 1] -> (0, 1], keep the direction, cap the magnitude at 1
	// (a diagonal on a square-gated stick can exceed the unit circle).
	float live = (mag - dz) / (1.0f - dz);
	if (live > 1.0f) {
		live = 1.0f;
	}
	float k = live / mag;
	*out_x = x * k;
	*out_y = y * k;
}

float
gamepad_map_axis_value(const struct gamepad_state *state, enum gamepad_axis axis, float deadzone)
{
	float x = 0.0f, y = 0.0f;
	switch (axis) {
	case GAMEPAD_AXIS_LEFT_X:
	case GAMEPAD_AXIS_LEFT_Y:
		gamepad_map_apply_deadzone(state->left_x, state->left_y, deadzone, &x, &y);
		return axis == GAMEPAD_AXIS_LEFT_X ? x : y;
	case GAMEPAD_AXIS_RIGHT_X:
	case GAMEPAD_AXIS_RIGHT_Y:
		gamepad_map_apply_deadzone(state->right_x, state->right_y, deadzone, &x, &y);
		return axis == GAMEPAD_AXIS_RIGHT_X ? x : y;
	case GAMEPAD_AXIS_LEFT_TRIGGER: return clamp_01(state->left_trigger);
	case GAMEPAD_AXIS_RIGHT_TRIGGER: return clamp_01(state->right_trigger);
	default: return 0.0f;
	}
}

void
gamepad_map_nav_velocity(const struct gamepad_state *state,
                         const struct gamepad_map_table *table,
                         const struct gamepad_map_config *cfg,
                         struct gamepad_nav_velocity *out)
{
	out->strafe_mps = 0.0f;
	out->forward_mps = 0.0f;
	out->up_mps = 0.0f;
	out->yaw_dps = 0.0f;
	out->pitch_dps = 0.0f;

	if (state == NULL || !state->connected || table == NULL || cfg == NULL) {
		return;
	}

	float sum[GAMEPAD_NAV_CHANNEL_COUNT] = {0};
	for (uint32_t i = 0; i < table->axis_count; i++) {
		const struct gamepad_axis_binding *b = &table->axes[i];
		if ((unsigned)b->channel >= GAMEPAD_NAV_CHANNEL_COUNT || (unsigned)b->axis >= GAMEPAD_AXIS_COUNT) {
			continue;
		}
		sum[b->channel] += b->scale * gamepad_map_axis_value(state, b->axis, cfg->deadzone);
	}
	for (int c = 0; c < GAMEPAD_NAV_CHANNEL_COUNT; c++) {
		sum[c] = clamp_unit(sum[c]);
	}

	const float look_y = cfg->invert_look_y ? -1.0f : 1.0f;
	out->strafe_mps = sum[GAMEPAD_NAV_STRAFE] * cfg->nav_speed_mps;
	out->forward_mps = sum[GAMEPAD_NAV_FORWARD] * cfg->nav_speed_mps;
	out->up_mps = sum[GAMEPAD_NAV_UP] * cfg->nav_speed_mps;
	out->yaw_dps = sum[GAMEPAD_NAV_YAW] * cfg->nav_turn_dps;
	out->pitch_dps = sum[GAMEPAD_NAV_PITCH] * cfg->nav_turn_dps * look_y;
}

void
gamepad_map_integrate(struct gamepad_nav_pose *pose, const struct gamepad_nav_velocity *vel, double dt_s)
{
	if (!(dt_s > 0.0)) {
		return;
	}
	const double deg = GAMEPAD_PI / 180.0;

	// Angles first: a long tick (a stall) must not turn into a long leap.
	pose->yaw_rad = (float)fmod((double)pose->yaw_rad + (double)vel->yaw_dps * deg * dt_s, 2.0 * GAMEPAD_PI);
	double pitch = (double)pose->pitch_rad + (double)vel->pitch_dps * deg * dt_s;
	const double limit = GAMEPAD_MAP_PITCH_LIMIT_DEG * deg;
	if (pitch > limit) {
		pitch = limit;
	} else if (pitch < -limit) {
		pitch = -limit;
	}
	pose->pitch_rad = (float)pitch;

	// Then translate in the yaw frame: forward = (−sin yaw, 0, −cos yaw),
	// right = (cos yaw, 0, −sin yaw) — a positive yaw turns the rig left
	// (right-handed about +Y), so its forward swings toward −X.
	const double s = sin((double)pose->yaw_rad);
	const double c = cos((double)pose->yaw_rad);
	const double fwd = (double)vel->forward_mps * dt_s;
	const double right = (double)vel->strafe_mps * dt_s;
	pose->x += (float)(right * c - fwd * s);
	pose->z += (float)(-right * s - fwd * c);
	pose->y += (float)((double)vel->up_mps * dt_s);
}

void
gamepad_map_nav_pose_to_xrt(const struct gamepad_nav_pose *pose, struct xrt_pose *out)
{
	// q = q_yaw(+Y) * q_pitch(+X): pitch in the rig's own frame, then yaw it round.
	const double hy = 0.5 * (double)pose->yaw_rad;
	const double hp = 0.5 * (double)pose->pitch_rad;
	const double sy = sin(hy), cy = cos(hy);
	const double sp = sin(hp), cp = cos(hp);

	// (0, sy, 0, cy) * (sp, 0, 0, cp), Hamilton product with w last.
	out->orientation.x = (float)(cy * sp);
	out->orientation.y = (float)(sy * cp);
	out->orientation.z = (float)(sy * sp);
	out->orientation.w = (float)(cy * cp);
	out->position.x = pose->x;
	out->position.y = pose->y;
	out->position.z = pose->z;
}

bool
gamepad_map_has_activity(const struct gamepad_state *state, float deadzone)
{
	if (state == NULL || !state->connected) {
		return false;
	}
	if (state->buttons != 0) {
		return true;
	}
	float x = 0.0f, y = 0.0f;
	gamepad_map_apply_deadzone(state->left_x, state->left_y, deadzone, &x, &y);
	if (x != 0.0f || y != 0.0f) {
		return true;
	}
	gamepad_map_apply_deadzone(state->right_x, state->right_y, deadzone, &x, &y);
	if (x != 0.0f || y != 0.0f) {
		return true;
	}
	return clamp_01(state->left_trigger) > GAMEPAD_MAP_TRIGGER_ACTIVITY ||
	       clamp_01(state->right_trigger) > GAMEPAD_MAP_TRIGGER_ACTIVITY;
}

bool
gamepad_map_recently_active(int64_t now_ns, int64_t last_activity_ns, float idle_timeout_s)
{
	if (!(idle_timeout_s > 0.0f)) {
		return true; // 0 (or NaN / negative): connected-only, no idle rule.
	}
	if (last_activity_ns <= 0) {
		return false; // Never touched since plug-in.
	}
	const double idle_s = (double)(now_ns - last_activity_ns) * 1e-9;
	return idle_s <= (double)idle_timeout_s;
}

bool
gamepad_map_action_pressed(const struct gamepad_state *state,
                           const struct gamepad_map_table *table,
                           enum gamepad_map_action action)
{
	if (state == NULL || !state->connected || table == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < table->button_count; i++) {
		const struct gamepad_button_binding *b = &table->buttons[i];
		if (b->action == action && (state->buttons & b->buttons) != 0) {
			return true;
		}
	}
	return false;
}

void
gamepad_map_touch_hand(const struct gamepad_state *state,
                       bool is_left,
                       float deadzone,
                       float hand_speed_mps,
                       struct gamepad_touch_hand *out)
{
	const struct gamepad_touch_hand zero = {0};
	*out = zero;
	if (state == NULL || !state->connected) {
		return;
	}
	const uint32_t b = state->buttons;

	out->trigger = clamp_01(is_left ? state->left_trigger : state->right_trigger);
	out->squeeze = (b & (is_left ? GAMEPAD_BTN_SHOULDER_LEFT : GAMEPAD_BTN_SHOULDER_RIGHT)) != 0 ? 1.0f : 0.0f;
	out->lower = (b & (is_left ? GAMEPAD_BTN_X : GAMEPAD_BTN_A)) != 0;
	out->upper = (b & (is_left ? GAMEPAD_BTN_Y : GAMEPAD_BTN_B)) != 0;
	out->menu = is_left && (b & GAMEPAD_BTN_MENU) != 0;

	// The D-pad is the right thumbstick, or the left one while View is held.
	const bool view = (b & GAMEPAD_BTN_VIEW) != 0;
	if (view == is_left) {
		out->thumbstick_x = ((b & GAMEPAD_BTN_DPAD_RIGHT) != 0 ? 1.0f : 0.0f) -
		                    ((b & GAMEPAD_BTN_DPAD_LEFT) != 0 ? 1.0f : 0.0f);
		out->thumbstick_y =
		    ((b & GAMEPAD_BTN_DPAD_UP) != 0 ? 1.0f : 0.0f) - ((b & GAMEPAD_BTN_DPAD_DOWN) != 0 ? 1.0f : 0.0f);
	}

	float x = 0.0f, y = 0.0f;
	if (is_left) {
		gamepad_map_apply_deadzone(state->left_x, state->left_y, deadzone, &x, &y);
	} else {
		gamepad_map_apply_deadzone(state->right_x, state->right_y, deadzone, &x, &y);
	}
	float speed = hand_speed_mps;
	if (!(speed == speed) || speed < 0.0f) {
		speed = 0.0f;
	}
	const bool depth = (b & (is_left ? GAMEPAD_BTN_THUMB_LEFT : GAMEPAD_BTN_THUMB_RIGHT)) != 0;
	if (depth) {
		// Stick up pushes the hand into the screen (−Z, away from the viewer).
		out->velocity_mps.z = -y * speed;
	} else {
		out->velocity_mps.x = x * speed;
		out->velocity_mps.y = y * speed;
	}
}

bool
gamepad_map_view_tap(struct gamepad_tap *tap, const struct gamepad_state *state, int64_t now_ns)
{
	const uint32_t dpad =
	    GAMEPAD_BTN_DPAD_UP | GAMEPAD_BTN_DPAD_DOWN | GAMEPAD_BTN_DPAD_LEFT | GAMEPAD_BTN_DPAD_RIGHT;
	const bool view = state != NULL && state->connected && (state->buttons & GAMEPAD_BTN_VIEW) != 0;

	if (view) {
		if (tap->down_ns == 0) {
			tap->down_ns = now_ns > 0 ? now_ns : 1;
			tap->spoiled = false;
		}
		if ((state->buttons & dpad) != 0) {
			tap->spoiled = true;
		}
		return false;
	}
	if (tap->down_ns == 0) {
		return false;
	}
	const bool is_tap = !tap->spoiled && now_ns - tap->down_ns <= GAMEPAD_MAP_TAP_MAX_NS;
	tap->down_ns = 0;
	tap->spoiled = false;
	return is_tap;
}
