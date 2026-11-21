// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keep an app's native_app_glue command queue moving while the runtime
 *         blocks the glue's own thread (#2087, #1358 bug 1).
 * @ingroup aux_android
 */

#pragma once

#include <xrt/xrt_config_os.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef XRT_OS_ANDROID

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * What one pump pass saw.
 */
struct android_glue_pump_result
{
	//! Glue commands processed on this pass.
	uint32_t processed;

	//! Commands kept from the app's handler (see android_glue_pump_once()).
	uint32_t withheld;

	//! The activity's window or the activity itself is going away: the caller
	//! should stop waiting and fail, without pumping again.
	bool teardown;
};

/*!
 * Is the pump enabled? `debug.dxr.glue_pump` = 0 turns it off (read once).
 */
bool
android_glue_pump_enabled(void);

/*!
 * Process the `native_app_glue` commands that are pending on the CALLING
 * thread's ALooper, without blocking.
 *
 * Why: the glue's UI-thread side blocks inside `onResume` / `onPause` /
 * `onNativeWindowDestroyed` … until the app's `android_main` thread has read
 * the matching command. When the runtime blocks that thread (the hosted
 * surface wait in xrCreateSession), the UI thread can never run the posted
 * `WindowManager.addView` the runtime is waiting for: both wait on each other
 * until the timeout. Reading the commands here lets the UI thread go.
 *
 * What the app sees: lifecycle commands reach its `onAppCmd` handler at the
 * usual point (between the glue's pre and post step), nested inside the
 * runtime call. Three are NOT forwarded, because handlers commonly create or
 * destroy OpenXR objects on them and the app is in the middle of an OpenXR
 * call: INIT_WINDOW (withheld, the wait continues), and the commands numbered
 * 15 and 16 (STOP or DESTROY depending on the glue flavour; withheld, and
 * @ref android_glue_pump_result::teardown is set — the glue still records
 * `destroyRequested`, which is what an app's main loop tests). TERM_WINDOW is
 * forwarded and sets teardown.
 *
 * Safe to call from any thread: a thread with no ALooper, or whose looper
 * carries no glue command source, gets an all-zero result.
 */
struct android_glue_pump_result
android_glue_pump_once(void);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_ANDROID
