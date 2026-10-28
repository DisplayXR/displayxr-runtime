// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-047 Amendment 8, home gone (#2305): the window-handle hand-back
 *         before a screen-list rebuild, end to end through the Vulkan segment
 *         manager (comp_vk_native_segments) with fake display processors.
 *
 * The rig of 2026-10-27: a DS1 panel (the session DP's home) left of a
 * laptop. The window sits on the laptop, so the laptop's DP holds the window
 * handle and the session's primary slot holds a windowless DP for the panel.
 * The panel is unplugged and, once the bounded wait is over, the session
 * hands the compositor a list without it. The old rule handed the window back
 * to the home's DP — a fresh DP for a panel that is not there. Now the window
 * stays on the laptop's ready DP, which takes the primary slot, and the
 * compositor remembers that (away_screen_id) so the next manager with the
 * panel back moves it out again. The D3D11 and GL managers carry the same
 * code; only this one builds off Windows, so it is the one driven here.
 */

// Catch2 first: the Vulkan headers' window-system includes define macros
// (X11's None / Always) that clash with Catch2's enumerators.
#include "catch_amalgamated.hpp"

#include "vk_native/comp_vk_native_segments.h"

#include "util/comp_segments.h"
#include "vk/vk_helpers.h"
#include "xrt/xrt_display_processor_vk.h"
#include "xrt/xrt_plugin.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t PANEL = 0xb84d0cb8b9e101d5ull;
constexpr uint64_t LAPTOP = 0xfcf5e577cbe800aeull;
constexpr int32_t kFormat = 44; // VK_FORMAT_B8G8R8A8_UNORM
void *const kHwnd = reinterpret_cast<void *>(uintptr_t{0x1234});

struct World;

//! A fake display processor: knows its screen, whether it holds the window, and whether it is ready.
struct FakeDp
{
	xrt_display_processor_vk vk; // first: interchangeable with xrt_display_processor*
	World *w;
	uint64_t screen;
	bool window;
	int serial;
};

struct World
{
	std::vector<FakeDp *> alive;
	int made = 0;
	int made_for_panel = 0;
	bool panel_ready = true;  //!< a DP for the panel reports geometry (false once it is unplugged)
	bool laptop_ready = true; //!< a DP for the laptop reports geometry
	bool fail_laptop_window = false;
	xrt_display_processor *slot = nullptr; //!< the compositor's primary slot (the session DP)
	uint64_t away = 0;                     //!< the compositor's away_screen_id

	FakeDp *
	holder() const
	{
		FakeDp *h = nullptr;
		int n = 0;
		for (FakeDp *d : alive) {
			if (d->window) {
				h = d;
				n++;
			}
		}
		return n == 1 ? h : nullptr;
	}

	int
	window_holders() const
	{
		int n = 0;
		for (FakeDp *d : alive) {
			n += d->window ? 1 : 0;
		}
		return n;
	}
};

void
fake_noop()
{}

//! Any non-NULL pointer: the registry's "this plug-in has a Vulkan factory" flag.
int g_vk_factory_flag;

FakeDp *
fake(xrt_display_processor *xdp)
{
	return reinterpret_cast<FakeDp *>(xdp);
}

void
fake_destroy(xrt_display_processor *xdp)
{
	FakeDp *d = fake(xdp);
	auto &v = d->w->alive;
	for (auto it = v.begin(); it != v.end(); ++it) {
		if (*it == d) {
			v.erase(it);
			break;
		}
	}
	delete d;
}

bool
fake_ready(const FakeDp *d)
{
	return d->screen == PANEL ? d->w->panel_ready : d->w->laptop_ready;
}

bool
fake_pixel_info(xrt_display_processor *xdp, uint32_t *w, uint32_t *h, int32_t *l, int32_t *t)
{
	if (!fake_ready(fake(xdp))) {
		return false;
	}
	*w = 2560;
	*h = 1600;
	if (l != nullptr) {
		*l = 0;
	}
	if (t != nullptr) {
		*t = 0;
	}
	return true;
}

bool
fake_dimensions(xrt_display_processor *xdp, float *w, float *h)
{
	if (!fake_ready(fake(xdp))) {
		return false;
	}
	*w = 0.34f;
	*h = 0.21f;
	return true;
}

FakeDp *
make_dp(World *w, uint64_t screen, bool window)
{
	auto *d = new FakeDp{};
	d->vk.base.struct_size = sizeof(xrt_display_processor_vk);
	d->vk.base.process_atlas = reinterpret_cast<decltype(d->vk.base.process_atlas)>(&fake_noop);
	d->vk.base.destroy = fake_destroy;
	d->vk.base.get_display_pixel_info = fake_pixel_info;
	d->vk.base.get_display_dimensions = fake_dimensions;
	d->w = w;
	d->screen = screen;
	d->window = window;
	d->serial = ++w->made;
	w->made_for_panel += screen == PANEL ? 1 : 0;
	w->alive.push_back(d);
	return d;
}

//! The plug-in instance doubles as the World pointer (one world per test).
xrt_result_t
fake_create_dp_vk_for_screen(xrt_plugin_instance *inst,
                             void * /*vk_bundle*/,
                             void * /*cmd_pool*/,
                             void *window,
                             int32_t /*format*/,
                             const xrt_screen_binding *binding,
                             xrt_display_processor **out)
{
	World *w = reinterpret_cast<World *>(inst);
	if (window != nullptr && binding->monitor_id == LAPTOP && w->fail_laptop_window) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	*out = &make_dp(w, binding->monitor_id, window != nullptr)->vk.base;
	return XRT_SUCCESS;
}

xrt_display_processor *
fake_swap_primary(void *userdata, xrt_display_processor *dp)
{
	World *w = static_cast<World *>(userdata);
	xrt_display_processor *old = w->slot;
	w->slot = dp;
	return old;
}

struct Rig
{
	World w;
	xrt_plugin_iface iface{};
	vk_bundle vk{};
	vk_bundle_queue q{};
	xrt_system_compositor_info info{};

	Rig()
	{
		iface.struct_size = sizeof(iface);
		iface.create_dp_vk_for_screen = fake_create_dp_vk_for_screen;
		vk.main_queue = &q;
		add_entry(PANEL, "vendor");
		add_entry(LAPTOP, "sim-display");
		// The session DP: made at session create for the panel, holding the window.
		w.slot = &make_dp(&w, PANEL, true)->vk.base;
	}

	//! The compositor's teardown: whatever DP is left (the slot's) goes.
	~Rig()
	{
		while (!w.alive.empty()) {
			fake_destroy(&w.alive.back()->vk.base);
		}
	}

	void
	add_entry(uint64_t id, const char *plugin)
	{
		xrt_dp_registry_entry &e = info.dp_registry.entries[info.dp_registry.entry_count++];
		e.monitor_id = id;
		std::snprintf(e.plugin_id, sizeof(e.plugin_id), "%s", plugin);
		e.owning_iface = &iface;
		e.owning_instance = &w;
		e.dp_factory_vk = &g_vk_factory_flag;
	}

	comp_vk_native_segments *
	manager(const xrt_screen_list &list)
	{
		comp_vk_native_segments *segs = comp_vk_native_segments_create(&vk, VK_NULL_HANDLE, VK_NULL_HANDLE);
		comp_vk_native_segments_hwnd_hooks hooks{};
		hooks.hwnd = kHwnd;
		hooks.userdata = &w;
		hooks.swap_primary = fake_swap_primary;
		hooks.away_screen_id = &w.away;
		comp_vk_native_segments_set_hwnd_hooks(segs, &hooks);
		comp_vk_native_segments_set_screens(segs, &list, &info, 0);
		return segs;
	}
};

void
add(xrt_screen_list &l, uint64_t id, const char *plugin, int32_t left, uint32_t w, uint32_t h, bool is_default)
{
	xrt_screen &s = l.screens[l.count++];
	std::memset(&s, 0, sizeof(s));
	s.id = id;
	s.desktop_left = left;
	s.desktop_width = w;
	s.desktop_height = h;
	s.native_width = w;
	s.native_height = h;
	std::snprintf(s.plugin_id, sizeof(s.plugin_id), "%s", plugin);
	s.flags = is_default ? XRT_SCREEN_FLAG_SYSTEM_DEFAULT : 0u;
}

xrt_screen_list
both()
{
	xrt_screen_list l{};
	add(l, PANEL, "vendor", -3840, 3840, 2160, true);
	add(l, LAPTOP, "sim-display", 0, 2560, 1600, false);
	return l;
}

//! The panel unplugged, as the session hands it on: the laptop alone, no default.
xrt_screen_list
laptop_only()
{
	xrt_screen_list l{};
	add(l, LAPTOP, "sim-display", 0, 2560, 1600, false);
	return l;
}

//! Weave updates for a bit over the hand-off dwell (0.5 s), the window at @p win.
void
run(comp_vk_native_segments *segs, comp_seg_rect win, int ms = 900)
{
	const comp_seg_rect canvas{0, 0, win.w, win.h};
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	do {
		(void)comp_vk_native_segments_update(segs, &win, &canvas, kFormat, true, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	} while (std::chrono::steady_clock::now() < end);
}

const comp_seg_rect ON_LAPTOP{200, 200, 1280, 720};
const comp_seg_rect ON_PANEL{-3000, 200, 1280, 720};
const comp_seg_rect MOSTLY_PANEL{-1000, 200, 1280, 720}; // 1000 px on the panel, 280 on the laptop

} // namespace

TEST_CASE("home gone: the window stays on the laptop's ready DP, never a new DP for the panel")
{
	Rig r;
	const xrt_screen_list b = both();
	comp_vk_native_segments *segs = r.manager(b);
	REQUIRE(comp_vk_native_segments_enabled(segs));

	// The window on the laptop: after the dwell the laptop's DP holds it and
	// the slot holds a windowless panel DP.
	run(segs, ON_LAPTOP);
	REQUIRE(r.w.holder() != nullptr);
	CHECK(r.w.holder()->screen == LAPTOP);
	REQUIRE(r.w.slot != nullptr);
	CHECK(fake(r.w.slot)->screen == PANEL);
	CHECK_FALSE(fake(r.w.slot)->window);
	FakeDp *laptop_dp = r.w.holder();

	// Unplugged: the panel's DP cannot report geometry any more.
	r.w.panel_ready = false;
	const int panel_dps = r.w.made_for_panel;
	const xrt_screen_list gone = laptop_only();
	comp_vk_native_segments_destroy_for_rebuild(&segs, &gone);

	// The laptop's DP — the same one — holds the window and is the session
	// DP; nothing was made for the panel, and its windowless DP is gone.
	REQUIRE(r.w.slot != nullptr);
	CHECK(fake(r.w.slot) == laptop_dp);
	CHECK(r.w.holder() == laptop_dp);
	CHECK(r.w.made_for_panel == panel_dps);
	for (FakeDp *d : r.w.alive) {
		CHECK(d->screen != PANEL);
	}
	CHECK(r.w.away == LAPTOP);

	// The rebuilt manager has one screen: no segmentation, the slot is untouched.
	segs = r.manager(gone);
	CHECK_FALSE(comp_vk_native_segments_enabled(segs));
	comp_vk_native_segments_destroy_for_rebuild(&segs, nullptr);
	CHECK(fake(r.w.slot) == laptop_dp);
	CHECK(r.w.away == LAPTOP);

	// The panel is back. The first update of the new manager moves the
	// laptop's DP out of the slot — still holding the window — and gives the
	// panel a windowless DP, whatever the window covers.
	r.w.panel_ready = true;
	segs = r.manager(b);
	REQUIRE(comp_vk_native_segments_enabled(segs));
	const comp_seg_rect canvas{0, 0, ON_PANEL.w, ON_PANEL.h};
	CHECK(comp_vk_native_segments_would_split(segs, &ON_PANEL, &canvas)); // the update must run
	(void)comp_vk_native_segments_update(segs, &ON_PANEL, &canvas, kFormat, true, 0);
	CHECK(r.w.away == 0);
	CHECK(r.w.holder() == laptop_dp);
	REQUIRE(r.w.slot != nullptr);
	CHECK(fake(r.w.slot)->screen == PANEL);
	CHECK_FALSE(fake(r.w.slot)->window);

	// The window is on the panel: after the dwell the panel's DP takes it.
	run(segs, ON_PANEL);
	REQUIRE(r.w.holder() != nullptr);
	CHECK(r.w.holder()->screen == PANEL);
	CHECK(fake(r.w.slot) == r.w.holder());
	CHECK(r.w.window_holders() == 1);

	comp_vk_native_segments_set_hwnd_hooks(segs, nullptr);
	comp_vk_native_segments_destroy(&segs);
}

TEST_CASE("home gone while the panel's DP still holds the window: handed to the ready laptop")
{
	Rig r;
	const xrt_screen_list b = both();
	comp_vk_native_segments *segs = r.manager(b);
	// Mostly on the panel: the laptop gets a windowless segment DP, the
	// panel's DP keeps the window.
	run(segs, MOSTLY_PANEL);
	REQUIRE(r.w.holder() != nullptr);
	REQUIRE(r.w.holder()->screen == PANEL);

	r.w.panel_ready = false;
	const int panel_dps = r.w.made_for_panel;
	const xrt_screen_list gone = laptop_only();
	comp_vk_native_segments_destroy_for_rebuild(&segs, &gone);

	REQUIRE(r.w.holder() != nullptr);
	CHECK(r.w.holder()->screen == LAPTOP);
	CHECK(fake(r.w.slot) == r.w.holder());
	CHECK(r.w.made_for_panel == panel_dps);
	CHECK(r.w.window_holders() == 1);
	CHECK(r.w.away == LAPTOP);
	for (FakeDp *d : r.w.alive) {
		CHECK(d->screen != PANEL);
	}

	r.w.panel_ready = true;
	segs = r.manager(b);
	(void)comp_vk_native_segments_update(segs, &MOSTLY_PANEL, &MOSTLY_PANEL, kFormat, true, 0);
	CHECK(r.w.away == 0);
	CHECK(fake(r.w.slot)->screen == PANEL);
	comp_vk_native_segments_set_hwnd_hooks(segs, nullptr);
	comp_vk_native_segments_destroy(&segs);
}

TEST_CASE("home gone and no screen with a ready DP: the window stays put, no DP for the panel")
{
	Rig r;
	const xrt_screen_list b = both();
	comp_vk_native_segments *segs = r.manager(b);
	run(segs, MOSTLY_PANEL);
	REQUIRE(r.w.holder()->screen == PANEL);
	FakeDp *session_dp = r.w.holder();

	// Neither the panel's DP nor the laptop's reports geometry.
	r.w.panel_ready = false;
	r.w.laptop_ready = false;
	const int panel_dps = r.w.made_for_panel;
	const xrt_screen_list gone = laptop_only();
	comp_vk_native_segments_destroy_for_rebuild(&segs, &gone);

	CHECK(fake(r.w.slot) == session_dp);
	CHECK(r.w.holder() == session_dp);
	CHECK(r.w.made_for_panel == panel_dps);
	CHECK(r.w.away == 0);
	comp_vk_native_segments_destroy_for_rebuild(&segs, nullptr);
}

TEST_CASE("home in the next list (a replug, a teardown): back to the panel's DP, as before")
{
	Rig r;
	const xrt_screen_list b = both();
	comp_vk_native_segments *segs = r.manager(b);
	run(segs, ON_LAPTOP);
	REQUIRE(r.w.holder()->screen == LAPTOP);

	// A rebuild whose list still has the home (its default): hand-back home.
	comp_vk_native_segments_destroy_for_rebuild(&segs, &b);
	REQUIRE(r.w.holder() != nullptr);
	CHECK(r.w.holder()->screen == PANEL);
	CHECK(fake(r.w.slot) == r.w.holder());
	CHECK(r.w.away == 0);

	// A teardown with hooks (no next list): the same.
	segs = r.manager(b);
	run(segs, ON_LAPTOP);
	REQUIRE(r.w.holder()->screen == LAPTOP);
	comp_vk_native_segments_destroy(&segs);
	CHECK(r.w.holder()->screen == PANEL);
	CHECK(r.w.away == 0);
}

TEST_CASE("home gone, the laptop's window-holding DP cannot be made: the panel's DP takes it, as before")
{
	Rig r;
	const xrt_screen_list b = both();
	comp_vk_native_segments *segs = r.manager(b);
	run(segs, MOSTLY_PANEL);
	REQUIRE(r.w.holder()->screen == PANEL);
	FakeDp *session_dp = r.w.holder();

	// The panel's DP holds the window: the laptop's is made first, and when
	// that fails nothing is freed — the window stays with the session DP.
	r.w.panel_ready = false;
	r.w.fail_laptop_window = true;
	const xrt_screen_list gone = laptop_only();
	comp_vk_native_segments_destroy_for_rebuild(&segs, &gone);
	CHECK(fake(r.w.slot) == session_dp);
	CHECK(r.w.holder() == session_dp);
	CHECK(r.w.away == 0);
	comp_vk_native_segments_destroy_for_rebuild(&segs, nullptr);
}
