// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_macos_window_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension allows an OpenXR application to provide its own NSView
 * (with CAMetalLayer backing) to the runtime on macOS. When provided,
 * the runtime will render into the application's view instead of creating
 * its own window.
 *
 * This enables:
 * - Windowed mode rendering (vs fullscreen)
 * - Application control over window input (keyboard, mouse)
 * - Multiple OpenXR applications on the same display
 * - HUD overlays and custom UI compositing
 *
 * The app provides an NSView subclass whose -makeBackingLayer returns
 * a CAMetalLayer. MoltenVK creates its VkSurfaceKHR from this layer.
 */
#ifndef XR_EXT_MACOS_WINDOW_BINDING_H
#define XR_EXT_MACOS_WINDOW_BINDING_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_macos_window_binding 1
#define XR_EXT_macos_window_binding_SPEC_VERSION 1
#define XR_EXT_MACOS_WINDOW_BINDING_EXTENSION_NAME "XR_EXT_macos_window_binding"

// Use a value in the vendor extension range (1000000000+)
// This should be replaced with an official Khronos-assigned value if the extension is standardized
#define XR_TYPE_MACOS_WINDOW_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999003)

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide
 *        an external NSView handle for session rendering on macOS.
 *
 * When this structure is provided in the next chain of XrSessionCreateInfo,
 * the runtime will render into the specified view instead of creating
 * its own window. The application is responsible for:
 * - Creating and managing the NSWindow + NSView lifecycle
 * - Running the NSApplication event loop
 * - Processing input events
 *
 * The viewHandle must point to an NSView whose backing layer is a
 * CAMetalLayer (i.e., -makeBackingLayer returns [CAMetalLayer layer]).
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrMacOSWindowBindingCreateInfoEXT {
    XrStructureType          type;       //!< Must be XR_TYPE_MACOS_WINDOW_BINDING_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS next;       //!< Pointer to next structure in chain
    void*                    viewHandle; //!< NSView* with CAMetalLayer backing (macOS only)
} XrMacOSWindowBindingCreateInfoEXT;

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_MACOS_WINDOW_BINDING_H
