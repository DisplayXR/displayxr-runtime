# Cross-API rollout of the local-3D-zone mask consumer — D3D12 + VK legs

**Branch:** `feature/unified-2d3d-crossapi` (base leg), off `main` post-Phase-2.
**Spec:** [`unified-2d-3d-compositing.md`](unified-2d-3d-compositing.md) §4 (composite pass + output-alpha), §5.1 (coordinate contract), §8 (rollout priority). Epic #439.
**Builds on:** D3D11 Phases 0–2 (merged: PR #469 + #471) — the proven pathfinder every port references.

> **STATUS: base leg merged (PR #473); D3D12 consumer leg DONE** — implemented, capture-validated (Z-cycle states 0–4 via the ported `DISPLAYXR_SURROUND_CAPTURE` probe) and Leia on-glass validated (PR #476). VK leg scoped to ride with Phase 3 (§4).

---

## 1. Three legs, two boundaries

| Leg | What | Where | Mergeable alone? |
|---|---|---|---|
| **Base** (this session, macOS) | Header v2 (D3D12/VK Tier-3 binding structs), oxr forwarding branches, comp entry points **stubbed** | oxr + headers + 2 stub blocks | **Yes** — caps report `supported=false` for D3D12/VK until each leg flips; stubs return `XRT_ERROR_NOT_IMPLEMENTED` |
| **D3D12 consumer** (Windows) | Real zone-mask object + masked composite + effective-canvas rule | `comp_d3d12_compositor.cpp` (+ renderer) | Yes, after base |
| **VK consumer** (see §4 — scoped) | Same, for `vk_native` | `comp_vk_native_compositor.c` (+ renderer) | Yes, after base |

**Why stubs, not Phase-1's declared-but-unimplemented boundary:** `XRT_HAVE_VK_NATIVE_COMPOSITOR` is defined on **every platform** (`oxr/CMakeLists.txt`: `XRT_HAVE_VULKAN AND TARGET comp_vk_native` — true on macOS/MoltenVK and Android, not just Windows). Declared-only VK entry points would break the macOS link immediately. Real stubs keep every platform green and make the base leg mergeable — which is what lets the two consumer legs run as **parallel sessions against `main`** instead of stacking on an unmerged branch.

## 2. Base leg (macOS, this session)

### 2.1 Header v2 — `XR_EXT_local_3d_zone.h`, SPEC_VERSION 2

Two new Tier-3 binding structs (model: the D3D11 one at type `...132`):

```c
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_EXT  ((XrStructureType)1000999133)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_EXT ((XrStructureType)1000999134)

// D3D12: the runtime returns the ID3D12Resource* (R8_UNORM, w×h client px);
// the app creates its OWN RTV on it (descriptor heaps are app-owned in D3D12).
// Sync: the in-process D3D12 compositor runs on the APP'S device AND queue
// (comp_d3d12_compositor.cpp:86–89), so same-queue submission order is the
// sync contract — draw the mask, then xrSubmitLocal3DZoneEXT; no fence needed.
typedef struct XrLocal3DZoneRenderTargetD3D12EXT { type; next;
    void *resource; uint32_t width, height; } ...;

// Vulkan: VkImage + VkImageView (R8_UNORM). The vk_native compositor shares
// the APP'S VkDevice (vk_init_from_given, comp_vk_native_compositor.c:2747),
// so the handles are app-usable directly; same-queue ordering applies.
typedef struct XrLocal3DZoneRenderTargetVulkanEXT { type; next;
    void *image; void *imageView; uint32_t width, height; } ...;
```

### 2.2 comp entry points — declarations + stubs

Mirror the six D3D11 signatures (`comp_d3d11_compositor.h:130-…`) in:
- `comp_d3d12_compositor.h` + stubs in `comp_d3d12_compositor.cpp` (Windows-compiled; PR CI validates)
- `comp_vk_native_compositor.h` + stubs in `comp_vk_native_compositor.c` (macOS-verified locally)

Acquire differs per API: D3D12 returns `void **out_resource`; VK returns `void **out_image, void **out_image_view`. All stubs return `XRT_ERROR_NOT_IMPLEMENTED` (`zone_mask_destroy` stubs are no-ops).

### 2.3 oxr forwarding — `oxr_local_3d_zone.c`

Add `#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR` / `XRT_HAVE_VK_NATIVE_COMPOSITOR` branches beside every D3D11 branch (7 handlers + destroy_cb), modeled on the 5-way surround handler (`oxr_api_session.c:1590`). Specifics:
- **Caps**: D3D12/VK branches report `supported = XR_FALSE` until the consumer leg flips them (one-line change per leg, listed in each leg's done-when). Honest contract: apps that check caps never hit a stub.
- **Create**: map `XRT_ERROR_NOT_IMPLEMENTED` → `XR_ERROR_FEATURE_UNSUPPORTED` (apps that skip the caps check get the right error, not RUNTIME_FAILURE).
- **AcquireRT**: validate the binding `type` per session API (D3D12_EXT / VULKAN_EXT).

## 3. D3D12 consumer leg (Windows hand-off) — fully viable now

Everything the port needs already exists on the D3D12 side:
- **2D source**: surround is implemented — Spec v6 KeyedMutex (`comp_d3d12_compositor_set_surround_2d`) + Spec v7 fence variant (`..._set_surround_2d_fence`, `comp_d3d12_compositor.h:199–203`).
- **Test app**: `cube_texture_d3d12_win` exists — port the `cube_texture_d3d11_win` 'Z'-cycle harness (Phase-1 commit `17ae19294`).
- **Sync simplification vs D3D11**: compositor uses the **app's queue**, so Tier-3 authoring needs no fence (§2.1). The *surround* producer keeps its existing v6/v7 sync.

Port checklist (reference: D3D11 commits `17ae19294` Phase 1 + `b67f9f6f1` Phase 2):
1. Zone-mask object: R8_UNORM committed resource + RTV (runtime-side, for Tier 1/2 clears) + staged snapshot copy + `submitted` flag; submit snapshots under the compositor lock (atomicity, spec §9 Q3).
2. Tier 1 = `ClearRenderTargetView`; Tier 2 = clear + per-rect clears (D3D12: `ClearRenderTargetView` takes rects natively — simpler than D3D11's `ClearView`).
3. Masked composite: port `comp_d3d11_renderer_composite_2d_masked` (t0 surround / t1 mask / t2 weave-snapshot, `use_rect_mask` flag, §4.2 output-alpha rule) to a D3D12 PSO; weave-target → SRV-capable scratch copy before the lerp.
4. `d3d12_effective_canvas` — the Phase-2 supersede rule at every canvas read site of the D3D12 frame path (find the analogs of the 8 D3D11 sites).
5. Flip caps `supported = XR_TRUE` for the D3D12 branch.
6. Validation on Leia (capture + on-glass): the Phase-1 §6 matrix + Phase-2 §5 matrix, on `cube_texture_d3d12_win`. No-mask path byte-identical (zero regression).

## 4. VK consumer leg — the 2D-source reality check (scope honestly)

The masked composite needs **2D pixels** to lerp toward. On VK, today, there are none:
- Every VK app in the tree is **handle-class** (demos: gaussian-splat, mediaplayer plan; `cube_handle_vk_*`) — handle apps have no surround (that's a texture-app feature) and no 2D layer until **Phase 3**.
- `vk_native` has **no surround implementation** (post-Phase-C TODO at `comp_vk_native_compositor.c:3401`); there is no VK texture test app.

So a full Phase-1-style VK port has nothing to composite and nothing to validate against. **Recommended scope:**

- **Now (base leg)**: stubs + `supported=false` (done in §2). Demos can code against the authoring API guarded by the caps query.
- **VK mask-object mechanism** (optional intermediate, macOS-developable): implement create/tiers/submit/destroy for real (R8 image + clears + staged copy — portable `vk_bundle` code, testable with `cube_handle_vk_macos` + sim_display), leaving the composite consumer for later. Cheap, but unconsumed until a 2D source exists.
- **VK composite consumer**: ride with **Phase 3** — the 2D-as-`xrEndFrame`-layer work gives handle apps (i.e. *all* demos) their 2D source, and §6 already plans Phase 3 as "D3D11, then cross-API". Building VK surround + a VK texture app just to validate the composite earlier is throwaway work against the §7 migration story.
- **Interim demo story**: demos wanting 2D/3D zones *before* Phase 3 are served by the **hardware-DP leg** (#224 / `local-3d-zones.md`) — publishing the mask to the panel needs no 2D pixels at all; the degenerate 1×1 grid = per-window auto 2D/3D today.

## 5. Done-when

**Base leg**
- [x] Header v2 (two structs, SPEC_VERSION 2) syncs to `displayxr-extensions` on merge.
- [x] oxr branches + stubs build green on macOS (`build_macos.sh`) and Windows (PR CI).
- [x] Caps report `supported=false` on D3D12/VK; create returns `XR_ERROR_FEATURE_UNSUPPORTED`. *(Merged as PR #473.)*

**D3D12 leg**
- [x] §3 checklist 1–5; caps flipped. *(Plus the `DISPLAYXR_SURROUND_CAPTURE` probe ported — the D3D12 compositor had no composite-target capture; the matrices depend on it. Composite PSOs cover RGBA8 + BGRA8 — D3D12 bakes the RTV format into the PSO and app shared textures are BGRA8 in the wild.)*
- [x] Phase-1 + Phase-2 validation matrices green on `cube_texture_d3d12_win` (Leia, capture + on-glass); no-mask diff 0. *(Capture leg: Z-cycle 0→4 + wrap-around, view dims resize both directions, beyond-window untouched. On-glass eyeballed by user 2026-06-07 — interlace correct at islands + gradient feather.)*
- [x] Epic #439 "D3D12 — all engine plugins" box ticked.

**VK leg**
- [ ] Scope decision recorded (this doc §4); composite consumer tracked into the Phase-3 plan.
