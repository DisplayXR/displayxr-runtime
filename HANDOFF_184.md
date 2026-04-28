# Handoff prompt — Issue #184 (Unity/Unreal SRGB clients render black in shell)

You are picking up an in-progress investigation. **Read this entire document before touching code.** A previous agent did one full investigation pass; the bug is reproducible but not yet fixed. The branch `fix/184-shell-srgb-atlas-regression` is at clean main code with one independent doc commit (`a72076768` — CLAUDE.md screenshot filename fix, unrelated to #184). The plan file `~/.claude/plans/let-look-at-issue-golden-toucan.md` has the long-form findings; this prompt is the executive summary plus next-action recipe.

## TL;DR of the bug

- `displayxr-shell.exe <unity-d3d11-or-d3d12-build>` → window chrome renders, slot **content area is black**.
- Verified by user: launching the same Unity exe **directly (non-shell) renders correctly**.
- `cube_handle_d3d11_win` (UNORM swapchain) and `cube_handle_d3d12_win` (UNORM) **render correctly in shell** in the same session.
- The Unity D3D11 plugin atlas-patches the projection layer to a single 3840×1080 SRGB atlas swapchain (one shared texture, two views as sub-rects). The Unity D3D12 plugin doesn't atlas-patch (stub) so it submits two per-view 1920×1080 SRGB swapchains. **Both fail in shell.**
- The shared factor is **SRGB-typed swapchain, in-shell, IPC client**. Cube uses UNORM, works.

## What's already verified (don't redo)

1. **Solid-red shader test wrote red into Unity's slot** when injected at the right place in `compositor_layer_commit` (`comp_d3d11_service.cpp:~9617`). Conclusion: per-client atlas RTV bind, multi-comp slot read, DP weave are all healthy in shell mode for SRGB clients. The write **destination side works**.
2. **Reading from Unity's source atlas swapchain returns zero bytes** at every sample position, verified via in-line CPU readback (CopyResource → Map → pixel inspection). Tested both:
   - Pre-cached SRV created at `compositor_create_swapchain` (`comp_d3d11_service.cpp:2685`) with `srv_desc.Format = dxgi_format` (= `R8G8B8A8_UNORM_SRGB` for Unity).
   - Fresh SRV created at draw time with `get_srgb_format(view_descs[eye].Format)`.
   - Direct `CopySubresourceRegion` from the source texture (no SRV).
3. **Per-client atlas format does not affect the bug.** Tried both UNORM (pre-3cffdf8b5 layout) and TYPELESS (current). Same black for Unity.
4. **TYPELESS shared swapchain texture also did not fix it.** Tried changing `compositor_create_swapchain`'s `tex_desc.Format` to `R8G8B8A8_TYPELESS` for SRGB-family inputs (kept the typed SRV). Cube still rendered, Unity still black. Reverted.
5. **Plugin atlas composite is firing.** Unity client log next to the exe (`displayxr.log`) shows `xrEndFrame: atlas composite OK (n=2 tiles=2x1 atlas=3840x1080 viewPx=1920x1080)` every 4 frames.
6. **Both client and service are pinned to the dGPU** (LUID `00000000-000138af`, NVIDIA RTX 3080 Laptop). Same physical adapter — verified in service log lines 45–53.
7. **Keyed mutex acquire never logs a timeout** (`comp_d3d11_service.cpp:9419`). The 100 ms acquire succeeds every frame.

## What's still unknown / next hypotheses to try

The mystery: standalone Unity works (so plugin writes ARE visible to **some** runtime path), but shell mode reads zero. Same plugin, same shared texture, same keyed-mutex protocol on key 0 (verified in `comp_d3d_common.cpp:99` + `comp_d3d11_service.cpp:9431`).

Try these in order:

### ~~Hypothesis A — render_mutex race~~ — **TESTED, DOES NOT FIX**

Wrapped the entire per-view loop (lines `~9543`–`~9700`) in `std::lock_guard<std::recursive_mutex>(sys->render_mutex)` so any concurrent `multi_compositor_render` serializes against the per-view writes. Unity still rendered black. So a logical race on the multi-comp side is NOT the bug. Reverted.

(Also confirmed `SetMultithreadProtected(TRUE)` is set on `sys->device` at line `~10562`, so concurrent `sys->context` calls don't UB even unlocked — they serialize at the API surface.)

### Hypothesis B — IPC client texture handle is being opened wrong on this code path

The runtime's IPC client (`comp_d3d11_client.cpp`, `client_d3d11_service_swapchain.cpp` if it exists, or wherever `OpenSharedResource1` is called on the client side) returns a typed texture pointer to the Unity plugin via `xrEnumerateSwapchainImages`. If shell mode somehow gives the plugin a NULL or pointer to a *different* texture than what the service writes back to via NT handle, plugin writes go to a different resource than what the service reads.

**Test:** in `compositor_layer_commit` at the start of the per-view loop, log the `view_textures[0]` pointer value. In the IPC client process (Unity) at `xrEnumerateSwapchainImages` return, log the texture pointer the plugin receives. Cross-process pointers won't be identical (different address spaces) but the **NT handle** at creation time should be — log that too. If the service-side handle differs from the client-side handle for atlas swapchain, that's the bug.

Relevant client code is in `src/xrt/compositor/client/comp_d3d11_client.cpp` and IPC plumbing in `src/xrt/ipc/client/`.

### Hypothesis C — service-side swapchain has been opened on wrong device

`compositor_create_swapchain` (line ~2540) creates the texture on `sys->device`. If the service has multiple D3D11 devices or there's an adapter mismatch in the recent dGPU-pinning code (commit `c49afc193`), the service's device might not be the same as the plugin's device that opens the NT handle.

**Test:** `sys->device->QueryInterface(IID_IDXGIDevice)` → `GetAdapter()` → `GetDesc()` and log the adapter LUID at swapchain creation. Compare to client's adapter LUID logged at line 45 of `comp_d3d11_service.cpp` ("Client D3D11 adapter LUID"). The user's last test had both on `00000000-000138af`, but verify it's still that way for THIS code path.

### Hypothesis D — Unity plugin's typed shadow swapchain isn't actually being written by Unity

The plugin pairs Unity's TYPELESS swapchain with a "typed shadow" SRGB swapchain (line ~94 of `displayxr_d3d11_backend.cpp`). Unity renders into TYPELESS via Unity's typed RTV. The plugin then `CopySubresourceRegion`'s the typed shadow into the atlas — but what does Unity actually write into the typed shadow? If Unity only writes to its own TYPELESS, the typed shadow stays zero and the plugin's atlas-composite copies zeros into the atlas.

Actually wait — re-reading the plugin code: `src_tex` at line 481-484 is `sub->typed_textures[sub->current_idx]`. Where is `typed_textures` populated? If Unity writes to its TYPELESS swapchain and the plugin's `typed_textures` array points at... Unity's TYPELESS textures? Or freshly-allocated typed textures?

Worth tracing: `sub->typed_textures` allocation in `displayxr_d3d11_backend.cpp` and `displayxr_hooks.cpp`. If the plugin's typed-shadow substitution silently feeds Unity an unrelated texture for rendering and the typed shadow is never updated, **standalone wouldn't work either**. So this contradicts the user's verification — but worth a 5-minute trace.

## How to iterate quickly

The previous agent set up these fast-loop conveniences (already in place — don't redo):

- **Build + deploy + clean + relaunch** (one cycle, ~30 sec):
  ```bash
  scripts/build_windows.bat build 2>&1 | tail -3 && \
    cp _package/bin/displayxr-service.exe "/c/Program Files/DisplayXR/Runtime/" && \
    cp _package/bin/DisplayXRClient.dll "/c/Program Files/DisplayXR/Runtime/" && \
    rm -f "/c/Users/Sparks i7 3080/AppData/Local/Temp/shell_screenshot_atlas.png" \
          "/c/Users/Sparks i7 3080/AppData/Local/Temp/shell_screenshot_trigger" && \
    echo "ready"
  ```
  Then in a second tool call (background): `"_package/bin/displayxr-shell.exe" "C:\Users\Sparks i7 3080\Documents\Unity\DisplayXR-test-Unity\Test-D3D11\DisplayXR-test.exe"`

- **Capture compositor screenshot** (~5 sec wait):
  ```bash
  sleep 18 && \
    touch "/c/Users/Sparks i7 3080/AppData/Local/Temp/shell_screenshot_trigger" && \
    sleep 4
  ```
  Then `Read C:\Users\Sparks i7 3080\AppData\Local\Temp\shell_screenshot_atlas.png`.

- **CRITICAL: always use the long-form path** `/c/Users/Sparks i7 3080/AppData/Local/Temp/...`. The DOS short-name `SPARKS~1` trips the harness's "suspicious Windows path pattern" check **before** the permission allowlist runs, forcing a manual prompt every iteration. Long-form path is auto-allowed.

- **Kill running processes between iterations** (otherwise binary deploy fails silently because DLL is locked):
  ```
  PowerShell: Get-Process displayxr-shell, displayxr-service, DisplayXR-test -ErrorAction SilentlyContinue | Stop-Process -Force
  ```

- **Repro app** (Unity D3D11 build, ~April 11 2026, plugin atlas-composites correctly):
  `C:\Users\Sparks i7 3080\Documents\Unity\DisplayXR-test-Unity\Test-D3D11\DisplayXR-test.exe`

- **Working baseline (cube_handle_d3d11_win)** to confirm shell rendering works for UNORM clients:
  `test_apps/cube_handle_d3d11_win/build/cube_handle_d3d11_win.exe`

## Diagnostic snippets that worked

- **Atlas readback** (see if bytes landed in the dest atlas after raw copy):
  Place inside `compositor_layer_commit` after the per-view loop. Creates a STAGING copy, maps, reads pixels at known coordinates. One-shot via `static bool diag_done = false`. Same pattern can be applied to the SOURCE swapchain texture (`view_textures[0]`) to check whether plugin writes are visible to the service. (This is what proved the source reads as zero.)

- **Solid-red sanity blit** (see if the shader path itself is plumbed correctly):
  Inside the per-view loop, before the actual blit, set up a `BlitConstants` with `convert_srgb = 2.0f` and `src_rect[0..2] = (1, 0, 0)`, bind `c->render.atlas_rtv`, viewport = atlas dims, draw 4 verts. If you see red in the slot, the dest write path is fine. (This confirmed the shader pipeline is healthy.)

## What NOT to spend time on

- Don't re-investigate whether 3cffdf8b5 (TYPELESS atlas) is the regression — it isn't. The bug is unrelated to per-client atlas format.
- Don't re-investigate keyed mutex timing — the acquire succeeds, no timeouts logged.
- Don't add diagnostics that do CPU readbacks INSIDE the per-frame hot path — they slow the IPC enough that Unity disconnects with `client_loop ReadFile failed`. Use one-shot static gates.
- Don't propose changing the IPC service to be in-process — that's a structural rewrite, not a bug fix.

## What to deliver

1. Reproduce the bug (1 cycle, baseline confirmed black).
2. Try Hypothesis A first — it's a 10-line change (single mutex around `sys->context` calls in the two functions). If it works, propose a refined locking strategy. If it doesn't, move to B.
3. For each hypothesis, **commit a diagnostic patch** even if it doesn't fix the bug — partial findings save the next session time.
4. End with either: a fix PR-ready, OR a tightened hypothesis list with one-line reasons each was ruled out.

## Memory hooks already saved (will load automatically)

- `feedback_branch_workflow.md` — work on a fix branch, never main.
- `reference_screenshot_temp_permission.md` — long-form path; user pre-authorized %TEMP% screenshot artifacts.
- `reference_runtime_screenshot.md` — current screenshot mechanism (ATLAS flag → `_atlas.png`).
- `reference_service_log_diagnostics.md` — service log location + one-shot BRIDGE BLIT / DP HANDOFF lines.
- `feedback_test_before_ci.md` — wait for user to test before pushing or running /ci-monitor.
- `feedback_pr_workflow.md` — don't auto-merge; user reviews + merges, prefer rebase.
- `feedback_dll_version_mismatch.md` — push runtime DLLs to Program Files\DisplayXR\Runtime after each build (not the installer).

Good luck.
