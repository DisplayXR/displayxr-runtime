// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sandbox/AppContainer detection utilities.
 * @author David Fattal
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * @defgroup aux_sandbox Sandbox Detection
 * @ingroup aux_util
 *
 * Utilities for detecting sandboxed execution environments such as
 * Windows AppContainer (used by WebXR/Chrome, UWP apps, etc.) and
 * macOS App Sandbox (used by Safari WebXR, Mac App Store apps, Chrome Seatbelt).
 */

/*!
 * Check if the current process is running in a platform sandbox.
 *
 * On Windows, detects AppContainer sandbox used by:
 * - Chrome/Edge for WebXR content
 * - Microsoft Store (UWP) applications
 * - Other sandboxed Windows applications
 *
 * On macOS, detects App Sandbox used by:
 * - Safari for WebXR content
 * - Chrome Seatbelt sandbox
 * - Mac App Store applications
 *
 * @return true if running in a sandbox, false otherwise.
 *
 * @note On unsupported platforms, this always returns false.
 *
 * @ingroup aux_sandbox
 */
bool
u_sandbox_is_app_container(void);

/*!
 * Check if the current process should use IPC mode.
 *
 * This considers:
 * - AppContainer sandbox detection
 * - XRT_FORCE_MODE environment variable override
 *
 * The XRT_FORCE_MODE environment variable can be set to:
 * - "native" - Force in-process native compositor
 * - "ipc" - Force IPC/service mode
 * - Unset or any other value - Use automatic detection
 *
 * @return true if IPC mode should be used, false for in-process native mode.
 *
 * @ingroup aux_sandbox
 */
bool
u_sandbox_should_use_ipc(void);

/*!
 * Was this process launched inside a workspace session?
 *
 * True when `DISPLAYXR_WORKSPACE_SESSION=1` is in the environment — set by the
 * workspace controller for the apps it launches, and by the service
 * orchestrator's spawn path. Two consumers:
 * - u_sandbox_should_use_ipc(): a workspace session always runs over IPC.
 * - #964 Phase A: only workspace-session clients are WORKSPACE clients (the
 *   controller enumerates, places and composes them). Everything else keeps
 *   its own window and reaches the panel through the foreground override.
 *
 * On Windows the process env block is consulted as well as the CRT's
 * environment: the host EXE may set the var with SetEnvironmentVariableA after
 * CRT init, which a separately-linked static CRT in this DLL would miss.
 *
 * @ingroup aux_sandbox
 */
bool
u_sandbox_is_workspace_session(void);

/*!
 * Outcome of the desktop-Linux XR_DXR_weave present-owner rule (#1744).
 *
 * @see u_sandbox_linux_present_owner_route
 * @ingroup aux_sandbox
 */
enum u_sandbox_present_owner_route
{
	//! Not a present-owner decision: the generic rules decide (workspace
	//! controller, u_sandbox_should_use_ipc). XRT_FORCE_MODE set, or no weave.
	U_SANDBOX_PRESENT_OWNER_NOT_APPLICABLE = 0,
	//! Weave enabled, but the app binds its own window/surface: not a
	//! present-owner. Falls through to the generic rules, like NOT_APPLICABLE.
	U_SANDBOX_PRESENT_OWNER_WINDOW_BOUND,
	//! A present-owner and the service socket exists: dial the service.
	U_SANDBOX_PRESENT_OWNER_SERVICE,
	//! A present-owner with no service socket: in-process fallback.
	U_SANDBOX_PRESENT_OWNER_NO_SERVICE,
};

/*!
 * The desktop-Linux present-owner routing decision (#1744), as a pure function.
 *
 * XR_DXR_weave's present path lives only in the service compositor, so a
 * present-owner is routed to it by capability. A present-owner is a session
 * that enables XR_DXR_weave AND binds no window of its own: an app that hands
 * the runtime its own window through an XR_DXR_*_window/surface_binding
 * extension (the Linux demos, which enable weave only for the in-process
 * xrWeaveSnapWindowRectDXR / xrWeaveSnapWindowGridDXR) is not one, and routing
 * it to the service turned it into a HOSTED session with no external window.
 *
 * XRT_FORCE_MODE, when set, is authoritative and this rule does not apply.
 *
 * @param env_forced             XRT_FORCE_MODE is set to a non-empty value.
 * @param weave_enabled          the app enabled XR_DXR_weave.
 * @param window_binding_enabled the app enabled a window/surface binding extension.
 * @param service_socket_present the displayxr-service socket file exists.
 *
 * @ingroup aux_sandbox
 */
enum u_sandbox_present_owner_route
u_sandbox_linux_present_owner_route(bool env_forced,
                                    bool weave_enabled,
                                    bool window_binding_enabled,
                                    bool service_socket_present);

/*!
 * Does a routing-property value select @p process_name?
 *
 * The grammar behind `debug.dxr.force_ipc` and `ro.dxr.force_ipc` (#1277 P2).
 * Extracted as a pure function with no platform calls so it can be unit tested
 * on the host — the property read around it cannot be.
 *
 * - `1` / `true` / `t` / `y` / `yes` / `on` / `all` / `*` — every process on
 *   the device. Honoured only when @p allow_device_wide is true.
 * - `0` / `false` / `f` / `n` / `no` / `off` — nothing. (Not "force native":
 *   this property only ever forces one direction; `XRT_FORCE_MODE=native` is
 *   the way back and it is checked earlier.)
 * - anything else — an ALLOW-LIST of process names, separated by any of
 *   `, ; SPACE TAB`. A token matches @p process_name exactly, matches its
 *   package base (the part before `:` — an Android satellite slot process is
 *   `<pkg>:dxrN`), or, with a trailing `*`, matches as a prefix.
 *
 * With no @p process_name to compare against, a list matches nothing. It must
 * never degrade to "true": a targeted list silently becoming a device-wide
 * force is the exact failure ADR-036's flavor merge removed.
 *
 * @param value             the property's value; NULL or empty selects nothing.
 * @param process_name      this process' name, or NULL if it could not be read.
 * @param allow_device_wide may this value name the whole device? True for the
 *                          `debug.` (dev) tier, false for the `ro.` (shipping,
 *                          OEM-set) tier, which gets an allow-list only.
 *
 * @ingroup aux_sandbox
 */
bool
u_sandbox_route_prop_selects(const char *value, const char *process_name, bool allow_device_wide);

/*!
 * This process' name, for @ref u_sandbox_route_prop_selects.
 *
 * On Linux and Android it is the first field of `/proc/self/cmdline` — on
 * Android that is the package name for the main process and `<pkg>:dxrN` for a
 * satellite slot, i.e. what `adb shell ps` shows. No JNI and no Context, so it
 * is reachable from aux_util at any point in instance creation. Other platforms
 * report nothing.
 *
 * @return true when @p out_name was filled with a non-empty name.
 *
 * @ingroup aux_sandbox
 */
bool
u_sandbox_process_name(char *out_name, size_t out_size);


#ifdef __cplusplus
}
#endif
