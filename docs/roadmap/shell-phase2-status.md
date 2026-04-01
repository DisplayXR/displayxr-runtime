# Shell Phase 2 — Implementation Status

Last updated: 2026-04-01 (branch `feature/shell-phase1-ci`)

## What Works (from Phase 1)

All Phase 1 features are on branch `feature/shell-phase1-ci`:
- `displayxr-shell.exe` single-command launcher (auto-starts service + activates shell mode + launches apps)
- Dynamic window poses with full 3D `xrt_pose` + `shell_set_window_pose` IPC
- Click-to-focus, right-click-drag to move, scroll-to-resize
- Z-ordering by focus, mouse coord remapping, blit clamping
- Shell revival after ESC dismiss
- `shell_activate` IPC for dynamic shell mode
- Per-app `--pose` args for programmatic window placement

See `shell-phase1-status.md` for design decisions and lessons learned.

## Phase 2 Progress

### Phase 2A: Window Chrome
**Status:** Not started

Title bars with app name and close button, rendered server-side in the compositor.

| Task | Status | Notes |
|------|--------|-------|
| 2A.1 Title bar rendering | | Colored strip above each window's blit rect |
| 2A.2 App name text | | Bitmap font, glyphs blit from compiled-in atlas |
| 2A.3 Close button | | Click on X sends EXIT_REQUEST |
| 2A.4 Title bar drag | | Right-click-drag on title bar moves window |

### Phase 2B: Layout Presets
**Status:** Not started

One-key layout switching for common arrangements.

| Task | Status | Notes |
|------|--------|-------|
| 2B.1 Layout algorithms | | Side-by-side, stacked, fullscreen, cascade |
| 2B.2 Number key triggers | | 1-4 in qwerty handler |
| 2B.3 Pose computation | | Compute poses for all active clients per layout |
| 2B.4 Animated transitions | | Optional: lerp over ~200ms |

### Phase 2C: Close / Minimize / Maximize
**Status:** Not started

Window management actions.

| Task | Status | Notes |
|------|--------|-------|
| 2C.1 Close from chrome | | Same as DELETE but from title bar |
| 2C.2 Minimize | | Hide from rendering, keep connected. `shell_set_visibility` IPC |
| 2C.3 Maximize | | Focused fills display, others hidden |
| 2C.4 Taskbar | | Bottom strip showing minimized app indicators |

### Phase 2D: Persistence
**Status:** Not started

Window layout saved to JSON, restored on restart.

| Task | Status | Notes |
|------|--------|-------|
| 2D.1 Config file | | `%LOCALAPPDATA%\DisplayXR\shell_layout.json` |
| 2D.2 Restore on connect | | Apply saved pose when app reconnects |
| 2D.3 Save on change | | Update config on drag/resize/layout |
| 2D.4 Shell reads config | | Apply saved poses via `shell_set_window_pose` |

## Known Issues

### Intermittent crash with two apps (#108)
Service crashes intermittently when two apps run simultaneously. Race condition or D3D11 threading issue. Needs debugger session.

### Apps don't survive shell exit (Phase 1A deferred)
ESC dismisses shell, apps become invisible. Must relaunch apps after shell revival.

## How to Launch the Shell

### Single command (recommended)

`displayxr-shell.exe` handles everything: auto-starts the service, activates shell mode, sets `XR_RUNTIME_JSON` and `DISPLAYXR_SHELL_SESSION=1`, launches apps with a 3-second delay between each.

```bash
# From the repo root:
_package\bin\displayxr-shell.exe app1.exe [app2.exe ...]

# Example: two cube apps
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe

# With custom window poses (x,y,z in meters from display center, w,h in meters)
_package\bin\displayxr-shell.exe --pose -0.1,0.05,0,0.14,0.08 app1.exe --pose 0.1,0.05,0,0.14,0.08 app2.exe

# Monitor only (no apps)
_package\bin\displayxr-shell.exe
```

### From Claude Code

```bash
# Kill leftovers
taskkill //F //IM displayxr-service.exe 2>&1 || true
taskkill //F //IM displayxr-shell.exe 2>&1 || true
taskkill //F //IM cube_handle_d3d11_win.exe 2>&1 || true

# Launch (run_in_background: true, timeout: 600000)
cd "/c/Users/Sparks i7 3080/Documents/GitHub/openxr-3d-display" && _package/bin/displayxr-shell.exe test_apps/cube_handle_d3d11_win/build/cube_handle_d3d11_win.exe test_apps/cube_handle_d3d11_win/build/cube_handle_d3d11_win.exe 2>&1
```

### Build before testing

```bash
scripts\build_windows.bat build       # Runtime + service + shell
scripts\build_windows.bat test-apps   # Test apps
```

### Shell controls

| Input | Action |
|-------|--------|
| Left-click | Focus window (cyan border, z-order on top) |
| Right-click-drag | Move window in display plane |
| Scroll wheel | Resize focused window (~5% per notch) |
| TAB | Cycle focus: app 0 → app 1 → unfocused → app 0 |
| DELETE | Close focused app |
| ESC | Dismiss shell (2D mode). New apps reopen it. |
| V | Toggle 2D/3D display mode |
| WASD / left-click-drag | Forwarded to focused app (camera control) |

### Key source files

| File | Role |
|------|------|
| `src/xrt/targets/shell/main.c` | Shell app: launcher, monitor, pose assignment |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp, render, slots, drag, resize, z-order, chrome |
| `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` | Blit + border shaders |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Window WndProc, input forwarding, scroll accum |
| `src/xrt/ipc/server/ipc_server_handler.c` | IPC handlers: shell_activate, shell_set_window_pose |
| `src/xrt/ipc/shared/proto.json` | IPC protocol definitions |

Logs: `%LOCALAPPDATA%\DisplayXR\*.log`
