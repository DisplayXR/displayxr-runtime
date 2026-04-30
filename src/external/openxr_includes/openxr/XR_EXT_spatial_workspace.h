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
#define XR_EXT_spatial_workspace_SPEC_VERSION 6
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

/*!
 * @brief Classification of a spatial hit-test result.
 *
 * Phase 2.D established hit-region as the shared vocabulary between runtime
 * and workspace controller: the runtime classifies (it has the geometry) and
 * the controller interprets (focus, drag, close, etc.). The taskbar and
 * launcher tile regions are produced by the launcher hit-test path; window
 * raycasts produce the rest.
 */
typedef enum XrWorkspaceHitRegionEXT {
    XR_WORKSPACE_HIT_REGION_BACKGROUND_EXT       = 0,  // miss
    XR_WORKSPACE_HIT_REGION_CONTENT_EXT           = 1,
    XR_WORKSPACE_HIT_REGION_TITLE_BAR_EXT         = 2,
    XR_WORKSPACE_HIT_REGION_CLOSE_BUTTON_EXT      = 3,
    XR_WORKSPACE_HIT_REGION_MINIMIZE_BUTTON_EXT   = 4,
    XR_WORKSPACE_HIT_REGION_MAXIMIZE_BUTTON_EXT   = 5,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_N_EXT     = 10,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_S_EXT     = 11,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_E_EXT     = 12,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_W_EXT     = 13,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_NE_EXT    = 14,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_NW_EXT    = 15,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_SE_EXT    = 16,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_SW_EXT    = 17,
    XR_WORKSPACE_HIT_REGION_TASKBAR_EXT           = 20,
    XR_WORKSPACE_HIT_REGION_LAUNCHER_TILE_EXT     = 21,
    XR_WORKSPACE_HIT_REGION_MAX_ENUM_EXT          = 0x7FFFFFFF
} XrWorkspaceHitRegionEXT;

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

// ---- Hit-test (spec_version 3, region out added in spec_version 4) ----

/*!
 * @brief Spatial raycast hit-test against workspace windows.
 *
 * Translates a screen-space cursor (display pixels, origin top-left) to a
 * client window hit. The runtime intersects an eye→cursor ray with each
 * workspace client's window quad and reports both the hit clientId and a
 * region classification (XrWorkspaceHitRegionEXT). The runtime classifies
 * geometry; the controller decides what each region means (focus on
 * CONTENT, drag on TITLE_BAR, close on CLOSE_BUTTON, resize on the
 * EDGE_RESIZE_* values, etc.).
 *
 * @param session       A valid workspace session.
 * @param cursorX       Cursor X in display pixels (origin top-left).
 * @param cursorY       Cursor Y in display pixels.
 * @param outClientId   Output: clientId of the hit window, or
 *                      XR_NULL_WORKSPACE_CLIENT_ID on background miss.
 * @param outLocalUV    Output: U,V on the content quad in [0,1] range,
 *                      origin top-left. Meaningful only when
 *                      *outHitRegion == CONTENT_EXT; 0,0 otherwise.
 * @param outHitRegion  Output: classification of the hit. May be
 *                      BACKGROUND_EXT (miss), CONTENT_EXT, TITLE_BAR_EXT,
 *                      a button, an EDGE_RESIZE_* value, TASKBAR_EXT, or
 *                      LAUNCHER_TILE_EXT.
 *
 * NOTE — hardcoded MVP key policy (consumed by xrEnumerateWorkspaceInputEventsEXT
 * documentation): TAB and DELETE are consumed by the runtime; ESC is
 * consumed when any window is maximized; everything else is delivered via
 * input event AND forwarded to the focused HWND.
 */
typedef XrResult (XRAPI_PTR *PFN_xrWorkspaceHitTestEXT)(
    XrSession                session,
    int32_t                  cursorX,
    int32_t                  cursorY,
    XrWorkspaceClientId     *outClientId,
    XrVector2f              *outLocalUV,
    XrWorkspaceHitRegionEXT *outHitRegion);

// ---- Focus control (spec_version 4) ----

/*!
 * @brief Set the workspace's focused client.
 *
 * The runtime forwards keyboard input (other than runtime-reserved keys
 * TAB/DELETE/ESC) and click-through events to the focused client's HWND.
 * The workspace controller decides who gets focus from its interpretation
 * of pointer events drained via xrEnumerateWorkspaceInputEventsEXT.
 *
 * @param session   A valid workspace session.
 * @param clientId  The client to focus, or XR_NULL_WORKSPACE_CLIENT_ID
 *                  to clear focus (no client receives forwarded input).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceFocusedClientEXT)(
    XrSession           session,
    XrWorkspaceClientId clientId);

/*!
 * @brief Read the workspace's currently focused client.
 *
 * @param session       A valid workspace session.
 * @param outClientId   Output: the focused clientId, or
 *                      XR_NULL_WORKSPACE_CLIENT_ID if no client is focused.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetWorkspaceFocusedClientEXT)(
    XrSession            session,
    XrWorkspaceClientId *outClientId);

// ---- Input event drain + pointer capture (spec_version 4) ----

typedef enum XrWorkspaceInputEventTypeEXT {
    XR_WORKSPACE_INPUT_EVENT_POINTER_EXT        = 0,
    XR_WORKSPACE_INPUT_EVENT_POINTER_HOVER_EXT  = 1, // reserved; not yet emitted
    XR_WORKSPACE_INPUT_EVENT_KEY_EXT            = 2,
    XR_WORKSPACE_INPUT_EVENT_SCROLL_EXT         = 3,
    XR_WORKSPACE_INPUT_EVENT_POINTER_MOTION_EXT = 4, // spec_version 6
    XR_WORKSPACE_INPUT_EVENT_FRAME_TICK_EXT     = 5, // spec_version 6
    XR_WORKSPACE_INPUT_EVENT_FOCUS_CHANGED_EXT  = 6, // spec_version 6
    XR_WORKSPACE_INPUT_EVENT_TYPE_MAX_ENUM_EXT  = 0x7FFFFFFF
} XrWorkspaceInputEventTypeEXT;

/*!
 * @brief One input event drained from the workspace input queue.
 *
 * Tagged C union — `eventType` selects the meaningful union member. Pointer
 * events carry the workspace hit-test result (clientId / region / UV) the
 * runtime computed at drain time, so the controller does not need to call
 * xrWorkspaceHitTestEXT in the common case.
 *
 * Hardcoded MVP key policy (see xrEnumerateWorkspaceInputEventsEXT):
 *   - TAB and DELETE are consumed by the runtime (never delivered to the
 *     focused HWND, but still emitted on this queue for visibility).
 *   - ESC is consumed when any window is maximized.
 *   - Everything else is delivered via this queue AND forwarded to the
 *     focused client's HWND.
 *
 * Pointer motion events (spec_version 6) deliver per-frame WM_MOUSEMOVE while
 * pointer capture is enabled. Controllers wanting hover feedback without
 * holding a button can opt in via xrEnableWorkspacePointerCaptureEXT (any
 * button), or poll xrWorkspaceHitTestEXT directly when capture is unset.
 *
 * Frame-tick events (spec_version 6) fire once per compositor frame so a
 * controller can pace per-frame work (animation interpolation, hover effects)
 * to display refresh without polling a timer.
 *
 * Focus-changed events (spec_version 6) fire when the runtime's focused client
 * transitions (TAB cycle, click auto-focus, controller-set, client disconnect).
 * They do NOT fire on every frame when focus is stable.
 */
typedef struct XrWorkspaceInputEventEXT {
    XrWorkspaceInputEventTypeEXT eventType;
    uint32_t                     timestampMs;        // host monotonic ms (low 32 bits)
    union {
        struct {
            XrWorkspaceClientId     hitClientId;
            XrWorkspaceHitRegionEXT hitRegion;
            XrVector2f              localUV;
            int32_t                 cursorX;        // display pixels, top-left origin
            int32_t                 cursorY;
            uint32_t                button;          // 1=L, 2=R, 3=M
            XrBool32                isDown;
            uint32_t                modifiers;       // bit0=SHIFT, bit1=CTRL, bit2=ALT
        } pointer;
        struct {  // reserved for future region-transition events
            XrWorkspaceClientId     prevClientId;
            XrWorkspaceHitRegionEXT prevRegion;
            XrWorkspaceClientId     currentClientId;
            XrWorkspaceHitRegionEXT currentRegion;
        } pointerHover;
        struct {
            uint32_t                vkCode;          // Win32 VK_* (cross-platform mapping TBD)
            XrBool32                isDown;
            uint32_t                modifiers;
        } key;
        struct {
            float                   deltaY;          // wheel ticks (+ = up)
            int32_t                 cursorX;
            int32_t                 cursorY;
            uint32_t                modifiers;
        } scroll;
        struct {  // spec_version 6: per-frame mouse motion (capture-gated)
            XrWorkspaceClientId     hitClientId;
            XrWorkspaceHitRegionEXT hitRegion;
            XrVector2f              localUV;
            int32_t                 cursorX;        // display pixels, top-left origin
            int32_t                 cursorY;
            uint32_t                buttonMask;      // bit0=L, bit1=R, bit2=M (currently held)
            uint32_t                modifiers;       // bit0=SHIFT, bit1=CTRL, bit2=ALT
        } pointerMotion;
        struct {  // spec_version 6: vsync-aligned compositor frame tick
            uint64_t                timestampNs;     // host monotonic ns at frame compose
        } frameTick;
        struct {  // spec_version 6: workspace focused-client transition
            XrWorkspaceClientId     prevClientId;
            XrWorkspaceClientId     currentClientId;
        } focusChanged;
    };
} XrWorkspaceInputEventEXT;

/*!
 * @brief Drain pending workspace input events into a controller-supplied buffer.
 *
 * Up to @p capacityInput events are consumed from the workspace input queue
 * and written into @p events. The runtime returns the actual number written
 * via @p countOutput. Events are destructive — once drained, they will not
 * appear on a subsequent call.
 *
 * Maximum events per RPC is implementation-defined and bounded by
 * IPC_BUF_SIZE; pass at most 16 in @p capacityInput in the current
 * implementation. A capacity of 0 returns 0 without draining.
 *
 * @param session       A valid workspace session.
 * @param capacityInput The maximum number of events to drain.
 * @param countOutput   Output: number of events actually written.
 * @param events        Output array; may be NULL when capacityInput == 0.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateWorkspaceInputEventsEXT)(
    XrSession                  session,
    uint32_t                   capacityInput,
    uint32_t                  *countOutput,
    XrWorkspaceInputEventEXT  *events);

/*!
 * @brief Begin pointer capture so events for @p button keep flowing even when
 * the cursor leaves any window.
 *
 * Pair with xrDisableWorkspacePointerCaptureEXT to release. Used to
 * implement controller-driven drag affordances (move, resize) without the
 * runtime needing to know about drag policy.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnableWorkspacePointerCaptureEXT)(
    XrSession session,
    uint32_t  button);

typedef XrResult (XRAPI_PTR *PFN_xrDisableWorkspacePointerCaptureEXT)(
    XrSession session);

// ---- Lifecycle requests (spec_version 6) ----

/*!
 * @brief Ask the runtime to close a specific workspace client.
 *
 * Equivalent to the runtime's built-in DELETE shortcut, but targeted at any
 * client (not just the focused one). For OpenXR clients the runtime emits
 * XRT_SESSION_EVENT_EXIT_REQUEST so the client exits cleanly; for capture
 * clients the runtime tears down the capture immediately.
 *
 * The controller can drive this from its own chrome (a custom close button on
 * a window decoration, an overview gesture, etc.) without the runtime needing
 * to know about that policy.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestWorkspaceClientExitEXT)(
    XrSession            session,
    XrWorkspaceClientId  clientId);

/*!
 * @brief Ask the runtime to toggle fullscreen for a specific workspace client.
 *
 * When @p fullscreen is XR_TRUE the target window animates to fill the display
 * and other clients hide; XR_FALSE restores the prior layout. Mirrors the
 * runtime's built-in F11 shortcut, but targeted at any client.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestWorkspaceClientFullscreenEXT)(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrBool32             fullscreen);

// ---- Frame capture (spec_version 5) ----

#define XR_WORKSPACE_CAPTURE_PATH_MAX_EXT 256

typedef XrFlags64 XrWorkspaceCaptureFlagsEXT;

// Bitmask of which view variants the runtime should write. ATLAS is the
// combined side-by-side back buffer the compositor presents to the display
// processor; future flags may select per-eye images, depth, etc.
static const XrWorkspaceCaptureFlagsEXT XR_WORKSPACE_CAPTURE_FLAG_ATLAS_BIT_EXT = 0x00000001u;

/*!
 * @brief Request struct for xrCaptureWorkspaceFrameEXT.
 *
 * The runtime appends format-specific suffixes (e.g. "_atlas.png") to
 * @c pathPrefix. The IPC schema only carries POD types so the prefix is
 * an in-struct char array rather than a separately-allocated string.
 */
typedef struct XrWorkspaceCaptureRequestEXT {
    XrStructureType            type;       // XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT
    void* XR_MAY_ALIAS         next;
    char                       pathPrefix[XR_WORKSPACE_CAPTURE_PATH_MAX_EXT];
    XrWorkspaceCaptureFlagsEXT flags;
} XrWorkspaceCaptureRequestEXT;

/*!
 * @brief Result returned by xrCaptureWorkspaceFrameEXT.
 *
 * The metadata is intended to support a sidecar JSON describing the capture
 * (atlas/eye dimensions, stereo layout, display physical size, eye poses at
 * capture time).
 */
typedef struct XrWorkspaceCaptureResultEXT {
    XrStructureType            type;       // XR_TYPE_WORKSPACE_CAPTURE_RESULT_EXT
    void* XR_MAY_ALIAS         next;
    uint64_t                   timestampNs;
    uint32_t                   atlasWidth;
    uint32_t                   atlasHeight;
    uint32_t                   eyeWidth;
    uint32_t                   eyeHeight;
    XrWorkspaceCaptureFlagsEXT viewsWritten; // Subset of request flags actually written.
    uint32_t                   tileColumns;
    uint32_t                   tileRows;
    float                      displayWidthM;
    float                      displayHeightM;
    float                      eyeLeftM[3];
    float                      eyeRightM[3];
} XrWorkspaceCaptureResultEXT;

/*!
 * @brief Capture the current workspace composite frame to disk.
 *
 * The runtime reads the D3D11 composite back buffer at the next safe
 * moment, writes the requested view variants to files named after
 * @c request->pathPrefix, and fills @c result with metadata describing
 * the capture. Used by workspace controllers to implement screenshot /
 * recording features without giving them direct access to client
 * swapchains.
 *
 * @param session A valid workspace session.
 * @param request The capture request (path prefix + flags).
 * @param result  Output: capture metadata.
 */
typedef XrResult (XRAPI_PTR *PFN_xrCaptureWorkspaceFrameEXT)(
    XrSession                            session,
    const XrWorkspaceCaptureRequestEXT  *request,
    XrWorkspaceCaptureResultEXT         *result);

// ---- Client enumeration (spec_version 5) ----

/*!
 * @brief Per-client metadata returned by xrGetWorkspaceClientInfoEXT.
 *
 * Stable for the lifetime of the connection. Capture clients (adopted via
 * xrAddWorkspaceCaptureClientEXT) report their controller-supplied name;
 * OpenXR clients report the application_name they passed at xrCreateInstance.
 */
typedef struct XrWorkspaceClientInfoEXT {
    XrStructureType         type;       // XR_TYPE_WORKSPACE_CLIENT_INFO_EXT
    void* XR_MAY_ALIAS      next;
    XrWorkspaceClientId     clientId;
    XrWorkspaceClientTypeEXT clientType;
    char                    name[XR_MAX_APPLICATION_NAME_SIZE];
    uint64_t                pid;        // platform PID (DWORD on Windows, pid_t elsewhere)
    XrBool32                isFocused;
    XrBool32                isVisible;
    uint32_t                zOrder;
} XrWorkspaceClientInfoEXT;

/*!
 * @brief Enumerate workspace clients.
 *
 * Returns the OpenXR clients currently connected to the workspace. Capture
 * clients (adopted via xrAddWorkspaceCaptureClientEXT) are tracked
 * controller-side and are not returned here — the controller already knows
 * their ids since it added them.
 *
 * Standard two-call enumerate idiom: pass capacityInput=0 to get the count,
 * then pass an array sized to *countOutput on the second call.
 *
 * @param session       A valid workspace session.
 * @param capacityInput Capacity of @p clientIds; 0 for count-query.
 * @param countOutput   Output: number of clientIds available (or written).
 * @param clientIds     Output array; may be NULL when capacityInput == 0.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateWorkspaceClientsEXT)(
    XrSession            session,
    uint32_t             capacityInput,
    uint32_t            *countOutput,
    XrWorkspaceClientId *clientIds);

/*!
 * @brief Read metadata for a single workspace client.
 *
 * @param session   A valid workspace session.
 * @param clientId  The client to query.
 * @param info      Output: filled XrWorkspaceClientInfoEXT. Caller must set
 *                  info->type = XR_TYPE_WORKSPACE_CLIENT_INFO_EXT before the call.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetWorkspaceClientInfoEXT)(
    XrSession                  session,
    XrWorkspaceClientId        clientId,
    XrWorkspaceClientInfoEXT  *info);

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
    XrSession                session,
    int32_t                  cursorX,
    int32_t                  cursorY,
    XrWorkspaceClientId     *outClientId,
    XrVector2f              *outLocalUV,
    XrWorkspaceHitRegionEXT *outHitRegion);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceFocusedClientEXT(
    XrSession           session,
    XrWorkspaceClientId clientId);

XRAPI_ATTR XrResult XRAPI_CALL xrGetWorkspaceFocusedClientEXT(
    XrSession            session,
    XrWorkspaceClientId *outClientId);

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateWorkspaceInputEventsEXT(
    XrSession                  session,
    uint32_t                   capacityInput,
    uint32_t                  *countOutput,
    XrWorkspaceInputEventEXT  *events);

XRAPI_ATTR XrResult XRAPI_CALL xrEnableWorkspacePointerCaptureEXT(
    XrSession session,
    uint32_t  button);

XRAPI_ATTR XrResult XRAPI_CALL xrDisableWorkspacePointerCaptureEXT(
    XrSession session);

XRAPI_ATTR XrResult XRAPI_CALL xrRequestWorkspaceClientExitEXT(
    XrSession            session,
    XrWorkspaceClientId  clientId);

XRAPI_ATTR XrResult XRAPI_CALL xrRequestWorkspaceClientFullscreenEXT(
    XrSession            session,
    XrWorkspaceClientId  clientId,
    XrBool32             fullscreen);

XRAPI_ATTR XrResult XRAPI_CALL xrCaptureWorkspaceFrameEXT(
    XrSession                            session,
    const XrWorkspaceCaptureRequestEXT  *request,
    XrWorkspaceCaptureResultEXT         *result);

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateWorkspaceClientsEXT(
    XrSession            session,
    uint32_t             capacityInput,
    uint32_t            *countOutput,
    XrWorkspaceClientId *clientIds);

XRAPI_ATTR XrResult XRAPI_CALL xrGetWorkspaceClientInfoEXT(
    XrSession                  session,
    XrWorkspaceClientId        clientId,
    XrWorkspaceClientInfoEXT  *info);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_SPATIAL_WORKSPACE_H
