# DisplayXR Documentation

> **New to the project?** Read **[ORIENTATION.md](ORIENTATION.md)** first — a top-down, plain-language tour of what DisplayXR is, why it's built this way, and where every doc lives. This page below is the role-based jump table.

## Who are you?

| **App developer** | **OXR contributor** | **DXR core contributor** | **Vendor contributor** |
|---|---|---|---|
| Building apps that target DisplayXR | Working on the OpenXR state tracker / extensions | Working on compositors, IPC, shell contracts, build | Integrating a 3D-display vendor (driver + DP) |
| → [`getting-started/`](getting-started/) | → [`architecture/`](architecture/) | → [`architecture/`](architecture/) | → [`guides/vendor-plugin-onboarding.md`](guides/vendor-plugin-onboarding.md) |
| → [`specs/extensions/`](specs/extensions/) | → [`specs/extensions/`](specs/extensions/) | → [`specs/runtime/`](specs/runtime/) | → [`specs/vendor/`](specs/vendor/) |
| → [`reference/`](reference/) | → [`guides/implementing-extension.md`](guides/implementing-extension.md) | → [`adr/`](adr/), [`roadmap/`](roadmap/) | → [`vendors/`](vendors/) (your vendor's subfolder) |
| | → [`adr/`](adr/) | | → [`adr/`](adr/) (003, 007, 015) |

---

## For App Developers

Build apps for 3D displays using the OpenXR standard.

- **[Getting Started](getting-started/overview.md)** — what is DisplayXR, architecture, sim_display
- **[Building](getting-started/building.md)** — build instructions for Windows, macOS, and Linux
- **[Android Build Guide](getting-started/android-build-guide.md)** — Lume Pad-class hardware: vendor SDK setup, Gradle, install
- **[Android Bring-Up Checklist](getting-started/android-bringup-checklist.md)** — A→B→C→D step-by-step test plan for first hardware install
- **[App Classes](getting-started/app-classes.md)** — handle, texture, hosted, IPC — which one to use
- **[Your First Handle App](getting-started/first-handle-app.md)** — tutorial walkthrough
- **[Ship a Manifest](getting-started/ship-a-manifest.md)** — make your app discoverable on every workspace controller, OEM shell, and showcase in the ecosystem with a 30-second JSON file + optional 3D logo
- **[Troubleshooting](getting-started/troubleshooting.md)** — symptom → cause → fix for the field-confirmed pitfalls (app hangs at startup / VPN Winsock LSP, "Failed to initialize OpenXR", Vulkan crashes, eye-tracking/camera, wrong runtime loads); start with `displayxr-cli selftest`
- **[FAQ](getting-started/faq.md)** — conceptual questions: what DisplayXR is, supported displays/OSes/graphics APIs, do I need hardware to develop, multiview vs stereo, engines, license, relation to Monado

### Extension Specs

- [XR_DXR_display_info](specs/extensions/XR_DXR_display_info.md) — display properties, eye tracking, rendering modes
- [XR_DXR_win32_window_binding](specs/extensions/XR_DXR_win32_window_binding.md) — app-provided Win32 HWND
- [XR_DXR_cocoa_window_binding](specs/extensions/XR_DXR_cocoa_window_binding.md) — app-provided Cocoa NSView
- [XR_DXR_xlib_window_binding](specs/extensions/XR_DXR_xlib_window_binding.md) — app-provided X11 window (desktop Linux)
- [XR_DXR_spatial_workspace](specs/extensions/XR_DXR_spatial_workspace.md) — workspace controller surface (shell-style apps)
- [XR_DXR_display_zones](specs/extensions/XR_DXR_display_zones.md) — N 3D zones + 2D zones + wish mask (design sketch, ADR-027)
- [Kooima Projection](architecture/kooima-projection.md) — N-view Kooima math and projection pipelines

---

## For OXR / DXR Core Contributors

Contribute to the DisplayXR runtime — compositors, state tracker, auxiliary code.

- **[Production Components](architecture/production-components.md)** — what ships, what runs, how the pieces connect (service, workspace controller, bridge, runtime DLL)
- **[Contributing Guide](guides/contributing.md)** — workflow, code style, CI expectations
- **[Separation of Concerns](architecture/separation-of-concerns.md)** — layer boundaries (authoritative)
- **[Project Structure](architecture/project-structure.md)** — source tree organization
- **[Compositor Pipeline](architecture/compositor-pipeline.md)** — end-to-end rendering pipeline (single-app)
- **[Service-Mode Multi-Compositor](architecture/multi-compositor.md)** — server-side N-client compositor (workspace + IPC apps + bridge)
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

- **[Vendor Plug-in Onboarding](guides/vendor-plugin-onboarding.md)** — zero-to-shipping guide for a new vendor's external plug-in repo (post-#263 model)
- **[`xrt_plugin_iface` reference](reference/xrt_plugin_iface.md)** — per-method contract for the plug-in vtable
- **[Plug-in discovery spec](specs/runtime/plugin-discovery.md)** — registry / JSON manifest formats, env-var overrides
- **[ADR-019: Vendor Plug-in / Aux Boundary](adr/ADR-019-vendor-plugin-aux-boundary.md)** — why the runtime DLL holds zero vendor identifiers
- **[ADR-022: Per-Mode Capability Flags + Frozen Enumerated App Structs](adr/ADR-022-per-mode-capability-flags-frozen-enum-structs.md)** — `mode_flags` bits (no more rendering-mode ABI breaks) + `XrDisplayRenderingModeInfoDXR` frozen at v13 (all future per-mode fields chain)
- **[Display Processor Interface](specs/vendor/display-processor-interface.md)** — the DP vtable you'll implement
- **[Eye Tracking Modes](specs/vendor/eye-tracking-modes.md)** — MANAGED vs MANUAL contract
- **[ADR-003: Vendor Abstraction](adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md)** — why vendor code is isolated
- **[ADR-007: Compositor Never Weaves](adr/ADR-007-compositor-never-weaves.md)** — compositor / DP boundary
- **[ADR-015: Multi-Display Routing](adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md)** — how multiple vendors coexist
- **[Separation of Concerns](architecture/separation-of-concerns.md)** — what goes where
- [Legacy: in-tree integration model](archive/vendor-integration-historical.md) — historical reference for pre-#263 vendors who forked the runtime
- [Writing a Driver](guides/writing-driver.md) — driver framework basics

### Integrated vendors (`vendors/`)

- **[Vendors index](vendors/README.md)** — list of integrated vendors + how to add a new one
- [Leia SR](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/docs/README.md) (in [displayxr-leia-plugin](https://github.com/DisplayXR/displayxr-leia-plugin)) — D3D11 / D3D12 / OpenGL / Vulkan, weaver, transparency model
- [sim_display](vendors/sim_display/README.md) — reference simulation vendor (SBS / anaglyph on a 2D window)

---

## Architecture Decision Records

> Index below is generated by `scripts/gen_adr_index.py` (CI-checked). Add an ADR file, re-run the script. Full standalone index: [`adr/README.md`](adr/README.md).

<!-- BEGIN ADR INDEX -->
- [ADR-001](adr/ADR-001-native-compositors-per-graphics-api.md) — Native Compositors Per Graphics API
- [ADR-002](adr/ADR-002-ipc-layer-preserved-for-multi-app.md) — IPC Layer Preserved for Multi-App
- [ADR-003](adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md) — Vendor Abstraction via Display Processor Vtable
- [ADR-004](adr/ADR-004-d3d11-native-over-vulkan-multi-compositor.md) — D3D11 Native Over Vulkan Multi-Compositor
- [ADR-005](adr/ADR-005-multiview-atlas-layout.md) — Multiview Atlas Layout
- [ADR-006](adr/ADR-006-legacy-app-compromise-view-scale.md) — Legacy App Compromise View Scale
- [ADR-007](adr/ADR-007-compositor-never-weaves.md) — Compositor Never Weaves
- [ADR-008](adr/ADR-008-display-as-spatial-entity.md) — Display as Spatial Entity
- [ADR-009](adr/ADR-009-upstream-cherry-pick-strategy.md) — Upstream Cherry-Pick Strategy
- [ADR-010](adr/ADR-010-shared-app-iosurface-worst-case-sized.md) — Shared App IOSurface Worst-Case Sized
- [ADR-011](adr/ADR-011-d3d11-typeless-swapchain-textures.md) — D3D11 Swapchain Textures Must Use TYPELESS Format
- [ADR-012](adr/ADR-012-window-relative-kooima-projection.md) — Window-Relative Kooima Projection
- [ADR-013](adr/ADR-013-universal-app-launch-model.md) — Universal App Launch Model (Hidden HWND Proxy)
- [ADR-014](adr/ADR-014-shell-owns-rendering-mode.md) — Shell Owns Rendering Mode Control
- [ADR-015](adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md) — DisplayXR Owns Multi-Display Vendor Routing
- [ADR-016](adr/ADR-016-workspace-controllers-own-tray-surface-and-lifecycle.md) — Workspace Controllers Own Their Tray Surface and Lifecycle
- [ADR-017](adr/ADR-017-modal-dialogs-tiered-strategy.md) — Tiered strategy for Win32 modal dialogs under the workspace shell
- [ADR-018](adr/ADR-018-workspace-hit-test-plumbing-vs-policy.md) — Workspace Hit-Test Is Plumbing; Drag/Resize/Cursor Policy Is the Controller's
- [ADR-019](adr/ADR-019-vendor-plugin-aux-boundary.md) — Aux Library Boundary for Vendor Plug-in DLLs
- [ADR-020](adr/ADR-020-plugin-abi-compatibility-policy.md) — Plug-in ABI Compatibility Policy (versioning, `struct_size` negotiation, drift guards)
- [ADR-021](adr/ADR-021-color-management-encoding-state-invariant.md) — Color Management & the Encoding-State Invariant
- [ADR-022](adr/ADR-022-per-mode-capability-flags-frozen-enum-structs.md) — Per-Mode Capability Flags + Frozen Enumerated App Structs
- [ADR-023](adr/ADR-023-unified-atlas-capture.md) — Unified Atlas Capture (XR_DXR_atlas_capture)
- [ADR-024](adr/ADR-024-raw-vs-render-ready-views.md) — Raw vs Render-Ready Views (XR_DXR_view_rig)
- [ADR-025](adr/ADR-025-android-vendor-dp-out-of-process.md) — Android Vendor Display Processors Run Out-of-Process
- [ADR-026](adr/ADR-026-orientation-aware-view-scaling.md) — Orientation-Independent Rendering Modes, Config-Derived View Scale, Rotation-Aware Worst-Case Atlas
- [ADR-027](adr/ADR-027-display-zones.md) — Display Zones — decoupled mixed 2D/3D layout, per-zone rig, wish mask
- [ADR-028](adr/ADR-028-display-mode-recipe-vs-hardware-state.md) — The rendering mode is the content recipe; the hardware state is an orthogonal override
- [ADR-029](adr/ADR-029-client-owned-transparent-ipc-present.md) — The IPC client owns the transparent present; the DP reconstructs alpha, never bakes a background
- [ADR-030](adr/ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md) — Compositor Crops to Content; Zero-Copy Only When the Swapchain Equals the Mode Atlas
- [ADR-031](adr/ADR-031-remove-surround-output-rect-zones-sole-region-model.md) — Remove the 2D-surround / output-rect mechanism — display-zones is the sole region paradigm
- [ADR-032](adr/ADR-032-array-layered-swapchains-first-class.md) — Array (Layered) Swapchains Are First-Class Alongside the Tiled Atlas
<!-- END ADR INDEX -->

---

## Roadmap

Design docs, status trackers, and plans — some shipped, some in progress. After the 2026-05-13 cleanup, this folder holds only living docs (PRDs, in-progress plans, current contracts) — historical agent prompts and shipped-phase status snapshots are recoverable from git history.

### Shipped

- [3D Capture](roadmap/3d-capture.md) — capture pipeline (shipped in Shell Phase 8)
- [Workspace/Runtime Contract](roadmap/workspace-runtime-contract.md) — IPC between a workspace controller and the runtime
- **MCP** — framework extracted to [`DisplayXR/displayxr-mcp`](https://github.com/DisplayXR/displayxr-mcp). See the [MCP spec](https://github.com/DisplayXR/displayxr-mcp/blob/main/docs/mcp-spec.md) for the protocol; Phase A (handle-app introspection) and Phase B (workspace tools, hosted in `displayxr-shell-pvt`) both shipped.
- [Desktop Overlay Apps — Forward Work](roadmap/avatar-overlay-native.md) — follow-on work after the transparent HWND path shipped (#191)

### Planned / In Progress

- **[Roadmap Overview](roadmap/overview.md)** — milestone status and project trajectory
- [Spatial Desktop PRD](roadmap/spatial-desktop-prd.md) — product vision
- [PR FAQ](roadmap/prfaq.md) — press-release-style framing
- [Spatial Workspace Extensions Plan](roadmap/spatial-workspace-extensions-plan.md) — three-phase plan to decouple the shell from the runtime: boundary rename (Phase 1, done), policy migration behind extensions (Phase 2), repo severance (Phase 3)
- [Workspace Extensions Header Sketch](roadmap/spatial-workspace-extensions-headers-draft.md) — `XR_DXR_spatial_workspace.h` + `XR_EXT_app_launcher.h` C-level API draft
- [Workspace Controller Detection](roadmap/spatial-workspace-controller-detection.md) — Phase 2.0 prep: orchestrator detects installed controller via sidecar `.controller.json` manifest
- [Workspace Activation Auth Handshake](roadmap/spatial-workspace-auth-handshake.md) — Phase 2.0 prep: orchestrator-PID match replaces the brand-coupled `application_name == "displayxr-shell"` check
- [Phase 2 Audit](roadmap/spatial-workspace-extensions-phase2-audit.md) — line-by-line classification of the remaining `shell` mentions in `comp_d3d11_service.cpp`
- [Per-App MCP Tools & Workspace Aggregator](roadmap/per-app-mcp-tools.md) — apps register their own MCP tools via `XR_DXR_mcp_tools`; one-connection `--target workspace` aggregator with `<app-id>__<tool>` namespacing
- [WebXR Support — Status & Roadmap](roadmap/webxr-support.md) — shipped Bridge v2 metadata sideband + the inline-3D (`session.displayXR.weave()`) roadmap via Chromium patches
- [Display Zones](roadmap/display-zones.md) — N 3D zones + 2D zones + wish mask: avatar migration + phased plan (ADR-027)
- [Display Spatial Model](roadmap/display-spatial-model.md) — displays in the spatial graph (#46)
- [Multi-Display Single Machine](roadmap/multi-display-single-machine.md) — multiple displays, one machine (#69)
- [Multi-Display Networked](roadmap/multi-display-networked.md) — displays across the network (#70)
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
