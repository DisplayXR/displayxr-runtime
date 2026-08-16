// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Server-derived peer identity for IPC clients (#954).
 * @ingroup ipc_server
 */

// struct ucred (SO_PEERCRED) needs _GNU_SOURCE on glibc; harmless elsewhere.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "ipc_server_peer_creds.h"

#include "xrt/xrt_config_os.h"
#include "util/u_logging.h"

#include <stddef.h>

#ifdef XRT_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

long
ipc_server_derive_peer_pid(xrt_ipc_handle_t handle, uint64_t *out_create_ns)
{
	if (out_create_ns != NULL) {
		*out_create_ns = 0;
	}

	ULONG pid = 0;
	if (!GetNamedPipeClientProcessId(handle, &pid) || pid == 0) {
		U_LOG_W("peer-creds: GetNamedPipeClientProcessId failed (err=%lu) — peer identity unknown (#954).",
		        GetLastError());
		return 0;
	}

	// Best-effort creation time for PID-reuse defence. Fails for a peer we may
	// not open (e.g. the low-integrity browser GPU process) — that is fine, the
	// PID itself is already authoritative for the current connection.
	if (out_create_ns != NULL) {
		HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (proc != NULL) {
			FILETIME create, exit, kernel, user;
			if (GetProcessTimes(proc, &create, &exit, &kernel, &user)) {
				// FILETIME is 100ns ticks since 1601-01-01; convert to ns since
				// the Unix epoch (11644473600 s between the two epochs).
				ULARGE_INTEGER t;
				t.LowPart = create.dwLowDateTime;
				t.HighPart = create.dwHighDateTime;
				uint64_t unix_100ns = t.QuadPart - 116444736000000000ULL;
				*out_create_ns = unix_100ns * 100ULL;
			}
			CloseHandle(proc);
		}
	}

	return (long)pid;
}

bool
ipc_server_peer_exe_path(long pid, char *out_path, size_t out_len)
{
	if (out_path == NULL || out_len == 0) {
		return false;
	}
	out_path[0] = 0;
	if (pid <= 0) {
		return false;
	}
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
	if (h == NULL) {
		return false;
	}
	wchar_t wpath[MAX_PATH * 2];
	DWORD wlen = (DWORD)(sizeof(wpath) / sizeof(wpath[0]));
	BOOL ok = QueryFullProcessImageNameW(h, 0, wpath, &wlen);
	CloseHandle(h);
	if (!ok || wlen == 0) {
		return false;
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)wlen, out_path, (int)out_len - 1, NULL, NULL);
	if (n <= 0) {
		out_path[0] = 0;
		return false;
	}
	out_path[n] = 0;
	return true;
}

#else /* POSIX */

#include <sys/types.h>
#include <sys/socket.h>

#include <sys/un.h>

long
ipc_server_derive_peer_pid(xrt_ipc_handle_t handle, uint64_t *out_create_ns)
{
	if (out_create_ns != NULL) {
		*out_create_ns = 0;
	}

#if defined(XRT_OS_LINUX)
	// SO_PEERCRED works on both desktop Linux (AF_UNIX) and Android (the
	// binder-passed connection is a socketpair, so the app end has creds).
	struct ucred cred;
	socklen_t len = sizeof(cred);
	if (getsockopt((int)handle, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid > 0) {
		return (long)cred.pid;
	}
	U_LOG_W("peer-creds: SO_PEERCRED failed — peer identity unknown (#954).");
	return 0;
#elif defined(XRT_OS_MACOS)
	pid_t pid = 0;
	socklen_t len = sizeof(pid);
	if (getsockopt((int)handle, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0 && pid > 0) {
		return (long)pid;
	}
	U_LOG_W("peer-creds: LOCAL_PEERPID failed — peer identity unknown (#954).");
	return 0;
#else
	(void)handle;
	return 0;
#endif
}


#if defined(XRT_OS_MACOS)
#include <libproc.h>
#endif
#include <unistd.h>
#include <stdio.h>

bool
ipc_server_peer_exe_path(long pid, char *out_path, size_t out_len)
{
	if (out_path == NULL || out_len == 0) {
		return false;
	}
	out_path[0] = 0;
	if (pid <= 0) {
		return false;
	}
#if defined(XRT_OS_LINUX)
	char link[64];
	snprintf(link, sizeof(link), "/proc/%ld/exe", pid);
	ssize_t n = readlink(link, out_path, out_len - 1);
	if (n <= 0) {
		out_path[0] = 0;
		return false;
	}
	out_path[n] = 0;
	return true;
#elif defined(XRT_OS_MACOS)
	char buf[PROC_PIDPATHINFO_MAXSIZE];
	int n = proc_pidpath((int)pid, buf, sizeof(buf));
	if (n <= 0) {
		return false;
	}
	snprintf(out_path, out_len, "%s", buf);
	return true;
#else
	return false;
#endif
}

#endif
