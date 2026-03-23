# DisplayXR Overview

An open-source [OpenXR](https://www.khronos.org/openxr/) runtime for glasses-free 3D displays — autostereoscopic monitors and laptops that deliver head-tracked stereoscopic 3D without worn hardware.

## Why DisplayXR?

3D display vendors each ship their own SDK. Apps built against one vendor's SDK don't work on another's hardware. DisplayXR solves this by providing a single, vendor-neutral OpenXR runtime that works across all 3D displays:

- **Future-proof** — write once, run on any supported 3D display
- **Standards-based** — OpenXR is the Khronos industry standard for XR
- **No interop overhead** — native compositors for every graphics API (D3D11, D3D12, Metal, OpenGL, Vulkan)

## Architecture

```
App (any graphics API)
        |
   OpenXR State Tracker
        |
   Core xrt interfaces
        |
   +----+-----+--------+--------+
   |    |     |        |        |
 D3D11 D3D12 Vulkan  Metal   OpenGL   <-- native compositors
   |    |     |        |        |
   Display Processor (vendor-specific: LeiaSR / sim_display)
        |
   Display
```

Each graphics API gets its own native compositor — no Vulkan intermediary, no format conversion, no GPU compatibility surprises. The display processor layer is where vendor-specific code lives (interlacing, lenticular weaving, etc.), cleanly separated from the rest of the stack.

## Compositor Status

| API | Windows | macOS |
|-----|---------|-------|
| D3D11 | Shipping | — |
| D3D12 | Shipping | — |
| Metal | — | Shipping |
| OpenGL | Shipping | Shipping |
| Vulkan | Shipping | Shipping |

## OpenXR Extensions

DisplayXR extends OpenXR with custom extensions for 3D display support:

| Extension | Purpose |
|-----------|---------|
| `XR_EXT_display_info` | Display geometry, eye tracking, rendering mode enumeration and switching |
| `XR_EXT_win32_window_binding` | App provides its own Win32 HWND for rendering |
| `XR_EXT_cocoa_window_binding` | App provides a Cocoa NSView for rendering (macOS) |
| `XR_EXT_android_surface_binding` | App provides an Android Surface for rendering |

See the [formal extension specifications](../specs/) for full details.

## Simulation Driver

You don't need a 3D display to develop against DisplayXR. The **sim_display** driver provides a simulated tracked 3D display with keyboard-controlled eye position, letting you test the full OpenXR pipeline on any machine:

```bash
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./build/test_apps/cube_handle_vk_macos/cube_handle_vk_macos
```

Use WASD + mouse to move the simulated eye position and observe perspective-correct stereo rendering.

## Next Steps

- [Building DisplayXR](building.md) — build instructions for Windows and macOS
- [App Classes](app-classes.md) — understand the four ways an app can integrate with DisplayXR
- [Your First Handle App](first-handle-app.md) — tutorial walkthrough
