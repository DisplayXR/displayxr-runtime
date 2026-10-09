// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief ADR-051 D3 (display dashboard phase 2): the display status snapshot
 *        crosses the DIAG IPC path in fixed-size pieces — the head with one
 *        screen row per `system_get_status_snapshot(screen_index)` reply, one
 *        client row per `system_get_client_segments(client_id)` reply — and a
 *        reader reassembles exactly the snapshot the service built.
 */

#include "util/u_status_snapshot.h"

#ifdef DXR_TESTS_HAVE_IPC_PROTOCOL
#include "shared/ipc_protocol.h"
#include "ipc_protocol_generated.h"
#endif

#include "catch_amalgamated.hpp"

#include <cjson/cJSON.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

/*
 * A two-panel service snapshot: two VERIFIED screens, a forced-IPC app whose
 * window straddles the seam (one segment per screen, the owner on screen 1),
 * and the CLI's own DIAG connection.
 */
std::unique_ptr<xrt_status_snapshot>
two_panel_snapshot()
{
	std::unique_ptr<xrt_status_snapshot> s(new xrt_status_snapshot);
	std::memset(s.get(), 0, sizeof(*s));
	s->schema = XRT_STATUS_SCHEMA;
	s->source = XRT_STATUS_SOURCE_SERVICE;
	s->generation.topology = 17;
	s->generation.status = 2412;
	std::snprintf(s->runtime.version, sizeof(s->runtime.version), "2.32.0");
	std::snprintf(s->runtime.git_tag, sizeof(s->runtime.git_tag), "v2.32.0");
	s->runtime.plugin_abi = 5;

	s->plugin_count = 2;
	std::snprintf(s->plugins[0].id, sizeof(s->plugins[0].id), "vendor-a");
	std::snprintf(s->plugins[0].load, sizeof(s->plugins[0].load), "ACTIVE");
	s->plugins[0].active = true;
	s->plugins[0].probe_order = 50;
	std::snprintf(s->plugins[1].id, sizeof(s->plugins[1].id), "sim-display");
	s->plugins[1].fallback = true;
	s->plugins[1].probe_order = 200;

	s->screen_count = 2;
	for (uint32_t i = 0; i < 2; i++) {
		xrt_status_screen &sc = s->screens[i];
		sc.id = 0x8c413a2f61152ce7ull + i;
		sc.index = i;
		std::snprintf(sc.device_name, sizeof(sc.device_name), "\\\\.\\DISPLAY%u", i == 0 ? 1u : 5u);
		sc.desktop = {(int32_t)(i * 3840), 0, 3840, 2160, 2.5f};
		sc.native = {3840, 2160, 60000, true};
		sc.physical = {344, 194, XRT_STATUS_MM_SOURCE_PLUGIN};
		sc.roles.runtime_default = i == 0;
		std::snprintf(sc.claim.plugin_id, sizeof(sc.claim.plugin_id), "vendor-a");
		sc.claim.confidence = 100;
		sc.claim.apis = XRT_STATUS_API_BIT_D3D11 | XRT_STATUS_API_BIT_D3D12;
		sc.eye_tracking.supported = 1;
		sc.eye_tracking.state = i == 0 ? XRT_STATUS_TRACKING_TRACKING : XRT_STATUS_TRACKING_NOT_TRACKING;
		sc.eye_tracking.not_tracking_ms = i == 0 ? 0u : 6100u;
		sc.mode = {true, 1, "3D", 2, true};
		sc.dp_count = 1;
		sc.dps[0] = {3, XRT_STATUS_DP_API_D3D11,
		             i == 0 ? XRT_STATUS_DP_KIND_PRIMARY : XRT_STATUS_DP_KIND_SEGMENT,
		             XRT_STATUS_DP_BACKEND_OK};
	}

	s->client_count = 2;
	xrt_status_client &app = s->clients[0];
	app.id = 3;
	app.pid = 24416;
	app.client_class = 0; // APP
	app.class_verified = true;
	std::snprintf(app.name, sizeof(app.name), "cube_handle_d3d11_win.exe");
	app.flags = {true, true, true, false};
	app.presenter = XRT_STATUS_PRESENTER_APP_HWND;
	app.lease = XRT_STATUS_LEASE_SLOT;
	app.window = {true, 3018, 285, 1664, 2093};
	app.owner_screen = s->screens[1].id;
	app.segments.generation = 41;
	app.segments.split = true;
	app.segments.count = 2;
	app.segments.items[0] = {s->screens[0].id, {0, 0, 822, 1875}, true, true, XRT_STATUS_EYE_SOURCE_DP};
	app.segments.items[1] = {s->screens[1].id, {822, 0, 842, 1875}, true, true, XRT_STATUS_EYE_SOURCE_DP};
	app.views = {2, 2, 4};
	app.integrity.paint = 1811;
	app.integrity.present = 1809;
	app.integrity.skip = 2;
	std::snprintf(app.integrity.weave_placement, sizeof(app.integrity.weave_placement), "scanout");

	xrt_status_client &cli = s->clients[1];
	cli.id = 4;
	cli.pid = 777;
	cli.client_class = 5; // DIAG
	cli.class_verified = true;
	std::snprintf(cli.name, sizeof(cli.name), "displayxr-cli");
	cli.presenter = XRT_STATUS_PRESENTER_NONE;
	cli.lease = XRT_STATUS_LEASE_NONE;

	u_status_warnings_derive(s.get());
	return s;
}

} // namespace

TEST_CASE("status ipc: head + rows reassemble the snapshot byte for byte")
{
	auto src = two_panel_snapshot();
	// TRACKER_DOWN fired on screen 1 (6.1 s untracked while screen 0 tracks):
	// the warnings travel with the screen row.
	REQUIRE(src->screens[1].warning_count == 1);
	CHECK(std::strcmp(src->screens[1].warnings[0].code, U_STATUS_W_TRACKER_DOWN) == 0);

	xrt_status_head head;
	u_status_snapshot_get_head(src.get(), &head);
	CHECK(head.screen_count == 2);
	REQUIRE(head.client_count == 2);
	CHECK(head.client_ids[0] == 3);
	CHECK(head.client_ids[1] == 4);
	CHECK(u_status_generation_equal(&head.generation, &src->generation));

	std::unique_ptr<xrt_status_snapshot> dst(new xrt_status_snapshot);
	u_status_snapshot_set_head(dst.get(), &head);
	CHECK(dst->clients[0].id == 3);
	for (uint32_t i = 0; i < head.screen_count; i++) {
		dst->screens[i] = src->screens[i];
	}
	for (uint32_t i = 0; i < head.client_count; i++) {
		dst->clients[i] = src->clients[i];
	}
	CHECK(std::memcmp(dst.get(), src.get(), sizeof(*src)) == 0);
}

TEST_CASE("status ipc: the head never carries more rows than the snapshot holds")
{
	auto src = two_panel_snapshot();
	src->screen_count = 1000;
	src->client_count = 1000;
	src->plugin_count = 1000;
	src->warning_count = 1000;
	xrt_status_head head;
	u_status_snapshot_get_head(src.get(), &head);
	CHECK(head.screen_count == XRT_STATUS_MAX_SCREENS);
	CHECK(head.client_count == XRT_STATUS_MAX_CLIENTS);
	CHECK(head.plugin_count == XRT_STATUS_MAX_PLUGINS);
	CHECK(head.warning_count == XRT_STATUS_MAX_WARNINGS);

	head.screen_count = 1000;
	head.client_count = 1000;
	std::unique_ptr<xrt_status_snapshot> dst(new xrt_status_snapshot);
	u_status_snapshot_set_head(dst.get(), &head);
	CHECK(dst->screen_count == XRT_STATUS_MAX_SCREENS);
	CHECK(dst->client_count == XRT_STATUS_MAX_CLIENTS);
}

TEST_CASE("status ipc: the presenters phase 2 adds have stable spellings")
{
	CHECK(std::strcmp(u_status_presenter_str(XRT_STATUS_PRESENTER_APP_HWND), "APP_HWND") == 0);
	CHECK(std::strcmp(u_status_presenter_str(XRT_STATUS_PRESENTER_CLIENT_TEXTURE), "CLIENT_TEXTURE") == 0);
	CHECK(std::strcmp(u_status_presenter_str(XRT_STATUS_PRESENTER_NONE), "NONE") == 0);
	CHECK(std::strcmp(u_status_presenter_str(XRT_STATUS_PRESENTER_SERVICE_WINDOW), "SERVICE_WINDOW") == 0);
	CHECK(std::strcmp(u_status_presenter_str(XRT_STATUS_PRESENTER_SELF), "SELF") == 0);
}

#ifdef DXR_TESTS_HAVE_IPC_PROTOCOL
TEST_CASE("status ipc: the generated messages round-trip every piece")
{
	// Requests are bounded by the server's receive buffer; replies are
	// fixed-size structs by value (the system_enumerate_displays mechanism).
	STATIC_REQUIRE(sizeof(ipc_system_get_status_snapshot_msg) <= IPC_BUF_SIZE);
	STATIC_REQUIRE(sizeof(ipc_system_get_client_segments_msg) <= IPC_BUF_SIZE);
	// Every piece is a fraction of the whole snapshot.
	STATIC_REQUIRE(sizeof(ipc_system_get_status_snapshot_reply) < sizeof(xrt_status_snapshot) / 2);
	STATIC_REQUIRE(sizeof(ipc_system_get_client_segments_reply) < 4096);

	auto src = two_panel_snapshot();
	std::unique_ptr<xrt_status_snapshot> dst(new xrt_status_snapshot);
	std::memset(dst.get(), 0, sizeof(*dst));

	// The service side of each call: the generated reply struct, as the server
	// fills it; the "wire" is the byte copy the message channel makes.
	for (uint32_t i = 0; i < src->screen_count; i++) {
		ipc_system_get_status_snapshot_msg msg;
		std::memset(&msg, 0, sizeof(msg));
		msg.cmd = IPC_SYSTEM_GET_STATUS_SNAPSHOT;
		msg.screen_index = i;
		ipc_system_get_status_snapshot_msg wire_msg;
		std::memcpy(&wire_msg, &msg, sizeof(msg));
		REQUIRE(wire_msg.screen_index == i);

		std::unique_ptr<ipc_system_get_status_snapshot_reply> reply(new ipc_system_get_status_snapshot_reply);
		std::memset(reply.get(), 0, sizeof(*reply));
		reply->result = XRT_SUCCESS;
		u_status_snapshot_get_head(src.get(), &reply->head);
		reply->screen = src->screens[wire_msg.screen_index];

		std::unique_ptr<ipc_system_get_status_snapshot_reply> wire(new ipc_system_get_status_snapshot_reply);
		std::memcpy(wire.get(), reply.get(), sizeof(*reply));
		if (i == 0) {
			u_status_snapshot_set_head(dst.get(), &wire->head);
		} else {
			// Every piece carries the generation it belongs to.
			REQUIRE(u_status_generation_equal(&wire->head.generation, &dst->generation));
		}
		dst->screens[i] = wire->screen;
	}

	for (uint32_t i = 0; i < dst->client_count; i++) {
		ipc_system_get_client_segments_msg msg;
		std::memset(&msg, 0, sizeof(msg));
		msg.cmd = IPC_SYSTEM_GET_CLIENT_SEGMENTS;
		msg.client_id = dst->clients[i].id;

		ipc_system_get_client_segments_reply reply;
		std::memset(&reply, 0, sizeof(reply));
		reply.result = XRT_SUCCESS;
		reply.generation = src->generation;
		for (uint32_t k = 0; k < src->client_count; k++) {
			if (src->clients[k].id == msg.client_id) {
				reply.client = src->clients[k];
			}
		}
		// The raw segment table rides along for tools that want the metres.
		reply.metrics.count = reply.client.segments.count;
		reply.metrics.generation = reply.client.segments.generation;
		for (uint32_t k = 0; k < reply.metrics.count; k++) {
			reply.metrics.seg[k].screen_id = reply.client.segments.items[k].screen;
			reply.metrics.seg[k].has_dp = true;
			reply.metrics.seg[k].woven = true;
		}

		ipc_system_get_client_segments_reply wire;
		std::memcpy(&wire, &reply, sizeof(reply));
		REQUIRE(u_status_generation_equal(&wire.generation, &dst->generation));
		dst->clients[i] = wire.client;
		CHECK(wire.metrics.count == wire.client.segments.count);
		CHECK(wire.metrics.generation == wire.client.segments.generation);
	}

	CHECK(std::memcmp(dst.get(), src.get(), sizeof(*src)) == 0);

	// And the reassembled snapshot renders the §3 JSON with the service label.
	cJSON *root = u_status_snapshot_to_cjson(dst.get());
	REQUIRE(root != nullptr);
	CHECK(cJSON_GetObjectItem(root, "schema")->valuedouble == 1.0);
	CHECK(std::strcmp(cJSON_GetObjectItem(root, "source")->valuestring, "service") == 0);
	const cJSON *clients = cJSON_GetObjectItem(root, "clients");
	REQUIRE(cJSON_GetArraySize(clients) == 2);
	const cJSON *c0 = cJSON_GetArrayItem(clients, 0);
	CHECK(std::strcmp(cJSON_GetObjectItem(c0, "presenter")->valuestring, "APP_HWND") == 0);
	CHECK(std::strcmp(cJSON_GetObjectItem(c0, "lease")->valuestring, "slot") == 0);
	cJSON_Delete(root);
}
#endif
