// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042): the per-stream mailbox / output-ring state
 *         machine the D3D11 service's lift thread runs on.
 *
 * What must hold, because the weave and IPC threads are built on it:
 *
 *  - the PRODUCER never waits: with one frame converting there is always a
 *    writable input slot;
 *  - LATEST WINS: a pending frame superseded before the worker takes it is
 *    dropped and counted, never queued behind the newer one;
 *  - frame ids are per-stream, monotonic from 1, and an aborted snapshot
 *    consumes none;
 *  - the worker never writes the LATEST result nor a PINNED slot (a consumer is
 *    mid-copy), and waits (begin_output false) rather than breaking that;
 *  - a "newer-than" acquire returns each result at most once; the weave's
 *    plain pin always sees the latest;
 *  - sourceTime is echoed verbatim and latency is submit → publish.
 */

#include "catch_amalgamated.hpp"

#include "util/u_lift_mailbox.h"

#include "sim_display_fake_ply.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

//! Submit one frame end to end on the producer side; returns its frame id.
uint64_t
submit(u_lift_mailbox &mb, int64_t source_time, uint64_t now_ns)
{
	int32_t slot = -1;
	REQUIRE(u_lift_mailbox_begin_submit(&mb, &slot));
	return u_lift_mailbox_commit_submit(&mb, slot, source_time, now_ns, 640, 360);
}

//! Worker: convert whatever is pending into an output slot at @p done_ns.
bool
convert(u_lift_mailbox &mb, uint64_t start_ns, uint64_t done_ns, u_lift_frame_meta *out = nullptr)
{
	int32_t in = -1;
	u_lift_frame_meta meta = {};
	if (!u_lift_mailbox_take_pending(&mb, start_ns, &in, &meta)) {
		return false;
	}
	int32_t o = -1;
	REQUIRE(u_lift_mailbox_begin_output(&mb, &o));
	u_lift_mailbox_finish_input(&mb, in);
	u_lift_mailbox_publish_output(&mb, o, &meta, done_ns);
	if (out != nullptr) {
		*out = meta;
	}
	return true;
}

} // namespace

TEST_CASE("lift mailbox: frame ids start at 1 and are monotonic", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);
	CHECK_FALSE(u_lift_mailbox_has_pending(&mb));
	CHECK_FALSE(u_lift_mailbox_has_result(&mb));
	CHECK(submit(mb, 100, 1000) == 1);
	CHECK(submit(mb, 200, 2000) == 2);
	CHECK(submit(mb, 300, 3000) == 3);
	CHECK(mb.submitted == 3);
}

TEST_CASE("lift mailbox: latest wins — an unconverted frame is dropped, not queued", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);
	submit(mb, 10, 1000);
	submit(mb, 20, 2000);
	submit(mb, 30, 3000);
	CHECK(mb.dropped == 2);

	int32_t in = -1;
	u_lift_frame_meta meta = {};
	REQUIRE(u_lift_mailbox_take_pending(&mb, 3500, &in, &meta));
	CHECK(meta.frame_id == 3);
	CHECK(meta.source_time == 30);
	CHECK(meta.convert_start_ns == 3500);
	// Nothing older is left behind.
	CHECK_FALSE(u_lift_mailbox_has_pending(&mb));
}

TEST_CASE("lift mailbox: the producer never blocks while a frame converts", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);
	submit(mb, 1, 100);
	int32_t in = -1;
	REQUIRE(u_lift_mailbox_take_pending(&mb, 150, &in, nullptr));

	// Many submits while one frame is converting: each one gets a slot, the
	// converting slot is never handed out, and only the newest stays pending.
	for (int i = 0; i < 10; i++) {
		int32_t s = -1;
		REQUIRE(u_lift_mailbox_begin_submit(&mb, &s));
		CHECK(s != in);
		u_lift_mailbox_commit_submit(&mb, s, 2 + i, 200 + i, 1, 1);
	}
	CHECK(mb.dropped == 9);
	u_lift_mailbox_finish_input(&mb, in);

	u_lift_frame_meta meta = {};
	REQUIRE(u_lift_mailbox_take_pending(&mb, 500, &in, &meta));
	CHECK(meta.frame_id == 11);
	CHECK(meta.source_time == 11);
}

TEST_CASE("lift mailbox: an aborted snapshot consumes no frame id", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);
	int32_t s = -1;
	REQUIRE(u_lift_mailbox_begin_submit(&mb, &s));
	u_lift_mailbox_abort_submit(&mb, s);
	CHECK_FALSE(u_lift_mailbox_has_pending(&mb));
	CHECK(submit(mb, 5, 50) == 1);
}

TEST_CASE("lift mailbox: overwriting the pending slot counts one drop", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);
	submit(mb, 1, 10); // pending in one slot
	int32_t conv = -1;
	REQUIRE(u_lift_mailbox_take_pending(&mb, 11, &conv, nullptr)); // converting
	submit(mb, 2, 20);                                              // pending in the other
	CHECK(mb.dropped == 0);

	// Both slots are busy (converting + pending): the next begin must take the
	// PENDING one, counting its frame as dropped.
	int32_t s = -1;
	REQUIRE(u_lift_mailbox_begin_submit(&mb, &s));
	CHECK(s != conv);
	CHECK(mb.dropped == 1);
	CHECK(u_lift_mailbox_commit_submit(&mb, s, 3, 30, 1, 1) == 3);
}

TEST_CASE("lift mailbox: results, newer-than acquire, sourceTime echo, latency", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);
	int32_t slot = -1;
	CHECK_FALSE(u_lift_mailbox_pin_latest(&mb, true, &slot, nullptr));
	CHECK_FALSE(u_lift_mailbox_pin_latest(&mb, false, &slot, nullptr));

	submit(mb, 777, 1000);
	REQUIRE(convert(mb, 1100, 1000 + 25));

	u_lift_frame_meta m = {};
	REQUIRE(u_lift_mailbox_pin_latest(&mb, true, &slot, &m));
	CHECK(m.frame_id == 1);
	CHECK(m.source_time == 777);
	CHECK(m.done_ns - m.submit_ns == 25);
	CHECK(mb.lat_last_ns == 25);
	u_lift_mailbox_unpin(&mb, slot);

	// Same result again: a newer-than acquire says NOT READY...
	CHECK_FALSE(u_lift_mailbox_pin_latest(&mb, true, &slot, nullptr));
	// ...the weave's plain pin still gets it.
	REQUIRE(u_lift_mailbox_pin_latest(&mb, false, &slot, &m));
	CHECK(m.frame_id == 1);
	u_lift_mailbox_unpin(&mb, slot);

	submit(mb, 888, 2000);
	REQUIRE(convert(mb, 2010, 2000 + 45));
	REQUIRE(u_lift_mailbox_pin_latest(&mb, true, &slot, &m));
	CHECK(m.frame_id == 2);
	CHECK(m.source_time == 888);
	u_lift_mailbox_unpin(&mb, slot);

	CHECK(mb.converted == 2);
	CHECK(mb.lat_min_ns == 25);
	CHECK(mb.lat_max_ns == 45);
	CHECK(mb.lat_ema_ns > 25);
	CHECK(mb.lat_ema_ns < 45);
}

TEST_CASE("lift mailbox: the worker never overwrites the latest or a pinned slot", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);

	submit(mb, 1, 10);
	REQUIRE(convert(mb, 11, 12)); // result A = latest
	int32_t a = mb.latest;

	// A consumer pins A (the weave is copying it).
	int32_t pinned = -1;
	REQUIRE(u_lift_mailbox_pin_latest(&mb, false, &pinned, nullptr));
	CHECK(pinned == a);

	// Worker publishes B into the other slot — allowed.
	submit(mb, 2, 20);
	REQUIRE(convert(mb, 21, 22));
	int32_t b = mb.latest;
	CHECK(b != a);

	// Next result: the only non-latest slot is A, still pinned → must wait.
	int32_t o = -1;
	CHECK_FALSE(u_lift_mailbox_begin_output(&mb, &o));

	// Unpin → A becomes writable, B stays untouched.
	u_lift_mailbox_unpin(&mb, pinned);
	REQUIRE(u_lift_mailbox_begin_output(&mb, &o));
	CHECK(o == a);
	// ...and while A is being written, a consumer still reads B.
	int32_t r = -1;
	u_lift_frame_meta m = {};
	REQUIRE(u_lift_mailbox_pin_latest(&mb, false, &r, &m));
	CHECK(r == b);
	CHECK(m.frame_id == 2);
	u_lift_mailbox_unpin(&mb, r);
}

TEST_CASE("lift mailbox: an abandoned conversion leaves the previous result in place", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);
	submit(mb, 1, 10);
	REQUIRE(convert(mb, 11, 12));
	int32_t first = mb.latest;

	submit(mb, 2, 20);
	int32_t in = -1;
	REQUIRE(u_lift_mailbox_take_pending(&mb, 21, &in, nullptr));
	int32_t o = -1;
	REQUIRE(u_lift_mailbox_begin_output(&mb, &o));
	u_lift_mailbox_finish_input(&mb, in);
	u_lift_mailbox_abort_output(&mb, o);

	CHECK(mb.failed == 1);
	CHECK(mb.latest == first);
	CHECK(u_lift_mailbox_has_result(&mb));
	int32_t r = -1;
	u_lift_frame_meta m = {};
	REQUIRE(u_lift_mailbox_pin_latest(&mb, false, &r, &m));
	CHECK(m.frame_id == 1);
	u_lift_mailbox_unpin(&mb, r);
}

TEST_CASE("lift mailbox: out-of-range slots from the wire are inert", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);
	CHECK(u_lift_mailbox_commit_submit(&mb, 7, 0, 0, 1, 1) == 0);
	CHECK(u_lift_mailbox_commit_submit(&mb, -1, 0, 0, 1, 1) == 0);
	u_lift_mailbox_abort_submit(&mb, 99);
	u_lift_mailbox_finish_input(&mb, -3);
	u_lift_mailbox_unpin(&mb, 5);
	u_lift_frame_meta meta = {};
	u_lift_mailbox_publish_output(&mb, 9, &meta, 0);
	u_lift_mailbox_abort_output(&mb, 9);
	CHECK(mb.submitted == 0);
	CHECK(mb.latest == -1);
	// Committing a slot that was never begun is refused too.
	CHECK(u_lift_mailbox_commit_submit(&mb, 0, 0, 0, 1, 1) == 0);
}

/*
 *
 * Cross-stream scheduling (XrLiftPriorityDXR).
 *
 */

namespace {

std::vector<uint64_t>
plan(u_lift_sched &s, const std::vector<u_lift_sched_entry> &e)
{
	uint64_t out[32] = {};
	uint32_t n = u_lift_sched_plan(&s, e.data(), (uint32_t)e.size(), out, 32);
	return std::vector<uint64_t>(out, out + n);
}

} // namespace

TEST_CASE("lift sched: HIGH every round, NORMAL round-robin one per round", "[lift][sched]")
{
	u_lift_sched s;
	u_lift_sched_init(&s);
	// A call: the active speaker HIGH, three other tiles NORMAL.
	std::vector<u_lift_sched_entry> e = {
	    {1, U_LIFT_PRIORITY_NORMAL, true},
	    {2, U_LIFT_PRIORITY_HIGH, true},
	    {3, U_LIFT_PRIORITY_NORMAL, true},
	    {4, U_LIFT_PRIORITY_NORMAL, true},
	};
	CHECK(plan(s, e) == std::vector<uint64_t>{2, 1});
	CHECK(plan(s, e) == std::vector<uint64_t>{2, 3});
	CHECK(plan(s, e) == std::vector<uint64_t>{2, 4});
	CHECK(plan(s, e) == std::vector<uint64_t>{2, 1}); // wraps
}

TEST_CASE("lift sched: only streams with a new frame are planned", "[lift][sched]")
{
	u_lift_sched s;
	u_lift_sched_init(&s);
	std::vector<u_lift_sched_entry> e = {
	    {1, U_LIFT_PRIORITY_HIGH, false},
	    {2, U_LIFT_PRIORITY_NORMAL, false},
	    {3, U_LIFT_PRIORITY_NORMAL, true},
	};
	CHECK(plan(s, e) == std::vector<uint64_t>{3});
	e[2].pending = false;
	CHECK(plan(s, e).empty());
	CHECK(s.round == 1); // an idle service does not advance rounds
}

TEST_CASE("lift sched: LOW every 4th round, PAUSED never", "[lift][sched]")
{
	u_lift_sched s;
	u_lift_sched_init(&s);
	std::vector<u_lift_sched_entry> e = {
	    {1, U_LIFT_PRIORITY_NORMAL, true},
	    {2, U_LIFT_PRIORITY_LOW, true},
	    {3, U_LIFT_PRIORITY_PAUSED, true},
	};
	int low_hits = 0;
	for (int r = 0; r < 16; r++) {
		auto p = plan(s, e);
		for (uint64_t id : p) {
			CHECK(id != 3);
			low_hits += id == 2 ? 1 : 0;
		}
		CHECK(std::find(p.begin(), p.end(), 1) != p.end()); // NORMAL every round
	}
	CHECK(low_hits == 16 / U_LIFT_LOW_EVERY_N);

	// Only a PAUSED stream pending: nothing, and no round consumed.
	std::vector<u_lift_sched_entry> paused = {{3, U_LIFT_PRIORITY_PAUSED, true}};
	uint64_t before = s.round;
	CHECK(plan(s, paused).empty());
	CHECK(s.round == before);
}

TEST_CASE("lift mailbox: effective rate from publish intervals", "[lift]")
{
	u_lift_mailbox mb;
	u_lift_mailbox_init(&mb);
	CHECK(u_lift_mailbox_rate_hz(&mb) == 0.0f);
	uint64_t t = 1000;
	for (int i = 0; i < 20; i++) {
		submit(mb, i, t);
		REQUIRE(convert(mb, t + 1, t + 2));
		t += 40 * 1000 * 1000; // 25 Hz
	}
	CHECK(u_lift_mailbox_rate_hz(&mb) == Catch::Approx(25.0f).epsilon(0.01));
}

TEST_CASE("sim fake lift: the splat PLY is a valid two-layer 3DGS binary PLY", "[lift][ply]")
{
	const uint32_t w = 64, h = 48;
	std::vector<uint8_t> rgba(w * h * 4, 0);
	for (uint32_t i = 0; i < w * h; i++) {
		rgba[i * 4 + 0] = 255; // red photo
		rgba[i * 4 + 3] = 255;
	}
	size_t need = sim_fake_ply_write(nullptr, 0, rgba.data(), w, h, w * 4);
	REQUIRE(need > 0);
	std::vector<uint8_t> ply(need);
	CHECK(sim_fake_ply_write(ply.data(), ply.size() - 1, rgba.data(), w, h, w * 4) == need); // too small: nothing
	CHECK(sim_fake_ply_write(ply.data(), ply.size(), rgba.data(), w, h, w * 4) == need);

	std::string text(reinterpret_cast<const char *>(ply.data()), std::min<size_t>(ply.size(), 1024));
	REQUIRE(text.rfind("ply\nformat binary_little_endian 1.0\n", 0) == 0);
	CHECK(text.find("element vertex " + std::to_string(SIM_FAKE_PLY_SPLATS) + "\n") != std::string::npos);
	for (const char *prop : {"property float f_dc_0\n", "property float opacity\n", "property float rot_3\n"}) {
		CHECK(text.find(prop) != std::string::npos);
	}
	size_t hdr_end = text.find("end_header\n");
	REQUIRE(hdr_end != std::string::npos);
	hdr_end += strlen("end_header\n");
	CHECK(ply.size() - hdr_end == (size_t)SIM_FAKE_PLY_SPLATS * SIM_FAKE_PLY_FLOATS_PER_SPLAT * 4);
	CHECK(SIM_FAKE_PLY_SPLATS >= 200); // "a few hundred splats"

	// First splat: front layer (z = 0), red dominant in its SH DC term.
	float f[SIM_FAKE_PLY_FLOATS_PER_SPLAT];
	memcpy(f, ply.data() + hdr_end, sizeof(f));
	CHECK(f[2] == 0.0f);
	CHECK(f[6] > f[7]);
	CHECK(f[6] > f[8]);
	// Last splat: back layer, behind the front one.
	memcpy(f, ply.data() + ply.size() - sizeof(f), sizeof(f));
	CHECK(f[2] > 0.0f);
}
