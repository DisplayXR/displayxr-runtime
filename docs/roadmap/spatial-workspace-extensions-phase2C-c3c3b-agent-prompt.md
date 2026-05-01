# Phase 2.C C3.C-3b agent prompt — title text via DirectWrite

Self-contained prompt for a fresh agent session implementing the
remaining title-text deliverable on the controller-owned chrome pill.
Drop into a new session as the user message after `/clear`. The agent
has no memory of the prior design conversations — this prompt assumes
nothing.

---

## What's already shipped

You're picking up a workspace-controller chrome architecture that is
already feature-complete except for the per-client title text. The
DisplayXR Shell renders a glassy floating pill above each workspace
client's content with:

- Rounded SDF pill background (frosted blue, edge highlight)
- App icon (left) — per-client PNG loaded by PID → exe → registered_app
  manifest icon path, with a Win32 .ico extraction fallback for
  unregistered apps. Cached PNGs under `%TEMP%\displayxr_app_icon_*`.
- 8-dot grip handle (center)
- Three buttons (right): close (red, ×), minimize (gray, −), maximize
  (gray, □) — all rendered procedurally in the same SDF pixel shader.
- Hover-fade ease-out cubic, hit-test plumbing for click/drag, runtime-
  side auto-anchor so the pill tracks the window edge in real time
  during resize.

Branch: `feature/workspace-extensions-2C`. The chrome shader + helpers
live in `src/xrt/targets/shell/shell_chrome.cpp`. The runtime owns
zero pixels of UI policy — chrome appearance is fully controller-owned.

`docs/roadmap/spatial-workspace-extensions-phase2C-status.md` is the
authoritative current state. Read it first.

## C3.C-3b job

Add per-client app-name text rendered between the icon (left) and the
grip dots (center) of the pill. Match the reference concept image at
`Pictures/DisplayXR/Shell UI Concept.png` — clean white sans-serif,
well-proportioned, no overlap with the icon or dots. **Adaptive
width**: if the available pill space (icon-right-edge → dots-left-edge,
minus padding) cannot fit the text without overlap, skip the text
entirely and render only icon + dots + buttons.

End-state visual:
- Pill renders title between icon and dots when there's room
- Pill renders icon + dots + buttons (no text) when too narrow
- Text re-renders when the title string changes (rare); not on hover or
  resize (those use cached title texture)

## Files to edit

Primary:
- `src/xrt/targets/shell/shell_chrome.cpp` — add DirectWrite/Direct2D
  init, per-slot title texture, title-rect compute, shader extension
  to sample the title texture, adaptive-width logic
- `src/xrt/targets/shell/shell_chrome.h` — extend `shell_chrome_on_client_connected`
  to take a `const char *title` (the user-facing app name to render)
- `src/xrt/targets/shell/main.c` — resolve the title string per client
  (parallel to `shell_resolve_icon_for_pid`); pass to chrome create
- `src/xrt/targets/shell/CMakeLists.txt` — link `dwrite` + `d2d1`

Don't touch the runtime, IPC, or state tracker — text rendering is
purely a controller concern.

## Architecture

The pill is rasterized into a 512×64 px sRGB chrome image. The image
is sampled by the runtime's blit shader and stretched onto the pill
quad. Per the existing implementation:

- **Pill background, dots, buttons, glyphs** are rasterized procedurally
  in `shell_chrome.cpp` 's HLSL pixel shader, in pill-space meters,
  inside one PS pass.
- **App icon** is sampled from a per-slot D3D11 SRV (loaded from PNG)
  via `Texture2D icon_tex : register(t0)`. The PS skips the sample
  when `has_icon = 0`.

For the title text, follow the **icon pattern**: bake the rendered
title into a small per-slot D3D11 texture, sample it in PS at register
`t1`, gate via `has_title` cbuffer flag.

### DirectWrite / Direct2D init (once at `shell_chrome_create`)

- `D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, ...)` →
  `ID2D1Factory`
- `DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, ...)` →
  `IDWriteFactory`
- `IDWriteFactory::CreateTextFormat("Segoe UI Variable", NULL,
  DWRITE_FONT_WEIGHT_NORMAL, ...)` at a font size you'll pick to match
  the reference. The chrome image is 512×64 px representing an 8 mm-tall
  pill — text height should be roughly 60-70 % of pill height
  (`~38-44 px` in image coords). Tune to taste.
- Cache these on the `shell_chrome` struct.

### Title texture per slot

Each chrome_slot needs:
- `ComPtr<ID3D11Texture2D> title_texture`
- `ComPtr<ID3D11ShaderResourceView> title_srv`
- A cached title string + measured width-in-meters (so adaptive logic
  can decide skip-or-render each `render_pill` call)

To bake the texture:

1. Create an `ID2D1RenderTarget` over a small offscreen D3D11 texture
   (or use a D2D1_RENDER_TARGET_PROPERTIES with shared D3D handle).
   Alternative: WIC bitmap render target → CopyResource into D3D11.
   Pick whichever is easier — both work.
2. `IDWriteFactory::CreateTextLayout(text, len, format, max_w, max_h, &layout)`
3. `IDWriteTextLayout::GetMetrics(&metrics)` — gives `metrics.width`,
   `metrics.height` in DIPs (1 DIP = 1/96 in). Convert to image-pixel
   width.
4. Allocate texture sized `(measured_w_px, line_height_px)` with format
   `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`, `D3D11_USAGE_DEFAULT` so D2D can
   render into it.
5. D2D `BeginDraw` → `Clear({0,0,0,0})` → `DrawTextLayout` with white
   brush → `EndDraw`.
6. Create SRV.
7. Store on slot + cache the title string + measured width-in-meters
   (= `measured_w_px / chrome_image_width_px * pill_w_m`, but pill_w_m
   varies — store the image-space width and compute pill-space width
   per render).

### Shader extension

Add to `PillCB`:
```c
float has_title;
float title_image_w_px;  // width of title texture in image pixels
float title_left_uv;     // u-coord (0..1) where title starts in chrome image
float title_right_uv;    // u-coord where title ends
```

Add to HLSL:
```hlsl
Texture2D title_tex : register(t1);
// (reuse icon_samp at register s0 — same linear-clamp sampler is fine)

// In PS, after icon, before dots/buttons:
if (has_title > 0.5 && input.uv.x >= title_left_uv && input.uv.x < title_right_uv) {
    // Sample title texture. UV.x maps to title_left_uv..title_right_uv → 0..1 in title
    // texture; UV.y maps directly (chrome image height = title texture height,
    // assuming we render at full chrome image height; otherwise also scale y).
    float tu = (input.uv.x - title_left_uv) / (title_right_uv - title_left_uv);
    float tv = input.uv.y; // adjust if title texture height ≠ chrome image height
    float4 title_sample = title_tex.Sample(icon_samp, float2(tu, tv));
    result = over(title_sample, result);
}
```

Bind in `render_pill`:
```cpp
ID3D11ShaderResourceView *srvs[2] = {
    slot.icon_srv ? slot.icon_srv.Get() : nullptr,
    slot.title_srv ? slot.title_srv.Get() : nullptr,
};
sc->context->PSSetShaderResources(0, 2, srvs);
```

### Adaptive width (per-render call)

Compute available text rect in pill-space meters:

```cpp
const float icon_right_m = btn_width_m + icon_size_m;
const float grip_left_m  = pill_w_m * 0.5f - grip_total_w * 0.5f;
const float padding_m    = pill_h_m * 0.25f; // 2 mm
const float text_avail_m = grip_left_m - icon_right_m - 2 * padding_m;
const float text_needed_m = slot.title_width_m; // measured at bake time, scaled to current pill_w
```

If `text_needed_m > text_avail_m`, set `has_title = 0` in cbuffer.

Else compute `title_left_uv` / `title_right_uv` from
`(icon_right_m + padding_m) / pill_w_m` etc.

### Title string source

Same path as the icon resolution in `main.c`:

1. Get PID via `XrWorkspaceClientInfoEXT`.
2. Open process, `QueryFullProcessImageName` → exe path.
3. Match against `g_registered_apps[].exe_path`. If found, use
   `g_registered_apps[i].name` (friendly sidecar name like
   "Cube D3D11 (Handle)").
4. Fallback: use `cinfo.name` (OpenXR application name; for test apps
   this is something like `SRCubeOpenXRExt`).
5. Truncate the " (N)" duplicate-instance suffix the runtime appends
   (mirror the existing logic at `comp_d3d11_service.cpp:7551`).

Pass the resolved string into the new `shell_chrome_on_client_connected`
parameter.

## Reference: existing patterns

- **Icon pattern** in `shell_chrome.cpp:load_icon_png` (lines ~430-475):
  load PNG, create immutable D3D11 texture + SRV, store on slot. Mirror
  this for the title texture (but use D2D draw instead of stb_image
  decode).
- **Cbuffer extension pattern** in the same file: existing `has_icon` /
  `icon_size_m` fields are the model for `has_title` / `title_left_uv`
  etc. Don't forget the `static_assert(sizeof(PillCB) == ...)` size
  check.
- **Per-render adaptive logic**: today the icon size is computed each
  render call from `pill_h_m`. Same kind of computation for the title
  rect — adaptive_skip / not_skip based on current pill width.
- **Color**: white-tinted `(0.97, 0.98, 1.0, 0.95)` (`glyph_col` in the
  existing shader). Match this for visual consistency with the button
  glyphs.

## Build + smoke

- Always build via `scripts\build_windows.bat build`. Per
  `feedback_use_build_windows_bat.md` — never call cmake/ninja directly
  on Windows.
- After every build, copy `_package/bin/{DisplayXRClient.dll,
  displayxr-service.exe, displayxr-shell.exe,
  displayxr-webxr-bridge.exe}` to `C:\Program Files\DisplayXR\Runtime\`
  (per `feedback_dll_version_mismatch.md` — strict git-tag check
  between client and service).
- Smoke: `_package\bin\displayxr-shell.exe
  test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
  test_apps\cube_handle_d3d12_win\build\cube_handle_d3d12_win.exe`
- Self-verify with the runtime atlas screenshot trigger
  (per `reference_runtime_screenshot.md`):
  `touch /c/Users/Sparks\ i7\ 3080/AppData/Local/Temp/workspace_screenshot_trigger`
  → `Read` the resulting PNG. Use the long-form path
  (`/c/Users/Sparks i7 3080/...`), NOT the short-name `SPARKS~1` form
  (Claude Code's path-suspicion check trips on the short form). The
  user has pre-authorized read/write under `%TEMP%`.

## Acceptance criteria

1. **Visual parity with the concept image**: title text is white,
   sans-serif, well-proportioned (height ≈ 70 % of pill height,
   centered vertically). Reference: `Pictures/DisplayXR/Shell UI Concept.png`.
2. **Adaptive**: at default 13 cm pill width, both cube apps show their
   names. Resize the window narrow enough that the title would overlap
   icon or dots — title disappears, no garbled overlap. Make wide
   again — title reappears.
3. **No regressions**: hover-fade still works, click handlers still
   work, resize tracks edge, icon still shows.
4. **No re-render thrash**: title texture is baked ONCE per app name;
   not re-baked on hover-fade, resize, or other state changes.
5. **Idle CPU stays at zero**: no new poll loops; the existing
   event-driven main loop covers everything (chrome_alive +
   `shell_chrome_is_animating` already keep the right cadence during
   fades).

## Hard-won lessons (DO NOT re-discover)

1. **Build-via-script, deploy-to-Program-Files** every iteration. The
   shell launches as elevated and reads from
   `C:\Program Files\DisplayXR\Runtime\` — failing to copy after a
   rebuild silently runs the OLD binaries. `feedback_dll_version_mismatch.md`.
2. **Atlas screenshot path uses long-form**, not `SPARKS~1`.
   `reference_screenshot_temp_permission.md`.
3. **Don't push or `/ci-monitor`** until the user signs off on the
   visual. `feedback_test_before_ci.md`.
4. **Phase 2.C C3.B-debug commit-message format**: short, describes
   what + why + the surprising bit, references commits / files when
   relevant. Two recent good models: `bb601f938` and `0160c1567`.
5. **chrome_image is 512×64 px sRGB**. Pill-space meters vary per
   client. The shader rasterizes in pill-space-meters today (see
   existing `pill_size_m` cbuffer field) — title sampling needs to
   harmonize with this. Title texture's UV-x should map to a slice of
   the chrome image's UV.

## Verification flow

1. Build, deploy, smoke.
2. Atlas screenshot — verify both pills show app names between icon and
   dots, white text, well-proportioned.
3. Resize one cube window narrower (drag right edge inward) — title
   should disappear when there's not enough space.
4. Resize back wide — title reappears.
5. Hover over each window — fade-in / fade-out should still work; title
   fades together with rest of pill.
6. Click close → window exits. Click max → toggles fullscreen.

If all 6 pass: commit + update the phase-2C status doc to mark C3.C-3b
✅. Then move on to C6 (test app smoke + final doc refresh).

## Phase 2.C status doc

Authoritative state:
[`docs/roadmap/spatial-workspace-extensions-phase2C-status.md`](spatial-workspace-extensions-phase2C-status.md).
Read it first. Then read the `bb601f938` commit message for the most
recent architectural decisions (auto-anchor, event wakeup, .ico
fallback). Then start implementing.

Estimated session length: 2-3 hours of focused work + verification.
~300-500 lines of new code. The patterns are established (icon support
shipped in this branch); you're applying the same shape to text.
