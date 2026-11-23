// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared-surface input routing to content apps (macOS + desktop Linux,
 *         XR_DXR_workspace_client_input): the app's copy carries the cursor
 *         normalized to its content rect while the controller's copy is
 *         unchanged, a press makes the app the pointer owner for the drag, and
 *         key forwarding follows the Windows reserved-key policy.
 */

#include "catch_amalgamated.hpp"

#include "server/ipc_server_input_queue.h"
#include "multi/comp_multi_workspace.h"

#include "xrt/xrt_defines.h"

#include <cmath>
#include <cstring>

namespace {

// Fake per-session compositors: the router and the registry only use the
// pointer as a key.
alignas(16) char g_app_a_storage[64];
alignas(16) char g_app_b_storage[64];
xrt_compositor *const APP_A = reinterpret_cast<xrt_compositor *>(g_app_a_storage);
xrt_compositor *const APP_B = reinterpret_cast<xrt_compositor *>(g_app_b_storage);

void
place(xrt_compositor *xc, int32_t x, int32_t y, int32_t w, int32_t h, float z)
{
	xrt_pose pose = {};
	pose.orientation.w = 1.0f;
	pose.position.z = z;
	comp_multi_workspace_store_window_pose(xc, &pose, 0.2f, 0.1f, x, y, w, h);
}

ipc_workspace_input_event_batch
drain(void *target)
{
	ipc_workspace_input_event_batch b = {};
	ipc_server_input_queue_drain(target, IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX, &b);
	return b;
}

void
reset_all()
{
	ipc_server_input_queue_set_input_grab(false);
	ipc_server_input_queue_set_pointer_capture(false);
	ipc_server_input_queue_set_reserved_keys(nullptr);
	ipc_server_input_queue_clear_frame_tick();
	ipc_server_input_queue_drop(APP_A); // also ends any pointer ownership
	ipc_server_input_queue_drop(APP_B);
	drain(IPC_INPUT_TARGET_CONTROLLER);
	comp_multi_workspace_set_focused_client(nullptr);
}

ipc_workspace_input_event
button(uint32_t b, bool down, int64_t x, int64_t y)
{
	ipc_workspace_input_event e = {};
	e.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER;
	e.u.pointer.button = b;
	e.u.pointer.is_down = down ? 1u : 0u;
	e.u.pointer.cursor_x = x;
	e.u.pointer.cursor_y = y;
	return e;
}

ipc_workspace_input_event
motion(uint32_t mask, int64_t x, int64_t y)
{
	ipc_workspace_input_event e = {};
	e.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION;
	e.u.pointer_motion.button_mask = mask;
	e.u.pointer_motion.cursor_x = x;
	e.u.pointer_motion.cursor_y = y;
	return e;
}

ipc_workspace_input_event
key(uint32_t vk, uint32_t mods)
{
	ipc_workspace_input_event e = {};
	e.event_type = IPC_WORKSPACE_INPUT_EVENT_KEY;
	e.u.key.vk_code = vk;
	e.u.key.modifiers = mods;
	e.u.key.is_down = 1;
	return e;
}

} // namespace

TEST_CASE("window_local_uv maps into the placed content rect")
{
	place(APP_A, 100, 200, 400, 300, 0.0f);
	float u = -1.0f;
	float v = -1.0f;
	REQUIRE(comp_multi_workspace_window_local_uv(APP_A, 300, 350, &u, &v));
	CHECK(u == Catch::Approx(0.5f));
	CHECK(v == Catch::Approx(0.5f));
	REQUIRE(comp_multi_workspace_window_local_uv(APP_A, 100, 200, &u, &v));
	CHECK(u == Catch::Approx(0.0f));
	CHECK(v == Catch::Approx(0.0f));
	// Not clamped: a drag past the right / top edge.
	REQUIRE(comp_multi_workspace_window_local_uv(APP_A, 900, 50, &u, &v));
	CHECK(u == Catch::Approx(2.0f));
	CHECK(v == Catch::Approx(-0.5f));

	alignas(16) static char unplaced_storage[64];
	auto *unplaced = reinterpret_cast<xrt_compositor *>(unplaced_storage);
	u = 7.0f;
	CHECK_FALSE(comp_multi_workspace_window_local_uv(unplaced, 300, 350, &u, &v));
	CHECK(u == 7.0f); // untouched
}

TEST_CASE("app copies carry local UV; the controller copy is unchanged")
{
	place(APP_A, 100, 200, 400, 300, 0.0f);
	reset_all();

	ipc_workspace_input_event m = motion(0, 200, 275);
	ipc_server_input_route(&m);

	auto c = drain(IPC_INPUT_TARGET_CONTROLLER);
	REQUIRE(c.count == 1);
	CHECK(c.events[0].u.pointer_motion.local_u == 0.0f);
	CHECK(c.events[0].u.pointer_motion.local_v == 0.0f);

	auto a = drain(APP_A);
	REQUIRE(a.count == 1);
	CHECK(a.events[0].u.pointer_motion.local_u == Catch::Approx(0.25f));
	CHECK(a.events[0].u.pointer_motion.local_v == Catch::Approx(0.25f));

	ipc_workspace_input_event s = {};
	s.event_type = IPC_WORKSPACE_INPUT_EVENT_SCROLL;
	s.u.scroll.delta_y = 1.0f;
	s.u.scroll.cursor_x = 400;
	s.u.scroll.cursor_y = 425;
	ipc_server_input_route(&s);
	CHECK(drain(IPC_INPUT_TARGET_CONTROLLER).count == 0); // scroll routes exclusively
	a = drain(APP_A);
	REQUIRE(a.count == 1);
	CHECK(a.events[0].u.scroll.delta_y == 1.0f);
	CHECK(a.events[0].u.scroll.local_u == Catch::Approx(0.75f));
	CHECK(a.events[0].u.scroll.local_v == Catch::Approx(0.75f));
}

TEST_CASE("a press owns the pointer until release, even outside the window")
{
	place(APP_A, 100, 200, 400, 300, 0.0f);
	place(APP_B, 600, 200, 400, 300, 0.0f);
	reset_all();

	ipc_workspace_input_event e = button(1, true, 300, 350);
	ipc_server_input_route(&e);
	REQUIRE(drain(APP_A).count == 1);

	// Drag across into B's rect: motion keeps going to A, unclamped.
	e = motion(1, 700, 350);
	ipc_server_input_route(&e);
	CHECK(drain(APP_B).count == 0);
	auto a = drain(APP_A);
	REQUIRE(a.count == 1);
	CHECK(a.events[0].u.pointer_motion.local_u == Catch::Approx(1.5f));

	// Release over B: still A's.
	e = button(1, false, 700, 350);
	ipc_server_input_route(&e);
	CHECK(drain(APP_B).count == 0);
	a = drain(APP_A);
	REQUIRE(a.count == 1);
	CHECK(a.events[0].u.pointer.is_down == 0u);

	// Ownership is over: motion now follows the cursor again.
	e = motion(0, 700, 350);
	ipc_server_input_route(&e);
	CHECK(drain(APP_A).count == 0);
	CHECK(drain(APP_B).count == 1);

	// Controller saw every button / motion event.
	CHECK(drain(IPC_INPUT_TARGET_CONTROLLER).count == 4);
}

TEST_CASE("a lost release does not leave a stale owner")
{
	place(APP_A, 100, 200, 400, 300, 0.0f);
	place(APP_B, 600, 200, 400, 300, 0.0f);
	reset_all();

	ipc_workspace_input_event e = button(1, true, 300, 350);
	ipc_server_input_route(&e);
	// No release; motion reports nothing held.
	e = motion(0, 700, 350);
	ipc_server_input_route(&e);
	e = button(1, true, 700, 350);
	ipc_server_input_route(&e);
	drain(APP_A);
	auto b = drain(APP_B);
	REQUIRE(b.count == 2); // the motion and the new press
	CHECK(b.events[1].event_type == IPC_WORKSPACE_INPUT_EVENT_POINTER);
	reset_all();
}

TEST_CASE("the controller's own drain is unaffected and the app queue is per app")
{
	place(APP_A, 100, 200, 400, 300, 0.0f);
	reset_all();
	ipc_workspace_input_event e = button(1, true, 50, 50); // over no window
	ipc_server_input_route(&e);
	CHECK(drain(APP_A).count == 0);
	auto c = drain(IPC_INPUT_TARGET_CONTROLLER);
	REQUIRE(c.count == 1);
	CHECK(c.events[0].u.pointer.local_u == 0.0f);
	reset_all();
}

TEST_CASE("key forwarding follows the Windows reserved-key policy")
{
	reset_all();
	comp_multi_workspace_set_focused_client(APP_A);

	const uint32_t SHIFT = 1u, CTRL = 2u;
	struct
	{
		uint32_t vk, mods;
		bool to_app;
	} defaults[] = {
	    {'T', CTRL, true},   // Ctrl chords reach the app (Ctrl+T)
	    {'A', 0, true},      //
	    {0x09, 0, false},    // bare TAB
	    {0x09, SHIFT, true}, // Shift+TAB reaches the app
	    {0x2E, 0, false},    // DELETE
	    {0x1B, 0, false},    // ESC
	    {0xDB, 0, false},    // [
	    {0xDD, 0, false},    // ]
	    {0x7A, 0, true},     // F11 is not in the Windows default
	    {0x25, 0, true},     // arrows neither
	};
	for (const auto &k : defaults) {
		ipc_workspace_input_event e = key(k.vk, k.mods);
		ipc_server_input_route(&e);
		CHECK(drain(IPC_INPUT_TARGET_CONTROLLER).count == 1); // always the controller
		auto a = drain(APP_A);
		INFO("vk=" << k.vk << " mods=" << k.mods);
		CHECK(a.count == (k.to_app ? 1u : 0u));
		if (k.to_app && a.count == 1) {
			CHECK(a.events[0].u.key.modifiers == k.mods);
		}
	}

	// A controller table replaces the default: exact (vk, mods) match.
	ipc_workspace_reserved_keys table = {};
	table.count = 2;
	table.keys[0] = {'L', CTRL};
	table.keys[1] = {0x09, 0};
	ipc_server_input_queue_set_reserved_keys(&table);
	struct
	{
		uint32_t vk, mods;
		bool to_app;
	} registered[] = {
	    {'L', CTRL, false}, {'T', CTRL, true}, {0x09, 0, false}, {0x1B, 0, true}, {'L', 0, true},
	};
	for (const auto &k : registered) {
		ipc_workspace_input_event e = key(k.vk, k.mods);
		ipc_server_input_route(&e);
		CHECK(drain(IPC_INPUT_TARGET_CONTROLLER).count == 1);
		INFO("vk=" << k.vk << " mods=" << k.mods);
		CHECK(drain(APP_A).count == (k.to_app ? 1u : 0u));
	}

	// count 0 restores the default.
	table.count = 0;
	ipc_server_input_queue_set_reserved_keys(&table);
	ipc_workspace_input_event e = key(0x1B, 0);
	ipc_server_input_route(&e);
	drain(IPC_INPUT_TARGET_CONTROLLER);
	CHECK(drain(APP_A).count == 0);
	reset_all();
}
