---
status: Proposal
owner: David Fattal
updated: 2026-03-21
issues: [43, 44]
code-paths: [src/xrt/compositor/multi/, src/xrt/ipc/]
---

> **Status: Proposal** — not yet implemented. Tracking issue: [#43](https://github.com/dfattal/openxr-3d-display/issues/43), [#44](https://github.com/dfattal/openxr-3d-display/issues/44)

# Shell / Runtime Contract

## Scope and Related Docs

This doc defines the **IPC message set** between the 3D shell and the spatial runtime. It establishes a clean boundary so rendering machinery does not drift into shell code, and UX policy does not drift into runtime code.

| Doc | Relationship |
|-----|-------------|
| [spatial-os.md](spatial-os.md) (#43) | **Compositing mechanism.** The runtime side of this contract — multi-compositor, shared textures, display processing. |
| [3d-shell.md](3d-shell.md) (#44) | **Shell layer.** The shell side of this contract — window placement, chrome, interaction. |
| [3d-capture.md](3d-capture.md) | **Capture pipeline.** Capture commands and completion events flow through this contract. |

## Purpose

Define a clean boundary so:
- Rendering machinery does not drift into shell code
- UX policy does not drift into runtime code
- The shell can be replaced (OEM shells, alternate shells, kiosk mode) without touching the runtime
- The runtime can evolve its compositing internals without breaking shell compatibility

## Boundary Rule

- **Runtime implements rendering truth** — compositing, projection, weaving, capture, hit testing
- **Shell implements desktop behavior** — placement policy, chrome, focus, persistence, launcher

## Shell → Runtime (Control Path)

The shell must be able to send:

| Message | Description |
|---------|-------------|
| **Window pose** | Position, orientation, scale of a window quad in 3D space |
| **Window scale** | Resize factor for a window surface |
| **Visibility** | Show/hide a window without destroying it |
| **Focus / activation** | Which window should receive input |
| **Z-order hints** | Ordering preference when windows overlap |
| **Capture commands** | Start/stop frame capture, recording, session capture |
| **Layout updates** | Batch update of multiple window transforms (layout preset apply) |

## Runtime → Shell (Service Path)

The runtime must be able to expose:

| Message | Description |
|---------|-------------|
| **Hit-test results** | Which window a mouse ray intersects, and where on the surface |
| **Focusable surfaces** | List of active window surfaces available for focus |
| **Capture completion** | Capture finished, with metadata (path, format, dimensions) |
| **App presence** | App connected/disconnected, session created/destroyed |
| **Display state** | Display resolution, refresh rate, capabilities |
| **Tracking state** | Eye tracking active/lost, quality metrics |

## Transport Options

The contract is transport-agnostic. Implementation options:

1. **Privileged IPC** — Shell connects as a privileged client over the existing IPC infrastructure (`src/xrt/ipc/`). Shell messages become additional IPC calls with elevated permissions.

2. **Custom OpenXR extension** — `XR_EXT_spatial_window_management` extension that the shell uses as a regular OpenXR client with shell-specific capabilities.

3. **Platform service abstraction** — Platform-specific mechanism (e.g., named pipes on Windows, XPC on macOS) if OpenXR extension overhead is too high for real-time window pose updates.

The privileged IPC path is the most natural fit given the existing architecture — the IPC layer already handles multi-app coordination, and adding shell-privileged messages is incremental.

## Design Constraints

- Shell must not call into compositor internals directly — all communication goes through the contract
- Runtime must not make UX decisions (e.g., where to place a new window) — it exposes primitives, shell decides policy
- Contract must support multiple shells simultaneously in the future (e.g., spatial shell + accessibility overlay)
- Latency-sensitive messages (window pose during drag) may need a fast path separate from general IPC
