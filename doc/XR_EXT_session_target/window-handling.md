# Window Handling: Drag, Resize, and Adaptive Rendering

## Overview

When the OpenXR runtime renders into an **app-provided window** (via `XR_EXT_session_target`), three problems arise that do not exist in fullscreen mode:

| Problem | Symptom | Solution |
|---------|---------|----------|
| **Window drag freezes rendering** | App hangs while user drags title bar | Single-threaded WM_PAINT trick |
| **FOV is wrong for small windows** | Image appears stretched/distorted | Window-adaptive Kooima FOV |
| **GPU wastes work on small windows** | Full SR-resolution stereo texture for a 400px window | Proportional render texture resize |
| **Window drag breaks 3D phase alignment** | Crosstalk jittering when window lands on non-aligned pixel position | Weaver auto-snaps window to phase-aligned positions |

This document describes how these are solved in the runtime, how native SR SDK apps handle the same issues, and recommendations for developers.

---

## Problem 1: Window Drag Freezes Rendering

### The Win32 Modal Loop Problem

When a user clicks the title bar to drag or resize a window, Windows enters a **modal message loop** inside `DefWindowProc`. This blocks the thread's normal message pump — and if rendering runs on the same thread, it freezes too:

```
Normal frame loop (single-threaded):
┌──────────────────────────────────────────────────────────┐
│  while (running) {                                        │
│      PeekMessage → DispatchMessage   // message pump      │
│      Render();                       // frame rendering   │
│  }                                                        │
└──────────────────────────────────────────────────────────┘

During window drag:
┌──────────────────────────────────────────────────────────┐
│  DefWindowProc(WM_NCLBUTTONDOWN)                          │
│      ├── enters modal loop                                │
│      ├── pumps WM_MOUSEMOVE, WM_PAINT internally          │
│      ├── BLOCKS until mouse button released               │
│      └── your Render() never runs ← FROZEN                │
└──────────────────────────────────────────────────────────┘
```

### How Native SR Apps Handle It: The WM_PAINT Trick

The SR SDK `parallax_toggle` example (from `LeiaSR-SDK-1.34.8-RC1/examples/parallax_toggle`) uses a single-threaded approach with a clever workaround:

```cpp
// From parallax_toggle/main.cpp

static bool isMoving = false;

case WM_ENTERSIZEMOVE:
    isMoving = true;
    InvalidateRect(hWnd, nullptr, false);  // Force WM_PAINT
    break;

case WM_EXITSIZEMOVE:
    isMoving = false;
    break;

case WM_PAINT:
    if (isMoving) {
        // Render INSIDE the modal drag loop via WM_PAINT
        Render();
        // Don't call BeginPaint/EndPaint → window stays invalidated
        // → Windows keeps sending WM_PAINT → continuous rendering
    } else {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
    }
    break;
```

**How it works:** When `WM_ENTERSIZEMOVE` fires, the app invalidates the window. Windows's modal drag loop dispatches `WM_PAINT` for invalidated windows. By calling `Render()` inside `WM_PAINT` without validating (no `BeginPaint`/`EndPaint`), the window stays invalidated and Windows keeps sending `WM_PAINT` — creating a render loop inside the modal loop.

**Limitations:**
- Couples rendering to the Windows message loop
- Frame rate depends on how often Windows dispatches `WM_PAINT` during drag
- Fragile — any path that validates the window (even by accident) breaks the loop
- The app must implement the trick — the runtime cannot inject it into the app's `WndProc`

### Our Solution: Single-Threaded WM_PAINT Approach

We initially implemented a **dedicated render thread** to decouple rendering from the message pump. However, the D3D11 immediate device context is **not thread-safe** — both the render thread (calling `weave()`) and the main thread (via `DefWindowProc` → DXGI housekeeping) touch the same context concurrently. This caused crashes on mouse-up events and required increasingly complex synchronization (recursive mutexes, WndProc wrappers) that still couldn't guarantee safety. See `mouse-race-condition.md` in git history for the full analysis.

The runtime now uses the **same WM_PAINT trick** as the SR SDK's native examples. The test app (`sr_cube_openxr_ext`) renders inside `WM_PAINT` during drag/resize, keeping frames flowing without requiring a separate thread:

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Single-Threaded App                                 │
│                                                                     │
│   while (running) {                                                  │
│       PeekMessage → DispatchMessage  // handles all messages         │
│       xrWaitFrame / xrBeginFrame / xrEndFrame  // normal frames     │
│   }                                                                  │
│                                                                     │
│   WndProc:                                                           │
│     WM_ENTERSIZEMOVE → set isMoving, InvalidateRect                  │
│     WM_EXITSIZEMOVE  → clear isMoving                                │
│     WM_PAINT (if isMoving) → run one OpenXR frame, don't validate   │
│              (Windows keeps sending WM_PAINT → continuous rendering) │
└─────────────────────────────────────────────────────────────────────┘
```

**Why single-threaded WM_PAINT is the right choice:**

1. **D3D11 thread safety.** The D3D11 immediate context cannot be safely shared between threads without `ID3D11Multithread::SetMultithreadProtected(TRUE)`, and even then the SR SDK's internal weaver state has no synchronization. A single thread eliminates all data races by design.

2. **The app owns the message pump.** With `XR_EXT_session_target`, the app creates the window and controls the message loop. The app is the natural place to implement the WM_PAINT trick — it already owns the `WndProc` and the render call.

3. **Simplicity.** No mutexes, no WndProc wrapper chains, no global instance pointers. The entire solution is ~15 lines of code in the app's `WndProc`.

**Limitations:**

- Frame rate during drag depends on how often Windows dispatches `WM_PAINT` (typically tied to the display refresh rate, which is adequate for smooth 3D).
- Any path that validates the window (e.g., `BeginPaint`/`EndPaint`) would break the loop. Apps must be careful not to validate during `isMoving`.
- The app must implement the WM_PAINT trick itself — the runtime cannot do it on the app's behalf.

---

## Problem 2: FOV Is Wrong for Small Windows

### The Kooima Projection Problem

The runtime uses the **Kooima generalized perspective projection** to compute asymmetric FOV based on the user's eye position relative to the physical display. The formula uses the display's physical dimensions (in meters) as the screen size:

```
                   Display (full physical size)
            ┌──────────────────────────────────────┐
            │                                      │
            │        Window (smaller)              │
            │        ┌──────────────┐              │
            │        │              │              │
            │        │   Rendered   │              │
            │        │   content    │              │
            │        │              │              │
            │        └──────────────┘              │
            │                                      │
            └──────────────────────────────────────┘

    If Kooima uses full display size for a small window:
    → FOV is too wide → image appears stretched horizontally
```

### SRHydra Reference Implementation

SRHydra (from `Session.cpp:862-1044`) solves this with a **viewport scale** formula that maps the window's physical size to an effective screen size:

```
PixelSize     = DisplayPhysicalSize / DisplayPixelResolution
WindowPhysSize = WindowPixelSize × PixelSize
viewportScale = min(DisplayPhysSize) / min(WindowPhysSize)
ScreenSize    = WindowPhysSize × viewportScale
EyeCenter    -= (WindowCenter - DisplayCenter) × PixelSize
```

### Our Implementation

**File:** `src/xrt/state_trackers/oxr/oxr_session.c`

The FOV computation in `oxr_session_locate_views()` queries live window metrics each frame and applies the SRHydra viewport scale formula:

```c
struct leiasr_window_metrics wm = {0};
bool have_wm = oxr_session_get_window_metrics(sess, &wm);

struct leiasr_eye_position adj_left = eye_pair.left;
struct leiasr_eye_position adj_right = eye_pair.right;

if (have_wm && wm.valid && wm.window_width_m > 0 && wm.window_height_m > 0) {
    // SRHydra viewport scale formula
    float min_disp = fminf(wm.display_width_m, wm.display_height_m);
    float min_win  = fminf(wm.window_width_m, wm.window_height_m);
    float vs = min_disp / min_win;

    screen_width_m  = wm.window_width_m * vs;
    screen_height_m = wm.window_height_m * vs;

    // Shift eye positions for window center offset
    adj_left.x  -= wm.window_center_offset_x_m;
    adj_left.y  -= wm.window_center_offset_y_m;
    adj_right.x -= wm.window_center_offset_x_m;
    adj_right.y -= wm.window_center_offset_y_m;
}

compute_kooima_fov(&adj_left, screen_width_m, screen_height_m, ...);
compute_kooima_fov(&adj_right, screen_width_m, screen_height_m, ...);
```

### Eye Position Offset for Off-Center Windows

When the window is not centered on the display, the eye positions must be shifted so that parallax is correct for the visible portion:

```
    Display physical center
            │
            ▼
    ┌───────────────────────────────────────┐
    │       │                               │
    │       │    Window center              │
    │       │        │                      │
    │       │  ┌─────┼──────┐              │
    │       │  │     │      │              │
    │       │  │     ▼      │              │
    │       │  │  (offset)  │              │
    │       │  └────────────┘              │
    │       │                               │
    └───────────────────────────────────────┘

    offset_x = (window_center_px - display_center_px) × pixel_size_x
    offset_y = -((window_center_py - display_center_py) × pixel_size_y)
               ↑ negated: screen coords Y-down, eye coords Y-up

    adjusted_eye.x -= offset_x
    adjusted_eye.y -= offset_y
```

### Window Metrics Computation

**File:** `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp`

The `comp_d3d11_compositor_get_window_metrics()` function computes all derived values from display and window geometry:

```cpp
// Get display pixel info from SR SDK (cached at init)
leiasr_d3d11_get_display_pixel_info(weaver,
    &disp_px_w, &disp_px_h,         // display resolution
    &disp_left, &disp_top,           // display screen position
    &disp_w_m, &disp_h_m);          // display physical size (meters)

// Get window client area (live per frame)
GetClientRect(hwnd, &rect);
ClientToScreen(hwnd, &client_origin);

// Compute pixel size (meters per pixel)
float pixel_size_x = disp_w_m / (float)disp_px_w;
float pixel_size_y = disp_h_m / (float)disp_px_h;

// Window physical size
float win_w_m = (float)win_px_w * pixel_size_x;
float win_h_m = (float)win_px_h * pixel_size_y;

// Window center offset from display center (meters)
// X: +right, Y: negated (screen Y-down → eye Y-up)
float offset_x = (win_center_px_x - disp_center_px_x) * pixel_size_x;
float offset_y = -((win_center_px_y - disp_center_px_y) * pixel_size_y);
```

### Fullscreen Regression Safety

When the window fills the display, all adjustments become identity:

| Metric | Fullscreen Value | Effect |
|--------|-----------------|--------|
| `viewportScale` | `1.0` (window == display) | Screen dims unchanged |
| `window_center_offset` | `(0, 0)` | Eye positions unchanged |
| `screen_width/height_m` | `display_width/height_m` | FOV identical to before |

---

## Problem 3: GPU Wastes Work on Small Windows

### The Problem

The SR SDK recommends a specific render texture size (`getRecommendedViewsTextureWidth/Height`) optimized for the full display. Native SR apps like `parallax_toggle` always render at this fixed size regardless of window dimensions. The weaver can handle the mismatch, but this wastes GPU for small windows.

### Our Solution: Proportional Render Texture Resize

**File:** `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp` (in `begin_frame`)

When the window size changes, the renderer's stereo texture is scaled proportionally:

```cpp
// In d3d11_compositor_begin_frame(), after target resize:

// Get SR recommended full-display view dimensions
leiasr_d3d11_get_recommended_view_dimensions(weaver, &sr_w, &sr_h);
leiasr_d3d11_get_display_pixel_info(weaver, &disp_px_w, &disp_px_h, ...);

// Scale by window/display ratio (capped at 1.0)
float ratio = fminf((float)window_width / (float)disp_px_w,
                    (float)window_height / (float)disp_px_h);
if (ratio > 1.0f) ratio = 1.0f;

uint32_t new_view_w = (uint32_t)(sr_w * ratio);
uint32_t new_view_h = (uint32_t)(sr_h * ratio);

comp_d3d11_renderer_resize(renderer, new_view_w, new_view_h);
```

**File:** `src/xrt/compositor/d3d11/comp_d3d11_renderer.cpp`

The resize function recreates only the size-dependent resources:

```cpp
xrt_result_t
comp_d3d11_renderer_resize(struct comp_d3d11_renderer *renderer,
                           uint32_t new_view_width,
                           uint32_t new_view_height)
{
    // Clamp minimum to 64×64
    // Skip if dimensions unchanged
    // Release: stereo_texture, stereo_srv, stereo_rtv, depth_texture, depth_dsv
    // Recreate at new dimensions (2×width for side-by-side stereo)
    // Shaders, samplers, blend/rasterizer state are NOT recreated
}
```

### Scaling Behavior

| Window Size | Ratio | Render Texture | GPU Savings |
|-------------|-------|---------------|-------------|
| Full display (2560×1600) | 1.0 | Full SR recommended | 0% (baseline) |
| Half display (1280×800) | 0.5 | 50% SR recommended | ~75% pixels |
| Quarter display (640×400) | 0.25 | 25% SR recommended | ~94% pixels |
| Minimum (64×64) | Clamped | 64×64 per eye | Maximum |

---

## Problem 4: Window Drag Breaks 3D Phase Alignment

### The Lenticular Phase Problem

On a lenticular autostereoscopic display, each subpixel projects light at a specific angle determined by its position relative to the lens array. The weaver computes a **phase** for each pixel — essentially, which view (left eye, right eye, or an intermediate angle) that pixel should show. This phase depends on the pixel's absolute position on the display panel.

When a windowed application is dragged to an arbitrary pixel position, the relationship between the rendered content and the lens array changes. If the new position doesn't preserve the original phase alignment, the viewer sees **crosstalk jittering** — left-eye and right-eye images bleed into each other, destroying the 3D effect:

```
Before drag (phase-aligned):          After drag (misaligned):
┌──────────────┐                      ┌──────────────┐
│ L R L R L R  │  ← clean stereo     │ ? ? ? ? ? ?  │  ← crosstalk
│ L R L R L R  │                      │ ? ? ? ? ? ?  │
└──────────────┘                      └──────────────┘
Subpixels map correctly               Phase shifted — left/right
to left/right eyes                    bleed into wrong eyes
```

### The SR Weaver's Built-In Solution

**This is handled automatically by the SR weaver — all applications using the weaver get this for free.**

When the weaver initializes, it **subclasses the application's window procedure** using the Win32 `SetWindowLongPtr` API. This replaces the window's `WndProc` with a custom dispatcher that intercepts window-movement messages before they reach the application or `DefWindowProc`. The original `WndProc` is preserved and called for all non-movement messages, so the application's own message handling is unaffected.

The subclassed `WndProc` intercepts three messages:

| Message | Purpose |
|---------|---------|
| `WM_ENTERSIZEMOVE` | Records the window's initial position when the user begins dragging |
| `WM_WINDOWPOSCHANGING` | Snaps the proposed new position to the nearest phase-aligned coordinate |
| `WM_EXITSIZEMOVE` | Clears the drag state when the user releases |

During a drag, every `WM_WINDOWPOSCHANGING` message carries the proposed new window position. The weaver replaces this position with the nearest pixel coordinate that preserves the same lenticular phase as the original position. The search considers both horizontal and vertical displacement (since lenticular lenses are slanted), and picks the candidate closest to where the user intended to move while maintaining correct phase alignment.

The effect is that the window moves in small discrete steps rather than pixel-by-pixel, but these steps are typically 1-2 pixels apart, so the motion feels smooth to the user while the 3D image remains stable throughout the drag.

### Implications for the OpenXR Runtime

Since the SR weaver performs this subclassing internally during initialization, the OpenXR runtime's compositor — which creates and owns the weaver instance — automatically benefits from phase-aligned window snapping. No additional code is needed in the runtime or the application.

This is complementary to the other window-handling solutions in this document:
- **Problem 1** (WM_PAINT trick) keeps frames flowing during drag
- **Problem 2** (viewport scale) keeps the FOV correct for the window size
- **Problem 3** (texture resize) keeps GPU usage proportional
- **Problem 4** (phase snapping) keeps the 3D stereo alignment correct at every position

---

## Comparison: Native SR App vs Runtime

| Aspect | `parallax_toggle` (native) | OpenXR Runtime |
|--------|---------------------------|----------------|
| **Threading** | Single thread, WM_PAINT trick | Single thread, WM_PAINT trick |
| **Drag handling** | Renders inside WM_PAINT during drag | Renders inside WM_PAINT during drag |
| **FOV adjustment** | None — uses full display size always | Viewport scale + eye offset |
| **Render texture** | Fixed at SR recommended size | Scaled proportionally to window |
| **Off-center correction** | None | Eye position offset applied |
| **Window resize** | Resizes swapchain only | Resizes swapchain + render texture |
| **Phase-aligned drag** | Automatic (weaver subclasses WndProc) | Automatic (weaver subclasses WndProc) |
| **Min window size** | 100×100 pixels | 64×64 per eye (render), no limit (window) |

---

## Developer Recommendations

### For OpenXR App Developers Using XR_EXT_session_target

**1. Let the runtime handle resize — don't fight it.**

The runtime handles all FOV, eye position, and render texture adjustments automatically. Your app just submits layers as normal via `xrEndFrame`. The swapchain dimensions reported by `xrEnumerateSwapchainImages` are the app-side textures and do not change with the window.

**2. Do NOT use a separate render thread with D3D11.**

The D3D11 immediate device context is not thread-safe. If your app runs the OpenXR frame loop on a separate thread while the main thread handles window messages, `DefWindowProc` and DXGI housekeeping on the main thread will race with `weave()` and `Present()` on the render thread. This causes crashes — particularly on mouse-up events where `ReleaseCapture()` triggers synchronous `WM_CAPTURECHANGED` messages that re-enter the D3D11 context. Even with `ID3D11Multithread::SetMultithreadProtected(TRUE)`, the SR SDK's internal weaver state is not synchronized. Keep everything on one thread.

**3. Use the WM_PAINT trick to keep rendering during window drag (recommended).**

The WM_PAINT trick is the proven approach used by the SR SDK's own examples and by the OpenXR runtime:

```cpp
static bool g_isMoving = false;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ENTERSIZEMOVE:
        g_isMoving = true;
        InvalidateRect(hWnd, nullptr, false);
        return 0;

    case WM_EXITSIZEMOVE:
        g_isMoving = false;
        return 0;

    case WM_PAINT:
        if (g_isMoving) {
            // Continue OpenXR frame loop during drag
            run_one_openxr_frame();
            // Don't validate — keeps WM_PAINT firing
            return 0;
        }
        break;  // Fall through to DefWindowProc

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = 100;
        mmi->ptMinTrackSize.y = 100;
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
```

**4. Handle `WM_SIZE` for your own state, but don't resize swapchains.**

The runtime detects window size changes in `begin_frame` and handles all internal resizing. Your app should only track the window size for its own layout/UI purposes. OpenXR swapchains are runtime-managed.

### For Runtime Developers

**1. Query window metrics per frame, not per resize event.**

Window position changes continuously during drag without triggering `WM_SIZE`. Use `GetClientRect` + `ClientToScreen` each frame in the FOV computation path.

**2. Cache display-side values at init, query window-side values live.**

Display pixel resolution and physical size don't change at runtime. Cache them once from the SR SDK. Window dimensions and position change every frame during drag — always query fresh.

**3. Use the viewport scale formula, not raw ratios.**

The SRHydra viewport scale (`min(display) / min(window)`) preserves aspect ratio correctly. A naive `window/display` ratio would distort non-square windows.

**4. Negate Y when converting screen coords to eye coords.**

Screen coordinates have Y-down (top-left origin). Eye coordinates have Y-up (display center origin). The window center offset Y component must be negated.

---

## Data Flow Summary

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Per-Frame Pipeline                                │
│                                                                     │
│  ┌──────────────┐    ┌───────────────┐    ┌──────────────────────┐  │
│  │ begin_frame  │───►│ Detect window │───►│ Resize renderer      │  │
│  │              │    │ size change   │    │ stereo texture       │  │
│  └──────────────┘    └───────────────┘    └──────────────────────┘  │
│                                                                     │
│  ┌──────────────┐    ┌───────────────┐    ┌──────────────────────┐  │
│  │LocateViews  │───►│ Get window    │───►│ Compute viewport     │  │
│  │              │    │ metrics       │    │ scale + eye offset   │  │
│  │              │    │ (live HWND    │    │                      │  │
│  │              │    │  query)       │    │ Feed adjusted values │  │
│  │              │    │               │    │ to Kooima FOV        │  │
│  └──────────────┘    └───────────────┘    └──────────────────────┘  │
│                                                                     │
│  ┌──────────────┐    ┌───────────────┐    ┌──────────────────────┐  │
│  │ layer_commit │───►│ Render to     │───►│ Weave + present      │  │
│  │              │    │ (resized)     │    │ to window            │  │
│  │              │    │ stereo texture│    │                      │  │
│  └──────────────┘    └───────────────┘    └──────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Files Modified

| File | Change |
|------|--------|
| `src/xrt/drivers/leiasr/leiasr_types.h` | `struct leiasr_window_metrics` |
| `src/xrt/drivers/leiasr/leiasr_d3d11.h` | `leiasr_d3d11_get_display_pixel_info()` declaration |
| `src/xrt/drivers/leiasr/leiasr_d3d11.cpp` | Display pixel info storage + getter |
| `src/xrt/compositor/d3d11/comp_d3d11_compositor.h` | `comp_d3d11_compositor_get_window_metrics()` declaration |
| `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp` | Window metrics computation + renderer resize in `begin_frame` |
| `src/xrt/compositor/d3d11/comp_d3d11_renderer.h` | `comp_d3d11_renderer_resize()` declaration |
| `src/xrt/compositor/d3d11/comp_d3d11_renderer.cpp` | Stereo texture resize implementation |
| `src/xrt/state_trackers/oxr/oxr_session.c` | Window-adaptive Kooima FOV + eye offset |

## References

- [app-control.md](app-control.md) — XR_EXT_session_target extension design
- [phase-5.md](phase-5.md) — SR weaver eye tracking integration
- [hybrid-mode-architecture.md](hybrid-mode-architecture.md) — D3D11 native compositor architecture
- SR SDK `parallax_toggle` example — Native app window handling reference
- SRHydra `Session.cpp:862-1044` — Viewport scale formula reference
