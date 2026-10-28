// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2305: a live session's home screen across display hotplugs
 *         (u_screen_home) — the SYSTEM_DEFAULT flag the D3D11 / D3D12 / GL /
 *         Vulkan segment managers read as "the screen the session's own DP
 *         weaves" never moves off the panel that DP was made for.
 */

#include "catch_amalgamated.hpp"

#include "util/u_screen_home.h"

#include <cstdio>
#include <cstring>

namespace {

void
add(xrt_screen_list &l, uint64_t id, const char *plugin, int32_t left, bool is_default)
{
	xrt_screen &s = l.screens[l.count++];
	std::memset(&s, 0, sizeof(s));
	s.id = id;
	s.desktop_left = left;
	s.desktop_width = 1920;
	s.desktop_height = 1080;
	std::snprintf(s.plugin_id, sizeof(s.plugin_id), "%s", plugin);
	s.flags = is_default ? XRT_SCREEN_FLAG_SYSTEM_DEFAULT : 0u;
}

uint32_t
defaults(const xrt_screen_list &l, uint64_t *out_id)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < l.count; i++) {
		if ((l.screens[i].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0) {
			n++;
			*out_id = l.screens[i].id;
		}
	}
	return n;
}

constexpr uint64_t LAPTOP = 0x1001;
constexpr uint64_t PANEL = 0x2002;
constexpr uint64_t PANEL2 = 0x2003;
constexpr uint64_t VIRTUAL = 0x3003; // a casting tool's "extend" monitor: no EDID, nobody's claim

} // namespace

TEST_CASE("screen home: recorded from the first list's system default")
{
	xrt_screen_list l{};
	add(l, LAPTOP, "sim-display", 0, false);
	add(l, PANEL, "vendor", 1920, true);
	u_screen_home h{};
	u_screen_home_record(&h, &l);
	CHECK(h.valid);
	CHECK(h.screen_id == PANEL);
	CHECK(std::strcmp(h.plugin_id, "vendor") == 0);

	xrt_screen_list none{};
	add(none, LAPTOP, "sim-display", 0, false);
	u_screen_home_record(&h, &none);
	CHECK_FALSE(h.valid);
	CHECK(u_screen_home_anchor(&h, &l) == U_SCREEN_HOME_UNRECORDED);
}

TEST_CASE("screen home: a virtual monitor appearing and disappearing never takes the session DP's screen")
{
	xrt_screen_list first{};
	add(first, LAPTOP, "sim-display", 0, false);
	add(first, PANEL, "vendor", 1920, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	// Appears: listed, unclaimed (sim-display's fallback), and — as the
	// worst case — flagged system default by the screen builder.
	xrt_screen_list with_virtual{};
	add(with_virtual, VIRTUAL, "sim-display", 5760, true);
	add(with_virtual, LAPTOP, "sim-display", 0, false);
	add(with_virtual, PANEL, "vendor", 1920, false);
	CHECK(u_screen_home_anchor(&h, &with_virtual) == U_SCREEN_HOME_REANCHORED);
	uint64_t def = 0;
	CHECK(defaults(with_virtual, &def) == 1);
	CHECK(def == PANEL);

	// Disappears: back to the first list, untouched.
	xrt_screen_list back = first;
	CHECK(u_screen_home_anchor(&h, &back) == U_SCREEN_HOME_IN_PLACE);
	CHECK(std::memcmp(&back, &first, sizeof(back)) == 0);
}

TEST_CASE("screen home: a second claimed panel plugged in does not move the session DP")
{
	xrt_screen_list first{};
	add(first, PANEL, "vendor", 0, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	xrt_screen_list two{};
	add(two, PANEL2, "vendor", 3840, true);
	add(two, PANEL, "vendor", 0, false);
	CHECK(u_screen_home_anchor(&h, &two) == U_SCREEN_HOME_REANCHORED);
	uint64_t def = 0;
	REQUIRE(defaults(two, &def) == 1);
	CHECK(def == PANEL);
}

TEST_CASE("screen home: the panel unplugged — no default at all, so no segment DP for it; replugged — restored")
{
	xrt_screen_list first{};
	add(first, LAPTOP, "sim-display", 0, false);
	add(first, PANEL, "vendor", 1920, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	// Unplugged; a virtual monitor now sits where the panel was.
	xrt_screen_list gone{};
	add(gone, LAPTOP, "sim-display", 0, false);
	add(gone, VIRTUAL, "sim-display", 1920, true);
	CHECK(u_screen_home_anchor(&h, &gone) == U_SCREEN_HOME_GONE);
	uint64_t def = 0;
	CHECK(defaults(gone, &def) == 0);

	// Replugged (same screen id, same plug-in): home again.
	xrt_screen_list replug{};
	add(replug, LAPTOP, "sim-display", 0, true);
	add(replug, PANEL, "vendor", 1920, false);
	CHECK(u_screen_home_anchor(&h, &replug) == U_SCREEN_HOME_REANCHORED);
	REQUIRE(defaults(replug, &def) == 1);
	CHECK(def == PANEL);
}

TEST_CASE("screen home: the panel re-claimed by another plug-in is not the session DP's any more")
{
	xrt_screen_list first{};
	add(first, PANEL, "vendor", 0, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	// The vendor service stopped: sim-display's fallback claim wins the monitor.
	xrt_screen_list reclaimed{};
	add(reclaimed, PANEL, "sim-display", 0, true);
	CHECK(u_screen_home_anchor(&h, &reclaimed) == U_SCREEN_HOME_RECLAIMED);
	uint64_t def = 0;
	CHECK(defaults(reclaimed, &def) == 0);
	CHECK(std::strcmp(u_screen_home_outcome_str(U_SCREEN_HOME_RECLAIMED), "re-claimed") == 0);
}

/*
 * #2305, runtime #2311 on Windows hardware (2026-10-27): with the panel
 * unplugged the re-push took the window off the laptop's segment DP and gave
 * it back to the session DP of the missing panel. When the home is gone AND
 * its plug-in wins no screen, the session keeps the compositor's last list
 * for a bounded wait; when the plug-in still wins one (the panel
 * re-identified under another id, a second panel), the re-push goes ahead
 * with no default, as before.
 */
TEST_CASE("screen home: gone with its plug-in unlisted (unplugged) vs gone but re-identified")
{
	xrt_screen_list first{};
	add(first, LAPTOP, "sim-display", 0, false);
	add(first, PANEL, "vendor", -1920, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	// Unplugged: only the laptop. Gone, and nothing is the vendor's.
	xrt_screen_list unplugged{};
	add(unplugged, LAPTOP, "sim-display", 0, true);
	CHECK(u_screen_home_anchor(&h, &unplugged) == U_SCREEN_HOME_GONE);
	CHECK(u_screen_home_plugin_unlisted(&h, &unplugged));

	// A virtual monitor took the panel's place: still nothing of the vendor's.
	xrt_screen_list virt{};
	add(virt, LAPTOP, "sim-display", 0, false);
	add(virt, VIRTUAL, "sim-display", -1920, true);
	CHECK(u_screen_home_plugin_unlisted(&h, &virt));

	// Mid-replug, Windows shows the panel alone at (0, 0) under a new,
	// position-derived id: gone by id, but the vendor wins a screen.
	xrt_screen_list transient{};
	add(transient, PANEL2, "vendor", 0, true);
	CHECK(u_screen_home_anchor(&h, &transient) == U_SCREEN_HOME_GONE);
	CHECK_FALSE(u_screen_home_plugin_unlisted(&h, &transient));

	// Back in place, or re-claimed by another plug-in: not "unlisted" (the
	// vendor's own screen is there, or the home id is listed at all).
	xrt_screen_list back{};
	add(back, LAPTOP, "sim-display", 0, false);
	add(back, PANEL, "vendor", -1920, true);
	CHECK_FALSE(u_screen_home_plugin_unlisted(&h, &back));

	// No home recorded, no list: never.
	u_screen_home none{};
	CHECK_FALSE(u_screen_home_plugin_unlisted(&none, &unplugged));
	CHECK_FALSE(u_screen_home_plugin_unlisted(&h, nullptr));
}

/*
 * #2305: the session DP may be another screen's from the first list handed on
 * with the home unplugged (the bound's push) until a list has the home back —
 * the same span as the compositors' away_screen_id.
 */
TEST_CASE("screen home: the session DP may be another screen's from an unplugged list until the home is back")
{
	xrt_screen_list first{};
	add(first, LAPTOP, "sim-display", 0, false);
	add(first, PANEL, "vendor", -1920, true);
	u_screen_home h{};
	u_screen_home_record(&h, &first);

	// The bound's push: the laptop alone, the panel unplugged.
	xrt_screen_list unplugged{};
	add(unplugged, LAPTOP, "sim-display", 0, true);
	bool away = u_screen_home_away_next(false, u_screen_home_anchor(&h, &unplugged),
	                                    u_screen_home_plugin_unlisted(&h, &unplugged));
	CHECK(away);

	// Another list while it is still unplugged: still away.
	away = u_screen_home_away_next(away, u_screen_home_anchor(&h, &unplugged),
	                               u_screen_home_plugin_unlisted(&h, &unplugged));
	CHECK(away);

	// The panel listed alone under a new id: nothing hands the window back,
	// so the slot's DP stands.
	xrt_screen_list alone{};
	add(alone, PANEL2, "vendor", 0, true);
	CHECK(u_screen_home_away_next(away, u_screen_home_anchor(&h, &alone),
	                              u_screen_home_plugin_unlisted(&h, &alone)));

	// The home back: the first update gives it its own DP again.
	xrt_screen_list back{};
	add(back, LAPTOP, "sim-display", 0, false);
	add(back, PANEL, "vendor", -1920, true);
	CHECK_FALSE(
	    u_screen_home_away_next(away, u_screen_home_anchor(&h, &back), u_screen_home_plugin_unlisted(&h, &back)));
	CHECK_FALSE(u_screen_home_away_next(true, U_SCREEN_HOME_REANCHORED, false));

	// A re-claimed panel never starts it; nothing does without a home.
	CHECK_FALSE(u_screen_home_away_next(false, U_SCREEN_HOME_RECLAIMED, false));
	CHECK_FALSE(u_screen_home_away_next(false, U_SCREEN_HOME_GONE, false));
	CHECK_FALSE(u_screen_home_away_next(true, U_SCREEN_HOME_UNRECORDED, false));
}

namespace {

constexpr uint64_t kSec = 1000ull * 1000ull * 1000ull;
constexpr uint64_t kBound = U_SCREEN_HOME_KEEP_BOUND_NS;

//! The rig of 2026-10-27: the panel left of the laptop, the laptop at the origin.
void
rig(xrt_screen_list &last, u_screen_home &h)
{
	last = xrt_screen_list{};
	add(last, PANEL, "vendor", -1920, true);
	add(last, LAPTOP, "sim-display", 0, false);
	h = u_screen_home{};
	u_screen_home_record(&h, &last);
}

u_screen_home_keep
decide(const u_screen_home &h,
       xrt_screen_list now,
       const xrt_screen_list *last,
       bool keeping,
       uint64_t since_ns,
       uint64_t now_ns)
{
	const u_screen_home_outcome o = u_screen_home_anchor(&h, &now);
	return u_screen_home_keep_decide(&h, o, &now, last, keeping, since_ns, now_ns, kBound);
}

} // namespace

TEST_CASE("screen home: the bound is the #266 move-back window (60 s)")
{
	CHECK(U_SCREEN_HOME_KEEP_BOUND_NS == 60 * kSec);
}

TEST_CASE("screen home: an unplug keeps the last list within the bound, then the A8 rule")
{
	xrt_screen_list last{};
	u_screen_home h{};
	rig(last, h);

	// Unplugged; the laptop stays at (0, 0).
	xrt_screen_list unplugged{};
	add(unplugged, LAPTOP, "sim-display", 0, true);

	const uint64_t t0 = 100 * kSec;
	// The unplug's generation: the keep starts.
	CHECK(decide(h, unplugged, &last, false, 0, t0) == U_SCREEN_HOME_KEEP_START);
	// Any re-push inside the bound (a follow-up sweep's generation, or the
	// session's own check): still kept.
	CHECK(decide(h, unplugged, &last, true, t0, t0 + 1) == U_SCREEN_HOME_KEEP_HOLD);
	CHECK(decide(h, unplugged, &last, true, t0, t0 + 30 * kSec) == U_SCREEN_HOME_KEEP_HOLD);
	CHECK(decide(h, unplugged, &last, true, t0, t0 + kBound - 1) == U_SCREEN_HOME_KEEP_HOLD);
	// The bound passed with the panel still out: the list goes through (A8).
	CHECK(decide(h, unplugged, &last, true, t0, t0 + kBound) == U_SCREEN_HOME_KEEP_EXPIRED);
	CHECK(decide(h, unplugged, &last, true, t0, t0 + 10 * kBound) == U_SCREEN_HOME_KEEP_EXPIRED);

	// After it went through, the compositor's list no longer has the home:
	// a later re-push with the panel still out keeps nothing (no second keep).
	xrt_screen_list pushed = unplugged;
	(void)u_screen_home_anchor(&h, &pushed);
	CHECK(decide(h, unplugged, &pushed, false, 0, t0 + 2 * kBound) == U_SCREEN_HOME_KEEP_NO);

	// A clock that went backwards is no time passed.
	CHECK(decide(h, unplugged, &last, true, t0, t0 - 5) == U_SCREEN_HOME_KEEP_HOLD);
}

TEST_CASE("screen home: a replug within the bound ends the keep")
{
	xrt_screen_list last{};
	u_screen_home h{};
	rig(last, h);
	const uint64_t t0 = 7 * kSec;

	// The panel is back at its rect (a < 1 s re-seat): in place, push.
	xrt_screen_list back{};
	add(back, PANEL, "vendor", -1920, false);
	add(back, LAPTOP, "sim-display", 0, true);
	CHECK(decide(h, back, &last, true, t0, t0 + kSec / 2) == U_SCREEN_HOME_KEEP_NO);

	// Mid-replug, Windows lists the panel alone at (0, 0) under a new id:
	// the vendor wins a screen again, push (as before).
	xrt_screen_list transient{};
	add(transient, PANEL2, "vendor", 0, true);
	CHECK(decide(h, transient, &last, true, t0, t0 + kSec) == U_SCREEN_HOME_KEEP_NO);
}

TEST_CASE("screen home: a new monitor or a moved layout ends the keep")
{
	xrt_screen_list last{};
	u_screen_home h{};
	rig(last, h);
	const uint64_t t0 = 3 * kSec;

	// A monitor the last list never had (plugged while the panel is out).
	xrt_screen_list added{};
	add(added, LAPTOP, "sim-display", 0, true);
	add(added, VIRTUAL, "sim-display", 1920, false);
	CHECK(decide(h, added, &last, false, 0, t0) == U_SCREEN_HOME_KEEP_NO);
	CHECK(decide(h, added, &last, true, t0, t0 + kSec) == U_SCREEN_HOME_KEEP_NO);

	// The panel was at the origin and the laptop shifted onto it: the kept
	// rects would be stale.
	xrt_screen_list shifted{};
	add(shifted, LAPTOP, "sim-display", -1920, true);
	CHECK(decide(h, shifted, &last, false, 0, t0) == U_SCREEN_HOME_KEEP_NO);
	CHECK(decide(h, shifted, &last, true, t0, t0 + kSec) == U_SCREEN_HOME_KEEP_NO);

	// The laptop changed size (a mode change) while the panel is out.
	xrt_screen_list resized{};
	add(resized, LAPTOP, "sim-display", 0, true);
	resized.screens[0].desktop_width = 1280;
	CHECK(decide(h, resized, &last, true, t0, t0 + kSec) == U_SCREEN_HOME_KEEP_NO);
}

TEST_CASE("screen home: never kept without a gone home, a recorded home or a last list")
{
	xrt_screen_list last{};
	u_screen_home h{};
	rig(last, h);
	xrt_screen_list unplugged{};
	add(unplugged, LAPTOP, "sim-display", 0, true);

	// Outcomes other than gone.
	CHECK(u_screen_home_keep_decide(&h, U_SCREEN_HOME_RECLAIMED, &unplugged, &last, false, 0, 1, kBound) ==
	      U_SCREEN_HOME_KEEP_NO);
	CHECK(u_screen_home_keep_decide(&h, U_SCREEN_HOME_IN_PLACE, &unplugged, &last, false, 0, 1, kBound) ==
	      U_SCREEN_HOME_KEEP_NO);
	CHECK(u_screen_home_keep_decide(&h, U_SCREEN_HOME_UNRECORDED, &unplugged, &last, false, 0, 1, kBound) ==
	      U_SCREEN_HOME_KEEP_NO);
	// No list handed yet.
	CHECK(u_screen_home_keep_decide(&h, U_SCREEN_HOME_GONE, &unplugged, nullptr, false, 0, 1, kBound) ==
	      U_SCREEN_HOME_KEEP_NO);
	// No home recorded.
	u_screen_home none{};
	CHECK(u_screen_home_keep_decide(&none, U_SCREEN_HOME_GONE, &unplugged, &last, false, 0, 1, kBound) ==
	      U_SCREEN_HOME_KEEP_NO);
	// The last list's home screen is another plug-in's (re-claimed before the unplug).
	xrt_screen_list reclaimed_last{};
	add(reclaimed_last, PANEL, "other", -1920, false);
	add(reclaimed_last, LAPTOP, "sim-display", 0, true);
	CHECK(decide(h, unplugged, &reclaimed_last, false, 0, 1) == U_SCREEN_HOME_KEEP_NO);
}

/*
 * The session's display poll (#2305, review of the bounded wait): the push is
 * due on a new generation or once a kept list's bound has passed; a push made
 * only for the bound must still announce a newer generation its own snapshot
 * picked up, and a kept list that could not be handed on stops being retried.
 */
TEST_CASE("screen home: the poll is due on a new generation or a passed bound")
{
	const uint64_t t0 = 50 * kSec;
	// Nothing new, nothing kept.
	CHECK_FALSE(u_screen_home_poll_due(4, 4, false, 0, t0, kBound));
	CHECK_FALSE(u_screen_home_poll_due(0, 0, false, 0, t0, kBound));
	// A new generation.
	CHECK(u_screen_home_poll_due(5, 4, false, 0, t0, kBound));
	// Kept: not yet, then due at the bound.
	CHECK_FALSE(u_screen_home_poll_due(4, 4, true, t0, t0 + kBound - 1, kBound));
	CHECK(u_screen_home_poll_due(4, 4, true, t0, t0 + kBound, kBound));
	// A clock that went backwards is never due by the bound.
	CHECK_FALSE(u_screen_home_poll_due(4, 4, true, t0, t0 - 1, kBound));
}

TEST_CASE("screen home: a push made for the bound announces a newer generation it picked up")
{
	// The bound's own push, nothing new: no event (the app saw the unplug).
	u_screen_home_poll_after a = u_screen_home_poll_settle(5, 5, 5, false, true);
	CHECK(a.follow == 5);
	CHECK_FALSE(a.announce);
	CHECK_FALSE(a.abandon_keep);

	// The bound's push, but a sweep published 6 between the poll's read and
	// the push's snapshot: the session follows 6 and the app hears of it
	// (it would otherwise be dropped: the next frame sees 6 == 6).
	a = u_screen_home_poll_settle(5, 5, 6, false, true);
	CHECK(a.follow == 6);
	CHECK(a.announce);

	// An ordinary new generation, taken by the compositor.
	a = u_screen_home_poll_settle(6, 5, 6, false, false);
	CHECK(a.follow == 6);
	CHECK(a.announce);
	// ... or by no compositor: follow what the poll read, still announced.
	a = u_screen_home_poll_settle(6, 5, 5, false, false);
	CHECK(a.follow == 6);
	CHECK(a.announce);
	// The push's snapshot was newer than the poll's read.
	a = u_screen_home_poll_settle(6, 5, 7, false, false);
	CHECK(a.follow == 7);
	CHECK(a.announce);
}

TEST_CASE("screen home: a kept list the push could not hand on is abandoned once, not retried every frame")
{
	const uint64_t t0 = 9 * kSec;
	// The bound passed, the push returned before deciding: still keeping.
	u_screen_home_poll_after a = u_screen_home_poll_settle(5, 5, 5, true, true);
	CHECK(a.abandon_keep);
	CHECK_FALSE(a.announce);
	// The session stops keeping: the next frame's poll is not due.
	CHECK_FALSE(u_screen_home_poll_due(5, 5, false, t0, t0 + 2 * kBound, kBound));

	// A push that decided (the keep ended or expired) abandons nothing.
	CHECK_FALSE(u_screen_home_poll_settle(5, 5, 5, false, true).abandon_keep);
	// Still keeping inside the bound (a generation's push kept the list): fine.
	CHECK_FALSE(u_screen_home_poll_settle(6, 5, 6, true, false).abandon_keep);
}
