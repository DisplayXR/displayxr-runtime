# Workspace / Multi-Compositor Optimization — Agent Prompt

Use this prompt to start a new Claude Code session on the `shell/optimization` branch (worktree at `../openxr-3d-display-shell-opt`).

> **Branch naming note:** the branch and the docs are still called `shell/optimization` / `shell-optimization-*` for git history continuity, but the work is purely **workspace / multi-compositor / runtime** — there is no shell-binary code here. Phase 1 honoured the project rule that compositor code must not introduce "shell" terminology (`feedback_no_shell_in_compositor.md`); Phase 2 must do the same.

---

## Prompt — Phase 2

```
I'm picking up the shell-optimization work on DisplayXR. **Phase 1 is shipped** — see `docs/roadmap/shell-optimization-status.md` for what landed and the measured numbers. **You are starting Phase 2.** Do not re-implement Phase 1.

## What Phase 1 left in place (read first)

1. `docs/roadmap/shell-optimization-status.md` — what shipped, measured 30 s standalone vs 4-cube workspace numbers. **Phase 1 result:** zero KeyedMutex timeouts under 4-cube load; per-cube rate is 14–17 fps vs 60 fps standalone (the architectural serialization Phase 3 will fix).
2. `docs/roadmap/shell-optimization.md` — the multi-phase roadmap.
3. `docs/roadmap/shell-optimization-plan.md` — **Phase 2 tasks 2.1, 2.2, 2.3 are in here.** This is your spec.
4. `CLAUDE.md` — project overview, build commands, architecture.
5. `docs/architecture/compositor-pipeline.md` — pipeline reference.
6. `docs/specs/swapchain-model.md` — swapchain / canvas semantics that any sync redesign must preserve.
7. `docs/adr/ADR-001-native-compositors-per-graphics-api.md` — why each API has its own compositor (still holds; all this work is *within* the D3D11 path).

## Memory files to honour

The Phase-1 commit added `feedback_no_shell_in_compositor.md` — **compositor code never says "shell"**, only `workspace` / `container app`. Audit your diff before each commit.

Other relevant feedback memories:
- `feedback_use_build_windows_bat.md` — never run cmake/ninja directly.
- `feedback_test_before_ci.md` — wait for the user to test locally before pushing.
- `feedback_dll_version_mismatch.md` — after each rebuild, copy `displayxr-service.exe` + `DisplayXRClient.dll` to `C:\Program Files\DisplayXR\Runtime\`.
- `feedback_local_vs_ci_builds.md` — prefer local builds.
- `feedback_branch_workflow.md` — already on `shell/optimization`; don't create new branches inside this one without asking.
- `feedback_atlas_stride_invariant.md` — slot stride invariant. Phase 2 must not break it.
- `feedback_mirror_inprocess_arch.md` — for any new tile geometry that crosses IPC, app (or bridge proxy) is source of truth.
- `feedback_srgb_blit_paths.md` — non-workspace needs shader blit (linearize for DP); workspace uses raw copy.
- `feedback_shell_screenshot_reliability.md` — atlas screenshots miss UI during eye-tracking warmup; eyeball the Leia SR display for visual correctness.
- `reference_runtime_screenshot.md` — file-trigger atlas screenshot.
- `reference_service_log_diagnostics.md` — service-log breadcrumb pattern (`[BRIDGE BLIT]`, `[DP HANDOFF]`, etc.). Phase 2 adds `[FENCE]` to the same family.

## What Phase 1 already gave you (don't redo)

- **Diagnostic taxonomy** in `comp_d3d11_service.cpp`'s `compositor_layer_commit`:
  - `[ZC] client=<ptr> views=N zero_copy=Y/N reason=<str>` — one-shot per decision flip
  - `[MUTEX] client=<ptr> timeouts=K acquires=A avg_acquire_us=U window_s=10` — rate-limited 1×/10 s
  - `[CLIENT_FRAME_NS] client=<ptr> dt_ns=<int>` — env-gated
  - `[PRESENT_NS] client=<ptr|workspace> dt_ns=<int>` — env-gated
  - All emit at `U_LOG_W` (the global log level filters out `U_LOG_I` by default — don't repeat that mistake).
  - All env-gated logs use `DISPLAYXR_LOG_PRESENT_NS=1`.
  - `client=` tags use the `d3d11_service_compositor*` struct pointer (stable per-client; `c->render.hwnd` is null in workspace mode for service-owned windows). `client=workspace` for the multi-compositor combined-atlas swap chain.
  - **Add `[FENCE]` to this family for Phase 2** per the plan's Diagnostics section.

- **Per-view zero-copy eligibility array** (`view_zc_eligible[XRT_MAX_VIEWS]`) and **per-view skip-on-timeout array** (`view_skip_blit[]`) already wired in the per-view loop around what was line 9885 (drifted with the Phase 1 inserts — verify before editing). Phase 2's GPU-fence wait should plug into the same per-view eligibility model rather than introducing a parallel one.

- **Per-client diagnostic state fields** added to `struct d3d11_service_compositor`: `zc_last_logged_*`, `mutex_*_in_window`, `last_commit_ns`. Add `fence_*` fields next to them following the same pattern.

- **Bench harness** at `scripts/bench_shell_present.ps1` + `scripts/bench_diff.ps1`. It already parses `[PRESENT_NS]` / `[CLIENT_FRAME_NS]` / `[MUTEX]` / `[ZC]` per-client. **Extend the parser** to handle your new `[FENCE]` lines; do not invent a parallel benchmarking framework.

## Phase 2 — what you're building

From `docs/roadmap/shell-optimization-plan.md` Phase 2:

1. **Task 2.1 — Add shared D3D11 fence to the IPC swapchain protocol.** `ID3D11Fence` with `D3D11_FENCE_FLAG_SHARED | D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER` exported as NT handle, opened by the client. Client increments the fence value on `xrEndFrame` after submitting render commands. Service queues `ID3D11DeviceContext4::Wait` on the GPU command stream — **no CPU-side `WaitForSingleObject` on the render thread, ever.** Feature-flagged so legacy KeyedMutex clients (WebXR bridge, older `_ipc` apps) still work.

2. **Task 2.2 — Per-tile staleness tracking; skip blit for unsignaled tiles.** Reuse the persistent per-client atlas slot's prior content (the same trick Phase 1 uses on mutex timeout). Check fence value cheaply (GPU-side query); if no new value since last compose, skip the blit for that view. Verify with a synthetic test where one client deliberately misses every other frame.

3. **Task 2.3 — Latest-frame-wins ring or double buffer per client.** Defer this to Phase 3 if 2.1+2.2 already hit the Phase 2 acceptance criteria. Don't over-engineer.

## Open questions — resolve before committing the design

From the plan's "Open questions" section:

1. Does Leia SR's D3D11 weaver have any sync-point assumptions about when the atlas is GPU-ready that the new fence path could violate? Check `drivers/leia/leiasr_d3d11.cpp` and the DP vtable contract before Phase 2.
2. Does the WebXR bridge currently rely on the broadcast pacing path? See `project_webxr_bridge_v2_phase3.md`. If yes, Phase 3 (not Phase 2) must keep the broadcast path available behind an opt-in, but verify Phase 2's fence path doesn't break it inadvertently.
3. Is `ID3D11Fence` cross-process `SHARED_NTHANDLE` actually supported on the Leia SR adapter on the dev box? **Validate with a tiny standalone proof-of-concept before committing to Phase 2.** If `D3D11_FENCE_FLAG_SHARED` works but `_CROSS_ADAPTER` doesn't, the design changes (you have to require same-adapter, which is fine on the Leia box but documents an assumption).

## Acceptance criteria (Phase 2)

* Service render thread `os_monotonic` measurement: < 1 ms spent in synchronization per client per frame, sustained.
* Synthetic stall test: one client misses every 2nd frame, the others maintain full cadence.
* No CPU `AcquireSync` / `WaitForSingleObject` on the service render thread (verify by source grep + ETW trace).
* Phase 1 acceptance criteria (zero `[MUTEX]` timeouts, `[ZC]` reasons remain consistent) **still hold** on the same 30 s 4-cube benchmark.
* Numbers committed to `docs/roadmap/shell-optimization-status.md` Phase 2 section.

## Tasks (track in shell-optimization-status.md)

The Phase 1 section is done. Add a Phase 2 section with the same checklist style:

- [ ] Verify the cited line numbers in `comp_d3d11_service.cpp` haven't drifted (Phase 1 added ~250 lines around the per-view loop; line numbers in the plan are pre-Phase-1).
- [ ] PoC: cross-process `ID3D11Fence` on the Leia SR dev box (open question #3 above).
- [ ] Task 2.1 — IPC protocol additive, fence creation in service, fence import in client, GPU-side `Wait` on the service.
- [ ] Task 2.2 — per-view fence-value tracking + skip-blit on unsignaled.
- [ ] Add Phase 2 `[FENCE]` diagnostic per `shell-optimization-plan.md`'s Diagnostics section. Use `U_LOG_W` (`U_LOG_I` is filtered by default); use the `d3d11_service_compositor*` pointer as `client=`.
- [ ] Build via `scripts\build_windows.bat build` and copy binaries to `C:\Program Files\DisplayXR\Runtime\`.
- [ ] Extend `scripts/bench_shell_present.ps1` to parse the new `[FENCE]` lines.
- [ ] Capture before/after Phase 2 benchmarks; commit numbers to status doc.
- [ ] Synthetic stall test: simulate slow client, verify fast clients unaffected.
- [ ] Wait for the user to test on the Leia SR display before pushing.
- [ ] Open Phase 2 PR back to `main`.

DO NOT bundle Phase 3 into the same PR.

## Compositor terminology — IMPORTANT

`feedback_no_shell_in_compositor.md`:
- Compositor / runtime code uses `workspace` / `container app`. **Never `shell`** in identifiers, comments, or in the text of emitted log lines.
- Existing in-tree counts: `comp_d3d11_service.cpp` has `workspace` ~300×, `shell` ~18× (legacy debt — don't add to it).
- Before each commit, grep your diff for `shell` (case-insensitive) and either delete it, rename to `workspace`, or move it into `src/xrt/targets/shell/` if it's specifically about that binary.

## How to verify your changes

1. Build via `scripts\build_windows.bat build`.
2. Copy `displayxr-service.exe` + `DisplayXRClient.dll` to `C:\Program Files\DisplayXR\Runtime\`.
3. Standalone baseline (1 cube, in-process): `cmd.exe /c "set DISPLAYXR_LOG_PRESENT_NS=1 && cube_handle_d3d11_win.exe"` — expect `[CLIENT_FRAME_NS]` ≈ 16.6 ms (60 fps).
4. Workspace (4 cubes via `displayxr-shell.exe`): same env var, 4 cube args. **Phase 2 success = each per-client `[CLIENT_FRAME_NS]` improves toward 60 fps; `[FENCE]` shows GPU-side waits, not CPU-side; `[MUTEX]` events go to zero (the path is no longer used for fence-capable clients).**
5. Synthetic stall test for Task 2.2 verification.
6. **Ask the user to confirm visual smoothness on the Leia SR display.** Screenshot-only verification is insufficient (`feedback_shell_screenshot_reliability.md`).

## Tone

Performance work. Numbers > opinions. If the fence path "should" be faster but the bench shows otherwise, the design was wrong. Always commit before/after numbers alongside the code.

This is also an additive IPC protocol change. The Phase 1 PR was small and surgical; Phase 2 is bigger but stays surgical by feature-flagging — clients/services without fence support fall back to the existing KeyedMutex path. Keep WebXR bridge + legacy `_ipc` apps working through every commit.

## Out of scope (don't touch)

- Phase 3 (per-client pacing) — that's the next agent.
- macOS / Metal multi-compositor.
- Vulkan service compositor.
- IPC protocol changes that BREAK existing clients (legacy apps, WebXR bridge). Additive only; feature-flag the new path.

## When in doubt

Ask the user. The user has deep context (designed the project, knows the SR SDK internals, has the Leia SR hardware). For any architectural decision not explicitly covered in the plan — especially anything that could affect the WebXR bridge, legacy apps, or the chrome / launcher rendering — ask before committing.
```

---

## How to use this prompt

1. Open a new Claude Code session in the worktree:
   ```
   cd ../openxr-3d-display-shell-opt
   claude
   ```
2. Paste the prompt above.
3. The agent will read context, validate the cross-process fence PoC, then start Phase 2.
