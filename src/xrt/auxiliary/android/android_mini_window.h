// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native access to the mini-window 1:1 layout rules (#1396).
 * @author David Fattal
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
 * The runtime's answer for a window an OEM container is scaling.
 *
 * Sizes are as `MiniWindowLayout` computed them: @p layout_* is LOGICAL (what the
 * app must lay its window/content view out at) and @p buffer_* is PHYSICAL panel
 * pixels (what the app must fix its buffer to). @p scale is the rationalised
 * container scale, informational only.
 *
 * @ingroup aux_android
 */
struct android_mini_window_hint
{
	int32_t layout_w, layout_h;
	int32_t buffer_w, buffer_h;
	int32_t rat_p, rat_q;
	float scale;
};

/*!
 * Ask the shared Java helper what an app-owned window should do inside an
 * OEM-scaled container.
 *
 * The measured policy (the vendor scale probe, the exact-integer layout search,
 * the tolerances) lives in ONE place —
 * `org.freedesktop.monado.auxiliary.MiniWindowLayout`, which the runtime's own
 * `MonadoView` also uses — so the hosted path and the
 * `XR_DXR_android_surface_binding` path can never drift apart. This is only the
 * bridge to it.
 *
 * The rect is the app's own published geometry (`xrSetAndroidWindowGeometryDXR`)
 * and @p disp_* is the panel extent in the SAME rotation. The helper applies the
 * container-scaled tell itself, so a fullscreen window answers false — that gate
 * is load-bearing, because the vendor API returns the NOMINAL mini-window
 * placement whether or not the app is in one.
 *
 * The activity comes from `android_globals_get_activity()`, i.e. the
 * `XrInstanceCreateInfoAndroidKHR::applicationActivity` the app already handed
 * us. Nothing here touches the app's view hierarchy.
 *
 * @return true and fills @p out when a 1:1 layout is both needed and knowable.
 * @ingroup aux_android
 */
bool
android_mini_window_compute_hint(int32_t x,
                                 int32_t y,
                                 uint32_t w,
                                 uint32_t h,
                                 uint32_t disp_w,
                                 uint32_t disp_h,
                                 struct android_mini_window_hint *out);

/*!
 * Is the app's Activity still in a container that can scale it (freeform /
 * split-screen), as opposed to fullscreen? #1396.
 *
 * The surface-binding latch needs this and cannot derive it from the published
 * rect: once a hint is applied the app publishes the PHYSICAL rect, which by
 * construction fits the panel, so "left the container" and "the hint is
 * working" look identical. `Activity.isInMultiWindowMode()` is the exact public
 * answer.
 *
 * @return true when still in a multi-window container, or when the question
 *         cannot be asked — never end a hint on ignorance; the tell will.
 * @ingroup aux_android
 */
bool
android_mini_window_still_scalable(void);

/*!
 * Forget everything measured for the current scaled-container episode — the
 * window left its container, so the next entry re-probes from scratch.
 * @ingroup aux_android
 */
void
android_mini_window_reset(void);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_ANDROID
