// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Linux launch-hotkey worker: the systemd unit the extension starts on a
 *         cold press (parsed from /proc/self/cgroup), and the worker's request
 *         plumbing. Built WITHOUT the D-Bus / X11 backends, so it never talks to
 *         the session bus or the X server of the box running the tests.
 */

#include "catch_amalgamated.hpp"

#include "service_hotkey_linux.h"

#include <string>

namespace {

std::string
unit(const char *cgroup)
{
	char out[256] = "x";
	if (!service_hotkey_linux_unit_from_cgroup(cgroup, out, sizeof(out))) {
		return out[0] == '\0' ? "<none>" : "<dirty>";
	}
	return out;
}

} // namespace

TEST_CASE("systemd user unit from /proc/self/cgroup", "[service][hotkey][linux]")
{
	// cgroup v2, socket-activated by the user manager.
	CHECK(unit("0::/user.slice/user-1000.slice/user@1000.service/app.slice/displayxr.service\n") ==
	      "displayxr.service");
	// The dev unit from the build tree.
	CHECK(unit("0::/user.slice/user-1000.slice/user@1000.service/app.slice/displayxr-dev.service") ==
	      "displayxr-dev.service");
	// Started from a terminal: a scope, not a unit anything could start.
	CHECK(unit("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
	           "vte-spawn-1b2c.scope\n") == "<none>");
	// A system service (not under a user manager).
	CHECK(unit("0::/system.slice/displayxr.service\n") == "<none>");
	// The user manager itself.
	CHECK(unit("0::/user.slice/user-1000.slice/user@1000.service\n") == "<none>");
	// cgroup v1 hybrid: the name=systemd line carries the path.
	CHECK(unit("12:cpu,cpuacct:/\n1:name=systemd:/user.slice/user-1000.slice/user@1000.service/app.slice/"
	           "displayxr.service\n0::/\n") == "displayxr.service");
	CHECK(unit("") == "<none>");
	CHECK(unit(nullptr) == "<none>");

	char tiny[4];
	CHECK_FALSE(service_hotkey_linux_unit_from_cgroup(
	    "0::/user.slice/user-1000.slice/user@1000.service/app.slice/displayxr.service", tiny, sizeof(tiny)));
}

namespace {
int g_presses = 0;
void
on_press(void)
{
	g_presses++;
}
} // namespace

TEST_CASE("worker without backends: arm / suspend / stop are safe and never activate", "[service][hotkey][linux]")
{
	service_hotkey_linux_start(on_press);
	service_hotkey_linux_start(on_press); // idempotent
	service_hotkey_linux_arm("Ctrl+Space");
	service_hotkey_linux_arm("Ctrl+Space");
	service_hotkey_linux_suspend(true);
	service_hotkey_linux_suspend(false);
	service_hotkey_linux_arm(nullptr);
	service_hotkey_linux_arm("");
	service_hotkey_linux_stop();
	service_hotkey_linux_stop(); // idempotent
	CHECK(g_presses == 0);

	// Restartable (the orchestrator's init after a shutdown in one process).
	service_hotkey_linux_start(on_press);
	service_hotkey_linux_arm("Alt+F1");
	service_hotkey_linux_stop();
	CHECK(g_presses == 0);
}
