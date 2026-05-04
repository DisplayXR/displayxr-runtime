// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sandbox/AppContainer detection utilities implementation.
 * @author David Fattal
 * @ingroup aux_util
 */

#include "u_sandbox.h"
#include "u_logging.h"

#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/*
 *
 * Windows implementation
 *
 */

#ifdef XRT_OS_WINDOWS

bool
u_sandbox_is_app_container(void)
{
	HANDLE token = NULL;
	BOOL is_app_container = FALSE;
	DWORD return_length = 0;

	// Open the current process token
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		U_LOG_W("Failed to open process token for AppContainer check (error %lu)", GetLastError());
		return false;
	}

	// Query whether the token is an AppContainer token
	if (!GetTokenInformation(token, TokenIsAppContainer, &is_app_container, sizeof(is_app_container),
	                         &return_length)) {
		U_LOG_W("Failed to query TokenIsAppContainer (error %lu)", GetLastError());
		CloseHandle(token);
		return false;
	}

	CloseHandle(token);

	return is_app_container != FALSE;
}

#elif defined(XRT_OS_MACOS)

/*
 *
 * macOS implementation
 *
 */

#include <sandbox.h>
#include <unistd.h>

bool
u_sandbox_is_app_container(void)
{
	int result = sandbox_check(getpid(), NULL, SANDBOX_FILTER_NONE);
	return result != 0;
}

#else /* stub for other platforms */

/*
 *
 * Stub implementation
 *
 */

bool
u_sandbox_is_app_container(void)
{
	return false;
}

#endif


/*
 *
 * Platform-independent functions
 *
 */

bool
u_sandbox_should_use_ipc(void)
{
	// Check for environment variable override first.
	// On Windows, also check the process env block via GetEnvironmentVariableA
	// because the host EXE (e.g. webxr bridge) may have set the var via
	// SetEnvironmentVariableA AFTER CRT init. The CRT's getenv() misses this
	// when host and DLL have separate static CRTs (/MT).
	const char *force_mode = getenv("XRT_FORCE_MODE");
#ifdef XRT_OS_WINDOWS
	char force_mode_buf[64] = {0};
	if (force_mode == NULL) {
		DWORD n = GetEnvironmentVariableA("XRT_FORCE_MODE", force_mode_buf, sizeof(force_mode_buf));
		if (n > 0) force_mode = force_mode_buf;
	}
#endif
	if (force_mode != NULL) {
		if (strcmp(force_mode, "native") == 0) {
			U_LOG_I("XRT_FORCE_MODE=native: forcing in-process native compositor");
			return false;
		}
		if (strcmp(force_mode, "ipc") == 0) {
			U_LOG_I("XRT_FORCE_MODE=ipc: forcing IPC/service mode");
			return true;
		}
		// Unknown value, fall through to automatic detection
		U_LOG_W("Unknown XRT_FORCE_MODE value '%s', using automatic detection", force_mode);
	}

	// Workspace session: app launched by workspace controller with hidden HWND, route to IPC
	const char *workspace_session = getenv("DISPLAYXR_WORKSPACE_SESSION");
#ifdef XRT_OS_WINDOWS
	char workspace_session_buf[16] = {0};
	if (workspace_session == NULL) {
		DWORD n = GetEnvironmentVariableA("DISPLAYXR_WORKSPACE_SESSION", workspace_session_buf,
		                                   sizeof(workspace_session_buf));
		if (n > 0) workspace_session = workspace_session_buf;
	}
#endif
	if (workspace_session != NULL && strcmp(workspace_session, "1") == 0) {
		U_LOG_I("DISPLAYXR_WORKSPACE_SESSION=1: forcing IPC mode for workspace controller");
		return true;
	}

	// Automatic detection
	bool is_sandboxed = u_sandbox_is_app_container();
	if (is_sandboxed) {
		U_LOG_I("Sandbox detected, using IPC/service mode");
	}

	return is_sandboxed;
}
