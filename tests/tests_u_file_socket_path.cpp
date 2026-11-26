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

#if defined(__linux__) && !defined(__ANDROID__)
#include <stdlib.h>

namespace {
// Save/restore one environment variable around a test.
struct EnvGuard
{
	std::string name;
	bool had;
	std::string value;
	explicit EnvGuard(const char *n) : name(n)
	{
		const char *v = getenv(n);
		had = v != nullptr;
		value = had ? v : "";
	}
	~EnvGuard()
	{
		if (had) {
			setenv(name.c_str(), value.c_str(), 1);
		} else {
			unsetenv(name.c_str());
		}
	}
};
} // namespace

TEST_CASE("u_file_get_runtime_dir fallbacks (Linux)")
{
	EnvGuard rt("XDG_RUNTIME_DIR"), cache("XDG_CACHE_HOME"), home("HOME");
	char out[256];

	SECTION("XDG_RUNTIME_DIR wins")
	{
		setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
		REQUIRE(u_file_get_runtime_dir(out, sizeof(out)) > 0);
		CHECK(std::string(out) == "/run/user/1000");
	}

	SECTION("no XDG dirs: $HOME/.cache, never a literal tilde")
	{
		unsetenv("XDG_RUNTIME_DIR");
		unsetenv("XDG_CACHE_HOME");
		setenv("HOME", "/home/deck", 1);
		REQUIRE(u_file_get_runtime_dir(out, sizeof(out)) > 0);
		CHECK(std::string(out) == "/home/deck/.cache");
	}

	SECTION("no XDG dirs and no HOME: /tmp")
	{
		unsetenv("XDG_RUNTIME_DIR");
		unsetenv("XDG_CACHE_HOME");
		unsetenv("HOME");
		REQUIRE(u_file_get_runtime_dir(out, sizeof(out)) > 0);
		CHECK(std::string(out) == "/tmp");
	}
}
#endif
