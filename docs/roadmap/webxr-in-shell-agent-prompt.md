# WebXR-in-Shell — Agent Kickoff Prompt

Paste the block below into a fresh Claude Code session on the new branch.
Full design lives in [`webxr-in-shell-plan.md`](./webxr-in-shell-plan.md).

---

Create branch `feature/webxr-in-shell` off `main` (after PR #161 merges; if it
hasn't, branch off `feature/service-orchestrator`). The goal is to make Chrome
WebXR pages render correctly inside a shell window.

Start by reading `docs/roadmap/webxr-in-shell-plan.md` end-to-end — it has
context, staging, test plan, gotchas, and files likely touched. Don't
re-derive the approach from code; the plan reflects a design conversation and
should be followed as-is. If you want to deviate, flag it first.

Work in stages as ordered in the plan:

- Stage 0: shared plumbing (per-client `xrt_shell_window_info` accessor on
  the service). No behavior change; verify by logging info on every new IPC
  client when shell is active.
- Stage 1 (optional fallback): drop the `!sys->shell_mode` guard on
  `needs_scale` in `comp_d3d11_service.cpp` so legacy content at least fills
  the window while the real fix is in progress. Watch the srgb paths — see
  the `feedback_srgb_blit_paths` memory.
- Stage 2: legacy path — `xrLocateViews` + `recommendedImageRect` override
  for shell-mode clients. Test with
  `immersive-web.github.io/webxr-samples/immersive-vr-session.html` in
  Chrome.
- Stage 3: bridge-aware path — bridge binds each session to its IPC
  client's shell window; `display-info` becomes window-scoped.
- Stage 4: live resize/pose events through the bridge.

Testing: user has a Leia SR display + full dev stack. Use
`scripts\build_windows.bat build` to rebuild (see
`feedback_use_build_windows_bat` memory — never call cmake/ninja directly).
After rebuild, copy binaries to `C:\Program Files\DisplayXR\Runtime\` per
the `feedback_dll_version_mismatch` memory. Shell auto-spawn via Ctrl+Space
and bridge trampoline on 9014 are already working from the previous
branch.

Commit per stage; don't push or merge. Each stage must build green and pass
the stage's test from the plan before moving on. Flag for user review
between stages so they can eyeball the display (see
`feedback_shell_screenshot_reliability` memory — automated capture is
unreliable during tracking warmup).

If you hit the blit-scale SRGB / atlas-stride concerns, re-read the
relevant feedback memories before touching — there are prior decisions
encoded there.
