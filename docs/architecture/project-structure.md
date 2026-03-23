# DisplayXR Project Structure

This document describes the architecture of this Monado-based OpenXR runtime fork, extended for multi-vendor 3D display support.

## Source Tree

All runtime source code lives under `src/xrt/`:

```
src/xrt/
├── include/xrt/              Core interfaces (45 headers)
│   ├── xrt_device.h                    Abstract device (HMDs, controllers)
│   ├── xrt_compositor.h                Compositor interface
│   ├── xrt_display_processor.h         Vulkan display processing
│   ├── xrt_display_processor_d3d11.h   D3D11 display processing
│   ├── xrt_instance.h                  Runtime instance
│   └── xrt_prober.h                    Device discovery
│
├── drivers/                   3 driver directories
│   ├── leia/                  Leia SR SDK — real 3D display hardware
│   ├── sim_display/           Simulation display — no hardware needed
│   └── qwerty/                Keyboard-based debugging device
│
├── compositor/                Rendering pipeline
│   ├── main/                  Vulkan compositor (+ display processor integration)
│   ├── multi/                 Multi-client session coordinator
│   ├── d3d11/                 Native D3D11 compositor (Windows, in-process)
│   ├── d3d11_service/         Native D3D11 compositor (Windows, service mode)
│   ├── d3d12/                 Native D3D12 compositor (Windows)
│   ├── metal/                 Native Metal compositor (macOS)
│   ├── gl/                    Native OpenGL compositor (Windows + macOS)
│   ├── vk_native/             Native Vulkan compositor (Windows + macOS)
│   ├── client/                Client-side API glue (GL, Vulkan, D3D11, D3D12)
│   ├── render/                Vulkan render helpers
│   ├── shaders/               GLSL shader sources
│   ├── mock/                  Mock compositor (testing)
│   ├── null/                  Null compositor (no-op)
│   └── util/                  Compositor utilities (swapchain, sync)
│
├── state_trackers/
│   └── oxr/                   OpenXR API implementation
│
├── targets/common/            Builder registration
│   ├── target_lists.c         Master list of all device builders
│   └── target_builder_*.c     5 builder implementations
│
├── auxiliary/                 Shared utilities
│   ├── math/                  Math (m_*), quaternions, matrices, poses
│   ├── util/                  General utilities (u_*), logging, threading
│   ├── os/                    OS abstraction (os_*)
│   ├── vk/                    Vulkan helpers (vk_*)
│   └── d3d/                   Direct3D helpers
│
└── ipc/                       Inter-process communication (service mode)
```

## Supported Devices

### Leia 3D Display (`src/xrt/drivers/leia/`)

Real lightfield display hardware with eye tracking.

| Platform | SDK | Graphics | Eye Tracking |
|----------|-----|----------|--------------|
| Windows  | SR SDK | Vulkan + D3D11 weaving | LookaroundFilter |
| Android  | CNSDK  | Vulkan interlacing     | Not yet |

**Files (13):**
- `leia_device.c` — `xrt_device` implementation (display specs, FOV, refresh rate)
- `leia_sr.cpp/.h` — Vulkan SR SDK weaver wrapper
- `leia_sr_d3d11.cpp/.h` — D3D11 SR SDK weaver wrapper
- `leia_cnsdk.cpp/.h` — Android CNSDK integration
- `leia_display_processor.cpp/.h` — Vulkan `xrt_display_processor` wrapping SR weaver
- `leia_display_processor_d3d11.cpp/.h` — D3D11 `xrt_display_processor_d3d11` wrapping SR weaver
- `leia_types.h`, `leia_interface.h`

**Builder:** `target_builder_leia.c` (priority -15)

### Simulation Display (`src/xrt/drivers/sim_display/`)

Pure software simulation for development and testing without hardware.

| Mode | Description | Implementation |
|------|-------------|----------------|
| `sbs` | Side-by-side stereo | Viewport configuration (no shader) |
| `anaglyph` | Red-cyan stereoscopy | Vulkan GLSL + D3D11 HLSL shaders |
| `blend` | 50/50 alpha overlay | Vulkan GLSL + D3D11 HLSL shaders |

**Configuration via environment variables:**
```
SIM_DISPLAY_ENABLE=1                  # Enable the driver
SIM_DISPLAY_OUTPUT=sbs|anaglyph|blend # Output mode
SIM_DISPLAY_WIDTH_M=0.344             # Physical width (meters)
SIM_DISPLAY_HEIGHT_M=0.194            # Physical height (meters)
SIM_DISPLAY_NOMINAL_Z_M=0.5           # Nominal viewing distance
SIM_DISPLAY_PIXEL_W=3840              # Resolution width
SIM_DISPLAY_PIXEL_H=2160              # Resolution height
```

**Files (4 + shaders):**
- `sim_display_device.c` — `xrt_device` with configurable specs
- `sim_display_processor.c` — Vulkan display processor (GLSL shaders)
- `sim_display_processor_d3d11.cpp` — D3D11 display processor (HLSL shaders)
- `sim_display_interface.h` — public API
- `shaders/anaglyph.frag`, `shaders/blend.frag`, `shaders/fullscreen.vert`

**Builder:** `target_builder_sim_display.c` (priority -20)

## Display Processor Abstraction

The display processor interface decouples the compositor from vendor-specific stereo-to-display output processing (interlacing, side-by-side, anaglyph, etc.). The compositor calls the interface generically — it does not know which vendor is behind it.

### Vulkan Interface

```c
// src/xrt/include/xrt/xrt_display_processor.h
struct xrt_display_processor {
    void (*process_atlas)(
        struct xrt_display_processor *xdp,
        VkCommandBuffer cmd_buffer,         // Records GPU commands
        VkImage_XDP atlas_image,            // Atlas texture (all views tiled)
        VkImageView atlas_view,             // Atlas image view
        uint32_t view_width, view_height,   // Per-view tile dimensions
        uint32_t tile_columns, tile_rows,   // Atlas layout
        VkFormat_XDP view_format,           // int32_t alias (no Vulkan header dep)
        VkFramebuffer target_fb,            // Output framebuffer
        uint32_t target_width, target_height,
        VkFormat_XDP target_format);
    VkRenderPass (*get_render_pass)(struct xrt_display_processor *xdp);
    void (*destroy)(struct xrt_display_processor *xdp);
};
```

Used by: `comp_renderer.c` (main compositor), `comp_multi_system.c` (multi-session)

### D3D11 Interface

```c
// src/xrt/include/xrt/xrt_display_processor_d3d11.h
struct xrt_display_processor_d3d11 {
    void (*process_atlas)(
        struct xrt_display_processor_d3d11 *xdp,
        void *d3d11_context,                // ID3D11DeviceContext*
        void *atlas_srv,                    // ID3D11ShaderResourceView* (atlas texture)
        uint32_t view_width, view_height,   // Per-view tile dimensions
        uint32_t tile_columns, tile_rows,   // Atlas layout
        uint32_t format,                    // DXGI_FORMAT as uint32_t
        uint32_t target_width, target_height);
    void (*destroy)(struct xrt_display_processor_d3d11 *xdp);
};
```

Used by: `comp_d3d11_compositor.cpp`

**Key differences from Vulkan:** Input is a tiled atlas SRV (not separate VkImageViews). Output goes to currently bound render target (no framebuffer parameter). No command buffer — D3D11 is immediate-mode.

### Implementations

| Vendor | Vulkan | D3D11 |
|--------|--------|-------|
| Leia (SR SDK) | `leia_display_processor.cpp` | `leia_display_processor_d3d11.cpp` |
| Simulation | `sim_display_processor.c` | `sim_display_processor_d3d11.cpp` |

## Rendering Pipelines

### Vulkan Path
```
OpenGL/Vulkan app
  → Client compositor (API translation)
    → Null compositor (pass-through)
      → Multi compositor (session coordination)
        → xrt_display_processor::process_atlas()
          → Display output
```

### D3D11 Native Path (Windows)
```
D3D11 app
  → comp_d3d11 compositor (direct D3D11 rendering)
    → xrt_display_processor_d3d11::process_atlas()
      → Display output
```

The D3D11 path bypasses Vulkan entirely, solving Intel GPU interop issues where D3D11-to-Vulkan texture import fails.

## Builder Priority System

Device builders are registered in `src/xrt/targets/common/target_lists.c` and tried in priority order (lower number = higher priority):

| Priority | Builder | Description |
|----------|---------|-------------|
| override | `qwerty` | Keyboard debugging device |
| override | `qwerty_input` | Keyboard input device |
| -15 | **`leia`** | Leia 3D display (SR SDK / CNSDK) |
| -20 | **`sim_display`** | Simulation display |
| last | `legacy` | Legacy device fallback |

## Design Patterns

### C Vtable Polymorphism
All key interfaces (`xrt_device`, `xrt_display_processor`, `xrt_builder`, `xrt_compositor`) are C structs with function pointers. This provides:
- ABI stability across shared library boundaries
- IPC compatibility (static data can be serialized)
- No C++ runtime dependency in core code

### Conditional Compilation
Platform-specific code is isolated via:
- **CMake:** `if(WIN32)`, `if(XRT_HAVE_VULKAN)`, `if(XRT_HAVE_LEIA_SR)`
- **C/C++:** `#ifdef XRT_OS_WINDOWS`, `#ifdef XRT_OS_ANDROID`

### Environment Variable Gates
Runtime driver selection without recompilation:
- `SIM_DISPLAY_ENABLE=1` — Enable simulation display
- `SIM_DISPLAY_OUTPUT=sbs|anaglyph|blend` — Select output mode
- `OXR_ENABLE_D3D11_NATIVE_COMPOSITOR=1` — Enable D3D11 compositor

## Extending the Project

### Adding a New Display Vendor

A new vendor needs **3 components** and **zero changes to compositor code**:

1. **Display Processor** — implement the vtable interface:
   ```
   src/xrt/drivers/vendor/
     vendor_display_processor.cpp      # xrt_display_processor implementation
     vendor_display_processor_d3d11.cpp # xrt_display_processor_d3d11 (if Windows)
   ```

2. **Device** — implement `xrt_device` with display specs:
   ```
   src/xrt/drivers/vendor/
     vendor_device.c                   # Resolution, FOV, refresh rate, physical size
     vendor_interface.h                # Public API
   ```

3. **Builder** — register in `target_lists.c`:
   ```
   src/xrt/targets/common/
     target_builder_vendor.c           # estimate_system() with priority + open_system()
   ```

   Then add to `target_builder_interface.h` and `target_lists.c`:
   ```c
   #ifdef T_BUILDER_VENDOR
   struct xrt_builder *t_builder_vendor_create(void);
   #endif
   ```

4. **CMake** — add to `src/xrt/drivers/CMakeLists.txt`:
   ```cmake
   if(XRT_HAVE_VENDOR_SDK)
       add_library(drv_vendor STATIC vendor/vendor_device.c vendor/vendor_display_processor.cpp)
       target_link_libraries(drv_vendor PRIVATE xrt-interfaces aux_util VENDOR::SDK)
       list(APPEND ENABLED_DRIVERS drv_vendor)
   endif()
   ```

The compositor discovers the display processor through the builder system and calls it through the generic interface. No vendor-specific code in the compositor.

### Adding a New OS Platform

Platform coupling by layer:

| Layer | Platform Dependency |
|-------|-------------------|
| `xrt_display_processor.h` | None (int32_t aliases for Vulkan types) |
| `xrt_display_processor_d3d11.h` | Windows only (void* for D3D types) |
| `sim_display_device.c` | None — fully portable |
| `sim_display_processor.c` | Vulkan only (any Vulkan-capable OS) |
| `leia_device.c` | `#ifdef XRT_OS_WINDOWS` for SR SDK, fallback defaults otherwise |
| `leia_cnsdk.cpp` | `#ifdef XRT_OS_ANDROID` for JNI init |

To add Linux support for a new display:
- The Vulkan display processor path works unchanged (Vulkan is cross-platform)
- Only the driver needs platform-specific code for SDK integration
- `sim_display` already works on any Vulkan-capable OS for testing
