// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stdio ↔ per-PID-socket/pipe bridge for the DisplayXR MCP server.
 *
 * Two blocking threads pump bytes in opposite directions. No parsing.
 * Works on POSIX (unix domain socket) and Windows (named pipe).
 */

#include "util/u_mcp_transport.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef _WIN32
typedef SSIZE_T ssize_t;
static ssize_t
read_fd(int fd, void *buf, size_t n)
{
	return _read(fd, buf, (unsigned)n);
}
static ssize_t
write_fd(int fd, const void *buf, size_t n)
{
	return _write(fd, buf, (unsigned)n);
}
#define STDIN_FD _fileno(stdin)
#define STDOUT_FD _fileno(stdout)
#else
static ssize_t
read_fd(int fd, void *buf, size_t n)
{
	return read(fd, buf, n);
}
static ssize_t
write_fd(int fd, const void *buf, size_t n)
{
	return write(fd, buf, n);
}
#define STDIN_FD STDIN_FILENO
#define STDOUT_FD STDOUT_FILENO
#endif

static void
usage(const char *argv0)
{
	fprintf(stderr,
	        "usage: %s [--target <auto|service|pid:N>] | --pid <N|auto> | --list\n"
	        "  --target auto      try service socket, then unique PID session (default)\n"
	        "  --target service   attach to the displayxr-service MCP endpoint\n"
	        "  --target pid:N     attach to a specific runtime process\n"
	        "  --pid N | auto     back-compat form of --target pid:N / --target auto\n"
	        "  --list             print discovered sessions and exit\n",
	        argv0);
}

// Thread args: pump stdin→conn or conn→stdout.
struct pump_args
{
	struct u_mcp_conn *conn;
	bool stdin_to_conn;
};

static void *
pump_thread(void *arg)
{
	struct pump_args *a = arg;
	char buf[4096];
	if (a->stdin_to_conn) {
		for (;;) {
			ssize_t r = read_fd(STDIN_FD, buf, sizeof(buf));
			if (r <= 0) {
				break;
			}
			if (!u_mcp_conn_write(a->conn, buf, (size_t)r)) {
				break;
			}
		}
	} else {
		for (;;) {
			// The transport's read is blocking and fetches as many
			// bytes as the caller asks for — pull in chunks instead.
			char c;
			if (!u_mcp_conn_read(a->conn, &c, 1)) {
				break;
			}
			// Drain any more bytes already queued — simple one-at-a-time
			// keeps the adapter simple and low-latency.
			if (write_fd(STDOUT_FD, &c, 1) <= 0) {
				break;
			}
		}
	}
	return NULL;
}

int
main(int argc, char **argv)
{
#ifdef _WIN32
	// MCP peers speak raw binary framing; disable CRLF translation.
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	const char *target_arg = NULL; // "auto", "service", or "pid:N"
	bool list_mode = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
			target_arg = argv[++i];
		} else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
			// Back-compat: --pid N → --target pid:N ; --pid auto → --target auto.
			const char *v = argv[++i];
			static char buf[64];
			if (strcmp(v, "auto") == 0) {
				target_arg = "auto";
			} else {
				snprintf(buf, sizeof(buf), "pid:%s", v);
				target_arg = buf;
			}
		} else if (strcmp(argv[i], "--list") == 0) {
			list_mode = true;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	if (list_mode) {
		long pids[64];
		size_t n = u_mcp_enumerate_sessions(pids, 64);
		// Report the service endpoint too if the socket/pipe exists. A
		// trial connect is the cheapest cross-platform probe.
		struct u_mcp_conn *probe = u_mcp_conn_connect_named("service");
		if (probe != NULL) {
			printf("service\n");
			u_mcp_conn_close(probe);
		}
		for (size_t i = 0; i < n; i++) {
			printf("%ld\n", (long)pids[i]);
		}
		return 0;
	}

	if (target_arg == NULL) {
		target_arg = "auto";
	}

	struct u_mcp_conn *conn = NULL;
	const char *connected_label = NULL;

	if (strcmp(target_arg, "service") == 0) {
		conn = u_mcp_conn_connect_named("service");
		connected_label = "service";
	} else if (strncmp(target_arg, "pid:", 4) == 0) {
		long pid = (long)strtol(target_arg + 4, NULL, 10);
		if (pid <= 0) {
			usage(argv[0]);
			return 2;
		}
		conn = u_mcp_conn_connect(pid);
		static char buf[32];
		snprintf(buf, sizeof(buf), "pid %ld", pid);
		connected_label = buf;
	} else if (strcmp(target_arg, "auto") == 0) {
		// Service first — covers the Phase B shell story.
		conn = u_mcp_conn_connect_named("service");
		if (conn != NULL) {
			connected_label = "service";
		} else {
			// Fall back to a unique handle-app session.
			long pids[64];
			size_t n = u_mcp_enumerate_sessions(pids, 64);
			if (n == 0) {
				fprintf(stderr, "displayxr-mcp: no running MCP sessions found\n");
				return 1;
			}
			if (n > 1) {
				fprintf(stderr, "displayxr-mcp: %zu sessions found, pass --target pid:N explicitly\n", n);
				return 1;
			}
			conn = u_mcp_conn_connect(pids[0]);
			static char buf[32];
			snprintf(buf, sizeof(buf), "pid %ld", pids[0]);
			connected_label = buf;
		}
	} else {
		usage(argv[0]);
		return 2;
	}

	if (conn == NULL) {
		fprintf(stderr, "displayxr-mcp: cannot connect to %s\n", connected_label ? connected_label : target_arg);
		return 1;
	}

	struct pump_args up = {.conn = conn, .stdin_to_conn = true};
	struct pump_args down = {.conn = conn, .stdin_to_conn = false};

	pthread_t t_up, t_down;
	pthread_create(&t_up, NULL, pump_thread, &up);
	pthread_create(&t_down, NULL, pump_thread, &down);

	// When the stdin-side thread exits (client closed stdin), tear the
	// connection so the conn-side thread can unblock its read.
	pthread_join(t_up, NULL);
	u_mcp_conn_close(conn);
	pthread_join(t_down, NULL);
	return 0;
}
