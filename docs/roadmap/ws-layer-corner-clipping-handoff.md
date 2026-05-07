# XR_EXT_window_space_layer corner clipping — handoff

Branch: `feature/window-space-in-workspace`
Last commit: `6d2592bf8 fix(workspace): stabilise XR_EXT_window_space_layer rendering and restore shell fps`

## TL;DR

The branch already lands a working window-space layer (HUD) implementation
for the D3D11 service compositor in workspace mode (committed as `6d2592bf8`).
The HUD is stable, renders in both eyes, and the 4-cube shell composite still
hits 60 fps with HUDs active.

**Open**: the HUD is composited onto the COMBINED atlas after chrome, so it
overlays the chrome's rounded / feathered window border. The user wants the
HUD treated as scene content — written into the per-client atlas alongside
the cube projection so the chrome content blit's corner-clip + edge-feather
covers it.

## Current architecture (working baseline)

**File**: `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`

- WS-layer rendering lives in `multi_compositor_render` on the
  capture-render thread, not on the per-client commit thread. Search for
  "Window-space layer pass" to find it.
- It blits the HUD onto the **combined** atlas via per-eye pixel rects
  (`eye_rect_*` mapped to per-tile coords with the chrome content blit's
  formula). Drawn AFTER chrome.
- Per-slot HUD cache (`slot->hud_cache_tex/_srv/_w/_h/_valid`): each
  successful non-blocking `AcquireSync` on the cube's HUD swapchain
  refreshes the cache via `CopyResource`. Blits sample from the cache, so
  acquire failures don't drop a frame.
- Per-client `ws_snapshot[]` populated at the end of
  `compositor_layer_commit` (under `slot->ws_snapshot_mutex`). Multi-comp
  reads only the snapshot — avoids racing the cube's `xrBeginFrame` reset.

Bench (`docs/roadmap/bench/c2-hud-cache-nb-4apps_summary.csv`): per-app
63 fps, shell composite 60.7 fps, multi-comp render 2.8 ms p50. Same as the
HUDless baseline.

## The corner-clipping requirement

User's framing (verbatim):

> "imagine the layer is some small transparent feature like an icon, you
> don't want to round and feather that blindly you want it to be written
> to scene then rounded /feathered at the end like B explains"

So the desired pipeline is:

1. cube projection content + HUD overlay → composited per-client atlas
2. Per-client atlas → combined atlas with corner-clip + edge-feather (the
   existing chrome content blit already does this for cube content alone)

The user does NOT want HUD-self-rounded (that would just make a rounded
HUD badge floating over chrome's rounded window border).

## What was tried

### Approach B (per-client atlas overlay) — broken, reverted

Insert a WS overlay pass BEFORE the chrome content blit's per-view loop in
`multi_compositor_render`, drawing the HUD into the per-client atlas's
tile region. The existing chrome content blit (with corner clipping) then
samples the atlas (now containing cube + HUD) and writes to combined
atlas with rounding/feather applied.

**Variants attempted:**

1. **UNORM RTV onto per-client atlas** — symptom: "all apps dark". Likely
   gamma mismatch: cube swapchain is sRGB → projection bytes are
   sRGB-encoded → atlas holds sRGB bytes. My UNORM-RTV shader output was
   linear → atlas became mixed sRGB/linear. Chrome's SRGB SRV
   re-linearised everything, destroying the cube content too.

2. **SRGB RTV onto per-client atlas** (added `atlas_rtv_srgb` field on
   `client_render_resources`) — symptom: "only rendering right eye, left
   eye entirely black". Asymmetric per-eye failure suggests the bug is
   not purely color-space.

Both variants are off the branch (reverted via `git checkout --` before
push). The `atlas_rtv_srgb` field/init code is also gone from the
committed state — re-add if pursuing this path.

### Why approach B is hard

D3D11 immediate context concurrency: `compositor_layer_commit` (cube IPC
thread) writes the per-client atlas via `CopySubresourceRegion` for cube
projection, no `render_mutex`. Capture thread is what runs
`multi_compositor_render`, which holds `render_mutex`. Adding a
capture-thread WRITE to the per-client atlas touches the same texture as
the commit-thread WRITE — both via `sys->context` (which is documented
not thread-safe). The pre-existing pattern tolerates concurrent reads
but adding a second writer may have surfaced state issues we haven't
fully traced.

Likely-but-unproven failure modes the next agent should rule in or out:

- **Resource hazard between the per-client atlas's RTV (write) and SRV
  (read)** when chrome content blit's slot_srv samples the same atlas
  immediately after our WS overlay's draws.
- **D3D11 multithread protection (already on per `D3D11 multithread
  protection enabled`)** is supposed to serialize, but may produce
  warnings or silently drop work.
- **TYPELESS storage + parallel UNORM/SRGB RTV/SRV views** — one
  hypothesis: blending with SRGB RTV reads the destination using sRGB
  decode, which interacts with cube's already-sRGB-encoded bytes
  differently than expected.
- **Per-eye asymmetry** — the second variant's left-eye-only failure is
  the strongest clue. View 0 is processed first; view 1 second. Some
  state established for view 0's draw might leak to chrome content
  blit's view-0 sample but not its view-1 sample.

## Concrete debugging plan for the next agent

(Each step keeps the WS overlay running, the runtime still launches —
just diagnostic instrumentation. Use the atlas screenshot (see Tools
below) between launches so you can SEE the per-client and combined
atlas state without trusting the live SR display.)

1. **Reproduce in isolation, single cube**.
   ```
   _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
   ```
   Trigger an atlas screenshot. The combined-atlas SBS image will show
   exactly which tile is broken.

2. **Bisect the failure surface — modify the WS overlay block, rebuild,
   atlas-grab between each variant:**
   - **Empty pass**: bind RTV + viewport, NO draws. If broken behaviour
     reproduces → the RTV binding alone breaks downstream chrome blit
     (likely RTV/SRV hazard).
   - **Hardcoded full-atlas scissor**: rule out scissor leak.
   - **No corner-radius / no quad_mode flags**: rule out shader-path
     divergence.
   - **Single view (col 0 only) draws**: rule out per-view loop bug.
   - **Same SRV format for both write and read**: try UNORM-write to
     UNORM-read (set `cc->atlas_holds_srgb_bytes = false` for cube's
     atlas — would degrade cube color accuracy but isolates the gamma
     dimension from the hazard dimension).

3. **Enable D3D11 debug layer** — in
   `comp_d3d11_service_create_system` find the `D3D11CreateDevice` call
   and OR in `D3D11_CREATE_DEVICE_DEBUG`. Run a single-cube launch with
   HUD on, watch the service log for runtime warnings. Likely candidates:
   - `Resource being set to OM RenderTarget slot 0 is still bound on
     input!`
   - `Resource hazard: read after write...`

4. **Step back to the architectural alternative if (1)-(3) don't reveal
   a clean fix**:

   **Approach D** — modify the chrome content-blit shader to also sample
   the HUD cache and composite both in a single draw. This is the
   cleanest design because corner-clip / edge-feather is applied ONCE
   over the composited output (cube + HUD), with no intermediate
   per-client atlas write to debug. Cost: shader edit (HLSL in
   `d3d11_service_shaders.h`), new uniform fields (HUD UV rect, HUD pixel
   rect on combined atlas, hud_present flag), plus binding the cache SRV
   in slot 1 alongside the cube atlas SRV in slot 0 during the chrome
   content blit's per-view loop.

   This deliberately bypasses the per-client-atlas write path. No WS
   overlay pass at all in multi-comp; the HUD is composited inside
   chrome's existing per-eye sample to combined atlas.

## Tools

### Atlas screenshot (quick visual debugging)

The D3D11 service compositor supports file-triggered screenshots of the
COMBINED atlas (post-weave back buffer). From CLAUDE.md:

```bash
rm -f "/c/Users/SPARKS~1/AppData/Local/Temp/workspace_screenshot.png"
touch "/c/Users/SPARKS~1/AppData/Local/Temp/workspace_screenshot_trigger"
sleep 3
# Read C:\Users\SPARKS~1\AppData\Local\Temp\workspace_screenshot.png
```

Code location: `comp_d3d11_service.cpp`, in `multi_compositor_render()`,
just before `swap_chain->Present()`. Result is the 3840×2160 SBS atlas
the DP weaves; left half is left eye, right half is right eye. An "empty
window" / dark tile is unmistakable.

### FPS benchmark

```powershell
.\scripts\bench_shell_present.ps1 -Tag <your-tag> -Seconds 60 -App `
  "test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe", `
  "test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe", `
  "test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe", `
  "test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe"
```

Compares against the existing baselines in `docs/roadmap/bench/`:
- `c2-no-hud-4apps`: HUD off — per-app 62 fps, shell 60.7 fps, render 2.8 ms
- `c2-hud-cache-nb-4apps`: HUD on, current baseline — per-app 63 fps,
  shell 60.7 fps, render 2.8 ms

### Build / push commands

```bash
scripts/build_windows.bat build       # runtime only (~30s)
scripts/build_windows.bat test-apps   # cube apps (rebuild after HUD toggles)
cp _package/bin/displayxr-service.exe "/c/Program Files/DisplayXR/Runtime/displayxr-service.exe"
cp _package/bin/DisplayXRClient.dll "/c/Program Files/DisplayXR/Runtime/DisplayXRClient.dll"
```

The runtime registry points at `C:\Program Files\DisplayXR\Runtime\`. The
shell launches the service from there. Each rebuild requires the copy.

### Launching shell with cubes

```bash
cmd.exe //c "_package\\bin\\displayxr-shell.exe test_apps\\cube_handle_d3d11_win\\build\\cube_handle_d3d11_win.exe ..."
```

Multiple instances of the cube path = multiple cubes. The shell
auto-launches the service on first run; subsequent runs reuse it (kill
the service before `cp`-ing a new build over it).

## Success criteria

1. **Cube content unchanged** — visible in both eyes, no flicker, no
   asymmetric darkening. Atlas screenshot shows clean cube projections in
   both tiles.
2. **HUD subject to corner clipping and edge feathering** — the HUD
   pixels at window corners are rounded along with the cube content.
   Atlas screenshot at the corner of a HUD-overlapped region should
   show the same alpha falloff curve as the cube content has at the
   window's corner.
3. **No FPS regression** — bench against
   `docs/roadmap/bench/c2-hud-cache-nb-4apps_summary.csv`. Per-app ≥ 60
   fps, shell composite ≥ 60 fps, multi-comp render ≤ 3.5 ms.
4. **HUD stable** (no flicker) — same as the current baseline. The cache
   mechanism should still shield from `AcquireSync` failures; if you
   refactor it, preserve that property.

## Project memories worth re-reading

- `feedback_atlas_stride_invariant`: per-client atlas slot stride =
  `atlas_w / tile_columns`, NOT `sys->view_width`. Diverges in workspace
  mode. Already correctly applied in the committed code.
- `feedback_acquiresync_load_bearing`: AcquireSync is the cache barrier
  for SHARED_KEYEDMUTEX cross-process textures; `Wait(fence)` alone is
  NOT enough. Currently we use a non-blocking `AcquireSync(0, 0)` paired
  with a runtime cache, which gives correctness when it succeeds and
  graceful degradation (stale cache) when it doesn't.
- `feedback_workspace_must_equal_desktop_fps`: workspace fps lever is
  app-side. Don't sacrifice shell composite fps for HUD features.

## Files in scope

- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — primary
- `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` — touch only
  if pursuing approach D (combined chrome+HUD shader)

## Out of scope

- Standalone (non-workspace) mode HUD rendering — deliberately not
  wired up; the chrome-style combined-atlas pattern is the model if
  needed in future.
- macOS / Vulkan / OpenGL compositors — different files, different
  strategies; this branch is D3D11-service-only.
