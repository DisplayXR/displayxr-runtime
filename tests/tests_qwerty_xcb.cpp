// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief #1727 — the X11 qwerty front-end (qwerty_process_xcb).
 *
 * The property that matters is the #1700 one: one select click advances
 * exactly one interactive-CTS case, so the trigger (bound to select/click) must
 * see exactly one press edge and one release edge per ButtonPress/ButtonRelease
 * pair — never an edge manufactured from state that survived a focus change or
 * a window teardown, and never one read out of a motion event's button bits.
 *
 * An edge is observed through qwerty_controller::trigger_timestamp, which every
 * qwerty_press_trigger / qwerty_release_trigger call stamps: a timestamp that
 * did not move means the device was not touched at all.
 */

#include "catch_amalgamated.hpp"

#include "qwerty_device.h"
#include "qwerty_interface.h"

#include <X11/keysym.h>

#include <chrono>
#include <thread>

namespace {

constexpr uint16_t kButton1Mask = 1u << 8; // X core Button1Mask
constexpr uint16_t kControlMask = 1u << 2;
constexpr uint16_t kMod1Mask = 1u << 3;

struct Devices
{
	xrt_device *devices[3]{};
	Devices()
	{
		REQUIRE(qwerty_create_devices(U_LOGGING_ERROR, &devices[0], &devices[1], &devices[2]) == XRT_SUCCESS);
	}
	~Devices()
	{
		for (int i = 2; i >= 0; --i) {
			if (devices[i] != nullptr) {
				devices[i]->destroy(devices[i]);
			}
		}
	}

	struct qwerty_controller *
	left()
	{
		return qwerty_controller(devices[1]);
	}

	//! With no modifier held, controller-only input goes to the right hand.
	struct qwerty_controller *
	right()
	{
		return qwerty_controller(devices[2]);
	}

	void
	send(qwerty_x11_input_type type,
	     uint32_t keysym = 0,
	     uint8_t button = 0,
	     uint16_t state = 0,
	     int x = 100,
	     int y = 100)
	{
		qwerty_x11_input in{};
		in.type = type;
		in.keysym = keysym;
		in.button = button;
		in.state = state;
		in.root_x = x;
		in.root_y = y;
		qwerty_process_xcb(devices, 3, &in);
		// Keep successive edges on distinct timestamps.
		std::this_thread::sleep_for(std::chrono::microseconds(50));
	}

	void
	key(uint32_t keysym, bool down)
	{
		send(down ? QWERTY_X11_KEY_PRESS : QWERTY_X11_KEY_RELEASE, keysym);
	}

	void
	button(uint8_t b, bool down, uint16_t state = 0)
	{
		send(down ? QWERTY_X11_BUTTON_PRESS : QWERTY_X11_BUTTON_RELEASE, 0, b, state);
	}
};

} // namespace

TEST_CASE("one LMB click is exactly one trigger press edge and one release edge")
{
	Devices d;
	struct qwerty_controller *r = d.right();
	REQUIRE_FALSE(r->trigger_clicked);

	d.button(1, true);
	CHECK(r->trigger_clicked);
	const int64_t t_press = r->trigger_timestamp;

	// A second ButtonPress without a release is not an edge.
	d.button(1, true);
	CHECK(r->trigger_timestamp == t_press);

	d.button(1, false, kButton1Mask);
	CHECK_FALSE(r->trigger_clicked);
	const int64_t t_release = r->trigger_timestamp;
	CHECK(t_release != t_press);

	// A second ButtonRelease is not an edge either.
	d.button(1, false);
	CHECK(r->trigger_timestamp == t_release);
}

TEST_CASE("pointer motion never produces a select edge, whatever its button bits say")
{
	Devices d;
	struct qwerty_controller *r = d.right();
	d.send(QWERTY_X11_MOTION, 0, 0, 0, 10, 10); // binds the system
	const int64_t t0 = r->trigger_timestamp;

	// The #1700 shape: a first motion on a fresh window still claiming Button1.
	d.send(QWERTY_X11_MOTION, 0, 0, kButton1Mask, 20, 20);
	d.send(QWERTY_X11_MOTION, 0, 0, kButton1Mask, 30, 30);
	CHECK_FALSE(r->trigger_clicked);
	CHECK(r->trigger_timestamp == t0);
}

TEST_CASE("focus loss releases a held trigger, and the stale release after it is not an edge")
{
	Devices d;
	struct qwerty_controller *r = d.right();

	d.button(1, true);
	REQUIRE(r->trigger_clicked);

	// The window lost focus / was unmapped / torn down mid-click.
	d.send(QWERTY_X11_FOCUS_OUT);
	CHECK_FALSE(r->trigger_clicked);
	const int64_t t_reset = r->trigger_timestamp;

	// The release that finally arrives is stale: no second release edge.
	d.button(1, false);
	CHECK(r->trigger_timestamp == t_reset);

	// FocusIn with Button1 still in the live mask must not press anything.
	d.send(QWERTY_X11_FOCUS_IN, 0, 0, kButton1Mask);
	CHECK_FALSE(r->trigger_clicked);
	CHECK(r->trigger_timestamp == t_reset);

	// And the next real click is a clean single edge.
	d.button(1, true);
	CHECK(r->trigger_clicked);
	CHECK(r->trigger_timestamp != t_reset);
	d.button(1, false);
	CHECK_FALSE(r->trigger_clicked);
}

TEST_CASE("key map mirrors the Win32 front-end")
{
	Devices d;

	SECTION("N is Menu on the default (right) controller")
	{
		d.key(XK_n, true);
		CHECK(d.right()->menu_clicked);
		CHECK_FALSE(d.left()->menu_clicked);
		d.key(XK_n, false);
		CHECK_FALSE(d.right()->menu_clicked);
	}

	SECTION("CTRL+ALT targets both hands (QuadHands / SpaceOffsets recipe)")
	{
		d.key(XK_Control_L, true);
		d.key(XK_Alt_L, true);
		d.key(XK_e, true);
		CHECK(d.left()->base.up_pressed);
		CHECK(d.right()->base.up_pressed);

		// SHIFT+Up / Z: the angular axes with sprint.
		d.key(XK_Shift_L, true);
		d.key(XK_Up, true);
		d.key(XK_z, true);
		CHECK(d.left()->base.sprint_pressed);
		CHECK(d.left()->base.look_up_pressed);
		CHECK(d.right()->base.roll_left_pressed);

		d.button(1, true);
		CHECK(d.left()->trigger_clicked);
		CHECK(d.right()->trigger_clicked);
		d.button(1, false);

		// Losing focus drops the chord and everything it held.
		d.send(QWERTY_X11_FOCUS_OUT);
		CHECK_FALSE(d.left()->base.up_pressed);
		CHECK_FALSE(d.right()->base.look_up_pressed);
		CHECK_FALSE(d.left()->base.sprint_pressed);

		// With the chord gone, N goes to the default controller only.
		d.key(XK_n, true);
		CHECK(d.right()->menu_clicked);
		CHECK_FALSE(d.left()->menu_clicked);
	}

	SECTION("FocusIn re-syncs a CTRL+ALT chord held into the window")
	{
		d.send(QWERTY_X11_FOCUS_IN, 0, 0, kControlMask | kMod1Mask);
		d.key(XK_d, true);
		CHECK(d.left()->base.right_pressed);
		CHECK(d.right()->base.right_pressed);
	}

	SECTION("MMB is squeeze, single edge")
	{
		d.button(2, true);
		CHECK(d.right()->squeeze_clicked);
		d.button(2, false);
		CHECK_FALSE(d.right()->squeeze_clicked);
	}
}
