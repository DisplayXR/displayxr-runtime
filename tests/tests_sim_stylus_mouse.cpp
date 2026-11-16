// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_stylus MODE=mouse, the pure half: the panel's monitor, the
 *         cursor pixel -> glass mapping, the pen state machine (tip force
 *         ramp, hover-height steps and clamp, cluster buttons, double taps,
 *         dock, gate, out of range) and the Scroll Lock toggle tracker.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_defines.h"

#include "sim_stylus_mouse.h"

#include <cmath>
#include <cstdint>
#include <cstring>


namespace {

constexpr int64_t kNsPerMs = 1000000LL;

sim_stylus_mouse_config
base_config()
{
	sim_stylus_mouse_config c = {};
	c.display_width_m = 0.60f;
	c.display_height_m = 0.34f;
	c.hover_height_m = 0.02f;
	c.tilt_deg = 30.0f;
	c.ramp_ms = 300;
	c.double_tap_ms = 500;
	c.left_handed = false;
	c.gate = SIM_STYLUS_MOUSE_GATE_SCROLLLOCK;
	return c;
}

sim_stylus_mouse_sample
on_panel(float x, float y)
{
	sim_stylus_mouse_sample s = {};
	s.gate_open = true;
	s.on_panel = true;
	s.x_m = x;
	s.y_m = y;
	return s;
}

sim_stylus_mouse_monitor
monitor(int32_t left, int32_t top, int32_t w, int32_t h, int32_t w_mm, int32_t h_mm, bool primary, const char *name)
{
	sim_stylus_mouse_monitor m = {};
	m.left = left;
	m.top = top;
	m.width = w;
	m.height = h;
	m.width_mm = w_mm;
	m.height_mm = h_mm;
	m.is_primary = primary;
	std::strncpy(m.name, name, sizeof(m.name) - 1);
	return m;
}

xrt_vec3
rotate(const xrt_quat &q, const xrt_vec3 &v)
{
	const xrt_vec3 u = {q.x, q.y, q.z};
	const xrt_vec3 c1 = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
	const xrt_vec3 c2 = {u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};
	return {v.x + 2.0f * (q.w * c1.x + c2.x), v.y + 2.0f * (q.w * c1.y + c2.y), v.z + 2.0f * (q.w * c1.z + c2.z)};
}

} // namespace


TEST_CASE("map_pixel: the monitor spans the glass, +Y up, centre origin", "[sim_stylus_mouse]")
{
	const sim_stylus_mouse_monitor m = monitor(1920, -100, 3840, 2160, 0, 0, false, "\\\\.\\DISPLAY2");
	float x = 99.0f, y = 99.0f;

	// The centre pixel pair straddles the origin.
	REQUIRE(sim_stylus_mouse_map_pixel(&m, 1920 + 1920, -100 + 1080, 0.6f, 0.34f, &x, &y));
	CHECK(std::fabs(x) < 0.001f);
	CHECK(std::fabs(y) < 0.001f);

	// Top-left pixel: left and UP.
	REQUIRE(sim_stylus_mouse_map_pixel(&m, 1920, -100, 0.6f, 0.34f, &x, &y));
	CHECK(x == Catch::Approx(-0.3f).margin(0.001f));
	CHECK(y == Catch::Approx(0.17f).margin(0.001f));

	// Bottom-right pixel: right and down.
	REQUIRE(sim_stylus_mouse_map_pixel(&m, 1920 + 3839, -100 + 2159, 0.6f, 0.34f, &x, &y));
	CHECK(x == Catch::Approx(0.3f).margin(0.001f));
	CHECK(y == Catch::Approx(-0.17f).margin(0.001f));

	// Off the monitor (the primary to its left, one past its right / bottom edge).
	x = y = 42.0f;
	CHECK_FALSE(sim_stylus_mouse_map_pixel(&m, 100, 100, 0.6f, 0.34f, &x, &y));
	CHECK_FALSE(sim_stylus_mouse_map_pixel(&m, 1920 + 3840, 0, 0.6f, 0.34f, &x, &y));
	CHECK_FALSE(sim_stylus_mouse_map_pixel(&m, 2000, -100 + 2160, 0.6f, 0.34f, &x, &y));
	CHECK(x == 42.0f);
}

TEST_CASE("pick_monitor: SCREEN, then physical size, then aspect, then primary", "[sim_stylus_mouse]")
{
	// A laptop (primary, 16:10) and a 15.6" 16:9 3D panel.
	sim_stylus_mouse_monitor mons[3] = {
	    monitor(0, 0, 2560, 1600, 300, 190, true, "\\\\.\\DISPLAY1"),
	    monitor(2560, 0, 3840, 2160, 344, 194, false, "\\\\.\\DISPLAY2"),
	    monitor(-1920, 0, 1920, 1080, 600, 340, false, "\\\\.\\DISPLAY3"),
	};
	sim_stylus_mouse_pick_rule rule;
	bool unmatched = false;

	// Physical size names the 15.6" one even though DISPLAY3 is 16:9 too.
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.344f, 0.194f, "", &rule, &unmatched) == 1);
	CHECK(rule == SIM_STYLUS_MOUSE_PICK_SIZE);
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.60f, 0.34f, nullptr, &rule, &unmatched) == 2);
	CHECK(rule == SIM_STYLUS_MOUSE_PICK_SIZE);

	// SCREEN wins: by index, by full name, by tail, case-insensitively.
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.344f, 0.194f, "0", &rule, &unmatched) == 0);
	CHECK(rule == SIM_STYLUS_MOUSE_PICK_SETTING);
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.344f, 0.194f, "\\\\.\\DISPLAY3", &rule, &unmatched) == 2);
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.344f, 0.194f, "display3", &rule, &unmatched) == 2);
	CHECK_FALSE(unmatched);

	// A SCREEN that names nothing is reported and ignored.
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.344f, 0.194f, "DISPLAY9", &rule, &unmatched) == 1);
	CHECK(unmatched);
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.344f, 0.194f, "7", &rule, &unmatched) == 1);
	CHECK(unmatched);

	// No physical sizes: aspect alone; two 16:9 candidates -> neither is primary, closest first.
	for (auto &m : mons) {
		m.width_mm = m.height_mm = 0;
	}
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.344f, 0.194f, "", &rule, &unmatched) == 1);
	CHECK(rule == SIM_STYLUS_MOUSE_PICK_ASPECT);

	// Aspect tie with the primary: the primary (the laptop-panel case).
	sim_stylus_mouse_monitor tie[2] = {
	    monitor(-1920, 0, 1920, 1080, 0, 0, false, "B"),
	    monitor(0, 0, 3840, 2160, 0, 0, true, "A"),
	};
	CHECK(sim_stylus_mouse_pick_monitor(tie, 2, 0.344f, 0.194f, "", &rule, &unmatched) == 1);

	// Nothing matches the aspect, or no display size: the primary.
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.30f, 0.30f, "", &rule, &unmatched) == 0);
	CHECK(rule == SIM_STYLUS_MOUSE_PICK_PRIMARY);
	CHECK(sim_stylus_mouse_pick_monitor(mons, 3, 0.0f, 0.0f, "", &rule, &unmatched) == 0);
	CHECK(rule == SIM_STYLUS_MOUSE_PICK_PRIMARY);

	CHECK(sim_stylus_mouse_pick_monitor(mons, 0, 0.344f, 0.194f, "", &rule, &unmatched) == -1);
}

TEST_CASE("tip: hovers until the left button, then ON the glass with a force ramp", "[sim_stylus_mouse]")
{
	const sim_stylus_mouse_config c = base_config();
	sim_stylus_mouse_machine m;
	sim_stylus_mouse_machine_init(&m, &c);
	sim_stylus_state s;

	sim_stylus_mouse_sample in = on_panel(0.1f, -0.05f);
	sim_stylus_mouse_machine_step(&m, &c, &in, 1000 * kNsPerMs, &s);
	CHECK(s.tracked);
	CHECK_FALSE(s.contact);
	CHECK(s.tip_force == 0.0f);
	CHECK(s.tip.position.x == Catch::Approx(0.1f));
	CHECK(s.tip.position.y == Catch::Approx(-0.05f));
	CHECK(s.tip.position.z == Catch::Approx(0.02f));

	in.left = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 1010 * kNsPerMs, &s);
	CHECK(s.contact);
	CHECK(s.tip.position.z == 0.0f);
	CHECK(s.tip_force == Catch::Approx(0.0f).margin(1e-6));

	sim_stylus_mouse_machine_step(&m, &c, &in, 1160 * kNsPerMs, &s);
	CHECK(s.tip_force == Catch::Approx(0.5f).margin(1e-3));
	sim_stylus_mouse_machine_step(&m, &c, &in, 1400 * kNsPerMs, &s);
	CHECK(s.tip_force == 1.0f);

	in.left = false;
	sim_stylus_mouse_machine_step(&m, &c, &in, 1500 * kNsPerMs, &s);
	CHECK_FALSE(s.contact);
	CHECK(s.tip_force == 0.0f);
	CHECK(s.tip.position.z == Catch::Approx(0.02f));

	// A new press ramps from 0 again; RAMP_MS = 0 is full force at once.
	in.left = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 1600 * kNsPerMs, &s);
	CHECK(s.tip_force < 0.01f);
	sim_stylus_mouse_config c0 = base_config();
	c0.ramp_ms = 0;
	sim_stylus_mouse_machine_step(&m, &c0, &in, 1601 * kNsPerMs, &s);
	CHECK(s.tip_force == 1.0f);
}

TEST_CASE("hover height: PageUp / PageDown step it, Shift steps bigger, clamped", "[sim_stylus_mouse]")
{
	const sim_stylus_mouse_config c = base_config();
	sim_stylus_mouse_machine m;
	sim_stylus_mouse_machine_init(&m, &c);
	sim_stylus_state s;
	sim_stylus_mouse_sample in = on_panel(0.0f, 0.0f);
	int64_t t = 0;
	auto tick = [&](int64_t ms) {
		t += ms * kNsPerMs;
		sim_stylus_mouse_machine_step(&m, &c, &in, t, &s);
	};

	tick(10);
	CHECK(s.tip.position.z == Catch::Approx(0.020f));

	// One press = one step, however many frames it is held for (before the repeat delay).
	in.page_up = true;
	tick(10);
	tick(10);
	tick(10);
	CHECK(s.tip.position.z == Catch::Approx(0.025f));
	in.page_up = false;
	tick(10);

	in.page_down = true;
	tick(10);
	in.page_down = false;
	tick(10);
	CHECK(s.tip.position.z == Catch::Approx(0.020f));

	in.shift = true;
	in.page_up = true;
	tick(10);
	in.page_up = false;
	in.shift = false;
	tick(10);
	CHECK(s.tip.position.z == Catch::Approx(0.040f));

	// Holding auto-repeats after the delay ...
	in.page_up = true;
	tick(10); // 0.045
	for (int i = 0; i < 40; i++) {
		tick(10); // 400 ms more: repeat starts
	}
	CHECK(s.tip.position.z > 0.046f);
	// ... and is clamped at the top.
	for (int i = 0; i < 400; i++) {
		tick(10);
	}
	CHECK(s.tip.position.z == Catch::Approx(SIM_STYLUS_MOUSE_HEIGHT_MAX_M));
	in.page_up = false;

	// And at the glass.
	in.page_down = true;
	in.shift = true;
	for (int i = 0; i < 400; i++) {
		tick(10);
	}
	CHECK(s.tip.position.z == 0.0f);
	CHECK(s.tracked);
}

TEST_CASE("cluster buttons, middle force, system click", "[sim_stylus_mouse]")
{
	const sim_stylus_mouse_config c = base_config();
	sim_stylus_mouse_machine m;
	sim_stylus_mouse_machine_init(&m, &c);
	sim_stylus_state s;
	sim_stylus_mouse_sample in = on_panel(0.0f, 0.0f);

	in.right = true;
	in.x2 = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 100 * kNsPerMs, &s);
	CHECK(s.front_click);
	CHECK_FALSE(s.back_click);
	CHECK(s.system_click);
	CHECK_FALSE(s.front_double_tap);

	in = on_panel(0.0f, 0.0f);
	in.middle = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 200 * kNsPerMs, &s);
	CHECK(s.back_click);
	CHECK_FALSE(s.front_click);

	// Ctrl and X1 both ramp the middle force.
	in = on_panel(0.0f, 0.0f);
	in.ctrl = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 300 * kNsPerMs, &s);
	sim_stylus_mouse_machine_step(&m, &c, &in, 450 * kNsPerMs, &s);
	CHECK(s.middle_force == Catch::Approx(0.5f).margin(1e-3));
	in.ctrl = false;
	in.x1 = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 800 * kNsPerMs, &s);
	CHECK(s.middle_force == 1.0f);
	in.x1 = false;
	sim_stylus_mouse_machine_step(&m, &c, &in, 900 * kNsPerMs, &s);
	CHECK(s.middle_force == 0.0f);
}

TEST_CASE("double tap: two presses within the double-click time, a short pulse", "[sim_stylus_mouse]")
{
	const sim_stylus_mouse_config c = base_config();
	sim_stylus_mouse_machine m;
	sim_stylus_mouse_machine_init(&m, &c);
	sim_stylus_state s;
	sim_stylus_mouse_sample in = on_panel(0.0f, 0.0f);
	auto at = [&](bool right, int64_t ms) {
		in.right = right;
		sim_stylus_mouse_machine_step(&m, &c, &in, ms * kNsPerMs, &s);
	};

	at(true, 1000);
	CHECK_FALSE(s.front_double_tap);
	at(false, 1100);
	at(true, 1300); // second press 300 ms after the first
	CHECK(s.front_double_tap);
	CHECK(s.front_click);
	at(true, 1400);
	CHECK(s.front_double_tap);
	at(false, 1460);
	CHECK_FALSE(s.front_double_tap); // pulse over (150 ms)

	// A third press does not chain into another double tap ...
	at(true, 1600);
	CHECK_FALSE(s.front_double_tap);
	at(false, 1650);
	// ... and two presses too far apart are two single clicks.
	at(true, 2300);
	CHECK_FALSE(s.front_double_tap);
	CHECK_FALSE(s.back_double_tap);

	// The back button has its own detector.
	in.right = false;
	in.middle = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 5000 * kNsPerMs, &s);
	in.middle = false;
	sim_stylus_mouse_machine_step(&m, &c, &in, 5050 * kNsPerMs, &s);
	in.middle = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 5100 * kNsPerMs, &s);
	CHECK(s.back_double_tap);
	CHECK_FALSE(s.front_double_tap);
}

TEST_CASE("gate closed = gone; cursor off the panel = untracked; Home docks", "[sim_stylus_mouse]")
{
	const sim_stylus_mouse_config c = base_config();
	sim_stylus_mouse_machine m;
	sim_stylus_mouse_machine_init(&m, &c);
	sim_stylus_state s;

	sim_stylus_mouse_sample in = on_panel(0.0f, 0.0f);
	in.left = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 100 * kNsPerMs, &s);
	CHECK(s.contact);

	// Gate closes mid-stroke: nothing is pressed or tracked.
	sim_stylus_mouse_sample closed = in;
	closed.gate_open = false;
	sim_stylus_mouse_machine_step(&m, &c, &closed, 200 * kNsPerMs, &s);
	CHECK_FALSE(s.tracked);
	CHECK_FALSE(s.contact);
	CHECK(s.tip_force == 0.0f);

	// Off the panel: untracked too, nothing pressed.
	sim_stylus_mouse_sample off = in;
	off.on_panel = false;
	sim_stylus_mouse_machine_step(&m, &c, &off, 300 * kNsPerMs, &s);
	CHECK_FALSE(s.tracked);
	CHECK(s.tip_force == 0.0f);

	// Back on with the button still held: a fresh press, ramping from 0.
	sim_stylus_mouse_machine_step(&m, &c, &in, 400 * kNsPerMs, &s);
	CHECK(s.contact);
	CHECK(s.tip_force < 0.01f);

	// Home toggles the dock: tracked, docked, nothing pressed, wherever the cursor is.
	in.home = true;
	sim_stylus_mouse_machine_step(&m, &c, &in, 500 * kNsPerMs, &s);
	CHECK(s.docked);
	CHECK(s.tracked);
	CHECK_FALSE(s.contact);
	CHECK(s.tip_force == 0.0f);
	CHECK(s.tip.position.x > 0.3f); // beside the lower-right corner
	sim_stylus_mouse_machine_step(&m, &c, &in, 600 * kNsPerMs, &s);
	CHECK(s.docked); // held, not re-toggled
	off.home = false;
	sim_stylus_mouse_machine_step(&m, &c, &off, 650 * kNsPerMs, &s);
	CHECK(s.docked);
	CHECK(s.tracked);
	in.home = true;
	in.left = false;
	sim_stylus_mouse_machine_step(&m, &c, &in, 700 * kNsPerMs, &s);
	CHECK_FALSE(s.docked);
	CHECK(s.tip.position.z == Catch::Approx(0.02f));
}

TEST_CASE("pose: -Z into the glass, tilt toward the viewer, grip back up the barrel", "[sim_stylus_mouse]")
{
	sim_stylus_mouse_config c = base_config();
	sim_stylus_mouse_machine m;
	sim_stylus_mouse_machine_init(&m, &c);
	sim_stylus_state s;
	sim_stylus_mouse_sample in = on_panel(0.05f, 0.05f);
	sim_stylus_mouse_machine_step(&m, &c, &in, 100 * kNsPerMs, &s);

	const xrt_vec3 back = rotate(s.tip.orientation, {0.0f, 0.0f, 1.0f});
	CHECK(back.z == Catch::Approx(std::cos(30.0 * 3.14159265358979 / 180.0)).margin(1e-3));
	CHECK(back.x > 0.0f); // right hand: leans right
	CHECK(back.y < 0.0f); // and down
	const float dx = s.grip.position.x - s.tip.position.x;
	const float dy = s.grip.position.y - s.tip.position.y;
	const float dz = s.grip.position.z - s.tip.position.z;
	CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) == Catch::Approx(SIM_STYLUS_GRIP_OFFSET_M).margin(1e-4));
	CHECK(dz > 0.0f);

	c.left_handed = true;
	c.tilt_deg = 0.0f;
	sim_stylus_mouse_machine_step(&m, &c, &in, 120 * kNsPerMs, &s);
	const xrt_vec3 straight = rotate(s.tip.orientation, {0.0f, 0.0f, 1.0f});
	CHECK(straight.z == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("velocity follows the cursor", "[sim_stylus_mouse]")
{
	const sim_stylus_mouse_config c = base_config();
	sim_stylus_mouse_machine m;
	sim_stylus_mouse_machine_init(&m, &c);
	sim_stylus_state s;
	float x = 0.0f;
	for (int i = 0; i < 20; i++) {
		sim_stylus_mouse_sample in = on_panel(x, 0.0f);
		sim_stylus_mouse_machine_step(&m, &c, &in, (int64_t)(100 + 10 * i) * kNsPerMs, &s);
		x += 0.001f; // 1 mm per 10 ms = 0.1 m/s
	}
	CHECK(s.tip_linear_velocity.x == Catch::Approx(0.1f).margin(0.002f));
	CHECK(std::fabs(s.tip_linear_velocity.y) < 1e-4f);
}

TEST_CASE("Scroll Lock tracker: async edges flip it, the queue's bit re-syncs it", "[sim_stylus_mouse]")
{
	sim_stylus_mouse_toggle t = {};
	CHECK_FALSE(sim_stylus_mouse_toggle_update(&t, false, false));
	// A press the thread's queue never sees still flips it, once per press.
	CHECK(sim_stylus_mouse_toggle_update(&t, false, true));
	CHECK(sim_stylus_mouse_toggle_update(&t, false, true));
	CHECK(sim_stylus_mouse_toggle_update(&t, false, false));
	CHECK_FALSE(sim_stylus_mouse_toggle_update(&t, false, true));
	CHECK_FALSE(sim_stylus_mouse_toggle_update(&t, false, false));
	// The queue reports on: trust it.
	CHECK(sim_stylus_mouse_toggle_update(&t, true, false));
	CHECK(sim_stylus_mouse_toggle_update(&t, true, false));
	// Starting state comes from the queue.
	sim_stylus_mouse_toggle t2 = {};
	CHECK(sim_stylus_mouse_toggle_update(&t2, true, false));
}

TEST_CASE("GATE and MODE parsing", "[sim_stylus_mouse]")
{
	sim_stylus_mouse_gate g = SIM_STYLUS_MOUSE_GATE_NONE;
	CHECK(sim_stylus_mouse_parse_gate("scrolllock", &g));
	CHECK(g == SIM_STYLUS_MOUSE_GATE_SCROLLLOCK);
	CHECK(sim_stylus_mouse_parse_gate("none", &g));
	CHECK(g == SIM_STYLUS_MOUSE_GATE_NONE);
	CHECK_FALSE(sim_stylus_mouse_parse_gate("capslock", &g));
	CHECK_FALSE(sim_stylus_mouse_parse_gate(nullptr, &g));
	CHECK(g == SIM_STYLUS_MOUSE_GATE_NONE);

	sim_stylus_mode mode = SIM_STYLUS_MODE_DRAW;
	CHECK(sim_stylus_parse_mode("mouse", &mode));
	CHECK(mode == SIM_STYLUS_MODE_MOUSE);
}
