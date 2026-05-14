# Leia SR Weaving -- DX11, DX12, OpenGL & Vulkan

This document describes the technical operation of all four weavers in the Leia SR SDK. All weavers share the same underlying optical algorithm (via `Dimenco::Weaver`) but differ in how they interact with each graphics API.

---

## Overview

The weaver takes a **stereo texture** and a **tracked eye position**, and produces a **weaved image** on the currently bound render target. The weaved image encodes per-sub-pixel view information so that the lenticular or switchable lens on the SR display directs the correct view to each eye.

---

## Creation

### Modern API (recommended)

| | DX11 | DX12 | OpenGL | Vulkan |
|---|---|---|---|---|
| Factory | `CreateDX11Weaver(SRContext*, ID3D11DeviceContext*, HWND, IDX11Weaver1**)` | `CreateDX12Weaver(SRContext*, ID3D12Device*, HWND, IDX12Weaver1**)` | `CreateGLWeaver(SRContext&, HWND, IGLWeaver1**)` | `CreateVulkanWeaver(SRContext&, VkDevice, VkPhysicalDevice, VkQueue, VkCommandPool, HWND, IVulkanWeaver1**)` |
| HWND | Optional; can be set later via `setWindowHandle()` | Optional; can be set later via `setWindowHandle()` | Optional; can be set later via `setWindowHandle()` | Optional; can be set later via `setWindowHandle()` |
| Returns | `WeaverErrorCode` | `WeaverErrorCode` | `WeaverErrorCode` | `WeaverErrorCode` |

All interfaces inherit from `IWeaverBase1` (common weaving API) and `IWeaverSettings1` (quality tuning).

### Deprecated API

Older classes (`DX11Weaver`, `PredictingDX11Weaver`, `DX12Weaver`, `PredictingDX12Weaver`, `GLWeaver`, `PredictingGLWeaver`) are still available but deprecated.

---

## Inputs

### Input Texture

**DX11** -- single SBS (side-by-side) texture:
```cpp
void setInputViewTexture(ID3D11ShaderResourceView* texture, int width, int height, DXGI_FORMAT format);
```
- `width` is the **full SBS width** (each eye occupies `width/2` pixels).
- The weaver calls `AddRef()` on the SRV and holds it until replaced.

**DX12** -- single SBS texture:
```cpp
void setInputViewTexture(ID3D12Resource* texture, int width, int height, DXGI_FORMAT format);
```
- Same semantics. The weaver creates an internal SRV at descriptor heap slot 0.
- The resource **must be in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`** when `weave()` is called.
- Typeless formats are supported -- pass the fully qualified format via the `format` parameter.

**OpenGL** -- single SBS texture:
```cpp
void setInputViewTexture(GLuint texture, int width, int height, GLenum format);
```
- `texture` is the GL texture name (ID) containing the SBS stereo image.
- Same SBS layout: left eye on the left half, right eye on the right half.

**Vulkan** -- two separate image views:
```cpp
void setInputViewTexture(VkImageView textureViewLeft, VkImageView textureViewRight, int width, int height, VkFormat format);
```
- Unlike the other backends, Vulkan accepts **separate left and right image views**.
- Both images must be in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` when `weave()` is called.

Additionally, Vulkan requires an explicit output framebuffer:
```cpp
void setOutputFrameBuffer(VkFramebuffer framebuffer, int width, int height, VkFormat format);
```

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

### DX11 (implicit state)

- **Render target**: Bind the output RT (e.g., backbuffer) via `OMSetRenderTargets` before calling `weave()`.
- **Viewport / scissor**: Set on the device context. The weaver reads them automatically via `RSGetViewports()`.
- **Device context**: Passed at creation or via `setContext()`.

The DX11 weaver **saves and restores all pipeline state** around the weave call (shaders, samplers, rasterizer state, depth-stencil, vertex buffers, input layout, constant buffers, scissor rects). It is transparent to the application's rendering pipeline.

### DX12 (explicit state)

```cpp
void setCommandList(ID3D12GraphicsCommandList* commandList);  // required
void setViewport(D3D12_VIEWPORT viewport);                     // required
void setScissorRect(D3D12_RECT scissorRect);                   // required
void setOutputFormat(DXGI_FORMAT format);                       // must match bound RT
```

The render target must already be bound on the command list and in `D3D12_RESOURCE_STATE_RENDER_TARGET`. The weaver **records draw commands** into the provided command list -- it does not execute them. The application is responsible for closing the list, executing it, and managing GPU synchronization. Throws an exception if `commandList`, `viewport`, or `scissorRect` are not set.

### OpenGL (implicit state)

- **Framebuffer**: Bind the output FBO (or default framebuffer 0) before calling `weave()`.
- **Viewport**: Set via `glViewport()`. The weaver reads it automatically via `glGetIntegerv(GL_VIEWPORT, ...)`.
- **No other state required**: The weaver saves and restores GL state (shaders, textures on units 0-3, samplers, depth test, scissor test, cull mode, active texture unit).

The GL weaver internally converts viewport coordinates from OpenGL's **bottom-left origin** to the **top-left origin** used by the optical model:
```cpp
vpY = windowHeight - (vpY + vpHeight);
```

### Vulkan (explicit state)

```cpp
void setCommandBuffer(VkCommandBuffer commandBuffer);  // required
void setViewport(RECT viewport);                        // required
void setScissorRect(RECT scissorRect);                  // required
void setOutputFrameBuffer(VkFramebuffer framebuffer, int width, int height, VkFormat format);  // required
```

The input images must be in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. The output framebuffer must be in `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`. The weaver **records commands** into the provided command buffer and **manages its own render pass** internally (begin/end). Throws an exception if `commandBuffer`, `viewport`, or `scissorRect` are not set.

---

## Output

All weavers **render to the currently bound render target** (DX11, GL) or the explicitly provided target (DX12 command list, Vulkan framebuffer) at the time `weave()` is called.

The rendering is a single **full-screen triangle** (3 vertices, oversized to cover the viewport). The pixel shader does all the optical work:
1. Samples the left/right views using phase-dependent UV offsets
2. Applies optical correction via the CorrectionA/B textures
3. Applies anti-crosstalk (ACT) compensation
4. Optionally applies sRGB conversion
5. Writes the final weaved pixel to the render target

---

## UV Orientation

All weavers use the same logical UV convention, regardless of the underlying graphics API's native coordinate system:

```
Vertex positions (clip space):    UVs:
(-1,  1)  top-left                (0, 0)
( 3,  1)  extends right           (2, 0)
(-1, -3)  extends down            (0, 2)
```

- UVs range from 0 to 2 horizontally: `U in [0,1]` samples the left eye, `U in [1,2]` samples the right eye.
- The oversized triangle (extending to 3.0 / -3.0 in clip space and 2.0 in UV) ensures full viewport coverage with a single triangle, relying on scissor clipping.

### Y-axis handling per API

| API | Native origin | Weaver handling |
|-----|---------------|-----------------|
| DX11 / DX12 | Top-left (V=0 at top) | Used directly, no conversion needed |
| OpenGL | Bottom-left (V=0 at bottom) | Weaver converts `vpY = windowHeight - (vpY + vpHeight)` before phase calculation; UV data accounts for the flip |
| Vulkan | Top-left (same as DX) | Uses negative viewport height (`height = -vpHeight`) to match DX convention |

### Y-offset negation

When computing the weaving position on the display, all weavers **negate `yOffset`** before passing it to `FillAttributes()`:
```cpp
Dimenco::Weaver::FillAttributes(
    {windowWidth, windowHeight},
    {xOffset, -yOffset},   // note: negated Y
    ...
);
```
This converts from the window coordinate system (Y-down) to the display's optical coordinate system.

### "Left-only" mode

When weaving is disabled (e.g., no tracking, window on a 2D monitor), the geometry switches to a wider triangle that maps the entire viewport to only the left half of the texture:
```
Vertex positions: (-1, 1), (7, 1), (-1, -3)
UVs:              (0, 0),  (2, 0), (0, 2)      // shader crops to left half
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

---

## Viewport Sub-Region Weaving

The viewport **can** be a sub-region of the full backbuffer. All four weavers correctly handle partial viewports by incorporating `vpX`/`vpY` offsets into the phase calculation. This is useful for split-screen, picture-in-picture, or rendering 3D content into only a portion of the window.

### How it works

All four backends follow the same flow:

1. **Read/receive the viewport** -- either from API state (DX11, GL) or from explicit setter (DX12, Vulkan)
2. **Get the window position** on the SR display via `Window2::getScreenRect()` -> `window_WeavingX`, `window_WeavingY`
3. **Combine offsets**: `xOffset = window_WeavingX + vpX`, `yOffset = window_WeavingY + vpY`
4. **Compute per-vertex optical phases** via `Dimenco::Weaver::FillAttributes({windowW, windowH}, {xOffset, -yOffset}, ...)`
5. **Set shader constants**: `origin = {xOffset/screenW, yOffset/screenH}`, `viewScale = {vpW/screenW, vpH/screenH}`

Because the viewport offset is added to the window position, the weaver knows exactly which **physical sub-pixels** on the display correspond to the sub-region. The phase calculation is correct regardless of where in the backbuffer the viewport sits.

### Setup per API

**DX11** -- set the D3D11 viewport to the desired sub-rect:
```cpp
D3D11_VIEWPORT vp = {};
vp.TopLeftX = 100;    // sub-rect X offset within the backbuffer
vp.TopLeftY = 50;     // sub-rect Y offset within the backbuffer
vp.Width    = 800;    // sub-rect width
vp.Height   = 600;    // sub-rect height
vp.MinDepth = 0.0f;
vp.MaxDepth = 1.0f;
context->RSSetViewports(1, &vp);

// Bind your backbuffer as render target
context->OMSetRenderTargets(1, &backbufferRTV, nullptr);

// Provide the SBS texture for just this sub-rect's content
weaver->setInputViewTexture(sbsSRV, sbsWidth, sbsHeight, format);

// Weave -- the weaver reads the viewport and computes correct phases
weaver->weave();
```

**DX12** -- pass the sub-rect viewport explicitly:
```cpp
D3D12_VIEWPORT vp = {};
vp.TopLeftX = 100;
vp.TopLeftY = 50;
vp.Width    = 800;
vp.Height   = 600;
vp.MinDepth = 0.0f;
vp.MaxDepth = 1.0f;

D3D12_RECT scissor = { 100, 50, 900, 650 };  // must match viewport

weaver->setCommandList(commandList);
weaver->setViewport(vp);
weaver->setScissorRect(scissor);
weaver->setOutputFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
weaver->setInputViewTexture(sbsResource, sbsWidth, sbsHeight, format);

// Render target must already be bound on commandList in RENDER_TARGET state
weaver->weave();
```

**OpenGL** -- set the GL viewport to the desired sub-rect:
```cpp
// Note: OpenGL viewport uses bottom-left origin
// If your sub-rect top-left is (100, 50) in window coords with window height 1080:
int glY = 1080 - (50 + 600);  // = 430
glViewport(100, glY, 800, 600);

// Bind your output framebuffer
glBindFramebuffer(GL_FRAMEBUFFER, 0);  // or your FBO

// Provide the SBS texture
weaver->setInputViewTexture(sbsTexture, sbsWidth, sbsHeight, GL_RGBA8);

// Weave -- the weaver reads GL_VIEWPORT, converts Y, and computes correct phases
weaver->weave();
```

**Vulkan** -- pass the sub-rect viewport explicitly via RECT:
```cpp
RECT viewport = { 100, 50, 900, 650 };  // left, top, right, bottom
RECT scissor  = { 100, 50, 900, 650 };  // must match viewport

weaver->setCommandBuffer(commandBuffer);
weaver->setViewport(viewport);
weaver->setScissorRect(scissor);
weaver->setOutputFrameBuffer(framebuffer, fbWidth, fbHeight, format);
weaver->setInputViewTexture(leftView, rightView, viewWidth, viewHeight, format);

// Input images must be in SHADER_READ_ONLY_OPTIMAL
// Output framebuffer must be in COLOR_ATTACHMENT_OPTIMAL
weaver->weave();
```

### Constraints

- **The backbuffer must still match the HWND client area** in physical pixels. The viewport is a sub-rect *within* that correctly-sized backbuffer. A smaller backbuffer will break the phase math.
- **The SBS texture contains the content for the sub-rect only** -- not the entire window. The weaver maps the full SBS texture onto the viewport sub-region.
- **Multiple sub-rects per frame**: You can call `weave()` multiple times per frame with different viewports and different SBS textures. Each call is independent. (On DX11, update the viewport and input texture between calls. On explicit APIs, set the new viewport/scissor/texture before each call.)

---

## Resize Handling

Neither weaver has an explicit resize callback. They query the current viewport and window position every frame via `SR::Window2::getScreenRect()` and adapt automatically.

---

## DPI Awareness

**The weaver operates in the coordinate space the application gives it.** It does not query `GetDpiForWindow()` during rendering.

### What the weaver does internally

- **Phase snapping** (via `WndProc` hook on `WM_WINDOWPOSCHANGING`): Uses `GetDpiForWindow()` and `GetSystemMetricsForDpi()` to correctly compute window size constraints in physical pixels during drag operations.
- **Monitor detection**: Uses `MonitorFromWindow()` to detect which monitor the window is on, updating the refresh rate for latency prediction when the monitor changes.

### What the application must handle

- **DPI-aware swap chain / framebuffer creation**: The backbuffer must be at the correct **physical pixel** resolution matching the HWND client area in physical pixels.
- If the app is DPI-unaware on a high-DPI display, Windows may virtualize coordinates. The swap chain might be at logical resolution while the display is at physical resolution. The weaver will compute phases based on the viewport it receives -- if that doesn't match actual physical pixels on the SR display, weaving will be incorrect.

---

## The `weave()` Call -- Step by Step

### DX11 flow

1. **Pre-checks**: Reload pixel stamp if display config changed; reload correction textures if flagged.
2. **Viewport query**: Read current viewport from the device context (`vpX`, `vpY`, `vpWidth`, `vpHeight`).
3. **Window mapping**: Call `SR::Window2::getScreenRect()` to get `window_WeavingX`, `window_WeavingY`, `windowWidth`, `windowHeight`, `screenWidth`, `screenHeight`.
4. **Weaving decision**: Call `canWeaveInternal(vpWidth, vpHeight, vpX, vpY)` -- checks correction textures loaded, valid window, window visible on SR display, not occluded.
5. **Shader selection**: If weaving state changed, load appropriate shaders.
6. **Save all pipeline state**: Rasterizer, depth-stencil, all shader stages, samplers, constant buffers, vertex buffers, input layout, scissor rects.
7. **Bind weaver resources**: Set weaver VS/PS, input layout, SBS texture (slot 0), correction textures (slots 1-2), pixel stamp (slot 3), constant buffers.
8. **Update geometry**: Predict eye position, then call `surface.updateUserPosition()` which calls `Dimenco::Weaver::SetGlobalParameters()` and `FillAttributes()` per vertex with `xOffset = window_WeavingX + vpX`.
9. **Update lens hint**: Enable/disable switchable lens.
10. **Draw**: For each draw region (split by 3D vs 2D monitor boundaries), set scissor rect, draw 3 vertices.
11. **Restore all pipeline state**.
12. **Late latching** (if enabled, immediate context only): Re-update vertex buffers for in-flight frames with fresher eye positions.

### DX12 flow

1. **Pre-checks**: Same as DX11.
2. **Validate state**: Throws exception if `commandList`, `viewport`, or `scissorRect` not set.
3. **Window mapping**: Same `getScreenRect()` call.
4. **Weaving decision**: Same `canWeaveInternal()` logic.
5. **Shader / PSO selection**: Create or retrieve cached PSO keyed by shader bytecode + RT format.
6. **Bind resources**: Set root signature, descriptor heap (4 SRVs), CBVs.
7. **Update geometry**: Same eye prediction and `FillAttributes()` logic with `xOffset = window_WeavingX + vpX`.
8. **Update lens hint**: Same.
9. **Draw**: For each draw region, set scissor rect, `DrawInstanced(3, 1, 0, 0)`.
10. **No state restore** -- application owns the command list.
11. **No late latching** in DX12.

### OpenGL flow

1. **Pre-checks**: Same as DX11.
2. **Viewport query**: Read current viewport via `glGetIntegerv(GL_VIEWPORT, ...)`, then convert Y: `vpY = windowHeight - (vpY + vpHeight)`.
3. **Window mapping**: Same `getScreenRect()` call.
4. **Weaving decision**: Same `canWeaveInternal()` logic.
5. **Shader selection**: Choose between optimized weaver shader and pattern shader.
6. **Save GL state**: Cull mode, depth test, current program, active texture unit, texture bindings on units 0-3, sampler bindings, scissor test and box.
7. **Bind weaver resources**: Use weaver shader, bind SBS texture (unit 0), correction textures (units 1-2), pixel stamp (unit 3).
8. **Update geometry**: Same `FillAttributes()` logic with `xOffset = window_WeavingX + vpX`.
9. **Update lens hint**: Same.
10. **Draw**: For each draw region, convert scissor Y to GL bottom-left coords, `glScissor()`, draw 3 vertices.
11. **Restore all GL state**.
12. **Late latching** (if supported): Uses `glFenceSync` / `glClientWaitSync` to track GPU progress, re-updates geometry for in-flight frames.

### Vulkan flow

1. **Pre-checks**: Same as DX11.
2. **Validate state**: Throws exception if `commandBuffer`, `viewport`, or `scissorRect` not set.
3. **Window mapping**: Same `getScreenRect()` call.
4. **Weaving decision**: Same `canWeaveInternal()` logic.
5. **Pipeline selection**: Create or retrieve cached pipeline and render pass.
6. **Begin render pass**: Weaver manages its own `vkCmdBeginRenderPass`.
7. **Bind resources**: Set descriptor sets (4 image bindings), push constants / uniform buffers.
8. **Update geometry**: Same `FillAttributes()` logic with `xOffset = window_WeavingX + vpX`.
9. **Set viewport**: Creates `VkViewport` with negative height for Y-flip: `height = -vpHeight`, `y = vpY + vpHeight`.
10. **Draw**: For each draw region, intersect with scissor, `vkCmdSetScissor`, draw 3 vertices.
11. **End render pass**: `vkCmdEndRenderPass`.
12. **No late latching** in Vulkan.

---

## API Comparison

| Aspect | DX11 | DX12 | OpenGL | Vulkan |
|--------|------|------|--------|--------|
| **Input texture** | Single SBS SRV | Single SBS resource | Single SBS GLuint | Two separate VkImageViews |
| **Output target** | Bound on context | Bound on command list | Bound FBO | Explicit VkFramebuffer |
| **Viewport** | Read from context | Explicit `setViewport()` | Read via `glGetIntegerv` | Explicit `setViewport(RECT)` |
| **Scissor** | Read from context | Explicit `setScissorRect()` | Read via `glGetIntegerv` | Explicit `setScissorRect(RECT)` |
| **Output format** | Inferred | Explicit `setOutputFormat()` | Inferred | Explicit via `setOutputFrameBuffer()` |
| **Resource states** | Implicit | App manages | Implicit | App manages image layouts |
| **Pipeline state save/restore** | Full save/restore | No | Full save/restore | No (owns render pass) |
| **Command recording** | Immediate | Into command list | Immediate | Into command buffer |
| **Late latching** | Yes (immediate ctx) | No | Yes (if `GL_EXT_buffer_storage`) | No |
| **Y-axis conversion** | None | None | Bottom-left to top-left | Negative viewport height |
| **Error on missing state** | Silent fallback | Throws exception | Silent fallback | Throws exception |
| **Render pass** | N/A | N/A | N/A | Self-managed internally |

---

## Latency Configuration

All weavers support two modes for eye position prediction:

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

## Late Latching

| API | Supported | Mechanism |
|-----|-----------|-----------|
| DX11 | Yes (immediate context only) | `D3D11_QUERY_EVENT` + `D3D11_MAP_WRITE_NO_OVERWRITE` |
| DX12 | No | Application manages frame pipelining |
| OpenGL | Yes (if `GL_EXT_buffer_storage`) | `glFenceSync` / `glClientWaitSync` + persistent mapped buffers |
| Vulkan | No | Application manages frame pipelining |

When enabled, the weaver reduces effective latency by retroactively updating vertex buffers for frames still in the GPU pipeline with the latest eye position. Up to `MAXFRAMESINFLIGHT` (8) vertex buffer copies are cycled to avoid write-after-read hazards.

---

## Anti-Crosstalk (ACT)

Three modes available via `setACTMode()`:

| Mode | Value | Description |
|------|-------|-------------|
| `Off` | 0 | No crosstalk reduction |
| `Static` | 1 | Applies a static crosstalk compensation factor |
| `Dynamic` | 2 | Applies both static and dynamic factors |

Factors are tunable via `setCrosstalkStaticFactor()` and `setCrosstalkDynamicFactor()`.

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

For sub-rect weaving, `origin` reflects the viewport's position on the physical display (not just the window position), and `viewScale` reflects the viewport size relative to the full screen.

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

All weavers spawn two polling threads:

1. **Monitor polling** (`pollingLoopMonitors`): Checks system monitor configuration every ~5 seconds. Updates the list of 2D vs 3D monitor rectangles.

2. **FPC polling** (`pollingLoopFPCs`): Monitors Flexible Display Controller state via shared memory. Reads device serial numbers and updates display calibration when the connected display changes.

Both threads are guarded by `determineWeavingEnabledMutex` and cleanly joined in the destructor.
