# Desktop Overlay Apps — Forward Work

Tracker for follow-on work after the transparent-HWND overlay path shipped (#191) and the Unity prototype demonstrated the feature end-to-end (`displayxr-unity#57`).

For the design and API of the shipped feature, see [Transparent HWND Overlay (Post-Weave Chroma-Key)](../vendors/leia/chroma-key-overlay.md).

## Shipped

- **Runtime path** (#191, commit `d2729977b`): opt-in transparent presentation via `XR_EXT_win32_window_binding` spec_version 5. DComp + premultiplied-alpha swapchain + post-weave chroma-key shader pass. D3D11 in-process, D3D11 service, D3D12 in-process all share the same shader and lifecycle.
- **Unity plugin** (`displayxr-unity#57`): `DisplayXRTransparentOverlay` MonoBehaviour wraps the runtime opt-in. Sets the camera clear to the chroma color, requests `transparentBackgroundEnabled = XR_TRUE` in the binding chain, forwards a screen-space bounding box for `WM_NCHITTEST`.

## Open work

### Native sample app

The path is currently exercised only through the Unity plugin. A small native handle-class test app — derived from `cube_handle_d3d11_win` — would document the recipe for non-Unity integrations and give us a regression target inside this repo.

Scope:

- Borderless `WS_POPUP | WS_VISIBLE` window, optional `WS_EX_TOPMOST`. No layered styles.
- Session create with `transparentBackgroundEnabled = XR_TRUE`, `chromaKeyColor = 0x00FF00FF`.
- D3D11 clear color magenta on per-eye render targets.
- `WM_NCHITTEST` returns `HTCLIENT` inside a hard-coded center bounding box, `HTTRANSPARENT` outside.
- Replicate for D3D12 once the D3D11 version is stable.

Verification mirrors the Unity smoke test: confirm desktop shows through magenta regions, clicks pass through magenta, clicks on the avatar reach the app's `WndProc`.

### Disocclusion-fringe characterization

The post-weave pass does an exact-RGB match, so silhouette pixels with `L ≠ R` (the de-occluded band) remain opaque and produce a chroma-color fringe to the eye that "should" see the desktop. The fringe width is the stereo disparity at the silhouette.

To do:

- Quantify visible halo width vs. avatar parallax on a Leia SR display. Sweep avatar Z-offset from screen depth to maximum comfortable parallax; capture compositor screenshots and viewer-eye photos.
- Prototype the silhouette-intersection mitigation (conservative chroma-color dilation in each view's clear). Measure how much silhouette fidelity is lost vs. how much fringe is suppressed.
- If neither parallax-budget nor dilation is acceptable for product use, prototype `SetWindowRgn` clipping on top of the DComp path. Hard binary edges, region update cost per frame, but reliable.

These experiments inform the avatar product's acceptable parallax budget and shader choices; they do not require runtime changes.

### Migration when the alpha-respecting weaver lands

#190 tracks a vendor request to Leia for an opt-in alpha-aware weaver mode. When that lands:

- Apps set `chromaKeyColor = 0`. The runtime skips the post-weave shader pass (already implemented; the pass is no-op when the color is zero).
- The display processor preserves the swapchain's per-pixel alpha through interlacing. Edge pixels carry partial alpha and blend correctly in both eyes — disocclusion fringes go away.
- Apps can drop the chroma-color clear and use real per-pixel alpha (`α = 0` in transparent regions) directly.

No additional runtime work is expected — the existing opt-in API already covers this case. The only thing needed is the weaver SDK update.

### macOS port

`XR_EXT_cocoa_window_binding` is the macOS analogue of the Win32 binding extension. A transparent-overlay path on macOS would use:

- `NSWindow.isOpaque = NO` and `NSWindow.backgroundColor = NSColor.clearColor` on the bound window.
- Metal compositor's drawable in BGRA8 with premultiplied alpha; CALayer composes against the desktop natively.
- Equivalent post-weave chroma-key pass on the Metal compositor side, mirroring the D3D11 / D3D12 implementation.

Defer until there is concrete demand on macOS.

### Per-pixel hit-testing

The current pattern is rectangular bounding boxes from the app via `WM_NCHITTEST`. For tighter silhouettes — especially with non-convex avatars or UI affordances — replace with a CPU-readable downsampled silhouette mask:

- App writes a 256×256 alpha mask once per N frames into a `STAGING` texture.
- `WndProc` samples the mask at the cursor's normalized window coordinate.

Plugin / app concern, not a runtime concern. Tracked here for completeness.

## References

- [Transparent HWND Overlay (Post-Weave Chroma-Key)](../vendors/leia/chroma-key-overlay.md)
- [Leia SR Weaver](../vendors/leia/weaver.md)
- `XR_EXT_win32_window_binding` — `src/external/openxr_includes/openxr/XR_EXT_win32_window_binding.h`
- Issue #190 — alpha-respecting weaver vendor request
- Issue #191 — runtime-side transparent HWND fix (closed)
- `displayxr-unity#57` — Unity plugin transparent overlay mode
