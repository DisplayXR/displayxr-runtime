// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stdio ↔ per-PID-socket bridge for the DisplayXR MCP server.
 *
 * Claude Code / Cursor launches this binary with `--pid <N|auto>`.
 * We pump byte streams between stdio (MCP over Content-Length frames)
 * and the runtime's unix socket. No parsing — the adapter is a dumb pipe.
 */

#include "util/u_mcp_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void
usage(const char *argv0)
{
	fprintf(stderr,
	        "usage: %s --pid <N|auto> | --list\n"
	        "  --pid N     attach to a specific runtime process\n"
	        "  --pid auto  attach iff exactly one MCP session exists\n"
	        "  --list      print discovered sessions and exit\n",
	        argv0);
}

static bool
set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		return false;
	}
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int
pump(int from, int to)
{
	char buf[4096];
	for (;;) {
		ssize_t r = read(from, buf, sizeof(buf));
		if (r == 0) {
			return 0; // EOF on this side
		}
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return 1; // drained, keep polling
			}
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		char *p = buf;
		size_t left = (size_t)r;
		while (left > 0) {
			ssize_t w = write(to, p, left);
			if (w <= 0) {
				if (w < 0 && errno == EINTR) {
					continue;
				}
				return -1;
			}
			p += w;
			left -= (size_t)w;
		}
	}
}

int
main(int argc, char **argv)
{
	const char *pid_arg = NULL;
	bool list_mode = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
			pid_arg = argv[++i];
		} else if (strcmp(argv[i], "--list") == 0) {
			list_mode = true;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	if (list_mode) {
		pid_t pids[64];
		size_t n = u_mcp_enumerate_sessions(pids, 64);
		for (size_t i = 0; i < n; i++) {
			printf("%ld\n", (long)pids[i]);
		}
		return 0;
	}

	if (pid_arg == NULL) {
		usage(argv[0]);
		return 2;
	}

	pid_t pid = 0;
	if (strcmp(pid_arg, "auto") == 0) {
		pid_t pids[64];
		size_t n = u_mcp_enumerate_sessions(pids, 64);
		if (n == 0) {
			fprintf(stderr, "displayxr-mcp: no running MCP sessions found\n");
			return 1;
		}
		if (n > 1) {
			fprintf(stderr, "displayxr-mcp: %zu sessions found, pass --pid <N> explicitly\n", n);
			return 1;
		}
		pid = pids[0];
	} else {
		pid = (pid_t)strtol(pid_arg, NULL, 10);
		if (pid <= 0) {
			usage(argv[0]);
			return 2;
		}
	}

	struct u_mcp_conn *conn = u_mcp_conn_connect(pid);
	if (conn == NULL) {
		fprintf(stderr, "displayxr-mcp: cannot connect to pid %ld\n", (long)pid);
		return 1;
	}
	int sock_fd = u_mcp_conn_fd(conn);

	if (!set_nonblock(STDIN_FILENO) || !set_nonblock(sock_fd)) {
		fprintf(stderr, "displayxr-mcp: fcntl failed: %s\n", strerror(errno));
		u_mcp_conn_close(conn);
		return 1;
	}

	struct pollfd pfds[2] = {
	    {.fd = STDIN_FILENO, .events = POLLIN},
	    {.fd = sock_fd, .events = POLLIN},
	};

	bool stdin_open = true;
	bool sock_open = true;
	while (stdin_open && sock_open) {
		pfds[0].fd = stdin_open ? STDIN_FILENO : -1;
		pfds[1].fd = sock_open ? sock_fd : -1;
		int n = poll(pfds, 2, -1);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (pfds[0].revents & (POLLIN | POLLHUP)) {
			int r = pump(STDIN_FILENO, sock_fd);
			if (r == 0) {
				stdin_open = false;
			} else if (r < 0) {
				break;
			}
		}
		if (pfds[1].revents & (POLLIN | POLLHUP)) {
			int r = pump(sock_fd, STDOUT_FILENO);
			if (r == 0) {
				sock_open = false;
			} else if (r < 0) {
				break;
			}
		}
	}

	u_mcp_conn_close(conn);
	return 0;
}
