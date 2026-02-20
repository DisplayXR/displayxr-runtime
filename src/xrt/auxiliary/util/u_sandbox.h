// Copyright 2025, Leia Inc.
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


#ifdef __cplusplus
}
#endif
