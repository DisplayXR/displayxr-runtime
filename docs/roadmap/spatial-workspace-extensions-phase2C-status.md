# Phase 2.C Status: Controller-Owned Chrome ✅ SHIPPED

**Branch:** `feature/workspace-extensions-2C` (off `feature/workspace-extensions-2K` tip)
**Status:** ✅ All sub-phases shipped. Public surface bumped 6 → 8 across spec_version 7 (chrome swapchain + layout + hit regions) and spec_version 8 (event-driven wakeup, auto-anchor flags, `WINDOW_POSE_CHANGED` event). Runtime ships with **zero default chrome** — controller-owned chrome is the only chrome path; idle CPU effectively zero (event-driven, no poll loop); pill tracks window edge in lockstep with content during resize (no IPC roundtrip per frame); per-client app name renders between icon and dots via DirectWrite, adaptive-skip when too narrow. C6 (test app chrome smoke + spec doc refresh + audit refresh) committed as the close-out.
**Date:** 2026-05-01 (Phase 2.C ✅ shipped after C6)

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
| [x] | C3.C-3a | Per-client app icon (PID → exe → registered_app icon_path; .ico extraction fallback via PrivateExtractIconsA + cached PNG under %TEMP%; stb_image decode; D3D11 texture + SRV; rounded-square mask in PS) |
| [x] | spec_v8: wakeup | Event-driven shell wakeup — `xrAcquireWorkspaceWakeupEventEXT` returns a Win32 HANDLE; runtime SetEvents on input event push + hovered/focused-slot transitions; shell's `MsgWaitForMultipleObjects` waits on it. Idle CPU drops from ~0.1 % → 0; hover transitions reach the shell with zero latency. |
| [x] | spec_v8: auto-anchor | `XrWorkspaceChromeLayoutEXT.anchorToWindowTopEdge` + `widthAsFractionOfWindow` flags. Runtime auto-recomputes chrome center/width every frame from CURRENT window dims; shell pushes layout once at create and never on resize. Pill tracks window edge in lockstep with content. |
| [x] | spec_v8: pose-changed | New `XR_WORKSPACE_INPUT_EVENT_WINDOW_POSE_CHANGED_EXT` event lets controllers react to runtime-driven pose / size changes (edge resize, fullscreen toggle). |
| [x] | spec_v8: glassy polish | Lighter / more transparent pill bg (22 % alpha, edge highlight ring); semi-transparent buttons with procedural × / − / □ glyphs in PS. |
| [x] | C3.C-3b | DirectWrite title-text atlas — per-client app name baked once via DirectWrite + D2D over a DXGI surface (R8G8B8A8_UNORM linear, 64 px tall, vertically centered Segoe UI Variable @ 28 DIP). Sampled at register t1 in the pill PS via new `over_pma()` Porter-Duff variant. Adaptive: render_pill flips `has_title=0` when the available rect (icon-right → dots-left, minus padding) can't fit the measured text, AND while a resize burst is in flight (prevents flicker on hover-fade ticks during drag). |
| [x] | C6 | Test app chrome smoke (`workspace_minimal_d3d11_win`: chrome swapchain create / acquire / wait / clear-RTV / release / layout-with-auto-anchor + 2 hit regions / wakeup-event handle / destroy — 28 PFNs total) + spec doc refresh to spec_version 8 (wakeup event, auto-anchor flags, `WINDOW_POSE_CHANGED` event) + audit doc refresh to fold in C3.C-3a / C3.C-3b deliverables. |

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
- `dd8e16747` docs: Phase 2.C C6 — spec_version 7 + audit + plan + separation-of-concerns
- `2b8a853b9` runtime + shell: Phase 2.C C3.C-4 polish — chrome hover-fade visible (POINTER_HOVER client_id; poll-while-chrome-alive)
- `bb601f938` Phase 2.C C3.C-3a + spec_version 8 — event wakeup, auto-anchor, glassy polish

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
- **POINTER_HOVER carries the OpenXR client_id, not `1000 + slot_index`.** Slots store the OpenXR client_id at chrome registration time (`workspace_client_id`) and the drain emits it. Lets the shell match hover events to its chrome by the same id it used at create time, instead of the runtime's internal slot-index convention.
- **Event-driven wakeup, not polling.** `xrAcquireWorkspaceWakeupEventEXT` (spec_version 8) returns a Win32 HANDLE the shell waits on. Runtime SetEvents it on every public-event ring push (POINTER / KEY / SCROLL / MOTION) and on hovered/focused-slot transitions detected per-frame in `render_pass`. Auto-reset semantics; controller drains all pending state on each wake. Replaces a 60 Hz polling loop while chrome was alive — idle CPU drops to effectively 0; hover transitions reach the shell with zero perceptible latency.
- **Chrome layout is invariant under window resize via auto-anchor.** `anchorToWindowTopEdge` + `widthAsFractionOfWindow` (spec_version 8) tell the runtime to recompute chrome center y / width every frame from CURRENT `window_height_m` / `window_width_m`. Without this, the controller's per-event push_layout always carried stale dims (the controller observed last-frame state) and the pill visibly lagged one frame behind the window edge during a drag. With auto-anchor the shell pushes layout ONCE at create and never on resize — runtime keeps chrome glued to the window edge in real time.
- **Edge-resize handles use CONTENT bounds, not extended-with-chrome bounds.** C4 originally extended `ext_top` to include the chrome quad so chrome hits would register; that also moved the resize handle ABOVE the visual content top. Decoupled: outer hover bounds still extend (so chrome hits work), but `RESIZE_TOP` is computed from `win_top` ± `resize_zone_m` so the user grabs at the visible content top edge.
- **RMB drag pitch reads from `euler.z` (X-axis rotation), not `euler.x`.** `math_quat_to_euler_angles` uses Eigen's Z-Y-X decomposition and writes `(.x = roll, .y = yaw, .z = pitch)`. Long-standing first-RMB-down pitch-snap bug was reading the roll component as pitch.

## Open issues

- **Pill image stretches during horizontal resize until 100 ms after release.** The chrome IMAGE has button slots and dot circles baked in pill-space-meters of the LAST render. Width changes scale `pill_w_m` but the runtime composites the cached image stretched onto the new quad — buttons/dots momentarily look elongated until the debounced post-resize re-render fires. Vertical resize is unaffected (pill width unchanged). Documented; could be mitigated by image-pixel-space SDF (always slight elongation, never wobble) but trade-off was not deemed worth it.
- **Keyed-mutex AcquireSync timeout is 4 ms.** If the shell's GPU is slow to flush its writes, the runtime's acquire could time out and the chrome blit silently uses stale texture content. Worth instrumenting with a one-shot warn-log if the acquire ever fails. Not yet observed in practice.
- **Test apps have no app-name string.** `XrWorkspaceClientInfoEXT.name` is the OpenXR `applicationName` (e.g. `SRCubeOpenXRExt`). Registered_app `name` is the friendly sidecar name (e.g. `Cube D3D11 (Handle)`). They don't match for test apps. C3.C-3b's `shell_resolve_title_for_pid` (`main.c`) prefers the registered_app `name` when the PID's exe matches a sidecar, and falls back to `cinfo.name` otherwise.

## Next-step plan

Phase 2.C is shipped. Next investigations are independent of this branch:

- **Window-content focus glow** (proposed) — soft inward rim on the focused client's chrome pill, controller-owned. Reuse the existing edge-highlight SDF in `shell_chrome.cpp`'s pixel shader and modulate by a new `set_focus(client_id)` API + `has_focus` cbuffer flag. ~100 lines.
- **C3.C-3 polish carryovers** if any surface during dogfooding (the in-tree status doc's "Open issues" list captures the known ones).
- **Phase 2.H final cleanup pass** — the rename pass over remaining `shell_*` mentions in the runtime body; tracked in `spatial-workspace-extensions-phase2-audit.md`.

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
- Phase 2.C is **architecturally complete and visually production-ready**. The runtime owns zero pixels of UI policy. The controller-side chrome is fully interactive (hit-test, click dispatch, hover-fade, drag/resize tracking, app icons, app-name title text), event-driven (zero idle CPU), and visually polished (glassy frosted-blue pill matching the concept art at [`docs/architecture/assets/chrome-pill-concept.png`](../architecture/assets/chrome-pill-concept.png)).
- C6 (test app smoke + final doc refresh) closes Phase 2.C.
- Per `feedback_test_before_ci.md`: any continuing session must build + smoke locally before pushing.
