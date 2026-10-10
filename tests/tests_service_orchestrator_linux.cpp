// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux workspace orchestrator: discovery through
 *         XRT_WORKSPACE_CONTROLLER_PATH, spawn on launch request / hotkey,
 *         ENABLE-mode crash respawn, AUTO stays off after an exit, the
 *         launch-hotkey arming rules and the spawned environment.
 *
 * Compiles service_orchestrator.c straight in; the hotkey backend
 * (service_hotkey_linux.c, which talks to the session bus / X server) is
 * replaced by the recording stubs below, so nothing on the desktop is touched.
 * The "controller" is a tiny shell script.
 */

#include "catch_amalgamated.hpp"

#include "service_config.h"
#include "service_hotkey_linux.h"
#include "service_orchestrator.h"
#include "service_workspace_registry.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>


/*
 *
 * Hotkey backend stubs (recorded).
 *
 */

namespace {

std::mutex g_hk_lock;
service_hotkey_linux_activate_fn g_activate = nullptr;
std::vector<std::string> g_armed;
std::vector<bool> g_suspends;
int g_starts = 0;
int g_stops = 0;

std::string
last_armed()
{
	std::lock_guard<std::mutex> l(g_hk_lock);
	return g_armed.empty() ? std::string("<never>") : g_armed.back();
}

} // namespace

extern "C" void
service_hotkey_linux_start(service_hotkey_linux_activate_fn on_activate)
{
	std::lock_guard<std::mutex> l(g_hk_lock);
	g_activate = on_activate;
	g_starts++;
}

extern "C" void
service_hotkey_linux_arm(const char *combo)
{
	std::lock_guard<std::mutex> l(g_hk_lock);
	g_armed.push_back(combo != nullptr ? combo : "");
}

extern "C" void
service_hotkey_linux_suspend(bool suspend)
{
	std::lock_guard<std::mutex> l(g_hk_lock);
	g_suspends.push_back(suspend);
}

extern "C" void
service_hotkey_linux_stop(void)
{
	std::lock_guard<std::mutex> l(g_hk_lock);
	g_activate = nullptr;
	g_stops++;
}

extern "C" bool
service_hotkey_linux_unit_from_cgroup(const char *, char *out, unsigned out_size)
{
	if (out_size > 0) {
		out[0] = '\0';
	}
	return false;
}


/*
 *
 * Fixture.
 *
 */

namespace {

struct fixture
{
	std::string dir;
	std::string log;
	std::string script;

	fixture()
	{
		char tmpl[] = "/tmp/dxr_orch_XXXXXX";
		const char *p = mkdtemp(tmpl);
		REQUIRE(p != nullptr);
		dir = p;
		log = dir + "/spawns.log";
		script = dir + "/fake-controller";
		REQUIRE(mkdir((dir + "/manifests").c_str(), 0755) == 0);
		REQUIRE(mkdir((dir + "/config").c_str(), 0755) == 0);
		REQUIRE(mkdir((dir + "/data").c_str(), 0755) == 0);

		// One line per launch: pid, argv, and the two env vars the contract
		// promises. --workspace-action children log and exit; the managed
		// controller stays up until killed.
		std::ofstream s(script);
		s << "#!/bin/sh\n"
		  << "echo \"$$ $* session=${DISPLAYXR_WORKSPACE_SESSION:-} hotkey=${DISPLAYXR_WORKSPACE_HOTKEY:-}\" >> '"
		  << log << "'\n"
		  << "case \"$1\" in --workspace-action) exit 0 ;; esac\n"
		  << "exec sleep 60\n";
		s.close();
		REQUIRE(chmod(script.c_str(), 0755) == 0);

		std::ofstream m(dir + "/manifests/testctl.json");
		m << "{\"file_format_version\":\"1.0\",\"controller\":{\"id\":\"testctl\",\"binary\":\"" << script
		  << "\",\"display_name\":\"Test Controller\",\"supports_file_dialog\":true}}";
		m.close();

		setenv("XRT_WORKSPACE_CONTROLLER_PATH", (dir + "/manifests").c_str(), 1);
		setenv("XDG_CONFIG_HOME", (dir + "/config").c_str(), 1);
		setenv("XDG_DATA_HOME", (dir + "/data").c_str(), 1);
		unsetenv("DISPLAYXR_WORKSPACE_HOTKEY");
		std::lock_guard<std::mutex> l(g_hk_lock);
		g_armed.clear();
		g_suspends.clear();
	}

	~fixture()
	{
		service_orchestrator_shutdown();
		unsetenv("XRT_WORKSPACE_CONTROLLER_PATH");
		std::string cmd = "rm -rf '" + dir + "'";
		(void)std::system(cmd.c_str());
	}

	std::vector<std::string>
	lines() const
	{
		std::vector<std::string> out;
		std::ifstream f(log);
		std::string line;
		while (std::getline(f, line)) {
			out.push_back(line);
		}
		return out;
	}

	//! Wait (bounded) until the log has @p n lines.
	bool
	wait_lines(size_t n, int timeout_ms = 5000) const
	{
		for (int t = 0; t < timeout_ms; t += 20) {
			if (lines().size() >= n) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return lines().size() >= n;
	}
};

service_config
config(enum service_child_mode mode)
{
	service_config cfg;
	service_config_set_defaults(&cfg);
	cfg.workspace = mode;
	// Prefer our manifest over anything a dev box has installed system-wide.
	std::snprintf(cfg.workspace_binary, sizeof(cfg.workspace_binary), "testctl");
	return cfg;
}

bool
wait_pid(bool nonzero, int timeout_ms = 5000)
{
	for (int t = 0; t < timeout_ms; t += 20) {
		if ((service_orchestrator_get_workspace_pid() != 0) == nonzero) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return false;
}

} // namespace


/*
 *
 * Tests.
 *
 */

TEST_CASE("AUTO: discovered, not spawned at init, launch request spawns once", "[service][orchestrator][linux]")
{
	fixture f;
	service_config cfg = config(SERVICE_CHILD_AUTO);
	REQUIRE(service_orchestrator_init(&cfg));

	REQUIRE(service_orchestrator_is_workspace_available());
	REQUIRE(service_orchestrator_get_workspace_entry() != nullptr);
	CHECK(std::string(service_orchestrator_get_workspace_entry()->id) == "testctl");
	CHECK(service_orchestrator_get_workspace_supports_file_dialog());
	CHECK(service_orchestrator_get_workspace_mode() == SERVICE_CHILD_AUTO);
	CHECK(std::string(service_orchestrator_get_launch_hotkey()) == "Ctrl+Space");

	// The hotkey is armed with the default combo, nothing spawned yet.
	CHECK(last_armed() == "Ctrl+Space");
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	CHECK(f.lines().empty());
	CHECK(service_orchestrator_get_workspace_pid() == 0);

	// Refusals have no side effect.
	CHECK(service_orchestrator_request_launch("someone-else") == SERVICE_LAUNCH_NOT_ACTIVE);
	CHECK(service_orchestrator_request_launch(nullptr) == SERVICE_LAUNCH_NOT_ACTIVE);

	CHECK(service_orchestrator_request_launch("testctl") == SERVICE_LAUNCH_STARTED);
	REQUIRE(f.wait_lines(1));
	REQUIRE(wait_pid(true));
	const auto l = f.lines();
	const long pid = std::strtol(l[0].c_str(), nullptr, 10);
	CHECK(pid == (long)service_orchestrator_get_workspace_pid());
	CHECK(l[0].find(" --service-managed ") != std::string::npos);
	CHECK(l[0].find("session=1") != std::string::npos);
	CHECK(l[0].find("hotkey=Ctrl+Space") != std::string::npos);

	CHECK(service_orchestrator_request_launch("testctl") == SERVICE_LAUNCH_ALREADY_RUNNING);

	// A crash in AUTO returns to the off state: no respawn.
	REQUIRE(kill((pid_t)pid, SIGKILL) == 0);
	REQUIRE(wait_pid(false));
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	CHECK(f.lines().size() == 1);
	CHECK(service_orchestrator_get_workspace_pid() == 0);

	// ... and the next request brings it back.
	CHECK(service_orchestrator_request_launch("testctl") == SERVICE_LAUNCH_STARTED);
	REQUIRE(f.wait_lines(2));
}

TEST_CASE("hotkey press spawns the controller; a press while running is a no-op", "[service][orchestrator][linux]")
{
	fixture f;
	service_config cfg = config(SERVICE_CHILD_AUTO);
	REQUIRE(service_orchestrator_init(&cfg));

	service_hotkey_linux_activate_fn press = nullptr;
	{
		std::lock_guard<std::mutex> l(g_hk_lock);
		press = g_activate;
	}
	REQUIRE(press != nullptr);

	press();
	REQUIRE(f.wait_lines(1));
	REQUIRE(wait_pid(true));
	press(); // controller running: it handles the chord itself
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	CHECK(f.lines().size() == 1);

	// Suspend is forwarded to the backend.
	CHECK(service_orchestrator_request_hotkey_suspend(true));
	CHECK(service_orchestrator_request_hotkey_suspend(false));
	{
		std::lock_guard<std::mutex> l(g_hk_lock);
		REQUIRE(g_suspends.size() == 2);
		CHECK(g_suspends[0]);
		CHECK_FALSE(g_suspends[1]);
	}

	// Shutdown stops the hotkey first and terminates the controller.
	const pid_t pid = (pid_t)service_orchestrator_get_workspace_pid();
	service_orchestrator_shutdown();
	{
		std::lock_guard<std::mutex> l(g_hk_lock);
		CHECK(g_activate == nullptr);
	}
	bool gone = false;
	for (int t = 0; t < 3000 && !gone; t += 20) {
		gone = kill(pid, 0) != 0;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	CHECK(gone);
}

TEST_CASE("ENABLE: spawned at init and respawned after a crash", "[service][orchestrator][linux]")
{
	fixture f;
	service_config cfg = config(SERVICE_CHILD_ENABLE);
	REQUIRE(service_orchestrator_init(&cfg));
	REQUIRE(f.wait_lines(1));
	REQUIRE(wait_pid(true));
	const long first = std::strtol(f.lines()[0].c_str(), nullptr, 10);

	REQUIRE(kill((pid_t)first, SIGKILL) == 0);
	// 1 s settle before the respawn (crash-loop guard).
	REQUIRE(f.wait_lines(2, 6000));
	const long second = std::strtol(f.lines()[1].c_str(), nullptr, 10);
	CHECK(second != first);
	REQUIRE(wait_pid(true));
	CHECK((long)service_orchestrator_get_workspace_pid() == second);
}

TEST_CASE("disabled / no hotkey / no controller disarm the grab", "[service][orchestrator][linux]")
{
	SECTION("disabled controller")
	{
		fixture f;
		service_config cfg = config(SERVICE_CHILD_AUTO);
		REQUIRE(service_config_set_controller_mode(&cfg, "testctl", SERVICE_CHILD_DISABLE, true));
		REQUIRE(service_orchestrator_init(&cfg));
		CHECK(last_armed() == "");
		CHECK(service_orchestrator_request_launch("testctl") == SERVICE_LAUNCH_DISABLED);
	}
	SECTION("--no-hotkey, then a custom combo through a live reload")
	{
		fixture f;
		service_config cfg = config(SERVICE_CHILD_AUTO);
		REQUIRE(service_config_set_controller_hotkey(&cfg, "testctl", nullptr));
		REQUIRE(service_orchestrator_init(&cfg));
		CHECK(last_armed() == "");

		// What `displayxr-cli workspace set testctl --hotkey ...` writes, then
		// system_reload_service_config applies.
		service_config next = config(SERVICE_CHILD_AUTO);
		REQUIRE(service_config_set_controller_hotkey(&next, "testctl", "Ctrl+Alt+W"));
		REQUIRE(service_config_save(&next));
		CHECK(service_orchestrator_request_config_reload());
		CHECK(last_armed() == "Ctrl+Alt+W");
		CHECK(std::string(service_orchestrator_get_launch_hotkey()) == "Ctrl+Alt+W");
	}
	SECTION("no controller registered")
	{
		fixture f;
		std::remove((f.dir + "/manifests/testctl.json").c_str());
		service_config cfg = config(SERVICE_CHILD_AUTO);
		cfg.workspace_binary[0] = '\0';
		setenv("XRT_WORKSPACE_CONTROLLER_PATH", (f.dir + "/manifests").c_str(), 1);
		REQUIRE(service_orchestrator_init(&cfg));
		if (!service_orchestrator_is_workspace_available()) {
			CHECK(last_armed() == "");
			CHECK(service_orchestrator_request_launch("testctl") == SERVICE_LAUNCH_NO_CONTROLLER);
		} else {
			// A dev box with a controller installed system-wide: the rule
			// under test does not apply.
			SUCCEED("a system-wide controller is installed; skipped");
		}
	}
}

TEST_CASE("controller actions are fire-and-forget and reaped", "[service][orchestrator][linux]")
{
	fixture f;
	service_config cfg = config(SERVICE_CHILD_AUTO);
	REQUIRE(service_orchestrator_init(&cfg));
	service_orchestrator_dispatch_controller_action("toggle");
	REQUIRE(f.wait_lines(1));
	CHECK(f.lines()[0].find("--workspace-action toggle") != std::string::npos);
	CHECK(service_orchestrator_get_workspace_pid() == 0); // never the managed controller

	// The reaper thread collects it: no zombie child left behind.
	const long pid = std::strtol(f.lines()[0].c_str(), nullptr, 10);
	bool reaped = false;
	for (int t = 0; t < 3000 && !reaped; t += 20) {
		std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
		reaped = !st.good();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	CHECK(reaped);
}
