// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  gamescope (SteamOS Game Mode) detection and the 1:1 verdict
 *         (auxiliary/util/u_gamescope.c).
 */

#include "catch_amalgamated.hpp"

#include "util/u_gamescope.h"

TEST_CASE("gamescope: detected from either environment variable")
{
	CHECK(u_gamescope_env_is_gamescope("gamescope", nullptr));
	CHECK(u_gamescope_env_is_gamescope("Gamescope", nullptr));
	CHECK(u_gamescope_env_is_gamescope("KDE:gamescope", nullptr));
	CHECK(u_gamescope_env_is_gamescope(nullptr, "gamescope-0"));
	CHECK(u_gamescope_env_is_gamescope("KDE", "gamescope-0"));

	CHECK_FALSE(u_gamescope_env_is_gamescope(nullptr, nullptr));
	CHECK_FALSE(u_gamescope_env_is_gamescope("", ""));
	CHECK_FALSE(u_gamescope_env_is_gamescope("KDE", nullptr));          // SteamOS Desktop Mode
	CHECK_FALSE(u_gamescope_env_is_gamescope("ubuntu:GNOME", nullptr)); // Ubuntu
	CHECK_FALSE(u_gamescope_env_is_gamescope("gamescopex", nullptr));   // a whole entry, not a prefix
	CHECK_FALSE(u_gamescope_env_is_gamescope("notgamescope:KDE", nullptr));
}

TEST_CASE("gamescope 1:1: the game resolution must be the panel's native size")
{
	// Steam Machine + a 3840x2160 3D panel, the game at Steam's default 1280x800.
	CHECK(u_gamescope_check_1to1(3840, 2160, 1280, 800, 1280, 800) == U_GAMESCOPE_1TO1_GAME_RES_SCALED);
	// "Game Resolution: Native".
	CHECK(u_gamescope_check_1to1(3840, 2160, 3840, 2160, 3840, 2160) == U_GAMESCOPE_1TO1_YES);
}

TEST_CASE("gamescope 1:1: a window smaller than the root is scaled up to it")
{
	CHECK(u_gamescope_check_1to1(3840, 2160, 3840, 2160, 1920, 1080) == U_GAMESCOPE_1TO1_WINDOW_SCALED);
}

TEST_CASE("gamescope 1:1: unknown sizes never degrade")
{
	CHECK(u_gamescope_check_1to1(0, 0, 1280, 800, 1280, 800) == U_GAMESCOPE_1TO1_UNKNOWN);
	CHECK(u_gamescope_check_1to1(3840, 2160, 0, 0, 3840, 2160) == U_GAMESCOPE_1TO1_UNKNOWN);
	// Window not created yet: the root alone decides.
	CHECK(u_gamescope_check_1to1(3840, 2160, 3840, 2160, 0, 0) == U_GAMESCOPE_1TO1_YES);
}

TEST_CASE("gamescope 1:1: every verdict has a name")
{
	for (auto v : {U_GAMESCOPE_1TO1_UNKNOWN, U_GAMESCOPE_1TO1_YES, U_GAMESCOPE_1TO1_GAME_RES_SCALED,
	               U_GAMESCOPE_1TO1_WINDOW_SCALED}) {
		CHECK(u_gamescope_1to1_str(v) != nullptr);
	}
}
