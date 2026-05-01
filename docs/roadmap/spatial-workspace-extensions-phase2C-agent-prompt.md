# Phase 2.C Agent Prompt — Controller-owned chrome

Self-contained prompt for a fresh agent session implementing Phase 2.C of the workspace-extensions effort. Drop into a new session as the user message after `/clear`. The agent has no memory of the prior design conversations — this prompt assumes nothing.

---

## What Phase 2.C is for

You're picking up Phase 2.C — the **controller-owned chrome** sub-phase. Phase 2.K shipped the floating-pill chrome design (pill bg, grip dots, close/min/max buttons, app icon, glyphs, focus rim glow, hover-fade) **inside the runtime** at `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`. That worked as a visual checkpoint but locks every pixel of UI to the runtime's release cycle and breaks the architectural North Star already established for motion (`feedback_controllers_own_motion`): the runtime owns mechanism, the controller owns policy + appearance.

Phase 2.C's job: add a public extension surface that lets a workspace controller submit a per-client **chrome swapchain** (a 2D image the controller draws to). The runtime composites it onto the workspace at a controller-specified pose, with controller-specified hit regions. After 2.C the runtime draws **zero chrome by default** — chrome is only visible when the controller submits it.

Migration must be visually lossless: the DisplayXR Shell ports the existing pill design to the controller side, and at commit-3 the user sees no perceptible visual change. From that point on, the shell can iterate on art / colors / behavior without touching the runtime.

After 2.C the runtime owns zero pixels of UI policy. This is the architectural endpoint for the workspace-extensions effort.

## Read these in order before touching code

1. **`docs/roadmap/spatial-workspace-extensions-phase2C-plan.md`** — the design doc. Surface design, six-commit shape, acceptance criteria, perf risk analysis. **Read it first end-to-end.**
2. **Memory file `feedback_controllers_own_motion`** — durable design principle from the user. **Internalise this before designing.** Chrome appearance is policy. Runtime never owns it.
3. **`docs/roadmap/spatial-workspace-extensions-phase2K-plan.md`** + **`-agent-prompt.md`** — Phase 2.K context. The chrome design we're moving was implemented there.
4. **`docs/roadmap/spatial-workspace-extensions-plan.md`** — master plan; section on Phase 2.C explains why it lands after 2.K.
5. **`docs/roadmap/spatial-workspace-extensions-phase2-audit.md`** — Phase 2.G + 2.K entries show the migration shape (extension surface bump, IPC schema, dispatch, then runtime emission, then controller adoption, then default-removal).
6. **Code reads (~60 minutes):**
   - `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — find the chrome render block (search `Phase 2.K Commit 8.B: floating-pill geometry`, around line ~7290–7800). This is what you're moving to the controller. Read it end-to-end so you know exactly which features need to land in the shell.
   - Same file — search `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`. The IPC client-swapchain pattern is your model for the chrome swapchain.
   - Same file — `workspace_raycast_hit_test` (around line 4960). You'll extend it to ray-cast chrome quads in addition to content quads.
   - Same file — `comp_d3d11_service_workspace_drain_input_events` (around line 11301). You'll extend the per-event enrichment to fill `chromeRegionId`.
   - `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` — current spec_version 6. You'll bump to 7.
   - `src/xrt/ipc/shared/proto.json` — wire format. The existing `workspace_*` entries are your model for the three new RPCs.
   - `src/xrt/state_trackers/oxr/oxr_workspace.c` — dispatch wrappers; existing entries are the model.
   - `src/xrt/targets/shell/main.c` — the controller. The chrome render module you'll add lives here. Search `shell_apply_preset` for the pose-update model — the chrome render hooks into the same lifecycle.
   - `src/xrt/targets/shell/shell_openxr.h`, `shell_openxr.cpp` — PFN resolution. Three new functions to wire.

## Branch + prerequisites

- **Start from the current tip of `feature/workspace-extensions-2K`**, not main. Phase 2.K isn't merged yet — the user is keeping the branch sequence (`2G → 2K → 2C`) in flight until 2.C is stable. Confirm tip with `git log --oneline -1 feature/workspace-extensions-2K` (should be `af412eb76 runtime: 8.E + 8.G — app icon at pill left, tilt-aware inner rim glow` or later).
- **New branch:** `feature/workspace-extensions-2C` off `feature/workspace-extensions-2K`'s tip.
- Per `feedback_branch_workflow.md`: never work on main directly; new branch off the current development tip is correct here.
- After every runtime build: copy `_package/bin/{DisplayXRClient.dll, displayxr-service.exe, displayxr-shell.exe, displayxr-webxr-bridge.exe}` to `C:\Program Files\DisplayXR\Runtime\` (elevated PowerShell) per `feedback_dll_version_mismatch.md`. Elevated processes ignore `XR_RUNTIME_JSON`, so the registry-installed runtime is what tests load.
- Always build via `scripts\build_windows.bat build` per `feedback_use_build_windows_bat.md`. Never invoke cmake / ninja directly. If the script fails, flag and fix the script, don't bypass.
- Per `feedback_test_before_ci.md`: build + smoke locally, ask the user to test, then commit. Don't `/ci-monitor` until the user has signed off.

## Phase 2.K hard-won lessons (apply to 2.C)

These are failures from 2.G / 2.K that wasted significant debug time. Do not re-create them in 2.C.

1. **`xrEnumerateWorkspaceClientsEXT` includes the controller's own session** — the shell's chrome render module must filter by `cinfo.pid == GetCurrentProcessId()`. The shell has no chrome.

2. **Display dimensions** must come from `XR_EXT_display_info` via `g_xr->display_*_m`. Chrome geometry math in the shell must read from there, not from hardcoded LP-3D fallbacks (0.700 × 0.394 m is wrong; on the actual unit it's 0.344 × 0.194 m).

3. **Connect-time race**: `xrEnumerateWorkspaceClientsEXT` returns a client ID a few ticks before the per-client compositor slot is bound. `xrCreateWorkspaceClientChromeSwapchainEXT` against an unbound slot may return validation failure; retry like `s_auto_tile_pending` does for poses.

4. **Slot entry animation**: the runtime starts a 300 ms entry animation when a client connects. Chrome submission during that window must coexist (the chrome composites at the layout's `poseInClient`, which is window-relative, so the entry animation just shifts the window — chrome follows).

5. **Per-process keyboard input does NOT reach the workspace WndProc reliably from `SendInput`** when the workspace doesn't have foreground focus. The shell's `displayxr_preset_{grid,immersive,carousel}` file-trigger mechanism (in `main.c`) is a debug back-door. Keep it for 2.C smoke tests.

6. **Atlas screenshot trigger**: per `reference_runtime_screenshot.md`, `touch %TEMP%\workspace_screenshot_trigger` produces `%TEMP%\workspace_screenshot_atlas.png`. Use this to verify visual parity at commit 3 (post-shell-port should look identical to pre-2.C). Per `feedback_shell_screenshot_reliability.md`, eye-tracking warmup can affect chrome rendering — wait ~5 seconds after launch before screenshotting.

7. **Atlas stride invariant** (`feedback_atlas_stride_invariant.md`) — slot stride = `atlas_width / tile_columns`, NOT `sys->view_width`. The chrome quad composite must respect the same invariant.

8. **`feedback_controllers_own_motion`** is the architectural North Star. Chrome appearance is policy. If implementation pressure tempts moving it back into the runtime, stop and re-read.

## What ships in Phase 2.C

The plan doc has detailed scope tables; this is the execution-order summary.

### Public surface changes (`XR_EXT_spatial_workspace.h`, spec_version 6 → 7)

- New struct `XrWorkspaceChromeSwapchainCreateInfoEXT` — pixel format, width, height, sample count.
- New struct `XrWorkspaceChromeLayoutEXT` — `poseInClient`, `sizeMeters`, `followsWindowOrient`, hit region array, depth bias.
- New struct `XrWorkspaceChromeHitRegionEXT` — region ID, UV bounds in [0,1]^2.
- New PFN typedefs + prototypes for `xrCreateWorkspaceClientChromeSwapchainEXT`, `xrDestroyWorkspaceClientChromeSwapchainEXT`, `xrSetWorkspaceClientChromeLayoutEXT`.
- New enum value `XR_WORKSPACE_HIT_REGION_CHROME_EXT`.
- New field `chromeRegionId` on `pointer` and `pointerMotion` event payloads.

### Runtime additions

- **`comp_d3d11_service.cpp`** — chrome swapchain create / import (SHARED_NTHANDLE pattern), per-slot `chrome_swapchain` field, layout storage, composite helper invoked from per-view loop. Hit-test extension to ray-cast chrome quads.
- **`oxr_workspace.c`** — dispatch wrappers for the three new functions.
- **`ipc/shared/proto.json`** — wire format for new RPCs + new event payload field. Regenerate IPC stubs.

### Runtime deletions (commit 5)

- The chrome render block in `comp_d3d11_service.cpp` (~lines 7290–7800).
- `slot_chrome_fade_*`, `WORKSPACE_CHROME_FADE_*_NS`, `BTN_INSET_FRAC`, `PILL_W_FRAC`, `GLYPH_Y_BIAS_PX`, etc.
- The in-runtime hover-detection block that seeds chrome_fade per slot (around line 5824).
- In-runtime chrome hit-test fields (`in_title_bar`, `in_grip_handle`, `in_close_btn`, …) on `workspace_hit_result`.
- `chrome_alpha` field in `BlitConstants` (if no remaining call site).

### Shell additions

- **PFN resolution** — three new functions resolved in `shell_openxr_init`.
- **Per-client chrome render module** in `main.c`:
  - Open chrome swapchain on connect.
  - In-process D3D11 render to chrome image: pill bg + grip dots + close/min/max buttons + app icon + glyphs + focus rim glow.
  - Re-render only on state change (hover, focus, button-hover).
  - Hover-fade ease-out cubic owned here; modulates the chrome image's alpha at draw time (or via a future `xrSetWorkspaceClientChromeOpacityEXT` if perf demands — see plan doc).
- **Layout submission** — `xrSetWorkspaceClientChromeLayoutEXT` on connect + on every preset switch (carousel changes the layout).
- **Hit-region wiring** — controller defines region IDs (e.g. `GRIP=1`, `CLOSE=2`, `MINIMIZE=3`, `MAXIMIZE=4`, `ICON=5`, `BACKGROUND=6`); on POINTER events, dispatch by `chromeRegionId`.

## Six-commit sequence

Same shape as 2.G / 2.K. Each commit ends with a green build + a discrete acceptance check.

### Commit 1 — Public surface bump
Header + IPC schema + dispatch stubs that return `XR_SUCCESS` and do nothing. Build green. PFN count 24 → 27. No behaviour change.

### Commit 2 — Runtime imports + composites chrome swapchain
Implement create / import (SHARED_NTHANDLE), per-slot storage, composite helper, layout setter. The in-runtime chrome still draws — this commit just **adds** the new path. Test app smoke: solid-color chrome bar at 4 mm above test client, atlas screenshot shows it.

### Commit 3 — Shell ports the floating-pill design to the controller
Port the visual design verbatim from runtime → shell. Hover fade, button states, glyph rendering, app icon, focus rim glow all move. Gate the in-runtime chrome render block on `slot->chrome_swapchain == nullptr` so chrome doesn't double-render. **Visual parity check**: atlas screenshot at this commit should be perceptually identical to the day-2.K-merge screenshot.

### Commit 4 — Hit-test plumbing
Extend `workspace_raycast_hit_test` to ray-cast chrome quads. Fill `chromeRegionId` on POINTER / POINTER_MOTION events. Remove in-runtime chrome hit-test fields from `workspace_hit_result`. Move drag-from-grip / RMB-rotate / button-click logic to the shell.

### Commit 5 — Remove default in-runtime chrome render
Delete the chrome render block, fade machinery, hit-test fields, hover-detection block. Without shell, clients show as bare quads. With shell, behaviour unchanged from commit 4.

### Commit 6 — Verification + docs
Test app smoke, spec doc update, separation-of-concerns doc update, audit doc, plan doc ✅ shipped.

## Critical files to modify

Public surface:
- `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` (spec_version 6 → 7)
- `src/xrt/ipc/shared/proto.json`
- `src/xrt/state_trackers/oxr/oxr_workspace.c`
- `src/xrt/state_trackers/oxr/oxr_api_negotiate.c`

Runtime:
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (additions in commits 2–4, deletions in commit 5)
- `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` (cleanup in commit 5)

Controller:
- `src/xrt/targets/shell/shell_openxr.h`, `shell_openxr.cpp`
- `src/xrt/targets/shell/main.c`

Test + docs:
- `test_apps/workspace_minimal_d3d11_win/main.cpp`
- `docs/specs/XR_EXT_spatial_workspace.md`
- `docs/architecture/separation-of-concerns.md`
- `docs/roadmap/spatial-workspace-extensions-plan.md`
- `docs/roadmap/spatial-workspace-extensions-phase2-audit.md`

## Existing utilities to reuse

- **`D3D11_RESOURCE_MISC_SHARED_NTHANDLE` texture pattern** — already used for IPC client compositor swapchains. Search `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` in `comp_d3d11_service.cpp`. Same exact wire pattern: create texture with the misc flag, query `IDXGIResource1::CreateSharedHandle`, hand the NT handle to the controller via the swapchain wire format, controller imports via `OpenSharedResource1`.
- **`project_local_rect_for_eye`** — projects window-local-meters rect through eye to atlas pixels. Works identically for the chrome quad since chrome's `poseInClient` lives in the same window-local space as content.
- **`blit_set_perspective_depth` / `blit_set_axis_aligned_depth`** — chrome bias toward eye via `WORKSPACE_CHROME_DEPTH_BIAS`.
- **`workspace_raycast_hit_test`** — extend with chrome quad ray-cast loop. The in-runtime hit-test fields go away in commit 4 / 5.
- **`d3d11_icon_load_from_file` / `d3d11_icon_load_from_memory`** — controller-side icon load now happens in the shell (already a pattern there for launcher icons; the shell already loads the same SRVs).

## Build + deploy

- Always build via `scripts\build_windows.bat build` (then `test-apps` if needed). Never invoke cmake / ninja directly on Windows.
- After every runtime build: copy `_package/bin/{DisplayXRClient.dll, displayxr-service.exe, displayxr-shell.exe, displayxr-webxr-bridge.exe}` to `C:\Program Files\DisplayXR\Runtime\` (elevated). Per `feedback_dll_version_mismatch.md`: every rebuild bakes a new `u_git_tag`; `ipc_client_connection.c` does strict strncmp — drift breaks all clients.
- Per `feedback_test_before_ci.md`: build + smoke locally, ask user to test, then commit. Don't `/ci-monitor` until user signs off.

## Verification (end-to-end)

After each commit:
1. `scripts\build_windows.bat build` (and `test-apps` for commits that touch the test app).
2. Copy binaries to `Program Files\DisplayXR\Runtime\`.
3. Run `_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d12_win\build\cube_handle_d3d12_win.exe` (two cubes for hit-test variety).
4. For commit 3: trigger atlas screenshot via `%TEMP%\workspace_screenshot_trigger`, compare to a baseline screenshot taken at the day-2.K-merge state. Should be perceptually identical.
5. For commit 4: hover, click, drag from grip, RMB rotate, click close / min / max — all should still work.
6. For commit 5: launch shell-less (skip the shell, just the service + a cube) — cube should render bare with no chrome. With shell — chrome restored.
7. Ask the user to verify on the LP-3D unit before committing.

End-of-phase acceptance per plan doc:
- ✅ Spec_version 7 surface lands clean.
- ✅ Visual parity at commit 3 vs day-2.K state.
- ✅ All chrome interactivity (hover, drag, button clicks, RMB rotate) driven from the shell.
- ✅ Runtime renders zero chrome by default; bare cube without shell.
- ✅ Atlas-screenshot smoke + 2-cube interactive smoke + workspace_minimal_d3d11_win smoke all pass.
- ✅ Idle CPU near zero (chrome doesn't re-render unless state changes); animation CPU under 1% of one core.

## Hand-off

- **Don't merge to main yet.** Branch sequence (`2G → 2K → 2C`) stays in flight until 2.C is stable.
- Wire-format compatibility: `spec_version` bump 6 → 7 makes the IPC contract explicit. Don't mix Phase 2.C controller binaries with Phase 2.K runtime binaries.
- Phase 2.C closes the chrome-ownership gap. The runtime owns zero pixels of UI policy after this — only the mechanism (cross-process texture sharing, atlas composite at a 3D pose, depth pipeline, hit-test plumbing).
- Follow-up risk: if a real consumer needs the old chrome (no controller), Phase 2.C-followup can ship a `default_chrome.cpp` source the shell `#includes` and submits verbatim. Not in 2.C scope.
