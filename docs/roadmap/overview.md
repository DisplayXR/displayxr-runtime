# DisplayXR Roadmap

This page summarizes where the project stands and where it's headed. For the full issue tracker, see the [GitHub milestones](https://github.com/DisplayXR/displayxr-runtime/milestones).

## Project Phases

### Foundation (M1 + M2) — Complete

Stripped Monado from 500+ files to ~150. Removed 34 VR drivers and the Vulkan server compositor. Built native compositors for every major graphics API.

**What you can build on today:**
- Native compositors: D3D11, D3D12, Metal, OpenGL, Vulkan — all shipping
- Drivers: vendor display-processor plug-ins (e.g. a lenticular hardware DP), plus in-tree sim_display (simulation) and qwerty (keyboard/mouse)
- Four app classes: handle, texture, hosted, IPC
- Custom OpenXR extensions: `XR_DXR_display_info`, `XR_DXR_win32_window_binding`, `XR_DXR_cocoa_window_binding`
- Platforms: Windows, macOS, Linux and Android. Linux **ships** on Ubuntu 22.04/24.04/26.04 (Vulkan-only X11/XCB compositor, `.deb` on every release; see [Linux Support](linux-support.md)); Android ships the runtime APK but has no public release path yet (#1212)

### Stabilization (M3 + M4) — Complete

Test coverage and extension API lockdown: CTS harness (#33, one non-interactive arm in CI — the full matrix is epic #1523), the event system (#3), per-eye multiview helpers (#38), extension docs (#66) and the MANAGED/MANUAL eye-tracking contract (#81) all landed.

### Standardization (M5) — Mostly complete

The display-processor interface is standardized across APIs (#45) and the upstream cherry-pick strategy is settled (#47, ADR-009). Open: #46 display spatial model (displays as spatial objects).

### Spatial Desktop (M6) — Shipping, one item open

The platform-native multi compositor (#43), the 3D Shell (#44, now `displayxr-shell-pvt`, Windows + macOS), and the Metal (#48) and D3D11 (#49) service compositors shipped. Open: #69 multi-display compositing on a single machine (Phase 3; see [multi-screen](multi-screen.md)).

### Component model (ADR-054) — Shipping

Three plug-in classes (display processors, input providers, conversion providers) behind one shared loader, four verbs per class, a provenance gate (enforcing on Windows), and a per-class conformance cell in CI. Next: the viewer-tracking provider class (#2144) and the decode task family (#2145).

## Vision

The long-term goal is a **spatial desktop platform** where multiple OpenXR apps render as 3D windows on a light field display, managed by a platform-native shell. See the [Spatial Desktop PRD](spatial-desktop-prd.md) for the full product vision.

## Proposal Documents

Documents in `docs/roadmap/` carry their own status line (shipped, in progress, or proposal); ADRs and specs are authoritative where they disagree:

- [Spatial Desktop PRD](spatial-desktop-prd.md) — product requirements
- [3D Capture](3d-capture.md) — capture pipeline
- [Workspace/Runtime Contract](workspace-runtime-contract.md) — IPC between a workspace controller and the runtime
- [Display Spatial Model](display-spatial-model.md) — displays in the spatial graph
- [Multi-Display Single Machine](multi-display-single-machine.md) — multiple displays, one machine
- [Multi-Display Networked](multi-display-networked.md) — displays across the network
- [Linux Support](linux-support.md) — the fourth platform: headless substrate bring-up (Phase 0), then in-process Vulkan/XCB compositor, service/IPC path, and a window-binding extension
- [WebXR Support — Status & Roadmap](webxr-support.md) — shipped Bridge v2 metadata sideband + the inline-3D (`session.displayXR.weave()`) roadmap, prototyping via Chromium patches, and the fork-vs-standards adoption path
- [XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW](XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW.md) — Khronos multiview proposal
- [MCP Spec](https://github.com/DisplayXR/displayxr-mcp/blob/main/docs/mcp-spec.md) — AI-native runtime: expose live spatial state and control to agents over Model Context Protocol (extracted to [`DisplayXR/displayxr-mcp`](https://github.com/DisplayXR/displayxr-mcp))
