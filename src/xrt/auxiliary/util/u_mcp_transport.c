// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  POSIX unix-socket transport for the MCP server.
 * @ingroup aux_util
 */

#include "xrt/xrt_config_os.h"

#include "u_mcp_transport.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XRT_OS_WINDOWS
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define LOG_PFX "[mcp-transport] "
#define SOCK_PREFIX "/tmp/displayxr-mcp-"
#define SOCK_SUFFIX ".sock"

#ifndef XRT_OS_WINDOWS

struct u_mcp_listener
{
	int fd;
	char path[128];
};

struct u_mcp_conn
{
	int fd;
};

static void
build_sock_path(char *out, size_t cap, pid_t pid)
{
	snprintf(out, cap, "%s%ld%s", SOCK_PREFIX, (long)pid, SOCK_SUFFIX);
}

struct u_mcp_listener *
u_mcp_listener_open(pid_t pid)
{
	struct u_mcp_listener *l = U_TYPED_CALLOC(struct u_mcp_listener);
	l->fd = -1;
	build_sock_path(l->path, sizeof(l->path), pid);

	// Always unlink any stale socket; we own /tmp/displayxr-mcp-<pid>.sock.
	(void)unlink(l->path);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		U_LOG_W(LOG_PFX "socket() failed: %s", strerror(errno));
		free(l);
		return NULL;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", l->path);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		U_LOG_W(LOG_PFX "bind(%s) failed: %s", l->path, strerror(errno));
		close(fd);
		free(l);
		return NULL;
	}

	// Owner-only access.
	(void)chmod(l->path, 0600);

	if (listen(fd, 4) != 0) {
		U_LOG_W(LOG_PFX "listen() failed: %s", strerror(errno));
		close(fd);
		(void)unlink(l->path);
		free(l);
		return NULL;
	}

	l->fd = fd;
	U_LOG_I(LOG_PFX "listening on %s", l->path);
	return l;
}

struct u_mcp_conn *
u_mcp_listener_accept(struct u_mcp_listener *listener)
{
	if (listener == NULL || listener->fd < 0) {
		return NULL;
	}
	int cfd = accept(listener->fd, NULL, NULL);
	if (cfd < 0) {
		return NULL;
	}
	struct u_mcp_conn *c = U_TYPED_CALLOC(struct u_mcp_conn);
	c->fd = cfd;
	return c;
}

void
u_mcp_listener_close(struct u_mcp_listener *listener)
{
	if (listener == NULL) {
		return;
	}
	if (listener->fd >= 0) {
		// shutdown() wakes a blocking accept() in the server thread.
		(void)shutdown(listener->fd, SHUT_RDWR);
		close(listener->fd);
	}
	(void)unlink(listener->path);
	free(listener);
}

bool
u_mcp_conn_read(struct u_mcp_conn *conn, void *buf, size_t len)
{
	if (conn == NULL) {
		return false;
	}
	uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = read(conn->fd, p, len);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

bool
u_mcp_conn_write(struct u_mcp_conn *conn, const void *buf, size_t len)
{
	if (conn == NULL) {
		return false;
	}
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = write(conn->fd, p, len);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

void
u_mcp_conn_close(struct u_mcp_conn *conn)
{
	if (conn == NULL) {
		return;
	}
	if (conn->fd >= 0) {
		close(conn->fd);
	}
	free(conn);
}

int
u_mcp_conn_fd(struct u_mcp_conn *conn)
{
	return conn != NULL ? conn->fd : -1;
}

struct u_mcp_conn *
u_mcp_conn_connect(pid_t pid)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return NULL;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	build_sock_path(addr.sun_path, sizeof(addr.sun_path), pid);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return NULL;
	}
	struct u_mcp_conn *c = U_TYPED_CALLOC(struct u_mcp_conn);
	c->fd = fd;
	return c;
}

size_t
u_mcp_enumerate_sessions(pid_t *out_pids, size_t cap)
{
	size_t n = 0;
	DIR *d = opendir("/tmp");
	if (d == NULL) {
		return 0;
	}
	const char *prefix = "displayxr-mcp-";
	const char *suffix = ".sock";
	size_t plen = strlen(prefix);
	size_t slen = strlen(suffix);
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < cap) {
		const char *name = de->d_name;
		size_t nlen = strlen(name);
		if (nlen <= plen + slen) {
			continue;
		}
		if (strncmp(name, prefix, plen) != 0) {
			continue;
		}
		if (strcmp(name + nlen - slen, suffix) != 0) {
			continue;
		}
		long pid = strtol(name + plen, NULL, 10);
		if (pid <= 0) {
			continue;
		}
		out_pids[n++] = (pid_t)pid;
	}
	closedir(d);
	return n;
}

#else // XRT_OS_WINDOWS — named pipe transport
//
// Uses \\.\pipe\displayxr-mcp-<pid> with PIPE_TYPE_BYTE; framing (Content-
// Length) is handled at a higher layer just like on POSIX. Enumeration
// uses FindFirstFile over \\.\pipe\* (Named Pipe filesystem).

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

#define PIPE_PREFIX "\\\\.\\pipe\\displayxr-mcp-"

struct u_mcp_listener
{
	HANDLE pipe;
	char name[128];
	volatile LONG closed;
};

struct u_mcp_conn
{
	HANDLE pipe;
	bool owns_handle;
};

static void
build_pipe_name(char *out, size_t cap, pid_t pid)
{
	snprintf(out, cap, PIPE_PREFIX "%ld", (long)pid);
}

struct u_mcp_listener *
u_mcp_listener_open(pid_t pid)
{
	struct u_mcp_listener *l = U_TYPED_CALLOC(struct u_mcp_listener);
	build_pipe_name(l->name, sizeof(l->name), pid);
	l->pipe = CreateNamedPipeA(
	    l->name,
	    PIPE_ACCESS_DUPLEX,
	    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
	    1,     // Max instances (single client).
	    65536, // Out buffer.
	    65536, // In buffer.
	    0,
	    NULL);
	if (l->pipe == INVALID_HANDLE_VALUE) {
		U_LOG_W(LOG_PFX "CreateNamedPipe(%s) failed: %lu", l->name, GetLastError());
		free(l);
		return NULL;
	}
	U_LOG_I(LOG_PFX "listening on %s", l->name);
	return l;
}

struct u_mcp_conn *
u_mcp_listener_accept(struct u_mcp_listener *listener)
{
	if (listener == NULL) {
		return NULL;
	}
	BOOL ok = ConnectNamedPipe(listener->pipe, NULL);
	if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
		return NULL;
	}
	if (InterlockedCompareExchange(&listener->closed, 0, 0)) {
		return NULL;
	}
	struct u_mcp_conn *c = U_TYPED_CALLOC(struct u_mcp_conn);
	c->pipe = listener->pipe;
	c->owns_handle = false; // listener retains ownership
	return c;
}

void
u_mcp_listener_close(struct u_mcp_listener *listener)
{
	if (listener == NULL) {
		return;
	}
	InterlockedExchange(&listener->closed, 1);
	if (listener->pipe != INVALID_HANDLE_VALUE) {
		// Unblock any thread waiting in ConnectNamedPipe / ReadFile.
		DisconnectNamedPipe(listener->pipe);
		CloseHandle(listener->pipe);
	}
	free(listener);
}

bool
u_mcp_conn_read(struct u_mcp_conn *conn, void *buf, size_t len)
{
	if (conn == NULL) {
		return false;
	}
	char *p = buf;
	while (len > 0) {
		DWORD got = 0;
		if (!ReadFile(conn->pipe, p, (DWORD)len, &got, NULL) || got == 0) {
			return false;
		}
		p += got;
		len -= got;
	}
	return true;
}

bool
u_mcp_conn_write(struct u_mcp_conn *conn, const void *buf, size_t len)
{
	if (conn == NULL) {
		return false;
	}
	const char *p = buf;
	while (len > 0) {
		DWORD wrote = 0;
		if (!WriteFile(conn->pipe, p, (DWORD)len, &wrote, NULL) || wrote == 0) {
			return false;
		}
		p += wrote;
		len -= wrote;
	}
	return true;
}

void
u_mcp_conn_close(struct u_mcp_conn *conn)
{
	if (conn == NULL) {
		return;
	}
	if (conn->owns_handle && conn->pipe != INVALID_HANDLE_VALUE) {
		CloseHandle(conn->pipe);
	}
	free(conn);
}

int
u_mcp_conn_fd(struct u_mcp_conn *conn)
{
	(void)conn;
	return -1; // Windows clients cannot poll() a pipe HANDLE; adapter uses threads.
}

struct u_mcp_conn *
u_mcp_conn_connect(pid_t pid)
{
	char name[128];
	build_pipe_name(name, sizeof(name), pid);
	HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	DWORD mode = PIPE_READMODE_BYTE;
	(void)SetNamedPipeHandleState(h, &mode, NULL, NULL);
	struct u_mcp_conn *c = U_TYPED_CALLOC(struct u_mcp_conn);
	c->pipe = h;
	c->owns_handle = true;
	return c;
}

size_t
u_mcp_enumerate_sessions(pid_t *out_pids, size_t cap)
{
	size_t n = 0;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA("\\\\.\\pipe\\displayxr-mcp-*", &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return 0;
	}
	do {
		const char *prefix = "displayxr-mcp-";
		const char *p = strstr(fd.cFileName, prefix);
		if (p == NULL) {
			continue;
		}
		long pid = strtol(p + strlen(prefix), NULL, 10);
		if (pid > 0 && n < cap) {
			out_pids[n++] = (pid_t)pid;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return n;
}

#endif
