// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display hotplug (ADR-047 Amendment 8): the claim sweep's pure parts.
 *
 * - the scheduler: debounce, the max-delay cap, the follow-up re-runs an OS
 *   event arms (a vendor backend may recognise a panel seconds after the OS
 *   saw the monitor) and the ones a plug-in request does not;
 * - the per-monitor winner across a hotplug, driven through the exact pair the
 *   loader's resolve uses (target_screen_collect_candidates +
 *   target_screen_pick_ex) over fake monitor lists and fake claim sources:
 *   a panel appearing, its vendor plug-in claiming it once loaded, the
 *   DXR_SCREEN_PLUGIN pin and PreferredPlugin still deciding, ProbeOrder
 *   forcing (#1521) unchanged, unplug;
 * - the published screen-list comparison (what bumps the generation);
 * - the topology signature the poll compares.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_screen.h"

#include "target_display_hotplug.h"
#include "target_screen_pin.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint64_t MS = 1000ull * 1000ull;

} // namespace


/*
 *
 * Scheduler.
 *
 */

TEST_CASE("hotplug sched: a storm is one debounced re-run", "[target][hotplug]")
{
	target_hotplug_sched s;
	target_hotplug_sched_init(&s, 300 * MS, 1000 * MS);
	CHECK(target_hotplug_sched_deadline(&s) == TARGET_HOTPLUG_NEVER);
	CHECK_FALSE(target_hotplug_sched_take(&s, 0, nullptr, nullptr));

	// CoreGraphics reports a reconfiguration as a burst; each event moves
	// the quiet deadline out.
	target_hotplug_sched_event(&s, 1000 * MS, TARGET_HOTPLUG_SOURCE_OS);
	target_hotplug_sched_event(&s, 1100 * MS, TARGET_HOTPLUG_SOURCE_OS);
	target_hotplug_sched_event(&s, 1250 * MS, TARGET_HOTPLUG_SOURCE_POLL);
	CHECK(target_hotplug_sched_deadline(&s) == 1550 * MS);
	CHECK_FALSE(target_hotplug_sched_take(&s, 1549 * MS, nullptr, nullptr));

	bool followup = true;
	uint32_t sources = 0;
	REQUIRE(target_hotplug_sched_take(&s, 1550 * MS, &followup, &sources));
	CHECK_FALSE(followup);
	CHECK(sources == (TARGET_HOTPLUG_SOURCE_OS | TARGET_HOTPLUG_SOURCE_POLL));
	// Consumed: the same instant does not run it twice (the first follow-up is 0.5 s out).
	CHECK_FALSE(target_hotplug_sched_take(&s, 1550 * MS, &followup, &sources));
}

TEST_CASE("hotplug sched: a continuous storm still runs within the max delay", "[target][hotplug]")
{
	target_hotplug_sched s;
	target_hotplug_sched_init(&s, 300 * MS, 1000 * MS);
	for (uint64_t t = 0; t <= 2000; t += 100) {
		target_hotplug_sched_event(&s, t * MS, TARGET_HOTPLUG_SOURCE_OS);
		if (t * MS >= 1000 * MS) {
			break;
		}
		CHECK_FALSE(target_hotplug_sched_take(&s, t * MS, nullptr, nullptr));
	}
	// Events every 100 ms never leave 300 ms of quiet; the cap (first + 1 s) fires.
	CHECK(target_hotplug_sched_deadline(&s) == 1000 * MS);
	CHECK(target_hotplug_sched_take(&s, 1000 * MS, nullptr, nullptr));
}

TEST_CASE("hotplug sched: an OS event arms follow-ups, a plug-in request does not", "[target][hotplug]")
{
	target_hotplug_sched s;
	target_hotplug_sched_init(&s, 0, 0); // defaults: 300 ms / 1 s
	CHECK(s.settle_ns == TARGET_HOTPLUG_SETTLE_NS);
	CHECK(s.max_delay_ns == TARGET_HOTPLUG_MAX_DELAY_NS);

	// Plug-in: "my displays are ready" — one run, nothing after it.
	target_hotplug_sched_event(&s, 0, TARGET_HOTPLUG_SOURCE_PLUGIN);
	bool followup = false;
	uint32_t sources = 0;
	REQUIRE(target_hotplug_sched_take(&s, 300 * MS, &followup, &sources));
	CHECK(sources == TARGET_HOTPLUG_SOURCE_PLUGIN);
	CHECK(target_hotplug_sched_deadline(&s) == TARGET_HOTPLUG_NEVER);

	// OS: the run at +300 ms, then follow-ups at +0.5 .. +32 s after it — the
	// tail outlasts an SR service retrying FPC authentication (1 s .. 30 s backoff).
	target_hotplug_sched_event(&s, 10000 * MS, TARGET_HOTPLUG_SOURCE_OS);
	REQUIRE(target_hotplug_sched_take(&s, 10300 * MS, &followup, &sources));
	CHECK_FALSE(followup);
	const uint64_t base = 10300 * MS;
	const uint64_t offs[] = {500, 1000, 1500, 2000, 3000, 4000, 6000, 8000, 12000, 16000, 24000, 32000};
	REQUIRE(sizeof(offs) / sizeof(offs[0]) == TARGET_HOTPLUG_FOLLOWUPS);
	for (uint64_t off : offs) {
		CHECK(target_hotplug_sched_deadline(&s) == base + off * MS);
		CHECK_FALSE(target_hotplug_sched_take(&s, base + off * MS - 1, nullptr, nullptr));
		REQUIRE(target_hotplug_sched_take(&s, base + off * MS, &followup, &sources));
		CHECK(followup);
		CHECK(sources == 0);
	}
	CHECK(target_hotplug_sched_deadline(&s) == TARGET_HOTPLUG_NEVER);
}

TEST_CASE("hotplug sched: a late wake-up runs a follow-up once, a new OS event restarts them", "[target][hotplug]")
{
	target_hotplug_sched s;
	target_hotplug_sched_init(&s, 0, 0);
	target_hotplug_sched_event(&s, 0, TARGET_HOTPLUG_SOURCE_OS);
	REQUIRE(target_hotplug_sched_take(&s, 300 * MS, nullptr, nullptr));

	// Asleep through +0.5, +1 and +1.5 s: one follow-up, not three.
	bool followup = false;
	REQUIRE(target_hotplug_sched_take(&s, 300 * MS + 1600 * MS, &followup, nullptr));
	CHECK(followup);
	CHECK_FALSE(target_hotplug_sched_take(&s, 300 * MS + 1600 * MS, nullptr, nullptr));
	CHECK(target_hotplug_sched_deadline(&s) == 300 * MS + 2000 * MS);

	// A new OS event (the panel's mode switch) restarts the window from its run.
	target_hotplug_sched_event(&s, 5000 * MS, TARGET_HOTPLUG_SOURCE_OS);
	REQUIRE(target_hotplug_sched_take(&s, 5300 * MS, &followup, nullptr));
	CHECK_FALSE(followup);
	CHECK(target_hotplug_sched_deadline(&s) == 5300 * MS + 500 * MS);

	// A plug-in request in the middle keeps the armed follow-ups.
	target_hotplug_sched_event(&s, 5400 * MS, TARGET_HOTPLUG_SOURCE_PLUGIN);
	REQUIRE(target_hotplug_sched_take(&s, 5700 * MS, &followup, nullptr));
	CHECK_FALSE(followup);
	CHECK(target_hotplug_sched_deadline(&s) == 5300 * MS + 500 * MS);
}


/*
 *
 * The per-monitor winner across a hotplug, through the loader's own pair.
 *
 */

namespace {

struct fake_source
{
	const char *id;
	bool active;
	std::vector<uint64_t> mons;
	std::vector<uint32_t> conf;
};

struct pick
{
	int index;
	const char *plugin;
	target_screen_pick_reason why;
};

/*!
 * Resolve one monitor the way target_plugin_resolve_displays does: collect
 * every source's claim (sources listed in ascending ProbeOrder), then pick.
 */
pick
resolve(const std::vector<fake_source> &srcs, uint64_t monitor, const char *pin, const char *preferred)
{
	std::vector<target_screen_source_claims> view;
	for (const auto &s : srcs) {
		view.push_back({s.id, s.active, s.mons.data(), s.conf.data(), (uint32_t)s.mons.size()});
	}
	target_screen_candidate cands[8];
	const uint32_t n =
	    target_screen_collect_candidates(view.data(), (uint32_t)view.size(), monitor, cands, nullptr, nullptr, 8);
	pick p{};
	p.index = target_screen_pick_ex(cands, n, pin, nullptr, preferred, &p.why, nullptr, nullptr);
	p.plugin = p.index >= 0 ? cands[p.index].plugin_id : nullptr;
	return p;
}

constexpr uint64_t BUILTIN = 0xB1B1;
constexpr uint64_t ODYSSEY = 0x0D55;

} // namespace

TEST_CASE("hotplug claims: a panel plugged in while sim-display runs goes to its vendor once it is loaded",
          "[target][hotplug]")
{
	// Instance create, no panel: leia-sr (ProbeOrder 50) declined, sim-display
	// (200) is active and claims every monitor at FALLBACK.
	std::vector<fake_source> srcs = {
	    {"sim-display", true, {BUILTIN}, {10}},
	};
	pick p = resolve(srcs, BUILTIN, nullptr, nullptr);
	CHECK(std::string(p.plugin) == "sim-display");
	CHECK(p.why == TARGET_SCREEN_PICK_ACTIVE);

	// The Odyssey appears; the OS saw it, the SR service has not authenticated
	// it yet, so leia-sr still declines: sim-display takes it (flat / sim weave).
	srcs[0].mons = {BUILTIN, ODYSSEY};
	srcs[0].conf = {10, 10};
	p = resolve(srcs, ODYSSEY, nullptr, nullptr);
	CHECK(std::string(p.plugin) == "sim-display");
	CHECK(p.why == TARGET_SCREEN_PICK_ACTIVE);

	// The panel is listed by SR but not authenticated yet: leia-sr claims it
	// PRESENT_NOT_READY (20). That already beats sim-display's FALLBACK claim
	// (the carve-out), so no sim weave reaches the panel — the registry flags
	// the entry not-ready and the screen shows flat 2D.
	{
		std::vector<fake_source> nr = srcs;
		nr.insert(nr.begin(), fake_source{"leia-sr", false, {ODYSSEY}, {20}});
		pick q = resolve(nr, ODYSSEY, nullptr, nullptr);
		CHECK(std::string(q.plugin) == "leia-sr");
		CHECK(q.why == TARGET_SCREEN_PICK_BETTER_RANKED);
		CHECK(q.index == 0);
	}

	// A follow-up sweep: leia-sr probes OK now and joins the claim sources in
	// ProbeOrder (before sim-display). Its VERIFIED claim beats the active
	// plug-in's FALLBACK one — the carve-out — and only on the panel.
	srcs.insert(srcs.begin(), fake_source{"leia-sr", false, {ODYSSEY}, {100}});
	p = resolve(srcs, ODYSSEY, nullptr, nullptr);
	CHECK(std::string(p.plugin) == "leia-sr");
	CHECK(p.why == TARGET_SCREEN_PICK_BETTER_RANKED);
	p = resolve(srcs, BUILTIN, nullptr, nullptr);
	CHECK(std::string(p.plugin) == "sim-display");
	CHECK(p.why == TARGET_SCREEN_PICK_ACTIVE);

	// DXR_SCREEN_PLUGIN still pins the monitor (the pin outranks everything).
	p = resolve(srcs, ODYSSEY, "sim-display", nullptr);
	CHECK(std::string(p.plugin) == "sim-display");
	CHECK(p.why == TARGET_SCREEN_PICK_PIN);
	// ...and a PreferredPlugin pinning sim-display keeps sim on the panel.
	p = resolve(srcs, ODYSSEY, nullptr, "sim-display");
	CHECK(std::string(p.plugin) == "sim-display");
	CHECK(p.why == TARGET_SCREEN_PICK_PREFERRED);

	// Unplug: leia-sr stays a claim source but claims nothing; the monitor
	// is simply gone, the built-in is untouched.
	srcs[0].mons.clear();
	srcs[0].conf.clear();
	srcs[1].mons = {BUILTIN};
	srcs[1].conf = {10};
	p = resolve(srcs, ODYSSEY, nullptr, nullptr);
	CHECK(p.index == -1);
	p = resolve(srcs, BUILTIN, nullptr, nullptr);
	CHECK(std::string(p.plugin) == "sim-display");
	CHECK(p.why == TARGET_SCREEN_PICK_ACTIVE);

	// Re-plug: claimed by leia-sr again, no restart needed.
	srcs[0].mons = {ODYSSEY};
	srcs[0].conf = {100};
	srcs[1].mons = {BUILTIN, ODYSSEY};
	srcs[1].conf = {10, 10};
	p = resolve(srcs, ODYSSEY, nullptr, nullptr);
	CHECK(std::string(p.plugin) == "leia-sr");
}

TEST_CASE("hotplug claims: the carve-out never fires for ProbeOrder forcing (#1521) or a real active claim",
          "[target][hotplug]")
{
	// run_cts.ps1 -Plugin sim-display: sim registered AHEAD of leia, both
	// load, sim is active. Its FALLBACK claim must still win the panel.
	const std::vector<fake_source> forced = {
	    {"sim-display", true, {BUILTIN, ODYSSEY}, {10, 10}},
	    {"leia-sr", false, {ODYSSEY}, {100}},
	};
	pick p = resolve(forced, ODYSSEY, nullptr, nullptr);
	CHECK(std::string(p.plugin) == "sim-display");
	CHECK(p.why == TARGET_SCREEN_PICK_ACTIVE);

	// An active vendor's own claim (EDID / VERIFIED) is never displaced by a
	// better-ranked plug-in: the carve-out is for FALLBACK claims only.
	const std::vector<fake_source> vendors = {
	    {"vendor-a", false, {ODYSSEY}, {100}},
	    {"vendor-b", true, {ODYSSEY}, {50}},
	};
	p = resolve(vendors, ODYSSEY, nullptr, nullptr);
	CHECK(std::string(p.plugin) == "vendor-b");
	CHECK(p.why == TARGET_SCREEN_PICK_ACTIVE);

	// A better-ranked source with only a FALLBACK claim of its own does not
	// displace the active one either.
	const std::vector<fake_source> two_fallbacks = {
	    {"other-fallback", false, {BUILTIN}, {10}},
	    {"sim-display", true, {BUILTIN}, {10}},
	};
	p = resolve(two_fallbacks, BUILTIN, nullptr, nullptr);
	CHECK(std::string(p.plugin) == "sim-display");
	CHECK(p.why == TARGET_SCREEN_PICK_ACTIVE);
}

TEST_CASE("hotplug claims: the #1874 shortcut is unchanged by the carve-out", "[target][hotplug]")
{
	// sim-display active and claiming both monitors: the resolve-time
	// shortcut still loads nothing else (the hotplug rescan, not the
	// shortcut, is what re-probes a declined vendor plug-in).
	target_screen_monitor mons[] = {
	    {BUILTIN, "BUILTIN", "", true, nullptr},
	    {ODYSSEY, "ODYSSEY", "", true, nullptr},
	};
	target_screen_pins none;
	REQUIRE(target_screen_pin_parse(nullptr, &none, nullptr) == 0);
	CHECK(target_screen_active_decides_every_monitor("sim-display", nullptr, &none, mons, 2));
	// A pin for the panel still loads the vendor plug-in for its claim.
	target_screen_pins pins;
	REQUIRE(target_screen_pin_parse("0xd55=leia-sr", &pins, nullptr) == 1);
	CHECK_FALSE(target_screen_active_decides_every_monitor("sim-display", nullptr, &pins, mons, 2));
}


/*
 *
 * The published screen-list comparison.
 *
 */

namespace {

xrt_screen
screen(uint64_t id, const char *plugin, int32_t x, uint32_t w, bool is_default = false)
{
	xrt_screen s{};
	s.id = id;
	s.flags = is_default ? XRT_SCREEN_FLAG_SYSTEM_DEFAULT : 0u;
	s.confidence = 10;
	s.desktop_left = x;
	s.desktop_width = w;
	s.desktop_height = 1080;
	s.native_width = w * 2;
	s.native_height = 2160;
	s.desktop_scale = 2.0f;
	std::snprintf(s.plugin_id, sizeof(s.plugin_id), "%s", plugin);
	std::snprintf(s.device_name, sizeof(s.device_name), "mon-%llx", (unsigned long long)id);
	return s;
}

} // namespace

TEST_CASE("hotplug screens: what counts as a change", "[target][hotplug]")
{
	xrt_screen_list before{};
	before.count = 1;
	before.screens[0] = screen(BUILTIN, "sim-display", 0, 1512, true);

	target_screens_change c{};
	// Identical: no change, no generation bump.
	xrt_screen_list same = before;
	CHECK_FALSE(target_screens_compare(&before, &same, &c));

	// The panel appears (claimed by sim-display until leia-sr is ready).
	xrt_screen_list plugged = before;
	plugged.count = 2;
	plugged.screens[1] = screen(ODYSSEY, "sim-display", 1512, 3840);
	REQUIRE(target_screens_compare(&before, &plugged, &c));
	CHECK(c.added == 1);
	CHECK(c.removed == 0);
	CHECK(c.claimant_changed == 0);
	CHECK_FALSE(c.default_moved);

	// leia-sr claims it: a claimant change on the same id.
	xrt_screen_list claimed = plugged;
	std::snprintf(claimed.screens[1].plugin_id, sizeof(claimed.screens[1].plugin_id), "leia-sr");
	claimed.screens[1].confidence = 100;
	REQUIRE(target_screens_compare(&plugged, &claimed, &c));
	CHECK(c.added == 0);
	CHECK(c.claimant_changed == 1);
	char line[512];
	target_screens_change_format(&plugged, &claimed, line, sizeof(line));
	CHECK(std::strstr(line, "claimant 'sim-display' -> 'leia-sr'") != nullptr);

	// Rearranged: geometry.
	xrt_screen_list moved = claimed;
	moved.screens[1].desktop_left = -3840;
	REQUIRE(target_screens_compare(&claimed, &moved, &c));
	CHECK(c.geometry_changed == 1);

	// Eye tracking came up on it (flags / info): "other".
	xrt_screen_list tracked = claimed;
	tracked.screens[1].flags |= XRT_SCREEN_FLAG_TRACKED;
	REQUIRE(target_screens_compare(&claimed, &tracked, &c));
	CHECK(c.other_changed == 1);

	// Unplugged.
	REQUIRE(target_screens_compare(&claimed, &before, &c));
	CHECK(c.removed == 1);
	target_screens_change_format(&claimed, &before, line, sizeof(line));
	CHECK(std::strstr(line, "-0x") != nullptr);

	// The system default moves (its monitor was unplugged).
	xrt_screen_list only_panel{};
	only_panel.count = 1;
	only_panel.screens[0] = screen(ODYSSEY, "leia-sr", 0, 3840, true);
	REQUIRE(target_screens_compare(&claimed, &only_panel, &c));
	CHECK(c.default_moved);
	CHECK(c.removed == 1);
}


/*
 *
 * Topology signature.
 *
 */

TEST_CASE("hotplug topology hash: equal lists hash equal, any monitor change does not", "[target][hotplug]")
{
	os_display_desktop_info a[2]{};
	a[0].left = 0;
	a[0].width = 1512;
	a[0].height = 982;
	a[0].native_width = 3024;
	a[0].native_height = 1964;
	a[0].scale = 2.0;
	a[0].is_primary = true;
	std::snprintf(a[0].device_name, sizeof(a[0].device_name), "37D8832A-2D66-02CA-B9F7-8F30A301B230");
	os_display_desktop_info b[2];
	std::memcpy(b, a, sizeof(a));

	const uint64_t h1 = target_display_topology_hash(a, 1);
	CHECK(h1 == target_display_topology_hash(b, 1));

	// A second monitor appears.
	b[1] = a[0];
	b[1].left = 1512;
	b[1].is_primary = false;
	std::snprintf(b[1].device_name, sizeof(b[1].device_name), "ODYSSEY");
	CHECK(target_display_topology_hash(b, 2) != h1);

	// A mode change on the same monitor.
	std::memcpy(b, a, sizeof(a));
	b[0].native_width = 2560;
	CHECK(target_display_topology_hash(b, 1) != h1);

	// A rearrangement.
	std::memcpy(b, a, sizeof(a));
	b[0].left = -1512;
	CHECK(target_display_topology_hash(b, 1) != h1);
}
