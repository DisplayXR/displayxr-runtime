# Phase 2.C — Controller-owned chrome (lift in-runtime chrome to a controller-submitted swapchain)

**Status:** Draft, 2026-04-30.

## Problem

Phase 2.K landed the floating-pill chrome design (commits 8.A–8.G) inside `comp_d3d11_service.cpp`'s render path. The runtime currently owns:

- Pill background, grip dots, close / minimize / maximize buttons, app icon, glyph rendering.
- Hover-fade per slot, focus-rim glow, button press-feedback, depth bias for chrome-over-content.
- Hit-testing for chrome regions (`workspace_raycast_hit_test` populates `in_title_bar`, `in_grip_handle`, `in_close_btn`, …).
- Geometry math (pill width fraction, gap above content, button inset, glyph y-bias, …).

Two structural problems with this:

1. **Visual design is locked to runtime releases.** Controllers (third-party shells, accessibility wrappers, OEM skins) cannot change chrome appearance — color, art assets, geometry, animation curves, hover behaviour — without forking the runtime.
2. **Phase 2.G's North Star (`feedback_controllers_own_motion`) is not yet honoured for chrome.** Motion policy moved to the controller in 2.G/2.K; chrome rendering is the last remaining piece of "design ownership" inside the runtime. The same architectural argument applies: the runtime must own the mechanism, the controller must own the policy + appearance.

## Goal

Add a public extension surface that lets a workspace controller submit a per-client **chrome swapchain** (a 2D image the controller draws to). The runtime composites it onto the workspace at a controller-specified pose, with controller-specified hit regions. After 2.C the runtime draws **zero chrome by default** — chrome is only visible when the controller submits it.

The migration must be visually lossless: the shell ports the existing pill design to the controller side and the user sees no change at the day-2.C-merge point. From there, the shell can iterate on art / colors / behavior without touching the runtime.

## North Star (re-stated for chrome)

- **Runtime owns the mechanism**: cross-process texture sharing, atlas compositing at a 3D pose, depth pipeline, hit-test plumbing, lifecycle.
- **Controller owns the policy / appearance**: every pixel of chrome, hit region geometry, hover/fade animation, button affordances, art assets, accessibility cues.

If a controller wants no chrome, it submits no swapchain → runtime draws nothing. If a controller wants chrome on only some clients, it submits per-client → runtime composites only those.

## Design

### Surface — chrome swapchain

The controller creates one chrome swapchain per client window it wants to decorate. The runtime composites the chrome image at the controller's specified pose, applies the same depth bias / corner rounding / hover-fade that the in-runtime chrome currently has.

**New extension functions** (added to `XR_EXT_spatial_workspace.h`, spec_version 6 → 7):

```c
typedef XrResult (XRAPI_PTR *PFN_xrCreateWorkspaceClientChromeSwapchainEXT)(
    XrSession                                   session,
    XrWorkspaceClientId                         clientId,
    const XrWorkspaceChromeSwapchainCreateInfoEXT *createInfo,
    XrSwapchain                                *swapchain);

typedef XrResult (XRAPI_PTR *PFN_xrDestroyWorkspaceClientChromeSwapchainEXT)(
    XrSwapchain swapchain);

typedef XrResult (XRAPI_PTR *PFN_xrSetWorkspaceClientChromeLayoutEXT)(
    XrSession                                  session,
    XrWorkspaceClientId                        clientId,
    const XrWorkspaceChromeLayoutEXT          *layout);
```

The two image-loop calls (`xrAcquireSwapchainImage`, `xrWaitSwapchainImage`, `xrReleaseSwapchainImage`) reuse the **existing** OpenXR swapchain APIs — the chrome swapchain is just a regular `XrSwapchain` with a magic handle that the runtime knows is a chrome swapchain (tracked in a side table). No new image-loop entry points.

`XrWorkspaceChromeSwapchainCreateInfoEXT` carries: pixel format, width, height, sample count, mip levels (likely 1). For the floating-pill design today, the shell would pick something like 512×64 px sRGB.

`XrWorkspaceChromeLayoutEXT` carries the **3D placement** of the chrome quad relative to the client's window:

```c
struct XrWorkspaceChromeLayoutEXT {
    XrStructureType type;
    const void*     next;
    XrPosef         poseInClient;       // chrome pose in client-window-local space
    XrExtent2Df     sizeMeters;          // chrome quad width/height in meters
    XrBool32        followsWindowOrient; // if XR_TRUE, chrome rotates with window orientation
    uint32_t        hitRegionCount;
    const XrWorkspaceChromeHitRegionEXT* hitRegions; // [hitRegionCount]
    float           depthBiasMeters;     // signed bias along view-z, default 0.001
};

struct XrWorkspaceChromeHitRegionEXT {
    XrWorkspaceChromeRegionIdEXT id;     // controller-defined opaque ID, 0 = none
    XrRect2Df                    bounds; // in chrome-UV space [0,1]^2
};
```

The shell calls `xrSetWorkspaceClientChromeLayoutEXT` once on layout change (preset switch, window resize, focus toggle), not per-frame. The runtime stores the layout per client and uses it every render.

For the floating pill, today's geometry maps to:

- `poseInClient = {orient=identity, position={0, +window_h/2 + pill_gap_m + pill_h/2, 0}}` — pill floats above content with the gap.
- `sizeMeters = {pill_w, pill_h}` — 75% of content width × 8 mm tall.
- `hitRegions[]` — five rects in chrome-UV: title-bar background, grip, close, minimize, maximize.

### Hit-test plumbing

`workspace_raycast_hit_test` already returns `XrWorkspaceHitRegionEXT` plus per-pixel `localUV` for both content and chrome quads. Phase 2.C extends this:

- New `XR_WORKSPACE_HIT_REGION_CHROME_EXT` value.
- New field on the public `XrWorkspaceInputEventEXT.pointer{,Motion}` payload: `XrWorkspaceChromeRegionIdEXT chromeRegionId` (= the `id` from the layout's hit region whose UV bounds contain the local hit). Filled at drain time, server-side, like `localUV` already is.

Controllers receive `chromeRegionId` directly in the `POINTER` / `POINTER_MOTION` events — no extra round-trip.

### Backwards-compat

Cleanest design: the runtime draws **zero default chrome** in workspace mode. If a controller doesn't submit a chrome swapchain for a client, the user sees the bare content quad (no pill, no buttons, no glyphs).

This is a behaviour change for any future controller that hasn't been updated for 2.C. We accept it because:

- The DisplayXR Shell (only known controller) updates in lockstep with the runtime.
- Public extension consumers are not yet documented; spec_version 7 marks the breaking point cleanly.
- "Runtime owns mechanism, not policy" requires this — leaving fallback chrome means the runtime still owns the design.

If we discover a real consumer that needs the old chrome, Phase 2.C-followup can bring it back as an in-shell-source `default_chrome.cpp` that any controller can `#include` and submit verbatim.

### Performance

State-driven re-render keeps the chrome update rate ≤ 10 Hz under typical use:

- Hover state change → re-render once (fade-in is GPU-side, no per-frame upload from controller).
- Focus change → re-render once.
- Button hover → re-render once.
- Idle → no updates.

Controller side: a typical pill is < 40 KB sRGB. SHARED_NTHANDLE eliminates copy across IPC. Worst-case 40 KB × 10 updates/sec × 8 clients = 3.2 MB/s — negligible.

Runtime side: chrome composite is one extra blit per visible client per eye, same path the in-runtime chrome already goes through. No measurable overhead.

### Animation policy

The existing `chrome_fade` per-slot (hover-driven 150 ms in / 300 ms out) currently lives in `comp_d3d11_service.cpp` and modulates the chrome blits' alpha. In 2.C the controller owns the fade — either by re-rendering the chrome SRV at varying opacity, or (cheaper) by calling a new `xrSetWorkspaceClientChromeOpacityEXT(session, clientId, opacity)` that the runtime applies as a multiplicative alpha on the chrome blit. The latter saves IPC traffic for hover-fade tweens; either approach is acceptable, decide during commit 4.

## Six-commit sequence

Same shape as 2.G / 2.K. Each commit ends green-build + a discrete acceptance check.

### Commit 1 — Public surface bump (header, IPC schema, dispatch stubs)

| File | Edit |
|---|---|
| `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` | Bump `XR_EXT_spatial_workspace_SPEC_VERSION` 6 → 7. Add structs `XrWorkspaceChromeSwapchainCreateInfoEXT`, `XrWorkspaceChromeLayoutEXT`, `XrWorkspaceChromeHitRegionEXT`. Add typedefs + prototypes for `xrCreateWorkspaceClientChromeSwapchainEXT`, `xrDestroyWorkspaceClientChromeSwapchainEXT`, `xrSetWorkspaceClientChromeLayoutEXT`. Add enum `XR_WORKSPACE_HIT_REGION_CHROME_EXT`. Add `chromeRegionId` field to `pointer` and `pointerMotion` payloads. |
| `src/xrt/ipc/shared/proto.json` | Add three new RPC entries; extend wire form for the new event payload field. |
| `src/xrt/state_trackers/oxr/oxr_workspace.c` | Add dispatch wrappers; commit-1 stubs return `XR_SUCCESS` and do nothing. |
| `src/xrt/state_trackers/oxr/oxr_api_negotiate.c` | Wire ENTRY_IF_EXT lines for the new functions. |

**Acceptance:** Build green via `scripts\build_windows.bat build`. PFN resolution count goes from 24 → 27. No behaviour change.

### Commit 2 — Runtime imports chrome swapchain + composites it

| File | Edit |
|---|---|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Implement `comp_d3d11_service_create_chrome_swapchain` — creates a `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` D3D11 texture, hands the NT handle back to the controller via the swapchain's existing import path. Mirror the existing client-swapchain SHARED_NTHANDLE pattern (search `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` in this file). Add a per-client `chrome_swapchain` field on `struct d3d11_multi_client_slot` storing the imported SRV + per-slot layout (pose, size, hit regions). |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | New helper `render_client_chrome(slot, view, eye_pos, …)` — composites the chrome SRV at `slot->chrome_layout.poseInClient` (relative to slot's `window_pose`), using the same `project_local_rect_for_eye` path as content, with `WORKSPACE_CHROME_DEPTH_BIAS` applied. Call from the per-view loop after content blit, before any remaining in-runtime chrome (which is still drawn for now — this commit just **adds** the new path). |

**Acceptance:** A new test path in `test_apps/workspace_minimal_d3d11_win` creates a 256×32 chrome swapchain, fills it with a solid color, calls `xrSetWorkspaceClientChromeLayoutEXT` to place it 4 mm above the test client, atlas screenshot shows the colored bar floating above. No regressions in default in-runtime chrome.

### Commit 3 — Shell ports the floating-pill design to the controller

| File | Edit |
|---|---|
| `src/xrt/targets/shell/main.c` | Add a per-client chrome render module: open chrome swapchain on connect, draw pill / grip / buttons / icon / glyphs into the swapchain image (D3D11 in-process render — the shell already has a D3D11 device for the launcher). Re-render only on state change (hover, focus, button-hover). Push `xrSetWorkspaceClientChromeLayoutEXT` on connect + on every preset switch (carousel needs the layout to follow rotation). |
| `src/xrt/targets/shell/main.c` | Port the visual design verbatim from `comp_d3d11_service.cpp` lines ~7290–7800. Move the `chrome_fade` ease-out cubic into shell-side. Move the glyph y-bias constant. Keep the same `PILL_W_FRAC = 0.75`, `pill_gap_m = tb_h/2`, button-inset 18%, etc. |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Gate the in-runtime chrome render block on `slot->chrome_swapchain == nullptr` so chrome doesn't double-render. |

**Acceptance:** Visual parity with Phase 2.K — atlas screenshot at the day-2.K-merge state vs day-2.C-commit-3 state should be perceptually identical. User confirms hover-fade, focus glow, all three button states, glyph centering, app icon look the same.

### Commit 4 — Hit-test plumbing for chrome regions

| File | Edit |
|---|---|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Extend `workspace_raycast_hit_test` to ray-cast against each client's chrome quad in addition to its content quad. On hit, populate the hit-result struct with `XR_WORKSPACE_HIT_REGION_CHROME_EXT` and the chrome-local UV. Look up the matching `chromeRegionId` from `slot->chrome_layout.hitRegions[]` by UV bounds. |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Extend `comp_d3d11_service_workspace_drain_input_events` to fill `chromeRegionId` on POINTER and POINTER_MOTION events when the hit was in chrome. Existing pattern around line 11301 — extend the per-event enrichment block. |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Remove the in-runtime chrome hit-test fields from `workspace_hit_result` (`in_title_bar`, `in_grip_handle`, `in_close_btn`, `in_minimize_btn`, `in_maximize_btn`). The runtime no longer interprets chrome regions; the controller does. Cursor logic + LMB / RMB drag start in `comp_d3d11_service_render_pass` becomes content-only. Drag-from-grip semantics moves to the shell (it knows which `chromeRegionId` is the grip). |
| `src/xrt/targets/shell/main.c` | On POINTER LMB-down with `chromeRegionId == GRIP`, start a window-drag animation. On RMB-down with grip, start rotation. On click of close / min / max region IDs, invoke the matching lifecycle request (Phase 2.K already shipped these). |

**Acceptance:** Drag from the grip dots still moves windows. Close / min / max button clicks still work. RMB drag rotates only on grip. All chrome interactivity now driven from the shell, not the runtime.

### Commit 5 — Remove default in-runtime chrome render

| File | Edit |
|---|---|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Delete the chrome render block (lines ~7290–7800, the whole `if (mc->clients[s].client_type != CLIENT_TYPE_CAPTURE) { … }` body for chrome). Keep the focus-rim glow if shell hasn't migrated it yet, or move it too. |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Delete `slot_chrome_fade_*`, `WORKSPACE_CHROME_FADE_*_NS`, `BTN_INSET_FRAC`, `PILL_W_FRAC`, `GLYPH_Y_BIAS_PX` and friends. |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Delete the in-runtime hover-detection block that seeds chrome_fade per slot (around line 5824). |
| `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` | Remove `chrome_alpha` from `BlitConstants` if no remaining call site references it (search). |

**Acceptance:** Chrome only visible when shell submits a chrome swapchain. Without shell, clients show as bare quads. Visual + behavioural smoke tests still pass with shell running.

### Commit 6 — Verification + docs

| File | Edit |
|---|---|
| `test_apps/workspace_minimal_d3d11_win/main.cpp` | Add a chrome-swapchain smoke that exercises create / set-layout / draw / submit / destroy. Verify hit-test reports `chromeRegionId` correctly. |
| `docs/specs/XR_EXT_spatial_workspace.md` | Document the chrome-swapchain surface at spec_version 7. |
| `docs/architecture/separation-of-concerns.md` | Update — chrome appearance is now controller-owned. |
| `docs/roadmap/spatial-workspace-extensions-plan.md` | Mark Phase 2.C ✅ shipped. |
| `docs/roadmap/spatial-workspace-extensions-phase2-audit.md` | Phase 2.C entry summarising what got moved out of the runtime. |

**Acceptance:** All deliverables green per acceptance criteria from each commit. Standalone test app + workspace test app + 2-cube interactive smoke all pass.

## Critical files to modify

Public surface:
- `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` (spec_version bump 6 → 7)
- `src/xrt/ipc/shared/proto.json` (wire format)
- `src/xrt/state_trackers/oxr/oxr_workspace.c` (dispatch)
- `src/xrt/state_trackers/oxr/oxr_api_negotiate.c` (PFN entries)

Runtime side:
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (chrome swapchain create + composite + hit-test extension; deletion of default chrome render block in commit 5)
- `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` (cleanup chrome_alpha if unused)

Controller side:
- `src/xrt/targets/shell/shell_openxr.h`, `shell_openxr.cpp` (PFN resolution for the three new functions)
- `src/xrt/targets/shell/main.c` (chrome render module — port the pill design here)

Test + docs:
- `test_apps/workspace_minimal_d3d11_win/main.cpp` (chrome swapchain smoke)
- `docs/specs/XR_EXT_spatial_workspace.md`, `docs/architecture/separation-of-concerns.md`, roadmap docs

## Existing utilities to reuse

- **`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`** texture pattern — already used for IPC client compositor swapchains (`comp_d3d11_service.cpp` — search for `SHARED_NTHANDLE`). Same exact wire pattern.
- **`project_local_rect_for_eye`** — projects a window-local-meters rect through the eye to atlas pixels; works identically for the chrome quad.
- **`blit_set_perspective_depth` / `blit_set_axis_aligned_depth`** — chrome bias toward the eye.
- **`workspace_raycast_hit_test`** — extend with chrome quad ray-cast loop.
- **`d3d11_icon_load_from_file` / `d3d11_icon_load_from_memory`** — controller-side icon load now happens in the shell (already a pattern there for launcher icons).

## Phase 2.K hard-won lessons (apply to 2.C)

Carry over from 2.G / 2.K. Most relevant for 2.C:

1. **`feedback_controllers_own_motion`** — the architectural North Star. Chrome appearance is policy. If implementation pressure tempts moving it back into the runtime, stop and re-read.
2. **`xrEnumerateWorkspaceClientsEXT` includes the controller's own session** — filter by PID match. The shell's chrome rendering module must skip the shell itself (it has no chrome).
3. **Display dimensions** come from `XR_EXT_display_info` via `g_xr->display_*_m`. Chrome geometry math in the shell must read from there, not constants.
4. **Connect-time race** — chrome swapchain creation may race with the slot binding. The shell's "open chrome swapchain on connect" path must retry like `s_auto_tile_pending` does for poses.
5. **Always build via `scripts\build_windows.bat build`** — never invoke `cmake` / `ninja` directly on Windows.
6. **Atlas screenshot trigger** — `touch %TEMP%\workspace_screenshot_trigger` produces `%TEMP%\workspace_screenshot_atlas.png`. Use this to verify visual parity at commit 3.
7. **Per `feedback_dll_version_mismatch.md`** — after every runtime build, copy `_package/bin/{DisplayXRClient.dll, displayxr-service.exe, displayxr-shell.exe, displayxr-webxr-bridge.exe}` to `C:\Program Files\DisplayXR\Runtime\` (elevated). Strict git-tag check between client and service.
8. **Per `feedback_test_before_ci.md`** — build + smoke locally, ask the user to test, then commit.

## Risks

- **Per-frame chrome render in shell** — the shell would re-render chrome only on state change (hover/focus), not per-frame. If a future feature needs per-frame chrome animation (e.g. spinning loading indicator), the shell can either re-render at 60 Hz (acceptable budget per the perf analysis) or use a shader uniform driven by `xrSetWorkspaceClientChromeOpacityEXT`-style escape hatches added later.
- **Multiple chrome quads per client** — current design is one swapchain per client. If a future feature needs a separate swapchain for the focus glow vs the pill (different sizes, different update cadences), the API supports this (one client can host multiple chrome swapchains, each with its own layout). Not in scope for 2.C.
- **Chrome-on-rotation rendering** — `followsWindowOrient = XR_TRUE` makes the chrome rotate with the window in immersive / carousel modes. The runtime must apply the rotation in its composite path (multiply the chrome's `poseInClient.orientation` by the window's `window_pose.orientation`). Verify with atlas screenshot at commit 2.

## Hand-off

- **Don't merge to main yet.** Branch sequence (`feature/workspace-extensions-2K` → `feature/workspace-extensions-2C`) stays in flight until both are stable.
- Wire-format compatibility: `spec_version` bump 6 → 7 makes the IPC contract explicit. Don't mix Phase 2.C controller binaries with Phase 2.K runtime binaries.
- Phase 2.C closes the chrome-ownership gap. After it ships, the runtime owns zero pixels of UI policy — only mechanism. This is the architectural endpoint for the workspace extensions effort.
