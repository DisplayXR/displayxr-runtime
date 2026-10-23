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

TEST_CASE("the service process never hosts a provider (this release)", "[viewer_arbitration]")
{
	const target_viewer_decision d = arbitrate(false, XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, true, false);
	CHECK(d.source == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(std::string(d.reason).find("service") != std::string::npos);
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
