# WebXR-in-Shell Plan

**Branch:** `feature/webxr-in-shell` (off `main` after `#161` is merged)
**Status:** Not started — plan only.

## Problem

When the shell is active and a user opens a Chrome WebXR page, the
Chrome-spawned OpenXR session connects to our service like any other IPC
client. The multi-compositor allocates a shell tile for it, so visually
*a window appears in the shell* with the page's content — that part
works automatically.

But the content inside the window is wrong:

1. Chrome allocates a headset-scale framebuffer (≈2160 px per eye,
   independent of any `recommendedImageRect` we hand it).
2. The shell tile is much smaller (e.g. 40 % of a 3840×2160 display =
   1536×2160 per view).
3. The compositor's shell-mode blit path **disables scaling**
   (`!sys->shell_mode` guard on `needs_scale` in
   `comp_d3d11_service.cpp`). So it copies the top-left
   `tile_w × tile_h` region of Chrome's framebuffer into the slot — a
   crop, not a resize. That's the "squished upper part" the user sees.
4. Even with scaling enabled, Chrome's view matrices would still be the
   ones it computed for a full-display-sized Kooima frustum: the
   perspective wouldn't match the window's size/position/distance.

The fix-by-scaling workaround would mask (3) but not (4), so the
geometry would still be wrong. We want the content to look correct:
**the app should render as if the shell window were the display**.

Two distinct codepaths are needed, matching our render-ready/raw split:

- **A. Legacy WebXR** (Chrome WebXR with no extension) — fix via
  `xrLocateViews` returning per-window Kooima matrices. The app
  renders "render-ready" per-view frustums just like any legacy handle
  app.

- **B. Bridge-aware WebXR** (pages using our `session.displayXR`
  extension) — scope `displayInfo` to the shell window. The app does
  its own Kooima math against a shell-window-sized "display". Same
  contract as our in-shell handle apps.

Both depend on the same piece of plumbing: **the per-client shell
window geometry must flow from `multi_comp` up to the state tracker
(for A) and to the bridge (for B)**.

## Today's state (as of this plan)

- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`
  - `struct d3d11_multi_compositor` holds all per-slot geometry
    (`clients[i].window_rect`, window world pose, etc.).
  - `needs_scale` blit path is gated by `!sys->shell_mode`
    (~line 8861) — intentional per the atlas-stride invariant
    (see `feedback_atlas_stride_invariant.md` memory).
- `src/xrt/state_trackers/oxr/oxr_views.c` computes `xrLocateViews`
  from the device's view descriptors. No per-client override hook
  today.
- `src/xrt/targets/webxr_bridge/main.cpp`
  - Bridge forwards display-info from the physical display.
  - Has no concept of "the window the bridge session corresponds to".
  - Shell-mode display-info is passed through unchanged.
- `webxr-bridge/extension/src/main-world.js`
  - `session.displayXR.displayInfo` already fills from bridge
    messages; the sample's render loop re-reads
    `session.displayXR` each frame, so if the bridge pushes new
    info the sample adapts.
- `webxr-bridge/extension/src/isolated-world.js`
  - Connects lazily to bridge on first main-world message; no
    window-binding concept.

## Goal

1. **Legacy WebXR renders correctly inside a shell window**:
   perspective matches the window's size/position/distance, content
   fills the window at native resolution.
2. **Bridge-aware WebXR renders correctly inside a shell window**:
   same as a handle app — raw eye poses in window-local space, display
   info describes the window, live re-read picks up resize/pose
   changes.
3. **No regression** to bridge-aware WebXR on the full physical display
   (the non-shell case that works today).
4. **No regression** to in-process handle apps in the shell.

## Design

### Shared plumbing: per-client window geometry

`multi_comp` already owns the slot rect + shell-window world pose for
each IPC client. Expose a small API:

```c
// Returns false if the client has no shell-window binding
// (e.g. shell_mode is off, or the client isn't registered yet).
bool
comp_d3d11_service_get_client_window_info(
    struct xrt_system_compositor *xsysc,
    uint32_t ipc_client_id,
    struct xrt_shell_window_info *out_info);

struct xrt_shell_window_info {
    uint32_t slot_w_px, slot_h_px;   // Tile size the client should render to
    struct xrt_pose  window_pose;    // Window plane pose in world (m)
    float            window_w_m;     // Window width in meters
    float            window_h_m;     // Window height in meters
    // ... whatever else matches Kooima inputs
};
```

Call sites:

- State tracker (oxr): for `xrLocateViews` override in shell mode.
- Bridge: when a WebXR session's IPC client shows up, bind it to the
  corresponding shell window.

Lookup key is the IPC client id (already available in both places).

### Path A: Legacy WebXR via `xrLocateViews`

In `oxr_views.c`, before returning the default per-view matrices:

```c
if (sys_is_shell_mode() && !is_bridge_aware(session)) {
    struct xrt_shell_window_info info;
    if (get_client_window_info(client_id, &info)) {
        compute_kooima_views(
            info.window_pose, info.window_w_m, info.window_h_m,
            user_eye_pose_left, user_eye_pose_right,
            &views[0], &views[1]);
        // Also clamp the returned per-view count to 2 for stereo
        // (WebXR is stereo only).
        return XR_SUCCESS;
    }
}
```

And in `xrEnumerateViewConfigurationViews`, return a
`recommendedImageRect = slot_w/2 × slot_h` (per-view) so Chrome
allocates tile-sized buffers and we skip the blit-scale path
entirely.

**How to detect "bridge-aware vs legacy"**: bridge-aware sessions go
through our bridge host, which is a separate IPC client. Legacy
WebXR connects Chrome directly. Bind tracking: the bridge already
sets `DXR_BridgeClientActive` on the compositor HWND when a page
uses the extension. Extend that to a per-client flag the state
tracker can read.

**Gotcha:** the "user eye pose" must be the same pose that drives
shell raycasting (so what the user sees matches where they're
pointing). The shell already has this pose — plumb it through the
same accessor.

### Path B: Bridge-aware WebXR via scoped displayInfo

When a bridge session is associated with a shell window:

1. Bridge's `hello → display-info` handshake returns **window-scoped**
   info: `displayPixelSize = slot_w × slot_h`, `displayMeters =
   window_w_m × window_h_m`, eye poses in window-local space.
2. Shell, when the user drags/resizes a window, fires a new bridge
   message `window-info` (or reuses `display-info`) with updated
   dims/pose. The bridge pushes to the extension; `main-world.js`
   updates `latestDisplayInfo`; the sample's per-frame re-read of
   `session.displayXR` picks up the new `displayInfo` naturally.
3. When the user closes the window, shell tells the bridge to end the
   session (or the bridge detects the client disconnect).

**Per-session window binding**: today the bridge assumes one
compositor HWND. Add a map `{ ipc_client_id → shell_window_info }`
maintained by the service and queryable by the bridge over IPC.

### Path B add-ons: events

New bridge message types (both directions bridge↔extension):

- `window-info` (bridge → extension) — sent on session start and on
  resize/pose changes. Replaces/extends `display-info`.
- `shell-focus-changed` (bridge → extension) — optional, for
  letting pages react to focus lost/gained.

Isolated-world passes these to main-world unchanged. Main-world
treats `window-info` like a fresher `display-info`.

### Why this split

Keeping legacy and bridge-aware as two codepaths (not one unified
render-ready handoff) matches the current architecture's
render-ready-vs-raw split and avoids:

- Re-deriving legacy view matrices in bridge-aware sessions (would
  break our handle-app-compatible contract).
- Forcing legacy WebXR apps to adopt the bridge (they don't know
  about it).

## Staging

Each stage should build + test cleanly on its own.

### Stage 0 — shared plumbing
- Add `comp_d3d11_service_get_client_window_info` + the
  `xrt_shell_window_info` struct.
- No behavior change yet; just API wiring. Both legacy and
  bridge-aware still behave as today.
- Test: a debug log prints the window info for every new IPC client
  when shell is active.

### Stage 1 — scale-blit fallback (optional but recommended)
- Drop the `!sys->shell_mode` guard on `needs_scale`.
- Ensure the SRGB shader-blit path works in shell mode for the
  scaled case (re-read `feedback_srgb_blit_paths` memory before
  touching — current behavior is intentional for 1:1 copies).
- Test: Chrome WebXR content fills the window but perspective is
  still a headset-scale Kooima (wrong perspective, correct coverage).
- This gives us a non-broken-looking fallback while Stages 2/3 land.

### Stage 2 — Path A (legacy xrLocateViews override)
- In `oxr_views.c`, add the shell-mode override calling into the
  plumbing from Stage 0.
- In `oxr_instance.c` (or wherever view configs are enumerated),
  override `recommendedImageRect` for shell-mode clients.
- Test matrix:
  - Chrome WebXR sample page, shell active, one window → content
    fills window, perspective correct, responds to window drag
    *on next session* (live drag-during-session can be Stage 4).
  - Shell inactive, same page → no change (physical display
    Kooima, as today).
  - Two WebXR tabs → each gets its own window with its own matrices.

### Stage 3 — Path B (bridge-aware displayInfo scoping)
- In the bridge, associate each WS session with its IPC client id
  (already available from Chrome's OpenXR client handle).
- On `hello`, look up the shell window info and return window-scoped
  `display-info`.
- Test: bridge-aware sample in shell → renders with correct
  window-local Kooima, same as a handle app.

### Stage 4 — live resize/pose events
- Service → bridge: when multi-comp updates a slot rect/pose, push
  a `window-info` event.
- Bridge → extension → main-world: forward as fresh display-info.
- Test: drag/resize the WebXR window during a live session;
  content reflows.

### Stage 5 — cleanup
- Remove Stage 1's scale-blit fallback if Stages 2/3 cover all
  real-world apps.
- Doc updates: `docs/architecture/extension-vs-legacy.md`,
  `docs/specs/XR_EXT_display_info.md`, any WebXR-specific docs.

## Test plan

Hardware: Leia SR display + dev machine (current setup).
Dependencies: working shell build, Chrome with WebXR enabled, local
static server for the bridge-aware sample.

| Stage | Test |
|-------|------|
| 0 | Build only. Log per-client window info on client connect. |
| 1 | Open `immersive-web.github.io/webxr-samples/immersive-vr-session.html` in Chrome while shell active. Content fills the window (but perspective looks off). |
| 2 | Same test → perspective is correct for the window's size/distance. Drag the window, exit and re-enter VR → new session uses the new window geometry. |
| 3 | Open `http://localhost:8000/sample/` (our bridge sample) in Chrome while shell active. Content renders with window-local Kooima — visually equivalent to a handle cube app in the same slot. |
| 4 | While the sample is live, drag the shell window. The cube's perspective smoothly tracks the new geometry. |
| 5 | Regression sweep: cube_handle_d3d11 in shell still works; legacy WebXR with shell off still works; bridge-aware WebXR with shell off still works (full-display Kooima). |

## Gotchas / known risks

- **Head-pose consistency**: Chrome's WebXR uses a stationary-type
  reference space by default. The "user eye pose" we inject must be
  stable in the same space; otherwise content jitters. Likely we want
  to use the shell's already-computed user pose and accept that
  WebXR's `XRFrame.getViewerPose` will track shell head-pose, not a
  controller.
- **Two compositor swapchains**: remember the canvas-vs-display split
  (see `docs/specs/swapchain-model.md`). `recommendedImageRect` must
  match canvas dims, not display dims, or legacy apps render into
  mis-sized buffers.
- **Atlas stride invariant**: tiles always at `sys->view_width` stride
  in shell mode. Don't break that when plumbing new slot sizes (see
  `feedback_atlas_stride_invariant.md`).
- **Bridge session lifecycle**: today the bridge assumes one session.
  Multiple WebXR windows = multiple bridge sessions. The existing
  singleton-ish state needs a per-session index before Stage 3.
- **SRGB in scale path**: see `feedback_srgb_blit_paths.md` — non-shell
  scaling uses a shader that linearizes; 1:1 shell copies don't. If
  Stage 1 ships, make sure the shader path runs for shell-mode scale
  too.
- **Chrome's own scaling**: Chrome may apply DPR / foveation that
  surprises the `recommendedImageRect` override. Log the fb size it
  actually allocates and iterate from there.

## Files likely touched

| File | Why |
|------|-----|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Expose per-client window info; scale-blit fallback (Stage 1). |
| `src/xrt/include/xrt/xrt_system.h` | New `xrt_shell_window_info` struct + accessor declaration. |
| `src/xrt/state_trackers/oxr/oxr_views.c` | `xrLocateViews` override (Stage 2). |
| `src/xrt/state_trackers/oxr/oxr_system.c` | `recommendedImageRect` override (Stage 2). |
| `src/xrt/targets/webxr_bridge/main.cpp` | Per-session window binding, new message types (Stages 3, 4). |
| `webxr-bridge/extension/src/isolated-world.js` | Forward new message types. |
| `webxr-bridge/extension/src/main-world.js` | Handle `window-info` as fresher display-info. |
| `webxr-bridge/sample/sample.js` | Verify live re-read handles resize; minor docs. |
| `webxr-bridge/DEVELOPER.md` | Document the shell-window-scoped displayInfo contract. |
| `docs/architecture/extension-vs-legacy.md` | Document the legacy shell-mode xrLocateViews behavior. |

## Non-goals for this phase

- Passthrough or video layers from WebXR.
- Per-window eye tracking (shell provides one user pose driving all
  windows — accept that).
- Android / macOS.
- Audio spatialization per-window.
