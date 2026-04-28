# Kickoff prompt for next session — Issue #184 architectural fix

> **Paste this whole document into the next session's first prompt.** It is self-contained.

You're picking up issue #184 (Unity SRGB clients render black in shell). The diagnostic phase is **complete** — 13 cycles documented. The bug is fully understood and the fix is designed but not yet implemented. Your job this session is to **implement the architectural fix** in the runtime IPC client compositors (D3D11 + D3D12), verify with the existing diagnostic infrastructure, then strip diagnostics for a shippable PR.

## Read first, in this order

1. `HANDOFF_184.md` — start at the very top "Executive summary" + "Recommended next move" sections (~3 min). The 13 detailed cycles below are evidence; skip unless you need to back-check a finding.
2. `src/xrt/compositor/client/comp_d3d11_client.cpp` — the IPC client compositor where the fix lands. Specifically:
   - `import_image` (line ~358) and `import_image_dxgi` (line ~391) — current code that opens shared NT handles via `OpenSharedResource1`. The textures returned here are what currently go to Unity.
   - `client_d3d11_create_swapchain` (line ~481) — where `data->app_images` gets populated and `sc->base.images[i]` exposes pointers to the app.
   - `client_d3d11_swapchain_data` struct (line ~205, in the upper anon header section) — where `app_images` lives.
3. `src/xrt/compositor/client/comp_d3d12_client.cpp` — the parallel D3D12 client. Same fix, parallel implementation.

## Branch state

- Runtime: `fix/184-shell-srgb-atlas-regression`. **Nothing committed beyond `cedeac421`** (initial handoff stub) and `a72076768` (unrelated CLAUDE.md doc fix). Diagnostic-only `[#184]` patches in working tree, build green.
- Plugin: `unity-3d-display`, branch unmodified main. 8 PROBEs + Cycle 12 substitution-skip in working tree. Build green via `native~/build-win.bat`. DLL deployed to `Documents/Unity/DisplayXR-test-Unity/Test-D3D11/...` and `.../DisplayXR-test/Build/...`.

## What the fix is (option 2 from handoff Cycle 12 + 13)

**Problem:** Unity's render pipeline silently no-ops writes to imported cross-process `SHARED_KEYEDMUTEX` (D3D11) / `SHARED_HEAP+Fence` (D3D12) swapchain textures. Standalone works because there's no cross-process texture handoff.

**Fix:** the runtime IPC client interposes a *non-shared* "Unity-private" texture per swapchain image. The app sees only the private texture. Inside `xrReleaseSwapchainImage`, the runtime IPC client does a `CopyResource` from the private texture into the existing shared texture, on a context the runtime fully controls. The service compositor reads from the shared texture as before — but now it sees Unity's content because the runtime's controlled copy lands reliably.

### D3D11 implementation outline

In `src/xrt/compositor/client/comp_d3d11_client.cpp`:

1. **Extend `client_d3d11_swapchain_data`** (around line 205) with a parallel array:
   ```cpp
   std::vector<wil::com_ptr<ID3D11Texture2D1>> private_images;  // [#184] Unity-private, non-shared
   ```

2. **In `client_d3d11_create_swapchain`** (around line 481), after `data->app_images` is populated by the import loop (~line 559), allocate parallel non-shared textures using the same `app_device`:
   ```cpp
   for (size_t i = 0; i < data->app_images.size(); ++i) {
       D3D11_TEXTURE2D_DESC d = {};
       data->app_images[i]->GetDesc(&d);
       d.MiscFlags = 0;  // strip SHARED_KEYEDMUTEX | SHARED_NTHANDLE
       wil::com_ptr<ID3D11Texture2D1> priv;
       THROW_IF_FAILED(c->app_device->CreateTexture2D1(&d, nullptr,
           reinterpret_cast<ID3D11Texture2D1**>(priv.put())));
       data->private_images.emplace_back(std::move(priv));
   }
   ```
   Then change the line that exposes pointers to the app (around line 556 `sc->base.images[i] = image.get()`) to expose `data->private_images[i].get()` instead of `data->app_images[i].get()`.

3. **In the swapchain release path** (find `release_image` in this file or in the shared `comp_d3d_common.cpp` — `releaseKeyedMutex` is called from there). Before the runtime's `releaseKeyedMutex`, do:
   ```cpp
   c->app_context->CopyResource(data->app_images[i].get(), data->private_images[i].get());
   c->app_context->Flush();  // ensure the copy reaches the GPU command queue
   ```
   The keyed-mutex acquire/release wrapping (already in place via `KeyedMutexCollection::waitKeyedMutex` / `releaseKeyedMutex`) remains correct because we ARE touching the shared texture (via CopyResource).

4. **Plugin side** (optional but recommended cleanup): once the runtime fix lands, the plugin's typed-sibling substitution in `unity-3d-display/native~/displayxr_d3d11_backend.cpp::on_swapchain_created` is dead weight (see Cycle 12 finding). Remove the substitution permanently — it just allocates a duplicate swapchain per eye for no benefit and complicates the keyed-mutex flow. Either:
   - Hard-disable: `return` at the top of `on_swapchain_created`.
   - Or: gate on a useful pre-condition (e.g., only substitute when `createInfo->format` is a typeless DXGI format — which modern Unity never requests).

### D3D12 implementation outline

Parallel structure in `src/xrt/compositor/client/comp_d3d12_client.cpp`:
- Allocate `private_resources` as committed `ID3D12Resource` with `D3D12_HEAP_TYPE_DEFAULT` (no `D3D12_HEAP_FLAG_SHARED`).
- Expose `private_resources[i]` from `xrEnumerateSwapchainImages`.
- In release, record a `CopyResource` from private → shared on the IPC client's command list, signal the existing fence.

The texture-format-conversion gotcha (`D3D12 texture format must match exactly for CopyResource` — same family rules as D3D11) applies. Use the original SRGB format for the private texture too.

## Verification plan

The diagnostic infrastructure stays in place during fix dev:

1. **Run Unity D3D11 in shell** (the original repro):
   ```bash
   "_package/bin/displayxr-shell.exe" "C:\Users\Sparks i7 3080\Documents\Unity\DisplayXR-test-Unity\Test-D3D11\DisplayXR-test.exe"
   ```
   Trigger atlas screenshot. **Success = Unity content visible** (cube + sky + UI). Failure = orange.
2. **Service log readback** should show non-orange:
   ```bash
   grep '\[#184\] readback' "/c/Users/Sparks i7 3080/AppData/Local/DisplayXR/DisplayXR_displayxr-service.exe.<latest>.log" | head -3
   ```
   Expected: any color other than `0xff00bcff` (UNORM-encoded orange-via-SRGB) at the corner samples.
3. **Cube regression checks must continue to pass:**
   ```bash
   _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
   cmd.exe /c "set DISPLAYXR_TEST_FORCE_SRGB=1 && _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe"
   _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d12_win\build\cube_handle_d3d12_win.exe
   cmd.exe /c "set DISPLAYXR_TEST_FORCE_SRGB=1 && _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d12_win\build\cube_handle_d3d12_win.exe"
   ```
   All four must show cube content.
4. **Standalone Unity** (no shell) must continue to work — the in-process compositor path is untouched but verify nothing regresses.

## Pre-PR cleanup

Before opening a PR, strip everything diagnostic:

- Runtime `[#184]` patches in `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (sentinel paint, periodic readback, `requested_dxgi_format` field, `get_unorm_format` helper if unused, `[#184] svc adapter LUID` log, the override comment in per-view loop, includes for `<set>` `<map>`).
- Runtime `[#184]` patches in `src/xrt/compositor/client/comp_d3d11_client.cpp` (the OpenSharedResource1 success log).
- Plugin `[#184]` PROBES 1–8 + typeless cache infra in `D3D11ScSub` + Cycle 12 substitution-skip log.
- Test app `[#184]` env-var override in `test_apps/common/xr_session_common.cpp` — **decide:** strip (purist) or keep as a permanent regression test path. Recommendation: keep, with the comment cleaned up.
- Unity test project `Assets/Editor/BuildScript.cs` — keep (general-purpose project file, not in our repo).

The PR should:
- Title: "Fix #184 — Unity SRGB clients black in shell (runtime IPC client interposes private texture + CopyResource at release)"
- Description: pull in the executive summary section from `HANDOFF_184.md`, link to the cycle table for evidence.
- Tests: add `cube_handle_d3d11_win` + `cube_handle_d3d12_win` SRGB regression to CI if not already present.

## Memory hooks (will load automatically)

These are in `~/.claude/projects/.../memory/` and load each session. Pay attention especially to:
- `feedback_branch_workflow.md` — work on the branch, not main.
- `feedback_pr_workflow.md` — don't auto-merge.
- `feedback_test_before_ci.md` — let user eyeball before push/CI.
- `feedback_dll_version_mismatch.md` — push runtime DLLs to `C:\Program Files\DisplayXR\Runtime\` after each build.
- `feedback_use_build_windows_bat.md` — never call cmake/ninja directly on Windows.
- `reference_screenshot_temp_permission.md` — long-form temp paths.
- `reference_runtime_screenshot.md` — atlas screenshot trigger mechanism.
- `reference_sentinel_paint_probe.md` — pattern for cross-process kernel-object identity tests (proven in this investigation).
- `reference_plugin_build_quirks.md` — `native~/build-win.bat` invocation needs absolute path; `cmd.exe //c "$(pwd -W)\\build-win.bat"`. (saved this session)
- `reference_unity_batchmode_build.md` — Unity 6000.4.0f1 batchmode CLI pattern. (saved this session)

## Don'ts

- Don't add more diagnostic probes — diagnosis is locked. If you find yourself wanting one, check `HANDOFF_184.md` cycle table first.
- Don't touch the service compositor (`comp_d3d11_service.cpp`, `comp_d3d12_service.cpp`) — fix is purely in the IPC client compositor.
- Don't push, don't `/ci-monitor`, don't merge until the user has eyeballed.

## If you get stuck

The bug is precisely characterized; if your fix doesn't work, re-read the executive summary then check whether you accidentally:
- Returned the imported `app_images` to the app instead of `private_images`.
- Forgot to `Flush()` after the CopyResource in release.
- Allocated `private_images` without matching the source format/dims/mip/array exactly (causes silent CopyResource no-op).
- Didn't keep the existing keyed-mutex acquire/release wrapping.

Branch link: https://github.com/DisplayXR/displayxr-runtime-pvt/pull/new/fix/184-shell-srgb-atlas-regression

Good luck. The hard part is done.
