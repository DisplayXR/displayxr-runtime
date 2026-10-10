// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `status [--json] [--watch [--interval ms]]` — the display status
 *         snapshot (ADR-051 D4).
 *
 * The one per-screen view the CLI, the Control Panel and agents share: the
 * snapshot from the running service when one is reachable (a session-free
 * DIAG connection, like `clients`), else the headless one this process builds
 * (labelled `source: headless` — "what a process starting now would get", not
 * what running apps see). Text is design §7, JSON is design §3
 * (`docs/roadmap/display-dashboard.md`).
 *
 * `--watch` keeps ONE DIAG connection, polls the service's two generation
 * counters every `--interval` ms (default 500, floor 100) and fetches + prints
 * the snapshot only when one moved — one NDJSON line per change with `--json`
 * (what the Control Panel reads), else the §7 table redrawn in place. Without
 * a service it prints the headless snapshot once and keeps trying to connect.
 * Ends on Ctrl-C or when stdin reaches EOF (a parent that closes the pipe).
 *
 * @author David Fattal
 */

#include "cli_common.h"
#include "cli_query.h" // cli_query_handles + teardown

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_instance.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_status_snapshot.h"
#include "util/u_time.h"
#include "target_status_snapshot.h"

#ifdef CLI_HAVE_IPC
#include "xrt/xrt_results.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client.h"
#include "ipc_client_generated.h"
#endif

#include <cjson/cJSON.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#define cli_isatty _isatty
#define cli_fileno _fileno
#else
#include <poll.h>
#include <unistd.h>
#define cli_isatty isatty
#define cli_fileno fileno
#endif

//! Default / minimum `--watch` poll interval.
#define STATUS_WATCH_DEFAULT_MS 500
#define STATUS_WATCH_MIN_MS 100
//! Without a service, retry the connection this often.
#define STATUS_RECONNECT_MS 2000
//! Keep the last service snapshot this long after losing the service before
//! switching to the headless one (ADR-051 D5.4).
#define STATUS_LOST_GRACE_MS 5000

#ifdef CLI_HAVE_IPC
//! A fetch that keeps straddling a rebuild gives up after this many tries.
#define STATUS_FETCH_TRIES 4

//! Connect to the service as a DIAG client (like `clients`). False: no service (or elevated, on Windows).
static bool
status_connect(struct ipc_connection *ipc_c, xrt_result_t *out_xret)
{
	struct xrt_instance_info ii;
	memset(&ii, 0, sizeof(ii));
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli");
	ii.app_info.declared_client_class = XRT_CLIENT_CLASS_DIAG;
	memset(ipc_c, 0, sizeof(*ipc_c));
	const xrt_result_t xret = ipc_client_connection_init(ipc_c, U_LOGGING_ERROR, &ii);
	if (out_xret != NULL) {
		*out_xret = xret;
	}
	return xret == XRT_SUCCESS;
}

/*!
 * Fetch the service's snapshot over an open DIAG connection: the head with
 * screen 0, the other screens by index, every client row by id, all at one
 * generation (a fetch that straddles a rebuild starts over).
 *
 * @return XRT_SUCCESS, or the first failing call's result (the caller treats
 *         an IPC failure as "service lost").
 */
static xrt_result_t
status_fetch_over(struct ipc_connection *ipc_c, struct xrt_status_snapshot *out)
{
	xrt_result_t xret = XRT_ERROR_IPC_FAILURE;
	for (int attempt = 0; attempt < STATUS_FETCH_TRIES; attempt++) {
		struct xrt_status_head head;
		struct xrt_status_screen screen;
		xret = ipc_call_system_get_status_snapshot(ipc_c, 0, &head, &screen);
		if (xret != XRT_SUCCESS) {
			return xret;
		}
		u_status_snapshot_set_head(out, &head);
		if (out->screen_count > 0) {
			out->screens[0] = screen;
		}
		bool consistent = true;
		for (uint32_t i = 1; i < out->screen_count && consistent; i++) {
			struct xrt_status_head h2;
			xret = ipc_call_system_get_status_snapshot(ipc_c, i, &h2, &out->screens[i]);
			if (xret != XRT_SUCCESS) {
				return xret;
			}
			consistent = u_status_generation_equal(&h2.generation, &head.generation);
		}
		for (uint32_t i = 0; i < out->client_count && consistent; i++) {
			struct xrt_segment_metrics metrics;
			struct xrt_status_generation g;
			const uint32_t id = out->clients[i].id;
			xret = ipc_call_system_get_client_segments(ipc_c, id, &out->clients[i], &metrics, &g);
			if (xret == XRT_ERROR_IPC_FAILURE) {
				// The client left between the head and its row (or the
				// connection broke — the next call says which).
				consistent = false;
				break;
			}
			if (xret != XRT_SUCCESS) {
				return xret;
			}
			consistent = u_status_generation_equal(&g, &head.generation);
		}
		if (consistent) {
			return XRT_SUCCESS;
		}
		xret = XRT_ERROR_IPC_FAILURE;
	}
	// Never settled: hand back the last (possibly mixed-generation) read rather than nothing.
	return XRT_SUCCESS;
}
#endif // CLI_HAVE_IPC

/*!
 * The live snapshot from the running service over the session-free DIAG path
 * (`system_get_status_snapshot` + `system_get_client_segments`). False when no
 * service is reachable — none running, or (Windows) this process is elevated —
 * and the caller falls back to the headless build.
 */
static bool
status_fetch_service(struct xrt_status_snapshot *out, xrt_result_t *out_why)
{
#ifdef CLI_HAVE_IPC
	struct ipc_connection ipc_c;
	if (!status_connect(&ipc_c, out_why)) {
		return false;
	}
	const xrt_result_t xret = status_fetch_over(&ipc_c, out);
	ipc_client_connection_fini(&ipc_c);
	if (out_why != NULL) {
		*out_why = xret;
	}
	return xret == XRT_SUCCESS;
#else
	(void)out;
	if (out_why != NULL) {
		*out_why = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	return false;
#endif
}

//! Build the headless snapshot: an instance + system with NO compositor, as `info` does.
static void
status_build_headless(struct xrt_status_snapshot *out)
{
	struct cli_query_handles h;
	memset(&h, 0, sizeof(h));
	if (xrt_instance_create(NULL, &h.xi) == XRT_SUCCESS) {
		(void)xrt_instance_create_system(h.xi, &h.xsys, &h.xsysd, &h.xso, NULL);
	}
	target_status_snapshot_build_headless(h.xi, h.xsysd, out);
	cli_query_teardown(&h);
}

void
cli_status_build_headless(struct xrt_status_snapshot *out)
{
	status_build_headless(out);
}

//! Say why the snapshot is headless — on stderr, so stdout stays the snapshot.
static void
note_headless(xrt_result_t why)
{
	fprintf(stderr,
	        "displayxr-cli status: no service reached (xrt_result=%d) — showing the HEADLESS snapshot (what a "
	        "process starting now would get).%s\n",
	        (int)why,
#ifdef XRT_OS_WINDOWS
	        " Is displayxr-service running? Run from a NON-elevated prompt."
#else
	        " Is displayxr-service running?"
#endif
	);
}

/*!
 * Print @p snap: pretty JSON (once), one compact NDJSON line (`--watch
 * --json`), or the §7 table (redrawn in place under `--watch` on a TTY).
 * False when stdout is gone (the reader closed the pipe).
 */
static bool
status_print(const struct xrt_status_snapshot *snap, bool json, bool watch, bool redraw)
{
	if (json) {
		cJSON *root = u_status_snapshot_to_cjson(snap);
		char *s = watch ? cJSON_PrintUnformatted(root) : cJSON_Print(root);
		if (s != NULL) {
			printf("%s\n", s);
			cJSON_free(s);
		}
		cJSON_Delete(root);
	} else {
		const size_t need = u_status_snapshot_format_text(snap, NULL, 0);
		char *buf = malloc(need + 1);
		if (buf != NULL) {
			(void)u_status_snapshot_format_text(snap, buf, need + 1);
			if (redraw) {
				fputs("\x1b[2J\x1b[H", stdout); // clear + home
			} else if (watch) {
				fputs("\n", stdout);
			}
			fputs(buf, stdout);
			free(buf);
		}
	}
	fflush(stdout);
	return ferror(stdout) == 0;
}


/*
 *
 * --watch.
 *
 */

static volatile sig_atomic_t g_stop = 0;

static void
on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/*!
 * True once stdin has reached EOF — the parent that spawned us closed its end
 * (the Control Panel's way to stop a `--watch` child). A console stdin never
 * reports EOF here (Ctrl-C ends a console session); anything we read is
 * discarded.
 */
static bool
stdin_closed(void)
{
#ifdef XRT_OS_WINDOWS
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	if (h == NULL || h == INVALID_HANDLE_VALUE) {
		return false;
	}
	if (GetFileType(h) != FILE_TYPE_PIPE) {
		return false; // console, NUL, a file: no "parent closed the pipe" signal
	}
	DWORD avail = 0;
	if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
		return GetLastError() == ERROR_BROKEN_PIPE;
	}
	while (avail > 0) {
		char buf[256];
		DWORD got = 0;
		if (!ReadFile(h, buf, avail < sizeof(buf) ? avail : (DWORD)sizeof(buf), &got, NULL) || got == 0) {
			return true;
		}
		avail -= got;
	}
	return false;
#else
	if (cli_isatty(STDIN_FILENO)) {
		return false;
	}
	struct pollfd p = {STDIN_FILENO, POLLIN, 0};
	if (poll(&p, 1, 0) <= 0) {
		return false;
	}
	if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 && (p.revents & POLLIN) == 0) {
		return true;
	}
	if ((p.revents & POLLIN) != 0) {
		char buf[256];
		const ssize_t got = read(STDIN_FILENO, buf, sizeof(buf));
		return got == 0;
	}
	return false;
#endif
}

//! Sleep @p ms in short slices so Ctrl-C / stdin EOF end the watch promptly.
static void
watch_sleep(uint32_t ms)
{
	uint32_t left = ms;
	while (left > 0 && !g_stop) {
		const uint32_t slice = left < 50 ? left : 50;
		os_nanosleep((int64_t)slice * U_TIME_1MS_IN_NS);
		left -= slice;
	}
}

//! Can the console take the clear-screen escape? (Enables VT on Windows.)
static bool
stdout_can_redraw(void)
{
	if (!cli_isatty(cli_fileno(stdout))) {
		return false;
	}
#ifdef XRT_OS_WINDOWS
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode = 0;
	if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) {
		return false;
	}
	return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
	return true;
#endif
}

static int
status_watch(uint32_t interval_ms, bool json)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
#ifndef XRT_OS_WINDOWS
	signal(SIGPIPE, SIG_IGN); // a closed stdout surfaces as a write error instead
#endif
	const bool redraw = !json && stdout_can_redraw();

	struct xrt_status_snapshot *snap = calloc(1, sizeof(*snap));
	if (snap == NULL) {
		fprintf(stderr, "status: out of memory\n");
		return 1;
	}

#ifdef CLI_HAVE_IPC
	struct ipc_connection ipc_c;
	bool connected = false;
	bool have_gen = false;
	struct xrt_status_generation last = {0, 0};
	uint64_t next_connect_ns = 0;
	uint64_t lost_ns = 0;     // when the service went away (0 = never had it / still there)
	bool printed_any = false; // anything printed at all
	bool showing_headless = false;

	while (!g_stop && !stdin_closed()) {
		const uint64_t now = os_monotonic_get_ns();
		if (!connected && now >= next_connect_ns) {
			xrt_result_t why = XRT_SUCCESS;
			connected = status_connect(&ipc_c, &why);
			next_connect_ns = now + (uint64_t)STATUS_RECONNECT_MS * U_TIME_1MS_IN_NS;
			if (connected) {
				have_gen = false;
				lost_ns = 0;
			} else {
				// No service: the headless snapshot once (on start, or once the
				// last service snapshot is older than the grace period).
				const bool grace_over =
				    lost_ns != 0 && now - lost_ns >= (uint64_t)STATUS_LOST_GRACE_MS * U_TIME_1MS_IN_NS;
				if (!showing_headless && (!printed_any || grace_over)) {
					note_headless(why);
					status_build_headless(snap);
					if (!status_print(snap, json, true, redraw)) {
						break;
					}
					printed_any = true;
					showing_headless = true;
				}
			}
		}
		if (connected) {
			struct xrt_status_generation g;
			xrt_result_t xret = ipc_call_system_get_status_generation(&ipc_c, &g);
			if (xret == XRT_SUCCESS && (!have_gen || !u_status_generation_equal(&g, &last))) {
				xret = status_fetch_over(&ipc_c, snap);
				if (xret == XRT_SUCCESS) {
					last = snap->generation;
					have_gen = true;
					showing_headless = false;
					if (!status_print(snap, json, true, redraw)) {
						break;
					}
					printed_any = true;
				}
			}
			if (xret != XRT_SUCCESS) {
				// Lost the service (or a refusal): keep the last snapshot on
				// screen, reconnect from the top.
				ipc_client_connection_fini(&ipc_c);
				connected = false;
				lost_ns = os_monotonic_get_ns();
				next_connect_ns = lost_ns + (uint64_t)STATUS_RECONNECT_MS * U_TIME_1MS_IN_NS;
				if (xret == XRT_ERROR_NOT_AUTHORIZED) {
					fprintf(
					    stderr,
					    "displayxr-cli status: the service refused the status read (not DIAG).\n");
				}
			}
		}
		watch_sleep(connected ? interval_ms : 200);
	}
	if (connected) {
		ipc_client_connection_fini(&ipc_c);
	}
#else
	// No IPC client in this build: the headless snapshot, once, then wait.
	(void)interval_ms;
	note_headless(XRT_ERROR_FEATURE_NOT_SUPPORTED);
	status_build_headless(snap);
	if (status_print(snap, json, true, redraw)) {
		while (!g_stop && !stdin_closed()) {
			watch_sleep(200);
		}
	}
#endif
	free(snap);
	return 0;
}

int
cli_cmd_status(int argc, const char **argv)
{
	const bool json = cli_has_flag(argc, argv, "--json");
	const bool watch = cli_has_flag(argc, argv, "--watch");
	uint32_t interval_ms = STATUS_WATCH_DEFAULT_MS;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
			const long v = strtol(argv[++i], NULL, 10);
			interval_ms = v < STATUS_WATCH_MIN_MS ? STATUS_WATCH_MIN_MS : (uint32_t)v;
		}
	}
	if (watch) {
		return status_watch(interval_ms, json);
	}

	// ~tens of KB: keep it off the stack.
	struct xrt_status_snapshot *snap = calloc(1, sizeof(*snap));
	if (snap == NULL) {
		fprintf(stderr, "status: out of memory\n");
		return 1;
	}
	xrt_result_t why = XRT_SUCCESS;
	if (!status_fetch_service(snap, &why)) {
		note_headless(why);
		status_build_headless(snap);
	}
	(void)status_print(snap, json, false, false);
	free(snap);
	return 0;
}
