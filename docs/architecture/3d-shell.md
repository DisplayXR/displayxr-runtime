---
status: Proposal
owner: David Fattal
updated: 2026-03-15
issues: [44]
code-paths: [src/xrt/compositor/multi/, src/xrt/targets/]
---

# 3D Shell: Spatial Window Manager for Lightfield Displays

## Vision

A shell application that manages windows in 3D space on a lightfield display -- the equivalent of a desktop environment / window manager, but for spatial computing on a flat display. Not a new OS, but a layer on top of Windows/macOS.

**Platform-specific work:**
- #60 (Windows) -- D3D11 shell app, Win32 window management
- #61 (macOS) -- Metal shell app, Cocoa window management

## Architecture

```
+---------------------------------------------+
|  Shell App (always running, owns the scene)  |
|  - 3D scene graph of window positions        |
|  - Eye-tracked gaze cursor                   |
|  - Window chrome (grab, resize, depth)       |
|  - Taskbar / app launcher                    |
+--------------+------------------------------+
               | Privileged IPC
               v
+---------------------------------------------+
|  OpenXR Runtime (service mode)               |
|  - Multi compositor (#43)                    |
|  - Per-app native compositors                |
|  - Eye tracking -> Kooima projection         |
|  - Gaze hit-test against window quads        |
+--------------+------------------------------+
               | IPC (existing)
    +----------+----------+
    v          v          v
  App A      App B      App C
 (D3D11)    (WebXR)     (GL)
```

## Two Window Types

### OpenXR-aware apps -- full native path
- Submit stereo frames via OpenXR swapchain
- Per-app native compositor exports shared texture
- Proper stereo rendering, per-window Kooima projection
- App can optionally request placement via `XR_EXT_spatial_window`

### Non-OpenXR apps -- captured as 2D panels
- OS window capture (DXGI Desktop Duplication on Windows, ScreenCaptureKit on macOS)
- Composited as flat 2D quads positioned in 3D space
- Still benefit from depth placement and eye-tracked parallax of the panel itself

## Shell Responsibilities

| Feature | Description |
|---|---|
| **Window placement** | 3D transform per window -- position, rotation, scale, depth from display plane |
| **Gaze interaction** | Eye tracking -> gaze ray -> hit test against window quads -> focus/hover |
| **Window chrome** | Title bar, grab handles, depth slider, close/minimize -- rendered by shell |
| **Focus & input routing** | Gaze dwell or click to focus -> keyboard/mouse routed to focused window |
| **Layout presets** | "Theater" (single window, curved), "Desktop" (tiled grid at varying depths), "Free" (manual) |
| **Persistence** | Remember per-app window positions across sessions |
| **App launcher** | Taskbar or spatial dock for launching/switching apps |

## Shell App Structure

Standalone app (separate repo, like the Unity plugin). Built on top of the runtime:

```
shell/
  shell_app.cpp          -- main loop, privileged OpenXR session
  shell_scene.cpp        -- 3D scene graph of window quads
  shell_input.cpp        -- eye tracking, gaze ray, keyboard/mouse routing
  shell_chrome.cpp       -- window decorations, grab handles, depth slider
  shell_layout.cpp       -- layout engine (tiled, free, theater presets)
  shell_capture.cpp      -- OS window capture for non-OpenXR apps
  shell_ipc.cpp          -- privileged IPC to runtime
```

## Runtime-Side Requirements

The runtime needs to support the shell via:

1. **Multi compositor** (#43) -- composites all windows into the final 3D scene
2. **Privileged IPC channel** -- shell sends window transforms (position, rotation, scale), z-order, visibility
3. **Gaze hit-test API** -- runtime knows 3D positions of all window quads, returns which window the gaze ray intersects (and where)
4. **Per-window Kooima projection** -- each window gets its own asymmetric frustum based on its 3D position relative to the display and viewer's eyes
5. **Custom OpenXR extension** -- `XR_EXT_spatial_window_management` for the shell to runtime protocol

## Analogy

The split mirrors the Linux desktop stack:
- **Runtime** = Wayland compositor (handles rendering, input routing, display output)
- **Shell** = GNOME Shell / KDE Plasma (handles UX, window chrome, layout, app launcher)

Apps don't need to know about the shell. They submit frames; the runtime + shell handle placement and compositing.

## Prerequisites

- [ ] Platform-native multi compositor (#43)
- [ ] All native compositors (#39, #40, #41, #42)
- [ ] IPC/service mode functional
- [ ] Eye tracking pipeline working end-to-end

## Open Questions

- Should the shell be a separate repo (like the Unity plugin) or live in this repo?
- What toolkit for the shell's own UI? (ImGui? Native? Web-based?)
- How does the shell handle multi-monitor? (one lightfield display + conventional monitors)
- Collaboration: multiple users looking at the same display with independent eye tracking?
- Should non-OpenXR window capture be a separate module/plugin for clean separation?
