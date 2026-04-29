// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_spatial_workspace extension (Phase 2.A subset)
 * @author DisplayXR
 * @ingroup external_openxr
 *
 * Public surface for a workspace controller — a privileged OpenXR client that
 * drives session lifecycle and 2D OS-window capture for a spatial workspace.
 *
 * Phase 2.A scope: lifecycle (activate/deactivate/get-state) and the capture-
 * client cluster (add/remove). The full surface (window pose, hit-test, frame
 * capture, client enumeration, lifecycle events) lands in subsequent sub-
 * phases. See docs/roadmap/spatial-workspace-extensions-headers-draft.md for
 * the complete API sketch.
 */
#ifndef XR_EXT_SPATIAL_WORKSPACE_H
#define XR_EXT_SPATIAL_WORKSPACE_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_spatial_workspace 1
#define XR_EXT_spatial_workspace_SPEC_VERSION 1
#define XR_EXT_SPATIAL_WORKSPACE_EXTENSION_NAME "XR_EXT_spatial_workspace"

// Provisional XrStructureType values. The 1000999100..104 range is reserved for
// this extension; final values reconcile with the Khronos registry before spec
// freeze.
#define XR_TYPE_WORKSPACE_CLIENT_INFO_EXT     ((XrStructureType)1000999100)
#define XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT ((XrStructureType)1000999101)
#define XR_TYPE_WORKSPACE_CAPTURE_RESULT_EXT  ((XrStructureType)1000999102)

/*!
 * @brief Workspace-local identifier for a client.
 *
 * Stable for the lifetime of the connection; reused after disconnect. 0 is
 * reserved (invalid) and represented by XR_NULL_WORKSPACE_CLIENT_ID.
 */
typedef uint32_t XrWorkspaceClientId;

#define XR_NULL_WORKSPACE_CLIENT_ID ((XrWorkspaceClientId)0)

/*!
 * @brief Type of a workspace client.
 *
 * OpenXR clients connect via xrCreateSession; captured-2D clients are adopted
 * via xrAddWorkspaceCaptureClientEXT. The workspace controller cannot tell
 * them apart from the enumeration result except via this enum.
 */
typedef enum XrWorkspaceClientTypeEXT {
    XR_WORKSPACE_CLIENT_TYPE_OPENXR_3D_EXT   = 0,
    XR_WORKSPACE_CLIENT_TYPE_CAPTURED_2D_EXT = 1,
    XR_WORKSPACE_CLIENT_TYPE_MAX_ENUM_EXT    = 0x7FFFFFFF
} XrWorkspaceClientTypeEXT;

// ---- Lifecycle ----

/*!
 * @brief Activate workspace mode on this session.
 *
 * The session becomes the privileged workspace controller. At most one
 * workspace is active per system. Caller authorization is via orchestrator-
 * PID match (with a manual-mode fallback when no orchestrator-spawned
 * controller is registered).
 *
 * @param session A valid XrSession with XR_EXT_spatial_workspace enabled.
 * @return XR_SUCCESS on success,
 *         XR_ERROR_LIMIT_REACHED if another workspace is already active,
 *         XR_ERROR_FEATURE_UNSUPPORTED if the caller is not authorized,
 *         XR_ERROR_FUNCTION_UNSUPPORTED if the extension was not enabled.
 */
typedef XrResult (XRAPI_PTR *PFN_xrActivateSpatialWorkspaceEXT)(XrSession session);

/*!
 * @brief Voluntarily release the workspace role.
 *
 * Other clients keep running; per-client compositors resume direct rendering.
 * xrDestroySession on the workspace session has the same effect implicitly.
 */
typedef XrResult (XRAPI_PTR *PFN_xrDeactivateSpatialWorkspaceEXT)(XrSession session);

/*!
 * @brief Query whether this session is currently the active workspace.
 *
 * @param session    A valid XrSession with XR_EXT_spatial_workspace enabled.
 * @param out_active Output: XR_TRUE if this session holds the workspace role.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetSpatialWorkspaceStateEXT)(
    XrSession session, XrBool32 *out_active);

// ---- Capture clients (adopt a 2D OS window) ----

/*!
 * @brief Adopt a 2D OS window as a workspace client.
 *
 * The runtime starts a platform-appropriate capture (Windows.Graphics.Capture
 * on Windows; macOS path lands in a follow-up sub-phase) and treats the
 * captured texture as a client swapchain — the workspace can position/hide it
 * like any other client.
 *
 * @param session       A valid workspace session.
 * @param nativeWindow  Windows: HWND cast to uint64_t. macOS: 0 + chained
 *                      XrWorkspaceCocoaCaptureBindingEXT (post-2.A).
 * @param nameOptional  User-visible label, may be NULL. Currently advisory —
 *                      will be propagated through IPC in a later sub-phase.
 * @param outClientId   Output: XrWorkspaceClientId for the adopted window.
 *                      Enters the same numbering space as OpenXR clients.
 */
typedef XrResult (XRAPI_PTR *PFN_xrAddWorkspaceCaptureClientEXT)(
    XrSession            session,
    uint64_t             nativeWindow,
    const char          *nameOptional,
    XrWorkspaceClientId *outClientId);

/*!
 * @brief Release a previously adopted 2D OS-window capture client.
 *
 * Stops the capture, removes the client from the workspace, and recycles the
 * client id.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRemoveWorkspaceCaptureClientEXT)(
    XrSession           session,
    XrWorkspaceClientId clientId);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrActivateSpatialWorkspaceEXT(
    XrSession session);

XRAPI_ATTR XrResult XRAPI_CALL xrDeactivateSpatialWorkspaceEXT(
    XrSession session);

XRAPI_ATTR XrResult XRAPI_CALL xrGetSpatialWorkspaceStateEXT(
    XrSession session,
    XrBool32 *out_active);

XRAPI_ATTR XrResult XRAPI_CALL xrAddWorkspaceCaptureClientEXT(
    XrSession            session,
    uint64_t             nativeWindow,
    const char          *nameOptional,
    XrWorkspaceClientId *outClientId);

XRAPI_ATTR XrResult XRAPI_CALL xrRemoveWorkspaceCaptureClientEXT(
    XrSession           session,
    XrWorkspaceClientId clientId);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_SPATIAL_WORKSPACE_H
