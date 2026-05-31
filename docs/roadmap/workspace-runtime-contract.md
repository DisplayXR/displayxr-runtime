---
status: Proposal
owner: David Fattal
updated: 2026-03-21
issues: [43, 44]
code-paths: [src/xrt/compositor/multi/, src/xrt/ipc/]
---

> **Status: Proposal** — not yet implemented. Tracking issue: [#43](https://github.com/DisplayXR/displayxr-runtime/issues/43), [#44](https://github.com/DisplayXR/displayxr-runtime/issues/44)

# Workspace / Runtime Contract

## Scope and Related Docs

This doc defines the **IPC message set** between a workspace controller and the spatial runtime. The DisplayXR Shell is our reference workspace controller, but the contract is the boundary — any privileged IPC client implementing it can play the role (verticals, kiosks, OEM-branded workspaces, AI-agent drivers).

The boundary exists so rendering machinery does not drift into workspace-controller code, and UX policy does not drift into runtime code.

| Doc | Relationship |
|-----|-------------|
| [spatial-os.md](spatial-os.md) (#43) | **Compositing mechanism.** The runtime side of this contract — multi-compositor, shared textures, display processing. |
| [3d-shell.md](3d-shell.md) (#44) | **Reference workspace controller.** The DisplayXR Shell side of this contract — window placement, chrome, interaction. |
| [3d-capture.md](3d-capture.md) | **Capture pipeline.** Capture commands and completion events flow through this contract. |
| [spatial-workspace-extensions-plan.md](spatial-workspace-extensions-plan.md) | **Extension plan.** Roadmap for promoting this contract into the public `XR_DISPLAYXR_spatial_workspace` + `XR_DISPLAYXR_app_launcher` extensions. |

## Purpose

Define a clean boundary so:
- Rendering machinery does not drift into workspace-controller code
- UX policy does not drift into runtime code
- The workspace controller can be replaced (OEM-branded workspaces, vertical-specific cockpits, kiosks, AI-agent drivers) without touching the runtime
- The runtime can evolve its compositing internals without breaking workspace-controller compatibility

## Boundary Rule

- **Runtime implements rendering truth** — compositing, projection, weaving, capture, cursor-sprite compositing at controller-supplied depth
- **Workspace controller implements desktop behavior** — placement policy, chrome, focus, persistence, launcher, **and hit-testing** (eye→cursor raycast; moved out of the runtime in spec_version 22, issue #370)
- **Apps require no SDK** — a normal OpenXR handle app works inside any workspace with zero code changes (Level 0 universal app, see [spatial-desktop-prd.md § 5.1](spatial-desktop-prd.md)). Workspace-aware extensions (Level 1) and spatial UI toolkits (Level 2) are optional enhancements, never requirements.

## Controller → Runtime (Control Path)

The workspace controller must be able to send:

| Message | Description | Status |
|---------|-------------|--------|
| **`workspace_activate`** | Enter workspace mode (multi-comp takes over DP + display) | Implemented |
| **`workspace_deactivate`** | Exit workspace mode (per-client compositors resume direct rendering) | Phase 4D |
| **`workspace_set_window_pose`** | Position, orientation, size of a window quad in 3D space | Implemented |
| **`workspace_get_window_pose`** | Query current window transform | Implemented |
| **`workspace_set_window_visibility`** | Show/hide a window without destroying it (minimize) | Implemented |
| **`workspace_add_capture_client`** | Adopt a 2D OS window: runtime starts `Windows.Graphics.Capture` for the given HWND, returns client_id | Phase 4A |
| **`workspace_remove_capture_client`** | Stop capturing a 2D window, remove virtual client slot | Phase 4A |
| **Capture commands** *(future)* | Start/stop frame capture, recording, session capture | Phase 5+ |
| **Layout updates** | Batch update of multiple window transforms (layout preset apply) | Phase 2+ |

## Runtime → Controller (Service Path)

The runtime must be able to expose:

| Message | Description | Status |
|---------|-------------|--------|
| **`system_get_clients`** | List of connected IPC apps (id, name, state) | Implemented |
| **`workspace_get_client_type`** | Returns `CLIENT_TYPE_OPENXR_3D` or `CLIENT_TYPE_CAPTURED_2D` | Phase 4A |
| **Hit-test results** | ~~Which window a mouse ray intersects, and where on the surface~~ | **Removed (spec_version 22, #370)** — hit-test is now controller-owned; the runtime instead delivers the OS cursor position on FRAME_TICK and accepts cursor depth via `workspace_set_cursor_depth` |
| **App presence** | App connected/disconnected, session created/destroyed | Implemented (poll-based) |
| **Display state** | Display resolution, refresh rate, capabilities | Implemented (shared memory) |
| **Tracking state** | Eye tracking active/lost, quality metrics | Implemented (shared memory) |
| **Capture completion** *(future)* | Capture finished, with metadata (path, format, dimensions) | Phase 5+ |

## Transport Options

The contract is transport-agnostic. Implementation options:

1. **Privileged IPC** — The workspace controller connects as a privileged client over the existing IPC infrastructure (`src/xrt/ipc/`). Controller messages are additional IPC calls with elevated permissions. **(Current implementation.)**

2. **Custom OpenXR extension** — `XR_DISPLAYXR_spatial_workspace` (window pose / focus / capture) and `XR_DISPLAYXR_app_launcher` (launcher tile registry), used by the controller as a regular OpenXR client with privileged capabilities. **(Phase 2 target — see [spatial-workspace-extensions-plan.md](spatial-workspace-extensions-plan.md).)**

3. **Platform service abstraction** — Platform-specific mechanism (e.g., named pipes on Windows, XPC on macOS) if OpenXR extension overhead is too high for real-time window pose updates.

The privileged IPC path is the most natural fit given the existing architecture — the IPC layer already handles multi-app coordination, and adding shell-privileged messages is incremental.

## Repo Boundary and SDK

The shell and runtime live in separate repositories. The shell builds against a stable SDK exported by the runtime.

| Repo | Visibility | Owns |
|------|-----------|------|
| `displayxr-runtime` | Public | Multi-compositor, capture mechanism, IPC protocol, SDK export |
| `displayxr-shell` | Private | Window adoption, layout policy, launcher, persistence, spatial companion UX |
| `displayxr-shell-releases` | Public | Binary-only shell releases |

**SDK surface (exported by runtime):**

| Component | Contents |
|-----------|----------|
| Headers | `xrt/*.h` (core types), `ipc/client/*.h` (client API), `ipc/shared/*.h` (protocol), `util/u_logging.h` |
| Libraries | `ipc_client.lib` (static), `ipc_shared.lib` (static), `aux_util.lib` (static) |
| CMake | `DisplayXRSDKConfig.cmake` package config |

The shell links statically — at runtime, the shell exe is fully standalone. It finds the service via named pipe (no library or path dependency on the runtime installation).

**Capture code lives in the runtime** because `Windows.Graphics.Capture` must run on the same D3D11 device as the multi-compositor. The shell tells the runtime which HWNDs to capture; the runtime handles all GPU work. This preserves the mechanism/policy split.

## Design Constraints

- Shell must not call into compositor internals directly — all communication goes through the contract
- Runtime must not make UX decisions (e.g., where to place a new window) — it exposes primitives, shell decides policy
- Contract must support multiple shells simultaneously in the future (e.g., spatial shell + accessibility overlay)
- Latency-sensitive messages (window pose during drag) may need a fast path separate from general IPC
- Runtime and shell are independently installable — runtime has standalone value without the shell
- SDK is the only build-time coupling — shell never includes runtime source directly
