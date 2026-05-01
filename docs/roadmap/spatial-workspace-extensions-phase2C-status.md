# Phase 2.C Status: Controller-Owned Chrome

**Branch:** `feature/workspace-extensions-2C` (off `feature/workspace-extensions-2K` tip)
**Status:** C1, C2, C3.A, C3.B committed. C3.B visual blocked on cross-process texture-sharing bug — controller's red verify color does not appear in the atlas despite the runtime composite path entering with a valid SRV. Pause for fresh eyes before C3.C.
**Date:** 2026-04-30

## Scope

Lift the floating-pill chrome (pill bg, grip dots, close/min/max buttons, app icon, glyphs, focus rim glow, hover-fade) from the runtime (`comp_d3d11_service.cpp:7297–7889`) to the workspace controller. After Phase 2.C the runtime owns zero pixels of UI policy — only mechanism (cross-process texture sharing, atlas composite at a 3D pose, depth pipeline, hit-test plumbing).

**Full plan:** [spatial-workspace-extensions-phase2C-plan.md](spatial-workspace-extensions-phase2C-plan.md)
**Agent prompt:** [spatial-workspace-extensions-phase2C-agent-prompt.md](spatial-workspace-extensions-phase2C-agent-prompt.md)
**Implementation plan:** `~/.claude/plans/read-docs-roadmap-spatial-workspace-exte-giggly-fountain.md`

## Tasks

| Status | Sub-step | Description |
|--------|----------|-------------|
| [x] | C1 | Public surface bump 6→7 — header, IPC schema, dispatch stubs, shell PFN resolution |
| [x] | C2 | Runtime imports + composites chrome swapchain (additive — old chrome still draws) |
| [x] | C3.A | Shell adds D3D11 graphics binding so chrome swapchain create works |
| [~] | C3.B | Controller-rendered chrome via swapchain — pipeline working, **visual not appearing** |
| [ ] | C3.C | Port full pill visual — rounded shader, grip dots, buttons, icon, glyphs, focus rim, hover-fade |
| [ ] | C4 | Hit-test plumbing — `chromeRegionId` on POINTER events; controller drives chrome region semantics |
| [ ] | C5 | Delete in-runtime chrome render block + animation machinery |
| [ ] | C6 | Test app smoke + spec/audit docs |

## Commits

- `3bd941e43` runtime + shell: Phase 2.C C1 — public surface bump for controller-owned chrome
- `b9f073195` runtime: Phase 2.C C2 — wire chrome swapchain composite path
- `f24ac89d6` shell: Phase 2.C C3.A — D3D11 graphics binding for chrome rendering
- `e2984065f` shell: Phase 2.C C3.B — controller-rendered chrome via swapchain
- (uncommitted, debugging C3.B visual) — id=0 fix, src_rect=pixels fix, d3d11 unwrap helper, Flush() in shell, debug logging, red verify color

## Design Decisions

- **Workspace-controller flag is auto-set when `XR_EXT_spatial_workspace` is enabled.** `oxr_session.c` flips `xsi.is_workspace_controller = true` for any session with the extension, before the graphics-binding paths run. The shell can have a D3D11 graphics binding (needed for chrome swapchain create) and still skip slot registration in the multi-compositor — no phantom tile inside the workspace it controls. Verified with the 2-cube smoke ("Layout 'grid' (2 windows)").
- **Chrome swapchain is just an `XrSwapchain`.** `xrCreateWorkspaceClientChromeSwapchainEXT` builds an `XrSwapchainCreateInfo` from the chrome createInfo and calls `sess->create_swapchain` — the standard image-loop path. The runtime's IPC swapchain id (a slot index in the controller's `xscs[]` table, range 0..N-1) is registered as chrome via `workspace_register_chrome_swapchain`. Acquire / Wait / Release reuse the existing OpenXR swapchain entry points.
- **id 0 is a valid swapchain id.** Initial dispatch wrapper rejected id==0 as "unresolved" — wrong, the first slot in the IPC `xscs[]` table is id 0 and the shell's chrome (its only swapchain) gets that id. Fixed: dropped the sentinel guard; runtime side-table matching now requires `chrome_xsc != nullptr` to disambiguate "no chrome registered" from "chrome at id 0".
- **D3D11 wrapper unwrap.** `sc->swapchain` for a D3D11-binding session is a `client_d3d11_swapchain` wrapping an `ipc_client_swapchain`. Direct cast to `ipc_client_swapchain*` returns garbage. Added `comp_d3d11_client_get_inner_xrt_swapchain` to unwrap before reading the IPC id.
- **`src_rect` is in source-texture pixels, not normalized.** Shader does `src_pos = src_rect.xy + uv * src_rect.zw; output.uv = src_pos / src_size`. C2's chrome blit set `src_rect = (0,0,1,1)`, sampling a 1-pixel corner of the 512×64 chrome image. Fixed.
- **Per-tick chrome work skipped when slot already chromed.** `shell_chrome_has(id)` short-circuits the lazy retry loop in `main.c`, and `shell_chrome_on_client_connected` fast-paths existing slots (only re-pushes layout if window size changed). Slot-anim transitions stay smooth — no per-tick `get_pose` + `set_chrome_layout` IPC traffic competing with `set_pose`.

## Open Bug — C3.B Visual

**Symptom:** controller's `ClearRenderTargetView(rtv, verify_red)` (in `shell_chrome.cpp:render_pill`) does not appear in the atlas. The in-runtime chrome (still drawing until C5) is visible; the controller's red rectangle that should overlay it is not.

**Confirmed working from logs:**
- `shell_chrome: chrome ready for client 2 (window 0.139×0.156 m, pill 0.104×0.008 m)` — swapchain created.
- Server: `Workspace: register_chrome_swapchain client_id=2 swapchain_id=0 → slot=0` — registration succeeded.
- Server: `chrome composite check slot=0: xsc=…550 layout_valid=1 type=0` — slot has chrome.
- Server: `chrome composite slot=0 csc=…550 image_count=1 srv=…A28` — SRV is non-null, Draw is issued.

**Already tried:**
- Fixed id=0 sentinel guards in dispatch + runtime.
- Fixed `src_rect` to be in source-texture pixels.
- Added `ID3D11DeviceContext::Flush()` after `ClearRTV` so GPU work is submitted before `xrReleaseSwapchainImage` signals the keyed mutex.

**Hypotheses for next session:**
1. **Cross-process texture sync.** The controller's writes via the shell's D3D11 device aren't visible on the runtime's D3D11 device despite SHARED_NTHANDLE + KEYEDMUTEX. Possible the keyed-mutex acquire/release dance during `xrWaitSwapchainImage` / `xrReleaseSwapchainImage` isn't completing the way the runtime expects.
2. **Geometry / depth.** Chrome quad ends up outside the visible atlas region or is depth-tested out by the in-runtime chrome (which should NOT happen given LESS_EQUAL + same depth bias, but worth verifying).
3. **In-runtime chrome paints over.** The runtime's existing chrome block runs BEFORE the new controller composite (line 7297 vs 7930), so this should not happen — but worth confirming with a hardcoded-color override.

**Bisect plan for next session:**
- Override the runtime's chrome blit shader to ignore the SRV and emit a hardcoded color when chrome is registered. If hardcoded color appears at the chrome quad position → SRV/sampling bug. If it doesn't appear → geometry/depth/visibility bug.
- If SRV-side: take a RenderDoc capture of the runtime, inspect the chrome SRV contents — confirm the controller's red bytes actually landed in the shared texture.
- If geometry-side: log the cc_corners[] values and verify they fall within the atlas's expected tile bounds.

## Files Touched (Phase 2.C across all sub-steps)

Public surface:
- `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h`
- `src/xrt/ipc/shared/proto.json`
- `src/xrt/ipc/shared/ipc_protocol.h`
- `src/xrt/state_trackers/oxr/oxr_workspace.c`
- `src/xrt/state_trackers/oxr/oxr_api_funcs.h`
- `src/xrt/state_trackers/oxr/oxr_api_negotiate.c`
- `src/xrt/state_trackers/oxr/CMakeLists.txt`
- `src/xrt/state_trackers/oxr/oxr_session.c`

Runtime:
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.h`
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`
- `src/xrt/compositor/client/comp_d3d11_client.cpp`
- `src/xrt/ipc/client/ipc_client_compositor.c`
- `src/xrt/ipc/server/ipc_server_handler.c`

Shell:
- `src/xrt/targets/shell/shell_openxr.h`
- `src/xrt/targets/shell/shell_openxr.cpp`
- `src/xrt/targets/shell/shell_chrome.h` (new)
- `src/xrt/targets/shell/shell_chrome.cpp` (new)
- `src/xrt/targets/shell/main.c`
- `src/xrt/targets/shell/CMakeLists.txt`

## Hand-off

- Branch sequence (`2G → 2K → 2C`) stays in flight. Don't merge to main.
- C3.B chrome rendering bug needs fresh eyes — see "Bisect plan" above. Once C3.B's visual works, C3.C ports the full visual (~800–1200 lines of D3D11/HLSL) for the rounded pill, grip dots, button geometry, app icon, DirectWrite glyphs, focus rim glow, hover-fade ease-out cubic.
- Per `feedback_test_before_ci.md`: any new agent / session continuing here must build + smoke locally before pushing.
