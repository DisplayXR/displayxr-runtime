// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sandbox/AppContainer detection utilities implementation.
 * @author David Fattal
 * @ingroup aux_util
 */

// Every platform branch below keys off XRT_OS_*, and NOTHING else this file
// includes pulls in the generated config header — not u_sandbox.h (stdbool
// only), not u_logging.h → xrt_api/xrt_compiler/xrt_results → xrt_defines.
// Without this include all three branches compiled out and the file silently
// reduced to the "other platforms" stub: u_sandbox_is_app_container() always
// returned false (so Windows AppContainer auto-detection never fired) and the
// Windows GetEnvironmentVariableA fallback for XRT_FORCE_MODE /
// DISPLAYXR_WORKSPACE_SESSION never ran. Found while wiring the Android
// hybrid-mode sysprop override (#1031), which was dead for the same reason.
#include "xrt/xrt_config_os.h"

#include "u_sandbox.h"
#include "u_logging.h"

#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef XRT_OS_ANDROID
#include <sys/system_properties.h>
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

bool
u_sandbox_is_app_container(void)
{
	// App Sandbox exports this into the process environment for, and only for,
	// a sandboxed app; it is the public, documented signal.
	//
	// This branch had never actually been compiled (see the xrt_config_os.h
	// note at the top), and what it contained —
	// `sandbox_check(getpid(), NULL, SANDBOX_FILTER_NONE)` — could not have
	// worked as written: `sandbox_check` is an undeclared SPI, absent from the
	// public <sandbox.h>, so enabling the branch is what surfaced it. It is
	// also the wrong test for us even if declared: it reports *any* sandbox
	// policy, which on a modern macOS is far broader than "this app is
	// containerised", and a false positive here silently pushes an ordinary
	// in-process macOS app onto the IPC path.
	return getenv("APP_SANDBOX_CONTAINER_ID") != NULL;
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
u_sandbox_is_workspace_session(void)
{
	// Same dual read as XRT_FORCE_MODE below: the launcher may have set this
	// with SetEnvironmentVariableA after our CRT snapshotted the environment.
	const char *workspace_session = getenv("DISPLAYXR_WORKSPACE_SESSION");
#ifdef XRT_OS_WINDOWS
	char workspace_session_buf[16] = {0};
	if (workspace_session == NULL) {
		DWORD n = GetEnvironmentVariableA("DISPLAYXR_WORKSPACE_SESSION", workspace_session_buf,
		                                  sizeof(workspace_session_buf));
		if (n > 0)
			workspace_session = workspace_session_buf;
	}
#endif
	return workspace_session != NULL && strcmp(workspace_session, "1") == 0;
}

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
#ifdef XRT_OS_ANDROID
	// An Android app is launched by the system, not by a parent process that
	// could export an env var, so `XRT_FORCE_MODE` alone is unusable there for
	// anything but a self-setenv() before xrCreateInstance. Accept the same
	// override as a system property as well, in both the u_debug spelling
	// (debug.xrt.<NAME>, what DEBUG_GET_ONCE_* reads) and a short convenience
	// form, so `adb shell setprop` can flip a single app onto the IPC path
	// with no rebuild. Env still wins — it is the more specific signal.
	// Per-app targeting is the manifest meta-data instead (#1031); see
	// android_globals_self_declares_force_ipc().
	char sysprop_buf[PROP_VALUE_MAX] = {0};
	if (force_mode == NULL && __system_property_get("debug.xrt.XRT_FORCE_MODE", sysprop_buf) > 0 &&
	    sysprop_buf[0] != '\0') {
		force_mode = sysprop_buf;
	}
	if (force_mode == NULL && __system_property_get("debug.dxr.force_ipc", sysprop_buf) > 0 &&
	    (sysprop_buf[0] == '1' || sysprop_buf[0] == 't')) {
		U_LOG_I("debug.dxr.force_ipc=%s: forcing IPC/service mode", sysprop_buf);
		return true;
	}
#endif

	// An already-connected service socket handed in by an embedder (#1056:
	// Chromium's GPU process, which has no Context and cannot bindService)
	// only makes sense on the IPC path — adopting it is the connection. The
	// process-global variant set through ipc_client_connection_adopt_fd() is
	// checked by the caller, which can see the IPC layer; this is the env
	// half, which aux_util can read without a layering violation.
	const char *ipc_fd = getenv("DXR_IPC_FD");
	if (ipc_fd != NULL && ipc_fd[0] != '\0') {
		U_LOG_I("DXR_IPC_FD=%s: adopting a connected service socket, forcing IPC mode", ipc_fd);
		return true;
	}

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
	if (u_sandbox_is_workspace_session()) {
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
