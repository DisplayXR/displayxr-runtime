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
#include "desktop/u_desktop_gnome_logic.h"
#include "desktop/u_desktop_mini_cache.h"
#include "desktop/u_desktop_policy.h"

#include "catch_amalgamated.hpp"

#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
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

	struct Capture
	{
		uint64_t id;
		int32_t result;
		uint32_t w, h;
		u_desktop_rect source;
		uint64_t frame_ns;
		std::string message;
	};

	std::mutex m;
	std::condition_variable cv;
	std::vector<u_desktop_control_state> states;
	std::vector<Completion> done;
	std::vector<u_desktop_control_state> capture_states;
	std::vector<Capture> captures;

	static void
	on_capture_state(void *ctx, u_desktop_control_state s)
	{
		auto *r = static_cast<Recorder *>(ctx);
		std::lock_guard<std::mutex> g(r->m);
		r->capture_states.push_back(s);
		r->cv.notify_all();
	}

	static void
	on_capture_done(void *ctx, const u_desktop_capture_done *d)
	{
		auto *r = static_cast<Recorder *>(ctx);
		std::lock_guard<std::mutex> g(r->m);
		r->captures.push_back({d->capture_id, d->result, d->w, d->h, d->source, d->frame_ns,
		                       d->message != nullptr ? d->message : ""});
		r->cv.notify_all();
	}

	bool
	wait_captures(size_t n, std::chrono::milliseconds timeout = 5000ms)
	{
		std::unique_lock<std::mutex> l(m);
		return cv.wait_for(l, timeout, [&] { return captures.size() >= n; });
	}

	bool
	wait_capture_state(u_desktop_control_state s, std::chrono::milliseconds timeout = 2000ms)
	{
		std::unique_lock<std::mutex> l(m);
		return cv.wait_for(l, timeout, [&] {
			for (auto x : capture_states) {
				if (x == s) {
					return true;
				}
			}
			return false;
		});
	}

	size_t
	capture_count()
	{
		std::lock_guard<std::mutex> g(m);
		return captures.size();
	}

	Capture
	last_capture()
	{
		std::lock_guard<std::mutex> g(m);
		return captures.empty() ? Capture{0, 1, 0, 0, {0, 0, 0, 0}, 0, "<none>"} : captures.back();
	}

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
		return {on_state, on_done, this, on_capture_state, on_capture_done};
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

TEST_CASE("desktop_policy: hand-edited store — idle_ms clamp; an unparseable store keeps the last good read",
          "[desktop][store]")
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
	// v2 (spec §6.1): never defaults — the last good read is kept.
	CHECK(u_desktop_policy_is_suspended(pol));
	CHECK(u_desktop_policy_app(pol, "/a") == U_DESKTOP_APP_DENY);
	CHECK_FALSE(u_desktop_policy_is_unreadable(pol));

	// A writer never turns an unparseable store into defaults.
	CHECK_FALSE(u_desktop_policy_set_app("/a", U_DESKTOP_APP_ALLOW, "test"));
	CHECK(slurp(dir.file("desktop_control.json")) == "{ not json");
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

	// v2: EVERY action waits, window ops included.
	CHECK(h.run(act(U_DESKTOP_ACTION_MINIMIZE, 103)).result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(h.run(act(U_DESKTOP_ACTION_ACTIVATE, 103)).result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(h.log().find("op ") == std::string::npos);
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


/*
 *
 * Fix round (adversarial review of the integrated branch) — every case below
 * failed on the code before it.
 *
 */

namespace {

//! A batch hook that runs @ref fn once, at batch @ref at of op @ref op.
struct Hook
{
	std::string op;
	uint32_t at = 0;
	std::function<void()> fn;
	bool fired = false;

	static void
	call(void *ctx, const char *op, uint32_t index)
	{
		auto *h = static_cast<Hook *>(ctx);
		if (!h->fired && h->op == op && index == h->at) {
			h->fired = true;
			h->fn();
		}
	}

	void
	install(u_desktop_fake *f)
	{
		u_desktop_fake_set_batch_hook(f, call, this);
	}
};

std::string
content(u_desktop_fake *f, uint64_t id)
{
	char buf[1024];
	REQUIRE(u_desktop_fake_window_content(f, id, buf, sizeof(buf)));
	return buf;
}

bool
saw_state(Recorder &r, u_desktop_control_state s)
{
	std::lock_guard<std::mutex> g(r.m);
	for (auto x : r.states) {
		if (x == s) {
			return true;
		}
	}
	return false;
}

} // namespace

// --- 1. the input guard ----------------------------------------------------------

TEST_CASE("fix1_guard: focus moving mid-text stops the rest of the text", "[desktop][fix][guard]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	Hook hook{"type", 3, [&] { u_desktop_fake_set_focus(h.fake, 102); }};
	hook.install(h.fake);

	auto r = h.run(type_text(101, "abcdefgh"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "focus moved");
	CHECK(content(h.fake, 101) == "abc"); // what was delivered stays
	CHECK(content(h.fake, 102).empty());  // nothing reaches the new focus
}

TEST_CASE("fix1_guard: the kill switch flipping mid-text stops it at once", "[desktop][fix][guard]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	Hook hook{"type", 2, [] { REQUIRE(u_desktop_policy_set_suspended(true, "test")); }};
	hook.install(h.fake);

	auto r = h.run(type_text(101, "secret-text"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "kill switch");
	CHECK(content(h.fake, 101) == "se");
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_SUSPENDED));
}

TEST_CASE("fix1_guard: the pick changing between the move and the press refuses the press", "[desktop][fix][guard]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	Hook hook{"press", 0, [&] {
		          u_desktop_window pop;
		          memset(&pop, 0, sizeof(pop));
		          pop.id = 105;
		          snprintf(pop.app_id, sizeof(pop.app_id), "popup");
		          snprintf(pop.title, sizeof(pop.title), "Phishing dialog");
		          pop.frame = {150, 150, 300, 300};
		          u_desktop_fake_add_window(h.fake, &pop);
	          }};
	hook.install(h.fake);

	auto r = h.run(click(101, 200, 200));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(h.log().find("button left down") == std::string::npos);
	CHECK(u_desktop_fake_pressed_buttons(h.fake) == 0);
}

TEST_CASE("fix1_guard: a chord stopped mid-way releases what it pressed", "[desktop][fix][guard]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	Hook hook{"key", 1, [&] { u_desktop_fake_set_focus(h.fake, 102); }};
	hook.install(h.fake);
	auto r = h.run(chord(101, "ctrl+shift+w"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "focus moved");
	CHECK(h.log().find("keys released 1") != std::string::npos);
	CHECK(h.log().find("key ctrl+shift+w") == std::string::npos);
}

TEST_CASE("fix1_guard: prepare_input runs BEFORE target verification", "[desktop][fix][guard]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	// Slow setup that steals focus (a portal dialog) must be caught by step 4.
	Hook hook{"prepare", 0, [&] { u_desktop_fake_set_focus(h.fake, 102); }};
	hook.install(h.fake);
	auto r = h.run(type_text(101, "x"));
	CHECK(u_desktop_fake_prepare_count(h.fake) == 1);
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "target window does not have focus \xe2\x80\x94 ACTIVATE it first");
	CHECK(content(h.fake, 101).empty());
	CHECK(content(h.fake, 102).empty());
}

// --- 2. the idle wait ------------------------------------------------------------

TEST_CASE("fix2_idle: reviewer case — one click, the user goes active, 20 queued texts all refused",
          "[desktop][fix][idle]")
{
	TempDir dir;
	const u_desktop_control_timing t = {10ull * 1000000ull, 0, 120ull * 1000000ull};
	Harness h(&t);
	h.grant();
	CHECK(h.run(click(101, 300, 300)).result == U_DESKTOP_XR_SUCCESS);
	u_desktop_fake_set_user_active(h.fake, true); // right after our click
	std::vector<uint64_t> ids;
	for (int i = 0; i < 20; i++) {
		ids.push_back(h.submit(type_text(101, "x")));
	}
	REQUIRE(h.rec.wait_done(21, 15000ms));
	for (auto id : ids) {
		auto c = h.rec.completion(id);
		CHECK(c.result == U_DESKTOP_XR_ERROR_REFUSED);
		CHECK(c.message == "the user is active");
	}
	CHECK(content(h.fake, 101).empty());
}

TEST_CASE("fix2_idle: window ops wait for the user too", "[desktop][fix][idle]")
{
	TempDir dir;
	const u_desktop_control_timing t = {10ull * 1000000ull, 0, 150ull * 1000000ull};
	Harness h(&t);
	h.grant();
	u_desktop_fake_set_user_active(h.fake, true);
	for (uint32_t ty : {U_DESKTOP_ACTION_MINIMIZE, U_DESKTOP_ACTION_MAXIMIZE, U_DESKTOP_ACTION_CLOSE,
	                    U_DESKTOP_ACTION_MOVE_RESIZE}) {
		auto r = h.run(act(ty, 103));
		CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
		CHECK(r.message == "the user is active");
	}
	CHECK(h.log().find("op ") == std::string::npos);
}

TEST_CASE("fix2_idle: input before our injection is the user's even if the OS ignores ours",
          "[desktop][fix][idle]")
{
	// macOS HID idle model: our synthetic events never reset the idle counter, so
	// after our click the OS still reports the USER's last keystroke. Old code:
	// "anything up to our last end + grace is ours" took it for ours.
	TempDir dir;
	auto write_idle = [&](int ms) {
		std::ofstream f(dir.file("desktop_control.json"));
		f << "{\"version\":1,\"suspended\":false,\"idle_ms\":" << ms << ",\"apps\":{}}";
	};
	write_idle(100);
	const u_desktop_control_timing t = {10ull * 1000000ull, 0, 300ull * 1000000ull};
	Harness h(&t);
	h.grant();
	u_desktop_fake_set_injection_is_input(h.fake, false);
	u_desktop_fake_set_user_active(h.fake, true);
	std::this_thread::sleep_for(50ms);
	u_desktop_fake_set_user_active(h.fake, false); // the user's last keystroke: U
	// Idle 100 ms is enough for the click: it runs after U, in its own window.
	CHECK(h.run(click(101, 300, 300)).result == U_DESKTOP_XR_SUCCESS);
	std::this_thread::sleep_for(20ms);
	write_idle(3000); // now the user must have been idle 3 s
	auto r = h.run(type_text(101, "x"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "the user is active");
	CHECK(content(h.fake, 101).empty());
}

// --- 3. corrupt store ------------------------------------------------------------

TEST_CASE("fix3_store: suspended + deny + a trailing comma stays SUSPENDED / DISABLED", "[desktop][fix][store]")
{
	TempDir dir;
	Harness h;
	h.grant();
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << "{\"version\":1,\"suspended\":true,\"idle_ms\":600,\"apps\":{\"" << kExe << "\":\"deny\"}}";
	}
	CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED);
	std::this_thread::sleep_for(20ms);
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << "{\"version\":1,\"suspended\":true,\"idle_ms\":600,\"apps\":{\"" << kExe << "\":\"deny\",}}";
	}
	CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED); // NOT back to GRANTED

	// deny alone, then broken: still DISABLED_BY_USER.
	std::this_thread::sleep_for(20ms);
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << "{\"version\":1,\"suspended\":false,\"idle_ms\":600,\"apps\":{\"" << kExe << "\":\"deny\"}}";
	}
	CHECK(h.state() == U_DESKTOP_STATE_DISABLED_BY_USER);
	std::this_thread::sleep_for(20ms);
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << "{\"version\":1,\"suspended\":false,\"apps\":{\"" << kExe << "\":\"deny\",},}";
	}
	CHECK(h.state() == U_DESKTOP_STATE_DISABLED_BY_USER);
}

TEST_CASE("fix3_store: unreadable with no good read -> SUSPENDED with a clear message", "[desktop][fix][store]")
{
	TempDir dir;
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << "{\"suspended\":true,}";
	}
	Harness h(&kFast);
	CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED);
	u_desktop_policy *pol = u_desktop_policy_create();
	CHECK(u_desktop_policy_is_unreadable(pol));
	CHECK(u_desktop_policy_is_suspended(pol));
	u_desktop_policy_destroy(&pol);

	// Resuming over it is refused; the kill switch moves it aside and works.
	CHECK_FALSE(u_desktop_policy_set_suspended(false, "test"));
	CHECK_FALSE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_ALLOW, "test"));
	REQUIRE(u_desktop_policy_set_suspended(true, "test"));
	CHECK(exists(dir.file("desktop_control.json.corrupt")));
	CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED);
	std::remove(dir.file("desktop_control.json.corrupt").c_str());

	// A MISSING file is defaults, though.
	std::remove(dir.file("desktop_control.json").c_str());
	CHECK(h.state() == U_DESKTOP_STATE_NOT_REQUESTED);
}

TEST_CASE("fix3_store: an action refused by an unreadable store says so", "[desktop][fix][store]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_fake_set_action_delay_ms(h.fake, 100);
	h.submit(chord(101, "ctrl+s")); // in flight
	uint64_t b = h.submit(chord(101, "ctrl+s"));
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << "not json at all";
	}
	REQUIRE(h.rec.wait_done(2));
	// This process had a good read (defaults: no file) - it keeps it ...
	CHECK(h.rec.completion(b).result == U_DESKTOP_XR_SUCCESS);
	// ... a process that never read the store fails closed, and never prompts.
	Harness h2(&kFast);
	CHECK(h2.state() == U_DESKTOP_STATE_SUSPENDED);
	CHECK(u_desktop_control_request(h2.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_request_count(h2.fake) == 0);
	CHECK(saw_state(h2.rec, U_DESKTOP_STATE_SUSPENDED));
}

// --- 4. store races --------------------------------------------------------------

TEST_CASE("fix4_store: concurrent read-modify-writes never lose an update", "[desktop][fix][store]")
{
	TempDir dir;
	const int N = 150;
	std::thread a([&] {
		for (int i = 0; i < N; i++) {
			if (i == N / 3) {
				u_desktop_policy_set_suspended(true, "stop");
			}
			u_desktop_policy_set_app(("/a/" + std::to_string(i)).c_str(), U_DESKTOP_APP_DENY, "a");
		}
	});
	std::thread b([&] {
		for (int i = 0; i < N; i++) {
			u_desktop_policy_set_app(("/b/" + std::to_string(i)).c_str(), U_DESKTOP_APP_ALLOW, "b");
		}
	});
	a.join();
	b.join();
	u_desktop_policy *pol = u_desktop_policy_create();
	CHECK(u_desktop_policy_is_suspended(pol)); // the kill switch was never overwritten
	int lost = 0;
	for (int i = 0; i < N; i++) {
		lost += u_desktop_policy_app(pol, ("/a/" + std::to_string(i)).c_str()) != U_DESKTOP_APP_DENY;
		lost += u_desktop_policy_app(pol, ("/b/" + std::to_string(i)).c_str()) != U_DESKTOP_APP_ALLOW;
	}
	CHECK(lost == 0);
	u_desktop_policy_destroy(&pol);
	std::remove(dir.file("desktop_control.json.lock").c_str());
}

// --- 5. revocation ---------------------------------------------------------------

TEST_CASE("fix5_revocation: NOT_GRANTED from the backend -> DENIED, no further backend calls",
          "[desktop][fix][revocation]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_fake_set_action_delay_ms(h.fake, 50);
	u_desktop_fake_set_action_result(h.fake, "type_text", U_DESKTOP_ERR_NOT_GRANTED);
	uint64_t a = h.submit(type_text(101, "x"));
	uint64_t b = h.submit(chord(101, "ctrl+s")); // queued behind it
	REQUIRE(h.rec.wait_done(2));
	CHECK(h.rec.completion(a).result == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	CHECK(h.rec.completion(b).result == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	CHECK(h.log().find("key ") == std::string::npos); // the chord never reached the OS
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_DENIED));
	CHECK(h.state() == U_DESKTOP_STATE_DENIED); // although the fake still says GRANTED

	uint32_t calls = u_desktop_fake_action_calls(h.fake);
	uint64_t id = 0;
	u_desktop_action c = chord(101, "ctrl+s");
	CHECK(u_desktop_control_submit(h.c, &c, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	std::this_thread::sleep_for(50ms);
	CHECK(u_desktop_fake_action_calls(h.fake) == calls);

	// A new request on a user gesture asks again.
	u_desktop_fake_set_action_result(h.fake, nullptr, U_DESKTOP_OK);
	CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
	CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
}

// --- 6. privacy ------------------------------------------------------------------

TEST_CASE("fix6_privacy: a bad chord is never echoed back", "[desktop][fix][privacy]")
{
	u_desktop_action a = chord(101, "ctrl+SECRETXYZ");
	char why[256];
	CHECK_FALSE(u_desktop_control_validate(&a, why, sizeof(why)));
	CHECK(std::string(why) == "keys is not a valid chord");
}

TEST_CASE("fix6_privacy: a backend message naming typed text never reaches the event or the audit",
          "[desktop][fix][privacy]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_fake_set_action_result(h.fake, "type_text", U_DESKTOP_ERR_FAILED);
	auto r = h.run(type_text(101, "hunter2-password"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_RUNTIME_FAILURE);
	CHECK(r.message == "the input could not be delivered");
	std::string audit = slurp(dir.file("desktop_control_audit.jsonl"));
	CHECK(audit.find("hunter2") == std::string::npos);
	CHECK(audit.find("could not be delivered") != std::string::npos);
}

TEST_CASE("fix6_privacy: only real shortcuts are audited — never AltGr / Option / shift text", "[desktop][fix][privacy]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	for (const char *k : {"ctrl+alt+q", "alt+e", "option+shift+2", "ctrl+alt+super+e", "shift+a", "q"}) {
		CHECK(h.run(chord(101, k)).result == U_DESKTOP_XR_SUCCESS);
	}
	CHECK(h.run(chord(101, "ctrl+s")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(chord(101, "super+shift+s")).result == U_DESKTOP_XR_SUCCESS);
	std::string audit = slurp(dir.file("desktop_control_audit.jsonl"));
	INFO(audit);
	CHECK(audit.find("\"keys\":\"ctrl+s\"") != std::string::npos);
	CHECK(audit.find("\"keys\":\"shift+super+s\"") != std::string::npos);
	size_t n = 0;
	for (size_t p = audit.find("\"keys\""); p != std::string::npos; p = audit.find("\"keys\"", p + 1)) {
		n++;
	}
	CHECK(n == 2);
}

// --- 7. robustness ---------------------------------------------------------------

TEST_CASE("fix7_robust: coordinates are clamped at validation", "[desktop][fix][validate]")
{
	u_desktop_action a = click(101, 1000000, -1000000);
	CHECK(u_desktop_control_validate(&a, nullptr, 0));
	a.x = 1000001;
	CHECK_FALSE(u_desktop_control_validate(&a, nullptr, 0));
	a = act(U_DESKTOP_ACTION_DRAG, 101);
	a.x2 = -2000000000;
	CHECK_FALSE(u_desktop_control_validate(&a, nullptr, 0));
	a = act(U_DESKTOP_ACTION_MOVE_RESIZE, 101);
	a.rect = {0, 0, 1000001, 10};
	CHECK_FALSE(u_desktop_control_validate(&a, nullptr, 0));
	a.rect = {-1000001, 0, 10, 10};
	CHECK_FALSE(u_desktop_control_validate(&a, nullptr, 0));
}

TEST_CASE("fix7_robust: an unknown action refused at submit is still audited", "[desktop][fix][audit]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	REQUIRE(u_desktop_policy_set_suspended(true, "test"));
	u_desktop_action a = act(99, 101);
	uint64_t id = 0;
	CHECK(u_desktop_control_submit(h.c, &a, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	std::string audit = slurp(dir.file("desktop_control_audit.jsonl"));
	CHECK(audit.find("\"action\":\"unknown\"") != std::string::npos);
}

TEST_CASE("fix7_robust: text at a point never ends in half a code point", "[desktop][fix][text]")
{
	CHECK(u_desktop_utf8_trim_partial("ab\xc3", 3) == 2);         // é cut after its lead byte
	CHECK(u_desktop_utf8_trim_partial("ab\xc3\xa9", 4) == 4);     // complete
	CHECK(u_desktop_utf8_trim_partial("a\xe2\x82", 3) == 1);      // € cut
	CHECK(u_desktop_utf8_trim_partial("a\xf0\x9f\x98", 4) == 1);  // emoji cut
	CHECK(u_desktop_utf8_trim_partial("a\xf0\x9f\x98\x80", 5) == 5);
	CHECK(u_desktop_utf8_trim_partial("abc", 3) == 3);

	TempDir dir;
	Harness h(&kFast);
	h.grant();
	CHECK(h.run(type_text(101, "ab\xc3\xa9z")).result == U_DESKTOP_XR_SUCCESS);
	char buf[4]; // "ab" + the first byte of é + NUL
	REQUIRE(u_desktop_control_text_at(h.c, 200, 200, buf, sizeof(buf)) == U_DESKTOP_XR_SUCCESS);
	CHECK(std::string(buf) == "ab");
}

// --- 8. lifetime ------------------------------------------------------------------

TEST_CASE("fix8_lifetime: a consent answer delivered after destroy is dropped safely", "[desktop][fix][lifetime]")
{
	TempDir dir;
	u_desktop_fake_orphan orphan = {nullptr, nullptr};
	{
		Harness h;
		u_desktop_fake_set_request_answer(h.fake, U_DESKTOP_ACCESS_GRANTED, true);
		u_desktop_fake_set_orphan_on_destroy(h.fake, &orphan);
		CHECK(u_desktop_control_request(h.c, nullptr) == U_DESKTOP_XR_SUCCESS);
		size_t states = h.rec.states.size();
		u_desktop_control_destroy(&h.c);
		REQUIRE(orphan.done != nullptr);
		// The backend broke its contract: the answer arrives after the control is
		// freed. Must not touch freed memory (ASan) nor call back.
		orphan.done(orphan.ctx, U_DESKTOP_ACCESS_GRANTED);
		CHECK(h.rec.states.size() == states);
	}
	u_desktop_policy *pol = u_desktop_policy_create();
	CHECK(u_desktop_policy_app(pol, kExe) == U_DESKTOP_APP_NONE);
	u_desktop_policy_destroy(&pol);
}

// --- polish round ------------------------------------------------------------------

namespace {

void
write_idle_ms(const TempDir &dir, int ms)
{
	std::ofstream f(dir.file("desktop_control.json"));
	f << "{\"version\":1,\"suspended\":false,\"idle_ms\":" << ms << ",\"apps\":{}}";
}

size_t
count_of(const std::string &hay, const std::string &needle)
{
	size_t n = 0;
	for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) {
		n++;
	}
	return n;
}

} // namespace

TEST_CASE("polish1_order: a user who starts typing during a slow prepare_input is still waited for",
          "[desktop][polish][idle]")
{
	TempDir dir;
	const u_desktop_control_timing t = {10ull * 1000000ull, 0, 300ull * 1000000ull};
	Harness h(&t);
	h.grant();
	// GNOME's silent portal restore takes 0.3-9 s; the user starts typing meanwhile.
	Hook hook{"prepare", 0, [&] {
		          u_desktop_fake_set_user_active(h.fake, true);
		          std::this_thread::sleep_for(200ms);
	          }};
	hook.install(h.fake);
	auto t0 = Clock::now();
	auto r = h.run(type_text(101, "x"));
	auto took = Clock::now() - t0;
	CHECK(u_desktop_fake_prepare_count(h.fake) == 1);
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "the user is active");
	CHECK(content(h.fake, 101).empty());
	CHECK(took >= 450ms); // 200 ms setup, THEN the 300 ms idle wait
}

TEST_CASE("polish1_order: a user active during prepare_input who then stops gets the action delivered",
          "[desktop][polish][idle]")
{
	TempDir dir;
	write_idle_ms(dir, 200);
	Harness h(&kFast);
	h.grant();
	Hook hook{"prepare", 0, [&] {
		          u_desktop_fake_set_user_active(h.fake, true);
		          std::this_thread::sleep_for(100ms);
		          u_desktop_fake_set_user_active(h.fake, false); // last keystroke now
	          }};
	hook.install(h.fake);
	auto t0 = Clock::now();
	auto r = h.run(type_text(101, "x"));
	CHECK(r.result == U_DESKTOP_XR_SUCCESS);
	CHECK(content(h.fake, 101) == "x");
	CHECK(Clock::now() - t0 >= 280ms); // 100 ms setup + the 200 ms idle requirement
}

TEST_CASE("polish2_grace: a user keystroke 50 ms after our injection ends is the user's",
          "[desktop][polish][idle]")
{
	// Reviewer case: with a 150 ms grace this keystroke was absorbed as ours and the
	// next action ran straight into the user's typing.
	TempDir dir;
	write_idle_ms(dir, 1000);
	const u_desktop_control_timing t = {10ull * 1000000ull, 0, 300ull * 1000000ull}; // default grace
	Harness h(&t);
	h.grant();
	CHECK(h.run(click(101, 300, 300)).result == U_DESKTOP_XR_SUCCESS);
	std::this_thread::sleep_for(50ms);
	u_desktop_fake_set_user_active(h.fake, true); // one keystroke at ~+50 ms
	u_desktop_fake_set_user_active(h.fake, false);
	auto r = h.run(type_text(101, "x"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "the user is active");
	CHECK(content(h.fake, 101).empty());
}

TEST_CASE("polish2_grace: back-to-back actions are not slowed by our own input (30 ms grace)",
          "[desktop][polish][idle]")
{
	TempDir dir;
	write_idle_ms(dir, 3000);
	Harness h(&kFast);
	h.grant();
	auto t0 = Clock::now();
	for (int i = 0; i < 5; i++) {
		h.submit(type_text(101, "ab"));
	}
	REQUIRE(h.rec.wait_done(5));
	for (auto &c : h.rec.done) {
		CHECK(c.result == U_DESKTOP_XR_SUCCESS);
	}
	CHECK(content(h.fake, 101) == "ababababab");
	CHECK(Clock::now() - t0 < 1500ms);
}

TEST_CASE("polish4_drag: the kill switch after the press releases at the START point, no move",
          "[desktop][polish][guard]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	Hook hook{"pressed", 0, [] { u_desktop_policy_set_suspended(true, "test"); }};
	hook.install(h.fake);
	u_desktop_action a = act(U_DESKTOP_ACTION_DRAG, 101);
	a.x = 200;
	a.y = 200;
	a.x2 = 600;
	a.y2 = 500;
	auto r = h.run(a);
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "kill switch");
	std::string log = h.log();
	CHECK(log.find("button left down") != std::string::npos);
	CHECK(log.find("button left up 200 200") != std::string::npos);
	CHECK(log.find("move 600 500") == std::string::npos);
	CHECK(u_desktop_fake_pressed_buttons(h.fake) == 0);
	CHECK(h.rec.wait_state(U_DESKTOP_STATE_SUSPENDED));
}

TEST_CASE("polish4_drag: an uninterrupted drag still moves and releases at the end point",
          "[desktop][polish][guard]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_action a = act(U_DESKTOP_ACTION_DRAG, 101);
	a.x = 200;
	a.y = 200;
	a.x2 = 600;
	a.y2 = 500;
	CHECK(h.run(a).result == U_DESKTOP_XR_SUCCESS);
	std::string log = h.log();
	CHECK(log.find("move 600 500") != std::string::npos);
	CHECK(log.find("button left up 600 500") != std::string::npos);
	CHECK(u_desktop_fake_pressed_buttons(h.fake) == 0);
}

TEST_CASE("polish5_lock: a slow lock query never holds the control's mutex", "[desktop][polish][lock]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_fake_set_lock_query_delay_ms(h.fake, 300);
	// Three threads observe at once. Under the mutex they would queue (900 ms).
	auto t0 = Clock::now();
	std::vector<std::thread> ts;
	std::atomic<int> granted{0};
	for (int i = 0; i < 3; i++) {
		ts.emplace_back([&] {
			if (u_desktop_control_get_state(h.c, nullptr) == U_DESKTOP_STATE_GRANTED) {
				granted++;
			}
		});
	}
	for (auto &t : ts) {
		t.join();
	}
	CHECK(Clock::now() - t0 < 700ms);
	CHECK(granted == 3);

	// The answer still counts: locked -> SUSPENDED, and an action is refused.
	u_desktop_fake_set_lock_query_delay_ms(h.fake, 0);
	u_desktop_fake_set_locked(h.fake, true);
	CHECK(h.state() == U_DESKTOP_STATE_SUSPENDED);
	u_desktop_fake_set_locked(h.fake, false);
	CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
	Hook hook{"type", 1, [&] { u_desktop_fake_set_locked(h.fake, true); }};
	hook.install(h.fake);
	auto r = h.run(type_text(101, "abc"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "screen locked");
	CHECK(content(h.fake, 101) == "a");
}

TEST_CASE("polish6_audit: an unknown action type while GRANTED is audited once per submit",
          "[desktop][polish][audit]")
{
	TempDir dir;
	Harness h(&kFast);
	h.grant();
	u_desktop_action a = act(99, 101);
	uint64_t id = 0;
	CHECK(u_desktop_control_submit(h.c, &a, &id) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
	CHECK(u_desktop_control_submit(h.c, &a, &id) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
	std::string audit = slurp(dir.file("desktop_control_audit.jsonl"));
	CHECK(count_of(audit, "\"action\":\"unknown\"") == 2);
	CHECK(audit.find("\"result\":\"invalid\"") != std::string::npos);
	// A known type that fails validation is not an "unknown" record.
	u_desktop_action t = type_text(101, "");
	CHECK(u_desktop_control_submit(h.c, &t, &id) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
	CHECK(count_of(slurp(dir.file("desktop_control_audit.jsonl")), "\"action\":\"unknown\"") == 2);
}

// --- 11. macOS minimized cache (platform-neutral logic) ---------------------------

namespace {

u_desktop_window
mini_win(uint64_t id, const char *title)
{
	u_desktop_window w;
	memset(&w, 0, sizeof(w));
	w.id = id;
	snprintf(w.title, sizeof(w.title), "%s", title);
	w.frame = {10, 20, 300, 200};
	w.state = U_DESKTOP_WINDOW_MINIMIZED;
	return w;
}

bool
mini_has(const u_desktop_mini_cache &c, uint64_t id)
{
	u_desktop_window out[U_DESKTOP_MINI_CACHE_MAX];
	uint32_t n = u_desktop_mini_cache_get(&c, out, U_DESKTOP_MINI_CACHE_MAX);
	for (uint32_t i = 0; i < n; i++) {
		if (out[i].id == id) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("polish11_mini: a window we minimize is listed at once and survives a stale snapshot",
          "[desktop][polish][macos]")
{
	static u_desktop_mini_cache c; // large: keep it off the stack
	memset(&c, 0, sizeof(c));
	const uint64_t S = 1000000000ull;
	u_desktop_window other = mini_win(5, "other");
	u_desktop_mini_cache_publish(&c, &other, 1, 1 * S);
	CHECK(mini_has(c, 5));
	CHECK_FALSE(mini_has(c, 718));

	// MINIMIZE 718 at t=2 s: listed immediately (the reviewer's repro).
	u_desktop_window w = mini_win(718, "notes");
	w.state = 0;
	u_desktop_mini_cache_pin(&c, 718, true, &w, 2 * S);
	REQUIRE(mini_has(c, 718));
	u_desktop_window out[U_DESKTOP_MINI_CACHE_MAX];
	uint32_t n = u_desktop_mini_cache_get(&c, out, U_DESKTOP_MINI_CACHE_MAX);
	for (uint32_t i = 0; i < n; i++) {
		if (out[i].id == 718) {
			CHECK((out[i].state & U_DESKTOP_WINDOW_MINIMIZED) != 0);
			CHECK(std::string(out[i].title) == "notes");
			CHECK(out[i].frame.w == 300);
		}
	}

	// A snapshot that started BEFORE the minimize (t=1.5 s) does not list it: the pin stays.
	u_desktop_mini_cache_publish(&c, &other, 1, 1500000000ull);
	CHECK(mini_has(c, 718));
	CHECK(mini_has(c, 5));
	// A snapshot after it that still misses it (a slow app's stale AX table): pin still young.
	u_desktop_mini_cache_publish(&c, &other, 1, 3 * S);
	CHECK(mini_has(c, 718));
	// A snapshot that confirms it retires the pin...
	u_desktop_window both[2] = {other, mini_win(718, "notes")};
	u_desktop_mini_cache_publish(&c, both, 2, 4 * S);
	CHECK(c.pin_n == 0);
	CHECK(mini_has(c, 718));

	// RESTORE at t=5 s: gone at once, even under a snapshot that started earlier.
	u_desktop_mini_cache_pin(&c, 718, false, nullptr, 5 * S);
	CHECK_FALSE(mini_has(c, 718));
	u_desktop_mini_cache_publish(&c, both, 2, 4500000000ull);
	CHECK_FALSE(mini_has(c, 718));
	CHECK(mini_has(c, 5));

	// A pin the snapshots keep contradicting expires after the TTL: the OS wins
	// (the user re-minimized it themselves).
	u_desktop_mini_cache_publish(&c, both, 2, 5 * S + U_DESKTOP_MINI_CACHE_PIN_TTL_NS + 1);
	CHECK(mini_has(c, 718));
	CHECK(c.pin_n == 0);

	// CLOSE of a minimized window removes it; a newer pin for the same id replaces the old one.
	u_desktop_mini_cache_pin(&c, 718, true, &w, 20 * S);
	u_desktop_mini_cache_pin(&c, 718, false, nullptr, 21 * S);
	CHECK(c.pin_n == 1);
	CHECK_FALSE(mini_has(c, 718));
}

TEST_CASE("polish11_mini: a full cache never overwrites an entry with a pin", "[desktop][polish][macos]")
{
	static u_desktop_mini_cache c;
	memset(&c, 0, sizeof(c));
	static u_desktop_window snap[U_DESKTOP_MINI_CACHE_MAX];
	for (uint32_t i = 0; i < U_DESKTOP_MINI_CACHE_MAX; i++) {
		snap[i] = mini_win(100 + i, "w");
	}
	u_desktop_mini_cache_publish(&c, snap, U_DESKTOP_MINI_CACHE_MAX, 1);
	u_desktop_window w = mini_win(9999, "new");
	u_desktop_mini_cache_pin(&c, 9999, true, &w, 2);
	CHECK(c.n == U_DESKTOP_MINI_CACHE_MAX);
	CHECK(mini_has(c, 100 + U_DESKTOP_MINI_CACHE_MAX - 1));
	CHECK_FALSE(mini_has(c, 9999));
}

/*
 * GNOME backend decisions (u_desktop_gnome_logic.h) — pure, so pinned on every OS.
 */

namespace {
u_desktop_paste_verdict
decide(bool lost_before, bool lost_after, uint32_t writes, int text_check, bool *restore)
{
	u_desktop_paste_obs o = {lost_before, lost_after, writes, text_check};
	return u_desktop_paste_decide(&o, restore);
}
} // namespace

TEST_CASE("gnome_paste: the user copying before the keystroke = refused, nothing pasted, clipboard untouched",
          "[desktop][gnome][paste]")
{
	bool restore = true;
	CHECK(decide(true, false, 0, -1, &restore) == U_DESKTOP_PASTE_CHANGED_HANDS);
	CHECK_FALSE(restore);
	CHECK(u_desktop_paste_result(U_DESKTOP_PASTE_CHANGED_HANDS) == U_DESKTOP_ERR_REFUSED);
	CHECK(std::string(u_desktop_paste_message(U_DESKTOP_PASTE_CHANGED_HANDS)).find("changed hands") !=
	      std::string::npos);
}

TEST_CASE(
    "gnome_paste: losing the clipboard after the keystroke = never delivered unless OUR text was read, no restore",
    "[desktop][gnome][paste]")
{
	bool restore = true;
	// Never read: the target was handed the user's clipboard. A changed field is
	// the USER's text arriving (GNOME 50.1 re-check: 37/80 trials 8-25 ms after
	// type start reported plain ok here) — failed, said honestly, whatever the field.
	CHECK(decide(false, true, 0, -1, &restore) == U_DESKTOP_PASTE_LOST_NOT_SERVED);
	CHECK_FALSE(restore);
	CHECK(decide(false, true, 0, 0, &restore) == U_DESKTOP_PASTE_LOST_NOT_SERVED);
	CHECK(decide(false, true, 0, 1, &restore) == U_DESKTOP_PASTE_LOST_NOT_SERVED);
	CHECK_FALSE(restore);
	CHECK(u_desktop_paste_result(U_DESKTOP_PASTE_LOST_NOT_SERVED) == U_DESKTOP_ERR_FAILED);
	CHECK(std::string(u_desktop_paste_message(U_DESKTOP_PASTE_LOST_NOT_SERVED)).find("user's clipboard") !=
	      std::string::npos);
	// Read, but nothing confirms which text landed: unknown.
	CHECK(decide(false, true, 3, -1, &restore) == U_DESKTOP_PASTE_LOST_UNCERTAIN);
	CHECK(decide(false, true, 1, 0, &restore) == U_DESKTOP_PASTE_LOST_UNCERTAIN);
	CHECK(u_desktop_paste_result(U_DESKTOP_PASTE_LOST_UNCERTAIN) == U_DESKTOP_ERR_FAILED);
	// Our text was served AND the field changed: delivered, the user's new clipboard stays.
	CHECK(decide(false, true, 1, 1, &restore) == U_DESKTOP_PASTE_DELIVERED);
	CHECK_FALSE(restore);
	// Lost before the key wins over everything else.
	CHECK(decide(true, true, 2, 1, &restore) == U_DESKTOP_PASTE_CHANGED_HANDS);
	CHECK_FALSE(restore);
}

TEST_CASE("gnome_paste: the end of a type never leaves the selection owned and empty", "[desktop][gnome][paste]")
{
	// Untouched, a previous text read: offer it again.
	CHECK(u_desktop_clip_end_decide(false, true, true) == U_DESKTOP_CLIP_END_RESTORE);
	// Untouched, nothing to restore: release, never sit on it serving nothing.
	CHECK(u_desktop_clip_end_decide(false, true, false) == U_DESKTOP_CLIP_END_RELEASE);
	// Changed hands while we still own it (the user's copy landed between
	// SelectionRead and our SetSelection — re-check 2 N1, 9/185): release; the old
	// text is never offered over the user's.
	CHECK(u_desktop_clip_end_decide(true, true, true) == U_DESKTOP_CLIP_END_RELEASE);
	CHECK(u_desktop_clip_end_decide(true, true, false) == U_DESKTOP_CLIP_END_RELEASE);
	// Someone else owns it: it is the user's — leave it.
	CHECK(u_desktop_clip_end_decide(true, false, true) == U_DESKTOP_CLIP_END_LEAVE);
	CHECK(u_desktop_clip_end_decide(false, false, true) == U_DESKTOP_CLIP_END_LEAVE);
	CHECK(u_desktop_clip_end_decide(false, false, false) == U_DESKTOP_CLIP_END_LEAVE);
	// The refusal that replaced the user's copy says so.
	CHECK(u_desktop_paste_result(U_DESKTOP_PASTE_CHANGED_HANDS_REPLACED) == U_DESKTOP_ERR_REFUSED);
	const std::string m = u_desktop_paste_message(U_DESKTOP_PASTE_CHANGED_HANDS_REPLACED);
	CHECK(m.find("gone") != std::string::npos);
	CHECK(m.find("nothing was pasted") != std::string::npos);
}

TEST_CASE("gnome_paste: a clipboard-manager read alone is never 'verified'", "[desktop][gnome][paste]")
{
	bool restore = false;
	// No accessible text field: a read is all there is -> ok, but unverified.
	CHECK(decide(false, false, 1, -1, &restore) == U_DESKTOP_PASTE_UNVERIFIED);
	CHECK(restore);
	CHECK(u_desktop_paste_result(U_DESKTOP_PASTE_UNVERIFIED) == U_DESKTOP_OK);
	CHECK(std::string(u_desktop_paste_message(U_DESKTOP_PASTE_UNVERIFIED)).rfind(U_DESKTOP_MSG_UNVERIFIED, 0) == 0);
	// A focused text field that did not change: the read was not the target's.
	CHECK(decide(false, false, 2, 0, &restore) == U_DESKTOP_PASTE_FIELD_UNCHANGED);
	CHECK(u_desktop_paste_result(U_DESKTOP_PASTE_FIELD_UNCHANGED) == U_DESKTOP_ERR_FAILED);
	// It changed: delivered.
	CHECK(decide(false, false, 1, 1, &restore) == U_DESKTOP_PASTE_DELIVERED);
	CHECK(restore);
	// Nobody read at all.
	CHECK(decide(false, false, 0, -1, &restore) == U_DESKTOP_PASTE_NOT_ASKED);
	CHECK(decide(false, false, 0, 0, &restore) == U_DESKTOP_PASTE_NOT_ASKED);
	// Messages never name text; they are all non-empty except delivered.
	for (int v = U_DESKTOP_PASTE_UNVERIFIED; v <= U_DESKTOP_PASTE_CHANGED_HANDS_REPLACED; v++) {
		CHECK(std::strlen(u_desktop_paste_message((u_desktop_paste_verdict)v)) > 0);
	}
}

TEST_CASE("gnome_paste: the focused text field comparison", "[desktop][gnome][paste]")
{
	u_desktop_text_state none = {false, 0, 0, 0};
	u_desktop_text_state a = {true, 10, 10, u_desktop_fnv1a("hello", 5)};
	u_desktop_text_state same = a;
	u_desktop_text_state longer = {true, 15, 15, u_desktop_fnv1a("hello world", 11)};
	u_desktop_text_state replaced = {true, 10, 10, u_desktop_fnv1a("hallo", 5)};
	CHECK(u_desktop_text_check(&none, &a) == -1);
	CHECK(u_desktop_text_check(&a, &none) == -1);
	CHECK(u_desktop_text_check(&a, &same) == 0);
	CHECK(u_desktop_text_check(&a, &longer) == 1);
	CHECK(u_desktop_text_check(&a, &replaced) == 1); // a selection replaced by text of equal length
	CHECK(u_desktop_fnv1a("", 0) != u_desktop_fnv1a("a", 1));
}

TEST_CASE("gnome_paste: success message — plain ok, or ok (unverified: …)", "[desktop][gnome][paste]")
{
	char out[U_DESKTOP_MSG_SIZE];
	u_desktop_success_message("", out, sizeof(out));
	CHECK(std::string(out) == "ok");
	u_desktop_success_message(nullptr, out, sizeof(out));
	CHECK(std::string(out) == "ok");
	// A stale failure message from a retried step is not the outcome.
	u_desktop_success_message("the pointer did not arrive at (1,2)", out, sizeof(out));
	CHECK(std::string(out) == "ok");
	u_desktop_success_message(U_DESKTOP_MSG_UNVERIFIED "nothing confirmed it", out, sizeof(out));
	CHECK(std::string(out) == "ok (unverified: nothing confirmed it)");
}

TEST_CASE("gnome_move: the drop waits >= 250 ms on the target monitor after the last crossing",
          "[desktop][gnome][move]")
{
	CHECK(U_DESKTOP_CROSS_DWELL_MS >= 250);
	CHECK(u_desktop_cross_dwell_left_ms(0, 5000, U_DESKTOP_CROSS_DWELL_MS) == 0); // no crossing
	CHECK(u_desktop_cross_dwell_left_ms(1000, 1000, 300) == 300);
	CHECK(u_desktop_cross_dwell_left_ms(1000, 1090, 300) == 210); // the PR's old 30-90 ms release
	CHECK(u_desktop_cross_dwell_left_ms(1000, 1299, 300) == 1);
	CHECK(u_desktop_cross_dwell_left_ms(1000, 1300, 300) == 0);
	CHECK(u_desktop_cross_dwell_left_ms(1000, 900, 300) == 300); // clock went backwards
}

TEST_CASE("gnome_activate: a press after a monitor crossing settles first; a moved frame is caught",
          "[desktop][gnome][activate]")
{
	// The press settle reuses the crossing dwell: no crossing = no wait.
	CHECK(U_DESKTOP_PRESS_SETTLE_MS >= 250);
	CHECK(u_desktop_cross_dwell_left_ms(0, 5000, U_DESKTOP_PRESS_SETTLE_MS) == 0);
	CHECK(u_desktop_cross_dwell_left_ms(1000, 1004, U_DESKTOP_PRESS_SETTLE_MS) > 0); // the 4-5 ms jump
	const u_desktop_rect before = {100, 200, 800, 600};
	const u_desktop_rect same = {102, 197, 800, 600};     // within tolerance
	const u_desktop_rect jumped = {2020, 180, 800, 600};  // relocated to the other monitor
	const u_desktop_rect resized = {100, 200, 960, 1080}; // tiled
	const u_desktop_rect nudged = {100, 200, 800, 605};   // one edge past tolerance
	CHECK_FALSE(u_desktop_frame_moved(&before, &before, 4));
	CHECK_FALSE(u_desktop_frame_moved(&before, &same, 4));
	CHECK(u_desktop_frame_moved(&before, &jumped, 4));
	CHECK(u_desktop_frame_moved(&before, &resized, 4));
	CHECK(u_desktop_frame_moved(&before, &nudged, 4));
	const u_desktop_rect huge = {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX}; // no overflow
	CHECK(u_desktop_frame_moved(&before, &huge, 4));
}

TEST_CASE("gnome_move: only shell UI the gesture opened is dismissed", "[desktop][gnome][move]")
{
	const uint8_t W = U_DESKTOP_PICK_WINDOW, D = U_DESKTOP_PICK_DESKTOP, S = U_DESKTOP_PICK_SHELL,
	              U = (uint8_t)U_DESKTOP_COVER_UNKNOWN;
	const uint8_t before[5] = {W, D, W, S, U};
	const uint8_t same[5] = {W, D, W, S, U};
	const uint8_t popup[5] = {S, S, W, S, U};     // a tiling popup over the free half
	const uint8_t was_shell[5] = {W, D, W, S, S}; // the user's dialog / unknown point
	CHECK_FALSE(u_desktop_shell_cover_appeared(before, same, 5));
	CHECK(u_desktop_shell_cover_appeared(before, popup, 5));
	CHECK_FALSE(u_desktop_shell_cover_appeared(before, was_shell, 5));
	CHECK_FALSE(u_desktop_shell_cover_appeared(before, popup, 0));
}

#ifndef _WIN32
TEST_CASE("gnome_clipboard: a reader that went away is EPIPE, never a SIGPIPE, disposition untouched",
          "[desktop][gnome][sigpipe]")
{
	struct sigaction before_sa = {};
	REQUIRE(sigaction(SIGPIPE, nullptr, &before_sa) == 0);
	sigset_t mask_before;
	pthread_sigmask(SIG_SETMASK, nullptr, &mask_before);

	int fds[2];
	REQUIRE(pipe(fds) == 0);
	close(fds[0]); // the reader is gone (the selection changed hands)
	const char data[] = "agent text";
	errno = 0;
	ptrdiff_t w = u_desktop_write_nosigpipe(fds[1], data, sizeof(data) - 1);
	int e = errno;
	close(fds[1]);
	// Still alive here (the default disposition would have killed the process).
	CHECK(w == -1);
	CHECK(e == EPIPE);

	// No SIGPIPE left pending, the thread's mask and the process disposition unchanged.
	sigset_t pending;
	sigpending(&pending);
	CHECK(sigismember(&pending, SIGPIPE) == 0);
	sigset_t mask_after;
	pthread_sigmask(SIG_SETMASK, nullptr, &mask_after);
	CHECK(sigismember(&mask_after, SIGPIPE) == sigismember(&mask_before, SIGPIPE));
	struct sigaction after_sa = {};
	REQUIRE(sigaction(SIGPIPE, nullptr, &after_sa) == 0);
	CHECK(after_sa.sa_handler == before_sa.sa_handler);

	// A reader that is there still gets the bytes.
	REQUIRE(pipe(fds) == 0);
	CHECK(u_desktop_write_nosigpipe(fds[1], data, sizeof(data) - 1) == (ptrdiff_t)(sizeof(data) - 1));
	char buf[32] = {0};
	CHECK(read(fds[0], buf, sizeof(buf)) == (ssize_t)(sizeof(data) - 1));
	close(fds[0]);
	close(fds[1]);
}
#endif

/*
 *
 * Spec v2: screen capture (§10, ADR-056 Amendment 1) — over the fake backend.
 *
 */

#include "desktop/u_desktop_capture.h"

namespace {

u_desktop_capture_info
cap_desktop(uint32_t max_w = 8192, uint32_t max_h = 8192)
{
	u_desktop_capture_info i;
	memset(&i, 0, sizeof(i));
	i.target = U_DESKTOP_CAPTURE_DESKTOP;
	i.max_w = max_w;
	i.max_h = max_h;
	return i;
}

u_desktop_capture_info
cap_region(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t max_w = 8192, uint32_t max_h = 8192)
{
	u_desktop_capture_info i = cap_desktop(max_w, max_h);
	i.target = U_DESKTOP_CAPTURE_REGION;
	i.region = {x, y, w, h};
	return i;
}

u_desktop_capture_info
cap_window(uint64_t id, uint32_t max_w = 8192, uint32_t max_h = 8192)
{
	u_desktop_capture_info i = cap_desktop(max_w, max_h);
	i.target = U_DESKTOP_CAPTURE_WINDOW;
	i.window_id = id;
	return i;
}

void
grant_capture(Harness &h)
{
	REQUIRE(u_desktop_control_request_scopes(h.c, "tests", U_DESKTOP_SCOPE_CAPTURE) == U_DESKTOP_XR_SUCCESS);
	REQUIRE(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_GRANTED);
}

//! One capture, waited for. The image (if any) is fetched into @p img.
Recorder::Capture
capture(Harness &h, const u_desktop_capture_info &info, std::vector<uint8_t> *img = nullptr)
{
	size_t before = h.rec.capture_count();
	uint64_t id = 0;
	REQUIRE(u_desktop_control_capture(h.c, &info, &id) == U_DESKTOP_XR_SUCCESS);
	REQUIRE(id != 0);
	REQUIRE(h.rec.wait_captures(before + 1));
	Recorder::Capture c = h.rec.last_capture();
	REQUIRE(c.id == id);
	if (img != nullptr && c.result == U_DESKTOP_XR_SUCCESS) {
		uint32_t n = 0;
		REQUIRE(u_desktop_control_capture_image(h.c, id, 0, &n, nullptr) == U_DESKTOP_XR_SUCCESS);
		REQUIRE(n == c.w * c.h * 4);
		img->assign(n, 0);
		REQUIRE(u_desktop_control_capture_image(h.c, id, n, &n, img->data()) == U_DESKTOP_XR_SUCCESS);
	}
	return c;
}

const uint8_t *
px(const std::vector<uint8_t> &img, const Recorder::Capture &c, uint32_t x, uint32_t y)
{
	return &img[((size_t)y * c.w + x) * 4];
}

//! The desktop point image pixel (x, y) maps to (spec §10.3).
void
map_back(const Recorder::Capture &c, uint32_t x, uint32_t y, int32_t *dx, int32_t *dy)
{
	*dx = (int32_t)std::floor(c.source.x + (x + 0.5) * c.source.w / (double)c.w);
	*dy = (int32_t)std::floor(c.source.y + (y + 0.5) * c.source.h / (double)c.h);
}

std::vector<std::string>
audit_lines(const TempDir &dir)
{
	std::vector<std::string> out;
	std::stringstream ss(slurp(dir.file("desktop_control_audit.jsonl")));
	std::string line;
	while (std::getline(ss, line)) {
		out.push_back(line);
	}
	return out;
}

//! Captures with a short retention (300 ms) and burst gap (600 ms > the 250 ms start cap).
void
fast_capture_timing(Harness &h)
{
	u_desktop_control_set_capture_timing_for_tests(h.c, 300ull * 1000000ull, 600ull * 1000000ull);
}

} // namespace

TEST_CASE("capture_helpers: fit never scales up and keeps the aspect ratio", "[desktop][capture]")
{
	uint32_t w = 0, h = 0;
	u_desktop_capture_fit(1920, 1080, 8192, 8192, &w, &h);
	CHECK((w == 1920 && h == 1080));
	u_desktop_capture_fit(1920, 1080, 960, 960, &w, &h);
	CHECK((w == 960 && h == 540));
	u_desktop_capture_fit(1920, 1080, 4000, 270, &w, &h);
	CHECK((w == 480 && h == 270));
	u_desktop_capture_fit(5000, 3, 100, 100, &w, &h);
	CHECK((w == 100 && h == 1)); // never 0
}

TEST_CASE("capture_helpers: area downscale averages, withheld pixels never darken real ones",
          "[desktop][capture]")
{
	u_desktop_image in;
	REQUIRE(u_desktop_image_alloc(&in, 4, 2));
	// Left 2x2 block: 0 and 200 in R, opaque. Right 2x2: one real pixel (R 100), three withheld.
	const uint8_t vals[4][4] = {{0, 0, 0, 255}, {200, 0, 0, 255}, {100, 50, 25, 255}, {0, 0, 0, 0}};
	auto set = [&](uint32_t x, uint32_t y, const uint8_t *v) { memcpy(in.rgba + (y * 4 + x) * 4, v, 4); };
	set(0, 0, vals[0]);
	set(1, 0, vals[1]);
	set(0, 1, vals[0]);
	set(1, 1, vals[1]);
	set(2, 0, vals[2]);
	set(3, 0, vals[3]);
	set(2, 1, vals[3]);
	set(3, 1, vals[3]);
	u_desktop_image out;
	REQUIRE(u_desktop_image_downscale(&in, 2, 1, &out));
	CHECK(out.rgba[0] == 100); // the mean of 0 and 200
	CHECK(out.rgba[3] == 255);
	CHECK(out.rgba[4] == 100); // only the real pixel's colour: never pulled to black
	CHECK(out.rgba[5] == 50);
	CHECK(out.rgba[7] == 64); // a quarter of the footprint is real
	u_desktop_image_free(&out);

	// Wholly withheld stays (0,0,0,0).
	memset(in.rgba + 2 * 4, 0, 8);
	memset(in.rgba + (4 + 2) * 4, 0, 8);
	REQUIRE(u_desktop_image_downscale(&in, 2, 1, &out));
	CHECK((out.rgba[4] == 0 && out.rgba[5] == 0 && out.rgba[6] == 0 && out.rgba[7] == 0));
	u_desktop_image_free(&out);
	u_desktop_image_free(&in);
}

TEST_CASE("capture_helpers: withholding rounds outwards", "[desktop][capture]")
{
	u_desktop_image img;
	REQUIRE(u_desktop_image_alloc(&img, 10, 10));
	memset(img.rgba, 255, 400);
	u_desktop_rect src = {0, 0, 20, 20}; // 2 desktop px per image px
	u_desktop_rect r = {3, 3, 2, 2};     // desktop 3..5 -> image px 1..2 (partially covered)
	u_desktop_image_withhold(&img, &src, &r);
	for (uint32_t y = 0; y < 10; y++) {
		for (uint32_t x = 0; x < 10; x++) {
			bool held = x >= 1 && x <= 2 && y >= 1 && y <= 2;
			CHECK((img.rgba[(y * 10 + x) * 4 + 3] == 0) == held);
		}
	}
	u_desktop_image_free(&img);
}

TEST_CASE("capture_scope: a v1 request is CONTROL only; CAPTURE has its own state and events",
          "[desktop][capture][state]")
{
	TempDir dir;
	Harness h;
	uint32_t caps = 0;
	u_desktop_control_get_state(h.c, &caps);
	CHECK((caps & U_DESKTOP_CAP_CAPTURE) != 0);
	h.grant();
	// No capture event for an app that never touched the scope.
	{
		std::lock_guard<std::mutex> g(h.rec.m);
		CHECK(h.rec.capture_states.empty());
	}
	CHECK(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_NOT_REQUESTED);
	u_desktop_capture_info info = cap_desktop();
	uint64_t id = 0;
	CHECK(u_desktop_control_capture(h.c, &info, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	CHECK(id == 0);

	grant_capture(h);
	CHECK(h.rec.wait_capture_state(U_DESKTOP_STATE_GRANTED));
	CHECK(h.state() == U_DESKTOP_STATE_GRANTED); // CONTROL untouched

	// The consent records name the scope.
	std::string audit = slurp(dir.file("desktop_control_audit.jsonl"));
	CHECK(audit.find("\"action\":\"request\",\"scope\":\"capture\"") != std::string::npos);
	CHECK(audit.find("\"action\":\"granted\",\"scope\":\"capture\"") != std::string::npos);
}

TEST_CASE("capture_scope: CAPTURE works without CONTROL; one prompt for both; GRANTED is not asked again",
          "[desktop][capture][state]")
{
	TempDir dir;
	Harness h;
	SECTION("capture alone")
	{
		grant_capture(h);
		CHECK(h.state() == U_DESKTOP_STATE_NOT_REQUESTED);
		Recorder::Capture c = capture(h, cap_desktop());
		CHECK(c.result == U_DESKTOP_XR_SUCCESS);
	}
	SECTION("both in one prompt, then nothing to ask")
	{
		u_desktop_fake_set_request_answer(h.fake, U_DESKTOP_ACCESS_GRANTED, true);
		REQUIRE(u_desktop_control_request_scopes(h.c, nullptr,
		                                         U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE) ==
		        U_DESKTOP_XR_SUCCESS);
		CHECK(h.state() == U_DESKTOP_STATE_PENDING);
		CHECK(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_PENDING);
		// A request while a listed scope is PENDING is a no-op.
		REQUIRE(u_desktop_control_request_scopes(h.c, nullptr, U_DESKTOP_SCOPE_CAPTURE) == U_DESKTOP_XR_SUCCESS);
		uint32_t n = 0;
		CHECK(u_desktop_fake_last_request_scopes(h.fake, &n) ==
		      (U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE));
		CHECK(n == 1);
		REQUIRE(u_desktop_fake_complete_request(h.fake));
		CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
		CHECK(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_GRANTED);
		uint32_t before = u_desktop_fake_request_count(h.fake);
		REQUIRE(u_desktop_control_request_scopes(h.c, nullptr,
		                                         U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE) ==
		        U_DESKTOP_XR_SUCCESS);
		CHECK(u_desktop_fake_request_count(h.fake) == before); // nothing asked again
	}
	SECTION("the dialog decides both: input unticked -> CONTROL DENIED, CAPTURE GRANTED")
	{
		h.grant();
		u_desktop_fake_set_request_answer(h.fake, U_DESKTOP_ACCESS_DENIED, false);
		u_desktop_fake_set_capture_request_answer(h.fake, U_DESKTOP_ACCESS_GRANTED, true);
		REQUIRE(u_desktop_control_request_scopes(h.c, nullptr, U_DESKTOP_SCOPE_CAPTURE) == U_DESKTOP_XR_SUCCESS);
		CHECK(u_desktop_fake_last_request_scopes(h.fake, nullptr) == U_DESKTOP_SCOPE_CAPTURE);
		CHECK(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_GRANTED);
		CHECK(h.state() == U_DESKTOP_STATE_DENIED);
	}
	SECTION("bad scopes")
	{
		CHECK(u_desktop_control_request_scopes(h.c, nullptr, 0) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
		CHECK(u_desktop_control_request_scopes(h.c, nullptr, 0x4) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
	}
}

TEST_CASE("capture_scope: no capture capability -> UNSUPPORTED and FEATURE_UNSUPPORTED", "[desktop][capture]")
{
	TempDir dir;
	Harness h;
	u_desktop_fake_set_caps(h.fake, U_DESKTOP_CAP_LIST_WINDOWS | U_DESKTOP_CAP_PICK | U_DESKTOP_CAP_INPUT);
	CHECK(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_UNSUPPORTED);
	REQUIRE(u_desktop_control_request_scopes(h.c, nullptr, U_DESKTOP_SCOPE_CAPTURE) == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_request_count(h.fake) == 0);
	CHECK(h.rec.wait_capture_state(U_DESKTOP_STATE_UNSUPPORTED));
	u_desktop_capture_info info = cap_desktop();
	uint64_t id = 0;
	CHECK(u_desktop_control_capture(h.c, &info, &id) == U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED);
}

TEST_CASE("capture_scope: the store answers per scope; Windows-model prompt writes the CAPTURE answer",
          "[desktop][capture][store]")
{
	TempDir dir;
	Harness h;
	SECTION("a capture deny leaves CONTROL alone")
	{
		REQUIRE(u_desktop_policy_set_app_scope(kExe, U_DESKTOP_SCOPE_CAPTURE, U_DESKTOP_APP_DENY, "test"));
		h.grant();
		CHECK(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_DISABLED_BY_USER);
		REQUIRE(u_desktop_control_request_scopes(h.c, nullptr, U_DESKTOP_SCOPE_CAPTURE) == U_DESKTOP_XR_SUCCESS);
		CHECK(u_desktop_fake_last_request_scopes(h.fake, nullptr) == 0); // never prompted
		CHECK(h.state() == U_DESKTOP_STATE_GRANTED);
	}
	SECTION("remembers_in_store: allow written per scope, a remembered allow needs no prompt")
	{
		u_desktop_fake_set_remembers_in_store(h.fake, true);
		grant_capture(h);
		u_desktop_policy *pol = u_desktop_policy_create();
		CHECK(u_desktop_policy_app_scope(pol, kExe, U_DESKTOP_SCOPE_CAPTURE) == U_DESKTOP_APP_ALLOW);
		CHECK(u_desktop_policy_app(pol, kExe) == U_DESKTOP_APP_NONE);
		u_desktop_policy_destroy(&pol);

		Harness h2;
		u_desktop_fake_set_remembers_in_store(h2.fake, true);
		REQUIRE(u_desktop_control_request_scopes(h2.c, nullptr, U_DESKTOP_SCOPE_CAPTURE) ==
		        U_DESKTOP_XR_SUCCESS);
		CHECK(u_desktop_fake_request_count(h2.fake) == 0);
		CHECK(u_desktop_control_get_capture_state(h2.c) == U_DESKTOP_STATE_GRANTED);
		// `desktop forget --scope capture` revokes it at once.
		REQUIRE(u_desktop_policy_set_app_scope(kExe, U_DESKTOP_SCOPE_CAPTURE, U_DESKTOP_APP_NONE, "test"));
		CHECK(u_desktop_control_get_capture_state(h2.c) == U_DESKTOP_STATE_DENIED);
	}
}

TEST_CASE("capture_store: v1 entries are read as is; a write makes the file v2, strings stay strings",
          "[desktop][capture][store]")
{
	TempDir dir;
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << "{\"version\":1,\"suspended\":false,\"idle_ms\":600,\"apps\":{\"/a\":\"deny\",\"/b\":\"allow\"}}";
	}
	u_desktop_policy *p = u_desktop_policy_create();
	CHECK(u_desktop_policy_app(p, "/a") == U_DESKTOP_APP_DENY);
	CHECK(u_desktop_policy_app_scope(p, "/a", U_DESKTOP_SCOPE_CAPTURE) == U_DESKTOP_APP_NONE);
	u_desktop_policy_destroy(&p);

	REQUIRE(u_desktop_policy_set_app_scope("/b", U_DESKTOP_SCOPE_CAPTURE, U_DESKTOP_APP_DENY, "test"));
	std::string txt = slurp(dir.file("desktop_control.json"));
	CHECK(txt.find("\"version\":\t2") != std::string::npos);
	CHECK(txt.find("\"/a\":\t\"deny\"") != std::string::npos); // untouched v1 entry stays a string
	p = u_desktop_policy_create();
	CHECK(u_desktop_policy_app(p, "/b") == U_DESKTOP_APP_ALLOW);
	CHECK(u_desktop_policy_app_scope(p, "/b", U_DESKTOP_SCOPE_CAPTURE) == U_DESKTOP_APP_DENY);
	u_desktop_policy_destroy(&p);

	// Forgetting the capture answer turns the entry back into the v1 string.
	REQUIRE(u_desktop_policy_set_app_scope("/b", U_DESKTOP_SCOPE_CAPTURE, U_DESKTOP_APP_NONE, "test"));
	txt = slurp(dir.file("desktop_control.json"));
	CHECK(txt.find("\"/b\":\t\"allow\"") != std::string::npos);
	// Forget all removes the entry.
	REQUIRE(u_desktop_policy_set_app_scope("/b", U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE,
	                                       U_DESKTOP_APP_NONE, "test"));
	txt = slurp(dir.file("desktop_control.json"));
	CHECK(txt.find("\"/b\"") == std::string::npos);
	std::string audit = slurp(dir.file("desktop_control_audit.jsonl"));
	CHECK(audit.find("\"action\":\"deny\",\"subject\":\"/b\",\"scope\":\"capture\"") != std::string::npos);
}

TEST_CASE("capture_validate: submit-time validation (spec §10.3)", "[desktop][capture][validate]")
{
	TempDir dir;
	Harness h;
	grant_capture(h);
	auto bad = [&](u_desktop_capture_info i) {
		uint64_t id = 7;
		CHECK(u_desktop_control_capture(h.c, &i, &id) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
		CHECK(id == 0);
	};
	u_desktop_capture_info i = cap_desktop();
	i.target = 3;
	bad(i);
	bad(cap_desktop(0, 100));
	bad(cap_desktop(100, 8193));
	bad(cap_region(0, 0, 0, 10));
	bad(cap_region(0, 0, 10, -1));
	bad(cap_region(2000000, 0, 10, 10));
	bad(cap_window(0));
	CHECK(u_desktop_control_capture(h.c, nullptr, nullptr) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
	CHECK(u_desktop_fake_capture_count(h.fake) == 0);
}

TEST_CASE("capture_modes: desktop, region (clipped), window frame as on screen, and the mapping back",
          "[desktop][capture]")
{
	TempDir dir;
	Harness h;
	grant_capture(h);
	std::vector<uint8_t> img;

	SECTION("desktop: every monitor's bounding box, 1:1")
	{
		Recorder::Capture c = capture(h, cap_desktop(), &img);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		CHECK((c.w == 1920 && c.h == 1080));
		CHECK((c.source.x == 0 && c.source.y == 0 && c.source.w == 1920 && c.source.h == 1080));
		CHECK(c.frame_ns != 0);
		// Every pixel maps back to the desktop point it shows.
		for (uint32_t y = 0; y < c.h; y += 37) {
			for (uint32_t x = 0; x < c.w; x += 41) {
				int32_t dx, dy;
				map_back(c, x, y, &dx, &dy);
				uint8_t want[4];
				u_desktop_fake_expected_pixel(h.fake, dx, dy, want);
				REQUIRE(memcmp(px(img, c, x, y), want, 4) == 0);
			}
		}
	}
	SECTION("region: clipped to the monitors; wholly outside is refused")
	{
		Recorder::Capture c = capture(h, cap_region(1800, 1000, 400, 400), &img);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		CHECK((c.source.x == 1800 && c.source.y == 1000 && c.source.w == 120 && c.source.h == 80));
		CHECK((c.w == 120 && c.h == 80));
		std::this_thread::sleep_for(260ms);
		c = capture(h, cap_region(3000, 0, 100, 100));
		CHECK(c.result == U_DESKTOP_XR_ERROR_REFUSED);
		CHECK(c.message == "found nothing: outside every monitor");
		CHECK((c.w == 0 && c.h == 0));
	}
	SECTION("window: its frame as on screen — the window on top of it is in the image")
	{
		// 101 (100,100 800x600) is on top of 102 (1000,200 800x600)? No: they do not
		// overlap; move 101 over 102's corner.
		u_desktop_window w;
		memset(&w, 0, sizeof(w));
		w.id = 104;
		w.pid = 1004;
		snprintf(w.app_id, sizeof(w.app_id), "popup");
		w.frame = {1100, 250, 100, 100};
		u_desktop_fake_add_window(h.fake, &w);
		Recorder::Capture c = capture(h, cap_window(102), &img);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		CHECK((c.source.x == 1000 && c.source.y == 200 && c.source.w == 800 && c.source.h == 600));
		const uint8_t *p = px(img, c, 150, 100); // desktop (1150, 300): the popup
		CHECK((p[0] == 0x10 && p[1] == 104 && p[3] == 255));
		p = px(img, c, 10, 10); // desktop (1010, 210): window 102 itself
		CHECK((p[0] == 0x10 && p[1] == 102));
	}
	SECTION("window: gone / minimized")
	{
		Recorder::Capture c = capture(h, cap_window(999));
		CHECK(c.result == U_DESKTOP_XR_ERROR_WINDOW_LOST);
		u_desktop_window w;
		memset(&w, 0, sizeof(w));
		w.id = 105;
		w.pid = 1005;
		w.state = U_DESKTOP_WINDOW_MINIMIZED;
		w.frame = {10, 10, 100, 100};
		u_desktop_fake_add_window(h.fake, &w);
		std::this_thread::sleep_for(260ms);
		c = capture(h, cap_window(105));
		CHECK(c.result == U_DESKTOP_XR_ERROR_REFUSED);
		CHECK(c.message == "the window is minimized");
	}
}

TEST_CASE("capture_scale: scaled down to fit, never up; device px on a HiDPI monitor; mapping holds",
          "[desktop][capture]")
{
	TempDir dir;
	Harness h;
	grant_capture(h);
	std::vector<uint8_t> img;

	SECTION("downscale to the max size keeps the aspect ratio")
	{
		Recorder::Capture c = capture(h, cap_desktop(960, 960), &img);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		CHECK((c.w == 960 && c.h == 540));
		CHECK((c.source.w == 1920 && c.source.h == 1080));
		// Background at desktop (1900..1901, 40..41): R averages x & 0xff of the two columns.
		int32_t dx, dy;
		map_back(c, 950, 20, &dx, &dy);
		CHECK((dx == 1901 && dy == 41)); // the footprint is 1900..1901 x 40..41
		const uint8_t *p = px(img, c, 950, 20);
		uint8_t a[4], b[4];
		u_desktop_fake_expected_pixel(h.fake, 1900, 40, a);
		u_desktop_fake_expected_pixel(h.fake, 1901, 41, b);
		CHECK(std::abs((int)p[0] - (a[0] + b[0] + 1) / 2) <= 1);
		CHECK(std::abs((int)p[1] - (a[1] + b[1] + 1) / 2) <= 1);
		CHECK(p[3] == 255);
	}
	SECTION("a small region is never scaled up")
	{
		Recorder::Capture c = capture(h, cap_region(950, 50, 30, 20, 4000, 4000), &img);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		CHECK((c.w == 30 && c.h == 20));
	}
	SECTION("a 200 % monitor: device px, and the mapping still lands")
	{
		u_desktop_fake_monitor m[2] = {{{0, 0, 1920, 1080}, 1, true}, {{1920, 0, 1280, 800}, 2, true}};
		u_desktop_fake_set_monitors(h.fake, m, 2);
		Recorder::Capture c = capture(h, cap_region(2000, 100, 100, 50), &img);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		CHECK((c.w == 200 && c.h == 100)); // device px
		for (uint32_t y = 0; y < c.h; y += 7) {
			for (uint32_t x = 0; x < c.w; x += 9) {
				int32_t dx, dy;
				map_back(c, x, y, &dx, &dy);
				uint8_t want[4];
				u_desktop_fake_expected_pixel(h.fake, dx, dy, want);
				REQUIRE(memcmp(px(img, c, x, y), want, 4) == 0);
			}
		}
		// The whole desktop: the bounding box of both, at the densest scale; the
		// part below the smaller monitor is on no monitor -> withheld.
		std::this_thread::sleep_for(260ms);
		c = capture(h, cap_desktop(), &img);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		CHECK((c.source.w == 3200 && c.source.h == 1080));
		CHECK((c.w == 6400 && c.h == 2160));
		const uint8_t *off = px(img, c, 2 * 2500, 2 * 1000); // desktop (2500, 1000): no monitor
		CHECK((off[0] == 0 && off[3] == 0));
	}
}

TEST_CASE("capture_privacy: own windows and capture-excluded windows are withheld; unknown exclusion refuses",
          "[desktop][capture][privacy]")
{
	TempDir dir;
	Harness h;
	grant_capture(h);
	std::vector<uint8_t> img;

	u_desktop_window own;
	memset(&own, 0, sizeof(own));
	own.id = 200;
	own.pid = 4242;
	own.state = U_DESKTOP_WINDOW_OWN;
	own.frame = {1300, 850, 200, 100};
	u_desktop_fake_add_window(h.fake, &own);
	u_desktop_fake_set_capture_excluded(h.fake, 102, true);

	Recorder::Capture c = capture(h, cap_desktop(), &img);
	REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
	auto at = [&](int32_t x, int32_t y) { return px(img, c, (uint32_t)x, (uint32_t)y); };
	// Own window: whole frame (0,0,0,0).
	CHECK((at(1300, 850)[3] == 0 && at(1499, 949)[3] == 0 && at(1400, 900)[0] == 0));
	// Excluded window 102 (1000,200 800x600): whole frame, even where it is visible.
	CHECK((at(1000, 200)[3] == 0 && at(1799, 799)[3] == 0 && at(1500, 500)[1] == 0));
	// Neighbours untouched.
	CHECK(at(999, 199)[3] == 255);
	CHECK(at(1299, 849)[3] == 255);

	std::this_thread::sleep_for(260ms);
	u_desktop_fake_set_exclusion_unknown(h.fake, true);
	c = capture(h, cap_desktop());
	CHECK(c.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(c.message == "cannot honour capture exclusion");
	uint32_t n = 0;
	CHECK(u_desktop_control_capture_image(h.c, c.id, 0, &n, nullptr) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);

	SECTION("an unshared monitor is withheld")
	{
		u_desktop_fake_set_exclusion_unknown(h.fake, false);
		u_desktop_fake_monitor m[2] = {{{0, 0, 1920, 1080}, 1, true}, {{1920, 0, 1920, 1080}, 1, false}};
		u_desktop_fake_set_monitors(h.fake, m, 2);
		std::this_thread::sleep_for(260ms);
		c = capture(h, cap_region(1900, 500, 40, 10), &img);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		CHECK(px(img, c, 10, 5)[3] == 255); // desktop 1910: shared
		CHECK(px(img, c, 30, 5)[3] == 0);   // desktop 1930: not shared
	}
}

TEST_CASE("capture_limits: one in flight, 4 Hz start cap, retention", "[desktop][capture][limits]")
{
	TempDir dir;
	Harness h;
	fast_capture_timing(h);
	grant_capture(h);
	u_desktop_capture_info info = cap_region(0, 0, 64, 64);

	SECTION("a second capture while one is pending is LIMIT_REACHED")
	{
		u_desktop_fake_set_capture_delay_ms(h.fake, 200);
		uint64_t a = 0, b = 0;
		REQUIRE(u_desktop_control_capture(h.c, &info, &a) == U_DESKTOP_XR_SUCCESS);
		CHECK(u_desktop_control_capture(h.c, &info, &b) == U_DESKTOP_XR_ERROR_LIMIT_REACHED);
		CHECK(b == 0);
		REQUIRE(h.rec.wait_captures(1));
		REQUIRE(u_desktop_control_capture(h.c, &info, &b) == U_DESKTOP_XR_SUCCESS);
		CHECK(b != a);
		REQUIRE(h.rec.wait_captures(2));
	}
	SECTION("starts are at least 250 ms apart; an early call is accepted and waits")
	{
		std::vector<Clock::time_point> starts;
		std::mutex m;
		auto hook = [](void *ctx, const char *op, uint32_t) {
			if (strcmp(op, "capture") == 0) {
				auto *v = static_cast<std::pair<std::mutex *, std::vector<Clock::time_point> *> *>(ctx);
				std::lock_guard<std::mutex> g(*v->first);
				v->second->push_back(Clock::now());
			}
		};
		std::pair<std::mutex *, std::vector<Clock::time_point> *> ctx{&m, &starts};
		u_desktop_fake_set_batch_hook(h.fake, hook, &ctx);
		for (int i = 0; i < 3; i++) {
			Recorder::Capture c = capture(h, info);
			CHECK(c.result == U_DESKTOP_XR_SUCCESS);
		}
		u_desktop_fake_set_batch_hook(h.fake, nullptr, nullptr);
		REQUIRE(starts.size() == 3);
		for (size_t i = 1; i < starts.size(); i++) {
			CHECK(starts[i] - starts[i - 1] >= 245ms);
		}
	}
	SECTION("retention: size queries keep it, a copy ends it, other ids fail, it expires")
	{
		std::vector<uint8_t> img;
		Recorder::Capture c = capture(h, info);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		uint32_t n = 0;
		CHECK(u_desktop_control_capture_image(h.c, c.id + 1, 0, &n, nullptr) ==
		      U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
		CHECK(u_desktop_control_capture_image(h.c, c.id, 0, &n, nullptr) == U_DESKTOP_XR_SUCCESS);
		CHECK(n == 64 * 64 * 4);
		img.assign(n, 0);
		CHECK(u_desktop_control_capture_image(h.c, c.id, n - 1, &n, img.data()) ==
		      U_DESKTOP_XR_ERROR_SIZE_INSUFFICIENT);
		CHECK(u_desktop_control_capture_image(h.c, c.id, n, &n, img.data()) == U_DESKTOP_XR_SUCCESS);
		CHECK(img[3] == 255);
		CHECK(u_desktop_control_capture_image(h.c, c.id, n, &n, img.data()) ==
		      U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);

		// Expiry (300 ms in this harness; 10 s in the spec).
		c = capture(h, info);
		REQUIRE(c.result == U_DESKTOP_XR_SUCCESS);
		std::this_thread::sleep_for(400ms);
		CHECK(u_desktop_control_capture_image(h.c, c.id, 0, &n, nullptr) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);

		// The next capture replaces the last one.
		c = capture(h, info);
		uint64_t first = c.id;
		std::this_thread::sleep_for(260ms);
		c = capture(h, info);
		CHECK(u_desktop_control_capture_image(h.c, first, 0, &n, nullptr) == U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
		CHECK(u_desktop_control_capture_image(h.c, c.id, 0, &n, nullptr) == U_DESKTOP_XR_SUCCESS);
	}
}

TEST_CASE("capture_safety: kill switch, lock and deny refuse; a frame finished as they land is dropped",
          "[desktop][capture][safety]")
{
	TempDir dir;
	Harness h;
	grant_capture(h);
	u_desktop_capture_info info = cap_region(0, 0, 32, 32);

	SECTION("kill switch / lock at submit: NOT_GRANTED, the backend is never reached")
	{
		REQUIRE(u_desktop_policy_set_suspended(true, "test"));
		uint64_t id = 0;
		CHECK(u_desktop_control_capture(h.c, &info, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
		CHECK(h.rec.wait_capture_state(U_DESKTOP_STATE_SUSPENDED));
		REQUIRE(u_desktop_policy_set_suspended(false, "test"));
		u_desktop_fake_set_locked(h.fake, true);
		CHECK(u_desktop_control_capture(h.c, &info, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
		CHECK(u_desktop_fake_capture_count(h.fake) == 0);
		u_desktop_fake_set_locked(h.fake, false);
		CHECK(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_GRANTED);
	}
	struct Flip
	{
		int what; // 0 kill switch, 1 lock, 2 capture deny
		u_desktop_fake *fake;
	};
	auto hook = [](void *ctx, const char *op, uint32_t) {
		auto *fl = static_cast<Flip *>(ctx);
		if (strcmp(op, "captured") != 0) {
			return;
		}
		if (fl->what == 0) {
			u_desktop_policy_set_suspended(true, "test");
		} else if (fl->what == 1) {
			u_desktop_fake_set_locked(fl->fake, true);
		} else {
			u_desktop_policy_set_app_scope(kExe, U_DESKTOP_SCOPE_CAPTURE, U_DESKTOP_APP_DENY, "test");
		}
	};
	const char *expect[3] = {"kill switch", "screen locked", "disabled by the user"};
	for (int what = 0; what < 3; what++) {
		DYNAMIC_SECTION("between the capture and the hand-over: " << expect[what])
		{
			Flip fl{what, h.fake};
			u_desktop_fake_set_batch_hook(h.fake, hook, &fl);
			Recorder::Capture c = capture(h, info);
			u_desktop_fake_set_batch_hook(h.fake, nullptr, nullptr);
			CHECK(u_desktop_fake_capture_count(h.fake) == 1); // the OS capture happened ...
			CHECK(c.result == U_DESKTOP_XR_ERROR_REFUSED);    // ... and the frame was dropped
			CHECK(c.message == expect[what]);
			CHECK((c.w == 0 && c.h == 0));
			uint32_t n = 0;
			CHECK(u_desktop_control_capture_image(h.c, c.id, 0, &n, nullptr) ==
			      U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
		}
	}
	SECTION("the OS revokes it: NOT_GRANTED, CAPTURE DENIED until requested again")
	{
		u_desktop_fake_set_capture_result(h.fake, U_DESKTOP_ERR_NOT_GRANTED);
		Recorder::Capture c = capture(h, info);
		CHECK(c.result == U_DESKTOP_XR_ERROR_NOT_GRANTED);
		CHECK(h.rec.wait_capture_state(U_DESKTOP_STATE_DENIED));
		CHECK(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_DENIED);
		uint64_t id = 0;
		CHECK(u_desktop_control_capture(h.c, &info, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
		u_desktop_fake_set_capture_result(h.fake, U_DESKTOP_OK);
		grant_capture(h);
	}
}

TEST_CASE("capture_audit: one record per burst, failures on their own, never a pixel", "[desktop][capture][audit]")
{
	TempDir dir;
	Harness h;
	fast_capture_timing(h);
	grant_capture(h);
	u_desktop_capture_info info = cap_region(100, 100, 64, 32);
	for (int i = 0; i < 3; i++) {
		REQUIRE(capture(h, info).result == U_DESKTOP_XR_SUCCESS);
	}
	// A failure closes the burst and gets its own record.
	REQUIRE(capture(h, cap_region(5000, 5000, 10, 10)).result == U_DESKTOP_XR_ERROR_REFUSED);
	REQUIRE(capture(h, info).result == U_DESKTOP_XR_SUCCESS);
	std::this_thread::sleep_for(900ms); // > the 600 ms burst gap of this harness

	std::vector<std::string> caps;
	for (const std::string &l : audit_lines(dir)) {
		if (l.find("\"action\":\"capture\"") != std::string::npos) {
			caps.push_back(l);
		}
	}
	REQUIRE(caps.size() == 3);
	CHECK(caps[0].find("\"count\":3") != std::string::npos);
	CHECK(caps[0].find("\"kind\":\"region\"") != std::string::npos);
	CHECK(caps[0].find("\"rect\":[100,100,64,32]") != std::string::npos);
	CHECK(caps[0].find("\"w\":64,\"h\":32") != std::string::npos);
	CHECK(caps[0].find("\"first\"") != std::string::npos);
	CHECK(caps[0].find("\"last\"") != std::string::npos);
	CHECK(caps[0].find("\"result\":\"ok\"") != std::string::npos);
	CHECK(caps[1].find("\"result\":\"refused\"") != std::string::npos);
	CHECK(caps[1].find("outside every monitor") != std::string::npos);
	CHECK(caps[2].find("\"count\":1") != std::string::npos);
	for (const std::string &l : caps) {
		// Only what, how big, how often: no pixel data, no hash, no thumbnail.
		CHECK(l.size() < 600);
		CHECK(l.find("rgba") == std::string::npos);
		CHECK(l.find("hash") == std::string::npos);
	}
}

TEST_CASE("capture_lifecycle: destroy with a capture in flight drops it without an event", "[desktop][capture]")
{
	TempDir dir;
	auto *h = new Harness();
	grant_capture(*h);
	u_desktop_fake_set_capture_delay_ms(h->fake, 150);
	u_desktop_capture_info info = cap_desktop();
	uint64_t id = 0;
	REQUIRE(u_desktop_control_capture(h->c, &info, &id) == U_DESKTOP_XR_SUCCESS);
	std::this_thread::sleep_for(30ms);
	u_desktop_control_destroy(&h->c);
	CHECK(h->rec.capture_count() == 0);
	delete h;
}

#include "desktop/u_desktop_spa_pod.h"

TEST_CASE("capture_spa: the hand-built pods are well formed and the format parses back", "[desktop][capture][spa]")
{
	alignas(8) uint8_t buf[U_SPA_POD_BUF];
	size_t n = u_spa_build_enum_format(buf, sizeof(buf));
	REQUIRE(n > 0);
	CHECK(n % 8 == 0);
	uint32_t size = 0, type = 0;
	memcpy(&size, buf, 4);
	memcpy(&type, buf + 4, 4);
	CHECK(size + 8 == n);
	CHECK(type == U_SPA_TYPE_Object);
	// The parser reads a Choice as its default: 1920x1080 BGRx.
	uint32_t w = 0, h = 0, fmt = 0;
	REQUIRE(u_spa_parse_video_format(buf, n, &w, &h, &fmt));
	CHECK((w == 1920 && h == 1080 && fmt == U_SPA_VIDEO_FORMAT_BGRx));
	CHECK(u_spa_build_enum_format(buf, 40) == 0); // never overflows
	n = u_spa_build_buffers(buf, sizeof(buf));
	REQUIRE(n > 0);
	CHECK(n % 8 == 0);
	CHECK(!u_spa_parse_video_format(buf, n, &w, &h, &fmt)); // not a Format object
}

TEST_CASE("capture_scope: ASK_AGAIN re-asks a GRANTED scope; the store's allow does not short-circuit it",
          "[desktop][capture][state]")
{
	TempDir dir;
	Harness h;
	grant_capture(h);
	uint32_t before = u_desktop_fake_request_count(h.fake);
	REQUIRE(u_desktop_control_request_scopes(h.c, nullptr, U_DESKTOP_SCOPE_CAPTURE) == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_request_count(h.fake) == before);
	u_desktop_fake_set_capture_request_answer(h.fake, U_DESKTOP_ACCESS_DENIED, false);
	REQUIRE(u_desktop_control_request_scopes(h.c, nullptr, U_DESKTOP_SCOPE_CAPTURE | U_DESKTOP_SCOPE_REASK) ==
	        U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_request_count(h.fake) == before + 1);
	CHECK((u_desktop_fake_last_request_scopes(h.fake, nullptr) & U_DESKTOP_SCOPE_REASK) != 0);
	CHECK(u_desktop_control_get_capture_state(h.c) == U_DESKTOP_STATE_DENIED); // the user declined this time
	CHECK(u_desktop_control_request_scopes(h.c, nullptr, U_DESKTOP_SCOPE_REASK) ==
	      U_DESKTOP_XR_ERROR_VALIDATION_FAILURE); // a modifier alone asks for nothing
}
