// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #960 CONTROLLER-class verification on POSIX: the peer's executable
 *         (symlink-resolved by the kernel, /proc/<pid>/exe) must match a
 *         controller manifest whose `binary` may name it through a symlink.
 */

#include "catch_amalgamated.hpp"

#include "service_client_class.h"

#include "xrt/xrt_instance.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {

struct temp_dir
{
	std::string path;

	temp_dir()
	{
		char tmpl[] = "/tmp/dxr_client_class_XXXXXX";
		const char *p = mkdtemp(tmpl);
		REQUIRE(p != nullptr);
		path = p;
	}

	~temp_dir()
	{
		std::string cmd = "rm -rf '" + path + "'";
		(void)std::system(cmd.c_str());
	}
};

void
write_file(const std::string &path, const std::string &text, mode_t mode = 0644)
{
	FILE *f = std::fopen(path.c_str(), "w");
	REQUIRE(f != nullptr);
	std::fputs(text.c_str(), f);
	std::fclose(f);
	REQUIRE(chmod(path.c_str(), mode) == 0);
}

} // namespace

TEST_CASE("client class path compare: bytes, empties, symlinks", "[service][client_class]")
{
	temp_dir t;
	const std::string real = t.path + "/real-controller";
	const std::string link = t.path + "/controller-link";
	const std::string dangling = t.path + "/dangling";
	write_file(real, "#!/bin/sh\n", 0755);
	REQUIRE(symlink(real.c_str(), link.c_str()) == 0);
	REQUIRE(symlink((t.path + "/nowhere").c_str(), dangling.c_str()) == 0);

	CHECK(service_client_class_path_equal(real.c_str(), real.c_str()));
	CHECK_FALSE(service_client_class_path_equal("", real.c_str()));
	CHECK_FALSE(service_client_class_path_equal(real.c_str(), ""));
	CHECK_FALSE(service_client_class_path_equal(nullptr, real.c_str()));

	// The manifest names a symlink, the kernel reports the target.
	CHECK(service_client_class_path_equal(link.c_str(), real.c_str()));
	CHECK(service_client_class_path_equal(real.c_str(), link.c_str()));

	// A path with redundant components canonicalises too.
	CHECK(service_client_class_path_equal((t.path + "/./real-controller").c_str(), real.c_str()));

	// Unresolvable sides only ever match byte-for-byte.
	CHECK_FALSE(service_client_class_path_equal(dangling.c_str(), real.c_str()));
	CHECK(service_client_class_path_equal(dangling.c_str(), dangling.c_str()));
	CHECK_FALSE(service_client_class_path_equal((real + " (deleted)").c_str(), real.c_str()));

	// POSIX stays case-sensitive.
	CHECK_FALSE(service_client_class_path_equal((t.path + "/REAL-controller").c_str(), real.c_str()));
}

TEST_CASE("CONTROLLER claim verifies against a manifest naming a symlink", "[service][client_class]")
{
	temp_dir t;
	const std::string bin_dir = t.path + "/lib/acme/bin";
	REQUIRE(std::system(("mkdir -p '" + bin_dir + "' '" + t.path + "/bin' '" + t.path + "/manifests'").c_str()) ==
	        0);
	const std::string real = bin_dir + "/acme-shell";
	const std::string link = t.path + "/bin/acme-shell";
	write_file(real, "#!/bin/sh\nexit 0\n", 0755);
	REQUIRE(symlink(real.c_str(), link.c_str()) == 0);

	write_file(t.path + "/manifests/acme.json", std::string("{\"file_format_version\":\"1.0\",\"controller\":{"
	                                                        "\"id\":\"acme\",\"binary\":\"") +
	                                                link + "\",\"display_name\":\"Acme\"}}");
	REQUIRE(setenv("XRT_WORKSPACE_CONTROLLER_PATH", (t.path + "/manifests").c_str(), 1) == 0);

	// What the IPC server hands us: the resolved executable of the peer.
	CHECK(service_client_class_verify(1234, real.c_str(), XRT_CLIENT_CLASS_CONTROLLER));
	// The symlink spelling itself still verifies (byte-equal path).
	CHECK(service_client_class_verify(1234, link.c_str(), XRT_CLIENT_CLASS_CONTROLLER));
	// Anything else does not.
	CHECK_FALSE(service_client_class_verify(1234, "/usr/bin/true", XRT_CLIENT_CLASS_CONTROLLER));
	CHECK_FALSE(service_client_class_verify(1234, "", XRT_CLIENT_CLASS_CONTROLLER));

	unsetenv("XRT_WORKSPACE_CONTROLLER_PATH");
}
