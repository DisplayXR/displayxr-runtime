// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The lens visibility policy's pure core (#2671): rect cover,
 *         occluder rules, hide-dwell debounce.
 */

#include "catch_amalgamated.hpp"

#include "util/u_window_visibility.h"

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
	f = opaque_window("Shell_TrayWnd", "explorer.exe");
	CHECK(u_wv_classify_occluder(&f, nullptr) == U_WV_SKIP_OVERLAY_CLASS);

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
	// One pixel column left open on the right screen keeps it visible.
	const std::vector<u_wv_rect> gap = {R(1000, 0, 1200, 1000), R(1201, 0, 1400, 1000)};
	CHECK(u_wv_part_state(&client, &right_screen, gap.data(), gap.size()) == U_WV_PART_VISIBLE);
}

TEST_CASE("tracker: the per-screen API is NULL-safe")
{
	CHECK_FALSE(u_window_visibility_screen_hidden(nullptr, "DISPLAY2"));
	const char *names[] = {"DISPLAY2"};
	u_window_visibility_set_screens(nullptr, names, 1); // no crash
}
