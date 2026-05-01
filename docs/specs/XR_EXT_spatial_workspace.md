# XR_EXT_spatial_workspace

| Field | Value |
|---|---|
| **Extension Name** | `XR_EXT_spatial_workspace` |
| **Spec Version** | 8 |
| **Authors** | David Fattal (DisplayXR / Leia Inc.) |
| **Status** | Provisional — published with the DisplayXR runtime; subject to revision before Khronos registry submission. |
| **Header** | `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` |
| **OpenXR Version** | 1.0 |
| **Dependencies** | OpenXR 1.0 core. The Windows platform path also relies on `XR_EXT_win32_window_binding` for the capture-client HWND argument; the Cocoa path on `XR_EXT_cocoa_window_binding`. |

---

## 1. Motivation

A spatial workspace is the OS-level shell for a 3D display: the privileged process that arranges multiple OpenXR clients (and 2D OS-window captures) on the panel, dispatches focus, drives layout presets, and renders chrome around the active app. It needs an OpenXR-shaped contract with the runtime — the runtime owns mechanism (atlas composition, IPC, hit-test geometry, swapchain plumbing) and the workspace controller owns policy (which apps exist, where their windows go, how they animate, what chrome looks like, what the cursor does).

`XR_EXT_spatial_workspace` is that contract. A privileged OpenXR session enables the extension, calls `xrActivateSpatialWorkspaceEXT` to claim the workspace role, and from there positions client windows, drives input, and hooks lifecycle events. Anything visible to the user is policy and lives in the controller. Anything per-frame and timing-sensitive is mechanism and lives in the runtime, exposed as a primitive on this surface.

---

## 2. Surface (spec_version 8)

### Lifecycle

- `xrActivateSpatialWorkspaceEXT(session)` — claim the workspace role. At most one per system. Caller authorisation is by orchestrator-PID match (manual-mode fallback when no orchestrator is registered).
- `xrDeactivateSpatialWorkspaceEXT(session)` — release the role. `xrDestroySession` has the same effect implicitly.
- `xrGetSpatialWorkspaceStateEXT(session, &active)` — query whether this session holds the role.

### Capture clients (adopt a 2D OS window)

- `xrAddWorkspaceCaptureClientEXT(session, nativeWindow, name, &outClientId)` — adopt a Windows HWND (or future macOS/Cocoa equivalent) as a workspace client. The runtime starts a platform-appropriate capture (Windows.Graphics.Capture on Windows) and treats the captured texture as a client swapchain.
- `xrRemoveWorkspaceCaptureClientEXT(session, clientId)` — release the capture.

### Window pose + visibility

- `xrSetWorkspaceClientWindowPoseEXT(session, clientId, &pose, widthMeters, heightMeters)` — position a client's window quad in display-centric space. Pose origin is the display centre; +x right, +y up, +z toward the viewer. The runtime composites the named client's swapchain into a quad of this physical size.
- `xrGetWorkspaceClientWindowPoseEXT(session, clientId, &outPose, &outW, &outH)` — read back current pose + size. Used by controllers to seed animations (snapshot current → animate to target) and to persist layouts.
- `xrSetWorkspaceClientVisibilityEXT(session, clientId, visible)` — show / hide without destroying.

### Hit-test

- `xrWorkspaceHitTestEXT(session, cursorX, cursorY, &outClientId, &outLocalUV, &outHitRegion)` — translate a screen-space cursor into a hit on a client window. The runtime intersects an eye→cursor ray with each client's window quad and reports the hit `clientId`, an interpolated UV on the content rect, and a `XrWorkspaceHitRegionEXT` classification (CONTENT, TITLE_BAR, CLOSE_BUTTON, EDGE_RESIZE_*, TASKBAR, LAUNCHER_TILE, BACKGROUND).

### Focus

- `xrSetWorkspaceFocusedClientEXT(session, clientId)` — set the focused client. The runtime forwards keyboard input (other than runtime-reserved keys) and click-through events to the focused client's HWND.
- `xrGetWorkspaceFocusedClientEXT(session, &outClientId)` — read the focused client.

### Input drain + pointer capture

- `xrEnumerateWorkspaceInputEventsEXT(session, capacityInput, &countOutput, events)` — drain pending workspace input events. Tagged-union `XrWorkspaceInputEventEXT` records carry one of:
  - **POINTER** (button down/up). Hit-test enriched at drain time so the controller does not need to call `xrWorkspaceHitTestEXT` per event.
  - **KEY** (down/up + modifiers). MVP key policy: TAB and DELETE are consumed by the runtime; ESC is consumed when any window is maximised; everything else is delivered here AND forwarded to the focused HWND.
  - **SCROLL** (mouse-wheel delta + cursor + modifiers).
  - **POINTER_MOTION** *(spec_version 6)* — per-frame `WM_MOUSEMOVE` while pointer capture is enabled. Hit-test enriched. Carries a `buttonMask` of currently-held buttons (bit0=L, bit1=R, bit2=M).
  - **FRAME_TICK** *(spec_version 6)* — fires once per displayed compositor frame with the host-monotonic ns at frame compose. Lets controllers pace per-frame work (animation interpolation, hover effects) to display refresh without polling.
  - **FOCUS_CHANGED** *(spec_version 6)* — fires only on focused-client transitions (TAB, click auto-focus, controller-set, client disconnect). Does **not** fire on stable frames. Carries `prevClientId` and `currentClientId`.
  - **WINDOW_POSE_CHANGED** *(spec_version 8)* — fires when the runtime itself changes a client's window pose or size (edge-resize drag, fullscreen toggle, F11). NOT emitted for controller-driven `xrSetWorkspaceClientWindowPoseEXT` calls — the controller already knows about those. Carries `clientId`, the new `pose`, and `widthMeters` / `heightMeters`. Lets the controller respond to runtime-driven geometry changes without polling.
- `xrEnableWorkspacePointerCaptureEXT(session, button)` — begin pointer capture. While enabled, button-up and motion events for the named button keep flowing even when the cursor leaves any window. The runtime drives Win32 SetCapture so motion outside the workspace HWND still reaches the WndProc. Used to implement controller-driven drag, carousel, and chrome highlight without the runtime knowing about drag policy.
- `xrDisableWorkspacePointerCaptureEXT(session)` — release.

### Frame capture

- `xrCaptureWorkspaceFrameEXT(session, &request, &result)` — read back the current composite atlas (and selected sub-views) to disk. Used by controllers that ship screenshot / recording features without giving them direct access to client swapchains.

### Lifecycle requests *(spec_version 6)*

- `xrRequestWorkspaceClientExitEXT(session, clientId)` — ask the runtime to close any client (not just the focused one). For OpenXR clients the runtime emits `XRT_SESSION_EVENT_EXIT_REQUEST`; for capture clients it tears down the capture immediately. Equivalent of the runtime's built-in DELETE shortcut, but targeted.
- `xrRequestWorkspaceClientFullscreenEXT(session, clientId, fullscreen)` — toggle fullscreen for any client. Mirrors F11 behaviour: animates the target window to fill the display and hides others; XR_FALSE restores.

### Client enumeration

- `xrEnumerateWorkspaceClientsEXT(session, capacity, &countOutput, clientIds)` — two-call enumerate of OpenXR clients connected to the workspace.
- `xrGetWorkspaceClientInfoEXT(session, clientId, &info)` — per-client metadata (name, PID, focus state, visibility, z-order).

### Controller-owned chrome *(spec_version 7)*

The runtime ships with **zero default chrome**. If a controller wants chrome (title bars, buttons, grip handles, focus glow) around its workspace clients, it submits a per-client chrome image as an OpenXR swapchain and tells the runtime where to composite it.

- `xrCreateWorkspaceClientChromeSwapchainEXT(session, clientId, &createInfo, &outSwapchain)` — mint a cross-process-shareable image-loop swapchain (D3D11 SHARED_NTHANDLE + KEYEDMUTEX texture; one image; controller-chosen format/size). The returned `XrSwapchain` is a regular OpenXR swapchain — `xrAcquireSwapchainImage` / `xrWaitSwapchainImage` / `xrReleaseSwapchainImage` work as normal. The runtime side-table records this swapchain as "chrome" for the named client.
- `xrSetWorkspaceClientChromeLayoutEXT(session, clientId, &layout)` — attach a chrome quad to the client's window. The layout carries `poseInClient` (chrome pose relative to the window), `sizeMeters` (quad dimensions), `followsWindowOrient` (rotate with window?), `depthBiasMeters` (bias toward the eye; 0 = runtime default 1 mm), and an inline array of up to 8 `XrWorkspaceChromeHitRegionEXT` entries. Each region carries a controller-defined `id` and a UV-space rect — when the cursor hits inside one, the runtime echoes the matched id back as `chromeRegionId` on POINTER / POINTER_MOTION events.
  - *(spec_version 8)* `anchorToWindowTopEdge` — when `XR_TRUE`, `poseInClient.position.y` is interpreted as an offset ABOVE the window's TOP edge (positive = above), not from window center. The runtime evaluates `effectiveY = window_h/2 + poseInClient.position.y` per frame using the **current** window height, so chrome stays glued to the top edge during a resize without the controller re-pushing layout.
  - *(spec_version 8)* `widthAsFractionOfWindow` — when > 0, chrome width is computed each frame as `window_w × widthAsFractionOfWindow` (`sizeMeters.width` is ignored). 0 means "absolute size, use `sizeMeters.width`." Pairs with `anchorToWindowTopEdge` so a single layout push survives arbitrary window resizes.
- `xrDestroyWorkspaceClientChromeSwapchainEXT(swapchain)` — tear down the chrome swapchain. The runtime drops the side-table entry and unlinks the slot.

**Authoring loop.** Once per state change (hover toggle, focus change, button-hover, layout switch), the controller calls `xrAcquireSwapchainImage` → `xrWaitSwapchainImage` → renders into image[0] → `xrReleaseSwapchainImage`. The runtime composites the latest image at the layout's pose every frame; idle = zero CPU, zero GPU, zero IPC.

**Hit-region semantics.** `chromeRegionId` is opaque to the runtime — the controller decides what each id means (1 = grip, 2 = close, 3 = minimize, 4 = maximize, etc.). Region 0 is reserved as `XR_NULL_WORKSPACE_CHROME_REGION_ID` (no region matched / no hit).

**Hover events.** `XR_WORKSPACE_INPUT_EVENT_POINTER_HOVER_EXT` fires whenever the runtime's per-frame raycast detects a hovered-slot transition (cursor enter / leave a window). Works in all layout modes, regardless of pointer-capture state — controllers use this as the trigger for chrome fade-in / fade-out animations.

**New hit-region enum value.** `XR_WORKSPACE_HIT_REGION_CHROME_EXT = 6` indicates the cursor hit a controller-submitted chrome quad. (Legacy `CLOSE_BUTTON / MINIMIZE_BUTTON / MAXIMIZE_BUTTON / TITLE_BAR` enum values remain in the header for spec_version ≤ 6 controllers; the runtime still emits them for backwards compatibility while the in-runtime hit-test fields exist.)

### Event-driven wakeup *(spec_version 8)*

- `xrAcquireWorkspaceWakeupEventEXT(session, &outNativeHandle)` — return a platform-native event handle the controller waits on instead of polling. The runtime signals the handle whenever async workspace state changes: an input event was pushed onto the drain queue, the focused-client transitioned, the hovered-slot transitioned, or a window pose / size changed via `WINDOW_POSE_CHANGED`. On Windows, `outNativeHandle` is a Win32 `HANDLE` cast to `uint64_t`; the caller passes it to `WaitForSingleObject` / `MsgWaitForMultipleObjects` and **owns** the handle (must `CloseHandle` when done). Auto-reset semantics — the handle clears as one waiter wakes; multiple `SetEvent`s in quick succession may collapse to one wake, so the controller must drain ALL pending state on each wake. The runtime can be called multiple times to mint fresh handles (each duplicates the runtime's single source-of-truth event into the caller's process). Returns `XR_ERROR_FEATURE_UNSUPPORTED` on non-Windows platforms.

The controller's idle CPU cost goes from ~0.1 % of one core (the polling baseline) to effectively 0 once this handle is wired into its wait set. The reference shell at `src/xrt/targets/shell/main.c` uses it together with the launcher's existing `MsgWaitForMultipleObjects` loop — drain on every wake, sleep otherwise.

---

## 3. Design notes

**3D depth-tested compositor.** Multiple windows occlude each other per-pixel based on their actual 3D depth — not draw order, not focus state. The runtime maintains a D32_FLOAT depth target alongside the combined atlas; each per-window blit outputs a per-corner depth value derived from `(eye_z − corner_world_z) / WORKSPACE_DEPTH_FAR_M` (1.0 m). Hardware LESS test resolves occlusion, including for tilted quads that intersect at angles. Window chrome (title bar, buttons, glyphs) biases its depth slightly toward the eye so it wins over its own window's content while still depth-testing against other windows. Controllers that pose windows in 3D — carousel rings, immersive paraboloids, edge-resize z-shifts, custom layouts — get correct occlusion without coordinating render order. The painter's-algorithm Z sort is retained only for transparent-edge alpha blending (rounded corners); opaque occlusion is hardware.

**Runtime cedes interactive policy under pointer capture.** When `xrEnableWorkspacePointerCaptureEXT` is active, the runtime suppresses its built-in title-bar drag, edge-resize, and right-button rotation. The controller is the sole authority for what dragging a window means — carousel rotation, edge-resize-as-z-shift, custom drag affordances all become possible without runtime cooperation. This pairs naturally with POINTER_MOTION events: the controller receives WM_MOUSEMOVE while capture is held and decides per-frame what to do with it.

**Hit-test enrichment.** Pointer events (POINTER and POINTER_MOTION) are enriched with the workspace hit-test (`hitClientId`, `hitRegion`, `localUV`) at drain time, so controllers don't pay the cost of a separate `xrWorkspaceHitTestEXT` call per event. The drain takes the runtime's render-mutex once for the whole batch so geometry stays stable across the events being enriched.

**Capture-client id encoding.** Capture-client IDs use the convention `slot + 1000` so they're disambiguated from OpenXR-client IDs (which are issued by the IPC layer). The runtime accepts either form on `xrSetWorkspaceClientWindowPoseEXT` and the new request functions; the controller treats them as opaque uint32_t handles.

**Per-frame motion cost.** With pointer capture enabled, the runtime emits one POINTER_MOTION event per WM_MOUSEMOVE message (typically ~60–120 events/s during cursor activity) plus one FRAME_TICK per displayed frame. Drain RPC overhead is ~10–20 µs. Aggregate worst case during a 4-client carousel drag is well under 1% of one core. Two escape hatches if real-world numbers surprise us: (a) the runtime can throttle motion server-side via the Enable call, (b) shared-memory ring for cursor state can replace IPC entirely.

**FOCUS_CHANGED coalescing.** The drain emits at most one FOCUS_CHANGED per drain pass — intermediate transitions inside the drain window are coalesced to the latest target. This keeps the controller from having to dedupe; the spec promises "fires only on transitions, never on stable frames."

**Wire-format compatibility.** Spec_version 6 strictly extends spec_version 5 by adding new enum values and new event-union members; old controllers that don't know the new variants see them as "unknown" and skip them via the existing `default:` case in their drain switch. The two new request PFNs are additive — controllers that don't resolve them are unaffected.

**Spec_version 7 adds controller-owned chrome.** Three new function PFNs (`xrCreate/Destroy/SetWorkspaceClientChrome*EXT`), three new structs (`XrWorkspaceChromeSwapchainCreateInfoEXT`, `XrWorkspaceChromeLayoutEXT`, `XrWorkspaceChromeHitRegionEXT`), one new enum value (`XR_WORKSPACE_HIT_REGION_CHROME_EXT`), one new field (`chromeRegionId`) on POINTER + POINTER_MOTION event payloads, and a no-longer-reserved POINTER_HOVER event. The runtime ships with **zero default chrome**: spec_version 6 controllers that don't know about chrome swapchains see clients as bare content quads. Migrating from 6 → 7 is opt-in — controllers that want chrome submit it via the new APIs.

**Spec_version 8 closes the controller-loop.** Three additions, all additive over v7: (1) `xrAcquireWorkspaceWakeupEventEXT` returns a Win32 event handle so the controller can sleep on `MsgWaitForMultipleObjects` instead of polling — drops idle CPU from ~0.1 % to ~0. (2) `XrWorkspaceChromeLayoutEXT` gains `anchorToWindowTopEdge` + `widthAsFractionOfWindow` flags so the runtime auto-recomputes chrome anchor + width every frame from current window dims; the controller pushes layout once at create and never on resize, and the chrome stays glued to the window edge in lockstep with content (instead of lagging one frame behind). (3) `XR_WORKSPACE_INPUT_EVENT_WINDOW_POSE_CHANGED_EXT` lets controllers react to runtime-driven pose / size changes (edge-resize drag, fullscreen toggle) without a re-query. v7 controllers that don't resolve the new PFN / fields keep working — none of the v8 additions are mandatory.

**Architectural endpoint.** Per the architectural North Star (`feedback_controllers_own_motion`), the runtime owns mechanism, the controller owns policy + appearance. After spec_version 8, this principle holds for chrome and for the controller's main-loop wakeup pattern: the runtime owns the cross-process texture-share mechanism, the depth-pipeline composite, the hit-test plumbing, the auto-anchor math, and the wakeup-event source-of-truth; the controller owns every pixel of chrome appearance, every region's hit-region geometry, every animation curve, and the wait primitive that drives its idle behavior.

---

## 4. Implementation

The extension is implemented in this repository:

- **Header**: `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` (frozen for the runtime ABI, auto-published to `DisplayXR/displayxr-extensions` on each push to main).
- **State tracker**: `src/xrt/state_trackers/oxr/oxr_workspace.c` (dispatch wrappers); `src/xrt/state_trackers/oxr/oxr_api_negotiate.c` (PFN entries).
- **IPC**: `src/xrt/ipc/shared/proto.json` (RPC definitions); `src/xrt/ipc/shared/ipc_protocol.h` (event wire format); `src/xrt/ipc/client/ipc_client_compositor.c` (bridge); `src/xrt/ipc/server/ipc_server_handler.c` (server handlers).
- **Service compositor (Windows D3D11)**: `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (drain, hit-test enrichment, FRAME_TICK + FOCUS_CHANGED + POINTER_HOVER emission, controller-chrome composite, chrome quad raycast, chromeRegionId resolution, request_*_by_slot helpers); `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` (WndProc, public ring, Win32 SetCapture / ReleaseCapture).
- **Reference controller (chrome appearance)**: `src/xrt/targets/shell/shell_chrome.cpp` — owns the rounded-pill SDF shader, grip dots + close/min/max button geometry, hover-fade ease-out cubic, per-state re-render. Reads cursor-over-chrome via POINTER_HOVER events; dispatches close → exit RPC, max → fullscreen RPC.
- **Reference controller (lifecycle)**: `src/xrt/targets/shell/main.c` (animation framework, smooth preset transitions, interactive carousel state machine, variable poll cadence, chrome lifecycle wiring).
- **Smoke test**: `test_apps/workspace_minimal_d3d11_win/main.cpp` — resolves all 28 PFNs, walks lifecycle + pose + visibility + hit-test + focus + drain + pointer capture + 30° yaw orientation + drain counts + lifecycle requests + chrome-swapchain create / acquire / wait / clear / release / layout-with-auto-anchor + wakeup-event handle + chrome destroy + client enumeration + frame capture.

The Phase 2 sub-phase that landed each part of this surface is tracked in [`docs/roadmap/spatial-workspace-extensions-phase2-audit.md`](../roadmap/spatial-workspace-extensions-phase2-audit.md).
