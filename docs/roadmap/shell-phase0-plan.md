# Fresh Start: 3D Shell Multi-Compositor — COMPLETE

> **Status:** Phase 0 is fully implemented and CI-validated on branch `feature/shell-phase0-ci`.
> See `shell-phase0-status.md` for implementation details and lessons learned.
> See `shell-phase1-plan.md` for the next milestone.

## Context

Phase 0 multi-comp branch proved the rendering pipeline works (commit c15a306 had visible content + correct weaving). But the IPC view pose path got tangled with multiple patches. Main has been refactored (#102, #103). Starting fresh on clean main with lessons learned.

## Pre-work: Save docs to main, clean up

### Cherry-pick to main:
- ADR-013 (universal app / hidden HWND proxy)
- shell-tasks.md updates (ADR references, architecture notes)
- Remove phase0-implementation-notes.md (stale)

### Reset shell-tasks.md Phase 0:
- Un-mark all tasks (fresh implementation)
- Rewrite Phase 0 based on new architecture (universal app, not IPC-specific)
- Add new Phase 0.5: "Shell active mode" for service

### Delete old worktree, create fresh:
- Remove `.claude/worktrees/multi-compositor`
- New worktree on `feature/shell-phase0-ci` from main

## Revised Implementation Order

The key insight: **develop with handle apps first** (they already work), then add shell routing. Don't start with IPC apps — they have the view pose issues we hit.

### Phase 0A: Service shell-active mode (no breaking changes)

**Goal:** Service can run in "shell active" mode where it creates a multi-comp window. WebXR/IPC apps still work as before when shell is not active.

| Task | Description |
|------|-------------|
| 0A.1 | Add `--shell` flag to `displayxr-service.exe`. When set, service creates multi-comp window + DP on first client connect. When not set, current behavior (per-client windows). |
| 0A.2 | `d3d11_multi_compositor` struct: window, swap chain, combined atlas, DP, client slots. Same design as before but cleaner on refactored #103 base. |
| 0A.3 | Per-client compositor in shell mode: skip window/DP creation, keep atlas. Multi comp reads atlas SRV directly. |
| 0A.4 | Multi comp render: clear combined atlas → render client quads (Level 2 Kooima via `display3d_compute_projection`) → call DP → present. |
| 0A.5 | Window lifecycle: deferred to first client, ESC dismisses (2D + stays closed), re-opens on new client. |

**Test:** `displayxr-service --shell` + `set XRT_FORCE_MODE=ipc && cube_ipc_d3d11_win` → cube visible in multi-comp window. Without `--shell`, same app works as before (per-client window).

**Key difference from v1:** `--shell` flag instead of `XRT_MULTI_APP=1` default. WebXR never affected.

### Phase 0B: Shell app skeleton + handle app in shell

**Goal:** A minimal shell app starts the service in shell mode, then a handle app launched with `DISPLAYXR_SHELL_SESSION=1` renders into the shell.

| Task | Description |
|------|-------------|
| 0B.1 | Minimal shell app: `displayxr-shell.exe` — starts service with `--shell`, opens multi-comp window, enters loop. Just a launcher for now. |
| 0B.2 | `DISPLAYXR_SHELL_SESSION=1` env var in `u_sandbox_should_use_ipc()`. When set, handle app's HWND is hidden, routing goes to IPC. |
| 0B.3 | Pass app's HWND to service via IPC session creation (new field in `xrt_session_info`). Service stores in client slot. |
| 0B.4 | **Test with existing handle app**: `cube_handle_d3d11_win` launched with `DISPLAYXR_SHELL_SESSION=1` → HWND hidden, renders in shell's multi-comp window. App code unchanged. |

**Test:** Shell app running → launch `cube_handle_d3d11_win` with env var → cube visible in shell window. Same app without env var → normal standalone behavior.

**Why handle app first:** Handle apps do their own Kooima, own their HWND, handle `WM_SIZE`. We bypass ALL the IPC view pose issues that plagued us. The app renders correctly because it computes its own projection. The multi-comp just reads the atlas and renders the quad.

### Phase 0C: Two apps + focus

**Goal:** Two handle apps running in shell, TAB cycling, DELETE close.

| Task | Description |
|------|-------------|
| 0C.1 | Second client slot layout: first = full display, second = 45% at offset. |
| 0C.2 | TAB cycles focus (cyan border). DELETE closes focused app. |
| 0C.3 | Level 1 eye transform: `eye_in_window = inverse(window_pose) * eye_in_display`. Override `XR_EXT_display_info` meters for shell apps (window dims). |
| 0C.4 | Shell resizes hidden HWND via `SetWindowPos` → app gets `WM_SIZE` → updates Kooima. |

**Test:** Shell → launch two `cube_handle_d3d11_win` → both visible, TAB switches focus, DELETE closes one.

### Phase 0D: Input forwarding via hidden HWND

**Goal:** Shell forwards keyboard/mouse to focused app's hidden HWND.

| Task | Description | Status |
|------|-------------|--------|
| 0D.1 | Shell captures keyboard from multi-comp window, `PostMessage(focused_hwnd, WM_KEYDOWN, ...)`. | ✅ Done |
| 0D.2 | Mouse coordinate mapping: 1:1 for fullscreen → `PostMessage(WM_MOUSEMOVE, ...)`. | ✅ Done |
| 0D.3 | WASD/mouse in focused app moves app's camera (app handles it via its own WndProc). | ✅ Done |

**Test:** Focus cube app → WASD moves camera within cube scene → works identically to standalone.

## Key Architecture Principles

### Service modes
```
displayxr-service              → WebXR/sandbox IPC (current, no multi-comp)
displayxr-service --shell      → Shell mode (multi-comp, universal apps)
```

### App routing
```
App launched from OS            → in-process native compositor (handle app)
App launched from shell         → DISPLAYXR_SHELL_SESSION=1 → IPC → multi-comp
  (HWND hidden, shell puppets)
Sandboxed app (Chrome)          → auto-IPC (sandbox detection, no shell needed)
```

### Qwerty device
- Shell mode: qwerty controls shell (window management, V/P keys, future WASD window drag)
- App input: forwarded via hidden HWND PostMessage, NOT via qwerty

### View pose flow (universal app in shell)
```
App creates HWND → runtime hides it → IPC to service
App calls xrLocateViews → server returns window-relative eyes
App reads XR_EXT_display_info → server returns window dims (meters)
App does own Kooima (same code as standalone)
App renders to swapchain → service reads atlas → multi-comp renders quad → DP weaves
```

## Files to modify (clean implementation on main)

| File | Change |
|------|--------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi compositor struct, render, lifecycle |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.h` | New APIs |
| `src/xrt/ipc/server/ipc_server_handler.c` | Level 1 eye transform, display_info override |
| `src/xrt/auxiliary/util/u_sandbox.c` | `DISPLAYXR_SHELL_SESSION` check |
| `src/xrt/targets/service/main.c` | `--shell` flag |
| `src/xrt/state_trackers/oxr/oxr_session.c` | Handle HWND hide + IPC routing in shell mode |
| `docs/roadmap/shell-tasks.md` | Reset + rewrite |

## What NOT to change (lessons learned)

- Don't modify `oxr_session_locate_views` 3D-GATE — it works correctly for in-process apps. Shell apps get their view poses from their own Kooima (they're handle apps).
- Don't add `qwerty_set_display_centric` or `XRT_DISPLAY_CENTRIC` — shell apps control their own projection via HWND.
- Don't create `cube_ipc_d3d11_win` as a separate app class — use existing `cube_handle_d3d11_win` with env var.
- Don't default multi-comp ON — use `--shell` flag. WebXR must not be affected.

## Verification (end-to-end)

1. `displayxr-service` (no --shell): WebXR works as before, handle apps work standalone
2. `displayxr-service --shell`: multi-comp window appears on first client
3. `cube_handle_d3d11_win` standalone: normal handle app, own window
4. `DISPLAYXR_SHELL_SESSION=1 cube_handle_d3d11_win`: HWND hidden, renders in shell
5. Two apps in shell: both visible, TAB focus, DELETE close
6. WASD in focused app: camera moves within app scene
7. ESC: dismisses shell window (2D mode), apps keep running
