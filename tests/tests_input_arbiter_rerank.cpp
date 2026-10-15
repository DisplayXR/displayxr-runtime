// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Live re-rank tests for @ref target_input_arbiter (ADR-054 N3d, #2051).
 *
 * The provider set is fixed per process, so a live `PreferredPlugin` /
 * `Enabled` change is a RE-RANK of the resident candidates, not a rebuild:
 * `t_input_arbiter_rerank` rewrites priorities, withdraws a disabled provider
 * exactly as presence loss would (its devices stay in `xsysd->xdevs`), and
 * recomputes the roles at once so `generation_id` moves without waiting for
 * a client's `get_roles`.
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
#include <cstdio>
#include <cstring>


namespace {

struct fake_provider
{
	struct xrt_input_plugin_iface iface;
	std::atomic<bool> present{true};
};

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

//! Two hand-only providers (A at ProbeOrder 50, B at 200) over the qwerty floor.
struct fixture
{
	struct xrt_device a_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "a-left");
	struct xrt_device a_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "a-right");
	struct xrt_device b_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "b-left");
	struct xrt_device b_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "b-right");
	struct xrt_device q_left = make_device(XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER, "qwerty-left");
	struct xrt_device q_right = make_device(XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER, "qwerty-right");
	int32_t a_left_index = -1;
	int32_t b_left_index = -1;
	int32_t q_left_index = -1;

	fixture()
	{
		t_input_arbiter_reset();
		g_sys.reset();
		init_provider(g_prov_a, "prov-a");
		init_provider(g_prov_b, "prov-b");
		a_left_index = g_sys.add(&a_left);
		g_sys.add(&a_right);
		b_left_index = g_sys.add(&b_left);
		g_sys.add(&b_right);
		q_left_index = g_sys.add(&q_left);
		g_sys.add(&q_right);
		t_input_arbiter_note_provider_pair(&g_prov_a.iface, instance_of(g_prov_a), 50, &a_left, &a_right);
		t_input_arbiter_note_provider_pair(&g_prov_b.iface, instance_of(g_prov_b), 200, &b_left, &b_right);
		t_input_arbiter_note_qwerty_pair(&q_left, &q_right);
		t_input_arbiter_install(&g_sys.base);
	}

	~fixture()
	{
		t_input_arbiter_reset();
	}
};

} // namespace


TEST_CASE("input arbiter re-rank: a PreferredPlugin flip moves the roles and bumps the generation")
{
	fixture f;
	struct xrt_system_roles before = read_roles();
	REQUIRE(before.left == f.a_left_index);
	REQUIRE(before.generation_id != 0);

	// `input use prov-b`: B to priority 0, A keeps its ProbeOrder.
	const struct t_input_arbiter_rank ranks[] = {
	    {"prov-a", 50, true},
	    {"prov-b", 0, true},
	};
	CHECK(t_input_arbiter_rerank(ranks, 2) == 1);

	struct xrt_system_roles after = read_roles();
	CHECK(after.left == f.b_left_index);
	CHECK(after.generation_id == before.generation_id + 1);

	SECTION("the same policy again is a no-op: nothing moves, no generation bump")
	{
		CHECK(t_input_arbiter_rerank(ranks, 2) == 0);
		CHECK(read_roles().generation_id == after.generation_id);
	}

	SECTION("`input reset`: B back to its ProbeOrder hands the roles back to A")
	{
		const struct t_input_arbiter_rank reset[] = {{"prov-b", 200, true}};
		CHECK(t_input_arbiter_rerank(reset, 1) == 1);
		struct xrt_system_roles back = read_roles();
		CHECK(back.left == f.a_left_index);
		CHECK(back.generation_id == after.generation_id + 1);
	}
}

TEST_CASE("input arbiter re-rank: Enabled = 0 withdraws the provider like presence loss; devices stay")
{
	fixture f;
	struct xrt_system_roles before = read_roles();
	REQUIRE(before.left == f.a_left_index);

	// `input disable prov-a`: withdrawn, still resident, still ranked first.
	const struct t_input_arbiter_rank disable_a[] = {{"prov-a", 50, false}};
	CHECK(t_input_arbiter_rerank(disable_a, 1) == 1);
	struct xrt_system_roles withdrawn = read_roles();
	CHECK(withdrawn.left == f.b_left_index);
	CHECK(withdrawn.generation_id == before.generation_id + 1);
	CHECK(g_sys.base.xdevs[f.a_left_index] == &f.a_left); // the device never left the system

	bool installed = false;
	bool provider_holds = false;
	t_input_arbiter_get_status(&installed, &provider_holds);
	CHECK(installed);
	CHECK(provider_holds); // B still holds them

	SECTION("withdrawing the last provider too falls to the qwerty floor")
	{
		const struct t_input_arbiter_rank disable_b[] = {{"prov-b", 200, false}};
		CHECK(t_input_arbiter_rerank(disable_b, 1) == 1);
		struct xrt_system_roles floor = read_roles();
		CHECK(floor.left == f.q_left_index);
		CHECK(floor.generation_id == withdrawn.generation_id + 1);
		t_input_arbiter_get_status(nullptr, &provider_holds);
		CHECK_FALSE(provider_holds);
	}

	SECTION("`input enable prov-a` re-admits it at once and it wins again")
	{
		const struct t_input_arbiter_rank enable_a[] = {{"prov-a", 50, true}};
		CHECK(t_input_arbiter_rerank(enable_a, 1) == 1);
		struct xrt_system_roles back = read_roles();
		CHECK(back.left == f.a_left_index);
		CHECK(back.generation_id == withdrawn.generation_id + 1);
	}

	SECTION("an id that is not a resident candidate changes nothing")
	{
		const struct t_input_arbiter_rank unknown[] = {{"prov-not-loaded", 0, true}};
		CHECK(t_input_arbiter_rerank(unknown, 1) == 0);
		CHECK(read_roles().generation_id == withdrawn.generation_id);
	}
}

TEST_CASE("input arbiter re-rank: not installed is a no-op")
{
	t_input_arbiter_reset();
	const struct t_input_arbiter_rank ranks[] = {{"prov-a", 0, true}};
	CHECK(t_input_arbiter_rerank(ranks, 1) == 0);
	t_input_arbiter_reset();
}
