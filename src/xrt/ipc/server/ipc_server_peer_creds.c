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
#include <stdlib.h>

#ifdef XRT_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

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

long
ipc_server_peer_parent_pid(long pid)
{
	if (pid <= 0) {
		return 0;
	}
	// Toolhelp needs no extra import library and no process handle on the
	// peer (works for a Low-integrity child we could not open).
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return 0;
	}
	PROCESSENTRY32W pe;
	pe.dwSize = sizeof(pe);
	long ppid = 0;
	if (Process32FirstW(snap, &pe)) {
		do {
			if ((long)pe.th32ProcessID == pid) {
				ppid = (long)pe.th32ParentProcessID;
				break;
			}
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return ppid;
}

/*!
 * Read a process's integrity-level RID (SECURITY_MANDATORY_*_RID).
 *
 * Only PROCESS_QUERY_LIMITED_INFORMATION is needed, which a Medium-integrity
 * service holds against both a Medium sibling and a Low child.
 */
static bool
ipc_server_process_integrity_rid(long pid, DWORD *out_rid)
{
	bool ok = false;
	HANDLE proc = NULL;
	HANDLE token = NULL;
	TOKEN_MANDATORY_LABEL *label = NULL;
	DWORD size = 0;

	if (pid <= 0) {
		return false;
	}

	proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
	if (proc == NULL) {
		goto out;
	}
	if (!OpenProcessToken(proc, TOKEN_QUERY, &token)) {
		goto out;
	}

	// First call reports the size; ERROR_INSUFFICIENT_BUFFER is the success path.
	if (GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &size) || size == 0) {
		goto out;
	}
	label = (TOKEN_MANDATORY_LABEL *)malloc(size);
	if (label == NULL) {
		goto out;
	}
	if (!GetTokenInformation(token, TokenIntegrityLevel, label, size, &size)) {
		goto out;
	}

	{
		UCHAR *count = GetSidSubAuthorityCount(label->Label.Sid);
		if (count == NULL || *count == 0) {
			goto out;
		}
		*out_rid = *GetSidSubAuthority(label->Label.Sid, (DWORD)(*count - 1));
		ok = true;
	}

out:
	free(label);
	if (token != NULL) {
		CloseHandle(token);
	}
	if (proc != NULL) {
		CloseHandle(proc);
	}
	return ok;
}

bool
ipc_server_peer_declaration_allowed(long opener_pid, long declared_pid, const char **out_why)
{
	const char *why = NULL;

	if (declared_pid <= 0) {
		why = "declared pid is not a process id";
		goto refuse;
	}
	if (opener_pid <= 0) {
		// Fail closed: with no authoriser there is nobody to authorise.
		why = "the opener's identity could not be derived";
		goto refuse;
	}
	if (declared_pid == opener_pid) {
		// The adopter turned out to BE the opener. Nothing is delegated, so
		// there is nothing to authorise; accepting keeps the client's "always
		// declare self" rule uniform.
		return true;
	}

	DWORD opener_rid = 0;
	DWORD declared_rid = 0;
	if (!ipc_server_process_integrity_rid(opener_pid, &opener_rid)) {
		why = "the opener's integrity level could not be read";
		goto refuse;
	}
	if (!ipc_server_process_integrity_rid(declared_pid, &declared_rid)) {
		why = "the declared target's integrity level could not be read";
		goto refuse;
	}

	// THE RULE. Delegation may only ever go downward.
	if (opener_rid < declared_rid) {
		why = "the opener may not delegate UPWARD (integrity escalation)";
		goto refuse;
	}

	return true;

refuse:
	if (out_why != NULL) {
		*out_why = why;
	}
	return false;
}

#else /* POSIX */

#include <sys/types.h>
#include <sys/socket.h>

#include <sys/un.h>

long
ipc_server_derive_peer_uid(xrt_ipc_handle_t handle)
{
#if defined(XRT_OS_LINUX)
	struct ucred cred;
	socklen_t len = sizeof(cred);
	if (getsockopt((int)handle, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
		return (long)cred.uid;
	}
	return -1;
#else
	(void)handle;
	return -1;
#endif
}

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

long
ipc_server_peer_parent_pid(long pid)
{
	if (pid <= 0) {
		return 0;
	}
#if defined(XRT_OS_LINUX)
	// /proc/<pid>/stat: "pid (comm) state ppid ..." — comm may hold spaces
	// and parens, so parse from the LAST ')'.
	char path[64];
	snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	char line[512];
	const char *got = fgets(line, sizeof(line), f);
	fclose(f);
	if (got == NULL) {
		return 0;
	}
	const char *rp = strrchr(line, ')');
	if (rp == NULL) {
		return 0;
	}
	char state = 0;
	long ppid = 0;
	if (sscanf(rp + 1, " %c %ld", &state, &ppid) != 2) {
		return 0;
	}
	return ppid > 0 ? ppid : 0;
#elif defined(XRT_OS_MACOS)
	struct proc_bsdinfo bi;
	int n = proc_pidinfo((int)pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi));
	if (n != (int)sizeof(bi)) {
		return 0;
	}
	return (long)bi.pbi_ppid;
#else
	return 0;
#endif
}

bool
ipc_server_peer_declaration_allowed(long opener_pid, long declared_pid, const char **out_why)
{
	// POSIX has no handle duplication into a target process and no integrity
	// level, so a declaration naming anyone but the opener buys nothing and is
	// refused: the OS-derived creds (SO_PEERCRED / LOCAL_PEERPID) stay
	// authoritative. Self-declaration is a no-op and is accepted so the client
	// side needs no platform branch.
	if (declared_pid > 0 && declared_pid == opener_pid) {
		return true;
	}
	if (out_why != NULL) {
		*out_why =
		    "POSIX attributes a connection to the transport's creator; only self-declaration is honoured";
	}
	return false;
}

#endif
