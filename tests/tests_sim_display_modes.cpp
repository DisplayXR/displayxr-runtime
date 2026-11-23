// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-065: sim_display's two run-time modes — 2D and the 3D kind its
 *         OUTPUT setting names — and its start-up rule (2D unless START_3D;
 *         FORCE_MODE pins 0 or 1 only).
 *
 * Pins sim_display_modes.h, the pure half of sim_display_hmd_create(), so the
 * table every kind produces is checked on every platform without loading the
 * plug-in (Windows plug-in discovery is registry-only, so a CI or dev box runs
 * whatever sim-display is registered, not necessarily this build's). Each
 * table is also run through the runtime's own two-mode rule to prove it
 * already conforms: u_rendering_modes_trim leaves it untouched.
 */

#include "catch_amalgamated.hpp"

#include "sim_display_modes.h"
#include "util/u_rendering_modes.h"
#include "util/u_tiling.h"

#include <cstring>

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
};

const kind_row kKinds[] = {
    {"anaglyph", SIM_DISPLAY_OUTPUT_ANAGLYPH, "Anaglyph", 2, 0.5f, 0.5f, 2, 1},
    {"sbs", SIM_DISPLAY_OUTPUT_SBS, "Cropped SBS", 2, 0.5f, 0.5f, 1, 2},
    {"squeezed", SIM_DISPLAY_OUTPUT_SQUEEZED_SBS, "Squeezed SBS", 2, 0.5f, 1.0f, 2, 1},
    {"squeezed_sbs", SIM_DISPLAY_OUTPUT_SQUEEZED_SBS, "Squeezed SBS", 2, 0.5f, 1.0f, 2, 1},
    {"quad", SIM_DISPLAY_OUTPUT_QUAD, "Quad", 4, 0.5f, 0.5f, 2, 2},
    {"interlaced", SIM_DISPLAY_OUTPUT_INTERLACED, "Interlaced", 2, 0.5f, 0.5f, 2, 1},
    {"interlace", SIM_DISPLAY_OUTPUT_INTERLACED, "Interlaced", 2, 0.5f, 0.5f, 2, 1},
    // blend finally has a run-time home: it used to have no mode index at all.
    {"blend", SIM_DISPLAY_OUTPUT_BLEND, "Blend", 2, 0.5f, 0.5f, 2, 1},
};

} // namespace

TEST_CASE("every OUTPUT kind yields exactly 2D + that kind", "[sim_display_modes]")
{
	for (const kind_row &k : kKinds) {
		INFO("OUTPUT=" << k.setting);
		enum sim_display_output_mode kind = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
		bool is_2d = true;
		REQUIRE(sim_display_kind_parse(k.setting, &kind, &is_2d));
		CHECK(kind == k.kind);
		CHECK_FALSE(is_2d);

		xrt_rendering_mode modes[XRT_MAX_RENDERING_MODES]{};
		sim_display_fill_modes(kind, modes);

		// Mode 0: the flat mode.
		CHECK(modes[0].mode_index == 0);
		CHECK(std::strcmp(modes[0].mode_name, "2D") == 0);
		CHECK(modes[0].view_count == 1);
		CHECK_FALSE(modes[0].hardware_display_3d);
		CHECK(modes[0].tile_columns == 1);
		CHECK(modes[0].tile_rows == 1);
		CHECK(modes[0].view_scale_x == Catch::Approx(1.0f));
		CHECK(modes[0].view_scale_y == Catch::Approx(1.0f));

		// Mode 1: the kind.
		CHECK(modes[1].mode_index == 1);
		CHECK(std::strcmp(modes[1].mode_name, k.name) == 0);
		CHECK(modes[1].view_count == k.views);
		CHECK(modes[1].hardware_display_3d);
		CHECK(modes[1].tile_columns == k.cols);
		CHECK(modes[1].tile_rows == k.rows);
		CHECK(modes[1].view_scale_x == Catch::Approx(k.sx));
		CHECK(modes[1].view_scale_y == Catch::Approx(k.sy));
		CHECK(modes[1].tile_columns * modes[1].tile_rows == modes[1].view_count);
		CHECK(modes[1].mode_flags == 0); // tracking is added by the device, not the table
		CHECK(sim_display_kind_desc_get(kind).output == k.kind);

		// Already conforms to the runtime's two-mode rule: nothing to trim.
		uint32_t count = 2;
		uint32_t active = 1;
		u_rendering_modes_trim_info info{};
		CHECK(u_rendering_modes_trim(modes, &count, &active, &info) == U_RENDERING_MODES_TRIM_UNCHANGED);
		CHECK_FALSE(info.renumbered);
		CHECK(count == 2);
	}
}

TEST_CASE("OUTPUT unset is the anaglyph kind", "[sim_display_modes]")
{
	// The default KIND. Anaglyph is the only kind that keeps positions (a
	// per-pixel colour combine, no canvas rearrangement), so it composes with
	// display zones; the rearranging kinds are opt-in inspection tools.
	for (const char *v : {(const char *)nullptr, ""}) {
		enum sim_display_output_mode kind = SIM_DISPLAY_OUTPUT_QUAD;
		bool is_2d = true;
		CHECK(sim_display_kind_parse(v, &kind, &is_2d));
		CHECK(kind == SIM_DISPLAY_OUTPUT_ANAGLYPH);
		CHECK_FALSE(is_2d);
	}
}

TEST_CASE("OUTPUT=2d chooses no kind and forces a 2D start", "[sim_display_modes]")
{
	for (const char *v : {"2d", "passthrough"}) {
		INFO("OUTPUT=" << v);
		enum sim_display_output_mode kind = SIM_DISPLAY_OUTPUT_QUAD;
		bool is_2d = false;
		CHECK(sim_display_kind_parse(v, &kind, &is_2d));
		CHECK(is_2d);
		// Mode 1 is the default kind, so V still has somewhere to go.
		CHECK(kind == SIM_DISPLAY_OUTPUT_ANAGLYPH);
		xrt_rendering_mode modes[2]{};
		sim_display_fill_modes(kind, modes);
		CHECK(std::strcmp(modes[1].mode_name, "Anaglyph") == 0);
	}
}

TEST_CASE("an unknown OUTPUT keeps the anaglyph default and says so", "[sim_display_modes]")
{
	enum sim_display_output_mode kind = SIM_DISPLAY_OUTPUT_QUAD;
	bool is_2d = true;
	CHECK_FALSE(sim_display_kind_parse("hologram", &kind, &is_2d));
	CHECK(kind == SIM_DISPLAY_OUTPUT_ANAGLYPH);
	CHECK_FALSE(is_2d);
}

TEST_CASE("a non-kind output value falls back to the anaglyph descriptor", "[sim_display_modes]")
{
	CHECK(sim_display_kind_desc_get(SIM_DISPLAY_OUTPUT_PASSTHROUGH).output == SIM_DISPLAY_OUTPUT_ANAGLYPH);
	CHECK(sim_display_kind_desc_get((enum sim_display_output_mode)99).output == SIM_DISPLAY_OUTPUT_ANAGLYPH);
}

TEST_CASE("the device starts in 2D unless START_3D", "[sim_display_modes]")
{
	SECTION("default: 2D, unpinned")
	{
		const sim_display_start s = sim_display_resolve_start(false, false, -1);
		CHECK(s.active == 0);
		CHECK_FALSE(s.pinned);
		CHECK_FALSE(s.force_ignored);
		CHECK_FALSE(s.start_3d_moot);
	}
	SECTION("START_3D: the kind")
	{
		const sim_display_start s = sim_display_resolve_start(false, true, -1);
		CHECK(s.active == 1);
		CHECK_FALSE(s.pinned);
	}
	SECTION("OUTPUT=2d outranks START_3D, and says so")
	{
		const sim_display_start s = sim_display_resolve_start(true, true, -1);
		CHECK(s.active == 0);
		CHECK(s.start_3d_moot);
	}
}

TEST_CASE("FORCE_MODE pins 0 or 1 and ignores anything else", "[sim_display_modes]")
{
	SECTION("0 pins 2D, even over START_3D")
	{
		const sim_display_start s = sim_display_resolve_start(false, true, 0);
		CHECK(s.active == 0);
		CHECK(s.pinned);
	}
	SECTION("1 pins the kind, even from a 2D start")
	{
		const sim_display_start s = sim_display_resolve_start(false, false, 1);
		CHECK(s.active == 1);
		CHECK(s.pinned);
	}
	SECTION("1 pins mode 1 under OUTPUT=2d too (an explicit pin wins)")
	{
		const sim_display_start s = sim_display_resolve_start(true, false, 1);
		CHECK(s.active == 1);
		CHECK(s.pinned);
	}
	SECTION("the old multi-mode indices are ignored, not clamped")
	{
		for (long v : {2L, 3L, 4L, 5L, 99L, -2L}) {
			INFO("FORCE_MODE=" << v);
			const sim_display_start s = sim_display_resolve_start(false, false, v);
			CHECK(s.active == 0);
			CHECK_FALSE(s.pinned);
			CHECK(s.force_ignored);
		}
	}
}

TEST_CASE("the worst-case atlas covers 2D and the selected kind only", "[sim_display_modes]")
{
	// ADR-010 still holds — size once, never recreate — over the two modes the
	// device serves. On a 1920x1080 panel every sim kind fits the 2D mode's
	// full-resolution atlas, so the size is the panel's; the view count is the
	// kind's (4 only for quad).
	for (const kind_row &k : kKinds) {
		INFO("OUTPUT=" << k.setting);
		xrt_rendering_mode modes[2]{};
		sim_display_fill_modes(k.kind, modes);
		for (auto &m : modes) {
			u_tiling_compute_mode(&m, 1920, 1080);
		}
		uint32_t w = 0, h = 0;
		u_tiling_compute_system_atlas(modes, 2, &w, &h);
		CHECK(w == 1920);
		CHECK(h == 1080);
		CHECK(u_rendering_modes_max_view_count(modes, 2) == k.views);
	}
}

TEST_CASE("F6: every consumer resolves the output from the ACTIVE mode", "[sim_display_modes]")
{
	const auto Q = SIM_DISPLAY_OUTPUT_QUAD;
	const auto P = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
	const auto I = SIM_DISPLAY_OUTPUT_INTERLACED;
	SECTION("a direct active-index write to 1 means the kind, whatever was last requested")
	{
		// The case F6 is about: started in 2D (last request passthrough), then a
		// runtime writer set active = 1 without an OUTPUT_MODE request.
		CHECK(sim_display_output_resolve(true, 1, I, P) == I);
		CHECK(sim_display_output_resolve(true, 1, Q, P) == Q);
	}
	SECTION("active 0 is passthrough even if the last request was the kind")
	{
		CHECK(sim_display_output_resolve(true, 0, I, I) == P);
	}
	SECTION("no device: the last request stands")
	{
		CHECK(sim_display_output_resolve(false, 1, I, P) == P);
		CHECK(sim_display_output_resolve(false, 0, I, I) == I);
	}
	SECTION("the draw agrees with the resolution, except a 1-tile atlas is always flat")
	{
		CHECK(sim_display_output_draw(1, I, I) == P);
		CHECK(sim_display_output_draw(2, I, I) == I);
		CHECK(sim_display_output_draw(4, Q, Q) == Q);
		// A multi-tile atlas on the frame the active mode went back to 2D.
		CHECK(sim_display_output_draw(2, P, I) == I);
	}
}
