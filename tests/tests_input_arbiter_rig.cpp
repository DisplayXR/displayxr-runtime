// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Rig (navigation) role arbitration tests for @ref target_input_arbiter.
 *
 * The rig role follows the same presence-ranked walk as the hands (ADR-034
 * Amendment 3, extended by Amendment 4): `xrt_system_roles::rig` is the index
 * of the navigation device of the highest-priority (lowest ProbeOrder) PRESENT
 * provider that has one. Its floor is not a candidate but an absence — `-1`
 * means the runtime's own fly camera (qwerty WASD/mouse-look) drives the rig.
 *
 * The arbiter polls presence on its own thread (#958), so a flip is observed
 * here by waiting for it rather than by assuming the next `get_roles` call
 * already sees it. Every case brackets itself with `t_input_arbiter_reset()`,
 * which stops that thread — without the trailing reset the poll thread would
 * outlive the fakes it dereferences.
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_system.h"

#include "target_input_arbiter.h"

#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>


namespace {

//! A provider's fake: an iface with a presence flag the test drives.
struct fake_provider
{
	struct xrt_input_plugin_iface iface;
	std::atomic<bool> present{true};
};

//! There is exactly one arbiter per process, so the fakes are file-scope too —
//! nothing the presence poll thread touches may live on a test's stack.
fake_provider g_prov_a;
fake_provider g_prov_b;

fake_provider *
provider_of(struct xrt_input_plugin_instance *inst)
{
	return reinterpret_cast<fake_provider *>(inst);
}

enum xrt_input_provider_presence
fake_get_presence(struct xrt_input_plugin_instance *inst)
{
	return provider_of(inst)->present.load() ? XRT_INPUT_PROVIDER_PRESENCE_PRESENT
	                                         : XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
}

void
init_provider(fake_provider &prov, const char *id)
{
	std::memset(&prov.iface, 0, sizeof(prov.iface));
	prov.iface.struct_size = sizeof(prov.iface);
	prov.iface.id = id;
	prov.iface.get_presence = fake_get_presence;
	prov.present.store(true);
}

//! The instance handle the arbiter hands back to get_presence is just the fake.
struct xrt_input_plugin_instance *
instance_of(fake_provider &prov)
{
	return reinterpret_cast<struct xrt_input_plugin_instance *>(&prov);
}

//! The whole of an xrt_device the arbiter reads: a type, a name and a string.
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

//! The system the roles index into. Devices are added in creation order, so an
//! index is just the position at which the test appended the device.
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
	static_roles(struct xrt_system_devices *xsysd, struct xrt_system_roles *out_roles)
	{
		(void)xsysd;
		struct xrt_system_roles init = XRT_SYSTEM_ROLES_INIT;
		*out_roles = init;
		return XRT_SUCCESS;
	}
};

fake_system g_sys;

struct xrt_system_roles
read_roles()
{
	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	REQUIRE(g_sys.base.get_roles(&g_sys.base, &roles) == XRT_SUCCESS);
	return roles;
}

/*!
 * Wait for the presence poll thread to pick a flip up. Level-triggered, so
 * polling for the expected index is the honest way to observe it; the timeout
 * is generous against the arbiter's ~250 ms poll interval.
 */
struct xrt_system_roles
wait_for_rig(int32_t want)
{
	struct xrt_system_roles roles = read_roles();
	for (int i = 0; i < 200 && roles.rig != want; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
		roles = read_roles();
	}
	return roles;
}

struct xrt_system_roles
wait_for_left(int32_t want)
{
	struct xrt_system_roles roles = read_roles();
	for (int i = 0; i < 200 && roles.left != want; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
		roles = read_roles();
	}
	return roles;
}

} // namespace


TEST_CASE("input arbiter: the rig role follows the highest-priority present provider")
{
	t_input_arbiter_reset();
	g_sys.reset();
	init_provider(g_prov_a, "prov-a");
	init_provider(g_prov_b, "prov-b");

	struct xrt_device a_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "a-left");
	struct xrt_device a_nav = make_device(XRT_DEVICE_TYPE_NAVIGATION, "a-nav");
	struct xrt_device b_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "b-left");
	struct xrt_device b_nav = make_device(XRT_DEVICE_TYPE_NAVIGATION, "b-nav");

	g_sys.add(&a_left);
	int32_t a_nav_index = g_sys.add(&a_nav);
	g_sys.add(&b_left);
	int32_t b_nav_index = g_sys.add(&b_nav);

	struct xrt_device *a_devs[] = {&a_left, &a_nav};
	struct xrt_device *b_devs[] = {&b_left, &b_nav};

	// ProbeOrder 50 beats 200, and the builder notes them in that order.
	t_input_arbiter_note_provider_pair(&g_prov_a.iface, instance_of(g_prov_a), 50, &a_left, nullptr);
	t_input_arbiter_note_provider_navigation(a_devs, 2);
	t_input_arbiter_note_provider_pair(&g_prov_b.iface, instance_of(g_prov_b), 200, &b_left, nullptr);
	t_input_arbiter_note_provider_navigation(b_devs, 2);

	t_input_arbiter_install(&g_sys.base);

	SECTION("both present: the lower ProbeOrder holds the rig")
	{
		CHECK(read_roles().rig == a_nav_index);
	}

	SECTION("the winner going absent hands the rig down the ranks, and back on replug")
	{
		REQUIRE(read_roles().rig == a_nav_index);

		g_prov_a.present.store(false);
		CHECK(wait_for_rig(b_nav_index).rig == b_nav_index);

		g_prov_b.present.store(false);
		CHECK(wait_for_rig(-1).rig == -1); // The runtime fly camera holds it.

		g_prov_a.present.store(true);
		CHECK(wait_for_rig(a_nav_index).rig == a_nav_index);
	}

	t_input_arbiter_reset();
}

TEST_CASE("input arbiter: a provider without a navigation device never wins the rig")
{
	t_input_arbiter_reset();
	g_sys.reset();
	init_provider(g_prov_a, "prov-a-hands-only");
	init_provider(g_prov_b, "prov-b-navigates");

	struct xrt_device a_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "a-left");
	struct xrt_device a_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "a-right");
	struct xrt_device b_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "b-left");
	struct xrt_device b_nav = make_device(XRT_DEVICE_TYPE_NAVIGATION, "b-nav");

	int32_t a_left_index = g_sys.add(&a_left);
	int32_t a_right_index = g_sys.add(&a_right);
	g_sys.add(&b_left);
	int32_t b_nav_index = g_sys.add(&b_nav);

	struct xrt_device *a_devs[] = {&a_left, &a_right};
	struct xrt_device *b_devs[] = {&b_left, &b_nav};

	t_input_arbiter_note_provider_pair(&g_prov_a.iface, instance_of(g_prov_a), 50, &a_left, &a_right);
	t_input_arbiter_note_provider_navigation(a_devs, 2);
	t_input_arbiter_note_provider_pair(&g_prov_b.iface, instance_of(g_prov_b), 200, &b_left, nullptr);
	t_input_arbiter_note_provider_navigation(b_devs, 2);

	t_input_arbiter_install(&g_sys.base);

	// A wins both hands on ProbeOrder; the rig is not a hand and skips it.
	struct xrt_system_roles roles = read_roles();
	CHECK(roles.left == a_left_index);
	CHECK(roles.right == a_right_index);
	CHECK(roles.rig == b_nav_index);

	t_input_arbiter_reset();
}

TEST_CASE("input arbiter: hand-role churn bumps the generation without moving the rig")
{
	t_input_arbiter_reset();
	g_sys.reset();
	init_provider(g_prov_a, "prov-a-hands-only");
	init_provider(g_prov_b, "prov-b-navigates");

	struct xrt_device a_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "a-left");
	struct xrt_device b_nav = make_device(XRT_DEVICE_TYPE_NAVIGATION, "b-nav");
	struct xrt_device qwerty_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "qwerty-left");

	int32_t a_left_index = g_sys.add(&a_left);
	int32_t b_nav_index = g_sys.add(&b_nav);
	int32_t qwerty_left_index = g_sys.add(&qwerty_left);

	struct xrt_device *a_devs[] = {&a_left};
	struct xrt_device *b_devs[] = {&b_nav};

	t_input_arbiter_note_provider_pair(&g_prov_a.iface, instance_of(g_prov_a), 50, &a_left, nullptr);
	t_input_arbiter_note_provider_navigation(a_devs, 1);
	t_input_arbiter_note_provider_pair(&g_prov_b.iface, instance_of(g_prov_b), 200, nullptr, nullptr);
	t_input_arbiter_note_provider_navigation(b_devs, 1);
	t_input_arbiter_note_qwerty_pair(&qwerty_left, nullptr);

	t_input_arbiter_install(&g_sys.base);

	struct xrt_system_roles before = read_roles();
	REQUIRE(before.left == a_left_index);
	REQUIRE(before.rig == b_nav_index);

	// Unplug the hands-only provider: the left hand falls to the qwerty
	// floor. That is a role change and must bump the generation, but the
	// rig holder did not move and its index must not either.
	g_prov_a.present.store(false);
	struct xrt_system_roles after = wait_for_left(qwerty_left_index);

	CHECK(after.left == qwerty_left_index);
	CHECK(after.generation_id > before.generation_id);
	CHECK(after.rig == b_nav_index);

	// And a repeated read with nothing moving does NOT bump again.
	struct xrt_system_roles again = read_roles();
	CHECK(again.generation_id == after.generation_id);
	CHECK(again.rig == after.rig);

	t_input_arbiter_reset();
}

TEST_CASE("input arbiter: with no providers the rig parks at -1")
{
	t_input_arbiter_reset();
	g_sys.reset();

	struct xrt_device qwerty_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "qwerty-left");
	struct xrt_device qwerty_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "qwerty-right");

	g_sys.add(&qwerty_left);
	g_sys.add(&qwerty_right);

	t_input_arbiter_note_qwerty_pair(&qwerty_left, &qwerty_right);
	t_input_arbiter_install(&g_sys.base);

	// A single candidate is not arbitrated at all — the static roles stand,
	// and they say the runtime's own fly camera holds the rig.
	CHECK(read_roles().rig == -1);

	t_input_arbiter_reset();
}
