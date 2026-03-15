---
status: Accepted
date: 2026-01-15
source: "#23"
---
# ADR-001: Native Compositors Per Graphics API

## Context
The original Monado architecture used a Vulkan server compositor as the single rendering backend, with interop layers for D3D11/D3D12/OpenGL. For tracked 3D displays, the Vulkan interop path adds latency, complexity, and doesn't support vendor SDK weavers that require native API access (e.g., LeiaSR D3D11 weaver).

## Decision
Each graphics API gets its own native compositor implementation (D3D11, D3D12, Metal, OpenGL, Vulkan). No Vulkan intermediary. Each compositor directly manages swapchains and rendering in its native API.

## Consequences
More compositor code to maintain (5 implementations vs 1), but each is simpler. Vendor display processors integrate natively. No interop overhead or texture copies. Each compositor follows the same vtable pattern via `comp_base`.
