# Transparent HWND Overlay (Post-Weave Chroma-Key) — Fallback Path

> **Note:** D3D11 / D3D12 / Vulkan Leia DPs now use **compose-under-bg** as the primary transparency path. See [`leia-transparency.md`](leia-transparency.md) for the current model. Chroma-key remains the documented fallback when Windows Graphics Capture is unavailable (older Windows, DRM, env-disabled), and is still the only path for the OpenGL Leia DP (blocked on the deferred DComp+`WGL_NV_DX_interop2` bridge).

How DisplayXR makes a bound app HWND composite transparently against the Windows desktop on a Leia 3D display, and how an app opts in.

This is the design that shipped via #191. It supersedes an earlier `WS_EX_LAYERED + LWA_COLORKEY` recipe — that recipe doesn't work on flip-model swapchains, which is what the runtime presents through.

## What the runtime does

When an app sets `XrWin32WindowBindingCreateInfoEXT::transparentBackgroundEnabled = XR_TRUE` (spec_version 5), the D3D11 / D3D12 native compositor switches the bound HWND's presentation path:

1. **Composition swapchain.** `CreateSwapChainForComposition` with `DXGI_FORMAT_R8G8B8A8_UNORM` and `DXGI_ALPHA_MODE_PREMULTIPLIED` (instead of the default opaque `CreateSwapChainForHwnd` + `ALPHA_MODE_IGNORE` from #163).
2. **DirectComposition binding.** `IDCompositionDevice` → `IDCompositionTarget` on the HWND → `IDCompositionVisual::SetContent(swapchain)`. DComp owns the redirection; the HWND itself is just the target geometry.
3. **Post-weave alpha pass.** When `chromaKeyColor != 0`, the runtime runs a full-screen pixel shader after the Leia weave. It reads each back-buffer pixel; if RGB matches `chromaKeyColor` within `1/512` tolerance, it writes `(0, 0, 0, 0)` premultiplied; otherwise it writes the pixel opaque.
4. **DWM blends.** DComp + DWM composite the swapchain onto the desktop using premultiplied alpha. Pixels with `α = 0` show the desktop through. Pixels with `α = 1` show the woven 3D content.

Default behavior (flag unset) is unchanged — opaque flip-model present, no DComp, no extra shader pass.

The same lifecycle is implemented in three places that share the same shader source:

- `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp` (in-process D3D11)
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (service D3D11, per client)
- `src/xrt/compositor/d3d12/comp_d3d12_compositor.cpp` (in-process D3D12)

D3D12 is the reason the runtime uses DComp at all: DXGI forbids `DXGI_SWAP_EFFECT_DISCARD` (BitBlt) on D3D12, so the simple "use a BitBlt swapchain + `LWA_COLORKEY`" path is unavailable. DComp is the only Microsoft-blessed transparent path that works on both APIs, and unifying both APIs on it gives the plugin a single recipe.

## Why this routes through chroma-key at all

The Leia SR weaver writes opaque RGB and discards alpha during interlacing. If the app simply rendered with per-pixel alpha and let DComp blend, the post-weave back buffer would be opaque everywhere and DWM would never see transparent pixels. The post-weave shader pass restores alpha based on a chroma-key match, so the magic color survives the weaver as RGB and is reconstituted as `α = 0` immediately before DComp composition.

When the display processor preserves alpha — the long-term goal tracked as a Leia vendor request (#190) — the post-weave pass is no longer necessary. Apps will then set `chromaKeyColor = 0` and rely on per-pixel alpha alone.

## API surface

```c
typedef struct XrWin32WindowBindingCreateInfoEXT {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    void*                       windowHandle;          // HWND
    PFN_xrReadbackCallback      readbackCallback;
    void*                       readbackUserdata;
    void*                       sharedTextureHandle;
    XrBool32                    transparentBackgroundEnabled;  // (spec_v4) opt in
    uint32_t                    chromaKeyColor;                // (spec_v5) 0x00BBGGRR; 0 disables post-weave pass
} XrWin32WindowBindingCreateInfoEXT;
```

`chromaKeyColor` is a Win32 `COLORREF` (`0x00BBGGRR`). The default-suggested value is `0x00FF00FF` (magenta). Apps must clear both eye views to the same color in regions that should be transparent — `L == R` ensures the weaver passes the color through unchanged. `chromaKeyColor = 0` disables the post-weave pass and is intended for forward compatibility with alpha-respecting display processors.

The flag is honored only when `windowHandle != NULL` and the session is standalone. Workspace / shell mode ignores it (the shell owns presentation).

## App-side recipe

1. Create a borderless top-level HWND (`WS_POPUP | WS_VISIBLE`, optionally `WS_EX_TOPMOST`). Do **not** add `WS_EX_LAYERED`, `WS_EX_TRANSPARENT`, or call `SetLayeredWindowAttributes` — DComp owns transparency and the layered-window styles are no longer needed.
2. Request the runtime's transparent path at session create:

   ```c
   XrWin32WindowBindingCreateInfoEXT bind = {
       .type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT,
       .windowHandle = hwnd,
       .transparentBackgroundEnabled = XR_TRUE,
       .chromaKeyColor = 0x00FF00FF,
   };
   bind.next = next_in_chain;
   XrSessionCreateInfo session_create = { ..., .next = &bind };
   xrCreateSession(instance, &session_create, &session);
   ```

3. Clear both eye views to `RGB(255, 0, 255)` (or whatever `chromaKeyColor` you chose) in transparent regions. Render the avatar / 3D content normally on top.
4. Implement selective click-through in your `WndProc`: return `HTTRANSPARENT` from `WM_NCHITTEST` for points outside the avatar's screen-space bounds, `HTCLIENT` for points inside.
5. Do not opt in if you cannot exclude the chroma color from your foreground palette.

The DisplayXR Unity plugin (`displayxr-unity#57`) wraps all of this behind a `DisplayXRTransparentOverlay` component.

## Limits — disocclusion fringe near the silhouette

This is the principal failure mode. It is not eliminated by the post-weave pass; it is fundamental to combining a chroma-key trigger with stereoscopic interlacing.

**The setup.** At a silhouette edge with non-zero parallax, the avatar lives at slightly different screen positions in the two views. There is a band of pixels — the de-occluded band — where one eye sees foreground (`F`) and the other eye sees the chroma key (`K`). The eye that doesn't see the foreground is "looking around" the avatar at the background.

**The weave.** In that band `L ≠ R`, so the woven back-buffer pixel is `mix(F, K, phase)` per pixel — neither pure `F` nor pure `K`.

**The post-weave pass.** It does an exact-RGB match (within `1/512`). `mix(F, K, phase)` is not close enough to `K` to match, so the pixel is written `α = 1`, not `α = 0`. The fringe is opaque.

**What each eye sees.** Through the lenticular optics, the eye that "should" see the foreground sees `F` correctly. The eye that "should" see the desktop sees `K` (the chroma color) instead — a band of pure magenta where it expected the desktop to show through.

**Halo width = stereo disparity at the silhouette.** Push the avatar further out → wider fringe. Render at screen depth → no fringe (but no parallax on the silhouette).

### Mitigations

In rough order of cost vs. effectiveness:

1. **Constrain silhouette parallax.** Keep the avatar mostly at or behind screen depth, or tune the rig so the silhouette specifically is near zero parallax even if interior features have depth. Halo width scales linearly with disparity at the silhouette.
2. **Conservative chroma-color dilation (silhouette intersection).** Bias each view's clear so the chroma-color region in `L` extends slightly into the `R`-view foreground silhouette and vice versa. The de-occluded band where one view sees `F` and the other sees `K` is replaced by a band where both views see `K`. The post-weave pass then matches and writes `α = 0`. The visible foreground silhouette is the *intersection* of the L and R silhouettes, slightly chewed in by the disparity.
3. **`SetWindowRgn` clip on top.** Clip the HWND to a polygonal silhouette region per frame. The fringe pixels are excised entirely. Hard binary edge, no antialiasing, region-update cost per frame.
4. **Alpha-respecting display processor (long-term fix, #190).** Once the weaver preserves alpha, set `chromaKeyColor = 0` and the post-weave pass is bypassed. Edge pixels carry partial alpha and blend correctly in both eyes.

## Other gotchas

- **Chroma-color exclusion.** The avatar / foreground must not contain any pixel exactly equal to `chromaKeyColor`, or it becomes a transparent hole. Pick a color well off the foreground palette and clamp shader outputs near it.
- **Color space.** Both the swapchain and the weaver may apply sRGB ↔ linear conversion. Pure primaries (`RGB(255,0,255)`, `RGB(0,255,0)`, `RGB(255,0,0)`) round-trip exactly through `R8G8B8A8_UNORM_SRGB` ↔ `R8G8B8A8_UNORM`. Avoid mid-luminance grays.
- **Premultiplication.** The post-weave pass writes premultiplied output: `(0,0,0,0)` for transparent pixels, `(rgb, 1)` for opaque. Apps that target `chromaKeyColor = 0` (DP-preserves-alpha mode) must also output premultiplied alpha or DWM will saturate to white at edges.
- **Workspace mode.** The flag is silently ignored in shell / workspace sessions. Transparent presentation is a standalone-session feature.
- **Fullscreen-exclusive games.** A topmost transparent window does not appear above FSE apps. Borderless-fullscreen is fine.
- **Multi-monitor + mixed DPI.** DComp's redirection is per-monitor; mixed DPI works in principle. Verify on first run for new hardware.

## Diagnostics

The compositor logs one warning at session create on the opt-in path:

```
Transparent HWND opt-in: DComp + flip-model swapchain (FLIP_DISCARD + PREMULTIPLIED, bc=N)
Post-weave chroma-key conversion enabled: 0xRRGGBBAA
```

If you see the first line but not the second, `chromaKeyColor` was zero — the runtime is relying on per-pixel alpha alone, which only works if the display processor preserves alpha (currently false for the Leia weaver).

## References

- `XR_EXT_win32_window_binding` spec_version 5 — `src/external/openxr_includes/openxr/XR_EXT_win32_window_binding.h`
- Issue #191 — root-cause investigation and the runtime fix
- Issue #163 — context for the default opaque present
- Issue #190 — vendor request for an alpha-respecting weaver mode (long-term replacement for the post-weave pass)
- `displayxr-unity#57` — Unity-side wrapper (`DisplayXRTransparentOverlay`)
- Microsoft Learn: [DirectComposition basic concepts](https://learn.microsoft.com/en-us/windows/win32/directcomp/basic-concepts), [`CreateSwapChainForComposition`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition)
