// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime's own X11 window declared full side by side follows the
 *         window manager's doubling (comp_vk_native_xcb_full_sbs.h).
 *
 * A stereo window manager (Stereo KWin, `_KDE_NET_WM_STEREO_CONTENT_SUPPORTED`
 * 3 or later) makes a declared window's X window twice its place on screen and
 * then sends a synthetic ConfigureNotify with one view's width. The runtime's
 * swapchain must follow the X window's own size (Vulkan creates an X11
 * swapchain only at the window's current size), and everything else the size
 * of one view, but only once the window manager has doubled the window: a
 * doubling taken too early or a synthetic size taken for the X window's makes
 * every view half as wide as it is shown.
 */

#include "catch_amalgamated.hpp"

#include "vk_native/comp_vk_native_xcb_full_sbs.h"

namespace {

struct comp_vk_native_xcb_full_sbs
window(uint32_t w, uint32_t h)
{
	struct comp_vk_native_xcb_full_sbs s = {};
	s.width = w;
	s.height = h;
	return s;
}

void
view(const struct comp_vk_native_xcb_full_sbs &s, uint32_t *w, uint32_t *h)
{
	comp_vk_native_xcb_full_sbs_view_size(&s, w, h);
}

} // namespace

TEST_CASE("xcb full sbs: an undeclared window takes every ConfigureNotify as before", "[xcb_full_sbs]")
{
	auto s = window(800, 450);
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, false, 1024, 576));
	uint32_t w = 0, h = 0;
	view(s, &w, &h);
	CHECK(w == 1024);
	CHECK(h == 576);
	// A window manager's synthetic event after a move carries the window's size.
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, true, 1000, 500));
	view(s, &w, &h);
	CHECK(w == 1000);
	CHECK(h == 500);
	CHECK_FALSE(s.doubled);
}

TEST_CASE("xcb full sbs: declared, the window manager doubles it at the same place", "[xcb_full_sbs]")
{
	auto s = window(1920, 1080);
	comp_vk_native_xcb_full_sbs_set_declared(&s, true);
	CHECK(s.view_width == 1920);
	uint32_t w = 0, h = 0;
	view(s, &w, &h);
	CHECK(w == 1920); // not doubled yet: the window's own size

	// The X server's ConfigureNotify, then KWin's synthetic one in one-view units.
	CHECK(comp_vk_native_xcb_full_sbs_configure(&s, false, 3840, 1080));
	CHECK(s.doubled);
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, true, 1920, 1080));
	CHECK(s.width == 3840); // the swapchain's size: the X window's
	view(s, &w, &h);
	CHECK(w == 1920);
	CHECK(h == 1080);
}

TEST_CASE("xcb full sbs: declared, the window manager doubles it at a new place", "[xcb_full_sbs]")
{
	auto s = window(800, 450);
	comp_vk_native_xcb_full_sbs_set_declared(&s, true);
	// Placed 760x430 and doubled in the same configure: the server's width alone
	// does not say it, the window manager's one-view size does.
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, false, 1520, 430));
	CHECK_FALSE(s.doubled);
	CHECK(comp_vk_native_xcb_full_sbs_configure(&s, true, 760, 430));
	CHECK(s.doubled);
	uint32_t w = 0, h = 0;
	view(s, &w, &h);
	CHECK(w == 760);
	CHECK(h == 430);
	CHECK(s.width == 1520);
}

TEST_CASE("xcb full sbs: late doubling, the window keeps its own size until it comes", "[xcb_full_sbs]")
{
	auto s = window(800, 450);
	comp_vk_native_xcb_full_sbs_set_declared(&s, true);
	uint32_t w = 0, h = 0;
	// A move before the window manager has read the declaration: its synthetic
	// event has the window's own size, which is not a doubling.
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, true, 800, 450));
	CHECK_FALSE(s.doubled);
	view(s, &w, &h);
	CHECK(w == 800);
	// Seconds later (a busy window manager), the doubling.
	CHECK(comp_vk_native_xcb_full_sbs_configure(&s, false, 1600, 450));
	view(s, &w, &h);
	CHECK(w == 800);
	CHECK(s.width == 1600);
	// Known once only.
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, true, 800, 450));
}

TEST_CASE("xcb full sbs: a resize of a doubled window keeps the views at one view each", "[xcb_full_sbs]")
{
	auto s = window(800, 450);
	comp_vk_native_xcb_full_sbs_set_declared(&s, true);
	CHECK(comp_vk_native_xcb_full_sbs_configure(&s, false, 1600, 450));
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, false, 2048, 600));
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, true, 1024, 600));
	uint32_t w = 0, h = 0;
	view(s, &w, &h);
	CHECK(w == 1024);
	CHECK(h == 600);
	CHECK(s.width == 2048);
}

TEST_CASE("xcb full sbs: withdrawn, the window is the X window's size again", "[xcb_full_sbs]")
{
	auto s = window(800, 450);
	comp_vk_native_xcb_full_sbs_set_declared(&s, true);
	CHECK(comp_vk_native_xcb_full_sbs_configure(&s, false, 1600, 450));
	comp_vk_native_xcb_full_sbs_set_declared(&s, false);
	CHECK_FALSE(s.doubled);
	uint32_t w = 0, h = 0;
	view(s, &w, &h);
	CHECK(w == 1600); // until the window manager gives the window its place back
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, false, 800, 450));
	view(s, &w, &h);
	CHECK(w == 800);
	// Declared again: the doubling is waited for again.
	comp_vk_native_xcb_full_sbs_set_declared(&s, true);
	view(s, &w, &h);
	CHECK(w == 800);
	CHECK(comp_vk_native_xcb_full_sbs_configure(&s, false, 1600, 450));
	view(s, &w, &h);
	CHECK(w == 800);
}

TEST_CASE("xcb full sbs: an empty size changes nothing", "[xcb_full_sbs]")
{
	auto s = window(800, 450);
	comp_vk_native_xcb_full_sbs_set_declared(&s, true);
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, false, 0, 0));
	CHECK_FALSE(comp_vk_native_xcb_full_sbs_configure(&s, true, 0, 450));
	CHECK(s.width == 800);
	CHECK(s.height == 450);
	CHECK_FALSE(s.doubled);
}
