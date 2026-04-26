# Bridge-Aware WebXR Color Shift in Shell — Plan

**Branch:** `feature/webxr-in-shell` (qwerty-freeze stack landed; pushed
to `origin/feature/webxr-in-shell`).
**Status:** Draft — design notes, not yet executed.
**Predecessor:** Qwerty-freeze stages 1–4c done. This is the next
follow-up from `webxr-in-shell-stage2-plan.md` §"Bridge-aware page color
shift in shell mode (pre-existing)" (lines 269–312 of that doc).

## Problem

Bridge-aware WebXR samples render with the wrong colors when hosted in
the spatial shell. Specifically, a deep-navy background

```js
scene.background = THREE.Color(0x0d0d40)   // ≈ linear (0.05, 0.05, 0.25)
```

displays as a brightened indigo on the Leia display (≈ 0.31 / 0.30 /
0.69 measured). The shift is consistent with **one extra gamma
transform** on the path — linear data being re-interpreted as sRGB-
encoded at one stage, then re-encoded again by the display processor.

The bug is **pre-existing**, not introduced by Stage 2 (verified by
reverting `comp_d3d11_service.cpp` to `4ab4f98ff` — the shift persists,
ruling out the scale-blit gate as the cause). It surfaces only when
**shell mode + bridge-aware** are combined.

Non-shell bridge-aware is correct. Non-bridge legacy WebXR in shell is
correct (after the qwerty-freeze stack). Handle apps in shell are
correct. Only this one combination is wrong.

## Path comparison (verified)

**Non-shell bridge-aware (correct):**
```
app swapchain
  → per-client atlas  (raw CopySubresourceRegion)
  → DP weaver
```
The display processor consumes the per-client atlas SRV directly. Works.

**Shell bridge-aware (purple):**
```
app swapchain
  → per-client atlas  (raw CopySubresourceRegion)
  → multi_compositor_render shader-blit  ← extra step
  → combined atlas
  → service_crop_atlas_for_dp
  → DP weaver
```
The combined-atlas pass and the subsequent crop are unique to shell mode.
That extra blit step is the most likely culprit.

The bridge-aware sample already passes the `needs_scale=0, srgb=1` Stage 1
diagnostic, so the per-client atlas itself is gamma-encoded **correctly**.
The drift happens at or after the multi-comp render.

## Code anchors (verify at impl time)

- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:4284-4300` —
  `combined_atlas` is created with `DXGI_FORMAT_R8G8B8A8_UNORM` (not
  SRGB). Default SRV / RTV are passed `nullptr` desc, so they inherit
  `R8G8B8A8_UNORM`. **No auto-linearize on sample, no auto-encode on
  write.**
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:6754-7106` —
  `multi_compositor_render` clears the combined atlas, then for each
  active client runs the blit shader (`PSMain` in
  `d3d11_service_shaders.h:628`) sampling the per-client `cc->render.atlas_srv`
  with `cb->convert_srgb = 0.0` (line 6820 / 7106 / etc.).
- `d3d11_service_shaders.h:705` — when `convert_srgb < 1.5`, shader does
  `src_tex.Sample(src_samp, input.uv)` and returns it directly.
  **No re-encode to sRGB.**
- `d3d11_service_shaders.h:622-626` — `linear_to_srgb(c)` exists but is
  only called when `convert_srgb` says so. The multi-comp blit doesn't
  invoke it.
- `comp_d3d11_service.cpp:3244-3357` — `service_crop_atlas_for_dp`
  optionally cropping/Y-flipping into a content-sized staging texture
  before the DP. Format `DXGI_FORMAT_R8G8B8A8_UNORM`, `convert_srgb=0`.

## Hypothesis

The extra multi-comp shader-blit step has **inconsistent SRV / RTV
typing along the chain** so a sample/write through the intermediate
combined-atlas applies one auto-(de)encode that the DP then mirrors,
yielding net `+1 gamma`. Three plausible causes (from the §270-312
analysis):

1. **One of the per-client atlases is created SRGB-typed** (or its SRV
   is SRGB-typed even though the storage is UNORM). Sampling via an SRGB
   SRV auto-linearizes; writing to a UNORM RTV stores the linearized
   value as if it were already sRGB-encoded; the DP weaver then encodes
   again. Net: linear → linear → sRGB-encode → looks washed-out.
   - For bridge-aware specifically, the bridge process does
     `xrCreateSwapchain` itself; its swapchain format choice (likely
     `R8G8B8A8_UNORM_SRGB` per most WebGL canvases) drives the
     per-client atlas typing.
2. **Combined-atlas RTV / SRV are SRGB-typed** somewhere by an
     undocumented path (e.g., a separate factory init line, vendored
     leiasr code path, or a debug toggle).
3. **DP weaver interprets bytes from the combined atlas differently**
   than from the per-client atlas (e.g., the SR SDK's `process_atlas`
   call has a per-input-format expectation).

Cause 1 is the most likely given (a) bridge-aware uses a different
swapchain code path than legacy WebXR (the bridge process owns its own
xr session), and (b) the bug is bridge-aware-specific in shell.

## Investigation plan

Before changing any code, **measure**. The diagnostic must clearly
separate the three causes.

### Diag-1. Format dump on the chain

Add a one-shot log in `multi_compositor_render` per active client that
prints, for the bridge-aware client specifically:

- `cc->render.atlas` `DXGI_FORMAT` (texture)
- `cc->render.atlas_srv` `DXGI_FORMAT` (`GetDesc().Format`)
- `mc->combined_atlas` `DXGI_FORMAT`
- `mc->combined_atlas_srv` `DXGI_FORMAT`
- `mc->combined_atlas_rtv` `DXGI_FORMAT`
- The crop texture / SRV `DXGI_FORMAT` (if `service_crop_atlas_for_dp`
  fires)

Compare to the same log for a legacy (non-bridge) WebXR client in shell.
If any format differs between the two — that's cause #1 confirmed.

### Diag-2. Pixel-pulldown probe

Use the existing file-trigger screenshot mechanism (`feedback_runtime_screenshot`,
`reference_runtime_screenshot`) to capture the combined atlas. Then
sample one pixel known to be `0x0d0d40` source. The captured byte
values reveal where in the chain the gamma drift accumulated:

- Bytes `(0x0d, 0x0d, 0x40)` → drift happens **after** combined atlas
  (cause #3 likely).
- Bytes around `(0x4f, 0x4d, 0xb1)` (linear of `0x0d0d40`, ≈ 0.31 / 0.30
  / 0.69) → drift happens **at or before** combined atlas (cause #1 or
  #2).

The screenshot path already writes a UNORM PNG, so what you see is what
the DP gets.

### Diag-3. Side-by-side

If feasible, run the bridge-aware sample twice — once with shell off
(non-shell direct DP), once with shell on. Same display, same DP, same
SR SDK. Only the multi-comp pipeline differs. Capture compositor atlas
in both cases.

## Likely fix paths (ranked, after measurement)

1. **Force consistent UNORM along the chain.** If diag-1 finds the
   per-client atlas SRV is SRGB-typed for bridge but UNORM for legacy,
   create a UNORM-typed SRV explicitly when the multi-comp samples it
   (don't pass `nullptr` desc to `CreateShaderResourceView`). Or write
   the blit shader to use `convert_srgb=1.0` for that input — sample as
   linear (auto-decode), then `linear_to_srgb` re-encode before writing
   the UNORM combined atlas. Mirrors what the DP would have seen if it
   read the per-client atlas directly.

2. **Mirror the non-shell path through the multi-comp.** If the DP
   weaver expects a specific gamma encoding (cause #3), match it
   explicitly in the multi-comp output. The crop staging texture's
   format / SRV typing is the right intervention point — it's the last
   surface the DP sees.

3. **Bypass the combined atlas for bridge-aware.** Most invasive — only
   if (1) and (2) prove unviable. The bridge already has the per-client
   atlas; in shell, send that directly to the DP for the bridge slot,
   skipping multi-comp compositing for it. Loses the multi-window
   composite for that client. Probably the wrong tradeoff — flag it
   only as a fallback option.

## Constraints

These are the hard rules to respect throughout — they came from prior
incidents (see memory entries):

- **`feedback_srgb_blit_paths`** — non-shell needs the **shader blit**
  (linearize for DP). Shell uses **raw copy**. **Never unify.** When
  modifying the shader-blit path in `multi_compositor_render`, do not
  refactor it into a single common helper that's used for non-shell
  too. The two paths have legitimately different format expectations.
- **`feedback_3d_mode_terminology`** — multiview-first language. No
  "stereo" / "left+right eye" / "SBS" in code, comments, logs, or
  commit messages. Use *tile / view / atlas / per-tile / per-view*.
- **`feedback_atlas_stride_invariant`** — slot stride =
  `atlas_width / tile_columns`. Three coupled call-sites:
  write, content-clamp, read. **No `sys->shell_mode` branches** around
  atlas dimensions. The color fix should not touch stride logic.
- **`feedback_use_build_windows_bat`** — build only via
  `scripts\build_windows.bat`. Never call cmake/ninja directly.
- **`feedback_dll_version_mismatch`** — copy all four binaries
  (`DisplayXRClient.dll`, `displayxr-service.exe`,
  `displayxr-shell.exe`, `displayxr-webxr-bridge.exe`) to
  `C:\Program Files\DisplayXR\Runtime\` after rebuild. Rename-then-copy
  the DLL if Chrome is still loaded.
- **`feedback_test_before_ci`** — wait for the user to confirm visually
  on the Leia display before pushing or running `/ci-monitor`.
- **`feedback_shell_screenshot_reliability`** — PrintWindow during eye-
  tracker warmup misses some UI; eyeball the live display, and use the
  file-trigger compositor screenshot (`reference_runtime_screenshot`)
  for atlas captures.

## Files likely touched

| File | Why |
|---|---|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp render; combined-atlas creation; `service_crop_atlas_for_dp`; format dump for diag-1. |
| `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` | The PSMain blit shader that consumes per-client atlas; possibly add a `convert_srgb=1.0` codepath for the bridge case. |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.h` | If a new helper or flag needs an interface declaration. |

No new headers. No IPC changes (the fix is server-side rendering only).
No state-tracker changes.

## Staging

1. **Diagnose.** Ship the format dump (diag-1) + a single one-shot log
   line per session listing the relevant `DXGI_FORMAT` values. No
   behavior change. Build, deploy, repro the bridge-aware page in shell,
   read the log. Confirm or rule out cause #1.
2. **Pixel probe.** If cause #1 isn't confirmed by the format dump, do
   diag-2 (file-trigger atlas screenshot, decode the test pixel). Adds
   ~no code if the trigger mechanism is already in place. Confirms
   where the drift accumulates.
3. **Fix.** Apply the path indicated by (1) or (2) — likely fix path 1
   (UNORM consistency) or fix path 2 (gamma re-encode in the crop). One
   commit per logical change. Verify on the bridge-aware sample, then
   the regression matrix.
4. **Regression sweep.** Run the post-qwerty-freeze regression matrix
   (see `webxr-qwerty-freeze-plan.md` §Verification) to ensure non-
   bridge sessions still render correctly.

## Verification matrix

Hardware: Leia SR + Chrome with the DisplayXR WebXR extension.

| # | Scenario | Expected |
|---|---|---|
| 1 | Bridge-aware sample in shell (`webxr-bridge/sample/`) | Background renders as deep navy, not brightened indigo. Match non-shell side-by-side. |
| 2 | Bridge-aware sample standalone (no shell) | Unchanged from today (was already correct). |
| 3 | Legacy WebXR in shell (`immersive-vr-session.html`) | Unchanged colors; Stage 4c qwerty-freeze behavior intact. |
| 4 | Legacy WebXR standalone | Unchanged colors. |
| 5 | Handle app (`cube_handle_d3d11_win`) in shell | Unchanged colors and convergence. |
| 6 | Handle app standalone | Unchanged. |
| 7 | Two-slot mix: bridge-aware + legacy WebXR in shell | Both correct; bridge-aware no longer drifts; legacy untouched. |
| 8 | Toggle camera/display rig (P key) during bridge-aware shell | No color flicker on transition; convergence plane unchanged (post stage 4c contract). |

## Pitfalls

1. **The two-path SRGB rule is intentional** — `feedback_srgb_blit_paths`
   exists because someone previously tried to unify and broke non-shell.
   If the fix tempts a refactor, resist; add a gated branch instead.
2. **Bridge-aware swapchain formats can differ from Chrome's WebGL
   default.** The bridge process is its own OpenXR client and chooses
   its own swapchain format from the runtime's
   `xrEnumerateSwapchainFormats` reply. Whatever that ends up being,
   that's what arrives at the per-client atlas. Don't assume.
3. **DP `process_atlas` is opaque.** The Leia SR SDK weaver is binary
   inside the runtime but its expectations are reverse-engineered in
   `comp_d3d11_service.cpp` and `leiasr_d3d11.cpp`. If the issue is
   cause #3 (DP-interpretation), the surface to inspect is the last
   thing handed to the SDK — the crop SRV, with its DXGI format and
   color-space attributes.
4. **Don't change `service_crop_atlas_for_dp` to a UNORM↔SRGB swap
   blindly.** That code path is shared by every shell-mode session,
   not just bridge. A wrong UNORM→SRGB assumption there could regress
   handle apps and legacy WebXR. Verify with diag-1 first.
5. **One extra gamma is a clean signature.** If the measured drift
   isn't ≈ `linear↔sRGB` at the test pixel, it's not the same bug —
   maybe contrast/blend or weaver/lenticular interaction. Don't chase
   color phantoms; let the measurement narrow the hypothesis first.
6. **Bridge process restart.** When the bridge child crashes or exits
   and is respawned, the swapchain format negotiation runs again. If
   format depends on bridge state, the bug may appear/disappear across
   restarts. Note this if the symptom proves intermittent.

## Definition of done

- [x] Bridge-aware WebXR in shell renders the test pixel `0x0d0d40` as
      deep navy (within 5% of the non-shell rendering).
- [x] Side-by-side bridge-aware shell vs non-shell matches visually.
- [x] All 8 regression-matrix rows pass on the live Leia display.
- [x] Two commits stacked on `feature/webxr-in-shell` (diag-1
      `7c0b7fc1a`, fix `48dbdc6b2`). Build green via
      `scripts\build_windows.bat build`.
- [x] No `feedback_srgb_blit_paths` rule violation: shell vs non-shell
      paths remain distinct.
- [x] Push to `origin/feature/webxr-in-shell` after user signs off
      (`feedback_test_before_ci`).

## Outcome

Resolved by commit `48dbdc6b2`. The fix took **path B** in this plan's
ranked candidates — match the DP's expectation at the multi-comp read
boundary — but with the read-side reinterpretation done via SRV typing
rather than a re-encode shader pass.

**What landed.** The shell-mode per-client atlas storage is now
`R8G8B8A8_TYPELESS` (was `R8G8B8A8_UNORM`). Two parallel SRVs view the
same bytes:

- `atlas_srv` (UNORM-typed) — used when the source swapchain bytes are
  already linear (UNORM swapchain, e.g. handle apps).
- `atlas_srv_srgb` (UNORM_SRGB-typed) — used when the source bytes are
  gamma-encoded (SRGB swapchain, e.g. Chrome with Three.js
  `outputColorSpace=SRGBColorSpace`).

A new per-client flag `atlas_holds_srgb_bytes` is set in
`compositor_layer_commit` from `view_is_srgb[0]` immediately after the
swapchain → atlas blit. `multi_compositor_render` selects the SRV at
sample time. With the SRGB SRV, the GPU auto-linearizes on sample, the
multi-comp blit shader (passthrough at `convert_srgb=0`) writes linear
values to the combined atlas, and the DP receives linear input as
expected.

**What stayed.** The swapchain → per-client atlas path in shell mode
remains a raw `CopySubresourceRegion` (byte-perfect, no race against
Chrome's keyed-mutex-protected swapchain release, no rasterizer state
churn). The `feedback_srgb_blit_paths` invariant — shell vs non-shell
pipelines stay distinct downstream — is preserved.

**Diagnostic findings worth recording.**
- diag-1 (one-shot per-slot DXGI_FORMAT dump) confirmed the chain is
  structurally a UNORM passthrough: per-client atlas, combined atlas,
  and crop staging all `R8G8B8A8_UNORM`, IPC blit shader bound with
  `convert_srgb=0.0`. Cause #1 (hidden SRGB SRV typing) ruled out.
- diag-2 (file-trigger combined-atlas screenshot) found the test pixel
  at `(0x40, 0x40, 0x89)` — the linear-of-source value sRGB-encoded
  once by Three.js's `outputColorSpace=SRGBColorSpace`. The
  passthrough chain preserved those gamma-encoded bytes verbatim and
  the DP weaver treated them as linear, producing the second encode at
  output (the visible double encode → brightened indigo).
- The `bridge_relay=no` row for the bridge-aware client surprised us:
  the bridge child is headless and produces no frames; Chrome's normal
  IPC compositor is what renders bridge-aware sessions, taking the
  exact same path as legacy WebXR. The color shift therefore affected
  legacy WebXR too — just less visibly because the legacy sample
  doesn't have a deep-navy reference color. Fixed for both paths.

**Path-A and path-C attempts (informative, not landed).** Two earlier
attempts moved the linearization to the swapchain-input boundary
(shader blit at `compositor_layer_commit` with the existing
`use_srgb_shader` path enabled in shell mode). They produced clean
colors but introduced a ghost-replicas regression: the shader blit's
GPU work wasn't guaranteed to complete before the keyed mutex was
released back to Chrome, leaving tile 1 stale on subsequent frames.
Path B avoids the boundary entirely — the SRV read happens against a
runtime-internal texture, not subject to keyed mutex or producer
contention.

## After this ships

Stage 4 (live resize during a session) is the last open item on
`webxr-in-shell-plan.md`. With this color fix landed, the parent plan
is ready to close once Stage 4 ships. There's also a known
intermittent service crash on window-close (captured under procdump in
`_dumps/`) — orthogonal, can be investigated independently.
