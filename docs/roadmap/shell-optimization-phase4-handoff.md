# Phase 4 — Workspace zero-copy — Handoff

**Branch:** `shell/optimization`
**Date:** 2026-05-03
**Status:** Phase 3 shipped (commits `b271f73e8` + `b32fd91bc`). Phase 4 not started.
**Goal:** per-cube fps in 4-cube workspace = desktop standalone fps (60 Hz). **P0 product blocker** per `project_workspace_must_equal_desktop_fps.md` — workspace mode is useless below this bar.

This doc is the technical handoff for the next agent. The companion onboarding prompt is `shell-optimization-phase4-agent-prompt.md`.

---

## What's locked in (don't re-derive)

### Architectural reality (re-verified, not from spec docs)

- The original Phase 3 spec (`shell-optimization-plan.md` lines 158–189) cited `src/xrt/compositor/multi/comp_multi_*` for the per-client pacing surgery. **This is wrong for DisplayXR.** Workspace mode bypasses `comp_multi` entirely.
- Each IPC client in workspace mode is a `d3d11_service_compositor` directly (`comp_d3d11_service.cpp:11079::system_create_native_compositor` does `new d3d11_service_compositor()`).
- Multi-client orchestration lives in `d3d11_multi_compositor` inside the same TU (`comp_d3d11_service.cpp:868`).
- `comp_multi/` is reachable only via `null_compositor.c` (testing) and `sdl_test/sdl_compositor.c` (dev harness). The four `comp_multi_*` files now carry `@note` headers warning about this (commit `b271f73e8`).
- Phase 3's surgery happened in `comp_d3d11_service.cpp`, not `comp_multi_*`.

### Phase 3 result (benched, committed)

| metric | pre-Phase-3 | post-Phase-3 | delta |
|---|---|---|---|
| per-cube p50 | 82.2 ms (12.2 fps) | 25.5 ms (**39.2 fps**) | **3.2×** |
| atlas Present p50 | 16.7 ms (60.0 fps) | 16.5 ms (60.6 fps) | unchanged ✓ |
| `wait_avg_us` | 24,894 µs | 4 µs | 6,224× |
| `client_renders`/sec | 35 | 0 | capture is sole driver ✓ |

Surgery: per-client commits no longer drive `multi_compositor_render`; `capture_render_thread_func` is the sole render driver. `[RENDER]` diagnostic family added.

### Phase 3.B lazy-copy attempt (benched, REVERTED — informative)

Hypothesis: per-client D3D11 immediate-context API contention is the remaining bottleneck. Move the per-client `CopySubresourceRegion` from per-client thread → capture thread (per-client commit publishes copy params, capture issues the Copy).

| metric | Phase 3 | Phase 3.B lazy-copy | verdict |
|---|---|---|---|
| atlas Present p50 | 60.6 fps | **43.5 fps** | regressed |
| per-cube p50 | 39.2 fps | 40.6 fps | within noise |
| `capture_avg_us` | 2,921 µs | 8,868 µs | 3× longer |
| `capture_renders`/sec | 59.5 | 44.4 | dropped 25% |

Bench artifacts: `docs/roadmap/bench/phase3b-lazy-copy_*.csv`.

**What this proves: GPU work is the bottleneck, not CPU thread contention.** Total per-cycle GPU work is unchanged when you move Copies between threads; the work just serializes in one place. Capture cycle ballooned past the 14 ms / 70 Hz budget, dragging atlas Present below 60 Hz.

**Conclusion: closing the 60 fps gap requires eliminating the per-client `CopySubresourceRegion` entirely.** Relocating it isn't enough.

---

## Phase 4 — proposed design (untried)

### The core change

Today (post-Phase-3):
- Per-client `compositor_layer_commit` (per IPC thread) does per-view `CopySubresourceRegion` from cube swapchain image → per-client atlas (`cc->render.atlas_texture`).
- `multi_compositor_render` (capture thread) reads per-client atlas SRV (`cc->render.atlas_srv` at `comp_d3d11_service.cpp:7722-7725`) and shader-blits into the combined atlas.

Phase 4:
- Per-client commit publishes per-view `(sc, img_idx, sub_rect, fence_value, tex_w, tex_h, is_srgb)` to a struct on `d3d11_service_compositor`. **Skips the `CopySubresourceRegion` entirely.** No per-client atlas write.
- `multi_compositor_render`'s per-view loop (around `comp_d3d11_service.cpp:7780+`) binds the **cube swapchain image SRV directly** (`sc->images[img_idx].srv`) instead of `cc->render.atlas_srv`. Per-view `AcquireSync` + `Wait(fence)` + Draw + `ReleaseSync` on capture thread.
- Per-client atlas (`cc->render.atlas_texture` etc.) becomes unused in workspace mode. Leave allocated to keep standalone-mode unaffected; clean up in a follow-up if desired.

### Why this should hit 60 fps

Per-cube `xrEndFrame` becomes:
- ~0.5 ms publish state under `c->mutex`
- Return

Capture-thread cycle becomes:
- Existing 3 ms compose work
- Per-view: AcquireSync (CPU CAS) + queue Wait+Draw + ReleaseSync (CPU CAS) — same Draw count as today, no `CopySubresourceRegion`
- Net: similar capture cycle to current 3 ms (maybe 4-5 ms with extra Acquire/Release overhead)

Total GPU work drops by ~4 cubes × 2 views × 1 `CopySubresourceRegion` per cycle. That's the bottleneck the lazy-copy experiment couldn't relocate away.

---

## Critical correctness considerations

### Cross-device GPU sync race

**The big one.** Capture thread queues `[Wait(fence, V), Draw]` on `sys->context`. Cube renders to its OWN GPU context. Both target the shared swapchain texture.

- After capture's `ReleaseSync`, cube can `AcquireSync` and start writing the next frame on cube's GPU context.
- Capture's queued `Draw` runs at GPU exec time, possibly while cube is mid-write.
- The `Wait(fence, V)` only waits for fence ≥ V (cube finished frame V). If cube has signaled higher values since, Wait passes immediately and Draw reads CURRENT texture state — possibly mid-write.

**Phase 2 lives with this race in microseconds.** Phase 4 widens it to the capture cycle (~14 ms).

### Swapchain buffer-cycle race

If cube swapchain has `image_count == 2` (typical Monado default):
- Cube writes buffer N at time T, signals fence
- Cube writes buffer N+1 at T+16.6 ms (next vsync)
- Cube cycles back to buffer N at T+33 ms
- Capture reads buffer N at some point in between

If capture's read of N happens at T+33 ms (cube starts writing N again), torn content.

**Mitigations to consider:**
- Force `image_count >= 3` for workspace-mode cube swapchains (gives capture 33+ ms safe window per buffer).
- Have capture thread hold the per-image `keyed_mutex` during the GPU Draw. Requires CPU-block-on-GPU-fence for proper safety; may be slow.
- Use the `workspace_sync_fence` as the cross-device sync, drop `SHARED_KEYEDMUTEX` from cube swapchain creation — pure-fence model. This is the cleanest answer but requires changes to swapchain creation flags + IPC client compositor.

**The user has authorized service-compositor re-architecture if needed.** A pure-fence cross-device sync model is in scope.

### Per-view SRV binding inside the shader-blit loop

Today (`comp_d3d11_service.cpp:7686`): one `slot_srv` chosen per client, used for all views via `src_rect` offsets within the per-client atlas (which has views tiled at known stride).

Phase 4: each view has its OWN cube swapchain image SRV. Different swapchain per eye is possible (`view_scs[eye]` in `compositor_layer_commit:10067`). The shader-blit loop at line 7780+ needs to:
- `PSSetShaderResources` per view (currently bound once per client at line 8070)
- `src_rect` from published `sub_rect` per view
- `src_size` from published `tex_w, tex_h` per view
- SRGB-vs-UNORM SRV selection per view (currently flat per-client at line 7722)

### `feedback_atlas_stride_invariant`

The existing per-client atlas uses `slot_w = atlas_w / tile_columns` as the stride across both eyes. Phase 4 reads from cube swapchain images directly. The stride concept doesn't apply to cube swapchains the same way — each view's `sub_rect` is explicit per `layer->data.proj.v[eye].sub.rect` and there's no atlas to derive stride from. **This invariant becomes moot for the zero-copy path** but stays load-bearing for the legacy non-zero-copy path. Don't accidentally break the legacy path while restructuring.

### Format / SRGB matrix

Per `feedback_srgb_blit_paths`:
- Non-workspace SRGB: shader blit linearizes for DP
- Workspace: raw copy keeps gamma-encoded; multi_compositor_render's `convert_srgb=0` shader passthrough handles it

For Phase 4, the cube swapchain SRV's format needs SRGB-vs-UNORM selection per view. The existing logic at `comp_d3d11_service.cpp:10082` (`view_is_srgb[eye] = is_srgb_format(...)`) is per-view in compositor_layer_commit; carry that into the publish struct.

---

## File:line entry points

### Critical files (read these first)

| Path | Why |
|---|---|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:587-609` | `d3d11_service_system` struct — render_mutex, last_workspace_render_ns, render_diag_* atomics from Phase 3 |
| `comp_d3d11_service.cpp:253-366` | `d3d11_service_compositor` struct — `c->mutex`, `c->render.atlas_*`, `workspace_sync_fence`, `last_signaled_fence_value`. Add Phase 4 publish struct here. |
| `comp_d3d11_service.cpp:143-...` | `d3d11_service_swapchain` struct — `images[i].texture`, `images[i].srv`, `images[i].keyed_mutex`. **All needed resources already exist.** |
| `comp_d3d11_service.cpp:9421+` | `compositor_layer_commit` — per-client commit handler. Workspace fast-path (Phase 3) at lines 10697-10726. Per-view tile-blit at lines 9966-10577. |
| `comp_d3d11_service.cpp:6094+` | `multi_compositor_render` — capture thread's combined-atlas composition. Per-client iteration at 7682+. Per-view shader-blit loop at 7780+. SRV bind at 8070. |
| `comp_d3d11_service.cpp:4429-4452` | `capture_render_thread_func` — sole render driver post-Phase-3 |

### Diagnostic taxonomy (already in place — extend, don't replace)

All emit `U_LOG_W`, rate-limited per spec:

| Tag | Emitted by | Period | What it measures |
|---|---|---|---|
| `[CLIENT_FRAME_NS]` | `compositor_layer_commit` line 9442 | per-frame, env-gated `DISPLAYXR_LOG_PRESENT_NS=1` | per-client xrEndFrame interval |
| `[PRESENT_NS]` | `multi_compositor_render` ~9395 + standalone Present ~10900 | per-frame, env-gated | per-Present interval (`client=workspace` for atlas) |
| `[ZC]` | `compositor_layer_commit` ~10396 | one-shot per decision flip | per-client zero-copy eligibility |
| `[MUTEX]` | `compositor_layer_commit` ~10246 | 1×/10 s window | per-client legacy KeyedMutex stats (Phase 1) |
| `[FENCE]` | `compositor_layer_commit` ~10272 | 1×/10 s window | per-client GPU-fence stats (Phase 2) |
| `[RENDER]` | `emit_render_diag_if_window_elapsed` ~4429 | 1×/10 s window | system-wide capture/client render stats (Phase 3) |

Phase 4 should add a `[ZCOPY]` family (1×/10 s) measuring how many views took the zero-copy path vs fallback, per client.

### Bench harness (`scripts/bench_shell_present.ps1`)

Already parses all six diagnostic families above. Default scenario is 2 cubes; pass `-App x4` for 4 cubes. Output goes to `docs/roadmap/bench/<tag>_{raw,summary}.csv`. Commit alongside Phase 4 status.

Reference invocation:
```powershell
.\scripts\bench_shell_present.ps1 -Tag phase4-zero-copy -Seconds 30 `
  -App test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe, `
       test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe, `
       test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe, `
       test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

### Build + deploy

Per `feedback_use_build_windows_bat.md` and `feedback_dll_version_mismatch.md`:
```powershell
scripts\build_windows.bat build
Copy-Item _package\bin\displayxr-service.exe "C:\Program Files\DisplayXR\Runtime\" -Force
Copy-Item _package\bin\DisplayXRClient.dll  "C:\Program Files\DisplayXR\Runtime\" -Force
```

---

## Acceptance criteria for Phase 4

* **per-cube `[CLIENT_FRAME_NS]` p50 ≥ 55 ms-per-frame-equiv (i.e. ≥ 18 ms) → ≥ 55 fps in 4-cube workspace.** Target 60 fps; floor 55 fps.
* Atlas `[PRESENT_NS] client=workspace` stays at 60 Hz vsync.
* `[ZCOPY]` reports zero-copy taken on the steady-state path for cube_handle clients (no fallback to legacy `CopySubresourceRegion`).
* No regressions on legacy / WebXR bridge / `cube_hosted_legacy_d3d11_win`.
* Phase 1+2+3 acceptance criteria still hold (`[MUTEX] timeouts=0`, `[FENCE] stale_views` low, `wait_avg_us` low).
* **User confirms visual smoothness on the live Leia SR display** per `feedback_shell_screenshot_reliability.md` — screenshots are insufficient.
* Bench numbers committed to `docs/roadmap/shell-optimization-status.md` Phase 4 section.

---

## Constraints to honor

- **Compositor terminology** (`feedback_no_shell_in_compositor.md`): never `shell` in identifiers / comments / log text. Use `workspace` / `container app`.
- **Test before CI** (`feedback_test_before_ci.md`): wait for user to bench locally before pushing.
- **PR workflow** (`feedback_pr_workflow.md`): user reviews and merges. Don't auto-merge.
- **Branch workflow**: stay on `shell/optimization`; ask before creating new branches.
- **Build via build_windows.bat** (`feedback_use_build_windows_bat.md`): never call cmake/ninja directly.
- **Deploy to Program Files** (`feedback_dll_version_mismatch.md`): copy `displayxr-service.exe` + `DisplayXRClient.dll` after each rebuild.
- **WebXR bridge dependency** (`project_webxr_bridge_v2_phase3.md`): bridge sessions go through d3d11_service in non-workspace mode by default. The `workspace_sync_fence` gate already correctly excludes bridge clients (no fence → falls back to legacy path). Phase 4 must preserve this.
- **Workspace controllers own motion** (`feedback_controllers_own_motion.md`): runtime is plumbing only.

---

## What's NOT in scope for Phase 4 (yet)

- macOS / Metal / Vulkan / D3D12 multi-client compositors. They don't exist as service compositors today.
- Removing `comp_multi_*` files. They have doc-headers warning they're Monado-legacy; full removal is a separate cleanup that needs `oxr_session.c` / `null_compositor.c` / `sdl_test/sdl_compositor.c` rework. Phase 4 should leave them alone.
- Damage tracking. Save for after Phase 4 lands.

---

## Memory references the next session should re-read

Before designing, read these in `~/.claude/projects/.../memory/`:
- `project_workspace_must_equal_desktop_fps.md` — the directive (P0 status; re-arch in scope)
- `feedback_decisive_under_arch_uncertainty.md` — don't loop in race analysis; surface options at 15-min budget
- `feedback_atlas_stride_invariant.md` — the legacy invariant; Phase 4 makes it moot for the new path but don't break legacy
- `feedback_srgb_blit_paths.md` — workspace = raw copy, non-workspace = shader linearize
- `feedback_no_shell_in_compositor.md` — terminology
- `project_webxr_bridge_v2_phase3.md` — bridge architecture; don't break it
- `feedback_dll_version_mismatch.md` — deployment
- `feedback_use_build_windows_bat.md` — build
- `feedback_test_before_ci.md` — gate on user testing
- `reference_runtime_screenshot.md` — file-trigger atlas screenshot for visual debugging
- `reference_service_log_diagnostics.md` — log conventions

---

## A reasonable Phase 4 task ladder

1. **Re-read this handoff + memory references.** Verify file:line citations haven't drifted.
2. **Design pass:** sketch the per-view publish struct, the multi_compositor_render per-view loop changes, and the `[ZCOPY]` diagnostic. Surface design to user before coding (per `feedback_decisive_under_arch_uncertainty.md`).
3. **Investigate the cross-device GPU sync race carefully.** Read Phase 2's actual race pattern in `compositor_layer_commit:10171-10196`. Understand the trade-off the codebase already accepts. Decide whether Phase 4 widens it acceptably or needs a stronger sync model (pure-fence, drop SHARED_KEYEDMUTEX).
4. **Investigate cube swapchain `image_count`.** Grep for swapchain creation, log it, decide whether to require ≥ 3.
5. **Implement publish struct + compositor_layer_commit changes** (skip Copy, publish state).
6. **Implement multi_compositor_render per-view restructure** (per-view SRV bind, AcquireSync, Wait, Draw, ReleaseSync, sub_rect/dims from publish data).
7. **Add `[ZCOPY]` diagnostic.** Bench parser extension.
8. **Build, deploy, bench, ask user to eyeball Leia SR.**
9. **Iterate on correctness if visual artifacts.**
10. **Commit when bench numbers + visual confirm 60 fps per cube + correct rendering.**

This is realistically a multi-hour focused session. Time-box analysis per `feedback_decisive_under_arch_uncertainty.md`; surface to user when stuck.
