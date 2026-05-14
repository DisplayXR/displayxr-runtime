# Leia Transparent Backgrounds — Compose-Under-Bg + Chroma-Key Fallback

How the Leia display processor composites a transparent app window over the Windows desktop, what each graphics API path does, and how an app opts in.

This supersedes [`chroma-key-transparent-overlay.md`](chroma-key-transparent-overlay.md) as the primary path. Chroma-key is still the documented fallback when WGC desktop capture is unavailable.

## Per-API status

| API | Primary path | Notes |
|---|---|---|
| D3D11 | compose-under-bg | Falls back to chroma-key on WGC failure. |
| D3D12 | compose-under-bg | Queue-level shared-fence `Wait` for sync. |
| Vulkan | compose-under-bg | D3D11 NT-handle → VK external image import. No GPU semaphore wait yet (relies on temporal separation; see [Limits](#limits)). |
| OpenGL | chroma-key only | Compose-under-bg blocked on the deferred DComp + `WGL_NV_DX_interop2` bridge. |
| Metal (macOS) | alpha-native | SR weaver preserves alpha end-to-end (commit `e94d07292`); no compose or chroma trick needed. |

## Why compose-under-bg

The SR weaver flattens alpha during interlacing. Two ways to expose the desktop through transparent app regions:

1. **Chroma-key (legacy).** Replace `α=0` atlas pixels with a magenta sentinel pre-weave, then post-weave detect that color and rewrite `α=0` for DWM. Survives the weaver as RGB. **Hard mask on AA edges** — the lerp toward key + exact-RGB strip can't represent `0<α<1`. Also vulnerable to disocclusion fringe at silhouette boundaries (see [`chroma-key-transparent-overlay.md`](chroma-key-transparent-overlay.md) §Limits).
2. **Compose-under-bg (preferred).** Capture the desktop region behind the window via Windows Graphics Capture and composite it UNDER each per-view atlas tile before the weaver runs. Output is genuinely opaque RGB with the desktop already integrated; the weaver consumes it normally. AA edges and semi-transparent pixels work correctly.

## Architecture

```
              ┌──────────────────────────────────────────┐
              │ leia_bg_capture (Win-only helper)         │
              │  - Internal D3D11 device                  │
              │  - WGC: monitor capture → staging tex     │
              │  - SHARED_NTHANDLE BGRA8 (monitor size)   │
              │  - D3D11_FENCE_FLAG_SHARED for sync       │
              └────────────┬─────────────────────────────┘
                           │ NT handles (texture + fence)
       ┌───────────────────┼───────────────────┐
       ▼                   ▼                   ▼
  D3D11 Leia DP      D3D12 Leia DP      Vulkan Leia DP
  OpenSharedResource1 OpenSharedHandle   VK_KHR_external_memory_win32
  ID3D11Fence Wait    ID3D12CommandQueue ::Wait   (no Wait yet — caveat)
       │                   │                   │
       └─── compose pass: lerp(bg, atlas.rgb, atlas.a), a=1 ───┐
                                                               ▼
                                                      SR weaver (opaque RGB in)
                                                               │
                                                               ▼
                                                  DComp swap chain → DWM
```

The per-DP `compose_*` pipelines reuse the existing chroma-key intermediate target (`ck_fill_*`) as the render target — same `R8G8B8A8_UNORM` format, no extra allocation. Distinct pipelines/descriptor sets/samplers (compose uses linear filtering; ck uses point).

## How "per eye" works out

The desktop sits at `z=0` (display plane), so the same captured background region is sampled into both tile 0 and tile 1. Per-eye-ness comes from the atlas content (which already has parallax per view), not the background. After the weaver interleaves, each eye sees `desktop + its own view's overlay` with correct parallax on the overlay and the desktop pinned at the display plane.

Shader logic (HLSL/GLSL identical in spirit):

```glsl
float4 a = atlas.Sample(samp, uv);
float2 tile_local = frac(uv * float2(tile_count));   // wrap into per-tile 0..1
float2 bg_uv = bg_uv_origin + tile_local * bg_uv_extent;
float3 b = bg.SampleLevel(samp, bg_uv, 0).rgb;
return float4(mix(b, a.rgb, a.a), 1.0);
```

`bg_uv_origin` / `bg_uv_extent` map the **client area** of the window onto the captured monitor texture (NOT the outer window rect — that would shift by the title-bar height).

## Self-capture defense

`leia_bg_capture_create` calls `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` so WGC does not recursively capture our own woven output back into the background. Requires Windows 10 build 19041+ (2004); on older Windows the bg-capture module fails to create and the DP falls back to chroma-key.

## Cross-API sync

The producer is the internal D3D11 device inside `leia_bg_capture_win`. Consumers are the DP's own device (D3D11/D3D12/VK). After each `CopyResource` from the WGC frame into the shared staging texture, the producer signals an `ID3D11Fence` (created with `D3D11_FENCE_FLAG_SHARED`) and flushes its context. Consumers wait before sampling:

- **D3D11:** `ID3D11DeviceContext4::Wait(fence, value)` in the DP's command stream.
- **D3D12:** `ID3D12CommandQueue::Wait(fence, value)` at the queue level before the cmd list executes.
- **VK:** Not implemented — the VK DP relies on the temporal gap between WGC's ~60Hz producer and the consumer's ~120Hz render rate (the producer's copy is sub-ms; the chance of mid-copy sample is negligible). A proper `VK_KHR_external_semaphore_win32` import via `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT` is a follow-up.

## App-side recipe

To get transparency on D3D11 / D3D12 / Vulkan apps with a Leia 3D display:

1. **Window style.** Standard top-level window with `WS_EX_NOREDIRECTIONBITMAP`, null background brush. (DComp owns the redirection bitmap; a non-null brush would paint over the composition swap chain.)
2. **Clear to `RGBA(0,0,0,0)`** in the app's render target. The compose pass turns the cleared regions into the captured desktop.
3. **Opt-in via `XR_EXT_win32_window_binding`:**
   ```c
   XrWin32WindowBindingCreateInfoEXT bind = {
       .type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT,
       .windowHandle = hwnd,
       .transparentBackgroundEnabled = XR_TRUE,
       .chromaKeyColor = 0,   // 0 = DP picks default (used only on fallback)
   };
   ```
4. **No `WS_EX_LAYERED`, no `SetLayeredWindowAttributes`** — those don't compose with the runtime's flip-model swap chain.

The test apps `cube_handle_d3d11_win`, `cube_handle_d3d12_win`, `cube_handle_vk_win` opt in via the `DISPLAYXR_TRANSPARENT_BG=1` env var.

## Fallback behavior

Compose-under-bg fails to initialize → DP transparently falls back to chroma-key. Triggers:

- `LEIA_DP_DISABLE_BG_CAPTURE=1` env var (for testing the fallback path).
- Windows version < 10 2004 (no `WDA_EXCLUDEFROMCAPTURE`).
- WGC `RoGetActivationFactory` failure (graphics class not registered).
- `SetWindowDisplayAffinity` failure (rare; some virtualization stacks).

The session log will show one of:
```
Leia D3D11 DP: transparency = compose-under-bg (WGC)
Leia D3D11 DP: transparency = chroma-key (key=0x00ff00ff — DP default)
```

## Limits

- **DRM-protected content** under the window appears black in WGC capture (standard WGC limitation — same as any screen recorder).
- **Window crossing monitors mid-session** — the capture session is bound to the monitor at create. `leia_bg_capture_poll` detects the monitor change and skips the compose pass for that frame so the DP doesn't sample the wrong desktop. Recreating the WGC session mid-stream is a follow-up.
- **Vulkan GPU sync** — see [Cross-API sync](#cross-api-sync). Visible only in pathological timing.
- **OpenGL apps** still use chroma-key. They inherit the AA-edge and disocclusion-fringe limitations documented in [`chroma-key-transparent-overlay.md`](chroma-key-transparent-overlay.md).

## Key source files

| File | Role |
|---|---|
| `src/xrt/drivers/leia/leia_bg_capture_win.{h,cpp}` | WGC capture, shared NT-handle staging tex, shared fence. Win-only. |
| `src/xrt/drivers/leia/leia_display_processor_d3d11.cpp` | D3D11 compose pipeline + fallback. |
| `src/xrt/drivers/leia/leia_display_processor_d3d12.cpp` | D3D12 compose pipeline (PSO, root sig, descriptor heap) + fallback. |
| `src/xrt/drivers/leia/leia_display_processor.cpp` | Vulkan compose pipeline (render pass reuse, external image import) + fallback. |
| `src/xrt/drivers/leia/shaders/compose_under_bg.frag` | GLSL fragment shader (Vulkan). HLSL inlined in the D3D11/D3D12 .cpp files. |

## References

- `XR_EXT_win32_window_binding` spec_version 5 — `src/external/openxr_includes/openxr/XR_EXT_win32_window_binding.h`
- [`chroma-key-transparent-overlay.md`](chroma-key-transparent-overlay.md) — the legacy fallback path, kept as reference for the GL DP and as historical context.
- [Windows Graphics Capture (WGC)](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture)
- [`VK_KHR_external_memory_win32`](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_external_memory_win32.html)
