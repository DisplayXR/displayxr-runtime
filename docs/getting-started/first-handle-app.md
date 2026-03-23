# Tutorial: Your First Handle App

> **Status: TODO** — This tutorial is a skeleton. Tracking issue: [#97](https://github.com/dfattal/openxr-3d-display/issues/97)

This tutorial walks through building a `_handle` app that creates its own window and passes it to DisplayXR for stereo rendering. We'll reference the `cube_handle_metal_macos` and `cube_handle_d3d11_win` test apps as annotated examples.

## Prerequisites

- DisplayXR built and working (see [Building](building.md))
- Familiarity with OpenXR basics (instances, sessions, swapchains)

## Step 1: Create Your Window

<!-- TODO: Show platform-specific window creation (NSView with CAMetalLayer for macOS, HWND for Windows) -->

## Step 2: Enable DisplayXR Extensions

<!-- TODO: Show xrCreateInstance with XR_EXT_display_info and XR_EXT_*_window_binding enabled -->

## Step 3: Query Display Info

<!-- TODO: Show xrGetDisplayPropertiesEXT, rendering mode enumeration -->

## Step 4: Create Session with Window Binding

<!-- TODO: Show XrSessionCreateInfo with window binding next chain -->

## Step 5: Render Loop

<!-- TODO: Show xrBeginFrame/xrEndFrame with multiview atlas rendering -->

## Step 6: Handle Mode Switching

<!-- TODO: Show XrEventDataRenderingModeChangedEXT handling -->

## Reference Code

The complete test apps serve as working examples:
- **macOS/Metal**: `test_apps/cube_handle_metal_macos/`
- **macOS/Vulkan**: `test_apps/cube_handle_vk_macos/`
- **macOS/OpenGL**: `test_apps/cube_handle_gl_macos/`
- **Windows/D3D11**: `test_apps/cube_handle_d3d11_win/`
- **Windows/D3D12**: `test_apps/cube_handle_d3d12_win/`
- **Windows/Vulkan**: `test_apps/cube_handle_vk_win/`
- **Windows/OpenGL**: `test_apps/cube_handle_gl_win/`
