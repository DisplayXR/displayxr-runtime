// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The #1589/#1610 colour model, pinned STRUCTURALLY in every backend.
 *
 * `tests_aux_color_encoding.cpp` pins what the two shared decisions ARE — the
 * OETF an atlas capture must read back, the fast-path truth table, and what
 * "the atlas holds encoded bytes" answers. This file pins that the backends
 * actually ASK, rather than each carrying a private copy of the answer.
 *
 * That distinction is the whole reason `u_color_encoding.h` exists. #1621 is
 * the cautionary tale one layer down: Metal and GL each shipped a faithful
 * local implementation of the blend rule, each measured self-consistently, and
 * only a cross-backend comparison found that both were the INVERSE of the
 * intended one. Colour has the same shape and a worse failure mode — a local
 * "is this sRGB?" test that disagrees with the shared predicate does not crash,
 * it just makes one graphics API a stop brighter than another, which is exactly
 * how D3D12 came to fail eight of the CTS's eleven
 * GradientFormatsLinearVsNonLinear pairs while D3D11 passed them all.
 *
 * The behaviour itself needs a GPU — the encode is done by a hardware `_SRGB`
 * render target, and there is no device in this harness — so what is pinnable
 * here is the source text, the same technique and the same file read as the
 * structural cases in tests_comp_layer_view_camera.cpp.
 *
 * @ingroup tests
 */

#include "catch_amalgamated.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string
read_whole_file(const std::string &path)
{
	std::ifstream f(path);
	REQUIRE(f.good());
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

/*!
 * Does @p src contain @p needle OUTSIDE a comment?
 *
 * The negative halves below are about code, not prose: every one of these
 * spellings is expected to appear in a comment somewhere (that is where the
 * rule gets explained), and a guard that could not tell the two apart would
 * have to be written as "do not mention", which nobody can follow.
 *
 * Line-granular and deliberately crude — a line whose first non-blank token
 * opens or continues a block comment is prose. Good enough for the one thing
 * it must never do, which is miss a real call.
 */
bool
contains_in_code(const std::string &src, const std::string &needle)
{
	std::stringstream ss(src);
	std::string line;
	while (std::getline(ss, line)) {
		const size_t first = line.find_first_not_of(" \t");
		if (first == std::string::npos) {
			continue;
		}
		const std::string lead = line.substr(first);
		if (lead.rfind("*", 0) == 0 || lead.rfind("//", 0) == 0 || lead.rfind("/*", 0) == 0) {
			continue;
		}
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

//! Every native compositor that composes layers into an atlas the display
//! processor consumes, relative to the compositor source root. GL, Metal and
//! vk_native are deliberately absent: their #1589 legs are still open, and a
//! test that fails for a known-open leg is noise, not a guard. Add a backend
//! here the moment it grows a compose target.
const char *const kColorBackends[] = {
    "d3d11/comp_d3d11_renderer.cpp",
    "d3d11_service/comp_d3d11_service.cpp",
    "d3d12/comp_d3d12_renderer.cpp",
};

//! The two IN-PROCESS compositors that own the zero-copy branch. (The service's
//! lives in comp_d3d11_service.cpp, which is in the list above.)
const char *const kZeroCopyOwners[] = {
    "d3d11/comp_d3d11_compositor.cpp",
    "d3d12/comp_d3d12_compositor.cpp",
    "d3d11_service/comp_d3d11_service.cpp",
};

} // namespace


TEST_CASE("colour: no backend re-derives the fast-path decision (#1589/#1610)")
{
	for (const char *rel : kColorBackends) {
		const std::string path = std::string(DXR_COMP_SRC_DIR) + "/" + rel;
		const std::string src = read_whole_file(path);

		// Positive: the frame's regime comes from the SHARED predicate.
		INFO(rel << " must decide 'does this frame owe an encode or a blend?' through "
		         << "u_color_compose_fast_path() — a local rule is how two backends come to "
		         << "carry diverging copies of one decision (u_color_encoding.h)");
		CHECK(contains_in_code(src, "u_color_compose_fast_path("));

		// ...and the escape hatch is the shared one, read once, not a
		// second getenv with its own spelling and its own caching.
		INFO(rel << " must read the escape hatch through u_color_legacy_unorm_encoded()");
		CHECK(contains_in_code(src, "u_color_legacy_unorm_encoded("));

		// Negative: nobody reads the environment variable directly.
		INFO(rel << " names DXR_COLOR_LEGACY_UNORM_ENCODED in CODE — the hatch is a shared "
		         << "accessor precisely so one process cannot be half rolled back");
		CHECK_FALSE(contains_in_code(src, "DXR_COLOR_LEGACY_UNORM_ENCODED"));

		// ...and the regime is stated once per component, so a hardware
		// check can prove which one a session ran under.
		INFO(rel << " must call u_color_log_state_once() — that WARN is the only evidence a "
		         << "capture has for which colour regime produced it");
		CHECK(contains_in_code(src, "u_color_log_state_once("));
	}
}

TEST_CASE("colour: the encode is a render target, never shader arithmetic (#1610)")
{
	for (const char *rel : kColorBackends) {
		const std::string path = std::string(DXR_COMP_SRC_DIR) + "/" + rel;
		const std::string src = read_whole_file(path);

		// The compose target's RTV format comes from the one rule, so
		// "which member of the family encodes?" is answered in a single
		// place for both D3D legs.
		INFO(rel << " must derive its compose target's RTV from d3d_dxgi_format_srgb_rtv() "
		         << "(d3d_dxgi_formats.h) rather than naming a format inline");
		CHECK(contains_in_code(src, "d3d_dxgi_format_srgb_rtv("));

		// The composite reaches the atlas as a COPY. A shader blit there
		// samples encoded bytes and writes them through a target that
		// encodes again — the double-apply this whole design exists to
		// avoid, and invisible in code review because it looks like every
		// other blit in the file.
		INFO(rel << " must publish its composite with a same-family COPY; a draw would "
		         << "re-apply the transfer function");
		CHECK(contains_in_code(src, "CopyResource("));

		/*
		 * No transfer function in any compose shader. The HLSL lives in
		 * these files as string literals (D3D12) or beside them
		 * (D3D11's shaders/), and a hand-rolled encode is recognisable
		 * by its constants: 1.055, 0.055, 2.4 or 1.0/2.4, and the
		 * 0.0031308 knee. u_color_srgb_encode() is allowed to hold them
		 * — it IS the oracle — but no backend may.
		 */
		for (const char *magic : {"1.055", "0.055", "0.0031308", "2.4)", "1.0 / 2.4", "1.0/2.4"}) {
			INFO(rel << " contains the sRGB OETF constant \"" << magic
			         << "\" — the hardware `_SRGB` render target applies that curve, and a "
			         << "second copy in a shader double-applies it (ADR-021 §2)");
			CHECK(src.find(magic) == std::string::npos);
		}
	}
}

TEST_CASE("colour: zero-copy refuses a source that still owes the encode (#1589)")
{
	for (const char *rel : kZeroCopyOwners) {
		const std::string path = std::string(DXR_COMP_SRC_DIR) + "/" + rel;
		const std::string src = read_whole_file(path);

		// Zero-copy hands the app's OWN image to a display processor that
		// is told the atlas is ENCODED, and has no pass in which to
		// encode. A UNORM (scene-linear) swapchain must therefore take the
		// atlas path — where the compose target does it — and say so once.
		INFO(rel << " must refuse zero-copy for a non-_SRGB source: that branch declares the "
		         << "atlas ENCODED and owns no pass that could make it true");
		CHECK(src.find("color_needs_encode") != std::string::npos);

		// It is a precondition on u_tiling_can_zero_copy()'s RESULT, never
		// a second eligibility rule folded into the tiling test (ADR-030).
		INFO(rel << " no longer calls u_tiling_can_zero_copy() — it is the SOLE tiling gate, "
		         << "and the colour rule must sit on its result");
		CHECK(contains_in_code(src, "u_tiling_can_zero_copy("));
	}
}

TEST_CASE("colour: the D3D12 leg keeps its two source views apart (#1589)")
{
	/*
	 * D3D12 differs from D3D11 in where the honest view lives: D3D11 builds
	 * a second SRV per swapchain image at create time, D3D12 resolves a
	 * FORMAT per draw (its descriptors are written per draw anyway, because
	 * the heap is consumed at GPU-execute time). Both spellings answer the
	 * same question, and the failure mode is the same one: a site that keeps
	 * the non-decoding view while the target encodes on write emits a bar a
	 * stop too bright, which is one half of every failing
	 * GradientFormatsLinearVsNonLinear pair.
	 *
	 * So: the renderer must have BOTH accessors available and must route its
	 * per-draw choice through one helper rather than deciding per SRV site —
	 * there are four of them, and three-out-of-four is the bug.
	 */
	const std::string path = std::string(DXR_COMP_SRC_DIR) + "/d3d12/comp_d3d12_renderer.cpp";
	const std::string src = read_whole_file(path);

	INFO(
	    "the D3D12 renderer must pick its source view through ONE helper, so a new SRV site "
	    "cannot quietly keep the non-decoding view while the target encodes");
	CHECK(contains_in_code(src, "layer_source_format("));

	// Every SRV the renderer builds over an APP image goes through that
	// helper; the only direct sample_format() calls left are the ones that
	// genuinely hand the app's bytes on unchanged (the Local2D flatten).
	size_t direct = 0;
	size_t pos = 0;
	while ((pos = src.find("comp_d3d12_swapchain_sample_format(", pos)) != std::string::npos) {
		const size_t nl = src.rfind('\n', pos);
		const size_t bol = nl == std::string::npos ? 0 : nl + 1;
		const std::string prefix = src.substr(bol, pos - bol);
		pos += 1;
		const size_t first = prefix.find_first_not_of(" \t");
		const std::string lead = first == std::string::npos ? "" : prefix.substr(first);
		if (lead.rfind("*", 0) == 0 || lead.rfind("//", 0) == 0) {
			continue;
		}
		direct++;
	}
	INFO("the D3D12 renderer calls comp_d3d12_swapchain_sample_format() directly "
	     << direct
	     << " time(s), expected 2 — one inside layer_source_format() (the fast-path arm) and one "
	        "in comp_d3d12_renderer_flatten_local_2d(), which hands the app's bytes on unchanged. "
	        "A new direct call is a draw that blends in the wrong space.");
	CHECK(direct == 2);
}
