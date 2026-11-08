// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2: util/u_share_link (the compositor share tap <->
 *         session service link hand-off) and the portable xbridge share tap
 *         hook surface (comp_xbridge_share_tap), which reports "no tap" on
 *         every platform until the Windows body lands.
 */

#include "catch_amalgamated.hpp"

#include "comp_xbridge_share_tap.h"
#include "util/u_share_link.h"
#include "xrt/xrt_stereo_camera.h"

#include <cstring>

namespace {

struct fake_link
{
	int set_tap_calls = 0;
	bool tap = false;
	uint32_t reason = 0;
	bool luid_valid = false;
	uint8_t luid[XRT_LUID_SIZE] = {};
	int polls = 0;
	int rings = 0;
	int frames = 0;
	uint32_t last_slot = 0;
};

void
fl_set_tap(void *ctx, bool available, uint32_t reason, const uint8_t luid[XRT_LUID_SIZE], bool luid_valid)
{
	auto *l = static_cast<fake_link *>(ctx);
	l->set_tap_calls++;
	l->tap = available;
	l->reason = reason;
	l->luid_valid = luid_valid;
	if (luid_valid) {
		std::memcpy(l->luid, luid, XRT_LUID_SIZE);
	}
}

bool
fl_poll(void *ctx, xrt_share_link_poll *out)
{
	auto *l = static_cast<fake_link *>(ctx);
	l->polls++;
	out->state = XRT_SHARE_LINK_STATE_SHARE_REQUESTED;
	out->eye_w = 1280;
	out->eye_h = 720;
	out->fps = 30.0f;
	out->stream_generation = 4;
	return true;
}

xrt_result_t
fl_publish_ring(void *ctx,
                const xrt_share_link_ring_desc *desc,
                const xrt_graphics_buffer_handle_t *textures,
                uint32_t texture_count,
                xrt_graphics_sync_handle_t fence)
{
	(void)desc;
	(void)textures;
	(void)texture_count;
	(void)fence;
	static_cast<fake_link *>(ctx)->rings++;
	return XRT_SUCCESS;
}

xrt_result_t
fl_frame_ready(void *ctx, const xrt_share_link_frame *frame, xrt_share_link_poll *out_poll)
{
	auto *l = static_cast<fake_link *>(ctx);
	l->frames++;
	l->last_slot = frame->slot;
	if (out_poll != nullptr) {
		out_poll->state = XRT_SHARE_LINK_STATE_SHARE_STOPPED;
	}
	return XRT_SUCCESS;
}

const u_share_link_sink k_sink = {fl_set_tap, fl_poll, fl_publish_ring, fl_frame_ready};

} // namespace

TEST_CASE("u_share_link: no sink = nothing shared", "[share_link]")
{
	u_share_link_reset();
	CHECK_FALSE(u_share_link_installed());
	xrt_share_link_poll p;
	CHECK_FALSE(u_share_link_poll(&p));
	CHECK(p.state == XRT_SHARE_LINK_STATE_NONE);
	xrt_share_link_ring_desc d = {};
	CHECK(u_share_link_publish_ring(&d, nullptr, 0, XRT_GRAPHICS_SYNC_HANDLE_INVALID) ==
	      XRT_ERROR_FEATURE_NOT_SUPPORTED);
	xrt_share_link_frame fr = {};
	CHECK(u_share_link_frame_ready(&fr, &p) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
}

TEST_CASE("u_share_link: forwards to the installed sink, replays the tap state", "[share_link]")
{
	u_share_link_reset();
	// The compositor decides its tap at create, before xrBeginSession installs the sink.
	const uint8_t luid[XRT_LUID_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8};
	u_share_link_set_tap(true, 0, luid, true);
	fake_link l;
	u_share_link_install(&k_sink, &l);
	CHECK(u_share_link_installed());
	CHECK(l.set_tap_calls == 1);
	CHECK(l.tap);
	CHECK(l.luid_valid);
	CHECK(std::memcmp(l.luid, luid, XRT_LUID_SIZE) == 0);

	xrt_share_link_poll p;
	REQUIRE(u_share_link_poll(&p));
	CHECK(p.state == XRT_SHARE_LINK_STATE_SHARE_REQUESTED);
	CHECK(p.stream_generation == 4);
	xrt_share_link_ring_desc d = {};
	CHECK(u_share_link_publish_ring(&d, nullptr, 0, XRT_GRAPHICS_SYNC_HANDLE_INVALID) == XRT_SUCCESS);
	xrt_share_link_frame fr = {};
	fr.slot = 2;
	CHECK(u_share_link_frame_ready(&fr, &p) == XRT_SUCCESS);
	CHECK(p.state == XRT_SHARE_LINK_STATE_SHARE_STOPPED);
	CHECK(l.rings == 1);
	CHECK(l.frames == 1);
	CHECK(l.last_slot == 2);

	// A later tap change reaches the installed sink at once.
	u_share_link_set_tap(false, XRT_APP_SHARE_UNAVAILABLE_SPLIT_DISABLED, nullptr, false);
	CHECK(l.set_tap_calls == 2);
	CHECK_FALSE(l.tap);
	CHECK(l.reason == XRT_APP_SHARE_UNAVAILABLE_SPLIT_DISABLED);

	// Another ctx cannot remove it; its own can, and nothing reaches it after.
	fake_link other;
	u_share_link_uninstall(&other);
	CHECK(u_share_link_installed());
	u_share_link_uninstall(&l);
	CHECK_FALSE(u_share_link_installed());
	CHECK_FALSE(u_share_link_poll(&p));
	CHECK(l.polls == 1);
	u_share_link_reset();
}

TEST_CASE("xbridge share tap: the portable surface reports no tap", "[share_link]")
{
	u_share_link_reset();
	fake_link l;
	u_share_link_install(&k_sink, &l);

	comp_xbridge_share_tap_info info = {};
	info.tier = XRT_SHARE_LINK_TIER_D3D11;
	comp_xbridge_share_tap *tap = reinterpret_cast<comp_xbridge_share_tap *>(0x1);
	CHECK(xbridge_share_tap_begin(&info, &tap) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(tap == nullptr);
	CHECK_FALSE(l.tap);
	CHECK(l.reason == XRT_APP_SHARE_UNAVAILABLE_NO_TAP);

	// A fill arm that already knows it declines passes its reason through.
	info.unavailable_reason = XRT_APP_SHARE_UNAVAILABLE_LEGACY_STANDALONE;
	CHECK(xbridge_share_tap_begin(&info, &tap) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(l.reason == XRT_APP_SHARE_UNAVAILABLE_LEGACY_STANDALONE);

	comp_xbridge_share_tap_frame frame = {};
	CHECK(xbridge_share_tap_commit(nullptr, &frame) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(l.frames == 0);
	xbridge_share_tap_end(&tap);
	CHECK(tap == nullptr);
	CHECK(l.reason == XRT_APP_SHARE_UNAVAILABLE_NO_TAP);
	u_share_link_uninstall(&l);
	u_share_link_reset();
}

TEST_CASE("u_share_link: set_tap without an adapter clears a stale one", "[share_link]")
{
	u_share_link_reset();
	const uint8_t luid[XRT_LUID_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8};
	u_share_link_set_tap(true, 0, luid, true);
	u_share_link_set_tap(false, XRT_APP_SHARE_UNAVAILABLE_OTHER_ADAPTER, nullptr, false);
	fake_link l;
	u_share_link_install(&k_sink, &l); // the replay carries the CURRENT state
	CHECK_FALSE(l.luid_valid);
	CHECK(l.reason == XRT_APP_SHARE_UNAVAILABLE_OTHER_ADAPTER);
	u_share_link_uninstall(&l);
	u_share_link_reset();
}

TEST_CASE("u_share_link poll cache: SHARE_STOPPED is reported exactly once", "[share_link]")
{
	u_share_link_poll_cache c;
	u_share_link_poll_cache_reset(&c);
	xrt_share_link_poll out;
	CHECK_FALSE(u_share_link_poll_cache_take(&c, &out));

	xrt_share_link_poll req = {};
	req.state = XRT_SHARE_LINK_STATE_SHARE_REQUESTED;
	req.stream_generation = 3;
	u_share_link_poll_cache_merge(&c, &req);
	REQUIRE(u_share_link_poll_cache_take(&c, &out));
	CHECK(out.state == XRT_SHARE_LINK_STATE_SHARE_REQUESTED);
	REQUIRE(u_share_link_poll_cache_take(&c, &out)); // REQUESTED persists
	CHECK(out.state == XRT_SHARE_LINK_STATE_SHARE_REQUESTED);

	// A STOPPED (say, a frame_ready reply) followed by the poll's NONE before
	// anyone read it: the STOPPED survives, and is taken once.
	xrt_share_link_poll stop = {};
	stop.state = XRT_SHARE_LINK_STATE_SHARE_STOPPED;
	stop.stream_generation = 3;
	xrt_share_link_poll none = {};
	u_share_link_poll_cache_merge(&c, &stop);
	u_share_link_poll_cache_merge(&c, &none);
	REQUIRE(u_share_link_poll_cache_take(&c, &out));
	CHECK(out.state == XRT_SHARE_LINK_STATE_SHARE_STOPPED);
	REQUIRE(u_share_link_poll_cache_take(&c, &out));
	CHECK(out.state == XRT_SHARE_LINK_STATE_NONE);

	// The same STOPPED arriving again (poll AND a frame_ready reply) after it
	// was taken is not a second report.
	u_share_link_poll_cache_merge(&c, &stop);
	REQUIRE(u_share_link_poll_cache_take(&c, &out));
	CHECK(out.state == XRT_SHARE_LINK_STATE_NONE);

	// A new stream supersedes an unread STOPPED; its own stop is reported.
	stop.stream_generation = 4;
	u_share_link_poll_cache_merge(&c, &stop);
	req.stream_generation = 5;
	u_share_link_poll_cache_merge(&c, &req);
	REQUIRE(u_share_link_poll_cache_take(&c, &out));
	CHECK(out.state == XRT_SHARE_LINK_STATE_SHARE_REQUESTED);
	CHECK(out.stream_generation == 5);
	stop.stream_generation = 5;
	u_share_link_poll_cache_merge(&c, &stop);
	REQUIRE(u_share_link_poll_cache_take(&c, &out));
	CHECK(out.state == XRT_SHARE_LINK_STATE_SHARE_STOPPED);
}
