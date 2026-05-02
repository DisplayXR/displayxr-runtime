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
#define XR_EXT_spatial_workspace_SPEC_VERSION 9
#define XR_EXT_SPATIAL_WORKSPACE_EXTENSION_NAME "XR_EXT_spatial_workspace"

// Provisional XrStructureType values. The 1000999100..105 range is reserved for
// this extension; final values reconcile with the Khronos registry before spec
// freeze.
#define XR_TYPE_WORKSPACE_CLIENT_INFO_EXT                  ((XrStructureType)1000999100)
#define XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT              ((XrStructureType)1000999101)
#define XR_TYPE_WORKSPACE_CAPTURE_RESULT_EXT               ((XrStructureType)1000999102)
#define XR_TYPE_WORKSPACE_CHROME_SWAPCHAIN_CREATE_INFO_EXT ((XrStructureType)1000999103)
#define XR_TYPE_WORKSPACE_CHROME_LAYOUT_EXT                ((XrStructureType)1000999104)
#define XR_TYPE_WORKSPACE_CLIENT_STYLE_EXT                 ((XrStructureType)1000999105)

/*!
 * @brief Workspace-local identifier for a client.
 *
 * Stable for the lifetime of the connection; reused after disconnect. 0 is
 * reserved (invalid) and represented by XR_NULL_WORKSPACE_CLIENT_ID.
 */
typedef uint32_t XrWorkspaceClientId;

#define XR_NULL_WORKSPACE_CLIENT_ID ((XrWorkspaceClientId)0)

/*!
 * @brief Controller-defined opaque identifier for a chrome hit region.
 *
 * The workspace controller assigns these when populating
 * XrWorkspaceChromeLayoutEXT::hitRegions. The runtime echoes the matching
 * region's id back on POINTER / POINTER_MOTION events when a ray-cast lands
 * inside the chrome quad. 0 (XR_NULL_WORKSPACE_CHROME_REGION_ID) means "no
 * chrome region hit" — either the hit was on content / background or the
 * controller chose to leave the region anonymous.
 */
typedef uint32_t XrWorkspaceChromeRegionIdEXT;

#define XR_NULL_WORKSPACE_CHROME_REGION_ID ((XrWorkspaceChromeRegionIdEXT)0)

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
    XR_WORKSPACE_HIT_REGION_CLOSE_BUTTON_EXT      = 3,  // legacy; spec_version <= 6
    XR_WORKSPACE_HIT_REGION_MINIMIZE_BUTTON_EXT   = 4,  // legacy; spec_version <= 6
    XR_WORKSPACE_HIT_REGION_MAXIMIZE_BUTTON_EXT   = 5,  // legacy; spec_version <= 6
    XR_WORKSPACE_HIT_REGION_CHROME_EXT            = 6,  // controller-owned chrome (spec_version 7)
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
    XR_WORKSPACE_INPUT_EVENT_POINTER_HOVER_EXT  = 1, // spec_version 7: emitted on hovered-slot transitions (per-frame raycast — fires regardless of pointer-capture state)
    XR_WORKSPACE_INPUT_EVENT_KEY_EXT            = 2,
    XR_WORKSPACE_INPUT_EVENT_SCROLL_EXT         = 3,
    XR_WORKSPACE_INPUT_EVENT_POINTER_MOTION_EXT = 4, // spec_version 6
    XR_WORKSPACE_INPUT_EVENT_FRAME_TICK_EXT     = 5, // spec_version 6
    XR_WORKSPACE_INPUT_EVENT_FOCUS_CHANGED_EXT  = 6, // spec_version 6
    XR_WORKSPACE_INPUT_EVENT_WINDOW_POSE_CHANGED_EXT = 7, // spec_version 8: runtime-driven pose / size change (edge resize, etc.)
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
            XrWorkspaceClientId          hitClientId;
            XrWorkspaceHitRegionEXT      hitRegion;
            XrVector2f                   localUV;
            int32_t                      cursorX;          // display pixels, top-left origin
            int32_t                      cursorY;
            uint32_t                     button;            // 1=L, 2=R, 3=M
            XrBool32                     isDown;
            uint32_t                     modifiers;         // bit0=SHIFT, bit1=CTRL, bit2=ALT
            XrWorkspaceChromeRegionIdEXT chromeRegionId;    // spec_version 7: controller-defined region within chrome quad, 0 if none
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
            XrWorkspaceClientId          hitClientId;
            XrWorkspaceHitRegionEXT      hitRegion;
            XrVector2f                   localUV;
            int32_t                      cursorX;          // display pixels, top-left origin
            int32_t                      cursorY;
            uint32_t                     buttonMask;        // bit0=L, bit1=R, bit2=M (currently held)
            uint32_t                     modifiers;         // bit0=SHIFT, bit1=CTRL, bit2=ALT
            XrWorkspaceChromeRegionIdEXT chromeRegionId;    // spec_version 7: controller-defined region within chrome quad, 0 if none
        } pointerMotion;
        struct {  // spec_version 6: vsync-aligned compositor frame tick
            uint64_t                timestampNs;     // host monotonic ns at frame compose
        } frameTick;
        struct {  // spec_version 6: workspace focused-client transition
            XrWorkspaceClientId     prevClientId;
            XrWorkspaceClientId     currentClientId;
        } focusChanged;
        struct {  // spec_version 8: window pose/size changed by the runtime
            // (edge resize, fullscreen toggle, etc. — NOT shell-driven
            // set_pose RPCs, which the shell already knows about).
            // Controllers re-push chrome layout in response so chrome
            // tracks the window's new top edge instead of staying at the
            // old size.
            XrWorkspaceClientId     clientId;
            XrPosef                 pose;
            float                   widthMeters;
            float                   heightMeters;
        } windowPoseChanged;
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

// ---- Controller-owned chrome (spec_version 7) ----

/*!
 * @brief Maximum hit regions a controller can register per chrome layout.
 *
 * Fixed-size cap so the IPC wire form stays POD.
 */
#define XR_WORKSPACE_CHROME_MAX_HIT_REGIONS_EXT 8

/*!
 * @brief Create-info for xrCreateWorkspaceClientChromeSwapchainEXT.
 *
 * Describes the 2D chrome image the controller will draw to. The runtime
 * mints a swapchain whose images are cross-process-shareable (D3D11
 * KEYEDMUTEX + NTHANDLE) so the controller's D3D11 device can author the
 * pixels in its own process. Format must be a 2D color format the runtime
 * supports for blits (today: DXGI_FORMAT_R8G8B8A8_UNORM_SRGB).
 */
typedef struct XrWorkspaceChromeSwapchainCreateInfoEXT {
    XrStructureType       type;        // XR_TYPE_WORKSPACE_CHROME_SWAPCHAIN_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS next;
    int64_t               format;       // graphics-API native format (e.g. DXGI_FORMAT)
    uint32_t              width;
    uint32_t              height;
    uint32_t              sampleCount;  // 1 (no MSAA) for the pill chrome
    uint32_t              mipCount;     // 1
} XrWorkspaceChromeSwapchainCreateInfoEXT;

/*!
 * @brief One controller-defined hit region inside a chrome quad.
 *
 * The runtime ray-casts the chrome quad and looks up the first region whose
 * UV bounds contain the hit point. Region @c id is echoed back on POINTER /
 * POINTER_MOTION events as @c chromeRegionId — opaque to the runtime, the
 * controller decides what each id means (grip, close, minimize, …).
 */
typedef struct XrWorkspaceChromeHitRegionEXT {
    XrWorkspaceChromeRegionIdEXT id;       // controller-defined; 0 = no region
    XrRect2Df                    bounds;   // chrome-UV space [0,1]^2; offset = top-left, extent = size
} XrWorkspaceChromeHitRegionEXT;

/*!
 * @brief Layout for a controller-submitted chrome quad.
 *
 * The controller calls xrSetWorkspaceClientChromeLayoutEXT once on connect
 * and on every layout change (preset switch, resize). The runtime caches the
 * layout per client and applies it every render. Per-frame IPC is not needed.
 *
 * @c poseInClient is the chrome quad's pose relative to the client window's
 * own pose — typically {orient = identity, position = (0, +window_h/2 + gap +
 * pill_h/2, 0)} for the floating-pill design. @c sizeMeters is the quad's
 * physical extent.
 *
 * If @c followsWindowOrient is XR_TRUE the chrome rotates with the window
 * (useful for the carousel preset). XR_FALSE keeps it axis-aligned to the
 * display regardless of window orientation.
 *
 * @c depthBiasMeters biases the chrome quad toward the eye in NDC; default
 * 0.001 m matches the runtime's prior pill-over-content depth bias. 0 means
 * "use runtime default."
 */
typedef struct XrWorkspaceChromeLayoutEXT {
    XrStructureType  type;        // XR_TYPE_WORKSPACE_CHROME_LAYOUT_EXT
    const void* XR_MAY_ALIAS next;
    XrPosef          poseInClient;
    XrExtent2Df      sizeMeters;
    XrBool32         followsWindowOrient;
    uint32_t         hitRegionCount;     // <= XR_WORKSPACE_CHROME_MAX_HIT_REGIONS_EXT
    const XrWorkspaceChromeHitRegionEXT* hitRegions;
    float            depthBiasMeters;    // 0 = runtime default (0.001)
    // spec_version 8: auto-anchor flags so chrome stays attached to a window
    // edge as the window resizes WITHOUT the controller having to re-push the
    // layout on every WINDOW_POSE_CHANGED event. Without these the chrome
    // appears to lag one frame behind the window edge during a resize drag,
    // because the controller's push_layout always carries the win_h_m value
    // from the LAST frame the controller observed.
    //   anchorToWindowTopEdge: when XR_TRUE, poseInClient.position.y is
    //     interpreted as an offset ABOVE the window's top edge (positive =
    //     above) instead of from the window center. Runtime evaluates
    //     effectiveY = window_h/2 + poseInClient.position.y per frame using
    //     the CURRENT window height — chrome stays glued to top edge.
    //   widthAsFractionOfWindow: when > 0, chrome width is computed each
    //     frame as window_w * widthAsFractionOfWindow (sizeMeters.width is
    //     ignored). 0 = absolute (use sizeMeters.width).
    XrBool32         anchorToWindowTopEdge;
    float            widthAsFractionOfWindow;
} XrWorkspaceChromeLayoutEXT;

/*!
 * @brief Create a chrome swapchain for a workspace client.
 *
 * The runtime creates a cross-process-shareable image-loop swapchain
 * dedicated to chrome rendering for the given client. The swapchain returned
 * is a normal XrSwapchain — Acquire / Wait / Release operate as usual; the
 * runtime tracks the magic handle internally so it composites the released
 * image onto the workspace at the layout-specified pose.
 *
 * Most controllers create one chrome swapchain per client they decorate. The
 * shell may filter out itself (its own session) since the controller does
 * not decorate its own session.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace,
 *         XR_ERROR_FUNCTION_UNSUPPORTED if the extension was not enabled.
 */
typedef XrResult (XRAPI_PTR *PFN_xrCreateWorkspaceClientChromeSwapchainEXT)(
    XrSession                                       session,
    XrWorkspaceClientId                             clientId,
    const XrWorkspaceChromeSwapchainCreateInfoEXT  *createInfo,
    XrSwapchain                                    *swapchain);

/*!
 * @brief Destroy a chrome swapchain.
 *
 * Equivalent to xrDestroySwapchain for a chrome swapchain — the runtime
 * stops compositing its image and releases the underlying texture. The
 * client's window stays visible (just without chrome) until a new chrome
 * swapchain is submitted.
 */
typedef XrResult (XRAPI_PTR *PFN_xrDestroyWorkspaceClientChromeSwapchainEXT)(
    XrSwapchain swapchain);

/*!
 * @brief Set / update the layout of a client's chrome quad.
 *
 * Layout is cached per client and re-applied every render. Call once on
 * connect after creating the chrome swapchain, and again whenever the
 * controller changes the quad's pose, size, hit regions, or depth bias.
 *
 * Calling this with a clientId that has no chrome swapchain stores the
 * layout for later — when the controller later creates a chrome swapchain
 * for that client, the cached layout takes effect.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_VALIDATION_FAILURE if hitRegionCount exceeds
 *         XR_WORKSPACE_CHROME_MAX_HIT_REGIONS_EXT,
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientChromeLayoutEXT)(
    XrSession                          session,
    XrWorkspaceClientId                clientId,
    const XrWorkspaceChromeLayoutEXT  *layout);

// ---- Event-driven wakeup (spec_version 8) ----

/*!
 * @brief Acquire a wakeup event the controller can wait on instead of polling.
 *
 * Returns a platform-native event handle that the runtime signals whenever
 * async workspace state changes (input event pushed onto the drain queue,
 * focused-slot transition, hovered-slot transition). The controller passes
 * the handle to a wait primitive (Win32 WaitForSingleObject /
 * MsgWaitForMultipleObjects, or analogous on other platforms) so its main
 * loop sleeps when nothing is happening and wakes immediately when there
 * is something to drain.
 *
 * Auto-reset semantics: the handle becomes signaled on each runtime
 * SetEvent and is cleared automatically when one waiter wakes. Multiple
 * SetEvent calls in quick succession may collapse to a single wake — the
 * controller is expected to drain ALL pending state on each wake (call
 * xrEnumerateWorkspaceInputEventsEXT, re-query whatever it cares about).
 *
 * The handle is a Win32 HANDLE on Windows, returned as @p outNativeHandle
 * cast to uint64_t. Caller takes ownership and must close (CloseHandle)
 * when done. Calling this function multiple times returns fresh handles
 * each time; the runtime keeps a single source-of-truth event internally
 * and duplicates it to the caller's process at each call.
 *
 * Platforms other than Windows currently return XR_ERROR_FEATURE_UNSUPPORTED.
 *
 * Idle CPU cost goes from ~0.1% of one core (the polling baseline) to
 * effectively 0 when this handle is wired into the controller's wait set.
 *
 * @param session            A valid workspace session.
 * @param outNativeHandle    Output: platform-native event handle as uint64_t.
 *                           NULL on error.
 */
typedef XrResult (XRAPI_PTR *PFN_xrAcquireWorkspaceWakeupEventEXT)(
    XrSession  session,
    uint64_t  *outNativeHandle);

// ---- Per-client visual style (spec_version 9) ----

/*!
 * @brief Per-client visual style applied at workspace content blit time.
 *
 * The runtime is a parameterized composite engine; this struct is the
 * controller's surface for adjusting the existing shader knobs (corner
 * radius, edge feather, focus glow). The runtime owns mechanism (sampling
 * the client's swapchain, applying the alpha mask, compositing onto the
 * atlas); the controller owns appearance by pushing values into this
 * struct. New visual treatments add new fields here additively over time
 * — controllers that don't know about a new field see it default to a
 * runtime-defined sane value.
 *
 * Coordinate system: @c edgeFeatherMeters and @c focusGlowFalloffMeters
 * are measured in physical display space — independent of window size.
 * The same 3 mm feather reads identically on a small or large window.
 *
 * Set per-client via xrSetWorkspaceClientStyleEXT. Runtime caches the
 * style per client and applies every render. Pass a zero-initialized
 * style (or NULL) to reset to runtime defaults.
 */
typedef struct XrWorkspaceClientStyleEXT {
    XrStructureType       type;        // XR_TYPE_WORKSPACE_CLIENT_STYLE_EXT
    const void* XR_MAY_ALIAS next;

    // Edge geometry. cornerRadius is a fraction of window HEIGHT (0..1).
    // 0 = sharp corners. Typical values 0.02..0.08 read as soft / deliberate
    // rounding. Always rounds all four corners. Negative values reserved
    // for future "top-only" / "bottom-only" semantics; treat as 0 today.
    float cornerRadius;

    // Soft alpha falloff at the perimeter (rounded square + edges) in
    // METERS. 0 = crisp pixel-perfect edge. Positive values fade the
    // alpha toward 0 over this physical width — reads as a subtle
    // ambient softening on every window. Always applied (focused or
    // not). Typical values 0.001..0.004 m (1-4 mm).
    float edgeFeatherMeters;

    // Focus glow. Active only when this client is the focused workspace
    // client (per xrSetWorkspaceFocusedClientEXT). Ignored when the
    // client is not focused — no effect on unfocused windows.
    //
    //   focusGlowColor       RGBA. Alpha is the peak opacity at the
    //                        inner edge of the glow band. Premultiplied
    //                        is fine; the runtime composites linearly.
    //   focusGlowIntensity   Scalar multiplier on color.a. 0 disables
    //                        the glow even when focused; ~0.6..1.0
    //                        reads as a clear focus indicator.
    //   focusGlowFalloffMeters
    //                        Distance from the rounded-square perimeter
    //                        where the glow's alpha falls off. ~5-15
    //                        mm reads as a soft halo. 0 disables.
    float focusGlowColor[4];
    float focusGlowIntensity;
    float focusGlowFalloffMeters;
} XrWorkspaceClientStyleEXT;

/*!
 * @brief Set / update the per-client visual style.
 *
 * Cached per client; applied every render. Call once on connect after
 * adding the client to the workspace, and again whenever the controller
 * changes the visual treatment (preset switch, theme change, focus
 * indicator scheme). Calling with a clientId that has no live slot
 * stores the style for later — when the slot binds, the cached style
 * takes effect.
 *
 * Pass @p style = NULL to reset that client's style to runtime defaults.
 *
 * @return XR_SUCCESS on success,
 *         XR_ERROR_HANDLE_INVALID if @p clientId is unknown,
 *         XR_ERROR_VALIDATION_FAILURE if any numeric field is non-finite
 *         or negative where required (cornerRadius, edgeFeatherMeters,
 *         focusGlowIntensity, focusGlowFalloffMeters all >= 0),
 *         XR_ERROR_FEATURE_UNSUPPORTED if @p session is not the active
 *         workspace.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientStyleEXT)(
    XrSession                              session,
    XrWorkspaceClientId                    clientId,
    const XrWorkspaceClientStyleEXT       *style);

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

XRAPI_ATTR XrResult XRAPI_CALL xrCreateWorkspaceClientChromeSwapchainEXT(
    XrSession                                       session,
    XrWorkspaceClientId                             clientId,
    const XrWorkspaceChromeSwapchainCreateInfoEXT  *createInfo,
    XrSwapchain                                    *swapchain);

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyWorkspaceClientChromeSwapchainEXT(
    XrSwapchain swapchain);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientChromeLayoutEXT(
    XrSession                          session,
    XrWorkspaceClientId                clientId,
    const XrWorkspaceChromeLayoutEXT  *layout);

XRAPI_ATTR XrResult XRAPI_CALL xrAcquireWorkspaceWakeupEventEXT(
    XrSession  session,
    uint64_t  *outNativeHandle);

XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientStyleEXT(
    XrSession                              session,
    XrWorkspaceClientId                    clientId,
    const XrWorkspaceClientStyleEXT       *style);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_SPATIAL_WORKSPACE_H
