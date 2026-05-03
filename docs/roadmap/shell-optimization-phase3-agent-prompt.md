# Workspace / Multi-Compositor Optimization — Phase 3 Agent Prompt

Use this prompt to start a new Claude Code session on the `shell/optimization` branch (worktree at `../openxr-3d-display-shell-opt`).

> **Branch naming note:** the branch and the docs are still called `shell/optimization` / `shell-optimization-*` for git history continuity, but the work is purely **workspace / multi-compositor / runtime** — there is no shell-binary code here. Phase 1 honoured the project rule that compositor code must not introduce "shell" terminology (`feedback_no_shell_in_compositor.md`); Phase 2 honoured it; Phase 3 must do the same.

---

## Prompt — Phase 3

```
I'm picking up the shell-optimization work on DisplayXR. **Phases 1 and 2 are shipped** — see `docs/roadmap/shell-optimization-status.md` for what landed and the measured numbers. **You are starting Phase 3.** Do not re-implement earlier phases.

## What Phase 1 + Phase 2 left in place (read first)

1. `docs/roadmap/shell-optimization-status.md` — what shipped, before/after numbers per phase.
   - **Phase 1 result:** zero `[MUTEX]` timeouts under 4-cube load; per-cube rate 14–17 fps (vs 60 fps standalone).
   - **Phase 2 result:** per-IPC-client `workspace_sync_fence` (`ID3D11Fence`) replaces the per-view CPU `IDXGIKeyedMutex::AcquireSync(0, 4ms)` with a GPU `ID3D11DeviceContext4::Wait`. `[MUTEX] acquires=0` (legacy path bypassed for fence-capable clients). `[FENCE] waits_queued` matches per-client commit rate. Per-cube fps 20–22 (+35-48% over Phase 1). Workspace combined-atlas Present remains 60 fps.
   - **Remaining gap:** per-cube ~21 fps vs 60 fps standalone is the multi-system render-loop serialization Phase 3 must address.
2. `docs/roadmap/shell-optimization.md` — the multi-phase roadmap.
3. `docs/roadmap/shell-optimization-plan.md` — **Phase 3 tasks 3.1, 3.2, 3.3 are in here.** This is your spec.
4. `CLAUDE.md` — project overview, build commands, architecture.
5. `docs/architecture/compositor-pipeline.md` — pipeline reference.
6. `docs/specs/swapchain-model.md` — swapchain / canvas semantics that any pacing redesign must preserve.
7. `docs/adr/ADR-001-native-compositors-per-graphics-api.md` — why each API has its own compositor (still holds; all this work is *within* the D3D11 path).

## Memory files to honour

The Phase 1 commit added `feedback_no_shell_in_compositor.md` — **compositor code never says "shell"**, only `workspace` / `container app`. Audit your diff before each commit (`git diff | rg -in shell` should be clean for new code).

Other relevant feedback memories:
- `feedback_use_build_windows_bat.md` — never run cmake/ninja directly.
- `feedback_test_before_ci.md` — wait for the user to test locally before pushing.
- `feedback_dll_version_mismatch.md` — after each rebuild, copy `displayxr-service.exe` + `DisplayXRClient.dll` to `C:\Program Files\DisplayXR\Runtime\`.
- `feedback_local_vs_ci_builds.md` — prefer local builds.
- `feedback_branch_workflow.md` — already on `shell/optimization`; don't create new branches inside this one without asking.
- `feedback_atlas_stride_invariant.md` — slot stride invariant. Phase 3 must not break it.
- `feedback_mirror_inprocess_arch.md` — for any new tile geometry that crosses IPC, app (or bridge proxy) is source of truth.
- `feedback_srgb_blit_paths.md` — non-workspace needs shader blit (linearize for DP); workspace uses raw copy.
- `feedback_shell_screenshot_reliability.md` — atlas screenshots miss UI during eye-tracking warmup; eyeball the Leia SR display for visual correctness.
- `feedback_pr_workflow.md` — don't auto-merge PRs; user reviews and merges.
- `reference_runtime_screenshot.md` — file-trigger atlas screenshot.
- `reference_service_log_diagnostics.md` — service-log breadcrumb pattern. Phase 3 adds `[PACE]` to the same family.
- `project_webxr_bridge_v2_phase3.md` — bridge depends on the broadcast pacing path. Read this before changing pacing defaults.

## What Phase 1 + Phase 2 already gave you (don't redo)

- **Diagnostic taxonomy** in `comp_d3d11_service.cpp`'s `compositor_layer_commit`:
  - `[ZC] client=<ptr> views=N zero_copy=Y/N reason=<str>` — one-shot per decision flip
  - `[MUTEX] client=<ptr> timeouts=K acquires=A avg_acquire_us=U window_s=10` — rate-limited 1×/10 s (Phase 1)
  - `[FENCE] client=<ptr> waits_queued=W stale_views=S last_value=V window_s=10` — rate-limited 1×/10 s (Phase 2)
  - `[CLIENT_FRAME_NS] client=<ptr> dt_ns=<int>` — env-gated
  - `[PRESENT_NS] client=<ptr|workspace> dt_ns=<int>` — env-gated
  - All emit at `U_LOG_W` (the global log level filters out `U_LOG_I` by default — don't repeat that mistake).
  - All env-gated logs use `DISPLAYXR_LOG_PRESENT_NS=1`.
  - `client=` tags use the `d3d11_service_compositor*` struct pointer (stable per-client). `client=workspace` for the multi-compositor combined-atlas swap chain.
  - **Add `[PACE]` to this family for Phase 3** per the plan's Diagnostics section: `[PACE] client=<ptr> target_hz=H actual_hz=H actual_ms=M window_s=10`.

- **Per-IPC-client `workspace_sync_fence`** (Phase 2): the service queues a GPU `ID3D11DeviceContext4::Wait` on the per-view loop instead of CPU-blocking on the keyed mutex. The cache-barrier gotcha — `IDXGIKeyedMutex::AcquireSync(0, 0)` is still required to issue the cross-process GPU memory barrier on `SHARED_KEYEDMUTEX` textures — is captured in the Phase 2 status section. Don't break this.

- **Per-view eligibility arrays** (`view_zc_eligible[XRT_MAX_VIEWS]`, `view_skip_blit[]`, `last_composed_fence_value[]`) and **per-client diagnostic state fields** (`zc_last_logged_*`, `mutex_*_in_window`, `fence_*_in_window`, `last_commit_ns`) on `struct d3d11_service_compositor`. Add `pace_*` fields next to them following the same pattern.

- **Bench harness** at `scripts/bench_shell_present.ps1`. Already parses `[PRESENT_NS]` / `[CLIENT_FRAME_NS]` / `[MUTEX]` / `[ZC]` / `[FENCE]` per-client. **Extend the parser** to handle your new `[PACE]` lines; do not invent a parallel benchmarking framework.

- **PoC pattern** at `scripts/poc_shared_fence.{cpp,bat}` — Phase 2 used it to validate cross-process `ID3D11Fence` before committing to the design. Mirror this style if Phase 3 needs to validate any standalone Win32 / threading / pacing assumption (e.g. per-client wait-event throughput, idle-client GPU residency) before committing.

## Phase 3 — what you're building

From `docs/roadmap/shell-optimization-plan.md` Phase 3:

1. **Task 3.1 — Carve out per-client pacing context.** Each client gets its own `predict_frame` + `wake_up_time_ns` derived from its own intended cadence (display Hz by default; client may want slower). Touches:
   - `src/xrt/compositor/multi/comp_multi_compositor.c` — `run_func()` wait thread (~lines 262–350), `wait_for_wait_thread()` (~line 875), `layer_commit` handler.
   - `src/xrt/compositor/multi/comp_multi_system.c` — `xrt_system_compositor` render loop (~lines 3048–3109), `predict_frame` per-client, broadcast wake-time removed.
   - IPC frame-timing path (`xrWaitFrame` / `xrBeginFrame` IPC calls) — wake time is now per-client, not broadcast.

   **Approach:** The system render loop runs at display Hz, samples the freshest tile from each client (regardless of whether that client just produced a fresh frame — Phase 2's per-view staleness skip-blit already handles "no new frame" gracefully), and composes. Idle clients consume zero CPU/GPU between their frames — their last tile is just sampled by the service.

2. **Task 3.2 — Mixed refresh-rate support.** A 30 Hz client + a 60 Hz client should coexist. The compose loop runs at 60 Hz; the 30 Hz client's tile is reused on alternate compose frames. Falls out naturally from 3.1 if 3.1 is done correctly. Validate with a synthetic 30 Hz client (a debug-build flag in `cube_handle_d3d11_win` that sleeps every other frame, or a separate test app).

3. **Task 3.3 — Backwards compatibility for legacy / IPC apps.** The existing broadcast pacing path must remain available for clients that depend on it (legacy `_ipc` apps, WebXR bridge — see `project_webxr_bridge_v2_phase3.md`). Per-client pacing opt-in via a swapchain or session creation flag, migrate clients incrementally.

## Open questions — resolve before committing the design

1. Does WebXR bridge currently rely on the broadcast pacing path for any frame-rate-sensitive behavior? Read `project_webxr_bridge_v2_phase3.md` and the bridge's frame submission path before changing default pacing semantics. The fall-back contract from Phase 2 — bridge stays on KeyedMutex / no fence — gives you a natural `is_legacy_pacing_client()` test, but verify.
2. What signals "this client is fence-capable AND wants per-client pacing"? Reuse the `workspace_sync_fence` presence check, or add a new flag? Phase 2's `compositor_get_workspace_sync_fence` returns `have_fence` — that's a natural signal but think through whether the two should be coupled or independent.
3. How does the per-client `predict_frame` interact with the existing OXR state tracker's `xrWaitFrame` flow? `oxr_session_frame_end.c` and the IPC client `xrWaitFrame` path need careful read.
4. The wait thread in `comp_multi_compositor.c` (`run_func()`, `wait_fence()`) — does it become vestigial after Phase 2's GPU-side fence? If so, Phase 3 may simplify or retire it (which is also a Phase 4 candidate cleanup; decide which phase owns it).
5. The `xrt_system_compositor::render_loop` cadence: today it's bound to the slowest client. After 3.1 it should run at display Hz unconditionally. Verify no downstream code depends on the render loop pausing when all clients are idle.

## Acceptance criteria (Phase 3)

* 1 idle + 1 active client: idle client consumes < 1% CPU and 0% GPU; active client maintains native cadence.
* 1 fast + 1 slow client: fast client maintains native cadence regardless of slow client's pacing.
* 4-cube workspace per-cube `[CLIENT_FRAME_NS]` p50 approaches 60 fps (target: ≥ 50 fps; ideal 60 fps). Phase 2 left this at ~21 fps — closing this gap is the Phase 3 win.
* No regressions on legacy / WebXR bridge / `_ipc` cube apps (they keep broadcast pacing).
* Phase 1 + Phase 2 acceptance criteria **still hold** on the same 30 s 4-cube benchmark (zero `[MUTEX]` timeouts, `[ZC]` reasons consistent, `[FENCE] stale_views` low under steady state, no CPU `AcquireSync(timeout > 0)` on the service render thread).
* Numbers committed to `docs/roadmap/shell-optimization-status.md` Phase 3 section.

## Tasks (track in shell-optimization-status.md)

The Phase 1 + Phase 2 sections are done. Add a Phase 3 section with the same checklist style:

- [ ] Verify the cited line numbers in `comp_multi_compositor.c` and `comp_multi_system.c` haven't drifted.
- [ ] Read the existing wait-thread + broadcast-pacing implementation top-to-bottom; document what it does + the invariants it preserves in the status doc before changing anything (avoid breaking subtle invariants).
- [ ] PoC / synthetic stall scaffolding: 30 Hz client (slow cube — `Sleep(33)` every other frame, gated by an env var or build flag). Verify Phase 2's per-view staleness skip-blit handles it. This is the test scaffolding 3.2 will use.
- [ ] Task 3.1 — per-client predict / wake state. Carve out per-client pacing context in `comp_multi_compositor.c`. Render loop in `comp_multi_system.c` becomes a pure consumer at display Hz.
- [ ] Task 3.2 — mixed refresh-rate test passes (30 Hz client + 60 Hz client coexist; 60 Hz client's `[CLIENT_FRAME_NS]` unaffected).
- [ ] Task 3.3 — opt-in flag for per-client pacing; legacy clients keep broadcast pacing. WebXR bridge demo still works.
- [ ] Add Phase 3 `[PACE]` diagnostic per `shell-optimization-plan.md`'s Diagnostics section. `U_LOG_W`, rate-limited 1×/10 s, `client=<ptr>`.
- [ ] Build via `scripts\build_windows.bat build` and copy binaries to `C:\Program Files\DisplayXR\Runtime\`.
- [ ] Extend `scripts/bench_shell_present.ps1` to parse the new `[PACE]` lines; add summary CSV columns mirroring the `fence_*` Phase 2 columns.
- [ ] Capture before/after Phase 3 benchmarks; commit numbers to status doc.
- [ ] 1-idle-+-1-active test: idle client confirmed < 1% CPU + 0% GPU on the dev box.
- [ ] 1-fast-+-1-slow synthetic test: fast client unaffected by slow client.
- [ ] Wait for the user to test on the Leia SR display before pushing.
- [ ] Open Phase 3 PR back to `main`. (At this point the user may want to fold Phase 1 + Phase 2 + Phase 3 into a single PR — ASK before splitting or bundling.)

DO NOT bundle Phase 4 cleanup (damage tracking, retire wait-thread, D3D12 service compositor parity) into the same PR.

## Compositor terminology — IMPORTANT

`feedback_no_shell_in_compositor.md`:
- Compositor / runtime code uses `workspace` / `container app`. **Never `shell`** in identifiers, comments, or in the text of emitted log lines.
- Existing in-tree counts: `comp_d3d11_service.cpp` has `workspace` ~300×, `shell` ~18× (legacy debt — don't add to it). `comp_multi_compositor.c` and `comp_multi_system.c` haven't been audited for legacy "shell" — check before adding new identifiers.
- Before each commit, grep your diff for `shell` (case-insensitive) and either delete it, rename to `workspace`, or move it into `src/xrt/targets/shell/` if it's specifically about that binary.

## How to verify your changes

1. Build via `scripts\build_windows.bat build`.
2. Copy `displayxr-service.exe` + `DisplayXRClient.dll` to `C:\Program Files\DisplayXR\Runtime\`.
3. Standalone baseline (1 cube, in-process): `cmd.exe /c "set DISPLAYXR_LOG_PRESENT_NS=1 && cube_handle_d3d11_win.exe"` — expect `[CLIENT_FRAME_NS]` ≈ 16.6 ms (60 fps).
4. Workspace (4 cubes via `displayxr-shell.exe`): same env var, 4 cube args. **Phase 3 success = each per-client `[CLIENT_FRAME_NS]` p50 reaches ~60 fps (or each client's own intended cadence); idle clients drop to 0% GPU; fast + slow clients are independent.**
5. Synthetic 30 Hz + 60 Hz coexistence test for Task 3.2 verification.
6. Idle + active test: one cube, one chrome / no-render workspace controller; the idle path consumes negligible CPU/GPU.
7. **Ask the user to confirm visual smoothness on the Leia SR display.** Screenshot-only verification is insufficient (`feedback_shell_screenshot_reliability.md`).

## Tone

Performance + architecture work. Numbers > opinions. The wait-thread + broadcast-pacing path has been load-bearing since the Monado fork — read it carefully before changing it. Always commit before/after numbers alongside the code.

This is the BIGGEST architectural change in the optimization series. Stay surgical: feature-flag the new pacing path so it's a default-off (or default-on-only-for-fence-capable-clients) variant, soak it with the dev-box scenarios, then flip the default once the soak is clean. Keep WebXR bridge + legacy `_ipc` apps working through every commit.

## Out of scope (don't touch)

- Phase 4 (damage tracking, retire wait-thread cleanup, D3D12 service compositor parity) — that's the next agent.
- macOS / Metal multi-compositor.
- Vulkan service compositor.
- IPC protocol changes that BREAK existing clients (legacy apps, WebXR bridge). Additive only; feature-flag the new path.
- Zero-copy enabling work — Phase 2 left `view_zc_eligible[eye]` deliberately untouched on the fence path. Phase 3 may unblock workspace-mode zero-copy as a side benefit, but treat it as an explicit follow-up not a Phase 3 goal.

## When in doubt

Ask the user. The user has deep context (designed the project, knows the SR SDK internals, has the Leia SR hardware). For any architectural decision not explicitly covered in the plan — especially anything that could affect the WebXR bridge, legacy apps, or the multi-system render loop's existing invariants — ask before committing.
```

---

## How to use this prompt

1. Open a new Claude Code session in the worktree:
   ```
   cd ../openxr-3d-display-shell-opt
   claude
   ```
2. Paste the prompt above (the fenced block).
3. The agent will read context, validate any open questions with PoCs, then start Phase 3.
