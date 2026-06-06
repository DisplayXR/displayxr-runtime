# Phase 0 — D3D11 masked-composite mechanism (implementation plan)

**Branch:** `feature/unified-2d-3d-compositing` (worktree `.claude/worktrees/unified-2d3d`)
**Spec:** [`docs/roadmap/unified-2d-3d-compositing.md`](docs/roadmap/unified-2d-3d-compositing.md) §4, §6 (Phase 0). Epic #439.
**Goal:** stand up the general masked-2D-over-3D composite as a runtime-compiled shader pass, prove it **pixel-identical** to today's rectangular `d3d11_blit_surround_strips`, behind an opt-in toggle. Zero default behavioral change.

**STATUS (drafted, UNBUILT):** shader + full renderer/compositor wiring written on this branch — **Option A (scratch copy) chosen** (§0). Not yet compiled (authored on macOS). Next: the Windows build loop (§6) — `build_windows.bat build` → fix compile errors → A/B capture diff. Default path (env unset) is untouched, so the draft is safe to carry until then.

> **BUILD NOTE:** D3D11 only compiles on Windows (`scripts\build_windows.bat build`). This worktree is macOS — the shader + this plan are authored here; the `.cpp` wiring + the pixel-diff validation run in a Windows session on the Leia machine. The shader (`shaders/masked_composite.hlsl`) and embedded-source string must stay byte-identical.

---

## 0. Design fork to resolve FIRST — surround texture is sample-capable?

The existing surround (`c->surround_texture`) is **`OpenSharedResource`'d from the app's handle** and used only as a `CopySubresourceRegion` *source*, which needs **no particular bind flag**. Sampling it in a pixel shader needs **`D3D11_BIND_SHADER_RESOURCE`** on that resource — which the app may not have set.

This means "re-express the surround strip-copy as a shader sample" is **not free**: it needs the 2D source to be SRV-capable. Three resolutions (pick one — affects the spec):

| Option | What | Cost | Spec impact |
|---|---|---|---|
| **A. Scratch copy** ✅ **CHOSEN** | `CopyResource` surround → an internal SRV-capable scratch texture (runtime-allocated, `BIND_SHADER_RESOURCE`), then the shader samples the scratch | +1 copy/frame (removed once the 2D side is a runtime-allocated layer in Phase 3) | none |
| **B. Require SRV on surround** | `XR_EXT_win32_window_binding` §3.6 mandates the app create the surround with `D3D11_BIND_SHADER_RESOURCE` | zero extra copy | spec bump + breaks existing surround apps until they add the flag |
| **C. Defer** | leave `d3d11_blit_surround_strips` untouched; only exercise the shader pass against a runtime-allocated 2D layer (Phase 1), never against the app surround | zero | none, but Phase 0 no longer "re-expresses surround" — it validates the shader against a synthetic source |

**Recommendation: A.** Keeps the existing surround contract intact, proves the shader pass against the *real* surround bytes (scratch holds an exact copy → 1:1 sample → pixel-identical), and the extra copy naturally evaporates in Phase 3 when the 2D layer is runtime-owned and SRV-capable from birth. The spec's Phase 0 wording ("re-express ... pixel-identical") holds with A; note the transient extra copy in §6 of the spec.

---

## 1. Where the pass lives — the renderer

GPU-pipeline state (runtime `D3DCompile`, `sampler_point`, `blend_opaque`, CB create/`Map`) all live in `comp_d3d11_renderer.cpp`. The pass goes there as a new entry; the compositor (which owns `surround_texture`/`surround_mutex` and the dst RTV) drives it. Do **not** duplicate shader infra into the compositor.

## 2. Renderer additions (`comp_d3d11_renderer.{h,cpp}`)

1. **Embed the shader source.** Add `static const char *masked_composite_vs_source` / `..._ps_source` `R"(...)"` literals **byte-identical to `shaders/masked_composite.hlsl`** (VS entry `VSMain`/`vs_5_0`, PS `PSMain`/`ps_5_0`), next to the existing `projection_*`/`quad_*` sources.
2. **State fields** on the renderer struct: `ID3D11VertexShader *composite_vs; ID3D11PixelShader *composite_ps; ID3D11Buffer *composite_cb;` Reuse existing `sampler_point` and `blend_opaque` (Phase 0 writes are opaque — no blend; this is what guarantees byte-identity with the copy).
3. **CB struct** mirroring `cbuffer CompositeParams` (HLSL b0), 16-byte aligned:
   ```c
   struct CompositeParams {
       float dst_dims[2];       float canvas_origin[2];
       float canvas_size[2];    uint32_t use_rect_mask;  uint32_t _pad;
   };
   ```
4. **Create** (in `comp_d3d11_renderer_create`, beside the projection/quad compiles): `compile_shader(... masked_composite_vs_source ...)` → `CreateVertexShader`; same for PS; `CreateBuffer` a dynamic CB (`D3D11_USAGE_DYNAMIC`, `BIND_CONSTANT_BUFFER`, `CPU_ACCESS_WRITE`, `sizeof(CompositeParams)`). **Destroy** them in `comp_d3d11_renderer_destroy` (SAFE_RELEASE, beside `quad_*`).
5. **The pass** — new public fn:
   ```c
   xrt_result_t comp_d3d11_renderer_composite_2d_masked(
       struct comp_d3d11_renderer *r,
       ID3D11RenderTargetView *dst_rtv,        // weave target (already holds the weave)
       ID3D11ShaderResourceView *twod_srv,     // 2D layer (Phase 0: scratch copy of surround)
       uint32_t dst_w, uint32_t dst_h,
       int32_t cx, int32_t cy, uint32_t cw, uint32_t ch);  // canvas rect → Phase 0 mask
   ```
   Body: `Map`/fill the CB (`use_rect_mask = 1`, dims/rect as floats); `OMSetRenderTargets(1, &dst_rtv, nullptr)`; `RSSetViewports` full dst; `IASetPrimitiveTopology(TRIANGLELIST)`, null IA/VB (vertex pulled from `SV_VertexID`); `VSSetShader(composite_vs)`, `PSSetShader(composite_ps)`; `PSSetSamplers(0, sampler_point)`; `PSSetConstantBuffers(0, composite_cb)` (and VS if needed); `PSSetShaderResources(0, twod_srv)` (t0); `OMSetBlendState(blend_opaque)`; `Draw(3,0)`. The PS `discard`s inside the canvas → weave untouched; outside → writes the 2D sample at 1:1.

## 3. Compositor changes (`comp_d3d11_compositor.cpp`)

1. **Scratch SRV texture** (Option A): add `ID3D11Texture2D *surround_scratch; ID3D11ShaderResourceView *surround_scratch_srv;` Lazily (re)allocate to `dst_w×dst_h`, surround's format, `BIND_SHADER_RESOURCE`. Release in `d3d11_release_surround` + destroy.
2. **New gated path** beside the two `d3d11_blit_surround_strips` call sites (`~:1568` offscreen, `~:1638` windowed):
   ```c
   if (surround_shader_enabled) {  // getenv("DISPLAYXR_SURROUND_SHADER") once, cached
       // acquire surround mutex; CopyResource surround → scratch; release mutex;
       // comp_d3d11_renderer_composite_2d_masked(c->renderer, <dst_rtv>, scratch_srv, dst_w,dst_h, cx,cy,cw,ch);
   } else {
       d3d11_blit_surround_strips(c, dst, dst_w, dst_h, cx, cy, cw, ch);   // unchanged default
   }
   ```
   `<dst_rtv>`: windowed path → `comp_d3d11_target`'s RTV (already bound for the DP); offscreen path → `c->shared_rtv`. Both already exist — pass them in.
3. **Keep `d3d11_blit_surround_strips` intact** — it is the pixel-identity reference for the A/B diff and the safe default.

## 4. Pixel-identity guards (the subtle part)

- **Point sampler**, `uv` from the full-screen triangle → at each dst pixel center the interpolated `uv=(px+0.5)/dim`; surface dims equal (already enforced) → samples the exact surround texel. No filtering, no half-texel drift.
- **Opaque blend state** (no blend) → the PS output is written verbatim; outside-canvas pixels equal the surround bytes.
- **Format/gamma:** `CopySubresourceRegion` is byte-exact regardless of sRGB. A sample→write round-trips through the view's format semantics. If the surround/dst is an **sRGB-typed** view, sampling does sRGB→linear and the RTV write linear→sRGB — round-trip is ~identity but may differ ≤1 LSB. **Create the scratch SRV and the dst RTV with the UNORM (non-sRGB) typeless-compatible view of the same format** so the shader path is raw bytes, matching the copy. Verify the dst/surround resource is `TYPELESS` or non-sRGB; if it's sRGB-only, document the ≤1 LSB tolerance in the diff.

## 5. CMake

No change — `.hlsl` files are reference only (compiled at runtime from the embedded strings; see `CMakeLists.txt:56`). Keep `masked_composite.hlsl` in sync with the embedded source by convention.

## 6. Validation — A/B capture diff (Windows, Leia machine)

Use `cube_texture_d3d11_win` (a `_texture` app with a canvas sub-rect + surround). The compositor's `MCP_CAPTURE_MODE_POST_COMPOSE` path already dumps the final composited frame.

1. Build: `scripts\build_windows.bat build` then copy binaries to `C:\Program Files\DisplayXR\Runtime`.
2. **Capture A (reference, strip copy):** run the texture app *without* `DISPLAYXR_SURROUND_SHADER`; trigger a POST_COMPOSE capture → `A.png`.
3. **Capture B (shader composite):** `set DISPLAYXR_SURROUND_SHADER=1` (process-level env, per the CLAUDE.md IPC-env caveat) and rerun; capture → `B.png`.
4. **Diff:** `python3 - <<'PY'` with PIL/numpy, assert `max(|A-B|) == 0` (or `≤ 1` if the sRGB-view caveat in §4 applies). A clean zero proves the mechanism is a faithful generalization.
5. Eyeball the live Leia display in both modes (surround region crisp 2D, canvas region weaved 3D, boundary identical).

## 7. Done-when

- [ ] Shader embedded + compiles at runtime (no `D3DCompile` warnings).
- [ ] `DISPLAYXR_SURROUND_SHADER=1` produces a POST_COMPOSE capture pixel-identical to the default (within the documented tolerance).
- [ ] Default path (env unset) is byte-for-byte unchanged (strip copy) — no regression.
- [ ] `git clang-format` clean; runtime self-test still passes.

## 8. Hand-off to Phase 1

The shader already carries the Phase-1 path (`use_rect_mask=0` → sample `mask_tex` (t1), lerp against `weave_tex` (t2)). Phase 1 wires `XR_EXT_local_3d_zone`'s authored mask into t1 and the weave SRV into t2, and the gate flips from a rect to the real mask. The renderer entry point's signature gains the mask SRV + weave SRV then.
