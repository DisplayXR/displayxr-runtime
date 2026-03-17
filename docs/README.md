# DisplayXR Documentation

## Specs (`specs/`)

Living feature and extension specifications.

- [XR_EXT_display_info](specs/XR_EXT_display_info.md) — Display properties, eye tracking, rendering modes, mode enumeration
- [XR_EXT_win32_window_binding](specs/XR_EXT_win32_window_binding.md) — App-provided Win32 HWND for windowed rendering
- [XR_EXT_cocoa_window_binding](specs/XR_EXT_cocoa_window_binding.md) — App-provided Cocoa NSView for rendering on macOS
- [XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW](specs/XR_VIEW_CONFIGURATION_PRIMARY_MULTIVIEW.md) — Khronos multiview view configuration proposal
- [Vendor Integration Guide](specs/vendor-integration.md) — How to add a new 3D display vendor
- [Multiview Tiling](specs/multiview-tiling.md) — Atlas layout for N-view rendering
- [Legacy App Support](specs/legacy-app-support.md) — Compromise scaling for non-extension apps
- [Eye Tracking Modes](specs/eye-tracking-modes.md) — Smooth/raw eye tracking contract
- [Display Processor Interface](specs/display-processor-interface.md) — Unified DP vtable design

## Architecture Decision Records (`adr/`)

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

## Architecture (`architecture/`)

System design documents.

- [Separation of Concerns](architecture/separation-of-concerns.md) — Layer boundaries: App → OXR → Compositor → Driver/DP
- [Compositor vs Display Processor](architecture/compositor-vs-display-processor.md) — Where weaving belongs
- [Display Spatial Model](architecture/display-spatial-model.md) — Displays in the spatial graph
- [Spatial OS](architecture/spatial-os.md) — Multi-app spatial operating system
- [3D Shell](architecture/3d-shell.md) — Window manager for 3D displays
- [Multi-Display Single Machine](architecture/multi-display-single-machine.md) — Multiple displays on one machine
- [Multi-Display Networked](architecture/multi-display-networked.md) — Networked multi-display
- [In-Process vs Service](architecture/in-process-vs-service.md) — Compositor deployment modes
- [Stereo 3D Math](architecture/stereo3d-math.md) — Kooima projection and stereo geometry
- [Project Structure](architecture/project-structure.md) — Source tree organization
- [IPC Design](architecture/ipc-design.md) — Inter-process communication (inherited from Monado)
- [Design Spaces](architecture/design-spaces.md) — OpenXR reference spaces (inherited from Monado)
- [Swapchains IPC](architecture/swapchains-ipc.md) — Swapchain handling over IPC (inherited from Monado)

## Notes (`notes/`)

Reference material.

- [Conventions](notes/conventions.md) — Code style and naming conventions
- [Implementing an Extension](notes/implementing-extension.md) — How to add OpenXR extensions
- [Writing a Driver](notes/writing-driver.md) — How to add device drivers
- [Understanding Targets](notes/understanding-targets.md) — Build target structure
- [Windows Build](notes/winbuild.md) — Windows build instructions
- [Qwerty Device](notes/qwerty-device.md) — Keyboard/mouse simulated controller
- [Window Drag Rendering](notes/window-drag-rendering.md) — Rendering during window drag

## Legacy Monado (`legacy-monado/`)

Inherited Monado documentation — kept for reference, not actively maintained.

- [Frame Pacing](legacy-monado/frame-pacing.md)
- [How to Release](legacy-monado/how-to-release.md)
- [Metrics](legacy-monado/metrics.md)
- [Packaging Notes](legacy-monado/packaging-notes.md)
- [Tracing](legacy-monado/tracing.md)
- [Tracing (Perfetto)](legacy-monado/tracing-perfetto.md)
- [Tracing (Tracy)](legacy-monado/tracing-tracy.md)
- [Vulkan Extensions](legacy-monado/vulkan-extensions.md)
