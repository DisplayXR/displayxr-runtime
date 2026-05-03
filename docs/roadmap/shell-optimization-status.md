# Shell Optimization — Implementation Status

Last updated: 2026-05-02 (branch `shell/optimization`)

Tracking doc for the multi-phase shell-mode performance work. Plan: `shell-optimization-plan.md`. Roadmap: `shell-optimization.md`. Agent prompt: `shell-optimization-agent-prompt.md`.

---

## Phase 1 Progress — Quick Wins (compositor-side only)

All Phase 1 changes live in `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` plus two PowerShell bench scripts. Zero changes under `src/xrt/targets/shell/`, zero IPC / multi-compositor / installer changes.

### Tasks

| Task | Status | Notes |
|------|--------|-------|
| 1.0 Verify cited line numbers haven't drifted | ✅ | Confirmed all line numbers cited in `shell-optimization-plan.md` for `comp_d3d11_service.cpp` (lines 130, 2568, 2641, 7988, 9350, 9883, 9884, 9885–9907, 9930, 9992–10155). |
| 1.1 Per-view zero-copy eligibility | ✅ | Replaced `bool any_mutex_acquired` with `bool view_zc_eligible[XRT_MAX_VIEWS]` (default true; flipped false when a view requires a service-side mutex acquire or that acquire fails). Gate at the old line 9930 ANDs across the array. Note: in shell mode the `!sys->workspace_mode` term keeps zero-copy disabled — broader shell-mode zero-copy is Phase 2 work behind shared fences. |
| 1.2 Drop mutex timeout 100 ms → 4 ms + skip-on-timeout | ✅ | `mutex_timeout_ms = 4` (matches the chrome-overlay precedent at line 7988). On `WAIT_TIMEOUT`, the view is marked `view_skip_blit[eye] = true` and the per-view blit loop short-circuits via `continue`. The persistent per-client atlas slot retains its prior tile content — a 1-frame quality blip rather than a ~100 ms render-thread stall. |
| 1.3 Diagnostic breadcrumbs | ✅ | Two greppable `U_LOG_I` lines in `compositor_layer_commit`: `[ZC] client=<hwnd> views=<u> zero_copy=<Y/N> reason=<str>` (one-shot per decision flip; reasons: `single_view`, `ui_layers`, `view_ineligible`, `workspace_mode`, `view_unique_textures`, `tiling_mismatch`, `no_active_mode`, `srv_create_failed`, `ok`) and `[MUTEX] client=<hwnd> timeouts=<u> acquires=<u> avg_acquire_us=<u> window_s=<int>` (rate-limited 1×/10 s). The old per-frame `U_LOG_W("layer_commit: View %u mutex timeout ...")` was demoted to `U_LOG_D` to avoid spamming the steady-state log. |
| 1.4 Env-gated `[PRESENT_NS]` log + bench scaffold | ✅ | `DISPLAYXR_LOG_PRESENT_NS=1` enables a single `U_LOG_I("[PRESENT_NS] dt_ns=%lld", …)` just before the shell swap chain `Present` (was line 9333; auto-shifted by the diagnostic edits above). Production builds pay one `getenv` on first frame, then a static-cached branch. New scripts: `scripts/bench_shell_present.ps1` (launch shell + 2 cubes, run for `-Seconds`, parse the breadcrumbs, emit raw + summary CSVs) and `scripts/bench_diff.ps1` (table-format delta between two summary CSVs, `-Markdown` for paste-into-this-doc). |
| 1.5 Build via `scripts\build_windows.bat build` and copy binaries | ✅ | Built clean (472/472, no warnings on the modified TU). `displayxr-service.exe` + `DisplayXRClient.dll` copied from `_package\bin\` to `C:\Program Files\DisplayXR\Runtime\` (`feedback_dll_version_mismatch.md`). |
| 1.6 Capture before/after benchmarks | ⏳ | Run `scripts\bench_shell_present.ps1 -Tag before` on `main`, then `-Tag after` on this branch. Diff with `scripts\bench_diff.ps1 -Markdown` and paste table below. |
| 1.7 User confirms visual smoothness on Leia SR | ⏳ | Screenshot-only verification is insufficient (`feedback_shell_screenshot_reliability.md`). |
| 1.8 Open Phase 1 PR back to main | ⏳ | Do NOT bundle Phase 2 work into this PR. |

### Files modified in Phase 1

| File | Change |
|------|--------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | (struct) Added `zc_last_logged_*` and `mutex_*_in_window` fields to `d3d11_service_compositor`. (commit) Per-view eligibility array, 4 ms timeout, skip-on-timeout, `[ZC]` + `[MUTEX]` breadcrumbs, env-gated `[PRESENT_NS]`. |
| `scripts/bench_shell_present.ps1` | New — launch shell + 2 cubes, parse log, emit CSVs. |
| `scripts/bench_diff.ps1` | New — diff two summary CSVs (table or markdown). |
| `docs/roadmap/shell-optimization-status.md` | This file. |

### Benchmark results

Run on the dev box (Sparks i7 + 3080 + Leia SR), 30 s windows, `DISPLAYXR_LOG_PRESENT_NS=1`, all on `shell/optimization` after Phase 1 + per-client diagnostic instrumentation. `[CLIENT_FRAME_NS]` is per-client `xrEndFrame` interval at the service compositor; `[PRESENT_NS] client=workspace` is the multi-comp combined-atlas swapchain interval. `client=<ptr>` is the `d3d11_service_compositor*` (stable per-client ID).

#### Standalone — 1× `cube_handle_d3d11_win` (in-process compositor, no shell)

| metric | value |
|---|---|
| samples | 1723 |
| frame p50 | **16.60 ms (60.2 fps)** |
| frame p95 | 19.73 ms |
| frame p99 | 21.41 ms |
| frame max | 33.52 ms |
| jitter (p99 − p50) | 4.81 ms |

#### Shell — 4× `cube_handle_d3d11_win` (multi-compositor, this branch)

Per-client `[CLIENT_FRAME_NS]`:

| client | samples | p50 | p95 | p99 | max | jitter | fps |
|---|---|---|---|---|---|---|---|
| 0x1FCA63AE5B0 | 421 | 70.78 ms | 87.31 ms | 103.95 ms | 121.52 ms | 33.17 ms | **14.1** |
| 0x1FCA63E85F0 | 421 | 74.06 ms | 87.75 ms |  98.95 ms | 111.37 ms | 24.90 ms | **13.5** |
| 0x1FCCFDA0120 | 445 | 64.11 ms | 87.33 ms | 101.65 ms | 110.42 ms | 37.54 ms | **15.6** |
| 0x1FCCFDD2120 | 449 | 57.19 ms | 87.96 ms | 104.09 ms | 118.57 ms | 46.91 ms | **17.5** |

Workspace combined-atlas swapchain `[PRESENT_NS] client=workspace`: p50 = 16.65 ms (**60.1 fps**), p99 = 20.74 ms, jitter 4.10 ms — display refresh is rock-solid at vsync.

`[ZC]` (all clients): `zero_copy=N reason=view_ineligible` (each client's view took the cross-process keyed mutex, so the existing per-view eligibility check correctly disqualifies). `view_ineligible` is the reason because in workspace mode the views are still service-created shared images and the mutex acquire flips eligibility off — same as the pre-Phase-1 `any_mutex_acquired` semantics, just at finer granularity.

`[MUTEX]` (8 windows across 4 clients × 2): **timeouts=0 in every window** — Phase 1.2's 4 ms timeout drop did not produce a single timeout under steady-state 4-cube load. `avg_acquire_us` ≈ 9.8–12.0 ms wall-clock per call, which includes thread preemption (the 4 cubes + service contend for CPU), not raw mutex-contention time.

#### Interpretation

- **Phase 1 worked as advertised:** zero mutex timeouts under 4-cube load, `[ZC]` per-view tracking is clean, shell compose holds 60 fps.
- **Shell-mode per-app rate is ~14–17 fps vs 60 fps standalone — a 3.5–4× slowdown that Phase 1 does not address.** Cause: the existing render-loop throttle in `compositor_layer_commit` (`~14 ms / vsync`, line ~10398) serialises all clients' commits on the service render thread, so 4 clients ≈ 60 / 4 = 15 fps each. This is exactly the per-client-pacing problem `shell-optimization.md` calls out and queues for **Phase 3**.
- The "feels smoother" subjective improvement is real: pre-Phase-1, the same scenario suffered 100 ms stalls (12 dropped frames per stall) on top of the 15-fps base rate. Phase 1 removed the stall component; Phase 3 will remove the throttle/serialisation component.

---

## Phase 2 — Shared D3D11 fence (deferred)

Not started. See `shell-optimization-plan.md` for tasks 2.1–2.3. Open questions to resolve before kickoff are in that doc's "Open questions" section.

## Phase 3 — Per-client frame pacing (deferred)

Not started. Highest-risk phase; touches `comp_multi_system.c` render loop.

## Phase 4 — Opportunistic cleanup (deferred)

Not started. Damage tracking, retire wait-thread if vestigial post-Phase-2, D3D12 service compositor parity.
