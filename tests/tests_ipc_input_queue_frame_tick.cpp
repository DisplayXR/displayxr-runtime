// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The shared-surface FRAME_TICK path into the controller's input queue
 *         (macOS + desktop Linux): one tick per rendered frame, COALESCED to
 *         the latest when the controller is not draining, appended after the
 *         ring's events, payload as the Windows D3D11 service emits it.
 */

#include "catch_amalgamated.hpp"

#include "server/ipc_server_input_queue.h"
#include "multi/comp_multi_workspace.h"

#include <cstring>

namespace {

void
drain_all(ipc_workspace_input_event_batch *b)
{
	ipc_server_input_queue_drain(IPC_INPUT_TARGET_CONTROLLER, IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX, b);
}

comp_multi_frame_tick
tick(uint64_t t, float z, bool valid, int32_t cx)
{
	comp_multi_frame_tick k = {};
	k.timestamp_ns = t;
	k.viewer_x = 0.01f;
	k.viewer_y = 0.02f;
	k.viewer_z = z;
	k.viewer_valid = valid;
	k.cursor_x = cx;
	k.cursor_y = 7;
	return k;
}

} // namespace

TEST_CASE("frame ticks coalesce to the latest and ride after input")
{
	ipc_workspace_input_event_batch b = {};
	drain_all(&b); // start clean
	ipc_server_input_queue_clear_frame_tick();

	comp_multi_workspace_set_frame_tick_sink(ipc_server_input_queue_frame_tick_sink, nullptr);

	// 100 frames rendered while the controller was not draining.
	for (uint64_t i = 1; i <= 100; i++) {
		comp_multi_frame_tick k = tick(i * 1000000ull, 0.5f + (float)i * 0.001f, (i % 2) == 0, (int32_t)i);
		comp_multi_workspace_emit_frame_tick(&k);
	}
	// One key arrives meanwhile.
	ipc_workspace_input_event key = {};
	key.event_type = IPC_WORKSPACE_INPUT_EVENT_KEY;
	key.u.key.vk_code = 0x41;
	key.u.key.is_down = 1;
	ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, &key);

	drain_all(&b);
	REQUIRE(b.count == 2);
	CHECK(b.events[0].event_type == IPC_WORKSPACE_INPUT_EVENT_KEY);
	const ipc_workspace_input_event &ft = b.events[1];
	REQUIRE(ft.event_type == IPC_WORKSPACE_INPUT_EVENT_FRAME_TICK);
	CHECK(ft.u.frame_tick.timestamp_ns == 100ull * 1000000ull); // the latest, not the first
	CHECK(ft.u.frame_tick.viewer_x == Catch::Approx(0.01f));
	CHECK(ft.u.frame_tick.viewer_y == Catch::Approx(0.02f));
	CHECK(ft.u.frame_tick.viewer_z == Catch::Approx(0.6f));
	CHECK(ft.u.frame_tick.viewer_valid == 1u);
	CHECK(ft.u.frame_tick.cursor_x == 100);
	CHECK(ft.u.frame_tick.cursor_y == 7);

	// Drained: nothing until the next frame.
	drain_all(&b);
	CHECK(b.count == 0);

	comp_multi_frame_tick k = tick(5, 0.6f, false, 3);
	comp_multi_workspace_emit_frame_tick(&k);
	drain_all(&b);
	REQUIRE(b.count == 1);
	CHECK(b.events[0].u.frame_tick.viewer_valid == 0u);

	comp_multi_workspace_set_frame_tick_sink(nullptr, nullptr);
}

TEST_CASE("a full batch leaves the tick pending; clear drops it; content clients never see it")
{
	ipc_workspace_input_event_batch b = {};
	drain_all(&b);
	ipc_server_input_queue_clear_frame_tick();

	ipc_workspace_input_event key = {};
	key.event_type = IPC_WORKSPACE_INPUT_EVENT_KEY;
	for (int i = 0; i < IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX; i++) {
		ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, &key);
	}
	comp_multi_frame_tick k = tick(42, 0.6f, true, 1);
	ipc_server_input_queue_frame_tick_sink(nullptr, &k);

	drain_all(&b);
	CHECK(b.count == IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX);
	CHECK(b.events[b.count - 1].event_type == IPC_WORKSPACE_INPUT_EVENT_KEY);
	drain_all(&b);
	REQUIRE(b.count == 1);
	CHECK(b.events[0].event_type == IPC_WORKSPACE_INPUT_EVENT_FRAME_TICK);

	// A content client's queue never carries ticks.
	int content_key = 0;
	ipc_server_input_queue_frame_tick_sink(nullptr, &k);
	ipc_server_input_queue_drain(&content_key, IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX, &b);
	CHECK(b.count == 0);

	// Controller loss drops the pending tick.
	ipc_server_input_queue_clear_frame_tick();
	drain_all(&b);
	CHECK(b.count == 0);
}
