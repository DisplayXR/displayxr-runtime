# EXT Test Apps

Five test applications that demonstrate the **external window binding** and **display info** OpenXR extensions. Each renders a spinning cube via OpenXR into an app-owned window, exercising the same core pipeline across different graphics APIs and platforms.

| App | Graphics API | Platform | Directory |
|-----|-------------|----------|-----------|
| `sr_cube_openxr_ext` | D3D11 | Windows | `sr_cube_openxr_ext/` |
| `sr_cube_openxr_ext_vk` | Vulkan | Windows | `sr_cube_openxr_ext_vk/` |
| `sr_cube_openxr_ext_gl` | OpenGL | Windows | `sr_cube_openxr_ext_gl/` |
| `sr_cube_openxr_ext_d3d12` | D3D12 | Windows | `sr_cube_openxr_ext_d3d12/` |
| `sim_cube_openxr_ext_macos` | Vulkan | macOS | `sim_cube_openxr_ext_macos/` |

## OpenXR Extensions Demonstrated

### XR_EXT_win32_window_binding / XR_EXT_macos_window_binding

The app creates its own window and passes the handle to the runtime at session creation. On Windows, an `XrWin32WindowBindingCreateInfoEXT` struct carrying the HWND is chained into `XrSessionCreateInfo.next`. On macOS, `XrMacOSWindowBindingCreateInfoEXT` carries the NSView (a CAMetalLayer-backed view). The runtime renders into the app's window instead of creating its own, enabling windowed mode, multi-app scenarios, and app-controlled input.

### XR_EXT_display_info

Queries physical display properties by chaining `XrDisplayInfoEXT` into `xrGetSystemProperties`:

- **Display size** (meters) -- used for Kooima projection
- **Nominal viewer position** -- default eye position in display space
- **Recommended view scale** (X, Y) -- per-eye viewport fraction of window size
- **Native display pixel dimensions** -- panel resolution in pixels, used for swapchain sizing
- **Display mode switch support** -- whether the runtime can toggle 2D/3D

### xrRequestDisplayModeEXT

Runtime-side 2D/3D mode switching. The V key calls `xrRequestDisplayModeEXT(session, XR_DISPLAY_MODE_3D_EXT | XR_DISPLAY_MODE_2D_EXT)` to toggle the display processor between stereo interlacing and mono passthrough.

### DISPLAY Reference Space

All apps create `XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT` for view location and layer submission. DISPLAY space is physically anchored to the display center and unaffected by `xrRecenterSpace` or LOCAL recentering. Falls back to LOCAL space if the extension is unavailable.

## Controls

| Action | Windows (all 4 apps) | macOS |
|--------|---------------------|-------|
| Move forward/back | W / S | W / S |
| Strafe left/right | A / D | A / D |
| Move up/down | E / Q | E / Q |
| Look around | Mouse drag (LMB) | Mouse drag (LMB) |
| Zoom | Scroll wheel | Scroll wheel |
| Reset view | Space or double-click | Space or double-click |
| 2D/3D toggle | V | V |
| HUD toggle | Tab | Tab |
| Fullscreen | F11 | Cmd+Ctrl+F |
| sim_display modes | -- | 1 / 2 / 3 |
| Quit | ESC | ESC |

- **Movement speed:** 0.1 m/s (fly mode, direction-relative)
- **Mouse sensitivity:** 0.005 rad/pixel
- **Pitch clamp:** +/-1.4 rad (~80 deg)
- **Zoom range:** 0.1x -- 10.0x (1.1x per scroll step)
- **Reset:** zeroes position, yaw, pitch, and zoom

## HUD Overlay

Toggled with Tab. Updated every 0.5s (macOS) or every frame (Windows).

**Windows** (DirectWrite/D2D via shared `common/hud_renderer.cpp`):

```
Session: FOCUSED
XR_EXT_win32_window_binding: ACTIVE (D3D11)
Display Mode: 3D Stereo [V=Toggle]
FPS: 60.0
Frame: 16.7ms
Render: 640x720
Window: 1280x720
Display: (344x194) mm
Nominal: (0,0,500) mm
L eye: (-32,0,500) mm
R eye: (32,0,500) mm
```

**macOS** (Core Text via NSView overlay):

```
FPS: 60  (16.7 ms)
Mode: 3D (Stereo)
Render: 640x720
Window: 1280x720
Display: 0.344 x 0.194 m
Scale: 0.50 x 1.00
Nominal: (0.000, 0.000, 0.500)
Eye L: (-0.032, 0.000, 0.500)
Eye R: (0.032, 0.000, 0.500)
Camera: (0.00, 0.00, 0.00)
Yaw: 0.0 deg  Pitch: 0.0 deg  Zoom: 1.00x
Track: Active

WASD/QE=Move  Drag=Look  Scroll=Zoom
Space=Reset  V=2D/3D  Tab=HUD  ESC=Quit
```

**Key fields:**
- **FPS + frame time** -- render performance
- **Display mode** -- "3D (Stereo)" or "2D (Mono)", plus "[no switch]" if `xrRequestDisplayModeEXT` is unsupported
- **Render vs window resolution** -- per-eye render size vs window size
- **Display size** -- physical display dimensions from `XR_EXT_display_info`
- **Recommended view scale** -- X, Y scale factors (macOS HUD only)
- **Nominal viewer position** -- default viewer distance from display center
- **Eye positions** -- raw display-space L/R eye positions (pre-player-transform, in mm on Windows or meters on macOS)
- **Camera offset + yaw/pitch/zoom** -- player transform state (macOS HUD only)
- **Eye tracking status** -- Active or None

## Camera & Projection Pipeline

All five apps share the same six-step pipeline. The Windows apps use a shared `xr_session_common.cpp` implementation; the macOS app reimplements the same logic with platform-native math.

```
xrLocateViews (DISPLAY space)
        |
        v
   rawEyePos[] ---- saved for Kooima + HUD
        |
        v
  Player Transform (view matrix)
   1. Scale eye pos by 1/zoomScale
   2. Rotate by player quaternion (yaw, pitch)
   3. Translate by camera offset (WASD)
        |
        v
  Mono Mode? ---- average raw + transformed positions
        |
        v
  Kooima Projection (from raw positions)
   kooimaEye = rawPos / zoomScale
   screenSize = displaySize / zoomScale
   (ratios cancel -- zoom doesn't change frustum shape)
        |
        v
  Render & Submit
   viewMatrix from player-transformed pose
   projMatrix from Kooima
   projectionViews[eye].fov from Kooima-computed FOV
```

### Step 1: xrLocateViews in DISPLAY space

```c
locateInfo.space = (xr.displaySpace != XR_NULL_HANDLE) ? xr.displaySpace : xr.localSpace;
xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views);
```

Raw eye positions from the eye tracker, anchored to the physical display center. Falls back to LOCAL space if DISPLAY is unavailable.

### Step 2: Save raw positions

```c
XrVector3f rawEyePos[2] = {views[0].pose.position, views[1].pose.position};
```

Stored **before** any player transform. These feed Kooima projection and HUD display.

### Step 3: Player transform (view matrix)

```c
// Scale eye position toward display center (zoom = baseline scaling)
localPos = localPos / zoomScale;
// Rotate by player quaternion
worldPos = rotate(playerOri, localPos) + playerPosition;
// Orientation: worldOri = playerOri * localOri
worldOri = multiply(playerOri, localOri);
```

This is the production-engine locomotion pattern: the reference space stays fixed at the tracking origin and a player transform is applied to every OpenXR pose.

### Step 4: Mono mode center-eye

In 2D mode, both raw and transformed positions are averaged into a single center eye. The app submits 1 projection layer instead of 2.

### Step 5: Kooima asymmetric projection

Uses **raw** display-space eye position, NOT the player-transformed position:

```c
kooimaEye = {rawPos.x / zoomScale, rawPos.y / zoomScale, rawPos.z / zoomScale};
screenW = displayWidthM * viewScaleX / zoomScale;  // stereo
screenH = displayHeightM * viewScaleY / zoomScale;
```

The `1/zoomScale` in both eye position and screen size cancels in the projection ratio, so zoom only affects the view matrix baseline (stereo parallax), not frustum shape. When display info is unavailable, falls back to runtime-provided symmetric FOV.

Reference: Robert Kooima, "Generalized Perspective Projection" (2009).

### Step 6: Render & submit

View matrix from player-transformed pose, projection matrix from Kooima. The computed FOV is submitted via `projectionViews[eye].fov` so the compositor can perform correct reprojection.

## 2D/3D Display Mode Toggle

- **V key** toggles `displayMode3D` flag
- Calls `xrRequestDisplayModeEXT(session, XR_DISPLAY_MODE_3D_EXT)` or `XR_DISPLAY_MODE_2D_EXT`
- Runtime switches the display processor between stereo interlacing and mono passthrough
- App renders 1 eye (center, averaged) in 2D, 2 eyes in 3D
- Submit count: `viewCount = displayMode3D ? 2 : 1`
- HUD shows "[no switch]" if runtime doesn't support `xrRequestDisplayModeEXT`

## sim_display Visualization Modes

Keys **1/2/3** switch between sim_display output modes for development without a real 3D display:

| Key | Mode | Description |
|-----|------|-------------|
| 1 | SBS | Side-by-side stereo |
| 2 | Anaglyph | Red-cyan anaglyph (use red-cyan glasses) |
| 3 | Blend | Alpha-blend 50/50 overlay of L/R eyes |

**macOS:** Uses `dlsym` to find `sim_display_set_output_mode()` in the loaded runtime via `_dyld` APIs. Calls the function directly for **live hot-reload** without restarting. Falls back to `setenv("SIM_DISPLAY_OUTPUT", ...)` if the symbol isn't found (restart required).

**Windows:** The Windows ext apps do not currently include 1/2/3 key bindings. Use the `SIM_DISPLAY_OUTPUT` environment variable before launching.

These modes are specific to the sim_display driver and are **not available** on Leia hardware, which uses its own light field weaver.

## Fullscreen

**Windows (F11):** Saves window style and position, then sets `WS_POPUP | WS_VISIBLE` spanning the nearest monitor. F11 again restores the saved windowed state.

**macOS (Cmd+Ctrl+F):** Saves window style and frame, then sets `NSWindowStyleMaskBorderless` spanning the screen with `NSStatusWindowLevel` (above menu bar). No macOS Space animation. Cmd+Ctrl+F again restores windowed mode.

## Platform Differences

| Aspect | Windows (4 apps) | macOS |
|--------|-----------------|-------|
| Window API | Win32 HWND | NSWindow + CAMetalLayer NSView |
| Graphics binding | D3D11 / Vulkan / OpenGL / D3D12 | Vulkan |
| Window extension | `XR_EXT_win32_window_binding` | `XR_EXT_macos_window_binding` |
| Fullscreen shortcut | F11 | Cmd+Ctrl+F |
| Fullscreen style | `WS_POPUP` borderless | `NSWindowStyleMaskBorderless` |
| HUD rendering | DirectWrite/D2D (shared `hud_renderer.cpp`) | Core Text NSView overlay (Menlo 11pt) |
| HUD units | Millimeters (display, eye pos) | Meters |
| sim_display switching | Environment variable only | Live hot-reload via `dlsym` + env fallback |
| Shared code | `common/` library (input, session, HUD, renderer) | Self-contained `main.mm` |
| Mouse yaw direction | `yaw -= dx * 0.005` | `yaw += dx * 0.005` (NSView Y-up) |

## Building & Running

### Windows

Built via GitHub Actions CI (`build-windows.yml`) or local CMake + SR SDK:

```bash
mkdir build && cd build
cmake .. -G Ninja
cmake --build .
```

Requires `LEIASR_SDKROOT` environment variable for SR SDK support. All four Windows ext apps are built as part of the main project.

### macOS

```bash
./scripts/build_macos.sh
```

Run the test app:

```bash
./_package/SRMonado-macOS/run_sim_cube_ext.sh
```

### Environment Variables

| Variable | Values | Description |
|----------|--------|-------------|
| `SIM_DISPLAY_ENABLE` | `1` | Enable the sim_display driver |
| `SIM_DISPLAY_OUTPUT` | `sbs`, `anaglyph`, `blend` | Set visualization mode (before launch) |
