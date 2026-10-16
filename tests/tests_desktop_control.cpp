// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief XR_DXR_desktop_control (ADR-056) — the common layer, end to end over
 *        the in-memory fake backend: chord parsing, the user store and the
 *        audit log, the consent state machine (spec §3, every row), observation
 *        gating, validation, the action queue and the safety layer (spec §5).
 *
 * Nothing here touches the real desktop or the real user's files: every case
 * points DXR_DESKTOP_CONTROL_DIR at a fresh temp directory and drives
 * u_desktop_fake. Timings are real but short (tens of ms); the idle-wait
 * bounds are injected through struct u_desktop_control_timing.
 */

#include "desktop/u_desktop_chord.h"
#include "desktop/u_desktop_control.h"
#include "desktop/u_desktop_fake.h"
#include "desktop/u_desktop_policy.h"

#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void
set_env(const char *name, const char *value)
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

//! A fresh, empty directory; DXR_DESKTOP_CONTROL_DIR points at it while alive.
struct TempDir
{
	std::string path;

	TempDir()
	{
		static int counter = 0;
#ifdef _WIN32
		char base[MAX_PATH];
		GetTempPathA(MAX_PATH, base);
		path = std::string(base) + "dxr_dc_test_" + std::to_string(GetCurrentProcessId()) + "_" +
		       std::to_string(++counter);
		CreateDirectoryA(path.c_str(), nullptr);
#else
		const char *t = getenv("TMPDIR");
		std::string tmpl = std::string(t != nullptr && t[0] != '\0' ? t : "/tmp") + "/dxr_dc_test_XXXXXX";
		std::vector<char> buf(tmpl.begin(), tmpl.end());
		buf.push_back('\0');
		char *p = mkdtemp(buf.data());
		REQUIRE(p != nullptr);
		path = p;
		(void)counter;
#endif
		set_env(U_DESKTOP_POLICY_DIR_ENV, path.c_str());
		set_env("DXR_DESKTOP_CONTROL", nullptr);
	}

	~TempDir()
	{
		for (const char *n : {"desktop_control.json", "desktop_control_audit.jsonl",
		                      "desktop_control_audit.jsonl.1"}) {
			std::remove(file(n).c_str());
		}
#ifdef _WIN32
		RemoveDirectoryA(path.c_str());
#else
		rmdir(path.c_str());
#endif
		set_env(U_DESKTOP_POLICY_DIR_ENV, nullptr);
	}

	std::string
	file(const char *name) const
	{
#ifdef _WIN32
		return path + "\\" + name;
#else
		return path + "/" + name;
#endif
	}
};

std::string
slurp(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

bool
exists(const std::string &path)
{
	std::ifstream f(path);
	return f.good();
}

//! Records every callback; waits on them.
struct Recorder
{
	struct Completion
	{
		uint64_t id;
		int32_t result;
		std::string message;
	};

	std::mutex m;
	std::condition_variable cv;
	std::vector<u_desktop_control_state> states;
	std::vector<Completion> done;

	static void
	on_state(void *ctx, u_desktop_control_state s)
	{
		auto *r = static_cast<Recorder *>(ctx);
		std::lock_guard<std::mutex> g(r->m);
		r->states.push_back(s);
		r->cv.notify_all();
	}

	static void
	on_done(void *ctx, uint64_t id, int32_t result, const char *msg)
	{
		auto *r = static_cast<Recorder *>(ctx);
		std::lock_guard<std::mutex> g(r->m);
		r->done.push_back({id, result, msg != nullptr ? msg : ""});
		r->cv.notify_all();
	}

	u_desktop_control_callbacks
	callbacks()
	{
		return {on_state, on_done, this};
	}

	bool
	wait_done(size_t n, std::chrono::milliseconds timeout = 5000ms)
	{
		std::unique_lock<std::mutex> l(m);
		return cv.wait_for(l, timeout, [&] { return done.size() >= n; });
	}

	bool
	wait_state(u_desktop_control_state s, std::chrono::milliseconds timeout = 2000ms)
	{
		std::unique_lock<std::mutex> l(m);
		return cv.wait_for(l, timeout, [&] {
			for (auto x : states) {
				if (x == s) {
					return true;
				}
			}
			return false;
		});
	}

	Completion
	completion(uint64_t id)
	{
		std::lock_guard<std::mutex> g(m);
		for (auto &c : done) {
			if (c.id == id) {
				return c;
			}
		}
		return {0, 1, "<none>"};
	}

	size_t
	done_count()
	{
		std::lock_guard<std::mutex> g(m);
		return done.size();
	}
};

const char *kExe = "/opt/test/agent-app";

//! A control over a fresh fake. The fake stays reachable through `fake`.
struct Harness
{
	Recorder rec;
	u_desktop_backend *backend = nullptr;
	u_desktop_fake *fake = nullptr;
	u_desktop_control *c = nullptr;

	explicit Harness(const u_desktop_control_timing *timing = nullptr, bool with_backend = true)
	{
		if (with_backend) {
			backend = u_desktop_fake_create();
			fake = u_desktop_fake_from_backend(backend);
		}
		u_desktop_app_info app = {kExe, "agent-app", nullptr};
		u_desktop_control_callbacks cb = rec.callbacks();
		REQUIRE(u_desktop_control_create(backend, &app, &cb, timing, &c) == 0);
	}

	~Harness()
	{
		u_desktop_control_destroy(&c);
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

	uint64_t
	submit(const u_desktop_action &a)
	{
		uint64_t id = 0;
		REQUIRE(u_desktop_control_submit(c, &a, &id) == U_DESKTOP_XR_SUCCESS);
		REQUIRE(id != 0);
		return id;
	}

	Recorder::Completion
	run(const u_desktop_action &a)
	{
		size_t before = rec.done_count();
		uint64_t id = submit(a);
		REQUIRE(rec.wait_done(before + 1));
		return rec.completion(id);
	}

	std::string
	log()
	{
		char buf[16384];
		u_desktop_fake_log(fake, buf, sizeof(buf));
		return buf;
	}
};

u_desktop_action
act(uint32_t type, uint64_t target)
{
	u_desktop_action a;
	memset(&a, 0, sizeof(a));
	a.type = type;
	a.target_window_id = target;
	return a;
}

u_desktop_action
click(uint64_t target, int32_t x, int32_t y)
{
	u_desktop_action a = act(U_DESKTOP_ACTION_CLICK, target);
	a.x = x;
	a.y = y;
	return a;
}

u_desktop_action
type_text(uint64_t target, const char *text)
{
	u_desktop_action a = act(U_DESKTOP_ACTION_TYPE_TEXT, target);
	a.text = text;
	return a;
}

u_desktop_action
chord(uint64_t target, const char *keys)
{
	u_desktop_action a = act(U_DESKTOP_ACTION_KEY_CHORD, target);
	a.keys = keys;
	return a;
}

const u_desktop_control_timing kFast = {10ull * 1000000ull, 0, 0};

} // namespace


/*
 *
 * Chords.
 *
 */

TEST_CASE("desktop_chord: valid chords parse to mods + one canonical key", "[desktop][chord]")
{
	u_desktop_chord c;
	REQUIRE(u_desktop_chord_parse("ctrl+s", &c));
	CHECK(c.mods == U_DESKTOP_MOD_CTRL);
	CHECK(std::string(c.key) == "s");

	REQUIRE(u_desktop_chord_parse("Ctrl+Shift+V", &c));
	CHECK(c.mods == (U_DESKTOP_MOD_CTRL | U_DESKTOP_MOD_SHIFT));
	CHECK(std::string(c.key) == "v");

	REQUIRE(u_desktop_chord_parse(" control + alt + delete ", &c));
	CHECK(c.mods == (U_DESKTOP_MOD_CTRL | U_DESKTOP_MOD_ALT));
	CHECK(std::string(c.key) == "delete");

	for (const char *s : {"super+a", "win+a", "cmd+a", "command+a", "meta+a"}) {
		REQUIRE(u_desktop_chord_parse(s, &c));
		CHECK(c.mods == U_DESKTOP_MOD_SUPER);
	}
	REQUIRE(u_desktop_chord_parse("option+tab", &c));
	CHECK(c.mods == U_DESKTOP_MOD_ALT);
	CHECK(std::string(c.key) == "tab");

	REQUIRE(u_desktop_chord_parse("super", &c)); // modifiers only
	CHECK(c.mods == U_DESKTOP_MOD_SUPER);
	CHECK(c.key[0] == '\0');

	REQUIRE(u_desktop_chord_parse("ENTER", &c));
	CHECK(c.mods == 0);
	CHECK(std::string(c.key) == "enter");

	for (const char *k : {"f1", "f12", "f24", "0", "9", "pageup", "bracketleft", "printscreen", "menu", "grave"}) {
		INFO(k);
		REQUIRE(u_desktop_chord_parse(k, &c));
		CHECK(std::string(c.key) == k);
	}

	char out[64];
	REQUIRE(u_desktop_chord_parse("shift+alt+ctrl+super+f5", &c));
	REQUIRE(u_desktop_chord_format(&c, out, sizeof(out)));
	CHECK(std::string(out) == "ctrl+shift+alt+super+f5");
}

TEST_CASE("desktop_chord: malformed chords are rejected", "[desktop][chord]")
{
	u_desktop_chord c;
	for (const char *s : {"", "+", "ctrl+", "+a", "ctrl++a", "a+b", "ctrl+a+b", "esc", "f0", "f25", "f01",
	                      "hyper+a", "ctrl+ ", "return", "ctrl+shift+alt+super+?", "aa"}) {
		INFO("chord '" << s << "'");
		CHECK_FALSE(u_desktop_chord_parse(s, &c));
		CHECK(c.mods == 0);
		CHECK(c.key[0] == '\0');
	}
	CHECK_FALSE(u_desktop_chord_parse(nullptr, &c));
}


/*
 *
 * Store + audit.
 *
 */

TEST_CASE("desktop_policy: paths honour DXR_DESKTOP_CONTROL_DIR; missing store = defaults", "[desktop][store]")
{
	TempDir dir;
	char p[1024];
	REQUIRE(u_desktop_policy_store_path(p, sizeof(p)));
	CHECK(std::string(p) == dir.file("desktop_control.json"));
	REQUIRE(u_desktop_policy_audit_path(p, sizeof(p)));
	CHECK(std::string(p) == dir.file("desktop_control_audit.jsonl"));

	u_desktop_policy *pol = u_desktop_policy_create();
	REQUIRE(pol != nullptr);
	CHECK_FALSE(u_desktop_policy_is_suspended(pol));
	CHECK(u_desktop_policy_idle_ms(pol) == U_DESKTOP_POLICY_DEFAULT_IDLE_MS);
	CHECK(u_desktop_policy_app(pol, kExe) == U_DESKTOP_APP_NONE);
	u_desktop_policy_destroy(&pol);
	CHECK(pol == nullptr);
}

TEST_CASE("desktop_policy: allow / deny / forget / suspend round trip, atomic, mtime-reloaded", "[desktop][store]")
{
	TempDir dir;
	u_desktop_policy *pol = u_desktop_policy_create();
	REQUIRE(pol != nullptr);

	REQUIRE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_ALLOW, "test"));
	CHECK(u_desktop_policy_refresh(pol)); // changed on disk -> reloaded
	CHECK(u_desktop_policy_app(pol, kExe) == U_DESKTOP_APP_ALLOW);
	CHECK_FALSE(u_desktop_policy_refresh(pol)); // unchanged -> cached

	// Two writes in quick succession (same mtime second) are still both seen.
	REQUIRE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_DENY, "test"));
	CHECK(u_desktop_policy_refresh(pol));
	CHECK(u_desktop_policy_app(pol, kExe) == U_DESKTOP_APP_DENY);

	REQUIRE(u_desktop_policy_set_app("/other/app", U_DESKTOP_APP_ALLOW, "test"));
	REQUIRE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_NONE, "test")); // forget
	u_desktop_policy_refresh(pol);
	CHECK(u_desktop_policy_app(pol, kExe) == U_DESKTOP_APP_NONE);
	CHECK(u_desktop_policy_app(pol, "/other/app") == U_DESKTOP_APP_ALLOW);

	REQUIRE(u_desktop_policy_set_suspended(true, "test"));
	u_desktop_policy_refresh(pol);
	CHECK(u_desktop_policy_is_suspended(pol));
	REQUIRE(u_desktop_policy_set_suspended(false, "test"));
	u_desktop_policy_refresh(pol);
	CHECK_FALSE(u_desktop_policy_is_suspended(pol));

	// The file is the documented shape, and no temp file is left behind.
	std::string json = slurp(dir.file("desktop_control.json"));
	CHECK(json.find("\"version\":") != std::string::npos);
	CHECK(json.find("\"suspended\":") != std::string::npos);
	CHECK(json.find("\"idle_ms\":") != std::string::npos);
	CHECK(json.find("\"/other/app\":") != std::string::npos);
	CHECK(json.find(kExe) == std::string::npos);
#ifndef _WIN32
	char tmpname[1100];
	snprintf(tmpname, sizeof(tmpname), "%s.tmp.%u", dir.file("desktop_control.json").c_str(),
	         (unsigned)getpid());
	CHECK_FALSE(exists(tmpname));
#endif

	// Store changes are audited (who, what), never silently.
	std::string audit = slurp(dir.file("desktop_control_audit.jsonl"));
	CHECK(audit.find("\"action\":\"allow\"") != std::string::npos);
	CHECK(audit.find("\"action\":\"deny\"") != std::string::npos);
	CHECK(audit.find("\"action\":\"forget\"") != std::string::npos);
	CHECK(audit.find("\"action\":\"suspended\"") != std::string::npos);
	CHECK(audit.find("\"action\":\"resumed\"") != std::string::npos);

	u_desktop_policy_destroy(&pol);
}

TEST_CASE("desktop_policy: hand-edited store — idle_ms clamp, malformed = defaults", "[desktop][store]")
{
	TempDir dir;
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << R"({"version":1,"suspended":true,"idle_ms":999999,"apps":{"/a":"deny","/b":"bogus"}})";
	}
	u_desktop_policy *pol = u_desktop_policy_create();
	CHECK(u_desktop_policy_is_suspended(pol));
	CHECK(u_desktop_policy_idle_ms(pol) == U_DESKTOP_POLICY_MAX_IDLE_MS);
	CHECK(u_desktop_policy_app(pol, "/a") == U_DESKTOP_APP_DENY);
	CHECK(u_desktop_policy_app(pol, "/b") == U_DESKTOP_APP_NONE);

	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << "{ not json";
	}
	std::this_thread::sleep_for(20ms);
	u_desktop_policy_refresh(pol);
	CHECK_FALSE(u_desktop_policy_is_suspended(pol));
	CHECK(u_desktop_policy_idle_ms(pol) == U_DESKTOP_POLICY_DEFAULT_IDLE_MS);
	CHECK(u_desktop_policy_app(pol, "/a") == U_DESKTOP_APP_NONE);

	// A writer over a malformed file starts from defaults rather than failing.
	REQUIRE(u_desktop_policy_set_app("/a", U_DESKTOP_APP_ALLOW, "test"));
	u_desktop_policy_refresh(pol);
	CHECK(u_desktop_policy_app(pol, "/a") == U_DESKTOP_APP_ALLOW);
	u_desktop_policy_destroy(&pol);
}

TEST_CASE("desktop_audit: rotation at 1 MiB and tail across the rotated file", "[desktop][audit]")
{
	TempDir dir;
	{
		std::ofstream f(dir.file("desktop_control_audit.jsonl"), std::ios::binary);
		std::string line(1000, 'x');
		for (int i = 0; i < 1100; i++) {
			f << "{\"old\":" << i << ",\"pad\":\"" << line << "\"}\n";
		}
	}
	u_desktop_audit_record r;
	memset(&r, 0, sizeof(r));
	r.action = "click";
	r.chars = -1;
	r.result = "ok";
	REQUIRE(u_desktop_audit_append(&r));

	CHECK(exists(dir.file("desktop_control_audit.jsonl.1")));
	std::string cur = slurp(dir.file("desktop_control_audit.jsonl"));
	CHECK(cur.size() < 2048);
	CHECK(cur.find("\"action\":\"click\"") != std::string::npos);

	std::vector<std::string> lines;
	REQUIRE(u_desktop_audit_tail(
	    3, [](void *ctx, const char *l) { static_cast<std::vector<std::string> *>(ctx)->push_back(l); },
	    &lines));
	REQUIRE(lines.size() == 3);
	CHECK(lines[0].find("\"old\":1098") != std::string::npos);
	CHECK(lines[1].find("\"old\":1099") != std::string::npos);
	CHECK(lines[2].find("\"action\":\"click\"") != std::string::npos);
}


/*
 *
 * Consent state machine (spec §3) — every row, and the precedence between them.
 *
 */

TEST_CASE("desktop_state: row 1 — no backend is UNSUPPORTED whatever else holds", "[desktop][state]")
{
	TempDir dir;
	REQUIRE(u_desktop_policy_set_suspended(true, "test"));
	Harness h(nullptr, false);
	CHECK(h.state() == U_DESKTOP_STATE_UNSUPPORTED);
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(h.state() == U_DESKTOP_STATE_UNSUPPORTED);
}

TEST_CASE("desktop_state: row 2 — DXR_DESKTOP_CONTROL=0 beats the kill switch", "[desktop][state]")
{
	TempDir dir;
	REQUIRE(u_desktop_policy_set_suspended(true, "test"));
	set_env("DXR_DESKTOP_CONTROL", "0");
	Harness h;
	set_env("DXR_DESKTOP_CONTROL", nullptr);
	CHECK(h.state() == U_DESKTOP_STATE_UNSUPPORTED);
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_request_count(h.fake) == 0);
	CHECK(h.state() == U_DESKTOP_STATE_UNSUPPORTED);
}

TEST_CASE("desktop_state: rows 3-5 precedence — kill switch > deny > lock, none of them prompts", "[desktop][state]")
{
	TempDir dir;
	Harness h;
	REQUIRE(u_desktop_policy_set_suspended(true, "test"));
	REQUIRE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_DENY, "test"));
	u_desktop_fake_set_locked(h.fake, true);

	CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED); // 3 beats 4 and 5
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_request_count(h.fake) == 0);

	REQUIRE(u_desktop_policy_set_suspended(false, "test"));
	CHECK(h.state() == U_DESKTOP_STATE_DISABLED_BY_USER); // 4 beats 5
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_request_count(h.fake) == 0);

	REQUIRE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_NONE, "test"));
	CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED); // 5: locked
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_request_count(h.fake) == 0);

	u_desktop_fake_set_locked(h.fake, false);
	CHECK(h.state() == U_DESKTOP_STATE_NOT_REQUESTED); // 6: refused requests never counted
}

TEST_CASE("desktop_state: rows 6, 7, 9, 10 — request flow on an OS-consent backend", "[desktop][state]")
{
	TempDir dir;
	Harness h;
	CHECK(h.state() == U_DESKTOP_STATE_NOT_REQUESTED);

	u_desktop_fake_set_request_answer(h.fake, U_DESKTOP_ACCESS_GRANTED, true);
	CHECK(u_desktop_control_request(h.c, "reason text") == U_DESKTOP_XR_SUCCESS);
	CHECK(h.state() == U_DESKTOP_STATE_PENDING);
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_PENDING));
	// A second request while the prompt is up does not stack a second prompt.
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_request_count(h.fake) == 1);

	REQUIRE(u_desktop_fake_complete_request(h.fake));
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_GRANTED));
	CHECK(h.state() == U_DESKTOP_STATE_GRANTED);

	// The OS revokes it: row 10.
	u_desktop_fake_set_access(h.fake, U_DESKTOP_ACCESS_DENIED);
	CHECK(h.state() == U_DESKTOP_STATE_DENIED);
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_DENIED));

	// The consent decisions are audited, with the app's reason.
	std::string audit = slurp(dir.file("desktop_control_audit.jsonl"));
	CHECK(audit.find("\"action\":\"request\"") != std::string::npos);
	CHECK(audit.find("\"reason\":\"reason text\"") != std::string::npos);
	CHECK(audit.find("\"action\":\"granted\"") != std::string::npos);
}

TEST_CASE("desktop_state: row 10 — the OS says no", "[desktop][state]")
{
	TempDir dir;
	Harness h;
	u_desktop_fake_set_request_answer(h.fake, U_DESKTOP_ACCESS_DENIED, false);
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(h.state() == U_DESKTOP_STATE_DENIED);
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_DENIED));
}

TEST_CASE("desktop_state: row 8 — store-remembering backend (Windows model)", "[desktop][state]")
{
	TempDir dir;
	Harness h;
	u_desktop_fake_set_remembers_in_store(h.fake, true);
	// An OS "granted" means nothing for such a backend: only the store grants.
	u_desktop_fake_set_access(h.fake, U_DESKTOP_ACCESS_GRANTED);

	SECTION("a remembered allow resolves without a prompt")
	{
		REQUIRE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_ALLOW, "test"));
		CHECK(h.state() == U_DESKTOP_STATE_NOT_REQUESTED); // row 6 before row 8
		CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
		CHECK(u_desktop_fake_request_count(h.fake) == 0);
		CHECK(h.state() == U_DESKTOP_STATE_GRANTED);

		// `desktop forget` revokes it on the next call.
		REQUIRE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_NONE, "test"));
		CHECK(h.state() == U_DESKTOP_STATE_DENIED);
	}
	SECTION("the prompt's allow is written to the store")
	{
		CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
		CHECK(u_desktop_fake_request_count(h.fake) == 1);
		CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
		u_desktop_policy *pol = u_desktop_policy_create();
		CHECK(u_desktop_policy_app(pol, kExe) == U_DESKTOP_APP_ALLOW);
		u_desktop_policy_destroy(&pol);
	}
	SECTION("the prompt's no is written as deny (spec: row 4 thereafter)")
	{
		u_desktop_fake_set_request_answer(h.fake, U_DESKTOP_ACCESS_DENIED, false);
		CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
		CHECK(h.state() == U_DESKTOP_STATE_DISABLED_BY_USER);
	}
}


/*
 *
 * Observation gating + validation.
 *
 */

TEST_CASE("desktop_observe: every observation needs GRANTED; missing caps are FEATURE_UNSUPPORTED",
          "[desktop][observe]")
{
	TempDir dir;
	Harness h;
	u_desktop_window wins[8];
	uint32_t n = 0;
	u_desktop_pick pick;
	char text[256];

	CHECK(u_desktop_control_list_windows(h.c, wins, 8, &n) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	CHECK(u_desktop_control_get_pointer(h.c, &pick) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	CHECK(u_desktop_control_pick(h.c, 10, 10, &pick) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	CHECK(u_desktop_control_text_at(h.c, 10, 10, text, sizeof(text)) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	uint64_t id = 7;
	u_desktop_action a = act(U_DESKTOP_ACTION_ACTIVATE, 101);
	CHECK(u_desktop_control_submit(h.c, &a, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	CHECK(id == 0);

	h.grant();
	REQUIRE(u_desktop_control_list_windows(h.c, wins, 8, &n) == U_DESKTOP_XR_SUCCESS);
	REQUIRE(n == 3);
	CHECK(wins[0].id == 101);
	CHECK((wins[0].state & U_DESKTOP_WINDOW_FOCUSED) != 0);
	CHECK(wins[2].z == 2);

	REQUIRE(u_desktop_control_pick(h.c, 200, 200, &pick) == U_DESKTOP_XR_SUCCESS);
	CHECK(pick.kind == U_DESKTOP_PICK_WINDOW);
	CHECK(pick.window.id == 101);
	REQUIRE(u_desktop_control_pick(h.c, 5, 5, &pick) == U_DESKTOP_XR_SUCCESS);
	CHECK(pick.kind == U_DESKTOP_PICK_SHELL);
	REQUIRE(u_desktop_control_get_pointer(h.c, &pick) == U_DESKTOP_XR_SUCCESS);
	REQUIRE(u_desktop_control_text_at(h.c, 200, 200, text, sizeof(text)) == U_DESKTOP_XR_SUCCESS);
	CHECK(std::string(text) == "notes.txt - Text Editor");

	// The kill switch closes observation too, on the very next call.
	REQUIRE(u_desktop_policy_set_suspended(true, "test"));
	CHECK(u_desktop_control_list_windows(h.c, wins, 8, &n) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_SUSPENDED));
	REQUIRE(u_desktop_policy_set_suspended(false, "test"));
	CHECK(h.state() == U_DESKTOP_STATE_GRANTED);

	u_desktop_fake_set_caps(h.fake, U_DESKTOP_CAP_LIST_WINDOWS);
	CHECK(u_desktop_control_pick(h.c, 1, 1, &pick) == U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED);
	CHECK(u_desktop_control_text_at(h.c, 1, 1, text, sizeof(text)) == U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED);
	CHECK(u_desktop_control_list_windows(h.c, wins, 8, &n) == U_DESKTOP_XR_SUCCESS);
}

TEST_CASE("desktop_validate: submit-time validation (spec §4)", "[desktop][validate]")
{
	TempDir dir;
	Harness h;
	h.grant();

	auto rejects = [&](u_desktop_action a) {
		uint64_t id = 42;
		int32_t r = u_desktop_control_submit(h.c, &a, &id);
		CHECK(r == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
		CHECK(id == 0);
	};

	rejects(act(7, 101));                                    // unknown type
	rejects(act(99, 101));                                   // unknown type
	rejects(act(U_DESKTOP_ACTION_ACTIVATE, 0));              // no target
	rejects(type_text(101, nullptr));                        // text missing
	rejects(type_text(101, ""));                             // text empty
	rejects(type_text(101, "\xff\xfe"));                     // not UTF-8
	rejects(chord(101, nullptr));                            // keys missing
	rejects(chord(101, ""));                                 // keys empty
	rejects(chord(101, "ctrl+banana"));                      // chord does not parse
	u_desktop_action a = click(101, 10, 10);
	a.button = 3;
	rejects(a);
	a = click(101, 10, 10);
	a.click_count = 4;
	rejects(a);
	a = act(U_DESKTOP_ACTION_SCROLL, 101);
	rejects(a); // nothing to scroll
	a.scroll_y = 101;
	rejects(a);
	a = act(U_DESKTOP_ACTION_MOVE_RESIZE, 101);
	a.rect.w = -1;
	rejects(a);
	std::string big(U_DESKTOP_CONTROL_MAX_TEXT_BYTES + 1, 'a');
	rejects(type_text(101, big.c_str()));

	// Valid ones are accepted (and every one of them completes).
	CHECK(h.submit(type_text(101, "h\xc3\xa9llo")) != 0);   // UTF-8
	CHECK(h.submit(chord(101, "ctrl+s")) != 0);
	a = act(U_DESKTOP_ACTION_MOVE_RESIZE, 101);
	a.rect = {10, 40, 0, 0}; // keep size
	CHECK(h.submit(a) != 0);
	CHECK(h.rec.wait_done(3));
	CHECK(h.rec.done_count() == 3);
}


/*
 *
 * The queue + the safety layer.
 *
 */

TEST_CASE("desktop_queue: actions run in submission order, one completion each", "[desktop][queue]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_fake_set_action_delay_ms(h.fake, 5);

	std::vector<uint64_t> ids;
	for (int i = 0; i < 10; i++) {
		char keys[8];
		snprintf(keys, sizeof(keys), "ctrl+%d", i);
		ids.push_back(h.submit(chord(101, keys)));
	}
	REQUIRE(h.rec.wait_done(10));
	std::this_thread::sleep_for(50ms);
	REQUIRE(h.rec.done_count() == 10);
	for (size_t i = 0; i < ids.size(); i++) {
		CHECK(h.rec.done[i].id == ids[i]);
		CHECK(h.rec.done[i].result == U_DESKTOP_XR_SUCCESS);
		CHECK(h.rec.done[i].message == "ok");
		if (i > 0) {
			CHECK(ids[i] > ids[i - 1]);
		}
	}
	std::string log = h.log();
	size_t pos = 0;
	for (int i = 0; i < 10; i++) {
		char line[32];
		snprintf(line, sizeof(line), "key ctrl+%d", i);
		size_t at = log.find(line, pos);
		INFO(log);
		REQUIRE(at != std::string::npos);
		pos = at;
	}
}

TEST_CASE("desktop_idle: waits while the user is active, then delivers", "[desktop][idle]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_fake_set_user_active(h.fake, true);

	auto t0 = Clock::now();
	uint64_t id = h.submit(type_text(101, "x"));
	std::this_thread::sleep_for(300ms);
	CHECK(h.rec.done_count() == 0); // still waiting
	CHECK(h.log().find("type") == std::string::npos);
	u_desktop_fake_set_user_active(h.fake, false);
	REQUIRE(h.rec.wait_done(1));
	auto took = Clock::now() - t0;
	CHECK(h.rec.completion(id).result == U_DESKTOP_XR_SUCCESS);
	// 300 ms active + 600 ms idle requirement.
	CHECK(took >= 850ms);
}

TEST_CASE("desktop_idle: our own injections do not count as the user", "[desktop][idle]")
{
	TempDir dir;
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << R"({"version":1,"suspended":false,"idle_ms":3000,"apps":{}})";
	}
	Harness h(&kFast);
	h.grant();
	auto t0 = Clock::now();
	for (int i = 0; i < 4; i++) {
		h.submit(click(101, 300 + i, 300));
	}
	REQUIRE(h.rec.wait_done(4));
	auto took = Clock::now() - t0;
	for (auto &c : h.rec.done) {
		CHECK(c.result == U_DESKTOP_XR_SUCCESS);
	}
	// Each click injects input, so a naive idle check would wait 3 s per action.
	CHECK(took < 1500ms);
}

TEST_CASE("desktop_idle: the user never stops -> REFUSED after the max wait", "[desktop][idle]")
{
	TempDir dir;
	const u_desktop_control_timing t = {10ull * 1000000ull, 0, 300ull * 1000000ull};
	Harness h(&t);
	h.grant();
	u_desktop_fake_set_user_active(h.fake, true);
	auto r = h.run(type_text(101, "x"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "the user is active");
	CHECK(h.log().find("type") == std::string::npos);

	// Window actions other than ACTIVATE do not wait.
	auto m = act(U_DESKTOP_ACTION_MINIMIZE, 103);
	CHECK(h.run(m).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(act(U_DESKTOP_ACTION_ACTIVATE, 103)).result == U_DESKTOP_XR_ERROR_REFUSED);
}

TEST_CASE("desktop_verify: pointer target verification (incl. dialogs and the re-pick)", "[desktop][verify]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();

	CHECK(h.run(click(101, 200, 200)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.log().find("button left down") != std::string::npos);
	u_desktop_fake_clear_log(h.fake);

	auto r = h.run(click(102, 200, 200)); // 101 is there, not 102
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "target is not under the point (found gedit: notes.txt - Text Editor)");
	CHECK(h.log().empty()); // nothing was delivered anywhere

	r = h.run(click(101, 5, 5)); // the shell panel
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "target is not under the point (found the shell)");

	// A dialog of the target counts as the target.
	u_desktop_window dlg;
	memset(&dlg, 0, sizeof(dlg));
	dlg.id = 104;
	dlg.parent_id = 101;
	snprintf(dlg.app_id, sizeof(dlg.app_id), "gedit");
	snprintf(dlg.title, sizeof(dlg.title), "Save As");
	dlg.frame = {300, 300, 200, 100};
	u_desktop_fake_add_window(h.fake, &dlg);
	CHECK(h.run(click(101, 350, 350)).result == U_DESKTOP_XR_SUCCESS);

	// Something pops up between the move and the press: re-pick catches it.
	u_desktop_window pop;
	memset(&pop, 0, sizeof(pop));
	pop.id = 105;
	snprintf(pop.app_id, sizeof(pop.app_id), "popup");
	snprintf(pop.title, sizeof(pop.title), "Update available");
	pop.frame = {150, 150, 300, 300};
	u_desktop_fake_add_window(h.fake, &pop);
	CHECK(h.run(act(U_DESKTOP_ACTION_ACTIVATE, 101)).result == U_DESKTOP_XR_SUCCESS); // 101 above 105
	u_desktop_fake_raise_on_next_move(h.fake, 105);
	u_desktop_fake_clear_log(h.fake);
	r = h.run(click(101, 200, 200));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "target is not under the point (found popup: Update available)");
	CHECK(h.log().find("move 200 200") != std::string::npos);
	CHECK(h.log().find("button") == std::string::npos);

	// The pointer did not arrive: no press.
	CHECK(h.run(act(U_DESKTOP_ACTION_ACTIVATE, 101)).result == U_DESKTOP_XR_SUCCESS);
	u_desktop_fake_set_pointer_skew(h.fake, 10, 0);
	u_desktop_fake_clear_log(h.fake);
	r = h.run(click(101, 200, 200));
	CHECK(r.result == U_DESKTOP_XR_ERROR_RUNTIME_FAILURE);
	CHECK(h.log().find("button") == std::string::npos);
}

TEST_CASE("desktop_verify: keyboard target verification (focus, dialog parent)", "[desktop][verify]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();

	CHECK(h.run(type_text(101, "hello")).result == U_DESKTOP_XR_SUCCESS);
	char content[256];
	REQUIRE(u_desktop_fake_window_content(h.fake, 101, content, sizeof(content)));
	CHECK(std::string(content) == "hello");

	auto r = h.run(type_text(102, "nope"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "target window does not have focus \xe2\x80\x94 ACTIVATE it first");
	REQUIRE(u_desktop_fake_window_content(h.fake, 102, content, sizeof(content)));
	CHECK(std::string(content).empty());
	CHECK(h.run(chord(102, "ctrl+w")).result == U_DESKTOP_XR_ERROR_REFUSED);

	// ACTIVATE first, then it works.
	CHECK(h.run(act(U_DESKTOP_ACTION_ACTIVATE, 102)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(type_text(102, "yes")).result == U_DESKTOP_XR_SUCCESS);

	// A focused dialog of the target counts as the target.
	u_desktop_window dlg;
	memset(&dlg, 0, sizeof(dlg));
	dlg.id = 106;
	dlg.parent_id = 102;
	snprintf(dlg.title, sizeof(dlg.title), "Find");
	dlg.frame = {1100, 300, 300, 100};
	u_desktop_fake_add_window(h.fake, &dlg);
	u_desktop_fake_set_focus(h.fake, 106);
	CHECK(h.run(chord(102, "enter")).result == U_DESKTOP_XR_SUCCESS);
	u_desktop_fake_set_focus(h.fake, 0);
	CHECK(h.run(chord(102, "enter")).result == U_DESKTOP_XR_ERROR_REFUSED);
}

TEST_CASE("desktop_verify: WINDOW_LOST when the target is gone", "[desktop][verify]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_fake_remove_window(h.fake, 102);
	auto r = h.run(act(U_DESKTOP_ACTION_MAXIMIZE, 102));
	CHECK(r.result == U_DESKTOP_XR_ERROR_WINDOW_LOST);
	CHECK(h.run(click(102, 1200, 400)).result == U_DESKTOP_XR_ERROR_WINDOW_LOST);
	CHECK(h.run(act(U_DESKTOP_ACTION_CLOSE, 999)).result == U_DESKTOP_XR_ERROR_WINDOW_LOST);
	CHECK(h.run(act(U_DESKTOP_ACTION_CLOSE, 103)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(act(U_DESKTOP_ACTION_CLOSE, 103)).result == U_DESKTOP_XR_ERROR_WINDOW_LOST);
}

TEST_CASE("desktop_safety: kill switch mid-queue refuses the rest and announces SUSPENDED", "[desktop][safety]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_fake_set_action_delay_ms(h.fake, 100);
	std::vector<uint64_t> ids;
	for (int i = 0; i < 5; i++) {
		ids.push_back(h.submit(chord(101, "ctrl+s")));
	}
	REQUIRE(h.rec.wait_done(1));
	REQUIRE(u_desktop_policy_set_suspended(true, "test"));
	REQUIRE(h.rec.wait_done(5));
	CHECK(h.rec.completion(ids[0]).result == U_DESKTOP_XR_SUCCESS);
	int refused = 0;
	for (size_t i = 1; i < ids.size(); i++) {
		auto c = h.rec.completion(ids[i]);
		if (c.result == U_DESKTOP_XR_ERROR_REFUSED) {
			CHECK(c.message == "kill switch");
			refused++;
		}
	}
	CHECK(refused >= 3); // the one in flight when it flipped may complete
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_SUSPENDED));
	CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED);

	// New submissions are refused synchronously while suspended.
	uint64_t id = 0;
	u_desktop_action a = chord(101, "ctrl+s");
	CHECK(u_desktop_control_submit(h.c, &a, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);

	// resume -> GRANTED again with no new prompt.
	REQUIRE(u_desktop_policy_set_suspended(false, "test"));
	CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
	CHECK(u_desktop_fake_request_count(h.fake) == 1);
}

TEST_CASE("desktop_safety: lock screen and per-app deny refuse with their reason", "[desktop][safety]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_fake_set_action_delay_ms(h.fake, 50);
	uint64_t a1 = h.submit(chord(101, "ctrl+s"));
	uint64_t a2 = h.submit(chord(101, "ctrl+s"));
	u_desktop_fake_set_locked(h.fake, true);
	REQUIRE(h.rec.wait_done(2));
	auto c2 = h.rec.completion(a2);
	CHECK(c2.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(c2.message == "screen locked");
	(void)a1;
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_SUSPENDED));
	u_desktop_fake_set_locked(h.fake, false);
	CHECK(h.state() == U_DESKTOP_STATE_GRANTED);

	uint64_t a3 = h.submit(chord(101, "ctrl+s"));
	REQUIRE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_DENY, "test"));
	uint64_t a4 = 0;
	u_desktop_action a = chord(101, "ctrl+s");
	CHECK(u_desktop_control_submit(h.c, &a, &a4) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	REQUIRE(h.rec.wait_done(3));
	auto c3 = h.rec.completion(a3);
	// a3 either ran before the deny landed, or was refused by it.
	CHECK((c3.result == U_DESKTOP_XR_SUCCESS || c3.message == "disabled by the user"));
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_DISABLED_BY_USER));
}

TEST_CASE("desktop_audit: every action is logged; typed text never is", "[desktop][audit]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	const char *secret = "correct horse battery staple";
	CHECK(h.run(type_text(101, secret)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(type_text(102, "zzz-refused-secret")).result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(h.run(chord(101, "ctrl+s")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(chord(101, "shift+q")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(click(101, 200, 220)).result == U_DESKTOP_XR_SUCCESS);

	std::string audit = slurp(dir.file("desktop_control_audit.jsonl"));
	INFO(audit);
	CHECK(audit.find(secret) == std::string::npos);
	CHECK(audit.find("horse") == std::string::npos);
	CHECK(audit.find("zzz-refused-secret") == std::string::npos);
	CHECK(audit.find("\"action\":\"type_text\"") != std::string::npos);
	CHECK(audit.find("\"chars\":28") != std::string::npos);
	CHECK(audit.find("\"chars\":18") != std::string::npos);
	CHECK(audit.find("\"result\":\"refused\"") != std::string::npos);
	CHECK(audit.find("\"keys\":\"ctrl+s\"") != std::string::npos);
	CHECK(audit.find("shift+q") == std::string::npos); // a typed character, not a shortcut
	CHECK(audit.find("\"action\":\"click\"") != std::string::npos);
	CHECK(audit.find("\"x\":200") != std::string::npos);
	CHECK(audit.find("\"app\":\"gedit\"") != std::string::npos);
	CHECK(audit.find(std::string("\"exe\":\"") + kExe + "\"") != std::string::npos);
}

TEST_CASE("desktop_lifecycle: queue limit, then destroy with a full queue", "[desktop][lifecycle]")
{
	TempDir dir;
	auto *h = new Harness(&kFast);
	h->grant();
	u_desktop_fake_set_action_delay_ms(h->fake, 200);

	// The worker takes the first one; 64 more fill the queue; one more overflows.
	h->submit(chord(101, "ctrl+a"));
	std::this_thread::sleep_for(50ms);
	for (int i = 0; i < U_DESKTOP_CONTROL_QUEUE_SIZE; i++) {
		h->submit(chord(101, "ctrl+b"));
	}
	uint64_t id = 1;
	u_desktop_action a = chord(101, "ctrl+c");
	CHECK(u_desktop_control_submit(h->c, &a, &id) == U_DESKTOP_XR_ERROR_LIMIT_REACHED);
	CHECK(id == 0);

	Recorder *rec = &h->rec;
	auto t0 = Clock::now();
	u_desktop_control_destroy(&h->c);
	auto took = Clock::now() - t0;
	CHECK(h->c == nullptr);
	// Only the in-flight backend call is waited for, never the queue.
	CHECK(took < 1000ms);
	size_t n = rec->done_count();
	CHECK(n <= 1);
	std::this_thread::sleep_for(300ms);
	CHECK(rec->done_count() == n); // nothing after destroy
	delete h;
}

TEST_CASE("desktop_lifecycle: destroy while a consent answer is still owed", "[desktop][lifecycle]")
{
	TempDir dir;
	Harness h;
	u_desktop_fake_set_remembers_in_store(h.fake, true);
	u_desktop_fake_set_request_answer(h.fake, U_DESKTOP_ACCESS_GRANTED, true);
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(h.state() == U_DESKTOP_STATE_PENDING);
	size_t states = h.rec.states.size();
	u_desktop_control_destroy(&h.c);
	CHECK(h.rec.states.size() == states);
	// The fake answers "denied" from its destroy; that must not be persisted.
	u_desktop_policy *pol = u_desktop_policy_create();
	CHECK(u_desktop_policy_app(pol, kExe) == U_DESKTOP_APP_NONE);
	u_desktop_policy_destroy(&pol);
}

TEST_CASE("desktop_select: DXR_DESKTOP_BACKEND chooses the fake / none", "[desktop][select]")
{
	set_env("DXR_DESKTOP_BACKEND", "fake");
	u_desktop_backend *b = u_desktop_backend_create();
	REQUIRE(b != nullptr);
	CHECK(std::string(b->name) == "fake");
	CHECK(u_desktop_fake_from_backend(b) != nullptr);
	b->destroy(b);

	set_env("DXR_DESKTOP_BACKEND", "none");
	CHECK(u_desktop_backend_create() == nullptr);
	set_env("DXR_DESKTOP_BACKEND", nullptr);

	char name[64];
	u_desktop_display_name_from_exe("/Applications/Lumo Agent.app/Contents/MacOS/lumo", name, sizeof(name));
	CHECK(std::string(name) == "Lumo Agent");
	u_desktop_display_name_from_exe("C:\\Program Files\\Lumo\\Lumo.EXE", name, sizeof(name));
	CHECK(std::string(name) == "Lumo");
	u_desktop_display_name_from_exe("/usr/bin/3dluma", name, sizeof(name));
	CHECK(std::string(name) == "3dluma");
}
