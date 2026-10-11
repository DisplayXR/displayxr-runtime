// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  u_file_copy_unix_socket_path: the bounds check that keeps a long
 *         XDG_RUNTIME_DIR from overflowing sockaddr_un::sun_path (the service
 *         aborted with a fortify "buffer overflow detected").
 */

#include "catch_amalgamated.hpp"

#include "util/u_file.h"

#include <string>

TEST_CASE("u_file_copy_unix_socket_path")
{
	char sun_path[108];

	SECTION("a short path is copied")
	{
		CHECK(u_file_copy_unix_socket_path(sun_path, sizeof(sun_path), "/run/user/1000/displayxr_comp_ipc"));
		CHECK(std::string(sun_path) == "/run/user/1000/displayxr_comp_ipc");
	}

	SECTION("107 chars + terminator fit exactly")
	{
		std::string p(107, 'a');
		CHECK(u_file_copy_unix_socket_path(sun_path, sizeof(sun_path), p.c_str()));
		CHECK(std::string(sun_path) == p);
	}

	SECTION("108 chars do not fit and leave an empty string")
	{
		std::string p(108, 'a');
		CHECK_FALSE(u_file_copy_unix_socket_path(sun_path, sizeof(sun_path), p.c_str()));
		CHECK(sun_path[0] == '\0');
	}

	SECTION("NULL and zero-size are refused")
	{
		CHECK_FALSE(u_file_copy_unix_socket_path(sun_path, sizeof(sun_path), nullptr));
		CHECK_FALSE(u_file_copy_unix_socket_path(sun_path, 0, "/x"));
		CHECK_FALSE(u_file_copy_unix_socket_path(nullptr, 108, "/x"));
	}
}
