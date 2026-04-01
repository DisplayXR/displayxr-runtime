# Shell Phase 1 — Implementation Status

Last updated: 2026-03-31 (branch `feature/shell-phase1-ci`)

## What Works (from Phase 0)

All Phase 0 features are merged to main and working:
- Multi-app compositor with shader-blit windowed rendering
- Live eye tracking via IPC (display-centric Kooima)
- 2D/3D mode switching via IPC shared memory
- Input forwarding with TAB focus cycling + cyan border
- Two-app slot-based layout: (5%,5%,40%,40%) and (55%,5%,40%,40%)
- ESC dismiss with 2D switch, DELETE close with dark gray clear
- All keys go to both qwerty and app (ADR-014)

See `shell-phase0-status.md` for full details and lessons learned.

## Phase 1 Progress

### Phase 1A: IPC-to-Standalone Hot-Switch
**Status:** Deferred (lowest priority, highest risk)

When shell exits (ESC), apps should seamlessly switch from IPC to standalone mode.

| Task | Status | Notes |
|------|--------|-------|
| 1A.1 Detect shell dismiss on client | | |
| 1A.2 Restore app HWND | | |
| 1A.3 Create native compositor in-process | | |
| 1A.4 Migrate swapchains | | |
| 1A.5 Create per-app DP | | |
| 1A.6 Resume rendering | | |

### Phase 1B: Dynamic Window Poses
**Status:** Done

Shell can reposition 3D windows dynamically via IPC using full 3D pose (position + orientation + dimensions).

| Task | Status | Notes |
|------|--------|-------|
| 1B.1 Per-client window pose storage | ✅ | `window_pose` (xrt_pose) + `window_width_m/height_m` on slot; `slot_pose_to_pixel_rect()` converts to pixel rect |
| 1B.2 Shell IPC: `shell_set_window_pose` | ✅ | `shell_set_window_pose(client_id, pose, width_m, height_m)` in proto.json |
| 1B.3 HWND resize on pose change | ✅ | `hwnd_resize_pending` flag triggers deferred SetWindowPos on next render |
| 1B.4 Update shader blit dest rect | ✅ | `window_rect_*` recomputed from pose; existing blit reads these per-frame |
| 1B.5 Eye position transform to window-local | ✅ | `comp_d3d11_service_get_client_window_metrics()` returns per-client window dims + center offset from pose; `ipc_try_get_sr_view_poses` uses per-client metrics |

### Phase 1C: Mouse Click-to-Focus + Coord Remapping
**Status:** Done

Click on a 3D window to focus it. Mouse coords remapped from shell window to app window.

| Task | Status | Notes |
|------|--------|-------|
| 1C.1 Click-to-focus hit-test | ✅ | `GetAsyncKeyState(VK_LBUTTON)` + `ScreenToClient` + rect test in render loop |
| 1C.2 Window rect intersection | ✅ | Tests cursor against each slot's `window_rect_x/y/w/h` |
| 1C.3 Focus change on hit | ✅ | Sets `focused_slot`, updates input forwarding + cyan border |
| 1C.4 Mouse coord remapping | ✅ | `input_forward_rect` on window struct; WndProc remaps `shell_xy - rect_xy` before PostMessage |
| 1C.5 Mouse clipping to window | ✅ | Mouse outside focused window rect is not forwarded to app |

### Phase 1D: Shell App + Window Drag/Resize
**Status:** Done

`displayxr-shell.exe` management app + server-side right-click-drag and scroll-to-resize.

| Task | Status | Notes |
|------|--------|-------|
| 1D.1 Privileged IPC client | ✅ | `src/xrt/targets/shell/main.c` — connects via `ipc_client_connection_init`, polls `system_get_clients` |
| 1D.2 Auto-start service | ✅ | IPC library auto-starts service; shell sends `shell_activate` IPC to enter shell mode dynamically |
| 1D.3 App connect/disconnect events | ✅ | 500ms poll loop diffs client list, prints changes |
| 1D.4 Default window placement | ✅ | Existing pose-based defaults from 1B (slot 0: left-upper, slot 1: right-upper) |
| 1D.5 Right-click-drag windows | ✅ | Server-side drag state machine in render loop: RMB down = start, held = translate pose, up = end |
| 1D.6 Scroll wheel resize | ✅ | Volatile scroll accumulator on window; render loop scales focused window ~5% per notch |
| 1D.7 App launcher | ✅ | Shell accepts app paths as args, launches with `DISPLAYXR_SHELL_SESSION=1` + `XR_RUNTIME_JSON` |
| 1D.8 Per-app pose args | ✅ | `--pose x,y,z,w,h` before each app path sets window position + size via `shell_set_window_pose` |

## Design Decisions

### Shell window revival after ESC dismiss
When ESC dismisses the shell, the service stays running and the `window_dismissed` flag is set. If a new app connects and submits a frame while `window_dismissed` is true, the multi-compositor destroys the old window resources and recreates them via `multi_compositor_ensure_output()`. This allows the shell to reopen without restarting the service — the user presses ESC to dismiss, then launches new apps to revive.

### Swapchain is never recreated on window resize
The app's swapchain is created once at `xrCreateSwapchain` time, sized for the worst-case render mode from the DP (may be larger than fullscreen). It is never recreated. When the shell resizes a window (drag, scroll), only the HWND and the blit destination rect change. The multi-comp's shader blit reads `content_view_w/h` (actual rendered region from `layer_commit`) and scales it into the window rect via `dst_rect_wh`.

### Z-ordering by focus
The focused window always renders on top. The blit loop builds a `render_order[]` array: non-focused slots first, focused slot last. When focus changes (click, TAB), the focused window is immediately drawn on top in the next frame.

### Dynamic shell activation via IPC (shell_activate)
The `--shell` command-line flag is no longer the only way to enter shell mode. The `shell_activate` IPC call lets a management client (like `displayxr-shell.exe`) activate shell mode dynamically after the service starts. This is the foundation for a GUI shell app that can start/stop shell mode without restarting the service.

Flow: IPC library auto-starts service → shell app connects → sends `shell_activate` → service enters shell mode → shell app launches apps with `DISPLAYXR_SHELL_SESSION=1`. One command (`displayxr-shell.exe app1.exe app2.exe`) replaces the previous 3-terminal workflow.

### ShowWindow must use SW_SHOW, not SW_SHOWDEFAULT
When the service is auto-launched by the IPC client library, its `STARTUPINFO` is zeroed. `SW_SHOWDEFAULT` maps to the parent's `wShowWindow` which is 0 = `SW_HIDE`, causing the shell window to be created but invisible. Fixed by using `SW_SHOW` which always makes the window visible regardless of how the process was started.

### Right-click reserved for window management
In shell mode, right-click and scroll wheel are NOT forwarded to apps — they're reserved for window drag and resize. Apps use WASD + left-click-drag for camera control instead. This is enforced in the WndProc which suppresses `WM_RBUTTONDOWN/UP` and `WM_MOUSEWHEEL` forwarding when `input_forward_hwnd` is set.

## Path to GUI Shell

Phase 1D establishes the architecture for a full GUI shell app:

1. **Service lifecycle** — ✅ Done. The IPC library auto-starts the service; `shell_activate` enters shell mode dynamically. No `--shell` flag or manual terminal needed.

2. **App launching** — ✅ Done. `displayxr-shell.exe` launches apps with correct env vars (`DISPLAYXR_SHELL_SESSION=1`, `XR_RUNTIME_JSON`). Currently via command-line args; future: GUI file picker or drag-and-drop.

3. **Window management** — ✅ Done. `shell_set_window_pose` IPC positions windows programmatically. `--pose` CLI args set initial layout. Server-side drag/resize for interactive use. Future: GUI panel with window list, snap layouts.

4. **Client monitoring** — ✅ Done. Shell polls `system_get_clients` and detects connect/disconnect. Future: GUI displays app thumbnails, status indicators.

5. **Missing for GUI**: A windowed UI framework (ImGui, Qt, or native Win32) for the shell app itself. The shell currently runs as a console app with text output. The rendering surface (3D shell window) is owned by the service — the GUI shell would be a separate management window.

## Known Issues

### Apps don't survive shell exit
When ESC dismisses the shell, apps are alive but invisible — IPC swapchains render to nowhere. Apps must be restarted standalone. Fix: Phase 1A.

### HWND centered for Kooima offset (resolved in 1B)
App HWNDs are still centered on the display (for DXGI Present compatibility), but `comp_d3d11_service_get_client_window_metrics()` now returns per-client virtual window dims and center offset computed from the slot's `window_pose`. The Kooima projection uses the virtual window position, not the HWND position.

## Test Procedure (Local Dev)

### Build
```bash
scripts\build_windows.bat build       # Runtime (service + DLL)
scripts\build_windows.bat test-apps   # Test apps + generates run scripts in _package/
```

### Run with displayxr-shell (recommended — single command)

`displayxr-shell.exe` auto-starts the service, activates shell mode, launches apps with correct env vars, and monitors clients. No manual terminal setup needed.

**Two apps (one command):**
```
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

**Two apps with custom poses:**
```
_package\bin\displayxr-shell.exe --pose -0.1,0.05,0,0.14,0.08 app1.exe --pose 0.1,0.05,0,0.14,0.08 app2.exe
```

**Monitor only (no apps — connect to existing or auto-start service):**
```
_package\bin\displayxr-shell.exe
```

### Run from generated scripts (legacy — multiple terminals)

**Single app:**
```
Terminal 1: _package\run_shell_service.bat
Terminal 2: _package\run_shell_app.bat
```

**Two apps:**
```
Terminal 1: _package\run_shell_service.bat
Terminal 2: _package\run_shell_app.bat     (→ slot 0, left)
Terminal 3: _package\run_shell_app.bat     (→ slot 1, right)
```

### Run from Claude Code (automated testing)

Use `displayxr-shell.exe` — it handles service auto-start, shell activation, env vars, and app launching.

```bash
# Step 1: Kill leftover processes
taskkill //F //IM displayxr-service.exe 2>&1 || true
taskkill //F //IM displayxr-shell.exe 2>&1 || true
taskkill //F //IM cube_handle_d3d11_win.exe 2>&1 || true

# Step 2: Launch shell + apps (run_in_background: true, timeout: 600000)
cd "/c/Users/Sparks i7 3080/Documents/GitHub/openxr-3d-display" && _package/bin/displayxr-shell.exe test_apps/cube_handle_d3d11_win/build/cube_handle_d3d11_win.exe test_apps/cube_handle_d3d11_win/build/cube_handle_d3d11_win.exe 2>&1
```

**Notes:**
- `displayxr-shell.exe` auto-starts the service via IPC library, sends `shell_activate`, sets `XR_RUNTIME_JSON` and `DISPLAYXR_SHELL_SESSION=1` on each app
- 3-second delay between app launches (built into the shell)
- `timeout: 600000` (10 minutes) gives the user time to interact
- The shell resolves `XR_RUNTIME_JSON` to `build/Release/openxr_displayxr-dev.json` (dev build) or `_package/DisplayXR_win64.json` (installed)

**Check results:**
```bash
grep -E "registered client|resized app|TAB|DELETE|click|focused" \
  "$(ls -t '/c/Users/Sparks i7 3080/AppData/Local/DisplayXR/'*service* | head -1)" \
  | head -20
```

**Shell controls:**
- **Left-click** — focus window under cursor (cyan border follows)
- **Right-click-drag** — move window in display plane
- **Scroll wheel** — resize focused window (~5% per notch)
- **TAB** — cycle focus: app 0 → app 1 → unfocused → app 0
- **DELETE** — close focused app (dark gray replaces it)
- **ESC** — dismiss shell, switch display to 2D
- **V** — toggle 2D/3D display mode
- **WASD/left-click-drag** — forwarded to focused app (camera control)

Logs: `%LOCALAPPDATA%\DisplayXR\*.log`

## Key Files

See `shell-phase1-plan.md` for the full file reference from Phase 0.
