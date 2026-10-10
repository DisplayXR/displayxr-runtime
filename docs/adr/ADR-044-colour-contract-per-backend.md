# ADR-044: The colour contract, per backend and swapchain format

**Status:** Proposed (2026-09-28); Metal row migrated 2026-10-09 (every in-process backend is now format-honest) · amends [ADR-021](ADR-021-color-management-encoding-state-invariant.md)
(principles unchanged; this records what shipped and replaces its "Model A = passthrough" reading
for app authors) · driven by [#1589](https://github.com/DisplayXR/displayxr-runtime/issues/1589) /
[#1610](https://github.com/DisplayXR/displayxr-runtime/issues/1610) (CTS) and the 2026-09-28
washed-out-demos regression · related: [INV-4.6](../guides/displayxr-app-rules.md),
[F-7](../guides/displayxr-app-rules.md), [compositor-pipeline](../architecture/compositor-pipeline.md),
[`test_apps/COLOR_REGRESSION_MATRIX.md`](../../test_apps/COLOR_REGRESSION_MATRIX.md)

## In one paragraph

**The swapchain's format says what its bytes mean. The runtime believes it.**
- An `_SRGB` swapchain holds **encoded** (display-referred) colour.
- Any other colour format (UNORM, float) holds **linear** values, and the runtime sRGB-encodes them on the way to the panel.

That is OpenXR's rule ("all other formats will be treated as linear values"), and the conformance tests judge it: `GradientFormatsLinearVsNonLinear` and the source-alpha blending tests. Every native compositor has followed it since v2.21.0–v2.21.7; Metal, the last one, since the release after v2.32.0 (§1).

An app that picks UNORM and stores already-encoded bytes gets them encoded twice, which looks **washed out**. The runtime is not wrong there. The app is, and the fix is the app's.

**The app rule:**

> **Write encoded colour into an `_SRGB` swapchain, or linear colour into a UNORM one. Prefer `_SRGB`, and make that choice the same way on every platform leg.**

## Context

ADR-021 set the principles:
- conversions come in matched pairs;
- the format is the source of truth;
- the display processor (DP) declares its handoff encoding.

ADR-021 also allowed a "Model A" in which the atlas is encoded and a UNORM swapchain was, *in practice*, a byte passthrough. App authors read that as "UNORM means display-referred", and the app guide said so explicitly ("a linear/UNORM swapchain is NOT color-managed"). Most apps therefore picked UNORM and wrote encoded bytes.

The Khronos CTS disagreed:

- **#1589.** `GradientFormatsLinearVsNonLinear` failed: a UNORM projection gradient came out dark because it was never encoded.
- **#1610.** `SourceAlphaBlending` and `SourceAlphaBlendingWithEnvironment` failed: layers blended in encoded space, not linear.

Fixing both made the runtime format-honest. Each backend shipped in its own release:

| Backend | Release |
|---|---|
| D3D11 in-process + service | v2.21.0 |
| D3D12 | v2.21.1 |
| GL | v2.21.2 |
| vk_native (748b540f3) | v2.21.7 |
| Metal (macOS) | the first release after v2.32.0 (`fix/metal-adr044-colour`) |

The in-tree Vulkan cube apps were migrated with the vk_native change (#1623, 0a6217a8d). The standalone demos were not. Their macOS/Linux legs had already moved to `_SRGB` for other reasons, but every Android leg and the avatar desktop legs still preferred UNORM. They went washed out on the first runtime ≥ v2.21.7 they met (Leia tablet, runtime v2.21.11, 2026-09-28).

The rule was right. What was missing was a single statement of it per backend, and a check that sees each leg of an app separately.

## Decision

### 1. The contract table

"Encoded" means standard sRGB (IEC 61966-2-1). "Atlas" is the tiled image handed to the DP's `process_atlas`.

| Backend (path) | Enumerated colour formats, in order | `_SRGB` swapchain | UNORM / float swapchain | Layers blend in | Atlas the DP receives | Since |
|---|---|---|---|---|---|---|
| **D3D11** in-process | `R8G8B8A8_UNORM`, `R8G8B8A8_UNORM_SRGB`, `B8G8R8A8_UNORM`, `B8G8R8A8_UNORM_SRGB`, `R16G16B16A16_FLOAT`, `R16G16B16A16_UNORM` | sampled through an `_SRGB` view (decode) | read as linear | linear, in a private `_SRGB`-RTV target (encodes on write) | **encoded** | v2.21.0 |
| **D3D11 service** (IPC clients of every API, workspace/shell) | same list, mapped per client API | decode on sample | linear | linear, private `_SRGB`-view target | **encoded**; **linear** under Model B (shell, ≥ 2 honest `_SRGB` clients, DP accepts linear) | v2.21.0 (+ #1591) |
| **D3D12** | same list as D3D11 | decode on sample | linear | linear, private `_SRGB`-view target | **encoded** | v2.21.1 |
| **OpenGL** (Windows, macOS) | `GL_RGBA8`, `GL_SRGB8_ALPHA8`, `GL_RGBA16F`, `GL_RGBA32F` | decode on sample | linear | linear, private `GL_SRGB8_ALPHA8` target | **encoded** | v2.21.2 |
| **Vulkan `vk_native`**: Windows, Linux, macOS (MoltenVK), **Android** | `B8G8R8A8_UNORM`, `B8G8R8A8_SRGB`, `R8G8B8A8_UNORM`, `R8G8B8A8_SRGB`, `R16G16B16A16_SFLOAT`, `R16G16B16A16_UNORM`, `A2B10G10R10_UNORM_PACK32` | the image **is** `_SRGB` (#1559); an `_SRGB` view decodes | linear | linear, private `MUTABLE_FORMAT` image through its `_SRGB` view; handoff by `vkCmdCopyImage`, never a blit | **encoded**, declared via `set_atlas_encoding` (#1484) | v2.21.7 |
| **Metal** (macOS; also Vulkan apps routed over it, whose Vulkan formats map 1:1 onto these) | `RGBA8Unorm`, `RGBA8Unorm_sRGB`, `BGRA8Unorm`, `BGRA8Unorm_sRGB`, `RGBA16Float`, `RGB10A2Unorm` | sampled as declared (the image's own `_sRGB` format decodes) | linear | linear, through a `BGRA8Unorm_sRGB` **view of the atlas itself** (`MTLTextureUsagePixelFormatView`; encodes on write). The view aliases the atlas storage, so the raw copy is the identity: the DP reads the encoded bytes through the UNORM atlas | **encoded**, declared via `set_atlas_encoding` | first release after v2.32.0 |

What the table implies:

- **Order is not a preference.** Every backend lists a UNORM format before its `_SRGB` sibling. That order is unchanged between v2.20.1 and v2.21.11. An app that takes `formats[0]` gets UNORM.
- **Float formats are linear.** They follow the UNORM column. The atlas is 8-bit encoded, so values above 1.0 clip. There is no HDR path (ADR-021 *Consequences*).
- **Where the rows do not differ:**
  - **Fast path.** A single-layer `_SRGB` frame skips the private target and is byte-identical to the old path. The predicate is `u_color_compose_fast_path()` in `auxiliary/util/u_color_encoding.h`.
  - **Zero-copy.** A UNORM source never takes zero-copy, because it still owes the encode.
- **Escape hatch.** `DXR_COLOR_LEGACY_UNORM_ENCODED=1` restores the passthrough reading process-wide. On Android set it with `adb shell setprop debug.xrt.DXR_COLOR_LEGACY_UNORM_ENCODED 1`. It is a diagnostic, not a setting.

### 2. What the app writes, per format

| App's swapchain | App writes | Result |
|---|---|---|
| `_SRGB` | linear shader output, GPU encodes on write (`_SRGB` RTV/attachment, `GL_FRAMEBUFFER_SRGB`) | ✓ |
| `_SRGB` | display-referred bytes moved **without** conversion. Vulkan: blit into an UNORM-sibling scratch, then `vkCmdCopyImage`. D3D: `CopyResource` within the typeless family. | ✓ |
| `_SRGB` | display-referred bytes through a blit, clear or `_SRGB` render target | ✗ encoded twice (washed out) |
| UNORM / float | linear values | ✓ |
| UNORM / float | display-referred bytes | ✗ **encoded twice (washed out)** on every row of §1 except Metal |

**Clears.** Clear values (render-pass `loadOp`, `vkCmdClearColorImage`, `ClearRenderTargetView`) are *linear* on an `_SRGB` target. A display-referred clear colour must be linearized first. displayxr-common provides this:
- `dxr::DisplayReferredToSceneLinear()`;
- `dxr::VkDisplayReferredClearColor()`;
- `ClearRenderTargetViewDisplayReferred()`, and its D3D12 twin (#1647).

The helper keys on the *target's* format. Pinning a format and calling the helper are one step (#1647).

**Alpha is never converted.** sRGB formats encode colour channels only.

### 3. Layers, alpha and blending (the same on every format-honest backend)

- **Blend modes (OpenXR §10.6.2), via `comp_layer_blend_mode()` in `compositor/util/comp_layer_view_camera.h`:**
  - no `SOURCE_ALPHA_BIT` → `OPAQUE_COVER`: colour replaces, and **alpha is written as 1** (folded into the shader);
  - `SOURCE_ALPHA_BIT` → premultiplied;
  - `+ UNPREMULTIPLIED_ALPHA_BIT` → straight.

  The **first full-tile projection layer** into a tile is `REPLACE`: its alpha reaches the atlas verbatim. The DP's compose-under-desktop gate depends on it (#225), and it is what a transparent-window app relies on.
- **Blending happens in linear light, in the private `_SRGB`-view target (#1610).** No compose shader contains gamma arithmetic; the encode is a property of the render target. The test `colour: the encode is a render target, never shader arithmetic` pins this.
- **Metal** implements the §10.6.2 blend rule (#1621) and, since its ADR-044 change, blends in linear light like every other row.

### 4. The DP side

- **Handoff.** The atlas reaches the DP **encoded** on every in-process backend. The only exception is D3D11-service Model B, where the atlas is **linear** and the DP performs the one matched encode.
- **DP declarations (ADR-021 §3).** The DP declares `get_handoff_color_capability`. The runtime states each frame's encoding with `set_atlas_encoding` (D3D11 service; vk_native since #1484; Metal's primary DP since its ADR-044 change — the multi-screen segment DPs are not told and take the slot's `ENCODED` default, which is what they receive). If the slot is absent, the DP assumes `ENCODED`.
- **Zero-copy.** A zero-copy frame hands the DP the app's own image. Every backend refuses it for a UNORM / float swapchain (it still owes the encode). Metal additionally hands an `_SRGB` image through its non-decoding view, so a DP that samples it gets the encoded bytes rather than a decode.
- **What the DP emits.** It emits encoded pixels for the panel. Any vendor panel curve lives inside the DP, never in the runtime (ADR-021 §2).
- **Leia Android CNSDK DP** (plug-in v2.7.x) implements neither slot. That is correct, because vk_native always hands it encoded bytes. Its v2.7.0→v2.7.6 changes are alpha-gate texel reads only; nothing colour-related.

### 5. Capture

- **What the PNG contains.** Atlas captures (`xrCaptureAtlasDXR`, the file triggers, MCP) write the atlas bytes, i.e. **encoded** colour. That is what the DP sees, so pixel values compare directly with an app's authored display-referred colour.
- **Alpha.** Alpha is stamped to 255 unless `DXR_ATLAS_CAPTURE_RAW_ALPHA=1` (#425). With it set, the capture becomes an oracle for §10.6.2: 0 means the opaque-cover fix is missing, 255 means it is present.
- **Where the files go:**

| Backend | Trigger | Output |
|---|---|---|
| vk_native | `$TMPDIR/displayxr_atlas_trigger` | `$TMPDIR/displayxr_atlas.png` |
| Metal, GL | `/tmp/dxr_atlas_trigger` | `/tmp/dxr_atlas.png` |
| D3D11 service | `%TEMP%\workspace_screenshot_trigger` | pre-weave `…_atlas_*.png` plus the post-weave file |

### 6. What guards each row

| Row | Pinned by |
|---|---|
| D3D11, D3D12, GL, vk_native on **Windows** | CTS 1.1.63.0: automated on the hosted lane (WARP / llvmpipe / lavapipe, all 5 arms gate). **Interactive composition, human-judged:**<br>- `GradientFormatsLinearVsNonLinear`, `SourceAlphaBlending`, `SourceAlphaBlendingWithEnvironment`, `QuadOcclusion`<br>- d3d11 is the reference lane; d3d12 and opengl match it case for case<br>- opengl needs the CTS GL-plugin `GL_FRAMEBUFFER_SRGB` patch for the gradients ([procedure §10](../reference/cts-interactive-procedure.md))<br>- a Windows `vulkan` hardware run passed the gradients 13/13 |
| vk_native on **Linux** | CTS `vulkan`/`vulkan2` automated (lavapipe, gating). Interactive Linux `vulkan` pass 2026-09-25: 16/0/11, gradients 13/13. |
| Every backend, structurally | `tests_comp_color_policy.cpp`: each backend asks the shared predicate, the encode is a render target, zero-copy refuses UNORM. `tests_aux_color_encoding.cpp`: the oracle curve, the fast-path truth table, legacy hatch off by default. |
| vk_native on **macOS (MoltenVK)** | **Not covered by CTS** (macOS is out of scope for #1523). Same source as the Linux row, so the structural tests cover it. `tools/vk_srgb_blend_probe.c` checked the `_SRGB`-view-over-`MUTABLE_FORMAT` blend on M1 Pro. |
| vk_native on **Android** | **Not covered by CTS** (the #1523 Android lane does not exist yet). Same source as Linux: `vk_native` has no `XRT_OS_ANDROID` branch in its swapchain or compose code. **The Adreno/Mali driver behaviour of the `_SRGB`-view blend target is unverified**; re-run `vk_srgb_blend_probe` on device. |
| **Metal** (macOS) | **Not covered by CTS** (macOS is out of scope for #1523). `tests_comp_color_policy.cpp` pins it structurally (shared predicate + hatch, no OETF constant, the atlas's own `_sRGB` view as the target, one source-view helper, zero-copy refuses UNORM). **Numerically**: `test_apps/probes/colour_probe_metal_macos` on sim_display (M1 Pro), solid CPU-uploaded layers read back from the per-exe atlas capture. Projection and window-space `_SRGB` 38 → **38**, UNORM 38 → **108** (exactly one encode; UNORM 200 → 229); straight black a=128 over 200 → **146** (encoded-space blend gave 100), white a=128 over 38 → **189** (gave 147); transparent-background alpha 0 reaches the atlas verbatim. Before the change every one of those read back the authored byte; `DXR_COLOR_LEGACY_UNORM_ENCODED=1` reproduces the old bytes exactly. |
| **Apps** (every platform) | `scripts/check_displayxr_app.py` INV-4.6, **per leg** (#1760): it flags a UNORM-only format preference list and a scan that stops at the first UNORM, in any leg that enumerates formats. Demo repos add a CI guard that every leg chooses through one shared helper (e.g. displayxr-demo-gaussiansplat#138 `lint.yml`). |

### 7. Rows still open

- **The IPC `comp_multi` paths** (macOS service, Android service flavour, the Linux headless service) have **not been audited** against §1. The shared-atlas path documents a `vkCmdBlitImage` from each client's swapchain format into one target-format atlas (`compositor/multi/comp_multi_system.c`). A blit converts through both formats, so any format-honesty there is incidental, not contracted. **Exception: the macOS shared surface's content pass is honest since #1801.** It sampled each client as its declared format but drew into the encoded atlas through its UNORM view, so every `_SRGB` app was stored one decode too dark (measured: luminance 61 against 131 for the same scene declared UNORM). It now draws through the same `_SRGB` atlas view as the #1795 decorations. The focus-glow style colour is display-referred and is decoded on the CPU first, like a clear colour. A UNORM client there is read as linear and encoded, like vk_native. The Android and Linux service flavours remain unaudited.
- **Local2D layers are not format-honest.** The Local2D flatten samples the app's swapchain through a *non-decoding* view and writes a UNORM scratch, on every in-process backend:
  - D3D11 `comp_d3d11_swapchain_get_srv`, not `get_compose_srv`;
  - D3D12 `comp_d3d12_swapchain_sample_format`;
  - GL `gl_bind_layer_source(…, compose=false)`;
  - vk_native `get_image_view`, not `get_true_image_view`.

  (Superseded per backend by #1795 on the four Windows backends and by the ADR-044 Metal change on macOS: each flatten now renders through an `_sRGB` view of its scratch, every layer sampled as declared; the legacy hatch keeps the passthrough.)

  **vk_native window-space layers had the same gap until #1795.** `vk_hud_blend` sampled every source through a hard-coded `R8G8B8A8_UNORM` view and blended into the atlas in encoded space: the model viewer's displayxr-common button bar gave byte-identical atlases with swapchain 37 and 43, and the hard-coded view would have swapped R and B for a `B8G8R8A8` source. Since #1795 the in-process vk_native pass raw-copies the atlas into the private compose target, blends each layer through its `_SRGB` view with the source sampled as its **declared** format, and raw-copies back (`vk_compositor_render_window_space_linear`). All four Windows backends are now honest. Measured with the `windowspace_handle_*_win` probes, which draw opaque layers authored at byte 38: an `_SRGB` swapchain reads back 38 and a UNORM one reads back 108 (exactly one encode) on D3D11, D3D12, GL and vk_native. The macOS service's workspace decorations (shell chrome, cursor, overlays, drawn into the shared atlas by `comp_multi`) got the same fix in #1795. The atlas is created `MUTABLE_FORMAT` with its `_SRGB` sibling, and the decorations render through that view, each sampled as its declared format. That change ships together with displayxr-shell-pvt#121, which declares the shell's CoreGraphics-filled macOS surfaces `_SRGB` on runtime ≥ v2.23.0; a UNORM shell on the fixed service would be washed out. Linux registers no workspace surfaces, so it has no such site.

  Encoded bytes therefore pass through whether the swapchain says UNORM or `_SRGB`. Neither is washed out today, but a true-linear UNORM Local2D app renders too dark. Local2D is also flattened after the atlas, so §5's atlas capture cannot see it. App guidance does not change: submit Local2D as `_SRGB` with raw-copied bytes (displayxr-unity v2.21.0 already does). **Order matters:** apps move first. Making the flatten honest before they do would wash out every app that still declares UNORM Local2D.
- **The displayxr-common Windows window-space HUD swapchain** stays `R8G8B8A8_UNORM` with display-referred bytes. On any format-honest backend it is encoded twice; its move to `_SRGB` is pending (displayxr-common).

## Consequences

- **One sentence for app authors** (the rule at the top), one table for runtime authors (§1), and a per-leg lint.
- **The runtime does not change to accommodate apps.** Every correct row is CTS- or test-pinned, and reverting a row to passthrough would fail `GradientFormatsLinearVsNonLinear` again. An app that looks washed out on v2.21.7+ is fixed app-side: request `_SRGB` and keep the bytes unconverted.
- **ADR-021 stands for the principles** (matched pairs, format as source of truth, DP-declared handoff). Its §5 "Model A baseline" now means "the atlas is encoded". It no longer means "a UNORM swapchain passes through", and the hop table in ADR-021 already says so.
- **Metal was the last exception, and is migrated** (§1). Its in-tree cube apps had picked `BGRA8Unorm` and written display-referred bytes, so they would have washed out on the format-honest compositor (measured: background 13 → 64); they now request `BGRA8Unorm_sRGB` and render through a `BGRA8Unorm` view of it (raw bytes), and pin displayxr-common v2.27.0 for an `_SRGB` HUD. Out-of-tree Metal apps that do the same need the same fix — `_SRGB` is correct on every row.
