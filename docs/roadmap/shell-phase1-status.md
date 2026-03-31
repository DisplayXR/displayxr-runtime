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
**Status:** Not started

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

### Phase 1D: Shell App Skeleton
**Status:** Not started

Minimal `displayxr-shell.exe` that manages window layout.

| Task | Status | Notes |
|------|--------|-------|
| 1D.1 Privileged IPC client | | |
| 1D.2 Auto-start service | | |
| 1D.3 App connect/disconnect events | | |
| 1D.4 Default window placement | | |
| 1D.5 Mouse drag windows | | |
| 1D.6 Scroll wheel Z depth | | |

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

### Run from generated scripts (recommended for manual testing)

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

When launching from Claude Code background commands, bash `export` does NOT propagate env vars to Windows processes. Use `cmd.exe //c "set VAR=val&& app.exe"` instead. All commands from repo root.

```bash
# Step 1: Kill leftover processes
taskkill //F //IM displayxr-service.exe 2>&1 || true
taskkill //F //IM cube_handle_d3d11_win.exe 2>&1 || true
sleep 2

# Step 2: Start service (background, 10-min timeout)
"_package/bin/displayxr-service.exe" --shell &
# Use: run_in_background: true, timeout: 600000

# Step 3: Start first app (5s delay, background)
sleep 5 && cmd.exe //c "set XR_RUNTIME_JSON=C:\Users\Sparks i7 3080\Documents\GitHub\openxr-3d-display\build\Release\openxr_displayxr-dev.json&& set DISPLAYXR_SHELL_SESSION=1&& test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe"
# Use: run_in_background: true, timeout: 600000

# Step 4: Start second app (same command, another 5s delay)
# Same as Step 3 — service assigns it to slot 1 automatically
```

**Important notes:**
- No spaces between `val&&` in `cmd.exe set` — trailing spaces become part of the value
- `XR_RUNTIME_JSON` points to the dev build DLL, bypassing the installed runtime
- `DISPLAYXR_SHELL_SESSION=1` forces IPC/shell mode
- 5-second `sleep` between launches gives each process time to connect
- `timeout: 600000` (10 minutes) gives the user time to interact
- After rebuilding `.cpp` files, delete `.obj` to force recompilation (ninja timestamp issues)

**Check results:**
```bash
grep -E "registered client|resized app|TAB|DELETE|display mode" \
  "/c/Users/Sparks i7 3080/AppData/Local/DisplayXR/$(ls -t '/c/Users/Sparks i7 3080/AppData/Local/DisplayXR/' | grep service | head -1)" \
  | head -15
```

**Shell controls:**
- **TAB** — cycle focus: app 0 → app 1 → unfocused → app 0 (cyan border)
- **DELETE** — close focused app (dark gray replaces it)
- **ESC** — dismiss shell, switch display to 2D
- **V** — toggle 2D/3D display mode
- **WASD/mouse** — forwarded to focused app

Logs: `%LOCALAPPDATA%\DisplayXR\*.log`

## Key Files

See `shell-phase1-plan.md` for the full file reference from Phase 0.
