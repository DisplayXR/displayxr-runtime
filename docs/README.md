# DisplayXR Documentation

## Who are you?

| **App developer** | **OXR contributor** | **DXR core contributor** | **Vendor contributor** |
|---|---|---|---|
| Building apps that target DisplayXR | Working on the OpenXR state tracker / extensions | Working on compositors, IPC, shell contracts, build | Integrating a 3D-display vendor (driver + DP) |
| → [`getting-started/`](getting-started/) | → [`architecture/`](architecture/) | → [`architecture/`](architecture/) | → [`guides/vendor-integration.md`](guides/vendor-integration.md) |
| → [`specs/extensions/`](specs/extensions/) | → [`specs/extensions/`](specs/extensions/) | → [`specs/runtime/`](specs/runtime/) | → [`specs/vendor/`](specs/vendor/) |
| → [`reference/`](reference/) | → [`guides/implementing-extension.md`](guides/implementing-extension.md) | → [`adr/`](adr/), [`roadmap/`](roadmap/) | → [`vendors/`](vendors/) (your vendor's subfolder) |
| | → [`adr/`](adr/) | | → [`adr/`](adr/) (003, 007, 015) |

---

## For App Developers

Build apps for 3D displays using the OpenXR standard.

- **[Getting Started](getting-started/overview.md)** — what is DisplayXR, architecture, sim_display
- **[Building](getting-started/building.md)** — build instructions for Windows and macOS
- **[App Classes](getting-started/app-classes.md)** — handle, texture, hosted, IPC — which one to use
- **[Your First Handle App](getting-started/first-handle-app.md)** — tutorial walkthrough
- **[Ship a Manifest](getting-started/ship-a-manifest.md)** — make your app discoverable on every workspace controller, OEM shell, and showcase in the ecosystem with a 30-second JSON file + optional 3D logo

### Extension Specs

- [XR_EXT_display_info](specs/extensions/XR_EXT_display_info.md) — display properties, eye tracking, rendering modes
- [XR_EXT_win32_window_binding](specs/extensions/XR_EXT_win32_window_binding.md) — app-provided Win32 HWND
- [XR_EXT_cocoa_window_binding](specs/extensions/XR_EXT_cocoa_window_binding.md) — app-provided Cocoa NSView
- [XR_EXT_spatial_workspace](specs/extensions/XR_EXT_spatial_workspace.md) — workspace controller surface (shell-style apps)
- [Kooima Projection](architecture/kooima-projection.md) — stereo math and projection pipelines

---

## For OXR / DXR Core Contributors

Contribute to the DisplayXR runtime — compositors, state tracker, auxiliary code.

- **[Production Components](architecture/production-components.md)** — what ships, what runs, how the pieces connect (service, workspace controller, bridge, runtime DLL)
- **[Contributing Guide](guides/contributing.md)** — workflow, code style, CI expectations
- **[Separation of Concerns](architecture/separation-of-concerns.md)** — layer boundaries (authoritative)
- **[Project Structure](architecture/project-structure.md)** — source tree organization
- **[Compositor Pipeline](architecture/compositor-pipeline.md)** — end-to-end rendering pipeline
- **[Extension vs Legacy Apps](architecture/extension-vs-legacy.md)** — how the runtime handles both app types
- **[In-Process vs Service](architecture/in-process-vs-service.md)** — compositor deployment modes
- **[Implementing an Extension](guides/implementing-extension.md)** — how to add OpenXR extensions

### Internal Specs (`specs/runtime/`)

- [Swapchain Model](specs/runtime/swapchain-model.md) — two-swapchain architecture and canvas concept
- [Multiview Tiling](specs/runtime/multiview-tiling.md) — atlas layout algorithm for N-view rendering
- [Legacy App Support](specs/runtime/legacy-app-support.md) — compromise scaling for non-extension apps
- [Workspace Controller Registration](specs/runtime/workspace-controller-registration.md) — how shell-style apps register with the runtime
- [DisplayXR App Manifest](specs/runtime/displayxr-app-manifest.md) — sidecar JSON for app discovery

---

## For Display Vendors

Integrate your 3D display hardware into DisplayXR.

- **[Vendor Integration Guide](guides/vendor-integration.md)** — comprehensive walkthrough
- **[Writing a Driver](guides/writing-driver.md)** — driver framework basics
- **[Display Processor Interface](specs/vendor/display-processor-interface.md)** — the DP vtable you'll implement
- **[Eye Tracking Modes](specs/vendor/eye-tracking-modes.md)** — MANAGED vs MANUAL contract
- **[ADR-003: Vendor Abstraction](adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md)** — why vendor code is isolated
- **[ADR-007: Compositor Never Weaves](adr/ADR-007-compositor-never-weaves.md)** — compositor / DP boundary
- **[ADR-015: Multi-Display Routing](adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md)** — how multiple vendors coexist
- **[Separation of Concerns](architecture/separation-of-concerns.md)** — what goes where

### Integrated vendors (`vendors/`)

- **[Vendors index](vendors/README.md)** — list of integrated vendors + how to add a new one
- [Leia SR](vendors/leia/README.md) — D3D11 / D3D12 / OpenGL / Vulkan, weaver, transparency model
- [sim_display](vendors/sim_display/README.md) — reference simulation vendor (SBS / anaglyph on a 2D window)

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
- [ADR-011](adr/ADR-011-d3d11-typeless-swapchain-textures.md) — D3D11 typeless swapchain textures
- [ADR-012](adr/ADR-012-window-relative-kooima-projection.md) — Window-relative Kooima projection
- [ADR-013](adr/ADR-013-universal-app-launch-model.md) — Universal app launch model
- [ADR-014](adr/ADR-014-shell-owns-rendering-mode.md) — Shell owns rendering mode
- [ADR-015](adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md) — DisplayXR owns multi-display vendor routing
- [ADR-016](adr/ADR-016-workspace-controllers-own-tray-surface-and-lifecycle.md) — Workspace controllers own tray surface and lifecycle

---

## Roadmap

Design docs, status trackers, and plans — some shipped, some in progress. After the 2026-05-13 cleanup, this folder holds only living docs (PRDs, in-progress plans, current contracts) — historical agent prompts and shipped-phase status snapshots are recoverable from git history.

### Shipped

- **[Shell Implementation Tasks](roadmap/shell-tasks.md)** — phased task tracker (Phase 0–8, all shipped on Windows)
- **[Shell Phase 1 Status](roadmap/shell-phase1-status.md)** — kept as the test-procedure reference for shell-mode launches
- **[Shell Phase 2 Plan](roadmap/shell-phase2-plan.md)** — kept as the spatial-workspace migration reference
- [Spatial OS](roadmap/spatial-os.md) — multi-compositor architecture (#43)
- [3D Shell](roadmap/3d-shell.md) — spatial window manager (#44)
- [3D Capture](roadmap/3d-capture.md) — capture pipeline (shipped in Shell Phase 8)
- [Workspace/Runtime Contract](roadmap/workspace-runtime-contract.md) — IPC between a workspace controller and the runtime
- **MCP** — framework extracted to [`DisplayXR/displayxr-mcp`](https://github.com/DisplayXR/displayxr-mcp). See the [MCP spec](https://github.com/DisplayXR/displayxr-mcp/blob/main/docs/mcp-spec.md) for the protocol; Phase A (handle-app introspection) and Phase B (workspace tools, hosted in `displayxr-shell-pvt`) both shipped.
- [Desktop Overlay Apps — Forward Work](roadmap/avatar-overlay-native.md) — follow-on work after the transparent HWND path shipped (#191)

### Planned / In Progress

- **[Roadmap Overview](roadmap/overview.md)** — milestone status and project trajectory
- [Spatial Desktop PRD](roadmap/spatial-desktop-prd.md) — product vision
- [PR FAQ](roadmap/prfaq.md) — press-release-style framing
- [Spatial Workspace Extensions Plan](roadmap/spatial-workspace-extensions-plan.md) — three-phase plan to decouple the shell from the runtime: boundary rename (Phase 1, done), policy migration behind extensions (Phase 2), repo severance (Phase 3)
- [Workspace Extensions Header Sketch](roadmap/spatial-workspace-extensions-headers-draft.md) — `XR_EXT_spatial_workspace.h` + `XR_EXT_app_launcher.h` C-level API draft
- [Workspace Controller Detection](roadmap/spatial-workspace-controller-detection.md) — Phase 2.0 prep: orchestrator detects installed controller via sidecar `.controller.json` manifest
- [Workspace Activation Auth Handshake](roadmap/spatial-workspace-auth-handshake.md) — Phase 2.0 prep: orchestrator-PID match replaces the brand-coupled `application_name == "displayxr-shell"` check
- [Phase 2 Audit](roadmap/spatial-workspace-extensions-phase2-audit.md) — line-by-line classification of the remaining `shell` mentions in `comp_d3d11_service.cpp`
- [WebXR Bridge v2 Plan](roadmap/webxr-bridge-v2-plan.md) — metadata/control sideband for Chrome's native WebXR
- [Display Spatial Model](roadmap/display-spatial-model.md) — displays in the spatial graph (#46)
- [Multi-Display Single Machine](roadmap/multi-display-single-machine.md) — multiple displays, one machine (#69)
- [Multi-Display Networked](roadmap/multi-display-networked.md) — displays across the network (#70)
- [Demo Distribution](roadmap/demo-distribution.md) — per-component tag scheme + runtime-compat covenant
- [XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW](roadmap/XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW.md) — Khronos multiview proposal (#80)

---

## Reference

Cross-cutting references that don't belong to a single audience.

- [Conventions](reference/conventions.md) — code style and naming conventions
- [Understanding Targets](reference/understanding-targets.md) — build target structure
- [Windows Build](reference/winbuild.md) — Windows build instructions
- [Qwerty Device](reference/qwerty-device.md) — keyboard/mouse simulated controller
- [Window Drag Rendering](reference/window-drag-rendering.md) — rendering during window drag
- [Debug Logging](reference/debug-logging.md) — log level conventions

Vendor-specific reference docs now live in [`vendors/<vendor>/`](vendors/).

---

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
