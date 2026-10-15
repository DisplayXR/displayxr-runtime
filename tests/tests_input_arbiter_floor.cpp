// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The optional hand-role floor of @ref target_input_arbiter (ADR-054
 *        N7, #2143 — the `EmulatedControllers` gate).
 *
 * With the gate at 0 the builder notes no qwerty pair, so the arbiter has no
 * floor candidate: the hands must fall to -1 (which the OpenXR layer reports
 * as XR_NULL_PATH, one profile-changed event per flip) whenever every
 * provider is absent or withdrawn — and a LONE provider must still get the
 * arbiter installed, or its hands could never fall and come back. A live
 * gate flip is a withdrawal / re-admission of a pair that exists
 * (`t_input_arbiter_set_floor_withdrawn`), the way `Enabled` withdraws a
 * provider; it never adds a pair the build did not create.
 *
 * Every case brackets itself with `t_input_arbiter_reset()` (it stops the
 * presence poll thread, #958, which would otherwise outlive the fakes).
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

struct fake_provider
{
	struct xrt_input_plugin_iface iface;
	std::atomic<bool> present{true};
};

fake_provider g_prov_a;

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

//! Level-triggered presence: poll for the expected index, generously past the
//! arbiter's ~250 ms poll interval.
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

//! One hand-only provider (ProbeOrder 50) and NO qwerty pair: the gate at 0.
struct no_floor_fixture
{
	struct xrt_device a_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "a-left");
	struct xrt_device a_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "a-right");
	int32_t a_left_index = -1;
	int32_t a_right_index = -1;

	no_floor_fixture()
	{
		t_input_arbiter_reset();
		g_sys.reset();
		init_provider(g_prov_a, "prov-a");
		a_left_index = g_sys.add(&a_left);
		a_right_index = g_sys.add(&a_right);
		t_input_arbiter_note_provider_pair(&g_prov_a.iface, instance_of(g_prov_a), 50, &a_left, &a_right);
		t_input_arbiter_note_qwerty_pair(nullptr, nullptr); // the gate at 0: nothing noted
		t_input_arbiter_install(&g_sys.base);
	}

	~no_floor_fixture()
	{
		t_input_arbiter_reset();
	}
};

//! The same provider OVER a qwerty pair: the gate at 1 (today's default).
struct floor_fixture
{
	struct xrt_device a_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "a-left");
	struct xrt_device a_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "a-right");
	struct xrt_device q_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "qwerty-left");
	struct xrt_device q_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "qwerty-right");
	int32_t a_left_index = -1;
	int32_t q_left_index = -1;

	floor_fixture()
	{
		t_input_arbiter_reset();
		g_sys.reset();
		init_provider(g_prov_a, "prov-a");
		a_left_index = g_sys.add(&a_left);
		g_sys.add(&a_right);
		q_left_index = g_sys.add(&q_left);
		g_sys.add(&q_right);
		t_input_arbiter_note_provider_pair(&g_prov_a.iface, instance_of(g_prov_a), 50, &a_left, &a_right);
		t_input_arbiter_note_qwerty_pair(&q_left, &q_right);
		t_input_arbiter_install(&g_sys.base);
	}

	~floor_fixture()
	{
		t_input_arbiter_reset();
	}
};

} // namespace


TEST_CASE("input arbiter floor: no qwerty pair noted means no floor candidate")
{
	t_input_arbiter_reset();
	CHECK_FALSE(t_input_arbiter_has_floor());
	t_input_arbiter_note_qwerty_pair(nullptr, nullptr);
	CHECK_FALSE(t_input_arbiter_has_floor());

	struct xrt_device q_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "qwerty-left");
	t_input_arbiter_note_qwerty_pair(&q_left, nullptr);
	CHECK(t_input_arbiter_has_floor());
	t_input_arbiter_reset();
	CHECK_FALSE(t_input_arbiter_has_floor());
}

TEST_CASE("input arbiter floor: a lone provider with no floor is still arbitrated — hands fall to -1 and come back")
{
	no_floor_fixture f;

	bool installed = false;
	bool provider_holds = false;
	t_input_arbiter_get_status(&installed, &provider_holds);
	REQUIRE(installed); // one candidate, but no floor under it: the walk is the only way down
	CHECK(provider_holds);

	struct xrt_system_roles before = read_roles();
	REQUIRE(before.left == f.a_left_index);
	REQUIRE(before.right == f.a_right_index);
	CHECK(before.left_profile == XRT_DEVICE_SIMPLE_CONTROLLER);

	SECTION("presence loss: both hands unassigned, no device, no profile; presence back: the provider again")
	{
		g_prov_a.present.store(false);
		struct xrt_system_roles gone = wait_for_left(-1);
		CHECK(gone.left == -1);
		CHECK(gone.right == -1);
		CHECK(gone.left_profile == XRT_DEVICE_INVALID);
		CHECK(gone.right_profile == XRT_DEVICE_INVALID);
		CHECK(gone.generation_id == before.generation_id + 1);
		t_input_arbiter_get_status(nullptr, &provider_holds);
		CHECK_FALSE(provider_holds);

		g_prov_a.present.store(true);
		struct xrt_system_roles back = wait_for_left(f.a_left_index);
		CHECK(back.left == f.a_left_index);
		CHECK(back.right == f.a_right_index);
		CHECK(back.generation_id == gone.generation_id + 1); // exactly one flip each way
	}

	SECTION("`input disable` of the only provider: withdrawn -> -1, not qwerty")
	{
		const struct t_input_arbiter_rank disable_a[] = {{"prov-a", 50, false}};
		CHECK(t_input_arbiter_rerank(disable_a, 1) == 1);
		struct xrt_system_roles none = read_roles();
		CHECK(none.left == -1);
		CHECK(none.right == -1);
		CHECK(none.left_profile == XRT_DEVICE_INVALID);
		CHECK(none.generation_id == before.generation_id + 1);
		CHECK(g_sys.base.xdevs[f.a_left_index] == &f.a_left); // devices stay resident

		const struct t_input_arbiter_rank enable_a[] = {{"prov-a", 50, true}};
		CHECK(t_input_arbiter_rerank(enable_a, 1) == 1);
		CHECK(read_roles().left == f.a_left_index);
	}

	SECTION("the gate's live half is a no-op when there is no pair to withdraw")
	{
		CHECK(t_input_arbiter_set_floor_withdrawn(true) == 0);
		CHECK(t_input_arbiter_set_floor_withdrawn(false) == 0);
		CHECK(read_roles().generation_id == before.generation_id);
	}
}

TEST_CASE("input arbiter floor: no candidates at all is not installed; the static roles (-1) stand")
{
	t_input_arbiter_reset();
	g_sys.reset();
	t_input_arbiter_note_qwerty_pair(nullptr, nullptr);
	t_input_arbiter_install(&g_sys.base);

	bool installed = true;
	t_input_arbiter_get_status(&installed, nullptr);
	CHECK_FALSE(installed);
	CHECK(g_sys.base.get_roles == fake_system::static_roles);
	struct xrt_system_roles roles = read_roles();
	CHECK(roles.left == -1);
	CHECK(roles.right == -1);
	CHECK(t_input_arbiter_set_floor_withdrawn(true) == 0); // nothing to apply to
	t_input_arbiter_reset();
}

TEST_CASE("input arbiter floor: a live gate flip withdraws and re-admits an existing pair")
{
	floor_fixture f;
	REQUIRE(t_input_arbiter_has_floor());
	struct xrt_system_roles before = read_roles();
	REQUIRE(before.left == f.a_left_index);

	// The provider goes away: the floor takes the hands (today's behaviour).
	g_prov_a.present.store(false);
	struct xrt_system_roles on_floor = wait_for_left(f.q_left_index);
	REQUIRE(on_floor.left == f.q_left_index);

	// `input emulated off` while the service runs: the pair is withdrawn like
	// a disabled provider — still in xdevs, out of the walk — so the hands
	// go to -1 at once, one generation.
	CHECK(t_input_arbiter_set_floor_withdrawn(true) == 1);
	struct xrt_system_roles withdrawn = read_roles();
	CHECK(withdrawn.left == -1);
	CHECK(withdrawn.right == -1);
	CHECK(withdrawn.left_profile == XRT_DEVICE_INVALID);
	CHECK(withdrawn.generation_id == on_floor.generation_id + 1);
	CHECK(g_sys.base.xdevs[f.q_left_index] == &f.q_left);
	CHECK(t_input_arbiter_has_floor()); // noted at build; withdrawn is a standing, not an absence

	SECTION("the same policy again is a no-op")
	{
		CHECK(t_input_arbiter_set_floor_withdrawn(true) == 0);
		CHECK(read_roles().generation_id == withdrawn.generation_id);
	}

	SECTION("the provider coming back still outranks a withdrawn floor")
	{
		g_prov_a.present.store(true);
		struct xrt_system_roles back = wait_for_left(f.a_left_index);
		CHECK(back.left == f.a_left_index);
		// ...and when it leaves again the hands go to -1, not to qwerty.
		g_prov_a.present.store(false);
		struct xrt_system_roles gone = wait_for_left(-1);
		CHECK(gone.left == -1);
	}

	SECTION("`input emulated on`: re-admitted at once, the floor holds the hands again")
	{
		CHECK(t_input_arbiter_set_floor_withdrawn(false) == 1);
		struct xrt_system_roles readmitted = read_roles();
		CHECK(readmitted.left == f.q_left_index);
		CHECK(readmitted.generation_id == withdrawn.generation_id + 1);
	}

	SECTION("a re-rank of the providers never touches the floor's standing")
	{
		const struct t_input_arbiter_rank ranks[] = {{"prov-a", 0, true}};
		CHECK(t_input_arbiter_rerank(ranks, 1) == 1);
		CHECK(read_roles().left == -1); // still withdrawn, provider still absent
	}
}

TEST_CASE("input arbiter floor: the gate's live half is a no-op when not installed")
{
	t_input_arbiter_reset();
	CHECK(t_input_arbiter_set_floor_withdrawn(true) == 0);
	t_input_arbiter_reset();
}
