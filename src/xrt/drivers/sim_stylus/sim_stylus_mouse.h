// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_stylus `MODE=mouse`: the desktop mouse drives the pen.
 *
 * Two halves:
 *
 *   - **Pure logic** (`sim_stylus_mouse.c`, no OS calls, unit-tested by
 *     `tests/tests_sim_stylus_mouse.cpp`): picking the panel's monitor,
 *     mapping a cursor pixel onto the glass in metres, and the state machine
 *     that turns one sample of mouse buttons + keys into a pen
 *     (@ref sim_stylus_state) — tip on the glass with a force ramp while the
 *     left button is held, hover height stepped by PageUp / PageDown, the
 *     cluster buttons, double taps, the dock toggle, the gate.
 *   - **The OS sampler** (`sim_stylus_mouse_os.c`): reads the global cursor
 *     and the async key state (no hooks, no focus, no window) under a pinned
 *     per-monitor-v2 DPI context, so it measures physical pixels whatever the
 *     host process declared (`docs/reference/dpi-awareness.md`). Windows
 *     only; elsewhere @ref sim_stylus_mouse_os_init fails and the provider
 *     falls back to `MODE=draw`.
 *
 * **Controls** (only while the gate is open — `GATE=scrolllock`: Scroll Lock
 * toggled on; `GATE=none`: always):
 *
 * | input                  | pen                                                |
 * |------------------------|----------------------------------------------------|
 * | cursor on the panel    | tip at that point of the glass (x, y)              |
 * | left button held       | tip ON the glass, tip force ramps 0 -> 1 over `RAMP_MS` |
 * | left button up         | tip hovers at the hover height                     |
 * | PageUp / PageDown      | hover height +/- 5 mm (Shift: 20 mm), 0 .. 150 mm  |
 * | right button           | cluster front click; a second press within the system double-click time = front double tap
 * | | middle button          | cluster back click (double press = back double tap) | | Ctrl or X1 button held | cluster
 * middle force ramps 0 -> 1 over `RAMP_MS`   | | X2 button              | system click | | Home                   |
 * toggle docked                                      |
 *
 * The mouse is only READ: clicks and keys also reach whatever window is under
 * the cursor / focused, exactly as without the provider.
 *
 * **Out of range.** Gate closed: the pen is gone (untracked, and the plug-in
 * reports ABSENT, so the hand role falls back). Gate open but the cursor on
 * another monitor: the pen is out of the tracking volume — untracked, but the
 * provider stays PRESENT (a real pen leaving the volume does not unplug), so
 * the role does not flap while the user reaches for another screen.
 *
 * @ingroup drv_sim_stylus
 */

#pragma once

#include "xrt/xrt_defines.h"

#include "sim_stylus_script.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Default `RAMP_MS`: tip / middle force 0 -> 1.
#define SIM_STYLUS_MOUSE_RAMP_DEFAULT_MS 300
//! Default `TILT_DEG`: the barrel's lean away from the glass normal.
#define SIM_STYLUS_MOUSE_TILT_DEFAULT_DEG 30.0f
//! Hover-height steps (metres): PageUp / PageDown, and with Shift.
#define SIM_STYLUS_MOUSE_HEIGHT_STEP_M 0.005f
#define SIM_STYLUS_MOUSE_HEIGHT_STEP_BIG_M 0.02f
//! Hover-height range (metres).
#define SIM_STYLUS_MOUSE_HEIGHT_MAX_M 0.15f
//! Holding PageUp / PageDown repeats after this, every REPEAT_MS.
#define SIM_STYLUS_MOUSE_KEY_DELAY_MS 400
#define SIM_STYLUS_MOUSE_KEY_REPEAT_MS 80
//! How long a double tap stays true, so a frame-rate poll always sees it.
#define SIM_STYLUS_MOUSE_DOUBLE_TAP_PULSE_MS 150
//! Fallback double-tap window when the OS reports none.
#define SIM_STYLUS_MOUSE_DOUBLE_TAP_DEFAULT_MS 500

enum sim_stylus_mouse_gate
{
	//! The pen exists only while Scroll Lock is toggled on.
	SIM_STYLUS_MOUSE_GATE_SCROLLLOCK = 0,
	//! Always on: the mouse IS the pen whenever it is over the panel.
	SIM_STYLUS_MOUSE_GATE_NONE = 1,
};

//! What the mouse mode needs, resolved from the settings once.
struct sim_stylus_mouse_config
{
	//! Physical display size (metres); <= 0 = the script fallbacks.
	float display_width_m;
	float display_height_m;
	//! Initial hover height (metres; `HOVER_HEIGHT_M`); < 0 = the default.
	float hover_height_m;
	//! `TILT_DEG`: lean of the barrel from the glass normal, toward the viewer's holding hand and down.
	float tilt_deg;
	//! `RAMP_MS`: 0 = force jumps straight to 1.
	int64_t ramp_ms;
	//! Two presses closer than this are a double tap (the OS double-click time).
	int64_t double_tap_ms;
	//! Mirrors the barrel's lean (a left hand's pen leans left).
	bool left_handed;
	enum sim_stylus_mouse_gate gate;
};

//! One sample of the desktop: where the cursor is and what is held.
struct sim_stylus_mouse_sample
{
	//! The gate is open (Scroll Lock on, or `GATE=none`).
	bool gate_open;
	//! The cursor is on the panel's monitor; @ref x_m / @ref y_m are valid.
	bool on_panel;
	//! The cursor's point of the glass (display-plane metres, centre origin, +Y up).
	float x_m;
	float y_m;

	bool left;   //!< tip down
	bool right;  //!< cluster front
	bool middle; //!< cluster back
	bool x1;     //!< middle force (as Ctrl)
	bool x2;     //!< system
	bool ctrl;   //!< middle force
	bool shift;  //!< bigger height steps
	bool page_up;
	bool page_down;
	bool home; //!< dock toggle
};

//! A double-tap detector for one button.
struct sim_stylus_mouse_tap
{
	bool was_down;
	bool have_press;
	int64_t last_press_ns;
	int64_t pulse_until_ns;
};

//! A key that steps on press and auto-repeats while held.
struct sim_stylus_mouse_repeat
{
	bool was_down;
	int64_t next_ns;
};

//! The mouse pen's state between samples.
struct sim_stylus_mouse_machine
{
	float hover_height_m;
	bool docked;
	bool home_was_down;

	bool tip_down;
	int64_t tip_down_ns;
	bool middle_down;
	int64_t middle_down_ns;

	struct sim_stylus_mouse_tap front;
	struct sim_stylus_mouse_tap back;
	struct sim_stylus_mouse_repeat up;
	struct sim_stylus_mouse_repeat down;

	//! Velocity estimate from successive tip positions.
	bool have_prev;
	struct xrt_vec3 prev_tip;
	int64_t prev_ns;
	struct xrt_vec3 velocity;
};

//! Reset @p m to the configured hover height, undocked, nothing held.
void
sim_stylus_mouse_machine_init(struct sim_stylus_mouse_machine *m, const struct sim_stylus_mouse_config *cfg);

/*!
 * Advance @p m by one @p sample taken at @p now_ns (monotonic, non-decreasing)
 * and write the pen at that instant to @p out.
 */
void
sim_stylus_mouse_machine_step(struct sim_stylus_mouse_machine *m,
                              const struct sim_stylus_mouse_config *cfg,
                              const struct sim_stylus_mouse_sample *sample,
                              int64_t now_ns,
                              struct sim_stylus_state *out);


/*
 *
 * Monitor selection and pixel mapping.
 *
 */

//! Largest monitor count the sampler considers.
#define SIM_STYLUS_MOUSE_MAX_MONITORS 16

//! One monitor, in physical virtual-desktop pixels.
struct sim_stylus_mouse_monitor
{
	int32_t left;
	int32_t top;
	int32_t width;
	int32_t height;
	//! Physical size the OS reports (EDID), 0 = unknown.
	int32_t width_mm;
	int32_t height_mm;
	bool is_primary;
	//! GDI name, e.g. `\\.\DISPLAY2`.
	char name[32];
};

//! Which rule chose the panel's monitor (logged once).
enum sim_stylus_mouse_pick_rule
{
	SIM_STYLUS_MOUSE_PICK_NONE = 0,
	//! `SCREEN` named it (index or device name).
	SIM_STYLUS_MOUSE_PICK_SETTING,
	//! Its physical size (mm) matches the display's, within 10 %, and so does its aspect.
	SIM_STYLUS_MOUSE_PICK_SIZE,
	//! Its pixel aspect matches the display's physical aspect (within 3 %); ties: the primary.
	SIM_STYLUS_MOUSE_PICK_ASPECT,
	//! Nothing matched: the primary monitor.
	SIM_STYLUS_MOUSE_PICK_PRIMARY,
};

const char *
sim_stylus_mouse_pick_rule_str(enum sim_stylus_mouse_pick_rule rule);

/*!
 * Pick the monitor the 3D panel is on.
 *
 * @param screen `SCREEN` setting: "" / NULL = automatic; a decimal index into
 *               @p mons; or a device name (`\\.\DISPLAY2`, or just `DISPLAY2`,
 *               case-insensitive). A setting that names no monitor is ignored
 *               (@p out_setting_unmatched set) and the automatic rules run.
 * @return the index into @p mons, or -1 when @p count is 0.
 */
int32_t
sim_stylus_mouse_pick_monitor(const struct sim_stylus_mouse_monitor *mons,
                              uint32_t count,
                              float display_width_m,
                              float display_height_m,
                              const char *screen,
                              enum sim_stylus_mouse_pick_rule *out_rule,
                              bool *out_setting_unmatched);

/*!
 * Map a cursor pixel to the point of the glass under it: display-plane
 * metres, origin at the display centre, +X right, +Y up. The whole monitor
 * rect spans the whole @p width_m x @p height_m display.
 *
 * @return false (outputs untouched) when the pixel is not on @p mon.
 */
bool
sim_stylus_mouse_map_pixel(const struct sim_stylus_mouse_monitor *mon,
                           int32_t x,
                           int32_t y,
                           float width_m,
                           float height_m,
                           float *out_x_m,
                           float *out_y_m);

//! Parse a `GATE` value ("scrolllock" | "none"); false leaves @p out unchanged.
bool
sim_stylus_mouse_parse_gate(const char *s, enum sim_stylus_mouse_gate *out);


/*
 *
 * Scroll Lock toggle tracking.
 *
 */

/*!
 * The Scroll Lock toggle as this process can see it. `GetKeyState`'s toggle
 * bit follows the calling THREAD's input queue, which a provider thread with
 * no focused window may never pump, so the tracker also flips on every
 * key-down edge of the async (physical) state and re-syncs whenever the
 * queue's toggle bit changes.
 */
struct sim_stylus_mouse_toggle
{
	bool init;
	bool on;
	bool key_was_down;
	bool last_queue_toggle;
};

//! Feed one sample; returns the tracked toggle.
bool
sim_stylus_mouse_toggle_update(struct sim_stylus_mouse_toggle *t, bool queue_toggle, bool key_down);


/*
 *
 * The OS sampler (sim_stylus_mouse_os.c). One per process; thread-safe.
 *
 */

/*!
 * Start the sampler with @p cfg and the `SCREEN` setting @p screen. Logs the
 * monitors and the one it picked (WARN, once).
 *
 * @return false on a platform without a sampler (anything but Windows).
 */
bool
sim_stylus_mouse_os_init(const struct sim_stylus_mouse_config *cfg, const char *screen);

//! Sample the desktop at @p now_ns and write the pen to @p out (untracked before init).
void
sim_stylus_mouse_os_eval(int64_t now_ns, struct sim_stylus_state *out);

//! Is the gate open right now? (Presence; cheap, non-blocking.)
bool
sim_stylus_mouse_os_gate_open(void);

//! The OS double-click time in ms (0 when unknown).
int64_t
sim_stylus_mouse_os_double_click_ms(void);

#ifdef __cplusplus
}
#endif
