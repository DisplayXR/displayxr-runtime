// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Per-client role resolution in @ref target_input_arbiter (ADR-034
 *        Amendment 5, #2181).
 *
 * The arbiter is compiled straight in (no loader); the policy store it reads
 * (per-app pins, Policy\Lock, PreferredPlugin, registration ids) is the fake
 * below, installed through `t_input_arbiter_set_policy`. Clients are driven
 * through the published `xsysd->input_selection`, exactly as the IPC server
 * drives them on a connection's behalf.
 *
 * Box: "hands" (ProbeOrder 50: a controller pair + unobstructed hand
 * tracking), "ctl" (100: controllers only), "pad" (150: a gamepad only), the
 * qwerty floor. Covers the issue's list: a pin to a controllers-only provider
 * keeps hand tracking on the next present claimant; an absent pinned provider
 * falls through; per-client generations stay monotonic across a global
 * re-rank; plus precedence, persist, Lock, enumerate, holders and detach.
 *
 * Every case brackets itself with `t_input_arbiter_reset()` (it stops the
 * presence poll thread, #958, which would otherwise outlive the fakes).
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_input_selection.h"
#include "xrt/xrt_system.h"

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
};

fake_provider g_hands;
fake_provider g_ctl;
fake_provider g_pad;

enum xrt_input_provider_presence
fake_get_presence(struct xrt_input_plugin_instance *inst)
{
	return reinterpret_cast<fake_provider *>(inst)->present.load() ? XRT_INPUT_PROVIDER_PRESENCE_PRESENT
	                                                               : XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
}

void
init_provider(fake_provider &prov, const char *id, const char *name)
{
	std::memset(&prov.iface, 0, sizeof(prov.iface));
	prov.iface.struct_size = sizeof(prov.iface);
	prov.iface.id = id;
	prov.iface.display_name = name;
	prov.iface.get_presence = fake_get_presence;
	prov.present.store(true);
}

struct xrt_input_plugin_instance *
instance_of(fake_provider &prov)
{
	return reinterpret_cast<struct xrt_input_plugin_instance *>(&prov);
}

struct xrt_device
make_device(enum xrt_device_type type, const char *str)
{
	struct xrt_device xdev;
	std::memset(&xdev, 0, sizeof(xdev));
	xdev.name = XRT_DEVICE_SIMPLE_CONTROLLER;
	xdev.device_type = type;
	std::snprintf(xdev.str, sizeof(xdev.str), "%s", str);
	return xdev;
}

//! The policy store the arbiter reads, in memory.
struct fake_store
{
	std::mutex m;
	std::map<std::string, std::string> pins;
	bool lock = false;
	std::string user_default;

	void
	clear()
	{
		std::lock_guard<std::mutex> g(m);
		pins.clear();
		lock = false;
		user_default.clear();
	}
} g_store;

bool
store_get_pin(const char *exe, char *out, size_t cap)
{
	std::lock_guard<std::mutex> g(g_store.m);
	out[0] = '\0';
	auto it = g_store.pins.find(exe);
	if (g_store.lock || it == g_store.pins.end()) {
		return false;
	}
	std::snprintf(out, cap, "%s", it->second.c_str());
	return true;
}

int
store_set_pin(const char *exe, const char *id)
{
	std::lock_guard<std::mutex> g(g_store.m);
	if (id == nullptr || id[0] == '\0') {
		g_store.pins.erase(exe);
	} else {
		g_store.pins[exe] = id;
	}
	return 0;
}

bool
store_lock()
{
	std::lock_guard<std::mutex> g(g_store.m);
	return g_store.lock;
}

bool
store_default(char *out, size_t cap)
{
	std::lock_guard<std::mutex> g(g_store.m);
	std::snprintf(out, cap, "%s", g_store.user_default.c_str());
	return out[0] != '\0';
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

struct fixture
{
	struct xrt_device h_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "hands-left");
	struct xrt_device h_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "hands-right");
	struct xrt_device h_ht_left = make_device(XRT_DEVICE_TYPE_HAND_TRACKER, "hands-ht-left");
	struct xrt_device h_ht_right = make_device(XRT_DEVICE_TYPE_HAND_TRACKER, "hands-ht-right");
	struct xrt_device c_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "ctl-left");
	struct xrt_device c_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "ctl-right");
	struct xrt_device pad = make_device(XRT_DEVICE_TYPE_GAMEPAD, "pad");
	struct xrt_device q_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "qwerty-left");
	struct xrt_device q_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "qwerty-right");
	int32_t h_left_i, h_ht_left_i, c_left_i, pad_i, q_left_i;

	struct xrt_input_selection_client a = client(1, "A.exe");
	struct xrt_input_selection_client b = client(2, "B.exe");

	explicit fixture(bool install_policy = true)
	{
		t_input_arbiter_reset();
		g_store.clear();
		g_sys.reset();
		init_provider(g_hands, "hands", "Optical hands");
		init_provider(g_ctl, "ctl", "Tracked controllers");
		init_provider(g_pad, "pad", "A gamepad");
		if (install_policy) {
			struct t_input_arbiter_policy p;
			std::memset(&p, 0, sizeof(p));
			p.get_app_pin = store_get_pin;
			p.set_app_pin = store_set_pin;
			p.get_policy_lock = store_lock;
			p.get_user_default = store_default;
			p.self_exe_basename = store_self;
			t_input_arbiter_set_policy(&p);
		} else {
			t_input_arbiter_set_policy(nullptr);
		}

		h_left_i = g_sys.add(&h_left);
		g_sys.add(&h_right);
		h_ht_left_i = g_sys.add(&h_ht_left);
		g_sys.add(&h_ht_right);
		c_left_i = g_sys.add(&c_left);
		g_sys.add(&c_right);
		pad_i = g_sys.add(&pad);
		q_left_i = g_sys.add(&q_left);
		g_sys.add(&q_right);

		t_input_arbiter_note_provider_pair(&g_hands.iface, instance_of(g_hands), 50, &h_left, &h_right);
		t_input_arbiter_note_provider_hand_tracking(&h_ht_left, &h_ht_right, nullptr, nullptr);
		t_input_arbiter_note_provider_pair(&g_ctl.iface, instance_of(g_ctl), 100, &c_left, &c_right);
		t_input_arbiter_note_provider_pair(&g_pad.iface, instance_of(g_pad), 150, nullptr, nullptr);
		struct xrt_device *pads[1] = {&pad};
		t_input_arbiter_note_provider_gamepad(pads, 1);
		t_input_arbiter_note_qwerty_pair(&q_left, &q_right);
		t_input_arbiter_install(&g_sys.base);
		REQUIRE(g_sys.base.input_selection != nullptr);
	}

	~fixture()
	{
		t_input_arbiter_reset();
		t_input_arbiter_set_policy(nullptr);
	}

	struct xrt_input_selection *
	xis()
	{
		return g_sys.base.input_selection;
	}

	struct xrt_system_roles
	roles(const struct xrt_input_selection_client &c)
	{
		struct xrt_system_roles r = XRT_SYSTEM_ROLES_INIT;
		REQUIRE(xrt_input_selection_get_roles(xis(), &c, &r) == XRT_SUCCESS);
		return r;
	}

	void
	request(const struct xrt_input_selection_client &c, enum xrt_input_role_family f, const char *id, bool persist)
	{
		REQUIRE(xrt_input_selection_set_preference(xis(), &c, f, id, persist) == XRT_SUCCESS);
	}

	//! Level-triggered presence: poll until @p c's left controller is @p want (past the ~250 ms poll).
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
};

} // namespace


TEST_CASE("per-client: with no preference every client sees the default walk", "[input_per_client]")
{
	fixture f;
	struct xrt_system_roles global = XRT_SYSTEM_ROLES_INIT;
	REQUIRE(g_sys.base.get_roles(&g_sys.base, &global) == XRT_SUCCESS); // the in-process implicit client
	const struct xrt_system_roles ra = f.roles(f.a);
	const struct xrt_system_roles rb = f.roles(f.b);
	for (const struct xrt_system_roles *r : {(const struct xrt_system_roles *)&global, &ra, &rb}) {
		CHECK(r->left == f.h_left_i);
		CHECK(r->hand_tracking.unobstructed.left == f.h_ht_left_i);
		CHECK(r->gamepad == f.pad_i);
		CHECK(r->generation_id > 0);
	}
	CHECK(ra.left_profile == global.left_profile);
	CHECK(rb.gamepad_profile == global.gamepad_profile);
}

TEST_CASE("per-client: a pin to a controllers-only provider keeps hand tracking on the next claimant",
          "[input_per_client]")
{
	fixture f;
	g_store.pins["A.exe"] = "ctl";
	const struct xrt_system_roles ra = f.roles(f.a);            // first use reads the pin
	CHECK(ra.left == f.c_left_i);                               // A's controllers: the pin
	CHECK(ra.hand_tracking.unobstructed.left == f.h_ht_left_i); // A's hands: NOT parked at -1
	CHECK(ra.gamepad == f.pad_i);                               // other families: default walk
	const struct xrt_system_roles rb = f.roles(f.b);
	CHECK(rb.left == f.h_left_i); // B is untouched by A's pin
}

TEST_CASE("per-client: an absent pinned provider falls through and comes back", "[input_per_client]")
{
	fixture f;
	g_store.pins["A.exe"] = "ctl";
	REQUIRE(f.roles(f.a).left == f.c_left_i);
	const uint64_t g0 = f.roles(f.a).generation_id;

	g_ctl.present.store(false);
	const struct xrt_system_roles gone = f.wait_left(f.a, f.h_left_i);
	CHECK(gone.left == f.h_left_i); // the default walk: the next present claimant
	CHECK(gone.generation_id == g0 + 1);

	g_ctl.present.store(true);
	const struct xrt_system_roles back = f.wait_left(f.a, f.c_left_i);
	CHECK(back.left == f.c_left_i);
	CHECK(back.generation_id == gone.generation_id + 1);

	SECTION("a pinned provider absent while every claimant is absent lands on the floor")
	{
		g_ctl.present.store(false);
		g_hands.present.store(false);
		CHECK(f.wait_left(f.a, f.q_left_i).left == f.q_left_i);
	}
}

TEST_CASE("per-client: generations are per client and monotonic across a global re-rank", "[input_per_client]")
{
	fixture f;
	f.request(f.a, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "ctl", false);
	const struct xrt_system_roles a0 = f.roles(f.a);
	const struct xrt_system_roles b0 = f.roles(f.b);
	REQUIRE(a0.left == f.c_left_i);
	REQUIRE(b0.left == f.h_left_i);

	// The selection generation (what the ADR-051 status tick folds in) at rest.
	const uint64_t sg0 = xrt_input_selection_get_generation(f.xis());
	(void)f.roles(f.a);
	(void)f.roles(f.b);
	CHECK(xrt_input_selection_get_generation(f.xis()) == sg0); // repeat reads: no status bump

	// The user makes ctl the default (a re-rank): B moves, A already was there.
	const struct t_input_arbiter_rank rank[2] = {{"ctl", 0, true}, {"hands", 50, true}};
	REQUIRE(t_input_arbiter_rerank(rank, 2) > 0);
	const struct xrt_system_roles a1 = f.roles(f.a);
	const struct xrt_system_roles b1 = f.roles(f.b);
	CHECK(b1.left == f.c_left_i);
	CHECK(b1.generation_id == b0.generation_id + 1);
	CHECK(a1.left == f.c_left_i);
	CHECK(a1.generation_id == a0.generation_id); // nothing moved for A: no bump
	// B's holder moved (hands -> ctl): the status generation moves, exactly once.
	const uint64_t sg1 = xrt_input_selection_get_generation(f.xis());
	CHECK(sg1 == sg0 + 1);
	(void)f.roles(f.b);
	CHECK(xrt_input_selection_get_generation(f.xis()) == sg1);

	// Back again, then A drops its request: every step only ever goes up.
	const struct t_input_arbiter_rank back[2] = {{"ctl", 100, true}, {"hands", 50, true}};
	REQUIRE(t_input_arbiter_rerank(back, 2) > 0);
	const struct xrt_system_roles b2 = f.roles(f.b);
	CHECK(b2.left == f.h_left_i);
	CHECK(b2.generation_id == b1.generation_id + 1);
	f.request(f.a, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "", false);
	const struct xrt_system_roles a2 = f.roles(f.a);
	CHECK(a2.left == f.h_left_i);
	CHECK(a2.generation_id == a1.generation_id + 1);
	CHECK(f.roles(f.a).generation_id == a2.generation_id); // a repeat read is a cache read
}

TEST_CASE("per-client: precedence — pin over request, Lock hides both", "[input_per_client]")
{
	fixture f;
	f.request(f.a, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "ctl", false);
	REQUIRE(f.roles(f.a).left == f.c_left_i);

	SECTION("a pin the user set beats the app's request")
	{
		g_store.pins["A.exe"] = XRT_INPUT_PROVIDER_ID_EMULATED;
		CHECK(t_input_arbiter_policy_changed() == 1);
		CHECK(f.roles(f.a).left == f.q_left_i);
	}
	SECTION("Policy\\Lock hides the request (and any pin)")
	{
		g_store.lock = true;
		(void)t_input_arbiter_policy_changed();
		CHECK(f.roles(f.a).left == f.h_left_i);
	}
	SECTION("a request for a family the provider does not supply is ignored")
	{
		f.request(f.a, XRT_INPUT_ROLE_FAMILY_HAND_TRACKING, "ctl", false);
		CHECK(f.roles(f.a).hand_tracking.unobstructed.left == f.h_ht_left_i);
	}
	SECTION("an unknown id is accepted and never resolves")
	{
		f.request(f.a, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "not-installed", false);
		CHECK(f.roles(f.a).left == f.h_left_i);
	}
}

TEST_CASE("per-client: persist writes the per-app pin every client of that app shares", "[input_per_client]")
{
	fixture f;
	struct xrt_input_selection_client a2 = client(3, "A.exe"); // a second instance of the same app
	REQUIRE(f.roles(a2).left == f.h_left_i);

	f.request(f.a, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "ctl", true);
	CHECK(g_store.pins["A.exe"] == "ctl");
	CHECK(f.roles(f.a).left == f.c_left_i);
	CHECK(f.roles(a2).left == f.c_left_i);
	CHECK(f.roles(f.b).left == f.h_left_i);

	f.request(f.a, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "", true); // clear
	CHECK(g_store.pins.count("A.exe") == 0);
	CHECK(f.roles(a2).left == f.h_left_i);

	SECTION("no executable identity: persist is refused and nothing is written")
	{
		// A restricted-token peer the service could not resolve (exe "") — the
		// OpenXR layer maps this to XR_ERROR_FEATURE_UNSUPPORTED.
		const size_t pins_before = g_store.pins.size();
		struct xrt_input_selection_client anon = client(9, "");
		CHECK(xrt_input_selection_set_preference(f.xis(), &anon, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "ctl",
		                                         true) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		CHECK(g_store.pins.size() == pins_before);
		CHECK(f.roles(anon).left == f.h_left_i); // and no request was recorded either
	}
}

TEST_CASE("per-client: a pin changed while the app is attached reaches it on the next get_roles", "[input_per_client]")
{
	fixture f;
	const struct xrt_system_roles before = f.roles(f.a); // attached, no pin
	REQUIRE(before.left == f.h_left_i);

	// The Dashboard / `input use ctl --app A.exe` writes the store while A runs ...
	g_store.pins["A.exe"] = "ctl";
	// ... and the service's policy watch (N3d) re-reads: A's cached pin follows.
	CHECK(t_input_arbiter_policy_changed() == 1);
	const struct xrt_system_roles after = f.roles(f.a);
	CHECK(after.left == f.c_left_i);
	CHECK(after.generation_id == before.generation_id + 1);
	CHECK(f.roles(f.b).left == f.h_left_i); // another app's pin is its own

	// Cleared while attached: back to the default walk, one more generation.
	g_store.pins.erase("A.exe");
	CHECK(t_input_arbiter_policy_changed() == 1);
	const struct xrt_system_roles cleared = f.roles(f.a);
	CHECK(cleared.left == f.h_left_i);
	CHECK(cleared.generation_id == after.generation_id + 1);
	CHECK(t_input_arbiter_policy_changed() == 0); // nothing moved: nothing counted
}

TEST_CASE("per-client: the gamepad is a per-client family; the rig is not", "[input_per_client]")
{
	fixture f;
	f.request(f.a, XRT_INPUT_ROLE_FAMILY_GAMEPAD, "pad", false);
	CHECK(f.roles(f.a).gamepad == f.pad_i);
	g_pad.present.store(false);
	struct xrt_system_roles r = f.roles(f.a);
	for (int i = 0; i < 80 && r.gamepad != -1; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
		r = f.roles(f.a);
	}
	CHECK(r.gamepad == -1); // presence-gated: no pad, no gamepad role, even when requested
	CHECK(r.rig == f.roles(f.b).rig);
}

TEST_CASE("per-client: enumerate, holders and detach", "[input_per_client]")
{
	fixture f;
	g_store.user_default = "hands";
	(void)t_input_arbiter_policy_changed();
	f.request(f.a, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "ctl", false);

	struct xrt_input_provider_page page;
	REQUIRE(xrt_input_selection_enumerate(f.xis(), &f.a, 0, &page) == XRT_SUCCESS);
	REQUIRE(page.total == 4); // hands, ctl, pad, qwerty
	std::map<std::string, struct xrt_input_provider_info> by_id;
	for (uint32_t start = 0; start < page.total; start += XRT_INPUT_PROVIDER_PAGE_MAX) {
		REQUIRE(xrt_input_selection_enumerate(f.xis(), &f.a, start, &page) == XRT_SUCCESS);
		for (uint32_t i = 0; i < page.count; i++) {
			by_id[page.items[i].id] = page.items[i];
		}
	}
	REQUIRE(by_id.size() == 4);
	const uint32_t C = XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_CONTROLLERS);
	const uint32_t H = XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_HAND_TRACKING);
	const uint32_t G = XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_GAMEPAD);
	CHECK(by_id["hands"].claimable_families == (C | H));
	CHECK(by_id["hands"].current_families == H); // A's controllers went to ctl
	CHECK(by_id["hands"].user_default);
	CHECK(std::string(by_id["hands"].display_name) == "Optical hands");
	CHECK(by_id["ctl"].current_families == C);
	CHECK(by_id["ctl"].requested_families == C);
	CHECK(by_id["pad"].claimable_families == G);
	CHECK(by_id["pad"].current_families == G);
	CHECK(std::string(XRT_INPUT_PROVIDER_ID_EMULATED) == "emulated"); // public + stable (review: not "qwerty")
	CHECK(by_id[XRT_INPUT_PROVIDER_ID_EMULATED].builtin);
	CHECK(by_id[XRT_INPUT_PROVIDER_ID_EMULATED].claimable_families == C);
	CHECK(by_id[XRT_INPUT_PROVIDER_ID_EMULATED].current_families == 0);

	// Holders: one row per attached client.
	(void)f.roles(f.b);
	bool saw_a = false, saw_b = false;
	struct xrt_input_client_holders h;
	for (uint32_t i = 0; xrt_input_selection_query_holders(f.xis(), i, &h) == XRT_SUCCESS; i++) {
		if (h.key == 1) {
			saw_a = true;
			CHECK(std::string(h.holder[XRT_INPUT_ROLE_FAMILY_CONTROLLERS]) == "ctl");
			CHECK(std::string(h.preferred[XRT_INPUT_ROLE_FAMILY_CONTROLLERS]) == "ctl");
			CHECK(std::string(h.holder[XRT_INPUT_ROLE_FAMILY_HAND_TRACKING]) == "hands");
		}
		if (h.key == 2) {
			saw_b = true;
			CHECK(std::string(h.holder[XRT_INPUT_ROLE_FAMILY_CONTROLLERS]) == "hands");
			CHECK(std::string(h.preferred[XRT_INPUT_ROLE_FAMILY_CONTROLLERS]).empty());
		}
	}
	CHECK(saw_a);
	CHECK(saw_b);
	CHECK(h.clients_dropped == 0);

	// Detach drops the request; a reconnect (new key) starts on the default walk.
	xrt_input_selection_client_detach(f.xis(), &f.a);
	struct xrt_input_selection_client again = client(4, "A.exe");
	CHECK(f.roles(again).left == f.h_left_i);
}

TEST_CASE("per-client: with no policy store everything still resolves", "[input_per_client]")
{
	fixture f(false);
	CHECK(f.roles(f.a).left == f.h_left_i);
	f.request(f.a, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "ctl", false);
	CHECK(f.roles(f.a).left == f.c_left_i);
	CHECK(xrt_input_selection_set_preference(f.xis(), &f.a, XRT_INPUT_ROLE_FAMILY_CONTROLLERS, "ctl", true) ==
	      XRT_ERROR_FEATURE_NOT_SUPPORTED);
}

TEST_CASE("per-client: table overflow is counted once per client, and the client gets the system roles",
          "[input_per_client]")
{
	fixture f;
	g_store.pins["Late.exe"] = "ctl";
	for (uint64_t k = 100; k < 100 + 40; k++) { // fill every entry
		(void)f.roles(client(k, "Filler.exe"));
	}
	struct xrt_input_selection_client late = client(500, "Late.exe");
	const struct xrt_system_roles r = f.roles(late);
	CHECK(r.left == f.h_left_i); // the system-wide roles: its pin does not apply
	(void)f.roles(late);         // a refused client's per-frame calls do not recount
	(void)f.roles(client(501, "Later.exe"));
	struct xrt_input_client_holders h;
	REQUIRE(xrt_input_selection_query_holders(f.xis(), 0, &h) == XRT_SUCCESS);
	CHECK(h.clients_dropped == 2);
}
