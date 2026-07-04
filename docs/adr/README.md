# Architecture Decision Records

> **Auto-generated** by `scripts/gen_adr_index.py` from the `ADR-*.md`
> files in this directory. Do not edit by hand — add or edit an ADR and
> re-run the script (CI enforces it via `--check`).

- [ADR-001](ADR-001-native-compositors-per-graphics-api.md) — Native Compositors Per Graphics API
- [ADR-002](ADR-002-ipc-layer-preserved-for-multi-app.md) — IPC Layer Preserved for Multi-App
- [ADR-003](ADR-003-vendor-abstraction-via-display-processor-vtable.md) — Vendor Abstraction via Display Processor Vtable
- [ADR-004](ADR-004-d3d11-native-over-vulkan-multi-compositor.md) — D3D11 Native Over Vulkan Multi-Compositor
- [ADR-005](ADR-005-multiview-atlas-layout.md) — Multiview Atlas Layout
- [ADR-006](ADR-006-legacy-app-compromise-view-scale.md) — Legacy App Compromise View Scale
- [ADR-007](ADR-007-compositor-never-weaves.md) — Compositor Never Weaves
- [ADR-008](ADR-008-display-as-spatial-entity.md) — Display as Spatial Entity
- [ADR-009](ADR-009-upstream-cherry-pick-strategy.md) — Upstream Cherry-Pick Strategy
- [ADR-010](ADR-010-shared-app-iosurface-worst-case-sized.md) — Shared App IOSurface Worst-Case Sized
- [ADR-011](ADR-011-d3d11-typeless-swapchain-textures.md) — D3D11 Swapchain Textures Must Use TYPELESS Format
- [ADR-012](ADR-012-window-relative-kooima-projection.md) — Window-Relative Kooima Projection
- [ADR-013](ADR-013-universal-app-launch-model.md) — Universal App Launch Model (Hidden HWND Proxy)
- [ADR-014](ADR-014-shell-owns-rendering-mode.md) — Shell Owns Rendering Mode Control
- [ADR-015](ADR-015-displayxr-owns-multi-display-vendor-routing.md) — DisplayXR Owns Multi-Display Vendor Routing
- [ADR-016](ADR-016-workspace-controllers-own-tray-surface-and-lifecycle.md) — Workspace Controllers Own Their Tray Surface and Lifecycle
- [ADR-017](ADR-017-modal-dialogs-tiered-strategy.md) — Tiered strategy for Win32 modal dialogs under the workspace shell
- [ADR-018](ADR-018-workspace-hit-test-plumbing-vs-policy.md) — Workspace Hit-Test Is Plumbing; Drag/Resize/Cursor Policy Is the Controller's
- [ADR-019](ADR-019-vendor-plugin-aux-boundary.md) — Aux Library Boundary for Vendor Plug-in DLLs
- [ADR-020](ADR-020-plugin-abi-compatibility-policy.md) — Plug-in ABI Compatibility Policy (versioning, `struct_size` negotiation, drift guards)
- [ADR-021](ADR-021-color-management-encoding-state-invariant.md) — Color Management & the Encoding-State Invariant
- [ADR-022](ADR-022-per-mode-capability-flags-frozen-enum-structs.md) — Per-Mode Capability Flags + Frozen Enumerated App Structs
- [ADR-023](ADR-023-unified-atlas-capture.md) — Unified Atlas Capture (XR_EXT_atlas_capture)
- [ADR-024](ADR-024-raw-vs-render-ready-views.md) — Raw vs Render-Ready Views (XR_EXT_view_rig)
- [ADR-025](ADR-025-android-vendor-dp-out-of-process.md) — Android Vendor Display Processors Run Out-of-Process
- [ADR-026](ADR-026-orientation-aware-view-scaling.md) — Orientation-Independent Rendering Modes, Config-Derived View Scale, Rotation-Aware Worst-Case Atlas
- [ADR-027](ADR-027-display-zones.md) — Display Zones — decoupled mixed 2D/3D layout, per-zone rig, wish mask
- [ADR-028](ADR-028-display-mode-recipe-vs-hardware-state.md) — The rendering mode is the content recipe; the hardware state is an orthogonal override
- [ADR-029](ADR-029-client-owned-transparent-ipc-present.md) — The IPC client owns the transparent present; the DP reconstructs alpha, never bakes a background
- [ADR-030](ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md) — Compositor Crops to Content; Zero-Copy Only When the Swapchain Equals the Mode Atlas
- [ADR-031](ADR-031-remove-surround-output-rect-zones-sole-region-model.md) — Remove the 2D-surround / output-rect mechanism — display-zones is the sole region paradigm
- [ADR-032](ADR-032-array-layered-swapchains-first-class.md) — Array (Layered) Swapchains Are First-Class Alongside the Tiled Atlas
