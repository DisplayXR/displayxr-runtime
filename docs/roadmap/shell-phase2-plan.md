# Shell Phase 2: Usable Shell

## Prerequisites

Phase 1 is complete on branch `feature/shell-phase1-ci`. All features working:
- `displayxr-shell.exe` single-command launcher (auto-starts service, activates shell mode, launches apps)
- Dynamic window poses with full 3D xrt_pose + `shell_set_window_pose` IPC
- Click-to-focus, right-click-drag to move, scroll-to-resize
- Z-ordering by focus, mouse coord remapping, blit clamping
- Shell revival after ESC dismiss
- `shell_activate` IPC for dynamic shell mode (no `--shell` flag needed)
- Per-app `--pose` args for programmatic window placement

See `shell-phase1-status.md` for full details, design decisions, and lessons learned.

### Known Issues from Phase 1

- **Intermittent crash with two apps** (#108) — race condition or D3D11 threading issue. Needs debugger session.
- **Apps don't survive shell exit** (1.A deferred) — ESC dismisses shell, apps become invisible. Revival creates new shell window but apps must be relaunched.

## Phase 2 Goals

Make the shell usable for daily work: window chrome with title bars, layout presets, app close/minimize, and persistence. Focus on features that make the 3D desktop functional beyond a tech demo.

## Phase 2A: Window Chrome

**Goal:** Each 3D window has a visible title bar showing the app name, with a close button.

| Task | Description |
|------|-------------|
| 2A.1 | Render title bar as a colored strip above each window's blit rect in the combined atlas |
| 2A.2 | Draw app name text in the title bar (bitmap font or pre-rendered glyphs) |
| 2A.3 | Close button (X) in title bar — click sends EXIT_REQUEST to the app |
| 2A.4 | Title bar acts as drag handle — right-click-drag on title bar moves window (reuse existing drag) |

**Key design:** Title bars are rendered server-side in `multi_compositor_render`, not by the app. They're drawn as additional blit rects above each window's content area. The app name comes from `ipc_app_state.info.application_name` (already available via `system_get_client_info`).

**Simplest approach for text:** Pre-render a small bitmap font atlas (8×16 ASCII glyphs) as a compiled-in texture. Blit individual glyphs to spell out the app name. No FreeType or font loading needed.

## Phase 2B: Layout Presets

**Goal:** One-key layout switching: side-by-side, stacked, single-fullscreen, cascade.

| Task | Description |
|------|-------------|
| 2B.1 | Define layout algorithms: side-by-side (horizontal split), stacked (vertical), fullscreen (focused app fills display), cascade (overlapping with offset) |
| 2B.2 | Number keys 1-4 trigger layout presets (qwerty handler, server-side) |
| 2B.3 | Each layout computes poses for all active clients, calls `slot_pose_to_pixel_rect` for each |
| 2B.4 | Smooth animated transitions (lerp pose over ~200ms) — optional, can start with instant snap |

**Key design:** Layouts are server-side in the render loop. When a layout key is pressed, iterate active slots, compute new poses based on the layout algorithm and client count, update `window_pose` + `window_rect_*` + `hwnd_resize_pending`. The existing blit pipeline handles the rest.

## Phase 2C: Close / Minimize / Maximize

**Goal:** Window management actions beyond DELETE.

| Task | Description |
|------|-------------|
| 2C.1 | Close: same as DELETE (EXIT_REQUEST) but triggered from title bar button |
| 2C.2 | Minimize: hide window from rendering (skip in render_order) but keep client connected. IPC flag `shell_set_visibility(client_id, false)` |
| 2C.3 | Maximize: focused window fills display. Others hidden. Toggle back to previous layout. |
| 2C.4 | Taskbar strip at bottom showing minimized app indicators (colored dots with app name initial) |

## Phase 2D: Persistence

**Goal:** Window layout survives shell restart. Per-app window positions saved to JSON.

| Task | Description |
|------|-------------|
| 2D.1 | JSON config file: `%LOCALAPPDATA%\DisplayXR\shell_layout.json` |
| 2D.2 | On app connect: check if config has a saved pose for this app name, apply it |
| 2D.3 | On pose change (drag, resize, layout): save updated poses to config |
| 2D.4 | Shell app reads config on startup, applies saved poses as clients connect via `shell_set_window_pose` |

## Key Architecture Notes

### Server-side vs shell-side rendering

Phase 2 continues the Phase 1 pattern: **window management is server-side**. Title bars, layout presets, and minimize/maximize are all handled in the multi-compositor render loop. The shell app (`displayxr-shell.exe`) is a launcher and persistence layer, not a renderer.

This means:
- No additional rendering framework needed in the shell app
- Title bars and chrome are shader-blit operations in the existing pipeline
- Layout algorithms run where the slot data lives (server-side)
- The shell app's role grows from "launcher + monitor" to "launcher + monitor + persistence + future GUI"

### IPC additions needed

| IPC Call | Direction | Description |
|----------|-----------|-------------|
| `shell_set_visibility` | shell → runtime | Hide/show a client's window (minimize) |
| `shell_get_window_poses` | shell → runtime | Get current poses for all clients (for persistence) |
| `shell_set_layout` | shell → runtime | Apply a named layout preset |

### Text rendering strategy

For title bars, use a minimal compiled-in bitmap font:
- 8×16 pixel ASCII glyphs (96 printable characters)
- Stored as a compiled-in byte array (no file I/O)
- Rendered via the existing blit shader with per-glyph source rects
- Total texture: 768×16 pixels (96 glyphs × 8px wide)

## Files from Phase 1 (key reference)

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp, render, slots, drag, resize, z-order |
| `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` | Blit + border shaders |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Window WndProc, input forwarding, scroll accum |
| `src/xrt/ipc/server/ipc_server_handler.c` | shell_activate, shell_set_window_pose handlers |
| `src/xrt/ipc/shared/proto.json` | IPC protocol definitions |
| `src/xrt/targets/shell/main.c` | Shell app: launcher, monitor, pose assignment |

## Test Procedure

```bash
# Build
scripts\build_windows.bat build
scripts\build_windows.bat test-apps

# Run (single command)
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe

# Or with custom poses
_package\bin\displayxr-shell.exe --pose -0.1,0.05,0,0.14,0.08 app1.exe --pose 0.1,0.05,0,0.14,0.08 app2.exe
```

**Shell controls (Phase 1, carried forward):**
- **Left-click** — focus window (cyan border, z-order on top)
- **Right-click-drag** — move window in display plane
- **Scroll wheel** — resize focused window
- **TAB** — cycle focus
- **DELETE** — close focused app
- **ESC** — dismiss shell (2D mode), reopen on new app connect
- **V** — toggle 2D/3D
- **WASD/left-click-drag** — forwarded to focused app
