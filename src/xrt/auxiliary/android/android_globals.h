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
 * Keeps the ANativeWindow pointer AND the globals' own reference on it, so the
 * pointer can never dangle between the clear and the next publish (#1040).
 * @ingroup aux_android
 */
void
android_globals_clear_window(void);

/*!
 * Atomically read the current window + generation + validity.
 *
 * The returned pointer is NOT referenced for the caller — it is only safe to
 * compare/inspect. A caller that intends to KEEP the window (build a
 * VkSurfaceKHR from it, stash it in a target struct) must use
 * @ref android_globals_acquire_window instead (#1040).
 *
 * @param[out] out_window     the current ANativeWindow (may be NULL).
 * @param[out] out_generation monotonic counter, bumped on every set/clear.
 * @param[out] out_valid      true if a live surface is currently published.
 * @ingroup aux_android
 */
void
android_globals_get_window_state(struct _ANativeWindow **out_window, uint64_t *out_generation, bool *out_valid);

/*!
 * Atomically take an OWNED reference on the currently published window (#1040).
 *
 * This is the only safe way for a consumer to keep the window: it acquires
 * under the same lock that publishes/replaces it, so a concurrent
 * @ref android_globals_set_window can never free the window between the read
 * and the acquire. The caller must pair the returned pointer with exactly one
 * `ANativeWindow_release()`.
 *
 * Before #1040 every consumer "adopted the published reference" instead, which
 * made the reference count depend on how many compositor instances happened to
 * consume one publish — a second consumer (an xrEndSession→xrBeginSession
 * cycle, or a second satellite session) released a reference nobody had taken,
 * destroying the native Surface under the still-live Java `Surface` and
 * SIGSEGVing the finalizer.
 *
 * @param[out] out_generation monotonic counter matching the returned window.
 * @param[out] out_valid      true if a live surface is currently published.
 * @return the referenced ANativeWindow, or NULL if none is published.
 * @ingroup aux_android
 */
struct _ANativeWindow *
android_globals_acquire_window(uint64_t *out_generation, bool *out_valid);

/*!
 * Store/retrieve the active android_custom_surface (opaque) so a periodic tick
 * (oxr_session_poll) can pull the live SurfaceView surface and republish it via
 * android_globals_set_window / android_globals_clear_window on resume/background.
 * Avoids relying on JNI surface-callback registration, which is unreliable when
 * the runtime loads MonadoView under multiple classloaders. #507
 * @ingroup aux_android
 */
void
android_globals_set_custom_surface(void *custom_surface);

void *
android_globals_get_custom_surface(void);

/*!
 * Per-session overlay mode (#558 per-app). Set by the runtime service
 * (MonadoImpl, from the connecting client's manifest) in the SERVICE process;
 * read by the vendor display-processor plug-in to decide keep-3D-while-
 * backgrounded. Process-local — only meaningful in the service process.
 * @ingroup aux_android
 */
void
android_globals_set_overlay_mode(bool enabled);

bool
android_globals_get_overlay_mode(void);

/*!
 * True if THIS process's own package declares
 * `<meta-data android:name="com.displayxr.overlay_mode" android:value="true"/>`.
 * Read in the APP process (the OpenXR client) — e.g. oxr_session keep-alive —
 * where the connecting app IS this process. Queried once via JNI and cached.
 * @ingroup aux_android
 */
bool
android_globals_self_declares_overlay(void);

/*!
 * @name Window screen rect (ADR-036 D6, #1033)
 *
 * The client's SurfaceView on-screen rectangle, sampled by the app process from
 * a `Choreographer` callback (`View.getLocationOnScreen` + width/height +
 * `Display.getDisplayId`) and forwarded to the SERVICE process over
 * `IMonado.updateWindowRect`. A pure window MOVE on Android raises no resize —
 * SurfaceFlinger just repositions the layer with the old buffer — so the
 * compositor cannot learn the window's panel origin from the surface itself,
 * and without it the vendor weaver interlaces against the wrong screen position
 * (visible as a collapsed/ghosted 3D image in any window that is not at the
 * panel's top-left).
 *
 * Process-local and NOT keyed by client: one satellite compositor process
 * serves exactly one client (ADR-036 Architecture C), which is what makes a
 * plain global correct here. If a process ever hosts several clients this must
 * become per-client state — the same keying #967b/F3 requires everywhere else.
 * @{
 */

/*!
 * Publish the client window's on-screen rect (physical screen pixels, current
 * orientation, as Android reports it). Bumps a generation so the consumer can
 * cheaply detect a change.
 * @ingroup aux_android
 */
void
android_globals_set_window_screen_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t display_id);

/*!
 * Read the last published window rect.
 *
 * @return false if no rect has ever been published (⟹ the compositor leaves the
 *         DP display-scoped, exactly today's behaviour).
 * @ingroup aux_android
 */
bool
android_globals_get_window_screen_rect(
    int32_t *out_x, int32_t *out_y, uint32_t *out_w, uint32_t *out_h, int32_t *out_display_id, uint64_t *out_generation);

/*! @} */

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_ANDROID
