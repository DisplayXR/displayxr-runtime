// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MCP capability marker on POSIX: the per-platform paths (desktop Linux:
 *         XDG config then /etc) and the "first existing marker decides" rule.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_config_os.h"
#include "util/u_mcp_capability.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef XRT_OS_WINDOWS
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef XRT_OS_LINUX_DESKTOP

namespace {

struct env_guard
{
	std::string name;
	std::string old;
	bool had;

	explicit env_guard(const char *n) : name(n)
	{
		const char *v = std::getenv(n);
		had = v != nullptr;
		old = had ? v : "";
	}
	~env_guard()
	{
		if (had) {
			setenv(name.c_str(), old.c_str(), 1);
		} else {
			unsetenv(name.c_str());
		}
	}
};

void
write_marker(const std::string &dir, const char *byte)
{
	REQUIRE(std::system(("mkdir -p '" + dir + "'").c_str()) == 0);
	FILE *f = std::fopen((dir + "/Enabled").c_str(), "w");
	REQUIRE(f != nullptr);
	std::fputs(byte, f);
	std::fclose(f);
}

} // namespace

TEST_CASE("Linux marker paths: XDG_CONFIG_HOME, else ~/.config, then /etc", "[aux][mcp]")
{
	env_guard g1("XDG_CONFIG_HOME");
	env_guard g2("HOME");
	char p[U_MCP_CAPABILITY_MAX_PATHS][U_MCP_CAPABILITY_PATH_MAX];

	setenv("XDG_CONFIG_HOME", "/xdg/cfg", 1);
	setenv("HOME", "/home/u", 1);
	REQUIRE(u_mcp_capability_marker_paths(p) == 2);
	CHECK(std::string(p[0]) == "/xdg/cfg/displayxr/capabilities/mcp/Enabled");
	CHECK(std::string(p[1]) == "/etc/displayxr/capabilities/mcp/Enabled");

	unsetenv("XDG_CONFIG_HOME");
	REQUIRE(u_mcp_capability_marker_paths(p) == 2);
	CHECK(std::string(p[0]) == "/home/u/.config/displayxr/capabilities/mcp/Enabled");

	// A relative XDG_CONFIG_HOME is invalid per the XDG spec: ignored.
	setenv("XDG_CONFIG_HOME", "relative", 1);
	REQUIRE(u_mcp_capability_marker_paths(p) == 2);
	CHECK(std::string(p[0]) == "/home/u/.config/displayxr/capabilities/mcp/Enabled");

	// No home at all: only the system marker.
	unsetenv("XDG_CONFIG_HOME");
	unsetenv("HOME");
	REQUIRE(u_mcp_capability_marker_paths(p) == 1);
	CHECK(std::string(p[0]) == "/etc/displayxr/capabilities/mcp/Enabled");
	for (int i = 0; i < 1; i++) {
		CHECK(std::string(p[i]).find("Library") == std::string::npos);
	}
}

TEST_CASE("Linux marker read: first byte '1', per-user marker wins", "[aux][mcp]")
{
	env_guard g1("XDG_CONFIG_HOME");
	char tmpl[] = "/tmp/dxr_mcp_cap_XXXXXX";
	REQUIRE(mkdtemp(tmpl) != nullptr);
	const std::string root = tmpl;
	setenv("XDG_CONFIG_HOME", root.c_str(), 1);
	const std::string dir = root + "/displayxr/capabilities/mcp";

	CHECK(u_mcp_capability_read_marker((dir + "/Enabled").c_str()) == -1);
	CHECK(u_mcp_capability_read_marker("") == -1);

	write_marker(dir, "1");
	CHECK(u_mcp_capability_read_marker((dir + "/Enabled").c_str()) == 1);
	CHECK(u_mcp_capability_marker_enabled());

	write_marker(dir, "1\n");
	CHECK(u_mcp_capability_marker_enabled());

	// A per-user "0" disables regardless of the system marker.
	write_marker(dir, "0");
	CHECK(u_mcp_capability_read_marker((dir + "/Enabled").c_str()) == 0);
	CHECK_FALSE(u_mcp_capability_marker_enabled());

	write_marker(dir, "");
	CHECK(u_mcp_capability_read_marker((dir + "/Enabled").c_str()) == 0);

	(void)std::system(("rm -rf '" + root + "'").c_str());
}

#else

TEST_CASE("non-Linux marker paths are unchanged", "[aux][mcp]")
{
	char p[U_MCP_CAPABILITY_MAX_PATHS][U_MCP_CAPABILITY_PATH_MAX];
	const int n = u_mcp_capability_marker_paths(p);
#ifdef XRT_OS_MACOS
	REQUIRE(n == 1);
	CHECK(std::string(p[0]) == "/Library/Application Support/DisplayXR/Capabilities/MCP/Enabled");
#else
	CHECK(n == 0);
#endif
}

#endif
