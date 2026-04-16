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

long
u_mcp_self_pid(void)
{
	return (long)getpid();
}

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
build_sock_path(char *out, size_t cap, long pid)
{
	snprintf(out, cap, "%s%ld%s", SOCK_PREFIX, (long)pid, SOCK_SUFFIX);
}

static void
build_sock_path_named(char *out, size_t cap, const char *role)
{
	snprintf(out, cap, "%s%s%s", SOCK_PREFIX, role, SOCK_SUFFIX);
}

static struct u_mcp_listener *
listener_open_path(const char *path)
{
	struct u_mcp_listener *l = U_TYPED_CALLOC(struct u_mcp_listener);
	l->fd = -1;
	snprintf(l->path, sizeof(l->path), "%s", path);

	// Always unlink any stale socket; we own the named path.
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

struct u_mcp_listener *
u_mcp_listener_open(long pid)
{
	char path[128];
	build_sock_path(path, sizeof(path), pid);
	return listener_open_path(path);
}

struct u_mcp_listener *
u_mcp_listener_open_named(const char *role)
{
	if (role == NULL || role[0] == '\0') {
		return NULL;
	}
	char path[128];
	build_sock_path_named(path, sizeof(path), role);
	return listener_open_path(path);
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

static struct u_mcp_conn *
conn_connect_path(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return NULL;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return NULL;
	}
	struct u_mcp_conn *c = U_TYPED_CALLOC(struct u_mcp_conn);
	c->fd = fd;
	return c;
}

struct u_mcp_conn *
u_mcp_conn_connect(long pid)
{
	char path[128];
	build_sock_path(path, sizeof(path), pid);
	return conn_connect_path(path);
}

struct u_mcp_conn *
u_mcp_conn_connect_named(const char *role)
{
	if (role == NULL || role[0] == '\0') {
		return NULL;
	}
	char path[128];
	build_sock_path_named(path, sizeof(path), role);
	return conn_connect_path(path);
}

size_t
u_mcp_enumerate_sessions(long *out_pids, size_t cap)
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
		out_pids[n++] = (long)pid;
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

long
u_mcp_self_pid(void)
{
	return (long)GetCurrentProcessId();
}

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
build_pipe_name(char *out, size_t cap, long pid)
{
	snprintf(out, cap, PIPE_PREFIX "%ld", (long)pid);
}

static void
build_pipe_name_named(char *out, size_t cap, const char *role)
{
	snprintf(out, cap, PIPE_PREFIX "%s", role);
}

static struct u_mcp_listener *
listener_open_pipe_name(const char *name)
{
	struct u_mcp_listener *l = U_TYPED_CALLOC(struct u_mcp_listener);
	snprintf(l->name, sizeof(l->name), "%s", name);
	l->pipe = CreateNamedPipeA(
	    l->name,
	    PIPE_ACCESS_DUPLEX,
	    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
	    1, 65536, 65536, 0, NULL);
	if (l->pipe == INVALID_HANDLE_VALUE) {
		U_LOG_W(LOG_PFX "CreateNamedPipe(%s) failed: %lu", l->name, GetLastError());
		free(l);
		return NULL;
	}
	U_LOG_I(LOG_PFX "listening on %s", l->name);
	return l;
}

struct u_mcp_listener *
u_mcp_listener_open_named(const char *role)
{
	if (role == NULL || role[0] == '\0') {
		return NULL;
	}
	char name[128];
	build_pipe_name_named(name, sizeof(name), role);
	return listener_open_pipe_name(name);
}

struct u_mcp_listener *
u_mcp_listener_open(long pid)
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
		OVERLAPPED ov = {0};
		ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
		DWORD got = 0;
		BOOL ok = ReadFile(conn->pipe, p, (DWORD)len, &got, &ov);
		if (!ok && GetLastError() == ERROR_IO_PENDING) {
			WaitForSingleObject(ov.hEvent, INFINITE);
			ok = GetOverlappedResult(conn->pipe, &ov, &got, FALSE);
		}
		CloseHandle(ov.hEvent);
		if (!ok || got == 0) {
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
		OVERLAPPED ov = {0};
		ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
		DWORD wrote = 0;
		BOOL ok = WriteFile(conn->pipe, p, (DWORD)len, &wrote, &ov);
		if (!ok && GetLastError() == ERROR_IO_PENDING) {
			WaitForSingleObject(ov.hEvent, INFINITE);
			ok = GetOverlappedResult(conn->pipe, &ov, &wrote, FALSE);
		}
		CloseHandle(ov.hEvent);
		if (!ok || wrote == 0) {
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
	if (conn->pipe != INVALID_HANDLE_VALUE) {
		if (conn->owns_handle) {
			CloseHandle(conn->pipe);
		} else {
			// Listener-owned pipe: reset it for the next ConnectNamedPipe.
			// Windows requires DisconnectNamedPipe between clients when
			// nMaxInstances=1, otherwise subsequent CreateFile calls time
			// out with ERROR_SEM_TIMEOUT.
			DisconnectNamedPipe(conn->pipe);
		}
	}
	free(conn);
}

int
u_mcp_conn_fd(struct u_mcp_conn *conn)
{
	(void)conn;
	return -1; // Windows clients cannot poll() a pipe HANDLE; adapter uses threads.
}

static struct u_mcp_conn *
conn_connect_pipe_name(const char *name)
{
	if (!WaitNamedPipeA(name, 5000)) {
		return NULL;
	}
	HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
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

struct u_mcp_conn *
u_mcp_conn_connect(long pid)
{
	char name[128];
	build_pipe_name(name, sizeof(name), pid);
	return conn_connect_pipe_name(name);
}

struct u_mcp_conn *
u_mcp_conn_connect_named(const char *role)
{
	if (role == NULL || role[0] == '\0') {
		return NULL;
	}
	char name[128];
	build_pipe_name_named(name, sizeof(name), role);
	return conn_connect_pipe_name(name);
}

size_t
u_mcp_enumerate_sessions(long *out_pids, size_t cap)
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
			out_pids[n++] = (long)pid;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return n;
}

#endif
