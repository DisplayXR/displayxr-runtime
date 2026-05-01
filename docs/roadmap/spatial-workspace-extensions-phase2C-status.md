# Phase 2.C Status: Controller-Owned Chrome

**Branch:** `feature/workspace-extensions-2C` (off `feature/workspace-extensions-2K` tip)
**Status:** C1, C2, C3.A, C3.B (visual fixed), C3.C-1, C3.C-2, C4, C3.C-4, C5 committed. Runtime now ships with **zero default chrome** — controller-owned chrome is the only chrome path. Remaining: C3.C-3 (icons + glyphs, deferred polish) → C6 (docs in progress; test app smoke deferred).
**Date:** 2026-05-01

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
| [x] | C3.B | Controller-rendered chrome via swapchain (initial pipeline) |
| [x] | C3.B-debug | Cross-process keyed-mutex acquire on read; src_rect=pixels; id=0 sentinel removal; D3D11 wrapper unwrap helper; shell Flush() after ClearRTV |
| [x] | C3.C-1 | Rounded pill bg via SDF shader, pill-space-meters geometry, anim-target initial-layout fix |
| [x] | C3.C-2 | Grip dots (4×2) + 3 circular buttons (red close, gray min, gray max) in same SDF pass |
| [x] | C4 | Hit-test plumbing — chrome quad raycast, `chromeRegionId` on POINTER / POINTER_MOTION events; shell dispatches close → exit RPC, max → fullscreen RPC |
| [x] | C3.C-4 | Hover-fade ease-out cubic + state-change re-render — per-slot fade alpha baked into chrome image, 150 ms hover-in / 300 ms hover-out, idle = zero GPU work |
| [x] | C5 | Delete in-runtime chrome render block, focus-rim glow, fade machinery (~700 lines net deletion). Runtime ships with zero default chrome. |
| [ ] | C3.C-3 | App icon (per-client PNG load + texture) + DirectWrite glyph atlas — **deferred polish** |
| [~] | C6 | Spec + separation-of-concerns + audit + plan docs (in progress); test app smoke deferred |

## Commits

- `3bd941e43` runtime + shell: Phase 2.C C1 — public surface bump for controller-owned chrome
- `b9f073195` runtime: Phase 2.C C2 — wire chrome swapchain composite path
- `f24ac89d6` shell: Phase 2.C C3.A — D3D11 graphics binding for chrome rendering
- `e2984065f` shell: Phase 2.C C3.B — controller-rendered chrome via swapchain
- `3c09adcfb` runtime + shell: Phase 2.C C3.B-debug — chrome visual fix
- `24187fa99` client D3D12: log adapter LUID + name at create — #184 diag
- `6755dacfc` shell: Phase 2.C C3.C-1 — rounded-pill chrome shader + initial-layout fix
- `1a68c81c5` shell: Phase 2.C C3.C-2 — grip dots + close/min/max buttons
- `722fbcd7e` runtime + shell: Phase 2.C C4 — chrome hit-test plumbing
- `3264496bf` shell: Phase 2.C C3.C-4 — chrome hover-fade + state-change re-render
- `522db1ad5` runtime + shell: Phase 2.C C3.C-4 follow-up — POINTER_HOVER + RMB pitch fix
- `0160c1567` runtime: Phase 2.C C5 — delete in-runtime chrome render block

## Design Decisions

- **Workspace-controller flag is auto-set when `XR_EXT_spatial_workspace` is enabled.** `oxr_session.c` flips `xsi.is_workspace_controller = true` for any session with the extension, before the graphics-binding paths run. The shell can have a D3D11 graphics binding (needed for chrome swapchain create) and still skip slot registration in the multi-compositor — no phantom tile inside the workspace it controls. Verified with the 2-cube smoke ("Layout 'grid' (2 windows)").
- **Chrome swapchain is just an `XrSwapchain`.** `xrCreateWorkspaceClientChromeSwapchainEXT` builds an `XrSwapchainCreateInfo` from the chrome createInfo and calls `sess->create_swapchain` — the standard image-loop path. The runtime's IPC swapchain id (a slot index in the controller's `xscs[]` table, range 0..N-1) is registered as chrome via `workspace_register_chrome_swapchain`. Acquire / Wait / Release reuse the existing OpenXR swapchain entry points.
- **id 0 is a valid swapchain id.** Initial dispatch wrapper rejected id==0 as "unresolved" — wrong, the first slot in the IPC `xscs[]` table is id 0 and the shell's chrome (its only swapchain) gets that id. Fixed: dropped the sentinel guard; runtime side-table matching now requires `chrome_xsc != nullptr` to disambiguate "no chrome registered" from "chrome at id 0".
- **D3D11 wrapper unwrap.** `sc->swapchain` for a D3D11-binding session is a `client_d3d11_swapchain` wrapping an `ipc_client_swapchain`. Direct cast to `ipc_client_swapchain*` returns garbage. Added `comp_d3d11_client_get_inner_xrt_swapchain` to unwrap before reading the IPC id.
- **`src_rect` is in source-texture pixels, not normalized.** Shader does `src_pos = src_rect.xy + uv * src_rect.zw; output.uv = src_pos / src_size`. C2's chrome blit set `src_rect = (0,0,1,1)`, sampling a 1-pixel corner of the 512×64 chrome image. Fixed.
- **Per-tick chrome work skipped when slot already chromed.** `shell_chrome_has(id)` short-circuits the lazy retry loop in `main.c`, and `shell_chrome_on_client_connected` fast-paths existing slots (only re-pushes layout if window size changed). Slot-anim transitions stay smooth — no per-tick `get_pose` + `set_chrome_layout` IPC traffic competing with `set_pose`.
- **Runtime acquires the keyed mutex when reading the chrome SRV.** Service-created swapchains' `swapchain_wait_image` is a no-op server-side; the runtime is the reader and must `IDXGIKeyedMutex::AcquireSync(0)` itself before `PSSetShaderResources` + `Draw`. Hoisted above the per-view loop — one acquire/release per composite tick. Without this, cross-process GPU writes from the shell's D3D11 device are not visible on the runtime's D3D11 device. Was the C3.B "visual not appearing" root cause.
- **Pill SDF in pill-space meters.** Chrome image is fixed 512×64 px sRGB. The pill quad it composites onto can be any aspect (typical: 16:1). Rasterizing the SDF in pill-space meters (passed via cbuffer) keeps corners + button circles geometrically correct under arbitrary stretch. Single-pass shader composes pill bg, grip dots, and 3 buttons via Porter-Duff "src over" — back-to-front, straight-alpha.
- **Anim-target dims for initial chrome layout.** When chrome is created in the lazy retry loop while a slot animation is still in flight (e.g. the auto grid preset just seeded but hasn't settled), `shell_slot_anim_get_target` returns the animation's destination dims. Without this the chrome locked in mid-glide / pre-preset dims and rendered at the wrong size for a different window. Closes the C3.B-debug position artifact.

## Open issues

- **C3.B-debug commit left a behavior change in the keyed-mutex AcquireSync timeout** (currently 4 ms). If the shell's GPU is slow to flush its writes, the runtime's acquire could time out and the chrome blit silently uses stale texture content. Worth instrumenting with a one-shot warn-log if the acquire ever fails. Not yet observed.
- **In-runtime chrome still draws** (additive composite). The runtime currently renders BOTH the in-runtime pill AND the controller-submitted pill. The depth bias + draw order means the controller pill wins where it overlaps. Both pills now fade in/out together (since C3.C-4 follow-up routed the same hover signal to both). C5 deletes the in-runtime path entirely.
- **Focus-rim glow is rectangular** — pre-existing runtime-side polish item (visible since Phase 2.K). The blue inward halo around the focused window's content quad doesn't follow the rounded corners. Lives in the runtime's render block (not in the controller chrome). Address as a runtime polish pass independent of Phase 2.C, or fold into C5 when the in-runtime chrome render block is being touched anyway.

## Next-step plan (session order)

**C5 — Delete in-runtime chrome render block + animation machinery.** ~600 lines of deletions in `comp_d3d11_service.cpp`: the chrome render block (~7297–7889), `slot_chrome_fade_*` machinery (5327–5368), `chrome_alpha` cbuffer field, and the in-runtime hit-test fields on `workspace_hit_result` (in_close_btn, in_minimize_btn, in_maximize_btn, in_grip_handle, in_title_bar) once the runtime's cursor + drag logic that consumes them is migrated to the shell or deleted. Validates the architecture: runtime ships with zero default chrome, only the controller-submitted version remains. Without a controller, clients show as bare quads.

After C5: `hit_result_to_region` returns `XR_WORKSPACE_HIT_REGION_CHROME_EXT` (= 6) for chrome-quad hits, dropping the legacy CLOSE_BUTTON / MIN / MAX / TITLE_BAR codes. Controllers disambiguate via `chromeRegionId`.

**C3.C-3 — App icon + DirectWrite glyphs (deferred polish).** App icon: per-client PID → exe → registered-app icon_path lookup, PNG decode via stb_image, D3D11 texture + SRV, shader extension to sample. Glyphs: heavy infra (DirectWrite atlas baking, ~400–600 lines port from runtime). Both are pure visual polish — not architectural — and the existing pill + dots + buttons is enough to demonstrate the controller-owned chrome architecture works end-to-end. Land in a follow-up session.

**C6 — Test app smoke + spec/audit docs.** chrome smoke in `workspace_minimal_d3d11_win`; spec doc update for `XR_EXT_spatial_workspace` v7; separation-of-concerns doc update; audit entry; mark Phase 2.C ✅ shipped.

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
- All major architectural pieces are in place: chrome image authored controller-side, runtime composites it via cross-process keyed-mutex, hit-test fills `chromeRegionId`, shell handles close + max + hover-fade. The remaining work (C5, C3.C-3, C6) is cleanup + polish + docs.
- **Verification before C5**: live-test on the LP-3D unit. Move cursor over a window's chrome → that pill should fade IN to full opacity (~150 ms); other windows' chromes should fade OUT (~300 ms). Click close button → window should exit. Click max button → window toggles fullscreen. Once verified, C5 deletes the in-runtime chrome path and only the controller path remains.
- Per `feedback_test_before_ci.md`: any new agent / session continuing here must build + smoke locally before pushing.
