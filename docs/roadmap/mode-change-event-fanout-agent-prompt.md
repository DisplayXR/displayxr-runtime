# Agent prompt: Runtime fix for `RENDERING_MODE_CHANGED_EXT` fan-out (#142)

Use this prompt to start a fresh Claude Code session on a Windows machine with the Leia SR display attached, to implement the runtime fix that unblocks WebXR Bridge v2 Phase 2.

---

## Prompt

```
I'm fixing a runtime regression that blocks WebXR Bridge v2 Phase 2 (#139). The bug: `XrEventDataRenderingModeChangedEXT` is not delivered to secondary / headless OpenXR sessions against the DisplayXR D3D11 service compositor — only to whichever rendering client is driving frames. This was discovered in Phase 1 of the WebXR Bridge v2 work and filed as issue #142.

The full background, root cause analysis, and fix plan are in `docs/roadmap/mode-change-event-fanout-plan.md`. The file is self-contained — read it first.

## Branch

I am working from `feature/webxr-bridge-v2`, commit `7c5f60ec3` or later. The bridge from Phase 1 is already built into the runtime and installs to `_package\bin\displayxr-webxr-bridge.exe`. Do NOT rebase off `main` unless needed — the plan doc and the bridge live on this branch and I want the fix committed here so Phase 2 can proceed directly after.

Commit messages must include `(#142)` and `(#139)`.

## Context to read in order

1. `CLAUDE.md` — project overview, build commands (`scripts\build_windows.bat`), logging conventions, debug-dump procedures. Skip the macOS sections — this work is Windows-only.
2. `docs/roadmap/mode-change-event-fanout-plan.md` — the plan you are implementing. Treat it as authoritative.
3. `docs/roadmap/webxr-bridge-v2-plan.md` — why this fix matters (Phase 2 blocker).
4. `src/xrt/state_trackers/oxr/oxr_session.c:855-884` — the old pull-mode detection you're deleting.
5. `src/xrt/state_trackers/oxr/oxr_session.c:892-929` — existing session-event dispatch you're extending.
6. `src/xrt/auxiliary/util/u_system.c:236` (`u_system_broadcast_event`) — the fan-out helper you're using.
7. `src/xrt/ipc/server/ipc_server_process.c:540-567` (`main_loop`) — the existing per-client shmem sync. Do NOT delete it.
8. `src/xrt/ipc/client/ipc_client_hmd.c:130-144` — where the old pull-mode sync happens. Do NOT delete it either — rendering clients still rely on it for per-frame `active_rendering_mode_index` reads.
9. `src/xrt/include/xrt/xrt_session.h` — `enum xrt_session_event_type`, event structs, `union xrt_session_event`.
10. `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — grep for `active_rendering_mode_index\s*=`. Six sites to patch (around lines 5862, 5865, 7014, 7017, 7073, 7075 in the current tree).
11. `src/xrt/external/openxr_includes/openxr/XR_EXT_display_info.h:272-299` — the extension spec for the two events being fanned out.

## What to implement

Follow `docs/roadmap/mode-change-event-fanout-plan.md` section by section. The five edits:

1. `xrt_session.h`: add `XRT_SESSION_EVENT_RENDERING_MODE_CHANGE = 12` and `XRT_SESSION_EVENT_HARDWARE_DISPLAY_STATE_CHANGE = 13`, add the two event structs, add both to `union xrt_session_event`.

2. **Verify `sizeof(union xrt_session_event)` is unchanged** — the union is memcpy'd across IPC and `src/xrt/ipc/shared/proto.json:146` references it by name. Easiest check: after regeneration of the IPC proto siblings, grep for the blob size constant in the generated files (should be the same as the `display_refresh_rate_change` size, which was previously the largest variant). If the size grew, stop and ask — we need to bump the protocol version and handle old clients.

3. `comp_d3d11_service.cpp`: at each of the six mutation sites, after setting `head->hmd->active_rendering_mode_index`, compute previous/new, build a `xrt_session_event`, call `u_system_broadcast_event(sys->usys, &xse)`. You will probably need to thread a `struct u_system *` pointer through `comp_d3d11_service_create_system` into the service-compositor's per-system struct if one isn't already reachable. Also emit the companion `XRT_SESSION_EVENT_HARDWARE_DISPLAY_STATE_CHANGE` when `hardware_display_3d` flips.

4. `oxr_session.c`: add two new `case` entries to the dispatch switch at 892-929, mirroring the bookkeeping that the old pull-mode block did (view scales, `sess->hardware_display_3d`, `sess->last_rendering_mode_index`) and then calling `oxr_event_push_XrEventDataRenderingModeChanged` / `...HardwareDisplayStateChanged`.

5. Delete the `oxr_session.c:855-884` pull-mode block. Keep `sess->last_rendering_mode_index` (the dispatch case still maintains it).

## Build / verify

Local Windows build only, per repo convention. **Do NOT use `/ci-monitor` or push to remote.**

```bat
scripts\build_windows.bat build
scripts\build_windows.bat installer
```

Uninstall the previous DisplayXR install first to avoid PID version mismatches (the bridge is picky — see CLAUDE.md or ask the user).

## Verification matrix (all four must pass before commit)

1. **Bridge sees events while Chrome WebXR runs.** Start Chrome WebXR, Enter VR. Launch `C:\Program Files\DisplayXR\Runtime\displayxr-webxr-bridge.exe` (it forces `XRT_FORCE_MODE=ipc` internally). Cycle the DP render mode hotkey several times. Bridge terminal must show one `RENDERING_MODE_CHANGED previous=X current=Y` per cycle, plus re-enumerated view dims. Expected display info: `displayPixelSize: 3840 x 2160`, not the null-compositor 1536x864 — that value tells you the bridge is talking to the real service.

2. **Chrome WebXR regression.** Same session as (1). Chrome's WebXR scene must continue to render correctly after mode changes and re-request an appropriately-sized framebuffer on the next frame. No visual glitch, no crash.

3. **`cube_hosted_d3d11_win` regression.** Close Chrome. Run `_package\run_cube_hosted_d3d11_win.bat`. Press the mode-cycle hotkey. Scene should re-settle with correct view scales — same behaviour as before the fix.

4. **Headless coexistence under two rendering clients.** Start Chrome WebXR. Start `run_cube_hosted_d3d11_win.bat` alongside. Start the bridge. All three OpenXR sessions should reach FOCUSED, cycle modes, and the bridge should observe every mode change.

Capture screenshots of the bridge terminal output for each scenario (the "Capturing Window Screenshots (Autonomous Testing)" section in `CLAUDE.md` shows how).

## Commit message template

```
Fix RENDERING_MODE_CHANGED fan-out to headless sessions (#142) (#139)

- xrt_session.h: add RENDERING_MODE_CHANGE / HARDWARE_DISPLAY_STATE_CHANGE
  session event kinds
- comp_d3d11_service.cpp: broadcast new session events via
  u_system_broadcast_event on every active_rendering_mode_index mutation
- oxr_session.c: dispatch new session events, remove client-side pull
  detection block
- Union size unchanged; IPC proto regenerated with no wire-format change

Unblocks WebXR Bridge v2 Phase 2. Bridge at
src/xrt/targets/webxr_bridge/ now receives XrEventDataRenderingModeChangedEXT
when any other session triggers a mode change via the service compositor.

Verified four-way: bridge+Chrome, Chrome-only regression, cube_hosted
regression, triple-session coexistence.
```

## Things to ask the user about, don't assume

- Whether to also patch the in-process per-API compositors (`comp_d3d11_compositor.cpp`, `comp_gl_compositor.c`, `comp_metal_compositor.m`, `comp_d3d12_compositor.cpp`, `comp_vk_native_compositor.c`) for consistency. The plan says this is out of scope for #142 (they don't serve multiple IPC clients today), but if the user wants full consistency in one shot, it's a mechanical fan-out of the same pattern.
- Whether `u_system *` is already reachable from the d3d11_service system struct. If not, the plumbing change (thread the pointer through `comp_d3d11_service_create_system`) is the riskiest part of this fix — flag it before editing the signature.
- If `sizeof(union xrt_session_event)` grows, how the user wants to handle backwards compat (bump IPC version? version-gate? neither on dev branches?).
- What the render-mode cycle hotkey currently is — the plan and the Phase 1 report both left this undocumented. If you can't find it by grepping `WndProc\|WM_KEY` in `comp_d3d11_service.cpp`, ask.

Start by reading `docs/roadmap/mode-change-event-fanout-plan.md` in full, then re-read the "Files to read first" list in that doc. Do not touch anything outside the files listed in the plan.
```

---

## Notes for the person kicking this off

- The bridge binary from Phase 1 is the best smoke test you have. `displayxr-webxr-bridge.exe` (commit `7c5f60ec3` or later) is a one-process reproducer for the bug — run it with Chrome in WebXR and watch the terminal.
- Do not delete the server-side shmem sync at `ipc_server_process.c:551-560` or the client-side shmem read at `ipc_client_hmd.c:130-144`. Those are still needed for rendering clients to pick the right view scales on subsequent frames. The fix adds event push *in addition to* the existing sync, not as a replacement.
- If the union size grows and you're tempted to skip the protocol-version bump "because it's just a dev branch" — don't. The installer-built `DisplayXRClient.dll` will be loaded by in-process apps that were compiled against the old union size, and `memcpy` across the wire will silently corrupt events.
- Once #142 is merged, Phase 2 of the WebXR Bridge v2 work can proceed directly. The bridge host code at `src/xrt/targets/webxr_bridge/main.cpp` already handles `RENDERING_MODE_CHANGED_EXT` and `HARDWARE_DISPLAY_STATE_CHANGED_EXT` events — no bridge-side changes required, the log lines will just start appearing once the runtime actually delivers the events.
