# Handoff prompt — Issue #184 (Unity/Unreal SRGB clients render black in shell)

> **READ THIS FIRST — investigation complete after 13 cycles. Skip the long history below unless you need evidence; everything actionable is in this top section.**

## Executive summary

**The bug is fully diagnosed.** Unity (D3D11 *and* D3D12, multi-pass mode) doesn't write to imported cross-process `SHARED_KEYEDMUTEX | SHARED_NTHANDLE` (D3D11) / `ID3D12Heap`+`ID3D12Fence` (D3D12) swapchain textures. In standalone (no shell) the runtime uses an in-process compositor and Unity's writes eventually land in same-process textures — so it works. In shell the IPC service compositor allocates cross-process shared textures, Unity's render pipeline silently no-ops the writes, the service-side compose reads orange (a sentinel we paint at swapchain create) every frame.

**The bug is NOT in our code.** The runtime D3D11 + D3D12 IPC chains both correctly handle SRGB cross-process shared textures when the client renders via standard immediate-context patterns. Proven by `cube_handle_d3d11_win` and `cube_handle_d3d12_win` with `DISPLAYXR_TEST_FORCE_SRGB=1` — both render correctly in shell. The bug is squarely in Unity's render pipeline's interaction with imported cross-process shared resources.

## Recommended next move (one design + two parallel implementations)

**Don't add more probes.** Diagnosis is locked across both APIs. Implement architectural fix **option 2** from Cycle 12:

> **Runtime IPC client allocates a Unity-private (non-shared) texture per swapchain image.** xrEnumerateSwapchainImages returns the Unity-private pointers (Unity writes to these — works because no cross-process gate). Inside `xrReleaseSwapchainImage`, the runtime IPC client does a `CopyResource` from Unity-private → the existing `SHARED_KEYEDMUTEX|SHARED_NTHANDLE` shared texture, on a context the runtime fully controls (immediate context with explicit Flush). Then signals release. The service compositor reads from the shared texture as before — but now sees Unity's content because the runtime's controlled CopyResource definitely lands.

Cost: one extra GPU CopyResource per eye per frame (1920×1080 = ~8 MB / 0.1 ms on RTX 3080 Laptop). Localized to `comp_d3d11_client.cpp` and `comp_d3d12_client.cpp` in parallel.

Touch points (D3D11):
- `src/xrt/compositor/client/comp_d3d11_client.cpp::client_d3d11_create_swapchain` — after `OpenSharedResource1` returns the imported `app_images`, allocate a parallel non-shared texture array `private_images` on `c->app_device` with the same desc minus `SHARED_*` MiscFlags. Return `private_images[i]` from `xrEnumerateSwapchainImages` instead of the imported pointers.
- `src/xrt/compositor/client/comp_d3d11_client.cpp::xrReleaseSwapchainImage` (or its IPC wrapper) — before signalling the runtime's release, `app_context->CopyResource(app_images[i].get(), private_images[i].get())` and `app_context->Flush()`. The keyed-mutex acquire/release that the IPC client already does around the wait/release window remains correct because the imported texture IS being touched (by our CopyResource).
- Plus: remove the Cycle 12 substitution-skip in plugin, OR remove the typed-sibling substitution entirely (it's dead weight per Cycle 12 finding).

Touch points (D3D12): parallel structure in `comp_d3d12_client.cpp`. Use `ID3D12Resource::CopyResource` via the IPC client's command queue, signal an `ID3D12Fence` before release.

Verification:
- Sentinel/readback `[#184]` patches in service + plugin still in place. Run Unity in shell — atlas screenshot should show Unity content (cube + sky + UI).
- Run `cube_handle_d3d11_win` + `cube_handle_d3d12_win` (no env var) — must continue to work (regression check).
- Run cube with `DISPLAYXR_TEST_FORCE_SRGB=1` — must continue to work.

Strip all `[#184]` diagnostics + plugin probes before merging the production fix. The `requested_dxgi_format` field on `d3d11_service_swapchain` and the `get_unorm_format` helper can stay if useful elsewhere.

## Branch + tree state at end of investigation (2026-04-28)

- Runtime: `fix/184-shell-srgb-atlas-regression`, clean diagnostic-only patches (`[#184]` sentinel paint, periodic readback, `requested_dxgi_format` plumbing, `get_unorm_format` helper, client OpenSharedResource1 log). Build green via `scripts\build_windows.bat build`. **Not pushed; nothing committed since `cedeac421`.** Two doc commits exist on the branch: `cedeac421` (the initial handoff stub) and `a72076768` (unrelated CLAUDE.md fix).
- Plugin (`unity-3d-display`, branch unmodified main, working tree dirty): 8 PROBEs in `displayxr_d3d11_backend.cpp` + Cycle 12 substitution-skip + typeless-cache infra in `D3D11ScSub`. Build via `native~/build-win.bat` (use absolute path — see memory hook below). DLL deployed to `Documents/Unity/DisplayXR-test-Unity/Test-D3D11/DisplayXR-test_Data/Plugins/x86_64/` and `DisplayXR-test/Build/.../Plugins/x86_64/`.
- Unity test project: `Documents/Unity/DisplayXR-test-Unity/DisplayXR-test/`. `Assets/Editor/BuildScript.cs` exposes `BuildScript.BuildWindows64` and `BuildScript.BuildWindows64NoGfxJobs` (both force D3D11 + can disable graphics jobs). Headless rebuild via Unity 6000.4.0f1 batchmode.
- `test_apps/common/xr_session_common.cpp::CreateSwapchain` has the `DISPLAYXR_TEST_FORCE_SRGB=1` env-var override (matches DXGI 29/91 + VK 43/50). Permanent regression-test path.

## Repro / iteration commands (battle-tested loop, ~30 sec)

```bash
# RUNTIME ONLY (most fixes will be runtime-side)
cd "/c/Users/Sparks i7 3080/Documents/GitHub/openxr-3d-display"
scripts/build_windows.bat build 2>&1 | tail -3 && \
  cp _package/bin/displayxr-service.exe "/c/Program Files/DisplayXR/Runtime/" && \
  cp _package/bin/DisplayXRClient.dll   "/c/Program Files/DisplayXR/Runtime/" && \
  rm -f "/c/Users/Sparks i7 3080/AppData/Local/Temp/shell_screenshot_atlas.png" \
        "/c/Users/Sparks i7 3080/AppData/Local/Temp/shell_screenshot_trigger" && echo ready

# Then in background:
"_package/bin/displayxr-shell.exe" "C:\Users\Sparks i7 3080\Documents\Unity\DisplayXR-test-Unity\Test-D3D11\DisplayXR-test.exe"

# Wait + capture:
sleep 18 && touch "/c/Users/Sparks i7 3080/AppData/Local/Temp/shell_screenshot_trigger" && sleep 4

# Then Read C:\Users\Sparks i7 3080\AppData\Local\Temp\shell_screenshot_atlas.png
```

Cube SRGB regression check:
```bash
cmd.exe //c "set DISPLAYXR_TEST_FORCE_SRGB=1 && _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe"
# (and again with cube_handle_d3d12_win)
```

Plugin rebuild (if also touching plugin):
```bash
cd "/c/Users/Sparks i7 3080/Documents/GitHub/unity-3d-display/native~"
# Note: build-win.bat needs absolute path because cwd reset + native~ tilde
cmd.exe //c "$(pwd -W)\\build-win.bat"
# DLL lands at unity-3d-display/Runtime/Plugins/Windows/x64/displayxr_unity.dll
# Copy into BOTH:
cp ".../Runtime/Plugins/Windows/x64/displayxr_unity.dll" "/c/Users/Sparks i7 3080/Documents/Unity/DisplayXR-test-Unity/Test-D3D11/DisplayXR-test_Data/Plugins/x86_64/"
cp ".../Runtime/Plugins/Windows/x64/displayxr_unity.dll" "/c/Users/Sparks i7 3080/Documents/Unity/DisplayXR-test-Unity/DisplayXR-test/Build/DisplayXR-test_Data/Plugins/x86_64/"
```

Unity headless rebuild (only when changing `Assets/` or PlayerSettings):
```bash
"/c/Program Files/Unity/Hub/Editor/6000.4.0f1/Editor/Unity.exe" -batchmode -quit -nographics \
  -projectPath "C:\Users\Sparks i7 3080\Documents\Unity\DisplayXR-test-Unity\DisplayXR-test" \
  -executeMethod BuildScript.BuildWindows64 \
  -logFile "C:\Users\Sparks i7 3080\Documents\Unity\DisplayXR-test-Unity\DisplayXR-test\Build\build.log"
# Build/ exe doesn't trigger plugin atlas-composite reliably — prefer Test-D3D11 exe + DLL injection above
```

## Important do/don't

- **Do** kill stragglers between iterations: `Get-Process displayxr-shell, displayxr-service, DisplayXR-test, cube_handle_d3d* | Stop-Process -Force`.
- **Do** use long-form path `/c/Users/Sparks i7 3080/AppData/Local/Temp/...` for screenshot artifacts (DOS short-name `SPARKS~1` trips harness auto-deny).
- **Do** verify both D3D11 (Test-D3D11/DisplayXR-test.exe) and D3D12 (cube_handle_d3d12_win + Unity D3D12) when implementing the fix — same architectural change, parallel D3D11/D3D12 implementations.
- **Don't** auto-merge any PR; user reviews + merges.
- **Don't** push or `/ci-monitor` until user has eyeballed local result.
- **Don't** add more diagnostic probes — diagnosis is locked. If you find yourself wanting one, you're probably re-investigating something already proven (check the cycle table below first).
- **Don't** use the fresh `DisplayXR-test/Build/DisplayXR-test.exe` for plugin-substitution tests — the older `Test-D3D11/DisplayXR-test.exe` is the known-atlas-composite-firing build. The fresh one's atlas-composite path doesn't fire reliably (cause unknown, low priority since we removed substitution dependency in Cycle 12).

---

## Cross-API summary table (locked)

| Cycle | Test | API | Format | Result |
|---|---|---|---|---|
| 7 | cube_handle_d3d11_win (default) | D3D11 | UNORM | renders ✓ |
| 7 | cube_handle_d3d11_win + FORCE_SRGB | D3D11 | SRGB | **renders ✓** |
| 13 | cube_handle_d3d12_win + FORCE_SRGB | D3D12 | SRGB | **renders ✓** |
| 1–12 | Unity D3D11 in shell | D3D11 | SRGB | broken (orange) |
| (prior agent) | Unity D3D12 in shell | D3D12 | SRGB | broken |

Eight ruled-out hypotheses (each definitively tested):
1. Cross-process kernel-object identity (PROBE 1: plugin sees orange = handle import correct)
2. AppContainer SA on `CreateSharedHandle` (Cycle 3 — removed, no change)
3. Per-client atlas format (prior agent — TYPELESS + UNORM both tried)
4. Service-side storage format (Cycles 5+6 — TYPELESS, UNORM, SRGB all fail when REQUEST is SRGB)
5. Runtime D3D11 IPC chain (Cycle 7 — cube SRGB works)
6. Plugin's typed-sibling substitution (Cycles 8+12 — bug present even without it)
7. Plugin↔Unity device mismatch (PROBE 3: device MATCH)
8. Unity multi-threaded rendering / graphics jobs (Cycle 10 — `graphicsJobs=false` doesn't help)

Plus runtime D3D12 path validated cross-process for SRGB (Cycle 13).

---

# Detailed cycle history (evidence; skip if you trust the summary above)

## TL;DR of the bug

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

## Cycle 2 — sentinel-paint probe (2026-04-28) — RESULT: cross-process kernel-object mismatch

**Patches** (still applied on this branch — diagnostic-only, no behavior change for non-SRGB):

- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`
  - After `CreateTexture2D` for SRGB+RENDER_TARGET swapchains: `AcquireSync(0,1000)` → `ClearRenderTargetView` orange `(1.0, 0.5, 0.0, 1.0)` → `Flush` → `ReleaseSync(0)`. Logs `[#184] sentinel paint: sc=... tex=... NT=... fmt=... WxH ...`.
  - At end of `compositor_create_swapchain`, log `[#184] svc adapter LUID=...`.
  - In `compositor_layer_commit` after the keyed-mutex acquire loop: one-shot per-source-texture readback (gated by `static std::set<ID3D11Texture2D*>` + mutex). `CopySubresourceRegion` slice 0 mip 0 → STAGING → `Map` → log RGBA at TL/TR/BL/BR/CTR. Logs `[#184] readback eye=... sc=... tex=... fmt=... TL=0x... ...`.
  - Added `#include <set>`.
- `src/xrt/compositor/client/comp_d3d11_client.cpp`
  - In `import_image` (NT-handle path) after success: log `[#184] client OpenSharedResource1 OK: NT=... tex=... fmt=... WxH ... client_adapter_LUID=...` via `OutputDebugStringA` (visible in DebugView, not in plugin's `displayxr.log`).
  - In `import_image_dxgi` (legacy fallback) after success: log `[#184] client LEGACY OpenSharedResource(DXGI) fired: ...`.

**Test cycle:** `scripts/build_windows.bat build` → deploy `displayxr-service.exe` + `DisplayXRClient.dll` to `C:\Program Files\DisplayXR\Runtime\` → kill stale procs → launch `_package/bin/displayxr-shell.exe` with `Documents\Unity\DisplayXR-test-Unity\Test-D3D11\DisplayXR-test.exe` → trigger `%TEMP%\shell_screenshot_trigger`.

**Result — atlas screenshot is bright ORANGE in both slots.** Sentinel painted at `compositor_create_swapchain` survived. Service log lines from `%LOCALAPPDATA%\DisplayXR\DisplayXR_displayxr-service.exe.25188_2026-04-28_13-58-43.log`:

```
166: [#184] sentinel paint: sc=0x027B0B7C6960 img=0 tex=0x027B1034ECE0 NT=0x0BE0 fmt=29 1920x1080 arr=1 mips=1 sample=1 misc=0x900
168: [#184] svc adapter LUID=00000000-000138af (srgb swapchain sc=0x027B0B7C6960 fmt=29 1920x1080)
... (4 more 1920x1080 SRGB swapchains — typed siblings)
206: [#184] sentinel paint: sc=0x027B1035A3A0 img=0 tex=0x027B1034EFA0 NT=0x0BD4 fmt=29 3840x1080 arr=1 mips=1 sample=1 misc=0x900   ← THE ATLAS
208: [#184] svc adapter LUID=00000000-000138af (srgb swapchain sc=0x027B1035A3A0 fmt=29 3840x1080)
216: [#184] readback eye=0 sc=0x027B1035A3A0 tex=0x027B1034EFA0 3840x1080 fmt=29 TL=0xff00bcff TR=0xff00bcff BL=0xff00bcff BR=0xff00bcff CTR=0xff00bcff
```

Decode: `0xff00bcff` = R=0xff G=0xbc B=0x00 A=0xff (RGBA8 little-endian). `G=0xbc=188` is the SRGB encoding of linear 0.5 (srgb(0.5) ≈ 0.7354 × 255 ≈ 187.5) — exactly what `ClearRenderTargetView` writes into an SRGB RTV when given `(1.0, 0.5, 0.0, 1.0)`. **The sentinel value is preserved bit-perfectly** at every sample position, including center. The plugin's `CopySubresourceRegion` writes from typed shadow → atlas never touched this kernel object.

**Conclusion:** Same `xrSwapchain` ↔ same NT handle on both processes ↔ different underlying GPU kernel object. The IPC client's `OpenSharedResource1` returns a texture pointer that is NOT bound to the same kernel object the service created. The previous agent's "every sample reads zero" was actually "every sample reads the texture's initial state" — which would be zero for an unwritten new texture.

**Why does cube_handle_d3d11_win work then?** The cube's swapchain is `R8G8B8A8_UNORM` (not SRGB), so it skips the sentinel paint entirely. We did NOT positively verify cube's kernel-object identity in this cycle; we only know cube content renders correctly — implying the bug is **SRGB-specific**, not "all IPC SRGB+UNORM".

**Why does standalone (non-shell) Unity work?** Standalone may go through a different in-process compositor that doesn't use `OpenSharedResource1` at all (no IPC, same process holding the texture). Worth confirming.

### Hypotheses for next cycle (ranked)

1. **DXGI_FORMAT_R8G8B8A8_UNORM_SRGB + `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | SHARED_NTHANDLE` + AppContainer SA on `IDXGIResource1::CreateSharedHandle` triggers a driver/runtime quirk** where `OpenSharedResource1` from a non-AppContainer client process returns a "handle-valid but resource-distinct" texture. Worth testing: drop the AppContainer SA in `compositor_create_swapchain` (line ~2632 `create_appcontainer_sa`) and re-run the sentinel test. If orange goes away (replaced by Unity content or black-from-plugin-write), the AppContainer SA is the culprit. The fix would be to either skip the SA for non-Chrome clients or to grant SR access in the SA more broadly.

2. **The IPC client is taking the legacy DXGI branch (`import_image_dxgi`) for SRGB swapchains.** The `[#184] client LEGACY OpenSharedResource(DXGI) fired` log in DebugView (not the plugin's displayxr.log) would confirm this. Capture next cycle by attaching DebugView to the Unity process before launch, or pipe `OutputDebugStringA` to a file via the runtime client (small one-line patch to also write to `%TEMP%\displayxr_client_184.log`).

3. **Format-typing mismatch on `OpenSharedResource1`.** Service creates the texture as `R8G8B8A8_UNORM_SRGB` (typed). `OpenSharedResource1` requests `__uuidof(ID3D11Texture2D1)` — fine, but maybe the client D3D11 device's feature level or DXGI version interprets the typed shared resource as a separate alias when the requested IID doesn't carry the format. Worth: log `tex->GetDesc()` on the client side (already in the patch) and compare `Format` value against service's `dxgi_format`. If mismatched, that's the bug.

4. **Service's `sys->context->Flush()` at sentinel paint isn't enough — sentinel write is GPU-pending and the keyed-mutex `ReleaseSync(0)` doesn't wait for the GPU.** Could explain "client opens before sentinel commits" — but then the readback later (after plugin had a chance to write) should still reflect the correct final state. The fact that orange survives the entire run, including post-plugin-write, rules this out.

### Next move (one cycle)

Try Hypothesis #1 first — drop AppContainer SA. It's a 5-line change (replace `create_appcontainer_sa(sa, sd); HANDLE shared_handle = nullptr; hr = dxgi_resource1->CreateSharedHandle(&sa, ...)` with `hr = dxgi_resource1->CreateSharedHandle(nullptr, ...)`). Rebuild → redeploy → re-run sentinel test. If orange disappears, narrow the SA to be permissive enough for non-AppContainer clients but still safe for Chrome.

Concurrently capture DebugView output during the same cycle (run `dbgview64.exe` from sysinternals before launching `displayxr-shell.exe`) so we get the client-side `[#184]` line and can confirm Hypothesis #2 status (which branch fires).

### Diagnostic patches to keep / remove

**Keep** for next cycle (still useful to verify any fix):
- All `[#184]` patches in both files. They're cheap (one-shot) and let us re-verify after each attempted fix.

**Don't ship** to main:
- The patches are diagnostic-only. Whatever the fix turns out to be, strip the `[#184]` blocks before merging.

## Cycle 3 — drop AppContainer SA (2026-04-28) — RESULT: not the cause

**Patch:** in `compositor_create_swapchain` ~line 2632, replaced `create_appcontainer_sa(sa, sd); ... CreateSharedHandle(&sa, ...)` with `CreateSharedHandle(nullptr, ...)`.

**Result — atlas screenshot still bright ORANGE.** Service log:
```
216: [#184] readback eye=0 sc=... tex=... 3840x1080 fmt=29 TL=0xff00bcff TR=... CTR=0xff00bcff
```

The DACL allowing `(A;;GA;;;WD)` (Generic All to Everyone) was already permissive — Unity wasn't being denied by the SA in the first place. Reverted.

## Cycle 4 — sentinel paint extended to UNORM (cube_handle_d3d11_win) (2026-04-28) — RESULT: cube renders correctly, NO orange

**Patch:** dropped the `is_srgb_format` gate from the sentinel paint so it fires for any RENDER_TARGET swapchain.

**Result — atlas screenshot shows the wooden cube on a dark blue grid floor (cube content).** No orange anywhere. Cube's UNORM swapchain content propagated cross-process correctly.

**Conclusion: the cross-process kernel-object identity bug is genuinely SRGB-create-format-specific, not universal.** UNORM `R8G8B8A8_UNORM` storage with `SHARED_KEYEDMUTEX | SHARED_NTHANDLE` works. SRGB-typed `R8G8B8A8_UNORM_SRGB` storage with the same misc flags does not.

## Cycle 5 — TYPELESS storage with SRGB-typed SRV/RTV (2026-04-28) — RESULT: bug persists

**Patches:**
- For SRGB-family inputs, allocated `tex_desc.Format = R8G8B8A8_TYPELESS` (via `d3d_dxgi_format_to_typeless_dxgi`). Sentinel RTV and compositor SRV continued to use the original SRGB-typed `dxgi_format`.
- Added `requested_dxgi_format` field to `d3d11_service_swapchain` struct (`info->format` mapped to DXGI). Set in both `compositor_create_swapchain` and `compositor_import_swapchain`.
- In `compositor_layer_commit` per-view loop, after `view_textures[eye]->GetDesc(&view_descs[eye])`, override `view_descs[eye].Format` and `view_is_srgb[eye]` if the storage came back non-SRGB but the swapchain's requested format was SRGB. This keeps the existing typed-SRGB SRV creation working over TYPELESS storage.

**Result — `[#184] readback eye=0 sc=... tex=... 3840x1080 fmt=29 TL=0xff00bcff TR=0xff00bcff BL=0xff00bcff BR=0xff00bcff CTR=0xff00bcff`.** Sentinel orange survives unchanged. Plugin's `xrEndFrame: atlas composite OK` lines fired (n=2 tiles=2x1 atlas=3840x1080 viewPx=1920x1080), so plugin DID write to the atlas — the writes still don't cross processes. Atlas screenshot: messed-up single yellow window because the plugin's typed-shadow path saw `fmt=27` from `GetDesc` and the rendering-mode dispatch shifted; not a clean visual (the readback line is the authoritative data).

**TYPELESS-storage REVERTED**, but kept the supporting plumbing (`requested_dxgi_format` field, the per-view-loop SRGB-intent override) since they are harmless when storage matches request and may be useful for the eventual fix.

## Refined hypotheses after Cycles 3–5

The bug correlates with **non-UNORM storage formats** for shared `SHARED_KEYEDMUTEX | SHARED_NTHANDLE` textures. Both SRGB-typed and TYPELESS storage exhibit it; UNORM storage does not. Strong candidates:

1. **Driver-level (NVIDIA RTX 3080 Laptop)** issue with cross-process SHARED_KEYEDMUTEX where the underlying GPU resource is not propagated identically when the source format is anything other than UNORM. Worth: try `R8G8B8A8_UNORM` storage with the existing `requested_dxgi_format` plumbing, and create the client-side SRV as SRGB-typed only if the storage is TYPELESS — for UNORM storage we'd lose SRGB linearization on read but writes would propagate. This is a workable workaround if the driver bug is real.

2. **D3D11 runtime `IDXGIResource1::CreateSharedHandle` quirk for non-UNORM-typed resources.** Worth a Microsoft / NVIDIA forum search.

3. **`SHARED_KEYEDMUTEX` semantics drift for non-UNORM** — try dropping `SHARED_KEYEDMUTEX` and using only `SHARED_NTHANDLE`, accepting loss of synchronization for the diagnostic. If sentinel disappears, keyed mutex + non-UNORM is the bug.

### Next move (one cycle, low risk)

**Try Hypothesis 1**: allocate as `R8G8B8A8_UNORM` (or `B8G8R8A8_UNORM` for BGRA family) for SRGB-family inputs. Keep `requested_dxgi_format` so the compose path knows the original intent. Adapt the client side similarly: at `comp_d3d11_client.cpp:369` `OpenSharedResource1`, query `GetDesc` and if format is UNORM but creation request was SRGB, the plugin will see UNORM textures (it needs to handle the format difference, but at least writes propagate). Test sentinel survival; if the orange disappears with UNORM storage, that's a winning workaround direction. The downside is that any client who reads back from the swapchain via SRV would need the same SRGB override.

If UNORM storage works, the proper fix is:
- Service: allocate UNORM storage for SRGB-requested swapchains.
- Service compose: SRV at SRGB format (already handled by the override).
- Client: tell the OpenXR app/plugin that the swapchain's "actual format" returned in `xrEnumerateSwapchainImages` is SRGB (or whatever the create-info specified), even though the underlying texture's `GetDesc` returns UNORM. This may require shadow-typing client side too — this is more involved.

If UNORM storage also fails the sentinel test → the bug is even broader (any non-default format), likely a driver issue. Document, file with NVIDIA, and look for a different architectural workaround (e.g., service-side blit from a UNORM relay buffer).

## Cycle 6 — UNORM storage for SRGB-requested swapchains, with periodic readback (2026-04-28) — RESULT: bug persists across 360 frames

**Patches:**
- For SRGB-family inputs, allocated `tex_desc.Format = R8G8B8A8_UNORM` (via new `get_unorm_format` helper). Sentinel RTV and compositor SRV both use the UNORM storage format (D3D11 forbids SRGB-typed views over UNORM-typed storage).
- Disabled the per-view-loop `view_is_srgb` override (it would attempt SRGB-over-UNORM and fail).
- Re-gated the readback to fire for swapchains whose `requested_dxgi_format` was SRGB (regardless of current storage), so we still get diagnostic data when storage is UNORM.
- **Changed readback from one-shot to periodic** (every 60 frames, capped at 8 fires) so we can rule out timing/race issues.

**Result — readback at frames 0, 60, 120, 180, 240, 300, 360 ALL show `0xff007fff` (UNORM-encoded sentinel orange) at every sample position.** Plugin's `xrEndFrame: atlas composite OK` lines fired at line 109+ (Unity's plugin DID atlas-composite at frame 1 and continues — the "OK" log throttles to first-4 frames per a plugin-side gate, normal).

```
217: [#184] readback n=0   eye=0 sc=... tex=... fmt=28 TL=0xff007fff TR=0xff007fff BL=0xff007fff BR=0xff007fff CTR=0xff007fff
859: [#184] readback n=60  eye=0 ... fmt=28 TL=0xff007fff TR=... ... CTR=0xff007fff
1466:[#184] readback n=120 ... fmt=28 TL=0xff007fff ...
2074:[#184] readback n=180 ... fmt=28 TL=0xff007fff ...
2680:[#184] readback n=240 ... fmt=28 TL=0xff007fff ...
3290:[#184] readback n=300 ... fmt=28 TL=0xff007fff ...
3896:[#184] readback n=360 ... fmt=28 TL=0xff007fff ...
```

**`0xff007fff` = R=0xff G=0x7f B=0x00 A=0xff = UNORM(1.0, 0.5, 0.0, 1.0) = orange.** Across 5+ seconds and 360 frames, the bytes never change. Plugin's writes never propagate.

**UNORM-storage REVERTED.** Kept the periodic readback gate change (previous one-shot too noisy to distinguish "writes happened but timing missed" from "writes never propagated"). The `get_unorm_format` helper stays (zero cost, useful future).

## Refined hypotheses after Cycle 6

The bug is **NOT** about underlying storage format. SRGB, UNORM, and TYPELESS storage all exhibit the same cross-process kernel-object identity failure when the swapchain was originally **requested** as SRGB via `xrCreateSwapchain`. UNORM **request** works (cube). SRGB **request** fails regardless of what we substitute for storage.

Critically: a recursive grep of `src/xrt/compositor/client/comp_d3d11_client.cpp` for `SRGB|srgb` found **zero matches**. The runtime D3D11 client compositor + IPC path are entirely format-agnostic between SRGB and UNORM. So the difference between cube (works) and Unity (fails) is NOT in our runtime code.

Where the difference must be:

1. **Unity plugin's hooked xrCreateSwapchain creates additional swapchains (typed sibling) and additional D3D11 resources/views over the imported texture** that the cube test doesn't. If any of those creates an SRGB-typed RTV/SRV over a UNORM-typed shared texture, D3D11 rejects it (debug-layer error or silent zero-write). When we forced UNORM storage in Cycle 6, **Unity itself** may still have tried to wrap the imported texture with an SRGB-typed view because Unity sees the swapchain as SRGB at the OpenXR-API level.

2. **Unity may use a DIFFERENT D3D11 device for actual rendering than the one passed to `xrCreateSession`** (multi-GPU / compute device pattern). Imported textures are device-bound. If Unity opens the texture on its session device but writes via a different render device, those writes go to a different kernel object — explaining the symptoms exactly.

3. **NVIDIA driver bug** specific to typed-SRGB shared keyed-mutex NT-handle resources. Less likely now that we know UNORM storage also fails for SRGB-requested swapchains — that points away from a storage-format-specific driver path.

### Strong recommendation: pivot from runtime to client/plugin investigation

We've ruled out:
- Per-client atlas format (TYPELESS + UNORM both tried by previous agent, both fail)
- AppContainer SA (Cycle 3)
- Storage format alone (Cycles 5 + 6: TYPELESS and UNORM both fail when request is SRGB)
- Same-process SRGB texture creation/use (cube + UNORM works, plugin's typed-shadow path also writes SRGB-equivalent content via Unity)

The next high-yield investigation is **what Unity / the Unity plugin does differently for SRGB-requested swapchains**. Concrete experiments:

A. **Capture the runtime IPC client's `[#184] client OpenSharedResource1 OK` line via DebugView**. Run `dbgview64.exe` (sysinternals) before launching Unity, capture all OutputDebugStringA. The line includes `client_adapter_LUID` — confirm it matches `00000000-000138af` (service LUID). If the LUID differs for the atlas swapchain only, that's adapter mismatch on import — direct cause.

B. **Patch the Unity plugin's `displayxr_d3d11_backend.cpp` to add a CPU readback right after `CopySubresourceRegion(atlas_tex, ..., src_tex, ...)` (line ~489)**. Verify Unity's writes ARE in `atlas_tex` from the plugin's perspective — that confirms the bug is between client-process and service-process visibility, not within the client process. A 30-line plugin diagnostic.

C. **Test with a NON-Unity SRGB OpenXR client** that doesn't use a plugin. Build a `cube_handle_srgb_d3d11_win` test that explicitly requests SRGB swapchains. If it works in shell, the bug is genuinely a Unity-plugin-specific interaction. If it fails, the bug is deeper.

I'd run **C first** (most data per cycle): if the simple SRGB cube fails, the bug is in the runtime; if it works, the bug is in the Unity plugin. Either way, we narrow the suspect list dramatically.

## Cycle 7 — Non-Unity SRGB OpenXR client test (2026-04-28) — RESULT: bug DOES NOT REPRODUCE — definitively narrowed to Unity / Unity plugin

**Patch:** added `DISPLAYXR_TEST_FORCE_SRGB=1` env-var override to `test_apps/common/xr_session_common.cpp::CreateSwapchain`. When set, the test app picks the first SRGB-typed format from `xrEnumerateSwapchainFormats` (DXGI 29 / 91 or VK 43 / 50) instead of the runtime's preferred format. Otherwise unchanged.

**Test:** `set DISPLAYXR_TEST_FORCE_SRGB=1 && _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe`. Cube enumerates DXGI formats (28, 29, 87, 91, 10) and picks 29 (R8G8B8A8_UNORM_SRGB). Service log: `Creating swapchain: ... 3840x2160, format=43 (VK SRGB)` for the atlas swapchain. Sentinel painted at create.

**Result — atlas screenshot shows the cube rendered correctly (wooden cube on dark blue grid floor, full-display 3840x2160 single tile).** Periodic readback at frames 0/60/120/.../480 over ~10 seconds:

```
162: [#184] readback n=0   eye=0 sc=... tex=... 3840x2160 fmt=29 TL=0xff893f3f TR=0xff893f3f BL=0xff893f3f BR=0xff893f3f CTR=0xff893f3f
455: [#184] readback n=60  ... fmt=29 TL=0xff893f3f ...
703: [#184] readback n=120 ... fmt=29 TL=0xff893f3f ...
... (frames 180, 240, 300, 360, 420, 480 — all identical)
2202:[#184] readback n=480 ... fmt=29 TL=0xff893f3f ...
```

`0xff893f3f` decodes (RGBA8 little-endian) to bytes (0x3f, 0x3f, 0x89, 0xff) = **R=63 G=63 B=137 A=255 — the cube's dark blue floor color** sampled at corner pixels. The SRGB sentinel orange (`0xff00bcff`) was bit-for-bit overwritten by cube's writes by frame 0.

**The cross-process kernel-object identity path works correctly** for an SRGB-requesting OpenXR client using exactly the same runtime + IPC service paths that produce the bug for Unity. The bug is **not in our runtime / IPC / compositor code**.

**Cube_handle_d3d11_win (DXGI 29 / VK 43 SRGB) acts as a regression-test for the Unity bug — if a future runtime change fixes Unity, cube_srgb should also continue to work; if a future change breaks cube_srgb, it will likely break Unity worse.**

## Bug location confirmed: Unity or Unity plugin (`DisplayXR/displayxr-unity`)

What Unity / the plugin does that our cube test does not:
- The plugin loads as native code into Unity's process and **hooks** `xrCreateSwapchain` to allocate a "typed sibling" SRGB swapchain in addition to the atlas, then maps Unity's typeless swapchain images to the typed sibling on `xrEnumerateSwapchainImages`.
- Unity may use a separate D3D11 device for actual rendering (different from the one passed to `xrCreateSession`). Imported textures are device-bound; writes via a different device go to a different kernel object.
- Unity's render path may create SRGB-typed RTV/SRV over the imported texture for view-side gamma management. If type incompatibility silently fails (debug-layer error or empty render), the typed sibling stays empty and the plugin's atlas-composite copies zeros (or the sentinel) into the atlas.

### Highest-yield next step (now in plugin code, not runtime)

Apply the same sentinel/readback pattern in `unity-3d-display/native~/displayxr_d3d11_backend.cpp`:

1. **Right after `imgs[k].texture` is captured at line ~424** (after `xrEnumerateSwapchainImages`), do a 1-pixel readback of the atlas texture — confirm what the plugin sees as initial state. If it sees orange, the plugin opened the same kernel object as the service (handle path is fine). If it sees zero/black, the plugin is operating on a different texture than the service has.
2. **Right after `CopySubresourceRegion(atlas_tex, ..., src_tex, ...)` at line ~489 + `Flush()` at 507**, do another 1-pixel readback of the atlas texture from the PLUGIN's D3D11 device. Confirm Unity's writes ARE in `atlas_tex` from the plugin's perspective. If they are → cross-process delivery is broken (driver bug). If they are NOT → the plugin's source texture (`src_tex` = `sub->typed_textures[sub->current_idx]`) is itself empty — meaning Unity's render hooked into a typed sibling that Unity is not actually drawing into.
3. **Log Unity's D3D11 device pointer at the plugin's hook init**, then again at the time of the actual `CopySubresourceRegion` `context` invocation. If they differ, Unity uses a multi-device setup and writes go to the wrong device.

This requires plugin rebuild + Unity project re-import, but it tells us *exactly* where in the chain Unity loses the writes.

### Diagnostic patches still on branch (all reversible)

Test-app side:
- `test_apps/common/xr_session_common.cpp` — `DISPLAYXR_TEST_FORCE_SRGB=1` env-var override at `CreateSwapchain`. Add `#include <cstdlib>`. **Keep** as a permanent regression-test path (zero cost when env var is unset).

Runtime side: see the updated cumulative-patches section below.

## Cycle 8 — Plugin-side sentinel/readback probes (2026-04-28) — RESULT: Unity is NOT writing to the plugin's typed sibling textures

**Setup:** built fresh Unity plugin DLL via `unity-3d-display/native~/build-win.bat` (MSVC, x64), patched `displayxr_d3d11_backend.cpp` with five `[#184]` probes around the atlas-composite path. Recreated missing `Assets/Editor/BuildScript.cs` in Unity test project, ran headless Unity build:
```
"C:\Program Files\Unity\Hub\Editor\6000.4.0f1\Editor\Unity.exe" -batchmode -quit -nographics \
  -projectPath ...\DisplayXR-test -executeMethod BuildScript.BuildWindows64 \
  -logFile ...\Build\build.log
```
Build wrote to `DisplayXR-test/Build/DisplayXR-test.exe`. **The fresh Unity build did not invoke the plugin's atlas-composite path** — its hooks resolve differently per build cache state. Workaround: also copied the new patched DLL into the older known-atlas-firing `Test-D3D11/DisplayXR-test_Data/Plugins/x86_64/displayxr_unity.dll` and ran from there.

**The five plugin-side probes (all in `displayxr_d3d11_backend.cpp` D3D11Backend::end_frame_apply):**
- PROBE 1 (post-`xrAcquireSwapchainImage`, pre-write): readback of `atlas_tex` corners.
- PROBE 2 (post-`CopySubresourceRegion`+`Flush`): readback of `atlas_tex` corners.
- PROBE 3 (one-shot): `atlas_tex->GetDevice()` vs plugin's captured `device` — MATCH/MISMATCH.
- PROBE 4 (one-shot per copy): logs `src_tex`, `current_idx`, `typed_img_count`, `sub->WxH`, `view_WxH`, `dst`.
- PROBE 5 (one-shot per src_tex): readback of `src_tex` (the typed sibling) corners.

**Plugin output (`displayxr.log` next to the exe):**

```
[#184] plugin PROBE3 device-id check: plugin_device=0x...5FA0 plugin_context=0x...7328 atlas_tex_device=0x...5FA0 (MATCH)
[#184] plugin PROBE1 (post-acquire, pre-write) atlas_tex=... 3840x1080 fmt=29 TL=0xff00bcff TR=0xff00bcff BL=0xff00bcff BR=0xff00bcff CTR=0xff00bcff
[#184] plugin COPY[0] v=0 src_tex=0x...4020 current_idx=0 typed_img_count=1 sub->WxH=1920x1080 view_WxH=1920x1080 dst=(0,0)
[#184] plugin PROBE5 (src_tex content) src_tex=0x...4020 1920x1080 fmt=29 TL=0xff00bcff CTR=0xff00bcff BR=0xff00bcff
[#184] plugin COPY[1] v=1 src_tex=0x...09A0 current_idx=0 typed_img_count=1 sub->WxH=1920x1080 view_WxH=1920x1080 dst=(1920,0)
[#184] plugin PROBE5 (src_tex content) src_tex=0x...09A0 1920x1080 fmt=29 TL=0xff00bcff CTR=0xff00bcff BR=0xff00bcff
[#184] plugin PROBE2 (post-CopySubresourceRegion+Flush) atlas_tex=... 3840x1080 fmt=29 TL=0xff00bcff TR=0xff00bcff BL=0xff00bcff BR=0xff00bcff CTR=0xff00bcff
```

**Decoded:**

- PROBE 3: plugin and atlas_tex are on the **same D3D11 device**. Not a multi-device issue.
- PROBE 1: atlas_tex contains the orange sentinel before plugin writes — plugin's import path is correct, it sees the same kernel object the service owns.
- PROBE 4: src_tex is non-null, dims correct (1920x1080), destinations correct (0,0) and (1920,0). The `CopySubresourceRegion` call args are clean.
- **PROBE 5: src_tex (the typed sibling) is itself FULL of orange sentinel (0xff00bcff) at TL/CTR/BR.** Unity never wrote anything into the typed sibling texture across multiple frames.
- PROBE 2: atlas_tex remains orange after CopySubresourceRegion — correct given src_tex is also orange (orange → orange copy).

**Also tested (failed fix attempt):** explicit `IDXGIKeyedMutex::AcquireSync(0, 100)` on src_tex before the copy returned `0x887A0001 = DXGI_ERROR_INVALID_CALL` consistently. Reverted (the runtime IPC client successfully holds these mutexes via the same QI pattern, so the failure is suspicious — but irrelevant: even with explicit acquire, src_tex contains no Unity content to copy).

## Bug location confirmed: Unity is not writing to the plugin's typed sibling

The plugin's typed-sibling substitution pattern:
1. Unity calls `xrCreateSwapchain(format=typeless)` → plugin's hook also creates a parallel `typed_sc` swapchain (SRGB-typed) via `s_real_create_swapchain`.
2. Unity calls `xrEnumerateSwapchainImages(typeless_sc)` → plugin's hook returns **typed_sc's images** (typed sibling textures) instead of typeless_sc's images.
3. Unity calls `xrAcquireSwapchainImage(typeless_sc)` / `xrWaitSwapchainImage` / `xrReleaseSwapchainImage` → plugin reroutes to `typed_sc` (so its keyed mutex is held during Unity's writes).
4. Unity renders into Unity's render textures (Unity assumes those are backed by the textures returned at step 2).
5. Plugin's `xrEndFrame` hook copies from typed sibling textures to atlas → patches projection layer.

Step 4 is where Unity's writes go missing. **Unity's writes never land in typed sibling.** Possibilities to investigate (ranked):

1. **Unity's OpenXR plugin caches swapchain images at a different point.** Unity may call `xrEnumerateSwapchainImages` at swapchain-create time (before our hook is fully wired) or store the typeless_sc images via a different mechanism (e.g., directly via `xrEnumerateSwapchainImages` on the underlying swapchain handle the hook returned, which may not match typed_sc). Trace Unity's actual `xrEnumerateSwapchainImages` calls and verify our hook returned typed_sc's images. The cube_handle test app calls `xrEnumerateSwapchainImages` once and works fine, so the hook itself is fine — but Unity's pattern may differ.

2. **Unity creates RTVs over the texture pointers from a multi-threaded render path.** Unity's render thread may have a separate `ID3D11DeviceContext` (deferred or another thread's immediate). Writes via that context never sync. Verify by logging the device/context Unity actually uses for its render commands (may need a wrapper proxy device passed to xrCreateSession to introspect).

3. **Unity skips `xrEnumerateSwapchainImages` entirely** when using its OpenXR plugin's "swapchain-images-already-known" optimization. Unity may map render textures based on `XR_KHR_D3D11_enable` graphics binding info before our hook runs.

### Concrete next-step diagnostics (in plugin)

Add these to `D3D11Backend`:
- Hook `xrCreateSwapchain` to log the typeless_sc XR handle returned to Unity AND the typed_sc handle the plugin allocates. Then in our hooked `xrEnumerateSwapchainImages`, log when Unity actually calls it for each handle. Cross-reference: did Unity ever call `xrEnumerateSwapchainImages` on typeless_sc?
- If yes, log the texture pointers our hook returned and the texture pointer Unity's writes would target (we can intercept Unity's render path via a D3D11 device proxy, but that's invasive).
- If no, Unity uses a side channel — figure out which one (maybe Unity uses the `XrSwapchainImageBaseHeader` from a different OpenXR call).

## Cumulative plugin patches (in `unity-3d-display`, branch unmodified main)

All under `[#184]` tag in `native~/displayxr_d3d11_backend.cpp::D3D11Backend::end_frame_apply`. Diagnostic-only, no behavior change for non-orange content (sentinel is painted by the runtime side, not the plugin).

- PROBE 1 (atlas_tex pre-write readback) — keep, useful
- PROBE 2 (atlas_tex post-Copy readback) — keep, useful
- PROBE 3 (device-id MATCH check) — keep, useful
- PROBE 4 (copy-args dump) — keep, useful
- PROBE 5 (src_tex content readback) — keep, useful

These all use one-shot static gates so per-frame cost is zero.

## Cycle 9 — Plugin xrEnumerateSwapchainImages tracing + post-release readback (2026-04-28) — RESULT: render-context mismatch confirmed; root cause is keyed-mutex-vs-render-thread race

**Two new probes added to `displayxr_d3d11_backend.cpp`:**

- **PROBE 6** in `handle_enumerate_swapchain_images`: log every call regardless of substitution status (sc handle, capInput, out_count ptr, images ptr, substituted YES/NO). Throttled to 16 calls.
- **PROBE 7** in `handle_release_swapchain_image`: after `context->Flush()` and BEFORE the plugin's typed_sc release, CPU-readback the typed sibling at `sub->typed_textures[sub->current_idx]` via the plugin's captured device. One-shot per typed_sc.

**Key results:**

```
[#184] plugin enum_sw_images call[0]  sc=0x...95B0 capInput=0 ... substituted=YES
[#184] plugin enum_sw_images call[1]  sc=0x...95B0 capInput=1 ... substituted=YES   ← actual fetch, returns typed[0]=0x...4020
[#184] plugin enum_sw_images call[2]  sc=0x...D160 capInput=0 ... substituted=YES
[#184] plugin enum_sw_images call[3]  sc=0x...D160 capInput=1 ... substituted=YES   ← actual fetch, returns typed[0]=0x...09A0
[#184] plugin enum_sw_images call[4..15]: capInput=0 (count-only re-queries)
[#184] plugin PROBE7 (post-release-Flush) typed_sc=0x...C860 tex=0x...34A0 1920x1080 TL=0xff00bcff CTR=0xff00bcff
[#184] plugin PROBE7 (post-release-Flush) typed_sc=0x...BB40 tex=0x...47E0 1920x1080 TL=0xff00bcff CTR=0xff00bcff
```

Unity DID receive the typed sibling pointers from our hook (same pointers that PROBE 4 reported as `src_tex` for the copy). After Unity's `xrReleaseSwapchainImage` + the plugin's `context->Flush()`, the typed sibling is **still 100% orange sentinel** — Unity's writes are not visible to the plugin's captured Unity-immediate-context.

## Root cause — render-context mismatch + keyed-mutex protocol race

Unity 6 by default uses **multi-threaded rendering** (graphics jobs). Unity's main thread submits high-level render commands; Unity's render thread holds the device's immediate context (or a deferred context whose work is later executed against the immediate context) and actually issues the D3D11 calls. The plugin captures `device->GetImmediateContext()` at session-bind time (`displayxr_d3d11_backend.cpp:202`) — but at that moment Unity's render thread isn't necessarily holding it yet, and even later when the plugin calls `Flush()` on this `context` reference, it doesn't drain Unity's render-thread work that's queued elsewhere or not-yet-submitted.

Now apply this to the keyed-mutex protocol:

1. Unity's main thread → `xrAcquireSwapchainImage` (just gets index)
2. Unity's main thread → `xrWaitSwapchainImage` → runtime IPC client `KeyedMutexCollection::waitKeyedMutex` → `AcquireSync(0)` on its own context.
3. Unity's main thread enqueues render work for the render thread.
4. Unity's main thread → `xrReleaseSwapchainImage` → plugin hook's `context->Flush()` (captured-context only — does NOT drain Unity's render-thread work) → `s_real_release_swapchain_image(unity_sc)` → runtime IPC client `releaseKeyedMutex` → `ReleaseSync(0)`.
5. Service later → `AcquireSync(0)` on its own device → succeeds → reads texture → **sees orange because Unity's render thread hasn't actually executed any writes against this resource yet.**
6. Some time later, Unity's render thread executes the queued commands — **the keyed mutex is no longer held by Unity's side**, the texture state is "released, key 0", and Unity's writes either go to a non-current state or are silently dropped by the D3D11 SHARED_KEYEDMUTEX semantics.

Standalone Unity has no SHARED_KEYEDMUTEX gate, so Unity's render thread eventually executes the commands, the writes land in the texture, the (in-process) runtime later reads from that texture and sees Unity content. There's no cross-process handoff race.

Cube works in shell because cube renders entirely on the main thread's immediate context — there's no "Unity render thread" sitting between the writes and the keyed mutex transitions. Cube's writes are on the GPU command queue before `xrReleaseSwapchainImage` returns, and `ReleaseSync` correctly fences them.

## Concrete fix directions (ranked)

1. **Unity-side workaround (lowest risk):** disable graphics jobs in `PlayerSettings → Other Settings → Graphics Jobs`. This forces Unity into single-threaded rendering — the same model cube uses. Worth testing as a one-cycle confirmation that the render-thread hypothesis is correct. If Unity then renders correctly in shell, the diagnosis is locked.
2. **Plugin-side fix (medium):** in the plugin's `handle_release_swapchain_image`, instead of calling `context->Flush()` on the captured immediate context, issue a real GPU sync — create an `ID3D11Query{D3D11_QUERY_EVENT}`, issue `End` on every D3D11 device-context Unity is using, then poll `GetData` until all queries complete. This drains Unity's render-thread work before the runtime sees the release. Heavy, but mechanically possible. (May need to wrap Unity's device with a proxy to discover all contexts in use.)
3. **Runtime-side fix (most invasive but bulletproof):** in `KeyedMutexCollection::releaseKeyedMutex`, before `ReleaseSync(0)`, issue a device-wide GPU fence and wait for completion. This guarantees ALL GPU work on the device is done before the mutex releases, regardless of which context queued it. Adds latency to every shell-mode release.

Recommend **option 1 first** as a 5-minute test. If Unity renders correctly with graphics jobs off, the diagnosis is locked. Then implement option 2 or 3 as the production fix.

## Cumulative plugin probes (in `unity-3d-display`, branch unmodified main)

All under `[#184]` tag in `displayxr_d3d11_backend.cpp`. One-shot or throttled — zero per-frame cost.

- PROBE 1 (atlas_tex pre-write readback) — `D3D11Backend::end_frame_apply` line ~470
- PROBE 2 (atlas_tex post-Copy readback) — `D3D11Backend::end_frame_apply` line ~590
- PROBE 3 (device-id check) — line ~485
- PROBE 4 (copy-args dump) — line ~520
- PROBE 5 (src_tex content readback) — line ~530
- PROBE 6 (every xrEnumerateSwapchainImages call) — `handle_enumerate_swapchain_images` line ~262
- PROBE 7 (typed sibling content right after Unity's release+Flush) — `handle_release_swapchain_image` line ~340

Plus diagnostic infrastructure recreated for the test project:
- `Documents/Unity/DisplayXR-test-Unity/DisplayXR-test/Assets/Editor/BuildScript.cs` — minimal Unity batchmode build script. Headless rebuild via `Unity.exe -batchmode -quit -nographics -projectPath ... -executeMethod BuildScript.BuildWindows64`.

## Cycle 10 — Graphics Jobs OFF + force D3D11 (2026-04-28) — RESULT: hypothesis REFUTED, typed sibling stays orange

**Setup:**
- Updated `Assets/Editor/BuildScript.cs` to expose two methods:
  - `BuildScript.BuildWindows64` — original
  - `BuildScript.BuildWindows64NoGfxJobs` — sets `PlayerSettings.graphicsJobs = false` and forces `PlayerSettings.SetGraphicsAPIs(StandaloneWindows64, [Direct3D11])` (the project default was auto-pick which selected D3D12 in Unity 6).
- Headless rebuild via batchmode, deployed patched plugin DLL into the new build's `DisplayXR-test_Data/Plugins/x86_64/`.

**Build log confirms:**
```
[BuildScript] GraphicsAPIs (StandaloneWindows64) before: [Direct3D12,Direct3D11] (auto=True)
[BuildScript] GraphicsAPIs (StandaloneWindows64) forced to: Direct3D11
[BuildScript] PlayerSettings.graphicsJobs before: True
[BuildScript] PlayerSettings.graphicsJobs set to false for [#184] test
[BuildScript] PlayerSettings.graphicsJobs at build time: False
```

**Plugin runtime log confirms D3D11 path is hit and the bug persists:**
```
[DisplayXR] Graphics binding: D3D11
[#184] plugin enum_sw_images call[1] sc=...95B0 capInput=1 ... substituted=YES
[#184] plugin enum_sw_images call[3] sc=...D160 capInput=1 ... substituted=YES
[#184] plugin PROBE7 (post-release-Flush) typed_sc=... tex=... 1920x1080 TL=0xff00bcff CTR=0xff00bcff
[#184] plugin PROBE7 (post-release-Flush) typed_sc=... tex=... 1920x1080 TL=0xff00bcff CTR=0xff00bcff
[DisplayXR] Atlas swapchain created: sc=... 3840x1080 ... mode=1
[#184] plugin PROBE3 device-id check: ... (MATCH)
[#184] plugin PROBE1 (post-acquire, pre-write) atlas_tex=... TL=0xff00bcff ... CTR=0xff00bcff
[#184] plugin PROBE5 (src_tex content) src_tex=... TL=0xff00bcff CTR=0xff00bcff BR=0xff00bcff
[#184] plugin PROBE5 (src_tex content) src_tex=... TL=0xff00bcff CTR=0xff00bcff BR=0xff00bcff
[#184] plugin PROBE2 (post-CopySubresourceRegion+Flush) atlas_tex=... TL=0xff00bcff ... CTR=0xff00bcff
```

Atlas screenshot: still 100% orange (sentinel preserved everywhere).

**Implication: the multi-threaded-rendering / `ReleaseSync`-fences-only-its-own-context hypothesis from Cycle 9 is wrong.** Even with single-threaded immediate-context Unity rendering, Unity does not write to the typed sibling textures that the plugin substitutes via the hooked `xrEnumerateSwapchainImages`.

**Revised root-cause space — what's still on the table:**

1. **Unity uses an internal swapchain-image cache that bypasses our hook** for actual rendering. Unity received the typed sibling pointers (proven by PROBE 6) but renders to a **different** texture — possibly the typeless swapchain's images obtained via a separate code path (Unity's OpenXR provider may call an internal Unity API that goes around our hook), or a Unity-private off-screen render texture that's never copied to the shared swapchain.
2. **Unity creates an SRGB-typed RTV over the imported `SHARED_KEYEDMUTEX` typed-sibling texture and the bind silently fails** (e.g., a `BindFlags` mismatch or a permissions issue specific to imported NT-handle resources from the Unity render-system's perspective). Writes go nowhere.
3. **Unity fails to handle the typed-sibling pointers altogether and bails out of rendering** for these swapchains, but doesn't log it — Unity's render pipeline silently falls through to a no-op for "broken" textures.

To distinguish, the next probes should target **typeless_sc directly**:
- Sample typeless_sc's actual images (NOT typed_sc) right after `xrReleaseSwapchainImage`. If those have Unity content, hypothesis 1 is locked: Unity renders to typeless not typed. Then the fix is plugin-side — instead of substituting via `xrEnumerateSwapchainImages` rerouting, make the typeless_sc images share storage with typed_sc somehow.
- **PROBE 8 was attempted but crashed Unity** (calling `s_real_enumerate_swapchain_images(typeless_sc, ...)` from inside the release hook closes Unity's IPC pipe — see `bjhsyf68s.output`: "WriteFile on pipe ... failed: 232 The pipe is being closed"). PROBE 8 was reverted; a safer rewrite would do the typeless_sc enumerate ONCE in `on_swapchain_created` (right after pairing), cache the typeless texture pointers in `D3D11ScSub`, and only sample them later in `handle_release_swapchain_image`.

## Cumulative plugin probes (in `unity-3d-display`, branch unmodified main)

PROBE 8 reverted. Remaining: PROBE 1–7 as before. Plus the test-app/build-script infrastructure:
- `unity-3d-display/native~/displayxr_d3d11_backend.cpp` — PROBE 1, 2, 3, 4, 5 in `D3D11Backend::end_frame_apply`; PROBE 6 in `handle_enumerate_swapchain_images`; PROBE 7 in `handle_release_swapchain_image`.
- `Documents/Unity/DisplayXR-test-Unity/DisplayXR-test/Assets/Editor/BuildScript.cs` — `BuildWindows64` and `BuildWindows64NoGfxJobs`. Both force `Direct3D11` graphics API for the Standalone target. The "no graphics jobs" variant additionally sets `PlayerSettings.graphicsJobs = false`.

## Cycle 11 — Safe PROBE 8 (typeless_sc content, cached at pairing time) (2026-04-28) — RESULT: bug is upstream of swapchain images entirely

**Setup:**
- Added `typeless_textures[8]` and `typeless_img_count` to `D3D11ScSub` (in `displayxr_hooks_internal.h`).
- In `on_swapchain_created`, after pairing succeeds, call `s_real_enumerate_swapchain_images(unity_sc, ...)` ONCE to fetch typeless_sc's actual image pointers and cache them on the sub. Avoids the IPC race that crashed Unity in the previous PROBE 8 attempt.
- In `handle_release_swapchain_image`, sample `sub->typeless_textures[sub->current_idx]` via the standard staging-copy + Map pattern.

**Plugin log:**
```
[#184] cached typeless_sc[0] tex=...8B60 fmt=29 1920x1080 for unity_sc=...6030
[#184] cached typeless_sc[0] tex=...8E20 fmt=29 1920x1080 for unity_sc=...9960
[#184] plugin PROBE8 (typeless_sc content) unity_sc=...6030 tex=...8B60 1920x1080 fmt=29 TL=0xff00bcff CTR=0xff00bcff BR=0xff00bcff
[#184] plugin PROBE8 (typeless_sc content) unity_sc=...9960 tex=...8E20 1920x1080 fmt=29 TL=0xff00bcff CTR=0xff00bcff BR=0xff00bcff
[#184] plugin PROBE7 (post-release-Flush) typed_sc=... tex=... TL=0xff00bcff CTR=0xff00bcff
[#184] plugin PROBE5 (src_tex content) ... TL=0xff00bcff
```

- **typeless_sc images are 100% orange.**
- typed_sc images are 100% orange.
- atlas is 100% orange.
- Unity does NOT write to ANY OpenXR-allocated texture.

**Also revealed by PROBE 8:** Unity requested `format=29` (SRGB) for `xrCreateSwapchain` directly. The plugin's "typed-sibling" substitution then creates a duplicate `format=29` swapchain. Both unity_sc and typed_sc are fmt=29 SRGB. **The substitution is a no-op in current Unity** — the comment in `on_swapchain_created` ("Pair... to the Unity-facing TYPELESS color swapchain") is stale, predating modern Unity's SRGB-direct request behavior. The substitution layer is now dead weight that adds a duplicate swapchain allocation per eye and complicates the keyed-mutex/release flow without benefit.

## Final root cause space

Unity is rendering to **a Unity-private off-screen texture** that is NOT either of the OpenXR-allocated shared textures (typeless_sc or typed_sc). At some point Unity's OpenXR plugin should be doing a Blit/CopyResource from its private RT into the swapchain image — and that final write step is where the bug lives. Three remaining hypotheses, in order of likelihood:

1. **Unity's blit-to-swapchain step happens on a context that doesn't reach the GPU before `xrReleaseSwapchainImage` returns** (the runtime then immediately `ReleaseSync`s the keyed mutex; service later acquires and reads the not-yet-written texture). Standalone has no shared-texture handoff so Unity's lazy-executing blit eventually lands and is read in-process. Even with `PlayerSettings.graphicsJobs = false` (Cycle 10), Unity may still queue render commands on its own command-buffer pipeline that flushes asynchronously.
2. **Unity's blit-to-swapchain step never executes for `SHARED_KEYEDMUTEX | SHARED_NTHANDLE` imported textures** because Unity validates the texture's bind/misc flags and silently skips the blit when they don't match Unity's expectations. Standalone gets non-shared textures from the in-process compositor, Unity's blit fires normally.
3. **Unity's OpenXR plugin uses a separate Unity-internal swapchain mechanism that intercepts the OpenXR runtime via Unity's own provider layer**, so the texture pointers we return from `xrEnumerateSwapchainImages` aren't the ones Unity's render pipeline ultimately uses. Unity's OpenXR provider source: `com.unity.xr.openxr` package, `Runtime/Native/UnityOpenXR.bundle` etc. — needs reading.

The next concrete diagnostic step is no longer in our plugin — it's in Unity's OpenXR provider source. Specifically:
- Read `Library/PackageCache/com.unity.xr.openxr@1.16.1/Runtime/...` to find where Unity picks up swapchain image pointers and where the final blit/copy to the OpenXR-allocated swapchain image happens.
- Confirm whether Unity does `Graphics.Blit(unityRT, swapchainTex)` or similar — and on what context.

The proper fix is most likely either:
- **Plugin-side: remove the typed-sibling substitution entirely.** It's dead weight in current Unity. Then atlas-composite reads from unity_sc images directly. If unity_sc images STILL stay orange, the bug is purely Unity-side (option 3). If they get Unity content with the substitution removed, the substitution itself is what's confusing Unity's OpenXR provider.
- **Runtime-side: alternative compositor topology** that doesn't rely on Unity actually writing to the swapchain image — e.g., a back-channel sharing mechanism that gets Unity's RT directly.

## Recommendation

Pause plugin/runtime cycles. Either:
1. **Remove the dead typed-sibling substitution** (5-line surgery in `on_swapchain_created` — `return` before pairing if `createInfo->format == 29`). One cycle. If Unity then renders correctly in shell, the substitution layer was confusing Unity. If still broken, definitive proof Unity needs an internal-provider-layer fix.
2. **Read `com.unity.xr.openxr` source** to understand Unity's blit-to-swapchain path. Out-of-scope for this run; would feed a future fix.

Recommendation #1 is a 30-second cycle. Strongly suggest doing it before pausing.

## Cumulative plugin probes (in `unity-3d-display`, branch unmodified main)

PROBE 1 through 8 all in place. Plus typeless texture caching infra in `D3D11ScSub`.

## Cycle 12 — Skip typed-sibling substitution for SRGB-direct clients (2026-04-28) — RESULT: substitution is NOT the cause; bug is purely Unity↔shared-texture interaction

**Patch:** in `on_swapchain_created` (`displayxr_d3d11_backend.cpp:231`), early-return BEFORE pairing if `createInfo->format == 29` (SRGB). Unity's own swapchain handle is then exposed unmodified to Unity. Plugin's atlas-composite path doesn't fire (no `D3D11ScSub` records). Runtime compositor sees Unity's per-view swapchains directly.

**Plugin log confirms the skip and that no substitution is in play:**
```
[#184] cycle 12: SKIPPING typed-sibling substitution for SRGB-direct unity_sc=...0370
[#184] cycle 12: SKIPPING typed-sibling substitution for SRGB-direct unity_sc=...1390
[#184] plugin enum_sw_images call[0] sc=...0370 ... substituted=NO
[#184] plugin enum_sw_images call[1] sc=...0370 capInput=1 ... substituted=NO
... (no Atlas swapchain created, no atlas composite OK)
```

**Service log shows Unity's OWN swapchain images stay orange across 420 frames:**
```
sentinel paint: sc=...D6E0 fmt=29 1920x1080  ← Unity's eye-0 swapchain
sentinel paint: sc=...CD20 fmt=29 1920x1080  ← Unity's eye-1 swapchain
readback n=0   eye=0 sc=...D6E0 ... TL=0xff00bcff CTR=0xff00bcff
readback n=0   eye=1 sc=...CD20 ... TL=0xff00bcff CTR=0xff00bcff
... readback at n=60, 120, 180, 240, 300, 360, 420 — all identical 0xff00bcff
```

Atlas screenshot: full orange.

## Final diagnosis

The bug is **NOT in any of these locations** (each definitively ruled out by a probe):

| Suspect | Cycle | Verdict |
|---|---|---|
| Cross-process kernel-object identity (NT handle path) | 8 | Plugin PROBE 1 sees orange — handle import is correct, plugin opens the same kernel object the service owns |
| AppContainer SA on `CreateSharedHandle` | 3 | Removed — no change |
| Per-client atlas format / runtime atlas format | (prior agent) | Both UNORM and TYPELESS atlas tried — no change |
| Storage format of the swapchain texture | 5, 6 | TYPELESS, UNORM, SRGB all fail when **request** is SRGB |
| Runtime D3D11 client compositor / IPC chain | 7 | `cube_handle_d3d11_win` with `DISPLAYXR_TEST_FORCE_SRGB=1` writes correctly cross-process — runtime path works |
| Plugin's typed-sibling substitution layer | 8, 12 | PROBE 8 shows unity_sc images also stay orange. Cycle 12 removes substitution entirely — Unity STILL doesn't write to its own swapchain |
| Plugin↔Unity device mismatch | 8 | PROBE 3 says MATCH |
| Unity multi-threaded rendering / graphics jobs | 10 | `PlayerSettings.graphicsJobs = false` — typed sibling still orange |

**The bug is in Unity's OpenXR provider's interaction with `SHARED_KEYEDMUTEX | SHARED_NTHANDLE` cross-process imported swapchain textures.** Unity's render pipeline either:

1. Silently rejects the texture for RTV creation (some flag mismatch unique to imported NT-handle resources from Unity's PoV).
2. Renders to the texture but on a deferred context whose `ExecuteCommandList` happens after `xrReleaseSwapchainImage` (and `graphicsJobs=false` doesn't actually disable this — Unity's render pipeline has more layers of deferral).
3. Detects "this is a sandboxed/imported resource" and rerouts rendering to a Unity-internal texture with no copy back to the original.

Cube works in shell because cube doesn't go through Unity's render pipeline — it directly creates an RTV on the imported texture and draws via the immediate context. Unity standalone works because the in-process compositor allocates regular (non-shared) textures, so whatever defers Unity's writes is fine because the runtime reads from the same texture in the same process when the writes eventually land.

## Concrete shippable fix paths

**The fix cannot be in our runtime alone** (cube works in our runtime). Three architectural options:

1. **Force Unity to use the runtime's in-process compositor in shell mode.** This means routing Unity through `compositor/d3d11/comp_d3d11_compositor.cpp` instead of `compositor/d3d11_service/comp_d3d11_service.cpp` even when shell is active, then using a separate IPC mechanism (e.g., shared NT handle of Unity's final back-buffer that the shell reads at compose time, NOT a per-swapchain SHARED_KEYEDMUTEX). This restructures the shell's relationship with Unity-class clients.

2. **Add a Unity-aware shim in the runtime** that detects Unity sessions (via app name / OpenXR application info) and creates a non-shared "Unity-private" texture that Unity writes to, then internally Blit-copies into the SHARED_KEYEDMUTEX swapchain texture in the runtime IPC client (on a context the runtime controls) before signalling release. This adds a copy per frame but bypasses whatever Unity does that drops the writes.

3. **Reach out to Unity / read `com.unity.xr.openxr` source** to identify the specific code path that drops imported SHARED_KEYEDMUTEX texture writes, and propose a Unity-side fix or workaround. The plugin source lives in `Library/PackageCache/com.unity.xr.openxr@1.16.1/Runtime/`.

## Recommendation

This is the right place to pause for product / architecture direction. The diagnosis is locked: bug is in Unity's render-pipeline-vs-imported-shared-texture interaction. The four fix-direction options (above + "remove substitution permanently in plugin", which Cycle 12 already shows is safe and harmless but doesn't fix the underlying bug) all need a product call before more code lands.

## Cumulative plugin probes (in `unity-3d-display`, branch unmodified main)

PROBE 1–8 in `displayxr_d3d11_backend.cpp` + Cycle 12 substitution-skip + typeless-cache infra in `D3D11ScSub`. All diagnostic-only or behavioral-skip; revert before any production merge.

## Cycle 13 — Cross-API confirmation: cube_d3d12 SRGB in shell (2026-04-28) — RESULT: runtime D3D12 IPC path is sound, bug is symmetrically Unity-only

**Test:** `set DISPLAYXR_TEST_FORCE_SRGB=1 && _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d12_win\build\cube_handle_d3d12_win.exe`. The DXGI-format selector in `xr_session_common.cpp::CreateSwapchain` (already in place from Cycle 7) picked DXGI 29 (SRGB) instead of the runtime's preferred UNORM.

**App log:**
```
[#184] DISPLAYXR_TEST_FORCE_SRGB=1 — searching for SRGB format in list
[#184] picked SRGB format 29 at index 1
Selected swapchain format: 29 (0x1D)
```

**Result:** atlas screenshot shows the wooden D3D12 cube on a dark blue grid floor in two side-by-side multiview tiles (window title `D3D12 Cube` visible). Cube content rendered through the runtime D3D12 IPC chain when explicitly requesting SRGB swapchains.

## Cross-API summary

| Test | API | Format | Result |
|---|---|---|---|
| Cycle 7 | D3D11 | UNORM (default) | renders ✓ |
| Cycle 7 | D3D11 | SRGB (forced) | **renders ✓** |
| Cycle 13 | D3D12 | SRGB (forced) | **renders ✓** |
| All Unity cycles | D3D11 | SRGB (Unity default) | orange (broken) |
| Prior agent | D3D12 | SRGB (Unity default) | broken |

The runtime's D3D11 AND D3D12 IPC chains both correctly handle SRGB-requested cross-process shared swapchain textures **when the client renders via standard immediate-context patterns**. Both APIs fail only when Unity's render pipeline is the writer.

## Definitive diagnosis (post-Cycle 13)

The bug is **symmetric across D3D11 and D3D12** and lives in **Unity's render pipeline's interaction with imported cross-process shared swapchain textures**. Mechanism differs per API (D3D11 = `SHARED_KEYEDMUTEX`, D3D12 = `ID3D12Fence`), but Unity's high-level render pipeline (SRP / RenderGraph / similar) hits the same architectural pattern: it does *something* when targeting an imported shared resource that prevents the writes from being visible to other contexts/processes.

**Implication for fixes:**

API-specific debugging (more PROBES on D3D12) won't yield a different fix — the bug is one architectural pattern, just expressed twice. The two architectural fix paths from Cycle 12 both apply to BOTH APIs once implemented in parallel:

1. **Route Unity-class clients through the in-process compositor in shell mode** + back-channel handoff for shell to read Unity's final back-buffer. Independent of per-API texture sharing primitives.
2. **Runtime-side Unity-private texture + Blit copy at release.** Allocate a non-shared texture per swapchain image; Unity renders into the non-shared one (writes work because no cross-process gate); runtime IPC client Blit-copies into the SHARED swapchain texture inside `xrReleaseSwapchainImage` on a context the runtime controls. One extra GPU Blit per eye per frame. Implementation lands in `comp_d3d11_client.cpp` and `comp_d3d12_client.cpp` in parallel with similar shape.

Option 2 is the cleaner path — localized changes in the runtime IPC client, no shell architecture change, parallel D3D11 + D3D12 implementations. Estimated 1-2 days each.

## Recommendation

Stop adding diagnostic probes. The diagnosis is locked across both APIs. Move to architectural fix design — pick option 2 from Cycle 12 (runtime-side Unity-private + Blit), spec it for both D3D11 and D3D12 in one design doc, then implement in parallel.

## Cumulative plugin probes (in `unity-3d-display`, branch unmodified main)

PROBE 1–8 + Cycle 12 substitution-skip + typeless cache infra. All in `displayxr_d3d11_backend.cpp` + `displayxr_hooks_internal.h::D3D11ScSub`. Plugin DLL deployed to both Unity test exe locations (`Test-D3D11/` and `DisplayXR-test/Build/`). Diagnostic-only or behavioral-skip; revert before any production merge.

## Cumulative `[#184]` patches still on branch

All diagnostic-only:

- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`:
  - `#include <set>` and `#include <map>` added (line ~72).
  - `get_unorm_format` helper (line ~1140), inverse of `get_srgb_format`. Currently unused in shipping path; kept for future cycles.
  - `requested_dxgi_format` field on `d3d11_service_swapchain` struct (~line 162).
  - Set `requested_dxgi_format = xrt_format_to_dxgi(info->format)` in `compositor_create_swapchain` (~line 2575) and `compositor_import_swapchain` (~line 2795).
  - Sentinel paint after `CreateTexture2D` for any RENDER_TARGET swapchain — orange `(1.0, 0.5, 0.0, 1.0)`. Logs `[#184] sentinel paint: ...`.
  - At end of `compositor_create_swapchain`, log `[#184] svc adapter LUID ...` (gated on srgb).
  - In `compositor_layer_commit` per-view loop, the SRGB-intent override on `view_descs[eye].Format` is currently DISABLED (Cycle 6 found it incompatible with UNORM storage; commented out). Reinstate if storage rewrite is reattempted with TYPELESS.
  - **Periodic** CPU readback per source texture (gated by `static std::map<ID3D11Texture2D*,uint32_t> readback_count` + mutex) on `is_srgb_format(requested_dxgi_format)`; fires at frame 0 then every 60th up to 480. Logs `[#184] readback n=... eye=... ...`.
- `src/xrt/compositor/client/comp_d3d11_client.cpp`:
  - `[#184] client OpenSharedResource1 OK: NT=... tex=... fmt=... LUID=...` after success at line 369 (via `OutputDebugStringA` → DebugView, NOT the plugin's `displayxr.log`).
  - `[#184] client LEGACY OpenSharedResource(DXGI) fired: ...` after success at line 402.

Strip all of these before merging any real fix.

## Memory hooks already saved (will load automatically)

- `feedback_branch_workflow.md` — work on a fix branch, never main.
- `reference_screenshot_temp_permission.md` — long-form path; user pre-authorized %TEMP% screenshot artifacts.
- `reference_runtime_screenshot.md` — current screenshot mechanism (ATLAS flag → `_atlas.png`).
- `reference_service_log_diagnostics.md` — service log location + one-shot BRIDGE BLIT / DP HANDOFF lines.
- `feedback_test_before_ci.md` — wait for user to test before pushing or running /ci-monitor.
- `feedback_pr_workflow.md` — don't auto-merge; user reviews + merges, prefer rebase.
- `feedback_dll_version_mismatch.md` — push runtime DLLs to Program Files\DisplayXR\Runtime after each build (not the installer).

Good luck.
