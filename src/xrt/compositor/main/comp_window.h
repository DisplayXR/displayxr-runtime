// Copyright 2019, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compositor window header.
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_main
 */

#pragma once

#include "main/comp_target_swapchain.h"
#include "main/comp_compositor.h"

#include "xrt/xrt_config_os.h"

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Functions.
 *
 */

/*!
 * Debug info target factor, always available.
 *
 * @ingroup comp_main
 */
extern const struct comp_target_factory comp_target_factory_debug_image;

#ifdef VK_USE_PLATFORM_XCB_KHR

/*!
 * Create a xcb window.
 *
 * @ingroup comp_main
 * @public @memberof comp_window_xcb
 */
struct comp_target *
comp_window_xcb_create(struct comp_compositor *c);

extern const struct comp_target_factory comp_target_factory_xcb;

#endif // VK_USE_PLATFORM_XCB_KHR

#ifdef VK_USE_PLATFORM_WAYLAND_KHR

/*!
 * Create a wayland window.
 *
 * @ingroup comp_main
 * @public @memberof comp_window_wayland
 */
struct comp_target *
comp_window_wayland_create(struct comp_compositor *c);

extern const struct comp_target_factory comp_target_factory_wayland;

/*!
 * Create a direct surface to a HMD using Wayland.
 *
 * @ingroup comp
 */
struct comp_target *
comp_window_direct_wayland_create(struct comp_compositor *c);

extern const struct comp_target_factory comp_target_factory_direct_wayland;

#endif // VK_USE_PLATFORM_WAYLAND_KHR

#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
/*!
 * Create a direct surface to an HMD over RandR.
 *
 * @ingroup comp_main
 * @public @memberof comp_window_direct_randr
 */
struct comp_target *
comp_window_direct_randr_create(struct comp_compositor *c);

extern const struct comp_target_factory comp_target_factory_direct_randr;

/*!
 * Create a direct surface to an HMD on NVIDIA.
 *
 * @ingroup comp_main
 * @public @memberof comp_window_direct_nvidia
 */
struct comp_target *
comp_window_direct_nvidia_create(struct comp_compositor *c);

extern const struct comp_target_factory comp_target_factory_direct_nvidia;
#endif // VK_USE_PLATFORM_XLIB_XRANDR_EXT

#if 1
/*!
 * Create a direct surface to an HMD on VkDisplay.
 *
 * @ingroup comp_main
 * @public @memberof comp_window_direct_vk_display
 */
struct comp_target *
comp_window_vk_display_create(struct comp_compositor *c);

extern const struct comp_target_factory comp_target_factory_vk_display;
#endif // 1

#ifdef XRT_OS_ANDROID
/*!
 * Create a surface to an HMD on Android.
 *
 * @ingroup comp_main
 * @public @memberof comp_window_android
 */
struct comp_target *
comp_window_android_create(struct comp_compositor *c);

extern const struct comp_target_factory comp_target_factory_android;
#endif // XRT_OS_ANDROID

#ifdef XRT_OS_WINDOWS

/*!
 * Create a rendering window on Windows.
 *
 * @ingroup comp_main
 * @public @memberof comp_window_mswin
 */
struct comp_target *
comp_window_mswin_create(struct comp_compositor *c);

/*!
 * Create a comp_target wrapping an external HWND provided by the application.
 * The compositor will not create its own window or window thread.
 * The application is responsible for the window message pump and lifecycle.
 *
 * @param c The compositor
 * @param external_hwnd The application's window handle (HWND)
 * @param out_ct Output parameter for the created target
 * @return true on success
 *
 * @ingroup comp_main
 * @public @memberof comp_window_mswin
 */
bool
comp_window_mswin_create_from_external(struct comp_compositor *c,
                                       void *external_hwnd,
                                       struct comp_target **out_ct);

extern const struct comp_target_factory comp_target_factory_mswin;

/*!
 * Check if the Vulkan window is in a modal drag/resize loop.
 * Returns false for non-mswin targets or app-owned windows.
 */
bool
comp_window_mswin_is_in_size_move(struct comp_target *ct);

/*!
 * Wait for WM_PAINT during drag (blocks up to 50ms).
 * Returns true if drag is active and compositor should render.
 */
bool
comp_window_mswin_wait_for_paint(struct comp_target *ct);

/*!
 * Signal that rendering is complete, unblocking the WM_PAINT handler.
 */
void
comp_window_mswin_signal_paint_done(struct comp_target *ct);

struct xrt_system_devices;

/*!
 * Pass system devices to the Windows Vulkan window for qwerty input forwarding.
 */
void
comp_window_mswin_set_system_devices(struct comp_target *ct, struct xrt_system_devices *xsysd);

#endif // XRT_OS_WINDOWS

#ifdef XRT_OS_MACOS

/*!
 * Create a rendering window on macOS using NSWindow + CAMetalLayer.
 *
 * @ingroup comp_main
 * @public @memberof comp_window_macos
 */
struct comp_target *
comp_window_macos_create(struct comp_compositor *c);

/*!
 * Create a comp_target wrapping an external NSView provided by the application.
 * The NSView must have a CAMetalLayer backing (via -makeBackingLayer).
 * The compositor will not create its own window.
 * The application is responsible for the event loop and window lifecycle.
 *
 * @param c The compositor
 * @param external_view The application's NSView* (with CAMetalLayer backing)
 * @param out_ct Output parameter for the created target
 * @return true on success
 *
 * @ingroup comp_main
 * @public @memberof comp_window_macos
 */
bool
comp_window_macos_create_from_external(struct comp_compositor *c,
                                       void *external_view,
                                       struct comp_target **out_ct);

extern const struct comp_target_factory comp_target_factory_macos;

#endif // XRT_OS_MACOS

/*!
 * Create an offscreen comp_target that reads back composited pixels via a callback.
 * Used for WebXR bridge readback mode (Option A).
 *
 * @param c The compositor
 * @param callback Called with composited RGBA pixels after each present
 * @param userdata Passed to callback
 * @return The created target, or NULL on failure
 *
 * @ingroup comp_main
 */
struct comp_target *
comp_window_offscreen_create(struct comp_compositor *c,
                             void (*callback)(const uint8_t *, uint32_t, uint32_t, void *),
                             void *userdata);

#ifdef __cplusplus
}
#endif
