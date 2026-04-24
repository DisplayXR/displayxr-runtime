# WebXR Qwerty-Freeze + Legacy-Offset — Agent Prompt

Paste this into a fresh Claude Code session to start the task.

---

## Prompt

```
Read docs/roadmap/webxr-qwerty-freeze-plan.md and execute the
Staging section (1 → 2 → 3 → 4), committing per fix.

Parent plans:
  docs/roadmap/webxr-in-shell-plan.md
  docs/roadmap/webxr-in-shell-stage3-plan.md (Stage 3, now complete)
  docs/roadmap/webxr-in-shell-stage2-plan.md (§1 follow-up — this task)

Commit per fix. Don't push. Don't run /ci-monitor.

Work on branch feature/webxr-in-shell — Stage 3 + three polish
commits are already on it (10+ commits ahead of origin). Branch
state: all green locally, ready for this task to stack on top.

Goal: make legacy Chrome WebXR usable inside the shell — WASD / Alt-drag
move the camera inside the WebXR scene (NOT the shell window), starting
from a sensible camera offset near the nominal viewer distance, not 2 m
behind the display. Bridge-aware WebXR must keep owning input while its
WS client is attached.
```

---

## Context to load before starting

1. **`docs/roadmap/webxr-qwerty-freeze-plan.md`** — this task's detailed
   plan with sub-stages, design options (D1 picks option 3, D2 picks
   option 1), verification matrix, pitfalls.
2. **`docs/roadmap/webxr-in-shell-plan.md`** — parent plan.
3. **`docs/roadmap/webxr-in-shell-stage2-plan.md`** — the Stage 2
   follow-up section documents this as §1.
4. **Memory entries (auto-loaded via MEMORY.md):**
   - `feedback_use_build_windows_bat`
   - `feedback_dll_version_mismatch`
   - `feedback_test_before_ci`
   - `feedback_3d_mode_terminology` (multiview language — no
     "stereo"/"left eye"/"SBS" in new code)
   - `feedback_shell_screenshot_reliability`
   - `reference_runtime_screenshot` (compositor file-trigger screenshot)
   - `reference_service_log_diagnostics` (%LOCALAPPDATA%\DisplayXR)

## Build / test workflow (standard)

Per `feedback_use_build_windows_bat` + `feedback_dll_version_mismatch`:

1. `scripts\build_windows.bat build`
2. Stop running displayxr-* processes (kill bridge child before service).
3. Copy **all four** binaries to `C:\Program Files\DisplayXR\Runtime\`:
   `DisplayXRClient.dll`, `displayxr-service.exe`,
   `displayxr-shell.exe`, `displayxr-webxr-bridge.exe`. Rename-then-copy
   for the DLL if Chrome is still loaded (Program Files dir already has
   `.old1`–`.old25` suffixes from prior iterations).
4. Restart service with the dev-repo env var:
   ```
   DISPLAYXR_REPO_ROOT="C:\\Users\\Sparks i7 3080\\Documents\\GitHub\\openxr-3d-display" \
     cmd //c start "" "C:\\Program Files\\DisplayXR\\Runtime\\displayxr-service.exe"
   ```
5. Tell the user to close all Chrome windows before relaunching (old
   DLL loaded in the live Chrome process until exit).

## Test pages

- **Legacy WebXR** (primary subject):
  `https://immersive-web.github.io/webxr-samples/immersive-vr-session.html`
- **Bridge-aware sample** (regression — must still freeze qwerty while
  attached): `webxr-bridge/sample/index.html` (served at e.g.
  `http://localhost:8000/sample/`).
- **Handle app** (regression): `cube_handle_d3d11_win` via launcher
  (Ctrl+L after shell activation).

## Visual tests on the user

The agent cannot see the Leia SR display. All visual verification
goes through the user. Per `feedback_shell_screenshot_reliability`,
screenshots can be unreliable during eye-tracker warmup — ask the
user to eyeball the live display.

## Pitfalls — stop and think at each, do not repeat earlier mistakes

1. **"My change shouldn't affect this path."** If you think a fix in
   qwerty won't touch bridge-aware behavior, add a diagnostic log line
   that prints the actual gate state (`bridge_client_is_live` result,
   `g_qwerty_bridge_relay_active`) and read the log to confirm.
   Multiple Stage 2 rounds lost time to reason-only analysis.

2. **Per-client `c->render.hwnd` is inconsistent.** The existing
   oscillation bug is rooted in this — each client's `layer_commit`
   passes a *different* hwnd to `bridge_client_is_live`. Don't
   replicate that pattern anywhere else. D1 option 3 compute
   authoritatively from `sys->compositor_hwnd` or by scanning
   `mc->clients[]`.

3. **Qwerty is a singleton.** There's one qwerty device shared across
   every session. "Per-session qwerty" isn't a thing without plumbing
   session identity through `get_tracked_pose` — don't add that this
   pass; accept "all qwerty on or all qwerty off" and make the
   single gate authoritative.

4. **Hardcoded starting pose has non-shell users.** `QWERTY_HMD_DISPLAY_POS`
   `(0, 1.6f, -2.0f)` at `qwerty_device.c:52` is reasonable for HMD
   simulator (gui, cli test harnesses). D2 option 1 changes the
   default; confirm the `nominal_viewer_*` fallback is populated for
   those use cases before flipping. If not, gate the override.

5. **Bridge has TWO IPC identities.** Stage 3b added a secondary
   `"displayxr-webxr-bridge-ipc"` connection alongside the OpenXR
   `"displayxr-webxr-bridge"` session. Only the OpenXR one is a
   bridge-relay session (sets `is_bridge_relay`, increments
   `g_bridge_relay_active`). The IPC query connection should not
   influence the qwerty gate. Filter/ignore it by name if you iterate
   IPC client lists.

6. **Multiview language.** No "left/right eye", "stereo", or "SBS"
   in new code, comments, log lines, or commit messages. Use
   tile / view / atlas / per-tile / per-view.

7. **DLL git_tag mismatch trap.** Every rebuild re-links every
   binary; the IPC handshake strncmps `u_git_tag`. Always copy all
   four to Program Files, not just the file you edited.

## Stage 2 follow-ups (DO NOT FIX in this task)

- §2 Bridge-aware gamma / color shift — next task after this.
- §3 Launcher empty-state on auto-spawn — separate.
- §4 Window pose reset on session re-create — Stage 4 / UX.

Verify none of these regress during testing.

## Definition of done

- [ ] 1–2 commits stacked on `feature/webxr-in-shell`, build green.
- [ ] Legacy WebXR in shell: WASD / Alt-drag moves the camera inside
      the WebXR scene. Shell window stays fixed.
- [ ] Legacy WebXR in shell: initial camera pose is close to the
      nominal viewer, not 2 m behind the display.
- [ ] Bridge-aware WebXR in shell: qwerty still frozen while WS client
      is attached; wakes up on WS disconnect / `bridge-detach`.
- [ ] Handle apps in shell: WASD + hotkeys unchanged.
- [ ] Two-slot mix (handle + legacy WebXR): both respond to qwerty.
- [ ] Regressions from the verification matrix: all pass.

## After this ships

Next hard problem is the **bridge-aware gamma / color shift**
(blue → purple). Separate plan + agent prompt; investigate the
compositor blit color-space pipeline with `feedback_srgb_blit_paths`
as the starting constraint. After that: Stage 4 (live resize during
session) closes the parent plan.
