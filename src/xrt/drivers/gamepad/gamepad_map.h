// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The gamepad provider's mapping policy — ONE table, pure functions.
 *
 * Everything that decides *what a pad input means* lives here and nowhere
 * else: the deadzone, which stick drives which navigation channel, which
 * buttons stand in for the emulated controllers' select / menu, and which
 * button recenters the rig. The hub (`gamepad_hub.c`) and the devices only
 * ever call into this file with a @ref gamepad_state and a
 * @ref gamepad_map_table; they never look at a button bit themselves.
 *
 * That split is deliberate: a per-title bindings mechanism (#1381) replaces
 * the TABLE, not the devices — hand a different @ref gamepad_map_table to
 * the hub and the whole provider follows. Until then
 * @ref gamepad_map_default_table is the policy.
 *
 * This file depends on nothing but `xrt_defines.h` (for `xrt_pose`) and
 * `<math.h>`, so `tests/tests_gamepad_map.cpp` compiles it straight in.
 *
 * Frames (ADR-034 Amendment 4): the navigation pose N(t) is ABSOLUTE in the
 * provider's own frame F — right-handed, +Y up, −Z forward, metres — and the
 * composer puts the alignment on the LEFT (`rig = T_world_F ∘ N`), so a step
 * along N's own −Z is "forward as the rig currently faces". The integrator
 * therefore moves the position in the yaw frame of the pose it integrates.
 *
 * @author David Fattal
 * @ingroup drv_gamepad
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Pad state — what every backend produces.
 *
 */

//! Button bits of @ref gamepad_state::buttons (Xbox-layout names; other pads are mapped onto them).
enum gamepad_button
{
	GAMEPAD_BTN_A = 1u << 0,
	GAMEPAD_BTN_B = 1u << 1,
	GAMEPAD_BTN_X = 1u << 2,
	GAMEPAD_BTN_Y = 1u << 3,
	GAMEPAD_BTN_SHOULDER_LEFT = 1u << 4,
	GAMEPAD_BTN_SHOULDER_RIGHT = 1u << 5,
	GAMEPAD_BTN_THUMB_LEFT = 1u << 6,  //!< left stick click
	GAMEPAD_BTN_THUMB_RIGHT = 1u << 7, //!< right stick click
	GAMEPAD_BTN_MENU = 1u << 8,        //!< "start" / "menu" / "options"
	GAMEPAD_BTN_VIEW = 1u << 9,        //!< "back" / "view" / "share"
	GAMEPAD_BTN_DPAD_UP = 1u << 10,
	GAMEPAD_BTN_DPAD_DOWN = 1u << 11,
	GAMEPAD_BTN_DPAD_LEFT = 1u << 12,
	GAMEPAD_BTN_DPAD_RIGHT = 1u << 13,
	GAMEPAD_BTN_GUIDE = 1u << 14, //!< the big logo button (not exposed on the OpenXR profile)
};

/*!
 * One sample of a pad, in the backend-neutral convention: sticks are
 * −1..+1 with +X right and +Y UP (XInput's convention; GameController's
 * matches), triggers 0..1, buttons a bit mask.
 */
struct gamepad_state
{
	bool connected;
	uint32_t buttons;
	float left_x, left_y;
	float right_x, right_y;
	float left_trigger, right_trigger;
	//! Monotonic time the backend read this sample.
	int64_t timestamp_ns;
};

//! The analogue axes a navigation channel may be bound to.
enum gamepad_axis
{
	GAMEPAD_AXIS_LEFT_X = 0,
	GAMEPAD_AXIS_LEFT_Y,
	GAMEPAD_AXIS_RIGHT_X,
	GAMEPAD_AXIS_RIGHT_Y,
	GAMEPAD_AXIS_LEFT_TRIGGER,
	GAMEPAD_AXIS_RIGHT_TRIGGER,
	GAMEPAD_AXIS_COUNT,
};


/*
 *
 * The table.
 *
 */

//! Navigation channels: the rig's own axes (rig-local, −Z forward) and its two look angles.
enum gamepad_nav_channel
{
	GAMEPAD_NAV_STRAFE = 0, //!< +X right, m/s at full deflection = nav_speed_mps
	GAMEPAD_NAV_FORWARD,    //!< −Z forward, m/s
	GAMEPAD_NAV_UP,         //!< +Y up (world up, not pitched), m/s
	GAMEPAD_NAV_YAW,        //!< turn, deg/s at full deflection = nav_turn_dps; positive = turn LEFT (about +Y)
	GAMEPAD_NAV_PITCH,      //!< look, deg/s; positive = look UP
	GAMEPAD_NAV_CHANNEL_COUNT,
};

//! One axis feeding one channel; several bindings to a channel SUM (the two triggers share NAV_UP).
struct gamepad_axis_binding
{
	enum gamepad_nav_channel channel;
	enum gamepad_axis axis;
	float scale; //!< sign and gain applied to the deadzoned axis
};

//! The digital actions the table assigns buttons to.
enum gamepad_map_action
{
	GAMEPAD_ACT_LEFT_SELECT = 0, //!< emulated left `khr/simple_controller` select
	GAMEPAD_ACT_LEFT_MENU,
	GAMEPAD_ACT_RIGHT_SELECT,
	GAMEPAD_ACT_RIGHT_MENU,
	GAMEPAD_ACT_RECENTER, //!< `XRT_INPUT_GENERIC_NAVIGATION_RECENTER` (rising edge)
	GAMEPAD_ACT_COUNT,
};

//! A button action is pressed when ANY button of its mask is down.
struct gamepad_button_binding
{
	enum gamepad_map_action action;
	uint32_t buttons;
};

/*!
 * The whole policy. @ref gamepad_map_default_table is the in-tree one; a
 * per-title mechanism (#1381) supplies another of the same shape.
 */
struct gamepad_map_table
{
	const struct gamepad_axis_binding *axes;
	uint32_t axis_count;
	const struct gamepad_button_binding *buttons;
	uint32_t button_count;
};

//! The numeric knobs (the ADR-054 settings `DEADZONE`, `NAV_SPEED`, `NAV_TURN_SPEED`, `INVERT_LOOK_Y`).
struct gamepad_map_config
{
	float deadzone;      //!< radial, 0..0.9 of full deflection; below it a stick reads 0
	float nav_speed_mps; //!< translation at full deflection
	float nav_turn_dps;  //!< yaw / pitch at full deflection, degrees per second
	bool invert_look_y;  //!< flip the pitch channel
};

//! What a provider that reads no settings uses.
#define GAMEPAD_MAP_CONFIG_DEFAULT                                                                                     \
	{                                                                                                              \
		.deadzone = 0.15f, .nav_speed_mps = 1.0f, .nav_turn_dps = 90.0f, .invert_look_y = false,               \
	}

//! The pitch clamp, degrees either side of level — the rig never flips over.
#define GAMEPAD_MAP_PITCH_LIMIT_DEG 80.0f

extern const struct gamepad_map_table gamepad_map_default_table;


/*
 *
 * Derived quantities.
 *
 */

//! Rig-local velocity: metres per second on the rig's axes, degrees per second on the look angles.
struct gamepad_nav_velocity
{
	float strafe_mps;  //!< +X
	float forward_mps; //!< along −Z
	float up_mps;      //!< world +Y
	float yaw_dps;     //!< positive = left turn (right-handed about +Y)
	float pitch_dps;   //!< positive = look up
};

/*!
 * The integrated navigation pose N(t) in F, kept as yaw/pitch so the clamp
 * and the "move in the yaw frame" rule are exact; @ref gamepad_map_nav_pose_to_xrt
 * turns it into a quaternion.
 */
struct gamepad_nav_pose
{
	float x, y, z;
	float yaw_rad;
	float pitch_rad;
};

#define GAMEPAD_NAV_POSE_IDENTITY                                                                                      \
	{                                                                                                              \
		0.0f, 0.0f, 0.0f, 0.0f, 0.0f                                                                           \
	}


/*
 *
 * Functions.
 *
 */

/*!
 * Radial deadzone with rescale: a stick inside the dead circle reads (0, 0);
 * outside it the magnitude is remapped so the first live reading is just
 * above 0 (no jump at the edge) and full deflection stays 1. The direction
 * is preserved. NaN / out-of-range inputs are clamped first.
 */
void
gamepad_map_apply_deadzone(float x, float y, float deadzone, float *out_x, float *out_y);

//! A deadzoned axis value from @p state (sticks go through @ref gamepad_map_apply_deadzone, triggers are clamped).
float
gamepad_map_axis_value(const struct gamepad_state *state, enum gamepad_axis axis, float deadzone);

//! Stick → rig-local velocity per @p table and @p cfg. A disconnected state yields all zeros.
void
gamepad_map_nav_velocity(const struct gamepad_state *state,
                         const struct gamepad_map_table *table,
                         const struct gamepad_map_config *cfg,
                         struct gamepad_nav_velocity *out);

/*!
 * Advance @p pose by @p vel over @p dt_s. Translation is applied in the
 * pose's YAW frame (so forward is where the rig faces, but pitch does not
 * tilt the motion — a flight stick, not a plane), vertical along world +Y;
 * pitch is clamped to ±@ref GAMEPAD_MAP_PITCH_LIMIT_DEG, yaw wraps.
 */
void
gamepad_map_integrate(struct gamepad_nav_pose *pose, const struct gamepad_nav_velocity *vel, double dt_s);

//! Quaternion form: yaw about +Y applied after pitch about +X (`q = q_yaw * q_pitch`).
void
gamepad_map_nav_pose_to_xrt(const struct gamepad_nav_pose *pose, struct xrt_pose *out);

//! Is @p action pressed in @p state under @p table? False for an unbound action.
bool
gamepad_map_action_pressed(const struct gamepad_state *state,
                           const struct gamepad_map_table *table,
                           enum gamepad_map_action action);

/*!
 * Is the user TOUCHING the pad in @p state? Any button down, any stick
 * outside the radial @p deadzone, or any trigger past
 * @ref GAMEPAD_MAP_TRIGGER_ACTIVITY. A disconnected state is never active.
 * This is what "last-used input wins" keys on: the hub timestamps it and
 * presence follows the timestamp (@ref gamepad_map_recently_active).
 */
bool
gamepad_map_has_activity(const struct gamepad_state *state, float deadzone);

//! Trigger pull counted as activity (0..1) — above a resting trigger's noise.
#define GAMEPAD_MAP_TRIGGER_ACTIVITY 0.05f


/*
 *
 * Touch emulation (`EMULATE_PROFILE` = touch).
 *
 */

/*!
 * One emulated `oculus/touch_controller` hand as the pad drives it. VR
 * sculpting / painting tools (Substance 3D Modeler and the like) need the
 * full Touch layout and hands that MOVE, so in this mode each stick moves
 * its own hand instead of the rig:
 *
 * - trigger = that side's trigger, squeeze = that side's shoulder;
 * - lower / upper face button = X / Y on the left hand, A / B on the right;
 * - menu = the pad's Menu button (left hand only, as on a real Touch);
 * - the app-facing thumbstick comes from the D-pad: the right hand's by
 *   default, the left hand's while View is held;
 * - the hand moves in the display plane with its stick, and along depth
 *   (stick up = into the screen) while that stick is clicked in.
 */
struct gamepad_touch_hand
{
	float trigger; //!< 0..1
	float squeeze; //!< 0..1
	bool lower;    //!< X (left) / A (right)
	bool upper;    //!< Y (left) / B (right)
	bool menu;     //!< left hand only
	float thumbstick_x, thumbstick_y;
	struct xrt_vec3 velocity_mps; //!< rig-local: +X right, +Y up, +Z toward the viewer
};

/*!
 * Map @p state to hand @p is_left. Stick motion goes through the radial
 * @p deadzone and is scaled to @p hand_speed_mps at full deflection. A
 * disconnected state yields all zeros.
 */
void
gamepad_map_touch_hand(const struct gamepad_state *state,
                       bool is_left,
                       float deadzone,
                       float hand_speed_mps,
                       struct gamepad_touch_hand *out);

/*!
 * Recenter is a TAP of View: pressed and released within
 * @ref GAMEPAD_MAP_TAP_MAX_NS with no D-pad direction pressed meanwhile
 * (View + D-pad steers the left thumbstick and is not a tap).
 */
#define GAMEPAD_MAP_TAP_MAX_NS (300LL * 1000 * 1000)

//! View-tap detector state; zero-initialise.
struct gamepad_tap
{
	int64_t down_ns; //!< when View went down, 0 = up
	bool spoiled;    //!< a D-pad direction was pressed while View was down
};

//! Feed one sample; true on the release that completes a tap.
bool
gamepad_map_view_tap(struct gamepad_tap *tap, const struct gamepad_state *state, int64_t now_ns);

/*!
 * The idle rule behind presence: true when @p idle_timeout_s <= 0 (the
 * connected-only behaviour), else when @p last_activity_ns is non-zero and
 * no more than @p idle_timeout_s before @p now_ns. A pad that was never
 * touched since it was plugged in (last_activity_ns == 0) is NOT active,
 * so plugging in alone takes no role — touching the pad does.
 */
bool
gamepad_map_recently_active(int64_t now_ns, int64_t last_activity_ns, float idle_timeout_s);

#ifdef __cplusplus
}
#endif
