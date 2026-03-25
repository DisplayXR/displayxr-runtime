# DisplayXR Roadmap

This page summarizes where the project stands and where it's headed. For the full issue tracker, see the [GitHub milestones](https://github.com/dfattal/openxr-3d-display/milestones).

## Project Phases

### Foundation (M1 + M2) — Complete

Stripped Monado from 500+ files to ~150. Removed 34 VR drivers and the Vulkan server compositor. Built native compositors for every major graphics API.

**What you can build on today:**
- Native compositors: D3D11, D3D12, Metal, OpenGL, Vulkan — all shipping
- Three drivers: Leia (LeiaSR SDK), sim_display (simulation), qwerty (keyboard/mouse)
- Four app classes: handle, texture, hosted, IPC
- Custom OpenXR extensions: `XR_EXT_display_info`, `XR_EXT_win32_window_binding`, `XR_EXT_cocoa_window_binding`

### Stabilization (M3 + M4) — In Progress (~60%)

Test coverage and extension API lockdown.

| Issue | Status | Description |
|-------|--------|-------------|
| #33 | Open | OpenXR conformance testing (Khronos CTS) |
| #3 | Open | Event system (display mode + eye tracking state changes) |
| #38 | Open | Refactor stereo3d helpers to per-eye API for multiview |
| #66 | Open | Update extension docs for M4 completion |
| #81 | Open | Formalize MANAGED vs MANUAL eye tracking contract |

### Standardization (M5) — In Progress (~22%)

Display processor interface standardization and upstream Monado integration.

| Issue | Status | Description |
|-------|--------|-------------|
| #45 | Open | Standardize display processor interface across APIs |
| #46 | Open | Display spatial model (displays as spatial objects) |
| #47 | Open | Upstream Monado cherry-pick strategy |

### Spatial Desktop (M6) — In Progress (~23%)

Multi-app spatial window manager — the long-term vision for 3D displays as a desktop platform.

| Issue | Status | Description |
|-------|--------|-------------|
| #43 | Open | Spatial OS (platform-native multi compositor) |
| #44 | Open | 3D Shell (spatial window manager) |
| #48 | Open | Metal service compositor for macOS IPC |
| #49 | Open | D3D11 service compositor for Windows |
| #69 | Open | Multi-display compositing on single machine |

## Vision

The long-term goal is a **spatial desktop platform** where multiple OpenXR apps render as 3D windows on a light field display, managed by a platform-native shell. See the [Spatial Desktop PRD](spatial-desktop-prd.md) for the full product vision.

## Task Tracker

- [Shell Implementation Tasks](shell-tasks.md) — phased task tracker for the 3D shell (Phase 0–5)

## Proposal Documents

All documents in `docs/roadmap/` describe planned features that are not yet implemented:

- [Spatial Desktop PRD](spatial-desktop-prd.md) — product requirements
- [Spatial OS](spatial-os.md) — multi-compositor architecture
- [3D Shell](3d-shell.md) — spatial window manager
- [3D Capture](3d-capture.md) — capture pipeline
- [Shell/Runtime Contract](shell-runtime-contract.md) — IPC between shell and runtime
- [Display Spatial Model](display-spatial-model.md) — displays in the spatial graph
- [Multi-Display Single Machine](multi-display-single-machine.md) — multiple displays, one machine
- [Multi-Display Networked](multi-display-networked.md) — displays across the network
- [XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW](XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW.md) — Khronos multiview proposal
