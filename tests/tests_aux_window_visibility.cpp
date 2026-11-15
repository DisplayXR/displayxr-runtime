// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The lens visibility policy's pure core (#2671): rect cover,
 *         occluder rules, hide-dwell debounce.
 */

#include "catch_amalgamated.hpp"

#include "util/u_window_visibility.h"

#include <string>
#include <vector>

namespace {

u_wv_rect
R(int32_t l, int32_t t, int32_t r, int32_t b)
{
	return u_wv_rect{l, t, r, b};
}

bool
covered(u_wv_rect target, std::vector<u_wv_rect> occ)
{
	return u_wv_rect_covered(&target, occ.data(), occ.size());
}

u_wv_window_facts
opaque_window(const char *cls = "Notepad", const char *exe = "notepad.exe")
{
	u_wv_window_facts f{};
	f.class_name = cls;
	f.process_name = exe;
	f.visible = true;
	return f;
}

constexpr uint64_t MS = 1000ull * 1000ull;

} // namespace

TEST_CASE("cover: one window over the whole target hides it")
{
	CHECK(covered(R(100, 100, 500, 400), {R(0, 0, 1000, 1000)}));
	CHECK(covered(R(100, 100, 500, 400), {R(100, 100, 500, 400)}));
}

TEST_CASE("cover: partial overlap keeps the target visible")
{
	CHECK_FALSE(covered(R(100, 100, 500, 400), {R(0, 0, 499, 1000)}));
	CHECK_FALSE(covered(R(100, 100, 500, 400), {R(200, 200, 300, 300)}));
	CHECK_FALSE(covered(R(100, 100, 500, 400), {}));
}

TEST_CASE("cover: a union of windows covers when no pixel is left")
{
	// Left and right halves, touching.
	CHECK(covered(R(0, 0, 100, 100), {R(-10, -10, 50, 110), R(50, -10, 110, 110)}));
	// Four quadrants overlapping.
	CHECK(covered(R(0, 0, 100, 100), {R(0, 0, 60, 60), R(40, 0, 100, 60), R(0, 40, 60, 100), R(40, 40, 100, 100)}));
	// A one-pixel gap between them stays visible.
	CHECK_FALSE(covered(R(0, 0, 100, 100), {R(0, 0, 50, 100), R(51, 0, 100, 100)}));
}

TEST_CASE("cover: a hole in the middle stays visible")
{
	CHECK_FALSE(
	    covered(R(0, 0, 100, 100), {R(0, 0, 100, 40), R(0, 60, 100, 100), R(0, 40, 40, 60), R(60, 40, 100, 60)}));
}

TEST_CASE("cover: empty target and empty occluders")
{
	CHECK_FALSE(covered(R(10, 10, 10, 50), {R(0, 0, 100, 100)}));
	CHECK_FALSE(covered(R(0, 0, 100, 100), {R(0, 0, 0, 0), R(5, 5, 5, 90)}));
}

TEST_CASE("cover: heavy fragmentation answers visible, never a false hide")
{
	// A grid of 1-px dots punches the target into many pieces; the core
	// either proves cover (it cannot here) or gives up on the visible side.
	std::vector<u_wv_rect> dots;
	for (int y = 0; y < 100; y += 2) {
		for (int x = 0; x < 100; x += 2) {
			dots.push_back(R(x, y, x + 1, y + 1));
		}
	}
	CHECK_FALSE(covered(R(0, 0, 100, 100), dots));
}

TEST_CASE("occluders: an ordinary opaque window occludes")
{
	auto f = opaque_window();
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_OCCLUDES);
}

TEST_CASE("occluders: the rules that remove false positives")
{
	auto f = opaque_window();
	f.cloaked = true;
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_CLOAKED);

	f = opaque_window();
	f.no_redirection_bitmap = true;
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_CLOAKED);

	f = opaque_window();
	f.same_process = true;
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_SAME_PROCESS);

	f = opaque_window();
	f.input_transparent = true;
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_TRANSPARENT);

	f = opaque_window();
	f.layered_not_opaque = true;
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_TRANSPARENT);

	f = opaque_window();
	f.visible = false;
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_NOT_VISIBLE);

	f = opaque_window("TaskSwitcherWnd", "explorer.exe");
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_OVERLAY_CLASS);
	// ADR-062 D1: the taskbar occludes unless it auto-hides.
	f = opaque_window("Shell_TrayWnd", "explorer.exe");
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_OCCLUDES);
	f.taskbar_auto_hide = true;
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_OVERLAY_CLASS);
	f = opaque_window("Shell_SecondaryTrayWnd", "explorer.exe");
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_OCCLUDES);
	f.taskbar_auto_hide = true;
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_OVERLAY_CLASS);
	// Auto-hide changes nothing for any other window.
	f = opaque_window();
	f.taskbar_auto_hide = true;
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_OCCLUDES);

	f = opaque_window("SomeOverlayClass", "presentmon.EXE");
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_OVERLAY_PROCESS);
}

TEST_CASE("occluders: the configurable overlay list")
{
	auto f = opaque_window("X", "MyOverlay.exe");
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_OCCLUDES);
	CHECK(u_wv_classify_occluder(&f, "") == U_WV_OCCLUDES);
	CHECK(u_wv_classify_occluder(&f, "a.exe, myoverlay.exe ,b.exe") == U_WV_SKIP_OVERLAY_PROCESS);
	CHECK(u_wv_classify_occluder(&f, "myoverlay.exe") == U_WV_SKIP_OVERLAY_PROCESS);
	// No prefix or suffix matches.
	CHECK(u_wv_classify_occluder(&f, "overlay.exe,myoverlay.ex") == U_WV_OCCLUDES);
}

TEST_CASE("debounce: hidden only after the dwell, visible at once")
{
	u_wv_debounce d{};
	const uint64_t dwell = 500 * MS;
	CHECK_FALSE(u_wv_debounce_update(&d, true, 1000 * MS, dwell));
	CHECK_FALSE(u_wv_debounce_update(&d, true, 1400 * MS, dwell));
	CHECK(u_wv_debounce_update(&d, true, 1500 * MS, dwell));
	CHECK(u_wv_debounce_update(&d, true, 2000 * MS, dwell));
	// One visible check returns at once.
	CHECK_FALSE(u_wv_debounce_update(&d, false, 2100 * MS, dwell));
	// And the dwell restarts.
	CHECK_FALSE(u_wv_debounce_update(&d, true, 2200 * MS, dwell));
	CHECK_FALSE(u_wv_debounce_update(&d, true, 2600 * MS, dwell));
	CHECK(u_wv_debounce_update(&d, true, 2700 * MS, dwell));
}

TEST_CASE("debounce: a flicker shorter than the dwell never hides")
{
	u_wv_debounce d{};
	const uint64_t dwell = 500 * MS;
	for (uint64_t t = 0; t < 5000; t += 100) {
		const bool raw = (t / 100) % 4 != 3; // visible one check in four
		CHECK_FALSE(u_wv_debounce_update(&d, raw, t * MS, dwell));
	}
}

TEST_CASE("tracker: NULL window means always visible")
{
	CHECK(u_window_visibility_create(nullptr, "", "test", nullptr, nullptr) == nullptr);
	CHECK_FALSE(u_window_visibility_hidden(nullptr));
	u_window_visibility *wv = nullptr;
	u_window_visibility_destroy(&wv);
	CHECK(wv == nullptr);
}

TEST_CASE("screens: each screen judges only the window's part on it")
{
	// Two side-by-side 3D screens; the window spans the seam at x = 1000.
	const u_wv_rect left_screen = R(0, 0, 1000, 1000);
	const u_wv_rect right_screen = R(1000, 0, 2000, 1000);
	const u_wv_rect client = R(600, 100, 1400, 500);

	// An occluder over the window's right part only.
	const std::vector<u_wv_rect> occ = {R(990, 50, 1500, 600)};
	CHECK(u_wv_part_state(&client, &right_screen, occ.data(), occ.size()) == U_WV_PART_HIDDEN);
	CHECK(u_wv_part_state(&client, &left_screen, occ.data(), occ.size()) == U_WV_PART_VISIBLE);

	// Nothing covers it: visible on both.
	CHECK(u_wv_part_state(&client, &right_screen, nullptr, 0) == U_WV_PART_VISIBLE);
	CHECK(u_wv_part_state(&client, &left_screen, nullptr, 0) == U_WV_PART_VISIBLE);
}

TEST_CASE("screens: a screen the window is not on is OFF, never hidden")
{
	const u_wv_rect far_screen = R(3000, 0, 4000, 1000);
	const u_wv_rect client = R(100, 100, 500, 500);
	const std::vector<u_wv_rect> occ = {R(-10000, -10000, 10000, 10000)};
	CHECK(u_wv_part_state(&client, &far_screen, occ.data(), occ.size()) == U_WV_PART_OFF);
	// Touching edges share no pixel (half-open rects).
	const u_wv_rect touching = R(500, 0, 900, 1000);
	CHECK(u_wv_part_state(&client, &touching, occ.data(), occ.size()) == U_WV_PART_OFF);
	CHECK(u_wv_part_state(nullptr, &touching, nullptr, 0) == U_WV_PART_OFF);
}

TEST_CASE("screens: a union of occluders hides one screen's part, a sliver keeps it")
{
	const u_wv_rect right_screen = R(1000, 0, 2000, 1000);
	const u_wv_rect client = R(600, 100, 1400, 500);
	// Two windows that together cover x in [1000, 1400) and leave the left part.
	const std::vector<u_wv_rect> two = {R(1000, 0, 1200, 1000), R(1200, 0, 1400, 1000)};
	CHECK(u_wv_part_state(&client, &right_screen, two.data(), two.size()) == U_WV_PART_HIDDEN);
	// ADR-062 D1: a one-pixel column left open is a sliver: still hidden...
	const std::vector<u_wv_rect> gap = {R(1000, 0, 1200, 1000), R(1201, 0, 1400, 1000)};
	CHECK(u_wv_part_state(&client, &right_screen, gap.data(), gap.size()) == U_WV_PART_HIDDEN);
	// ...a 100-px strip (25% of the part) keeps it visible.
	const std::vector<u_wv_rect> strip = {R(1000, 0, 1200, 1000), R(1300, 0, 1400, 1000)};
	CHECK(u_wv_part_state(&client, &right_screen, strip.data(), strip.size()) == U_WV_PART_VISIBLE);
}

TEST_CASE("area: the uncovered remainder is measured exactly")
{
	uint64_t a = 0;
	const u_wv_rect t = R(0, 0, 100, 100);
	CHECK(u_wv_rect_uncovered_area(&t, nullptr, 0, &a));
	CHECK(a == 10000);
	const std::vector<u_wv_rect> half = {R(0, 0, 50, 100)};
	CHECK(u_wv_rect_uncovered_area(&t, half.data(), half.size(), &a));
	CHECK(a == 5000);
	// Overlapping occluders are not double-counted.
	const std::vector<u_wv_rect> two = {R(0, 0, 60, 100), R(40, 0, 100, 90)};
	CHECK(u_wv_rect_uncovered_area(&t, two.data(), two.size(), &a));
	CHECK(a == 400); // x in [60,100) by y in [90,100)
	const u_wv_rect empty = R(5, 5, 5, 5);
	CHECK(u_wv_rect_uncovered_area(&empty, nullptr, 0, &a));
	CHECK(a == 0);
}

TEST_CASE("threshold: 2% of the area or a 4096 px floor, never more than the part")
{
	// Big window: 2% wins.
	CHECK(u_wv_min_visible_area(1920ull * 1080ull) == 1920ull * 1080ull * 20 / 1000);
	// Small window: the floor wins.
	CHECK(u_wv_min_visible_area(200ull * 100ull) == 4096);
	// Tiny window: the part itself, so an uncovered tiny window stays visible.
	CHECK(u_wv_min_visible_area(30ull * 30ull) == 900);
	CHECK(u_wv_min_visible_area(0) == 0);
}

TEST_CASE("threshold: a sliver behind an edge does not hold the window visible")
{
	const u_wv_rect win = R(0, 0, 1920, 1080);
	// Covered but for a 10-px band at the bottom (0.9%): hidden.
	const std::vector<u_wv_rect> sliver = {R(-10, -10, 2000, 1070)};
	CHECK(u_wv_rect_mostly_covered(&win, sliver.data(), sliver.size()));
	// Covered but for a 40-px band (3.7%): visible.
	const std::vector<u_wv_rect> band = {R(-10, -10, 2000, 1040)};
	CHECK_FALSE(u_wv_rect_mostly_covered(&win, band.data(), band.size()));
	// Nothing over it: visible. Empty target: never hidden.
	CHECK_FALSE(u_wv_rect_mostly_covered(&win, nullptr, 0));
	const u_wv_rect empty = R(0, 0, 0, 0);
	CHECK_FALSE(u_wv_rect_mostly_covered(&empty, sliver.data(), sliver.size()));
	// An uncovered tiny window is visible, a fully covered one hidden.
	const u_wv_rect tiny = R(0, 0, 20, 20);
	CHECK_FALSE(u_wv_rect_mostly_covered(&tiny, nullptr, 0));
	const std::vector<u_wv_rect> all = {R(-1, -1, 21, 21)};
	CHECK(u_wv_rect_mostly_covered(&tiny, all.data(), all.size()));
}

TEST_CASE("reason: a window never shown is NOT_SHOWN, never hidden")
{
	u_wv_window_state s{};
	s.exists = true;
	// Startup: created hidden, the app renders, then ShowWindow.
	CHECK(u_wv_window_reason(&s) == U_WV_REASON_NOT_SHOWN);
	CHECK_FALSE(u_wv_reason_is_hidden(U_WV_REASON_NOT_SHOWN));
	s.minimized = true; // still never shown
	CHECK(u_wv_window_reason(&s) == U_WV_REASON_NOT_SHOWN);
	// Shown once, hidden later: hidden at once.
	s.minimized = false;
	s.ever_shown = true;
	CHECK(u_wv_window_reason(&s) == U_WV_REASON_NOT_VISIBLE);
	CHECK(u_wv_reason_is_immediate(U_WV_REASON_NOT_VISIBLE));
	s.visible = true;
	CHECK(u_wv_window_reason(&s) == U_WV_REASON_VISIBLE);
	s.minimized = true;
	CHECK(u_wv_window_reason(&s) == U_WV_REASON_MINIMIZED);
	s.minimized = false;
	s.cloaked = true;
	CHECK(u_wv_window_reason(&s) == U_WV_REASON_CLOAKED);
}

TEST_CASE("reason: lock and display-off hide everything at once, gone hides nothing")
{
	u_wv_window_state s{};
	s.exists = true;
	s.ever_shown = true;
	s.visible = true;
	s.locked = true;
	CHECK(u_wv_window_reason(&s) == U_WV_REASON_LOCKED);
	s.locked = false;
	s.display_off = true;
	CHECK(u_wv_window_reason(&s) == U_WV_REASON_DISPLAY_OFF);
	CHECK(u_wv_reason_is_immediate(U_WV_REASON_LOCKED));
	CHECK(u_wv_reason_is_immediate(U_WV_REASON_DISPLAY_OFF));
	s.exists = false;
	CHECK(u_wv_window_reason(&s) == U_WV_REASON_GONE);
	CHECK_FALSE(u_wv_reason_is_hidden(U_WV_REASON_GONE));
	// Covered is hidden but waits for the dwell; the visible reasons are not hidden.
	CHECK(u_wv_reason_is_hidden(U_WV_REASON_COVERED));
	CHECK_FALSE(u_wv_reason_is_immediate(U_WV_REASON_COVERED));
	CHECK_FALSE(u_wv_reason_is_hidden(U_WV_REASON_PARTLY_COVERED));
	CHECK_FALSE(u_wv_reason_is_hidden(U_WV_REASON_OFF_SCREEN));
	CHECK(std::string(u_wv_reason_str(U_WV_REASON_COVERED)) == "covered");
	CHECK(std::string(u_wv_reason_str(U_WV_REASON_NOT_SHOWN)) == "not-shown");
}

TEST_CASE("dwell: covered waits 1.5 s, minimize and lock are immediate")
{
	const uint64_t dwell = U_WINDOW_VISIBILITY_HIDE_DWELL_NS;
	CHECK(dwell == 1500 * MS);
	u_wv_debounce d{};
	CHECK_FALSE(u_wv_debounce_step(&d, true, false, false, 0, dwell));
	CHECK(u_wv_debounce_deadline(&d, dwell) == dwell);
	CHECK_FALSE(u_wv_debounce_step(&d, true, false, false, 1499 * MS, dwell));
	CHECK(u_wv_debounce_step(&d, true, false, false, 1500 * MS, dwell));
	CHECK(u_wv_debounce_deadline(&d, dwell) == 0);
	CHECK_FALSE(u_wv_debounce_step(&d, false, false, false, 1600 * MS, dwell));

	u_wv_debounce m{};
	CHECK(u_wv_debounce_step(&m, true, true, false, 5 * MS, dwell));
	// Visible again is immediate too.
	CHECK_FALSE(u_wv_debounce_step(&m, false, false, false, 6 * MS, dwell));
	// A covered run that turns into a minimize hides at once.
	CHECK_FALSE(u_wv_debounce_step(&m, true, false, false, 10 * MS, dwell));
	CHECK(u_wv_debounce_step(&m, true, true, false, 20 * MS, dwell));
}

TEST_CASE("dwell: no covered transition while a window is being moved or sized")
{
	const uint64_t dwell = U_WINDOW_VISIBILITY_HIDE_DWELL_NS;
	u_wv_debounce d{};
	// A window dragged across the 3D window for 3 s never hides it...
	for (uint64_t t = 0; t <= 3000; t += 100) {
		CHECK_FALSE(u_wv_debounce_step(&d, true, false, true, t * MS, dwell));
	}
	// ...and the dwell starts when the drag ends.
	CHECK_FALSE(u_wv_debounce_step(&d, true, false, false, 3100 * MS, dwell));
	CHECK_FALSE(u_wv_debounce_step(&d, true, false, false, 4499 * MS, dwell));
	CHECK(u_wv_debounce_step(&d, true, false, false, 4500 * MS, dwell));
	// Once hidden, a drag does not un-hide it.
	CHECK(u_wv_debounce_step(&d, true, false, true, 4600 * MS, dwell));
	// A minimize during a drag still hides at once.
	u_wv_debounce m{};
	CHECK(u_wv_debounce_step(&m, true, true, true, 0, dwell));
}

TEST_CASE("tracker: the tier-2 and state readers are NULL-safe")
{
	CHECK_FALSE(u_window_visibility_hidden_everywhere(nullptr));
	u_window_visibility_state st{};
	st.tracked = true;
	u_window_visibility_get_state(nullptr, &st);
	CHECK_FALSE(st.tracked);
	CHECK_FALSE(st.hidden);
}

TEST_CASE("tracker: the per-screen API is NULL-safe")
{
	CHECK_FALSE(u_window_visibility_screen_hidden(nullptr, "DISPLAY2"));
	const char *names[] = {"DISPLAY2"};
	u_window_visibility_set_screens(nullptr, names, 1); // no crash
}

/*
 *
 * ADR-062 D5 (tier 4): the visible region and the published wish.
 *
 */

namespace {

u_wv_region
region_of(u_wv_rect target, std::vector<u_wv_rect> occ)
{
	u_wv_region r{};
	u_wv_region_compute(&target, occ.data(), occ.size(), &r);
	return r;
}

u_wv_region
region_list(std::vector<u_wv_rect> rects)
{
	u_wv_region r{};
	for (const auto &x : rects) {
		r.rects[r.count++] = x;
	}
	return r;
}

u_window_visibility_region
vis(uint32_t w, uint32_t h, std::vector<u_wv_rect> rects)
{
	u_window_visibility_region v{};
	v.gen = 1;
	v.known = true;
	v.client_w = w;
	v.client_h = h;
	v.region = region_list(rects);
	return v;
}

//! Rasterize a plan on the CPU the way a compositor does: source (or 0), ON = 1, OFF = 0.
std::vector<uint8_t>
raster(const u_wv_wish_plan &p, const std::vector<uint8_t> *source)
{
	std::vector<uint8_t> m(size_t(p.mask_w) * p.mask_h, 0);
	if (p.kind == U_WV_WISH_CUT && source != nullptr) {
		m = *source;
	}
	auto fill = [&](const u_wv_rect &r, uint8_t v) {
		for (int32_t y = r.top; y < r.bottom; y++) {
			for (int32_t x = r.left; x < r.right; x++) {
				m[size_t(y) * p.mask_w + size_t(x)] = v;
			}
		}
	};
	for (uint32_t i = 0; i < p.on_count; i++) {
		fill(p.on[i], 1);
	}
	for (uint32_t i = 0; i < p.off_count; i++) {
		fill(p.off[i], 0);
	}
	return m;
}

//! The sim_display-style verdict: a cell is 3D when any mask pixel in it is nonzero.
std::string
cells(const std::vector<uint8_t> &m, uint32_t w, uint32_t h, uint32_t gw, uint32_t gh)
{
	std::string s;
	for (uint32_t cy = 0; cy < gh; cy++) {
		for (uint32_t cx = 0; cx < gw; cx++) {
			char bit = '0';
			for (uint32_t y = cy * h / gh; y < (cy + 1) * h / gh; y++) {
				for (uint32_t x = cx * w / gw; x < (cx + 1) * w / gw; x++) {
					if (m[size_t(y) * w + x] != 0) {
						bit = '1';
					}
				}
			}
			s += bit;
		}
		if (cy + 1 < gh) {
			s += '/';
		}
	}
	return s;
}

} // namespace

TEST_CASE("region: the uncovered remainder, and its area")
{
	// Nothing over it: the target itself.
	u_wv_region a = region_of(R(0, 0, 100, 100), {});
	REQUIRE(a.count == 1);
	CHECK(u_wv_region_area(&a) == 10000);
	CHECK_FALSE(a.whole);

	// The right half covered: the left half remains.
	u_wv_region b = region_of(R(0, 0, 100, 100), {R(50, -10, 200, 200)});
	CHECK(u_wv_region_area(&b) == 5000);
	u_wv_region left = region_list({R(0, 0, 50, 100)});
	CHECK(u_wv_region_contains(&left, &b));
	CHECK(u_wv_region_contains(&b, &left));

	// Fully covered: empty.
	u_wv_region c = region_of(R(0, 0, 100, 100), {R(0, 0, 100, 100)});
	CHECK(c.count == 0);
}

TEST_CASE("region: fragmenting past the cap falls back to the whole target")
{
	// A grid of small holes punched by many occluders fragments the remainder.
	std::vector<u_wv_rect> occ;
	for (int y = 0; y < 20; y++) {
		for (int x = 0; x < 20; x++) {
			occ.push_back(R(x * 10, y * 10, x * 10 + 5, y * 10 + 5));
		}
	}
	u_wv_rect t = R(0, 0, 200, 200);
	u_wv_region r{};
	CHECK_FALSE(u_wv_region_compute(&t, occ.data(), occ.size(), &r));
	CHECK(r.whole);
	REQUIRE(r.count == 1);
	CHECK(r.rects[0].right == 200);
}

TEST_CASE("region: merge, clip, translate")
{
	u_wv_region a = region_list({R(0, 0, 10, 10)});
	u_wv_region b = region_list({R(5, 5, 10, 10), R(20, 0, 30, 10)});
	u_wv_rect whole = R(0, 0, 100, 100);
	u_wv_region_merge(&a, &b, &whole);
	CHECK(a.count == 2); // (5,5)-(10,10) was already in
	CHECK(u_wv_region_contains(&a, &b));

	u_wv_region c{};
	u_wv_rect clip = R(0, 0, 25, 5);
	u_wv_region_clip(&a, &clip, &c);
	CHECK(u_wv_region_area(&c) == 10 * 5 + 5 * 5);

	u_wv_region_translate(&c, 100, 0);
	CHECK(c.rects[0].left == 100);
}

TEST_CASE("region dwell: growth at once, a shrink after the dwell, held while dragging")
{
	const uint64_t dwell = U_WINDOW_VISIBILITY_HIDE_DWELL_NS;
	const u_wv_rect whole = R(0, 0, 100, 100);
	u_wv_region_debounce d{};
	u_wv_region full = region_list({whole});
	u_wv_region half = region_list({R(0, 0, 50, 100)});

	CHECK(u_wv_region_debounce_step(&d, &half, false, false, 0, dwell, &whole)); // first: at once
	CHECK(u_wv_region_equal(&d.pub, &half));
	CHECK(u_wv_region_debounce_step(&d, &full, false, false, 10 * MS, dwell, &whole)); // grows: at once
	CHECK(u_wv_region_equal(&d.pub, &full));

	// Covered again: still the full region until the dwell runs out.
	CHECK_FALSE(u_wv_region_debounce_step(&d, &half, false, false, 20 * MS, dwell, &whole));
	CHECK(u_wv_region_equal(&d.pub, &full));
	CHECK(u_wv_region_debounce_deadline(&d, dwell) == 20 * MS + dwell);
	// A drag holds it, and restarts the dwell.
	const uint64_t t0 = 20 * MS + dwell;
	CHECK_FALSE(u_wv_region_debounce_step(&d, &half, false, true, t0, dwell, &whole));
	CHECK(u_wv_region_equal(&d.pub, &full));
	CHECK_FALSE(u_wv_region_debounce_step(&d, &half, false, false, t0 + dwell - 1, dwell, &whole));
	CHECK(u_wv_region_debounce_step(&d, &half, false, false, t0 + dwell, dwell, &whole));
	CHECK(u_wv_region_equal(&d.pub, &half));

	// Immediate (a minimize): empty at once.
	u_wv_region none{};
	CHECK(u_wv_region_debounce_step(&d, &none, true, false, t0 + dwell + MS, dwell, &whole));
	CHECK(d.pub.count == 0);
}

TEST_CASE("region dwell: what appears during a shrink's dwell is added at once")
{
	const uint64_t dwell = U_WINDOW_VISIBILITY_HIDE_DWELL_NS;
	const u_wv_rect whole = R(0, 0, 100, 100);
	u_wv_region_debounce d{};
	u_wv_region left = region_list({R(0, 0, 50, 100)});
	u_wv_region right = region_list({R(50, 0, 100, 100)});
	u_wv_region_debounce_step(&d, &left, false, false, 0, dwell, &whole);
	// The occluder moved from the right half to the left half.
	CHECK(u_wv_region_debounce_step(&d, &right, false, false, MS, dwell, &whole));
	CHECK(u_wv_region_contains(&d.pub, &right)); // the right half is 3D at once
	CHECK(u_wv_region_contains(&d.pub, &left));  // the left half waits out the dwell
	CHECK(u_wv_region_debounce_step(&d, &right, false, false, MS + dwell, dwell, &whole));
	CHECK(u_wv_region_equal(&d.pub, &right));
}

TEST_CASE("wish: the region mask applies only to a grid larger than 1x1")
{
	// The tier-1 boolean path (today's panel-wide switch) is untouched: every
	// consumer gates its whole tier-4 branch on this predicate.
	CHECK_FALSE(u_wv_region_mask_applies(1, 1));
	CHECK_FALSE(u_wv_region_mask_applies(0, 0));
	CHECK_FALSE(u_wv_region_mask_applies(1, 0));
	CHECK(u_wv_region_mask_applies(2, 1));
	CHECK(u_wv_region_mask_applies(1, 2));
	CHECK(u_wv_region_mask_applies(4, 2));
}

TEST_CASE("wish: a zones wish intersected with the visible region")
{
	// 400x200 client, mask at the same size; zone = left half; the window's
	// top-left quarter is covered.
	std::vector<uint8_t> src(400 * 200, 0);
	for (int y = 0; y < 200; y++) {
		for (int x = 0; x < 200; x++) {
			src[size_t(y) * 400 + x] = 255;
		}
	}
	u_window_visibility_region v = vis(400, 200, {R(0, 100, 400, 200), R(100, 0, 400, 100)});
	u_wv_wish_input in{};
	in.kind = U_WV_WISH_CUT;
	in.mask_w = 400;
	in.mask_h = 200;
	in.source_seq = 7;
	in.visible = &v;
	u_wv_wish_plan p{};
	u_wv_wish_plan_build(&in, &p);
	CHECK(p.on_count == 0);
	CHECK_FALSE(p.cut_skipped);
	// Grid 4x2: the zone covers columns 0-1, cell (0,0) is covered.
	CHECK(cells(raster(p, &src), 400, 200, 4, 2) == "0100/1100");
	// Nothing known: the source verbatim.
	in.visible = nullptr;
	u_wv_wish_plan_build(&in, &p);
	CHECK(p.off_count == 0);
	CHECK(cells(raster(p, &src), 400, 200, 4, 2) == "1100/1100");
}

TEST_CASE("wish: a non-zones app's implicit wish is its visible region minus Local2D rects")
{
	u_window_visibility_region v = vis(400, 200, {R(0, 0, 200, 200)}); // right half covered
	u_wv_rect flat = R(0, 100, 100, 200);                              // a Local2D panel, bottom-left cell
	u_wv_wish_input in{};
	in.kind = U_WV_WISH_IMPLICIT;
	in.mask_w = 400;
	in.mask_h = 200;
	in.visible = &v;
	in.flat = &flat;
	in.flat_count = 1;
	in.region_w = 400;
	in.region_h = 200;
	u_wv_wish_plan p{};
	u_wv_wish_plan_build(&in, &p);
	CHECK(cells(raster(p, nullptr), 400, 200, 4, 2) == "1100/0100");

	// Uncovered window, no Local2D: every cell 3D.
	in.visible = nullptr;
	in.flat_count = 0;
	u_wv_wish_plan_build(&in, &p);
	CHECK(cells(raster(p, nullptr), 400, 200, 4, 2) == "1111/1111");

	// 2D mode or hidden: the empty wish.
	in.kind = U_WV_WISH_EMPTY;
	u_wv_wish_plan_build(&in, &p);
	CHECK(u_wv_wish_plan_is_empty(&p));
	CHECK(cells(raster(p, nullptr), 400, 200, 4, 2) == "0000/0000");
}

TEST_CASE("wish: scaling to a smaller mask rounds toward 3D")
{
	// Client 1000 wide, mask 100 wide: a visible rect 5..15 covers mask pixels 0..2.
	u_window_visibility_region v = vis(1000, 100, {R(5, 0, 15, 100)});
	u_wv_rect flat = R(5, 0, 15, 100); // a flat rect shrinks inward: 1..1 = nothing
	u_wv_wish_input in{};
	in.kind = U_WV_WISH_IMPLICIT;
	in.mask_w = 100;
	in.mask_h = 10;
	in.visible = &v;
	in.flat = &flat;
	in.flat_count = 1;
	in.region_w = 1000;
	in.region_h = 100;
	u_wv_wish_plan p{};
	u_wv_wish_plan_build(&in, &p);
	REQUIRE(p.on_count == 1);
	CHECK(p.on[0].left == 0);
	CHECK(p.on[0].right == 2);
	CHECK(p.off_count == 0);
}

TEST_CASE("wish: the content signature moves with the content only (republish, bump on change)")
{
	u_window_visibility_region v = vis(400, 200, {R(0, 0, 200, 200)});
	u_wv_wish_input in{};
	in.kind = U_WV_WISH_IMPLICIT;
	in.mask_w = 400;
	in.mask_h = 200;
	in.visible = &v;
	u_wv_wish_plan a{}, b{};
	u_wv_wish_plan_build(&in, &a);
	// Same content, a new region generation (the tracker re-published): same signature.
	v.gen = 99;
	u_wv_wish_plan_build(&in, &b);
	CHECK(a.signature == b.signature);

	u_wv_wish_seq seq{};
	bool changed = false;
	const uint64_t s1 = u_wv_wish_seq_step(&seq, a.signature, &changed);
	CHECK(changed);
	CHECK(u_wv_wish_seq_step(&seq, b.signature, &changed) == s1);
	CHECK_FALSE(changed);

	// A different visible region: a new signature and a bump.
	v.region = region_list({R(0, 0, 100, 200)});
	u_wv_wish_plan_build(&in, &b);
	CHECK(a.signature != b.signature);
	CHECK(u_wv_wish_seq_step(&seq, b.signature, &changed) == s1 + 1);
	CHECK(changed);

	// A CUT's source generation is part of its content.
	in.kind = U_WV_WISH_CUT;
	in.source_seq = 1;
	u_wv_wish_plan_build(&in, &a);
	in.source_seq = 2;
	u_wv_wish_plan_build(&in, &b);
	CHECK(a.signature != b.signature);
	// And the kind is too: an empty wish never signs like an implicit one with nothing on.
	in.kind = U_WV_WISH_EMPTY;
	u_wv_wish_plan_build(&in, &a);
	in.kind = U_WV_WISH_IMPLICIT;
	v.region.count = 0;
	u_wv_wish_plan_build(&in, &b);
	CHECK(a.signature != b.signature);
}

TEST_CASE("tracker: the region API is NULL-safe and reads 'not known'")
{
	u_window_visibility_region r{};
	r.known = true;
	CHECK_FALSE(u_window_visibility_get_region(nullptr, nullptr, &r));
	CHECK_FALSE(r.known);
	CHECK(u_window_visibility_region_gen(nullptr) == 0);
}
