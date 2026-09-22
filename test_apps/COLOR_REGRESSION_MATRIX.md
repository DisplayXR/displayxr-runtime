<!--
Copyright 2026, Leia Inc.
SPDX-License-Identifier: BSL-1.0
-->
# Color-management regression matrix (ADR-021)

Manual verification for the encoding-state contract and Model A/B. Closes the
ADR-021 verification gap ("no test app renders true-linear / honest-sRGB"). The
multi-layer-blend and workspace cases need the live display + shell, so this is a
manual procedure, not an automated test.

## Driving the encoding axis

The cube test apps select their color swapchain via `DXR_SWAPCHAIN_ENCODING`
(handled in `common/xr_session_common.cpp::SelectColorSwapchainFormat`):

| Value | Swapchain | Bytes reaching the runtime | Exercises |
|---|---|---|---|
| `srgb` | an advertised `*_SRGB` format | GPU auto-encodes on the per-frame RTV write → **honest encoded**; runtime sets `atlas_holds_srgb_bytes=true` | the **Model B decode leg** when ≥2 such clients blend |
| `unorm` | a plain UNORM format | app writes raw → **encoded-into-UNORM** (today's default) / **true-linear-into-UNORM** (content = whatever the renderer wrote) | Model A passthrough |
| unset | runtime-preferred (`formats[0]`) | unchanged | default behavior |

Set it process-level (the runtime DLL has its own static-CRT env block; use a real
env var, not a run-script line):
```cmd
set DXR_SWAPCHAIN_ENCODING=srgb && test_apps\handle\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

## Renderer-side linearize

`_SRGB` is the default colour swapchain since displayxr-common v2.15.0, and an
`_SRGB` render target **encodes on write**. The cube apps author
display-referred colour (their basecolor/AO textures are plain UNORM), so their
pixel/fragment shaders have to **decode** first or the authored colour is
encoded twice and washes out. That decision is `dxr::RenderSceneLinear()`
(`common/color_policy.h`); the background clear follows the same flag, because a
clear value is taken in the attachment's own space.

| App | Shader-variant selection | Clear colour |
|---|---|---|
| every D3D11 cube app (via the shared reference renderer) | both PS variants compiled up front (`DXR_LINEARIZE`), picked per draw by `CubePixelShaderForTarget()` / `GridPixelShaderForTarget()` | `ClearRenderTargetViewDisplayReferred()` |
| `cube_handle_vk_win`, `cube_zones_vk_win`, `cube_zones_texture_vk_win`, `cube_hosted_legacy_vk_win` | one FS module; the `uLinearize` **specialization constant** (SpecId 0) is baked at `vkCreateGraphicsPipelines` from `dxr::RenderSceneLinear()` — `InitializeVkRenderer()` runs after `CreateSwapchain()`, so the noted format is already authoritative | `VkClearColorValue` RGB decoded through `dxr::DisplayReferredToSceneLinear()`; alpha is linear in both spaces and is never converted |

The VK apps log the latched decision once at init:
`WARN [color] colorFormat=<n> sceneLinear=yes|no (...)`. That line is the
discriminator when a capture looks washed out — it separates "the shader did not
decode" from "the swapchain is not the format you think it is", with no rebuild.

The Linux and macOS Vulkan cube apps pick their swapchain format with their own
enumeration rather than `dxr::ChooseColorSwapchainFormat()`, so they never take
the `_SRGB` default and are not part of this axis yet.

### `cube_handle_vk_win` rows

| Run | Swapchain | Shaders emit | Expected on screen |
|---|---|---|---|
| default (no env) | an advertised `*_SRGB` format | scene-linear (decoded) | identical to the pre-v2.15.0 UNORM build |
| `DXR_SWAPCHAIN_ENCODING=unorm` | plain UNORM | display-referred (raw) | identical again — the A/B escape hatch |
| `DXR_TRUE_LINEAR=0` (or `false` / `off` / `no`) with the default `_SRGB` | `*_SRGB` | display-referred (raw) | **washed out** — reproduces the double-encode on demand |
| `DXR_TRUE_LINEAR=1` + `DXR_SWAPCHAIN_ENCODING=unorm` | plain UNORM | scene-linear | **too dark** pre-DP — the true-linear-into-UNORM cell |

## Matrix

`{sRGB swapchain, UNORM-encoded, true-linear} × {single-layer opaque, multi-layer blend} × {in-process, IPC, workspace}`

- **Encoding** → `DXR_SWAPCHAIN_ENCODING` (`srgb` / `unorm`). "true-linear" = `unorm` with the renderer writing linear radiance.
- **Single-layer** → one app. **Multi-layer blend** → two overlapping windows under the shell (or a transparent-bg app: `DISPLAYXR_TRANSPARENT_BG=1`).
- **Transport** → in-process (default), IPC (`set XRT_FORCE_MODE=ipc` + running `displayxr-service.exe`), workspace (launch under `displayxr-shell.exe`).

Model B engages **only** in the workspace/service multi-layer column with honest-`srgb` clients against a `LINEAR`/`EITHER` DP; every other cell is Model A passthrough (B == A when nothing blends), so the in-process fast path is identical across encodings.

## In-repo DP test double

`sim_display`'s **D3D11** variant declares `XRT_DP_COLOR_EITHER` and applies the
standard sRGB OETF on output when the runtime declares a `LINEAR` atlas
(`out_encode()` in `sim_display_processor_d3d11.cpp`). It is the hardware-free
exerciser of the Model-B encode-at-handoff direction — register the freshly built
`DisplayXR-SimDisplay.dll` and run two `DXR_SWAPCHAIN_ENCODING=srgb` cubes under
the shell. (The VK/GL/Metal sim_display variants are in-process-only and stay
`ENCODED` passthrough.)

## Capture verification

The D3D11 service compositor captures the combined atlas **post-compose, pre-DP**:
```bash
rm -f "$TEMP/workspace_screenshot_atlas.png"
touch "$TEMP/workspace_screenshot_trigger"   # wait ~3s, then read the PNG
```
- **No double-encode (Model A):** cubes are UNORM-encoded; the captured atlas bytes must equal the app's output (no ~2.2× darkening).
- **Model B:** the captured atlas is intentionally **linear** (numerically darker) — the encode happens *after* capture, in the DP — so a dark pre-DP atlas under B is expected, not a regression. Confirm the on-screen (post-DP) result is correct by eyeballing the live display (screenshots during eye-tracking warmup miss UI).
