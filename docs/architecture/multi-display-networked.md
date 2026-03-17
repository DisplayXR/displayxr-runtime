---
status: Proposal
owner: David Fattal
updated: 2026-03-15
issues: [70]
code-paths: [src/xrt/ipc/]
---

# Multi-Display Compositing Across Networked Machines

## Summary

Extend the multi-compositor to drive displays on remote machines over the network. One primary service owns the world model and makes all compositing decisions. Satellite services on other machines expose their local displays as remote render targets.

## Architecture

```
Machine A (primary)                    Machine B (satellite)
  Apps -> IPC -> multi-compositor ---->  compositor proxy -> native compositor -> Display B
                    |
                    +-> native compositor -> Display A
```

The primary's multi-compositor treats remote displays identically to local ones -- same compositing logic, different transport (network instead of shared memory).

## Scope and Related Docs

This doc extends multi-display compositing **across networked machines**. It assumes local multi-display routing already works.

| Doc | Relationship |
|-----|-------------|
| [spatial-os.md](spatial-os.md) (#43) | **Single-display compositing.** Foundation pipeline. |
| [3d-shell.md](3d-shell.md) (#44) | **Window manager.** Owns window placement and interaction. |
| [multi-display-single-machine.md](multi-display-single-machine.md) (#69) | **Local multi-display.** This doc generalizes its routing over the network. Prerequisite. |

## Design: Hub with Remote Compositor Targets

- **Primary service**: Runs the multi-compositor, owns the world model (display poses, window placement, z-ordering), makes all compositing decisions.
- **Satellite service**: Thin mode of `displayxr-service` that connects to a primary, exposes local display(s) as remote targets, receives composited frames.
- **Network transport**: Generalize IPC transport layer to work over TCP/WebSocket in addition to named pipes/Unix sockets.
- **Frame sync**: Primary controls frame timing, pushes viewport data to satellites in sync.

## Key Use Cases

1. **Spanning content**: 3D movie across multiple displays on different machines -- primary composites the single source and pushes each display's viewport with frame-perfect sync.
2. **Floating windows**: App on Machine B connects to local satellite service, which proxies to primary. Primary decides which display(s) show it, composites accordingly.
3. **Shared workspace**: Two laptops side by side forming a unified spatial desktop.

## Tasks

- [ ] Abstract IPC transport to support network (TCP/WebSocket) alongside local pipes/sockets
- [ ] Implement satellite mode for `displayxr-service` (connect to primary, expose local displays)
- [ ] Implement remote display target in multi-compositor (send composited frames over network)
- [ ] Content streaming optimization (compression, GPU-direct transfer, NDI, etc.)
- [ ] Frame synchronization protocol across network
- [ ] Satellite discovery (mDNS/Bonjour for zero-config LAN setup)
- [ ] Display pose configuration across machines (shared room-scale coordinate system)

## Dependencies

- #43 (multi-compositor)
- #44 (spatial window manager)
- #69 (multi-display on single machine)

## Context

This is step 2b of the multi-display architecture. Builds on the single-machine multi-display work by generalizing the transport. This same architecture scales to cross-runtime interop (step 3): external XR runtimes could connect as a different kind of satellite, consuming the world model to render displays in VR.
