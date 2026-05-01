# Phase 2.C Status: Controller-Owned Chrome

**Branch:** `feature/workspace-extensions-2C` (off `feature/workspace-extensions-2K` tip)
**Status:** C1, C2, C3.A, C3.B (visual fixed), C3.C-1 (rounded pill bg), C3.C-2 (grip dots + 3 buttons) committed. Next: C4 (hit-test plumbing) → C3.C-4 (hover-fade in shell) → C3.C-3 (icons + glyphs, deferred polish) → C5 (delete in-runtime chrome) → C6 (docs).
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
| [x] | C3.B | Controller-rendered chrome via swapchain (initial pipeline) |
| [x] | C3.B-debug | Cross-process keyed-mutex acquire on read; src_rect=pixels; id=0 sentinel removal; D3D11 wrapper unwrap helper; shell Flush() after ClearRTV |
| [x] | C3.C-1 | Rounded pill bg via SDF shader, pill-space-meters geometry, anim-target initial-layout fix |
| [x] | C3.C-2 | Grip dots (4×2) + 3 circular buttons (red close, gray min, gray max) in same SDF pass |
| [ ] | C4 | Hit-test plumbing — chrome quad raycast, `chromeRegionId` on POINTER / POINTER_MOTION events (**next**) |
| [ ] | C3.C-4 | Hover-fade ease-out cubic + state-change re-render in shell (consumes `chromeRegionId`) |
| [ ] | C3.C-3 | App icon (per-client PNG load + texture) + DirectWrite glyph atlas — **deferred as polish** |
| [ ] | C5 | Delete in-runtime chrome render block, hit-test fields, fade machinery |
| [ ] | C6 | Test app smoke + spec/audit docs |

## Commits

- `3bd941e43` runtime + shell: Phase 2.C C1 — public surface bump for controller-owned chrome
- `b9f073195` runtime: Phase 2.C C2 — wire chrome swapchain composite path
- `f24ac89d6` shell: Phase 2.C C3.A — D3D11 graphics binding for chrome rendering
- `e2984065f` shell: Phase 2.C C3.B — controller-rendered chrome via swapchain
- `3c09adcfb` runtime + shell: Phase 2.C C3.B-debug — chrome visual fix
- `24187fa99` client D3D12: log adapter LUID + name at create — #184 diag
- `6755dacfc` shell: Phase 2.C C3.C-1 — rounded-pill chrome shader + initial-layout fix
- `1a68c81c5` shell: Phase 2.C C3.C-2 — grip dots + close/min/max buttons

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
- **In-runtime chrome still draws** (additive composite). The runtime currently renders BOTH the in-runtime pill AND the controller-submitted pill. The depth bias + draw order means the controller pill wins where it overlaps. C5 deletes the in-runtime path entirely. Until then, the visual atlas screenshot can show two slightly-different pills if positions don't match exactly.

## Next-step plan (session order)

**C4 — Hit-test plumbing** (additive). Extend `workspace_raycast_hit_test` to ray-cast each chromed slot's chrome quad in addition to its content quad. On chrome hit, populate `chrome_region_id` from `slot->chrome_layout.hitRegions[]` by UV bounds. Fill `chromeRegionId` on POINTER + POINTER_MOTION events in `workspace_drain_input_events`. **Keep the in-runtime chrome hit-test fields in place** — the runtime's existing cursor + drag logic still uses them while the in-runtime pill is drawing. C5 deletes them together with the in-runtime render block.

Shell side of C4: define region IDs (`SHELL_REGION_GRIP`, `SHELL_REGION_CLOSE`, `SHELL_REGION_MIN`, `SHELL_REGION_MAX`), populate `hitRegions[]` in the chrome layout, dispatch `chromeRegionId` from POINTER LMB events to the matching action (close → exit RPC, grip → start window-drag, etc.).

**C3.C-4 — Hover-fade ease-out cubic + state-change re-render.** Once the shell receives POINTER_MOTION events with `chromeRegionId`, it can track per-slot hover state (over chrome / over button N / over grip). State change → re-render the chrome SRV with the appropriate alpha multiplier (or button color modulation). Tween via 300 ms ease-out cubic on entry, 150 ms on exit. Idle = no GPU work.

**C3.C-3 — App icon + DirectWrite glyphs (deferred polish).** App icon: per-client PID → exe → registered-app icon_path lookup, PNG decode via stb_image, D3D11 texture + SRV, shader extension to sample. Glyphs: heavy infra (DirectWrite atlas baking, ~400–600 lines port from runtime). Both are pure visual polish — not architectural — and the existing pill+dots+buttons is enough to demonstrate the controller-owned chrome architecture works end-to-end. Land in a follow-up session.

**C5 — Delete in-runtime chrome render block + animation machinery.** ~600 lines of deletions in `comp_d3d11_service.cpp`. Validates the architecture: runtime ships with zero default chrome, only the controller-submitted version remains. Without a controller, clients show as bare quads.

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
- C3.B visual is fixed (verified via atlas screenshot). C3.C-1 and C3.C-2 land the rounded pill + grip dots + 3 buttons. Glyphs and per-app icons are deferred to a polish-pass session — pill + dots + buttons is sufficient to demonstrate the controller-owned chrome architecture.
- Resuming session continues with **C4 (hit-test plumbing)** then **C3.C-4 (hover-fade)**. Hover-fade depends on C4's `chromeRegionId` events; without it, the shell has no way to detect cursor-over-chrome state.
- Per `feedback_test_before_ci.md`: any new agent / session continuing here must build + smoke locally before pushing.
