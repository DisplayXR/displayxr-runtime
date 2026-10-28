// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The controller role follows the engaged device (ADR-034 Amendment 6)
 *        in @ref target_input_arbiter.
 *
 * The arbiter is compiled straight in, like the other arbiter units. Box:
 * "cam" (ProbeOrder 50: a controller pair that implements `get_engaged_ns`,
 * e.g. camera hands whose pinch is the deliberate input), optionally "leap"
 * (ProbeOrder 60: a controller pair built BEFORE the slot), and the qwerty
 * floor, whose engagement is read through `t_input_arbiter_set_floor_engagement`
 * exactly as the qwerty builder installs it.
 *
 * Covers: nothing engaged -> the keyboard holds (from generation 1); the
 * first pinch -> the provider; qwerty input -> qwerty takes it back (with the
 * reported profile unchanged); no floor -> a never-engaged slot provider
 * holds by rank; presence loss -> the last
 * engaged present candidate, and an old engagement does not reclaim on
 * return; a provider without the slot keeps today's behaviour; a pin and a
 * PreferredPlugin stop the switching; `ControllerSwitching` = presence is
 * today's walk; and the engagement slot is called only while a client
 * resolves roles (a provider may treat the calls as demand).
 *
 * Every case brackets itself with `t_input_arbiter_reset()` (it stops the
 * presence poll thread, #958, which would otherwise outlive the fakes).
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_input_selection.h"
#include "xrt/xrt_system.h"

#include "os/os_time.h"

#include "target_input_arbiter.h"

#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>


namespace {

struct fake_provider
{
	struct xrt_input_plugin_iface iface;
	std::atomic<bool> present{true};
	std::atomic<int64_t> engaged[2];
	std::atomic<uint32_t> engage_calls{0};
};

fake_provider g_cam;
fake_provider g_leap;

enum xrt_input_provider_presence
fake_get_presence(struct xrt_input_plugin_instance *inst)
{
	return reinterpret_cast<fake_provider *>(inst)->present.load() ? XRT_INPUT_PROVIDER_PRESENCE_PRESENT
	                                                               : XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
}

int64_t
fake_get_engaged_ns(struct xrt_input_plugin_instance *inst, uint32_t hand)
{
	fake_provider *p = reinterpret_cast<fake_provider *>(inst);
	p->engage_calls.fetch_add(1);
	return hand < 2 ? p->engaged[hand].load() : 0;
}

void
init_provider(fake_provider &prov, const char *id, bool with_slot)
{
	std::memset(&prov.iface, 0, sizeof(prov.iface));
	prov.iface.id = id;
	prov.iface.display_name = id;
	prov.iface.get_presence = fake_get_presence;
	if (with_slot) {
		prov.iface.struct_size = sizeof(prov.iface);
		prov.iface.get_engaged_ns = fake_get_engaged_ns;
	} else {
		// Built against the header before Amendment 6: the slot is past its struct_size.
		prov.iface.struct_size = (uint32_t)offsetof(struct xrt_input_plugin_iface, get_engaged_ns);
	}
	prov.present.store(true);
	prov.engaged[0].store(0);
	prov.engaged[1].store(0);
	prov.engage_calls.store(0);
}

struct xrt_input_plugin_instance *
instance_of(fake_provider &prov)
{
	return reinterpret_cast<struct xrt_input_plugin_instance *>(&prov);
}

//! The qwerty floor's input stamps, per emulated controller.
std::atomic<int64_t> g_q_input[2];
struct xrt_device *g_q_devs[2];

int64_t
fake_floor_engaged(struct xrt_device *xdev)
{
	for (int h = 0; h < 2; h++) {
		if (xdev == g_q_devs[h]) {
			return g_q_input[h].load();
		}
	}
	return 0;
}

//! What the qwerty controller emulates (its binding-profile remaps); one is enough here.
struct xrt_binding_profile g_q_profiles[1];

struct xrt_device
make_device(enum xrt_device_type type, const char *str, enum xrt_device_name name)
{
	struct xrt_device xdev;
	std::memset(&xdev, 0, sizeof(xdev));
	xdev.name = name;
	xdev.device_type = type;
	std::snprintf(xdev.str, sizeof(xdev.str), "%s", str);
	return xdev;
}

//! The policy store, in memory.
struct fake_store
{
	std::mutex m;
	std::map<std::string, std::string> pins;
	std::string user_default;
	bool switching_use = true;
} g_store;

bool
store_get_pin(const char *exe, char *out, size_t cap)
{
	std::lock_guard<std::mutex> g(g_store.m);
	out[0] = '\0';
	auto it = g_store.pins.find(exe);
	if (it == g_store.pins.end()) {
		return false;
	}
	std::snprintf(out, cap, "%s", it->second.c_str());
	return true;
}

bool
store_default(char *out, size_t cap)
{
	std::lock_guard<std::mutex> g(g_store.m);
	std::snprintf(out, cap, "%s", g_store.user_default.c_str());
	return out[0] != '\0';
}

bool
store_switching()
{
	std::lock_guard<std::mutex> g(g_store.m);
	return g_store.switching_use;
}

bool
store_self(char *out, size_t cap)
{
	std::snprintf(out, cap, "%s", "inproc.exe");
	return true;
}

struct fake_system
{
	struct xrt_system_devices base;

	void
	reset()
	{
		std::memset(&base, 0, sizeof(base));
		base.get_roles = static_roles;
	}

	int32_t
	add(struct xrt_device *xdev)
	{
		int32_t index = (int32_t)base.xdev_count;
		base.xdevs[base.xdev_count++] = xdev;
		return index;
	}

	static xrt_result_t
	static_roles(struct xrt_system_devices *, struct xrt_system_roles *out_roles)
	{
		struct xrt_system_roles init = XRT_SYSTEM_ROLES_INIT;
		*out_roles = init;
		return XRT_SUCCESS;
	}
} g_sys;

struct xrt_input_selection_client
client(uint64_t key, const char *exe)
{
	struct xrt_input_selection_client c;
	std::memset(&c, 0, sizeof(c));
	c.key = key;
	std::snprintf(c.exe_basename, sizeof(c.exe_basename), "%s", exe);
	return c;
}

struct fixture_options
{
	bool with_leap = false;
	bool no_floor = false;
	bool switching_use = true;
	std::string user_default;
	std::map<std::string, std::string> pins;
};

struct fixture
{
	struct xrt_device c_left =
	    make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "cam-left", XRT_DEVICE_SIMPLE_CONTROLLER);
	struct xrt_device c_right =
	    make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "cam-right", XRT_DEVICE_SIMPLE_CONTROLLER);
	struct xrt_device l_left =
	    make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "leap-left", XRT_DEVICE_SIMPLE_CONTROLLER);
	struct xrt_device l_right =
	    make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "leap-right", XRT_DEVICE_SIMPLE_CONTROLLER);
	struct xrt_device q_left =
	    make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "qwerty-left", XRT_DEVICE_WMR_CONTROLLER);
	struct xrt_device q_right =
	    make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "qwerty-right", XRT_DEVICE_WMR_CONTROLLER);
	int32_t c_left_i = -1, l_left_i = -1, q_left_i = -1;

	struct xrt_input_selection_client a = client(1, "A.exe");
	struct xrt_input_selection_client b = client(2, "B.exe");

	explicit fixture(const fixture_options &opt = fixture_options())
	{
		t_input_arbiter_reset();
		g_sys.reset();
		{
			std::lock_guard<std::mutex> g(g_store.m);
			g_store.pins = opt.pins;
			g_store.user_default = opt.user_default;
			g_store.switching_use = opt.switching_use;
		}
		init_provider(g_cam, "cam", true);
		init_provider(g_leap, "leap", false);
		g_q_input[0].store(0);
		g_q_input[1].store(0);
		std::memset(g_q_profiles, 0, sizeof(g_q_profiles));
		g_q_profiles[0].name = XRT_DEVICE_SIMPLE_CONTROLLER;
		q_left.binding_profiles = g_q_profiles;
		q_left.binding_profile_count = 1;
		q_right.binding_profiles = g_q_profiles;
		q_right.binding_profile_count = 1;
		g_q_devs[0] = &q_left;
		g_q_devs[1] = &q_right;

		struct t_input_arbiter_policy p;
		std::memset(&p, 0, sizeof(p));
		p.get_app_pin = store_get_pin;
		p.get_user_default = store_default;
		p.self_exe_basename = store_self;
		p.get_controller_switching = store_switching;
		t_input_arbiter_set_policy(&p);

		c_left_i = g_sys.add(&c_left);
		g_sys.add(&c_right);
		if (opt.with_leap) {
			l_left_i = g_sys.add(&l_left);
			g_sys.add(&l_right);
		}
		if (!opt.no_floor) {
			q_left_i = g_sys.add(&q_left);
			g_sys.add(&q_right);
		}

		t_input_arbiter_note_provider_pair(&g_cam.iface, instance_of(g_cam), 50, &c_left, &c_right);
		if (opt.with_leap) {
			t_input_arbiter_note_provider_pair(&g_leap.iface, instance_of(g_leap), 60, &l_left, &l_right);
		}
		if (!opt.no_floor) {
			t_input_arbiter_note_qwerty_pair(&q_left, &q_right);
			t_input_arbiter_set_floor_engagement(fake_floor_engaged);
		}
		t_input_arbiter_install(&g_sys.base);
		REQUIRE(g_sys.base.input_selection != nullptr);
	}

	~fixture()
	{
		t_input_arbiter_reset();
		t_input_arbiter_set_policy(nullptr);
	}

	struct xrt_system_roles
	roles(const struct xrt_input_selection_client &c)
	{
		struct xrt_system_roles r = XRT_SYSTEM_ROLES_INIT;
		REQUIRE(xrt_input_selection_get_roles(g_sys.base.input_selection, &c, &r) == XRT_SUCCESS);
		return r;
	}

	//! Poll (as an app's xrSyncActions would) until @p c's left controller is @p want, or ~2 s.
	struct xrt_system_roles
	wait_left(const struct xrt_input_selection_client &c, int32_t want)
	{
		struct xrt_system_roles r = roles(c);
		for (int i = 0; i < 80 && r.left != want; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
			r = roles(c);
		}
		return r;
	}

	//! Keep resolving for @p ms (several poll ticks) and return the last answer.
	struct xrt_system_roles
	settle(const struct xrt_input_selection_client &c, int ms = 700)
	{
		struct xrt_system_roles r = roles(c);
		for (int t = 0; t < ms; t += 25) {
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
			r = roles(c);
		}
		return r;
	}
};

int64_t
now_ns()
{
	return (int64_t)os_monotonic_get_ns();
}

} // namespace


TEST_CASE("engagement: nothing engaged -> the keyboard holds; a pinch takes the hand, qwerty input takes it back",
          "[input_engagement]")
{
	fixture f;
	// Generation 1 already: a slot provider that has not been engaged does not hold a hand while
	// the floor exists — the app starts on the emulated controllers, not on controllers that
	// report nothing until the user touches the mouse (the first live run's bug).
	struct xrt_system_roles global = XRT_SYSTEM_ROLES_INIT;
	REQUIRE(g_sys.base.get_roles(&g_sys.base, &global) == XRT_SUCCESS);
	CHECK(global.left == f.q_left_i);
	CHECK(global.right == f.q_left_i + 1);
	CHECK(global.generation_id == 1);
	struct xrt_system_roles r = f.settle(f.a, 400);
	CHECK(r.left == f.q_left_i);
	CHECK(r.right == f.q_left_i + 1);
	// Profile stability (#6): the floor masquerades as the top provider's profile, so the
	// provider <-> qwerty swap is a pose-source change, not an interaction-profile change.
	const enum xrt_device_name profile = r.left_profile;
	CHECK(profile == XRT_DEVICE_SIMPLE_CONTROLLER);
	const uint64_t g0 = r.generation_id;

	// The first pinch (left hand): the provider takes that hand only.
	g_cam.engaged[0].store(now_ns());
	r = f.wait_left(f.a, f.c_left_i);
	CHECK(r.left == f.c_left_i);
	CHECK(r.right == f.q_left_i + 1); // the right hand was not engaged: still the keyboard's
	CHECK(r.generation_id > g0);
	CHECK(r.left_profile == profile);
	const uint64_t g1 = r.generation_id;

	// Pinching on: the leader refreshes its own engagement — nothing moves.
	g_cam.engaged[0].store(now_ns());
	r = f.settle(f.a, 400);
	CHECK(r.left == f.c_left_i);
	CHECK(r.generation_id == g1);

	// The user reaches for the keyboard (a key aimed at the left emulated controller).
	g_q_input[0].store(now_ns());
	r = f.wait_left(f.a, f.q_left_i);
	CHECK(r.left == f.q_left_i);
	CHECK(r.generation_id > g1);
	CHECK(r.left_profile == profile);

	// The in-process implicit client follows the same walk.
	REQUIRE(g_sys.base.get_roles(&g_sys.base, &global) == XRT_SUCCESS);
	CHECK(global.left == f.q_left_i);
}

TEST_CASE("engagement: with no floor a never-engaged slot provider holds by rank", "[input_engagement]")
{
	fixture_options opt;
	opt.no_floor = true; // EmulatedControllers = 0
	fixture f(opt);
	struct xrt_system_roles r = f.settle(f.a, 400);
	CHECK(r.left == f.c_left_i);
	CHECK(r.right == f.c_left_i + 1);
}

TEST_CASE("engagement: presence loss falls to the last engaged; an old pinch does not reclaim on return",
          "[input_engagement]")
{
	fixture f;
	g_q_input[0].store(now_ns());
	REQUIRE(f.wait_left(f.a, f.q_left_i).left == f.q_left_i);
	g_cam.engaged[0].store(now_ns());
	REQUIRE(f.wait_left(f.a, f.c_left_i).left == f.c_left_i);

	g_cam.present.store(false);
	CHECK(f.wait_left(f.a, f.q_left_i).left == f.q_left_i); // gone -> the remaining engaged candidate

	g_cam.present.store(true);
	// Its last pinch predates its return: the keyboard (engaged) keeps the hand.
	CHECK(f.settle(f.a, 700).left == f.q_left_i);

	g_cam.engaged[0].store(now_ns()); // a fresh pinch after the return counts
	CHECK(f.wait_left(f.a, f.c_left_i).left == f.c_left_i);
}

TEST_CASE("engagement: a provider without the slot keeps today's behaviour (continuously engaged)",
          "[input_engagement]")
{
	fixture_options opt;
	opt.with_leap = true;
	fixture f(opt);
	g_cam.present.store(false); // only the legacy provider and the floor compete
	REQUIRE(f.wait_left(f.a, f.l_left_i).left == f.l_left_i);
	g_q_input[0].store(now_ns());
	CHECK(f.settle(f.a, 700).left == f.l_left_i); // the keyboard does not take it from a present legacy provider
	g_leap.present.store(false);
	CHECK(f.wait_left(f.a, f.q_left_i).left == f.q_left_i); // presence still gates
	CHECK(g_leap.engage_calls.load() == 0);                 // the slot is past its struct_size: never called
}

TEST_CASE("engagement: a per-app pin is a hard choice — no switching for that client", "[input_engagement]")
{
	fixture_options opt;
	opt.pins["A.exe"] = "cam";
	fixture f(opt);
	REQUIRE(f.roles(f.a).left == f.c_left_i); // A pinned: the provider, engaged or not
	REQUIRE(f.roles(f.b).left == f.q_left_i); // B: nothing engaged -> the keyboard
	g_cam.engaged[0].store(now_ns());
	CHECK(f.wait_left(f.b, f.c_left_i).left == f.c_left_i); // B follows the pinch
	g_q_input[0].store(now_ns());
	CHECK(f.wait_left(f.b, f.q_left_i).left == f.q_left_i); // ...and the keyboard
	CHECK(f.roles(f.a).left == f.c_left_i);                 // A stays pinned throughout
}

TEST_CASE("engagement: PreferredPlugin naming a controller provider stops the switching", "[input_engagement]")
{
	fixture_options opt;
	opt.user_default = "cam";
	fixture f(opt);
	g_q_input[0].store(now_ns());
	CHECK(f.settle(f.a, 700).left == f.c_left_i);
}

TEST_CASE("engagement: ControllerSwitching = presence is today's walk", "[input_engagement]")
{
	fixture_options opt;
	opt.switching_use = false;
	fixture f(opt);
	g_q_input[0].store(now_ns());
	CHECK(f.settle(f.a, 700).left == f.c_left_i);
	CHECK(g_cam.engage_calls.load() == 0); // not asked at all

	// Flipped live through the policy hook.
	{
		std::lock_guard<std::mutex> g(g_store.m);
		g_store.switching_use = true;
	}
	t_input_arbiter_policy_changed();
	g_q_input[0].store(now_ns());
	CHECK(f.wait_left(f.a, f.q_left_i).left == f.q_left_i);
}

TEST_CASE("engagement: the slot is called only while a client resolves roles (demand)", "[input_engagement]")
{
	fixture f;
	(void)f.settle(f.a, 600);
	CHECK(g_cam.engage_calls.load() > 0); // asked while a client is resolving, holder or not

	// No client resolves roles any more: after the demand window the calls stop.
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	const uint32_t before = g_cam.engage_calls.load();
	std::this_thread::sleep_for(std::chrono::milliseconds(700));
	CHECK(g_cam.engage_calls.load() == before);

	// A client comes back: the calls resume.
	(void)f.settle(f.a, 600);
	CHECK(g_cam.engage_calls.load() > before);
}
