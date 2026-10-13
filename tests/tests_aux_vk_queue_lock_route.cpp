// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #886 — VK_LAYER_DXR_queue_lock activation-route policy.
 *
 * The field log has to say HOW the queue-lock layer got into an app's chain
 * (`#886: VK_LAYER_DXR_queue_lock route=…`), because for a vulkan_enable1 app
 * the runtime cannot put it there itself. The layer reports what it saw
 * (named / implicit / loader-env); the runtime adds what only it knows (did
 * it inject the name?). That combination, and the log-only runtime#2039
 * guard, are pure — pinned here with no GPU. The loader side is covered by
 * tests_vk_queue_lock_activation (needs a Vulkan device, skips without one).
 */

#include "catch_amalgamated.hpp"

#include "util/u_vk_queue_lock.h"

#include <cstring>

TEST_CASE("no marker means absent, whatever else is claimed", "[vk_queue_lock]")
{
	CHECK(u_vk_ql_route_resolve(false, U_VK_QL_ACT_UNKNOWN, false) == U_VK_QL_ROUTE_ABSENT);
	CHECK(u_vk_ql_route_resolve(false, U_VK_QL_ACT_NAMED, true) == U_VK_QL_ROUTE_ABSENT);
	CHECK(u_vk_ql_route_resolve(false, U_VK_QL_ACT_IMPLICIT, false) == U_VK_QL_ROUTE_ABSENT);
}

TEST_CASE("named splits on who named it", "[vk_queue_lock]")
{
	// enable2: the runtime added the name at xrCreateVulkanInstanceKHR.
	CHECK(u_vk_ql_route_resolve(true, U_VK_QL_ACT_NAMED, true) == U_VK_QL_ROUTE_INJECTED);
	// enable1 (or an enable2 app that named it before the runtime could).
	CHECK(u_vk_ql_route_resolve(true, U_VK_QL_ACT_NAMED, false) == U_VK_QL_ROUTE_APP_ENABLED);
}

TEST_CASE("not named: implicit manifest vs loader env", "[vk_queue_lock]")
{
	CHECK(u_vk_ql_route_resolve(true, U_VK_QL_ACT_IMPLICIT, false) == U_VK_QL_ROUTE_IMPLICIT);
	CHECK(u_vk_ql_route_resolve(true, U_VK_QL_ACT_LOADER_ENV, false) == U_VK_QL_ROUTE_LOADER_ENV);
	// The layer's own answer wins over a stale injection flag: if the layer
	// was not named, the runtime's name did not reach this instance.
	CHECK(u_vk_ql_route_resolve(true, U_VK_QL_ACT_IMPLICIT, true) == U_VK_QL_ROUTE_IMPLICIT);
}

TEST_CASE("a layer older than the activation query", "[vk_queue_lock]")
{
	CHECK(u_vk_ql_route_resolve(true, U_VK_QL_ACT_UNKNOWN, true) == U_VK_QL_ROUTE_INJECTED);
	CHECK(u_vk_ql_route_resolve(true, U_VK_QL_ACT_UNKNOWN, false) == U_VK_QL_ROUTE_UNKNOWN);
	CHECK(u_vk_ql_route_resolve(true, 99u, false) == U_VK_QL_ROUTE_UNKNOWN);
}

TEST_CASE("route tokens are the grep-able log vocabulary", "[vk_queue_lock]")
{
	CHECK(std::strcmp(u_vk_ql_route_str(U_VK_QL_ROUTE_ABSENT), "absent") == 0);
	CHECK(std::strcmp(u_vk_ql_route_str(U_VK_QL_ROUTE_INJECTED), "injected") == 0);
	CHECK(std::strcmp(u_vk_ql_route_str(U_VK_QL_ROUTE_APP_ENABLED), "app-enabled") == 0);
	CHECK(std::strcmp(u_vk_ql_route_str(U_VK_QL_ROUTE_IMPLICIT), "implicit") == 0);
	CHECK(std::strcmp(u_vk_ql_route_str(U_VK_QL_ROUTE_LOADER_ENV), "loader-env") == 0);
	CHECK(std::strcmp(u_vk_ql_route_str(U_VK_QL_ROUTE_UNKNOWN), "unknown") == 0);
}

TEST_CASE("wire values are shared with the layer", "[vk_queue_lock]")
{
	// queue_lock_layer.c keeps its own copies (it links nothing of ours).
	CHECK(U_VK_QL_ACT_NAMED == 1);
	CHECK(U_VK_QL_ACT_IMPLICIT == 2);
	CHECK(U_VK_QL_ACT_LOADER_ENV == 3);
	CHECK(std::strcmp(U_VK_QL_ACTIVATION_FN, "vkGetQueueLockActivationDXR") == 0);
	CHECK(std::strcmp(U_VK_QL_MARKER_FN, "vkGetQueueLockMarkerDXR") == 0);
	CHECK(std::strcmp(U_VK_QL_IMPLICIT_ENABLE_ENV, "DXR_VK_QUEUE_LOCK") == 0);
}

TEST_CASE("runtime#2039 guard: log only the enable1 + live + shared + pipelined case", "[vk_queue_lock]")
{
	// The one case it is for: triple-buffered enable1 app on the layer tier.
	CHECK(u_vk_ql_should_note_serialisation(true, true, true, 2));
	// enable2 (Unity): the runtime asked for it — measured no cost, no line.
	CHECK_FALSE(u_vk_ql_should_note_serialisation(false, true, true, 2));
	// No layer, or the repaint not on the shared queue: nothing serialised for it.
	CHECK_FALSE(u_vk_ql_should_note_serialisation(true, false, true, 2));
	CHECK_FALSE(u_vk_ql_should_note_serialisation(true, true, false, 2));
	// One frame in flight (double-buffered / static): nothing can queue up.
	CHECK_FALSE(u_vk_ql_should_note_serialisation(true, true, true, 1));
	CHECK_FALSE(u_vk_ql_should_note_serialisation(true, true, true, 0));
}
