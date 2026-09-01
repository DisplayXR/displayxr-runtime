// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Functions for adding a new Surface to a window and otherwise
 *         interacting with an Android View.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup aux_android
 */

#pragma once

#include <xrt/xrt_config_os.h>
#include <xrt/xrt_limits.h>

#ifdef XRT_OS_ANDROID

#include <android/native_window.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _JNIEnv;
struct _JavaVM;

struct xrt_android_display_metrics
{
	int width_pixels;
	int height_pixels;
	int density_dpi;
	float density;
	float scaled_density;
	float xdpi;
	float ydpi;
	float refresh_rate;
	float refresh_rates[XRT_MAX_SUPPORTED_REFRESH_RATES];
	uint32_t refresh_rate_count;
};

/*!
 * Opaque type representing a custom surface added to a window, and the async
 * operation to perform this adding.
 *
 * @note You must keep this around for as long as you're using the surface.
 */
struct android_custom_surface;

/*!
 * Start adding a custom surface to a window.
 *
 * This is an asynchronous operation, so this creates an opaque pointer for you
 * to check on the results and maintain a reference to the result.
 *
 * Uses org.freedesktop.monado.auxiliary.MonadoView
 *
 * @param vm Java VM pointer
 * @param context An android.content.Context jobject, cast to `void *`.
 * @param display_id ID of the display that the surface is attached to.
 * @param surface_title Title of the surface.
 * @param preferred_display_mode_id The preferred display mode ID.
 *        A value of 0 indicates no preference.
 *        Non-zero values map to the corresponding display mode
 *        ID that are returned from the getSupportedModes() method for
 *        the given Android display. (the 1-indexed IDs.)
 * @param span_system_bars When true the window lays out edge-to-edge over the
 *        FULL physical panel (FLAG_LAYOUT_IN_SCREEN | FLAG_LAYOUT_NO_LIMITS),
 *        ignoring system-bar insets. The weave satellite (#1277) needs this:
 *        its overlay is the physical-pixel canvas, and an inset overlay cannot
 *        cover an edge-to-edge client window (the immersive-fullscreen browser
 *        extends into the status-bar band, and the weave then lands shifted by
 *        the inset — the "tap toggles immersive -> broken weave" bug). Existing
 *        callers pass false and keep the inset layout.
 *
 * @return An opaque handle for monitoring this operation and referencing the
 * surface, or NULL if there was an error.
 *
 * @public @memberof android_custom_surface
 */
struct android_custom_surface *
android_custom_surface_async_start(struct _JavaVM *vm,
                                   void *context,
                                   int32_t display_id,
                                   const char *surface_title,
                                   int32_t preferred_display_mode_id,
                                   bool span_system_bars);

/*!
 * Destroy the native handle for the custom surface.
 *
 * Depending on the state, this may not necessarily destroy the underlying
 * surface, if other references exist. However, a flag will be set to indicate
 * that native code is done using it.
 *
 * @param ptr_custom_surface Pointer to the opaque pointer: will be set to NULL.
 *
 * @public @memberof android_custom_surface
 */
void
android_custom_surface_destroy(struct android_custom_surface **ptr_custom_surface);

/*!
 * Get the ANativeWindow pointer corresponding to the added Surface, if
 * available, waiting up to the specified duration.
 *
 * This may return NULL because the underlying operation is asynchronous.
 *
 * OWNERSHIP (#1040 / #1146): the returned window carries an OWNED reference
 * that is independent of the one `android_globals` holds. The caller must pair
 * it with exactly one release — in practice by handing it to
 * @ref android_globals_store_window, which consumes one reference. The window
 * is also published into the globals as a side effect, using a separate
 * reference of its own; do NOT assume the two are the same one.
 *
 * @public @memberof android_custom_surface
 */
ANativeWindow *
android_custom_surface_wait_get_surface(struct android_custom_surface *custom_surface, uint64_t timeout_ms);

/*!
 * Pull the current SurfaceView surface (non-blocking) and republish it to
 * android_globals: clears the window when the surface is gone (backgrounded) and
 * publishes a fresh ANativeWindow when a new surface arrives (resume). Call this
 * periodically from a JVM-attached thread (oxr_session_poll) so the compositor's
 * surface re-sync sees background/resume without relying on JNI surface-callback
 * registration. #507
 *
 * @public @memberof android_custom_surface
 */
void
android_custom_surface_refresh_window(struct android_custom_surface *custom_surface);

bool
android_custom_surface_get_display_metrics(struct _JavaVM *vm,
                                           void *activity,
                                           struct xrt_android_display_metrics *out_metrics);

bool
android_custom_surface_can_draw_overlays(struct _JavaVM *vm, void *context);

float
android_custom_surface_get_display_refresh_rate(struct _JavaVM *vm, void *context);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_ANDROID
