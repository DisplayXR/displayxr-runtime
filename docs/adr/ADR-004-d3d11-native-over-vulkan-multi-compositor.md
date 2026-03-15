---
status: Accepted
date: 2026-02-28
source: "vendor_abstraction_refactor.md §8"
---
# ADR-004: D3D11 Native Over Vulkan Multi-Compositor

## Context
On Windows with LeiaSR hardware, the D3D11 native compositor provides direct access to the SR D3D11 weaver. The Vulkan multi-compositor path requires Vulkan->D3D11 interop or a Vulkan weaver.

## Decision
For windowed 3D display apps on Windows, prefer the D3D11 native compositor path. The Vulkan multi-compositor path is reserved for IPC/service mode and multi-app scenarios.

## Consequences
Best latency and simplest integration for single-app Windows scenarios. Multi-app still works through IPC path.
