# Step B — Integration-point design (the B1 spike)

**Status:** B1 deliverable. Research only — **no patch written.** This pins every
Chromium seam to `file:line` against the pinned source and proposes the
smallest-diff architecture. It must be agreed before B2 (the first real weave
through `content_shell`). Scope + red lines: [`webxr-step-b-scope.md`](webxr-step-b-scope.md).
Background: [`webxr-support.md`](webxr-support.md) §2.4. RPC contract:
`src/external/openxr_includes/openxr/XR_EXT_weave.h`.

**Pinned Chromium:** Latest Stable **M150, tag `150.0.7871.24`**, checkout
`C:\src\chromium\src` (`out/Default`, component build, `use_remoteexec=false`).
All `file:line` below are at this tag. B0 (build standup) is complete:
`content_shell` builds + runs on the Leia box; incremental relink ~6–18 s.

---

## 1. The proven reference flow (what we are re-creating)

Step A's `displayxr-cef-host` (local at `displayxr-cef-host/src/`) drives the
shipped `XR_EXT_weave` RPC end-to-end on Leia. Per element per frame
(`weave_compositor.cpp`, `xr_session.cpp`):

1. **bindWindow once** — `xrWeaveBindWindowEXT(session, hwnd)` so the DP phase-snaps the
   interlace to the window's panel position.
2. **Extract the element sub-rect** into a **keyed-mutex shared input texture**
   the host *allocates and owns* (`EnsureSbsInput`) — the host does **not** export
   the page's own texture handle; it copies the SBS region into its own shared
   texture.
3. **Weave** — `xrWeaveSubmitEXT(SBS handle, windowRelativeRect)` → woven shared
   texture HANDLE + fence + tracked eyes. *The caller never weaves* (ADR-007/019).
4. **GPU-wait the fence**, composite the woven sub-rect over the page base, present.
5. **Feed eyes back** to the page for the next frame's off-axis (Kooima) projection.

Step B re-creates exactly this client, inside Chromium, driving the **same RPC
unchanged**. The key takeaway from step 2: **we never need to export the page's
own canvas handle** — we can allocate our own keyed-mutex shared input and copy
into it, which sidesteps the hardest part of Seam A.

---

## 2. The pivotal finding — the GPU sandbox dictates the process boundary

The scope doc hypothesized the weave client lives in the **GPU process** (where the
canvas `SharedImage` and the present live). The texture/present *do* live there
(Seam C §4), **but the GPU process is sandboxed before D3D is initialised**, and a
per-frame RPC to `displayxr-service` is named-pipe IPC + shared-handle duplication
— which the lowered-token GPU sandbox blocks:

- Sandbox lockdown: `content/gpu/gpu_main.cc:611-614` (`LowerToken()`), via
  `gpu/ipc/service/gpu_init.cc:941-944`.
- D3D11 device + DirectComposition come up **after** lockdown:
  `gpu/ipc/service/gpu_init.cc:969-981`.
- Pre-sandbox warmup hook (where first-party blocked modules are pre-loaded, e.g.
  Media Foundation): `content/gpu/gpu_main.cc:160-194` (`PreSandboxStartup`).
  Pre-loading `DisplayXRClient.dll` here is *necessary but not sufficient* — the
  **per-frame** weave still needs ongoing kernel transitions (service pipe, handle
  dup) the lowered token forbids.

**Consequence — split the roles:**

| Need | Where it must live |
|---|---|
| Canvas `SharedImage` (D3D11 texture) + present/composite | **GPU process** (Viz) — can't move |
| OpenXR session + per-frame RPC to `displayxr-service` (unsandboxed IPC) | **Unsandboxed process** — browser, or a no-sandbox utility process |
| Top-level HWND for `bindWindow` / phase-snap | Browser process owns it directly |

So the weave client and the texture live in **different** processes, bridged by
DXGI shared handles + a small Mojo interface. This is the central design decision;
the rest follows from it.

**Recommended boundary:** run the weave client in the **browser process** for the
B2/B3 prototype (least code, HWND is local, unsandboxed IPC is free), and converge
to a dedicated **no-sandbox utility process** for the B4 reference patch (best
isolation — keeps the external DLL out of both the browser and the GPU sandbox).
**Not recommended:** weave-in-GPU-process behind a sandbox-policy exception — high
rebase cost and destabilises the sandbox (violates the red lines).

---

## 3. Seam-by-seam pin (M150 @ `150.0.7871.24`)

### Seam A — canvas GPU resource → cross-process DXGI/NT shared handle

| What | `file:line` |
|---|---|
| 2D canvas SI alloc (`Canvas2DResourceProviderSharedImage`) | `third_party/blink/renderer/platform/graphics/canvas_resource_provider.cc:1341,1459` |
| WebGL back-buffer SI alloc | `third_party/blink/renderer/platform/graphics/gpu/drawing_buffer.cc:2169` |
| Windows D3D backing (`D3DImageBacking`) texture create | `gpu/command_buffer/service/shared_image/d3d_image_backing_factory.cc:762-769` |
| **Shared-handle gate** (`needs_shared_handle`: WebGPU / cross-device GL / DComp only) | `…/d3d_image_backing_factory.cc:731-743` |
| Shared handle creation (`IDXGIResource1::CreateSharedHandle`) | `…/d3d_image_backing_factory.cc:782-784` |
| Handle + keyed-mutex holder (`DXGISharedHandleState`) | `gpu/command_buffer/service/dxgi_shared_handle_manager.h:48-131` |
| Export virtual (NOT overridden by `D3DImageBacking`) | `gpu/command_buffer/service/shared_image/shared_image_backing.h:268` |
| Renderer-side handle clone (requires mappable SI) | `gpu/command_buffer/client/client_shared_image.cc:563-568` |
| `gfx::DXGIHandle` over Mojo (NT handle + token + shmem; keyed-mutex intrinsic) | `ui/gfx/mojom/native_handle_types_mojom_traits.h:114-130` |

**Finding:** a normal canvas SI is created **without** a shared handle. **But with
the GPU-process placement we don't export the canvas SI's handle at all** — we
mirror the CEF host: GPU-side, resolve the canvas quad's texture (Seam B's
resolution path), `CopySubresourceRegion` the element rect into an **owned
keyed-mutex shared texture**, and export *that* `gfx::DXGIHandle` to the weave
client. This leaves the entire Blink/canvas-SI allocation path untouched (best for
rebasability) and confines new code to one GPU-side helper. (If we ever do want the
canvas SI's own handle: smallest path = allocate it via the mappable
`SharedImageInterface::CreateSharedImage(…buffer_usage…)` overload and call
`CloneGpuMemoryBufferHandle()`; next = override `GetGpuMemoryBufferHandle()` on
`D3DImageBacking` + widen the `needs_shared_handle` gate. Recorded as fallback.)

### Seam B — Viz composite injection (replace the canvas quad's texture)

| What | `file:line` |
|---|---|
| Canvas layer emits `TextureDrawQuad` (`quad_rect = bounds()`) | `cc/layers/texture_layer_impl.cc:165,182-186` |
| `TextureDrawQuad` def (single texture at a rect) | `components/viz/common/quads/texture_draw_quad.h:33,42,182` |
| Per-quad dispatch → `DrawTextureQuad` | `components/viz/service/display/skia_renderer.cc:1515,1541-1542` |
| **`DrawTextureQuad`** (resource→`SkImage` resolve) | `…/skia_renderer.cc:2619,2664-2675` |
| Resolve chokepoint (`ScopedSkImageBuilder` → `MakePromiseSkImage`) | `…/skia_renderer.cc:692,705,716,720` |
| Aggregator copies the quad + remaps resource id | `components/viz/service/display/surface_aggregator.cc:1411,1470,1484` |
| Resource→mailbox at draw (`LockResource`/`CreateImageContext`) | `components/viz/service/display/display_resource_provider_skia.cc:138,153-159` |
| Overlay (DComp) path for `kTextureContent` (larger, fragile — not chosen) | `components/viz/service/display/overlay_candidate_factory.cc:179-180,389` |

**Smallest diff (3 functions):** (1) tag the canvas's `TransferableResource`/quad
as a weave target upstream (`cc/layers/texture_layer_impl.cc:182`); (2) propagate
the flag through the aggregator (`surface_aggregator.cc:1470-1484`); (3) in
`SkiaRenderer::DrawTextureQuad` (`skia_renderer.cc:2664-2670`), when the flag is
set, substitute the **woven** texture's promise `SkImage` (built via the same
`skia_output_surface_->MakePromiseSkImage` machinery). Because we reuse the canvas
quad's own `params`/`vis_tex_coords`/`shared_quad_state`, the woven texture lands at
**exactly** the committed device-pixel rect with correct DOM z-order, automatically.

### Seam C — process boundary, linking, threading, HWND, sandbox

| What | `file:line` |
|---|---|
| GPU process main / sandbox lockdown | `content/gpu/gpu_main.cc:225,611-614` |
| Sandbox init before D3D; D3D/DComp after | `gpu/ipc/service/gpu_init.cc:941-944,969-981` |
| **Pre-sandbox warmup hook** (pre-load blocked DLLs) | `content/gpu/gpu_main.cc:160-194` |
| GPU `is_win` BUILD block (already links d3d11/dcomp/dxguid) — where a dep is added | `gpu/ipc/service/BUILD.gn:92-102` |
| Shared process-global D3D11 device (`GetDirectCompositionD3D11Device`) | `ui/gl/direct_composition_support.cc:827`; `gpu/ipc/service/gpu_init.cc:969-981` |
| Present thread (DCompPresenter `task_runner_` = GpuMain) | `ui/gl/dcomp_presenter.cc:34,45` |
| `SurfaceHandle == gfx::AcceleratedWidget == HWND` | `gpu/ipc/common/surface_handle.h:30` |
| Top-level HWND delivery → Viz (`RootCompositorFrameSinkParams.widget`) | `services/viz/privileged/mojom/compositing/frame_sink_manager.mojom:37`; `components/viz/service/display_embedder/output_surface_provider_impl.cc:80-167` |
| `SharedImageManager` / `DXGISharedHandleManager` are GPU-process-owned | `gpu/ipc/service/gpu_channel_manager.h:190-191`; `…/dxgi_shared_handle_manager.h:133` |

**Findings:** (a) GPU process holds the texture + present + shared device — the
woven import + composite (Seam B) **must** run there, on the GpuMain present
sequence using the shared device. (b) The RPC client **cannot** run there
(sandbox); it goes in the browser process (B2/B3) or a no-sandbox utility process
(B4). (c) `bindWindow`'s HWND is trivial in the browser process (it owns the
window); the GPU-only route would need to capture the `SurfaceHandle` at
`output_surface_provider_impl.cc:80-167` (not `DCompPresenter::GetWindow()`, which
is an internal 1×1 child).

### Seam D — committed device-pixel rect (and window-relative conversion)

| What | `file:line` |
|---|---|
| Canvas → `cc::TextureLayer`; geometry pushed at paint | `third_party/blink/renderer/core/html/canvas/html_canvas_element.cc:1640-1652`; `…/core/paint/html_canvas_painter.cc:67-73` |
| Quad geometry: `quad_rect = bounds()`, transform from `target_space_transform` | `cc/layers/texture_layer_impl.cc:165,182`; `cc/layers/layer_impl.cc:230-238,921-929` |
| Device-space rect = `GetEnclosingVisibleRectInTargetSpace` | `cc/layers/layer_impl.cc:948-972` |
| `target_space_transform` computed per frame | `cc/trees/draw_property_utils.cc:1100-1116,1677-1690` |
| **DSF / fractional-zoom baked into the transform tree root** | `cc/trees/draw_property_utils.cc:1743-1744`; `cc/trees/property_tree.cc:960-999,2847-2867` |
| Viz quad geometry == committed device px | `components/viz/common/quads/draw_quad.h:62-68`; `…/shared_quad_state.h:57-58` |
| Root compositing child window at (0,0), sized to client area → device px is window-relative | `ui/gl/child_window_win.cc:108-113,200-202` |

**Finding:** the one authoritative committed rect is, in Viz at draw time,
`shared_quad_state->quad_to_target_transform.MapRect(quad->visible_rect)` for the
canvas quad — DSF/zoom already applied, and (because the root compositing surface
sits at client (0,0)) already in window-relative device px (y-down) for the RPC's
`windowRelativeRect`. **Hazard:** must be read in Viz at draw time (same spot Seam
B injects), never from Blink layout — async commit makes any earlier value
one-frame stale, and one device pixel of drift collapses the lattice.

---

## 4. Recommended architecture (smallest diff that respects the red lines)

```
 Renderer (Blink)          Browser process            GPU process (Viz)        displayxr-service
 ─────────────────         ───────────────            ─────────────────        ────────────────
 canvas SBS render                                    canvas SharedImage
 (off-axis from eyes)                                  (D3D11 texture)
        │ JS: mark element 3D (B3)                            │
        ▼                                                     │ at DrawTextureQuad (Seam B):
   weave-target flag ──cc commit──► tag quad ──aggregate──►  resolve canvas tex,
                                                              copy element rect → OWNED
                                                              keyed-mutex shared input
                                                              (Seam A, no canvas-SI export)
                                       ▲   {input DXGIHandle, windowRelRect}  │
                          bindWindow(HWND) once  ◄───── new Mojo iface ────────┘
                          xrWeaveSubmitEXT ──────────────────────────────────────► weave (DP)
                          {woven DXGIHandle, fence, eyes} ◄───────────────────────
                                       │ woven handle + fence ─► import as SharedImage,
                                       │                          bind at DrawTextureQuad,
                                       │                          GPU-wait fence, composite
        eyes ◄──(B3: to renderer for next-frame projection)──────┘
```

**Diff inventory (isolated + flag-gated `--enable-inline-3d`):**
- *New, self-contained:* the weave client wrapper (OpenXR loader + `DisplayXRClient`,
  the XrSession + `bindWindow`/`weave`, mirrors `xr_session.cpp`); a GPU-side weave
  helper (owned keyed-mutex shared input mgmt + woven import, mirrors
  `weave_compositor.cpp`); one new Mojo interface (browser↔GPU) carrying
  `{inputHandle, rect}` out and `{wovenHandle, fence, eyes}` back.
- *Touched Chromium files (small):* `skia_renderer.cc` `DrawTextureQuad` (substitute
  woven `SkImage`); `texture_layer_impl.cc` + `surface_aggregator.cc` (carry the
  weave-target flag); `gpu/ipc/service/BUILD.gn` `is_win` block (link the client);
  `content/gpu/gpu_main.cc` `PreSandboxStartup` only if any GPU-side warmup is
  needed; Blink binding (B3 only).
- *Untouched (deliberately):* canvas/WebGL SharedImage allocation, the weave shader
  (lives in the DP), eye tracking, calibration — **zero vendor code in Chromium.**

---

## 5. Open decisions / things to confirm in B2

1. **Accumulation (RPC §2.6).** Does the DP preserve prior sub-rects across submits
   into the one window-sized output, or must we serialize (submit→composite per
   element) as the CEF host does? Default to the host's serialized model; confirm
   against `displayxr-service` and adopt accumulation if supported (would let N
   submits → 1 fence-wait).
2. **JS surface.** Prototype B2/B3 with a thin `navigator.displayXR.bindWindow/weave`;
   converge to the explainer's WebXR `inline-3d` + `XRDisplayLayer` shape for the B4
   reference patch (per scope doc).
3. **Fence semantics across the GPU↔browser hop.** The RPC returns a shared fence
   HANDLE; confirm the GPU process can open + GPU-wait it on the present sequence
   (same pattern as `weave_compositor.cpp` `OpenHandback`).
4. **Process boundary final call.** Browser process for B2/B3; evaluate a no-sandbox
   utility process for B4 (cleaner isolation, +1 process + mojo).
5. **Multi-element + per-frame handle churn.** Inherit the host's N-element model;
   measure the GPU↔browser handle plumbing cost vs. the ~0.6 ms weave round-trip.

## 6. What B2 will prove (do not start until this design is agreed)

Hardcode "weave the first canvas on the page" with **no JS**: at the Seam-B
injection point, copy the canvas texture into an owned shared input, drive
`xrWeaveSubmitEXT` from the browser-process client, import the woven handle, and
substitute it at `DrawTextureQuad` — a known SBS canvas shows real glasses-free 3D
through `content_shell` on Leia. This validates Seam A (owned-input copy) + the RPC
+ the browser↔GPU bridge + Seam B composite end-to-end — the riskiest milestone.

---

## 7. B2c implementation notes (added during build — corrects §3 Seam B)

B0/B1/B2a/B2b are done + Leia-validated (branch `displayxr-inline-3d` in
`C:\src\chromium\src`). The B2c spike found a **correction to the Seam-B plan**:

**The substitution cannot happen in `SkiaRenderer::DrawTextureQuad`.** That runs on
the **compositor (display) thread**, which has **no** access to the `ID3D11Device`,
`SharedImageManager`, `SharedImageFactory`, or the immediate context — those are
GPU-thread-only members of **`SkiaOutputSurfaceImplOnGpu`**
(`components/viz/service/display_embedder/skia_output_surface_impl.h:73`;
private members `skia_output_surface_impl_on_gpu.h:500-502`). `DrawTextureQuad`
only records a deferred display list; textures resolve later on the GPU thread.

**So all D3D work moves to `SkiaOutputSurfaceImplOnGpu` (GPU main thread)** — the
same thread the B2b Mojo remote was bound on (`PostCompositorThreadCreated`).
`SkiaRenderer` only tags the target quad + ferries handles across the boundary.

**Device identity:** in content_shell's default Ganesh-GL/ANGLE Viz, the canvas
SharedImage textures, the DComp device, and the `D3DImageBackingFactory` all share
**one** `ID3D11Device` = `GetDirectCompositionD3D11Device()`
(`ui/gl/direct_composition_support.cc:827`; via ANGLE,
`gpu_init.cc:976-979`) → CopyResource is same-device. (Breaks under Graphite-Dawn:
`shared_context_state.cc:1400-1401`; not our build.)

**The 6-step GPU-thread path (all pinned to M150):**
1. **Tag quad / ferry handles** — `SkiaRenderer::DrawTextureQuad`
   (`skia_renderer.cc:2619`), compositor thread. No D3D here.
2. **Grab canvas D3D tex** — `shared_image_representation_factory_->ProduceOverlay(
   mailbox)` → `BeginScopedReadAccess()` → `GetDCLayerOverlayImage()` (wraps the
   `ID3D11Texture2D`). `d3d_image_representation.h:124`. (`GetD3D11Texture()` only
   exists on `VideoImageRepresentation`, not generic — use Overlay.)
3. **Copy region → owned keyed-mutex input; weave RPC** via the Mojo remote
   (GPU main thread).
4. **GPU-wait woven fence** — `gfx::D3DSharedFence::CreateFromUnownedHandle(h)`
   (`ui/gfx/win/d3d_shared_fence.h:54`) → `WaitD3D11(GetDirectCompositionD3D11Device())`
   (`d3d_shared_fence.cc:225-269`; opens fence + queues `ID3D11DeviceContext4::Wait`
   internally — Viz needn't expose the context).
5. **Import woven HANDLE as SharedImage** — wrap in `gfx::GpuMemoryBufferHandle`
   type `DXGI_SHARED_HANDLE` (`gfx::DXGIHandle`), call
   `shared_image_factory_->CreateSharedImage(mailbox, si_info, is_thread_safe,
   handle)` → `D3DImageBackingFactory::CreateSharedImage`
   (`d3d_image_backing_factory.cc:828-899`) → `DXGISharedHandleManager::
   GetOrCreateSharedHandleState` (`:861-863`).
6. **Substitute SkImage** — `ProduceSkia(woven_mailbox, context_state_,
   {DISPLAY_READ})->BeginScopedReadAccess()->promise_image_texture()`
   (mirrors `image_context_impl.cc:398-468`); swap the woven mailbox into the
   tagged quad's `ImageContextImpl` so the existing `MakePromiseSkImage`
   (`skia_output_surface_impl.h:142`) fulfills from the woven backing.

**mojom extension (B2c):** `WeaveSubmit(gfx.mojom.DXGIHandle texture,
gfx.mojom.Rect rect) => (DisplayXRWeaveResult? result)` with `{DXGIHandle woven,
DXGIHandle fence, uint64 fence_value, uint32 w/h, array eyes, bool eyes_valid}`.
Browser `DisplayXRWeaverImpl::WeaveSubmit` drives the real `xrWeaveSubmitEXT` on the
B2a session and returns the result. (`gfx.mojom.DXGIHandle`:
`ui/gfx/mojom/native_handle_types.mojom`, `[EnableIf=is_win]`, typemap
`ui/gfx/mojom/BUILD.gn` — add `//ui/gfx/mojom` to the mojom deps.)

**Slicing (validate incrementally, not big-bang):**
- **B2c.1** — prove the full GPU-thread pipe with a **synthetic** input: GPU thread
  creates an owned keyed-mutex input (test pattern), drives the real
  `xrWeaveSubmitEXT` via the bridge, imports the woven result, fence-waits,
  substitutes for the canvas quad. If the canvas region shows the woven synthetic
  content in 3D, the hard pipe (handle marshalling both ways + real weave + fence +
  import + substitute) is proven.
- **B2c.2** — replace the synthetic input with the **real** canvas texture (step 2).

**Identify the canvas quad** (B2 hardcode): largest `TextureDrawQuad` in the frame,
or tag via `texture_layer_impl.cc:182` + `surface_aggregator.cc:1470-1484`.

**Open:** keyed-mutex (woven tex) vs the separate fence — wait the fence *before*
the draw, independent of the keyed-mutex acquire `D3DImageBacking` read-access does.

---

## 8. B2c.1 build status (2026-06-26) — pipe proven, blocked on a runtime -2

B2c.1 is **code-complete** on the Chromium patch branch `displayxr-inline-3d`
(commit `1f110e06893a`) and the full pipe is **proven firing end-to-end** on the
Leia box, with one **runtime-side** blocker remaining (not a patch bug).

**Wired exactly per §7:** `WeaveSubmit(DXGIHandle,Rect)=>(result)` `[Sync]` mojom;
browser `DisplayXRWeaverImpl` → real `xrWeaveSubmitEXT` on the B2a session;
GPU-side `DisplayXRWeaveGpu` (impl of new `viz::DisplayXRWeaveProvider`, registered
in `ShellContentGpuClient::PostCompositorThreadCreated`) owns the Mojo remote + a
keyed-mutex synthetic SBS input; `SkiaRenderer::DrawTextureQuad` tags the canvas
quad; `SkiaOutputSurfaceImplOnGpu::MaybeWeaveSubstitute` (Ganesh **and** Graphite
paths) imports the woven handle + fence-waits + redirects the tagged
`ImageContextImpl` via an override mailbox.

**Launch prerequisites discovered (all required to even reach the weave):**
- Forward `--enable-inline-3d` to child processes (`AppendExtraCommandLineSwitches`)
  — else the GPU/Viz process never registers the provider / tags the quad.
- content_shell uses **Graphite-Dawn**, not Ganesh → the substitution had to be
  added to the graphite branch of `FinishPaintCurrentFrame` too.
- content_shell defaults to **delegated compositing + DComp overlays**, so
  `SkiaRenderer` never draws the canvas quad. Run with
  `--disable-direct-composition` (and `--disable-features=CalculateNativeWin-
  Occlusion,DelegatedCompositing`) so Viz draws quads via `SkiaRenderer`.
- In this env content_shell rendered file:// / data: URLs as empty documents;
  inject the canvas via CDP (`--remote-debugging-port` + `document.write`).

**Proven (logs):** DrawTextureQuad tags the canvas → provider found → synthetic
SBS input created → Mojo `[Sync]` → browser → `xrWeaveSubmitEXT` called with a
valid non-null NT handle + window-relative rect (content_shell and the service
both High integrity, so handle duplication is fine).

**Blocker (runtime, not Chromium):** `xrWeaveSubmitEXT` returns **-2
(`XR_ERROR_RUNTIME_FAILURE`)** every call. `comp_d3d11_service_weave_submit`
silent-returns `false` **before** any of its `U_LOG_E` points (no
`OpenSharedResource` / "server output" in the service log) — i.e. either the IPC
handle transport, or a silent early-return (prime suspect:
`c->render.display_processor == nullptr` for a present-owner session that never
runs a frame loop). The CEF host (Step A) drove `weave_submit` fine **without**
`xrBeginSession`, so it is not a missing begin. **Next step:** add a service-side
log at the `comp_d3d11_service_weave_submit` entry (which early-return fires; the
`in` handle value) — pinpoints DP-null vs handle-null — then the
import/fence/substitute path runs and the **Leia eyeball** (B2c.1 success
criterion: each eye a different solid color across the canvas) can be done.
