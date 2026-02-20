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
	// Check for environment variable override first
	const char *force_mode = getenv("XRT_FORCE_MODE");
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

	// Automatic detection
	bool is_sandboxed = u_sandbox_is_app_container();
	if (is_sandboxed) {
		U_LOG_I("Sandbox detected, using IPC/service mode");
	}

	return is_sandboxed;
}
