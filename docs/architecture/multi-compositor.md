---
status: Active
owner: David Fattal
updated: 2026-05-31
code-paths: [src/xrt/compositor/multi/, src/xrt/compositor/d3d11_service/, src/xrt/ipc/]
---
# Service-Mode Multi-Compositor

The multi-compositor is the **server-side compositor for out-of-process (service-mode) sessions**: it
imports the rendered output of N per-app native compositors — each running in its own process — and
composites them into a single lightfield frame for one display.

It is a **mechanism**, independent of any window-placement policy. It backs every out-of-process consumer
(see [Consumers](#consumers)); the spatial **workspace** is one of them, layered on top via a
`workspace_mode` flag. Where windows go, how they animate, and what chrome looks like is the controller's
policy — see [separation-of-concerns.md](separation-of-concerns.md) and the
[`XR_EXT_spatial_workspace`](../specs/extensions/XR_EXT_spatial_workspace.md) surface. For the **single-app
(in-process)** pipeline, see [compositor-pipeline.md](compositor-pipeline.md).

## Topology

```
App A (D3D11)    App B (GL)    App C (VK)    App D (Metal)     <- separate processes
     |               |             |             |
  D3D11 comp      GL comp       VK comp      Metal comp        <- per-app native compositor
     |               |             |             |
  shared tex      shared tex    shared tex   shared tex        <- DXGI shared handle / IOSurface
     +-------+-------+------+------+------+------+
                          |
              Multi-Compositor (server)                        <- one per display, in the service
              import · composite at poses · crop
                          |
                  Display Processor (weave)
                          |
                       Display
```

## Per-app compositor vs multi-compositor split

Each native compositor keeps its **app-facing** half and loses its **display-facing** half; the
multi-compositor takes over the latter. The `xrt_display_processor` abstraction already separates weaving
from compositing ([ADR-007](../adr/ADR-007-compositor-never-weaves.md)), so the split is clean.

| Stage | Single-app (in-process) | Multi-app (service mode) |
|---|---|---|
| Swapchain management | native compositor | native compositor (same) |
| Per-app layer compositing (overlays, HUD) | native compositor | native compositor (same) |
| Shared-texture export | — | native compositor (adds) |
| Composite N clients at their window poses | — | **multi-compositor** |
| Crop to the active mode's tile layout | native compositor | **multi-compositor** |
| Display-processor weave + present | native compositor | **multi-compositor** |

Cross-process texture sharing uses the platform-native primitive per API — DXGI shared handles
(D3D11/D3D12), IOSurface (Metal), and the GL/VK shared-texture paths — never a Vulkan intermediary
([ADR-001](../adr/ADR-001-native-compositors-per-graphics-api.md)). The client→service handoff is
synchronised with a keyed mutex (D3D11 source) or a shared fence (D3D12).

## Per-window parallax

Each app renders **tiled multiview** content (not pre-composited SBS) into its swapchain. The runtime hands
each app eye positions already transformed into its **own window's** local frame, so the per-window
parallax is correct at render time and the multi-compositor does **not** re-derive geometry — it composites
the pre-rendered view tiles. This window-relative Kooima projection is
[ADR-012](../adr/ADR-012-window-relative-kooima-projection.md); the projection math itself is in
[kooima-projection.md](kooima-projection.md).

The multi-compositor blits each client's view tiles into a combined atlas at the controller-specified
window pose, honouring per-tile alpha, then crops to the active mode's `tile_columns × tile_rows` layout and
hands the atlas to the display processor's `process_atlas()`. The atlas/tiling and color-space details (and
the load-bearing shell-mode vs non-shell invariants) live in
[compositor-pipeline.md](compositor-pipeline.md); the slot/stride rules in
[multiview-tiling.md](../specs/runtime/multiview-tiling.md).

| Window type | Per-app render | Multi-compositor |
|---|---|---|
| OpenXR multiview app | tiled views, window-relative Kooima | composite view tiles at the window pose |
| Captured 2D app (OS-window snapshot) | flat texture (no app render) | composite as a flat panel at the window pose |

A 2D captured panel still gets head-tracked parallax from its 3D pose; its *content* is flat, which is the
correct behaviour for a 2D window floating at a depth.

## Why platform-native (not Vulkan)

The original Monado multi-compositor (`compositor/multi/`) is deeply Vulkan-coupled, which would force every
API through Vulkan interop for final compositing — the exact cost this runtime exists to eliminate. The
service multi-compositor is therefore platform-native: D3D11 on Windows (`d3d11_service`), Metal on macOS.
All graphics APIs export a native shared texture the server imports directly.
See [ADR-001](../adr/ADR-001-native-compositors-per-graphics-api.md).

## Consumers

The multi-compositor is shared by every out-of-process consumer; `workspace_mode` gates only the
workspace-specific composition (controller poses, chrome, per-tile alpha):

- **Spatial workspace** (the DisplayXR Shell and other workspace controllers) — `workspace_mode = true`;
  windows placed via [`XR_EXT_spatial_workspace`](../specs/extensions/XR_EXT_spatial_workspace.md).
- **Plain service-mode IPC apps** — the `_ipc` app class: one or more app processes connect to a single
  runtime service and composite through the same path, with no workspace controller.
- **WebXR bridge** — bridges browser WebXR sessions through the `d3d11_service` compositor.

## Further reading

- [compositor-pipeline.md](compositor-pipeline.md) — single-app pipeline + atlas/color-space details
- [separation-of-concerns.md](separation-of-concerns.md) — layer boundaries; runtime mechanism vs controller policy
- [in-process-vs-service.md](in-process-vs-service.md) — when the service path is used
- [workspace-runtime-contract.md](../roadmap/workspace-runtime-contract.md) — the controller ↔ runtime IPC contract
- [ADR-001](../adr/ADR-001-native-compositors-per-graphics-api.md), [ADR-007](../adr/ADR-007-compositor-never-weaves.md), [ADR-012](../adr/ADR-012-window-relative-kooima-projection.md)
