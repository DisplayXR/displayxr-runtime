# Surround 2D Rollout — `xrSetSharedTextureSurround2DEXT`

**Status:** design + spec draft (2026-05-28). Pre-implementation.
**Branch:** `feature/surround-2d-spec`.
**Spec:** [`XR_EXT_win32_window_binding` §3.6](../specs/extensions/XR_EXT_win32_window_binding.md#36-xrsetsharedtexturesurround2dext), [`XR_EXT_cocoa_window_binding` §4](../specs/extensions/XR_EXT_cocoa_window_binding.md). Both bumped to spec version 6.
**Related:** [#224 local-3d-zones](../roadmap/local-3d-zones.md) (deliberately *not* the same path — see §6).

## TL;DR

A `_texture` app on a display with a fixed hardware 3D zone needs to fill the area *around* the canvas sub-rect with full-resolution 2D content. Today, those non-canvas pixels of the target swapchain are undefined — the runtime only writes the canvas region. Spec v6 adds one new function, `xrSetSharedTextureSurround2DEXT`, that registers a full-window 2D shared texture; the compositor blits its non-canvas pixels into the target swapchain each frame. No new layer types, no new display-processor methods, no per-pixel mask. Builds on the canvas sub-rect plumbing that just landed for #85.

End goal: ship the capability through Unity's plugin so Unity editors and runtime applications can render a 3D viewport in a fixed zone with crisp 2D toolbars / chrome around it.

## Phases

### Phase A — Spec + headers (this PR)

- [x] Bump `XR_EXT_win32_window_binding_SPEC_VERSION` 5 → 6 in `src/external/openxr_includes/openxr/XR_EXT_win32_window_binding.h`.
- [x] Add `PFN_xrSetSharedTextureSurround2DEXT` typedef + `xrSetSharedTextureSurround2DEXT` prototype to the Win32 header.
- [x] Mirror on Cocoa header (`XR_EXT_cocoa_window_binding.h` v5 → v6) with IOSurface lifecycle deltas.
- [x] Add §3.6 to the Win32 spec doc with parameters, valid usage, pipeline integration, fallback, and a worked example.
- [x] Add a parity section in the Cocoa spec doc that points back to Win32 §3.6 and lists the Metal/IOSurface deltas.
- [x] Extend §3.2 *Three Modes* "Texture" row prose with a forward link to §3.6 for the "fill the area around the canvas" question.

### Phase B — Runtime state-tracker + compositor plumbing

- [ ] Add a new extension entry to `src/xrt/state_trackers/oxr/oxr_extension_support.h` (no new bool — the existing `XR_EXT_win32_window_binding` / `XR_EXT_cocoa_window_binding` bools cover v6 since it's an additive function, not a separate extension).
- [ ] Implement `oxr_xrSetSharedTextureSurround2DEXT` in `src/xrt/state_trackers/oxr/oxr_session.c` next to `oxr_xrSetSharedTextureOutputRectEXT`:
  - Validate session has a window binding.
  - Validate `width × height == HWND client area`.
  - Open the shared handle (D3D11 `OpenSharedResource1` / D3D12 `OpenSharedHandle`).
  - Stash the opened texture + KeyedMutex on the per-session compositor state.
  - On NULL handle: release the previous texture + KeyedMutex.
- [ ] Wire the function pointer into the dispatch table (`oxr_xr_to_str.c` / `xrGetInstanceProcAddr`).

### Phase C — D3D11 compositor surround blit

- [ ] In `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp` (or wherever the per-frame `process_atlas` is dispatched), add a surround-blit pass after the DP's weave finishes:
  1. `AcquireSync(0)` on the surround texture's KeyedMutex.
  2. Bind two SRVs on the surround texture (UNORM + UNORM_SRGB, parallel to the shell-mode `atlas_holds_srgb_bytes` pattern in [`compositor-pipeline.md`](../architecture/compositor-pipeline.md#color-space-handling-d3d11-service-compositor-shell-mode)).
  3. Run a fullscreen blit shader against the target swapchain with a scissor-rect strategy:
     - Path A: four scissor-rects (top/bottom/left/right strips around the canvas) → four `CopySubresourceRegion` or quad draws.
     - Path B: one fullscreen pass with a "skip if inside canvas rect" branch in the pixel shader.
     Path A is simpler and cache-friendlier when the canvas is large. Default to A; revisit if vendor weavers care about overwrite ordering with the DP's output.
  4. `ReleaseSync(0)`.
- [ ] Honor the existing color-space contract: target swapchain stays UNORM; the runtime selects the SRV that matches the surround texture's format so the bytes written to the target are always linear (or SRGB-encoded, depending on the target's setup — match what the DP already does for the canvas region).

### Phase D — D3D12 compositor surround blit

- [ ] Mirror Phase C in `src/xrt/compositor/d3d12/`. Same scissor-rect strategy. D3D12 uses `ID3D12Resource::CreateSharedHandle` + `ID3D12Device::OpenSharedHandle` and exposes the keyed mutex via DXGI.
- [ ] Verify against the d3d12 native compositor's existing canvas sub-rect handling (already done for #85 — the surround blit is its natural complement).

### Phase E — Test-app updates

- [ ] **`test_apps/cube_texture_d3d11_win/`**:
  - In `xr_session.cpp`, after the existing `xrGetInstanceProcAddr("xrSetSharedTextureOutputRectEXT", ...)` line, add the analogous proc-address lookup for `xrSetSharedTextureSurround2DEXT` and stash the PFN.
  - In `main.cpp` (or wherever the shared multiview texture is created), allocate a *second* D3D11 shared texture at HWND client dimensions with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`, `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`.
  - Per frame: render a deliberately-2D pattern into it — a colored gradient with crisp text overlay reading e.g. "FULL-RES 2D — `<window_w>×<window_h>`", a checkerboard at panel-pixel scale, and a thin border around the canvas hole. Acquire/release the KeyedMutex on key 0.
  - Set the canvas rect to a centered ~60% sub-rect (simulating the fixed hardware 3D zone). The cube still weaves inside the canvas; the surround shows the 2D pattern.
  - Bump the build version comment and add a HUD line indicating the surround texture is active.
- [ ] **`test_apps/cube_texture_d3d12_win/`**: same pattern, D3D12 idioms.
- [ ] (Optional, Phase F-late) `test_apps/cube_texture_metal_macos/`: mirror with IOSurface, gated on Metal compositor surround-blit support landing first.

### Phase F — Local validation

- [ ] Build runtime + test apps locally on Windows (`scripts\build_windows.bat all`), run `cube_texture_d3d11_win` against the Leia SR plugin or sim_display. Expected visual: 3D cube weaves inside the centered canvas; outside, the 2D pattern is crisp at full panel res; no ghosting, no leakage across the boundary.
- [ ] Capture a compositor screenshot via `%TEMP%\workspace_screenshot_trigger` (path documented in `CLAUDE.md` § Capturing Compositor Screenshots) to inline-verify pixel-perfect canvas/surround boundary.
- [ ] Run the MinGW compile-check on macOS (`./scripts/build-mingw-check.sh aux_util displayxr_mcp`) to catch any obvious Win32 platform-guard regressions before pushing.

### Phase G — Unity plugin integration

Lives in [`DisplayXR/displayxr-unity`](https://github.com/DisplayXR/displayxr-unity), not this repo. Tracked separately, but the surface shape is fixed by spec v6 above.

The Unity plugin's existing C# binding for `xrSetSharedTextureOutputRectEXT` is in `native~/` (the native bridge module). The Unity-side surface looks like:

```csharp
// Existing today (sets the canvas sub-rect):
DisplayXR.SetCanvasRect(int x, int y, int width, int height);

// New (registers a Unity RenderTexture as the 2D surround source):
DisplayXR.SetSurround2D(RenderTexture surround);   // pass null to clear
```

C# → native plumbing:

1. Unity creates a `RenderTexture` at the panel's HWND client size, `RenderTextureFormat.ARGB32`, sRGB-aware. Unity's `RenderTexture` already wraps a D3D11/D3D12 shared resource; the plugin extracts the underlying `ID3D11Texture2D*` via `GetNativeTexturePtr()` and calls `IDXGIResource1::CreateSharedHandle` to get an NT HANDLE.
2. Plugin calls `pfnSetSharedTextureSurround2DEXT(session, handle, width, height)`.
3. Per frame, Unity renders its 2D UI camera (UGUI canvas, UI Toolkit document, whatever the app uses) to that `RenderTexture` instead of to the back buffer. The plugin's per-frame hook acquires the keyed mutex around Unity's render submission, then releases.
4. The runtime's compositor blits the surround pixels around the weaved canvas region into the target swapchain.

Discovery: `displayxr-unity` already reflects on `xrGetInstanceProcAddr` at plugin init; bumping its `XR_EXT_win32_window_binding` requested-version to 6 + adding the new PFN lookup is mechanical.

Migration path for existing Unity apps:
- Apps that already use the texture path (canvas sub-rect) get the new surround capability by adding one C# call.
- Apps that don't set a canvas rect (full-window canvas — the default) are unaffected; the surround region is empty and the new call is a no-op even if invoked.

### Phase H — Documentation closeout

- [ ] Add a row to the `XR_EXT_win32_window_binding` spec version history when this lands.
- [ ] Update `docs/specs/runtime/swapchain-model.md` § Canvas Concept with a one-paragraph addition explaining the surround texture as the "fill the rest" partner to the canvas sub-rect.
- [ ] Update `docs/getting-started/app-classes.md` Texture-row description: "good for apps that want to render 3D content as part of a larger 2D UI" → tighten with the surround texture mechanism.

## What's intentionally *not* in this design

- **Per-pixel masking** ([#224](../roadmap/local-3d-zones.md)): the surround texture is rectangular complement of the canvas sub-rect. There's no per-pixel "this region is 2D" mask. When per-zone hardware lands and apps want to publish dynamic 2D/3D masks, that's `XR_EXT_local_3d_zone` from the #224 spec, layered alongside this — not replacing it.
- **Multi-canvas / picture-in-picture**: §9.2 of the Win32 spec already reserves this as a future additive direction. Out of scope here.
- **Cursor handling over the canvas boundary**: the Windows hardware cursor sits above the swapchain. If it crosses the canvas boundary, it visually transitions between weaved and 2D regions. Same constraint as #224. Vendor-side fix or accept-the-artifact; not blocking.
- **Apps that aren't `_texture`**: handle apps render directly into the HWND's swap chain and already own all pixels (including the surround region by default). They don't need this extension. If a handle app wants the runtime to weave into a sub-rect, it would first need to opt into texture-mode by providing a `sharedTextureHandle` at session creation.

## Why this over the alternatives we discussed

- **Post-weave 2D overlay layer (new `XR_COMPOSITION_LAYER_PANEL_NATIVE_BIT`)**: more OpenXR-idiomatic, works for handle apps too, but introduces a new layer type. The texture-app idiom already plumbs the canvas rect; adding a sibling "surround texture" call is the smallest API surface that does the job. Reconsider if/when handle apps need the capability.
- **Inverse mask (#224 software variant)**: same DP-vtable plumbing as #224, but the mask is degenerate (constant per session = hardware zone). Not worth the per-frame mask-publish cost for a static rect.
- **Texture-app trick using one buffer**: implicit — relies on "outside canvas is whatever the app left there." Doesn't work because the target swapchain is runtime-owned, not app-owned. The surround texture is the explicit form of this.
