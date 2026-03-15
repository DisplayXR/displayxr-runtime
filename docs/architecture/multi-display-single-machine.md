---
status: Proposal
owner: David Fattal
updated: 2026-03-15
issues: [69]
code-paths: [src/xrt/drivers/, src/xrt/compositor/multi/]
---

# Multi-Display Compositing on a Single Machine

## Summary

Extend the multi-compositor to route layers to multiple physical displays connected to the same machine. Each display has a known pose in a shared room-scale coordinate system.

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
