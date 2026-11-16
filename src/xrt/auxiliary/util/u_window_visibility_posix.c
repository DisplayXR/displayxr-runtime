// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671): the tracker shared by
 *         desktop Linux and macOS.
 * @ingroup aux_util
 *
 * The thread, the debounce, the transition logging and the public API, over a
 * per-platform backend (u_window_visibility_posix.h). Same contract and same
 * log lines as the Windows tracker (u_window_visibility_win32.c), so one
 * recipe reads all three.
 *
 * The thread sleeps in poll(2) on a wake pipe plus the backend's event fd (if
 * it has one): at most @ref U_WINDOW_VISIBILITY_PERIOD_VISIBLE_MS between
 * checks while visible, @ref U_WINDOW_VISIBILITY_PERIOD_HIDDEN_MS while hidden
 * or going hidden, and never more often than every @ref WV_MIN_INTERVAL_MS
 * however many native events arrive.
 *
 * ADR-062 tiers 2 and 3: each tick also judges the whole window (every screen,
 * the backend with no panel) on a debounce of its own, for
 * u_window_visibility_hidden_everywhere and the session-facing snapshot. These
 * platforms have no segment DPs and no foreground fact, so the per-screen calls
 * read visible and u_window_visibility_foreground reads false.
 */

#include "util/u_window_visibility.h"
#include "util/u_window_visibility_posix.h"
#include "util/u_logging.h"
#include "util/u_debug.h"
#include "os/os_time.h"
#include "os/os_threading.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

//! Floor between two checks, whatever wakes the thread.
#define WV_MIN_INTERVAL_MS 50

/*
 * The POSIX cadence. Windows dropped to one 500 ms safety poll (#2680) because
 * its window events do the fast work; the macOS backend has no event fd and
 * polls, so these keep restore detection at the speed the hardware runs were
 * judged at.
 */
//! Longest gap between checks while the window is visible.
#define U_WINDOW_VISIBILITY_PERIOD_VISIBLE_MS 200u
//! Longest gap between checks while it is hidden or going hidden.
#define U_WINDOW_VISIBILITY_PERIOD_HIDDEN_MS 100u

struct u_window_visibility
{
	char who[64];
	u_window_visibility_change_fn on_change;
	void *userdata;

	struct u_wv_backend *backend;

	//! Guards @ref panel, @ref next_target, @ref retarget and @ref stop.
	struct os_mutex lock;
	char panel[128];
	void *next_target;
	bool retarget;
	bool stop;

	//! Debounced result, read by any thread (__atomic builtins).
	int hidden;
	//! Debounced, the whole window (every screen): ADR-062 tiers 2-3 (__atomic).
	int hidden_all;

	//! Guarded by @ref lock: the snapshot @ref u_window_visibility_get_state reads.
	enum u_wv_reason reason;
	enum u_wv_reason reason_all;
	uint64_t since_ns;

	//! The thread was joined (@ref u_window_visibility_stop); readers see visible.
	bool stopped;

	//! Wake pipe: [0] polled by the thread, [1] written by set_window / destroy.
	int wake[2];
	struct os_thread thread;

	// Thread-private state below.
	bool have_target;
	struct u_wv_debounce debounce;
	struct u_wv_debounce debounce_all;
};

/*!
 * The shared reason for a backend's label (the backends name their reasons in
 * prose, for the log lines). A hidden label maps to a hidden reason; anything
 * else reads visible.
 */
static enum u_wv_reason
wv_reason_from_label(bool hidden, const char *label)
{
	if (label == NULL) {
		return hidden ? U_WV_REASON_COVERED : U_WV_REASON_VISIBLE;
	}
	if (hidden) {
		if (strstr(label, "minimized") != NULL) {
			return U_WV_REASON_MINIMIZED;
		}
		if (strstr(label, "not visible") != NULL) {
			return U_WV_REASON_NOT_VISIBLE;
		}
		if (strstr(label, "another desktop") != NULL) {
			return U_WV_REASON_CLOAKED;
		}
		return U_WV_REASON_COVERED; // fully covered / obscured / another Space
	}
	if (strstr(label, "gone") != NULL) {
		return U_WV_REASON_GONE;
	}
	if (strstr(label, "off the 3D screen") != NULL) {
		return U_WV_REASON_OFF_SCREEN;
	}
	if (strstr(label, "partly covered") != NULL) {
		return U_WV_REASON_PARTLY_COVERED;
	}
	return U_WV_REASON_VISIBLE;
}

static void
wv_wake(struct u_window_visibility *wv)
{
	const char c = 1;
	// Non-blocking: a full pipe already means "wake up".
	ssize_t r = write(wv->wake[1], &c, 1);
	(void)r;
}

static void
wv_drain_wake(struct u_window_visibility *wv)
{
	char buf[64];
	while (read(wv->wake[0], buf, sizeof(buf)) > 0) {
	}
}

static int
wv_ms_left(uint64_t deadline_ns)
{
	const uint64_t now = os_monotonic_get_ns();
	if (now >= deadline_ns) {
		return 0;
	}
	return (int)((deadline_ns - now + 999999u) / 1000000u);
}

/*!
 * Sleep until @p period_ms after @p last_check_ns, or earlier when woken —
 * but never before the @ref WV_MIN_INTERVAL_MS floor.
 */
static void
wv_wait(struct u_window_visibility *wv, uint64_t last_check_ns, uint32_t period_ms)
{
	const int bfd = u_wv_backend_fd(wv->backend);
	const uint64_t deadline = last_check_ns + (uint64_t)period_ms * 1000000u;
	const uint64_t floor_ns = last_check_ns + (uint64_t)WV_MIN_INTERVAL_MS * 1000000u;

	for (;;) {
		struct pollfd fds[2];
		nfds_t n = 0;
		fds[n].fd = wv->wake[0];
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;
		if (bfd >= 0) {
			fds[n].fd = bfd;
			fds[n].events = POLLIN;
			fds[n].revents = 0;
			n++;
		}
		const int r = poll(fds, n, wv_ms_left(deadline));
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			return; // period elapsed (or poll failed: check anyway)
		}
		if ((fds[0].revents & POLLIN) != 0) {
			wv_drain_wake(wv);
			return; // stop or retarget: act now
		}
		if (n > 1 && fds[1].revents != 0) {
			u_wv_backend_drain(wv->backend);
			// Native events say "look again", but not faster than the floor.
			const int left = wv_ms_left(floor_ns);
			if (left > 0) {
				struct pollfd p = {wv->wake[0], POLLIN, 0};
				if (poll(&p, 1, left) > 0) {
					wv_drain_wake(wv);
				}
			}
			return;
		}
	}
}

static void *
wv_thread(void *ptr)
{
	struct u_window_visibility *wv = (struct u_window_visibility *)ptr;
	uint32_t period = U_WINDOW_VISIBILITY_PERIOD_VISIBLE_MS;
	uint64_t last_check = os_monotonic_get_ns();
	char panel[sizeof(wv->panel)];

	for (;;) {
		wv_wait(wv, last_check, period);

		os_mutex_lock(&wv->lock);
		const bool stop = wv->stop;
		const bool retarget = wv->retarget;
		void *next = wv->next_target;
		wv->retarget = false;
		memcpy(panel, wv->panel, sizeof(panel));
		os_mutex_unlock(&wv->lock);
		if (stop) {
			break;
		}

		if (retarget) {
			u_wv_backend_set_target(wv->backend, next);
			wv->have_target = next != NULL;
			// A new window starts visible: nothing is known about it yet.
			memset(&wv->debounce, 0, sizeof(wv->debounce));
			memset(&wv->debounce_all, 0, sizeof(wv->debounce_all));
			__atomic_store_n(&wv->hidden_all, 0, __ATOMIC_SEQ_CST);
			os_mutex_lock(&wv->lock);
			wv->reason = U_WV_REASON_VISIBLE;
			wv->reason_all = U_WV_REASON_VISIBLE;
			os_mutex_unlock(&wv->lock);
			if (__atomic_exchange_n(&wv->hidden, 0, __ATOMIC_SEQ_CST) != 0) {
				U_LOG_W("#2671 %s: tracking a new 3D window — lens wish back to the session's mode",
				        wv->who);
				if (wv->on_change != NULL) {
					wv->on_change(wv->userdata, false);
				}
			}
		}

		last_check = os_monotonic_get_ns();
		if (!wv->have_target) {
			period = U_WINDOW_VISIBILITY_PERIOD_VISIBLE_MS;
			continue;
		}

		const char *reason = NULL;
		const bool raw = u_wv_backend_check(wv->backend, panel, &reason);
		// Every screen: the whole window. Without a panel the 3D-screen check
		// already judged the whole window, so it is the same answer.
		const char *reason_all = reason;
		bool raw_all = raw;
		if (panel[0] != 0) {
			raw_all = u_wv_backend_check(wv->backend, "", &reason_all);
		}
		const uint64_t t = os_monotonic_get_ns();
		const enum u_wv_reason r = wv_reason_from_label(raw, reason);
		const enum u_wv_reason r_all = wv_reason_from_label(raw_all, reason_all);
		const bool was = wv->debounce.reported_hidden;
		const bool now = u_wv_debounce_update(&wv->debounce, raw, t, U_WINDOW_VISIBILITY_HIDE_DWELL_NS);
		const bool was_all = wv->debounce_all.reported_hidden;
		const bool now_all = u_wv_debounce_update(&wv->debounce_all, raw_all, t, U_WINDOW_VISIBILITY_HIDE_DWELL_NS);
		os_mutex_lock(&wv->lock);
		wv->reason = r;
		wv->reason_all = r_all;
		if (now != was || now_all != was_all) {
			wv->since_ns = t;
		}
		os_mutex_unlock(&wv->lock);
		if (now_all != was_all) {
			__atomic_store_n(&wv->hidden_all, now_all ? 1 : 0, __ATOMIC_SEQ_CST);
		}
		if (now != was) {
			__atomic_store_n(&wv->hidden, now ? 1 : 0, __ATOMIC_SEQ_CST);
			U_LOG_W("#2671 %s: the 3D window is %s (%s) — lens wish %s", wv->who,
			        now ? "hidden" : "visible again", reason != NULL ? reason : "?",
			        now ? "2D unless another 3D window is visible" : "back to the session's mode");
			if (wv->on_change != NULL) {
				wv->on_change(wv->userdata, now);
			}
		}
		period = (raw || raw_all) ? U_WINDOW_VISIBILITY_PERIOD_HIDDEN_MS : U_WINDOW_VISIBILITY_PERIOD_VISIBLE_MS;
	}

	return NULL;
}

/*
 *
 * API.
 *
 */

DEBUG_GET_ONCE_BOOL_OPTION(lens_visibility, "DXR_LENS_VISIBILITY", true)

static bool
wv_pipe_nonblocking(int fds[2])
{
	if (pipe(fds) != 0) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		const int fl = fcntl(fds[i], F_GETFL, 0);
		(void)fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
		(void)fcntl(fds[i], F_SETFD, FD_CLOEXEC);
	}
	return true;
}

struct u_window_visibility *
u_window_visibility_create(void *native_window,
                           const char *panel_device_name,
                           const char *who,
                           u_window_visibility_change_fn on_change,
                           void *userdata)
{
	if (native_window == NULL) {
		return NULL;
	}
	if (!debug_get_bool_option_lens_visibility()) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("#2671: DXR_LENS_VISIBILITY=0 — hidden 3D windows keep the lens wish");
		}
		return NULL;
	}

	struct u_wv_backend *b = u_wv_backend_create();
	if (b == NULL) {
		U_LOG_I("#2671 %s: no window-visibility source here — the 3D window counts as always visible",
		        who != NULL ? who : "window");
		return NULL;
	}

	struct u_window_visibility *wv = (struct u_window_visibility *)calloc(1, sizeof(*wv));
	if (wv == NULL) {
		u_wv_backend_destroy(b);
		return NULL;
	}
	wv->backend = b;
	wv->on_change = on_change;
	wv->userdata = userdata;
	(void)snprintf(wv->who, sizeof(wv->who), "%s", who != NULL ? who : "window");
	(void)snprintf(wv->panel, sizeof(wv->panel), "%s", panel_device_name != NULL ? panel_device_name : "");
	// The thread picks the first window up like any later one.
	wv->next_target = native_window;
	wv->retarget = true;

	if (os_mutex_init(&wv->lock) != 0) {
		u_wv_backend_destroy(b);
		free(wv);
		return NULL;
	}
	if (!wv_pipe_nonblocking(wv->wake)) {
		os_mutex_destroy(&wv->lock);
		u_wv_backend_destroy(b);
		free(wv);
		return NULL;
	}
	os_thread_init(&wv->thread);
	if (os_thread_start(&wv->thread, wv_thread, wv) != 0) {
		close(wv->wake[0]);
		close(wv->wake[1]);
		os_mutex_destroy(&wv->lock);
		u_wv_backend_destroy(b);
		free(wv);
		return NULL;
	}
	os_thread_name(&wv->thread, "dxr-lens-vis");
	// Check the first window at once rather than after a full period.
	wv_wake(wv);
	U_LOG_I("#2671 %s: tracking 3D window visibility (window %p, 3D screen '%s')", wv->who, native_window,
	        wv->panel);
	return wv;
}

bool
u_window_visibility_hidden(const struct u_window_visibility *wv)
{
	if (wv == NULL) {
		return false;
	}
	return __atomic_load_n(&wv->hidden, __ATOMIC_SEQ_CST) != 0;
}

void
u_window_visibility_set_panel(struct u_window_visibility *wv, const char *panel_device_name)
{
	if (wv == NULL) {
		return;
	}
	os_mutex_lock(&wv->lock);
	(void)snprintf(wv->panel, sizeof(wv->panel), "%s", panel_device_name != NULL ? panel_device_name : "");
	os_mutex_unlock(&wv->lock);
}

void
u_window_visibility_set_window(struct u_window_visibility *wv, void *native_window)
{
	if (wv == NULL) {
		return;
	}
	os_mutex_lock(&wv->lock);
	wv->next_target = native_window;
	wv->retarget = true;
	os_mutex_unlock(&wv->lock);
	// Readers see the old window's state until the thread applies it, so clear
	// a stale "hidden" now (the safe direction).
	__atomic_store_n(&wv->hidden, 0, __ATOMIC_SEQ_CST);
	wv_wake(wv);
}

bool
u_window_visibility_hidden_everywhere(const struct u_window_visibility *wv)
{
	if (wv == NULL || !u_window_visibility_skip_present_enabled()) {
		return false;
	}
	return __atomic_load_n(&wv->hidden_all, __ATOMIC_SEQ_CST) != 0;
}

void
u_window_visibility_get_state(const struct u_window_visibility *wv, struct u_window_visibility_state *out)
{
	memset(out, 0, sizeof(*out));
	if (wv == NULL) {
		return;
	}
	struct u_window_visibility *m = (struct u_window_visibility *)wv;
	os_mutex_lock(&m->lock);
	const bool stopped = m->stopped;
	const enum u_wv_reason reason = m->reason;
	const enum u_wv_reason reason_all = m->reason_all;
	const uint64_t since = m->since_ns;
	os_mutex_unlock(&m->lock);
	if (stopped) {
		return;
	}
	out->tracked = true;
	out->reason = reason;
	out->reason_every_screen = reason_all;
	out->since_ns = since;
	out->hidden = u_window_visibility_hidden(wv);
	out->hidden_every_screen = __atomic_load_n(&m->hidden_all, __ATOMIC_SEQ_CST) != 0;
	out->hidden_everywhere = u_window_visibility_hidden_everywhere(wv);
}

bool
u_window_visibility_foreground(const struct u_window_visibility *wv)
{
	// No foreground fact on these platforms (see the file comment).
	(void)wv;
	return false;
}

void
u_window_visibility_set_screens(struct u_window_visibility *wv, const char *const *device_names, uint32_t count)
{
	// No segment DPs on these platforms: the session's 3D screen is the only one.
	(void)wv;
	(void)device_names;
	(void)count;
}

bool
u_window_visibility_screen_hidden(const struct u_window_visibility *wv, const char *device_name)
{
	(void)wv;
	(void)device_name;
	return false; // an untracked screen: the safe direction
}

//! Stop and join the thread once; readers see visible from then on.
static void
wv_stop_thread(struct u_window_visibility *wv)
{
	os_mutex_lock(&wv->lock);
	const bool already = wv->stopped;
	wv->stop = true;
	wv->stopped = true;
	os_mutex_unlock(&wv->lock);
	if (already) {
		return;
	}
	wv_wake(wv);
	// Joined, never detached: the thread must not outlive its owner.
	os_thread_join(&wv->thread);
	os_thread_destroy(&wv->thread);
	__atomic_store_n(&wv->hidden, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&wv->hidden_all, 0, __ATOMIC_SEQ_CST);
}

void
u_window_visibility_stop(struct u_window_visibility *wv)
{
	if (wv == NULL) {
		return;
	}
	wv_stop_thread(wv);
}

void
u_window_visibility_destroy(struct u_window_visibility **wv_ptr)
{
	if (wv_ptr == NULL || *wv_ptr == NULL) {
		return;
	}
	struct u_window_visibility *wv = *wv_ptr;
	*wv_ptr = NULL;
	wv_stop_thread(wv);
	u_wv_backend_destroy(wv->backend);
	close(wv->wake[0]);
	close(wv->wake[1]);
	os_mutex_destroy(&wv->lock);
	free(wv);
}
