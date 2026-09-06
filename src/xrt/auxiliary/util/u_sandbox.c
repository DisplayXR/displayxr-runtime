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

#if defined(XRT_OS_LINUX) || defined(XRT_OS_ANDROID)
#include <stdio.h>
#endif

/*
 *
 * Routing-policy helpers (platform independent, so they can be unit tested)
 *
 */

//! Is @p c a separator in a routing-property value?
static bool
route_is_sep(char c)
{
	// NOT ':' — a satellite slot process is "<pkg>:dxrN" and that colon is
	// part of a name, not a separator.
	return c == ',' || c == ' ' || c == '\t' || c == ';';
}

bool
u_sandbox_route_prop_selects(const char *value, const char *process_name, bool allow_device_wide)
{
	if (value == NULL || value[0] == '\0') {
		return false;
	}

	// Whole-device booleans first — this is what the property has always
	// meant and what every existing script sets. Refused outright on the
	// shipping (read-only-property) tier: ADR-036's flavor merge exists
	// because a device-wide deployment decision pushed apps that wanted
	// in-process out of process, and a `ro.` property is exactly the shape
	// that could bring that back. Devices get an allow-list, not a switch.
	if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "t") == 0 ||
	    strcmp(value, "y") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "on") == 0 ||
	    strcmp(value, "all") == 0 || strcmp(value, "*") == 0) {
		return allow_device_wide;
	}
	if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "f") == 0 ||
	    strcmp(value, "n") == 0 || strcmp(value, "no") == 0 || strcmp(value, "off") == 0) {
		return false;
	}

	// Anything else is an allow-list of process/package names. Without a
	// process name to compare against we cannot honour it — and must NOT
	// fall back to "true", or a targeted list would silently become a
	// device-wide force.
	if (process_name == NULL || process_name[0] == '\0') {
		return false;
	}

	// A satellite/slot process is "<pkg>:dxrN"; the package is what an
	// operator types, so match on the base as well as the full name.
	size_t base_len = 0;
	while (process_name[base_len] != '\0' && process_name[base_len] != ':') {
		base_len++;
	}
	const size_t full_len = strlen(process_name);

	const char *p = value;
	while (*p != '\0') {
		while (*p != '\0' && route_is_sep(*p)) {
			p++;
		}
		const char *tok = p;
		while (*p != '\0' && !route_is_sep(*p)) {
			p++;
		}
		size_t len = (size_t)(p - tok);
		if (len == 0) {
			continue;
		}
		if (tok[len - 1] == '*') {
			// Prefix form: "com.displayxr.*".
			len--;
			if (len == 0) {
				// A bare "*" token is a device-wide switch wearing
				// a list's clothes; same rule as above.
				if (allow_device_wide) {
					return true;
				}
				continue;
			}
			if (full_len >= len && strncmp(process_name, tok, len) == 0) {
				return true;
			}
			continue;
		}
		if (len == full_len && strncmp(process_name, tok, len) == 0) {
			return true;
		}
		if (len == base_len && strncmp(process_name, tok, len) == 0) {
			return true;
		}
	}
	return false;
}

bool
u_sandbox_process_name(char *out_name, size_t out_size)
{
	if (out_name == NULL || out_size == 0) {
		return false;
	}
	out_name[0] = '\0';

#if defined(XRT_OS_LINUX) || defined(XRT_OS_ANDROID)
	// On Android the main process' cmdline IS the package name, and a
	// satellite slot's is "<pkg>:dxrN" — the same string an operator sees in
	// `adb shell ps`. No JNI, no Context, so this is reachable from
	// aux_util at any point in instance creation.
	FILE *f = fopen("/proc/self/cmdline", "r");
	if (f == NULL) {
		return false;
	}
	size_t n = fread(out_name, 1, out_size - 1, f);
	fclose(f);
	if (n == 0) {
		out_name[0] = '\0';
		return false;
	}
	out_name[n] = '\0'; // cmdline is NUL separated; the first field is what we want
	return out_name[0] != '\0';
#else
	(void)out_size;
	return false;
#endif
}

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
	if (force_mode == NULL) {
		// #1277 P2: `debug.dxr.force_ipc` grew a per-package grammar. `1`
		// still means "every app on this device" (what it has always
		// meant); a value containing a package name is an ALLOW-LIST, so
		// one demo can be routed to the service without dragging every
		// other app on the panel with it. See
		// u_sandbox_route_prop_selects() for the grammar.
		char pkg[128] = {0};
		char dev_buf[PROP_VALUE_MAX] = {0};
		const bool have_pkg = u_sandbox_process_name(pkg, sizeof(pkg));
		if (__system_property_get("debug.dxr.force_ipc", sysprop_buf) > 0 &&
		    u_sandbox_route_prop_selects(sysprop_buf, have_pkg ? pkg : NULL, /* allow_device_wide */ true)) {
			U_LOG_I("debug.dxr.force_ipc=%s selects '%s': forcing IPC/service mode", sysprop_buf,
			        have_pkg ? pkg : "(unknown process)");
			return true;
		}
		// Device-class policy tier (#1277 P2 candidate (iii)). A read-only
		// property can only be set by the OEM's build, survives reboot and
		// cannot be flipped by adb on a locked device — which is exactly
		// what "on THIS large-format panel, these apps run out of process"
		// needs to be. Same grammar; the debug property above wins.
		if (__system_property_get("ro.dxr.force_ipc", dev_buf) > 0 &&
		    u_sandbox_route_prop_selects(dev_buf, have_pkg ? pkg : NULL, /* allow_device_wide */ false)) {
			U_LOG_I("ro.dxr.force_ipc=%s selects '%s': forcing IPC/service mode (device policy)", dev_buf,
			        have_pkg ? pkg : "(unknown process)");
			return true;
		}
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
