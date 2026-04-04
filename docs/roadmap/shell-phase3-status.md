# Shell Phase 3 — Implementation Status

Last updated: 2026-04-03 (branch `feature/shell-phase3-ci`)

## Phase 3+ Dynamic Spatial Layouts (Experimental)

Exploratory dynamic layout system — continuously-driven window poses with auto-animation and interactive drag. These are UI experiments; the interaction model needs deeper design work before productization.

### Dynamic Layouts
| Key | Mode | Description |
|-----|------|-------------|
| Ctrl+5 | Carousel | Full 360° circle, auto-spins ~10°/s. Title bar drag controls rotation with momentum. Content click pauses rotation + forwards to app. TAB snaps focused to front + 5s pause. Scroll adjusts radius. |
| Ctrl+6 | Orbital | Elliptical multi-orbit. Focused window on inner orbit (largest). Others orbit at increasing radii with different speeds. |
| Ctrl+7 | Helix | Vertical spiral staircase. Windows rotate + move up/down cyclically. |
| Ctrl+8 | Expose | Grid overview (Mission Control style). Click a window to select and return to previous layout. |

### Architecture
- `dynamic_layout` state on `d3d11_multi_compositor`: mode, angle_offset, angular_velocity, drag state, pause_until_ns
- `dynamic_layout_tick()`: called every frame, computes poses from layout math, updates pixel rects
- Per-layout compute functions: `carousel_compute_pose()`, `orbital_compute_pose()`, `helix_compute_pose()`, `expose_compute_pose()`
- Z-depth comfort: all layouts clamp Z within ±zmax where zmax = max(display_w, display_h) / 5

### Other Changes
- **Z-depth render order**: painter's algorithm (back-to-front by Z), replaces focus-based ordering
- **Focus border in Z-order**: drawn inside per-slot render loop, not as separate post-pass
- **Animation system**: `slot_animate_to()` + `slot_animate_tick()` with ease-out cubic, 300ms for layout transitions, 400ms for window entry
- **Input forwarding**: rect updated every frame in dynamic mode (windows move continuously)

## What Works (from Phase 2)

All Phase 2 features are on branch `feature/shell-phase2-ci`:
- Window chrome: title bars with app name, close (X) and minimize (—) buttons, button hover highlights
- Layout presets: Ctrl+1 side-by-side, Ctrl+2 stacked (Phase 3 replaced Ctrl+3-4, added Ctrl+5)
- Edge/corner resize: asymmetric (only dragged edge moves), continuous HWND update, mouse suppression during resize
- Minimize/maximize: minimize button, taskbar with indicators, double-click title bar maximize toggle
- Persistence: `%LOCALAPPDATA%\DisplayXR\shell_layout.json` with numbered app instance names
- Spatial raycasting: `shell_raycast_hit_test()` — eye→cursor→window plane in meters
- Per-tile scissor rects: uniform overflow clipping on all edges
- Cursor feedback: arrow/move/resize arrows via `WM_SETCURSOR` on window thread
- Right-click forwards to app (focus on RMB down)
- All UI dimensions in meters (rendering converts to tile pixels, hit-testing uses meters directly)

## Phase 3 Progress

### Phase 3A: Z-Depth Positioning
**Status:** Done (locally tested on Leia display, 2026-04-03)

Windows at arbitrary Z depths with per-eye parallax.

| Task | Status | Notes |
|------|--------|-------|
| 3A.1 Per-eye parallax in slot_pose_to_pixel_rect | ✅ | `slot_pose_to_pixel_rect_for_eye()`: projects window through eye to Z=0 plane, scale = eye_z / (eye_z - win_z) |
| 3A.2 Different blit positions in left/right SBS halves | ✅ | `eye_rect[2]` arrays computed per slot, used in per-view blit loop. Left/right eyes see window at different positions. |
| 3A.3 Apparent size scaling by Z | ✅ | Built into the projection math: closer windows appear larger (scale > 1) |
| 3A.4 Raycast with non-zero Z | ✅ | Already implemented in Phase 2, verified working with Z != 0 |
| 3A.5 Z-depth controls | ✅ | Shift+Scroll (2mm/notch), [/] keys (5mm steps), range ±50mm |
| 3A.6 Chrome at window Z | ✅ | Title bar, buttons, text, focus border all use per-eye projected rects |

### Phase 3B: Angled Windows
**Status:** Done (locally tested on Leia display, 2026-04-03)

Windows with non-identity orientation (tilted/rotated).

| Task | Status | Notes |
|------|--------|-------|
| 3B.1 Perspective-correct quad blit | ✅ | Extended `BlitConstants` with `quad_mode`, `quad_corners_01/23`, `quad_w`. VS branches: axis-aligned (w=1) vs perspective quad (w=depth). |
| 3B.2 Quad corner computation | ✅ | `compute_projected_quad_corners()`: rotate 4 corners by quaternion, project per-eye to Z=0 plane. `project_local_rect_for_eye()` for arbitrary local-space rects. |
| 3B.3 Shader update for arbitrary quads | ✅ | Blit VS uses `output.position = float4(ndc * w, 0, w)` for perspective-correct UV interpolation. D3D11 rasterizer auto-divides by w. |
| 3B.4 Raycast with rotated planes | ✅ | Plane normal from `math_quat_rotate_vec3(q, (0,0,1))`, ray-plane intersection via dot product, hit→local coords via inverse rotation. |
| 3B.5 Chrome on rotated plane | ✅ | All chrome elements (title bar bg, close/minimize buttons, text glyphs, focus border) rendered as perspective quads via `CHROME_BLIT_POS` macro + `project_local_rect_for_eye`. |

**Rotation controls:**
- Title bar RMB drag: horizontal = yaw ±30°, vertical = pitch ±15° (~1°/10px)
- Rising-edge detection (`rmb_held && !prev_rmb_held`) avoids GetAsyncKeyState press-bit consumption

**Window-local Kooima eyes:**
- Added `window_orientation` (quaternion) and `window_center_offset_z_m` to `xrt_window_metrics`
- IPC server handler (`ipc_server_handler.c`): transforms eye positions to window-local frame via `local_eye = Q_inv * (eye - window_pos)`
- Apps receive correct Kooima projection for rotated/translated windows

### Phase 3C: 3D Layout Presets
**Status:** Done (locally tested on Leia display, 2026-04-03)

Layout presets exploiting depth and angle.

| Task | Status | Notes |
|------|--------|-------|
| 3C.1 Theater layout (Ctrl+3) | ✅ | Focused: Z=0, 60% display. Others: Z=-3cm, ±15° yaw inward, 40% width, flanking left/right. |
| 3C.2 Stack layout (Ctrl+4) | ✅ | Card pile: focused at Z=+2cm, others behind with 5mm XY offset per layer. TAB reorders Z. |
| 3C.3 Carousel layout (Ctrl+5) | ✅ | Semicircle: 60° arc, 15cm radius. Center window slightly forward. Yaw to face center. |
| 3C.4 Key bindings | ✅ | Ctrl+3=Theater, Ctrl+4=Stack, Ctrl+5=Carousel. Ctrl+1-2 unchanged (SBS, stacked). `current_layout` field tracks active preset for TAB Z-reorder. |

## Key Architecture Decisions

### Perspective Quad Rendering Pipeline
```
Per window, per eye:
  1. If rotated: compute 4 world-space corners (rotate local by quaternion + translate)
  2. Project each corner through eye to Z=0 plane (similar triangles)
  3. Compute per-corner W = eye_z - corner_z (for perspective-correct UV)
  4. Convert to SBS tile pixel coordinates
  5. Pass as quad_corners_01/23 + quad_w in BlitConstants
  6. VS: output.position = float4(ndc * w, 0, w) → D3D11 perspective-correct interpolation
```

### HLSL Constant Buffer Alignment
`float2[4]` in HLSL has 16-byte-per-element stride (array padding), but C `float[8]` is tightly packed. Fixed by packing as two `float4` values: `quad_corners_01` (TL.xy, BL.xy) and `quad_corners_23` (TR.xy, BR.xy).

### IPC vs In-Process Eye Transform
Shell apps connect via IPC → the Kooima eye transform runs in `ipc_server_handler.c`, NOT `oxr_session.c`. Both paths now apply the same `Q_inv * (eye - window_pos)` transform.

## Shell Controls (Updated)

| Input | Action |
|-------|--------|
| Left-click | Focus window (cyan border, z-order on top). Click title bar buttons: close (X), minimize (—) |
| Left-click-drag title bar | Move window in display plane (move cursor shown) |
| Left-click-drag edge/corner | Resize window (resize cursor shown, asymmetric) |
| **Right-click-drag title bar** | **Rotate window: horizontal=yaw ±30°, vertical=pitch ±15°** |
| Right-click content | Focus window + forward to app |
| Double-click title bar | Toggle maximize / restore |
| Scroll wheel | Resize focused window (~5% per notch) |
| **Shift+Scroll** | **Z-depth: ±2mm per notch (range ±50mm)** |
| **[ / ]** | **Z-depth: ±5mm steps** |
| Ctrl+1 | Layout: side-by-side |
| Ctrl+2 | Layout: stacked |
| **Ctrl+3** | **Layout: Theater (focused at Z=0, others recessed + angled)** |
| **Ctrl+4** | **Layout: Stack (card pile, TAB reorders Z)** |
| **Ctrl+5** | **Layout: Carousel (semicircle arrangement)** |
| TAB | Cycle focus (+ reorder Z in Stack layout) |
| DELETE | Close focused app |
| ESC | Dismiss shell (2D mode) |
| V | Toggle 2D/3D display mode |

## Known Issues (Inherited)

### Service crash with 3+ apps (#108) — Open
Inter-app launch delay reduced from 3s to 100ms. **2-app launch is stable.** **3+ apps crash the service** — log ends abruptly (segfault) a few frames after clients register. The `render_mutex` from Phase 2 handles the 2-client case but there's a deeper threading/D3D11 issue with 3+ simultaneous clients. Needs debugger session with 3-app repro.

### Apps don't survive shell exit (Phase 1A deferred)
ESC dismisses shell, apps become invisible. Must relaunch apps after shell revival.

## Key Source Files

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp, render loop, spatial raycast, per-eye parallax, perspective quad projection, rotation drag, 3D presets, chrome rendering |
| `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` | Blit shaders: `BlitConstants` (quad_mode, quad_corners, quad_w), perspective-correct VS |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Window WndProc, WM_SETCURSOR, input forwarding |
| `src/xrt/ipc/server/ipc_server_handler.c` | IPC Kooima: window-local eye transform (Q_inv * (eye - pos)) |
| `src/xrt/include/xrt/xrt_display_metrics.h` | `xrt_window_metrics`: window_orientation, window_center_offset_z_m |
| `src/xrt/state_trackers/oxr/oxr_session.c` | In-process Kooima: window-local eye transform (same math) |
| `src/xrt/targets/shell/main.c` | Shell launcher, persistence, --pose args |
| `src/xrt/auxiliary/math/m_display3d_view.h` | Kooima projection math |

## How to Test

```bash
scripts\build_windows.bat build && scripts\build_windows.bat test-apps
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

**Z-depth test:** Shift+Scroll or [/] to move window forward/back. Window shows stereo parallax on Leia display.

**Rotation test:** Right-click-drag on title bar to rotate. Content, chrome, and border all rotate together. App content updates Kooima projection for the rotated viewing angle.

**Preset test:** Ctrl+3 Theater, Ctrl+4 Stack (TAB to cycle focus + reorder Z), Ctrl+5 Carousel.

**Pose CLI test:**
```bash
_package\bin\displayxr-shell.exe --pose -0.1,0.05,0.03,0.14,0.08 app1.exe --pose 0.1,0.05,-0.02,0.14,0.08 app2.exe
```

Logs: `%LOCALAPPDATA%\DisplayXR\*.log`
