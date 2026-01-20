// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_session_target extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension allows an OpenXR application to provide its own window handle
 * (HWND on Windows) to the runtime. When provided, the runtime will render
 * into the application's window instead of creating its own window.
 *
 * This enables:
 * - Windowed mode rendering (vs fullscreen)
 * - Application control over window input (keyboard, mouse)
 * - Multiple OpenXR applications on the same display
 */
#ifndef XR_EXT_SESSION_TARGET_H
#define XR_EXT_SESSION_TARGET_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_session_target 1
#define XR_EXT_session_target_SPEC_VERSION 1
#define XR_EXT_SESSION_TARGET_EXTENSION_NAME "XR_EXT_session_target"

// Use a value in the vendor extension range (1000000000+)
// This should be replaced with an official Khronos-assigned value if the extension is standardized
#define XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT ((XrStructureType)1000999001)

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide
 *        an external window handle for session rendering.
 *
 * When this structure is provided in the next chain of XrSessionCreateInfo,
 * the runtime will render into the specified window instead of creating
 * its own window. The application is responsible for:
 * - Creating and managing the window lifecycle
 * - Handling the window message pump
 * - Processing input events
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrSessionTargetCreateInfoEXT {
    XrStructureType             type;           //!< Must be XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS    next;           //!< Pointer to next structure in chain
#ifdef _WIN32
    void*                       windowHandle;   //!< HWND of the target window (Windows only)
#else
    void*                       windowHandle;   //!< Platform-specific window handle (reserved)
#endif
} XrSessionTargetCreateInfoEXT;

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_SESSION_TARGET_H
