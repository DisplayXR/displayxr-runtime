---
status: Active
owner: David Fattal
updated: 2026-08-16
code-paths: [src/xrt/compositor/d3d11_service/, src/xrt/compositor/multi/, src/xrt/ipc/]
---
# Service-Mode Multi-Compositor

The multi-compositor is the **server-side compositor for out-of-process (service-mode) sessions**: it
imports the rendered output of N per-app native compositors — each running in its own process — and
composites them into a single lightfield frame for one display.

It is a **mechanism**, independent of any window-placement policy. It backs every out-of-process consumer
(see [Consumers](#consumers)); the spatial **workspace** is one of them, layered on top via a
`workspace_mode` flag. Where windows go, how they animate, and what chrome looks like is the controller's
policy — see [separation-of-concerns.md](separation-of-concerns.md) and the
[`XR_DXR_spatial_workspace`](../specs/extensions/XR_DXR_spatial_workspace.md) surface. For the **single-app
(in-process)** pipeline, see [compositor-pipeline.md](compositor-pipeline.md).

## Which implementation

"Multi-compositor" names a role, and two different implementations fill it. On
Windows the service compositor is `comp_d3d11_service`
(`src/xrt/targets/common/target_instance.c:245-260` calls
`comp_d3d11_service_create_system()`). `compositor/multi/` (`comp_multi`) is the
system compositor on macOS, Linux, and Android, reached through the null
compositor (`null_compositor.c:1089`); on Windows it contributes only
`comp_multi_workspace.c` — a chrome/window-pose state registry — and none of the
compositing. `ipc_server_handler.c` states the split directly: "D3D11 service on
Windows, null+comp_multi on Android/macOS". Both implementations, and the two
modes of the Windows one, are described in
[service-architecture.md](service-architecture.md).

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
synchronised with a keyed mutex (D3D11 source) or a shared fence (D3D12); the D3D11 leg also has a
shared-fence model (`workspace_sync_fence`), not only KeyedMutex.

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
  windows placed via [`XR_DXR_spatial_workspace`](../specs/extensions/XR_DXR_spatial_workspace.md).
- **Plain service-mode IPC apps** — the `_ipc` app class. On Windows these do **not** go through the
  multi-compositor: without an active workspace controller each IPC client takes the **standalone** path
  and presents its **own** per-client swap chain through its own display processor
  (`comp_d3d11_service.cpp`, `init_client_render_resources`, :3559-4086). The multi-compositor exists only
  under workspace mode.
- **Chrome / Edge WebXR** — frames come from Chrome's own OpenXR client (AppContainer → IPC), which is a
  regular IPC client of the service.
- **WebXR bridge** — **headless, metadata-only, submits no frames** (`webxr_bridge/main.cpp`, session
  created with `XR_MND_headless`). It carries display info, modes, eye poses, and input; it is not a
  compositing consumer.
- **`displayxr-browser`** — a present-owner via `XR_DXR_weave`.

Having two structurally different service compositor modes is the arbitration hole tracked as #939;
[ADR-035](../adr/ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md) targets a single
always-on pipeline instead.

## Further reading

- [compositor-pipeline.md](compositor-pipeline.md) — single-app pipeline + atlas/color-space details
- [separation-of-concerns.md](separation-of-concerns.md) — layer boundaries; runtime mechanism vs controller policy
- [in-process-vs-service.md](in-process-vs-service.md) — when the service path is used
- [workspace-runtime-contract.md](../roadmap/workspace-runtime-contract.md) — the controller ↔ runtime IPC contract
- [ADR-001](../adr/ADR-001-native-compositors-per-graphics-api.md), [ADR-007](../adr/ADR-007-compositor-never-weaves.md), [ADR-012](../adr/ADR-012-window-relative-kooima-projection.md)
