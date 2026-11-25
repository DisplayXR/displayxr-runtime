// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-066 C1: sim_display's declared rendering-mode list, its config
 *         (OUTPUT: which entry the 3D slot maps to, Same as 2D by default),
 *         its start-up rule, and a live config switch through the runtime's
 *         own selection + remap.
 *
 * Pins sim_display_modes.h, the pure half of sim_display_hmd_create(), so the
 * list is checked on every platform without loading the plug-in (Windows
 * plug-in discovery is registry-only, so a CI or dev box runs whatever
 * sim-display is registered, not necessarily this build's). The list is then
 * run through the runtime's real selection (u_device_enforce_two_rendering_modes)
 * and a live switch (u_device_rendering_modes_refresh_config) on a device that
 * answers XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE the way sim_display does.
 */

#include "catch_amalgamated.hpp"

#include "sim_display_modes.h"
#include "util/u_rendering_modes.h"
#include "util/u_tiling.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct kind_row
{
	const char *setting;
	enum sim_display_output_mode kind;
	const char *name;
	uint32_t views;
	float sx;
	float sy;
	uint32_t cols;
	uint32_t rows;
	uint32_t entry; //!< its index in the declared list
};

const kind_row kKinds[] = {
    {"default", SIM_DISPLAY_OUTPUT_PASSTHROUGH, "Same as 2D", 1, 1.0f, 1.0f, 1, 1, 1},
    {"same_as_2d", SIM_DISPLAY_OUTPUT_PASSTHROUGH, "Same as 2D", 1, 1.0f, 1.0f, 1, 1, 1},
    {"2d", SIM_DISPLAY_OUTPUT_PASSTHROUGH, "Same as 2D", 1, 1.0f, 1.0f, 1, 1, 1},
    {"passthrough", SIM_DISPLAY_OUTPUT_PASSTHROUGH, "Same as 2D", 1, 1.0f, 1.0f, 1, 1, 1},
    {"anaglyph", SIM_DISPLAY_OUTPUT_ANAGLYPH, "Anaglyph", 2, 0.5f, 0.5f, 2, 1, 2},
    {"sbs", SIM_DISPLAY_OUTPUT_SBS, "Cropped SBS", 2, 0.5f, 0.5f, 1, 2, 3},
    {"squeezed", SIM_DISPLAY_OUTPUT_SQUEEZED_SBS, "Squeezed SBS", 2, 0.5f, 1.0f, 2, 1, 4},
    {"squeezed_sbs", SIM_DISPLAY_OUTPUT_SQUEEZED_SBS, "Squeezed SBS", 2, 0.5f, 1.0f, 2, 1, 4},
    {"interlaced", SIM_DISPLAY_OUTPUT_INTERLACED, "Interlaced", 2, 0.5f, 0.5f, 2, 1, 5},
    {"interlace", SIM_DISPLAY_OUTPUT_INTERLACED, "Interlaced", 2, 0.5f, 0.5f, 2, 1, 5},
    {"blend", SIM_DISPLAY_OUTPUT_BLEND, "Blend", 2, 0.5f, 0.5f, 2, 1, 6},
    {"quad", SIM_DISPLAY_OUTPUT_QUAD, "Quad", 4, 0.5f, 0.5f, 2, 2, 7},
};

} // namespace

TEST_CASE("the declared list: 2D, Same as 2D, every kind, Quad last", "[sim_display_modes]")
{
	for (bool quad : {false, true}) {
		INFO("quad declared = " << quad);
		xrt_rendering_mode modes[XRT_MAX_RENDERING_MODES]{};
		enum sim_display_output_mode kinds[XRT_MAX_RENDERING_MODES]{};
		const uint32_t n = sim_display_fill_declared(quad, false, modes, kinds);
		REQUIRE(n == 8);

		// Entry 0: the flat mode, never configurable.
		CHECK(std::string(modes[0].mode_name) == "2D");
		CHECK(u_rendering_mode_is_flat(&modes[0]));

		for (const kind_row &k : kKinds) {
			INFO("OUTPUT=" << k.setting);
			enum sim_display_output_mode kind = SIM_DISPLAY_OUTPUT_QUAD;
			REQUIRE(sim_display_kind_parse(k.setting, &kind));
			CHECK(kind == k.kind);
			REQUIRE(sim_display_entry_for_kind(kind, n) == k.entry);
			const xrt_rendering_mode &m = modes[k.entry];
			CHECK(m.mode_index == k.entry);
			CHECK(std::string(m.mode_name) == k.name);
			CHECK(m.view_count == k.views);
			CHECK(m.view_scale_x == Catch::Approx(k.sx));
			CHECK(m.view_scale_y == Catch::Approx(k.sy));
			CHECK(m.tile_columns == k.cols);
			CHECK(m.tile_rows == k.rows);
			CHECK(m.tile_columns * m.tile_rows == m.view_count);
			CHECK(u_rendering_mode_is_3d(&m));
			CHECK((m.mode_flags & XRT_RENDERING_MODE_FLAG_HAS_TRACKING) == 0); // no tracking source
		}
		// Same as 2D: a passthrough 3D entry that does not set the lens.
		CHECK(u_rendering_mode_is_passthrough_3d(&modes[1]));
		CHECK_FALSE(modes[1].hardware_display_3d);
		// Quad: start-up-only unless declared.
		CHECK(((modes[7].mode_flags & XRT_RENDERING_MODE_FLAG_START_UP_ONLY) != 0) == !quad);
		CHECK(u_rendering_modes_envelope_of(modes, n) == (quad ? 8u : 7u));
		CHECK(u_rendering_modes_max_view_count(modes, u_rendering_modes_envelope_of(modes, n)) ==
		      (quad ? 4u : 2u));
	}
}

TEST_CASE("OUTPUT unset is the Same-as-2D config", "[sim_display_modes]")
{
	// Project lead, 2026-11-23: most apps ask for 3D at start, and an anaglyph
	// on an ordinary screen looks broken - a 3D look is chosen on purpose.
	for (const char *v : {(const char *)nullptr, ""}) {
		enum sim_display_output_mode kind = SIM_DISPLAY_OUTPUT_QUAD;
		CHECK(sim_display_kind_parse(v, &kind));
		CHECK(kind == SIM_DISPLAY_OUTPUT_PASSTHROUGH);
	}
	CHECK(SIM_DISPLAY_DEFAULT_KIND == SIM_DISPLAY_OUTPUT_PASSTHROUGH);
}

TEST_CASE("an unknown OUTPUT keeps the default and says so", "[sim_display_modes]")
{
	enum sim_display_output_mode kind = SIM_DISPLAY_OUTPUT_QUAD;
	CHECK_FALSE(sim_display_kind_parse("hologram", &kind));
	CHECK(kind == SIM_DISPLAY_OUTPUT_PASSTHROUGH);
	CHECK(sim_display_kind_desc_get((enum sim_display_output_mode)99).output == SIM_DISPLAY_OUTPUT_PASSTHROUGH);
}

TEST_CASE("the tracking seam marks every 3D entry when a source is on", "[sim_display_modes]")
{
	xrt_rendering_mode modes[XRT_MAX_RENDERING_MODES]{};
	const uint32_t n = sim_display_fill_declared(false, true, modes, nullptr);
	CHECK((modes[0].mode_flags & XRT_RENDERING_MODE_FLAG_HAS_TRACKING) == 0);
	for (uint32_t i = 1; i < n; i++) {
		CHECK((modes[i].mode_flags & XRT_RENDERING_MODE_FLAG_HAS_TRACKING) != 0);
		CHECK(sim_display_entry_tracked(i, true));
		CHECK_FALSE(sim_display_entry_tracked(i, false));
	}
}

TEST_CASE("the device starts in 2D unless START_3D", "[sim_display_modes]")
{
	sim_display_start s = sim_display_resolve_start(false, -1);
	CHECK(s.active == 0);
	CHECK_FALSE(s.pinned);
	CHECK_FALSE(s.force_ignored);
	s = sim_display_resolve_start(true, -1);
	CHECK(s.active == 1);
	CHECK_FALSE(s.pinned);
}

TEST_CASE("FORCE_MODE pins 0 or 1 and ignores anything else", "[sim_display_modes]")
{
	sim_display_start s = sim_display_resolve_start(true, 0);
	CHECK(s.active == 0);
	CHECK(s.pinned);
	s = sim_display_resolve_start(false, 1);
	CHECK(s.active == 1);
	CHECK(s.pinned);
	for (long v : {2L, 3L, 4L, 5L, 99L, -2L}) {
		INFO("FORCE_MODE=" << v);
		s = sim_display_resolve_start(false, v);
		CHECK(s.active == 0);
		CHECK_FALSE(s.pinned);
		CHECK(s.force_ignored);
	}
}

TEST_CASE("the worst-case atlas is the panel's whatever the config", "[sim_display_modes]")
{
	// ADR-010 still holds - size once, never recreate - and now over the whole
	// envelope: every sim_display entry fits the 2D mode's full-resolution
	// atlas, so sim_display never needs a recreation (ADR-066 C1.5).
	for (bool quad : {false, true}) {
		xrt_rendering_mode modes[XRT_MAX_RENDERING_MODES]{};
		const uint32_t n = sim_display_fill_declared(quad, false, modes, nullptr);
		const uint32_t env = u_rendering_modes_envelope_of(modes, n);
		for (uint32_t i = 0; i < env; i++) {
			u_tiling_compute_mode(&modes[i], 1920, 1080);
		}
		uint32_t w = 0, h = 0;
		u_tiling_compute_system_atlas(modes, env, &w, &h);
		CHECK(w == 1920);
		CHECK(h == 1080);
	}
}

TEST_CASE("every consumer resolves the output from the ACTIVE slot", "[sim_display_modes]")
{
	const auto Q = SIM_DISPLAY_OUTPUT_QUAD;
	const auto P = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
	const auto I = SIM_DISPLAY_OUTPUT_INTERLACED;
	CHECK(sim_display_output_resolve(true, 1, I, P) == I);
	CHECK(sim_display_output_resolve(true, 1, Q, P) == Q);
	CHECK(sim_display_output_resolve(true, 0, I, I) == P);
	CHECK(sim_display_output_resolve(false, 1, I, P) == P);
	// Same as 2D in slot 1: passthrough, one tile.
	CHECK(sim_display_output_resolve(true, 1, P, P) == P);
	CHECK(sim_display_output_draw(1, P, P) == P);
	CHECK(sim_display_output_draw(1, I, I) == P);
	CHECK(sim_display_output_draw(2, I, I) == I);
	CHECK(sim_display_output_draw(4, Q, Q) == Q);
	CHECK(sim_display_output_draw(2, P, I) == I);
}


/*
 *
 * A live config switch through the runtime's own selection + remap.
 *
 */

namespace {

//! A head with sim_display's list whose CONFIGURED_3D_MODE answers OUTPUT, like the driver.
struct sim_like_head
{
	xrt_device xdev{};
	xrt_hmd_parts hmd{};
	const char *output = nullptr; //!< the OUTPUT setting, re-read on every query
	uint32_t count = 0;
	std::vector<int32_t> output_mode_calls;
	bool destroyed = false;

	static sim_like_head *
	of(xrt_device *x)
	{
		return reinterpret_cast<sim_like_head *>(x);
	}

	static xrt_result_t
	set_prop(xrt_device *x, xrt_device_property_id p, int32_t v)
	{
		sim_like_head *h = of(x);
		if (p != XRT_DEVICE_PROPERTY_OUTPUT_MODE || v < 0 || (uint32_t)v >= h->count) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		h->output_mode_calls.push_back(v);
		x->hmd->active_rendering_mode_index = (uint32_t)v;
		return XRT_SUCCESS;
	}

	static xrt_result_t
	get_prop(xrt_device *x, xrt_device_property_id p, int32_t *out)
	{
		sim_like_head *h = of(x);
		if (p != XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		enum sim_display_output_mode k = SIM_DISPLAY_DEFAULT_KIND;
		(void)sim_display_kind_parse(h->output, &k);
		*out = (int32_t)sim_display_entry_for_kind(k, h->count);
		return XRT_SUCCESS;
	}

	static void
	destroy(xrt_device *x)
	{
		of(x)->destroyed = true;
	}

	sim_like_head(const char *out, bool quad)
	{
		output = out;
		count = sim_display_fill_declared(quad, false, xdev.rendering_modes, nullptr);
		xdev.rendering_mode_count = count;
		for (uint32_t i = 0; i < count; i++) {
			u_tiling_compute_mode(&xdev.rendering_modes[i], 1920, 1080);
		}
		hmd.view_count = quad ? 4 : 2;
		xdev.hmd = &hmd;
		xdev.set_property = set_prop;
		xdev.get_property = get_prop;
		xdev.destroy = destroy;
	}

	~sim_like_head()
	{
		if (!destroyed && xdev.destroy != nullptr) {
			xdev.destroy(&xdev);
		}
	}
};

} // namespace

TEST_CASE("live: switching OUTPUT remaps slot 1 and bumps the table generation", "[sim_display_modes][adr066]")
{
	u_rendering_modes_set_choice_resolver(nullptr);
	sim_like_head h(nullptr, false); // the default config
	REQUIRE(u_device_enforce_two_rendering_modes(&h.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	CHECK(h.xdev.rendering_mode_count == 2);
	CHECK(std::string(h.xdev.rendering_modes[1].mode_name) == "Same as 2D");
	CHECK(u_device_rendering_modes_envelope_count(&h.xdev) == 7); // Quad listed start-up-only
	CHECK(h.hmd.view_count == 2);

	// The app goes 3D (slot 1).
	REQUIRE(xrt_device_set_property(&h.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 1) == XRT_SUCCESS);
	CHECK(h.output_mode_calls.back() == 1);
	const uint32_t g0 = u_device_rendering_modes_generation(&h.xdev);

	struct step
	{
		const char *output;
		const char *slot1;
		uint32_t driver_entry;
		uint32_t views;
	};
	const step steps[] = {{"anaglyph", "Anaglyph", 2, 2},
	                      {"sbs", "Cropped SBS", 3, 2},
	                      {"default", "Same as 2D", 1, 1},
	                      {"interlaced", "Interlaced", 5, 2}};
	uint32_t gen = g0;
	for (const step &st : steps) {
		INFO("OUTPUT=" << st.output);
		h.output = st.output;
		REQUIRE(u_device_rendering_modes_refresh_config(&h.xdev, "sim-display"));
		CHECK(u_device_rendering_modes_generation(&h.xdev) == ++gen);
		// The new table, same index, same sizes.
		CHECK(h.xdev.rendering_mode_count == 2);
		CHECK(h.hmd.active_rendering_mode_index == 1);
		CHECK(std::string(h.xdev.rendering_modes[1].mode_name) == st.slot1);
		CHECK(h.xdev.rendering_modes[1].view_count == st.views);
		CHECK(h.xdev.rendering_modes[1].view_width_pixels != 0); // tiled already
		CHECK(h.hmd.view_count == 2);
		// The driver was told its new entry, in its own numbering.
		CHECK(h.output_mode_calls.back() == (int32_t)st.driver_entry);
	}

	SECTION("quad is not declared on this run: refused, applies at the next start")
	{
		h.output = "quad";
		CHECK_FALSE(u_device_rendering_modes_refresh_config(&h.xdev, "sim-display"));
		CHECK(u_device_rendering_modes_generation(&h.xdev) == gen);
		CHECK(std::string(h.xdev.rendering_modes[1].mode_name) == "Interlaced");
	}
}
