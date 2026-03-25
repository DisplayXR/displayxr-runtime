---
status: Active
owner: David Fattal
updated: 2026-03-24
issues: [43, 44, 49, 58, 60]
platform: Windows (D3D11 first, macOS deferred)
---

# 3D Shell Implementation Tasks

Living task tracker for the spatial shell. For architecture and design rationale, see [spatial-os.md](spatial-os.md) (#43), [3d-shell.md](3d-shell.md) (#44), and [shell-runtime-contract.md](shell-runtime-contract.md).

## Architecture

```
App A (D3D11)     App B (VK)      App C (GL)
     |                |               |
  D3D11 comp       VK comp         GL comp       (native, in-process)
     |                |               |
  IPC client       IPC client      IPC client    (existing)
     |                |               |
     +-------+--------+------+-------+
             |               |
       IPC transport (named pipes)    (existing)
             |
    D3D11 Multi Compositor (NEW, server-side)
      - imports shared textures from all clients
      - renders window quads with Level 2 Kooima
      - calls vendor DP (LeiaSR weaver)
      - presents to display (full-screen)
```

### Native compositor split

Per-client compositors keep the app-facing half (swapchain management, layer compositing, atlas crop) but lose the display-facing half (DP call, present). They export L/R as DXGI shared textures instead. The D3D11 multi compositor takes over DP + present.

| Per-client compositor (keeps) | D3D11 Multi compositor (new) |
|-------------------------------|------------------------------|
| `xrCreateSwapchain` | Import N shared textures |
| `xrAcquireSwapchainImage` | Render window quads per-eye |
| Layer compositing (atlas crop) | Level 2 Kooima projection |
| Export L/R shared textures | Call vendor DP (weave) |
| | Present to display (full-screen) |

Single-app fast path (direct in-process) is preserved — no regression.

### Kooima simplification

Multi compositor window is always full-screen = physical display size:
- **Level 2** (multi compositor): raw tracked eye positions, physical display corners — no transform needed
- **Level 1** (per-app): `eye_in_window = inverse(window_pose) * eye_in_display` — the only transform

### Mode switching and DP ownership

- Multi compositor owns the single vendor DP instance and handles mode switching (V/1/2/3 keys)
- Mode changes propagate from multi compositor → per-client compositors → apps (`XrEventDataRenderingModeChangedEXT`)
- Eye tracking: multi compositor gets raw positions from DP, transforms per-window for each client

---

## Phase 0: Two Apps in One Window

**Goal:** Two IPC apps rendered as textured quads in a single output window. Hardcoded layout, no shell app.

**Test:** `displayxr-service` + 2× `cube_ipc_d3d11_win` → both cubes visible as quads in one window.

| | Task | Size | Repo | Description |
|---|------|------|------|-------------|
| [ ] | 0.1 Per-client shared texture export | M | runtime | Modify `d3d11_service_compositor` to export L/R as DXGI shared handles after layer compositing (instead of calling DP directly). Each per-client compositor renders its atlas, then exports two shared textures. |
| [ ] | 0.2 D3D11 multi compositor | L | runtime | New composition pass: imports shared textures from N clients, renders each as a textured quad with Level 2 Kooima projection (eye → physical display corners), sends combined L/R to display processor. |
| [ ] | 0.3 Plumb into service startup | M | runtime | Wire multi compositor into `d3d11_service_system`. Per-client compositors become texture exporters. Multi compositor owns the single output window + weaver. |
| [ ] | 0.4 Hardcoded two-window layout | S | runtime | Place window 0 at `(x=-0.05, y=0, z=0)` and window 1 at `(x=+0.05, y=0, z=0)`. Verifies end-to-end pipeline. |
| [ ] | 0.5 Two-app test | S | runtime | Start service, launch two `cube_ipc_d3d11_win` instances, verify both cubes visible. |

**Dependencies:** 0.1 → 0.2 → 0.3 → 0.4 → 0.5

**Key files:**
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — starting point
- `src/xrt/compositor/multi/comp_multi_system.c` — reference for Vulkan multi compositor patterns
- `src/xrt/include/xrt/xrt_compositor.h` — `xrt_multi_compositor_control` vtable

---

## Phase 1: Basic Spatial Shell

**Goal:** A shell app connects via privileged IPC. Mouse-drag windows in 3D space. Two OpenXR apps running in spatially rearrangeable windows.

**Test:** service + shell + 2× `cube_ipc_d3d11_win` → drag windows with mouse, correct per-window parallax.

| | Task | Size | Repo | Description |
|---|------|------|------|-------------|
| [ ] | 1.1 Shell IPC protocol | M | runtime | Add to `proto.json`: `shell_set_window_pose`, `shell_get_windows`, `shell_set_visibility`, `shell_set_focus`, `shell_hit_test`. Mark as privileged. |
| [ ] | 1.2 Dynamic window poses | M | runtime | Multi compositor stores per-client pose + scale. IPC handler updates them. Render loop uses poses for quad placement. |
| [ ] | 1.3 Level 1 eye transform | M | runtime | Transform eye positions into each window's local frame: `eye_in_window = inverse(window_pose) * eye_in_display`. Provide to per-client compositors for app Kooima projection. |
| [ ] | 1.4 Mouse-ray hit-test | M | runtime | Shell sends 2D cursor coords. Runtime constructs ray from cyclopean eye through cursor, intersects window quads, returns closest hit (client_id, UV, 3D point). |
| [ ] | 1.5 Shell app skeleton | L | shell | Minimal C++ app in `src/xrt/targets/shell/`. Connects as privileged IPC client. Main loop: poll mouse, call hit-test, send window pose updates. |
| [ ] | 1.6 App connect/disconnect | M | runtime | Runtime notifies shell when IPC clients create/destroy sessions. Shell learns about existing apps on connect. |
| [ ] | 1.7 Window drag | M | shell | Click on quad + drag = translate in XY at current depth. Scroll wheel = adjust Z. Shell sends updated pose each frame during drag. |
| [ ] | 1.8 Default placement | S | shell | New apps cascade: center of display with slight X/Z offset based on window count. |

**Dependencies:** 1.1–1.4 parallel (runtime), 1.5 depends on 1.1, 1.7 depends on 1.4+1.5

---

## Phase 2: Usable Shell

**Goal:** Window chrome, focus/input routing, layout presets, persistence.

**Test:** Multiple apps with title bars, click-to-focus, type in focused app, layout presets.

| | Task | Size | Repo | Description |
|---|------|------|------|-------------|
| [ ] | 2.1 Window chrome | L | shell+runtime | Title bars, grab handles, close/minimize as textured quads attached to each window. Simple sprites/text rendering. |
| [ ] | 2.2 Input routing | L | runtime | Mouse/keyboard forwarded to focused app via IPC. Shell controls focus via `shell_set_focus`. |
| [ ] | 2.3 Depth slider | M | shell | Drag widget on chrome to move window along Z axis. Visual feedback shows depth relative to display plane. |
| [ ] | 2.4 Close/minimize/maximize | M | shell+runtime | Close sends session destroy. Minimize = `shell_set_visibility(false)`. Maximize scales to fill display. |
| [ ] | 2.5 Window resize | M | shell+runtime | Drag corner/edge handles to change scale factor. (Full swapchain resize deferred — scale texture for now.) |
| [ ] | 2.6 Layout presets | M | shell | Desktop (tiled grid), Theater (one large, others minimized), Free (manual). Apply = batch pose update. |
| [ ] | 2.7 Persistence | M | shell | JSON config: per-app window pose, scale, layout. Load on start, save on change. Keyed by app name. |
| [ ] | 2.8 Depth-matched cursor | M | runtime | Render cursor quad at the 3D hit point of hovered window. Visual feedback for depth targeting. |

---

## Phase 3: App Launching

**Goal:** Launch OpenXR apps from within the shell.

**Test:** Launch `cube_ipc_d3d11_win` from launcher panel, switch via taskbar.

| | Task | Size | Repo | Description |
|---|------|------|------|-------------|
| [ ] | 3.1 Process launcher | M | shell | Spawn child processes with `XR_RUNTIME_JSON` env var pointing to running service. Child auto-connects via IPC. |
| [ ] | 3.2 Registered apps config | S | shell | JSON file: `[{name, exe_path, icon_path, category}]`. Shell reads on startup. Pre-populated with demo apps. |
| [ ] | 3.3 Launcher panel | L | shell | 3D panel showing app grid as textured quads. Click to launch. Summoned by hotkey or button. First-run default view. |
| [ ] | 3.4 Taskbar | M | shell | Persistent bar showing running apps. Click to focus/bring-to-front. Right-click for close/minimize. |
| [ ] | 3.5 Auto-start shell | S | runtime | `displayxr-service` launches `displayxr-shell` on startup. Shell exit doesn't kill service. |
| [ ] | 3.6 File browser | M | shell | Browse filesystem, double-click `.exe` to launch as OpenXR app. Fallback for unregistered apps. |
| [ ] | 3.7 CLI launch | S | shell | `displayxr-shell --launch "path/to/app.exe" [args]` for scripting and development. |

**First-run experience:** Launcher panel with pre-registered demo apps (cube_handle, gaussian_splatting) + "Browse for app..." button.

---

## Phase 4: 2D App Capture

**Goal:** Non-OpenXR OS windows displayed as 2D panels in 3D space.

**Test:** Notepad or Chrome window as 2D panel alongside OpenXR 3D apps.

| | Task | Size | Repo | Description |
|---|------|------|------|-------------|
| [ ] | 4.1 DXGI capture | L | shell/runtime | Capture OS window content as D3D11 texture via `IDXGIOutputDuplication` or `Windows.Graphics.Capture`. |
| [ ] | 4.2 Virtual client | M | runtime | Captured texture rendered as a quad in multi compositor — a "virtual client" with no OpenXR session. |
| [ ] | 4.3 Window picker | M | shell | UI showing list of OS windows. Select to capture and display in 3D scene. |
| [ ] | 4.4 Input forwarding | L | shell | Forward mouse/keyboard to original OS window via `SendMessage`/`SendInput` when focused. |

---

## Phase 5: Polish & macOS

**Goal:** macOS parity, performance, multi-display groundwork.

**Test:** Same scenarios on macOS; 8+ windows at < 1ms compositor overhead.

| | Task | Size | Repo | Description |
|---|------|------|------|-------------|
| [ ] | 5.1 Metal multi compositor | L | runtime | Port D3D11 multi compositor to Metal. IOSurface import. Same two-level Kooima pipeline. |
| [ ] | 5.2 macOS shell app | L | shell | Port shell to macOS. Metal rendering, Cocoa event handling. |
| [ ] | 5.3 Performance | M | runtime | Profile multi compositor. Optimize quad rendering (instanced draw, minimal state changes). |
| [ ] | 5.4 Window overlap | M | runtime | Depth sorting, transparency at edges, anti-aliased quad borders. |
| [ ] | 5.5 3D capture | M | runtime | Hook capture pipeline into multi compositor output (before DP). See [3d-capture.md](3d-capture.md). |

---

## Issue Cross-Reference

| Issue | Phase | Description |
|-------|-------|-------------|
| #43 | 0, 1 | Spatial OS — multi compositor architecture |
| #44 | 1, 2, 3 | 3D Shell — spatial window manager |
| #49 | 0 | D3D11 service compositor (extend for multi-client) |
| #58 | 0 | D3D11 multi compositor (Windows) |
| #60 | 1, 2, 3 | D3D11 shell app (Windows) |
| #48 | 5 | Metal service compositor (macOS) |
| #59 | 5 | Metal multi compositor (macOS) |
| #61 | 5 | Metal shell app (macOS) |
| #69 | 5 | Multi-display single machine |
