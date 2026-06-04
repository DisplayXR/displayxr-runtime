---
status: Accepted
date: 2026-01-15
source: "#23"
---
# ADR-002: IPC Layer Preserved for Multi-App

## Context
The IPC layer was designed for out-of-process service mode. During the lightweight runtime cleanup, the question arose whether to remove it.

## Decision
Preserve the IPC layer (`ipc/`, `compositor/client/`) for WebXR, multi-app spatial shell, and out-of-process compositor scenarios. Server-side multi-app compositing is the **D3D11 service multi-compositor** (`compositor/d3d11_service/`; Metal on macOS) — see [`docs/architecture/multi-compositor.md`](../architecture/multi-compositor.md).

## Consequences
IPC code remains in the repo even though most test apps use in-process compositors. The `_ipc` app class exercises this path. Foundation for spatial shell (#43, #44).
