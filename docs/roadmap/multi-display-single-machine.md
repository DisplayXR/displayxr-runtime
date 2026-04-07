---
status: Proposal
owner: David Fattal
updated: 2026-03-15
issues: [69]
code-paths: [src/xrt/drivers/, src/xrt/compositor/multi/]
---

> **Status: Proposal** — not yet implemented. Tracking issue: [#69](https://github.com/DisplayXR/displayxr-runtime-pvt/issues/69)

# Multi-Display Compositing on a Single Machine

## Summary

Extend the multi-compositor to route layers to multiple physical displays connected to the same machine. Each display has a known pose in a shared room-scale coordinate system.

## Scope and Related Docs

This doc extends the spatial OS compositor to **multiple physical displays on one machine**. It assumes the single-display compositing pipeline and shell already work.

| Doc | Relationship |
|-----|-------------|
| [spatial-os.md](spatial-os.md) (#43) | **Single-display compositing.** The multi-compositor pipeline this doc extends. Prerequisite. |
| [3d-shell.md](3d-shell.md) (#44) | **Window manager.** Manages window placement; this doc adds display-routing beneath it. Prerequisite. |
| [multi-display-networked.md](multi-display-networked.md) (#70) | **Networked extension.** Generalizes this doc's multi-display routing across machines. Depends on this doc. |

## Architecture

```
                                    +-> native compositor -> Display A (pose A)
Apps -> IPC -> multi-compositor ----+-> native compositor -> Display B (pose B)
                                    +-> native compositor -> Display C (pose C)
```

One `displayxr-service` process discovers and manages all local displays. The multi-compositor treats each as a separate render target with its own pose.

## Tasks

- [ ] Enumerate multiple physical displays from a single service instance (multiple Leia/sim_display devices)
- [ ] Define per-display pose configuration (JSON config or calibration)
- [ ] Route compositor layers to the correct display(s) based on window pose vs display frustum
- [ ] Handle windows spanning multiple displays (split compositing)
- [ ] Frame synchronization across local displays

## Dependencies

- #43 (multi-compositor)
- #44 (spatial window manager)

## Context

This is step 2a of the multi-display architecture. All displays are local to one machine, one process, shared memory -- no network involved.
