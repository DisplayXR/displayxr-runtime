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

The goal is to create a desktop-class spatial computing environment for tracked 3D displays, while also laying the foundation for 3D content capture, replay, and future 3D dataset generation.

---

## 2. Product Vision

Today, 3D displays exist mostly as isolated endpoints with vendor-specific runtimes and limited desktop integration.

This platform changes that by providing:

- a **runtime** that can spatially compose multiple applications into one coherent 3D scene
- a **shell** that turns that scene into a usable spatial desktop
- a path toward **3D-native capture and dataset generation**
- a future extension toward **multi-display unified spatial workspaces**

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

---

## 6. Core Feature 1: 3D Capture

3D capture enables native capture of spatial content before display-specific weaving, so output remains reusable as stereo content, spatial replay, or dataset material.

**Full specification:** [3d-capture.md](3d-capture.md)

---

## 7. Core Feature 2: Cross-App Spatial Compositing

Cross-app spatial compositing allows multiple applications to coexist in a shared 3D scene on a tracked display, with correct spatial placement and coherent viewing behavior. This is the central runtime capability that makes the rest of the platform possible.

**Full specification:** [spatial-os.md](spatial-os.md)

---

## 8. Core Feature 3: Multi-Display Unified Space

Multi-display extends the single-display spatial desktop into a workspace where multiple tracked 3D displays participate in one coherent spatial environment. This is a later phase than single-display compositing.

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

The three strategic differentiators remain:

1. **3D capture**
   The platform can capture native spatial content before weave, enabling screenshots, recordings, replay, and future datasets.

2. **Cross-app spatial compositing**
   The runtime can combine multiple app outputs into one coherent 3D desktop scene.

3. **Multi-display unified space**
   The architecture can extend from one tracked display to a workstation-scale multi-display environment.

This is the foundation for a real spatial workstation platform rather than a single-device demo stack.
