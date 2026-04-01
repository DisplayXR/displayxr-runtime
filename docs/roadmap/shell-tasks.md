---
status: Active
owner: David Fattal
updated: 2026-03-28
issues: [43, 44, 49, 58, 60]
platform: Windows (D3D11 first, macOS deferred)
---

# 3D Shell Implementation Tasks

Living task tracker for the spatial shell. For architecture and design rationale, see [spatial-os.md](spatial-os.md) (#43), [3d-shell.md](3d-shell.md) (#44), and [shell-runtime-contract.md](shell-runtime-contract.md).

### Key Architecture Decisions (ADRs)

- **ADR-012**: [Window-relative Kooima projection](../adr/ADR-012-window-relative-kooima-projection.md) — apps use window dims as screen, eyes offset to window center
- **ADR-013**: [Universal app launch model](../adr/ADR-013-universal-app-launch-model.md) — hidden HWND proxy: one binary, standalone (own HWND) or shell (HWND hidden, shell puppets via SetWindowPos/PostMessage). No new extensions needed.

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

### Repo boundary

The multi compositor is **runtime infrastructure**, not shell code. It lives in `displayxr-runtime` (public) because:
- It's the server-side IPC component — any IPC app (cube_ipc, Chrome WebXR) needs it
- It extends `d3d11_service` and is tightly coupled to `xrt_compositor.h`, `proto.json`, the IPC server, and the DP vtable
- It's pure mechanism: shared texture import, Kooima projection, display processing

The shell is a privileged IPC client that sends window poses and receives hit-test results — it never touches the compositor directly.

| Repo | Content | Principle |
|------|---------|-----------|
| `displayxr-runtime` (public) | Multi compositor, IPC protocol, hit-test API, shared texture import | Mechanism |
| `displayxr-shell` (private) | Scene graph, window chrome, launcher, layout presets, persistence | Policy / UX |

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

**Test:** `displayxr-shell.exe app1.exe app2.exe` → drag windows with mouse, correct per-window parallax.

**Status:** Done (branch `feature/shell-phase1-ci`). See `shell-phase1-status.md` for full details.

| | Task | Size | Repo | Description |
|---|------|------|------|-------------|
| [x] | 1.1 Shell IPC protocol | M | runtime | `shell_set_window_pose(client_id, xrt_pose, width_m, height_m)` and `shell_activate` in `proto.json`. |
| [x] | 1.2 Dynamic window poses | M | runtime | Full 3D `xrt_pose` per slot. `slot_pose_to_pixel_rect()` converts to pixel coords. Blit uses per-slot rects. |
| [x] | 1.3 Per-client eye transform | M | runtime | `comp_d3d11_service_get_client_window_metrics()` returns per-client window size + center offset from pose. `ipc_try_get_sr_view_poses` uses per-client metrics for Kooima. |
| [x] | 1.4 Click-to-focus hit-test | M | runtime | Left-click polls cursor, tests against slot rects, sets focused_slot. Mouse coords remapped shell→app. |
| [x] | 1.5 Shell app | L | shell | `displayxr-shell.exe` in `src/xrt/targets/shell/`. Auto-starts service, sends `shell_activate`, launches apps with env vars, polls clients. |
| [x] | 1.6 App connect/disconnect | M | runtime+shell | Shell polls `system_get_clients`, detects changes, prints status. Server-side multi-client focus override. |
| [x] | 1.7 Window drag + resize | M | runtime | Right-click-drag = translate in display plane (server-side state machine). Scroll wheel = resize focused window (~5%/notch). |
| [x] | 1.8 Default placement | S | runtime | Pose-based: slot 0 left-upper, slot 1 right-upper, 40% of display. `--pose` CLI args for custom layout. |
| [x] | 1.9 Z-order by focus | S | runtime | Focused slot rendered last (on top). `render_order[]` array. |
| [x] | 1.10 Shell revival | S | runtime | ESC dismisses → `window_dismissed`. New client → recreate window. Service survives across dismiss/reopen cycles. |
| [ ] | 1.A IPC-to-standalone hot-switch | L | runtime | Deferred. When shell exits, apps switch from IPC to standalone. Complex compositor swap. |

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
