// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Functions for Android-specific global state.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
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

struct _JNIEnv;
struct _JavaVM;
struct _ANativeWindow;

/*!
 * Store the Java VM pointer and the android.app.Activity jobject.
 */
void
android_globals_store_vm_and_activity(struct _JavaVM *vm, void *activity);


/*!
 * Store the Java VM pointer and the android.content.Context jobject.
 */
void
android_globals_store_vm_and_context(struct _JavaVM *vm, void *context);


/*!
 * Is the provided jobject an instance of android.app.Activity?
 */
bool
android_globals_is_instance_of_activity(struct _JavaVM *vm, void *obj);

/*!
 * Retrieve the Java VM pointer previously stored, if any.
 */
struct _JavaVM *
android_globals_get_vm(void);

/*!
 * Retrieve the android.app.Activity jobject previously stored, if any.
 *
 * For usage, cast the return value to jobject - a typedef whose definition
 * differs between C (a void *) and C++ (a pointer to an empty class)
 */
void *
android_globals_get_activity(void);

/*!
 * Retrieve the android.content.Context jobject previously stored, if any.
 *
 * Since android.app.Activity is a sub-class of android.content.Context, the
 * activity jobject will be returned if it has been set but the context has not.
 *
 * For usage, cast the return value to jobject - a typedef whose definition
 * differs between C (a void *) and C++ (a pointer to an empty class)
 */
void *
android_globals_get_context(void);


void
android_globals_store_window(struct _ANativeWindow *window);

struct _ANativeWindow *
android_globals_get_window(void);

/*!
 * Publish a freshly-acquired ANativeWindow (e.g. from a new SurfaceView surface
 * on resume). Bumps a monotonic generation counter and marks the window valid so
 * a consumer (the compositor target) can detect "the surface changed under me"
 * and rebuild its VkSurfaceKHR + swapchain. Thread-safe.
 *
 * @param window the new ANativeWindow (may be NULL, treated like a clear).
 * @ingroup aux_android
 */
void
android_globals_set_window(struct _ANativeWindow *window);

/*!
 * Mark the current ANativeWindow as gone (surfaceDestroyed). Bumps the generation
 * counter and clears the valid flag so the compositor tears its surface down
 * instead of presenting to a dead window. Thread-safe.
 *
 * Keeps the ANativeWindow pointer around (still the "current" window) so a
 * consumer mid-frame can finish/idle before it next re-syncs; the consumer
 * releases the old reference itself once it has idled the GPU.
 * @ingroup aux_android
 */
void
android_globals_clear_window(void);

/*!
 * Atomically read the current window + generation + validity.
 *
 * @param[out] out_window     the current ANativeWindow (may be NULL).
 * @param[out] out_generation monotonic counter, bumped on every set/clear.
 * @param[out] out_valid      true if a live surface is currently published.
 * @ingroup aux_android
 */
void
android_globals_get_window_state(struct _ANativeWindow **out_window, uint64_t *out_generation, bool *out_valid);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_ANDROID
