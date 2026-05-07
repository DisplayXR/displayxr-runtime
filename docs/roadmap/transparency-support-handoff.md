# Transparency Support — Handoff for Windows Agent

**Branch:** `feature/transparency-support` (off `main`)
**Started:** 2026-05-07 on macOS by Claude / David
**Plan:** `~/.claude/plans/ok-i-need-a-melodic-volcano.md` (kept locally; summarized below)

## Why this branch exists

Today, transparent OpenXR surfaces work only on D3D11 standalone, only via a Win32-specific extension, and only because the **app pre-fills its swapchain with a chroma key color** that the runtime strips post-weave (was at `comp_d3d11_compositor.cpp:1431–1614`). Goals of this work:

1. Apps just say "my texture has alpha" — the chroma-key trick becomes a Leia-DP implementation detail invisible to apps.
2. Workspace apps composite transparent client tiles trivially. Workspace's own output stays opaque (documented limitation).
3. Cover all platforms / APIs (D3D11, D3D12, GL, Vulkan on Windows; Metal/GL/Vulkan on macOS via NSWindow).
4. Update `displayxr-unity-test-transparent` to ride the new mechanism.

API surface (final design):

| Concern | Mechanism |
|---|---|
| "This layer's texture has per-pixel alpha" | Standard `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` (already plumbed end-to-end including IPC) |
| "This output window should be transparent" | `XR_EXT_win32_window_binding.transparentBackgroundEnabled` (existing) + new `XR_EXT_cocoa_window_binding.transparentBackgroundEnabled` (PR #4) |
| Optional chroma-key override | `XR_EXT_win32_window_binding.chromaKeyColor` — non-zero = app-supplied, **zero = DP picks** (new behavior) |

## What's done on this branch (2 commits)

### `924153ef7` — DP vtable additions

- `is_alpha_native(xdp) -> bool` — DP declares whether it preserves alpha through to its output (sim_display) vs needs the chroma-key trick (Leia).
- `set_chroma_key(xdp, key_color, transparent_bg_enabled)` — compositor informs DP of session-level transparency config. DP that needs chroma-key uses this to enable its internal pre-weave fill + post-weave strip.
- Added to all 5 DP variants (Vulkan, D3D11, D3D12, GL, Metal) + matching inline helpers. Both methods are optional (NULL = false / no-op). Pure plumbing — **no behavior change**.

### `ea591435c` — Leia D3D11 DP internalization (the meat of PR #1)

Compositor side (`comp_d3d11_compositor.cpp`, `−245 LOC`):
- Removed `ck_*` fields, forward decls, ~190 LOC of shader / pass code, `<d3dcompiler.h>` include.
- Constructor signature unchanged.
- After DP factory succeeds, calls `xrt_display_processor_d3d11_set_chroma_key(c->display_processor, chroma_key_color, transparent_background)` — forwards the constructor params it was already taking.

Leia D3D11 DP side (`leia_display_processor_d3d11.cpp`, `+506 LOC`):
- New chroma-key fields on impl struct: `ck_enabled`, `ck_color`, fill PS / strip PS / sampler / constants, fill RTV+SRV+tex, strip SRV+tex.
- New methods: `leia_dp_d3d11_is_alpha_native` (returns `false`), `leia_dp_d3d11_set_chroma_key` (stores; `key=0` → `kDefaultChromaKey = 0x00FF00FF` magenta).
- New static helpers: `ck_init_pipeline`, `ck_ensure_fill_target`, `ck_ensure_strip_source`, `ck_run_pre_weave_fill`, `ck_run_post_weave_strip`, `ck_release_resources`.
- `process_atlas` now wraps the SR weaver with pre-fill (3D mode) and post-strip (2D + 3D) when `ck_should_run()`. **No behavior change when `ck_enabled == false`** (the common case).
- Reuses existing `blit_vs` (fullscreen quad VS, 4 verts, TRIANGLESTRIP) for both fill and strip — no new VS.

Pre-weave fill shader (new):
```hlsl
float3 rgb = lerp(chroma_rgb, c.rgb, c.a);
return float4(rgb, 1.0);  // opaque RGB to weaver
```
Handles both flows correctly:
- **Legacy** (alpha=1 atlas, app pre-filled with key RGB): `lerp(key, src.rgb, 1.0) == src.rgb` → no-op, identical to today.
- **New** (alpha-bearing atlas, transparent regions = alpha=0): `lerp(key, ?, 0) == key` → fills with key color before weaving.

Post-weave strip shader: equality test on RGB → alpha=0 for matches, premultiplied for DWM. **Moved verbatim** from the compositor; no algorithmic change.

## What's been validated

- macOS build (`./scripts/build_macos.sh`): all 15 dependent TUs rebuilt cleanly after touching the modified DP headers. sim_display drivers (which include `xrt_display_processor*.h`) compile.
- `git diff --stat` checks reasonable: `+513 / −245` net.
- No leftover references to removed symbols (`d11_chroma_key`, `c->ck_*`, `c->chroma_key_color`) outside the moved code.
- Walkthrough of three call paths (`transparent_background=false`, legacy app-supplied key, new DP-picks): all reach the same observable behavior or strictly improved behavior.

## What needs Windows validation

This is the entire D3D11 standalone path. **None of it has been compiled or run on Windows.** clangd on macOS doesn't have D3D11 / Win32 headers so all type errors against `ID3D11Device` etc. are macOS-only noise; the Windows toolchain will resolve them.

### Step 1: Build

```bat
scripts\build_windows.bat all
```

If the build fails, the most likely culprits are (in rough order of probability):

1. **Missing `<d3dcompiler.h>` include in `leia_display_processor_d3d11.cpp`** — the existing file already has it (line 27) so this should be fine, but verify if compile errors cite `D3DCompile` undefined.
2. **`OMGetRenderTargets` / `RSGetViewports` / `__uuidof` semantics** — these are widely supported on the standard MSVC toolchain. If they trip the build, check the `XRT_HAVE_LEIA_SR` gate is on (DP file might be skipped).
3. **`comp_d3d11_target_get_back_buffer()` return type** — if it returns something other than what `static_cast<ID3D11Texture2D *>` accepts, the strip pass needs adjustment. The original code at the old call site already used the same cast, so this should match.

### Step 2: Smoke-test backward compat (Unity v1.2.9 path)

The Unity `displayxr-unity-test-transparent` repo today pins to plugin `v1.2.9` and submits `chromaKeyColor = 0x00817F80` (gray 128,127,129 in 0x00BBGGRR). It should keep working unchanged after this branch lands.

```bat
REM Run the existing test app — should look identical to before
cd \path\to\displayxr-unity-test-transparent
REM Build Unity scene to a Windows player; launch
```

Expected: rotating cube renders over Notepad with click-through, gray-to-transparent edges visible (same fringing as today). No regression.

If the cube **doesn't render at all** or shows magenta instead of gray, the override path (non-zero `chromaKeyColor`) is broken. Check `set_chroma_key` is being called with the right args — add a `U_LOG_W` if needed.

If the runtime **crashes** on first frame:
- Most likely cause: `ck_init_pipeline` failing silently, then `ck_run_pre_weave_fill` / `ck_run_post_weave_strip` proceeding anyway. Both helpers gate on `ck_init_pipeline` returning true, but a partial-init state could deref NULL. Trace via U_LOG_E messages from the init helpers.
- Alternative: the SRV passed to the weaver post-fill is wrong. Verify `weaver_srv` in `leia_dp_d3d11_process_atlas` is `ldp->ck_fill_srv` (not the original atlas) when `ck_should_run()`.

### Step 3: Verify the new flow (manual)

Before PR #5 / #6 (Unity plugin v1.3.0 + test app updates), you can test the new "DP-picks" code path by hand:

1. Modify `cube_handle_d3d11_win` to:
   - Pass `transparentBackgroundEnabled = XR_TRUE`, `chromaKeyColor = 0` via `XrWin32WindowBindingCreateInfoEXT`.
   - Set `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` on the projection layer in `xrEndFrame`.
   - Clear the swapchain to RGBA(0,0,0,0) instead of opaque background each frame.
2. Run.

Expected: cube renders over the desktop with **magenta-tinted hard edges** (because key=0 → DP picks magenta, antialiasing becomes hard-mask). This confirms the new flow lights up.

If you see magenta **everywhere** or no cube: the per-tile pass isn't preserving alpha. Check `BLEND_TEXTURE_SOURCE_ALPHA_BIT` reaches `XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT` in `oxr_session_frame_end.c:125-126`, and that the per-tile pass at `render_gfx.c:407` selects the alpha view.

### Step 4: Capture the atlas (optional, for visual debugging)

Use the screenshot trigger from `CLAUDE.md` to capture the post-DP composited atlas:

```bash
rm -f "/c/Users/SPARKS~1/AppData/Local/Temp/workspace_screenshot.png"
touch "/c/Users/SPARKS~1/AppData/Local/Temp/workspace_screenshot_trigger"
sleep 3
# Open workspace_screenshot.png — alpha=0 regions should show as transparent.
```

## What's left after PR #1

Tracked in tasks (see `TaskList`):

- **PR #2** — Workspace multi-compositor alpha-flag respect. `comp_d3d11_service.cpp:8315` unconditionally `blend_alpha`s tiles; needs to read `XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT` from the layer (already arrives via IPC at `ipc_client_compositor.c:1390`). Workspace OUTPUT stays opaque — document in `docs/specs/workspace-controller-registration.md` and `docs/architecture/compositor-pipeline.md`.
- **PR #3** — Leia D3D12 / GL / Vulkan DP chroma-key. Same pattern as the D3D11 internalization but for the other 3 Leia DPs. D3D12 already has a chroma-key pass at `comp_d3d12_compositor.cpp:1445/1802` to move; GL + Vulkan need new implementations.
- **PR #4** — macOS NSWindow alpha. New `transparentBackgroundEnabled` field on `XR_EXT_cocoa_window_binding` (no chromaKeyColor — Cocoa does true alpha). Configure NSWindow `setOpaque:NO` + clear background + `CAMetalLayer.isOpaque = NO`. sim_display passes alpha through naturally; just declare `is_alpha_native = true` on the 5 sim_display DPs.
- **PR #5** — `displayxr-unity` plugin v1.3.0. Drop `chromaKeyColor`, set `BLEND_TEXTURE_SOURCE_ALPHA_BIT` on submitted layer.
- **PR #6** — `displayxr-unity-test-transparent`. Switch camera clear to RGBA(0,0,0,0), bump plugin pin to v1.3.0.

PR #1 (this branch) unblocks every other piece. PR #5 + #6 can land any time after PR #1.

## Files touched (cheat sheet)

```
src/xrt/include/xrt/xrt_display_processor.h           +68
src/xrt/include/xrt/xrt_display_processor_d3d11.h     +50
src/xrt/include/xrt/xrt_display_processor_d3d12.h     +50
src/xrt/include/xrt/xrt_display_processor_gl.h        +50
src/xrt/include/xrt/xrt_display_processor_metal.h     +50
src/xrt/drivers/leia/leia_display_processor_d3d11.cpp +506
src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp    -245 (chroma-key code moved)
```

## Key invariants to preserve when working on this branch

- **Backward compat with v1.2.9 Unity plugin** — apps that pass non-zero `chromaKeyColor` must keep working unchanged. `set_chroma_key`'s "key != 0 means app override" semantics are load-bearing.
- **DP owns the chroma-key trick** — the compositor must stay vendor-agnostic. Don't sneak chroma-key code back into `comp_d3d11_compositor.cpp` or `comp_d3d11_service.cpp`.
- **Workspace output stays opaque** — workspace clients can have transparent tiles, but the workspace itself can't be transparent. Don't try to plumb chroma-key through the multi-compositor's final output.
- **Antialiased edges become hard mask on Leia hardware** — this is a fundamental lenticular weaver limitation, not a bug. Don't chase soft edges; document the limit.
