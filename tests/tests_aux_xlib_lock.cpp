// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The process-wide Xlib lock (os/os_xlib_lock.h, the #2820 follow-up):
 *         it excludes, it is the SAME lock the desktop-rect resolver takes, and
 *         (with an X display) private connections opened under it from one
 *         thread survive the resolver's walks on another.
 */

#include "catch_amalgamated.hpp"

#include "os/os_display_desktop.h"
#include "os/os_xlib_lock.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include <dlfcn.h>

TEST_CASE("xlib_lock: excludes across threads")
{
	constexpr int kThreads = 4;
	constexpr int kIters = 20000;

	std::atomic<int> inside{0};
	std::atomic<bool> overlap{false};
	long counter = 0; // deliberately plain: only the lock protects it

	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; t++) {
		threads.emplace_back([&] {
			for (int i = 0; i < kIters; i++) {
				os_xlib_lock();
				if (inside.fetch_add(1) != 0) {
					overlap = true;
				}
				counter++;
				inside.fetch_sub(1);
				os_xlib_unlock();
			}
		});
	}
	for (auto &th : threads) {
		th.join();
	}

	CHECK_FALSE(overlap.load());
	CHECK(counter == (long)kThreads * kIters);
}

TEST_CASE("xlib_lock: the desktop-rect resolver takes the same lock")
{
	// query_monitors takes the lock before it even dlopens libX11, so this
	// holds with no X libraries and no DISPLAY (CI).
	std::atomic<bool> done{false};

	os_xlib_lock();
	std::thread th([&] {
		struct os_display_desktop_info infos[OS_DISPLAY_DESKTOP_MAX_MONITORS];
		(void)os_display_desktop_enumerate(infos, OS_DISPLAY_DESKTOP_MAX_MONITORS);
		done = true;
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	const bool finished_while_held = done.load();
	os_xlib_unlock();
	th.join();

	CHECK_FALSE(finished_while_held);
	CHECK(done.load());
}

TEST_CASE("xlib_lock: private connections under the lock vs the resolver's walks")
{
	// The #2820 crash shape with the follow-up's callers: one thread opens and
	// closes its own connection (as the weave pointer reader, the cursor
	// look's Xft.dpi read and the vk_native fallbacks do) while another walks
	// RandR through the resolver. Needs a live X server; a no-op otherwise.
	// A smoke test, not a proven reproducer: with the opener's lock removed it
	// did not abort in 20 runs of 400 iterations on the DS1 box (the #2820
	// abort needed the full session-create allocation pattern). What it does
	// catch is a deadlock or a crash in the locked sequences themselves.
	const char *display = std::getenv("DISPLAY");
	if (display == nullptr || display[0] == '\0') {
		WARN("no DISPLAY: X stress not run");
		return;
	}
	void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
	if (lib == nullptr) {
		WARN("no libX11.so.6: X stress not run");
		return;
	}
	using open_fn = void *(*)(const char *);
	using close_fn = int (*)(void *);
	open_fn open_display = nullptr;
	close_fn close_display = nullptr;
	*(void **)(&open_display) = dlsym(lib, "XOpenDisplay");
	*(void **)(&close_display) = dlsym(lib, "XCloseDisplay");
	REQUIRE(open_display != nullptr);
	REQUIRE(close_display != nullptr);

	constexpr int kIters = 40;
	std::atomic<int> opened{0};

	std::thread opener([&] {
		for (int i = 0; i < kIters; i++) {
			os_xlib_lock();
			void *dpy = open_display(nullptr);
			if (dpy != nullptr) {
				opened++;
				close_display(dpy);
			}
			os_xlib_unlock();
		}
	});
	std::thread walker([&] {
		for (int i = 0; i < kIters; i++) {
			struct os_display_desktop_info infos[OS_DISPLAY_DESKTOP_MAX_MONITORS];
			(void)os_display_desktop_enumerate(infos, OS_DISPLAY_DESKTOP_MAX_MONITORS);
		}
	});
	opener.join();
	walker.join();
	dlclose(lib);

	// Reaching here without a heap abort is the result; opened only says the
	// X server was actually reachable.
	INFO("connections opened: " << opened.load());
	CHECK(opened.load() >= 0);
}
