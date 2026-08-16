// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service-side verification of declared IPC client classes (#960).
 * @ingroup ipc_server
 */

#include "service_client_class.h"
#include "service_workspace_registry.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_instance.h"
#include "util/u_logging.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(XRT_OS_WINDOWS) || defined(XRT_OS_MACOS)
#include "service_orchestrator.h"
#define HAVE_ORCHESTRATOR 1
#endif

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#ifdef XRT_OS_MACOS
#include <mach-o/dyld.h>
#endif
#ifdef XRT_OS_LINUX
#include <unistd.h>
#endif


/*
 *
 * Path helpers. Windows paths compare case-insensitively with either separator;
 * POSIX compares bytes.
 *
 */

static char
norm_ch(char c)
{
#ifdef XRT_OS_WINDOWS
	if (c == '/') {
		return '\\';
	}
	if (c >= 'A' && c <= 'Z') {
		return (char)(c - 'A' + 'a');
	}
#endif
	return c;
}

static bool
path_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL || a[0] == 0 || b[0] == 0) {
		return false;
	}
	while (*a != 0 && *b != 0) {
		if (norm_ch(*a) != norm_ch(*b)) {
			return false;
		}
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

//! Directory part of a path (up to and excluding the last separator), normalised.
static void
path_dir(const char *path, char *out, size_t out_len)
{
	out[0] = 0;
	if (path == NULL || out_len == 0) {
		return;
	}
	size_t n = strlen(path);
	size_t cut = 0;
	for (size_t i = 0; i < n; i++) {
		if (path[i] == '\\' || path[i] == '/') {
			cut = i;
		}
	}
	if (cut >= out_len) {
		cut = out_len - 1;
	}
	memcpy(out, path, cut);
	out[cut] = 0;
}

//! Absolute path of this service executable.
static bool
self_exe_path(char *out, size_t out_len)
{
	out[0] = 0;
#if defined(XRT_OS_WINDOWS)
	wchar_t w[MAX_PATH * 2];
	DWORD n = GetModuleFileNameW(NULL, w, (DWORD)(sizeof(w) / sizeof(w[0])));
	if (n == 0) {
		return false;
	}
	int m = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out, (int)out_len - 1, NULL, NULL);
	if (m <= 0) {
		out[0] = 0;
		return false;
	}
	out[m] = 0;
	return true;
#elif defined(XRT_OS_MACOS)
	uint32_t sz = (uint32_t)out_len;
	return _NSGetExecutablePath(out, &sz) == 0;
#elif defined(XRT_OS_LINUX)
	ssize_t n = readlink("/proc/self/exe", out, out_len - 1);
	if (n <= 0) {
		return false;
	}
	out[n] = 0;
	return true;
#else
	return false;
#endif
}


/*
 *
 * Verification.
 *
 */

static bool
verify_controller(const char *peer_exe_path)
{
	if (peer_exe_path == NULL || peer_exe_path[0] == 0) {
		return false;
	}

#ifdef HAVE_ORCHESTRATOR
	// The orchestrator's selected entry — includes the `workspace_binary`
	// dev override, which never appears in the registry.
	const struct workspace_controller_entry *sel = service_orchestrator_get_workspace_entry();
	if (sel != NULL && path_equal(sel->binary, peer_exe_path)) {
		return true;
	}
#endif

	// The entry table is large (16 x ~6 KB); this runs on a client IPC thread,
	// so keep it off the stack.
	struct workspace_controller_entry *entries =
	    calloc(WORKSPACE_REGISTRY_MAX_ENTRIES, sizeof(struct workspace_controller_entry));
	if (entries == NULL) {
		return false;
	}
	int n = service_workspace_registry_enumerate(entries, WORKSPACE_REGISTRY_MAX_ENTRIES);
	bool found = false;
	for (int i = 0; i < n && !found; i++) {
		found = path_equal(entries[i].binary, peer_exe_path);
	}
	free(entries);
	return found;
}

static bool
verify_diag(const char *peer_exe_path)
{
	if (peer_exe_path == NULL || peer_exe_path[0] == 0) {
		return false;
	}
	char self[1024];
	if (!self_exe_path(self, sizeof(self))) {
		return false;
	}
	char self_dir[1024];
	char peer_dir[1024];
	path_dir(self, self_dir, sizeof(self_dir));
	path_dir(peer_exe_path, peer_dir, sizeof(peer_dir));
	return path_equal(self_dir, peer_dir);
}

bool
service_client_class_verify(long peer_pid, const char *peer_exe_path, uint32_t declared_class)
{
	(void)peer_pid;
	switch (declared_class) {
	case XRT_CLIENT_CLASS_CONTROLLER: return verify_controller(peer_exe_path);
	case XRT_CLIENT_CLASS_DIAG: return verify_diag(peer_exe_path);
	case XRT_CLIENT_CLASS_PROVIDER_HOST:
		// Phase 4 (#968): the service will spawn the host and know its pid.
		return false;
	default: return false;
	}
}
