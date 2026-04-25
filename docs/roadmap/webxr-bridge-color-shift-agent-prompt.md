# Bridge-Aware WebXR Color Shift in Shell — Agent Prompt

Paste this into a fresh Claude Code session to start the task.

---

## Prompt

```
Read docs/roadmap/webxr-bridge-color-shift-plan.md and execute the
Staging section (1 → 2 → 3 → 4), committing per logical change.

Parent plans:
  docs/roadmap/webxr-in-shell-plan.md
  docs/roadmap/webxr-in-shell-stage2-plan.md (§"Bridge-aware page
    color shift in shell mode" — root analysis lives here)

Commit per fix. Don't push. Don't run /ci-monitor.

Work on branch feature/webxr-in-shell — qwerty-freeze stack (5 commits)
already pushed and live at origin. Branch is green locally and on
origin.

Goal: bridge-aware WebXR samples (e.g. webxr-bridge/sample/) render
correct colors when hosted in the spatial shell. A deep navy
0x0d0d40 background currently displays as a brightened indigo —
consistent with one extra gamma transform somewhere in the shell-mode
multi-comp pipeline. Pre-existing bug, not introduced by recent work;
isolated to shell + bridge-aware combo.
```

---

## Context to load before starting

1. **`docs/roadmap/webxr-bridge-color-shift-plan.md`** — this task's
   detailed plan with diagnostic strategy, fix-path candidates, and
   verification matrix. Reads in 5 minutes.
2. **`docs/roadmap/webxr-in-shell-stage2-plan.md`** — the root analysis
   of the issue lives at lines 269–312 of this file ("Bridge-aware page
   color shift in shell mode (pre-existing)"). Path comparison and
   three plausible causes are listed there.
3. **`docs/roadmap/webxr-in-shell-plan.md`** — parent plan; this is
   the last follow-up before Stage 4 (live resize) closes the parent.
4. **Memory entries (auto-loaded via `MEMORY.md`):**
   - `feedback_srgb_blit_paths` — *critical*. Don't unify shell vs
     non-shell SRGB blit paths. They have different format expectations
     by design.
   - `feedback_3d_mode_terminology` — multiview language only. No
     "stereo"/"left+right eye"/"SBS" in code, comments, or commits.
   - `feedback_atlas_stride_invariant` — atlas stride logic must not
     branch on `sys->shell_mode`. Don't touch stride.
   - `feedback_use_build_windows_bat` — build only via the bat script.
   - `feedback_dll_version_mismatch` — copy all four binaries on
     deploy; rename DLL to `.oldN` if Chrome holds it.
   - `feedback_test_before_ci` — wait for visual confirmation on the
     Leia display before pushing.
   - `feedback_shell_screenshot_reliability` — eyeball the live
     display; PrintWindow can miss UI during eye-tracker warmup.
   - `reference_runtime_screenshot` — file-trigger compositor
     screenshot (`%TEMP%\shell_screenshot_trigger`). This is the right
     tool for capturing the combined atlas at byte level for diag-2.
   - `reference_service_log_diagnostics` — service log path
     (`%LOCALAPPDATA%\DisplayXR`).

## Build / test workflow (standard)

Per `feedback_use_build_windows_bat` + `feedback_dll_version_mismatch`:

1. `scripts\build_windows.bat build`
2. Stop running displayxr-* processes (kill bridge child first, then
   service).
3. Copy **all four** binaries to `C:\Program Files\DisplayXR\Runtime\`:
   `DisplayXRClient.dll`, `displayxr-service.exe`,
   `displayxr-shell.exe`, `displayxr-webxr-bridge.exe`. Rename DLL to
   `.oldN` first if Chrome is still loaded (the dir already has
   `.old1`–`.old35`).
4. Restart service with the dev-repo env var:
   ```
   DISPLAYXR_REPO_ROOT="C:\\Users\\Sparks i7 3080\\Documents\\GitHub\\openxr-3d-display" \
     cmd //c start "" "C:\\Program Files\\DisplayXR\\Runtime\\displayxr-service.exe"
   ```
5. Tell the user to close all Chrome windows before relaunching for
   the new DLL to load.

## Test pages

- **Bridge-aware (primary subject)**: `webxr-bridge/sample/` — served
  via local HTTP server (`python -m http.server 8000` in
  `webxr-bridge/`). Already running in the previous session — verify
  with `netstat -ano | findstr :8000`.
- **Legacy WebXR (regression)**: `https://immersive-web.github.io/webxr-samples/immersive-vr-session.html`
- **Handle app (regression)**: `cube_handle_d3d11_win` via launcher
  (Ctrl+L after shell activation).

## Visual checks on the user

The agent cannot see the Leia SR display. All visual verification
goes through the user. Per `feedback_shell_screenshot_reliability`,
PrintWindow screenshots can be unreliable during eye-tracker warmup —
ask the user to eyeball the live display, **but** the file-trigger
compositor screenshot (`reference_runtime_screenshot`) is reliable for
inspecting atlas pixel values directly. Use that for diagnostic
pixel pull-down.

## Diagnostic strategy

Per the plan, **measure before fixing**. The plan's diag-1 (format
dump) and diag-2 (pixel probe) cleanly separate the three plausible
causes. Don't speculate — get the data first.

The hypothesis is **one extra gamma transform** in the multi-comp
shader-blit step. Three candidates:

1. Per-client atlas SRV is SRGB-typed for bridge-aware (auto-linearize
   on sample) → most likely. If diag-1 confirms, fix is to align the
   sample-side typing or re-encode in the shader before writing UNORM.
2. Combined-atlas RTV/SRV is SRGB-typed somewhere undocumented.
3. DP weaver interprets bytes from combined-atlas differently than
   from per-client atlas.

Cause 1 is most likely; check it first.

## Pitfalls — don't trip these

1. **`feedback_srgb_blit_paths` is the hard rule.** The non-shell
   shader-blit path linearizes; the shell raw-copy path doesn't. They
   exist independently for a reason. Don't refactor them into a single
   helper. The fix is targeted at the multi-comp render or the crop
   step, not the per-client atlas blit.
2. **Don't unify atlas formats between bridge and legacy.** If the
   bridge swapchain format differs from Chrome WebGL's, that's not the
   fix surface — the multi-comp must be robust to either. Forcing the
   bridge to a different swapchain format would be brittle.
3. **`process_atlas` is opaque.** The DP weaver is binary; its
   expectations are reverse-engineered. If the bug turns out to be
   cause #3 (DP-interpretation), don't fight the SDK — match what the
   non-shell direct-DP path delivers.
4. **One extra gamma is a clean signature.** If measured drift isn't
   `linear ↔ sRGB` shaped at the test pixel, the hypothesis is wrong.
   Don't chase phantoms; revisit the analysis.
5. **Bridge process restart can change behavior.** The bridge negotiates
   its swapchain format at session-create time. If the format depends
   on bridge state (cold vs warm boot, prior session detritus), the bug
   may appear/disappear across restarts. Note if symptom is
   intermittent.
6. **Multiview language.** No "stereo", "left/right eye", "SBS" in new
   code, comments, log lines, or commit messages. Use *tile / view /
   atlas / per-tile / per-view*.

## Adjacent issues (DO NOT FIX in this task)

- **Intermittent service crash on window close.** Captured under
  procdump (running watcher PID, dumps land in `_dumps/`). Different
  crash signature than the qwerty-bounds one already fixed
  (`48be6c890`). Independent investigation. Verify it doesn't recur
  during regression testing, but don't chase it as part of this task.
- **Window pose resets on session re-create.** Documented in
  `webxr-in-shell-stage2-plan.md` §"Window pose resets on session re-
  create". UX bucket; Stage 4 territory.
- **Launcher empty-state on auto-spawn from IPC.** Documented in
  Stage 2 plan. Separate.

Verify none of these regress during testing.

## Definition of done

- [ ] Bridge-aware WebXR in shell: deep navy `0x0d0d40` background
      renders correctly (within 5% of non-shell rendering).
- [ ] Side-by-side: shell vs non-shell match visually.
- [ ] Regression matrix (8 rows in plan) all pass.
- [ ] 1–2 commits stacked on `feature/webxr-in-shell`. Build green.
- [ ] `feedback_srgb_blit_paths` honored: shell vs non-shell paths stay
      distinct.
- [ ] Nothing pushed without user sign-off.

## After this ships

This is the last follow-up between Stage 3 and Stage 4. Once it lands,
Stage 4 (live resize during an active session) closes the parent
`webxr-in-shell-plan.md`. The intermittent close-time service crash
remains as a separate small task.
