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

#include <stdbool.h>
#include <stdint.h>

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

/*!
 * Tier-2 window placement (#59): reposition and resize the runtime-owned NSWindow
 * to a display sub-rect so multiple workspace clients tile instead of stacking
 * full-screen. @p x,y,w,h are top-left-origin display PIXELS (the same space the
 * Windows service uses); this converts them to AppKit points (bottom-left origin)
 * for the NSWindow frame and updates the CAMetalLayer drawableSize to the pixel
 * size on the main thread. The caller (comp_multi) flags the per-session swapchain
 * for recreation so the next frame rebuilds at the new surface size.
 *
 * @ingroup comp_main
 */
void
comp_window_macos_set_window_rect(struct comp_target *ct, int32_t x, int32_t y, int32_t w, int32_t h);

/*!
 * Show (orderFront) or hide (orderOut) the full-screen surface window (#61). The
 * macOS service is a persistent daemon; when no workspace controller is connected
 * (Ctrl+Space toggled the shell off) it hides the borderless full-screen window
 * so the normal macOS desktop returns, and shows it again when a controller
 * reconnects. Runs on the main thread.
 *
 * @ingroup comp_main
 */
void
comp_window_macos_set_visible(struct comp_target *ct, bool visible);

#ifdef __cplusplus
}
#endif
