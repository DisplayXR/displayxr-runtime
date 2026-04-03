# Shell Phase 3 — Implementation Status

Last updated: 2026-04-02 (branch `feature/shell-phase2-ci`)

## What Works (from Phase 2)

All Phase 2 features are on branch `feature/shell-phase2-ci`:
- Window chrome: title bars with app name, close (X) and minimize (—) buttons, button hover highlights
- Layout presets: Ctrl+1 side-by-side, Ctrl+2 stacked, Ctrl+3 fullscreen, Ctrl+4 cascade
- Edge/corner resize: asymmetric (only dragged edge moves), continuous HWND update, mouse suppression during resize
- Minimize/maximize: minimize button, taskbar with indicators, double-click title bar maximize toggle
- Persistence: `%LOCALAPPDATA%\DisplayXR\shell_layout.json` with numbered app instance names
- Spatial raycasting: `shell_raycast_hit_test()` — eye→cursor→window plane in meters
- Per-tile scissor rects: uniform overflow clipping on all edges
- Cursor feedback: arrow/move/resize arrows via `WM_SETCURSOR` on window thread
- Right-click forwards to app (focus on RMB down)
- All UI dimensions in meters (rendering converts to tile pixels, hit-testing uses meters directly)
- App instance numbering: "AppName", "AppName (2)" for duplicates
- Title text: strip subtitle after " - ", replace non-ASCII, 16x32 glyph rendering

See `shell-phase2-status.md` for full design decisions and lessons learned.

## Phase 3 Progress

### Phase 3A: Z-Depth Positioning
**Status:** Not started

Windows at arbitrary Z depths with per-eye parallax.

| Task | Status | Notes |
|------|--------|-------|
| 3A.1 Per-eye parallax in slot_pose_to_pixel_rect | | Project window through eye to display plane |
| 3A.2 Different blit positions in left/right SBS halves | | Parallax offset from eye separation |
| 3A.3 Apparent size scaling by Z | | Similar triangles: closer = larger |
| 3A.4 Raycast with non-zero Z | | Already implemented, needs testing |
| 3A.5 Z-depth controls | | Mouse wheel + modifier, --pose z arg |
| 3A.6 Chrome at window Z | | Title bar/buttons follow window depth |

### Phase 3B: Angled Windows
**Status:** Not started

Windows with non-identity orientation (tilted/rotated).

| Task | Status | Notes |
|------|--------|-------|
| 3B.1 Perspective-correct quad blit | | Replace axis-aligned blit with projected quad |
| 3B.2 Quad corner computation | | Apply orientation quaternion, project per-eye |
| 3B.3 Shader update for arbitrary quads | | Pass 4 corner positions to VS |
| 3B.4 Raycast with rotated planes | | Plane normal from quaternion |
| 3B.5 Chrome on rotated plane | | Title bar follows window tilt |

### Phase 3C: 3D Layout Presets
**Status:** Not started

Layout presets exploiting depth and angle.

| Task | Status | Notes |
|------|--------|-------|
| 3C.1 Theater layout | | Focused at Z=0, others recessed + angled |
| 3C.2 Carousel layout | | Semicircle at varying Z |
| 3C.3 Stack layout | | Increasing Z depths, offset |
| 3C.4 Key bindings | | Ctrl+5/6/7 or replace 2D presets |

## Known Issues (Inherited)

### Intermittent crash with two apps (#108)
Service crashes intermittently when two apps run simultaneously. Race condition or D3D11 threading issue. Needs debugger session.

### Apps don't survive shell exit (Phase 1A deferred)
ESC dismisses shell, apps become invisible. Must relaunch apps after shell revival.

## How to Launch the Shell

See `shell-phase2-status.md` for full launch instructions. Summary:

```bash
scripts\build_windows.bat build && scripts\build_windows.bat test-apps
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

### Key source files

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp, render loop, `shell_raycast_hit_test()`, spatial UI, layout presets, resize, chrome |
| `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` | Blit + border shaders |
| `src/xrt/compositor/d3d11_service/d3d11_bitmap_font.h` | 8x16 bitmap font atlas |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | WndProc, WM_SETCURSOR, input forwarding |
| `src/xrt/ipc/shared/proto.json` | IPC protocol (shell_set_window_pose, shell_set_visibility, shell_get_window_pose) |
| `src/xrt/targets/shell/main.c` | Shell launcher, persistence, numbered instance tracking |
| `src/xrt/auxiliary/math/m_display3d_view.h` | Kooima projection math |

Logs: `%LOCALAPPDATA%\DisplayXR\*.log`
