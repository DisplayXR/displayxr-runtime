// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS present target for the out-of-process service compositor.
 * @ingroup comp_main
 *
 * DisplayXR (macOS shell, Tier 1): a @ref comp_target_swapchain subclass that
 * presents to a runtime-owned NSWindow inside the service process via a
 * VK_EXT_metal_surface (MoltenVK) swapchain over a CAMetalLayer. Mirrors
 * @ref comp_window_android, but — unlike Android, where the app injects its
 * surface across IPC (android_globals) — macOS cannot pass an NSView across
 * process boundaries, so for the single-app Tier-1 path the service *creates
 * its own* NSWindow (the runtime-owned / hosted model). The CAMetalLayer and
 * VkSurfaceKHR are created lazily in @ref comp_target::init_post_vulkan.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct comp_compositor;
struct comp_target;

/*!
 * Create a macOS present target. @p c is the owning compositor (in the service
 * this is a @ref null_compositor up-cast to @ref comp_compositor — both start
 * with @ref comp_base, so @p c->base.vk is valid). Returns the target's
 * @ref comp_target base, or NULL on allocation failure. The NSWindow,
 * CAMetalLayer and VkSurfaceKHR are not created until
 * @ref comp_target::init_post_vulkan runs (which carries the requested extent).
 *
 * @ingroup comp_main
 */
struct comp_target *
comp_window_macos_create(struct comp_compositor *c);

#ifdef __cplusplus
}
#endif
