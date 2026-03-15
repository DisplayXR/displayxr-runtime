---
status: Proposal
owner: David Fattal
updated: 2026-03-15
issues: [43]
code-paths: [src/xrt/compositor/multi/, src/xrt/ipc/]
---

# Spatial OS: Platform-Native Multi Compositor for 3D Window Management

## Vision

A "spatial OS" concept where multiple OpenXR apps using different graphics APIs can be composed into a single 3D scene -- windows positioned and oriented in 3D space on a lightfield display.

**Platform-specific work:**
- #58 (Windows) -- D3D11 multi compositor, DXGI shared textures
- #59 (macOS) -- Metal multi compositor, IOSurface shared textures

## Architecture

```
App A (D3D11)    App B (GL)    App C (VK)    App D (Metal)
     |               |              |              |
  D3D11 comp      GL comp       VK comp       Metal comp
  (per-app)      (per-app)     (per-app)      (per-app)
     |               |              |              |
  Shared tex      Shared tex    Shared tex    Shared tex
  (DXGI handle)  (DXGI handle) (DXGI handle) (IOSurface)
     |               |              |              |
     +-------+-------+------+------+------+-------+
             |                              |
   Multi Compositor (D3D11)      Multi Compositor (Metal)
    [spatial layout + weave]      [spatial layout + weave]
             |                              |
          Display                        Display
```

## How Native Compositors Feed Into This

The native compositors become the **per-app client compositors**. They split into two roles:

**Per-app compositor (the native compositors we have):**
- Accept app-submitted textures via swapchain -- keep as-is
- Do per-app layer compositing (overlays, HUD) -- keep as-is
- Export final composited image as a shared texture -- small addition
- Export L and R as **separate** shared textures (not pre-composited SBS)
- Do NOT weave or present (that moves to multi compositor)

**Multi compositor (new, one per display):**
- Imports shared textures from all active per-app compositors
- Spatial layout -- positions/orients windows in 3D space
- Two-level Kooima rendering pipeline (see below)
- Sends combined scene to weaver (display_processor)
- Presents to display

The `xrt_display_processor` abstraction already separates weaving from compositing, so the split point is clean:

| Component | Single-app (today) | Multi-app (spatial OS) |
|---|---|---|
| Swapchain management | Native compositor | Native compositor (same) |
| Layer compositing | Native compositor | Native compositor (same) |
| Shared texture export | N/A | Native compositor (add) |
| Spatial 3D layout | N/A | Multi compositor (new) |
| Weaver integration | Native compositor | Multi compositor (moves here) |
| Window/present | Native compositor | Multi compositor (moves here) |

In single-app mode, the native compositor still goes straight to the weaver as it does today -- no regression. The multi-app path inserts the spatial layout step between per-app compositing and weaving.

## Two-Level Kooima Rendering Pipeline

Windows in 3D space are rectangles with a 3D pose (position + orientation) relative to the physical display. A window can be slanted, tilted, or offset in depth. The rendering pipeline must handle this correctly so that:
- Content within each window has correct depth/parallax (app-rendered)
- Windows themselves have correct parallax relative to each other and the display (multi compositor)
- The weaver receives standard L/R images in physical display pixel space

### Level 1: Per-app rendering (inside each app's native compositor)

Each app renders stereo content with Kooima projection, but the "screen" is its **virtual window quad**, not the physical display. The runtime transforms eye positions into the window's local coordinate frame:

```
eye_in_window = inverse(window_pose) * eye_in_display
```

The app doesn't know it's on a slanted window -- it sees eye positions relative to "its screen" and renders with Kooima as usual. This produces correct depth/parallax within the window's content.

Output: L and R textures exported as separate shared textures.

### Level 2: Multi compositor compositing (per physical eye)

For each physical eye position:
1. Set up a Kooima projection from that eye to the **physical display corners**
2. Render all virtual windows as **textured 3D quads** in this projection
3. For the left-eye pass, texture each window quad with that app's **left** texture; same for right
4. Output: a 2D image in physical display pixel space for that eye

This is standard 3D rendering -- textured quads with perspective. The window's slant naturally produces correct parallax: a window tilted away has its far edge compressed, near edge expanded.

### Level 3: Weave normally

The weaver gets L/R images already in physical display space. It doesn't know about windows. Standard interlacing -- no changes needed.

```
  Per-app (Level 1):
  +----------+  eye_in_window = inv(window_pose) * eye_in_display
  |  App A   |  Kooima(eye_in_window, window_corners)
  |  renders |  -> L/R textures on virtual window plane
  +----+-----+
       | L + R shared textures
       v
  Multi compositor (Level 2):
  +----------------------------+
  | For each physical eye:     |
  |  Kooima(eye, display)      |
  |  Render window quads       | <- textured with L or R from Level 1
  |  as 3D geometry            |
  +----+-----------------------+
       | L/R in physical display pixel space
       v
  Weaver (Level 3):
  Standard interlace -> Display
```

### Window Types and Their Rendering

| Window type | Level 1 (per-app) | Level 2 (multi comp) |
|---|---|---|
| **OpenXR 3D app** | Kooima with eye_in_window -> L/R with depth | Textured quad in 3D -> display plane |
| **Captured 2D app** | N/A -- flat texture | Textured quad in 3D -> display plane |
| **Shell chrome** | N/A -- rendered by shell | Textured quad in 3D -> display plane |

2D captured windows still get parallax from their 3D position (the panel moves with head tracking) but the content itself is flat -- correct behavior for a 2D window floating at some depth.

### Subtleties

**Distortion at extreme slant angles**: The app rendered assuming the viewer looks through the window plane. When the multi compositor reprojects onto the physical display, there's an implicit assumption that the lenticular can reproduce the needed view angles. For small slants (+/-15-20 degrees) this works fine. At extreme angles (window nearly edge-on), the display physically can't produce enough angular variation -- hardware limitation. The shell (#44) should clamp maximum window slant.

**Performance**: Level 1 is done by each app (already happening today). Level 2 is cheap -- rendering N textured quads with perspective. The expensive parts (per-app 3D rendering and weaving) are unchanged.

## Why Platform-Native (Not Vulkan)

The current multi compositor (`compositor/multi/`) is deeply Vulkan-coupled (~24 VK API calls, VkImage/VkPipeline/VkRenderPass per session). This forces all APIs through Vulkan interop for final compositing -- the exact pain this project exists to eliminate.

**Platform-native approach:**
- **Windows**: D3D11 multi compositor backend. All APIs export DXGI shared handles. D3D11 imports trivially. LeiaSR D3D11 weaver is mature.
- **macOS**: Metal multi compositor backend. All APIs export IOSurfaces. Metal imports natively.

The shared texture infrastructure already exists:
- DXGI shared handles: #20 (D3D11), #21 (D3D12)
- IOSurface: #11 (Metal)
- GL shared textures: #37
- VK shared textures: #22

## Key Components to Build

1. **Shared texture export in native compositors** -- each per-app compositor exports L and R as separate shared textures (DXGI handle on Windows, IOSurface on macOS).
2. **Platform-native multi compositor** -- D3D11 on Windows, Metal on macOS. Imports shared textures, renders 3D spatial scene with Level 2 Kooima, sends to weaver.
3. **Eye position transform** -- runtime transforms raw eye positions into each window's local coordinate frame before providing to apps.
4. **Spatial layout engine** -- position/orient window quads in 3D space relative to the display surface.
5. **Input routing** -- gaze ray hit-test against window quads in 3D, route input to focused window.
6. **Window management protocol** -- OpenXR extension or IPC API for the shell (#44) to set window poses.
7. **IPC/service mode** -- already exists (`src/xrt/ipc/`, 39 files), allows multiple app processes to connect to a single runtime.

## Runtime-Side Changes Needed

| Component | Change |
|---|---|
| Eye tracking | Transform raw eyes into each window's local frame before providing to apps |
| Native compositors | Export L and R as separate shared textures (not pre-composited SBS) |
| Multi compositor | Render textured window quads per-eye with Kooima to physical display |
| Weaver | **No change** -- receives standard L/R display-plane images |
| Shell (#44) | Manages window poses, feeds transforms to multi compositor |

## Prerequisites

- [ ] All native compositors complete (#39, #40, #41, #42)
- [ ] Shared texture support for all APIs (#22)
- [ ] IPC/service mode preserved and functional

## Open Questions

- Should the multi compositor completely replace the existing Vulkan-based one, or coexist?
- What OpenXR extension for spatial window placement? (custom `XR_EXT_spatial_window`?)
- How to handle apps that don't know about spatial placement (default flat layout)?
- Per-window depth/parallax budget to avoid cross-talk between overlapping windows?
- Single-app fast path: skip multi compositor entirely for lowest latency?
- How to handle window overlap -- depth sorting, transparency, occlusion?
