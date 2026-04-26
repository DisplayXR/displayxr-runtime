---
status: Accepted
date: 2026-03-24
source: "#91"
---
# ADR-011: D3D11 Swapchain Textures Must Use TYPELESS Format

## Context
Unity D3D11 builds produced a black screen while D3D12 worked perfectly. After exhaustive runtime-side diagnostics (10+ builds, CPU pixel readback confirming swapchain textures were never written to), the root cause was identified: the OpenXR specification requires D3D11 runtimes to create swapchain textures with **TYPELESS format variants** so that applications can create their own typed views (RTVs, SRVs, UAVs).

Our D3D11 compositor was creating textures with concrete formats (e.g., `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`). Unity's OpenXR plugin expects TYPELESS textures and creates typed RTVs from them — when the source texture has a concrete format, the RTV creation silently fails and Unity never renders to the texture.

The Khronos `hello_xr` reference implementation confirms this requirement:
> *"OpenXR will return to you TYPELESS textures, so be sure to handle typing accordingly when you create your views."*

D3D12 does not have this issue because its format casting model allows creating typed views from concrete-format resources.

## Decision
D3D11 swapchain textures are created with TYPELESS format variants. The existing `d3d_dxgi_format_to_typeless_dxgi()` helper (in `src/xrt/auxiliary/d3d/d3d_dxgi_formats.h`) maps concrete formats to their TYPELESS equivalents:

| App requests | Texture created with | Views use |
|---|---|---|
| `R8G8B8A8_UNORM_SRGB` | `R8G8B8A8_TYPELESS` | `R8G8B8A8_UNORM_SRGB` |
| `R8G8B8A8_UNORM` | `R8G8B8A8_TYPELESS` | `R8G8B8A8_UNORM` |
| `B8G8R8A8_UNORM` | `B8G8R8A8_TYPELESS` | `B8G8R8A8_UNORM` |
| `R16G16B16A16_FLOAT` | `R16G16B16A16_TYPELESS` | `R16G16B16A16_FLOAT` |

Format enumeration (`xrEnumerateSwapchainFormats`) continues to return concrete formats per the OpenXR spec. Only the internal `CreateTexture2D` call uses TYPELESS.

All view creation (RTVs, SRVs) — both in the compositor and in applications — must specify the concrete format explicitly via view descriptors. `nullptr` descriptors are not valid for TYPELESS textures.

Depth textures already used TYPELESS prior to this change and are unaffected.

## Consequences
- **Unity D3D11 works** — the original black screen issue is resolved.
- **All D3D11 apps must create typed views** — applications that previously relied on `nullptr` view descriptors (inferring format from the texture) must now pass explicit `D3D11_RENDER_TARGET_VIEW_DESC` / `D3D11_SHADER_RESOURCE_VIEW_DESC` with the concrete format. Our test apps were updated accordingly.
- **Internal compositor textures are mostly unaffected** — the renderer's atlas, HUD, and display processor input textures remain concrete format. *Exception:* the **shell-mode per-client atlas** (`d3d11_client_render_resources::atlas_texture` when `sys->shell_mode == true`) was later switched to `R8G8B8A8_TYPELESS` for the same cross-format-view reason as app swapchains: it needs both a UNORM SRV and a UNORM_SRGB SRV onto the same bytes, which D3D11 only allows on TYPELESS storage. Non-shell mode keeps the atlas at concrete UNORM. See [Compositor Pipeline §Color-space handling](../architecture/compositor-pipeline.md#color-space-handling-d3d11-service-compositor-shell-mode) and `webxr-bridge-color-shift-plan.md` for the motivation (DP-side reinterpretation of SRGB-source vs UNORM-source per-client atlases).
- **Matches Khronos reference behavior** — aligns with `hello_xr` and other conformant OpenXR runtimes.
