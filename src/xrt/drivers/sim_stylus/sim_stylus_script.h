// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_stylus's SCRIPT: where the simulated pen is, how hard it presses
 *         and which buttons are down, as pure functions of the requested
 *         timestamp.
 *
 * Split out of the device so the script can be unit-tested with no device,
 * no plug-in and no display (`tests/tests_sim_stylus_script.cpp`).
 *
 * **Frame.** Every pose is in physical display-plane metres: origin at the
 * display centre, +X right, +Y up, +Z toward the viewer — the frame of an
 * `XRT_TRACKING_TYPE_RIG_LOCAL_PHYSICAL` origin (input-provider-discovery.md
 * §4b.4a). z = 0 is the glass.
 *
 * **Pen axes.** The tip pose has its origin at the point of the tip, -Z out
 * of the tip along the pen's axis (from the back of the pen toward the tip)
 * and +Y along the input cluster's normal (up the pen's top side). The aim
 * pose equals the tip pose; the grip pose has the same orientation, moved
 * @ref SIM_STYLUS_GRIP_OFFSET_M back up the barrel.
 *
 * **Modes.**
 *   - `draw`: the tip traces a figure-eight ON the glass (z = 0) for the first
 *     @ref SIM_STYLUS_DRAW_CONTACT_FRAC of each period with a rising-and-falling
 *     pressure ramp, then lifts @ref SIM_STYLUS_LIFT_HEIGHT_M off the glass
 *     (force 0) for the rest — one stroke per period.
 *   - `hover`: the same figure, held `HOVER_HEIGHT_M` above the glass, force 0.
 *
 * **Buttons** (`BUTTONS` = 1): short pulses on fixed schedules — front click,
 * back click, front / back double tap and a middle-cluster force ramp — so an
 * app sees every edge with no hardware.
 *
 * **Dock** (`DOCK_MS` = n > 0): a square wave of period 2n. While docked the
 * pen rests in a dock beside the panel's lower-right corner, `docked` is true,
 * force and buttons are 0, and after `DOCK_SLEEP_MS` (if > 0) the pen stops
 * tracking and the provider reports itself ABSENT — the hot-plug path. The
 * dock state is therefore visible to an app for `DOCK_SLEEP_MS` before the pen
 * "goes to sleep", as a real stylus reports docking just before it stops.
 *
 * @ingroup drv_sim_stylus
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Display size when the host publishes none (metres).
#define SIM_STYLUS_DISPLAY_WIDTH_FALLBACK_M 0.60f
#define SIM_STYLUS_DISPLAY_HEIGHT_FALLBACK_M 0.34f
//! Default `PERIOD_S`: one lap of the figure-eight.
#define SIM_STYLUS_PERIOD_DEFAULT_S 6.0f
//! Default `HOVER_HEIGHT_M`.
#define SIM_STYLUS_HOVER_HEIGHT_DEFAULT_M 0.02f
//! Default `DOCK_SLEEP_MS`.
#define SIM_STYLUS_DOCK_SLEEP_DEFAULT_MS 1000
//! Fraction of a draw period the tip is on the glass.
#define SIM_STYLUS_DRAW_CONTACT_FRAC 0.85f
//! Height of the pen-up gap between draw strokes (metres).
#define SIM_STYLUS_LIFT_HEIGHT_M 0.015f
//! Lowest tip force while in contact (the ramp runs MIN..1..MIN).
#define SIM_STYLUS_FORCE_MIN 0.15f
//! Figure-eight half-extent, as fractions of the display width / height.
#define SIM_STYLUS_FIGURE_W_FRAC 0.25f
#define SIM_STYLUS_FIGURE_H_FRAC 0.20f
//! Distance from the tip back to the grip pose (metres).
#define SIM_STYLUS_GRIP_OFFSET_M 0.06f
//! Pen length (metres): where the back of the pen is, for an app that draws the barrel.
#define SIM_STYLUS_LENGTH_M 0.16f

enum sim_stylus_mode
{
	SIM_STYLUS_MODE_DRAW = 0,
	SIM_STYLUS_MODE_HOVER = 1,
};

//! What the script needs, resolved from the provider's settings once.
struct sim_stylus_script_config
{
	enum sim_stylus_mode mode;
	//! Physical display size (metres); <= 0 = the fallbacks above.
	float display_width_m;
	float display_height_m;
	//! `HOVER_HEIGHT_M` (metres); < 0 = the default.
	float hover_height_m;
	//! `PERIOD_S`; <= 0 = the default.
	float period_s;
	//! Which hand holds the pen: mirrors the barrel's tilt (a right hand's pen leans right).
	bool left_handed;
	//! `BUTTONS`: run the button schedule.
	bool buttons;
	//! `DOCK_MS`: half-period of the dock square wave; 0 = never docks.
	int64_t dock_ms;
	//! `DOCK_SLEEP_MS`: docked this long, the pen stops tracking; 0 = stays tracked while docked.
	int64_t dock_sleep_ms;
};

//! The pen at one instant.
struct sim_stylus_state
{
	struct xrt_pose tip;  //!< == aim
	struct xrt_pose grip; //!< tip pose moved back up the barrel
	struct xrt_vec3 tip_linear_velocity;
	float tip_force;    //!< 0..1
	float middle_force; //!< 0..1
	bool front_click;
	bool front_double_tap;
	bool back_click;
	bool back_double_tap;
	bool system_click;
	bool docked;
	//! The tip is on the glass (z = 0, force > 0).
	bool contact;
	//! The pen tracks (poses valid). False only while asleep in the dock.
	bool tracked;
};

/*!
 * Evaluate the script at @p t_ns (any clock; the provider passes the
 * monotonic time it is asked about, so prediction is exact by construction).
 */
void
sim_stylus_script_eval(const struct sim_stylus_script_config *cfg, int64_t t_ns, struct sim_stylus_state *out);

//! Is the pen tracked at @p t_ns? (== `eval().tracked`, without the poses.)
bool
sim_stylus_script_tracked(const struct sim_stylus_script_config *cfg, int64_t t_ns);

//! Parse a `MODE` value ("draw" | "hover"); false leaves @p out unchanged.
bool
sim_stylus_parse_mode(const char *s, enum sim_stylus_mode *out);

#ifdef __cplusplus
}
#endif
