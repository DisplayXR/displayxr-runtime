// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Child-process helpers shared by the service's spawners (see service_spawn.h).
 * @ingroup ipc
 */

#include "service_spawn.h"

#include "util/u_logging.h"

#include <stdio.h>
#include <string.h>

#if defined(XRT_OS_WINDOWS)

bool
service_spawn_sibling_path(const char *exe_name, char *buf, size_t buf_size)
{
	char self_path[MAX_PATH];
	const DWORD len = GetModuleFileNameA(NULL, self_path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH || exe_name == NULL || buf == NULL || buf_size == 0) {
		return false;
	}
	char *last_sep = strrchr(self_path, '\\');
	char *fwd = strrchr(self_path, '/');
	if (fwd != NULL && (last_sep == NULL || fwd > last_sep)) {
		last_sep = fwd;
	}
	if (last_sep == NULL) {
		return false;
	}
	last_sep[1] = '\0';
	const int n = snprintf(buf, buf_size, "%s%s", self_path, exe_name);
	return n > 0 && (size_t)n < buf_size;
}

bool
service_spawn_file_exists(const char *path)
{
	const DWORD attrs = GetFileAttributesA(path);
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool
service_spawn_child(const char *exe_path, const char *args, const char *log_path, uint32_t flags, PROCESS_INFORMATION *pi)
{
	char cmd[1024];
	if (args != NULL && args[0] != '\0') {
		snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe_path, args);
	} else {
		snprintf(cmd, sizeof(cmd), "\"%s\"", exe_path);
	}

	STARTUPINFOA si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(pi, sizeof(*pi));

	HANDLE log_h = INVALID_HANDLE_VALUE;
	BOOL inherit = FALSE;
	if (log_path != NULL && log_path[0] != '\0') {
		SECURITY_ATTRIBUTES sa;
		ZeroMemory(&sa, sizeof(sa));
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		log_h = CreateFileA(log_path, FILE_APPEND_DATA | SYNCHRONIZE,
		                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
		                    FILE_ATTRIBUTE_NORMAL, NULL);
		if (log_h != INVALID_HANDLE_VALUE) {
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			si.hStdOutput = log_h;
			si.hStdError = log_h;
			inherit = TRUE;
		} else {
			U_LOG_W("[spawn] failed to open log file %s (error %lu)", log_path,
			        (unsigned long)GetLastError());
		}
	}

	DWORD create = CREATE_NO_WINDOW;
	if ((flags & SERVICE_SPAWN_SUSPENDED) != 0) {
		create |= CREATE_SUSPENDED;
	}
	const BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, inherit, create, NULL, NULL, &si, pi);
	if (log_h != INVALID_HANDLE_VALUE) {
		CloseHandle(log_h); // our copy; the child keeps its own
	}
	if (!ok) {
		U_LOG_W("[spawn] failed to launch: %s (error %lu)", cmd, (unsigned long)GetLastError());
		return false;
	}
	if ((flags & SERVICE_SPAWN_SUSPENDED) == 0) {
		// Only the process handle is needed.
		CloseHandle(pi->hThread);
		pi->hThread = NULL;
	}
	return true;
}

bool
service_spawn_appdata_log_path(const char *basename, char *buf, size_t buf_size)
{
	char appdata[MAX_PATH];
	const DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", appdata, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		return false;
	}
	const int n = snprintf(buf, buf_size, "%s\\DisplayXR\\%s.log", appdata, basename);
	return n > 0 && (size_t)n < buf_size;
}

void
service_spawn_terminate(PROCESS_INFORMATION *pi, uint32_t exit_code, uint32_t wait_ms)
{
	if (pi->hProcess != NULL) {
		TerminateProcess(pi->hProcess, exit_code);
		WaitForSingleObject(pi->hProcess, wait_ms);
		CloseHandle(pi->hProcess);
		pi->hProcess = NULL;
	}
	if (pi->hThread != NULL) {
		CloseHandle(pi->hThread);
		pi->hThread = NULL;
	}
}

#else // POSIX

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef XRT_OS_APPLE
#include <mach-o/dyld.h>
#endif

bool
service_spawn_sibling_path(const char *exe_name, char *buf, size_t buf_size)
{
	if (exe_name == NULL || buf == NULL || buf_size == 0) {
		return false;
	}
	char self_path[PATH_MAX];
#ifdef XRT_OS_APPLE
	uint32_t sz = (uint32_t)sizeof(self_path);
	if (_NSGetExecutablePath(self_path, &sz) != 0) {
		return false;
	}
#else
	const ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
	if (len <= 0) {
		return false;
	}
	self_path[len] = '\0';
#endif
	char *last_sep = strrchr(self_path, '/');
	if (last_sep == NULL) {
		return false;
	}
	last_sep[1] = '\0';
	const int n = snprintf(buf, buf_size, "%s%s", self_path, exe_name);
	return n > 0 && (size_t)n < buf_size;
}

bool
service_spawn_file_exists(const char *path)
{
	struct stat sb;
	return path != NULL && stat(path, &sb) == 0 && S_ISREG(sb.st_mode);
}

#endif
