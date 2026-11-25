// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-066 Part B end to end, headless: the per-screen viewer-tracking
 *         setting (off / 3d / always) over the REAL sim_tracking plug-in
 *         (manifest discovery), the runtime glue (target_viewer_tracking.c)
 *         and the per-screen store's environment tier.
 *
 * No compositor, no window, no camera: `[script]` runs sim_tracking's scripted
 * source (SOURCE=script, its own opt-in ENABLE off, so it serves only screens
 * that ask), `[stub]` runs SOURCE=webcam on a build whose face estimator is
 * the stub, which must read "unavailable: no face estimator" — never silently
 * nothing. Each arm is its own process (the tracking scan is one-shot).
 * Three synthetic screens: A (primary, EXTERNAL_OK), B (EXTERNAL_OK, to its
 * right), C (a display that tracks the viewer itself).
 */

#include "catch_amalgamated.hpp"

#include "target_screen_modes.h"
#include "target_viewer_tracking.h"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_screen.h"
#include "xrt/xrt_viewer_tracking.h"

#include "os/os_time.h"
#include "util/u_screen_modes.h"
#include "util/u_setting.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

uint32_t
external_ok(struct xrt_plugin_instance *, const struct xrt_display_descriptor *)
{
	return XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK;
}

xrt_plugin_iface g_iface = [] {
	xrt_plugin_iface i = {};
	i.struct_size = (uint32_t)sizeof(i);
	i.get_viewer_policy = external_ok;
	return i;
}();

void
add_screen(xrt_screen_list &l,
           xrt_dp_factory_registry &reg,
           uint64_t id,
           const char *key,
           int32_t left,
           bool primary,
           bool tracks)
{
	xrt_screen &s = l.screens[l.count++];
	s = {};
	s.id = id;
	s.flags = primary ? XRT_SCREEN_FLAG_SYSTEM_DEFAULT : 0;
	s.desktop_left = left;
	s.desktop_width = 1920;
	s.desktop_height = 1080;
	std::snprintf(s.plugin_id, sizeof(s.plugin_id), "%s", "sim-display");
	std::snprintf(s.key, sizeof(s.key), "%s", key);
	s.info.width_m = 0.4f;
	s.info.height_m = 0.225f;
	s.info.supported_eye_tracking_modes = tracks ? 1u : 0u;
	xrt_dp_registry_entry &e = reg.entries[reg.entry_count++];
	e = {};
	e.monitor_id = id;
	std::snprintf(e.plugin_id, sizeof(e.plugin_id), "%s", "sim-display");
	e.owning_iface = &g_iface;
	e.owning_instance = nullptr;
}

void
set_env(const char *v)
{
#ifdef _WIN32
	_putenv_s(U_SETTING_VIEWER_TRACKING_ENV, v);
#else
	setenv(U_SETTING_VIEWER_TRACKING_ENV, v, 1);
#endif
	u_setting_per_screen_reload(); // what the service's DIAG re-probe does before re-resolving
}

struct Rig
{
	xrt_screen_list *list = new xrt_screen_list();
	xrt_dp_factory_registry *reg = new xrt_dp_factory_registry();
	xrt_system_compositor_info *info = new xrt_system_compositor_info();

	//! @p first_publish false: only the apply, the caller runs the first publish.
	explicit Rig(bool first_publish = true)
	{
		add_screen(*list, *reg, 0xA1, "TST-A", 0, true, false);
		add_screen(*list, *reg, 0xB2, "TST-B", 1920, false, false);
		add_screen(*list, *reg, 0xC3, "TST-C", 3840, false, true);
		// What system creation runs: apply (the primary, before the table), then the first publish.
		xrt_plugin_display_info pdi = {};
		pdi.struct_size = sizeof(pdi);
		pdi.display_width_m = 0.4f;
		pdi.display_height_m = 0.225f;
		pdi.nominal_viewer_z_m = 0.6f;
		target_viewer_tracking_apply(info, nullptr, &g_iface, &pdi);
		if (first_publish) {
			target_viewer_tracking_update_screens(list, reg);
			target_viewer_tracking_recompose(info, nullptr);
		}
	}
	~Rig()
	{
		target_viewer_tracking_shutdown();
		delete list;
		delete reg;
		delete info;
	}

	const target_viewer_screen_decision
	decision(uint64_t id)
	{
		target_viewer_screen_decision out[TARGET_VIEWER_MAX_SCREENS];
		const uint32_t n = target_viewer_tracking_screen_decisions(list, reg, out, TARGET_VIEWER_MAX_SCREENS);
		for (uint32_t i = 0; i < n; i++) {
			if (out[i].screen_id == id) {
				return out[i];
			}
		}
		FAIL("screen not found");
		return {};
	}
};

xrt_viewer_source
source(uint64_t id, xrt_viewer_mode mode)
{
	xrt_viewer_tracking *vt = target_viewer_tracking_get_iface();
	return xrt_viewer_tracking_get_source_mode(vt, id, mode);
}

bool
eyes(uint64_t id, xrt_eye_positions *e)
{
	xrt_viewer_tracking *vt = target_viewer_tracking_get_iface();
	*e = {};
	return vt->get_eyes_at(vt, id, (int64_t)os_monotonic_get_ns(), e);
}

} // namespace

TEST_CASE("per-screen setting: 3d / always / off per screen, live, over the scripted provider",
          "[viewer_tracking_per_screen][script]")
{
	// A asks for 3d, B is off, C (a tracking display) asks for always — which it can never get.
	set_env("TST-A=3d;TST-B=off;TST-C=always");
	Rig rig;

	// The advertisement was recomposed from A's setting (the provider's own opt-in is off).
	CHECK(rig.info->supported_eye_tracking_modes ==
	      (TARGET_VIEWER_MODE_MANAGED_BIT | TARGET_VIEWER_MODE_MANUAL_BIT));

	// A: the provider in the 3D slot only.
	CHECK(source(XRT_VIEWER_SCREEN_PRIMARY, XRT_VIEWER_MODE_3D) == XRT_VIEWER_SOURCE_PROVIDER);
	CHECK(source(XRT_VIEWER_SCREEN_PRIMARY, XRT_VIEWER_MODE_2D) == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(source(0xA1, XRT_VIEWER_MODE_3D) == XRT_VIEWER_SOURCE_PROVIDER);
	xrt_eye_positions e;
	REQUIRE(eyes(0xA1, &e));
	CHECK(e.count == 2);
	CHECK(e.eyes[1].x - e.eyes[0].x == Catch::Approx(0.063f).margin(0.002f));
	xrt_viewer_tracked_modes tm;
	REQUIRE(target_viewer_tracking_tracked_modes(0xA1, &tm));
	CHECK(tm.provider);
	CHECK(tm.tracked_3d);
	CHECK_FALSE(tm.tracked_2d);

	// B: off — no provider, and the setting is still offered (a provider is available).
	CHECK(source(0xB2, XRT_VIEWER_MODE_3D) == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK_FALSE(eyes(0xB2, &e));
	target_viewer_screen_decision b = rig.decision(0xB2);
	CHECK(b.offered);
	CHECK(std::string(b.offer_reason).find("scripted") != std::string::npos);
	CHECK(b.d.setting == TARGET_VIEWER_SETTING_OFF);
	CHECK(target_viewer_tracking_provider_state(&b.d) == TARGET_VIEWER_PROVIDER_OFF);

	// C: a display that tracks the viewer itself keeps its own eyes; the setting is not offered there.
	CHECK(source(0xC3, XRT_VIEWER_MODE_2D) == XRT_VIEWER_SOURCE_DP);
	CHECK(source(0xC3, XRT_VIEWER_MODE_3D) == XRT_VIEWER_SOURCE_DP);
	target_viewer_screen_decision c = rig.decision(0xC3);
	CHECK_FALSE(c.offered);
	CHECK(std::string(c.offer_reason) == "this display tracks the viewer itself");
	REQUIRE(target_viewer_tracking_tracked_modes(0xC3, &tm));
	CHECK_FALSE(tm.provider);

	// Live: B goes to always (the service's DIAG re-probe path) — 2D too, mapped into B's space.
	set_env("TST-A=3d;TST-B=always;TST-C=always");
	CHECK(target_viewer_tracking_settings_changed(rig.list, rig.reg));
	CHECK(source(0xB2, XRT_VIEWER_MODE_2D) == XRT_VIEWER_SOURCE_PROVIDER);
	REQUIRE(eyes(0xB2, &e));
	xrt_eye_positions ea;
	REQUIRE(eyes(0xA1, &ea));
	// The provider reports in A's space; A's centre is one screen width to B's left.
	CHECK((e.eyes[0].x - ea.eyes[0].x) == Catch::Approx(-0.4f).margin(0.02f));
	// Nothing moved since: the re-check is a no-op.
	CHECK_FALSE(target_viewer_tracking_settings_changed(rig.list, rig.reg));

	// Live: A off. Its views leave the provider at once; the advertisement stays (static per instance).
	set_env("TST-A=off;TST-B=always;TST-C=always");
	CHECK(target_viewer_tracking_settings_changed(rig.list, rig.reg));
	CHECK(source(XRT_VIEWER_SCREEN_PRIMARY, XRT_VIEWER_MODE_3D) == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(rig.info->supported_eye_tracking_modes ==
	      (TARGET_VIEWER_MODE_MANAGED_BIT | TARGET_VIEWER_MODE_MANUAL_BIT));

	// Reset: no per-screen value is the default — off, as the provider does not opt in by itself.
	set_env("");
	CHECK(target_viewer_tracking_settings_changed(rig.list, rig.reg));
	CHECK(source(0xB2, XRT_VIEWER_MODE_3D) == XRT_VIEWER_SOURCE_NOMINAL);
	target_viewer_screen_decision a = rig.decision(0xA1);
	CHECK(a.d.setting == TARGET_VIEWER_SETTING_DEFAULT);
	CHECK(a.d.effective_setting == TARGET_VIEWER_SETTING_OFF);
}

//! A head device with a 2D entry (index 0) and one untracked 3D entry (index 1, slot 1).
static xrt_device *
make_head()
{
	xrt_device *head = new xrt_device();
	head->rendering_mode_count = 2;
	for (uint32_t i = 0; i < 2; i++) {
		xrt_rendering_mode &m = head->rendering_modes[i];
		m.mode_index = i;
		std::snprintf(m.mode_name, sizeof(m.mode_name), "%s", i == 0 ? "2D" : "3D");
		m.view_count = i == 0 ? 1 : 2;
		m.hardware_display_3d = i != 0;
		m.tile_columns = i == 0 ? 1 : 2;
		m.tile_rows = 1;
		m.view_scale_x = i == 0 ? 1.0f : 0.5f;
		m.view_scale_y = 1.0f;
		m.view_width_pixels = i == 0 ? 1920 : 960;
		m.view_height_pixels = 1080;
		m.atlas_width_pixels = 1920;
		m.atlas_height_pixels = 1080;
		m.mode_flags = 0;
	}
	return head;
}

static bool
entry_tracked(uint64_t screen_id, uint32_t entry)
{
	u_screen_mode_table *t = new u_screen_mode_table();
	const bool have = u_screen_modes_get(screen_id, t);
	REQUIRE(have);
	REQUIRE(entry < t->count);
	const bool tracked = (t->modes[entry].mode_flags & XRT_RENDERING_MODE_FLAG_HAS_TRACKING) != 0;
	delete t;
	return tracked;
}

TEST_CASE("per-screen setting: the first mode-table publish already carries each screen's tracked flags",
          "[viewer_tracking_per_screen][script]")
{
	// The mode tables' per-entry tracked flags read the per-screen viewer
	// decisions: system creation publishes both in one step, decisions first, so
	// the first table is right (it used to wait for the next re-probe).
	set_env("TST-A=3d;TST-B=always;TST-C=always");
	Rig rig(false);
	xrt_device *head = make_head();
	target_screen_modes_publish_with_viewers(rig.list, rig.reg, head, true, false);

	// A (3d): the 3D entry is tracked, the 2D one is not.
	CHECK(entry_tracked(0xA1, 1));
	CHECK_FALSE(entry_tracked(0xA1, 0));
	// B (always, not the primary — the screen a stale table got wrong): both.
	CHECK(entry_tracked(0xB2, 1));
	CHECK(entry_tracked(0xB2, 0));
	// C tracks the viewer itself: its own (head-copied, untracked) flags stand.
	CHECK_FALSE(entry_tracked(0xC3, 1));

	// Live: B goes off. The re-check re-publishes the tables with the new flags.
	set_env("TST-A=3d;TST-B=off;TST-C=always");
	CHECK(target_screen_modes_viewer_settings_changed(rig.list, rig.reg, head));
	CHECK_FALSE(entry_tracked(0xB2, 1));
	CHECK_FALSE(entry_tracked(0xB2, 0));
	CHECK(entry_tracked(0xA1, 1));
	// Nothing moved since: no re-publish.
	CHECK_FALSE(target_screen_modes_viewer_settings_changed(rig.list, rig.reg, head));

	u_screen_modes_publish(nullptr, 0);
	delete head;
}

TEST_CASE("per-screen setting: no per-screen value and no opt-in advertises nothing",
          "[viewer_tracking_per_screen][script_default]")
{
	set_env("");
	Rig rig;
	CHECK(rig.info->supported_eye_tracking_modes == 0);
	CHECK(source(XRT_VIEWER_SCREEN_PRIMARY, XRT_VIEWER_MODE_3D) == XRT_VIEWER_SOURCE_NOMINAL);
	target_viewer_screen_decision a = rig.decision(0xA1);
	CHECK(a.offered); // a provider is available: the selector is offered, set to off
}

TEST_CASE("per-screen setting: the stub build reads 'unavailable: no face estimator'",
          "[viewer_tracking_per_screen][stub]")
{
	set_env("TST-A=always;TST-B=3d");
	Rig rig;
	// Nothing resident (the provider declined), so nothing is offered and nothing tracks.
	CHECK(source(0xA1, XRT_VIEWER_MODE_3D) == XRT_VIEWER_SOURCE_NOMINAL);
	CHECK(source(0xB2, XRT_VIEWER_MODE_2D) == XRT_VIEWER_SOURCE_NOMINAL);
	for (uint64_t id : {(uint64_t)0xA1, (uint64_t)0xB2}) {
		CAPTURE(id);
		target_viewer_screen_decision d = rig.decision(id);
		CHECK_FALSE(d.offered);
		// The default build compiles the stub estimator; an onnx build on a box without a
		// camera stops one step later ("no camera").
		const std::string why = d.offer_reason;
		CAPTURE(why);
		CHECK(
		    (why.find("no face estimator") != std::string::npos || why.find("no camera") != std::string::npos));
	}
	CHECK(rig.info->supported_eye_tracking_modes == 0);
}
