# Transparency Support — Follow-Up Handoff (PRs #2, #3, #5, #6)

**Branch:** `feature/transparency-support` (continue here, do not branch off)
**Predecessor:** `docs/roadmap/transparency-support-handoff.md` (PR #1, validated on Windows 2026-05-07)
**Owner of this slice:** Windows + Leia SR machine
**In parallel, on macOS:** PR #4 (NSWindow alpha + `XR_EXT_cocoa_window_binding`). Different files; low conflict risk.

## Prompt to give the Windows agent

Copy-paste the block below into a fresh Claude Code session on the Windows + Leia SR machine. Self-contained — the agent does not see prior conversation context.

```
You are continuing the transparency-support feature on
displayxr-runtime. PR #1 (the foundation — DP vtable + Leia D3D11
chroma-key internalization) was already done and validated on this
machine. You are now picking up PRs #2, #3, #5, #6.

START HERE:
1. cd to the displayxr-runtime checkout.
2. git fetch && git checkout feature/transparency-support && git pull
   (the branch is shared with a macOS session working on PR #4 —
   ALWAYS pull before pushing, NEVER force-push).
3. Read docs/roadmap/transparency-support-followup.md end-to-end.
   That doc lists per-PR scope, file targets, code patterns to follow,
   and test plans. It is the source of truth for this slice.
4. Skim docs/roadmap/transparency-support-handoff.md to refresh the
   PR #1 context — the patterns you'll mirror in PR #3 are documented
   there.
5. Read CLAUDE.md for build/run conventions.

WHAT YOU ARE DOING:
Four PRs, in suggested order:

  PR #2 — Workspace multi-compositor alpha-flag respect.
          comp_d3d11_service.cpp:8315 unconditionally blend_alpha's
          tiles. Read the per-client BLEND_TEXTURE_SOURCE_ALPHA_BIT
          flag (already arrives via IPC) and pick the right blend
          state. Document the workspace-output-opaque limitation.
          Smallest PR; do this first.

  PR #3 — Leia D3D12 / GL / Vulkan DP chroma-key internalization.
          Three sub-pieces (a, b, c). Same pattern as PR #1's
          ea591435c — move the existing chroma-key pass (where it
          exists) into the Leia DP, add the pre-weave fill. Each
          sub-piece is its own commit on the same branch.

  PR #5 — displayxr-unity plugin v1.3.0 (SEPARATE repo:
          /path/to/displayxr-unity, sibling of this checkout).
          Drop default chromaKeyColor=0 instead of gray, set
          BLEND_TEXTURE_SOURCE_ALPHA_BIT on submitted projection
          layer. Bump version, build, tag.

  PR #6 — displayxr-unity-test-transparent (SEPARATE repo, sibling).
          Switch camera clear to RGBA(0,0,0,0), drop the chroma-key
          knob, pin to plugin v1.3.0, document the antialiased-edge
          limitation in README. Build, test on Leia hardware, tag.

After each PR, run the test plan in this doc and report results.
Commit per-PR (or per-sub-PR for #3). Push to the shared branch
between PRs so the macOS session can rebase if needed.

SCOPE BOUNDARIES — DO NOT TOUCH:
- src/external/openxr_includes/openxr/XR_EXT_cocoa_window_binding.h
  (PR #4, owned by macOS session)
- src/xrt/compositor/metal/, src/xrt/compositor/gl/ macOS path,
  src/xrt/compositor/vk_native/ macOS path (PR #4)
- sim_display DP files in src/xrt/drivers/sim_display/ (PR #4 will
  add is_alpha_native=true declarations there; you don't need to)
- The XR_EXT_win32_window_binding.h header — DON'T add new fields.
  The chromaKeyColor=0 means DP-picks contract is set; just consume.

KEY INVARIANTS (same as PR #1, re-state for safety):
- Apps that pass non-zero chromaKeyColor (v1.2.9 Unity flow) must
  keep working unchanged across all 4 graphics APIs. App override
  semantics are load-bearing.
- The DP owns the chroma-key trick. Don't sneak it back into any
  *compositor*.cpp. The compositor's job is to preserve alpha to
  the atlas; the DP decides what to do with it.
- Workspace OUTPUT stays opaque. Workspace clients can have
  transparent tiles, but the workspace itself never punches through
  to the desktop. Documented as a limitation.
- Antialiased edges become hard mask on Leia hardware regardless of
  graphics API — fundamental lenticular weaver limitation.

WHEN YOU'RE DONE WITH ALL FOUR:
Report back with:
  - PR-by-PR commit hashes and what each tested green
  - Any blockers or follow-ups worth filing as issues
  - End-to-end demo result: cube_handle_d3d11_win + Unity test app
    both running with transparent backgrounds against the desktop
  - Workspace shell with two transparent client tiles (PR #2 demo)
```

## Branch coordination

`feature/transparency-support` is shared between Windows (this slice) and macOS (PR #4). Conflict surface is small — different files mostly — but follow the discipline:

- **`git pull --rebase` before every `git push`.** The macOS side might land PR #4 commits between your pushes.
- **Do not force-push.** If a rebase fails, stop and resolve in-place.
- **Push per-PR (or per-sub-PR for #3).** Don't bundle multiple PRs into one push — let the macOS side rebase incrementally.
- **If you touch `src/xrt/state_trackers/oxr/oxr_session.c`** (none of these PRs should, but the Win32 binding parsing lives there), pull first. The macOS side may have just edited the cocoa binding parsing in the same file.

If both sides hit the same file in the same session, prefer letting the macOS side land first (PR #4 is smaller and more contained).

## PR #2 — Workspace multi-compositor alpha-flag respect

### Why

`comp_d3d11_service.cpp:8315` (in `multi_compositor_render`'s tile blit phase, lines 7706–8326) hardcodes `sys->blend_alpha` for every client tile, regardless of whether the client requested alpha-blending via `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`. The flag *does* arrive at the service compositor via IPC (it's part of `xrt_layer_data.flags` copied at `ipc_client_compositor.c:1390` and stored in `ipc_layer_entry`), but the service compositor never reads it.

Effect today: workspace clients that submit RGBA textures get them blended against the workspace background even when they didn't ask for blending — and unpremultiplied/premultiplied distinctions are lost.

### What to do

In `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` around line 8315:

1. Read the layer flag from the layer entry. The flag is on `xrt_layer_data.flags` — search for how `comp_d3d11_compositor.cpp` reads it (the `XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT` constant is in `xrt_compositor.h`). The pattern in `comp_render_helpers.h:31–39` shows how to switch to the alpha-aware image view based on the flag.

2. Switch the blend state per-tile:
   - Flag unset → `sys->blend_opaque` (or use the no-alpha image view, which has the same effect — pick whichever matches the existing code's idiom).
   - Flag set + premultiplied (`XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT` clear) → `sys->blend_premul`.
   - Flag set + unpremultiplied (`XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT` set) → `sys->blend_alpha`.
   The existing `set_blend_state()` function in this same file (around line 1912–1921) already implements this branching for the in-process path; reuse it or copy the pattern.

3. Verify all three blend states already exist on `d3d11_service_system *sys`. If `sys->blend_opaque` doesn't exist, add it (look at how `sys->blend_alpha` is created — it's a one-liner `D3D11_BLEND_DESC` with `BlendEnable = FALSE`).

### Documentation updates

- `docs/specs/workspace-controller-registration.md` — add a section at the end:
  > **Workspace output is opaque.** Workspace apps composite client tiles with per-pixel alpha against the workspace's own background. The workspace's final atlas to the DP is opaque; the workspace itself cannot present a transparent window to the desktop. Standalone apps that want a transparent output window must run outside workspace mode (using the in-process D3D11 compositor + transparent-background extension).

- `docs/architecture/compositor-pipeline.md` — update the alpha lifecycle section to note that per-tile alpha is now respected at the workspace level, but the workspace's combined atlas is presented opaquely.

### Test plan

1. `scripts\build_windows.bat all` — build clean.
2. Modify `cube_handle_d3d11_win` temporarily to set `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` and clear with RGBA(0,0,0,0).
3. Run two instances under shell mode:
   ```
   _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
   ```
4. Expect: both cubes render against the workspace background with their tiles correctly alpha-blended (you should see the workspace background through transparent regions of each cube tile, but the workspace window itself stays opaque against the desktop).
5. Sanity-regress with an opaque cube (don't set the flag): tile renders fully opaque against the workspace background. No regression vs PR #1 baseline.
6. Optional: trigger `%TEMP%\workspace_screenshot_trigger` and inspect the combined SBS atlas — transparent tile regions should show the workspace background pixel, not garbage.

### Estimated size

Single commit, ~30 LOC delta on `comp_d3d11_service.cpp` plus two doc edits.

## PR #3 — Leia D3D12 / GL / Vulkan DP chroma-key internalization

Three sub-pieces. Each follows the **same pattern as PR #1** (commits `924153ef7` + `ea591435c`). Read those commits first to internalize the recipe; then each sub-PR is "do the same thing but for $API".

The pattern, in case you've context-switched:

1. Compositor side (`comp_<api>_compositor.cpp` if the API has an existing chroma-key pass): remove the inline pass, add `xrt_display_processor_<api>_set_chroma_key(c->display_processor, chroma_key_color, transparent_background)` after DP creation.
2. Leia DP side (`leia_display_processor_<api>.cpp`): add `is_alpha_native` (returns false), `set_chroma_key` (stores; key=0 → kDefaultChromaKey 0x00FF00FF magenta), pre-weave fill helper, post-weave strip helper. Wire into `process_atlas`.
3. Both shaders are already designed in `leia_display_processor_d3d11.cpp`:
   - **Pre-fill**: `lerp(chroma_rgb, src.rgb, src.a)` → output alpha=1. Translates straightforwardly to HLSL/GLSL/SPIR-V.
   - **Strip**: equality test on RGB → alpha=0 if match, else alpha=1, RGB premultiplied. Same pattern.

### Sub-PR 3a — D3D12

**Pre-existing chroma-key pass to move:** `src/xrt/compositor/d3d12/comp_d3d12_compositor.cpp:55, 1445, 1802`. Already takes `chroma_key_color` and runs a post-weave pass. Move the body into `src/xrt/drivers/leia/leia_display_processor_d3d12.cpp`.

**Add pre-weave fill** in the Leia D3D12 DP — D3D12 fill needs a `D3D12_BLEND_DESC` with no blending (overwrite) and an intermediate `ID3D12Resource` (RTV + SRV descriptor heap entries).

**Wire** in `comp_d3d12_compositor.cpp` constructor (find where DP factory is called — pattern matches D3D11).

**Output presentation**: D3D12 already configures `DXGI_ALPHA_MODE_PREMULTIPLIED` when `transparent_background` is on (verify by reading the swap-chain creation site in `comp_d3d12_compositor.cpp`). If not, add that — it's required for DWM punch-through.

**Test:** `scripts\build_windows.bat all`, run `cube_handle_d3d12_win` patched to clear RGBA(0,0,0,0) + set the layer flag → expect transparent cube over desktop with magenta-tinted hard edges (DP-picks default).

### Sub-PR 3b — Leia OpenGL

**No pre-existing chroma-key pass** — this is greenfield. Look at the GL compositor and Leia GL DP:
- `src/xrt/compositor/gl/comp_gl_compositor.c` (or .cpp) — find where DP is created and wire `xrt_display_processor_gl_set_chroma_key` after.
- `src/xrt/drivers/leia/leia_display_processor_gl.cpp` — add chroma-key state, GL shader objects (`GL_FRAGMENT_SHADER` for fill + strip, reuse a fullscreen-triangle VS), FBO + texture for fill target, FBO + texture for strip source.

**Output presentation on Windows GL**: WGL doesn't expose `DXGI_ALPHA_MODE_PREMULTIPLIED` directly. Two options:
- A. Render to an off-screen FBO with alpha, then blit to a D3D-interop swap chain via `WGL_NV_DX_interop2` (expensive, complex).
- B. Use a layered window (`WS_EX_LAYERED` + `UpdateLayeredWindow`) with the GL FBO read back to CPU. Slow.
- C. Skip transparent-window support on GL for now; document as a limitation.

**Pragmatic recommendation**: option C for this PR. Most apps that want transparency are D3D11 anyway. Document in `docs/specs/XR_EXT_win32_window_binding.md` that Leia GL transparency is not yet supported. Still implement `set_chroma_key` so the API surface is complete; leave the actual fill/strip TODO with a U_LOG_W warning.

If you disagree and want to go deep on option A/B, do it as a separate sub-sub-PR — don't block the rest of the work.

### Sub-PR 3c — Leia Vulkan

**No pre-existing chroma-key pass** — also greenfield.
- `src/xrt/compositor/vk_native/comp_vk_native_compositor.c` (or wherever the Windows Vulkan DP is wired) — wire `xrt_display_processor_set_chroma_key` (note: the Vulkan variant uses the unsuffixed name from `xrt_display_processor.h`).
- `src/xrt/drivers/leia/leia_display_processor.cpp` (the Vulkan one) — add chroma-key state, SPIR-V shaders for fill + strip, `VkPipeline`s, `VkRenderPass`es, intermediate `VkImage`s with `VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, descriptor sets.

**Output presentation**: `VK_KHR_swapchain` exposes `compositeAlpha` at swap-chain creation. Configure `VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR` (or `VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR` if pre-multiplied isn't supported by the surface — query via `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` and pick the best available). Wire from session-level `transparent_background_enabled`.

**Vulkan SPIR-V**: easiest to build the SPIR-V offline with `glslangValidator` and embed as a header (existing pattern — search for `*.spv.h` or `compile_shaders.sh`). Keep shaders in a sibling file (`leia_chroma_key_fill.frag.glsl`, `leia_chroma_key_strip.frag.glsl`) so they're readable.

**Test**: `scripts\build_windows.bat all`, run `cube_handle_vk_win` patched the same way. Expect the same transparent-with-magenta-fringe result as D3D12.

### Per-sub-PR commit message template

Same format as `ea591435c`. Mention: which file moved (or "greenfield"), what shaders compile to, output presentation alpha mode, backward-compat verification (legacy app-supplied key still works).

### Estimated size

- 3a (D3D12): ~400 LOC moved + ~200 LOC added (similar to D3D11).
- 3b (GL): ~150 LOC stub + a U_LOG_W (or full ~500 LOC if you go deep).
- 3c (Vulkan): ~600 LOC (Vulkan plumbing is verbose). Largest sub-PR.

## PR #5 — displayxr-unity plugin v1.3.0

**Repo:** `displayxr-unity` (sibling of this checkout, separate Git repo). Per memory, lives at `/path/to/displayxr-unity` and consolidates from old `dfattal/unity-3d-display`.

### What to change

In the plugin's `displayxr_hooks.cpp` (the file that injects `XrWin32WindowBindingCreateInfoEXT` into the session create chain — see PR #1's handoff doc Section 2 for context):

1. Default `chromaKeyColor = 0` instead of the gray fallback. The runtime DP will pick its own default (magenta) when key=0.
2. Keep the override path: if a Unity user explicitly sets a non-zero chroma key via the plugin's existing public API, forward it. This lets apps that want a content-safe key (e.g. gray to avoid magenta fringing on dark scenes) keep using one.
3. **Add a hook to set `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`** on the submitted projection layer when transparent background is requested. Find where the plugin populates `XrCompositionLayerProjection` per-frame; OR the bit on `layerFlags` when `state->transparent_background_requested && !displayxr_is_shell_mode()`.
4. Bump version to `1.3.0` in the plugin manifest, package.json, AssemblyInfo.cs, and any `Readme.md` install snippets.

### Backward compat for downstream apps

Apps that pinned to v1.2.9 keep working — they explicitly set `chromaKeyColor`, the runtime honors it, identical behavior. Apps that upgrade to v1.3.0 without changing their code will now use the DP-picks default (magenta) and may see fringing if their scene clears with a near-magenta color. Document in v1.3.0 release notes: "If you see magenta fringing, set a content-safe chromaKeyColor explicitly via [API]".

### Build + tag

Plugin's existing build flow — see its README. Tag `v1.3.0` once landed and tested with the test app (PR #6).

### Estimated size

Small — ~20 LOC across 2-3 files plus version bumps.

## PR #6 — displayxr-unity-test-transparent updates

**Repo:** `displayxr-unity-test-transparent` (sibling, separate). Per the PR #1 handoff doc, currently pinned to plugin v1.2.9 with gray chroma key.

### What to change

In `Assets/TransparentAutoSetup.cs`:

1. Change camera `clearFlags` to `SolidColor` with `Color(0, 0, 0, 0)` — true transparent black. The `m_Camera.backgroundColor = chromaKeyColor` line goes away.
2. Drop the `transparent_chroma_key_color` knob (or default it to 0 = DP-picks).
3. Bump `Packages/manifest.json` dependency on `com.displayxr.unity` to `#upm/v1.3.0`.

### README updates

Add a "Limitations" section:
> On Leia hardware, antialiased cube edges become hard-mask alpha (alpha=0 or alpha=1 with no in-between). This is a fundamental limitation of the chroma-key trick used by the SR weaver — fully transparent regions are punched through cleanly, but partial-transparency pixels on antialiased edges either snap to opaque (with possible fringing toward the magenta default key) or to fully transparent. Apps that need soft alpha should choose a content-safe chroma key via the plugin's override API to minimize fringing.

### Test plan

1. Open the project in Unity Editor on the Windows + Leia machine.
2. Build standalone Windows player.
3. Launch over a Notepad window.
4. Expected behavior:
   - Rotating cube renders over the desktop with transparent regions truly punched through (alpha=0 desktop visible).
   - Cube interior is opaque (alpha=1).
   - Edges have **hard masks** (no soft antialiasing) — magenta fringing may be visible against light desktops; gray fringing if the user opts into a gray override.
   - Click-through still works (cyclopean raycasting from PR #1 unaffected).
5. Compare to v1.2.9 baseline: edges should look **different** (no longer gray fringing — now hard-mask). This is expected and documented.

### Tag

`v1.3.0` of the test repo once green. Update the README's "Tested with plugin v1.X.Y" line.

### Estimated size

Smallest PR — ~10 LOC across 2-3 files plus README.

## End-to-end demo (success criterion for the whole feature)

After PRs #1–#6 land:

1. **Standalone D3D11**: Unity test app v1.3.0 over Notepad → transparent cube, hard-mask edges, click-through. (No regression vs v1.2.9 baseline aesthetically; functionally cleaner because app no longer needs to know about chroma key.)
2. **Standalone D3D12 / Vulkan**: `cube_handle_d3d12_win` / `cube_handle_vk_win` patched temporarily to set the alpha bit + RGBA(0,0,0,0) clear → same transparent result.
3. **Workspace**: shell with two transparent client tiles → tiles composite correctly against opaque workspace background.
4. **macOS Metal** (Mac validates separately): `cube_handle_metal_macos` with alpha=0 clear → desktop visible through transparent regions via NSWindow alpha (true alpha, no chroma key).

If all four work, the feature ships. File any partial-success or skipped paths (e.g. GL transparency deferred per Sub-PR 3b) as follow-up issues.

## Files touched in PR #1 (don't re-touch unless rebasing conflicts)

```
src/xrt/include/xrt/xrt_display_processor*.h          (5 headers)
src/xrt/drivers/leia/leia_display_processor_d3d11.cpp
src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp
docs/roadmap/transparency-support-handoff.md
```

## Files this slice will touch

```
PR #2:  src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp
        docs/specs/workspace-controller-registration.md
        docs/architecture/compositor-pipeline.md
PR #3a: src/xrt/drivers/leia/leia_display_processor_d3d12.cpp
        src/xrt/compositor/d3d12/comp_d3d12_compositor.cpp
PR #3b: src/xrt/drivers/leia/leia_display_processor_gl.cpp
        src/xrt/compositor/gl/comp_gl_compositor.{c,cpp}
PR #3c: src/xrt/drivers/leia/leia_display_processor.cpp  (Vulkan)
        src/xrt/compositor/vk_native/comp_vk_native_compositor.c
PR #5:  displayxr-unity (separate repo)
PR #6:  displayxr-unity-test-transparent (separate repo)
```

## Files PR #4 (macOS, parallel) will touch — leave alone

```
src/external/openxr_includes/openxr/XR_EXT_cocoa_window_binding.h
src/xrt/compositor/metal/*
src/xrt/compositor/gl/*macos*
src/xrt/compositor/vk_native/*macos*  (if any)
src/xrt/drivers/sim_display/sim_display_processor*.{c,m,cpp}
src/xrt/state_trackers/oxr/oxr_session.c   (cocoa binding parsing only)
docs/specs/XR_EXT_cocoa_window_binding.md
```

If you find yourself needing to edit any of these for a Windows-side fix, stop and `git pull` first — the macOS session may have just landed a change.
