// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not. SPEC_VERSION continues the pre-rename XR_EXT_* numbering (the
// interface history did not restart with the name).
// See GOVERNANCE.md in https://github.com/DisplayXR/displayxr-extensions.
//
/*!
 * @file
 * @brief  Header for XR_DXR_xlib_window_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension allows an OpenXR application to provide its own X11 window
 * (Display* + Window) to the runtime on desktop Linux. When provided, the
 * runtime will render into the application's window instead of creating its
 * own window. Sibling of XR_DXR_win32_window_binding (HWND) and
 * XR_DXR_cocoa_window_binding (NSView).
 *
 * This enables:
 * - Windowed mode rendering (vs fullscreen)
 * - Application control over window input (keyboard, mouse)
 * - Multiple OpenXR applications on the same display
 *
 * The API takes Xlib types (Display* / Window) — the dominant convention for
 * X11 apps and toolkits. The runtime converts to XCB internally via
 * XGetXCBConnection() (libX11-xcb) and builds its VkSurfaceKHR with
 * VK_KHR_xcb_surface, so the supplied Display must be an Xlib display (as
 * returned by XOpenDisplay), not a bare XCB connection.
 *
 * Offscreen readback and shared-texture (`_texture` class) handoff are NOT
 * part of this revision — see the win32/cocoa siblings for the intended shape
 * of a future addition.
 */
#ifndef XR_DXR_XLIB_WINDOW_BINDING_H
#define XR_DXR_XLIB_WINDOW_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_xlib_window_binding 1
#define XR_DXR_xlib_window_binding_SPEC_VERSION 2
#define XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME "XR_DXR_xlib_window_binding"

// Window-space composition layer. Every window/surface binding extension
// (win32, cocoa, xlib, wayland, android) enables it and declares it with this
// SAME guarded block, so any combination of these headers can be included in
// any order. The guard is the type value, shared by all five copies.
#ifndef XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR ((XrStructureType)1004999002)

/*!
 * @brief Composition layer positioned in fractional window coordinates.
 *
 * Renders a textured quad at a position specified as fractions of the target
 * window dimensions. Coordinates automatically scale when the window is resized.
 * The same texture is composited into every view with a per-eye horizontal
 * shift controlled by the disparity parameter. The layer is composited before
 * the display processor (it is woven like any other layer).
 *
 * Valid on a session created with a window/surface binding, on a session of a
 * native compositor, and on an IPC (service) session.
 *
 * @extends XrFrameEndInfo (submitted as a composition layer)
 */
typedef struct XrCompositionLayerWindowSpaceDXR {
    XrStructureType             type;       //!< Must be XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR
    const void* XR_MAY_ALIAS    next;       //!< Pointer to next structure in chain
    XrCompositionLayerFlags     layerFlags; //!< e.g. XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    XrSwapchainSubImage         subImage;   //!< Source swapchain + rect
    float                       x;          //!< Left edge, fraction of window width  [0..1]
    float                       y;          //!< Top edge, fraction of window height   [0..1]
    float                       width;      //!< Fraction of window width  [0..1]
    float                       height;     //!< Fraction of window height [0..1]
    float                       disparity;  //!< Horizontal shift, fraction of window width.
                                            //!< 0 = screen depth, negative = toward viewer
} XrCompositionLayerWindowSpaceDXR;
#endif // XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR

// Value from the DisplayXR provisional 1004999xxx block — decade 1004999200–209
// claimed in this directory's README.md allocation registry. Replace with an
// official Khronos-assigned value if the extension is standardized.
#define XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999200)

// The binding struct uses Xlib types and is only meaningful on desktop Linux
// (Android also reports __linux__ but has no X11).
#if defined(__linux__) && !defined(__ANDROID__)

#if !defined(_XLIB_H_) && !defined(_X11_XLIB_H_)
// <X11/Xlib.h> was not included first — provide binary-compatible stand-ins so
// this header stays self-contained (same trick as xrt_openxr_includes.h).
// Display is opaque; Window is an XID, i.e. unsigned long on the client side.
typedef struct _XDisplay Display;
#define XR_XLIB_WINDOW_TYPE_ unsigned long
#else
#define XR_XLIB_WINDOW_TYPE_ Window
#endif

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide
 *        an external X11 window for session rendering on desktop Linux.
 *
 * When this structure is provided in the next chain of XrSessionCreateInfo,
 * the runtime will render into the specified window instead of creating
 * its own window. The application is responsible for:
 * - Creating and managing the window lifecycle
 * - Mapping the window and running the X event loop
 * - Processing input events
 *
 * Both xDisplay and window must be valid; the Display connection must outlive
 * the session (the runtime derives its XCB connection from it and borrows it
 * for the lifetime of the Vulkan surface).
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrXlibWindowBindingCreateInfoDXR {
    XrStructureType             type;      //!< Must be XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS    next;      //!< Pointer to next structure in chain
    Display*                    xDisplay;  //!< Xlib display connection (XOpenDisplay)
    XR_XLIB_WINDOW_TYPE_        window;    //!< X11 Window (XID) owned by the app
    //! When XR_TRUE, the runtime configures the bound window for transparent
    //! desktop composition: pixels the app writes with full opacity appear
    //! opaque on screen, and pixels written transparent (alpha = 0) compose
    //! through to the desktop underneath. On X11 the runtime selects a 32-bit
    //! ARGB visual and a non-opaque swapchain compositeAlpha; a compositing
    //! window manager (e.g. GNOME/Mutter, KWin, picom) must be running for the
    //! desktop to show through. Only honored when both xDisplay and window are
    //! valid. Sibling of XrWin32WindowBindingCreateInfoDXR::transparentBackgroundEnabled
    //! (SPEC_VERSION 2).
    XrBool32                    transparentBackgroundEnabled;
} XrXlibWindowBindingCreateInfoDXR;

#undef XR_XLIB_WINDOW_TYPE_

#endif // defined(__linux__) && !defined(__ANDROID__)

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_XLIB_WINDOW_BINDING_H
