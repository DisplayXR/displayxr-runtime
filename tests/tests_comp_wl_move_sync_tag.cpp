// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin the move-sync frame tag encoding (#1748).
 *
 * The runtime places a 1x1 synchronised subsurface at the woven LOGICAL
 * content position modulo 256 (comp_vk_native_wl_move_sync_encode); the GNOME
 * extension decodes it as the tag surface's offset from the main surface,
 * modulo 256 (MoveSyncChoice.decode in lib.js, unit-tested by
 * scripts/test_gnome_extension_move_sync.js against a transcription of this
 * function). The two must agree for every position a window can have,
 * including negative stage coordinates on a multi-monitor layout.
 */

#include "catch_amalgamated.hpp"

#include "vk_native/comp_vk_native_wl_move_sync.h"

TEST_CASE("move-sync tag: the modulus the extension expects", "[wl_move_sync]")
{
	CHECK(COMP_VK_NATIVE_WL_MOVE_SYNC_TAG_MOD == 256);
}

TEST_CASE("move-sync tag: encode is v mod 256, never negative", "[wl_move_sync]")
{
	CHECK(comp_vk_native_wl_move_sync_encode(0) == 0);
	CHECK(comp_vk_native_wl_move_sync_encode(1) == 1);
	CHECK(comp_vk_native_wl_move_sync_encode(255) == 255);
	CHECK(comp_vk_native_wl_move_sync_encode(256) == 0);
	CHECK(comp_vk_native_wl_move_sync_encode(257) == 1);
	CHECK(comp_vk_native_wl_move_sync_encode(1920 + 3) == (1923 % 256));
	// A window left of / above the stage origin (a monitor at negative
	// logical coordinates).
	CHECK(comp_vk_native_wl_move_sync_encode(-1) == 255);
	CHECK(comp_vk_native_wl_move_sync_encode(-256) == 0);
	CHECK(comp_vk_native_wl_move_sync_encode(-257) == 255);
	CHECK(comp_vk_native_wl_move_sync_encode(-3000) == 72); // -3000 + 12 * 256
}

TEST_CASE("move-sync tag: every value is a valid subsurface offset", "[wl_move_sync]")
{
	for (int32_t v = -4096; v <= 4096; v++) {
		const int32_t e = comp_vk_native_wl_move_sync_encode(v);
		REQUIRE(e >= 0);
		REQUIRE(e < COMP_VK_NATIVE_WL_MOVE_SYNC_TAG_MOD);
		// Positions 256 apart share a tag; the extension separates them by
		// time (it resolves against the window's last second of positions).
		REQUIRE(e == comp_vk_native_wl_move_sync_encode(v + COMP_VK_NATIVE_WL_MOVE_SYNC_TAG_MOD));
		// Neighbouring positions never do.
		REQUIRE(e != comp_vk_native_wl_move_sync_encode(v + 1));
	}
}
