// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR Control Panel — an ImGui + SDL2 GUI over `displayxr-cli`.
 *
 * The panel is deliberately "dumb": it spawns the sibling `displayxr-cli`
 * with `--json`, parses stdout with cJSON, and renders. All runtime / plug-in
 * knowledge stays in the CLI (single source of truth), and because the CLI
 * runs as a separate process the panel links zero vendor symbols (ADR-019).
 *
 *   Tier 0  runtime / plug-in / display dashboard + self-test + copy diagnostics
 *   Tier 1  display-processor switch via the PreferredPlugin override
 *   #918    GPU topology — does the weave cross adapters to reach the panel?
 *   #1252   Performance: Target GPU, Mode, Diagnostics (via `displayxr-cli perf`)
 *   ADR-051 Tabs; Displays + Windows pages rendered from ONE long-lived
 *           `displayxr-cli status --watch --json` child (falls back to polling
 *           `status --json` every 30 s when the CLI has no `--watch`)
 *
 * @author David Fattal
 */

#include "glad/gl.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui/cimgui.h"
#include "cimgui/cimgui_impl.h"

#include <cjson/cJSON.h>

#include <SDL2/SDL.h>
#ifdef _WIN32
#include <SDL2/SDL_syswm.h> // real HWND, to size the ImGui window from the client rect
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h> // ShellExecuteA: "Open in vendor"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif


/*
 *
 * Shell out to displayxr-cli.
 *
 */

/*!
 * Run the sibling `displayxr-cli <args>` and capture its stdout into @p out
 * (NUL-terminated, truncated to @p cap). stderr (the noisy plug-in WARN
 * lines) is discarded so stdout stays clean JSON. Returns true if the
 * process launched and was waited on.
 */
static void
cli_ui_acquire(void);
static void
cli_ui_release(void);

static bool
run_cli_locked(const char *args, char *out, size_t cap);

/*!
 * Run `displayxr-cli <args>` for the UI. Never concurrently with the status
 * feed's child: every headless CLI run loads the vendor plug-in, which the
 * vendor service counts as a tracker client (ADR-051 D5 "passive only"), so
 * the panel runs at most ONE CLI child at any time. The feed's child is ended
 * first and resumes after.
 */
static bool
run_cli(const char *args, char *out, size_t cap)
{
	cli_ui_acquire();
	bool ok = run_cli_locked(args, out, cap);
	cli_ui_release();
	return ok;
}

static bool
run_cli_locked(const char *args, char *out, size_t cap)
{
	if (cap == 0) {
		return false;
	}
	out[0] = '\0';

#ifdef _WIN32
	// Resolve displayxr-cli.exe next to our own executable.
	char dir[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir));
	if (len == 0 || len >= sizeof(dir)) {
		return false;
	}
	char *slash = strrchr(dir, '\\');
	if (slash != NULL) {
		*(slash + 1) = '\0';
	} else {
		dir[0] = '\0';
	}

	char cmd[2048];
	snprintf(cmd, sizeof(cmd), "\"%sdisplayxr-cli.exe\" %s", dir, args);

	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE rd = NULL, wr = NULL;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		return false;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
	                         OPEN_EXISTING, 0, NULL);

	STARTUPINFOA si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr;
	si.hStdError = nul;
	si.hStdInput = nul;

	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));

	BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	CloseHandle(wr); // parent must close its write end so ReadFile sees EOF
	if (!ok) {
		CloseHandle(rd);
		if (nul != INVALID_HANDLE_VALUE) {
			CloseHandle(nul);
		}
		return false;
	}

	size_t total = 0;
	char buf[1024];
	DWORD n = 0;
	while (ReadFile(rd, buf, (DWORD)sizeof(buf), &n, NULL) && n > 0) {
		size_t room = (total < cap - 1) ? (cap - 1 - total) : 0;
		size_t take = (n < room) ? n : room;
		if (take > 0) {
			memcpy(out + total, buf, take);
			total += take;
		}
		if (take < (size_t)n) {
			break; // buffer full
		}
	}
	out[total] = '\0';

	WaitForSingleObject(pi.hProcess, INFINITE);
	CloseHandle(rd);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (nul != INVALID_HANDLE_VALUE) {
		CloseHandle(nul);
	}
	return true;
#else
	char cmd[2048];
	snprintf(cmd, sizeof(cmd), "displayxr-cli %s 2>/dev/null", args);
	FILE *f = popen(cmd, "r");
	if (f == NULL) {
		return false;
	}
	size_t total = fread(out, 1, cap - 1, f);
	out[total] = '\0';
	pclose(f);
	return true;
#endif
}


/*
 *
 * The status feed (ADR-051 D4/D5).
 *
 * ONE long-lived `displayxr-cli status --watch --json` child, read on its own
 * thread: every complete JSON object on its stdout is one snapshot. The newest
 * snapshot waits in `pending` until the UI thread takes it, so the UI renders
 * from a tree it owns outright and never holds the lock while drawing.
 *
 * Fallback: a watcher never exits on its own after one snapshot or within
 * seconds, so a child that does (a CLI without `--watch` prints one snapshot,
 * or a usage error, and leaves) flips the feed to polling headless
 * `status --json` every 30 s on the same thread. Refresh re-reads once and
 * tries `--watch` again.
 *
 * Cadence (D5 "passive only"): every headless CLI run loads the vendor
 * plug-in, which the vendor service counts as a tracker client, so headless
 * is read at most every 30 s and the panel never runs more than ONE CLI child
 * at a time (g_cli_lock; a UI command pre-empts the feed's child). Only the
 * live `--watch` path, where the service already holds its vendor instance,
 * runs at ~2 s.
 *
 * Feed discipline (D5): held only while the window is visible and not
 * minimised; the last snapshot survives a child restart; the snapshot is
 * dropped when the feed is released, so nothing shows stale data.
 *
 */

#define FEED_WATCH_MIN_LIFETIME_MS 5000u
// Headless fallback cadence. NOT 2 s: each headless run creates a vendor
// instance the vendor service counts as a tracker client. Only the live
// `--watch` path (the service already holds its vendor instance) runs at ~2 s.
#define FEED_POLL_INTERVAL_MS 30000u
#define FEED_RESPAWN_DELAY_MS 1000u
#define FEED_MAX_BUFFER (4u * 1024u * 1024u)

enum feed_mode
{
	FEED_MODE_WATCH,
	FEED_MODE_POLL,
};

struct status_feed
{
	SDL_mutex *mutex;
	SDL_Thread *thread;

	// Requests from the UI thread (under the mutex).
	bool stop;
	bool restart;
	int ui_waiting; //!< UI threads waiting to run their own CLI child

	// Published by the reader thread (under the mutex).
	cJSON *pending; //!< newest complete snapshot, not yet taken by the UI
	enum feed_mode mode;
	bool child_running;
	uint32_t snapshots;     //!< complete snapshots received since the feed started
	uint32_t last_rx_ticks; //!< SDL_GetTicks() of the newest one
	char note[256];         //!< what the feed is running, and why
	char error[256];        //!< the last failure, cleared by the next snapshot

#ifdef _WIN32
	HANDLE child; //!< the live child, so stop / restart can end it
#else
	pid_t child;
#endif
};

//! What one child run produced.
struct child_result
{
	bool launched;
	bool interrupted; //!< ended because the UI asked (stop / restart)
	int exit_code;
	uint32_t elapsed_ms;
	uint32_t parsed;
};

//! Copy of the feed's bookkeeping, for the UI.
struct feed_view
{
	bool running;
	enum feed_mode mode;
	bool child_running;
	uint32_t snapshots;
	uint32_t last_rx_ticks;
	char note[256];
	char error[256];
};

static void
feed_set_text(struct status_feed *f, char *dst, size_t cap, const char *text)
{
	SDL_LockMutex(f->mutex);
	snprintf(dst, cap, "%s", text);
	SDL_UnlockMutex(f->mutex);
}

static void
feed_publish(struct status_feed *f, cJSON *snap)
{
	SDL_LockMutex(f->mutex);
	if (f->pending != NULL) {
		cJSON_Delete(f->pending); // the UI never saw it; only the newest matters
	}
	f->pending = snap;
	f->snapshots++;
	f->last_rx_ticks = SDL_GetTicks();
	f->error[0] = '\0';
	SDL_UnlockMutex(f->mutex);
}

/*!
 * Publish every complete top-level JSON object in `buf[0, *len)` and keep the
 * incomplete tail. Brace-matched (string-aware) rather than line-split, so the
 * same code reads NDJSON from `--watch` and the pretty-printed object a plain
 * `status --json` writes. A line that does not start an object (a usage error,
 * a stray message) is dropped whole. Only objects carrying `schema` count.
 */
static uint32_t
feed_drain(struct status_feed *f, char *buf, size_t *len)
{
	uint32_t parsed = 0;
	size_t pos = 0;
	while (pos < *len) {
		const char c = buf[pos];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			pos++;
			continue;
		}
		if (c != '{') {
			const char *nl = memchr(buf + pos, '\n', *len - pos);
			if (nl == NULL) {
				break; // wait for the rest of the line
			}
			pos = (size_t)(nl - buf) + 1;
			continue;
		}

		int depth = 0;
		bool in_str = false;
		bool esc = false;
		size_t end = 0;
		for (size_t k = pos; k < *len; k++) {
			const char ch = buf[k];
			if (in_str) {
				if (esc) {
					esc = false;
				} else if (ch == '\\') {
					esc = true;
				} else if (ch == '"') {
					in_str = false;
				}
				continue;
			}
			if (ch == '"') {
				in_str = true;
			} else if (ch == '{') {
				depth++;
			} else if (ch == '}' && --depth == 0) {
				end = k + 1;
				break;
			}
		}
		if (end == 0) {
			break; // incomplete: wait for more bytes
		}

		cJSON *j = cJSON_ParseWithLength(buf + pos, end - pos);
		if (j != NULL && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "schema"))) {
			feed_publish(f, j);
			parsed++;
		} else if (j != NULL) {
			cJSON_Delete(j);
		}
		pos = end;
	}
	if (pos > 0) {
		memmove(buf, buf + pos, *len - pos);
		*len -= pos;
	}
	return parsed;
}

//! Append @p n bytes to the growing read buffer, then drain it.
static bool
feed_consume(struct status_feed *f, char **buf, size_t *len, size_t *cap, const char *data, size_t n, uint32_t *parsed)
{
	if (*len + n + 1 > *cap) {
		size_t want = *cap * 2;
		while (want < *len + n + 1) {
			want *= 2;
		}
		if (want > FEED_MAX_BUFFER) {
			*len = 0; // a runaway object: drop it rather than grow without bound
			if (n + 1 > *cap) {
				return true;
			}
		} else {
			char *nb = realloc(*buf, want);
			if (nb == NULL) {
				return false;
			}
			*buf = nb;
			*cap = want;
		}
	}
	memcpy(*buf + *len, data, n);
	*len += n;
	*parsed += feed_drain(f, *buf, len);
	return true;
}

//! End the live child, if any. Caller holds the mutex.
static void
feed_kill_child_locked(struct status_feed *f)
{
#ifdef _WIN32
	if (f->child != NULL) {
		TerminateProcess(f->child, 1);
	}
#else
	if (f->child > 0) {
		kill(f->child, SIGTERM);
	}
#endif
}

//! Serialises every CLI child the panel spawns (feed and UI): one at a time.
static SDL_mutex *g_cli_lock;
//! The feed a UI command must pause before it runs its own child.
static struct status_feed *g_feed;

/*!
 * Run `displayxr-cli <argv...>` to completion, publishing every snapshot it
 * writes as it arrives. @p argv is NULL-terminated. Caller holds g_cli_lock.
 */
static void
feed_run_child_locked(struct status_feed *f, const char *const *argv, struct child_result *r)
{
	memset(r, 0, sizeof(*r));
	const uint32_t t0 = SDL_GetTicks();

	size_t cap = 64 * 1024;
	size_t len = 0;
	char *buf = malloc(cap);
	if (buf == NULL) {
		return;
	}

#ifdef _WIN32
	char dir[MAX_PATH];
	DWORD dlen = GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir));
	if (dlen == 0 || dlen >= sizeof(dir)) {
		free(buf);
		return;
	}
	char *slash = strrchr(dir, '\\');
	if (slash != NULL) {
		*(slash + 1) = '\0';
	} else {
		dir[0] = '\0';
	}
	char cmd[1024];
	int used = snprintf(cmd, sizeof(cmd), "\"%sdisplayxr-cli.exe\"", dir);
	for (int i = 0; argv[i] != NULL && used > 0 && (size_t)used < sizeof(cmd); i++) {
		used += snprintf(cmd + used, sizeof(cmd) - (size_t)used, " %s", argv[i]);
	}

	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE rd = NULL, wr = NULL;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		free(buf);
		return;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
	                         OPEN_EXISTING, 0, NULL);

	STARTUPINFOA si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr;
	si.hStdError = nul; // the plug-in WARN lines; stdout stays clean JSON
	si.hStdInput = nul;
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));
	BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	CloseHandle(wr); // our write end must close or ReadFile never sees EOF
	if (nul != INVALID_HANDLE_VALUE) {
		CloseHandle(nul);
	}
	if (!ok) {
		CloseHandle(rd);
		free(buf);
		return;
	}
	r->launched = true;

	SDL_LockMutex(f->mutex);
	f->child = pi.hProcess;
	f->child_running = true;
	if (f->stop || f->restart || f->ui_waiting > 0) {
		feed_kill_child_locked(f);
	}
	SDL_UnlockMutex(f->mutex);

	char chunk[4096];
	DWORD n = 0;
	while (ReadFile(rd, chunk, (DWORD)sizeof(chunk), &n, NULL) && n > 0) {
		if (!feed_consume(f, &buf, &len, &cap, chunk, n, &r->parsed)) {
			break;
		}
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	r->exit_code = (int)code;

	SDL_LockMutex(f->mutex);
	f->child = NULL;
	f->child_running = false;
	r->interrupted = f->stop || f->restart || f->ui_waiting > 0;
	SDL_UnlockMutex(f->mutex);

	CloseHandle(rd);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
#else
	const char *child_argv[16];
	int argc = 0;
	child_argv[argc++] = "displayxr-cli";
	for (int i = 0; argv[i] != NULL && argc < 15; i++) {
		child_argv[argc++] = argv[i];
	}
	child_argv[argc] = NULL;

	int fds[2];
	if (pipe(fds) != 0) {
		free(buf);
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		free(buf);
		return;
	}
	if (pid == 0) {
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 2);
		}
		execvp(child_argv[0], (char *const *)child_argv);
		_exit(127);
	}
	close(fds[1]);
	r->launched = true;

	SDL_LockMutex(f->mutex);
	f->child = pid;
	f->child_running = true;
	if (f->stop || f->restart || f->ui_waiting > 0) {
		feed_kill_child_locked(f);
	}
	SDL_UnlockMutex(f->mutex);

	char chunk[4096];
	for (;;) {
		ssize_t n = read(fds[0], chunk, sizeof(chunk));
		if (n <= 0) {
			break;
		}
		if (!feed_consume(f, &buf, &len, &cap, chunk, (size_t)n, &r->parsed)) {
			break;
		}
	}
	int st = 0;
	waitpid(pid, &st, 0);
	r->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

	SDL_LockMutex(f->mutex);
	f->child = 0;
	f->child_running = false;
	r->interrupted = f->stop || f->restart || f->ui_waiting > 0;
	SDL_UnlockMutex(f->mutex);
	close(fds[0]);
#endif

	free(buf);
	r->elapsed_ms = SDL_GetTicks() - t0;
}

/*!
 * One feed child, never alongside another CLI child: a UI command that is
 * waiting goes first, and one that arrives while the child runs ends it
 * (feed_run_child_locked re-checks after the spawn, so the two cannot miss
 * each other).
 */
static void
feed_run_child(struct status_feed *f, const char *const *argv, struct child_result *r)
{
	memset(r, 0, sizeof(*r));
	for (;;) {
		SDL_LockMutex(f->mutex);
		const bool quit = f->stop || f->restart;
		const bool ui = f->ui_waiting > 0;
		SDL_UnlockMutex(f->mutex);
		if (quit) {
			r->interrupted = true;
			return;
		}
		if (!ui) {
			break;
		}
		SDL_Delay(50);
	}
	if (g_cli_lock != NULL) {
		SDL_LockMutex(g_cli_lock);
	}
	feed_run_child_locked(f, argv, r);
	if (g_cli_lock != NULL) {
		SDL_UnlockMutex(g_cli_lock);
	}
}

//! Sleep up to @p ms, waking early for stop / restart.
static void
feed_wait(struct status_feed *f, uint32_t ms)
{
	const uint32_t t0 = SDL_GetTicks();
	for (;;) {
		SDL_LockMutex(f->mutex);
		const bool wake = f->stop || f->restart;
		SDL_UnlockMutex(f->mutex);
		if (wake || SDL_GetTicks() - t0 >= ms) {
			return;
		}
		SDL_Delay(50);
	}
}

static int SDLCALL
feed_thread(void *userdata)
{
	struct status_feed *f = (struct status_feed *)userdata;
	static const char *const watch_args[] = {"status", "--watch", "--json", NULL};
	static const char *const poll_args[] = {"status", "--json", NULL};
	enum feed_mode mode = FEED_MODE_WATCH;
	char text[256];

	for (;;) {
		SDL_LockMutex(f->mutex);
		if (f->stop) {
			SDL_UnlockMutex(f->mutex);
			break;
		}
		if (f->restart) {
			f->restart = false;
			mode = FEED_MODE_WATCH; // Refresh gives --watch another chance
		}
		f->mode = mode;
		SDL_UnlockMutex(f->mutex);

		struct child_result r;
		if (mode == FEED_MODE_WATCH) {
			feed_set_text(f, f->note, sizeof(f->note), "displayxr-cli status --watch --json");
			feed_run_child(f, watch_args, &r);
			if (r.interrupted) {
				continue;
			}
			if (!r.launched) {
				feed_set_text(f, f->error, sizeof(f->error),
				              "could not launch displayxr-cli (not found alongside the panel?)");
				feed_wait(f, FEED_POLL_INTERVAL_MS);
				continue;
			}
			if (r.parsed <= 1 || r.elapsed_ms < FEED_WATCH_MIN_LIFETIME_MS) {
				// A watcher does not end on its own after one snapshot, nor
				// within seconds: this CLI has no --watch (it ignored the
				// flag, printed one snapshot and left, or printed a usage
				// error). Any snapshot it did print is already published.
				// Plug-in load alone can take several seconds headless, so
				// elapsed time by itself cannot tell the two apart.
				snprintf(text, sizeof(text),
				         "status --watch unavailable (exited rc=%d after %u ms, %u snapshot%s) - "
				         "polling headless 'status --json' every 30 s",
				         r.exit_code, (unsigned)r.elapsed_ms, (unsigned)r.parsed,
				         r.parsed == 1 ? "" : "s");
				feed_set_text(f, f->note, sizeof(f->note), text);
				mode = FEED_MODE_POLL;
				if (r.parsed > 0) {
					feed_wait(f, FEED_POLL_INTERVAL_MS); // that run WAS the read
				}
				continue;
			}
			snprintf(text, sizeof(text), "status --watch exited (rc=%d); restarting it", r.exit_code);
			feed_set_text(f, f->error, sizeof(f->error), text);
			feed_wait(f, FEED_RESPAWN_DELAY_MS);
		} else {
			feed_run_child(f, poll_args, &r);
			if (r.interrupted) {
				// Stop / Refresh wake the wait at once; a UI command that
				// pre-empted this poll does not buy an extra headless run.
				feed_wait(f, FEED_POLL_INTERVAL_MS);
				continue;
			}
			if (!r.launched) {
				feed_set_text(f, f->error, sizeof(f->error),
				              "could not launch displayxr-cli (not found alongside the panel?)");
			} else if (r.parsed == 0) {
				snprintf(text, sizeof(text), "'status --json' returned no snapshot (rc=%d)",
				         r.exit_code);
				feed_set_text(f, f->error, sizeof(f->error), text);
			}
			feed_wait(f, FEED_POLL_INTERVAL_MS);
		}
	}
	return 0;
}

static void
cli_ui_acquire(void)
{
	if (g_feed != NULL && g_feed->mutex != NULL) {
		SDL_LockMutex(g_feed->mutex);
		g_feed->ui_waiting++;
		feed_kill_child_locked(g_feed);
		SDL_UnlockMutex(g_feed->mutex);
	}
	if (g_cli_lock != NULL) {
		SDL_LockMutex(g_cli_lock);
	}
}

static void
cli_ui_release(void)
{
	if (g_cli_lock != NULL) {
		SDL_UnlockMutex(g_cli_lock);
	}
	if (g_feed != NULL && g_feed->mutex != NULL) {
		SDL_LockMutex(g_feed->mutex);
		g_feed->ui_waiting--;
		SDL_UnlockMutex(g_feed->mutex);
	}
}

static void
feed_init(struct status_feed *f)
{
	memset(f, 0, sizeof(*f));
	f->mutex = SDL_CreateMutex();
}

//! Acquire the feed (window visible). Idempotent.
static void
feed_start(struct status_feed *f)
{
	if (f->thread != NULL || f->mutex == NULL) {
		return;
	}
	SDL_LockMutex(f->mutex);
	f->stop = false;
	f->restart = false;
	f->snapshots = 0;
	f->note[0] = '\0';
	f->error[0] = '\0';
	SDL_UnlockMutex(f->mutex);
	f->thread = SDL_CreateThread(feed_thread, "dxr-status-feed", f);
}

//! Release the feed (window hidden / minimised / closing): end the child, join
//! the thread, drop the pending snapshot. Idempotent.
static void
feed_stop(struct status_feed *f)
{
	if (f->thread == NULL) {
		return;
	}
	SDL_LockMutex(f->mutex);
	f->stop = true;
	feed_kill_child_locked(f);
	SDL_UnlockMutex(f->mutex);
	SDL_WaitThread(f->thread, NULL);
	f->thread = NULL;
	SDL_LockMutex(f->mutex);
	if (f->pending != NULL) {
		cJSON_Delete(f->pending);
		f->pending = NULL;
	}
	SDL_UnlockMutex(f->mutex);
}

//! Refresh: end the child and start over with --watch. The UI keeps showing
//! the last snapshot meanwhile.
static void
feed_restart(struct status_feed *f)
{
	if (f->thread == NULL) {
		return;
	}
	SDL_LockMutex(f->mutex);
	f->restart = true;
	feed_kill_child_locked(f);
	SDL_UnlockMutex(f->mutex);
}

//! Hand the newest snapshot (if any) to the UI, which then owns it.
static void
feed_take(struct status_feed *f, cJSON **current)
{
	if (f->mutex == NULL) {
		return;
	}
	SDL_LockMutex(f->mutex);
	if (f->pending != NULL) {
		if (*current != NULL) {
			cJSON_Delete(*current);
		}
		*current = f->pending;
		f->pending = NULL;
	}
	SDL_UnlockMutex(f->mutex);
}

static void
feed_get_view(struct status_feed *f, struct feed_view *v)
{
	memset(v, 0, sizeof(*v));
	if (f->mutex == NULL) {
		return;
	}
	SDL_LockMutex(f->mutex);
	v->running = f->thread != NULL;
	v->mode = f->mode;
	v->child_running = f->child_running;
	v->snapshots = f->snapshots;
	v->last_rx_ticks = f->last_rx_ticks;
	snprintf(v->note, sizeof(v->note), "%s", f->note);
	snprintf(v->error, sizeof(v->error), "%s", f->error);
	SDL_UnlockMutex(f->mutex);
}

static void
feed_destroy(struct status_feed *f)
{
	feed_stop(f);
	if (f->mutex != NULL) {
		SDL_DestroyMutex(f->mutex);
		f->mutex = NULL;
	}
}


/*
 *
 * State + JSON parsing.
 *
 */

#define MAX_CHECKS 8
#define MAX_DPS 8

struct dp_row
{
	char id[64];
	char name[128];
	int order;
	bool active;
	bool preferred;
};

struct check_row
{
	char name[40];
	bool ok;
	char detail[256];
};

//! One hardware adapter from the #918 GPU-topology probe (`info --json` → `gpu.adapters`).
struct gpu_row
{
	char name[128];
	char luid[32]; // "00000000:00024f0b"
	int vram_mb;
};

#define MAX_GPUS 8

/*!
 * One allow-listed performance lever (`info --json` → `performance.levers`).
 *
 * `source` is carried, never dropped: "env" means the value came from the
 * environment of the `displayxr-cli` child this panel spawned — which inherits
 * the panel's own environment and says nothing about any other process —
 * whereas "user" / "machine" / "default" are machine-wide and DO describe what
 * a newly launched app will see. The UI states which.
 */
struct setting_row
{
	char name[64];
	char value[128]; // empty = unset
	bool set;
	char source[16];
};

#define MAX_SETTINGS 16

struct panel_state
{
	// info
	bool have_info;
	char info_err[256];
	char rt_desc[256], rt_tag[128];
	int rt_abi;
	bool ar_queried, ar_set;
	char ar_value[1024];
	bool have_plugin;
	char pl_id[64], pl_name[128], pl_vendor[64], pl_ver[64];
	char device[256];
	// ADR-045: every registered plug-in's platform state (from `info --json`).
	int n_pstates;
	struct
	{
		char line[400];
		bool degraded; // a non-READY state the user can act on
	} pstates[8];

	// GPU topology (#918). Machine facts…
	bool gpu_probed;
	char gpu_note[128];
	char gpu_verdict[192];
	bool gpu_split_applies;
	int n_gpus;
	struct gpu_row gpus[MAX_GPUS];
	bool gpu_scanout_resolved, gpu_render_resolved, gpu_ingest_resolved;
	char gpu_scanout_name[128], gpu_scanout_luid[32];
	char gpu_render_name[128], gpu_render_luid[32];
	char gpu_ingest_name[128], gpu_ingest_luid[32], gpu_ingest_provenance[64];
	// …and, kept separate on purpose, the CONFIGURED half: resolved through
	// the settings chain, so each carries a source. "env" means the CLI
	// child's environment (which is this panel's) and describes no other
	// process; the rest are machine-wide. The GPU section states which.
	char gpu_weave[64]; // empty = unset
	bool gpu_weave_set;
	char gpu_weave_source[16]; // env / user / machine / default
	char gpu_ingress[32];
	char gpu_service_split[160];

	// Performance settings (#1252) — what the three controls below read and
	// write. Each row carries its provenance.
	int n_settings;
	struct setting_row settings[MAX_SETTINGS];
	char settings_user_file[512];
	char settings_user_written[32];

	// selftest
	bool have_selftest;
	char verdict[16];
	int result_code;
	int n_checks;
	struct check_row checks[MAX_CHECKS];

	// dp list
	int n_dp;
	bool have_preferred;
	char preferred[64];
	struct dp_row dps[MAX_DPS];

	// ADR-051 status snapshot (`status --watch --json`), owned by the UI
	// thread; replaced wholesale by feed_take() each frame. The Displays and
	// Windows tabs, the Displays badge and the Overview summary line read it
	// and nothing else.
	struct status_feed *feed;
	cJSON *snap;
	int select_tab; // one-shot programmatic tab switch (-1 = none)

	// last dp use/reset feedback
	char last_action[512];
};

static void
cpy_str(char *dst, size_t cap, const cJSON *parent, const char *key)
{
	dst[0] = '\0';
	const cJSON *n = cJSON_GetObjectItemCaseSensitive(parent, key);
	if (cJSON_IsString(n) && n->valuestring != NULL) {
		snprintf(dst, cap, "%s", n->valuestring);
	}
}

static double
get_num(const cJSON *parent, const char *key)
{
	const cJSON *n = cJSON_GetObjectItemCaseSensitive(parent, key);
	return cJSON_IsNumber(n) ? n->valuedouble : 0.0;
}

static void
refresh_info(struct panel_state *s)
{
	s->have_info = false;
	s->have_plugin = false;
	s->ar_queried = false;
	s->info_err[0] = '\0';
	// Refresh is idempotent: n_gpus must be cleared or every click appends
	// another copy of the adapter list until MAX_GPUS.
	s->gpu_probed = false;
	s->n_gpus = 0;
	s->gpu_scanout_resolved = false;
	s->gpu_render_resolved = false;
	s->gpu_ingest_resolved = false;
	s->gpu_weave_set = false;
	s->gpu_weave[0] = '\0';
	s->gpu_weave_source[0] = '\0';
	s->gpu_ingress[0] = '\0';
	s->gpu_service_split[0] = '\0';
	s->n_settings = 0;
	s->settings_user_written[0] = '\0';
	s->gpu_note[0] = '\0';
	s->gpu_verdict[0] = '\0';

	char out[16384];
	if (!run_cli("info --json", out, sizeof(out)) || out[0] == '\0') {
		snprintf(s->info_err, sizeof(s->info_err),
		         "Could not run displayxr-cli (not found alongside the panel?)");
		return;
	}
	cJSON *root = cJSON_Parse(out);
	if (root == NULL) {
		snprintf(s->info_err, sizeof(s->info_err), "Failed to parse 'info --json' output.");
		return;
	}
	s->have_info = true;

	const cJSON *rt = cJSON_GetObjectItemCaseSensitive(root, "runtime");
	if (rt != NULL) {
		cpy_str(s->rt_desc, sizeof(s->rt_desc), rt, "description");
		cpy_str(s->rt_tag, sizeof(s->rt_tag), rt, "git_tag");
		s->rt_abi = (int)get_num(rt, "plugin_abi_version");
	}

	const cJSON *ar = cJSON_GetObjectItemCaseSensitive(root, "active_openxr_runtime");
	if (ar != NULL) {
		s->ar_queried = true;
		const cJSON *set = cJSON_GetObjectItemCaseSensitive(ar, "set");
		s->ar_set = cJSON_IsTrue(set);
		cpy_str(s->ar_value, sizeof(s->ar_value), ar, "value");
	}

	const cJSON *pl = cJSON_GetObjectItemCaseSensitive(root, "plugin");
	if (cJSON_IsObject(pl)) {
		s->have_plugin = true;
		cpy_str(s->pl_id, sizeof(s->pl_id), pl, "id");
		cpy_str(s->pl_name, sizeof(s->pl_name), pl, "display_name");
		cpy_str(s->pl_vendor, sizeof(s->pl_vendor), pl, "vendor");
		cpy_str(s->pl_ver, sizeof(s->pl_ver), pl, "version");
	}

	cpy_str(s->device, sizeof(s->device), root, "device");

	// ADR-045: per-plug-in platform state + vendor hint, shown verbatim.
	s->n_pstates = 0;
	const cJSON *pls = cJSON_GetObjectItemCaseSensitive(root, "plugins");
	const cJSON *pe = NULL;
	cJSON_ArrayForEach(pe, pls)
	{
		if (s->n_pstates >= 8) {
			break;
		}
		char name[128], ver[64], st[32], hint[160], lr[32];
		cpy_str(name, sizeof(name), pe, "display_name");
		if (name[0] == '\0') {
			cpy_str(name, sizeof(name), pe, "id");
		}
		cpy_str(ver, sizeof(ver), pe, "version");
		cpy_str(st, sizeof(st), pe, "platform_state");
		cpy_str(hint, sizeof(hint), pe, "hint");
		cpy_str(lr, sizeof(lr), pe, "load_result");
		snprintf(s->pstates[s->n_pstates].line, sizeof(s->pstates[0].line), "%s %s - %s%s%s  [%s]", name, ver,
		         st, hint[0] != '\0' ? ": " : "", hint, lr);
		s->pstates[s->n_pstates].degraded = strcmp(st, "READY") != 0 && strcmp(st, "UNKNOWN") != 0;
		s->n_pstates++;
	}

	// #918 GPU topology. Absent off-Windows, and `probed` false when DXGI
	// could not answer — both render as one line rather than an empty table.
	const cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "gpu");
	if (cJSON_IsObject(g)) {
		s->gpu_probed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "probed"));
		cpy_str(s->gpu_note, sizeof(s->gpu_note), g, "note");
		cpy_str(s->gpu_verdict, sizeof(s->gpu_verdict), g, "verdict");
		s->gpu_split_applies = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "split_applies"));

		const cJSON *arr = cJSON_GetObjectItemCaseSensitive(g, "adapters");
		const cJSON *it = NULL;
		cJSON_ArrayForEach(it, arr)
		{
			if (s->n_gpus >= MAX_GPUS) {
				break;
			}
			struct gpu_row *row = &s->gpus[s->n_gpus++];
			cpy_str(row->name, sizeof(row->name), it, "name");
			cpy_str(row->luid, sizeof(row->luid), it, "luid");
			row->vram_mb = (int)get_num(it, "dedicated_vram_mb");
		}

		const cJSON *sc = cJSON_GetObjectItemCaseSensitive(g, "scanout");
		if (sc != NULL) {
			s->gpu_scanout_resolved = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "resolved"));
			cpy_str(s->gpu_scanout_name, sizeof(s->gpu_scanout_name), sc, "name");
			cpy_str(s->gpu_scanout_luid, sizeof(s->gpu_scanout_luid), sc, "luid");
		}
		const cJSON *rd = cJSON_GetObjectItemCaseSensitive(g, "render");
		if (rd != NULL) {
			s->gpu_render_resolved = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rd, "resolved"));
			cpy_str(s->gpu_render_name, sizeof(s->gpu_render_name), rd, "name");
			cpy_str(s->gpu_render_luid, sizeof(s->gpu_render_luid), rd, "luid");
		}
		const cJSON *ig = cJSON_GetObjectItemCaseSensitive(g, "service_ingest");
		if (ig != NULL) {
			s->gpu_ingest_resolved = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ig, "resolved"));
			cpy_str(s->gpu_ingest_name, sizeof(s->gpu_ingest_name), ig, "name");
			cpy_str(s->gpu_ingest_luid, sizeof(s->gpu_ingest_luid), ig, "luid");
			cpy_str(s->gpu_ingest_provenance, sizeof(s->gpu_ingest_provenance), ig, "provenance");
		}
		const cJSON *sp = cJSON_GetObjectItemCaseSensitive(g, "split");
		if (sp != NULL) {
			s->gpu_weave_set = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sp, "weave_on_scanout_set"));
			cpy_str(s->gpu_weave, sizeof(s->gpu_weave), sp, "weave_on_scanout");
			cpy_str(s->gpu_weave_source, sizeof(s->gpu_weave_source), sp, "weave_on_scanout_source");
			cpy_str(s->gpu_ingress, sizeof(s->gpu_ingress), sp, "ingress");
			cpy_str(s->gpu_service_split, sizeof(s->gpu_service_split), sp, "service_split");
		}
	}

	// #1252 performance levers, each with its provenance.
	const cJSON *pf = cJSON_GetObjectItemCaseSensitive(root, "performance");
	if (cJSON_IsObject(pf)) {
		cpy_str(s->settings_user_file, sizeof(s->settings_user_file), pf, "user_file");
		cpy_str(s->settings_user_written, sizeof(s->settings_user_written), pf, "user_written");
		const cJSON *arr = cJSON_GetObjectItemCaseSensitive(pf, "levers");
		const cJSON *it = NULL;
		cJSON_ArrayForEach(it, arr)
		{
			if (s->n_settings >= MAX_SETTINGS) {
				break;
			}
			struct setting_row *row = &s->settings[s->n_settings++];
			cpy_str(row->name, sizeof(row->name), it, "name");
			cpy_str(row->value, sizeof(row->value), it, "value");
			cpy_str(row->source, sizeof(row->source), it, "source");
			row->set = row->value[0] != '\0';
		}
	}

	cJSON_Delete(root);
}

static void
refresh_selftest(struct panel_state *s)
{
	s->have_selftest = false;
	s->n_checks = 0;

	char out[16384];
	if (!run_cli("selftest --json", out, sizeof(out)) || out[0] == '\0') {
		return;
	}
	cJSON *root = cJSON_Parse(out);
	if (root == NULL) {
		return;
	}
	s->have_selftest = true;
	cpy_str(s->verdict, sizeof(s->verdict), root, "verdict");
	s->result_code = (int)get_num(root, "result_code");

	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "checks");
	if (cJSON_IsArray(arr)) {
		const cJSON *c = NULL;
		cJSON_ArrayForEach(c, arr)
		{
			if (s->n_checks >= MAX_CHECKS) {
				break;
			}
			struct check_row *r = &s->checks[s->n_checks++];
			cpy_str(r->name, sizeof(r->name), c, "name");
			r->ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "ok"));
			cpy_str(r->detail, sizeof(r->detail), c, "detail");
		}
	}
	cJSON_Delete(root);
}

static void
refresh_dp(struct panel_state *s)
{
	s->n_dp = 0;
	s->have_preferred = false;
	s->preferred[0] = '\0';

	char out[16384];
	if (!run_cli("dp list --json", out, sizeof(out)) || out[0] == '\0') {
		return;
	}
	cJSON *root = cJSON_Parse(out);
	if (root == NULL) {
		return;
	}
	const cJSON *pref = cJSON_GetObjectItemCaseSensitive(root, "preferred");
	if (cJSON_IsString(pref) && pref->valuestring != NULL) {
		s->have_preferred = true;
		snprintf(s->preferred, sizeof(s->preferred), "%s", pref->valuestring);
	}

	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "plugins");
	if (cJSON_IsArray(arr)) {
		const cJSON *p = NULL;
		cJSON_ArrayForEach(p, arr)
		{
			if (s->n_dp >= MAX_DPS) {
				break;
			}
			struct dp_row *r = &s->dps[s->n_dp++];
			cpy_str(r->id, sizeof(r->id), p, "id");
			cpy_str(r->name, sizeof(r->name), p, "display_name");
			r->order = (int)get_num(p, "probe_order");
			r->active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "active"));
			r->preferred = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "preferred"));
		}
	}
	cJSON_Delete(root);
}

//! Overview data (`info`, `dp list`) on demand; the Displays / Windows data
//! comes from the status feed, which Refresh restarts (keeping the last
//! snapshot on screen until the new child reports).
static void
refresh_all(struct panel_state *s)
{
	refresh_info(s);
	refresh_dp(s);
	if (s->feed != NULL) {
		feed_restart(s->feed);
	}
}

static void
dp_action(struct panel_state *s, const char *args)
{
	char out[2048];
	if (run_cli(args, out, sizeof(out))) {
		// first line is the human-readable result
		char *nl = strchr(out, '\n');
		if (nl != NULL) {
			*nl = '\0';
		}
		snprintf(s->last_action, sizeof(s->last_action), "%s", out[0] ? out : "(done)");
	} else {
		snprintf(s->last_action, sizeof(s->last_action), "Failed to run: displayxr-cli %s", args);
	}
	refresh_dp(s);
	if (s->feed != NULL) {
		feed_restart(s->feed); // the override changed which DP binds each display (#793)
	}
}

/*!
 * Write one performance lever through `displayxr-cli perf` (#1252).
 *
 * The panel deliberately does not write the settings file itself: one writer,
 * the same shape as `dp use` / `dp reset`, and the GUI keeps no runtime
 * knowledge (#378). `refresh_info` afterwards re-reads the resolved state, so
 * what the UI shows is always what the chain resolved — never what we assumed
 * the click did.
 */
static void
perf_action(struct panel_state *s, const char *args)
{
	char out[2048];
	if (run_cli(args, out, sizeof(out))) {
		char *nl = strchr(out, '\n');
		if (nl != NULL) {
			*nl = '\0';
		}
		snprintf(s->last_action, sizeof(s->last_action), "%s", out[0] ? out : "(done)");
	} else {
		snprintf(s->last_action, sizeof(s->last_action), "Failed to run: displayxr-cli %s", args);
	}
	refresh_info(s);
}

//! Resolved value of one lever, or "" when nothing set it.
static const char *
setting_value(const struct panel_state *s, const char *name)
{
	for (int i = 0; i < s->n_settings; i++) {
		if (strcmp(s->settings[i].name, name) == 0) {
			return s->settings[i].value;
		}
	}
	return "";
}

//! Provenance of one lever ("env"/"user"/"machine"/"default"), or "".
static const char *
setting_source(const struct panel_state *s, const char *name)
{
	for (int i = 0; i < s->n_settings; i++) {
		if (strcmp(s->settings[i].name, name) == 0) {
			return s->settings[i].source;
		}
	}
	return "";
}

//! Is any lever set by something other than the runtime's own default?
static bool
any_setting_non_default(const struct panel_state *s)
{
	for (int i = 0; i < s->n_settings; i++) {
		if (s->settings[i].set) {
			return true;
		}
	}
	return false;
}

/*!
 * Is Compatibility mode in force?
 *
 * Derived from the resolved values rather than stored as its own key. A preset
 * that stored its own name would drift from what the levers actually say the
 * moment anything else wrote one of them; deriving it means the UI can never
 * claim a mode the runtime is not in, and "Custom" falls out for free.
 */
static bool
compat_mode_on(const struct panel_state *s)
{
	const char *split = setting_value(s, "DXR_WEAVE_ON_SCANOUT");
	const char *repaint = setting_value(s, "DXR_WEAVE_REPAINT");
	return split[0] == '0' && repaint[0] == '0';
}


/*
 *
 * UI.
 *
 */

static const ImVec4 COL_GREEN = {0.30f, 0.85f, 0.40f, 1.0f};
static const ImVec4 COL_RED = {0.95f, 0.35f, 0.35f, 1.0f};
static const ImVec4 COL_AMBER = {0.98f, 0.75f, 0.25f, 1.0f}; // override-forced binding (#793)

static const ImVec4 COL_GREY = {0.55f, 0.57f, 0.60f, 1.0f};
static const ImVec4 COL_CHIP = {0.22f, 0.27f, 0.36f, 1.0f};
static const ImVec4 COL_TEXT = {0.92f, 0.93f, 0.95f, 1.0f};
static const ImVec4 COL_MONITOR = {0.16f, 0.17f, 0.20f, 1.0f};
static const ImVec4 COL_MONITOR_OWNER = {0.16f, 0.26f, 0.40f, 1.0f};
static const ImVec4 COL_MONITOR_EDGE = {0.50f, 0.52f, 0.56f, 1.0f};
static const ImVec4 COL_WINDOW = {0.35f, 0.80f, 0.95f, 1.0f};


/*
 *
 * Status snapshot readers (ADR-051 §3). The panel reads only the documented
 * keys, tolerates any of them missing, and never parses a vendor string or
 * `dashboard_command`: those are shown or handed on verbatim.
 *
 */

enum warn_level
{
	WARN_LEVEL_NONE = 0,
	WARN_LEVEL_INFO = 1,
	WARN_LEVEL_WARN = 2,
	WARN_LEVEL_CRITICAL = 3,
};

static const cJSON *
jget(const cJSON *o, const char *key)
{
	return o != NULL ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
}

static const char *
jstr(const cJSON *o, const char *key, const char *fallback)
{
	const cJSON *n = jget(o, key);
	return (cJSON_IsString(n) && n->valuestring != NULL) ? n->valuestring : fallback;
}

static bool
jtrue(const cJSON *o, const char *key)
{
	return cJSON_IsTrue(jget(o, key));
}

static enum warn_level
warning_level(const cJSON *w)
{
	const char *l = jstr(w, "level", "");
	if (strcmp(l, "critical") == 0) {
		return WARN_LEVEL_CRITICAL;
	}
	if (strcmp(l, "warn") == 0) {
		return WARN_LEVEL_WARN;
	}
	return cJSON_IsObject(w) ? WARN_LEVEL_INFO : WARN_LEVEL_NONE;
}

//! Worst level on one screen: its `warnings[]` plus the vendor's worst warning.
static enum warn_level
screen_worst(const cJSON *screen)
{
	enum warn_level worst = WARN_LEVEL_NONE;
	const cJSON *w = NULL;
	cJSON_ArrayForEach(w, jget(screen, "warnings"))
	{
		enum warn_level l = warning_level(w);
		worst = l > worst ? l : worst;
	}
	enum warn_level v = warning_level(jget(jget(screen, "vendor"), "worst_warning"));
	return v > worst ? v : worst;
}

//! The Displays badge: warn + critical across screens (incl. vendor) and system.
static int
count_badge_warnings(const cJSON *snap, bool *any_critical)
{
	int n = 0;
	*any_critical = false;
	const cJSON *scr = NULL;
	cJSON_ArrayForEach(scr, jget(snap, "screens"))
	{
		const cJSON *w = NULL;
		cJSON_ArrayForEach(w, jget(scr, "warnings"))
		{
			enum warn_level l = warning_level(w);
			n += l >= WARN_LEVEL_WARN ? 1 : 0;
			*any_critical = *any_critical || l == WARN_LEVEL_CRITICAL;
		}
		enum warn_level v = warning_level(jget(jget(scr, "vendor"), "worst_warning"));
		n += v >= WARN_LEVEL_WARN ? 1 : 0;
		*any_critical = *any_critical || v == WARN_LEVEL_CRITICAL;
	}
	const cJSON *w = NULL;
	cJSON_ArrayForEach(w, jget(snap, "warnings"))
	{
		enum warn_level l = warning_level(w);
		n += l >= WARN_LEVEL_WARN ? 1 : 0;
		*any_critical = *any_critical || l == WARN_LEVEL_CRITICAL;
	}
	return n;
}

static bool
screen_claimed(const cJSON *screen)
{
	return cJSON_IsString(jget(jget(screen, "claim"), "plugin_id"));
}

struct screen_counts
{
	int monitors, claimed, verified, tracking;
};

static void
count_screens(const cJSON *snap, struct screen_counts *c)
{
	memset(c, 0, sizeof(*c));
	const cJSON *scr = NULL;
	cJSON_ArrayForEach(scr, jget(snap, "screens"))
	{
		c->monitors++;
		if (screen_claimed(scr)) {
			c->claimed++;
			if (strcmp(jstr(jget(scr, "claim"), "confidence", ""), "VERIFIED") == 0) {
				c->verified++;
			}
		}
		if (strcmp(jstr(jget(scr, "eye_tracking"), "state", ""), "TRACKING") == 0) {
			c->tracking++;
		}
	}
}

//! "\\.\DISPLAY5" -> "DISPLAY5" (display only; the id stays the key).
static const char *
short_device(const char *device_name)
{
	const char *bs = strrchr(device_name, '\\');
	return bs != NULL ? bs + 1 : device_name;
}

static const cJSON *
find_screen(const cJSON *snap, const char *id)
{
	const cJSON *scr = NULL;
	cJSON_ArrayForEach(scr, jget(snap, "screens"))
	{
		if (id != NULL && strcmp(jstr(scr, "id", ""), id) == 0) {
			return scr;
		}
	}
	return NULL;
}

//! A screen's short label for cross-references: "DISPLAY5", else its id.
static const char *
screen_label(const cJSON *snap, const char *id)
{
	const cJSON *scr = find_screen(snap, id);
	if (scr == NULL) {
		return id != NULL ? id : "-";
	}
	return short_device(jstr(scr, "device_name", jstr(scr, "id", "?")));
}

//! Bounded append into a fixed buffer.
static void
appendf(char *buf, size_t cap, const char *fmt, ...)
{
	size_t used = strlen(buf);
	if (used + 1 >= cap) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + used, cap - used, fmt, ap);
	va_end(ap);
}

//! The four text lines of one screen card — shared by the card and Copy list.
struct screen_text
{
	char title[192];
	char roles[128];
	char identity[256];
	char claim[256];
	char state[384];
	char vendor[256];
};

static void
format_screen(const cJSON *scr, struct screen_text *t)
{
	memset(t, 0, sizeof(*t));
	const char *device = jstr(scr, "device_name", "?");
	snprintf(t->title, sizeof(t->title), "%s", jstr(scr, "friendly_name", short_device(device)));

	const cJSON *roles = jget(scr, "roles");
	if (jtrue(roles, "os_main")) {
		appendf(t->roles, sizeof(t->roles), "%sOS main", t->roles[0] ? ", " : "");
	}
	if (jtrue(roles, "runtime_default")) {
		appendf(t->roles, sizeof(t->roles), "%sruntime default", t->roles[0] ? ", " : "");
	}
	if (jtrue(roles, "vendor_primary")) {
		appendf(t->roles, sizeof(t->roles), "%svendor primary", t->roles[0] ? ", " : "");
	}

	// device · WxH @ Hz · (x,y) · ×scale · W×H mm
	const cJSON *d = jget(scr, "desktop");
	const cJSON *nat = jget(scr, "native");
	const cJSON *mm = jget(scr, "physical_mm");
	snprintf(t->identity, sizeof(t->identity), "%s · %d×%d", device, (int)get_num(d, "width"),
	         (int)get_num(d, "height"));
	if (get_num(nat, "refresh_mhz") > 0.0) {
		appendf(t->identity, sizeof(t->identity), " @ %.0f Hz", get_num(nat, "refresh_mhz") / 1000.0);
	}
	appendf(t->identity, sizeof(t->identity), " · (%d,%d) · ×%.4g", (int)get_num(d, "left"), (int)get_num(d, "top"),
	        get_num(d, "scale"));
	if (get_num(mm, "width") > 0.0 && get_num(mm, "height") > 0.0) {
		appendf(t->identity, sizeof(t->identity), " · %d×%d mm", (int)get_num(mm, "width"),
		        (int)get_num(mm, "height"));
	} else {
		appendf(t->identity, sizeof(t->identity), " · no physical size");
	}

	// plugin_id · confidence · serial · apis
	const cJSON *claim = jget(scr, "claim");
	if (!screen_claimed(scr)) {
		snprintf(t->claim, sizeof(t->claim), "not claimed by any plug-in");
	} else {
		snprintf(t->claim, sizeof(t->claim), "%s · %s", jstr(claim, "plugin_id", "?"),
		         jstr(claim, "confidence", "?"));
		const char *serial = jstr(claim, "serial", "");
		if (serial[0] != '\0') {
			appendf(t->claim, sizeof(t->claim), " · %s", serial);
		}
		const cJSON *api = NULL;
		bool first = true;
		cJSON_ArrayForEach(api, jget(claim, "apis"))
		{
			if (cJSON_IsString(api)) {
				appendf(t->claim, sizeof(t->claim), "%s%s", first ? " · " : " ", api->valuestring);
				first = false;
			}
		}
	}

	// tracking STATE · lens L · DP: …
	const cJSON *vendor = jget(scr, "vendor");
	snprintf(t->state, sizeof(t->state),
	         "tracking %s · lens %s · DP: ", jstr(jget(scr, "eye_tracking"), "state", "UNKNOWN"),
	         jstr(vendor, "lens", "UNKNOWN"));
	const cJSON *dp = NULL;
	int n_dp = 0;
	cJSON_ArrayForEach(dp, jget(scr, "dps"))
	{
		appendf(t->state, sizeof(t->state), "%sclient %d %s %s %s", n_dp > 0 ? ", " : "",
		        (int)get_num(dp, "client_id"), jstr(dp, "api", "?"), jstr(dp, "kind", "?"),
		        jstr(dp, "backend", "?"));
		n_dp++;
	}
	if (n_dp == 0) {
		appendf(t->state, sizeof(t->state), "none");
	}

	// The vendor cell, only when the plug-in filled it. Strings verbatim.
	if (jtrue(vendor, "present")) {
		snprintf(t->vendor, sizeof(t->vendor), "vendor: %s · %s · %s · tracker %s",
		         jtrue(vendor, "ready") ? "ready" : "not ready",
		         jtrue(vendor, "verified") ? "verified" : "unverified",
		         jtrue(vendor, "calibrated") ? "calibrated" : "not calibrated",
		         jstr(vendor, "tracker", "UNKNOWN"));
		const char *model = jstr(vendor, "model", "");
		const char *serial = jstr(vendor, "serial", "");
		if (model[0] != '\0' || serial[0] != '\0') {
			appendf(t->vendor, sizeof(t->vendor), " · %s %s", model, serial);
		}
	}
}

//! Plain-text dump of the Displays rows, for the clipboard.
static void
copy_display_list(const cJSON *snap)
{
	size_t cap = 32 * 1024;
	char *out = calloc(1, cap);
	if (out == NULL) {
		return;
	}
	const cJSON *gen = jget(snap, "generation");
	struct screen_counts c;
	count_screens(snap, &c);
	appendf(out, cap, "DisplayXR displays - source: %s (gen %.0f/%.0f)\n", jstr(snap, "source", "?"),
	        get_num(gen, "topology"), get_num(gen, "status"));
	appendf(out, cap, "%d monitors · %d claimed (%d verified) · %d tracking\n", c.monitors, c.claimed, c.verified,
	        c.tracking);
	const cJSON *scr = NULL;
	cJSON_ArrayForEach(scr, jget(snap, "screens"))
	{
		struct screen_text t;
		format_screen(scr, &t);
		appendf(out, cap, "\n[%d] %s%s%s%s\n", (int)get_num(scr, "index"), t.title, t.roles[0] ? "  [" : "",
		        t.roles, t.roles[0] ? "]" : "");
		appendf(out, cap, "    %s\n    %s\n    %s\n", t.identity, t.claim, t.state);
		if (t.vendor[0] != '\0') {
			appendf(out, cap, "    %s\n", t.vendor);
		}
		const cJSON *w = NULL;
		cJSON_ArrayForEach(w, jget(scr, "warnings"))
		{
			appendf(out, cap, "    %s %s: %s\n", jstr(w, "level", "?"), jstr(w, "code", "?"),
			        jstr(w, "text", ""));
		}
	}
	const cJSON *w = NULL;
	cJSON_ArrayForEach(w, jget(snap, "warnings"))
	{
		appendf(out, cap, "\nsystem %s %s: %s\n", jstr(w, "level", "?"), jstr(w, "code", "?"),
		        jstr(w, "text", ""));
	}
	SDL_SetClipboardText(out);
	free(out);
}

//! Launch the vendor's dashboard command exactly as handed over (ADR-051 D7):
//! the panel substitutes nothing and parses nothing.
static bool
open_vendor_dashboard(const char *command)
{
#ifdef _WIN32
	// The documented shape is a full command line ("Tool.exe --page … --display …"),
	// which only CreateProcess takes whole; ShellExecute takes a bare target (a
	// path, a URL) and is the fallback for that shape.
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "%s", command);
	STARTUPINFOA si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));
	if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return true;
	}
	return (INT_PTR)ShellExecuteA(NULL, "open", command, NULL, NULL, SW_SHOWNORMAL) > 32;
#else
	(void)command;
	return false;
#endif
}


/*
 *
 * Small drawing helpers. The default ImGui font covers Latin-1 only, so the
 * dot / badge glyphs are drawn, not typed.
 *
 */

static ImVec4
level_colour(enum warn_level l, bool claimed)
{
	switch (l) {
	case WARN_LEVEL_CRITICAL: return COL_RED;
	case WARN_LEVEL_WARN: return COL_AMBER;
	default: return claimed ? COL_GREEN : COL_GREY;
	}
}

//! A filled dot one text-line tall, then stay on the line.
static void
draw_dot(ImVec4 col)
{
	const float sz = igGetFontSize();
	ImVec2 p;
	igGetCursorScreenPos(&p);
	ImDrawList_AddCircleFilled(igGetWindowDrawList(), (ImVec2){p.x + sz * 0.5f, p.y + sz * 0.5f}, sz * 0.3f,
	                           igGetColorU32_Vec4(col), 0);
	igDummy((ImVec2){sz, sz});
	igSameLine(0.0f, -1.0f);
}

//! A role chip on the current line, wrapping to the next if it does not fit.
static void
draw_chip(const char *label)
{
	ImVec2 ts;
	igCalcTextSize(&ts, label, NULL, false, -1.0f);
	const float pad = igGetStyle()->FramePadding.x;
	const float w = ts.x + 2.0f * pad;
	igSameLine(0.0f, -1.0f);
	ImVec2 avail;
	igGetContentRegionAvail(&avail);
	if (avail.x < w) {
		igNewLine();
	}
	ImVec2 p;
	igGetCursorScreenPos(&p);
	ImDrawList *dl = igGetWindowDrawList();
	ImDrawList_AddRectFilled(dl, p, (ImVec2){p.x + w, p.y + ts.y}, igGetColorU32_Vec4(COL_CHIP), ts.y * 0.3f, 0);
	ImDrawList_AddText_Vec2(dl, (ImVec2){p.x + pad, p.y}, igGetColorU32_Vec4(COL_TEXT), label, NULL);
	igDummy((ImVec2){w, ts.y});
}

static void
draw_warning_line(const cJSON *w, const char *prefix)
{
	const enum warn_level l = warning_level(w);
	igIndent(0.0f);
	draw_dot(l >= WARN_LEVEL_WARN ? level_colour(l, true) : COL_GREY);
	// The text is the runtime's (or the vendor's) sentence, shown verbatim.
	igTextColored(l >= WARN_LEVEL_WARN ? level_colour(l, true) : COL_GREY, "%s%s: %s", prefix, jstr(w, "code", "?"),
	              jstr(w, "text", ""));
	igUnindent(0.0f);
}

static void
draw_dashed_line(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float dash, float thickness)
{
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	float len = dx * dx + dy * dy;
	if (len <= 0.0f || dash <= 0.0f) {
		return;
	}
	len = SDL_sqrtf(len);
	for (float t = 0.0f; t < len; t += 2.0f * dash) {
		const float t1 = (t + dash < len) ? t + dash : len;
		ImDrawList_AddLine(dl, (ImVec2){a.x + dx * t / len, a.y + dy * t / len},
		                   (ImVec2){a.x + dx * t1 / len, a.y + dy * t1 / len}, col, thickness);
	}
}

//! The desktop map: every monitor to scale, every client window as an outline,
//! the seams between monitors dashed, each client's owner screen tinted.
static void
draw_desktop_map(const cJSON *snap)
{
	const cJSON *screens = jget(snap, "screens");
	const cJSON *clients = jget(snap, "clients");
	double minx = 0, miny = 0, maxx = 0, maxy = 0;
	bool any = false;
	const cJSON *scr = NULL;
	cJSON_ArrayForEach(scr, screens)
	{
		const cJSON *d = jget(scr, "desktop");
		const double l = get_num(d, "left"), t = get_num(d, "top");
		const double r = l + get_num(d, "width"), b = t + get_num(d, "height");
		if (r <= l || b <= t) {
			continue;
		}
		if (!any) {
			minx = l, miny = t, maxx = r, maxy = b;
			any = true;
		} else {
			minx = l < minx ? l : minx;
			miny = t < miny ? t : miny;
			maxx = r > maxx ? r : maxx;
			maxy = b > maxy ? b : maxy;
		}
	}
	if (!any) {
		igTextDisabled("(no monitor geometry in the snapshot)");
		return;
	}

	ImVec2 avail;
	igGetContentRegionAvail(&avail);
	const float font = igGetFontSize();
	float W = avail.x > font * 4.0f ? avail.x : font * 4.0f;
	double scale = W / (maxx - minx);
	float H = (float)((maxy - miny) * scale);
	const float max_h = font * 12.0f;
	if (H > max_h) {
		scale = max_h / (maxy - miny);
		H = max_h;
		W = (float)((maxx - minx) * scale);
	}

	ImVec2 o;
	igGetCursorScreenPos(&o);
	ImDrawList *dl = igGetWindowDrawList();
#define MAP_X(v) (o.x + (float)(((v) - minx) * scale))
#define MAP_Y(v) (o.y + (float)(((v) - miny) * scale))

	// Monitors, tinted when they own a live window.
	cJSON_ArrayForEach(scr, screens)
	{
		const cJSON *d = jget(scr, "desktop");
		const double l = get_num(d, "left"), t = get_num(d, "top");
		const ImVec2 p0 = {MAP_X(l), MAP_Y(t)};
		const ImVec2 p1 = {MAP_X(l + get_num(d, "width")), MAP_Y(t + get_num(d, "height"))};
		bool owner = false;
		const cJSON *c = NULL;
		cJSON_ArrayForEach(c, clients)
		{
			owner = owner || strcmp(jstr(c, "owner_screen", ""), jstr(scr, "id", "-")) == 0;
		}
		ImDrawList_AddRectFilled(dl, p0, p1, igGetColorU32_Vec4(owner ? COL_MONITOR_OWNER : COL_MONITOR), 0.0f,
		                         0);
		ImDrawList_AddRect(dl, p0, p1, igGetColorU32_Vec4(COL_MONITOR_EDGE), 0.0f, 0, 1.0f);
		char label[256];
		snprintf(label, sizeof(label), "%s\n%s", short_device(jstr(scr, "device_name", "?")),
		         jstr(scr, "friendly_name", ""));
		ImDrawList_PushClipRect(dl, p0, p1, true);
		ImDrawList_AddText_Vec2(dl, (ImVec2){p0.x + font * 0.3f, p0.y + font * 0.2f},
		                        igGetColorU32_Vec4(COL_TEXT), label, NULL);
		ImDrawList_PopClipRect(dl);
	}

	// Seams: a shared vertical or horizontal edge between two monitors.
	const ImU32 seam = igGetColorU32_Vec4(COL_AMBER);
	const cJSON *a = NULL;
	cJSON_ArrayForEach(a, screens)
	{
		const cJSON *da = jget(a, "desktop");
		const double al = get_num(da, "left"), at = get_num(da, "top");
		const double ar = al + get_num(da, "width"), ab = at + get_num(da, "height");
		const cJSON *b = NULL;
		cJSON_ArrayForEach(b, screens)
		{
			if (b == a) {
				continue;
			}
			const cJSON *db = jget(b, "desktop");
			const double bl = get_num(db, "left"), bt = get_num(db, "top");
			const double br = bl + get_num(db, "width"), bb = bt + get_num(db, "height");
			if (ar == bl) { // b to the right of a
				const double y0 = at > bt ? at : bt, y1 = ab < bb ? ab : bb;
				if (y1 > y0) {
					draw_dashed_line(dl, (ImVec2){MAP_X(ar), MAP_Y(y0)},
					                 (ImVec2){MAP_X(ar), MAP_Y(y1)}, seam, font * 0.3f, 2.0f);
				}
			}
			if (ab == bt) { // b below a
				const double x0 = al > bl ? al : bl, x1 = ar < br ? ar : br;
				if (x1 > x0) {
					draw_dashed_line(dl, (ImVec2){MAP_X(x0), MAP_Y(ab)},
					                 (ImVec2){MAP_X(x1), MAP_Y(ab)}, seam, font * 0.3f, 2.0f);
				}
			}
		}
	}

	// Client windows as outlines at their desktop rects.
	const cJSON *c = NULL;
	cJSON_ArrayForEach(c, clients)
	{
		const cJSON *w = jget(c, "window");
		if (!cJSON_IsObject(w) || get_num(w, "width") <= 0.0 || get_num(w, "height") <= 0.0) {
			continue;
		}
		const double l = get_num(w, "left"), t = get_num(w, "top");
		const ImVec2 p0 = {MAP_X(l), MAP_Y(t)};
		const ImVec2 p1 = {MAP_X(l + get_num(w, "width")), MAP_Y(t + get_num(w, "height"))};
		ImDrawList_AddRect(dl, p0, p1, igGetColorU32_Vec4(COL_WINDOW), 0.0f, 0, 2.0f);
		ImDrawList_PushClipRect(dl, p0, p1, true);
		ImDrawList_AddText_Vec2(dl, (ImVec2){p0.x + font * 0.2f, p0.y + font * 0.1f},
		                        igGetColorU32_Vec4(COL_WINDOW), jstr(c, "name", "?"), NULL);
		ImDrawList_PopClipRect(dl);
	}
#undef MAP_X
#undef MAP_Y

	igDummy((ImVec2){W, H});
}


/*
 *
 * Tabs that render the status snapshot.
 *
 */

//! "source: headless - no service reached" — the snapshot has no live rows.
static void
draw_feed_banner(const struct panel_state *s)
{
	if (s->snap != NULL && strcmp(jstr(s->snap, "source", ""), "headless") == 0) {
		igTextColored(COL_AMBER,
		              "source: headless - no service; refreshed every 30 s, Refresh to re-read. Live rows "
		              "(clients, bound DPs, tracking) are absent; this is what a process starting now would "
		              "get.");
	}
	struct feed_view v;
	feed_get_view(s->feed, &v);
	if (v.error[0] != '\0') {
		igTextColored(COL_AMBER, "feed: %s", v.error);
	} else if (v.note[0] != '\0') {
		igTextDisabled("feed: %s", v.note);
	}
}

static void
draw_no_snapshot(const struct panel_state *s)
{
	struct feed_view v;
	feed_get_view(s->feed, &v);
	if (v.error[0] != '\0') {
		igTextColored(COL_RED, "No status snapshot: %s", v.error);
	} else {
		igTextDisabled("Waiting for displayxr-cli status ...");
	}
}

static void
draw_displays_tab(struct panel_state *s)
{
	if (s->snap == NULL) {
		draw_no_snapshot(s);
		return;
	}
	const cJSON *snap = s->snap;
	draw_feed_banner(s);

	struct screen_counts c;
	count_screens(snap, &c);
	igText("%d monitors · %d claimed (%d verified) · %d tracking", c.monitors, c.claimed, c.verified, c.tracking);
	igSameLine(0.0f, -1.0f);
	if (igSmallButton("Copy list")) {
		copy_display_list(snap);
		snprintf(s->last_action, sizeof(s->last_action), "Copied the display list to the clipboard.");
	}

	int idx = 0;
	const cJSON *scr = NULL;
	cJSON_ArrayForEach(scr, jget(snap, "screens"))
	{
		struct screen_text t;
		format_screen(scr, &t);
		const bool claimed = screen_claimed(scr);

		igPushID_Int(idx++);
		igBeginChild_Str("##screen", (ImVec2){0, 0}, ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY, 0);
		igPushTextWrapPos(0.0f);

		draw_dot(level_colour(screen_worst(scr), claimed));
		igText("%s", t.title);
		const cJSON *roles = jget(scr, "roles");
		if (jtrue(roles, "os_main")) {
			draw_chip("OS main");
		}
		if (jtrue(roles, "runtime_default")) {
			draw_chip("runtime default");
		}
		if (jtrue(roles, "vendor_primary")) {
			draw_chip("vendor primary");
		}

		igIndent(0.0f);
		igTextDisabled("%s", t.identity);
		// A claim by the plug-in the PreferredPlugin override forces is amber:
		// it is bound by the override, not by its own confidence (#793).
		const bool forced = claimed && s->have_preferred &&
		                    strcmp(jstr(jget(scr, "claim"), "plugin_id", ""), s->preferred) == 0;
		if (!claimed) {
			igTextDisabled("%s", t.claim);
		} else if (forced) {
			igTextColored(COL_AMBER, "%s   (forced by the PreferredPlugin override)", t.claim);
		} else {
			igText("%s", t.claim);
		}
		igText("%s", t.state);
		if (t.vendor[0] != '\0') {
			igText("%s", t.vendor);
			const char *cmd = jstr(jget(scr, "vendor"), "dashboard_command", "");
			if (cmd[0] != '\0') {
				igSameLine(0.0f, -1.0f);
				if (igSmallButton("Open in vendor ->")) {
					snprintf(s->last_action, sizeof(s->last_action), "%s: %s",
					         open_vendor_dashboard(cmd) ? "Opened" : "Could not open", cmd);
				}
			}
		}
		igUnindent(0.0f);

		const cJSON *w = NULL;
		cJSON_ArrayForEach(w, jget(scr, "warnings"))
		{
			draw_warning_line(w, "");
		}
		const cJSON *vw = jget(jget(scr, "vendor"), "worst_warning");
		if (cJSON_IsObject(vw)) {
			draw_warning_line(vw, "vendor ");
		}

		igPopTextWrapPos();
		igEndChild();
		igPopID();
	}
	if (idx == 0) {
		igTextDisabled("(no monitors enumerated)");
	}

	// System-level warnings (e.g. SERVICE_HEADLESS), in place.
	const cJSON *w = NULL;
	cJSON_ArrayForEach(w, jget(snap, "warnings"))
	{
		draw_warning_line(w, "system ");
	}

	igSeparatorText("Desktop");
	draw_desktop_map(snap);
	igTextDisabled("Monitors to scale; live windows as outlines; seams dashed; a window's owner screen tinted.");
}

static void
draw_windows_tab(struct panel_state *s)
{
	if (s->snap == NULL) {
		draw_no_snapshot(s);
		return;
	}
	const cJSON *snap = s->snap;
	draw_feed_banner(s);

	const cJSON *clients = jget(snap, "clients");
	if (cJSON_GetArraySize(clients) == 0) {
		igTextDisabled("%s", strcmp(jstr(snap, "source", ""), "headless") == 0 ? "no live clients (headless)"
		                                                                       : "no clients");
		return;
	}

	int idx = 0;
	const cJSON *c = NULL;
	cJSON_ArrayForEach(c, clients)
	{
		igPushID_Int(idx++);
		igBeginChild_Str("##client", (ImVec2){0, 0}, ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY, 0);
		igPushTextWrapPos(0.0f);

		const cJSON *fl = jget(c, "flags");
		igText("%s", jstr(c, "name", "?"));
		igSameLine(0.0f, -1.0f);
		igTextDisabled("client %d · pid %d · %s", (int)get_num(c, "id"), (int)get_num(c, "pid"),
		               jstr(c, "class", "?"));
		char flags[96] = "";
		if (jtrue(fl, "active")) {
			appendf(flags, sizeof(flags), "%sactive", flags[0] ? " " : "");
		}
		if (jtrue(fl, "visible")) {
			appendf(flags, sizeof(flags), "%svisible", flags[0] ? " " : "");
		}
		if (jtrue(fl, "focused")) {
			appendf(flags, sizeof(flags), "%sfocused", flags[0] ? " " : "");
		}
		if (jtrue(fl, "overlay")) {
			appendf(flags, sizeof(flags), "%soverlay", flags[0] ? " " : "");
		}

		igIndent(0.0f);
		igText("flags: %s · presenter %s · lease %s", flags[0] ? flags : "none", jstr(c, "presenter", "?"),
		       jstr(c, "lease", "?"));
		const cJSON *win = jget(c, "window");
		if (cJSON_IsObject(win)) {
			igText("window (%d,%d) %d×%d · owner %s", (int)get_num(win, "left"), (int)get_num(win, "top"),
			       (int)get_num(win, "width"), (int)get_num(win, "height"),
			       screen_label(snap, jstr(c, "owner_screen", NULL)));
		} else {
			igText("window: none reported · owner %s", screen_label(snap, jstr(c, "owner_screen", NULL)));
		}

		const cJSON *sg = jget(c, "segments");
		const cJSON *items = jget(sg, "items");
		const int rows = cJSON_GetArraySize(items);
		igTextDisabled("segments gen %.0f%s", get_num(sg, "generation"), jtrue(sg, "split") ? " · split" : "");
		if (rows > 0) {
			const int shown = rows < 6 ? rows : 6;
			const float h = igGetFrameHeightWithSpacing() * ((float)shown + 1.25f);
			const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
			                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
			if (igBeginTable("##segments", 5, tf, (ImVec2){0.0f, h}, 0.0f)) {
				igTableSetupScrollFreeze(0, 1);
				igTableSetupColumn("screen", 0, 1.0f, 0);
				igTableSetupColumn("canvas", 0, 1.6f, 0);
				igTableSetupColumn("has_dp", 0, 0.7f, 0);
				igTableSetupColumn("woven", 0, 0.7f, 0);
				igTableSetupColumn("eyes", 0, 0.8f, 0);
				igTableHeadersRow();
				const cJSON *it = NULL;
				cJSON_ArrayForEach(it, items)
				{
					const cJSON *cv = jget(it, "canvas");
					igTableNextRow(0, 0.0f);
					igTableNextColumn();
					igTextUnformatted(screen_label(snap, jstr(it, "screen", NULL)), NULL);
					igTableNextColumn();
					igText("%d,%d %d×%d", (int)get_num(cv, "x"), (int)get_num(cv, "y"),
					       (int)get_num(cv, "w"), (int)get_num(cv, "h"));
					igTableNextColumn();
					if (jtrue(it, "has_dp")) {
						igTextUnformatted("yes", NULL);
					} else {
						igTextColored(COL_AMBER, "no");
					}
					igTableNextColumn();
					igTextUnformatted(jtrue(it, "woven") ? "yes" : "no", NULL);
					igTableNextColumn();
					igTextUnformatted(jstr(it, "eye_source", "?"), NULL);
				}
				igEndTable();
			}
		}

		// #1248: the view-set counts and the integrity triple + weave placement
		// on ONE row. Raw counters only — the panel computes no rate.
		const cJSON *vw = jget(c, "views");
		const cJSON *in = jget(c, "integrity");
		igText("views %d/%d (reported %d)   paint %.0f · present %.0f · skip %.0f · weave: %s",
		       (int)get_num(vw, "active"), (int)get_num(vw, "capacity"), (int)get_num(vw, "reported"),
		       get_num(in, "paint"), get_num(in, "present"), get_num(in, "skip"),
		       jstr(in, "weave_placement", "-"));

		igUnindent(0.0f);

		// A segment without a DP is flat 2D on its screen: show the same text
		// the screen row carries (SEGMENT_FLAT_2D), verbatim.
		const cJSON *it = NULL;
		cJSON_ArrayForEach(it, items)
		{
			if (jtrue(it, "has_dp")) {
				continue;
			}
			const cJSON *found = NULL;
			const cJSON *w = NULL;
			cJSON_ArrayForEach(w, jget(find_screen(snap, jstr(it, "screen", NULL)), "warnings"))
			{
				if (strcmp(jstr(w, "code", ""), "SEGMENT_FLAT_2D") == 0) {
					found = w;
				}
			}
			if (found != NULL) {
				draw_warning_line(found, "");
			} else {
				// The screen row did not carry it (an older CLI): name the
				// code and the screen, nothing more.
				igIndent(0.0f);
				draw_dot(COL_AMBER);
				igTextColored(COL_AMBER, "SEGMENT_FLAT_2D: segment on %s has no display processor.",
				              screen_label(snap, jstr(it, "screen", NULL)));
				igUnindent(0.0f);
			}
		}

		igPopTextWrapPos();
		igEndChild();
		igPopID();
	}
}

//! Overview's one-line summary (vendor dashboard Home rule): only with more
//! than one claimed screen or a warn / critical warning.
static void
draw_overview_summary(struct panel_state *s)
{
	if (s->snap == NULL) {
		return;
	}
	struct screen_counts c;
	count_screens(s->snap, &c);
	bool crit = false;
	const int warns = count_badge_warnings(s->snap, &crit);
	if (c.claimed <= 1 && warns == 0) {
		return;
	}
	const ImVec4 col = warns == 0 ? COL_GREEN : (crit ? COL_RED : COL_AMBER);
	draw_dot(col);
	igText("%d screens · %d tracking · %d warning%s", c.monitors, c.tracking, warns, warns == 1 ? "" : "s");
	igSameLine(0.0f, -1.0f);
	if (igSmallButton("-> Displays")) {
		s->select_tab = 1;
	}
}

static void
draw_developer_tab(void)
{
	igSeparatorText("Developer settings");
	igTextDisabled(
	    "Phase 2 (design, not implemented yet): a gated list of the Tier 1 + Tier 2 levers "
	    "with name, current value, provenance (env / user / machine / default), compiled "
	    "default and a per-row Reset, plus one 'Reset all performance settings'. Off by "
	    "default and not persisted across panel launches; Tier-4 names never appear.");
	igTextDisabled("Design: docs/roadmap/control-panel-performance-settings.md (Design, Phasing).");
}

//! Right-aligned "source: … · gen T/S" on the current line (top-right).
static void
draw_source_label(const struct panel_state *s)
{
	char label[160];
	if (s->snap != NULL) {
		const cJSON *gen = jget(s->snap, "generation");
		snprintf(label, sizeof(label), "source: %s · gen %.0f/%.0f", jstr(s->snap, "source", "?"),
		         get_num(gen, "topology"), get_num(gen, "status"));
	} else {
		snprintf(label, sizeof(label), "source: -");
	}
	ImVec2 ts;
	igCalcTextSize(&ts, label, NULL, false, -1.0f);
	igSameLine(0.0f, -1.0f);
	ImVec2 avail;
	igGetContentRegionAvail(&avail);
	if (avail.x > ts.x) {
		igSetCursorPosX(igGetCursorPosX() + avail.x - ts.x);
	}
	igTextDisabled("%s", label);
	if (igIsItemHovered(0)) {
		struct feed_view v;
		feed_get_view(s->feed, &v);
		const uint32_t age = v.snapshots > 0 ? (SDL_GetTicks() - v.last_rx_ticks) / 1000u : 0u;
		igSetTooltip("feed: %s\n%s%s%u snapshot(s); newest %u s ago%s%s", v.running ? "held" : "released",
		             v.note[0] ? v.note : "", v.note[0] ? "\n" : "", (unsigned)v.snapshots, (unsigned)age,
		             v.error[0] ? "\nlast error: " : "", v.error);
	}
}

static void
draw_overview_tab(struct panel_state *s)
{
	if (!s->have_info) {
		igSpacing();
		igTextColored(COL_RED, "%s", s->info_err[0] ? s->info_err : "No runtime info.");
		return;
	}

	draw_overview_summary(s);

	// ---- Runtime ----
	igSeparatorText("Runtime");
	igText("Version : %s", s->rt_desc);
	igText("Git tag : %s", s->rt_tag);
	igText("Plug-in ABI : v%d", s->rt_abi);
	if (s->ar_queried) {
		bool is_dxr = s->ar_set && strstr(s->ar_value, "DisplayXR") != NULL;
		igText("Active OpenXR runtime :");
		igSameLine(0.0f, -1.0f);
		igTextColored(is_dxr ? COL_GREEN : COL_RED, "%s", s->ar_set ? s->ar_value : "<unset>");
		if (!is_dxr) {
			if (igButton("Set DisplayXR as active OpenXR runtime", (ImVec2){0, 0})) {
				char out[2048];
				if (run_cli("runtime activate", out, sizeof(out))) {
					char *nl = strchr(out, '\n');
					if (nl != NULL) {
						*nl = '\0';
					}
					snprintf(s->last_action, sizeof(s->last_action), "%s",
					         out[0] ? out : "(done)");
				}
				refresh_info(s);
			}
		}
	}

	// ---- Display processor ----
	igSeparatorText("Display processor");
	if (!s->have_plugin) {
		igTextColored(COL_RED, "No active vendor plug-in.");
	} else {
		igText("Plug-in : %s  (%s)", s->pl_id, s->pl_name[0] ? s->pl_name : "?");
		igText("Vendor  : %s", s->pl_vendor[0] ? s->pl_vendor : "?");
		igText("Version : %s", s->pl_ver[0] ? s->pl_ver : "?");
		igTextColored(COL_GREEN, "ABI v%d (loader-verified match)", s->rt_abi);
		igText("Device  : %s", s->device);
	}
	if (s->n_pstates > 0) {
		igText("Registered plug-ins (platform state):");
		for (int i = 0; i < s->n_pstates; i++) {
			if (s->pstates[i].degraded) {
				igTextColored(COL_AMBER, "  %s", s->pstates[i].line);
			} else {
				igText("  %s", s->pstates[i].line);
			}
		}
	}

	// ---- GPU topology (#918) ----
	//
	// Two kinds of fact, and the section keeps them visibly apart. The adapter
	// list, the scanout/render/ingest adapters and the verdict are properties
	// of the MACHINE. `DXR_WEAVE_ON_SCANOUT`, the ingress policy and the
	// "service split" line are read from the environment of the displayxr-cli
	// CHILD THIS PANEL SPAWNED, which inherits the panel's environment and has
	// nothing to do with the environment a running app or the DisplayXR service
	// was started in. Rendering the second kind as machine state is the trap
	// docs/roadmap/control-panel-performance-settings.md exists to prevent — so
	// it is drawn dimmed, under its own labelled sub-heading, and never in the
	// same visual weight as the adapter list.
	igSeparatorText("GPU topology");
	if (igIsItemHovered(0)) {
		igSetTooltip(
		    "Does the woven frame have to cross adapters to reach the panel? On a hybrid "
		    "laptop the panel is often scanned out by the integrated GPU while the app "
		    "renders on the discrete one (#918 / ADR-037).");
	}
	if (!s->gpu_probed) {
		igTextDisabled("(not probed - %s)", s->gpu_note[0] ? s->gpu_note : "Windows-only");
	} else if (s->n_gpus <= 1) {
		// One adapter: the whole question is moot. A table here would be noise.
		igTextColored(COL_GREEN, "Single adapter - the weave never crosses GPUs.");
		if (s->n_gpus == 1) {
			igTextDisabled("%s  LUID=%s", s->gpus[0].name, s->gpus[0].luid);
		}
	} else {
		for (int i = 0; i < s->n_gpus; i++) {
			struct gpu_row *g = &s->gpus[i];
			bool is_scanout = s->gpu_scanout_resolved && strcmp(g->luid, s->gpu_scanout_luid) == 0;
			bool is_render = s->gpu_render_resolved && strcmp(g->luid, s->gpu_render_luid) == 0;
			igText("[%d] %s", i, g->name);
			igTextDisabled("    LUID=%s  %d MB dedicated%s%s", g->luid, g->vram_mb,
			               is_scanout ? "   <- panel scanout" : "",
			               is_render ? "   <- render (default)" : "");
		}
		igTextColored(s->gpu_split_applies ? COL_AMBER : COL_GREEN, "%s", s->gpu_verdict);
		if (s->gpu_ingest_resolved) {
			igTextDisabled("service ingest: %s (%s)", s->gpu_ingest_name, s->gpu_ingest_provenance);
		}
	}
	if (s->gpu_probed) {
		// The configured half, dimmed and with its provenance stated. "env"
		// means THIS panel's environment, which says nothing about another
		// process; every other source is machine-wide and does.
		const bool weave_from_env = strcmp(s->gpu_weave_source, "env") == 0;
		igTextDisabled("DXR_WEAVE_ON_SCANOUT=%s [%s]%s   ingress=%s",
		               s->gpu_weave_set ? s->gpu_weave : "<unset>",
		               s->gpu_weave_source[0] ? s->gpu_weave_source : "?",
		               weave_from_env ? "  <- this panel's environment only" : "",
		               s->gpu_ingress[0] ? s->gpu_ingress : "?");
		if (s->gpu_service_split[0] != '\0') {
			igTextDisabled("%s", s->gpu_service_split);
		}
		igTextDisabled(
		    "A running app's real placement is the 'weave placement:' line in its log "
		    "(%%LOCALAPPDATA%%\\DisplayXR\\DisplayXR_<exe>.*.log).");
	}

	// ---- Self-test ----
	igSeparatorText("Self-test");
	if (igButton("Run self-test", (ImVec2){0, 0})) {
		refresh_selftest(s);
	}
	if (s->have_selftest) {
		igSameLine(0.0f, -1.0f);
		bool pass = (s->result_code == 0);
		igTextColored(pass ? COL_GREEN : COL_RED, "%s (rc=%d)", s->verdict, s->result_code);
		for (int i = 0; i < s->n_checks; i++) {
			igTextColored(s->checks[i].ok ? COL_GREEN : COL_RED, "  [%s] %s",
			              s->checks[i].ok ? "PASS" : "FAIL", s->checks[i].name);
			igSameLine(0.0f, -1.0f);
			igTextDisabled("- %s", s->checks[i].detail);
		}
	}

	// ---- Tier 1: DP switch ----
	igSeparatorText("Display-processor switch (PreferredPlugin override)");
	igText("PreferredPlugin : %s", s->have_preferred ? s->preferred : "<unset> (auto - by probe order)");
	if (igIsItemHovered(0)) {
		igSetTooltip(
		    "Optional manual override that forces a specific display processor. "
		    "When unset (the normal state), the runtime auto-selects by probe order; "
		    "the active plug-in is shown under 'Display processor' above.");
	}
	for (int i = 0; i < s->n_dp; i++) {
		struct dp_row *r = &s->dps[i];
		igText("%s %s id='%s' (ProbeOrder %d)%s", r->active ? "*" : " ",
		       r->preferred ? "[preferred]" : "          ", r->id, r->order, r->name[0] ? "" : "");
		igSameLine(0.0f, -1.0f);
		char btn[96];
		snprintf(btn, sizeof(btn), "Use##%d", i);
		if (igButton(btn, (ImVec2){0, 0})) {
			char args[128];
			snprintf(args, sizeof(args), "dp use %s", r->id);
			dp_action(s, args);
		}
	}
	if (igButton("Reset to default discovery", (ImVec2){0, 0})) {
		dp_action(s, "dp reset");
	}
	igTextDisabled("Switching takes effect on the next process - restart the service or relaunch your app.");
}

static void
draw_performance_tab(struct panel_state *s)
{
	// ---- Performance (#1252) ----
	//
	// Three controls, deliberately. There is no quality-vs-performance dial in
	// this runtime — the defaults ARE the tuned configuration — so a
	// Quality/Balanced/Performance menu would be fiction. What users actually
	// have is three unrelated needs: which GPU, "is the pipeline the problem",
	// and "I am filing a bug". They are separate axes, so they are separate
	// controls; folding them into one dropdown would produce a combinatorial
	// menu that is both larger and less clear.
	//
	// Every write goes through `displayxr-cli perf`, and the state shown is
	// re-read from the resolved chain afterwards — never assumed from the click.
	igSeparatorText("Performance");
	if (igIsItemHovered(0)) {
		igSetTooltip(
		    "Settings the runtime reads inside each app's own process. They apply to apps "
		    "started AFTER the change - the panel cannot reach into a running app.");
	}

	if (!s->have_info) {
		igTextDisabled("(unavailable - displayxr-cli did not report)");
	} else {
		// -- 1. Target GPU. Only a real choice on a multi-adapter box; on a
		//    single-GPU machine there is nothing to choose, so don't offer it.
		if (s->n_gpus > 1) {
			const char *gpu_now = setting_value(s, "DXR_D3D_FORCE_GPU");
			igText("Target GPU");
			igTextDisabled(
			    "    Which adapter apps render on. 'Panel's adapter' keeps the weave local "
			    "to the display.");
			struct
			{
				const char *label;
				const char *value; // "" = Auto (clear the setting)
			} gpu_opts[] = {
			    {"Auto (recommended)", ""},
			    {"Panel's display adapter", "scanout"},
			    {"High performance (discrete)", "dgpu"},
			    {"Power saving (integrated)", "igpu"},
			};
			for (int i = 0; i < (int)(sizeof(gpu_opts) / sizeof(gpu_opts[0])); i++) {
				const bool active = (gpu_opts[i].value[0] == '\0')
				                        ? (gpu_now[0] == '\0')
				                        : (strcmp(gpu_now, gpu_opts[i].value) == 0);
				char id[96];
				snprintf(id, sizeof(id), "%s##gpu%d", gpu_opts[i].label, i);
				if (igRadioButton_Bool(id, active) && !active) {
					char args[160];
					if (gpu_opts[i].value[0] == '\0') {
						// Both variables, so a mixed D3D/VK state cannot linger.
						perf_action(s, "perf reset DXR_D3D_FORCE_GPU");
						perf_action(s, "perf reset DXR_VK_FORCE_GPU");
					} else {
						snprintf(args, sizeof(args), "perf set DXR_D3D_FORCE_GPU %s",
						         gpu_opts[i].value);
						perf_action(s, args);
						snprintf(args, sizeof(args), "perf set DXR_VK_FORCE_GPU %s",
						         gpu_opts[i].value);
						perf_action(s, args);
					}
				}
			}
			const char *gpu_src = setting_source(s, "DXR_D3D_FORCE_GPU");
			if (strcmp(gpu_src, "env") == 0) {
				igTextColored(COL_AMBER,
				              "    An environment variable is setting this and OUTRANKS the panel.");
			}
			igSpacing();
		}

		// -- 2. Mode. Compatibility turns off the two levers that change what
		//    the DISPLAY PROCESSOR is asked to do — the scanout split and the
		//    repaint. Late weave is deliberately NOT in the bundle: it only
		//    changes when we present on our own swapchain, it already
		//    self-disables where the platform gives no present-timing feedback,
		//    and it is the single largest latency win (96->17 ms on VK), so
		//    bundling it would charge every compatibility click a 5x latency
		//    regression on the lever least likely to be the culprit.
		//
		//    THE REPAINT HALF IS IN-PROCESS ONLY, and the text below says so.
		//    `DXR_WEAVE_REPAINT` has no reader in comp_d3d11_service.cpp,
		//    and wiring one in would be wrong rather than merely missing: the
		//    service's render thread weaves on ITS OWN clock, so "a weave with
		//    no new paint behind it is precisely a #868 repaint"
		//    (comp_d3d11_service.cpp, witness_paint_seq). Repaint is what the
		//    service pipeline IS, not a feature it opts into, so there is no
		//    flag that could switch it off without redesigning the pacing.
		//    Under a workspace controller only the split half of this preset
		//    takes effect.
		const bool compat = compat_mode_on(s);
		igText("Mode");
		if (igRadioButton_Bool("Balanced (default)##mode", !compat) && compat) {
			perf_action(s, "perf reset DXR_WEAVE_ON_SCANOUT");
			perf_action(s, "perf reset DXR_WEAVE_REPAINT");
		}
		if (igRadioButton_Bool("Compatibility##mode", compat) && !compat) {
			perf_action(s, "perf set DXR_WEAVE_ON_SCANOUT 0");
			perf_action(s, "perf set DXR_WEAVE_REPAINT 0");
		}
		igTextDisabled("    Compatibility turns off the cross-adapter weave split and the repaint -");
		igTextDisabled("    for 'the 3D looks wrong, is it the pipeline?'. It COSTS latency; it is not");
		igTextDisabled("    a faster setting. Leave it on Balanced unless you are diagnosing.");
		igTextDisabled("    Under the workspace/shell only the split half applies: the service weaves");
		igTextDisabled("    on its own clock, so its repaint is structural rather than a setting.");
		igSpacing();

		// -- 3. Diagnostics. Pure observers: they change no behaviour, which is
		//    exactly what makes them safe to hand a user.
		const bool diag = setting_value(s, "DXR_FRAME_WITNESS")[0] != '\0' ||
		                  setting_value(s, "DXR_FRAME_STAGE_TIMING")[0] != '\0';
		igText("Diagnostics");
		if (igRadioButton_Bool("Off##diag", !diag) && diag) {
			perf_action(s, "perf reset DXR_FRAME_WITNESS");
			perf_action(s, "perf reset DXR_FRAME_STAGE_TIMING");
		}
		if (igRadioButton_Bool("On (for bug reports)##diag", diag) && !diag) {
			perf_action(s, "perf set DXR_FRAME_WITNESS 5");
			perf_action(s, "perf set DXR_FRAME_STAGE_TIMING 1");
		}
		igTextDisabled("    Adds frame/weave/present counters to each app's log. Changes no behaviour.");

		// -- The anti-stale banner. A setting nobody remembers making is the
		//    failure mode this whole surface has to defend against, so it is
		//    stated loudly with the date and a one-click way out.
		if (any_setting_non_default(s)) {
			igSpacing();
			igTextColored(COL_AMBER, "[!] Non-default performance settings are in force%s%s.",
			              s->settings_user_written[0] ? " since " : "",
			              s->settings_user_written[0] ? s->settings_user_written : "");
			for (int i = 0; i < s->n_settings; i++) {
				if (s->settings[i].set) {
					igTextDisabled("      %s = %s [%s]", s->settings[i].name, s->settings[i].value,
					               s->settings[i].source);
				}
			}
			if (igButton("Reset all performance settings", (ImVec2){0, 0})) {
				perf_action(s, "perf reset");
			}
			igTextDisabled("    Applies to apps started after the change. Values shown as [env] come");
			igTextDisabled("    from an environment variable and cannot be reset from here.");
		}
	}
}

static void
draw_panel(struct panel_state *s)
{
	ImGuiIO *io = igGetIO();
	igSetNextWindowPos((ImVec2){0, 0}, ImGuiCond_Always, (ImVec2){0, 0});
	igSetNextWindowSize(io->DisplaySize, ImGuiCond_Always);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
	                         ImGuiWindowFlags_NoSavedSettings;
	igBegin("DisplayXR Control Panel", NULL, flags);

	// Wrap all text at the window's right edge so long values (device
	// strings, ActiveRuntime path, self-test details) reflow instead of
	// clipping. Pos 0.0f tracks the content-region width, so it follows
	// the window live on resize. Popped before every igEnd() below.
	igPushTextWrapPos(0.0f);

	if (igButton("Refresh", (ImVec2){0, 0})) {
		refresh_all(s);
	}
	igSameLine(0.0f, -1.0f);
	if (igButton("Copy diagnostics", (ImVec2){0, 0})) {
		char dump[16384];
		if (run_cli("info", dump, sizeof(dump))) {
			SDL_SetClipboardText(dump);
			snprintf(s->last_action, sizeof(s->last_action),
			         "Copied diagnostics to clipboard (logs: %%LOCALAPPDATA%%\\DisplayXR).");
		}
	}
	draw_source_label(s);

	// ---- Persistent-override banner (#793 Phase 2) ----
	// `dp use` writes HKLM PreferredPlugin and PERSISTS across reboots — a known
	// footgun: a box left pinned (e.g. to sim_display for a test) keeps using that
	// DP for every app and every reboot until someone notices. Surface it loudly,
	// above everything else, whenever an override is active, with one-click reset.
	// Shown above the tabs, on every tab, so a mis-pinned DP that breaks the
	// runtime is still self-evidently the cause (and its reset's result shows
	// in the last-action line below the tabs).
	if (s->have_preferred) {
		igSpacing();
		igTextColored(COL_AMBER,
		              "[!] DP OVERRIDE ACTIVE - '%s' is forced for every app and persists across "
		              "reboots until reset.",
		              s->preferred);
		const ImVec4 red = {0.60f, 0.12f, 0.12f, 1.0f};
		const ImVec4 red_hi = {0.78f, 0.18f, 0.18f, 1.0f};
		igPushStyleColor_Vec4(ImGuiCol_Button, red);
		igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, red_hi);
		igPushStyleColor_Vec4(ImGuiCol_ButtonActive, red_hi);
		if (igButton("Reset DP override now", (ImVec2){0, 0})) {
			dp_action(s, "dp reset");
		}
		igPopStyleColor(3);
		igSpacing();
		igSeparator();
	}

	// ---- Tabs (ADR-051 D4; docs/roadmap/display-dashboard.md §8) ----
	//
	// Overview and Performance render `info --json` as before; Displays and
	// Windows render the status snapshot. The Displays label carries a badge:
	// the count of warn + critical warnings (info never lights it). The default
	// font has no dot glyph, so the label reserves room and the badge is drawn.
	bool crit = false;
	const int badge = s->snap != NULL ? count_badge_warnings(s->snap, &crit) : 0;
	if (igBeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
		static const char *const names[] = {"Overview", "Displays", "Windows", "Performance", "Developer"};
		for (int i = 0; i < 5; i++) {
			char label[64];
			char badge_text[16] = "";
			if (i == 1 && badge > 0) {
				snprintf(badge_text, sizeof(badge_text), "%d", badge);
				// Spaces wide enough for the drawn dot + number.
				snprintf(label, sizeof(label), "%s %*s###%s", names[i], (int)strlen(badge_text) + 3, "",
				         names[i]);
			} else {
				snprintf(label, sizeof(label), "%s###%s", names[i], names[i]);
			}
			const ImGuiTabItemFlags tif = (s->select_tab == i) ? ImGuiTabItemFlags_SetSelected : 0;
			const bool open = igBeginTabItem(label, NULL, tif);
			if (badge_text[0] != '\0') {
				ImVec2 mx, mn, ts;
				igGetItemRectMin(&mn);
				igGetItemRectMax(&mx);
				igCalcTextSize(&ts, badge_text, NULL, false, -1.0f);
				const float font = igGetFontSize();
				const float pad = igGetStyle()->FramePadding.x;
				const float x = mx.x - pad - ts.x - font;
				const float cy = (mn.y + mx.y) * 0.5f;
				const ImU32 col = igGetColorU32_Vec4(crit ? COL_RED : COL_AMBER);
				ImDrawList *dl = igGetWindowDrawList();
				ImDrawList_AddCircleFilled(dl, (ImVec2){x + font * 0.4f, cy}, font * 0.3f, col, 0);
				ImDrawList_AddText_Vec2(dl, (ImVec2){x + font * 0.85f, cy - ts.y * 0.5f}, col,
				                        badge_text, NULL);
			}
			if (!open) {
				continue;
			}
			switch (i) {
			case 0: draw_overview_tab(s); break;
			case 1: draw_displays_tab(s); break;
			case 2: draw_windows_tab(s); break;
			case 3: draw_performance_tab(s); break;
			default: draw_developer_tab(); break;
			}
			igEndTabItem();
		}
		igEndTabBar();
	}
	s->select_tab = -1;

	if (s->last_action[0] != '\0') {
		igSpacing();
		igSeparator();
		igTextWrapped("%s", s->last_action);
	}

	igPopTextWrapPos();
	igEnd();
}

/*
 *
 * SDL2 + OpenGL3 host.
 *
 */

//! Everything needed to paint one frame. Passed to the resize event watch so
//! the panel can repaint *during* Windows' modal move/size loop — that loop
//! blocks the normal main loop, so without this the content only reflows on
//! mouse release (it appears to "stretch" mid-drag, then snap-wrap).
struct frame_ctx
{
	SDL_Window *win;
	ImGuiIO *io;
	struct panel_state *state;
	void *hwnd; // HWND on Windows, NULL elsewhere
};

//! Paint exactly one frame: new ImGui frame (sized from the live client rect on
//! Windows), the panel, then present.
static void
render_frame(struct frame_ctx *c)
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
#ifdef _WIN32
	// Override the (possibly stale) backend size with the true client rect so
	// the full-window panel + its wrap edge track live resizes.
	if (c->hwnd != NULL) {
		RECT rc;
		if (GetClientRect((HWND)c->hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
			c->io->DisplaySize.x = (float)(rc.right - rc.left);
			c->io->DisplaySize.y = (float)(rc.bottom - rc.top);
			c->io->DisplayFramebufferScale.x = 1.0f;
			c->io->DisplayFramebufferScale.y = 1.0f;
		}
	}
#endif
	igNewFrame();

	draw_panel(c->state);

	igRender();
	glViewport(0, 0, (int)c->io->DisplaySize.x, (int)c->io->DisplaySize.y);
	glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
	SDL_GL_SwapWindow(c->win);
}

//! SDL event watch: fires synchronously as events are *sent*, including from
//! inside Windows' modal resize loop (where the main loop is blocked).
//! Repainting on every size change is what makes the panel reflow live while
//! the user drags the window edge.
static int SDLCALL
resize_event_watch(void *userdata, SDL_Event *e)
{
	if (e->type == SDL_WINDOWEVENT &&
	    (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_EXPOSED)) {
		render_frame((struct frame_ctx *)userdata);
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		return 1;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

	/*
	 * Default window size.
	 *
	 * The base is in 100%-DPI units and is SCALED BY THE DISPLAY's DPI, because
	 * `io->FontGlobalScale` below scales the content: a fixed pixel size holds
	 * only ~1/2.5 as much on a 250% display as it does at 100%, which is
	 * exactly the "the window is too small" report this fixes. Scaling the
	 * window the same way the content is scaled makes it hold the same amount
	 * of panel at any DPI.
	 *
	 * Then clamped to the display's USABLE bounds (work area, taskbar
	 * excluded), so a high scale factor can never open a window bigger than the
	 * screen it opens on. Display 0 because the window is centred there; the
	 * font scale below re-queries whichever display it actually landed on, and
	 * a mismatch on a mixed-DPI multi-monitor box only affects the initial
	 * size of a window the user can resize.
	 */
	int win_w = 900;
	int win_h = 1000;
	{
		float dpi = 96.0f;
		if (SDL_GetDisplayDPI(0, &dpi, NULL, NULL) == 0 && dpi > 0.0f) {
			float s = dpi / 96.0f;
			if (s < 1.0f) {
				s = 1.0f;
			}
			if (s > 3.0f) {
				s = 3.0f;
			}
			win_w = (int)((float)win_w * s);
			win_h = (int)((float)win_h * s);
		}
		SDL_Rect usable;
		if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
			const int max_w = (int)((float)usable.w * 0.9f);
			const int max_h = (int)((float)usable.h * 0.9f);
			if (win_w > max_w) {
				win_w = max_w;
			}
			if (win_h > max_h) {
				win_h = max_h;
			}
		}
	}

	SDL_Window *win =
	    SDL_CreateWindow("DisplayXR Control Panel", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
	                     SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (win == NULL) {
		SDL_Quit();
		return 1;
	}
	SDL_GLContext ctx = SDL_GL_CreateContext(win);
	if (ctx == NULL) {
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}
	SDL_GL_MakeCurrent(win, ctx);
	SDL_GL_SetSwapInterval(1);

	if (gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress) == 0) {
		SDL_GL_DeleteContext(ctx);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	// Scale the UI to the display's DPI (96 = 100%), like u_debug_gui.
	float gui_scale = 1.0f;
	int disp_idx = SDL_GetWindowDisplayIndex(win);
	if (disp_idx >= 0) {
		float dpi = 96.0f;
		if (SDL_GetDisplayDPI(disp_idx, &dpi, NULL, NULL) == 0) {
			gui_scale = dpi / 96.0f;
			if (gui_scale < 1.0f) {
				gui_scale = 1.0f;
			}
			if (gui_scale > 3.0f) {
				gui_scale = 3.0f;
			}
		}
	}

	igCreateContext(NULL);
	ImGuiIO *io = igGetIO();
	io->IniFilename = NULL; // don't litter an imgui.ini
	io->FontGlobalScale = gui_scale;
	igStyleColorsDark(NULL);
	ImGuiStyle_ScaleAllSizes(igGetStyle(), gui_scale);

	ImGui_ImplSDL2_InitForOpenGL(win, ctx);
	ImGui_ImplOpenGL3_Init(NULL);

	struct panel_state state;
	memset(&state, 0, sizeof(state));
	snprintf(state.info_err, sizeof(state.info_err), "Loading... (querying displayxr-cli)");
	state.select_tab = -1;

	// ADR-051 D5: the status feed is held only while the window is visible
	// and not minimised — the child runs for nobody otherwise.
	struct status_feed feed;
	feed_init(&feed);
	state.feed = &feed;
	g_cli_lock = SDL_CreateMutex();
	g_feed = &feed;
	// Started after the first info / dp reads below, so it does not begin by
	// being pre-empted by them.

	// The first query (and every Refresh) spawns displayxr-cli, which loads
	// the vendor plug-in — up to a second or two. Present one frame first so
	// the window paints immediately instead of freezing black on launch.
	bool did_initial = false;

#ifdef _WIN32
	// On this HighDPI Win32 path SDL's cached window size doesn't reliably
	// follow OS resizes, which pinned the ImGui window — and thus the text
	// wrap edge — to the launch width (text clipped instead of reflowing).
	// Drive the window size from the real client rect each frame instead.
	HWND panel_hwnd = NULL;
	{
		SDL_SysWMinfo wmi;
		SDL_VERSION(&wmi.version);
		if (SDL_GetWindowWMInfo(win, &wmi)) {
			panel_hwnd = wmi.info.win.window;
		}
	}
#endif

	struct frame_ctx fctx;
	fctx.win = win;
	fctx.io = io;
	fctx.state = &state;
#ifdef _WIN32
	fctx.hwnd = panel_hwnd;
#else
	fctx.hwnd = NULL;
#endif
	// Repaint during the modal move/size loop (live resize).
	SDL_AddEventWatch(resize_event_watch, &fctx);

	bool running = true;
	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			ImGui_ImplSDL2_ProcessEvent(&e);
			if (e.type == SDL_QUIT) {
				running = false;
			}
			if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
			    e.window.windowID == SDL_GetWindowID(win)) {
				running = false;
			}
			if (e.type == SDL_WINDOWEVENT && e.window.windowID == SDL_GetWindowID(win)) {
				switch (e.window.event) {
				case SDL_WINDOWEVENT_MINIMIZED:
				case SDL_WINDOWEVENT_HIDDEN:
					// Release: end the child and drop the snapshot, so
					// nothing (the badge included) shows stale data.
					feed_stop(&feed);
					if (state.snap != NULL) {
						cJSON_Delete(state.snap);
						state.snap = NULL;
					}
					break;
				case SDL_WINDOWEVENT_RESTORED:
				case SDL_WINDOWEVENT_SHOWN:
				case SDL_WINDOWEVENT_MAXIMIZED:
					if (did_initial) {
						feed_start(&feed);
					}
					break;
				default: break;
				}
			}
		}

		feed_take(&feed, &state.snap);

		if ((SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0) {
			SDL_Delay(50); // nothing to paint; don't spin
			continue;
		}

		render_frame(&fctx);

		if (!did_initial) {
			refresh_info(&state); // not refresh_all: the feed is already up
			refresh_dp(&state);
			did_initial = true;
			feed_start(&feed);
		}
	}

	feed_destroy(&feed);
	g_feed = NULL;
	if (g_cli_lock != NULL) {
		SDL_DestroyMutex(g_cli_lock);
		g_cli_lock = NULL;
	}
	if (state.snap != NULL) {
		cJSON_Delete(state.snap);
		state.snap = NULL;
	}
	SDL_DelEventWatch(resize_event_watch, &fctx);
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	igDestroyContext(NULL);
	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
