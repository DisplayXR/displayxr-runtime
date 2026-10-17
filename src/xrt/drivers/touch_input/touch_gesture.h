// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Touch gesture recogniser + the rig/aim math it drives — platform-free.
 *
 * THE MAPPING TABLE (the one place it lives; the spec quotes it):
 *
 * | Gesture                              | Effect                                                          |
 * |--------------------------------------|-----------------------------------------------------------------|
 * | one-finger drag (past the slop)      | ORBIT the rig about the pivot: yaw from dx, pitch from dy       |
 * | two-finger pinch                     | DOLLY: pivot distance d = d_at_pinch_start * dist0 / dist       |
 * | two-finger drag (centroid moves)     | PAN: the pivot slides in the rig's own XY plane, 1:1 with the   |
 * |                                      | fingers in display metres                                        |
 * | tap (up within TAP_MAX, inside slop) | SELECT click: a pulse of SELECT_PULSE on the emulated controller |
 * | press-and-hold (still past TAP_MAX)  | SELECT held until the finger lifts; the aim ray follows it, and |
 * |                                      | the finger's motion is NOT an orbit (a select-drag)             |
 * | double tap                           | RECENTER: orbit/pan/dolly reset + the durable recenter level    |
 *
 * Frames. The navigation pose `N(t)` is ABSOLUTE in this provider's own frame
 * F (ADR-034 Amendment 4): F's origin is the rig at rest, -Z its forward, and
 * the orbit PIVOT starts `d0` metres straight ahead — the nominal viewer-to-
 * panel distance, so an untouched scene orbits about the display plane, which
 * is where its content sits. The runtime aligns F to the world once and
 * composes; the provider never sees the world, the rig_initial, or the eyes.
 *
 *   N = T(pivot) ∘ R ∘ T(0, 0, d),   R = Ry(yaw) ∘ Rx(pitch)
 *
 * Signs, chosen so the content feels GRABBED: dragging right spins the
 * content's front toward +X (the camera orbits to -X), dragging down tips its
 * top toward the viewer (the camera rises), a two-finger drag carries the
 * content with the fingers, pinching out brings it closer.
 *
 * The emulated controller's AIM ray starts at the nominal viewer and passes
 * through the finger's point on the display plane, in the RIG-LOCAL frame
 * (display centre, +X right, +Y up, +Z toward the viewer, metres). The surface
 * is mapped onto the whole panel — exact for a fullscreen `_hosted` surface,
 * an approximation for a sub-panel window (the provider never learns where a
 * window sits; see @ref xrt_input_pointer_event).
 *
 * Everything here is a pure function of the events fed in and the timestamps
 * handed to @ref touch_gesture_tick — no clock, no logging, no allocation, no
 * aux dependency — so it compiles straight into a ctest (tests_touch_gesture)
 * with no device, no loader and no display.
 *
 * @ingroup drv_touch_input
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_input_plugin.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Pointers tracked at once (Android reports at most 10 on every shipping panel).
#define TOUCH_GESTURE_MAX_POINTERS 10

//! A full-width drag turns the rig half a revolution (times `sensitivity`).
#define TOUCH_GESTURE_YAW_RAD_PER_WIDTH 3.14159265358979323846
//! A full-height drag tips the rig a quarter revolution (times `sensitivity`).
#define TOUCH_GESTURE_PITCH_RAD_PER_HEIGHT 1.57079632679489661923
//! Pitch never reaches the pole: the orbit stays invertible.
#define TOUCH_GESTURE_PITCH_LIMIT_RAD 1.4835298641951802 // 85 degrees
//! Dolly bounds, as multiples of the rest distance d0.
#define TOUCH_GESTURE_DOLLY_MIN_FACTOR 0.1
#define TOUCH_GESTURE_DOLLY_MAX_FACTOR 10.0
//! How long a tap's SELECT stays pressed: longer than one xrSyncActions interval at any frame rate.
#define TOUCH_GESTURE_SELECT_PULSE_NS (120LL * 1000 * 1000)

enum touch_gesture_mode
{
	TOUCH_GESTURE_MODE_IDLE = 0,       //!< no finger down
	TOUCH_GESTURE_MODE_PENDING = 1,    //!< one finger down, undecided (tap / hold / orbit)
	TOUCH_GESTURE_MODE_ORBIT = 2,      //!< one finger past the slop within TAP_MAX
	TOUCH_GESTURE_MODE_SELECT_HOLD = 3, //!< one finger still past TAP_MAX: select held, aim follows
	TOUCH_GESTURE_MODE_TWO_FINGER = 4, //!< two (or more) fingers: pinch + pan
};

struct touch_gesture_config
{
	float display_width_m;  //!< panel width, for pan metres/pixel and the aim plane; 0 = 0.30
	float display_height_m; //!< panel height; 0 = display_width_m * 9/16
	float viewer_x_m;       //!< nominal viewer in the rig-local frame (aim-ray origin)
	float viewer_y_m;
	float viewer_z_m;       //!< nominal viewer distance; also the rest pivot distance d0; 0 = 0.50
	float sensitivity;      //!< multiplies the orbit rates; 0 = 1.0
	int64_t tap_max_ns;     //!< a finger still for longer than this is a hold; 0 = 300 ms
	int64_t double_tap_ns;  //!< two tap-UPs closer than this recenter; 0 = 350 ms
	float slop_px;          //!< motion within this is "still"; 0 = 2 % of the shorter surface edge, min 8 px
};

struct touch_gesture_pointer
{
	uint32_t id;
	bool down;
	float x;
	float y;
};

struct touch_gesture
{
	struct touch_gesture_config cfg;

	struct touch_gesture_pointer pointers[TOUCH_GESTURE_MAX_POINTERS];
	//! Down pointers in the order they went down (indices into `pointers`), -1 = empty.
	int order[TOUCH_GESTURE_MAX_POINTERS];
	uint32_t down_count;

	uint32_t surface_w;
	uint32_t surface_h;

	enum touch_gesture_mode mode;

	//! Where and when the primary finger went down (tap / hold / slop decisions).
	float down_x;
	float down_y;
	int64_t down_ns;
	//! Last position the orbit consumed (so a MOVE is a delta from here).
	float last_x;
	float last_y;

	//! Two-finger reference: distance and centroid when the second finger landed (or last consumed).
	float pinch_dist0;
	float pinch_d0; //!< d at the moment the pinch started
	float pan_cx;
	float pan_cy;

	//! The rig state in F.
	float yaw;
	float pitch;
	float d;         //!< pivot distance
	float pivot[3];  //!< pivot in F

	//! Tap bookkeeping.
	int64_t last_tap_up_ns; //!< for double-tap detection; 0 = none pending
	int64_t select_until_ns; //!< a tap's SELECT pulse end; 0 = none
	//! Durable recenter level: the time of the most recent double tap, 0 = never.
	int64_t recenter_ns;

	//! Last known primary-finger position, kept across UP so the aim ray holds for the select pulse.
	float aim_x;
	float aim_y;
	bool aim_valid;
};

//! Fill @p cfg with the documented defaults (every zero above resolved).
void
touch_gesture_config_defaults(struct touch_gesture_config *cfg);

//! Reset @p g to rest with @p cfg (zeros in @p cfg take the defaults; NULL = all defaults).
void
touch_gesture_init(struct touch_gesture *g, const struct touch_gesture_config *cfg);

/*!
 * Feed one pointer event. Order matters; feed them as they were produced.
 * A CANCEL lifts every finger without a tap.
 */
void
touch_gesture_feed(struct touch_gesture *g, const struct xrt_input_pointer_event *ev);

/*!
 * Advance time to @p now_ns: promotes a still PENDING finger to SELECT_HOLD
 * past `tap_max_ns`, and expires a tap pulse. Call once per poll, after the
 * events of that poll were fed.
 */
void
touch_gesture_tick(struct touch_gesture *g, int64_t now_ns);

//! `N(t)`: the absolute rig pose in F for the current state.
void
touch_gesture_nav_pose(const struct touch_gesture *g, struct xrt_pose *out_pose);

//! Is SELECT pressed at @p now_ns (a hold, or a tap's pulse still running)?
bool
touch_gesture_select(const struct touch_gesture *g, int64_t now_ns);

/*!
 * The emulated controller's poses in the RIG-LOCAL frame: @p out_aim at the
 * nominal viewer pointing through the finger's point on the display plane,
 * @p out_grip at that point with the same orientation. Returns false (outputs
 * untouched) while no finger has ever touched the surface.
 */
bool
touch_gesture_aim(const struct touch_gesture *g, struct xrt_pose *out_aim, struct xrt_pose *out_grip);

//! Time of the most recent double-tap recenter, 0 = never. Monotonic; never cleared.
int64_t
touch_gesture_recenter_timestamp(const struct touch_gesture *g);

//! Current recogniser mode (diagnostics / tests).
enum touch_gesture_mode
touch_gesture_mode(const struct touch_gesture *g);

#ifdef __cplusplus
}
#endif
