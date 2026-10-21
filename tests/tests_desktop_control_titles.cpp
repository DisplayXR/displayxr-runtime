// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief XR_DXR_desktop_control (ADR-056): the backend hears when the CONTROL grant
 *        begins and ends (u_desktop_backend::grant_changed). On GNOME that is what
 *        lets the runtime see other apps' window titles from the DisplayXR
 *        extension (v18) only while the user's grant lasts.
 *
 * Linked into tests_desktop_control. Same rules as tests_desktop_control.cpp: a
 * fresh DXR_DESKTOP_CONTROL_DIR per case, the in-memory fake backend, nothing on
 * the real desktop.
 */

#include "desktop/u_desktop_control.h"
#include "desktop/u_desktop_fake.h"
#include "desktop/u_desktop_policy.h"

#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

void
titles_set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value != nullptr ? value : "");
#else
	if (value != nullptr) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

//! A fresh, empty DXR_DESKTOP_CONTROL_DIR while alive.
struct GrantDir
{
	std::string path;

	GrantDir()
	{
#ifdef _WIN32
		char base[MAX_PATH];
		GetTempPathA(MAX_PATH, base);
		static int counter = 0;
		path = std::string(base) + "dxr_dc_grant_" + std::to_string(GetCurrentProcessId()) + "_" +
		       std::to_string(++counter);
		CreateDirectoryA(path.c_str(), nullptr);
#else
		const char *t = getenv("TMPDIR");
		std::string tmpl = std::string(t != nullptr && t[0] != '\0' ? t : "/tmp") + "/dxr_dc_grant_XXXXXX";
		std::vector<char> buf(tmpl.begin(), tmpl.end());
		buf.push_back('\0');
		char *p = mkdtemp(buf.data());
		REQUIRE(p != nullptr);
		path = p;
#endif
		titles_set_env(U_DESKTOP_POLICY_DIR_ENV, path.c_str());
		titles_set_env("DXR_DESKTOP_CONTROL", nullptr);
	}

	~GrantDir()
	{
		for (const char *n : {"desktop_control.json", "desktop_control_audit.jsonl",
		                      "desktop_control_audit.jsonl.1"}) {
#ifdef _WIN32
			std::remove((path + "\\" + n).c_str());
#else
			std::remove((path + "/" + n).c_str());
#endif
		}
#ifdef _WIN32
		RemoveDirectoryA(path.c_str());
#else
		rmdir(path.c_str());
#endif
		titles_set_env(U_DESKTOP_POLICY_DIR_ENV, nullptr);
	}
};

//! What the backend was told, in order. One harness at a time (the hook is a plain
//! function pointer, so it finds the log through a global).
struct GrantLog
{
	std::mutex m;
	std::condition_variable cv;
	std::vector<bool> calls;

	std::vector<bool>
	get()
	{
		std::lock_guard<std::mutex> g(m);
		return calls;
	}

	//! Wait until the latest call says @p v (true at once if it already does).
	bool
	wait_last(bool v, std::chrono::milliseconds timeout = 2000ms)
	{
		std::unique_lock<std::mutex> l(m);
		return cv.wait_for(l, timeout, [&] { return !calls.empty() && calls.back() == v; });
	}
};

GrantLog *g_grant_log = nullptr;

void
on_grant_changed(struct u_desktop_backend * /*b*/, bool granted)
{
	std::lock_guard<std::mutex> g(g_grant_log->m);
	g_grant_log->calls.push_back(granted);
	g_grant_log->cv.notify_all();
}

const char *kTitlesExe = "/opt/test/titles-agent";
const u_desktop_control_timing kTitlesFast = {10ull * 1000000ull, 0, 0};

struct Done
{
	std::mutex m;
	std::condition_variable cv;
	std::vector<std::pair<uint64_t, int32_t>> done;

	static void
	on_state(void * /*ctx*/, u_desktop_control_state /*s*/)
	{}

	static void
	on_done(void *ctx, uint64_t id, int32_t result, const char * /*msg*/)
	{
		auto *d = static_cast<Done *>(ctx);
		std::lock_guard<std::mutex> g(d->m);
		d->done.push_back({id, result});
		d->cv.notify_all();
	}

	int32_t
	wait(uint64_t id)
	{
		std::unique_lock<std::mutex> l(m);
		int32_t r = 1;
		cv.wait_for(l, 5000ms, [&] {
			for (auto &x : done) {
				if (x.first == id) {
					r = x.second;
					return true;
				}
			}
			return false;
		});
		return r;
	}
};

//! A control over a fresh fake whose backend has the grant hook.
struct GrantHarness
{
	GrantLog log;
	Done done;
	u_desktop_backend *backend = nullptr;
	u_desktop_fake *fake = nullptr;
	u_desktop_control *c = nullptr;

	explicit GrantHarness(const u_desktop_control_timing *timing = nullptr)
	{
		g_grant_log = &log;
		backend = u_desktop_fake_create();
		fake = u_desktop_fake_from_backend(backend);
		backend->grant_changed = on_grant_changed;
		u_desktop_app_info app = {kTitlesExe, "titles-agent", nullptr, nullptr};
		u_desktop_control_callbacks cb = {Done::on_state, Done::on_done, &done, nullptr, nullptr};
		REQUIRE(u_desktop_control_create(backend, &app, &cb, timing, &c) == 0);
	}

	~GrantHarness()
	{
		u_desktop_control_destroy(&c);
		g_grant_log = nullptr;
	}

	u_desktop_control_state
	state()
	{
		return u_desktop_control_get_state(c, nullptr);
	}

	void
	grant()
	{
		REQUIRE(u_desktop_control_request(c, "tests") == U_DESKTOP_XR_SUCCESS);
		REQUIRE(state() == U_DESKTOP_STATE_GRANTED);
	}

	//! What the backend was last told matches the state now (none told = not granted).
	bool
	told_matches_state()
	{
		bool granted = state() == U_DESKTOP_STATE_GRANTED;
		std::vector<bool> v = log.get();
		return (v.empty() ? false : v.back()) == granted;
	}
};

//! No two calls in a row say the same thing, and the first says true.
bool
alternates(const std::vector<bool> &v)
{
	for (size_t i = 0; i < v.size(); i++) {
		if (v[i] != (i % 2 == 0)) {
			return false;
		}
	}
	return true;
}

} // namespace


TEST_CASE("grant_changed: told once the grant begins, only on a change, and false at destroy", "[desktop][titles]")
{
	GrantDir dir;
	GrantHarness h;
	CHECK(h.state() == U_DESKTOP_STATE_NOT_REQUESTED);
	CHECK(h.log.get().empty()); // nothing granted, nothing said

	h.grant();
	CHECK(h.log.get() == std::vector<bool>{true});

	// Observation calls while GRANTED say nothing more.
	u_desktop_window wins[8];
	uint32_t n = 0;
	u_desktop_pick pick;
	for (int i = 0; i < 20; i++) {
		CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
		CHECK(u_desktop_control_list_windows(h.c, wins, 8, &n) == U_DESKTOP_XR_SUCCESS);
		CHECK(u_desktop_control_pick(h.c, 200, 200, &pick) == U_DESKTOP_XR_SUCCESS);
	}
	CHECK(h.log.get() == std::vector<bool>{true});

	// The session ends: the backend hears false before it is destroyed.
	u_desktop_control_destroy(&h.c);
	CHECK(h.log.get() == std::vector<bool>({true, false}));
}

TEST_CASE("grant_changed: a session that never had the grant says nothing, also at destroy", "[desktop][titles]")
{
	GrantDir dir;
	GrantHarness h;
	u_desktop_fake_set_request_answer(h.fake, U_DESKTOP_ACCESS_DENIED, false);
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(h.state() == U_DESKTOP_STATE_DENIED);
	u_desktop_window wins[4];
	uint32_t n = 0;
	CHECK(u_desktop_control_list_windows(h.c, wins, 4, &n) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	u_desktop_control_destroy(&h.c);
	CHECK(h.log.get().empty());
}

TEST_CASE("grant_changed: every way the grant ends says false; coming back says true", "[desktop][titles]")
{
	GrantDir dir;
	GrantHarness h;
	h.grant();
	REQUIRE(h.log.get() == std::vector<bool>{true});

	SECTION("the kill switch")
	{
		REQUIRE(u_desktop_policy_set_suspended(true, "test"));
		CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED);
		CHECK(h.log.get() == std::vector<bool>({true, false}));
		REQUIRE(u_desktop_policy_set_suspended(false, "test"));
		// Whatever the state after the switch is turned off, the backend agrees with it.
		CHECK(h.told_matches_state());
	}
	SECTION("the store's per-app switch")
	{
		REQUIRE(u_desktop_policy_set_app(kTitlesExe, U_DESKTOP_APP_DENY, "test"));
		CHECK(h.state() == U_DESKTOP_STATE_DISABLED_BY_USER);
		CHECK(h.log.get() == std::vector<bool>({true, false}));
		REQUIRE(u_desktop_policy_set_app(kTitlesExe, U_DESKTOP_APP_NONE, "test"));
		CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
		CHECK(h.log.get() == std::vector<bool>({true, false, true}));
	}
	SECTION("the lock screen")
	{
		u_desktop_fake_set_locked(h.fake, true);
		CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED);
		CHECK(h.log.get() == std::vector<bool>({true, false}));
		u_desktop_fake_set_locked(h.fake, false);
		CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
		CHECK(h.log.get() == std::vector<bool>({true, false, true}));
	}
	SECTION("the OS takes it back")
	{
		u_desktop_fake_set_access(h.fake, U_DESKTOP_ACCESS_DENIED);
		CHECK(h.state() == U_DESKTOP_STATE_DENIED);
		CHECK(h.log.get() == std::vector<bool>({true, false}));
	}
	SECTION("asking again: no grant while the prompt is up")
	{
		u_desktop_fake_set_request_answer(h.fake, U_DESKTOP_ACCESS_GRANTED, true);
		CHECK(u_desktop_control_request_scopes(h.c, nullptr, U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_REASK) ==
		      U_DESKTOP_XR_SUCCESS);
		CHECK(h.state() == U_DESKTOP_STATE_PENDING);
		CHECK(h.log.get() == std::vector<bool>({true, false}));
		REQUIRE(u_desktop_fake_complete_request(h.fake));
		CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
		CHECK(h.log.get() == std::vector<bool>({true, false, true}));
	}
}

TEST_CASE("grant_changed: an end noticed by the worker is passed on without another app call", "[desktop][titles]")
{
	GrantDir dir;
	GrantHarness h(&kTitlesFast);
	h.grant();

	SECTION("the backend reports NOT_GRANTED mid-action (the OS revoked it)")
	{
		u_desktop_fake_set_action_result(h.fake, "type_text", U_DESKTOP_ERR_NOT_GRANTED);
		u_desktop_action a;
		memset(&a, 0, sizeof(a));
		a.type = U_DESKTOP_ACTION_TYPE_TEXT;
		a.target_window_id = 101;
		a.text = "x";
		uint64_t id = 0;
		REQUIRE(u_desktop_control_submit(h.c, &a, &id) == U_DESKTOP_XR_SUCCESS);
		CHECK(h.done.wait(id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
		CHECK(h.log.wait_last(false));
		CHECK(h.log.get() == std::vector<bool>({true, false}));
	}
	SECTION("the kill switch arrives between two batches of a text (the input guard)")
	{
		struct KillAt
		{
			static void
			call(void *ctx, const char *op, uint32_t index)
			{
				auto *fired = static_cast<std::atomic<bool> *>(ctx);
				if (strcmp(op, "type") == 0 && index == 2 && !fired->exchange(true)) {
					u_desktop_policy_set_suspended(true, "test");
				}
			}
		};
		std::atomic<bool> fired{false};
		u_desktop_fake_set_batch_hook(h.fake, KillAt::call, &fired);
		u_desktop_action a;
		memset(&a, 0, sizeof(a));
		a.type = U_DESKTOP_ACTION_TYPE_TEXT;
		a.target_window_id = 101;
		a.text = "secret-text";
		uint64_t id = 0;
		REQUIRE(u_desktop_control_submit(h.c, &a, &id) == U_DESKTOP_XR_SUCCESS);
		CHECK(h.done.wait(id) == U_DESKTOP_XR_ERROR_REFUSED);
		CHECK(h.log.wait_last(false));
		CHECK(h.log.get() == std::vector<bool>({true, false}));
		u_desktop_fake_set_batch_hook(h.fake, nullptr, nullptr);
	}
}

TEST_CASE("grant_changed: callers on many threads keep the calls in order and true to the state", "[desktop][titles]")
{
	GrantDir dir;
	GrantHarness h;
	h.grant();

	std::atomic<bool> stop{false};
	std::vector<std::thread> callers;
	for (int t = 0; t < 4; t++) {
		callers.emplace_back([&] {
			while (!stop.load()) {
				u_desktop_control_get_state(h.c, nullptr);
			}
		});
	}
	for (int i = 0; i < 40; i++) {
		REQUIRE(u_desktop_policy_set_suspended(i % 2 == 0, "test"));
		std::this_thread::sleep_for(3ms);
	}
	REQUIRE(u_desktop_policy_set_suspended(false, "test"));
	std::this_thread::sleep_for(20ms);
	stop = true;
	for (auto &t : callers) {
		t.join();
	}

	std::vector<bool> v = h.log.get();
	CHECK(v.size() >= 1);
	CHECK(alternates(v)); // never the same thing twice in a row: only changes, in order
	CHECK(h.told_matches_state());
}
