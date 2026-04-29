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
#define XR_EXT_spatial_workspace_SPEC_VERSION 3
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

// ---- Window pose + visibility (spec_version 2) ----

/*!
 * @brief Set position, orientation, and physical size of a client's window.
 *
 * The runtime composites the named client's swapchain into a quad at this
 * pose; the next frame reflects the change. Pose origin is the display
 * center; +x right, +y up, +z toward the viewer. Width and height are the
 * quad's physical extent in meters — the runtime stretches the client's
 * atlas to fit.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_VALIDATION_FAILURE if clientId is unknown or not a
 *         positionable client (some platform-internal clients may be
 *         excluded), XR_ERROR_FEATURE_UNSUPPORTED if the session is not
 *         the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientWindowPoseEXT)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    const XrPosef       *pose,
    float                widthMeters,
    float                heightMeters);

/*!
 * @brief Read back the current pose and physical size of a client's window.
 *
 * Useful for controllers that need to preserve relative layout when one
 * client moves, or for restoring saved workspaces. All output pointers
 * must be non-NULL.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetWorkspaceClientWindowPoseEXT)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrPosef             *outPose,
    float               *outWidthMeters,
    float               *outHeightMeters);

/*!
 * @brief Show or hide a client's window without destroying it.
 *
 * A hidden client keeps running but does not contribute to the composite —
 * standard "minimize" semantics. The client may continue rendering frames
 * that the runtime drops, or its render thread may pause; that is a
 * client-side concern. xrGetSpatialWorkspaceClientVisibilityEXT (future)
 * would expose readback; Phase 2.C ships set-only.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientVisibilityEXT)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrBool32             visible);

// ---- Hit-test (spec_version 3) ----

/*!
 * @brief Spatial raycast hit-test against workspace windows.
 *
 * Translates a screen-space cursor (display pixels, origin top-left) to a
 * client window content hit. The runtime intersects an eye→cursor ray
 * with each workspace client's window quad; the closest content hit
 * (excluding chrome and edge-resize zones — those stay runtime-policy
 * until Phase 2.C migrates chrome) wins.
 *
 * @param session      A valid workspace session.
 * @param cursorX      Cursor X in display pixels (origin top-left).
 * @param cursorY      Cursor Y in display pixels.
 * @param outClientId  Output: clientId of the hit window, or
 *                     XR_NULL_WORKSPACE_CLIENT_ID for miss / chrome hit.
 * @param outLocalUV   Output: U,V on the window quad in [0,1] range, origin
 *                     top-left. Both 0,0 on miss.
 *
 * The workspace controller calls this from its input handler, then decides
 * what the hit means (focus, drag, right-click forward, etc.). The runtime
 * does NOT classify hits — that is policy.
 */
typedef XrResult (XRAPI_PTR *PFN_xrWorkspaceHitTestEXT)(
    XrSession            session,
    int32_t              cursorX,
    int32_t              cursorY,
    XrWorkspaceClientId *outClientId,
    XrVector2f          *outLocalUV);

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

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientWindowPoseEXT(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    const XrPosef       *pose,
    float                widthMeters,
    float                heightMeters);

XRAPI_ATTR XrResult XRAPI_CALL xrGetWorkspaceClientWindowPoseEXT(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrPosef             *outPose,
    float               *outWidthMeters,
    float               *outHeightMeters);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientVisibilityEXT(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrBool32             visible);

XRAPI_ATTR XrResult XRAPI_CALL xrWorkspaceHitTestEXT(
    XrSession            session,
    int32_t              cursorX,
    int32_t              cursorY,
    XrWorkspaceClientId *outClientId,
    XrVector2f          *outLocalUV);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_SPATIAL_WORKSPACE_H
