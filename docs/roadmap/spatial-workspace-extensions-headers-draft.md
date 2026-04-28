# Workspace Extensions — Header Sketch (draft)

**Status:** Draft v0, 2026-04-27. Branch `feature/shell-brand-separation`. Not frozen.

Companion to [spatial-workspace-extensions-plan.md](spatial-workspace-extensions-plan.md). That doc explains *why* the runtime needs neutral workspace primitives; this doc proposes *what* the C-level API surface looks like — full enough that the next implementer (or reviewer) can argue with specifics rather than prose.

## What this draft is for

Phase 2 moves policy out of `comp_d3d11_service.cpp` (launcher registry, layout-preset semantics, chrome rendering, capture lifecycle, ESC/empty-workspace logic) into the workspace controller process. Each policy migration removes runtime code **and** adds extension API surface — the workspace controller calls the extension instead of the runtime owning the policy.

Without a header sketch first, we'd design the API one migration at a time and end up with an inconsistent surface. Sketching now lets us:
- Pick names that work across all migrations (e.g., the same `XrWorkspaceClientId` is used by hit-test, capture, pose-set, visibility, etc.).
- Decide what's two extensions versus one.
- Settle the activation handshake before writing any of it.
- Spec what a non-DisplayXR-Shell workspace controller actually has to call to bootstrap.

It is *not* a frozen specification. SPEC_VERSION 1 freezes when the first non-shell consumer (a vertical workspace, a kiosk, an OEM controller, or our own test harness) successfully drives the runtime end-to-end.

## Naming and scope

Two extensions, both prefixed `XR_EXT_` to match the existing DisplayXR extensions (`XR_EXT_display_info`, `XR_EXT_win32_window_binding`, etc.). The `EXT_` prefix signals "any DisplayXR-compatible runtime should implement", same as the others — not single-vendor `XR_DISPLAYXR_`.

| Extension | Purpose |
|---|---|
| `XR_EXT_spatial_workspace` | Privileged client opts into workspace mode; manages window poses, visibility, hit-test, 2D capture clients, frame capture, client enumeration, lifecycle events. |
| `XR_EXT_app_launcher` | Tile registry rendered by the runtime over the atlas. Workspace pushes tiles, polls click events, sets the running-tile mask. Optional second extension — see "Why two extensions" below. |

### Why two extensions

Could be one. Keeping them separate buys two things:

1. **Independent versioning.** Launcher UX is most likely to evolve (we may want richer tile metadata, glow states, drag-to-reorder, group tabs). The workspace surface should stabilize first; the launcher can iterate without bumping `XR_EXT_spatial_workspace_SPEC_VERSION`.
2. **A non-launcher controller is a real use case.** A medical cockpit, a kiosk, or a test harness wants `spatial_workspace` but doesn't need a launcher. Two extensions = the controller declares only what it uses; the runtime can run a workspace without instantiating launcher state.

Counter-argument: more surface area to keep coherent. Mitigated by the fact that a workspace controller almost always uses both, and they share the lifecycle (launcher commands no-op when no workspace is active).

We start with two and merge if the boundary feels artificial after Phase 2 is in real use.

## Activation handshake

The workspace controller is a regular OpenXR client that opts into privileged mode. Bootstrap:

1. Process launches; binary path is whatever `service.json::workspace_binary` points at (defaults to `displayxr-shell.exe`, configurable per Phase 1).
2. Process calls `xrCreateInstance` with `XR_EXT_SPATIAL_WORKSPACE_EXTENSION_NAME` in `enabledExtensionNames` (and optionally `XR_EXT_APP_LAUNCHER_EXTENSION_NAME`).
3. Process creates a session normally (graphics binding can be `XR_NULL_HANDLE` — workspace controllers don't render swapchains; they only instruct).
4. Process calls `xrActivateSpatialWorkspaceEXT(session)`. The runtime checks:
   - At most one workspace is active per system. Returns `XR_ERROR_LIMIT_REACHED` if another controller already holds the role.
   - Caller authorization: in the current implementation the runtime checks `application_name == config.workspace_app_name` (defaults to `"displayxr-shell"`). Phase 2 promotes this to a proper handshake — see "Open questions". For now, the simple match is enough to gate the privileged surface.
5. On success the session is *the* workspace session. All subsequent extension calls go through this `XrSession` handle.

Lifecycle:
- `xrDeactivateSpatialWorkspaceEXT(session)` — voluntarily release the role. Other clients keep running; per-client compositors resume direct rendering.
- `xrDestroySession` on the workspace session — implicit deactivate.
- Workspace process crash — service detects via IPC pipe close and treats as deactivate.

A non-workspace OpenXR session has *no access* to any of the functions below; calling them returns `XR_ERROR_FEATURE_UNSUPPORTED` (extension not enabled) or `XR_ERROR_VALIDATION_FAILURE` (extension enabled but session is not the active workspace).

## Header sketch — `XR_EXT_spatial_workspace.h`

```c
// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
#ifndef XR_EXT_SPATIAL_WORKSPACE_H
#define XR_EXT_SPATIAL_WORKSPACE_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_spatial_workspace 1
#define XR_EXT_spatial_workspace_SPEC_VERSION 1
#define XR_EXT_SPATIAL_WORKSPACE_EXTENSION_NAME "XR_EXT_spatial_workspace"

// ---- Type values (provisional — coordinate with extension registry) ----
#define XR_TYPE_WORKSPACE_CLIENT_INFO_EXT          ((XrStructureType)1000999100)
#define XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT      ((XrStructureType)1000999101)
#define XR_TYPE_WORKSPACE_CAPTURE_RESULT_EXT       ((XrStructureType)1000999102)
#define XR_TYPE_EVENT_DATA_WORKSPACE_CLIENT_CONNECTED_EXT    ((XrStructureType)1000999103)
#define XR_TYPE_EVENT_DATA_WORKSPACE_CLIENT_DISCONNECTED_EXT ((XrStructureType)1000999104)

// Workspace-local identifier for a client. Stable for the lifetime of the
// connection; reused after disconnect. 0 is reserved (invalid).
typedef uint32_t XrWorkspaceClientId;

#define XR_NULL_WORKSPACE_CLIENT_ID ((XrWorkspaceClientId)0)

// ---- Lifecycle ----

/*!
 * Activate workspace mode on this session. The session becomes the privileged
 * workspace controller. At most one workspace is active per system.
 *
 * Returns:
 *   XR_SUCCESS                    Activated.
 *   XR_ERROR_LIMIT_REACHED        Another workspace is already active.
 *   XR_ERROR_FEATURE_UNSUPPORTED  Caller is not authorized as a workspace
 *                                 controller (see "Activation handshake").
 *   XR_ERROR_FUNCTION_UNSUPPORTED Extension was not enabled at instance create.
 */
typedef XrResult (XRAPI_PTR *PFN_xrActivateSpatialWorkspaceEXT)(XrSession session);

/*!
 * Voluntarily release workspace role. Other clients keep running.
 */
typedef XrResult (XRAPI_PTR *PFN_xrDeactivateSpatialWorkspaceEXT)(XrSession session);

/*!
 * Query whether this session is currently the active workspace controller.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetSpatialWorkspaceStateEXT)(
    XrSession session, XrBool32 *out_active);


// ---- Client enumeration ----

typedef enum XrWorkspaceClientTypeEXT {
    XR_WORKSPACE_CLIENT_TYPE_OPENXR_3D_EXT  = 0,  // a regular OpenXR app session
    XR_WORKSPACE_CLIENT_TYPE_CAPTURED_2D_EXT = 1, // a 2D OS window adopted via xrAddWorkspaceCaptureClientEXT
    XR_WORKSPACE_CLIENT_TYPE_MAX_ENUM_EXT   = 0x7FFFFFFF
} XrWorkspaceClientTypeEXT;

/*!
 * Per-client metadata. Returned by xrEnumerateWorkspaceClientsEXT.
 */
typedef struct XrWorkspaceClientInfoEXT {
    XrStructureType             type;        //!< XR_TYPE_WORKSPACE_CLIENT_INFO_EXT
    void* XR_MAY_ALIAS          next;
    XrWorkspaceClientId         clientId;
    XrWorkspaceClientTypeEXT    clientType;
    char                        applicationName[XR_MAX_APPLICATION_NAME_SIZE];
    char                        processName[XR_MAX_SYSTEM_NAME_SIZE]; //!< OS process name (best-effort)
    XrPosef                     pose;        //!< Current window pose in display space
    XrExtent2Df                 sizeMeters;  //!< Current width/height in meters
    XrBool32                    visible;     //!< True if not minimized
    XrBool32                    focused;     //!< True if input is currently routed to this client
} XrWorkspaceClientInfoEXT;

/*!
 * Standard two-call enumerate. First call with capacityInput=0 gets count;
 * second call with allocated array gets details.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateWorkspaceClientsEXT)(
    XrSession                   session,
    uint32_t                    clientCapacityInput,
    uint32_t                   *clientCountOutput,
    XrWorkspaceClientInfoEXT   *clients);


// ---- Window pose ----

/*!
 * Set position, orientation, and physical size of a client's window in display
 * space. The runtime composites that client's swapchain into a quad at this
 * pose; on the next frame the result is visible.
 *
 * pose origin is the display center; +x right, +y up, +z toward the viewer.
 * Sizes are physical (meters). Width/height are the quad's extent — the
 * runtime stretches the client's atlas to fit.
 *
 * Returns XR_ERROR_VALIDATION_FAILURE if clientId is unknown or not a
 * positionable client (some platform-internal clients may be excluded).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientWindowPoseEXT)(
    XrSession           session,
    XrWorkspaceClientId clientId,
    const XrPosef      *pose,
    float               widthMeters,
    float               heightMeters);

typedef XrResult (XRAPI_PTR *PFN_xrGetWorkspaceClientWindowPoseEXT)(
    XrSession           session,
    XrWorkspaceClientId clientId,
    XrPosef            *outPose,
    float              *outWidthMeters,
    float              *outHeightMeters);

/*!
 * Show or hide a client's window without destroying it. A hidden client keeps
 * running but does not contribute to the composite (minimize semantics).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientVisibilityEXT)(
    XrSession           session,
    XrWorkspaceClientId clientId,
    XrBool32            visible);


// ---- Capture clients (adopt a 2D OS window) ----

/*!
 * Adopt a 2D OS window as a workspace client. The runtime starts a platform-
 * appropriate capture (Windows.Graphics.Capture on Windows; CGDisplayCreate-
 * Image / SCStream on macOS) and treats the captured texture as a client
 * swapchain — the workspace can position/hide it like any other client.
 *
 * nativeWindow is platform-defined: HWND on Windows passed as uint64_t, NSView*
 * on macOS via chained struct (see XR_EXT_cocoa_window_binding for the chain
 * pattern). For now we ship Windows-only and pass HWND as uint64_t directly;
 * macOS support adds a chained XrWorkspaceCocoaCaptureBindingEXT struct.
 *
 * The returned clientId enters the same numbering space as OpenXR clients —
 * the workspace controller cannot tell them apart from xrEnumerate's result
 * except via XrWorkspaceClientTypeEXT.
 */
typedef XrResult (XRAPI_PTR *PFN_xrAddWorkspaceCaptureClientEXT)(
    XrSession            session,
    uint64_t             nativeWindow,    // Windows: HWND. macOS: 0 + chained binding.
    const char          *nameOptional,    // user-visible label, may be NULL
    XrWorkspaceClientId *outClientId);

typedef XrResult (XRAPI_PTR *PFN_xrRemoveWorkspaceCaptureClientEXT)(
    XrSession           session,
    XrWorkspaceClientId clientId);


// ---- Hit-test ----

/*!
 * Given a screen-space cursor position (display pixels, origin top-left),
 * raycast through the workspace and return which client window was hit and
 * where on its surface in normalized UV (0..1, origin top-left of the window
 * quad).
 *
 * Returns:
 *   XR_SUCCESS — outClientId valid (XR_NULL_WORKSPACE_CLIENT_ID = miss)
 *
 * The workspace controller calls this from its input handler, then decides
 * what the hit means (focus, drag, right-click forward, title-bar grab,
 * resize-edge etc.). The runtime does NOT classify hits — that is policy.
 */
typedef XrResult (XRAPI_PTR *PFN_xrWorkspaceHitTestEXT)(
    XrSession            session,
    int32_t              cursorX,
    int32_t              cursorY,
    XrWorkspaceClientId *outClientId,
    XrVector2f          *outLocalUV);


// ---- Frame capture ----

#define XR_WORKSPACE_CAPTURE_FLAG_LEFT_VIEW_BIT_EXT   0x00000001
#define XR_WORKSPACE_CAPTURE_FLAG_RIGHT_VIEW_BIT_EXT  0x00000002
#define XR_WORKSPACE_CAPTURE_FLAG_ATLAS_BIT_EXT       0x00000004
#define XR_WORKSPACE_CAPTURE_FLAG_INCLUDE_METADATA_BIT_EXT  0x00000008
typedef XrFlags64 XrWorkspaceCaptureFlagsEXT;

typedef struct XrWorkspaceCaptureRequestEXT {
    XrStructureType                type;        //!< XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT
    const void* XR_MAY_ALIAS       next;
    char                           pathPrefix[XR_MAX_PATH_LENGTH]; //!< filename prefix without extension
    XrWorkspaceCaptureFlagsEXT     flags;
} XrWorkspaceCaptureRequestEXT;

typedef struct XrWorkspaceCaptureResultEXT {
    XrStructureType                type;        //!< XR_TYPE_WORKSPACE_CAPTURE_RESULT_EXT
    void* XR_MAY_ALIAS             next;
    uint32_t                       viewsWritten;
    uint32_t                       atlasWidthPixels;
    uint32_t                       atlasHeightPixels;
    char                           atlasPath[XR_MAX_PATH_LENGTH];
    char                           metadataJsonPath[XR_MAX_PATH_LENGTH]; //!< empty if not requested
} XrWorkspaceCaptureResultEXT;

/*!
 * Capture the current pre-weave atlas to disk. The runtime writes one or more
 * PNGs and an optional JSON sidecar with per-client window bounding boxes.
 * Synchronous within the workspace render thread.
 */
typedef XrResult (XRAPI_PTR *PFN_xrCaptureWorkspaceFrameEXT)(
    XrSession                            session,
    const XrWorkspaceCaptureRequestEXT  *request,
    XrWorkspaceCaptureResultEXT         *outResult);


// ---- Events ----

typedef struct XrEventDataWorkspaceClientConnectedEXT {
    XrStructureType             type;       //!< XR_TYPE_EVENT_DATA_WORKSPACE_CLIENT_CONNECTED_EXT
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;    //!< the workspace session
    XrWorkspaceClientId         clientId;
    XrWorkspaceClientTypeEXT    clientType;
} XrEventDataWorkspaceClientConnectedEXT;

typedef struct XrEventDataWorkspaceClientDisconnectedEXT {
    XrStructureType             type;       //!< XR_TYPE_EVENT_DATA_WORKSPACE_CLIENT_DISCONNECTED_EXT
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;
    XrWorkspaceClientId         clientId;
} XrEventDataWorkspaceClientDisconnectedEXT;


#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrActivateSpatialWorkspaceEXT(XrSession);
XRAPI_ATTR XrResult XRAPI_CALL xrDeactivateSpatialWorkspaceEXT(XrSession);
XRAPI_ATTR XrResult XRAPI_CALL xrGetSpatialWorkspaceStateEXT(XrSession, XrBool32*);
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateWorkspaceClientsEXT(
    XrSession, uint32_t, uint32_t*, XrWorkspaceClientInfoEXT*);
XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientWindowPoseEXT(
    XrSession, XrWorkspaceClientId, const XrPosef*, float, float);
XRAPI_ATTR XrResult XRAPI_CALL xrGetWorkspaceClientWindowPoseEXT(
    XrSession, XrWorkspaceClientId, XrPosef*, float*, float*);
XRAPI_ATTR XrResult XRAPI_CALL xrSetWorkspaceClientVisibilityEXT(
    XrSession, XrWorkspaceClientId, XrBool32);
XRAPI_ATTR XrResult XRAPI_CALL xrAddWorkspaceCaptureClientEXT(
    XrSession, uint64_t, const char*, XrWorkspaceClientId*);
XRAPI_ATTR XrResult XRAPI_CALL xrRemoveWorkspaceCaptureClientEXT(
    XrSession, XrWorkspaceClientId);
XRAPI_ATTR XrResult XRAPI_CALL xrWorkspaceHitTestEXT(
    XrSession, int32_t, int32_t, XrWorkspaceClientId*, XrVector2f*);
XRAPI_ATTR XrResult XRAPI_CALL xrCaptureWorkspaceFrameEXT(
    XrSession, const XrWorkspaceCaptureRequestEXT*, XrWorkspaceCaptureResultEXT*);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_SPATIAL_WORKSPACE_H
```

## Header sketch — `XR_EXT_app_launcher.h`

```c
// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
#ifndef XR_EXT_APP_LAUNCHER_H
#define XR_EXT_APP_LAUNCHER_H 1

#include <openxr/openxr.h>
#include <openxr/XR_EXT_spatial_workspace.h>  // for XrWorkspaceClientId — not strictly required, but launcher state is workspace-scoped

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_app_launcher 1
#define XR_EXT_app_launcher_SPEC_VERSION 1
#define XR_EXT_APP_LAUNCHER_EXTENSION_NAME "XR_EXT_app_launcher"

#define XR_TYPE_LAUNCHER_APP_INFO_EXT       ((XrStructureType)1000999110)
#define XR_TYPE_EVENT_DATA_LAUNCHER_CLICK_EXT ((XrStructureType)1000999111)

#define XR_LAUNCHER_MAX_APPS_EXT 64
#define XR_LAUNCHER_INVALID_TILE_INDEX_EXT ((int32_t)-1)

/*!
 * One launcher tile. The runtime renders an icon + label; the workspace
 * controller decides what clicking means (typically: launch the named binary).
 */
typedef struct XrLauncherAppInfoEXT {
    XrStructureType             type;       //!< XR_TYPE_LAUNCHER_APP_INFO_EXT
    void* XR_MAY_ALIAS          next;
    char                        name[XR_MAX_APPLICATION_NAME_SIZE];
    char                        iconPath[XR_MAX_PATH_LENGTH];   //!< absolute path to a PNG; runtime loads + caches
    uint32_t                    appIndex;                       //!< caller-defined opaque id; returned in click events
} XrLauncherAppInfoEXT;


// ---- Tile registry ----

/*!
 * Empty the launcher's tile list. Called by the workspace controller at the
 * start of each registry push (clear-then-add-N pattern keeps each call's
 * payload below the IPC message buffer cap and makes registry diffs trivial).
 */
typedef XrResult (XRAPI_PTR *PFN_xrClearLauncherAppsEXT)(XrSession session);

/*!
 * Append one tile. Silently ignored if the registry is full (max
 * XR_LAUNCHER_MAX_APPS_EXT entries). The workspace controller is expected to
 * push a clear+add-N sequence whenever its registered-app set changes.
 */
typedef XrResult (XRAPI_PTR *PFN_xrAddLauncherAppEXT)(
    XrSession                       session,
    const XrLauncherAppInfoEXT     *app);


// ---- Visibility ----

/*!
 * Show or hide the launcher panel. The runtime renders the tile grid as a
 * compositor overlay at zero-disparity (z=0 in display space).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetLauncherVisibleEXT)(
    XrSession session, XrBool32 visible);


// ---- Click polling ----

/*!
 * Poll-and-clear the most recent unconsumed tile click. Returns the appIndex
 * the user clicked, or XR_LAUNCHER_INVALID_TILE_INDEX_EXT if none.
 *
 * Workspace controllers typically poll once per main-loop iteration. On a hit,
 * the controller looks up the app in its own registry (the one it pushed via
 * xrAddLauncherAppEXT) and launches the corresponding binary.
 *
 * Alternative shape under consideration: push as XrEventDataLauncherClickEXT
 * via xrPollEvent. Decision deferred — see "Open questions".
 */
typedef XrResult (XRAPI_PTR *PFN_xrPollLauncherClickEXT)(
    XrSession   session,
    int32_t    *outAppIndex);


// ---- Running indicator ----

/*!
 * Set bitmask of tiles whose corresponding apps are currently running. The
 * runtime draws a glow border on tiles whose bit is set so the user can see
 * which apps are open. The workspace controller pushes this whenever its
 * computed running set changes.
 *
 * Bit i corresponds to the tile whose appIndex was i (so the workspace's
 * appIndex assignment defines the bitmask layout).
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetLauncherRunningTileMaskEXT)(
    XrSession session,
    uint64_t  mask);


#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrClearLauncherAppsEXT(XrSession);
XRAPI_ATTR XrResult XRAPI_CALL xrAddLauncherAppEXT(XrSession, const XrLauncherAppInfoEXT*);
XRAPI_ATTR XrResult XRAPI_CALL xrSetLauncherVisibleEXT(XrSession, XrBool32);
XRAPI_ATTR XrResult XRAPI_CALL xrPollLauncherClickEXT(XrSession, int32_t*);
XRAPI_ATTR XrResult XRAPI_CALL xrSetLauncherRunningTileMaskEXT(XrSession, uint64_t);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_APP_LAUNCHER_H
```

## Mapping: existing IPC RPCs → extension functions

The Phase 1 boundary rename produced these IPC RPCs. Phase 2 wraps them in extension functions:

| Phase 1 IPC RPC | Extension function | Notes |
|---|---|---|
| `workspace_activate` | `xrActivateSpatialWorkspaceEXT` | Adds caller-authorization check |
| `workspace_deactivate` | `xrDeactivateSpatialWorkspaceEXT` | |
| `workspace_get_state` | `xrGetSpatialWorkspaceStateEXT` | |
| *(new)* | `xrEnumerateWorkspaceClientsEXT` | Phase 2 unifies what's currently fragmented across `system_get_clients` + per-client info pulls |
| `workspace_set_window_pose` | `xrSetWorkspaceClientWindowPoseEXT` | `client_id` → `clientId` |
| `workspace_get_window_pose` | `xrGetWorkspaceClientWindowPoseEXT` | |
| `workspace_set_window_visibility` | `xrSetWorkspaceClientVisibilityEXT` | |
| `workspace_add_capture_client` | `xrAddWorkspaceCaptureClientEXT` | |
| `workspace_remove_capture_client` | `xrRemoveWorkspaceCaptureClientEXT` | |
| `workspace_capture_frame` | `xrCaptureWorkspaceFrameEXT` | Wraps the flag bitfield as `XrWorkspaceCaptureFlagsEXT` |
| *(new)* | `xrWorkspaceHitTestEXT` | Promotes today's internal `workspace_raycast_hit_test` (still C++ inside the compositor) to a public extension; lets the workspace controller use the runtime's geometry instead of duplicating it |
| `launcher_set_visible` | `xrSetLauncherVisibleEXT` | |
| `launcher_clear_apps` | `xrClearLauncherAppsEXT` | |
| `launcher_add_app` | `xrAddLauncherAppEXT` | `ipc_launcher_app` → `XrLauncherAppInfoEXT` |
| `launcher_poll_click` | `xrPollLauncherClickEXT` | |
| `launcher_set_running_tile_mask` | `xrSetLauncherRunningTileMaskEXT` | |

Result: 14 RPCs become 16 extension functions (+2 for hit-test and client enumeration, both currently internal). The wire format goes through the existing IPC pipe — the loader's extension dispatch is the only new code on the runtime side; the IPC handler layer reuses what Phase 1 already renamed.

## What stays out of the extensions

These survive in the runtime under non-extension-exposed neutral C names — they are mechanism that no workspace controller needs to invoke directly:

- Per-client virtual-window-pose → atlas-region computation
- Deferred HWND resize on next frame after a pose change
- Spatial cursor → display-plane intersection math (the *implementation* of hit-test; `xrWorkspaceHitTestEXT` is the entry point)
- The 2D Windows.Graphics.Capture pumping thread
- The capture client's offscreen render pass
- Multi-compositor scheduling
- Per-frame composite + present
- Atlas write-to-PNG path (the *implementation* of capture)
- Input event ring buffer that forwards mouse/keyboard from the workspace's window to the focused client's HWND
- Window-relative Kooima projection per client

These are all currently in `comp_d3d11_service.cpp`. They keep their internal `shell_*` names through Phase 1 (Phase 1 deferral) and get cleaned up by Phase 2 either by deletion (when policy moves) or by renaming once the surrounding code is unambiguously mechanism.

## What moves to the workspace controller process

Counterpart list — these stop being runtime concerns once Phase 2 lands:

- Launcher tile registry storage (today: `comp_d3d11_service.cpp` `registered_apps`). Becomes: workspace process owns the array, pushes via `xrAddLauncherAppEXT` whenever its registry changes.
- Layout-preset semantics (`grid` / `immersive` / `carousel`). Becomes: workspace process computes per-window poses for a chosen preset and pushes them via `xrSetWorkspaceClientWindowPoseEXT` for each client.
- Right-click context menu, double-click maximize/restore, edge-drag resize, title-bar drag. Becomes: workspace process receives input events (or hit-test results), interprets them, and pushes the resulting pose changes.
- Title-bar chrome rendering (close/minimize buttons, app name). Becomes: workspace process renders chrome into its own client window which the runtime composites alongside the app window via the workspace pose API. The runtime stops drawing chrome.
- Eye-tracking-mode focus policy (when to switch MANAGED ↔ MANUAL based on which client is focused). Becomes: workspace process tracks focus, the runtime exposes mode-set per session. The integration is via `XR_EXT_display_info`'s eye-tracking-mode functions, called by the workspace on the focused client's behalf — design TBD in the migration that touches it.
- "Empty workspace" ESC-handling carve-out, `pending_shell_reentry` state, deactivate-and-restore-2D-windows lifecycle. Becomes: deleted. The runtime's "no clients connected → idle composite path" is generic.

## Lifecycle and threading

- All workspace extension calls execute on the workspace's main session thread (the thread that owns the `XrSession`). No worker threads inside the workspace process need access.
- Calls are synchronous from the workspace's perspective; the runtime returns when the state change is committed (it may take effect on the next render frame).
- Events (`XrEventDataWorkspaceClientConnectedEXT`, etc.) are delivered via the standard `xrPollEvent` mechanism on the workspace session.
- The runtime is free to coalesce or defer state changes internally (e.g., merging multiple pose updates per render frame). The workspace controller does not need to throttle.

## Versioning and compatibility

- `SPEC_VERSION` starts at 1 when the first non-shell consumer drives the runtime end-to-end.
- The two extensions version independently.
- Breaking changes to a struct (adding non-final fields) bump SPEC_VERSION; extension hosts gate on the version they need.
- New optional fields can be added at the end of structs without a SPEC_VERSION bump as long as the runtime initializes them safely on read.
- New functions can be added at any version; consumers gate on `xrGetInstanceProcAddr` returning non-null for the new function name.

The shell repo, once severed (Phase 3), pins a minimum SPEC_VERSION in its CMake. Bumping the SPEC_VERSION on the runtime side is a coordinated release.

## Open questions

1. **Authorization model.** Today the IPC server gates the privileged role on `application_name == "displayxr-shell"`. For Phase 2, options:
   - Keep matching `application_name`, but make the allowed name configurable in `service.json` (`workspace_app_name = "..."`).
   - Add a per-binary capability the OS verifies (Authenticode / code-signing on Windows, codesign entitlement on macOS).
   - Trust the orchestrator: only the binary the orchestrator spawned (matching its PID) can activate.
   The orchestrator-PID approach is the easiest defensible thing — the service knows which PID it spawned via `service_orchestrator.c`, and `xrActivateSpatialWorkspaceEXT` only succeeds for that PID. Configurable name is the fallback for legacy multi-terminal flows.

2. **Click events: poll-and-clear vs xrPollEvent.** Current Phase 1 IPC uses poll-and-clear (`launcher_poll_click`). Two patterns to choose between:
   - Keep poll-and-clear (simple, current model).
   - Promote to event (`XrEventDataLauncherClickEXT` queued on `xrPollEvent`). More consistent with OpenXR conventions and naturally handles multiple-click-before-poll.
   Recommend: promote to event in SPEC_VERSION 1. The poll-and-clear shape was shaped by the IPC's bidirectional pipe constraint, not by user-facing requirements.

3. **Should `xrEnumerateWorkspaceClientsEXT` be in the workspace extension or its own?** A vertical that uses workspace primitives but doesn't want enumeration is unusual; keep it in `XR_EXT_spatial_workspace`.

4. **Capture API surface.** The flags-and-paths shape mirrors the existing IPC. An alternative is exposing a `XrSwapchain`-shaped capture target the workspace can read directly; deferred — file-based capture covers the current MCP and ad-hoc-screenshot use cases.

5. **macOS native window for capture clients.** Phase 2's first cut is Windows-only because the current capture pipeline is Windows-only. When macOS lands, `xrAddWorkspaceCaptureClientEXT` grows a chained `XrWorkspaceCocoaCaptureBindingEXT` struct (NSView*) the same way `XR_EXT_cocoa_window_binding` chains.

6. **Focus / input routing.** Not yet in the extension. The current implementation has the runtime forward mouse/keyboard from the workspace window to the focused client's HWND. In Phase 2, the workspace likely needs to:
   - Tell the runtime which client is focused (`xrSetWorkspaceFocusedClientEXT`?).
   - Get raw input events from the runtime (currently consumed by the runtime's window proc).
   This is significant policy migration work — the workspace currently has no choice over input routing; the runtime makes the call. Design deferred to its own Phase 2 sub-step.

7. **macOS workspace controller.** Out of scope for Phase 2 spec freeze. The extensions are platform-neutral by design but the only working implementation will be Windows for the foreseeable future.

## What this draft implies for Phase 2 sequencing

Re-reading the plan doc's recommended Phase 2 order against the header sketch:

1. **Layout presets** — pure policy, no new extension surface needed (workspace just calls `xrSetWorkspaceClientWindowPoseEXT` per client). Easiest first migration.
2. **Launcher tile registry + click polling** — exercises `XR_EXT_app_launcher` end-to-end. Small and well-bounded; good second step to validate the launcher extension's shape.
3. **Title-bar chrome rendering** — the most invasive. Requires the workspace controller to render chrome as its own client window, positioned via `xrSetWorkspaceClientWindowPoseEXT`. Probably the migration that exposes the most surface gaps in the spec.
4. **Capture client lifecycle** — `xrAddWorkspaceCaptureClientEXT` / `xrRemoveWorkspaceCaptureClientEXT`; mostly already neutral, light cleanup.
5. **Focus / input routing** — open question above; design before migrating.
6. **ESC / empty-workspace cleanup** — dies naturally as the rest moves; no explicit migration needed.

After all six, the runtime side of `comp_d3d11_service.cpp` should be measurably smaller (not just renamed), the workspace controller process should be measurably bigger, and the extension surface should be in real use — at which point we can freeze SPEC_VERSION 1 and ship to `displayxr-extensions`.
