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
├── drivers/                   2 in-tree driver directories
│   │                          (vendor display processors ship as plug-in DLLs — ADR-019)
│   ├── sim_display/           Simulation display + vendor-neutral plug-in — no hardware
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

### Vendor 3D Displays (plug-in DLLs)

Real lightfield-display hardware is supported through **vendor plug-in DLLs**, not in-tree
drivers. Per [ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md), each vendor's display
processor (eye-tracking + per-API weaving) ships from its own repo as a plug-in discovered at
`xrCreateInstance`; the runtime DLL carries zero vendor identifiers in its link line. The
runtime calls the plug-in only through the vendor-neutral `xrt_display_processor*` vtables and
the `xrt_plugin_iface` discovery contract.

- To add a vendor: [`docs/guides/vendor-plugin-onboarding.md`](../guides/vendor-plugin-onboarding.md)
  → [`docs/reference/xrt_plugin_iface.md`](../reference/xrt_plugin_iface.md) +
  [`docs/specs/runtime/plugin-discovery.md`](../specs/runtime/plugin-discovery.md).
- The first vendor integration (Leia SR) ships from
  [`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin); its
  implementation notes live under [`displayxr-leia-plugin/docs/`](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/docs/README.md).

### Simulation Display (`src/xrt/drivers/sim_display/`)

Pure software simulation for development and testing without hardware.

| Mode | Description | Implementation |
|------|-------------|----------------|
| `sbs` | Side-by-side stereo | Viewport configuration (no shader) |
| `anaglyph` | Red-cyan stereoscopy | Vulkan GLSL + D3D11 HLSL shaders |
| `blend` | 50/50 alpha overlay | Vulkan GLSL + D3D11 HLSL shaders |

**Configuration via environment variables:**
```
FORCE_SIM_DISPLAY=1                   # Force sim_display over vendor drivers
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
| Vendor DP (plug-in DLL) | per-vendor, e.g. [`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin) | per-vendor |
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
| -20 | **`sim_display`** | Simulation display |
| last | `legacy` | Legacy device fallback |

Vendor 3D-display DPs are **not** in-tree builders — they ship as plug-in DLLs discovered at
`xrCreateInstance` and register their DP factory via `xrt_plugin_iface` (ADR-019). Discovery
order is governed by the plug-in's registered probe order, not this table. See
[`docs/specs/runtime/plugin-discovery.md`](../specs/runtime/plugin-discovery.md).

## Design Patterns

### C Vtable Polymorphism
All key interfaces (`xrt_device`, `xrt_display_processor`, `xrt_builder`, `xrt_compositor`) are C structs with function pointers. This provides:
- ABI stability across shared library boundaries
- IPC compatibility (static data can be serialized)
- No C++ runtime dependency in core code

### Conditional Compilation
Platform-specific code is isolated via:
- **CMake:** `if(WIN32)`, `if(XRT_HAVE_VULKAN)`, `if(XRT_HAVE_D3D11)`
- **C/C++:** `#ifdef XRT_OS_WINDOWS`, `#ifdef XRT_OS_ANDROID`

### Environment Variable Gates
Runtime driver selection without recompilation:
- `FORCE_SIM_DISPLAY=1` — Force simulation display over vendor drivers
- `SIM_DISPLAY_OUTPUT=sbs|anaglyph|blend` — Select output mode
- `OXR_ENABLE_D3D11_NATIVE_COMPOSITOR=1` — Enable D3D11 compositor

## Extending the Project

### Adding a New Display Vendor

A vendor ships its integration as a **plug-in DLL from its own repo** (ADR-019) — there are
**zero changes to compositor or runtime code**. The plug-in implements the
`xrt_display_processor*` vtables (one per graphics API) plus an `xrt_device` with display specs,
exposes the `xrtPluginNegotiate` entry point, and registers for discovery (registry on Windows /
JSON manifest on POSIX). The compositor instantiates the discovered DP through the generic
interface and never sees vendor code.

Full walkthrough: [`docs/guides/vendor-plugin-onboarding.md`](../guides/vendor-plugin-onboarding.md)
→ [`docs/reference/xrt_plugin_iface.md`](../reference/xrt_plugin_iface.md) +
[`docs/specs/runtime/plugin-discovery.md`](../specs/runtime/plugin-discovery.md). The
`src/xrt/drivers/sim_display/` plug-in in this repo is the vendor-neutral reference to fork from.

### Adding a New OS Platform

Platform coupling by layer:

| Layer | Platform Dependency |
|-------|-------------------|
| `xrt_display_processor.h` | None (int32_t aliases for Vulkan types) |
| `xrt_display_processor_d3d11.h` | Windows only (void* for D3D types) |
| `sim_display_device.c` | None — fully portable |
| `sim_display_processor.c` | Vulkan only (any Vulkan-capable OS) |
| vendor plug-in device (per-vendor repo) | typically `#ifdef XRT_OS_WINDOWS` / `XRT_OS_ANDROID` for the vendor SDK, fallback defaults otherwise |

To add Linux support for a new display:
- The Vulkan display processor path works unchanged (Vulkan is cross-platform)
- Only the vendor plug-in needs platform-specific code for SDK integration
- `sim_display` already works on any Vulkan-capable OS for testing
