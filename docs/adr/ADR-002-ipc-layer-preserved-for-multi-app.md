---
status: Accepted
date: 2026-01-15
source: "#23"
---
# ADR-002: IPC Layer Preserved for Multi-App

## Context
The Monado IPC layer (client compositor -> IPC transport -> multi-compositor -> server compositor) was designed for VR service mode. During the lightweight runtime cleanup, the question arose whether to remove it.

## Decision
Preserve the IPC layer (`ipc/`, `compositor/client/`, `compositor/multi/`) for WebXR, multi-app spatial shell, and out-of-process compositor scenarios.

## Consequences
IPC code remains in the repo even though most test apps use in-process compositors. The `_ipc` app class exercises this path. Foundation for spatial shell (#43, #44).
