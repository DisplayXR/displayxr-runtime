// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief ADR-054 N6 Part 2 — the viewer arbitration, the composed
 *        XR_DXR_display_info advertisement, the tracking-provider manifest
 *        router, the DP viewer-policy slot rule, and sim_tracking's script.
 *
 * All pure: no plug-in is loaded. The end-to-end arm (sim_tracking driving
 * xrLocateViews through sim_display) is tests_oxr_view_space_viewer_tracking.
 */

#include "catch_amalgamated.hpp"

#include "target_component_loader.h"
#include "target_viewer_tracking.h"

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_tracking_plugin.h"

#include "sim_tracking_script.h"

#include <cmath>
#include <cstring>
#include <string>

namespace {

target_viewer_decision
arbitrate(bool dp_tracks, uint32_t policy, bool provider, bool hosted = true)
{
	target_viewer_inputs in = {};
	in.dp_tracks = dp_tracks;
	in.dp_policy = policy;
	in.hosted = hosted;
	in.provider_usable = provider;
	in.provider_id = provider ? "sim-tracking" : nullptr;
	target_viewer_decision d;
	target_viewer_arbitrate(&in, &d);
	return d;
}

std::string
label(const target_viewer_decision &d)
{
	char buf[96];
	target_viewer_source_label(&d, buf, sizeof(buf));
	return buf;
}

} // namespace

TEST_CASE("a display that tracks beats a provider, whatever its policy", "[viewer_arbitration]")
{
	for (uint32_t policy : {(uint32_t)XRT_PLUGIN_VIEWER_POLICY_OWN, (uint32_t)XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK,
	                        (uint32_t)XRT_PLUGIN_VIEWER_POLICY_FIXED}) {
		CAPTURE(policy);
		const target_viewer_decision d = arbitrate(true, policy, true);
		CHECK(d.source == XRT_VIEWER_SOURCE_DP);
		CHECK(label(d) == "dp");
		CHECK(std::string(d.reason) == "display tracks the viewer on this screen");
		CHECK(d.provider_id[0] == '\0');
	}
}

TEST_CASE("FIXED ignores the provider", "[viewer_arbitration]")
{
	const target_viewer_decision d = arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_FIXED, true);
	CHECK(d.source == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(label(d) == "nominal");
	CHECK(std::string(d.reason) == "display is FIXED");
}

TEST_CASE("OWN without tracking keeps the display's own eyes, never a provider", "[viewer_arbitration]")
{
	const target_viewer_decision d = arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_OWN, true);
	CHECK(d.source == XRT_VIEWER_SOURCE_DP);
	CHECK(std::string(d.reason).find("OWN") != std::string::npos);
}

TEST_CASE("EXTERNAL_OK uses the provider", "[viewer_arbitration]")
{
	const target_viewer_decision d = arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, true);
	CHECK(d.source == XRT_VIEWER_SOURCE_PROVIDER);
	CHECK(label(d) == "provider:sim-tracking");
	CHECK(d.reason[0] == '\0');
}

TEST_CASE("no provider gives the nominal viewer", "[viewer_arbitration]")
{
	const target_viewer_decision d = arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, false);
	CHECK(d.source == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(std::string(d.reason) == "no source");
}

TEST_CASE("a process that does not host providers gives the nominal viewer", "[viewer_arbitration]")
{
	const target_viewer_decision d = arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, true, false);
	CHECK(d.source == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(std::string(d.reason).find("not hosted") != std::string::npos);
	// ... but a tracking display still wins there.
	CHECK(arbitrate(true, XRT_PLUGIN_VIEWER_POLICY_FIXED, false, false).source == XRT_VIEWER_SOURCE_DP);
}

TEST_CASE("composed advertisement", "[viewer_arbitration][advertisement]")
{
	uint32_t sup = 99, def = 99;
	bool mark = true;

	SECTION("provider chosen: MANAGED|MANUAL, MANAGED default, 3D modes marked")
	{
		const target_viewer_decision d = arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, true);
		target_viewer_compose_advertisement(0, 0, &d, &sup, &def, &mark);
		CHECK(sup == (TARGET_VIEWER_MODE_MANAGED_BIT | TARGET_VIEWER_MODE_MANUAL_BIT));
		CHECK(def == 0u);
		CHECK(mark);
	}
	SECTION("DP tracks: its own caps, unchanged")
	{
		const target_viewer_decision d = arbitrate(true, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, true);
		target_viewer_compose_advertisement(TARGET_VIEWER_MODE_MANUAL_BIT, 1, &d, &sup, &def, &mark);
		CHECK(sup == TARGET_VIEWER_MODE_MANUAL_BIT);
		CHECK(def == 1u);
		CHECK_FALSE(mark);
	}
	SECTION("FIXED / no source: the honest zero stays zero")
	{
		for (const target_viewer_decision &d :
		     {arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_FIXED, true),
		      arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, false)}) {
			target_viewer_compose_advertisement(0, 0, &d, &sup, &def, &mark);
			CHECK(sup == 0u);
			CHECK(def == 0u);
			CHECK_FALSE(mark);
		}
	}
	SECTION("idempotent: composing an already-composed answer changes nothing")
	{
		const target_viewer_decision d = arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, true);
		target_viewer_compose_advertisement(3, 0, &d, &sup, &def, &mark);
		CHECK(sup == 3u);
		CHECK_FALSE(mark);
	}
}

namespace {

uint32_t g_policy_answer = 0;

uint32_t
fake_policy(struct xrt_plugin_instance *, const struct xrt_display_descriptor *)
{
	return g_policy_answer;
}

} // namespace

TEST_CASE("the viewer-policy slot: absent or unknown reads FIXED", "[viewer_arbitration][slot]")
{
	CHECK(xrt_plugin_iface_viewer_policy(nullptr, nullptr, nullptr) == XRT_PLUGIN_VIEWER_POLICY_FIXED);

	xrt_plugin_iface iface = {};
	// A plug-in built before the slot: struct_size stops short of it.
	iface.struct_size = (uint32_t)offsetof(xrt_plugin_iface, get_viewer_policy);
	iface.get_viewer_policy = fake_policy;
	g_policy_answer = XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK;
	CHECK(xrt_plugin_iface_viewer_policy(&iface, nullptr, nullptr) == XRT_PLUGIN_VIEWER_POLICY_FIXED);

	iface.struct_size = (uint32_t)sizeof(iface);
	CHECK(xrt_plugin_iface_viewer_policy(&iface, nullptr, nullptr) == XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK);
	g_policy_answer = XRT_PLUGIN_VIEWER_POLICY_OWN;
	CHECK(xrt_plugin_iface_viewer_policy(&iface, nullptr, nullptr) == XRT_PLUGIN_VIEWER_POLICY_OWN);
	g_policy_answer = 0; // zero-filled / unknown
	CHECK(xrt_plugin_iface_viewer_policy(&iface, nullptr, nullptr) == XRT_PLUGIN_VIEWER_POLICY_FIXED);
	g_policy_answer = 77;
	CHECK(xrt_plugin_iface_viewer_policy(&iface, nullptr, nullptr) == XRT_PLUGIN_VIEWER_POLICY_FIXED);
	iface.get_viewer_policy = nullptr;
	CHECK(xrt_plugin_iface_viewer_policy(&iface, nullptr, nullptr) == XRT_PLUGIN_VIEWER_POLICY_FIXED);

	// Append-only: the slot sits after every earlier one.
	STATIC_REQUIRE(offsetof(xrt_plugin_iface, get_viewer_policy) > offsetof(xrt_plugin_iface, get_claimed_cameras));
	STATIC_REQUIRE(XRT_PLUGIN_IFACE_HAS_VIEWER_POLICY == 1);
}

TEST_CASE("the manifest router sends -tracking-provider.json to the tracking class", "[viewer_arbitration][router]")
{
	CHECK(target_plugin_classify_manifest("200-sim-tracking-tracking-provider.json") ==
	      TARGET_PLUGIN_MANIFEST_TRACKING);
	CHECK(target_plugin_classify_manifest("-tracking-provider.json") == TARGET_PLUGIN_MANIFEST_NONE);
	// The neighbours keep their routes; a tracking manifest is never read as a DP.
	CHECK(target_plugin_classify_manifest("200-sim-input-input-provider.json") == TARGET_PLUGIN_MANIFEST_INPUT);
	CHECK(target_plugin_classify_manifest("200-sim-display.json") == TARGET_PLUGIN_MANIFEST_DISPLAY);
	CHECK(std::string(target_component_class_id_str(TARGET_COMPONENT_CLASS_TRACKING)) == "tracking");
	STATIC_REQUIRE(TARGET_COMPONENT_CLASS_TRACKING == 4); // crosses IPC: never renumbered
}

TEST_CASE("the tracking ABI keeps its shapes", "[viewer_arbitration][abi]")
{
	STATIC_REQUIRE(XRT_TRACKING_PLUGIN_API_VERSION_CURRENT == 1);
	STATIC_REQUIRE(offsetof(xrt_tracking_viewer_sample, struct_size) == 0);
	STATIC_REQUIRE(offsetof(xrt_tracking_plugin_iface, struct_size) == 0);
	STATIC_REQUIRE(offsetof(xrt_tracking_provider_info, struct_size) == 0);
	STATIC_REQUIRE(offsetof(xrt_tracking_sink, struct_size) == 0);
	// The camera-space block (calibration input) is in v1, behind CAMERA_VALID.
	STATIC_REQUIRE(offsetof(xrt_tracking_viewer_sample, camera) > offsetof(xrt_tracking_viewer_sample, confidence));
	STATIC_REQUIRE(sizeof(xrt_tracking_camera_sample{}.camera_key) == XRT_TRACKING_CAMERA_KEY_MAX);
}

TEST_CASE("sim_tracking's scripted viewer", "[viewer_arbitration][sim_tracking]")
{
	sim_tracking_script s = {};
	s.nominal[0] = 0.0f;
	s.nominal[1] = 0.1f;
	s.nominal[2] = 0.6f;
	s.ipd_m = 0.064f;
	s.amplitude_m = 0.1f;
	s.period_ns = 4'000'000'000LL;
	s.epoch_ns = 1'000'000'000LL;
	float l[3], r[3], h[3];

	SECTION("offset: still, the pair straddles nominal + offset")
	{
		s.kind = SIM_TRACKING_SCRIPT_OFFSET;
		s.offset[0] = 0.05f;
		REQUIRE(sim_tracking_script_eval(&s, s.epoch_ns + 123456789, l, r, h));
		CHECK(l[0] == Catch::Approx(0.05f - 0.032f));
		CHECK(r[0] == Catch::Approx(0.05f + 0.032f));
		CHECK(l[1] == Catch::Approx(0.1f));
		CHECK(h[2] == Catch::Approx(0.6f));
	}
	SECTION("sweep: centre at t=0, +amplitude at a quarter period, -amplitude at three quarters")
	{
		s.kind = SIM_TRACKING_SCRIPT_SWEEP;
		sim_tracking_script_eval(&s, s.epoch_ns, l, r, h);
		CHECK(h[0] == Catch::Approx(0.0f).margin(1e-6));
		sim_tracking_script_eval(&s, s.epoch_ns + s.period_ns / 4, l, r, h);
		CHECK(h[0] == Catch::Approx(0.1f).margin(1e-5));
		sim_tracking_script_eval(&s, s.epoch_ns + 3 * s.period_ns / 4, l, r, h);
		CHECK(h[0] == Catch::Approx(-0.1f).margin(1e-5));
		CHECK(r[0] - l[0] == Catch::Approx(0.064f));
	}
	SECTION("loss period: tracking for the first half of every period")
	{
		s.kind = SIM_TRACKING_SCRIPT_OFFSET;
		s.loss_period_ns = 1'000'000'000LL;
		CHECK(sim_tracking_script_eval(&s, s.epoch_ns + 100'000'000LL, l, r, h));
		CHECK_FALSE(sim_tracking_script_eval(&s, s.epoch_ns + 600'000'000LL, l, r, h));
		CHECK(sim_tracking_script_eval(&s, s.epoch_ns + 1'100'000'000LL, l, r, h));
	}
	SECTION("script names")
	{
		sim_tracking_script_kind k = SIM_TRACKING_SCRIPT_OFFSET;
		CHECK(sim_tracking_parse_script("SWEEP", &k));
		CHECK(k == SIM_TRACKING_SCRIPT_SWEEP);
		CHECK(sim_tracking_parse_script("offset", &k));
		CHECK(k == SIM_TRACKING_SCRIPT_OFFSET);
		CHECK_FALSE(sim_tracking_parse_script("webcam", &k));
	}
}


/*
 *
 * Per-screen routing (ADR-054 N6 Part 2 follow-up, #2144): each screen read
 * from its OWN display processor, one provider for every eligible screen.
 *
 */

namespace {

// Two 1920x1080 screens side by side, 0.4 m wide each (pitch ~0.2083 mm/px).
target_viewer_screen_in
screen(uint64_t id, bool primary, bool tracks, uint32_t policy, int32_t left, bool has_dp = true)
{
	target_viewer_screen_in s = {};
	s.screen_id = id;
	s.is_primary = primary;
	s.has_dp = has_dp;
	s.dp_tracks = tracks;
	s.dp_policy = policy;
	s.desktop_left = left;
	s.desktop_top = 0;
	s.desktop_width = 1920;
	s.desktop_height = 1080;
	s.width_m = 0.4f;
	s.height_m = 0.225f;
	return s;
}

target_viewer_provider_in
provider(bool usable = true)
{
	target_viewer_provider_in p = {};
	p.hosted = true;
	p.usable = usable;
	p.enabled = usable;
	p.id = usable ? "sim-tracking" : nullptr;
	return p;
}

} // namespace

TEST_CASE("per screen: a tracking display keeps its screen, the provider serves the EXTERNAL_OK one",
          "[viewer_arbitration][per_screen]")
{
	const target_viewer_screen_in in[2] = {
	    screen(0x11, true, true, XRT_PLUGIN_VIEWER_POLICY_OWN, 0),
	    screen(0x22, false, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, 1920),
	};
	const target_viewer_provider_in p = provider();
	target_viewer_screen_decision out[2];
	REQUIRE(target_viewer_arbitrate_screens(in, 2, &p, out) == 2);

	CHECK(out[0].is_primary);
	CHECK(out[0].d.source == XRT_VIEWER_SOURCE_DP);
	CHECK(target_viewer_provider_state_of(&out[0].d, false, false) == TARGET_VIEWER_PROVIDER_STANDBY);

	CHECK(out[1].d.source == XRT_VIEWER_SOURCE_PROVIDER);
	CHECK(label(out[1].d) == "provider:sim-tracking");
	// The provider reports in the primary's display space; the primary's centre
	// sits one screen width to the LEFT of the secondary's centre.
	CHECK(out[1].offset_m[0] == Catch::Approx(-0.4f).margin(1e-4));
	CHECK(out[1].offset_m[1] == Catch::Approx(0.0f).margin(1e-6));
	CHECK(out[1].offset_m[2] == 0.0f);
	CHECK(target_viewer_provider_state_of(&out[1].d, false, false) == TARGET_VIEWER_PROVIDER_IDLE);
	CHECK(target_viewer_provider_state_of(&out[1].d, true, true) == TARGET_VIEWER_PROVIDER_SERVING);
	CHECK(target_viewer_provider_state_of(&out[1].d, true, false) == TARGET_VIEWER_PROVIDER_PAUSED);
}

TEST_CASE("per screen: FIXED stays nominal, EXTERNAL_OK takes the provider", "[viewer_arbitration][per_screen]")
{
	const target_viewer_screen_in in[2] = {
	    screen(0x11, true, false, XRT_PLUGIN_VIEWER_POLICY_FIXED, 0),
	    screen(0x22, false, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, -1920),
	};
	const target_viewer_provider_in p = provider();
	target_viewer_screen_decision out[2];
	REQUIRE(target_viewer_arbitrate_screens(in, 2, &p, out) == 2);
	CHECK(out[0].d.source == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(std::string(out[0].d.reason) == "display is FIXED");
	CHECK(target_viewer_provider_state_of(&out[0].d, true, true) == TARGET_VIEWER_PROVIDER_NONE);
	CHECK(out[1].d.source == XRT_VIEWER_SOURCE_PROVIDER);
	CHECK(out[1].offset_m[0] == Catch::Approx(0.4f).margin(1e-4)); // the primary is to its right
}

TEST_CASE("per screen: no provider is 'no_source', distinct from 'standby'", "[viewer_arbitration][per_screen]")
{
	const target_viewer_screen_in in[2] = {
	    screen(0x11, true, true, XRT_PLUGIN_VIEWER_POLICY_OWN, 0),
	    screen(0x22, false, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, 1920),
	};
	const target_viewer_provider_in p = provider(false);
	target_viewer_screen_decision out[2];
	REQUIRE(target_viewer_arbitrate_screens(in, 2, &p, out) == 2);
	CHECK(target_viewer_provider_state_of(&out[0].d, false, false) == TARGET_VIEWER_PROVIDER_NONE);
	CHECK(out[1].d.source == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(std::string(out[1].d.reason) == "no source");
	CHECK(target_viewer_provider_state_of(&out[1].d, false, false) == TARGET_VIEWER_PROVIDER_NO_SOURCE);
	CHECK(std::string(target_viewer_provider_state_str(TARGET_VIEWER_PROVIDER_NO_SOURCE)) == "no_source");
	CHECK(std::string(target_viewer_provider_state_str(TARGET_VIEWER_PROVIDER_STANDBY)) == "standby");
}

TEST_CASE("per screen: hotplug re-routes a new EXTERNAL_OK screen and drops an unplugged one",
          "[viewer_arbitration][per_screen][hotplug]")
{
	const target_viewer_provider_in p = provider();
	target_viewer_screen_decision out[3];

	// Before: the primary alone (EXTERNAL_OK, no tracker) — the provider serves it, no offset.
	target_viewer_screen_in one[1] = {screen(0x11, true, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, 0)};
	REQUIRE(target_viewer_arbitrate_screens(one, 1, &p, out) == 1);
	CHECK(out[0].d.source == XRT_VIEWER_SOURCE_PROVIDER);
	CHECK(out[0].offset_m[0] == 0.0f);

	// Plug in a tracking panel and a FIXED one: each keeps its own answer, the primary is unchanged.
	target_viewer_screen_in three[3] = {
	    screen(0x11, true, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, 0),
	    screen(0x22, false, true, XRT_PLUGIN_VIEWER_POLICY_OWN, 1920),
	    screen(0x33, false, false, XRT_PLUGIN_VIEWER_POLICY_FIXED, 3840),
	};
	REQUIRE(target_viewer_arbitrate_screens(three, 3, &p, out) == 3);
	CHECK(out[0].d.source == XRT_VIEWER_SOURCE_PROVIDER);
	CHECK(out[1].d.source == XRT_VIEWER_SOURCE_DP);
	CHECK(out[2].d.source == XRT_VIEWER_SOURCE_NOMINAL);

	// The FIXED panel is replaced by an EXTERNAL_OK one at the same place: it takes the provider.
	three[2].dp_policy = XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK;
	three[2].screen_id = 0x44;
	REQUIRE(target_viewer_arbitrate_screens(three, 3, &p, out) == 3);
	CHECK(out[2].screen_id == 0x44);
	CHECK(out[2].d.source == XRT_VIEWER_SOURCE_PROVIDER);
	CHECK(out[2].offset_m[0] == Catch::Approx(-0.8f).margin(1e-4));

	// Unplug back to one screen: the table shrinks with it.
	CHECK(target_viewer_arbitrate_screens(one, 1, &p, out) == 1);
}

TEST_CASE("per screen: a plain monitor (no DP) and unmappable geometry stay nominal",
          "[viewer_arbitration][per_screen]")
{
	target_viewer_screen_in in[2] = {
	    screen(0x11, true, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, 0),
	    screen(0x22, false, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, 1920, false),
	};
	const target_viewer_provider_in p = provider();
	target_viewer_screen_decision out[2];
	REQUIRE(target_viewer_arbitrate_screens(in, 2, &p, out) == 2);
	CHECK(out[1].d.source == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(std::string(out[1].d.reason) == "no display processor on this screen");

	// A DP-backed secondary with an empty rect cannot be mapped.
	in[1].has_dp = true;
	in[1].desktop_width = 0;
	REQUIRE(target_viewer_arbitrate_screens(in, 2, &p, out) == 2);
	CHECK(out[1].d.source == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(std::string(out[1].d.reason).find("map") != std::string::npos);
}

TEST_CASE("per screen: the offset reads each half at its own pitch", "[viewer_arbitration][per_screen][map]")
{
	target_viewer_screen_in home = screen(0x11, true, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, 0);
	target_viewer_screen_in to = screen(0x22, false, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, 1920);
	to.width_m = 0.6f; // a coarser panel at the same desktop size
	to.height_m = 0.3375f;
	float off[3];
	REQUIRE(target_viewer_map_offset(&home, &to, off));
	CHECK(off[0] == Catch::Approx(-(0.2f + 0.3f)).margin(1e-4)); // half of each, abutting
	REQUIRE(target_viewer_map_offset(&home, &home, off));
	CHECK(off[0] == 0.0f);
	// Stacked: the primary above (desktop y down → +y in the lower screen's space).
	to = screen(0x22, false, false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, 0);
	to.desktop_top = 1080;
	REQUIRE(target_viewer_map_offset(&home, &to, off));
	CHECK(off[0] == Catch::Approx(0.0f).margin(1e-6));
	CHECK(off[1] == Catch::Approx(0.225f).margin(1e-4));
}


/*
 *
 * The view-path splice the service and the in-process locate share.
 *
 */

namespace {

struct fake_vt
{
	xrt_viewer_tracking base;
	xrt_viewer_source source;
	uint64_t served_screen;
	float offset_x;
	int calls;
};

xrt_viewer_source
fake_get_source(xrt_viewer_tracking *vt, uint64_t screen_id)
{
	fake_vt *f = (fake_vt *)vt;
	return screen_id == f->served_screen ? f->source : XRT_VIEWER_SOURCE_DP;
}

bool
fake_get_eyes_at(xrt_viewer_tracking *vt, uint64_t screen_id, int64_t, xrt_eye_positions *out)
{
	fake_vt *f = (fake_vt *)vt;
	f->calls++;
	if (screen_id != f->served_screen) {
		return false;
	}
	*out = {};
	out->count = 2;
	out->valid = true;
	out->is_tracking = true;
	out->timestamp_ns = 42;
	out->eyes[0] = {-0.032f + f->offset_x, 0.1f, 0.6f};
	out->eyes[1] = {0.032f + f->offset_x, 0.1f, 0.6f};
	return true;
}

} // namespace

TEST_CASE("the shared splice: a routed screen takes the provider pair, others keep the DP's",
          "[viewer_arbitration][splice][service]")
{
	fake_vt f = {};
	f.base.get_source = fake_get_source;
	f.base.get_eyes_at = fake_get_eyes_at;
	f.source = XRT_VIEWER_SOURCE_PROVIDER;
	f.served_screen = 0x22;
	f.offset_x = -0.4f;

	xrt_eye_positions dp = {};
	dp.count = 2;
	dp.valid = true;
	dp.eyes[0] = {-0.03f, 0.0f, 0.5f};
	dp.eyes[1] = {0.03f, 0.0f, 0.5f};

	SECTION("NULL arbitration (an IPC client) never touches the answer")
	{
		xrt_eye_positions e = dp;
		CHECK_FALSE(xrt_viewer_tracking_override_eyes(nullptr, 0x22, 0, &e, true));
		CHECK(e.eyes[0].x == dp.eyes[0].x);
	}
	SECTION("a screen not routed to the provider keeps the DP's eyes, and the provider is not asked")
	{
		xrt_eye_positions e = dp;
		CHECK_FALSE(xrt_viewer_tracking_override_eyes(&f.base, 0x11, 0, &e, true));
		CHECK(e.eyes[0].x == dp.eyes[0].x);
		CHECK(f.calls == 0);
	}
	SECTION("the routed screen takes the mapped pair, tracking + time from the provider")
	{
		xrt_eye_positions e = dp;
		REQUIRE(xrt_viewer_tracking_override_eyes(&f.base, 0x22, 0, &e, true));
		CHECK(e.count == 2);
		CHECK(e.eyes[0].x == Catch::Approx(-0.432f));
		CHECK(e.eyes[1].z == Catch::Approx(0.6f));
		CHECK(e.is_tracking);
		CHECK(e.timestamp_ns == 42);
	}
	SECTION("no DP answer: the pair alone")
	{
		xrt_eye_positions e = {};
		REQUIRE(xrt_viewer_tracking_override_eyes(&f.base, 0x22, 0, &e, false));
		CHECK(e.valid);
		CHECK(e.count == 2);
	}
	SECTION("an N=4 nominal layout keeps its shape, translated onto the pair's midpoint")
	{
		xrt_eye_positions e = {};
		e.count = 4;
		e.valid = true;
		e.eyes[0] = {-0.03f, 0.0f, 0.5f};
		e.eyes[1] = {0.03f, 0.0f, 0.5f};
		e.eyes[2] = {-0.03f, 0.05f, 0.5f};
		e.eyes[3] = {0.03f, 0.05f, 0.5f};
		REQUIRE(xrt_viewer_tracking_override_eyes(&f.base, 0x22, 0, &e, true));
		CHECK(e.count == 4);
		CHECK(e.eyes[1].x - e.eyes[0].x == Catch::Approx(0.06f));
		const float cx = (e.eyes[0].x + e.eyes[1].x + e.eyes[2].x + e.eyes[3].x) / 4.0f;
		CHECK(cx == Catch::Approx(-0.4f));
	}
	SECTION("a DP source never splices")
	{
		f.source = XRT_VIEWER_SOURCE_DP;
		xrt_eye_positions e = dp;
		CHECK_FALSE(xrt_viewer_tracking_override_eyes(&f.base, 0x22, 0, &e, true));
	}
}
