# DisplayXR App Authoring Rules

> **Audience:** anyone (human or coding agent) writing a DisplayXR OpenXR app against this
> runtime. These are the **invariants** that the test apps and shipping demos follow but
> that are easy to get wrong from first principles. Each rule states the invariant, the
> common wrong pattern, the right pattern, and a real `file:line` reference you can open.
>
> **Authoritative source = shipped code, not prose.** Where this doc and an inline spec
> example disagree, the test apps under `test_apps/` win (several spec examples predate the
> multiview model — see [§12 Known discrepancies](#12-known-discrepancies)).

---

## 0. The mental model (read this first)

There is **one** window-binding extension per platform
(`XR_EXT_win32_window_binding` / `XR_EXT_cocoa_window_binding`) with **one knob**: whether
you hand the runtime a *window* or a *shared GPU surface*. That knob defines the three app
classes:

| Class | window handle | shared texture | canvas | 2D surround |
|---|---|---|---|---|
| `_handle` | **real** HWND/NSView | NULL | implicitly the full window | n/a |
| `_texture` | real HWND/NSView | **non-NULL** | a sub-rect (defaults to full client area) | optional |
| `_hosted` | **NULL** (runtime creates one) | NULL | full window | n/a |

**On the "handle = special case of texture" framing you asked about:** the *visual/geometric*
claim is correct and worth teaching — _a full-window canvas with no surround is exactly the
weave a handle app produces._ But the *architectural* subset claim is backwards: in code a
handle app does **less** (renders straight into its window), while a texture app does **more**
(adds a shared-surface round-trip + canvas rect + optional surround). So teach it as:

> "All classes use the same binding struct. **Handle**: give the runtime your window, it
> renders into it. **Texture**: give the runtime a shared surface, it renders the weaved
> *canvas* into a sub-rect of it, you blit that back into your own window — which unlocks
> confining 3D to a sub-rect and filling the 2D area around it. **Hosted**: give it no window
> at all. The full-window/no-surround texture config *is* the handle case, geometrically."

The compositor's 3-way branch makes this concrete:
`src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp:1613-1648` — handle **and** texture both
keep the app's real HWND; only the presence of `shared_texture_handle` separates them; hosted
is the only class with no app window.

---

## 1. Window binding & app class

- **INV-1.1 — Pass a real window for handle/texture; NULL for hosted.** Create your window
  *before* OpenXR init and pass it at session creation via the binding struct
  (`XrWin32WindowBindingCreateInfoEXT` / `XrCocoaWindowBindingCreateInfoEXT`). Hosted apps pass
  NULL and the runtime self-creates a native-resolution window.
  Ref: `test_apps/cube_handle_d3d11_win/main.cpp:675`, `xr_session.cpp:193-194`.

- **INV-1.2 — The texture vs handle difference is one field.** A texture app sets
  `sharedTextureHandle` (D3D11/D3D12) / `sharedIOSurface` (Cocoa) **and still passes its real
  window** (used by the display processor for position/phase tracking). Don't drop the HWND
  when you go texture-mode. Ref: `test_apps/cube_texture_d3d11_win/xr_session.cpp:193-194`.

---

## 2. Display info & rendering modes (`XR_EXT_display_info`)

- **INV-2.1 — Query display info once, by chaining to `XrSystemProperties`.** Chain
  `XrDisplayInfoEXT` onto `XrSystemProperties` at `xrGetSystemProperties` time (before session
  creation). It returns static properties: `displaySizeMeters`,
  `nominalViewerPositionInDisplaySpace` (~`{0,0,0.65}`), `recommendedViewScaleX/Y`,
  `displayPixelWidth/Height`. These are **static** — do **not** re-query or change them on
  resize. Ref: `test_apps/cube_handle_d3d11_win/xr_session.cpp:173-185`; header
  `src/external/openxr_includes/openxr/XR_EXT_display_info.h:46-55` (SPEC_VERSION 13).

- **INV-2.2 — `hardwareDisplay3D` is per-mode, not on `XrDisplayInfoEXT`.** It moved to
  `XrDisplayRenderingModeInfoEXT` (`XR_EXT_display_info.h:237`). Reading it off the display-info
  struct (as an older spec example shows) won't compile against current headers.

- **INV-2.3 — Enumerate rendering modes with the two-call idiom.** Use
  `xrEnumerateDisplayRenderingModesEXT`. Each `XrDisplayRenderingModeInfoEXT` carries
  `modeIndex`, `viewCount`, `viewScaleX/Y`, `tileColumns`, `tileRows`, `viewWidthPixels`,
  `viewHeightPixels`, `hardwareDisplay3D`, and (v13) `isActive` + `isRequestable`. Read
  `isActive` at startup to learn the runtime's current mode without waiting for an event.
  Ref: `test_apps/cube_handle_d3d11_win/xr_session.cpp:324-327`.

- **INV-2.4 — The runtime owns the active mode; you *request*, never *set*.** Key presses are
  requests via `xrRequestDisplayRenderingModeEXT`. Update your local `currentModeIndex` **only**
  when `XrEventDataRenderingModeChangedEXT` arrives. Optimistically mutating local state
  desyncs you from the runtime.
  - Wrong: `xr.currentModeIndex = next;` then render with assumed view count.
  - Right: request, then react. Ref: request `main.cpp:300-319`; event handler
    `test_apps/common/xr_session_common.cpp:375-381`.

- **INV-2.5 — Gate your mode-toggle UI on `isRequestable`.** It's false for non-controller
  workspace clients (the shell owns mode there). HUD does this at `main.cpp:579`.

- **INV-2.6 — Re-derive per-mode state every frame.** Because the mode can change at runtime,
  recompute (don't cache across frames): `modeViewCount`, `tileColumns`, `tileRows`,
  `monoMode = !renderingModeDisplay3D[currentModeIndex]`, and refresh
  `recommendedViewScaleX/Y` from the **active** mode. Ref: `main.cpp:404-415`.

- **INV-2.7 — Eye tracking: `xrLocateViews` always returns populated views.** Query
  `XrEyeTrackingModeCapabilitiesEXT` (chained to `XrSystemProperties`) for supported modes
  (MANAGED=1, MANUAL=2, 0=none). Per-frame, chain `XrViewEyeTrackingStateEXT` to `XrViewState`;
  `isTracking` tells you whether poses are live or a vendor fallback — it does **not** mean the
  views are unpopulated. Ref: `XR_EXT_display_info.h:141-165`.

---

## 3. Views — the locate-views gotcha

- **INV-3.1 — Locate over a MAX-sized buffer (8), render/submit the *active mode's* count.**
  Two distinct counts are in play and conflating them is the single most common bug:
  1. `xrLocateViews` fills an array sized to `XRT_MAX_VIEWS` (8, max across all modes); only the
     first `viewCountOutput` entries are valid.
  2. The number of views you actually **render and submit** is the **active mode's**
     `viewCount`, *not* the locate output and *not* a hardcoded 2.

  - **Wrong** (has shipped as `XR_ERROR_SIZE_INSUFFICIENT` — quad mode = 4 views):
    ```cpp
    XrView views[2];
    xrLocateViews(session, ..., 2, &count, views);   // too small
    for (int eye = 0; eye < 2; eye++) { ... }          // hardcoded 2
    ```
    Documented at `docs/specs/runtime/multiview-tiling.md:176`.
  - **Right** — locate into 8:
    ```cpp
    uint32_t viewCount = 8;  XrView views[8];
    for (uint32_t i = 0; i < 8; i++) views[i] = {XR_TYPE_VIEW};
    xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, views);
    ```
    (`test_apps/common/xr_session_common.cpp:444-451`)
  - **Right** — bound the render loop by the active mode's count, clamp array reads:
    ```cpp
    uint32_t modeViewCount = (renderingModeCount > 0 && idx < renderingModeCount)
        ? renderingModeViewCounts[idx] : 2;
    int eyeCount = monoMode ? 1 : (int)modeViewCount;
    // ...
    int safeIdx = (eye < (int)viewCount) ? eye : 0;        // never read past valid region
    EndFrame(..., projectionViews.data(), eyeCount);        // submit eyeCount, not 2
    ```
    (`test_apps/cube_handle_d3d11_win/main.cpp:404-415,719,761,791-793`)

- **INV-3.2 — Use dynamic/`XRT_MAX_VIEWS`-sized arrays for projection views.** Allocate
  `std::vector<XrCompositionLayerProjectionView>(eyeCount, ...)` each frame. Ref:
  `main.cpp:412`; VK equivalent `test_apps/cube_handle_vk_win/main.cpp:537,566`.

---

## 4. Swapchain sizing & tiling

- **INV-4.1 — There are two swapchains; you only render into one.** The **app swapchain**
  (`xrCreateSwapchain`) holds your tiled atlas; the **target swapchain** holds the weaved output
  and is owned by the compositor — never touch it. They never share images. Ref:
  `docs/specs/runtime/swapchain-model.md:6-21`.

- **INV-4.2 — Size the app swapchain once, to the worst-case atlas across all modes.** Do not
  resize it on window resize or mode change.
  ```cpp
  for (mode i) {
      aw = tileColumns[i] * scaleX[i] * displayPixelWidth;
      ah = tileRows[i]    * scaleY[i] * displayPixelHeight;
      maxAtlasW = max(maxAtlasW, aw);  maxAtlasH = max(maxAtlasH, ah);
  }
  ```
  Ref: `test_apps/common/xr_session_common.cpp:200-225`.

- **INV-4.3 — Per-tile render size = window (canvas) × scaleXY, NEVER display size.** This is
  the one you called out. `scaleX/Y` is the **active mode's** `viewScaleX/Y`.
  - **Wrong:** `renderW = displayPixelWidth / 2;` (the legacy hardcoded hack —
    `multiview-tiling.md:13`).
  - **Right** (clamp to the swapchain's per-tile capacity):
    ```cpp
    uint32_t maxTileW = tileColumns ? swapchain.width  / tileColumns : swapchain.width;
    uint32_t maxTileH = tileRows    ? swapchain.height / tileRows    : swapchain.height;
    renderW = (uint32_t)(g_windowWidth  * recommendedViewScaleX);
    renderH = (uint32_t)(g_windowHeight * recommendedViewScaleY);
    if (renderW > maxTileW) renderW = maxTileW;
    if (renderH > maxTileH) renderH = maxTileH;
    ```
    (`test_apps/cube_handle_d3d11_win/main.cpp:705-717`; VK `cube_handle_vk_win/main.cpp:368-382`)
    For a `_texture` app, substitute **canvas** size for window size.

- **INV-4.4 — Lay tiles top-left, report the exact subImage rect.** Tile `(col,row) = (eye %
  tileColumns, eye / tileColumns)`; viewport at `(col*renderW, row*renderH)`; set
  `projectionViews[eye].subImage.imageRect` to that offset+extent. The compositor crops to the
  content region before the display processor. Ref: `main.cpp:720-758`.

- **INV-4.5 — Mono (2D) mode: 1 view, tile (0,0), full window res.** When `monoMode`,
  `eyeCount = 1`, render at full window resolution, center-eye = average of the located eyes.
  The runtime accepts `viewCount == 1` only in a 2D/non-3D mode. Ref: `main.cpp:415,632-644,707-711`.

- **INV-4.6 — Color space: request an sRGB swapchain.** The display panel is sRGB, and an
  in-process native compositor is a **byte passthrough to the panel** — it does *not* color-manage
  a linear-format swapchain. So the encoding of the bytes you store is what reaches the display.
  - **Do this — request an sRGB swapchain format:** `..._UNORM_SRGB` (D3D11/D3D12),
    `GL_SRGB8_ALPHA8` (GL), `..._SRGB` (Vulkan), `MTLPixelFormat*sRGB` (Metal). Then you are
    correct **either way you render into it**:
    - render in linear and let the GPU encode on write — `GL_FRAMEBUFFER_SRGB` on / an `_SRGB`
      RTV / an sRGB color attachment, *or*
    - write display-referred (already gamma-encoded) bytes directly with GPU sRGB-write off.
    Don't do both (double-encode → washed out).
  - **A linear / UNORM swapchain (`R8G8B8A8_UNORM`, `GL_RGBA8`, …) is NOT color-managed** on the
    in-process path: the compositor passes the bytes straight through with no linear→sRGB encode.
    It's only correct if you write display-referred bytes into it; writing genuinely-linear values
    and expecting the runtime to encode → **too bright**. ("Submit a linear-format swapchain and
    the runtime color-manages it" is a future runtime follow-up, not current behavior.) **Subject to
    change — see [#409](https://github.com/DisplayXR/displayxr-runtime/issues/409):** if the color
    model lands on linear-compose ("Model B"), a linear-format swapchain *would* be honored as
    linear (the runtime would encode at the DP boundary). This bullet describes current ("Model A")
    behavior; the recommendation above (request an sRGB swapchain) stays correct either way.
  - **Data textures (normal, AO, roughness, metalness) are ALWAYS linear — never sRGB.** Only
    albedo/color is sRGB; sample those through an `_SRGB` texture view (or decode in-shader) so
    lighting runs in linear, then write your result into the sRGB swapchain per above.
  - **In-process vs service:** this is the in-process native-compositor contract. The D3D11
    *service* (workspace) path color-manages differently (it linearizes app swapchains into a
    linear atlas — see the canonical reference). As an app author you don't depend on which path
    runs you: request an sRGB swapchain and write a correctly-encoded image, and both paths are
    correct.
  - Reference: PRs [#407](https://github.com/DisplayXR/displayxr-runtime/pull/407) (GL) and
    [#408](https://github.com/DisplayXR/displayxr-runtime/pull/408) (D3D11/D3D12/Vulkan/Metal),
    which established the cross-API sRGB-passthrough contract, plus
    `docs/architecture/compositor-pipeline.md#color-space-handling-d3d11-service-compositor-shell-mode`.
    As of #408 **all five in-process native compositors pass sRGB swapchains through correctly**;
    the broader color-management design is tracked in
    [#409](https://github.com/DisplayXR/displayxr-runtime/issues/409).

---

## 5. Texture apps: canvas sub-rect + 2D surround

### 5a. Canvas sub-rect — `xrSetSharedTextureOutputRectEXT`

- **INV-5.1 — Declare where the weaved 3D lives inside your window.**
  ```c
  xrSetSharedTextureOutputRectEXT(session, int32_t x, int32_t y, uint32_t w, uint32_t h);
  ```
  `(x,y,w,h)` in client-area pixels. **Never called → runtime assumes the full client area**
  (the handle-app geometry). Flows to the DP as `canvas_offset_x/y` + `canvas_width/height`.
  Ref: header `XR_EXT_win32_window_binding.h:161`; example
  `test_apps/cube_texture_d3d11_win/main.cpp:471-483`; re-issue on resize
  `cube_texture_metal_macos/main.mm:1868`.

- **INV-5.2 — The runtime writes the canvas at `(x,y)` in your shared surface, not at origin —
  blit back from that same sub-rect.** Vendor weavers compute lenticular phase from screen-space
  position, so the offset is load-bearing.
  ```
  uvScale  = (w / sharedTexW, h / sharedTexH)
  uvOffset = (x / sharedTexW, y / sharedTexH)
  ```
  Sampling from origin → wrong placement + crosstalk. Ref: `cube_texture_d3d11_win/main.cpp:505-508`;
  `ADR-010` "Read-Back Contract".

- **INV-5.3 — View dims & Kooima use canvas size, not window size, for `_texture` apps.**
  Ref: `multiview-tiling.md:38,50`; `swapchain-model.md:40-44`.

### 5b. 2D surround — `xrSetSharedTextureSurround2DEXT` (D3D11) / `…Surround2DFenceEXT` (D3D12)

- **INV-5.4 — The surround = the 2D pixels *outside* the canvas; the compositor strip-blits the
  non-canvas region 1:1.** Input only, no read-back. Apps whose canvas == full window don't need
  it. **Size it like the multiview shared texture (worst-case atlas), not the HWND** — the
  compositor requires `surround.dims == sharedTexture.dims` for the `CopySubresourceRegion`
  strip-blit (a mismatch is logged once and silently skipped — `comp_d3d11_compositor.cpp`
  `d3d11_blit_surround_strips`). Allocate + register **once**; on resize you do **not**
  reallocate or re-register — just redraw your 2D content into the top-left
  `clientWidth × clientHeight` region (window-aligned, 1:1) and update the canvas rect. The app
  only presents that client-area region, so surround pixels beyond it are never shown.
  Ref: `cube_texture_d3d11_win/main.cpp:1145-1147,1178-1180`; spec §3.6 (corrected).
  ```c
  // D3D11:  PFN_xrSetSharedTextureSurround2DEXT(session, void* handle, uint32_t w, uint32_t h)
  // D3D12:  PFN_xrSetSharedTextureSurround2DFenceEXT(session, void* handle, w, h,
  //                                                  void* fenceHandle, uint64_t awaitFenceValue)
  ```
  Ref: header `XR_EXT_win32_window_binding.h:223,299`.

- **INV-5.5 — Two variants exist because of D3D12 shared-resource limits.** D3D11 synchronizes
  via `IDXGIKeyedMutex` (acquire/release key 0). D3D12-native shared textures often can't expose
  a keyed mutex (`QueryInterface` → `E_NOINTERFACE`), so D3D12 uses a shared `ID3D12Fence`:
  `Signal(fence, N)` after rendering the surround, pass `N` as `awaitFenceValue` (must be
  strictly increasing), runtime does `Wait(fence, N)` before the strip blit. Signal before
  `xrEndFrame`. Ref: D3D11 `cube_texture_d3d11_win/main.cpp:320-321,370,415,1178-1180`; D3D12
  `cube_texture_d3d12_win/main.cpp:481-509,725-730`.

- **INV-5.6 — Clear the surround registration with a NULL handle on resize/shutdown.** Ref:
  `cube_texture_d3d11_win/main.cpp:1300`, `cube_texture_d3d12_win/main.cpp:1666`.

### 5c. Shared-surface sizing

- **INV-5.7 — Allocate the shared surface once at worst-case atlas size; never resize it.** Same
  formula as INV-4.2 (`max over modes of tileColumns×viewScale×displayPixels`). A 2560×1440 BGRA8
  surface is ~14 MB — over-allocation is negligible, and it removed the old
  `xrUpdateSharedSurfaceEXT` API. The canvas rect (not the surface) is what changes on resize.
  Ref: `ADR-010`; `cube_texture_d3d11_win/main.cpp:1082-1088`.

---

## 6. Kooima projection

- **INV-6.1 — With `XR_EXT_display_info` enabled, `xrLocateViews` is RAW mode and YOU own the
  camera.** It returns screen-centered eye positions (meters), identity orientation, advisory
  FOV — regardless of the reference space (pass LOCAL). The runtime applies no
  convergence/comfort; you build your own asymmetric off-axis frustum. Ref:
  `XR_EXT_display_info.md:976-989`.

- **INV-6.2 — Without the extension (legacy/RENDER_READY), the runtime returns converged poses +
  FOV; you still build the matrix from `XrFovf`** (the runtime never returns a matrix).

- **INV-6.3 — Use window-relative (canvas-relative) Kooima, not full-panel.** Feeding full
  `displaySizeMeters` + display-center eyes into Kooima for a windowed app gives wrong
  perspective/aspect. Convert: `pxSize = displaySizeMeters / swapchainPixels`, screen =
  `windowPixels × pxSize`, shift eyes to window center (flip Y: screen Y-down → eye Y-up). Ref:
  `main.cpp:443-474`; math `docs/architecture/kooima-projection.md:148-176`; helper
  `display3d_compute_projection()` in `xr_session_common.cpp:113-138`.

- **INV-6.4 — Matrices are column-major (GL/VK/Metal); DirectX callers transpose.** Ref:
  `ColumnMajorToXMMatrix` `xr_session_common.h:223`; `kooima-projection.md:360-363`.

---

## 7. Frame capture — `XR_EXT_atlas_capture` (`xrCaptureAtlasEXT`)

> This replaced the old per-API app-side readback (`CaptureAtlasRegion{D3D11,D3D12,GL,VK,Metal}`),
> deleted in the #396 W6 refactor. **Do not reintroduce app-side staging-texture readback.**

- **INV-7.1 — Capture is runtime-owned and fire-and-forget.** Call `xrCaptureAtlasEXT` from your
  render loop on a user trigger; it *latches* the request and the readback runs at your next
  `xrEndFrame`. `XR_SUCCESS` means "accepted," not "file on disk yet." It must be non-blocking
  (in-process it shares the compositor thread).
  ```c
  XrResult xrCaptureAtlasEXT(XrSession, const XrAtlasCaptureInfoEXT*, XrAtlasCaptureResultEXT* /*may be NULL*/);
  ```
  Header `src/external/openxr_includes/openxr/XR_EXT_atlas_capture.h`; impl
  `src/xrt/state_trackers/oxr/oxr_capture.c`; design `docs/roadmap/unified-atlas-capture.md`.

- **INV-7.2 — `pathPrefix` is an in-struct char[256] with NO extension.** The runtime appends
  `_atlas.png`. Pass a path prefix only.
  ```c
  XrAtlasCaptureInfoEXT info = {XR_TYPE_ATLAS_CAPTURE_INFO_EXT};
  info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT;   // app content, no chrome
  strncpy_s(info.pathPrefix, prefix.c_str(), _TRUNCATE);     // no ".png"
  xr.pfnCaptureAtlasEXT(xr.session, &info, nullptr);
  ```
  Stages: `PROJECTION_ONLY_EXT` (0) = your tile content only (what test apps use);
  `POST_COMPOSE_EXT` (1) = full composed frame incl. HUD/quad/cursor/chrome.

- **INV-7.3 — Guard mono layouts.** Skip capture when `tileColumns <= 1 && tileRows <= 1`.
  Ref: helper `test_apps/common/atlas_capture.cpp:168-197`; Windows call site
  `cube_handle_d3d11_win/main.cpp:772-776`; macOS inline `cube_handle_vk_macos/main.mm:3161-3187`.

- **INV-7.4 — In-process vs IPC routing is automatic, but stage support differs over IPC.**
  In-process → runtime reads its own atlas. Over IPC/service: `PROJECTION_ONLY` works for a
  single client; `POST_COMPOSE` requires workspace (shell) mode and otherwise returns
  `XR_ERROR_FEATURE_UNSUPPORTED`. The optional result's `tileColumns/Rows` and `eyeLeftM/RightM`
  are zero on the in-process path. Ref: `oxr_capture.c:179-187,140-148`.

- **INV-7.5 (rendering, not capture) — Upload a full mip chain + trilinear sampler on every
  backend.** "Mip parity" in W6 means your texture must look identical across GL/Metal/VK/D3D so
  captures are comparable. GL `glGenerateMipmap`; Metal `generateMipmapsForTexture`; VK
  `vkCmdBlitImage` chain; D3D equivalents. (The compositor's own staging texture is single-level
  — it captures whatever you rendered.)

---

## 8. App folder layout — what to include

A DisplayXR app is a normal OpenXR client app + distribution machinery. Canonical layout
(union of in-tree test app and shipping demo):

```
my_app/
├── CMakeLists.txt          # find OpenXR loader, link common, embed manifest, copy loader DLL + assets + sidecar
├── main.cpp / main.mm      # window + render loop + input + HUD glue (write fresh)
├── xr_session.{cpp,h}      # instance/session creation, graphics binding + window-binding ext (write fresh)
│                           #   #define XR_USE_GRAPHICS_API_* and XR_USE_PLATFORM_* BEFORE including openxr.h
├── <renderer>.{cpp,h}      # per-API renderer (D3D11 apps reuse common/d3d11_renderer)
├── resource.rc             # Windows VERSIONINFO (Windows only)
├── *.manifest              # Win32 app manifest: PerMonitorV2 DPI, supportedOS, asInvoker (Windows only)
├── displayxr/              # shell sidecar bundle (see §9) — copied next to exe by displayxr_install_manifest()
│   ├── <exe_basename>.displayxr.json
│   ├── icon.png            # 512×512 (2D tile)
│   └── icon_sbs.png        # 1024×512 (3D tile)
└── assets/ textures/       # bundled content
```

Shipping demos add, on top of that: vendored `openxr_includes/openxr/`, a synced copy of
`common/`, an `installer/` (NSIS `.exe` on Windows, `.pkg` on macOS), `.github/workflows/`
(PR compile-check + tag → installer + `versions-bump` dispatch), a `README.md` with a
**runtime-compat covenant** ("Requires DisplayXR runtime vX.Y.Z or newer"), a `CLAUDE.md`
restating "couple to the runtime only via the OpenXR extension wire protocol — never include
runtime-internal source," and `scripts/dev_register.bat`.

- **INV-8.1 — Reuse `common/`, don't reinvent it.** Two libs are built from `test_apps/common/`:
  `sr_common_base` (API-agnostic: `xr_session_common`, `display3d_view`/`camera3d_view` (the only
  files that build on **both** Windows and macOS), `input_handler`, `window_manager`, `logging`,
  `atlas_capture`, `stb_*`) and `sr_common` (Windows/D3D11-only: `d3d11_renderer`, HUD). **Always**
  use `displayxr_manifest.cmake`'s `displayxr_install_manifest(target dir)`. Write fresh only:
  `main.*`, `xr_session.*`, your non-D3D11 renderer, the sidecar+icons, `resource.rc`/`.manifest`.
  macOS apps are self-contained in `main.mm` and cherry-pick a few `common/*.c/.mm` files rather
  than linking `sr_common`. `displayxr-demo-mediaplayer` is a clean from-scratch model that
  avoids `common/` entirely.

- **INV-8.2 — Ship the OpenXR loader with your app; never compile in a runtime path.** Runtime
  selection is the `XR_RUNTIME_JSON` env var (dev) or registry/ActiveRuntime (installed). Copy
  the loader next to the exe using `$<TARGET_FILE:OpenXR::openxr_loader>` (not
  `IMPORTED_LOCATION`, which Khronos CONFIG leaves NOTFOUND).

- **INV-8.3 — Extension-header include set.** Always: `openxr/openxr.h`,
  `openxr/openxr_platform.h`, `openxr/XR_EXT_display_info.h`. Platform window binding:
  `XR_EXT_win32_window_binding.h` (Win) / `XR_EXT_cocoa_window_binding.h` (mac). Optional:
  `XR_EXT_atlas_capture.h`, `XR_EXT_workspace_file_dialog.h`, `XR_EXT_spatial_workspace.h`,
  `XR_EXT_macos_gl_binding.h`. Vendor a pinned copy from `src/external/openxr_includes/` (or the
  `displayxr-extensions` repo), record the source commit, never edit in place.

- **INV-8.4 — Run wrappers set two env knobs.** `XR_RUNTIME_JSON` (which runtime DLL the loader
  loads) and, on POSIX, `XRT_PLUGIN_SEARCH_PATH` (where the runtime finds the DP plug-in; Windows
  uses the registry). Non-Vulkan Windows apps also set `VK_LOADER_LAYERS_DISABLE=*` (issue #105).
  These are generated by `scripts/build_windows.bat` / `build_macos.sh`, not hand-written.

---

## 9. Manifest & logos (workspace discovery)

Discovery is **manifest-gated**: an OpenXR exe with no sidecar never appears in any workspace
launcher. The bare runtime ignores manifests — discovery is the workspace layer.

- **INV-9.1 — Ship a `<exe_basename>.displayxr.json` (schema v1).** Full field reference:

  | Field | Type | Req | Meaning | Default |
  |---|---|---|---|---|
  | `schema_version` | int | **yes** | must be exactly `1` | — |
  | `name` | string | **yes** | tile name, 1–64 chars UTF-8 | — |
  | `type` | string | **yes** | `"3d"` (OpenXR session) or `"2d"` (legacy Win32, HWND-captured) | — |
  | `exe_path` | string | cond. | absolute exe path — **required** in registered mode, **must be absent** in sidecar mode | — |
  | `icon` | string | no | rel. path to 2D icon (PNG/JPEG), **512×512** | text tile |
  | `icon_3d` | string | no | rel. path to stereo icon, **1024×512**; **requires `icon` too** | — |
  | `icon_3d_layout` | string | no | `"sbs-lr"` / `"sbs-rl"` / `"tb"` / `"bt"` | `"sbs-lr"` |
  | `category` | string | no | `test` / `demo` / `app` / `tool` (free-form ok) | `"app"` |
  | `display_mode` | string | no | `"auto"` or forwarded to runtime | `"auto"` |
  | `description` | string | no | tooltip, ≤256 chars | `""` |

  Reserved (do NOT use for custom data): `version`, `publisher`, `homepage`, `min_runtime`,
  `required_extensions`, `screenshots`, `trailer`, `pose`, `window_size`, `args`, `working_dir`.
  Real example: `displayxr-demo-gaussiansplat/windows/displayxr/gaussian_splatting_handle_vk_win.displayxr.json`.
  Minimum valid: `{ "schema_version": 1, "name": "My App", "type": "3d" }`. Spec:
  `docs/specs/runtime/displayxr-app-manifest.md`; validator (authoritative):
  `displayxr-shell-pvt/src/shell_app_scan.c`.

- **INV-9.2 — The 512×512 is the 2D logo (`icon`); the 1024×512 is the 3D logo (`icon_3d`).**
  The 3D logo is two 512×512 eye views packed side-by-side (left eye left for `sbs-lr`) → 1024×512.
  PNG (RGBA) or JPEG. `icon_3d` **requires** `icon` to be set (used as the flat fallback). Paths
  resolve **relative to the manifest file**, never the exe/CWD. (The scanner checks the files
  exist + are readable PNG/JPEG but does **not** enforce pixel dimensions — 512×512/1024×512 are
  authoring conventions, and what every real asset uses.)

- **INV-9.3 — Author the 3D icon from the stereo camera pair.** Two viewpoints at ~0.5–1.0 m
  convergence, **parallax budget ±2% of image width**, one static snapshot, composited per
  `icon_3d_layout`. The shell currently fully renders only `sbs-lr` — author SBS-LR for now
  (`shell_launcher.cpp:133`). Embed the same source as your PE/Start-Menu icon so the taskbar
  matches the tile.

- **INV-9.4 — Sidecar vs registered placement.** *Sidecar* (dev / self-bundled): manifest +
  icons next to the `.exe`, **omit** `exe_path`. *Registered* (3rd-party install outside Program
  Files): drop into `%LOCALAPPDATA%\DisplayXR\apps\` (per-user) or `%ProgramData%\DisplayXR\apps\`
  (system, elevated), **set** `exe_path`. Don't hand-edit `registered_apps.json` (that's shell UI
  state — poses/MRU/hide — not the source of truth). How-to: `docs/getting-started/ship-a-manifest.md`.

- **INV-9.5 — Workspace-CONTROLLER registration is a different thing.** If you're building a
  *shell/controller* (drives the workspace, not just appears in it), that's registry keys under
  `HKLM\Software\DisplayXR\WorkspaceControllers\<id>` — see
  `docs/specs/runtime/workspace-controller-registration.md`. Most apps do **not** do this.

---

## 10. Being a good workspace citizen

- **INV-10.1 — Under a workspace, the controller owns the display mode.** When
  `DISPLAYXR_WORKSPACE_SESSION=1`, your `xrRequestDisplayRenderingModeEXT` calls are silently
  no-opped and `isRequestable` is false. Don't fight it; gate your mode UI (INV-2.5).

- **INV-10.2 — File picking goes through `XR_EXT_workspace_file_dialog` when available.** Call
  `xrRequestFilePickerEXT`. `XR_FILE_PICKER_FALLBACK_TIER0_EXT` → fall back to a flat OS dialog
  (the controller handles z-order). `XR_ERROR_FEATURE_UNSUPPORTED` → not under a workspace, call
  the OS dialog directly. The *app* declares nothing in its manifest for this; the controller
  advertises `SupportsFileDialog=1`. Windows-only today. Spec:
  `docs/specs/extensions/XR_EXT_workspace_file_dialog.md`.

- **INV-10.3 — Know which compositor you're talking to.** In-process (`_handle`/`_texture`/
  `_hosted`) you drive a **native per-API compositor** in your own process. Under a workspace or
  `XRT_FORCE_MODE=ipc` you drive a **client compositor → IPC → the D3D11 service multi-compositor**
  (Metal on macOS), which imports each app's shared texture and composites N apps at their window
  poses, then hands the atlas to the display processor. Every path is native to the graphics API —
  there is no cross-API intermediary. Authoritative: `docs/architecture/multi-compositor.md`; live
  code `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (`multi_compositor_render`).

---

## 11. Quick checklist (paste into a PR description)

- [ ] Window passed for handle/texture, NULL for hosted (INV-1.1); HWND kept even in texture mode (INV-1.2)
- [ ] Display info queried once via `XrSystemProperties`, treated as static (INV-2.1)
- [ ] Modes enumerated; active mode tracked via the *event*, not set locally (INV-2.4); per-mode state re-derived each frame (INV-2.6)
- [ ] `xrLocateViews` into an 8-wide buffer; render/submit `eyeCount` from the active mode, not 2 (INV-3.1)
- [ ] App swapchain sized once to worst-case atlas (INV-4.2); per-tile = window/canvas × scaleXY, never display (INV-4.3)
- [ ] Color space: request an **sRGB swapchain** and write a correctly-encoded image (linear render + GPU sRGB-write, or display-referred bytes — not both); linear/UNORM swapchain is not color-managed; data textures always linear (INV-4.6)
- [ ] (texture) canvas rect set; blit back from `(x,y,w,h)` sub-rect, not origin (INV-5.2); surround cleared on resize (INV-5.6)
- [ ] (texture) shared surface sized once to worst-case (INV-5.7)
- [ ] Window-relative Kooima; matrices transposed for DirectX (INV-6.3/6.4)
- [ ] Capture via `xrCaptureAtlasEXT`, prefix without extension, mono-guarded; no app-side readback (INV-7.1/7.2/7.3)
- [ ] Full mip chain + trilinear on every backend (INV-7.5)
- [ ] Loader DLL shipped next to exe; no compiled-in runtime path (INV-8.2)
- [ ] `.displayxr.json` (schema 1) + `icon.png` (512×512) + `icon_sbs.png` (1024×512, sbs-lr) shipped (INV-9.1/9.2)

---

## 12. Discrepancy resolutions (history)

The three spec-vs-code discrepancies found during research have been resolved:

1. **Surround texture size — RESOLVED (spec corrected).** The contract is: **content** is
   HWND-client-area-aligned, 1:1, redrawn on resize; **allocation** is the worst-case-atlas size
   (= the multiview shared texture), allocated/registered once to avoid a destroy/recreate on
   every resize. The spec's old "must equal HWND client area / re-register on resize /
   `XR_ERROR_VALIDATION_FAILURE`" language was wrong (the runtime checks against the shared-texture
   dims and *skips* on mismatch, it never rejects). `XR_EXT_win32_window_binding.md` §3.6 was
   updated to match the shipped runtime + this contract. See INV-5.4.

2. **macOS `cube_texture_metal_macos` divergence — TRACKED.** The macOS sample uses the
   offscreen/IOSurface mode (not real-view + shared-surface) and omits the surround API. Porting
   it is tracked in [#406](https://github.com/DisplayXR/displayxr-runtime/issues/406). Until then,
   the "real handle + shared surface + surround" model in §5 is **Windows-accurate**; macOS is the
   exception.

3. **Stale `XR_EXT_display_info.md` examples — RESOLVED, plus a full v13 spec refresh.** The
   view-count hardcoding (`XrView views[2]` / `for (eye<2)`) was fixed to the `XRT_MAX_VIEWS`(8)-wide
   / active-mode-count pattern across all example blocks, with banners pointing here + to the test
   apps as authoritative. Beyond the examples, the spec was brought up to the v13 header: bumped
   "Spec version: 6" → 13; **formally documented the previously-undocumented v8 enumeration API**
   (`XrDisplayRenderingModeInfoEXT` + `xrEnumerateDisplayRenderingModesEXT`, incl. the v13 `isActive`
   / `isRequestable` fields); corrected every `hardwareDisplay3D`-on-`XrDisplayInfoEXT` reference
   (it's per-mode + event now); fixed the version history (corrected the misleading v12 "event-only"
   entry, removed the wrong "header frozen at v12" claim, added the v13 row dated from #234). The
   eye-tracking (v6) section already matched the header.
