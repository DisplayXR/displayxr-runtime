# Leia SR DirectX Weaving -- DX11 & DX12

This document describes the technical operation of the DX11 and DX12 weavers in the Leia SR SDK. Both weavers share the same underlying optical algorithm (via `Dimenco::Weaver`) but differ in how they interact with the graphics API.

## Overview

The weaver takes a **side-by-side (SBS) stereo texture** (left eye on the left half, right eye on the right half) and a **tracked eye position**, and produces a **weaved image** on the currently bound render target. The weaved image encodes per-sub-pixel view information so that the lenticular or switchable lens on the SR display directs the correct view to each eye.

---

## Creation

### Modern API (recommended)

| | DX11 | DX12 |
|---|---|---|
| Factory | `CreateDX11Weaver(SRContext*, ID3D11DeviceContext*, HWND, IDX11Weaver1**)` | `CreateDX12Weaver(SRContext*, ID3D12Device*, HWND, IDX12Weaver1**)` |
| HWND | Optional at creation; can be set later via `setWindowHandle()` | Optional at creation; can be set later via `setWindowHandle()` |
| Returns | `WeaverErrorCode` | `WeaverErrorCode` |

Both interfaces inherit from `IWeaverBase1` (common weaving API) and `IWeaverSettings1` (quality tuning).

### Deprecated API

Older classes (`DX11Weaver`, `PredictingDX11Weaver`, `DX12Weaver`, `PredictingDX12Weaver`) are still available but deprecated. The deprecated DX12 constructors additionally required `ID3D12CommandAllocator*` and `ID3D12CommandQueue*` for setup, plus explicit input/output framebuffer pointers.

---

## Inputs

### Input Texture

Both weavers expect a **single SBS stereo texture** -- left view on the left half, right view on the right half. There are no separate left/right texture inputs and no depth buffer input.

**DX11:**
```cpp
void setInputViewTexture(ID3D11ShaderResourceView* texture, int width, int height, DXGI_FORMAT format);
```
- `width` is the **full SBS width** (each eye occupies `width/2` pixels).
- The weaver calls `AddRef()` on the SRV and holds it until replaced.

**DX12:**
```cpp
void setInputViewTexture(ID3D12Resource* texture, int width, int height, DXGI_FORMAT format);
```
- Same semantics. The weaver creates an internal SRV at descriptor heap slot 0.
- The resource **must be in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`** when `weave()` is called.
- Typeless formats are supported -- pass the fully qualified format via the `format` parameter.

### Additional Shader Resources (internal)

The weaver binds three additional textures automatically:
- **CorrectionA** (slot 1) -- optical correction lookup texture
- **CorrectionB** (slot 2) -- optical correction lookup texture
- **Pixel stamp** (slot 3) -- sub-pixel layout offsets (`R16G16B16A16_FLOAT`)

These are loaded from calibration data during initialization and updated when the display configuration changes.

### Eye Position

The weaver internally queries the eye tracker via `PredictingWeaverTracker::predict()`, which returns a predicted eye midpoint position (`FLOAT3`, in centimeters) compensated for the configured latency. The application does not need to pass eye positions to `weave()`.

The application can retrieve the predicted eye positions for its own stereo camera setup:
```cpp
void getPredictedEyePositions(float left[3], float right[3]);
```

---

## State Required Before `weave()`

### DX11

- **Render target**: Bind the output RT (e.g., backbuffer) via `OMSetRenderTargets` before calling `weave()`.
- **Viewport / scissor**: Set on the device context. The weaver reads them automatically.
- **Device context**: Passed at creation or via `setContext()`.

The DX11 weaver **saves and restores all pipeline state** around the weave call (shaders, samplers, rasterizer state, depth-stencil, vertex buffers, input layout, constant buffers, scissor rects). It is transparent to the application's rendering pipeline.

### DX12

Before calling `weave()`, the application must explicitly set:

```cpp
void setCommandList(ID3D12GraphicsCommandList* commandList);  // required
void setViewport(D3D12_VIEWPORT viewport);                     // required
void setScissorRect(D3D12_RECT scissorRect);                   // required
void setOutputFormat(DXGI_FORMAT format);                       // must match bound RT
```

The render target must already be bound on the command list and in `D3D12_RESOURCE_STATE_RENDER_TARGET`. The weaver **records draw commands** into the provided command list -- it does not execute them. The application is responsible for closing the list, executing it, and managing GPU synchronization.

The weaver throws an exception if `commandList`, `viewport`, or `scissorRect` are not set.

---

## Output

Both weavers **render to whatever render target is currently bound** at the time `weave()` is called. There is no `setOutputFrameBuffer` in the modern API.

The rendering is a single **full-screen triangle** (3 vertices, oversized to cover the viewport). The pixel shader does all the optical work:
1. Samples the left/right halves of the SBS input using phase-dependent UV offsets
2. Applies optical correction via the CorrectionA/B textures
3. Applies anti-crosstalk (ACT) compensation
4. Optionally applies sRGB conversion
5. Writes the final weaved pixel to the render target

---

## UV Orientation

The weaver uses a standard DirectX top-left origin convention:

```
Vertex positions (clip space):    UVs:
(-1,  1)  top-left                (0, 0)
( 3,  1)  extends right           (2, 0)
(-1, -3)  extends down            (0, 2)
```

- **V=0 is at the top**, V=1 at the bottom (standard DX convention, NOT OpenGL bottom-up).
- UVs range from 0 to 2 horizontally: `U in [0,1]` samples the left eye, `U in [1,2]` samples the right eye.
- The oversized triangle (extending to 3.0 / -3.0 in clip space and 2.0 in UV) ensures full viewport coverage with a single triangle, relying on scissor clipping.

### Y-axis subtlety

When computing the weaving position on the display, the weaver **negates `yOffset`** before passing it to `FillAttributes()`:

```cpp
Dimenco::Weaver::FillAttributes(
    {windowWidth, windowHeight},
    {xOffset, -yOffset},   // note: negated Y
    ...
);
```

This converts from the window coordinate system (Y-down) to the display's optical coordinate system.

### "Left-only" mode

When weaving is disabled (e.g., no tracking, window on a 2D monitor), the geometry switches to a wider triangle that maps the entire viewport to only the left half of the SBS texture:
```
Vertex positions: (-1, 1), (7, 1), (-1, -3)   // wider X coverage
UVs:              (0, 0),  (2, 0), (0, 2)      // same UVs, shader crops to left half
```

---

## Output Size vs. HWND Size

**The backbuffer size should match the HWND client area size.** This is stated in the `IWeaverBase1` interface:

> "The backbuffer size should match the client size of the window or the user will see artifacts."

### Why this matters

The weaver computes an **absolute pixel offset** on the physical display for each pixel:

```cpp
float xOffset = window_WeavingX + vpX;   // window position on display + viewport offset
float yOffset = window_WeavingY + vpY;
```

These offsets feed into the optical phase formula:

```
phase(x, y) = (x + slant * y) / pitch
```

If the backbuffer resolution doesn't match the HWND client area, the weaver's pixel-to-display mapping is wrong -- it thinks pixel N corresponds to one physical sub-pixel, but it actually maps to another. This causes **severe 3D artifacts and crosstalk**.

### Viewport sub-regions

The viewport **can** be a sub-region of the full backbuffer. The weaver correctly handles partial viewports by incorporating `vpX`/`vpY` offsets into the phase calculation. This is useful for split-screen or picture-in-picture scenarios.

### Resize handling

Neither weaver has an explicit resize callback. They query the current viewport and window position every frame via `SR::Window2::getScreenRect()` and adapt automatically.

---

## DPI Awareness

**The weaver operates in the coordinate space the application gives it.** It does not query `GetDpiForWindow()` during rendering.

### What the weaver does internally

- **Phase snapping** (via `WndProc` hook on `WM_WINDOWPOSCHANGING`): Uses `GetDpiForWindow()` and `GetSystemMetricsForDpi()` to correctly compute window size constraints in physical pixels during drag operations.
- **Monitor detection**: Uses `MonitorFromWindow()` to detect which monitor the window is on, updating the refresh rate for latency prediction when the monitor changes.

### What the application must handle

- **DPI-aware swap chain creation**: The backbuffer must be at the correct **physical pixel** resolution matching the HWND client area in physical pixels.
- If the app is DPI-unaware on a high-DPI display, Windows may virtualize coordinates. The swap chain might be at logical resolution while the display is at physical resolution. The weaver will compute phases based on the viewport it receives -- if that doesn't match actual physical pixels on the SR display, weaving will be incorrect.
- The weaver uses `logical pixel` coordinates (standard D3D viewport/scissor conventions).

---

## The `weave()` Call -- Step by Step

### DX11 flow

1. **Pre-checks**: Reload pixel stamp if display config changed; reload correction textures if flagged.
2. **Viewport query**: Read current viewport from the device context (`vpX`, `vpY`, `vpWidth`, `vpHeight`).
3. **Window mapping**: Call `SR::Window2::getScreenRect()` to map the HWND's client area to its position on the SR display (`window_WeavingX`, `window_WeavingY`, `windowWidth`, `windowHeight`, `screenWidth`, `screenHeight`).
4. **Weaving decision**: Call `canWeaveInternal()` -- checks correction textures loaded, valid window, window visible on SR display, not occluded. Also detects tracking state (eye separation > 1mm = tracking active).
5. **Shader selection**: If weaving state changed, load appropriate shaders (weaving shaders vs. blit shader for pass-through).
6. **Save all pipeline state**: Rasterizer, depth-stencil, all shader stages, samplers, constant buffers, vertex buffers, input layout, scissor rects.
7. **Bind weaver resources**: Set weaver VS/PS, input layout, bind SBS texture (slot 0), correction textures (slots 1-2), pixel stamp (slot 3), constant buffers.
8. **Update geometry**: If weaving is on and tracking, predict eye position via `predictingWeaverTracker->predict()`, then call `surface.updateUserPosition()` which internally calls `Dimenco::Weaver::SetGlobalParameters()` and `FillAttributes()` per vertex to compute phases, DXY offsets, screen positions.
9. **Update lens hint**: Enable/disable switchable lens hardware based on weaving state.
10. **Draw**: For each draw region (split by 3D vs 2D monitor boundaries), set scissor rect, call `surface.draw()` -- sets topology to `TRIANGLE_LIST`, binds vertex buffer, draws 3 vertices.
11. **Restore all pipeline state**.
12. **Late latching** (if enabled, immediate context only): Check how many frames are in flight via D3D11 queries. For frames not yet consumed by the GPU, asynchronously re-update vertex buffers with a fresher eye position using `MAP_WRITE_NO_OVERWRITE`.

### DX12 flow

1. **Pre-checks**: Same as DX11 (pixel stamp, correction textures).
2. **Validate state**: Throws exception if `commandList`, `viewport`, or `scissorRect` not set.
3. **Window mapping**: Same `SR::Window2::getScreenRect()` call.
4. **Weaving decision**: Same `canWeaveInternal()` logic.
5. **Shader / PSO selection**: If state changed, load shaders and create or retrieve cached PSO (keyed by shader bytecode + RT format).
6. **Bind resources**: Set root signature, descriptor heap (4 SRVs), CBVs for weaver data and pixel layout stamp.
7. **Update geometry**: Same eye prediction and `FillAttributes()` logic as DX11.
8. **Update lens hint**: Same as DX11.
9. **Draw**: For each draw region, set scissor rect on the command list, call `surface.draw()` -- sets vertex buffer view, topology, CBV, calls `DrawInstanced(3, 1, 0, 0)`.
10. **No state restore** -- the application owns the command list and manages PSO state.
11. **No late latching** in DX12 -- the application manages its own frame pipelining.

---

## DX11 vs. DX12 Key Differences

| Aspect | DX11 | DX12 |
|--------|------|------|
| **Device context** | Implicit immediate context | Explicit command list provided by app |
| **Viewport / scissor** | Read from device context | Must be set explicitly via `setViewport()` / `setScissorRect()` |
| **Render target** | Already bound on context | Already bound on command list |
| **Output format** | Inferred | Must be set via `setOutputFormat()` |
| **Resource states** | Implicit (runtime manages) | App must ensure correct states (`PIXEL_SHADER_RESOURCE`, `RENDER_TARGET`) |
| **Pipeline state save/restore** | Full save/restore around `weave()` | No save/restore -- app manages PSO |
| **GPU sync** | Internal queries for late latching | App manages fences and frame pipelining |
| **Late latching** | Supported (immediate context only) | Not supported |
| **Initialization** | Lightweight | Creates internal command queue/allocator/list/fence for setup, waits for GPU |
| **Error on missing state** | Silent fallback | Throws exception |

---

## Latency Configuration

Both weavers support two modes for eye position prediction:

### Frame-based (default)
```cpp
void setLatencyInFrames(uint64_t latencyInFrames);
```
Latency is computed dynamically as `latencyInFrames * (1,000,000 / monitorRefreshRate)` microseconds. Automatically updates when the window moves to a different monitor.

### Time-based
```cpp
void setLatency(uint64_t latency);  // microseconds
```
Explicit latency value. Typical formula: `n * 1,000,000 / framerate` microseconds.

**Guidance**: A low-latency app with 1 buffer of latency at 60 Hz would use ~16,666 us. V-sync adds at least 1 buffer; the Windows display manager may add another.

---

## Late Latching (DX11 only)

When enabled via `enableLateLatching(true)`, the DX11 weaver reduces effective latency by retroactively updating frames still in the GPU pipeline:

1. After each `weave()` draw, a `D3D11_QUERY_EVENT` is issued.
2. The weaver tracks how many frames are submitted (CPU) vs. completed (GPU).
3. For frames still in flight, vertex buffers are re-mapped with `D3D11_MAP_WRITE_NO_OVERWRITE` and updated with the latest eye position.
4. This works only with the **immediate context** (not deferred).
5. Up to `MAXFRAMESINFLIGHT` (8) vertex buffer copies are cycled to avoid write-after-read hazards.

Late latching can be force-enabled or force-disabled via display calibration configuration.

---

## Anti-Crosstalk (ACT)

Three modes available via `setACTMode()`:

| Mode | Value | Description |
|------|-------|-------------|
| `Off` | 0 | No crosstalk reduction |
| `Static` | 1 | Applies a static crosstalk compensation factor |
| `Dynamic` | 2 | Applies both static and dynamic factors |

Factors are tunable via `setCrosstalkStaticFactor()` and `setCrosstalkDynamicFactor()`, passed to the shader constant buffer as `weavingFactor_dc` and `weavingFactor_a`.

---

## sRGB Conversion

```cpp
void setShaderSRGBConversion(bool read, bool write);
```

- `read = true`: Shader converts input samples from sRGB to linear before weaving.
- `write = true`: Shader converts output from linear to sRGB before writing to RT.
- Set to `false` when hardware handles conversion (e.g., `_SRGB` format views) or when the pipeline is already in the desired color space.

---

## Phase Snapping

The weaver installs a `WndProc` hook that intercepts `WM_WINDOWPOSCHANGING` messages and snaps the window position to maintain optical phase alignment:

```
phase(x, y) = (x + slant * y) / pitch
```

`SnapToPhase()` finds the nearest integer window position to the proposed movement that keeps `phase` within a configurable threshold (default 0.1) of an integer value. This prevents crosstalk jitter when dragging/resizing windows.

---

## Multi-Monitor Support

The weaver maintains lists of 2D and 3D monitor rectangles. When the window spans multiple monitors, `getDrawRegions()` splits it into regions:

- **3D regions** (on SR display): Full weaving with eye tracking.
- **2D regions** (on regular monitors): Blit pass showing the left view only.

Each region gets its own scissor rect and weaving on/off flag. The weaver polls monitor configuration every ~5 seconds via a background thread.

---

## Constant Buffer Layout

The weaver passes optical parameters to the pixel shader via a constant buffer:

```cpp
struct WeaverDataStruct {
    float weavingFactor_dc;       // static crosstalk factor
    float viewFilterSlope;        // from display calibration
    float weavingPattern;         // pattern type
    int   weavingOn;              // 0 or 1

    float resolution_aspect;      // screenWidth / screenHeight
    float weavingFactor_a;        // dynamic crosstalk factor
    float contrast;               // user-set, default 1.0
    float srgb_convert_read;      // 0.0 or 1.0

    float srgb_convert_write;     // 0.0 or 1.0
    float padding;
    FLOAT2 origin;                // (xOffset/screenWidth, yOffset/screenHeight)

    FLOAT2 viewScale;             // (vpWidth/screenWidth, vpHeight/screenHeight)
    FLOAT2 screen_resolution;     // (screenWidth, screenHeight) in pixels
};
```

---

## Vertex Layout

Each of the 3 vertices in the full-screen triangle carries:

```cpp
struct WeaverVertex {
    XMFLOAT2 position;       // NDC position
    XMFLOAT2 UV;             // texture UV (0-2 range for SBS)
    XMFLOAT4 phases;         // optical phase per sub-pixel
    XMFLOAT4 DXY;            // differential sampling offsets
    XMFLOAT2 screenPosition; // position on physical display
    XMFLOAT2 weaverVars;     // additional weaver parameters
    XMFLOAT2 DXYInitial;     // original DXY before late-latch update
};
```

These are computed per-frame by `Dimenco::Weaver::FillAttributes()` based on the current eye position and display calibration. The GPU interpolates them across the triangle and the pixel shader uses them to determine which sub-pixel sees which view.

---

## Background Threads

Both weavers spawn two polling threads:

1. **Monitor polling** (`pollingLoopMonitors`): Checks system monitor configuration every ~5 seconds. Updates the list of 2D vs 3D monitor rectangles. Triggers reconfiguration when monitors are added/removed.

2. **FPC polling** (`pollingLoopFPCs`): Monitors Flexible Display Controller state via shared memory. Reads device serial numbers and updates display calibration when the connected display changes.

Both threads are guarded by `determineWeavingEnabledMutex` and cleanly joined in the destructor.
