// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief XR_DXR_desktop_control spec version 3 (ADR-056 Amendment 3, proposed) — the
 *        common layer over the in-memory fake backend: held keys and buttons
 *        (KEY_DOWN / KEY_UP / BUTTON_DOWN / BUTTON_UP), modifiers around a pointer
 *        action, and the automatic release of everything held.
 *
 * Built into tests_desktop_control (its own file so the spec v1/v2 suite stays as
 * it is). Same rules: every case points DXR_DESKTOP_CONTROL_DIR at a fresh temp
 * directory and drives u_desktop_fake; nothing touches the real desktop.
 */

#include "desktop/u_desktop_chord.h"
#include "desktop/u_desktop_control.h"
#include "desktop/u_desktop_fake.h"
#include "desktop/u_desktop_policy.h"

#include "catch_amalgamated.hpp"

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
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

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
#ifdef _WIN32
		static int counter = 0;
		char base[MAX_PATH];
		GetTempPathA(MAX_PATH, base);
		path = std::string(base) + "dxr_dc3_test_" + std::to_string(GetCurrentProcessId()) + "_" +
		       std::to_string(++counter);
		CreateDirectoryA(path.c_str(), nullptr);
#else
		const char *t = getenv("TMPDIR");
		std::string tmpl = std::string(t != nullptr && t[0] != '\0' ? t : "/tmp") + "/dxr_dc3_test_XXXXXX";
		std::vector<char> buf(tmpl.begin(), tmpl.end());
		buf.push_back('\0');
		char *p = mkdtemp(buf.data());
		REQUIRE(p != nullptr);
		path = p;
#endif
		set_env(U_DESKTOP_POLICY_DIR_ENV, path.c_str());
		set_env("DXR_DESKTOP_CONTROL", nullptr);
	}

	~TempDir()
	{
		for (const char *n : {"desktop_control.json", "desktop_control.json.lock", "desktop_control_audit.jsonl",
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

	std::string
	audit() const
	{
		std::ifstream f(file("desktop_control_audit.jsonl"), std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}

	//! The audit log once it contains @p needle (records are appended asynchronously,
	//! e.g. an automatic release right after its key-ups); "" after @p timeout.
	bool
	audit_has(const char *needle, std::chrono::milliseconds timeout = 2000ms) const
	{
		auto end = std::chrono::steady_clock::now() + timeout;
		do {
			if (audit().find(needle) != std::string::npos) {
				return true;
			}
			std::this_thread::sleep_for(5ms);
		} while (std::chrono::steady_clock::now() < end);
		return false;
	}
};

//! Every completion, in order; waits on them.
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

	bool
	wait_done(size_t n, std::chrono::milliseconds timeout = 5000ms)
	{
		std::unique_lock<std::mutex> l(m);
		return cv.wait_for(l, timeout, [&] { return done.size() >= n; });
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
const u_desktop_control_timing kFast = {5ull * 1000000ull, 0, 0};

//! A granted control over a fresh fake.
struct Harness
{
	Recorder rec;
	u_desktop_backend *backend = nullptr;
	u_desktop_fake *fake = nullptr;
	u_desktop_control *c = nullptr;

	explicit Harness(const u_desktop_control_timing *timing = &kFast)
	{
		backend = u_desktop_fake_create();
		fake = u_desktop_fake_from_backend(backend);
		u_desktop_app_info app = {kExe, "agent-app", nullptr, nullptr};
		u_desktop_control_callbacks cb = {Recorder::on_state, Recorder::on_done, &rec, nullptr, nullptr};
		REQUIRE(u_desktop_control_create(backend, &app, &cb, timing, &c) == 0);
		REQUIRE(u_desktop_control_request(c, "tests") == U_DESKTOP_XR_SUCCESS);
		REQUIRE(u_desktop_control_get_state(c, nullptr) == U_DESKTOP_STATE_GRANTED);
	}

	~Harness()
	{
		u_desktop_control_destroy(&c);
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

	std::string
	held_keys()
	{
		char buf[256];
		u_desktop_fake_held_keys(fake, buf, sizeof(buf));
		return buf;
	}

	//! Wait until the fake holds nothing (keys and buttons).
	bool
	wait_released(std::chrono::milliseconds timeout = 3000ms)
	{
		auto end = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < end) {
			if (held_keys().empty() && u_desktop_fake_pressed_buttons(fake) == 0) {
				return true;
			}
			std::this_thread::sleep_for(5ms);
		}
		return held_keys().empty() && u_desktop_fake_pressed_buttons(fake) == 0;
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
key_down(uint64_t target, const char *keys)
{
	u_desktop_action a = act(U_DESKTOP_ACTION_KEY_DOWN, target);
	a.keys = keys;
	return a;
}

u_desktop_action
key_up(uint64_t target, const char *keys)
{
	u_desktop_action a = act(U_DESKTOP_ACTION_KEY_UP, target);
	a.keys = keys;
	return a;
}

u_desktop_action
button(uint32_t type, uint64_t target, int32_t x, int32_t y, uint32_t btn = U_DESKTOP_BUTTON_LEFT)
{
	u_desktop_action a = act(type, target);
	a.x = x;
	a.y = y;
	a.button = btn;
	return a;
}

u_desktop_action
click(uint64_t target, int32_t x, int32_t y, uint32_t mods = 0)
{
	u_desktop_action a = button(U_DESKTOP_ACTION_CLICK, target, x, y);
	a.mods = mods;
	return a;
}

u_desktop_action
chord(uint64_t target, const char *keys)
{
	u_desktop_action a = act(U_DESKTOP_ACTION_KEY_CHORD, target);
	a.keys = keys;
	return a;
}

bool
valid(const u_desktop_action &a)
{
	char why[160];
	return u_desktop_control_validate(&a, why, sizeof(why));
}

//! Lines of @p log that contain @p needle, in order.
std::vector<std::string>
lines_with(const std::string &log, const char *needle)
{
	std::vector<std::string> out;
	std::istringstream ss(log);
	for (std::string l; std::getline(ss, l);) {
		if (l.find(needle) != std::string::npos) {
			out.push_back(l);
		}
	}
	return out;
}

} // namespace


/*
 *
 * Item 1: held input — KEY_DOWN / KEY_UP / BUTTON_DOWN / BUTTON_UP, modifiers.
 *
 */

TEST_CASE("v3_validate: KEY_DOWN / KEY_UP name one key or one modifier", "[desktop][v3][validate]")
{
	CHECK(valid(key_down(101, "shift")));
	CHECK(valid(key_down(101, "Alt")));
	CHECK(valid(key_down(101, "tab")));
	CHECK(valid(key_down(101, "a")));
	CHECK(valid(key_up(101, "f5")));
	CHECK(valid(key_up(101, "super")));
	CHECK_FALSE(valid(key_down(101, "ctrl+a")));     // a chord: two keys
	CHECK_FALSE(valid(key_down(101, "ctrl+shift"))); // two modifiers
	CHECK_FALSE(valid(key_down(101, "")));
	CHECK_FALSE(valid(key_down(101, nullptr)));
	CHECK_FALSE(valid(key_up(101, "esc"))); // not a canonical name
	CHECK_FALSE(valid(key_down(0, "shift"))); // a target is required

	// The message never echoes the input.
	char why[160];
	u_desktop_action a = key_down(101, "secret+x");
	CHECK_FALSE(u_desktop_control_validate(&a, why, sizeof(why)));
	CHECK(std::string(why).find("secret") == std::string::npos);
}

TEST_CASE("v3_validate: BUTTON_DOWN / BUTTON_UP and the modifiers field", "[desktop][v3][validate]")
{
	CHECK(valid(button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 200, 200, U_DESKTOP_BUTTON_RIGHT)));
	CHECK(valid(button(U_DESKTOP_ACTION_BUTTON_UP, 101, 200, 200, U_DESKTOP_BUTTON_MIDDLE)));
	CHECK_FALSE(valid(button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 200, 200, 3)));
	CHECK_FALSE(valid(button(U_DESKTOP_ACTION_BUTTON_UP, 101, 2000000, 200)));

	CHECK(valid(click(101, 200, 200, U_DESKTOP_MOD_CTRL | U_DESKTOP_MOD_SHIFT)));
	u_desktop_action d = button(U_DESKTOP_ACTION_DRAG, 101, 200, 200);
	d.mods = U_DESKTOP_MOD_ALT;
	CHECK(valid(d));
	u_desktop_action s = button(U_DESKTOP_ACTION_SCROLL, 101, 200, 200);
	s.scroll_y = 2;
	s.mods = U_DESKTOP_MOD_CTRL;
	CHECK(valid(s));
	CHECK_FALSE(valid(click(101, 200, 200, 0x10))); // unknown bit
	u_desktop_action k = chord(101, "a");
	k.mods = U_DESKTOP_MOD_CTRL;
	CHECK_FALSE(valid(k)); // modifiers on a non-pointer action
	u_desktop_action bd = button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 200, 200);
	bd.mods = U_DESKTOP_MOD_SHIFT;
	CHECK_FALSE(valid(bd)); // a held press takes its modifiers from KEY_DOWN
}

TEST_CASE("v3_keys: KEY_DOWN holds, KEY_UP releases, the key is verified like a chord", "[desktop][v3][hold]")
{
	TempDir dir;
	Harness h;
	auto r = h.run(key_down(101, "shift"));
	CHECK(r.result == U_DESKTOP_XR_SUCCESS);
	CHECK(r.message == "ok");
	CHECK(h.held_keys() == "shift");
	CHECK(h.run(key_up(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.held_keys().empty());
	CHECK(lines_with(h.log(), "key") == std::vector<std::string>{"keydown shift", "keyup shift"});

	// Not held: a release of nothing is refused (it says so) and sends nothing.
	r = h.run(key_up(101, "shift"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "the key is not held");

	// Pressed twice: refused, the first press stands.
	CHECK(h.run(key_down(101, "a")).result == U_DESKTOP_XR_SUCCESS);
	r = h.run(key_down(101, "a"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "the key is already held");
	CHECK(h.run(key_up(101, "a")).result == U_DESKTOP_XR_SUCCESS);

	// The keyboard check: the target must hold focus.
	r = h.run(key_down(102, "ctrl"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message.find("does not have focus") != std::string::npos);
	CHECK(h.held_keys().empty());
}

TEST_CASE("v3_keys: Alt held across several Tabs, then released", "[desktop][v3][hold]")
{
	TempDir dir;
	Harness h;
	std::vector<uint64_t> ids;
	ids.push_back(h.submit(key_down(101, "alt")));
	for (int i = 0; i < 3; i++) {
		ids.push_back(h.submit(key_down(101, "tab")));
		ids.push_back(h.submit(key_up(101, "tab")));
	}
	ids.push_back(h.submit(key_up(101, "alt")));
	REQUIRE(h.rec.wait_done(ids.size()));
	for (uint64_t id : ids) {
		INFO(h.rec.completion(id).message);
		CHECK(h.rec.completion(id).result == U_DESKTOP_XR_SUCCESS);
	}
	CHECK(lines_with(h.log(), "key") ==
	      std::vector<std::string>{"keydown alt", "keydown tab", "keyup tab", "keydown tab", "keyup tab",
	                               "keydown tab", "keyup tab", "keyup alt"});
	CHECK(h.held_keys().empty());
}

TEST_CASE("v3_keys: a chord under held modifiers presses only what is not held", "[desktop][v3][hold]")
{
	TempDir dir;
	Harness h;
	CHECK(h.run(key_down(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(chord(101, "ctrl+shift+a")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(chord(101, "shift")).result == U_DESKTOP_XR_SUCCESS); // all held: nothing to press
	CHECK(h.held_keys() == "shift");
	CHECK(lines_with(h.log(), "key ") == std::vector<std::string>{"key ctrl+a"});

	// A chord whose key is held is refused; text and window ops wait for the release.
	CHECK(h.run(key_down(101, "a")).result == U_DESKTOP_XR_SUCCESS);
	auto r = h.run(chord(101, "ctrl+a"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	u_desktop_action t = act(U_DESKTOP_ACTION_TYPE_TEXT, 101);
	t.text = "hi";
	r = h.run(t);
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message.find("before typing text") != std::string::npos);
	r = h.run(act(U_DESKTOP_ACTION_MAXIMIZE, 101));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message.find("before a window action") != std::string::npos);
	CHECK(h.run(key_up(101, "a")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_up(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(t).result == U_DESKTOP_XR_SUCCESS);
}

TEST_CASE("v3_mods: a click / drag / scroll holds its modifiers around it, always released",
          "[desktop][v3][mods]")
{
	TempDir dir;
	Harness h;
	auto r = h.run(click(101, 300, 300, U_DESKTOP_MOD_CTRL | U_DESKTOP_MOD_SHIFT));
	CHECK(r.result == U_DESKTOP_XR_SUCCESS);
	CHECK(lines_with(h.log(), "key").size() == 4);
	std::string log = h.log();
	size_t kd = log.find("keydown ctrl"), ks = log.find("keydown shift"), bd = log.find("button left down"),
	       bu = log.find("button left up"), us = log.find("keyup shift"), uc = log.find("keyup ctrl");
	CHECK(kd < ks);
	CHECK(ks < bd);
	CHECK(bd < bu);
	CHECK(bu < us);
	CHECK(us < uc);
	CHECK(h.held_keys().empty());

	// A modifier the session holds already is not pressed again nor released.
	u_desktop_fake_clear_log(h.fake);
	CHECK(h.run(key_down(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(click(101, 320, 300, U_DESKTOP_MOD_CTRL)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(lines_with(h.log(), "key") == std::vector<std::string>{"keydown ctrl"});
	CHECK(h.held_keys() == "ctrl");
	CHECK(h.run(key_up(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);

	// Scroll with ctrl (zoom), drag with alt.
	u_desktop_fake_clear_log(h.fake);
	u_desktop_action s = button(U_DESKTOP_ACTION_SCROLL, 101, 300, 300);
	s.scroll_y = -2;
	s.mods = U_DESKTOP_MOD_CTRL;
	CHECK(h.run(s).result == U_DESKTOP_XR_SUCCESS);
	u_desktop_action d = button(U_DESKTOP_ACTION_DRAG, 101, 300, 300);
	d.x2 = 500;
	d.y2 = 400;
	d.mods = U_DESKTOP_MOD_ALT;
	CHECK(h.run(d).result == U_DESKTOP_XR_SUCCESS);
	CHECK(lines_with(h.log(), "key") ==
	      std::vector<std::string>{"keydown ctrl", "keyup ctrl", "keydown alt", "keyup alt"});

	// The press refused after the modifiers went down (a popup opened over the point
	// just then): they are released anyway, and nothing was clicked.
	u_desktop_fake_clear_log(h.fake);
	struct Ctx
	{
		u_desktop_fake *f;
	} ctx{h.fake};
	u_desktop_fake_set_batch_hook(
	    h.fake,
	    [](void *p, const char *op, uint32_t) {
		    auto *cx = static_cast<Ctx *>(p);
		    if (strcmp(op, "press") == 0) {
			    u_desktop_window w;
			    memset(&w, 0, sizeof(w));
			    w.id = 105;
			    w.pid = 1005;
			    snprintf(w.app_id, sizeof(w.app_id), "popup");
			    w.frame = {250, 250, 200, 200};
			    u_desktop_fake_add_window(cx->f, &w);
		    }
	    },
	    &ctx);
	r = h.run(click(101, 300, 300, U_DESKTOP_MOD_SHIFT));
	u_desktop_fake_set_batch_hook(h.fake, nullptr, nullptr);
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(lines_with(h.log(), "key") == std::vector<std::string>{"keydown shift", "keyup shift"});
	CHECK(lines_with(h.log(), "button").empty());
	CHECK(h.held_keys().empty());
}

TEST_CASE("v3_buttons: button down, moves (a drag with stops), button up", "[desktop][v3][hold]")
{
	TempDir dir;
	Harness h;
	CHECK(h.run(button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 200, 200)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_pressed_buttons(h.fake) == 1u);
	u_desktop_action m = button(U_DESKTOP_ACTION_POINTER_MOVE, 101, 400, 300);
	CHECK(h.run(m).result == U_DESKTOP_XR_SUCCESS);
	m.x = 600;
	CHECK(h.run(m).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(button(U_DESKTOP_ACTION_BUTTON_UP, 101, 700, 350)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_pressed_buttons(h.fake) == 0u);
	std::vector<std::string> want = {"move 200 200", "button left down", "move 400 300", "move 600 300",
	                                 "move 700 350", "button left up 700 350"};
	std::vector<std::string> got;
	for (auto &l : lines_with(h.log(), "")) {
		if (l.rfind("move", 0) == 0 || l.rfind("button", 0) == 0) {
			got.push_back(l);
		}
	}
	CHECK(got == want);

	// A second press of a held button, a click with it, a release of nothing.
	CHECK(h.run(button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 200, 200)).result == U_DESKTOP_XR_SUCCESS);
	auto r = h.run(button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 210, 200));
	CHECK(r.message == "the button is already held");
	r = h.run(click(101, 210, 200));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	r = h.run(button(U_DESKTOP_ACTION_BUTTON_UP, 101, 200, 200, U_DESKTOP_BUTTON_RIGHT));
	CHECK(r.message == "the button is not held");
	CHECK(u_desktop_fake_pressed_buttons(h.fake) == 1u);

	// Releasing over something that is not the target: released where the pointer is
	// (never moved onto the unverified point), and the action says why.
	u_desktop_fake_clear_log(h.fake);
	r = h.run(button(U_DESKTOP_ACTION_BUTTON_UP, 101, 1200, 400)); // the terminal is there
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message.find("found gnome-terminal") != std::string::npos);
	CHECK(u_desktop_fake_pressed_buttons(h.fake) == 0u);
	CHECK(lines_with(h.log(), "move").empty());
	CHECK(lines_with(h.log(), "button left up 200 200").size() == 1);
}

TEST_CASE("v3_buttons: a pointer action without a hit test or key_event is UNSUPPORTED", "[desktop][v3][hold]")
{
	TempDir dir;
	Harness h;
	u_desktop_fake_set_key_event_supported(h.fake, false);
	CHECK(h.run(key_down(101, "shift")).result == U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED);
	CHECK(h.run(key_up(101, "shift")).result == U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED);
	CHECK(h.run(click(101, 300, 300, U_DESKTOP_MOD_CTRL)).result == U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED);
	CHECK(h.run(click(101, 300, 300)).result == U_DESKTOP_XR_SUCCESS); // no modifiers: as in v2
}


/*
 *
 * The automatic release (§11.3): every trigger.
 *
 */

TEST_CASE("v3_release: the burst ending releases everything, last pressed first", "[desktop][v3][release]")
{
	TempDir dir;
	Harness h;
	u_desktop_control_set_hold_timing_for_tests(h.c, 0, 150ull * 1000000ull);
	CHECK(h.run(key_down(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_down(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 200, 200)).result == U_DESKTOP_XR_SUCCESS);
	REQUIRE(h.wait_released());
	std::string log = h.log();
	size_t bu = log.find("button left up"), us = log.find("keyup shift"), uc = log.find("keyup ctrl");
	CHECK(bu != std::string::npos);
	CHECK(bu < us);
	CHECK(us < uc);
	REQUIRE(dir.audit_has("\"action\":\"release\""));
	std::string audit = dir.audit();
	CHECK(audit.find("the action burst ended") != std::string::npos);
	CHECK(audit.find("\"count\":3") != std::string::npos);

	// The KEY_UP that comes too late says what happened.
	auto r = h.run(key_up(101, "shift"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message.find("it was released: the action burst ended") != std::string::npos);
}

TEST_CASE("v3_release: the maximum hold time", "[desktop][v3][release]")
{
	TempDir dir;
	Harness h;
	u_desktop_control_set_hold_timing_for_tests(h.c, 200ull * 1000000ull, 60ull * 1000000000ull);
	CHECK(h.run(key_down(101, "alt")).result == U_DESKTOP_XR_SUCCESS);
	// Keep the burst going: actions every 50 ms do not extend the hold.
	auto start = std::chrono::steady_clock::now();
	Recorder::Completion last{};
	while (std::chrono::steady_clock::now() - start < 600ms) {
		u_desktop_action m = button(U_DESKTOP_ACTION_POINTER_MOVE, 101, 300, 300);
		last = h.run(m);
		if (h.held_keys().empty()) {
			break;
		}
		std::this_thread::sleep_for(40ms);
	}
	CHECK(h.held_keys().empty());
	CHECK(dir.audit_has("held for the maximum time (200 ms)"));
	CHECK(std::chrono::steady_clock::now() - start < 600ms);
}

TEST_CASE("v3_release: the kill switch, the lock and a deny release at once", "[desktop][v3][release]")
{
	TempDir dir;
	SECTION("kill switch")
	{
		Harness h;
		CHECK(h.run(key_down(101, "super")).result == U_DESKTOP_XR_SUCCESS);
		REQUIRE(u_desktop_policy_set_suspended(true, "test"));
		REQUIRE(h.wait_released(1000ms));
		CHECK(dir.audit_has("\"message\":\"kill switch\""));
		REQUIRE(u_desktop_policy_set_suspended(false, "test"));
	}
	SECTION("lock")
	{
		Harness h;
		CHECK(h.run(button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 200, 200)).result == U_DESKTOP_XR_SUCCESS);
		u_desktop_fake_set_locked(h.fake, true);
		REQUIRE(h.wait_released(1000ms));
		CHECK(dir.audit_has("\"message\":\"screen locked\""));
	}
	SECTION("deny")
	{
		Harness h;
		CHECK(h.run(key_down(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);
		REQUIRE(u_desktop_policy_set_app(kExe, U_DESKTOP_APP_DENY, "test"));
		REQUIRE(h.wait_released(1000ms));
		INFO(dir.audit());
		CHECK(dir.audit_has("\"message\":\"disabled by the user\""));
	}
}

TEST_CASE("v3_release: the user's own input releases, and drops what was queued", "[desktop][v3][release]")
{
	TempDir dir;
	Harness h;
	CHECK(h.run(key_down(101, "alt")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_down(101, "tab")).result == U_DESKTOP_XR_SUCCESS);
	// The user starts typing; the rest of the agent's sequence is already queued.
	u_desktop_fake_set_action_delay_ms(h.fake, 30);
	u_desktop_fake_set_user_active(h.fake, true);
	std::this_thread::sleep_for(60ms); // past our last injection's 30 ms grace (spec §9)
	std::vector<uint64_t> ids = {h.submit(key_up(101, "tab")), h.submit(key_down(101, "tab")),
	                             h.submit(key_up(101, "tab")), h.submit(key_up(101, "alt"))};
	REQUIRE(h.rec.wait_done(2 + ids.size()));
	u_desktop_fake_set_user_active(h.fake, false);
	for (uint64_t id : ids) {
		auto r = h.rec.completion(id);
		INFO(r.message);
		CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
		CHECK(r.message.find("held input was released (the user is active)") != std::string::npos);
	}
	CHECK(h.held_keys().empty());
	// One press of each, then both released by the runtime; nothing of the dropped part.
	CHECK(lines_with(h.log(), "key") ==
	      std::vector<std::string>{"keydown alt", "keydown tab", "keyup tab", "keyup alt"});
	// A new sequence afterwards runs normally.
	u_desktop_fake_set_action_delay_ms(h.fake, 0);
	CHECK(h.run(key_down(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_up(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
}

TEST_CASE("v3_release: our own input during the hold is not the user's", "[desktop][v3][release]")
{
	TempDir dir;
	Harness h;
	CHECK(h.run(key_down(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	for (int i = 0; i < 5; i++) {
		CHECK(h.run(click(101, 200 + i * 20, 300)).result == U_DESKTOP_XR_SUCCESS);
		std::this_thread::sleep_for(40ms); // longer than the 30 ms grace
	}
	CHECK(h.held_keys() == "shift");
	CHECK(h.run(key_up(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
}

TEST_CASE("v3_release: input from before the hold is not interference (an OS that ignores ours)",
          "[desktop][v3][release]")
{
	TempDir dir;
	// The idle wait is off (idle_ms 0) so the press does not wait for the user first.
	{
		std::ofstream f(dir.file("desktop_control.json"));
		f << "{\"version\": 1, \"suspended\": false, \"idle_ms\": 0, \"apps\": {}}";
	}
	Harness h;
	// macOS HID idle: our injections do not reset it, so the latest input it reports
	// is the user's — from before the hold began.
	u_desktop_fake_set_injection_is_input(h.fake, false);
	u_desktop_fake_set_user_active(h.fake, true);
	std::this_thread::sleep_for(20ms);
	u_desktop_fake_set_user_active(h.fake, false);
	std::this_thread::sleep_for(20ms);
	CHECK(h.run(key_down(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);
	std::this_thread::sleep_for(80ms);
	CHECK(h.held_keys() == "ctrl");
	// The user typing NOW is.
	u_desktop_fake_set_user_active(h.fake, true);
	REQUIRE(h.wait_released(1000ms));
	u_desktop_fake_set_user_active(h.fake, false);
	CHECK(dir.audit_has("\"message\":\"the user is active\""));
}

TEST_CASE("v3_release: a key the OS repeats as input (GNOME) is not the user; the other triggers stay",
          "[desktop][v3][release]")
{
	TempDir dir;
	Harness h;
	u_desktop_fake_set_key_repeat(h.fake, true);
	u_desktop_control_set_hold_timing_for_tests(h.c, 600ull * 1000000ull, 0);
	// Shift does not repeat: the idle counter keeps meaning the user.
	CHECK(h.run(key_down(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	// A held letter is repeated: the idle counter reads 0 — our own repeat, not the user.
	CHECK(h.run(key_down(101, "a")).result == U_DESKTOP_XR_SUCCESS);
	std::this_thread::sleep_for(150ms);
	CHECK(h.held_keys() == "shift a");
	// ... and the next action does not wait for an "active user" that is our repeat.
	auto start = std::chrono::steady_clock::now();
	CHECK(h.run(click(101, 300, 300)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(std::chrono::steady_clock::now() - start < 500ms);
	CHECK(h.run(key_up(101, "a")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_up(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	// The maximum hold still ends a repeating hold.
	CHECK(h.run(key_down(101, "down")).result == U_DESKTOP_XR_SUCCESS);
	REQUIRE(h.wait_released(2000ms));
	CHECK(dir.audit_has("held for the maximum time (600 ms)"));
}

TEST_CASE("v3_release: keyboard focus moving to another app's window releases", "[desktop][v3][release]")
{
	TempDir dir;
	Harness h;
	CHECK(h.run(key_down(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);
	// Focus going nowhere, or to a dialog of the target, is not "away".
	u_desktop_window dlg;
	memset(&dlg, 0, sizeof(dlg));
	dlg.id = 106;
	dlg.pid = 1001;
	dlg.parent_id = 101;
	snprintf(dlg.app_id, sizeof(dlg.app_id), "gedit");
	dlg.frame = {300, 300, 200, 100};
	u_desktop_fake_add_window(h.fake, &dlg);
	u_desktop_fake_set_focus(h.fake, 106);
	std::this_thread::sleep_for(60ms);
	u_desktop_fake_set_focus(h.fake, 0);
	std::this_thread::sleep_for(60ms);
	CHECK(h.held_keys() == "ctrl");
	// Another app's window: released.
	u_desktop_fake_set_focus(h.fake, 102);
	REQUIRE(h.wait_released(1000ms));
	CHECK(dir.audit_has("focus moved to another window (gnome-terminal)"));

	// A click of ours on another window moves focus there: that is our own doing.
	u_desktop_fake_set_focus(h.fake, 101);
	CHECK(h.run(key_down(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(click(102, 1200, 400)).result == U_DESKTOP_XR_SUCCESS); // focuses the terminal
	std::this_thread::sleep_for(60ms);
	CHECK(h.held_keys() == "shift");
	CHECK(h.run(key_up(102, "shift")).result == U_DESKTOP_XR_SUCCESS);
}

TEST_CASE("v3_release: the session ending releases everything (audited)", "[desktop][v3][release]")
{
	TempDir dir;
	{
		Harness h;
		CHECK(h.run(key_down(101, "alt")).result == U_DESKTOP_XR_SUCCESS);
		CHECK(h.run(button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 200, 200, U_DESKTOP_BUTTON_RIGHT)).result ==
		      U_DESKTOP_XR_SUCCESS);
	} // destroyed here, holding both
	std::string audit = dir.audit();
	CHECK(audit.find("\"message\":\"the session ended\"") != std::string::npos);
	CHECK(audit.find("\"count\":2") != std::string::npos);
}

/*
 *
 * The audit: down / up like chords, never a typed character.
 *
 */

TEST_CASE("v3_audit: held keys are logged by the chord rule, modifiers always", "[desktop][v3][audit]")
{
	TempDir dir;
	Harness h;
	CHECK(h.run(key_down(101, "q")).result == U_DESKTOP_XR_SUCCESS); // a character: not logged
	CHECK(h.run(key_up(101, "q")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_down(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_down(101, "z")).result == U_DESKTOP_XR_SUCCESS); // shift+z is text
	CHECK(h.run(key_up(101, "z")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_up(101, "shift")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_down(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_down(101, "s")).result == U_DESKTOP_XR_SUCCESS); // ctrl+s: a shortcut
	CHECK(h.run(key_up(101, "s")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_up(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(click(101, 300, 300, U_DESKTOP_MOD_CTRL | U_DESKTOP_MOD_ALT)).result == U_DESKTOP_XR_SUCCESS);

	std::string audit = dir.audit();
	INFO(audit);
	CHECK(audit.find("\"keys\":\"q\"") == std::string::npos);
	CHECK(audit.find("\"keys\":\"z\"") == std::string::npos);
	CHECK(audit.find("\"keys\":\"shift\"") != std::string::npos);
	CHECK(audit.find("\"keys\":\"ctrl\"") != std::string::npos);
	CHECK(audit.find("\"keys\":\"s\"") != std::string::npos);
	CHECK(audit.find("\"keys\":\"ctrl+alt\"") != std::string::npos); // the click's modifiers
	CHECK(lines_with(audit, "\"action\":\"key_down\"").size() == 5);
	CHECK(lines_with(audit, "\"action\":\"key_up\"").size() == 5);
}


/*
 *
 * Item 2: the shell target (§11.5) — input where there is no app window, only on
 * explicit request.
 *
 */

namespace {

const uint64_t kShell = U_DESKTOP_TARGET_SHELL;

} // namespace

TEST_CASE("v3_shell: input actions only; window actions with it fail validation", "[desktop][v3][shell]")
{
	CHECK(valid(click(kShell, 100, 10)));
	CHECK(valid(button(U_DESKTOP_ACTION_POINTER_MOVE, kShell, 100, 10)));
	CHECK(valid(button(U_DESKTOP_ACTION_BUTTON_DOWN, kShell, 100, 10)));
	CHECK(valid(key_down(kShell, "super")));
	CHECK(valid(chord(kShell, "alt+f2")));
	u_desktop_action t = act(U_DESKTOP_ACTION_TYPE_TEXT, kShell);
	t.text = "terminal";
	CHECK(valid(t));
	char why[160];
	for (uint32_t op = U_DESKTOP_ACTION_ACTIVATE; op <= U_DESKTOP_ACTION_CLOSE; op++) {
		u_desktop_action w = act(op, kShell);
		CHECK_FALSE(u_desktop_control_validate(&w, why, sizeof(why)));
		CHECK(std::string(why) == "the shell target takes input actions only");
	}
}

TEST_CASE("v3_shell: pointer input — the shell, the desktop, never an app window", "[desktop][v3][shell]")
{
	TempDir dir;
	Harness h;
	// The top panel (pick SHELL) and the desktop background (pick DESKTOP).
	CHECK(h.run(click(kShell, 500, 10)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(click(kShell, 960, 1050)).result == U_DESKTOP_XR_SUCCESS);
	u_desktop_action s = button(U_DESKTOP_ACTION_SCROLL, kShell, 500, 10);
	s.scroll_y = 1;
	CHECK(h.run(s).result == U_DESKTOP_XR_SUCCESS);
	// An app window there: refused, nothing clicked.
	u_desktop_fake_clear_log(h.fake);
	auto r = h.run(click(kShell, 300, 300));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "the shell is not under the point (found gedit: notes.txt - Text Editor)");
	CHECK(lines_with(h.log(), "button").empty());
	// Without the shell target, version 2's refusal stays.
	r = h.run(click(101, 500, 10));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "target is not under the point (found the shell)");
	r = h.run(click(101, 960, 1050));
	CHECK(r.message == "target is not under the point (found the desktop)");

	// A window popping up over the point between the move and the press: refused.
	struct Ctx
	{
		u_desktop_fake *f;
	} ctx{h.fake};
	u_desktop_fake_set_batch_hook(
	    h.fake,
	    [](void *p, const char *op, uint32_t) {
		    if (strcmp(op, "press") == 0) {
			    u_desktop_window w;
			    memset(&w, 0, sizeof(w));
			    w.id = 107;
			    w.pid = 1007;
			    snprintf(w.app_id, sizeof(w.app_id), "notification");
			    w.frame = {900, 1000, 300, 80};
			    u_desktop_fake_add_window(static_cast<Ctx *>(p)->f, &w);
		    }
	    },
	    &ctx);
	r = h.run(click(kShell, 960, 1050));
	u_desktop_fake_set_batch_hook(h.fake, nullptr, nullptr);
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(u_desktop_fake_pressed_buttons(h.fake) == 0u);
}

TEST_CASE("v3_shell: keys — only while no app window holds the keyboard", "[desktop][v3][shell]")
{
	TempDir dir;
	Harness h;
	// gedit has focus: keys for the shell would land in it.
	auto r = h.run(chord(kShell, "super"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message.find("an app window has keyboard focus (gedit)") != std::string::npos);
	// Nothing focused: Super, Alt+Tab, shell shortcuts go to the shell.
	u_desktop_fake_set_focus(h.fake, 0);
	CHECK(h.run(chord(kShell, "super")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_down(kShell, "alt")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_down(kShell, "tab")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_up(kShell, "tab")).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(key_up(kShell, "alt")).result == U_DESKTOP_XR_SUCCESS);
	// Without the shell target nothing changed: no window, no keys.
	r = h.run(chord(101, "super"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message.find("does not have focus") != std::string::npos);
}

TEST_CASE("v3_shell: the overview holds the keyboard — keys for the shell, never for a window",
          "[desktop][v3][shell]")
{
	TempDir dir;
	Harness h;
	// The overview is open (the backend reports the shell holds the keyboard), while
	// gedit is still mutter's focus window.
	u_desktop_fake_set_shell_keyboard(h.fake, true);
	u_desktop_action t = act(U_DESKTOP_ACTION_TYPE_TEXT, kShell);
	t.text = "terminal\n";
	CHECK(h.run(t).result == U_DESKTOP_XR_SUCCESS);
	auto r = h.run(chord(101, "ctrl+a"));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message.find("the shell holds the keyboard") != std::string::npos);
	// Focus moving to the shell while a key is held (Super opened the overview) is not "away".
	u_desktop_fake_set_shell_keyboard(h.fake, false);
	CHECK(h.run(key_down(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);
	u_desktop_fake_set_shell_keyboard(h.fake, true);
	std::this_thread::sleep_for(60ms);
	CHECK(h.held_keys() == "ctrl");
	u_desktop_fake_set_shell_keyboard(h.fake, false);
	CHECK(h.run(key_up(101, "ctrl")).result == U_DESKTOP_XR_SUCCESS);
}

TEST_CASE("v3_shell: held buttons and drags to the shell; every other guard still holds",
          "[desktop][v3][shell]")
{
	TempDir dir;
	Harness h;
	// Drag from an app window onto the desktop: the press is the window's, the drop the shell's.
	CHECK(h.run(button(U_DESKTOP_ACTION_BUTTON_DOWN, 101, 300, 300)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(button(U_DESKTOP_ACTION_POINTER_MOVE, kShell, 960, 900)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(h.run(button(U_DESKTOP_ACTION_BUTTON_UP, kShell, 960, 1000)).result == U_DESKTOP_XR_SUCCESS);
	CHECK(u_desktop_fake_pressed_buttons(h.fake) == 0u);

	// The kill switch, the lock and a deny refuse the shell target like any other.
	REQUIRE(u_desktop_policy_set_suspended(true, "test"));
	uint64_t id = 0;
	u_desktop_action k = click(kShell, 500, 10);
	CHECK(u_desktop_control_submit(h.c, &k, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	REQUIRE(u_desktop_policy_set_suspended(false, "test"));
	REQUIRE(u_desktop_control_get_state(h.c, nullptr) == U_DESKTOP_STATE_GRANTED);
	u_desktop_fake_set_locked(h.fake, true);
	CHECK(u_desktop_control_submit(h.c, &k, &id) == U_DESKTOP_XR_ERROR_NOT_GRANTED);
	CHECK(u_desktop_control_get_state(h.c, nullptr) == U_DESKTOP_STATE_SUSPENDED);
	u_desktop_fake_set_locked(h.fake, false);
	REQUIRE(u_desktop_control_get_state(h.c, nullptr) == U_DESKTOP_STATE_GRANTED);
}

TEST_CASE("v3_shell: the user is waited for, the audit names the shell", "[desktop][v3][shell][audit]")
{
	TempDir dir;
	u_desktop_control_timing t = kFast;
	t.idle_max_wait_ns = 150ull * 1000000ull;
	Harness h(&t);
	u_desktop_fake_set_user_active(h.fake, true);
	auto r = h.run(click(kShell, 500, 10));
	CHECK(r.result == U_DESKTOP_XR_ERROR_REFUSED);
	CHECK(r.message == "the user is active");
	u_desktop_fake_set_user_active(h.fake, false);
	std::this_thread::sleep_for(700ms); // idle_ms (600) since the user's last input
	CHECK(h.run(click(kShell, 500, 10)).result == U_DESKTOP_XR_SUCCESS);
	std::string audit = dir.audit();
	INFO(audit);
	CHECK(audit.find("\"target\":{\"kind\":\"shell\"}") != std::string::npos);
	CHECK(audit.find("18446744073709551615") == std::string::npos); // never the raw id
}
