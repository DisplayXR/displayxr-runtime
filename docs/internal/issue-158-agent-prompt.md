# Issue #158 Handoff: D3D11 service — stereo view_height ignores rendering_mode view_scale_y

Use this prompt to start a fresh Claude Code session for fixing the compositor per-view stretch bug that Phase 8 capture made visible.

---

## Prompt

```
I'm working on the DisplayXR runtime. Issue #158 is a D3D11 service-compositor bug: in stereo SBS (2×1) render mode the per-view tile is allocated/used at 2× the correct vertical resolution, so each eye's content is rendered stretched 2× vertically. Please implement the fix.

## Context (read in order)

1. `CLAUDE.md` — project overview, build commands, architecture
2. Issue text on GitHub: https://github.com/DisplayXR/displayxr-runtime-pvt/issues/158
3. Phase 8 status doc `docs/roadmap/shell-phase8-status.md` — Phase 8 surfaced the bug; the capture code intentionally dumps the full atlas uncropped for now, and re-adding the crop depends on this fix.
4. `src/xrt/include/xrt/xrt_device.h` lines 234–251 — the `xrt_rendering_mode` struct. Note the vendor-set `view_scale_x/y` and the runtime-computed `view_width_pixels / view_height_pixels / atlas_width_pixels / atlas_height_pixels`.

## Current state on `main`

- Phase 8 MVP shipped on `feature/shell-phase8` (not yet merged). The capture code dumps the full atlas so this bug is visible in captures as vertically-stretched per-eye content.
- `docs/roadmap/shell-phase8-plan.md` and status doc describe what Phase 8 does and why capture is currently uncropped.

## Bug

`src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:859-860`:

    sys->view_width  = sys->display_width  / sys->tile_columns;
    sys->view_height = sys->display_height / sys->tile_rows;

For stereo on a 4K display (`tile_columns=2, tile_rows=1`): view = 3840/2 × 2160/1 = 1920×2160 — effective scale 0.5 × 1.0. For 2×2 quad view this formula happens to produce the right answer (1920×1080) because `tile_rows=2`, which is why nobody noticed.

Similar divide-by-tile computations exist at lines ~1929-1930 and ~5466-5467. Check whether they need the same treatment or whether they're correct as-is.

## Proposed fix

Use the precomputed values on `xrt_rendering_mode`:

    sys->view_width  = sys->xdev->rendering_modes[idx].view_width_pixels;
    sys->view_height = sys->xdev->rendering_modes[idx].view_height_pixels;

Atlas allocation stays at `display_pixel_width × display_pixel_height` (worst-case swapchain sizing — that part is correct). The compositor should render each client into its top-left per-tile rect at `view_w × view_h`, leaving the remainder of the tile (and anything outside `(view_w × tile_cols) × (view_h × tile_rows)`) black. The display processor crop-blits the active region to the weaver.

Driver side: confirm the Leia SR driver populates `view_scale_x/y` and that `u_tiling_compute_mode` (or equivalent) computes `view_width_pixels / view_height_pixels` correctly. Grep the codebase for `view_scale_x`, `view_scale_y`, `view_width_pixels`, `u_tiling_compute_mode`.

## Tasks (suggested split)

1. **Trace**: grep every site in the D3D11 service compositor that computes per-view dims by dividing display dims. Decide which are correct (2×2 quad math) vs bugs (stereo 2×1). Likely candidates already known: lines ~859-860, ~1929-1930, ~5466-5467.
2. **Driver audit**: verify the Leia SR driver (`src/xrt/drivers/leia/`) sets `view_scale_x/y` and that modes are populated so `rendering_modes[idx].view_width_pixels/view_height_pixels` are meaningful at the call sites.
3. **Fix**: replace the divide-by-tile code with reads from `rendering_modes[idx]`. Handle the case where the mode's `view_width_pixels` is 0 (fall back to the old formula, log a warning).
4. **Verify compositor render path respects the smaller tile**: the per-client draw into the atlas must use `view_w × view_h` as the dst rect, not `atlas_w/tile_columns × atlas_h/tile_rows`. Grep for where client swapchains are blit into `combined_atlas` (search for `combined_atlas_rtv`, `ca_rtvs`, `dst_rect`).
5. **Re-enable capture crop** (optional, tied to Phase 8): once the atlas has natural black padding outside the active region, restore the `(view_w × tile_cols) × (view_h × tile_rows)` crop in `comp_d3d11_service_capture_frame` so Phase 8 captures drop the black padding. The previous crop code is in the Phase 8 commits on `feature/shell-phase8`; the revert commit is `0773e5388`.

## Verification

1. Build locally:
   ```
   scripts\build_windows.bat build
   ```
2. Stop any running service/shell and redeploy to `C:\Program Files\DisplayXR\Runtime\` (both `displayxr-service.exe` AND `DisplayXRClient.dll` per `feedback_dll_version_mismatch.md`).
3. Launch the shell with the D3D11 cube app and press **Ctrl+Shift+C** to capture:
   ```
   _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
   ```
4. Inspect `%USERPROFILE%\Pictures\DisplayXR\capture_*_sbs.png`:
   - Atlas should still be 3840×2160 (full swapchain)
   - Each eye tile should show correct 16:9 content at `1920×1080` in the top-left of its tile (top 3840×1080 of the atlas)
   - The lower 3840×1080 strip of the atlas should be black
   - Cubes should have natural aspect, not tall/narrow
5. Check non-captured live display: per-eye content should no longer look horizontally-squashed on the weaver output.
6. Regression-check quad-view (if you have a 4-view mode available) — tile dims should stay 1920×1080 there.

## When to ask the user

- If the driver doesn't populate `view_scale_x/y` for stereo, confirm whether to add the driver change or scope the fix to the compositor only (with a fallback).
- Before modifying the compositor's per-client blit path — that can cascade.
- Before the Phase 8 capture crop re-enable if you're not sure the atlas black-padding convention holds in every mode (2D / stereo / quad-view).

## Branch

Create `feature/issue-158-stereo-view-scale` off `main`. Commits reference `#158`.

## Memory files to rely on

- `feedback_dll_version_mismatch.md` — copy both binaries after IPC or struct changes
- `feedback_test_before_ci.md` — wait for user live-test before pushing
- `feedback_local_vs_ci_builds.md` — prefer local builds
- `reference_runtime_screenshot.md` — file-trigger screenshot (%TEMP%\shell_screenshot_trigger → shell_screenshot_sbs.png) for autonomous self-inspection
```
