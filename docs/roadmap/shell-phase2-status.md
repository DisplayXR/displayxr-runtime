# Shell Phase 2 — Implementation Status

Last updated: 2026-04-01 (branch `feature/shell-phase2-ci`)

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
**Status:** Done (locally tested on Leia display, 2026-04-01)

Title bars with app name and close button, rendered server-side in the compositor.

| Task | Status | Notes |
|------|--------|-------|
| 2A.1 Title bar rendering | ✅ | 24px dark blue-gray strip above each window, solid-color blit with configurable RGB via `src_rect` |
| 2A.2 App name text | ✅ | 8x16 bitmap font in `d3d11_bitmap_font.h` (public domain VGA font), 768x16 font atlas texture, alpha-blended glyph rendering with point sampler |
| 2A.3 Close button | ✅ | Red rect + white X glyph at right end of title bar; click sends EXIT_REQUEST. Rising-edge LMB detection for reliable single-click. |
| 2A.4 Title bar drag | ✅ | Left-click-drag on title bar moves window (title_drag state machine); right-click-drag also works on title bar. Fractional SBS-aware positioning. |

**Key implementation details:**
- Blit PS shader now reads solid color from `src_rect.rgb` when `convert_srgb > 1.5` (was hardcoded cyan)
- Title bars use fractional positioning (`fx * half_w`) matching the content blit — required for correct SBS rendering
- Focus border (cyan) encompasses title bar + content area
- LMB click detection uses rising-edge (`lmb_held && !prev_lmb_held`) instead of `GetAsyncKeyState & 1` for reliability
- App name populated via `GetWindowTextA()` on the app's HWND at client registration, fallback to "App N"

### Phase 2B: Layout Presets
**Status:** Done (locally built, 2026-04-01)

One-key layout switching for common arrangements.

| Task | Status | Notes |
|------|--------|-------|
| 2B.1 Layout algorithms | ✅ | `apply_layout()` with 4 modes: side-by-side, stacked, fullscreen, cascade |
| 2B.2 Key triggers | ✅ | Ctrl+1-4 in render loop via GetAsyncKeyState. Ctrl+digit suppressed in WndProc. |
| 2B.3 Pose computation | ✅ | Each layout computes pose + size per active non-minimized client, calls `slot_pose_to_pixel_rect` |
| 2B.4 Animated transitions | — | Deferred (instant snap for now) |

### Phase 2C: Close / Minimize / Maximize
**Status:** Done (locally built, 2026-04-01)

Window management actions.

| Task | Status | Notes |
|------|--------|-------|
| 2C.1 Close from chrome | ✅ | Done in Phase 2A (title bar X button) |
| 2C.2 Minimize | ✅ | `minimized` flag on slot. Gray minimize button (—) in title bar. Skipped in render/hit-test. Focus advances on minimize. |
| 2C.3 Maximize | ✅ | Double-click title bar toggles maximize. Saves/restores pre_max state. |
| 2C.4 Taskbar | ✅ | 28px dark strip at bottom when minimized windows exist. Indicators with app name (first 6 chars). Click to un-minimize. |
| 2C.5 IPC | ✅ | `shell_set_visibility(client_id, visible)` in proto.json + handler. `comp_d3d11_service_set_client_visibility()`. |

### Phase 2D: Persistence
**Status:** Done (locally built, 2026-04-01)

Window layout saved to JSON, restored on restart.

| Task | Status | Notes |
|------|--------|-------|
| 2D.1 Config file | ✅ | `%LOCALAPPDATA%\DisplayXR\shell_layout.json` via cJSON (already linked via aux_util) |
| 2D.2 Restore on connect | ✅ | Shell detects new clients, checks config for saved pose by app name, applies via `shell_set_window_pose` |
| 2D.3 Save on change | ✅ | Every 5 seconds, shell polls all client poses via `shell_get_window_pose` IPC, saves if changed |
| 2D.4 Shell reads config | ✅ | `shell_config_load()` at startup, `shell_config_save()` on changes |
| 2D.5 IPC | ✅ | `shell_get_window_pose(client_id)` → returns pose + width_m + height_m |

### Phase 2E: Edge/Corner Resize + DPI Scaling
**Status:** Done (locally tested, 2026-04-01)

| Task | Status | Notes |
|------|--------|-------|
| 2E.1 Edge/corner resize | ✅ | Left-click-drag on window edges/corners. Asymmetric (only dragged edge moves). Resize zone DPI-scaled. |
| 2E.2 Continuous HWND resize | ✅ | App HWND resized every frame during drag (no content distortion). `SWP_ASYNCWINDOWPOS`. |
| 2E.3 Mouse suppression | ✅ | Mouse events not forwarded to app during resize (prevents camera animation). |
| 2E.4 Resize cursors | ✅ | Cursor changes to resize arrows (↔ ↕ ⤡ ⤢) when hovering near edges/corners. |
| 2E.5 DPI-aware UI | ✅ | All UI (title bars, buttons, glyphs, taskbar, resize zone) scaled via `GetDpiForWindow()`. Fallback to resolution heuristic. |
| 2E.6 Title bar z-order | ✅ | Title bars render inside render_order loop — foreground window always covers background title bars. |

## Known Issues

### Intermittent crash with two apps (#108)
Service crashes intermittently when two apps run simultaneously. Race condition or D3D11 threading issue. Needs debugger session.

### Apps don't survive shell exit (Phase 1A deferred)
ESC dismisses shell, apps become invisible. Must relaunch apps after shell revival.

### Title bar button hit-test slight offset
The minimize button click target area is shifted a few pixels to the right of the visual button. Close button has a similar minor offset. Root cause: the visual rendering uses fractional SBS-half coordinates while the hit-test uses full-display `window_rect` coordinates. These match in tile_columns=1 (2D mode) but may drift slightly due to float→int rounding with DPI scaling. Workaround: aim slightly left of the visual button center.

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
