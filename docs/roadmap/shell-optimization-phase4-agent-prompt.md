# Phase 4 — Workspace zero-copy — Agent onboarding prompt

Use this prompt to start a new Claude Code session on the `shell/optimization` branch.

> **Branch naming note:** the branch name is `shell/optimization` for git history continuity, but the work is purely **workspace / d3d11_service compositor / runtime**. Per `feedback_no_shell_in_compositor.md`, no new code/comments/log text uses the word `shell`.

---

## Prompt — Phase 4

```
I'm picking up the workspace-optimization work on DisplayXR. **Phases 1, 2, and 3 are shipped** — see `docs/roadmap/shell-optimization-status.md` for what landed and the measured numbers. **You are starting Phase 4.** Do not re-implement earlier phases.

## What Phase 1 + 2 + 3 left in place (read first)

1. **`docs/roadmap/shell-optimization-phase4-handoff.md`** — comprehensive handoff for this work. **Read this first; everything you need is there.**
2. `docs/roadmap/shell-optimization-status.md` — what shipped, before/after numbers per phase. Phase 3 result: per-cube 12 → 39 fps in 4-cube workspace. Phase 3.B lazy-copy was attempted, regressed atlas Present, and was reverted — informative because it proved GPU work (not CPU contention) is the bottleneck.
3. `CLAUDE.md` — project overview, build commands, architecture.

## The mission — non-negotiable

**Per-cube fps in 4-cube workspace must equal desktop standalone fps (60 Hz on dev hardware).** Per `project_workspace_must_equal_desktop_fps.md`, this is a P0 product blocker. A workspace that gives apps half their desktop framerate is useless.

Phase 3 closed 80% of the gap (12 → 39 fps). Phase 4 closes the rest.

**Re-architecting the d3d11_service compositor (per-client compositor lifecycle, multi-client orchestrator, atlas pipeline, render-thread model, swapchain creation flags, cross-device sync model) is in-bounds if it's what gets us there.** The user has explicitly authorized this scope.

## Architectural reality (locked in — do NOT redo)

The original Phase 3 spec told the previous agent to modify `src/xrt/compositor/multi/comp_multi_*`. **That spec was wrong** — workspace mode bypasses `comp_multi` entirely. Each IPC client is a `d3d11_service_compositor` directly, created at `comp_d3d11_service.cpp:11079::system_create_native_compositor`. Multi-client orchestration lives in `d3d11_multi_compositor` inside the same TU. The `comp_multi_*` files have `@note` headers warning about this (commit `b271f73e8`).

**All Phase 4 work is in `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`**, possibly also touching `comp_d3d11_service.h`, the IPC layer (`src/xrt/ipc/`), and the `comp_d3d11_client.cpp` per-client side if swapchain creation flags need to change.

## What Phase 3 + Phase 3.B definitively proved

The Phase 3.B lazy-copy experiment (reverted; bench artifacts in `docs/roadmap/bench/phase3b-lazy-copy_*.csv`):
- Moved per-client `CopySubresourceRegion` from per-client thread → capture thread.
- Atlas Present REGRESSED from 60.6 → 43.5 fps. Per-cube fps barely changed (39 → 40).
- `capture_avg_us` 3 ms → 8.8 ms (because capture thread now does all the Copies).

**Conclusion: GPU work is the bottleneck, not CPU thread contention.** Total per-cycle GPU work is unchanged when you move Copies between threads — it just gets serialized in one place. The 14 ms / 70 Hz capture budget can't fit 4 cubes' worth of `CopySubresourceRegion` plus the existing atlas compose.

**The only path to 60 fps per cube is to eliminate the per-client `CopySubresourceRegion` entirely.** Not relocate it. Eliminate it.

## Phase 4 — what you're building

Detailed design + correctness analysis lives in `docs/roadmap/shell-optimization-phase4-handoff.md`. Summary:

**True workspace zero-copy:** `multi_compositor_render`'s per-view shader-blit reads directly from cube swapchain image SRVs (`sc->images[img_idx].srv`) instead of from the per-client atlas (`cc->render.atlas_srv`). Per-client commit publishes per-view `(sc, img_idx, sub_rect, fence_value, tex_w, tex_h, is_srgb)` and skips both the atlas write AND `CopySubresourceRegion`.

The cube swapchain image SRVs already exist — they're created at `comp_d3d11_service.cpp:3018` and `:3141`, with corresponding `keyed_mutex` at `:2962` and `:3126`. No new GPU resources needed.

## Critical correctness pitfalls (read before designing)

These are the reasons this work isn't trivial. Internalize them BEFORE coding.

1. **Cross-device GPU sync race.** Capture thread queues `[Wait(fence, V), Draw]` on `sys->context`. Cube renders on cube's GPU context. Both target shared swapchain texture. After capture's `ReleaseSync`, cube can start writing the next frame; capture's queued `Draw` runs at GPU exec time, possibly while cube is mid-write. The `Wait(fence, V)` waits for fence ≥ V (cube's frame V finished); if cube has signaled higher values, Wait passes and Draw reads CURRENT (possibly mid-write) texture state. **Phase 2 lives with this in microseconds; Phase 4 widens it to ~14 ms** (capture cycle).

2. **Swapchain buffer-cycle race.** If cube swapchain has `image_count == 2`, cube cycles back to buffer N within ~33 ms. Capture's read of N might catch cube mid-write of N's next iteration. **Investigate cube swapchain `image_count`** as part of design — may need to require ≥ 3.

3. **Per-view SRV binding.** Today, `multi_compositor_render` chooses one `slot_srv` per client (line 7686) and uses it for all views via `src_rect` offsets. Phase 4 needs per-view SRV bind inside the loop at line 7780+. Per-view SRGB-vs-UNORM SRV selection (currently flat per-client at line 7722).

4. **`feedback_atlas_stride_invariant`.** The legacy non-zero-copy path's stride invariant becomes moot for the zero-copy path (no atlas to derive stride from; `sub_rect` is explicit). But the legacy path stays load-bearing. Don't break it.

## Open questions to resolve in design phase

Per `feedback_decisive_under_arch_uncertainty.md`: **time-box your analysis (15 min per question) and surface options to me with trade-offs.** Do not loop trying to find a safe path alone. The user (me) has context on Leia SR vendor SDK quirks, hardware behavior, and tolerances that you can't infer from code.

1. **How to handle the cross-device GPU sync race?**
   - Option A: ship with the same widened race trade-off Phase 2 has; bench, ask me to eyeball Leia SR for tearing
   - Option B: capture thread holds `keyed_mutex` during the GPU Draw (CPU blocks on GPU fence to be safe — slower but spec-compliant)
   - Option C: re-architect cube swapchain to NOT use `SHARED_KEYEDMUTEX` — pure-fence cross-device sync. Bigger surgery (touches IPC + client compositor), cleanest answer.

2. **Cube swapchain `image_count`.** What's the current default? Is it ≥ 3? Force ≥ 3 in workspace mode?

3. **`workspace_sync_fence` direction.** Today fence is signaled by cube on `xrEndFrame` and waited on by service. For Phase 4, do we also need a service→cube fence so cube knows when capture is done reading? (Answer probably no for the same Phase 2 reason — it works in practice — but think about whether buffer-cycle race needs it.)

4. **Fallback path for non-fence-capable clients (legacy `_ipc`, WebXR bridge).** They must keep working through the legacy `CopySubresourceRegion` path. Easy: gate Phase 4 zero-copy on `c->workspace_sync_fence != nullptr`. Same gate Phase 2 uses.

## Acceptance criteria (Phase 4)

* Per-cube `[CLIENT_FRAME_NS]` p50 ≤ 18 ms → ≥ 55 fps per cube. Target 60 fps; floor 55.
* Atlas `[PRESENT_NS] client=workspace` stays at 60 Hz.
* `[ZCOPY]` diagnostic (add it, mirroring `[FENCE]`/`[MUTEX]` pattern, 1×/10 s) reports zero-copy taken on the steady-state path for cube_handle clients.
* No regressions on legacy / WebXR bridge / `cube_hosted_legacy_d3d11_win`.
* Phase 1 + 2 + 3 acceptance criteria still hold (`[MUTEX] timeouts=0`, `[FENCE] stale_views=0`, `[RENDER] wait_avg_us` low, `client_renders` = 0).
* User confirms visual smoothness on the live Leia SR display per `feedback_shell_screenshot_reliability.md`.
* Bench numbers committed to `docs/roadmap/shell-optimization-status.md` Phase 4 section.

## Tasks (track in shell-optimization-status.md)

The Phase 1 + 2 + 3 sections are done. Add a Phase 4 section with the same checklist style:

- [ ] Read `docs/roadmap/shell-optimization-phase4-handoff.md` end-to-end. Verify file:line citations against current code.
- [ ] Read memory files: `project_workspace_must_equal_desktop_fps.md`, `feedback_decisive_under_arch_uncertainty.md`, `feedback_atlas_stride_invariant.md`, `feedback_srgb_blit_paths.md`, `feedback_no_shell_in_compositor.md`, `project_webxr_bridge_v2_phase3.md`.
- [ ] Investigate cube swapchain `image_count` default. Decide whether to force ≥ 3 in workspace mode.
- [ ] Resolve the cross-device GPU sync race question (use AskUserQuestion with concrete options + trade-offs after 15 min analysis budget).
- [ ] Design per-view publish struct, surface to user with concrete `d3d11_service_compositor` field additions.
- [ ] Implement publish struct + `compositor_layer_commit` changes (skip Copy, skip atlas write, publish state).
- [ ] Implement `multi_compositor_render` per-view restructure (per-view SRV bind, per-view AcquireSync/Wait/ReleaseSync, per-view src_rect/src_size from publish data).
- [ ] Add `[ZCOPY]` diagnostic family. Extend `scripts/bench_shell_present.ps1` parser.
- [ ] Build via `scripts\build_windows.bat build` and copy binaries to `C:\Program Files\DisplayXR\Runtime\` (`feedback_dll_version_mismatch.md`).
- [ ] Bench 4-cube workspace; capture before/after numbers.
- [ ] Bench legacy `cube_hosted_legacy_d3d11_win` to confirm no regression.
- [ ] Ask user to eyeball Leia SR for tearing / visual artifacts.
- [ ] Iterate on correctness if visual issues.
- [ ] Commit (don't push) once bench + visual confirm.
- [ ] Open Phase 4 PR back to `main` ONLY after user confirms.

DO NOT bundle other cleanup (delete comp_multi, damage tracking, D3D12 service compositor parity) into the same PR.

## Compositor terminology — IMPORTANT

`feedback_no_shell_in_compositor.md`:
- Compositor / runtime code uses `workspace` / `container app`. **Never `shell`** in identifiers, comments, or in the text of emitted log lines.
- Before each commit, grep your diff for `shell` (case-insensitive); rename or delete.

## How to verify your changes

1. Build via `scripts\build_windows.bat build`.
2. Copy `displayxr-service.exe` + `DisplayXRClient.dll` to `C:\Program Files\DisplayXR\Runtime\`.
3. Standalone baseline (1 cube, in-process): `cmd.exe /c "set DISPLAYXR_LOG_PRESENT_NS=1 && cube_handle_d3d11_win.exe"` — expect `[CLIENT_FRAME_NS]` ≈ 16.6 ms (60 fps).
4. Workspace 4-cube bench:
   ```powershell
   .\scripts\bench_shell_present.ps1 -Tag phase4-zero-copy -Seconds 30 `
     -App test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe, `
          test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe, `
          test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe, `
          test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
   ```
   Phase 4 success = each per-client `[CLIENT_FRAME_NS]` p50 ≤ 18 ms (≥ 55 fps); atlas `[PRESENT_NS] client=workspace` ≈ 16.6 ms (60 Hz); `[ZCOPY]` shows zero-copy in steady state.
5. Regression test: `cube_hosted_legacy_d3d11_win` and the WebXR bridge demo. Both should still work — they fall back to the legacy `CopySubresourceRegion` path (no `workspace_sync_fence`).
6. **Ask the user to confirm visual smoothness on the Leia SR display** before pushing. Screenshot-only is insufficient (`feedback_shell_screenshot_reliability.md`).

## Tone

P0 product blocker. Numbers > opinions. The cross-device GPU sync race is the real risk; analyze it, surface options, decide with the user, then ship. Per `feedback_decisive_under_arch_uncertainty.md`, time-box your analysis loops and consult the user when uncertain — they have context you don't.

## Out of scope (don't touch)

- Removing `comp_multi_*` files. They have `@note` headers warning they're Monado-legacy. Full removal needs `oxr_session.c` + `null_compositor.c` + `sdl_test/sdl_compositor.c` rework. Separate cleanup PR.
- macOS / Metal / Vulkan / D3D12 service compositors. Not relevant to workspace mode today.
- Damage tracking. Save for after Phase 4 lands.
- IPC protocol changes that break legacy clients. Additive only; gate Phase 4 zero-copy on `workspace_sync_fence` presence (same gate Phase 2 uses for fence-capable clients).

## When in doubt

Ask the user. They designed the project, know the SR SDK internals, have the Leia SR hardware. For any architectural decision not explicitly covered here — especially anything that could affect the WebXR bridge, legacy apps, or cross-device GPU sync semantics — ask before committing.
```

---

## How to use this prompt

1. Open a new Claude Code session in the worktree:
   ```
   cd ../openxr-3d-display-shell-opt
   claude
   ```
2. Paste the prompt above (the fenced block).
3. The agent will read context, investigate the open questions with you, then start Phase 4.
