# DisplayXR Documentation

## For App Developers

Build apps for 3D displays using the OpenXR standard.

- **[Getting Started](getting-started/overview.md)** — what is DisplayXR, architecture, sim_display
- **[Building](getting-started/building.md)** — build instructions for Windows and macOS
- **[App Classes](getting-started/app-classes.md)** — handle, texture, hosted, IPC — which one to use
- **[Your First Handle App](getting-started/first-handle-app.md)** — tutorial walkthrough

### Extension Specs

- [XR_EXT_display_info](specs/XR_EXT_display_info.md) — display properties, eye tracking, rendering modes
- [XR_EXT_win32_window_binding](specs/XR_EXT_win32_window_binding.md) — app-provided Win32 HWND
- [XR_EXT_cocoa_window_binding](specs/XR_EXT_cocoa_window_binding.md) — app-provided Cocoa NSView
- [Kooima Projection](architecture/kooima-projection.md) — stereo math and projection pipelines

---

## For Contributors

Contribute to the DisplayXR runtime — compositors, state tracker, auxiliary code.

- **[Production Components](architecture/production-components.md)** — what ships, what runs, how the pieces connect (service, workspace controller, bridge, runtime DLL)
- **[Contributing Guide](guides/contributing.md)** — workflow, code style, CI expectations
- **[Separation of Concerns](architecture/separation-of-concerns.md)** — layer boundaries (authoritative)
- **[Project Structure](architecture/project-structure.md)** — source tree organization
- **[Compositor Pipeline](architecture/compositor-pipeline.md)** — end-to-end rendering pipeline
- **[Extension vs Legacy Apps](architecture/extension-vs-legacy.md)** — how the runtime handles both app types
- **[In-Process vs Service](architecture/in-process-vs-service.md)** — compositor deployment modes
- **[Implementing an Extension](guides/implementing-extension.md)** — how to add OpenXR extensions

### Internal Specs

- [Swapchain Model](specs/swapchain-model.md) — two-swapchain architecture and canvas concept
- [Multiview Tiling](specs/multiview-tiling.md) — atlas layout algorithm for N-view rendering
- [Legacy App Support](specs/legacy-app-support.md) — compromise scaling for non-extension apps
- [Eye Tracking Modes](specs/eye-tracking-modes.md) — MANAGED vs MANUAL contract
- [Display Processor Interface](specs/display-processor-interface.md) — unified DP vtable design

---

## For Display Vendors

Integrate your 3D display hardware into DisplayXR.

- **[Vendor Integration Guide](guides/vendor-integration.md)** — comprehensive walkthrough
- **[Writing a Driver](guides/writing-driver.md)** — driver framework basics
- **[Display Processor Interface](specs/display-processor-interface.md)** — the DP vtable you'll implement
- **[ADR-003: Vendor Abstraction](adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md)** — why vendor code is isolated
- **[Separation of Concerns](architecture/separation-of-concerns.md)** — what goes where

---

## Architecture Decision Records

- [ADR-001](adr/ADR-001-native-compositors-per-graphics-api.md) — Native compositors per graphics API
- [ADR-002](adr/ADR-002-ipc-layer-preserved-for-multi-app.md) — IPC layer preserved for multi-app
- [ADR-003](adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md) — Vendor abstraction via DP vtable
- [ADR-004](adr/ADR-004-d3d11-native-over-vulkan-multi-compositor.md) — D3D11 native over Vulkan multi-compositor
- [ADR-005](adr/ADR-005-multiview-atlas-layout.md) — Multiview atlas layout
- [ADR-006](adr/ADR-006-legacy-app-compromise-view-scale.md) — Legacy app compromise view scale
- [ADR-007](adr/ADR-007-compositor-never-weaves.md) — Compositor never weaves
- [ADR-008](adr/ADR-008-display-as-spatial-entity.md) — Display as spatial entity
- [ADR-009](adr/ADR-009-upstream-cherry-pick-strategy.md) — Upstream cherry-pick strategy
- [ADR-010](adr/ADR-010-shared-app-iosurface-worst-case-sized.md) — Shared app IOSurface worst-case sized

---

## Roadmap

Design docs, status trackers, and plans — some shipped, some in progress.

### Shipped

- **[Shell Implementation Tasks](roadmap/shell-tasks.md)** — phased task tracker (Phase 0–8, all shipped on Windows)
- [Spatial OS](roadmap/spatial-os.md) — multi-compositor architecture (#43)
- [3D Shell](roadmap/3d-shell.md) — spatial window manager (#44)
- [3D Capture](roadmap/3d-capture.md) — capture pipeline (shipped in Shell Phase 8)
- [Workspace/Runtime Contract](roadmap/workspace-runtime-contract.md) — IPC between a workspace controller and the runtime
- [MCP Spec v0.2](roadmap/mcp-spec-v0.2.md) — AI-native runtime control & introspection over Model Context Protocol
- [MCP Phase A Status](roadmap/mcp-phase-a-status.md) — handle-app introspection (complete)
- [MCP Phase B Plan](roadmap/mcp-phase-b-plan.md) — service-mode shell tools (complete)

### Planned

- **[Roadmap Overview](roadmap/overview.md)** — milestone status and project trajectory
- [Spatial Desktop PRD](roadmap/spatial-desktop-prd.md) — product vision
- [Spatial Workspace Extensions Plan](roadmap/spatial-workspace-extensions-plan.md) — three-phase plan to decouple the shell from the runtime: boundary rename (Phase 1, done), policy migration behind extensions (Phase 2), repo severance (Phase 3)
- [Workspace Extensions Header Sketch](roadmap/spatial-workspace-extensions-headers-draft.md) — `XR_EXT_spatial_workspace.h` + `XR_EXT_app_launcher.h` C-level API draft, mapping table from the existing IPC RPCs, mechanism-vs-policy lists, open design questions
- [Workspace/Runtime Contract](roadmap/workspace-runtime-contract.md) — IPC message set between a workspace controller and the runtime (the current implementation; promoted to public extensions in Phase 2)
- [Display Spatial Model](roadmap/display-spatial-model.md) — displays in the spatial graph (#46)
- [Multi-Display Single Machine](roadmap/multi-display-single-machine.md) — multiple displays, one machine (#69)
- [Multi-Display Networked](roadmap/multi-display-networked.md) — displays across the network (#70)
- [XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW](roadmap/XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW.md) — Khronos multiview proposal (#80)

---

## Reference

- [Conventions](reference/conventions.md) — code style and naming conventions
- [Understanding Targets](reference/understanding-targets.md) — build target structure
- [Windows Build](reference/winbuild.md) — Windows build instructions
- [Qwerty Device](reference/qwerty-device.md) — keyboard/mouse simulated controller
- [Window Drag Rendering](reference/window-drag-rendering.md) — rendering during window drag
- [Debug Logging](reference/debug-logging.md) — log level conventions
- [Leia SR Weaver](reference/LeiaWeaver.md) — DX11, DX12, OpenGL & Vulkan weaver internals

## Archive

Resolved or superseded documents — kept for historical reference.

- [Compositor vs Display Processor](archive/compositor-vs-display-processor.md) — resolved by ADR-007 + process_atlas
- [IPC Design](archive/ipc-design.md) — inherited from Monado
- [Design Spaces](archive/design-spaces.md) — inherited from Monado
- [Swapchains IPC](archive/swapchains-ipc.md) — inherited from Monado

## Legacy Monado

Inherited Monado documentation — kept for reference, not actively maintained.

- [Frame Pacing](legacy-monado/frame-pacing.md)
- [How to Release](legacy-monado/how-to-release.md)
- [Metrics](legacy-monado/metrics.md)
- [Packaging Notes](legacy-monado/packaging-notes.md)
- [Tracing](legacy-monado/tracing.md)
- [Tracing (Perfetto)](legacy-monado/tracing-perfetto.md)
- [Tracing (Tracy)](legacy-monado/tracing-tracy.md)
- [Vulkan Extensions](legacy-monado/vulkan-extensions.md)
