# Shell Phase 3: 3D Window Positioning

## Prerequisites

Phase 2 is complete on branch `feature/shell-phase2-ci`. All features working:
- Window chrome: title bars, close/minimize buttons, maximize toggle, bitmap font text
- Layout presets (Ctrl+1-4): side-by-side, stacked, fullscreen, cascade
- Edge/corner resize (asymmetric, continuous HWND update)
- Taskbar for minimized apps
- Persistence (JSON config per numbered app instance)
- Spatial raycasting hit-test (eye position through cursor to window plane in meters)
- Per-tile scissor clipping (uniform overflow on all edges, no SBS wrapping)
- Cursor feedback (move, resize, arrow, button highlights via WM_SETCURSOR)
- Right-click forwards to app (no more RMB drag)

See `shell-phase2-status.md` for full details, design decisions, and lessons learned.

### Known Issues from Phase 2

- **Intermittent crash with two apps** (#108) — race condition or D3D11 threading issue.
- **Apps don't survive shell exit** (Phase 1A deferred) — ESC dismisses, apps invisible.

## Phase 3 Goals

Enable true 3D window positioning: windows can be placed at different Z depths (in front of or behind the display surface), rotated at angles, and the Kooima projection + blit pipeline handles correct perspective rendering. This is the defining feature of a spatial shell on a 3D display.

## Architecture: What Already Exists

### Spatial Foundation (from Phase 2)

Phase 2 established the spatial architecture that Phase 3 builds on:

1. **Every window has a full `xrt_pose`** (position + orientation in meters from display center). Currently only `position.x/y` are used; `position.z` is always 0 and `orientation` is identity.

2. **All UI dimensions are in meters** (`UI_TITLE_BAR_H_M = 0.008f`, etc.). Rendering converts meters to tile pixels via `ui_m_to_tile_px_x/y()`. Hit-testing uses meters directly.

3. **Spatial raycasting** (`shell_raycast_hit_test()`): casts a ray from the user's tracked eye position through the cursor point on the display surface, intersects with window planes. Already handles arbitrary window Z via ray-plane intersection:
   ```cpp
   float t = (win_z - eye_z) / ray_dz;
   float hit_x = eye_x + t * ray_dx;
   float hit_y = eye_y + t * ray_dy;
   ```

4. **Eye tracking** available via `comp_d3d11_service_get_predicted_eye_positions()`.

5. **Per-tile scissor rects** prevent SBS cross-half bleed for any tile layout.

### What Phase 3 Needs to Add

Currently, rendering assumes all windows are coplanar with the display (Z=0, identity orientation). Phase 3 must handle:

1. **Non-zero Z** — Windows at Z > 0 (in front of display) or Z < 0 (behind display). The blit dest rect must account for parallax: a window closer to the viewer appears larger and its position shifts with eye position.

2. **Rotated windows** — `window_pose.orientation` (quaternion) defines window tilt. A window angled toward the viewer requires perspective-correct quad rendering instead of axis-aligned blit.

3. **Double-level Kooima projection** — The app renders with Kooima projection for its virtual window (Level 1). The compositor renders the window into the combined atlas using Kooima projection for the window's pose relative to the physical display (Level 2). The display processor then weaves for the physical lenticular (Level 3). See `docs/architecture/kooima-projection.md`.

4. **Per-eye parallax in the combined atlas** — For a window at Z > 0, the left-eye and right-eye views see it at different positions. The SBS atlas left/right halves must render the window at different pixel positions (parallax shift).

## Phase 3A: Z-Depth Positioning (Windows in Front/Behind)

**Goal:** Windows can be placed at arbitrary Z depths. A window at Z=0.05m hovers 5cm in front of the display; at Z=-0.05m it recedes behind.

| Task | Description |
|------|-------------|
| 3A.1 | Update `slot_pose_to_pixel_rect()` to account for Z-depth parallax per eye |
| 3A.2 | Blit each window at different positions in left/right SBS halves (parallax offset from eye separation) |
| 3A.3 | Scale window apparent size based on Z (closer = larger, via similar triangles with eye distance) |
| 3A.4 | Update raycast hit-test (already supports non-zero Z — just needs testing) |
| 3A.5 | Shell commands to move window forward/back: mouse wheel + modifier key, or CLI arg `--pose x,y,z,w,h` |
| 3A.6 | Title bar and chrome follow window Z (rendered at same depth) |

**Key math — per-eye parallax:**
```
For a window at position (wx, wy, wz) viewed by eye at (ex, ey, ez):
  // Project window center onto display plane (Z=0) through eye
  t = -ez / (wz - ez)   // parameter where ray hits Z=0
  projected_x = ex + t * (wx - ex)
  projected_y = ey + t * (wy - ey)
  
  // Scale factor: closer windows appear larger
  scale = ez / (ez - wz)
  projected_w = window_width_m * scale
  projected_h = window_height_m * scale
```

Left eye and right eye project to different positions — this creates the 3D depth effect on the lenticular display.

## Phase 3B: Angled Windows (Rotation)

**Goal:** Windows can be tilted/rotated. A window angled 15 degrees toward the viewer shows correct perspective.

| Task | Description |
|------|-------------|
| 3B.1 | Replace axis-aligned blit with perspective-correct quad rendering (transform 4 corner vertices by window pose) |
| 3B.2 | Compute quad corners: apply orientation quaternion to window rect corners, then project per-eye |
| 3B.3 | Update blit shader to accept arbitrary quad vertices (not just axis-aligned dst_offset + dst_rect_wh) |
| 3B.4 | Update raycast: ray-plane intersection with rotated plane (plane normal from orientation quaternion) |
| 3B.5 | UI chrome (title bar, buttons) renders on the rotated window plane |

**Key design:** The existing blit VS computes NDC position from `dst_offset + uv * dst_rect_wh` (axis-aligned rectangle). For rotated windows, we need to pass 4 corner positions and interpolate UV. Options:
- Modify blit VS to accept 4 corner positions as a float4x2 in the constant buffer
- Use the existing quad layer VS/PS which already handles arbitrary 3D quads via MVP matrix

## Phase 3C: Layout Presets for 3D

**Goal:** Layout presets that exploit depth and angle.

| Task | Description |
|------|-------------|
| 3C.1 | "Theater" layout: focused window at Z=0 (display surface), others recessed at Z=-0.03 and angled inward |
| 3C.2 | "Carousel" layout: windows arranged in a semicircle at varying Z, angled to face the viewer |
| 3C.3 | "Stack" layout: windows at increasing Z depths, slightly offset, creating a card-stack effect |
| 3C.4 | Ctrl+5/6/7 for 3D layout presets (or replace some 2D presets) |

## Key Architecture Notes

### Rendering Pipeline for 3D Windows

```
Per window, per eye:
  1. Compute projected quad corners on display plane (eye → window corners → Z=0 projection)
  2. Convert to SBS-tile pixel coordinates
  3. Set scissor rect to tile bounds
  4. Blit window content from client atlas to projected quad (perspective-correct UV)
  5. Draw title bar and chrome on the projected window plane
```

### Spatial Hit-Testing (Already Implemented)

The `shell_raycast_hit_test()` function already handles non-zero Z:
```cpp
float t = (win_z - eye_z) / ray_dz;
float hit_x = eye_x + t * ray_dx;
float hit_y = eye_y + t * ray_dy;
```

For rotated windows (Phase 3B), this becomes a ray-plane intersection where the plane normal is derived from the window's orientation quaternion.

### Double-Level Kooima

1. **Level 1 (app):** App renders its 3D scene using Kooima projection for its virtual window. Eye position comes from the IPC server based on DP tracking + window offset.
2. **Level 2 (compositor):** Shell compositor renders window content into the combined atlas. For windows at Z=0, this is a simple blit. For Z != 0 or rotated, this is a perspective-projected quad blit.
3. **Level 3 (DP):** Display processor weaves the combined atlas for the lenticular display.

### Lessons Learned from Phase 2 (Critical for Phase 3)

#### 1. Pixel coordinates vs spatial coordinates
Phase 2 spent significant time debugging hit-test misalignment because button positions were defined in pixels but rendered at fractional SBS-tile positions. **Resolution:** All UI dimensions are now defined in meters. Rendering converts meters → tile pixels. Hit-testing uses meters directly via raycasting. Phase 3 MUST continue this pattern — never define positions in pixels.

#### 2. SBS tiling and per-tile scissor rects
Content rendered in one SBS half that overflows bleeds into the adjacent eye's half (wrapping). The fix was per-tile scissor rects (`D3D11_RECT` set via `RSSetScissorRects`). The rasterizer state has `ScissorEnable = TRUE`. Phase 3 must maintain this — the parallax-shifted per-eye rendering makes overflow more likely.

#### 3. Thread safety for cursor
`SetCursor()` only works from the thread that owns the window. The render loop runs on the compositor thread, the window runs on a dedicated thread. **Resolution:** Cursor ID is communicated via `InterlockedExchange` on `desired_cursor`, applied in `WM_SETCURSOR` handler on the window thread. Use `SetCursorPos(p.x, p.y)` nudge to force immediate cursor update on drag/resize release.

#### 4. GetAsyncKeyState double-call consumes press bit
Calling `GetAsyncKeyState(VK_LBUTTON)` twice in the same frame — once for `& 0x8000` (held) and once for `& 1` (just pressed) — the first call consumes the `& 1` bit. **Resolution:** Call once, store result, check both bits. Or use rising-edge detection: `lmb_held && !prev_lmb_held`.

#### 5. HWND title contains Unicode
App HWND titles may contain Unicode characters (em dash `\u2014` etc.) that `GetWindowTextA` converts to ANSI replacement chars. **Resolution:** Replace non-ASCII chars with `-` in the compositor's app_name field.

#### 6. Duplicate app instances
When two instances of the same app run, they share the same `application_name`. Persistence and display need unique names. **Resolution:** Compositor appends "(N)" suffix: "AppName", "AppName (2)". Shell tracks client_id → numbered_name mapping for persistence.

#### 7. Window overflow and clamping
Early implementations clamped window positions and sizes to display bounds, causing squishing (content distortion). Later, removing clamping caused SBS wrapping. **Resolution:** No position/size clamping in `slot_pose_to_pixel_rect()` (output is `int32_t`, can be negative). Proper source+dest clipping in the blit for off-screen portions, plus per-tile scissor rects to prevent SBS wrapping.

#### 8. Right-click should forward to app
Initially right-click was reserved for window drag. Users expect right-click to work in apps (camera rotation, context menus). **Resolution:** Removed right-click drag entirely. RMB now focuses window and forwards to app. Title bar left-click-drag handles window movement.

## Files from Phase 2 (Key Reference)

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp, render loop, spatial raycast, slots, resize, z-order, chrome, layout presets |
| `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` | Blit + border shaders (solid-color mode via `src_rect.rgb`) |
| `src/xrt/compositor/d3d11_service/d3d11_bitmap_font.h` | 8x16 VGA bitmap font (96 ASCII glyphs, 1536 bytes) |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.h` | Public API: set_client_window_pose, set_client_visibility, get_client_window_pose |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Window WndProc, WM_SETCURSOR, input forwarding, scroll accum, set_cursor |
| `src/xrt/compositor/d3d11/comp_d3d11_window.h` | Window public API |
| `src/xrt/ipc/server/ipc_server_handler.c` | IPC handlers: shell_activate, shell_set_window_pose, shell_set_visibility, shell_get_window_pose |
| `src/xrt/ipc/shared/proto.json` | IPC protocol definitions |
| `src/xrt/targets/shell/main.c` | Shell app: launcher, monitor, pose assignment, persistence (cJSON) |
| `src/xrt/auxiliary/math/m_display3d_view.h` | Kooima projection: `display3d_compute_fov()`, `display3d_compute_projection()` |

## Test Procedure

```bash
# Build
scripts\build_windows.bat build
scripts\build_windows.bat test-apps

# Run (single command)
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe

# With Z-depth pose (Phase 3A)
_package\bin\displayxr-shell.exe --pose -0.1,0.05,0.03,0.14,0.08 app1.exe --pose 0.1,0.05,-0.02,0.14,0.08 app2.exe
```

**Expected (Phase 3A):** First app hovers 3cm in front of display. Second app recedes 2cm behind. Both show correct 3D parallax on Leia display.
