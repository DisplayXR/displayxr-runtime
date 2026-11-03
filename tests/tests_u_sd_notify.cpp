// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  u_sd_notify (#2515): the NOTIFY_SOCKET / WATCHDOG_* parsers, and on
 *         desktop Linux one real round trip over an abstract datagram socket.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_config_os.h"
#include "util/u_sd_notify.h"

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef XRT_OS_LINUX_DESKTOP
#include <stddef.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

TEST_CASE("u_sd_notify_parse_socket")
{
	char path[108];

	SECTION("a filesystem path keeps its terminator")
	{
		size_t n = u_sd_notify_parse_socket("/run/user/1000/systemd/notify", path, sizeof(path));
		CHECK(n == strlen("/run/user/1000/systemd/notify") + 1);
		CHECK(std::string(path) == "/run/user/1000/systemd/notify");
	}

	SECTION("an abstract name gets a leading NUL and no terminator")
	{
		size_t n = u_sd_notify_parse_socket("@notify-123", path, sizeof(path));
		CHECK(n == strlen("@notify-123"));
		CHECK(path[0] == '\0');
		CHECK(std::string(path + 1, n - 1) == "notify-123");
	}

	SECTION("108 path bytes + terminator do not fit; 107 do")
	{
		std::string p = "/" + std::string(106, 'a');
		CHECK(u_sd_notify_parse_socket(p.c_str(), path, sizeof(path)) == 108);
		p += "a";
		CHECK(u_sd_notify_parse_socket(p.c_str(), path, sizeof(path)) == 0);
	}

	SECTION("an abstract name may use all 108 bytes")
	{
		std::string p = "@" + std::string(107, 'b');
		CHECK(u_sd_notify_parse_socket(p.c_str(), path, sizeof(path)) == 108);
		p += "b";
		CHECK(u_sd_notify_parse_socket(p.c_str(), path, sizeof(path)) == 0);
	}

	SECTION("refused: NULL, empty, bare '@' or '/', relative")
	{
		CHECK(u_sd_notify_parse_socket(nullptr, path, sizeof(path)) == 0);
		CHECK(u_sd_notify_parse_socket("", path, sizeof(path)) == 0);
		CHECK(u_sd_notify_parse_socket("@", path, sizeof(path)) == 0);
		CHECK(u_sd_notify_parse_socket("/", path, sizeof(path)) == 0);
		CHECK(u_sd_notify_parse_socket("run/notify", path, sizeof(path)) == 0);
		CHECK(u_sd_notify_parse_socket("/x", path, 0) == 0);
	}
}

TEST_CASE("u_sd_notify_parse_watchdog_usec")
{
	CHECK(u_sd_notify_parse_watchdog_usec("30000000", nullptr, 42) == 30000000u);
	CHECK(u_sd_notify_parse_watchdog_usec("30000000", "", 42) == 30000000u);
	CHECK(u_sd_notify_parse_watchdog_usec("30000000", "42", 42) == 30000000u);
	// WATCHDOG_PID names another process: the watchdog is not ours.
	CHECK(u_sd_notify_parse_watchdog_usec("30000000", "43", 42) == 0);
	CHECK(u_sd_notify_parse_watchdog_usec("30000000", "4x", 42) == 0);
	CHECK(u_sd_notify_parse_watchdog_usec(nullptr, nullptr, 42) == 0);
	CHECK(u_sd_notify_parse_watchdog_usec("", nullptr, 42) == 0);
	CHECK(u_sd_notify_parse_watchdog_usec("0", nullptr, 42) == 0);
	CHECK(u_sd_notify_parse_watchdog_usec("-5", nullptr, 42) == 0);
	CHECK(u_sd_notify_parse_watchdog_usec("12abc", nullptr, 42) == 0);
	CHECK(u_sd_notify_parse_watchdog_usec("99999999999999999999999", nullptr, 42) == 0);
}

#ifdef XRT_OS_LINUX_DESKTOP

static std::string
recv_one(int fd)
{
	char buf[256];
	ssize_t n = recv(fd, buf, sizeof(buf), 0);
	return n > 0 ? std::string(buf, (size_t)n) : std::string();
}

TEST_CASE("u_sd_notify round trip over an abstract socket")
{
	// The receiver plays systemd.
	int rx = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	REQUIRE(rx >= 0);
	struct timeval tv = {0, 200 * 1000};
	setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	std::string name = "@dxr-sd-notify-test-" + std::to_string((long)getpid());
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path + 1, name.c_str() + 1, name.size() - 1);
	REQUIRE(bind(rx, (struct sockaddr *)&addr, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name.size())) ==
	        0);

	setenv("NOTIFY_SOCKET", name.c_str(), 1);
	setenv("WATCHDOG_USEC", "2000000", 1); // 2 s → ping every 1 s
	setenv("WATCHDOG_PID", std::to_string((long)getpid()).c_str(), 1);
	u_sd_notify_init();

	// Children must not inherit the manager's variables.
	CHECK(getenv("NOTIFY_SOCKET") == nullptr);
	CHECK(getenv("WATCHDOG_USEC") == nullptr);
	CHECK(getenv("WATCHDOG_PID") == nullptr);
	CHECK(u_sd_notify_watchdog_enabled());

	CHECK(u_sd_notify_send("STOPPING=1"));
	CHECK(recv_one(rx) == "STOPPING=1");

	const uint64_t s = 1000000000ull;
	u_sd_notify_watchdog_tick(10 * s); // first tick always pings
	CHECK(recv_one(rx) == "WATCHDOG=1");
	u_sd_notify_watchdog_tick(10 * s + s / 2); // < half the interval: no ping
	CHECK(recv_one(rx).empty());
	u_sd_notify_watchdog_tick(11 * s); // half the interval: ping
	CHECK(recv_one(rx) == "WATCHDOG=1");

	close(rx);
}

#endif // XRT_OS_LINUX_DESKTOP
