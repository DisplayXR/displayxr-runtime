# #491 part 3 — 2D-under-3D backdrop via the DP background capture (cross-repo design)

**Status:** design, ready to implement. Pre-implementation.
**Scope:** the native compositors (D3D11/D3D12/GL/VK on Windows; Metal/macOS behind) **+ `displayxr-leia-plugin`** (the DP background-capture code). Cross-repo, ABI-touching.
**Depends on:** #491 part 1 (premultiplied alpha-over for 2D-over-3D — PR #495, landed).
**Supersedes:** the runtime-side "weave-over-backdrop" prototype on branch `feature/491-part2-3-under-over-stack` (validated the under/over *split* + look on VK, but composited the backdrop in the wrong layer — see §1).

---

## 1. Why the prototype was the wrong layer

The part-3 prototype flattened the **under** Local2D layers (those *before* the projection in `xrEndFrame` list order) into a `backdrop_scratch` and drew the **weave over the backdrop** in the runtime's *post-weave* composite (`*_composite_zone_mask` / `vk_composite_local_2d`). On Leia with an **opaque** backdrop this looked correct (the backdrop just covers where the desktop would show). But it is architecturally wrong:

- The **Leia DP owns the background capture.** During `process_atlas` it captures the **desktop** (WGC / compose-under-bg, gated by `set_chroma_key` transparent-bg) and composites it under the woven 3D as its **sole** background layer.
- A **semi-transparent** 2D backdrop must let the **desktop show through it** (`desktop ⊕ backdrop`), then the 3D weaves over *that*. Only the DP can do this — it holds the captured desktop. The runtime post-weave composite never reaches the DP background.

**Correct layering (designer model, back→front):**

```
1. desktop                     (DP captures)
2. 2D-under backdrop  over (1) (premultiplied "over"; semi-transparent → desktop shows through)
3. 3D weave           over (2) (the DP's normal weave, but its background is now (2) not (1))
4. 2D-over overlay    over (3) (#491 part 1 — runtime post-weave alpha-over; unchanged)
```

So the **backdrop must reach the DP before the weave**, and the DP composites `backdrop over desktop` as its background. The overlay (part 1) stays a runtime post-weave pass.

---

## 2. The DP-vtable extension (ABI — ADR-020 append-only)

Add **one** appended method to the DP vtable, in the canonical `xrt_display_processor.h` **and** the four per-API headers (`_d3d11/_d3d12/_gl/_metal.h`), each with the API-typed handle:

```c
// Appended AFTER set_atlas_encoding (current last slot = 15). New slot = 16.
//
// Hand the DP this frame's flattened 2D-under backdrop (premultiplied RGBA, in
// the same client-window pixel space / canvas rect as process_atlas). The DP
// composites it OVER the captured desktop and uses the result as the under-3D
// background for the NEXT process_atlas. Pass a null handle (VK_NULL_HANDLE / 0)
// to clear (no backdrop this frame → desktop-only background, today's behavior).
//
// Optional — absent slot (older plug-in struct_size) or NULL ⟹ no-op; the
// runtime then keeps the part-1-only behavior (no under-layer support) so old
// plug-ins are unaffected.
void (*set_background_2d)(struct xrt_display_processor *xdp,
                          VkImageView_XDP background_view,   // per-API: ID3D11ShaderResourceView* / ID3D12Resource* / GLuint / id<MTLTexture>
                          uint32_t width,
                          uint32_t height);
```

ABI bookkeeping in **each** header (the asserts are the compile-time safety net):
- Add `XRT_DP_ABI_ASSERT(offsetof(..., set_background_2d) == XRT_DP_BASE_OFF + 16 * sizeof(void *), ...)`.
- Bump the size assert: `sizeof(struct xrt_display_processor) == XRT_DP_BASE_OFF + 17 * sizeof(void *)`.
- Add the `xrt_display_processor_set_background_2d(...)` inline wrapper gated on `XRT_DP_HAS_SLOT(xdp, set_background_2d)`.
- **No `XRT_PLUGIN_API_VERSION_CURRENT` bump** — appending at the end within a major is forward/backward compatible (old plug-ins report a smaller `struct_size`, so the wrapper no-ops). Confirm `scripts/check_plugin_abi.py` stays green.

> Append-only means an old Leia plug-in (pre-this-change) sees the slot as absent and the runtime degrades to part-1 (overlay-only, no under-layer). The new plug-in (with the slot) gets backdrops. No lock-step required.

---

## 3. Runtime side (this repo)

Per native compositor (`comp_{d3d11,d3d12,vk_native,gl,metal}`):

1. **Split** Local2D layers by list order vs the projection layer — **under** (index < projection) vs **over** (after / no projection). (Already prototyped for VK; reuse the `proj_idx` scan.)
2. **Flatten the under-layers → `backdrop_scratch`** (premultiplied), the same flatten primitive used for the overlay. Do this **before** the DP's `process_atlas` call (not in the post-weave composite).
3. **Call `xrt_display_processor_set_background_2d(dp, backdrop_view, w, h)`** right before `process_atlas` when under-layers exist; call it with a null handle (or skip) otherwise.
4. **Remove the weave-over-backdrop** from `*_composite_zone_mask` (the prototype). The post-weave composite reverts to part-1 (overlay alpha-over / explicit mask only); its `weave` input is the plain weave again.
5. **Consolidate the legacy `surround_2D`**: when under-layers are present they ARE the backdrop; the surround backdrop is the no-Local2D fallback. (Decide whether surround also routes through `set_background_2d` for a single DP background path — recommended, so the DP has one background concept.)

State/lifetime: `backdrop_scratch` is window-sized, premultiplied, sampled by the DP (so its final layout/usage must be DP-readable — VK `SHADER_READ`/`SAMPLED`, D3D `SRV`, etc.). The DP reads it during `process_atlas`, so it must outlive that call.

---

## 4. Plugin side (`displayxr-leia-plugin`)

- **`leia_bg_capture_win.cpp`** (the WGC desktop capture): add a "background 2D" input. When set, the background the weaver composites under the 3D becomes `composite(backdrop OVER captured_desktop)` (premultiplied over, honoring the backdrop's alpha so semi-transparent backdrops reveal desktop). When unset, today's desktop-only behavior.
- **`leia_display_processor.cpp` + `leia_display_processor_{d3d11,d3d12,gl}.cpp`** (+ the VK DP path): implement `set_background_2d` — store the handle/dims; feed it into the bg-capture compose for the next `process_atlas`. Match the per-API texture types.
- Re-pin `DXR_RUNTIME_GIT_TAG` (+ CI runtime-checkout ref) to a runtime build carrying the new slot; rebuild the plug-in + bundle.

---

## 5. Sequencing + validation

1. Land #491 part 1 (#495 — done) and part 2 docs.
2. Implement the DP-vtable slot + the **VK** runtime wiring + the **VK** plugin DP compose; validate end-to-end on Leia (the `DXR_LOCAL2D_BACKDROP` test app mode: opaque backdrop behind the floating cube **and** a semi-transparent backdrop variant → desktop shows through it).
3. Propagate to D3D11 / D3D12 / GL (+ Metal code-only); validate each.
4. One coordinated runtime + plugin release (re-pin), since the visible effect needs both.

**Test app:** `cube_handle_*_win` `DXR_LOCAL2D_BACKDROP=1` (prototyped on VK — large opaque cyan backdrop submitted before the projection). Add a **semi-transparent** backdrop variant to prove `desktop ⊕ backdrop` (the case the runtime-only prototype could not do).

---

## 6. Open questions

- **Surround consolidation:** route the legacy rect-surround through `set_background_2d` too (one DP background path), or keep it as the separate strip-blit fallback? Recommended: converge on `set_background_2d`.
- **Multiple under-layers:** flatten them in list order into the one backdrop (premultiplied over) — a single backdrop texture to the DP. Straightforward.
- **Non-Leia / sim_display DP:** `set_background_2d` is optional (HAS_SLOT-gated); a DP that ignores it degrades to overlay-only. sim_display can implement a trivial composite for the in-repo test double.
