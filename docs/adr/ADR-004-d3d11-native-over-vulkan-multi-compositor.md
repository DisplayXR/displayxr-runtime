---
status: Superseded
date: 2026-02-28
source: "vendor_abstraction_refactor.md §8"
superseded-by: "ADR-001, docs/architecture/multi-compositor.md"
---
# ADR-004: D3D11 Native Over Vulkan Multi-Compositor

## Status

**Superseded.** Both the single-app and the multi-app/service paths use native graphics-API
compositors with no cross-API intermediary. On Windows, multi-app/service compositing is the
**D3D11 service multi-compositor** (`src/xrt/compositor/d3d11_service/`); macOS uses the Metal
equivalent. See [`docs/architecture/multi-compositor.md`](../architecture/multi-compositor.md) and
[ADR-001](ADR-001-native-compositors-per-graphics-api.md).

## Context

On Windows with a vendor whose SDK ships a D3D11 weaver, the D3D11 native compositor provides
direct access to that weaver.

## Decision

For windowed 3D-display apps on Windows, use the D3D11 native compositor path. The multi-app /
service path is the D3D11 service multi-compositor (`compositor/d3d11_service/`).

## Consequences

Best latency and simplest integration; direct vendor-weaver access. Multi-app works through the IPC
path into the D3D11 service multi-compositor.
