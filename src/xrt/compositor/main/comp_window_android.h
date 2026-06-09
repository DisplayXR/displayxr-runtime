// Copyright 2019-2020, Collabora, Ltd.
// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Android present target for the out-of-process service compositor.
 * @ingroup comp_main
 *
 * DisplayXR (#510): a @ref comp_target_swapchain subclass that presents to the
 * app's ANativeWindow inside the runtime service process. The window is the one
 * the app handed across the IPC boundary (Client.java passAppSurface →
 * MonadoImpl → android_globals); this target pulls it from android_globals
 * rather than receiving a window handle through IPC. Ported from the
 * Monado-legacy comp_window_android (the factory + in-process Activity/custom
 * surface branches are dropped — the service has no Activity).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct comp_compositor;
struct comp_target;

/*!
 * Create an Android present target. @p c is the owning compositor (in the
 * service this is a @ref null_compositor up-cast to @ref comp_compositor — both
 * start with @ref comp_base, so @p c->base.vk is valid). Returns the target's
 * @ref comp_target base, or NULL on allocation failure. The VkSurfaceKHR is not
 * created until @ref comp_target::init_post_vulkan runs.
 *
 * @ingroup comp_main
 */
struct comp_target *
comp_window_android_create(struct comp_compositor *c);

#ifdef __cplusplus
}
#endif
