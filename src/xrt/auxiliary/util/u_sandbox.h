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


#ifdef __cplusplus
}
#endif
