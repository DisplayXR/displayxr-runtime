---
status: Draft
version: "0.2"
owner: David Fattal
updated: 2026-03-21
issues: [43, 44]
platforms: [Windows, macOS]
---

> **Status: Proposal** — not yet implemented. Tracking issue: [#43](https://github.com/dfattal/openxr-3d-display/issues/43), [#44](https://github.com/dfattal/openxr-3d-display/issues/44)

# Spatial Desktop Platform PRD

## Runtime + 3D Shell for Spatial Workstations

**Scope:** Single-display first, multi-display extension later

---

## 1. Purpose

This document defines the product requirements for a **Spatial Desktop Platform** built on top of an existing OS, not a new kernel-level operating system.

The platform is split into two major layers:

1. **Spatial Runtime**
   Responsible for per-app compositing, cross-app spatial compositing, display processing, and system-level 3D capture.

2. **3D Shell**
   Responsible for window management, layout, chrome, persistence, launcher behavior, and user-facing spatial interaction.

This split aligns with the current architecture direction:

- **Issue #43**: platform-native multi compositor for 3D window management
- **Issue #44**: 3D shell / spatial window manager built on top of that compositor

The goal is to create a desktop-class spatial computing environment for tracked 3D displays: run multiple 3D and 2D apps as spatial windows on one tracked display.

---

## 2. Product Vision

Today, 3D displays exist mostly as isolated endpoints with vendor-specific runtimes and limited desktop integration.

This platform changes that by providing:

- a **runtime** that can spatially compose multiple applications into one coherent 3D scene
- a **shell** that turns that scene into a usable spatial desktop

The v1 product is: **run multiple 3D and 2D apps as spatial windows on one tracked display.** That alone is the core value proposition.

Future extensions (Phase 2+) include 3D-native capture/dataset generation and multi-display unified workspaces, but these are not v1 requirements.

The platform should feel like the equivalent of a desktop environment for lightfield / tracked 3D displays.

---

## 3. Design Principles

### 3.1 Runtime owns mechanism
The runtime owns the hard rendering and display machinery:
- per-app compositors
- shared texture flow
- multi-compositor
- Kooima projection math
- weave / display processing
- capture pipeline
- low-level hit testing support

### 3.2 Shell owns policy and UX
The shell owns:
- window placement policy
- layout presets
- focus behavior
- interaction affordances
- persistence
- launcher / task switching
- capture UX triggers

### 3.3 Single-app fast path remains valid
Single-app rendering should continue to bypass unnecessary layers for minimum latency where appropriate.

### 3.4 Support both native 3D and legacy 2D apps
The platform must support:
- **OpenXR-aware apps** with stereo / spatial awareness
- **non-OpenXR apps** captured as 2D panels in 3D space

### 3.5 Platform first, shell second
The runtime must be useful even without the full shell:
- single-app mode
- kiosk mode
- OEM-specific shells
- future alternate shells

---

## 4. System Architecture

### High-level stack

```text
Apps
 ├─ OpenXR-aware 3D apps
 └─ Non-OpenXR 2D apps

Per-app compositors / capture path
 ├─ Native compositor per OpenXR app
 └─ OS window capture for 2D apps

Spatial Runtime
 ├─ Shared texture import/export
 ├─ Multi compositor
 ├─ Per-window projection / scene composition
 ├─ Hit testing support
 ├─ 3D capture pipeline
 └─ Display processor / weaver

3D Shell
 ├─ Spatial scene graph
 ├─ Window chrome
 ├─ Layout engine
 ├─ Focus / launcher / persistence
 └─ Capture UX

Physical 3D Display
```

### Detailed architecture docs

| Component | Doc |
|-----------|-----|
| Multi-compositor pipeline | [spatial-os.md](spatial-os.md) |
| Window manager / shell | [3d-shell.md](3d-shell.md) |
| Shell/runtime IPC contract | [shell-runtime-contract.md](shell-runtime-contract.md) |
| 3D capture pipeline | [3d-capture.md](3d-capture.md) |
| Multi-display (local) | [multi-display-single-machine.md](multi-display-single-machine.md) |
| Multi-display (networked) | [multi-display-networked.md](multi-display-networked.md) |

---

## 5. App Models

The platform supports two broad categories of apps:

**OpenXR-aware apps** use the native stereo path — submit stereo frames via OpenXR swapchains, export L/R as separate shared textures, get correct per-window Kooima projection. These map to the existing four app classes documented in `CLAUDE.md` (handle, texture, hosted, IPC).

**Non-OpenXR apps** are captured as 2D content panels through OS-native window capture (DXGI Desktop Duplication on Windows, ScreenCaptureKit on macOS). They are presented as flat textured quads in 3D space — the panel gets parallax from spatial placement, but content inside remains 2D.

### 5.1 Developer Integration Levels

The platform is designed so that **no SDK is required for basic spatial app support.** Developers adopt deeper integration incrementally:

| Level | What the developer does | What they get | SDK required? |
|-------|------------------------|---------------|---------------|
| **0 — Universal app** | Write a normal OpenXR handle app (`XR_EXT_win32_window_binding` + `XR_EXT_display_info`, own Kooima projection). No shell awareness. | App works standalone AND in the shell with zero code changes. Same binary, same rendering code. The shell launches it with an env var; the runtime handles routing. | **No** |
| **1 — Shell-aware app** | Adopt optional OpenXR extensions (e.g. `XR_EXT_spatial_window`) to request a specific window size/position, declare capabilities ("resizable", "supports depth"), respond to spatial input (3D pointer, gaze). | Deeper shell integration — app can negotiate its placement, receive 3D input events, advertise features to the shell. | **Small extension library** on top of OpenXR |
| **2 — Spatial UI app** | Use a spatial UI toolkit for 3D widgets, floating panels, and world-anchored UI elements. | Full spatial-native experience with 3D UI primitives, shared world anchors, spatial audio hooks. | **Spatial UI SDK** (future) |

**Key insight:** Level 0 is the foundation and is already implemented (ADR-013: universal app launch model). Any OpenXR handle app is a spatial app today. Levels 1 and 2 are optional enhancements — they add capabilities but are never required. This ensures the platform has immediate app compatibility with the existing OpenXR ecosystem.

### 5.2 Terminology: Spatial OS vs Shell

- **Shell** = the window manager + launcher. Manages window layout, focus, input routing, app lifecycle UX. A single component, replaceable (OEM shells, kiosk mode, alternate shells). Developed in a private repo, communicates with the runtime via the [shell-runtime contract](shell-runtime-contract.md).
- **Spatial OS** = the full platform stack: runtime + shell + extensions + capture pipeline. The product/platform name encompassing all components. Issues #43 (multi-compositor) and #44 (shell/spatial OS) track the two main subsystems.

The shell is the first visible deliverable of the spatial OS, but the spatial OS also includes the runtime's multi-compositor, the extension APIs, the capture pipeline, and future components like spatial audio and shared world models.

> **Terminology note:** "Spatial OS" is an internal vision label for the full platform stack. Externally, lead with **DisplayXR runtime** (open, developer-facing) and **3D Shell** (closed, user-facing). The term "OS" can sound overreaching since the platform is built on top of Windows/macOS, not a new kernel.

---

## 6. Future Direction: 3D Capture (Phase 2+)

3D capture enables native capture of spatial content before display-specific weaving, so output remains reusable as stereo content, spatial replay, or dataset material. This is a strategic capability but not a v1 requirement — the core shell must be working first.

**Full specification:** [3d-capture.md](3d-capture.md)

---

## 7. Core Feature: Cross-App Spatial Compositing (v1)

Cross-app spatial compositing allows multiple applications to coexist in a shared 3D scene on a tracked display, with correct spatial placement and coherent viewing behavior. This is the central runtime capability that makes the rest of the platform possible — and the v1 differentiator.

**Full specification:** [spatial-os.md](spatial-os.md)

---

## 8. Future Direction: Multi-Display Unified Space (Phase 3+)

Multi-display extends the single-display spatial desktop into a workspace where multiple tracked 3D displays participate in one coherent spatial environment. This is a later phase — the single-display shell must be solid first.

**Full specifications:**
- [multi-display-single-machine.md](multi-display-single-machine.md)
- [multi-display-networked.md](multi-display-networked.md)

---

## 9. Shell / Runtime Contract

The shell/runtime contract defines the IPC message set and boundary rules between the rendering runtime and the desktop shell, ensuring clean separation of mechanism from policy.

**Full specification:** [shell-runtime-contract.md](shell-runtime-contract.md)

---

## 10. Interaction Model

The 3D shell provides mouse/keyboard interaction, gaze-based hover, shell chrome manipulation, and app launcher/task switching. Each managed window exposes title bar, move/resize handles, depth control, and close/minimize affordances.

**Full specification:** [3d-shell.md](3d-shell.md)

---

## 11. Privacy, Safety, and Capture Policy

Because the platform can capture app content system-wide, capture features must be explicitly controlled.

### Requirements
- Capture is user-initiated by default
- Dataset mode is opt-in
- Shell surfaces clear state when capture is active
- Future enterprise policy controls may disable capture per app / per workspace

---

## 12. Roadmap

### Phase 1: Single-display platform foundation
- Platform-native multi compositor
- Shell scene graph
- Basic shell chrome
- OpenXR-aware app path
- Simple 2D panel capture path
- 3D screenshot

### Phase 2: Usable desktop shell
- Layout presets
- Persistence
- App launcher / task switching
- Recording
- Better input routing
- Single-app fast path optimization

### Phase 3: Spatial workstation expansion
- Session capture
- Dataset mode
- Multi-display registration
- Coordinated multi-display workspace

### Phase 4: Advanced ecosystem
- Alternate shells
- Collaborative spaces
- Deeper dataset / replay tooling
- Integration with future spatial AI workflows

---

## 13. Success Metrics

### Platform metrics
- Number of concurrent apps supported stably
- Compositor latency overhead
- Single-app fast-path latency
- Capture reliability

### User metrics
- Number of 3D captures per active user
- Frequency of multi-window spatial layouts
- Average session duration in shell
- Repeated use of saved layouts

### Strategic metrics
- Number of integrated native apps
- Number of legacy 2D apps usable as panels
- Dataset volume captured in opt-in mode
- Number of multi-display deployments

---

## 14. Non-Goals for Initial Release

The first release is **not**:
- A new kernel or full standalone OS
- A replacement for Windows or macOS
- A multi-user simultaneous tracked-display system
- A fully general distributed spatial collaboration platform

Those may become future directions, but they are not initial product requirements.

---

## 15. Summary

The Spatial Desktop Platform consists of:

- a **Spatial Runtime** that performs cross-app 3D composition, display processing, and capture
- a **3D Shell** that turns that runtime into a usable spatial desktop

**v1 differentiator:**

- **Cross-app spatial compositing** — the runtime combines multiple app outputs into one coherent 3D desktop scene. This is the core capability that makes the platform useful.

**Future strategic extensions (Phase 2+):**

- **3D capture** — capture native spatial content before weave, enabling screenshots, recordings, replay, and future datasets.
- **Multi-display unified space** — extend from one tracked display to a workstation-scale multi-display environment.

Build the open runtime first, then build the proprietary shell as the first compelling product on top of it. Keep multi-app spatial windows as the v1 core. Everything else is Phase 2+.
