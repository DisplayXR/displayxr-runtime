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

/*!
 * The body of the function whose definition line STARTS with @p name, up to the
 * first line that is a lone closing brace. Crude on purpose — it only has to be
 * right for the tree's own formatting, where a top-level definition's name sits
 * at column 0 and its closing brace is the first `}` at column 0 after it.
 *
 * Needed because one of the pins below is about what a SPECIFIC function does,
 * not about what the file contains: "the publish is a copy" cannot be checked
 * file-wide in the GL leg, which legitimately blits elsewhere.
 */
std::string
function_body(const std::string &src, const std::string &name)
{
	const size_t def = src.find("\n" + name + "(");
	if (def == std::string::npos) {
		return "";
	}
	const size_t open = src.find("\n{", def);
	if (open == std::string::npos) {
		return "";
	}
	const size_t close = src.find("\n}", open + 1);
	if (close == std::string::npos) {
		return "";
	}
	return src.substr(open, close - open);
}

//! How many CODE (non-comment, non-preprocessor) lines of @p src mention
//! @p needle. The preprocessor exclusion matters for the GL leg, where the GL
//! enums it uses are #defined locally (they are not in the tree's GLAD spec).
size_t
count_code_lines_with(const std::string &src, const std::string &needle)
{
	size_t n = 0;
	std::stringstream ss(src);
	std::string line;
	while (std::getline(ss, line)) {
		const size_t first = line.find_first_not_of(" \t");
		if (first == std::string::npos) {
			continue;
		}
		const std::string lead = line.substr(first);
		if (lead.rfind("*", 0) == 0 || lead.rfind("//", 0) == 0 || lead.rfind("/*", 0) == 0 ||
		    lead.rfind("#", 0) == 0) {
			continue;
		}
		if (line.find(needle) != std::string::npos) {
			n++;
		}
	}
	return n;
}

//! Every native compositor that composes layers into an atlas the display
//! processor consumes, relative to the compositor source root. Metal and
//! vk_native are deliberately absent: their #1589 legs are still open, and a
//! test that fails for a known-open leg is noise, not a guard. Add a backend
//! here the moment it grows a compose target.
const char *const kColorBackends[] = {
    "d3d11/comp_d3d11_renderer.cpp",
    "d3d11_service/comp_d3d11_service.cpp",
    "d3d12/comp_d3d12_renderer.cpp",
    "gl/comp_gl_compositor.cpp",
};

/*!
 * The backends whose compose target is a D3D one, and whose publish is
 * therefore `CopyResource` over a TYPELESS family.
 *
 * The rule is the same everywhere — the encode is a property of the RENDER
 * TARGET and the composite reaches the atlas as a raw same-family copy — but
 * its SPELLING is per-API, so the pins split here rather than the rule doing
 * so. GL's half is the case below this list's users.
 */
const char *const kD3DColorBackends[] = {
    "d3d11/comp_d3d11_renderer.cpp",
    "d3d11_service/comp_d3d11_service.cpp",
    "d3d12/comp_d3d12_renderer.cpp",
};

//! The three IN-PROCESS compositors that own the zero-copy branch. (The
//! service's lives in comp_d3d11_service.cpp, which is in the list above.)
const char *const kZeroCopyOwners[] = {
    "d3d11/comp_d3d11_compositor.cpp",
    "d3d12/comp_d3d12_compositor.cpp",
    "d3d11_service/comp_d3d11_service.cpp",
    "gl/comp_gl_compositor.cpp",
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
	for (const char *rel : kD3DColorBackends) {
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
	}

	for (const char *rel : kColorBackends) {
		const std::string path = std::string(DXR_COMP_SRC_DIR) + "/" + rel;
		const std::string src = read_whole_file(path);

		/*
		 * No transfer function in any compose shader. The shader source
		 * lives in these files as string literals (D3D12's HLSL, GL's
		 * GLSL) or beside them (D3D11's shaders/), and a hand-rolled
		 * encode is recognisable
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

TEST_CASE("colour: the GL leg spells the same model in GL (#1589/#1610)")
{
	/*
	 * GL states the SAME model with different machinery, and each of the
	 * three differences is a place a faithful-looking local reinvention
	 * would go unnoticed:
	 *
	 *   - the encode is the ATTACHMENT's format plus GL_FRAMEBUFFER_SRGB,
	 *     not a view (D3D11) or a baked PSO format (D3D12);
	 *   - "does this sampler decode?" is texture-object state, so the twin
	 *     of layer_source_format() is a twin BIND;
	 *   - the publish is glCopyImageSubData, and this file legitimately
	 *     calls glBlitFramebuffer elsewhere (the DP crop), so "the publish
	 *     is a copy" has to be asked of the publish FUNCTION, not of the
	 *     file.
	 */
	const std::string path = std::string(DXR_COMP_SRC_DIR) + "/gl/comp_gl_compositor.cpp";
	const std::string src = read_whole_file(path);

	INFO("the GL compositor must compose into a GL_SRGB8_ALPHA8 target — that format IS the "
	     "encode, and it is what makes the fixed-function blender work in linear");
	CHECK(contains_in_code(src, "GL_SRGB8_ALPHA8"));

	INFO("...and must turn the write-side conversion ON for the composing pass. Without "
	     "glEnable(GL_FRAMEBUFFER_SRGB) the sRGB attachment is written raw and nothing encodes");
	CHECK(contains_in_code(src, "glEnable(GL_FRAMEBUFFER_SRGB)"));

	const std::string publish = function_body(src, "gl_publish_compose_to_atlas");
	INFO("gl_publish_compose_to_atlas() must exist — the publish is one named step, not a copy "
	     "pasted into each pass");
	REQUIRE_FALSE(publish.empty());

	INFO("...and must publish with glCopyImageSubData(): GL_SRGB8_ALPHA8 and GL_RGBA8 are the "
	     "same 32-bit RGBA class, so that moves the encoded bytes verbatim");
	CHECK(publish.find("glCopyImageSubData(") != std::string::npos);

	// A draw re-applies the transfer function; a glBlitFramebuffer's sRGB
	// behaviour depends on GL_FRAMEBUFFER_SRGB and drivers have disagreed
	// about it. Either one looks exactly like the copy in review.
	for (const char *forbidden : {"glDrawArrays", "glDrawElements", "glBlitFramebuffer"}) {
		INFO("gl_publish_compose_to_atlas() calls " << forbidden
		                                            << " — the publish must be a raw same-class copy, "
		                                               "never a draw or a filtered/converting blit");
		CHECK(publish.find(forbidden) == std::string::npos);
	}

	INFO("the GL compositor must pick its source read through ONE helper, so a new bind site "
	     "cannot quietly keep the non-decoding read while the target encodes");
	CHECK(contains_in_code(src, "gl_bind_layer_source("));

	// The GL analogue of the D3D12 direct-call count. GL_SKIP_DECODE_EXT IS
	// the non-decoding read, so every code mention of it outside the helper
	// is a site deciding for itself. (Preprocessor lines are excluded: the
	// enum is #defined here because it is not in the tree's GLAD spec.)
	const size_t direct_skip = count_code_lines_with(src, "GL_SKIP_DECODE_EXT");
	INFO("the GL compositor names GL_SKIP_DECODE_EXT in " << direct_skip
	                                                      << " code line(s), expected 1 — the one inside "
	                                                         "gl_bind_layer_source(). Another is a read that "
	                                                         "decided its own colour space.");
	CHECK(direct_skip == 1);
}
