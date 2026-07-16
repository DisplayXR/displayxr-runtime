// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not.
// See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_DXR_wayland_surface_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension lets an OpenXR application provide its own Wayland surface
 * (wl_display* + wl_surface*) to the runtime on desktop Linux. When provided,
 * the runtime renders into the application's surface instead of creating its
 * own window. Sibling of XR_DXR_xlib_window_binding (X11), XR_DXR_win32_window_binding
 * (HWND), and XR_DXR_cocoa_window_binding (NSView).
 *
 * The app owns the surface lifecycle (registry, xdg-shell toplevel, configure
 * acks, the Wayland event loop); the runtime only builds its VkSurfaceKHR from
 * the pair via VK_KHR_wayland_surface. Transparency is native on Wayland — a
 * surface composites its premultiplied alpha over whatever is behind it, so
 * transparentBackgroundEnabled needs no ARGB-visual dance (unlike X11).
 */
#ifndef XR_DXR_WAYLAND_SURFACE_BINDING_H
#define XR_DXR_WAYLAND_SURFACE_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_wayland_surface_binding 1
#define XR_DXR_wayland_surface_binding_SPEC_VERSION 1
#define XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME "XR_DXR_wayland_surface_binding"

// Value from the DisplayXR provisional 1004999xxx block — decade 1004999210–219
// (the xlib sibling claimed 200–209). Replace with an official Khronos-assigned
// value if the extension is standardized.
#define XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999210)

// Only meaningful on desktop Linux (Android also reports __linux__ but has no
// Wayland desktop surface path).
#if defined(__linux__) && !defined(__ANDROID__)

// Opaque stand-ins so the header stays self-contained when <wayland-client.h>
// was not included first (same trick as the xlib sibling). wl_display/wl_surface
// are opaque structs on the client side.
struct wl_display;
struct wl_surface;

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide an
 *        external Wayland surface for session rendering on desktop Linux.
 *
 * Both wlDisplay and wlSurface must be valid; the wl_display connection must
 * outlive the session (the runtime borrows it for the lifetime of the Vulkan
 * surface).
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrWaylandSurfaceBindingCreateInfoDXR {
    XrStructureType             type;       //!< Must be XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS    next;       //!< Pointer to next structure in chain
    struct wl_display*          wlDisplay;  //!< Wayland display connection (wl_display_connect)
    struct wl_surface*          wlSurface;  //!< Wayland surface owned by the app
    //! When XR_TRUE, the runtime picks a non-opaque swapchain compositeAlpha so
    //! pixels the app writes transparent (alpha = 0) compose through to whatever
    //! is behind the surface. Native on Wayland — no ARGB visual needed. Only
    //! honored when both wlDisplay and wlSurface are valid. Sibling of
    //! XrXlibWindowBindingCreateInfoDXR::transparentBackgroundEnabled.
    XrBool32                    transparentBackgroundEnabled;
} XrWaylandSurfaceBindingCreateInfoDXR;

#endif // defined(__linux__) && !defined(__ANDROID__)

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_WAYLAND_SURFACE_BINDING_H
