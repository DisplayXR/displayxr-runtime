---
status: Proposal
owner: David Fattal
updated: 2026-03-15
issues: [44]
code-paths: [src/xrt/compositor/multi/, src/xrt/targets/]
---

# 3D Shell: Spatial Window Manager for Lightfield Displays

## Scope and Related Docs

This doc covers the **window manager / policy layer** — placement, interaction, chrome. It depends on the multi-compositor pipeline but does not define it.

| Doc | Relationship |
|-----|-------------|
| [spatial-os.md](spatial-os.md) (#43) | **Compositing mechanism.** Defines the multi-compositor pipeline this shell drives. This doc depends on it. |
| [multi-display-single-machine.md](multi-display-single-machine.md) (#69) | **Multi-display extension.** Extends shell + compositor to multiple local displays. Depends on #43 + #44. |
| [multi-display-networked.md](multi-display-networked.md) (#70) | **Networked extension.** Extends multi-display to remote machines. Depends on #69. |

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
|  - Mouse-ray + depth-matched cursor           |
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
|  - Mouse-ray hit-test against window quads   |
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
| **Mouse-ray interaction** | Cyclopean eye → cursor-on-display ray → hit test against window quads → focus/hover/drag |
| **Depth-matched cursor** | 3D cursor renders at the Z-depth of the hovered window, giving spatial feedback |
| **Window chrome** | Title bar, grab handles, depth slider, close/minimize -- rendered by shell |
| **Focus & input routing** | Click-to-focus via mouse ray hit → keyboard/mouse routed to focused window |
| **Depth adjustment** | Scroll wheel or modifier+drag to push/pull windows along Z axis |
| **Layout presets** | "Theater" (single window, curved), "Desktop" (tiled grid at varying depths), "Free" (manual) |
| **Persistence** | Remember per-app window positions across sessions |
| **App launcher** | Taskbar or spatial dock for launching/switching apps |

## Interaction Model

### Mouse-Ray Primary

The shell uses **mouse raycasting** as the primary interaction mechanism:

1. **Ray construction**: From the cyclopean eye position (midpoint of left/right tracked eyes) through the 2D mouse cursor position on the physical display plane
2. **Hit testing**: Ray intersects window quads in 3D space; closest hit = target window
3. **Depth-matched cursor**: The cursor renders at the Z-depth of the hovered window, providing visual feedback about which depth layer is targeted

Without eye tracking, the ray falls back to perpendicular projection from the display surface (standard 3D editor behavior).

### Interaction Primitives

| Action | Mechanism |
|---|---|
| **Select/focus** | Click -- ray hits window quad, that window gets focus |
| **Drag/move** | Click+drag on title bar -- translate window in XY at its current depth |
| **Depth adjust** | Scroll wheel or modifier+drag -- push/pull window along Z |
| **Resize** | Drag edge/corner handles -- resize in the window's local plane |
| **Hover** | Mouse-over highlight -- ray hit without click |

### Gaze as Secondary

Eye tracking serves two distinct roles:
- **Kooima projection** (always active): Eye positions drive per-window asymmetric frustum for the 3D effect. This is core to the display, not an interaction mechanism.
- **Optional interaction assist**: When windows overlap, gaze can bias hit-testing (the window you're looking at wins ties). Mouse click is always authoritative.

## Shell App Structure

Standalone app (separate repo, like the Unity plugin). Built on top of the runtime:

```
shell/
  shell_app.cpp          -- main loop, privileged OpenXR session
  shell_scene.cpp        -- 3D scene graph of window quads
  shell_input.cpp        -- mouse ray, eye tracking, keyboard/mouse routing
  shell_chrome.cpp       -- window decorations, grab handles, depth slider
  shell_layout.cpp       -- layout engine (tiled, free, theater presets)
  shell_capture.cpp      -- OS window capture for non-OpenXR apps
  shell_ipc.cpp          -- privileged IPC to runtime
```

## Runtime-Side Requirements

The runtime needs to support the shell via:

1. **Multi compositor** (#43) -- composites all windows into the final 3D scene
2. **Privileged IPC channel** -- shell sends window transforms (position, rotation, scale), z-order, visibility
3. **Mouse-ray hit-test API** -- runtime knows 3D positions of all window quads, returns which window the mouse ray (cyclopean eye through cursor on display) intersects (and where)
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
