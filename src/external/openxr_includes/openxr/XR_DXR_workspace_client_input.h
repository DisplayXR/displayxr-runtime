// Copyright 2026, DisplayXR
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
 * @brief  Header for XR_DXR_workspace_client_input extension
 * @ingroup external_openxr
 *
 * KEYBOARD AND MOUSE FOR A CONTENT APP INSIDE A WORKSPACE. Under a workspace
 * controller (the DisplayXR Shell, or any registered equivalent) on a platform
 * where the service composes every app onto ONE shared surface (desktop Linux,
 * macOS), the app's own window is not on screen: the service owns the surface
 * the user points at and types into. The service hit-tests each event against
 * the placed windows and keeps a queue per content app (keys follow workspace
 * focus, pointer / motion / scroll follow the window under the cursor). This
 * extension is how a CONTENT app drains its queue.
 *
 * Enable it whenever the runtime advertises it. Outside a workspace (no
 * controller active, or an in-process session) the drain returns zero events:
 * the app's own window receives input natively there, and the runtime never
 * duplicates it. Inside a workspace the app's own window is not mapped, so
 * there is no double delivery either way.
 *
 * Not for the workspace controller: it drains its own stream with
 * xrEnumerateWorkspaceInputEventsDXR (XR_DXR_spatial_workspace). A controller
 * session that calls xrEnumerateWorkspaceClientInputEventsDXR gets zero events.
 *
 * Advertised on desktop Linux and macOS only. On Windows the service delivers
 * workspace input to a content app as ordinary window messages on the app's
 * own (hidden) window, so no extension is needed there.
 */
#ifndef XR_DXR_WORKSPACE_CLIENT_INPUT_H
#define XR_DXR_WORKSPACE_CLIENT_INPUT_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_workspace_client_input 1
#define XR_DXR_workspace_client_input_SPEC_VERSION 1
#define XR_DXR_WORKSPACE_CLIENT_INPUT_EXTENSION_NAME "XR_DXR_workspace_client_input"

typedef enum XrWorkspaceClientInputEventTypeDXR {
    XR_WORKSPACE_CLIENT_INPUT_EVENT_KEY_DXR    = 0,
    XR_WORKSPACE_CLIENT_INPUT_EVENT_BUTTON_DXR = 1,
    XR_WORKSPACE_CLIENT_INPUT_EVENT_MOTION_DXR = 2,
    XR_WORKSPACE_CLIENT_INPUT_EVENT_SCROLL_DXR = 3,
    XR_WORKSPACE_CLIENT_INPUT_EVENT_TYPE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrWorkspaceClientInputEventTypeDXR;

/*!
 * @brief One input event routed to this session by the workspace.
 *
 * A plain record (no type / next): fields that do not apply to @ref eventType
 * are zero.
 *
 * @ref position is the cursor in the app's window CONTENT area, normalized to
 * [0,1] with a top-left origin. While a button pressed inside the window is
 * held, MOTION and the release keep coming to this app even when the cursor
 * leaves the window, and @ref position may then fall outside [0,1].
 */
typedef struct XrWorkspaceClientInputEventDXR {
    XrWorkspaceClientInputEventTypeDXR eventType;
    uint32_t   timestampMs;   //!< host monotonic ms, low 32 bits
    uint32_t   vkCode;        //!< KEY: Win32 VK_* code (letters = 'A'..'Z', digits '0'..'9')
    XrBool32   isDown;        //!< KEY, BUTTON
    uint32_t   button;        //!< BUTTON: 1 = left, 2 = right, 3 = middle
    uint32_t   buttonMask;    //!< MOTION: bit0 left, bit1 right, bit2 middle held
    uint32_t   modifiers;     //!< bit0 SHIFT, bit1 CTRL, bit2 ALT, bit3 SUPER (as the wire carries)
    float      scrollDeltaY;  //!< SCROLL: wheel notches, + = up / away
    XrBool32   positionValid; //!< BUTTON/MOTION/SCROLL: @ref position is set
    XrVector2f position;      //!< the cursor in the app's window CONTENT, normalized [0,1], top-left origin (may fall outside during a drag)
} XrWorkspaceClientInputEventDXR;

/*!
 * @brief Drain this session's routed workspace input, oldest first.
 *
 * Destructive: the events written are removed from the queue. Writes up to
 * @p capacityInput events and sets @p countOutput to the number written.
 * @p capacityInput 0 writes nothing, drains nothing and sets @p countOutput
 * to 0. Returns XR_SUCCESS with 0 events when no workspace controller is
 * active, when the session is in-process, or when the caller is the workspace
 * controller itself.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateWorkspaceClientInputEventsDXR)(XrSession session, uint32_t capacityInput, uint32_t* countOutput, XrWorkspaceClientInputEventDXR* events);

#ifndef XR_NO_PROTOTYPES
#ifdef XR_EXTENSION_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateWorkspaceClientInputEventsDXR(
    XrSession                                   session,
    uint32_t                                    capacityInput,
    uint32_t*                                   countOutput,
    XrWorkspaceClientInputEventDXR*             events);
#endif /* XR_EXTENSION_PROTOTYPES */
#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif
