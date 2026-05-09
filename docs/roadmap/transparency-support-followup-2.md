# Transparency Support — Follow-Up #2 Handoff (PRs #3b, #3c, #5, #6)

**Predecessor:** PR #213 on `DisplayXR/displayxr-runtime` (`feature/transparency-support` → `main`).
That PR shipped #1, #2, #3a, #4. This handoff picks up the remaining slice.

**Branch model:** start a fresh `feature/transparency-support-2` branch off updated `main`.
Don't continue on the old `feature/transparency-support` — it has been merged and deleted.

## Prompt to give the next agent

Copy-paste the block below into a fresh Claude Code session on the Windows + Leia SR
machine. Self-contained — the agent does not see prior conversation context.

```
You are picking up the remaining slice of the transparency-support
feature on displayxr-runtime. PRs #1, #2, #3a, #4 already shipped via
PR #213 (merged to main). You are now picking up PRs #3b, #3c, #5, #6.

START HERE:
1. cd to the displayxr-runtime checkout.
2. git fetch && git checkout main && git pull
3. git checkout -b feature/transparency-support-2
4. Read docs/roadmap/transparency-support-followup-2.md end-to-end.
   That doc is the source of truth.
5. Skim docs/specs/XR_EXT_win32_window_binding.md and the
   "Transparent-window contract" section — the WS_EX_NOREDIRECTIONBITMAP
   requirement applies to every test-app patch you'll write.
6. Read CLAUDE.md for build/run conventions.

WHAT YOU ARE DOING:
Four PRs, in suggested order. PRs #5 and #6 require GitHub CLI access
to the unity repos.

  PR #3b — Leia OpenGL chroma-key (STUB ONLY, agreed scope).
           Wire is_alpha_native=false + set_chroma_key (stores) into
           the Leia GL DP. The actual fill/strip is a TODO with a
           one-time U_LOG_W. Document GL transparency as "not yet
           supported" in XR_EXT_win32_window_binding.md. Smallest PR.

  PR #3c — Leia Vulkan chroma-key (FULL IMPLEMENTATION).
           ~600 LOC. Mirror PR #3a's structure. SPIR-V shaders
           offline-built with glslangValidator, embedded as .spv.h
           headers. Configure DXGI / VK_KHR_swapchain compositeAlpha
           = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR. See
           docs/roadmap/transparency-support-followup-2.md §3c for
           the design.

  PR #5  — displayxr-unity plugin v1.3.0.
           Repo: /path/to/unity-3d-display (sibling of this
           checkout). Drop default chromaKeyColor=0 instead of gray;
           set BLEND_TEXTURE_SOURCE_ALPHA_BIT on submitted projection
           layer; bump version; tag v1.3.0.

  PR #6  — displayxr-unity-test-transparent v1.3.0.
           Repo: /path/to/displayxr-unity-test-transparent. Switch
           camera clear to RGBA(0,0,0,0); drop chroma-key knob; pin
           plugin to v1.3.0; document antialiased-edge limitation;
           build standalone Windows player; test on Leia hardware;
           tag.

KEY INVARIANTS (non-negotiable):
- The DP owns the chroma-key trick. Compositor stays vendor-agnostic.
- Workspace OUTPUT stays opaque — only per-tile alpha is respected.
- Apps that pass non-zero chromaKeyColor (legacy v1.2.9 Unity flow)
  must keep working unchanged.
- Transparent-window apps must create their HWND with
  WS_EX_NOREDIRECTIONBITMAP + null background brush. Not optional.
  Documented in docs/specs/XR_EXT_win32_window_binding.md.
- Antialiased edges become hard mask on Leia hardware regardless of
  graphics API — fundamental lenticular-weaver limitation.
- Always copy rebuilt _package/bin/DisplayXRClient.dll AND
  displayxr-service.exe to "C:\Program Files\DisplayXR\Runtime\"
  after build, otherwise apps load the registry-resolved stale DLL.

WHEN YOU'RE DONE:
Report back per-PR commit hashes and what tested green.
End-to-end demo result: cube_handle_vk_win patched + Unity test app
both running with transparent backgrounds against the desktop.
```

## Architecture recap (what shipped in PR #213)

```
App (any graphics API)
        |
   OpenXR State Tracker
   - parses XR_EXT_win32_window_binding.transparentBackgroundEnabled (v4)
   - parses XR_EXT_win32_window_binding.chromaKeyColor (v5)
   - parses XR_EXT_cocoa_window_binding.transparentBackgroundEnabled (v5)
        |
   Native compositor
   - configures swap chain alpha mode for transparency (DXGI_ALPHA_MODE_PREMULTIPLIED
     on D3D11/D3D12, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR on Vulkan,
     CAMetalLayer.isOpaque=NO on Metal)
   - for D3D12: uses CreateSwapChainForComposition + IDCompositionTarget for DComp
   - calls xrt_display_processor_*_set_chroma_key(dp, color, transparent_bg)
        |
   Display Processor (vendor-specific)
   - Leia D3D11, D3D12: is_alpha_native=false; chroma-key fill+strip
                        wraps the SR weaver (PR #1 + #3a, shipped)
   - Leia GL, Vulkan:   is_alpha_native=false; PR #3b (stub) + #3c (full) ← this slice
   - sim_display all 5: is_alpha_native=true; set_chroma_key=no-op (PR #4, shipped)
        |
   Display
```

## PR #3b — Leia OpenGL chroma-key (FULL)

### Status: SHIPPED

The original handoff doc proposed a stub-only GL implementation, deferring real
transparency. **The actual PR went FULL** (Option A: `WGL_NV_DX_interop2` +
DComp present path). Implemented:

- Leia GL DP fill+strip chroma-key around the SR GL weaver (GLSL programs
  compiled at runtime, fullscreen-triangle, `GL_NEAREST` sampler for the
  strip's exact-equality test).
- DComp + `CreateSwapChainForComposition` + per-back-buffer
  `wglDXRegisterObjectNV` interop bridge in `comp_gl_compositor.cpp` (Windows
  only). GL renders into per-back-buffer FBOs that wrap D3D11 textures.
- `comp_gl_compositor.c` was renamed to `.cpp` so `dcomp.h` (C++-only) can be
  included directly.
- Graceful fallback to opaque `SwapBuffers` with a one-time warning when
  `WGL_NV_DX_interop2` is unavailable (Intel iGPUs).
- `cube_handle_gl_win` patched with `DISPLAYXR_TRANSPARENT_BG=1` opt-in
  matching the cube_handle_vk_win pattern.

### Files

```
src/xrt/drivers/leia/leia_display_processor_gl.cpp   (~50 LOC: vtable wire + stubs)
src/xrt/compositor/gl/comp_gl_compositor.c           (~5 LOC: set_chroma_key call after factory)
docs/specs/XR_EXT_win32_window_binding.md            (note GL transparency unsupported)
```

### Implementation

In `leia_display_processor_gl.cpp`:

```cpp
static bool
leia_dp_gl_is_alpha_native(struct xrt_display_processor_gl *xdp)
{
    (void)xdp;
    return false; // SR GL weaver destroys alpha, same as D3D11/D3D12.
}

static void
leia_dp_gl_set_chroma_key(struct xrt_display_processor_gl *xdp,
                          uint32_t key_color,
                          bool transparent_bg_enabled)
{
    struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
    ldp->ck_enabled = transparent_bg_enabled;
    ldp->ck_color = (key_color != 0) ? key_color : 0x00FF00FF;

    static bool warned_once = false;
    if (transparent_bg_enabled && !warned_once) {
        U_LOG_W("Leia GL DP: transparent_background requested but GL chroma-key "
                "fill+strip not yet implemented; output will not be transparent");
        warned_once = true;
    }
}
```

Wire in factory's vtable assignment block (mirror D3D11/D3D12).

In `comp_gl_compositor.c` after the DP factory succeeds (around line 2308):

```c
xrt_display_processor_gl_set_chroma_key(c->display_processor,
                                         chroma_key_color, transparent_background);
```

You'll need to plumb `chroma_key_color` and `transparent_background` through the
compositor constructor — search how `comp_d3d12_compositor.cpp` does it for the
exact pattern.

### Test plan

1. `scripts\build_windows.bat all` — clean build.
2. Run `cube_handle_gl_win` with stock flags — same as baseline (no regression).
3. Patch `cube_handle_gl_win` like cube_handle_d3d12_win (transparent_background
   + RGBA(0,0,0,0) clear + BLEND_TEXTURE_SOURCE_ALPHA_BIT + WS_EX_NOREDIRECTIONBITMAP).
4. Run — cube renders normally (NOT transparent), and the log contains the
   one-time `Leia GL DP: ... not yet implemented` warning.

### Estimated size

~50 LOC. Single commit.

## PR #3c — Leia Vulkan chroma-key FULL

### Scope

Full implementation. Mirror PR #3a's D3D12 structure exactly:
- `is_alpha_native` returns false
- `set_chroma_key` stores enabled + color (default magenta on key=0)
- Pre-weave fill: VkPipeline + VkImage (color attachment + sampled), VkRenderPass,
  shaders compiled offline to .spv.h headers
- Post-weave strip: same pattern as fill but reads back-buffer
- Wire from `comp_vk_native_compositor.c` after the DP factory succeeds
- Configure `compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR` in
  `vkCreateSwapchainKHR` (currently hardcoded to `VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR`)

### Files

```
src/xrt/drivers/leia/leia_display_processor.cpp                   (~600 LOC: ck state + helpers + vtable)
src/xrt/drivers/leia/leia_chroma_key_fill.frag.glsl               (NEW)
src/xrt/drivers/leia/leia_chroma_key_strip.frag.glsl              (NEW)
src/xrt/drivers/leia/leia_chroma_key_fullscreen.vert.glsl         (NEW)
src/xrt/drivers/leia/leia_chroma_key_shaders.h                    (NEW: .spv.h embed)
src/xrt/drivers/leia/CMakeLists.txt                               (compile_shaders rule)
src/xrt/compositor/vk_native/comp_vk_native_compositor.c          (~10 LOC: set_chroma_key call)
src/xrt/compositor/vk_native/comp_vk_native_target.c              (~30 LOC: query + select PRE_MULTIPLIED compositeAlpha)
```

### SPIR-V build pipeline

The codebase doesn't have an existing SPIR-V build pipeline. Add one:

```cmake
# src/xrt/drivers/leia/CMakeLists.txt
find_program(GLSLANG_VALIDATOR glslangValidator)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found (Vulkan SDK)")
endif()

function(compile_glsl_to_spv_header GLSL_FILE STAGE VAR_NAME OUT_HEADER)
    add_custom_command(
        OUTPUT ${OUT_HEADER}
        COMMAND ${GLSLANG_VALIDATOR} -V -S ${STAGE}
                --vn ${VAR_NAME}
                -o ${OUT_HEADER}
                ${CMAKE_CURRENT_SOURCE_DIR}/${GLSL_FILE}
        DEPENDS ${GLSL_FILE}
    )
endfunction()

compile_glsl_to_spv_header(leia_chroma_key_fullscreen.vert.glsl vert ck_vs_spv leia_chroma_key_vs.spv.h)
compile_glsl_to_spv_header(leia_chroma_key_fill.frag.glsl       frag ck_fill_ps_spv leia_chroma_key_fill.spv.h)
compile_glsl_to_spv_header(leia_chroma_key_strip.frag.glsl      frag ck_strip_ps_spv leia_chroma_key_strip.spv.h)
```

Outputs `unsigned int ck_vs_spv[] = { ... };` etc. — `#include` from
`leia_display_processor.cpp` and pass to `vkCreateShaderModule`.

### Shaders (translate from D3D12 HLSL)

```glsl
// leia_chroma_key_fullscreen.vert.glsl
#version 450
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * vec2(2,-2) + vec2(-1,1), 0, 1);
}
```

```glsl
// leia_chroma_key_fill.frag.glsl
#version 450
layout(set=0, binding=0) uniform sampler2D src;
layout(push_constant) uniform PC { vec3 chroma_rgb; float pad; } pc;
layout(location=0) out vec4 frag;
void main() {
    vec2 uv = vec2(gl_FragCoord.x / textureSize(src, 0).x,
                   gl_FragCoord.y / textureSize(src, 0).y);
    vec4 c = texture(src, uv);
    frag = vec4(mix(pc.chroma_rgb, c.rgb, c.a), 1.0);
}
```

```glsl
// leia_chroma_key_strip.frag.glsl  — same algorithm as D3D11/D3D12
#version 450
layout(set=0, binding=0) uniform sampler2D src;
layout(push_constant) uniform PC { vec3 chroma_rgb; float pad; } pc;
layout(location=0) out vec4 frag;
void main() {
    vec2 uv = vec2(gl_FragCoord.x / textureSize(src, 0).x,
                   gl_FragCoord.y / textureSize(src, 0).y);
    vec3 c = texture(src, uv).rgb;
    vec3 d = abs(c - pc.chroma_rgb);
    bool match = max(max(d.r, d.g), d.b) < (1.0/512.0);
    float a = match ? 0.0 : 1.0;
    frag = vec4(c * a, a);
}
```

### compositeAlpha selection

In `comp_vk_native_target.c:180`, replace `VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR`
with a runtime selection:

```c
VkSurfaceCapabilitiesKHR caps;
vk->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical_device, target->surface, &caps);

VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
if (transparent_background) {
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
        composite_alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    } else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        composite_alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else {
        U_LOG_W("VK transparent_background requested but no PRE_MULTIPLIED/INHERIT compositeAlpha — falling back to opaque");
    }
}
ci.compositeAlpha = composite_alpha;
```

`transparent_background` plumbing: thread it through `comp_vk_native_compositor_create`
the same way `chroma_key_color` already is on D3D12, then forward to
`comp_vk_native_target_create`.

### Test plan

1. `scripts\build_windows.bat all` — must build (glslangValidator should be on PATH from Vulkan SDK).
2. Backward compat: stock `cube_handle_vk_win` — identical to baseline.
3. New flow: patch `cube_handle_vk_win` (same as cube_handle_d3d12_win patches) +
   `WS_EX_NOREDIRECTIONBITMAP`. Expect: cube over desktop, magenta-tinted hard
   edges, click-through.

### Estimated size

~600 LOC + 4 new files. Single commit (or split into "shaders + Vulkan plumbing"
and "wire into compositor" if desired).

## PR #5 — `displayxr-unity` plugin v1.3.0

**Repo:** `/path/to/unity-3d-display` (sibling of the runtime checkout, NOT
inside the runtime tree).

### Changes

In `native~/displayxr_hooks.cpp::win32_inject_window_binding`:

1. Default `chromaKeyColor = 0` instead of the legacy gray default. The runtime
   DP now picks magenta when key=0.
2. Keep the user-overridable path: `state->transparent_chroma_key_color` flows
   through unchanged.

In the C# layer (`Runtime/DisplayXRTransparentOverlay.cs`):

3. Add `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` on the projection
   layer when `transparent_background_requested && !displayxr_is_shell_mode()`.
   (The plugin currently injects this via the native hook chain — find where
   it sets `XrCompositionLayerProjection.layerFlags` per-frame.)

4. Bump version to `1.3.0` in:
   - `package.json`
   - `CHANGELOG.md`
   - any `Runtime/AssemblyInfo.cs` (if present)
   - README install snippets

### Backward compat

Apps pinned to `v1.2.9` keep working (they explicitly set chromaKeyColor; runtime
honors the override). Apps that upgrade to `v1.3.0` without changing their code
get the DP-picks default (magenta) and may see fringing on dark scenes — document
in v1.3.0 release notes: "If you see magenta fringing, set a content-safe
chromaKeyColor explicitly via [API]".

### Build + tag

Plugin's existing build: `cmd.exe //c "$(pwd -W)\\native~\\build-win.bat"` (per
memory `reference_plugin_build_quirks.md` — needs absolute path because of the
trailing `~` in `native~`). Copy DLL to BOTH `Test-D3D11/` and
`DisplayXR-test/Build/`.

Tag `v1.3.0` once the test app (PR #6) confirms green.

### Estimated size

~20 LOC + version bumps.

## PR #6 — `displayxr-unity-test-transparent` v1.3.0

**Repo:** `/path/to/displayxr-unity-test-transparent` (sibling).

### Changes

In `Assets/TransparentAutoSetup.cs`:

1. Change camera `clearFlags` to `SolidColor` with `Color(0, 0, 0, 0)` — true
   transparent black. The current `m_Camera.backgroundColor = chromaKeyColor`
   line goes away.
2. Drop the `transparent_chroma_key_color` knob (or default to 0 = DP picks).

In `Packages/manifest.json`:

3. Bump dependency on `com.displayxr.unity` to `#upm/v1.3.0`.

### README updates

Add a "Limitations" section:

> On Leia hardware, antialiased cube edges become hard-mask alpha (alpha=0 or
> alpha=1 with no in-between). This is a fundamental limitation of the chroma-key
> trick used by the SR weaver — fully transparent regions are punched through
> cleanly, but partial-transparency pixels on antialiased edges either snap to
> opaque (with possible fringing toward the magenta default key) or to fully
> transparent. Apps that need soft alpha should choose a content-safe chroma key
> via the plugin's override API to minimize fringing.

### Test plan

1. Open the project in Unity Editor on the Windows + Leia machine.
2. Build standalone Windows player (`Unity.exe -batchmode -quit -nographics
   -projectPath ... -executeMethod BuildScript.BuildWindows64 -logFile ...`
   per memory `reference_unity_batchmode_build.md`).
3. Launch over Notepad.
4. Expected:
   - Rotating cube renders over the desktop, transparent regions truly punched
     through (alpha=0 → desktop visible).
   - Cube interior opaque (alpha=1).
   - Hard-mask edges with magenta fringing on light desktops; gray fringing if
     the user opts into a gray override.
   - Click-through still works.
5. Compare to v1.2.9 baseline: edges should look DIFFERENT (no longer gray
   fringing — now hard-mask). This is expected.

### Tag

`v1.3.0` of the test repo once green. Update README's "Tested with plugin v1.X.Y"
line.

### Estimated size

~10 LOC + README + version bumps.

## End-to-end demo (success criterion for the whole feature)

After PRs #3b, #3c, #5, #6 land:

1. **Standalone D3D11**: Unity test app v1.3.0 over Notepad → transparent cube,
   hard-mask edges, click-through. Cleaner than v1.2.9 (app no longer needs to
   know about chroma key).
2. **Standalone D3D12**: `cube_handle_d3d12_win` patched (or any third-party
   D3D12 app) → same.
3. **Standalone Vulkan**: `cube_handle_vk_win` patched (this PR) → same.
4. **Standalone GL**: stock cube renders normally; transparency NOT supported
   (documented).
5. **Workspace**: shell with two transparent client tiles → tiles composite
   correctly against opaque workspace background.
6. **macOS Metal** (already shipping): `cube_handle_metal_macos` with
   `DISPLAYXR_TRANSPARENT_BG=1` → desktop visible through transparent regions
   via NSWindow alpha (true alpha, no chroma key).

## Lessons learned (read these before implementation)

### 1. Test-app HWND must use `WS_EX_NOREDIRECTIONBITMAP` + null bg brush

Already documented in `docs/specs/XR_EXT_win32_window_binding.md`. Without these
two flags on the bound HWND, DComp's `alpha = 0` swap-chain pixels composite
over the HWND's redirection surface (default-cleared opaque) instead of the
desktop. Apps see opaque black where they expected transparency. The runtime
cannot work around this — it has to be on the app side.

The `cube_handle_*_win` test apps in this repo currently DO NOT set these flags.
You'll need to patch them (or add a `DISPLAYXR_TRANSPARENT_BG=1` opt-in like the
macOS test apps).

### 2. Multi-comp races with client xrBeginFrame — use snapshots

PR #2 found that reading `cc->layer_accum` directly from the multi-compositor
races with the client's `xrBeginFrame` reset (~99% of multi-comp ticks see
`layer_count == 0`). The fix is to snapshot per-client state under
`ws_snapshot_mutex` in `compositor_layer_commit`, alongside the existing
WS-layer HUD snapshot.

If you add new per-tile state to the workspace blit in PR #3b/#3c (unlikely
since GL/Vulkan don't go through the workspace today), use the same snapshot
pattern.

### 3. D3D12 has no command-list `Get*` — pass RTV handles in

PR #3a found that the pre-weave fill rebound the RTV to `ck_fill_tex` and the
weaver then ran with the wrong target. D3D11's `OMGetRenderTargets` saves and
restores the RTV; D3D12 has no equivalent. Pass the back-buffer RTV in as a
parameter and explicitly restore.

This will apply to PR #3c (Vulkan) too: there's no implicit "save/restore RT
binding". Track explicitly.

### 4. Always copy rebuilt binaries to Program Files

After every `scripts\build_windows.bat build`, copy
`_package\bin\DisplayXRClient.dll` AND `_package\bin\displayxr-service.exe` to
`C:\Program Files\DisplayXR\Runtime\`. Apps load the registry-resolved DLL from
there, NOT the dev-manifest-pointed one — that warning shows up in the cube's
log under elevated context. We lost an hour debugging stale DLL once.

### 5. PrintWindow lies for DComp content; use BitBlt from desktop DC

For autonomous validation of transparent windows, PrintWindow returns opaque
black even with `PW_RENDERFULLCONTENT`. BitBlt from the desktop window DC
captures the actual on-screen pixels. See memory entry
`reference_dcomp_screenshot.md`.

### 6. SR weaver's `setViewport` is phase-calc only

Per memory `feedback_sr_sdk_d3d12_weave_viewport.md`: the SR D3D12 weaver's
`setViewport`/`setScissorRect` APIs are used internally for phase calculation
only — they don't call `RSSetViewports`/`RSSetScissorRects` on the cmd list.
You MUST set both on the cmd list yourself before `weave()`. Same gotcha
applies if you take similar shortcuts in the Vulkan path.

## Files NOT to touch

```
src/xrt/include/xrt/xrt_display_processor*.h           (frozen by PR #1)
src/xrt/drivers/leia/leia_display_processor_d3d11.cpp  (PR #1, shipped)
src/xrt/drivers/leia/leia_display_processor_d3d12.cpp  (PR #3a, shipped)
src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp     (PR #1, shipped)
src/xrt/compositor/d3d12/comp_d3d12_compositor.cpp     (PR #3a, shipped)
src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp (PR #2, shipped)
src/xrt/compositor/metal/                              (PR #4, shipped)
src/xrt/drivers/sim_display/                           (PR #4, shipped)
src/external/openxr_includes/openxr/XR_EXT_*window_binding.h  (frozen at v5)
docs/specs/XR_EXT_win32_window_binding.md              (frozen at SPEC v5)
docs/specs/XR_EXT_cocoa_window_binding.md              (frozen at SPEC v5)
```

## Files this slice will touch

```
PR #3b: src/xrt/drivers/leia/leia_display_processor_gl.cpp
        src/xrt/compositor/gl/comp_gl_compositor.c
        docs/specs/XR_EXT_win32_window_binding.md  (just a note about GL TODO)

PR #3c: src/xrt/drivers/leia/leia_display_processor.cpp        (Vulkan)
        src/xrt/drivers/leia/leia_chroma_key_*.glsl            (NEW, 3 files)
        src/xrt/drivers/leia/leia_chroma_key_*.spv.h           (NEW, generated, 3 files)
        src/xrt/drivers/leia/CMakeLists.txt                    (compile_shaders rule)
        src/xrt/compositor/vk_native/comp_vk_native_compositor.c
        src/xrt/compositor/vk_native/comp_vk_native_target.c

PR #5:  unity-3d-display repo (separate)
PR #6:  displayxr-unity-test-transparent repo (separate)
```
