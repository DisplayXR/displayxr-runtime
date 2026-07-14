# Step B — Integration-point design (the B1 spike)

**Status:** B1 deliverable. Research only — **no patch written.** This pins every
Chromium seam to `file:line` against the pinned source and proposes the
smallest-diff architecture. It must be agreed before B2 (the first real weave
through `content_shell`). Scope + red lines: [`webxr-step-b-scope.md`](webxr-step-b-scope.md).
Background: [`webxr-support.md`](webxr-support.md) §2.4. RPC contract:
`src/external/openxr_includes/openxr/XR_DXR_weave.h`.

**Pinned Chromium:** Latest Stable **M150, tag `150.0.7871.24`**, checkout
`C:\src\chromium\src` (`out/Default`, component build, `use_remoteexec=false`).
All `file:line` below are at this tag. B0 (build standup) is complete:
`content_shell` builds + runs on the Leia box; incremental relink ~6–18 s.

---

## 1. The proven reference flow (what we are re-creating)

Step A's `displayxr-cef-host` (local at `displayxr-cef-host/src/`) drives the
shipped `XR_DXR_weave` RPC end-to-end on Leia. Per element per frame
(`weave_compositor.cpp`, `xr_session.cpp`):

1. **bindWindow once** — `xrWeaveBindWindowDXR(session, hwnd)` so the DP phase-snaps the
   interlace to the window's panel position.
2. **Extract the element sub-rect** into a **keyed-mutex shared input texture**
   the host *allocates and owns* (`EnsureSbsInput`) — the host does **not** export
   the page's own texture handle; it copies the SBS region into its own shared
   texture.
3. **Weave** — `xrWeaveSubmitDXR(SBS handle, windowRelativeRect)` → woven shared
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
                          xrWeaveSubmitDXR ──────────────────────────────────────► weave (DP)
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
`xrWeaveSubmitDXR` from the browser-process client, import the woven handle, and
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
Browser `DisplayXRWeaverImpl::WeaveSubmit` drives the real `xrWeaveSubmitDXR` on the
B2a session and returns the result. (`gfx.mojom.DXGIHandle`:
`ui/gfx/mojom/native_handle_types.mojom`, `[EnableIf=is_win]`, typemap
`ui/gfx/mojom/BUILD.gn` — add `//ui/gfx/mojom` to the mojom deps.)

**Slicing (validate incrementally, not big-bang):**
- **B2c.1** — prove the full GPU-thread pipe with a **synthetic** input: GPU thread
  creates an owned keyed-mutex input (test pattern), drives the real
  `xrWeaveSubmitDXR` via the bridge, imports the woven result, fence-waits,
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

> **Update: the runtime -2 is RESOLVED — see [§9](#9-b2c1-runtime--2--root-caused--fixed-2026-06-26).** This section is the point-in-time build status that led to the diagnosis.

B2c.1 is **code-complete** on the Chromium patch branch `displayxr-inline-3d`
(commit `1f110e06893a`) and the full pipe is **proven firing end-to-end** on the
Leia box, with one **runtime-side** blocker remaining (not a patch bug).

**Wired exactly per §7:** `WeaveSubmit(DXGIHandle,Rect)=>(result)` `[Sync]` mojom;
browser `DisplayXRWeaverImpl` → real `xrWeaveSubmitDXR` on the B2a session;
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
SBS input created → Mojo `[Sync]` → browser → `xrWeaveSubmitDXR` called with a
valid non-null NT handle + window-relative rect (content_shell and the service
both High integrity, so handle duplication is fine).

**Blocker (runtime, not Chromium):** `xrWeaveSubmitDXR` returns **-2
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

---

## 9. B2c.1 runtime -2 — ROOT-CAUSED + FIXED (2026-06-26)

The -2 is a **runtime DP-availability bug, gated on workspace mode** — *not* the
IPC handle transport. Diagnosed by instrumenting every silent early-return in
`comp_d3d11_service_weave_submit` and reproducing the weave with two **browser-
free** present-owners (the `weave_rpc_probe_d3d11_win` test app and the Step-A CEF
host), which fire the same `xrWeaveSubmitDXR` RPC deterministically (no dependence
on a browser compositor producing frames — content_shell would not composite under
the headless automation harness, so it was a poor diagnosis vehicle).

**Findings (service-side `#625 weave_submit ENTRY` log):**
- The present-owner session reaches `weave_submit` with a **valid** `in_handle`
  and a bound `weave_hwnd` — handle transport is fine, `bindWindow` is fine.
- `c->render.display_processor == nullptr` **iff `workspace_mode == 1`.** The per-
  client DP is created in `init_client_render_resources` **only when no workspace
  is active** (`dp_fac != NULL && !sys->workspace_mode`) — a second per-client DP
  while a workspace is up makes the SR SDK recalibrate its weaver. So a present-
  owner that connects *while the spatial shell is active* has no per-client DP and
  `weave_submit` hit its `sys/DP-null` early-return → false → -2.
- In **non-workspace** mode the per-client DP exists and the weave **succeeds**
  end-to-end (CEF host: 50+ submits, no early-returns, `process_atlas` runs).

**The original B2c.1 -2 happened because the spatial shell was running** (workspace
mode active) when content_shell connected.

**Fix** (`comp_d3d11_service_weave_submit`): when `c->render.display_processor` is
null, fall back to the **shared multi-compositor DP** (`sys->multi_comp->
display_processor`) instead of failing. `process_atlas` takes the output dims +
sub-rect explicitly, so it weaves the present-owner's own window-sized handback
regardless of which DP instance drives it; the render thread and `weave_submit`
both run `process_atlas` under `sys->render_mutex`, so the shared DP is serialized
(no concurrent immediate-context use). Lowest-risk option — **creates no new DP**,
so zero SR-recalibration risk to the active workspace. (Phase caveat: the shared
DP's interlace phase references the workspace window. Phase-independent for the
synthetic SBS test pattern; real content phase-snaps via `xrWeaveSnapWindowRectDXR`
— a B2c.2 follow-up if a window-bound phase is needed.)

**Validated** (deterministic, both modes) with `weave_rpc_probe_d3d11_win`: in
workspace mode the new `per-client DP absent … using the shared multi-compositor
DP` path engages, `process_atlas weave: vp=(240,100 640x360)` runs, the probe logs
no failure, and the captured woven output (`%TEMP%\weave_probe_output.bmp`) shows
the synthetic SBS confined to the requested sub-rect. Non-workspace mode is
unchanged (per-client DP, no fallback log). The runtime block is cleared; the live
**Leia eyeball** of the 3D effect remains a human step.

---

## 10. B2c.2 — real canvas texture substituted (2026-06-27)

B2c.2 swaps the B2c.1 synthetic test-pattern input for the **real tagged canvas
texture**, so the woven 3D is the page's own canvas pixels. **Code-complete +
builds clean** on `displayxr-inline-3d` (commit `656af477c0d3c`). Implements §7
step 2, with two corrections to the §7 plan found during the build:

- **Device identity is a non-issue on the default config.** content_shell's
  Graphite-Dawn runs the **D3D11 backend** (`kSkiaGraphiteDawnUseD3D12` is
  default-off) which **shares ANGLE's D3D11 device** (`dawn_context_provider.cc`
  "Share D3D11 device with ANGLE"), so the canvas SharedImage, DComp, and
  `context_state_->GetD3D11Device()` are **one device X**. The weave input is now
  allocated on device X (the provider dropped its private device), making the
  `CopySubresourceRegion` a legal same-device copy.
- **`ProduceOverlay` can't be used as-is.** A normal 2D canvas SI is
  `DISPLAY_READ`-only (no `SCANOUT`), and the manager's `ProduceOverlay` hard-
  enforces SCANOUT. Added a Win-only **`ProduceOverlayForWeave`** (SharedImage
  `Manager` + `RepresentationFactory`) = `ProduceOverlay` minus the SCANOUT gate;
  it still yields the D3D overlay representation whose `GetDCLayerOverlayImage()`
  exposes the canvas `ID3D11Texture2D`. (`EnforceSharedImageUsage` is non-fatal —
  `DumpWithoutCrashing` — but would fire per-frame, hence the dedicated variant.)

Flow: Viz `MaybeWeaveSubstitute` → `ProduceOverlayForWeave(target->mailbox())` →
`BeginScopedReadAccess()` → `GetDCLayerOverlayImage()->d3d11_video_texture()` →
`WeaveCanvas(deviceX, canvas_tex, slice, src, window)`; the provider copies the
canvas sub-rect into the keyed-mutex input (format taken from the canvas desc so
the copy is always legal) and drives the **unchanged** Mojo weave RPC / woven
import / fence-wait / substitute. Test page (`%TEMP%\inject_b2c1.py`) now paints
left-GREEN / right-MAGENTA SBS (deliberately ≠ the synthetic red/blue) so a
successful eyeball proves canvas provenance.

**Live Leia eyeball: ✅ PASSED (2026-07-04).** The full pipe came up live
(`xrWeaveBindWindowDXR -> 0 — B2a PASSED`, `B2c GPU weave provider registered`,
`B2c.2 weave target found; driving canvas weave`, `B2c.2 canvas weave input ready`),
the service wove every frame (`leia_dp_d3d11_process_atlas weave: target=2058x1745
view=1029x1745`), and the user confirmed the real green/magenta SBS canvas rendered
as **glasses-free 3D with correct opposite-eye parallax** on the Leia panel. The
"blocked on frame production" wall below (§11) was resolved; the working recipe is
**about:blank + `IPC_IGNORE_VERSION=1` + Medium-integrity launch** (see §11.1).

---

## 11. content_shell "zero compositor frames" — ROOT-CAUSED + FIXED (2026-06-28)

The §9/§10 "content_shell won't composite under automation" wall was **not** a
foreground/automation limitation — it was a **launch-flag bug**, now fixed. The
window composites fine when launched by the agent (no human foreground needed).

**Diagnosis (decoupled, agent-driven via CDP on a synthetic rAF/timer counter):**
- A `setInterval` timer counter vs a `requestAnimationFrame` counter, read over
  CDP, cleanly separates "renderer wedged" from "begin-frames dead." With a
  corrected launch the rAF counter climbs ~60/s and `Page.captureScreenshot`
  returns real PNG bytes — all with `document.hasFocus()===false`. **Foreground
  is not required.** (The earlier "needs a human at the desktop" belief was a red
  herring; it was masked by a probe bug — the CDP reply nests `result.result.value`,
  and an early probe read `result.value` → always `None` → false "zero frames".)

**Two independent causes, both fixed:**
1. **`--run-all-compositor-stages-before-draw` WEDGES the renderer.** This is the
   web-test (RunWebTests) synchronous-compositor switch: it makes Blink's main
   thread block in `BeginMainFrame` waiting for an **externally-issued** BeginFrame
   that only the web-test harness sends. content_shell run normally never issues it
   → the renderer goes unresponsive (even `Runtime.evaluate` never returns) → zero
   frames. **Fix: remove the flag.** This was the dominant blocker — `run_b2c1.bat`
   already disabled occlusion (cause #2) but still failed because of this flag.
2. **`CalculateNativeWinOcclusion` (default-on) halts frames** for a window
   launched non-foreground (agent launch): Windows marks it occluded → the
   compositor stops after the first paint (the "body bg painted once then frozen"
   symptom). **Fix: `--disable-features=CalculateNativeWinOcclusion`** (already in
   `run_b2c1.bat`).

`--disable-direct-composition` is **safe** for frame production (60 fps confirmed) —
it is *not* a frame killer; the earlier suspicion was the same probe-parser bug.

**Verified** (corrected `run_b2c1.bat`, `--enable-inline-3d`, agent launch, CDP
canvas inject): the real injected SBS canvas's own rAF (`window.__t`) climbs ~60/s
at **both High and Medium integrity**. The net-service crash-loop in the log is
**unrelated** to compositing (it is why `file://` loads empty → we CDP-inject; it
does not affect begin-frames).

**Fix applied:** dropped `--run-all-compositor-stages-before-draw` from the launch
(`%TEMP%\run_b2c1.bat`).

### 11.1 The "weave freeze" — RESOLVED: it was `file://` + a version-gate, NOT the weave (2026-07-04)

> **RESOLUTION (read this first; the investigation below reached a WRONG conclusion
> that this corrects).** The apparent "weave engaged → frames freeze" was **two
> unrelated environment issues, neither in the weave/GPU code**:
>
> 1. **`file://` loads as an empty document in this env → it never composites.**
>    Every "frozen" run loaded `file:///…/b2c1.html`; every "working" run loaded
>    `about:blank`. The injected canvas in an empty-`file://` document produces zero
>    frames (`window.__t` stuck at 0); the same canvas in `about:blank` runs at
>    ~60 fps **with the weave provider live**. Fix: load `about:blank` + CDP-inject.
>    (The all-processes-idle dumps below are of `file://` runs — they were idle
>    because the empty doc never produced a frame, *not* because the GPU weave
>    provider suppressed BeginFrames. The "provider registration perturbs Viz"
>    hypothesis is WRONG.)
> 2. **`xrCreateInstance` `-2` = an IPC version-gate reject.** Deployed
>    `DisplayXRClient.dll` (v1.26.2-**18**) is newer than the running
>    `displayxr-service.exe` (v1.26.2-**3**) — a prior session copied the DLL to
>    Program Files but couldn't overwrite the service .exe (Administrators ACL). The
>    runtime says so: `ipc_client_check_git_tag … Set IPC_IGNORE_VERSION=1`. Set
>    `IPC_IGNORE_VERSION=1` (ABI is append-only per ADR-020) → the weave IPC connects.
>    (Also: content_shell must run **Medium** integrity to match the Medium service,
>    else the service's `OpenProcess(PROCESS_DUP_HANDLE)` on content_shell is
>    `Access denied` → a *different* `-2`.)
>
> **Working recipe → live weave + eyeball PASSED:** `about:blank` +
> `set IPC_IGNORE_VERSION=1` + explorer-handoff (Medium) launch with
> `--enable-inline-3d` and the §11 frame flags (NO `--run-all-compositor-stages-
> before-draw`). `%TEMP%\run_b2c1_aboutblank.bat`. Permanent fix: deploy a matching
> `displayxr-service.exe` to Program Files (elevated) so `IPC_IGNORE_VERSION` isn't
> needed, and change `run_b2c1.bat` to load `about:blank`.

--- historical investigation (conclusion superseded by the RESOLUTION above) ---

With frames now flowing, a failure *appeared* distinct. What looked like clean
attribution was actually confounded by the `file://`-vs-`about:blank` difference:
- weave dormant (service down, or `--enable-inline-3d` removed) → `window.__t`
  climbs to thousands, raw SBS visible (eyeballed: L=green/R=magenta), stable;
- weave engaged (service **up** + `--enable-inline-3d`) → `window.__t` frozen at 0,
  the log ends right after `B2c GPU weave provider registered`, frame production
  stops permanently.  *(← these "weave engaged" runs all loaded `file://`; that,
  not the weave, is why they froze.)*

**It is a hang/stall, NOT a crash** (the "Application Error" dialog was a red
herring — a stray orphan-crash dialog from a prior teardown). Proven by procdump
(`%TEMP%\gpu_hang.dmp`, `browser_hang.dmp`) + cdb (`gpu_stacks.txt`,
`browser_stacks.txt`) while hung:
- **All processes alive** (browser, GPU, renderers, service). WER recorded no fault.
- **GPU process entirely idle** — `VizCompositorThread` parked on
  `NtRemoveIoCompletion` (waiting for work), GPU main in its normal `GpuMain` wait.
  No weave/Mojo-Sync/fence frames anywhere. So the GPU is **not** deadlocked in the
  B2c substitution; it is idle because **no begin-frames are arriving**.
- **Browser `CrBrowserMain` (UI thread) idle** in `MessagePumpForUI::DoRunLoop`
  (normal message loop) — also **not** blocked in the weave.
- The one weave-related block: a **DisplayXRClient.dll background thread stuck in
  `ConnectNamedPipe`** (`KERNELBASE!ConnectNamedPipe` ← `DisplayXRClient!…`),
  i.e. the **runtime's IPC client is waiting for the service to connect a named
  pipe that never connects** — the present-owner / weave handback channel setup
  stalls when the weave session goes live.

So the frame-production blocker (this section's subject) is fully resolved.

**Deeper diagnosis (2026-07 dump session) — it is a begin-frame SCHEDULER STALL,
not a deadlock and not the runtime IPC.** Dumped all four processes while hung
(`%TEMP%\{gpu_hang2,rend_hang2,browser_hang,svc_hang}.dmp` + `*_stacks.txt`):
- **Every process is IDLE, none blocked on a lock or a sync call.** Renderer
  `Compositor` thread parked in `GetQueuedCompletionStatus` (awaiting a BeginFrame),
  GPU `VizCompositorThread` parked on `NtRemoveIoCompletion`, browser `CrBrowserMain`
  in its normal `MessagePumpForUI::DoRunLoop`, service per-client IPC thread in
  `ReadFile` (awaiting the next RPC). The renderer main thread stays CDP-responsive
  (`window.__t` reads return) — only rAF/BeginFrames have stopped.
- **The `ConnectNamedPipe`-in-DisplayXRClient frame was a RED HERRING** — it is
  normal runtime-client infrastructure, not the weave channel (the weave path is
  pure synchronous RPC over the existing client connection, `oxr_weave.c` →
  `comp_ipc_client_compositor_weave_*`; no second pipe is created).
- **The freeze is gated on `--enable-inline-3d` (GPU weave provider registered) +
  service up — NOT on the weave client succeeding.** Reproduced with
  `xrCreateInstance` *failing* (`-2`, no weave session at all) → still `__t=0`.
  With the service **down** (same flags, provider still registers) → `__t` climbs
  to 121, frames fine. So the GPU-side substitution path (active once the provider
  is registered and the service is reachable) stalls the frame pipeline on/after the
  first frame; with **`--disable-gpu-vsync --disable-frame-rate-limit`** the
  unthrottled begin-frame source waits on the previous frame's presentation ack, so
  a single broken/never-completing frame parks the whole pipeline permanently.

**Leading hypothesis:** the B2c `MaybeWeaveSubstitute` / `DrawTextureQuad`-tag path,
when the provider is live, breaks the tagged frame's presentation-feedback (or never
completes that frame), and the unthrottled scheduler never schedules the next
BeginFrame → all processes idle. **NOT** a runtime/service bug (service is idle and
innocent), **NOT** a crash (the `0xC0000022` Application Errors are teardown/orphan
artifacts of the net-service crash-loop, unrelated).

**Sharper symptom:** the renderer never fires rAF **even once** (`window.__t` stays
at exactly 0 — not "climbs then stops"), i.e. the renderer receives **zero**
BeginFrames from the moment the GPU weave provider registers with the service up. So
the stall is at/near **Viz frame-sink bring-up or the provider registration in
`ShellContentGpuClient::PostCompositorThreadCreated`**, not a mid-stream frame that
fails to complete.

**Hypothesis TESTED + DISPROVEN (2026-07):** dropping `--disable-gpu-vsync
--disable-frame-rate-limit` (timer-driven BeginFrames, not presentation-ack-gated)
did **not** help — `__t` still stuck at 0. So it is not a presentation-feedback stall
on frame N; frame 0 never starts.

**Next steps (Chromium B2c side — code-level, NOT black-box; black-box repro only
spams the `0xC0000022` dialogs and yields no new signal):**
1. Add logs at `ShellContentGpuClient::PostCompositorThreadCreated` (provider
   registration) and the earliest Viz frame-sink / BeginFrameSource setup to see what
   the provider registration touches that could suppress the first BeginFrame — the
   provider is process-global and registered on the GPU compositor thread; suspect it
   perturbs Viz init only when the service is reachable (the `service up` gate).
2. Log `SkiaOutputSurfaceImplOnGpu::MaybeWeaveSubstitute` + the `DrawTextureQuad` tag
   site; confirm whether they even run (they may not, if frame 0 never happens).
3. Bisect the B2c patch: does the freeze appear with the provider registered but the
   substitution hook disabled? That isolates "registration perturbs Viz" vs
   "substitution stalls the frame."
Also worth noting: `xrCreateInstance` in the browser weave client returns **-2 even
with the service up** on the Medium/explorer launches — a separate B2a-vs-service
connect issue to run down (integrity/handshake), though it is NOT the cause of the
frame freeze (freeze happens regardless).

**Root cause of the `-2` FOUND (content_shell runtime log,
`%LOCALAPPDATA%\DisplayXR\DisplayXR_content_shell.exe.*.log`):**
```
[oxr_instance_create] DisplayXR runtime … loaded from …\DisplayXRClient.dll
[ipc_receive] ReadFile from pipe … failed: 109 The pipe has been ended.
[ipc_client_setup_shm] Failed to retrieve shm fd!
XR_ERROR_RUNTIME_FAILURE in xrCreateInstance: Failed to create instance '-1'
```
So `xrCreateInstance` **connects to the service**, then the **shared-memory-fd
handshake breaks** (`ERROR_BROKEN_PIPE`/109 during `ipc_client_setup_shm`) — a
handle-duplication / integrity failure between content_shell and the service. Seen
at High content_shell + Medium service (mismatch) *and* on Medium/explorer launches,
so it is not purely integrity — the shm handshake for a browser-hosted client is
failing. **But the frame freeze is decoupled from this** — the freeze's common
factor across all frozen runs is merely that content_shell **establishes the IPC
connection** to displayxr-service (the weave client's `xrCreateInstance`), which is
absent when the service is down or `--enable-inline-3d` is off (both of which give
flowing frames). To confirm client-connection vs GPU-provider as the freeze source,
a `DXR_WEAVE_SKIP_CLIENT=1` env gate was added to `displayxr_weave_client.cc`
(disables the browser client while the GPU provider still registers); bisection
result pending a rebuild.

---

## 12. B3 — the JS surface (WebXR `inline-3d`) + head-tracking + teardown

B2c wove a page's own hand-painted SBS canvas. B3 makes inline-3D a *real* WebXR
session: a JS surface, two head-tracked eye views with off-axis projections, and
proper feature detection + teardown. All on the Chromium branch
`displayxr-inline-3d`; the four seams from §3 are unchanged.

**Standing constraint — per-element eyeball is deferred to real chrome (B4).**
`content_shell` flattens the whole page (canvas included) into **one** GPU-side
web-contents `TextureDrawQuad` (proven by a PID+pointer trace: the renderer tags
its cc layer tree, but `AppendQuads` runs in the GPU process on a *different*
`TextureLayerImpl` that never gets the per-canvas bit). So the per-element cc tag
can't cross the renderer→GPU flatten boundary here; it is correct-by-construction
for standard chrome (renderer submits per-element quads straight to Viz). B3 is
validated here by compile-green + numeric CDP checks + the negative control; the
glasses-free per-element eyeball lands at B4.

### 12.1 B3a–B3c (recap)
- **B3a** — an explicit per-element `weave_target` bool flows
  `cc::TextureLayer` → `TextureLayerImpl` → `TextureDrawQuad` → `quads.mojom`
  (both getter + `Read`, since cc runs in the renderer and the quad serializes
  renderer→Viz) → `SkiaRenderer::DrawTextureQuad`. `DisplayXRInline3D`
  RuntimeEnabledFeature (experimental). The tag must also be carried pending→active
  in `TextureLayerImpl::MovePropertiesToActiveLayer` (it flips after the canvas
  first composites).
- **B3b** — `navigator.xr.requestSession('inline-3d')` is a Blink-local **sensorless
  inline** session (no device/vr mojo). `XRDisplayLayer` (a `ScriptWrappable`, *not*
  an `XRCompositionLayer` — the weave is cc-tag-driven, so the Layers backend is
  skipped) tags its target canvas via `HTMLCanvasElement::SetInline3D` →
  `TextureLayer::SetWeaveTarget`. `V8EnumToSessionMode` collapses `inline-3d` → the
  device `kInline` mode.
- **B3c** — Seam-D per-element committed rect via
  `ExternalUseClient::ImageContext::weave_rect` (set in `DrawTextureQuad` from
  `quad_to_target_transform.MapRect(visible_rect)`); `MaybeWeaveSubstitute` loops
  every tagged context with per-target woven/fence state in a
  `base::flat_map<Mailbox, DisplayXRWeaveTargetState>` keyed by the source canvas
  mailbox.

### 12.2 B3d — two head-tracked eye views with off-axis (Kooima) projections
The session now produces **two** `XRView`s whose projections are window-relative
off-axis frusta built from the runtime's tracked eyes.

- **`is_inline_3d` threading (Blink).** `V8EnumToSessionMode` erases the inline-3d
  distinction, so `requestSession` captures `mode.AsEnum() == kInline3d` *before*
  the collapse and carries a bool on `PendingRequestSessionQuery` →
  `CreateSensorlessInlineSession` → `CreateSession` → the `XRSession` ctor.
- **Two views (`XRSession::UpdateInlineView`).** When `is_inline_3d_` and eyes +
  element geometry are available, emplace two `XRViewData` (left = index 0 / left
  canvas half, right = index 1 / right half — matching `XRDisplayLayer::getViewport`)
  instead of the single mono view; `getViewerPose` returns both automatically
  (`XRViewerPose` iterates `session()->views()`). Falls back to the mono view until
  eyes arrive (and as the negative control).
- **Off-axis projection + per-eye pose.** Each eye sets *both* halves of the
  display-centric Kooima decomposition on its `XRViewData` (an `XRViewGeometry`):
  `UpdateProjectionMatrixFromFoV(up,down,left,right,near,far)` for the asymmetric
  frustum, and `SetMojoFromView(translate(eye))` placing the eye at its tracked
  display-space position. Head motion moves the eye → shifts both the pose and the
  frustum skew → glasses-free look-around. Math ports
  `docs/architecture/kooima-projection.md` §7 + "Windowed Mode" (the element is the
  physical window; its rect projected through each eye gives the four half-angles).
- **Eyes → JS: a new *blink-accessible* `DisplayXRService` mojom.** The tracked eyes
  already return on every weave RPC (`DisplayXRWeaveResult.eyes`) but were discarded.
  **Hard layering constraint:** Blink cannot depend on the `content/shell` weave
  mojom, so a new interface lives at
  `third_party/blink/public/mojom/xr/displayxr_service.mojom`
  (`IsInline3DSupported() => (bool)`, `GetLatestEyes() => (array<float> eyes, bool
  valid, DisplayXRDisplayInfo)`), a `mojom_component` consumed by both content/shell
  (regular variant) and the xr module (`_blink` variant), mirroring how the xr module
  already reaches `device::mojom::blink::VRService`. It is bound frame-scoped via the
  `BrowserInterfaceBroker` (`ShellContentBrowserClient::
  RegisterBrowserInterfaceBindersForFrame`, `RustTestService` pattern), gated on
  `--enable-inline-3d`. The renderer's inline-3d session pulls `GetLatestEyes` each
  animation frame (async, one-frame-lagged cache) and builds the projections from it.
- **Browser cache + geometry.** `DisplayXRWeaveClient` caches the latest eyes +
  committed weave rect (written from `DisplayXRWeaverImpl::WeaveSubmit`), queries the
  physical display dimensions once via `XR_DXR_display_info`
  (`xrGetSystemProperties`), and converts the window-relative weave rect to a
  **display-space element rect** (bound-window client origin + committed rect) so the
  renderer's window-relative Kooima is exact. (Multi-monitor origin mapping is a B4
  refinement; the weave display is assumed to host the window, the same assumption
  `bindWindow`'s phase-snap makes.)

### 12.3 B3e — feature detection, teardown, prune
- **`isSessionSupported('inline-3d')`.** Previously always `true` (inline-3d
  collapses to plain inline, which "is always supported"). Now special-cased before
  the collapse to route through `DisplayXRService::IsInline3DSupported()`, which
  returns the browser's real session/weave-live state. **Negative control:** without
  `--enable-inline-3d` the browser never binds the service → the renderer remote
  disconnects → resolves `false`; the GPU weave provider is never registered either,
  so a still-created inline-3d session is inert (mono fallback, canvas tag drives
  nothing).
- **Teardown untag.** `XRSession` now tracks its live `XRDisplayLayer`s and calls
  `close()` (the sole untag hook: `SetInline3D(false)` → `SetWeaveTarget(false)`) on
  each from `ForceEnd`, so ending the session stops the weave and the GPU sees the
  source mailbox untagged next frame.
- **GPU state prune.** `MaybeWeaveSubstitute` now records the source mailboxes tagged
  this frame and, after the loop, destroys the woven SharedImage + erases every
  `displayxr_weave_state_` entry not seen this frame (the map otherwise grows
  unbounded and leaks woven SharedImages across canvas resize / mailbox churn).

---

## 13. B4 — real `chrome` port + the disproven per-element premise (2026-07-06)

B4 moved the patch from `content_shell` to the real `chrome` target to get the
per-element glasses-free eyeball `content_shell` architecturally can't reach (it
flattens the page). The chrome port **works and is wired end-to-end**; the
per-element weave **does not fire**, and the reason invalidates the Seam-B premise.
Checkpoint commit: `displayxr-inline-3d` @ `c1753d518a77b`.

### 13.1 The chrome port (works)
The `cc` / `components/viz` / `gpu` / `third_party/blink` changes are
layer-agnostic and already compile into `chrome`; only six `content/shell`-only
files needed a home. They were extracted into a **shared `components/displayxr/`
component** (`common` weave mojom, `browser` weave client + weaver/service mojom
impls, `gpu` provider), consumed by *both* `content_shell` and `chrome`. The three
`Shell*` hook sites were mirrored onto the chrome embedder:
- `ChromeBrowserMainParts::PostBrowserStart` → weave-client init, but **deferred
  to a delayed UI-thread task** (chrome shows its first window via a posted task,
  so at `PostBrowserStart` the HWND exists but isn't visible yet; the one-shot
  `bindWindow` needs a visible top-level window — content_shell got away with a
  synchronous call because it shows its window synchronously).
- `ChromeContentBrowserClient` → `BindGpuHostReceiver` (GPU→browser weave bridge)
  + `AppendExtraCommandLineSwitches` (forward `--enable-inline-3d`) +
  `PopulateChromeFrameBinders` (`blink::mojom::DisplayXRService` frame binder).
- `ChromeContentGpuClient::PostCompositorThreadCreated` → GPU weave provider.

**Validated live on Leia (`--enable-inline-3d --enable-blink-features=DisplayXRInline3D`):**
weave-client init, `xrWeaveBindWindowDXR -> 0`, GPU provider registered,
`XR_DXR_display_info` query, `isSessionSupported('inline-3d')` → `true` (full
DisplayXRService mojom round-trip), `XRDisplayLayer` constructs + tags the canvas
(`layer:true, targetOk:true`). No `IPC_IGNORE_VERSION`, no integrity mismatch
(Medium chrome via explorer-handoff matched the Medium service).

### 13.2 The finding — chrome never gives the canvas a taggable SkiaRenderer quad
Instrumenting `SkiaRenderer::DrawTextureQuad` (does a tagged quad reach it?) and
`SkiaOutputSurfaceImplOnGpu::MaybeWeaveSubstitute` (any tagged `ImageContext`?)
with a late-window + unconditional-on-`weave_target` throttle showed **the canvas
never becomes a distinct `SkiaRenderer` `TextureDrawQuad`** — `weave_target=1`
*never* logged, `MaybeWeaveSubstitute` always `tagged=0`. Cross-checked with CDP
`LayerTree`:

| Scenario | Result |
|---|---|
| Sub-page **2D** canvas | Squashed into the page content layer — no canvas compositing layer, no canvas quad. |
| Sub-page **WebGL** canvas | Same — folded into the page layer (even GPU-backed). |
| `will-change:transform` (verified applied) | Creates a compositing layer but **still** no canvas `TextureDrawQuad`. |
| **Full-window** WebGL canvas | **Zero** `DrawTextureQuad` calls — drawn via the overlay/root path, bypassing `SkiaRenderer`. |
| Whole web-contents at `SkiaRenderer` | One ~`1800×1200` `kTextureContent` quad (pre-composited) + scrollbars. |

**So the §3/§4/§7 Seam-B premise — "real chrome's renderer submits per-element
quads to Viz, so the tag reaches the per-canvas quad" — is false.** On Windows
chrome a canvas element is either (a) squashed into the page content layer (its
pixels rasterized into the page, never a `TextureLayer`/`TextureDrawQuad`), or
(b) promoted to a **DirectComposition overlay** (its own surface, but composited
by the system — never a `SkiaRenderer::DrawTextureQuad`). In neither case does the
tag-at-`DrawTextureQuad` + substitute-woven-`SkImage` seam ever run for the canvas.
(`content_shell`'s B2c.2 eyeball only passed because its UI-compositor-in-GPU
*flattens* the full-window canvas into the single quad `SkiaRenderer` draws — a
quirk, not the per-element path.)

### 13.3 Redesign direction — the CEF sub-rect model (Seam B, open)

**Reframing from the proven Step-A reference.** The `displayxr-cef-host` (Step A)
*did* weave per-element inline-3D on Leia, and re-reading its source
(`src/weave_compositor.cpp`, `src/cef_client.cpp`) shows **why**, and why Step B's
seam was wrong. CEF ran in **offscreen-rendering (OSR)** mode: Chromium composited
the **whole page** — the 2D surround *and* the canvas's flat SBS pixels, already
squashed together — into one shared texture and handed it to the host via
`OnAcceleratedPaint(shared_texture_handle)` → `pageTex`. The host was then an
**external compositor**: per element per frame it `CopySubresourceRegion`'d the
element's sub-rect **out of the finished page texture** (`weave_compositor.cpp:294`),
wove that rectangle, drew the page base into its own back buffer, composited each
woven sub-rect back over it, and **presented to its own window**
(`:254–344`). Element rects came from JS via the host bridge; weaves were
serialized per element (submit → wait → copy → blit) so accumulation-vs-serialize
(§5 #1) never mattered.

**The takeaway that inverts Seam B:** the CEF host **never needed the canvas to be
a distinct quad.** The canvas SBS was *baked into the page texture at its rect* —
exactly the "squash" §13.2 documents in chrome — and the host simply weaved a
**rectangle of the finished page**. Step B instead tried to substitute a woven
`SkImage` for the canvas's *own* `TextureDrawQuad` inside Viz (Seam A/B), which
requires a per-canvas quad chrome never produces. The `1800×1200 kTextureContent`
quad §13.2 kept finding **is** CEF's `pageTex` (the composited web-contents).

**So the redesign is to transplant the CEF external-compositor flow *into* Viz,**
not to chase the DComp-overlay path (an earlier draft of this section suggested
`OverlayProcessorWin`/`DCLayerOverlay`; the CEF model is simpler, proven, and
sidesteps the compositing-mode fight entirely):

1. **Element rects → Viz via `CompositorFrameMetadata` (kills Seam-D staleness).**
   `XRDisplayLayer` reports its canvas's window-relative device-px rect
   (`getBoundingClientRect × DSF`) into the renderer's `CompositorFrameMetadata`
   (a new `inline_3d_rects` list). This is the Chromium-native equivalent of the
   CEF host's JS-reported rects, and it arrives at Viz **atomically with the frame**
   whose pixels it describes — so the rect always matches the composited page (the
   §3 Seam-D "one-frame-stale → lattice collapse" hazard disappears; no need to read
   a quad transform). Replaces the B3a `weave_target` cc/quads.mojom tag plumbing.
2. **Post-paint copy → weave → composite in Viz (the CEF `Composite` loop).** In
   `SkiaOutputSurfaceImplOnGpu`, **after** the frame's quads are painted (the
   composited page with the flat SBS canvas is now in the output target) and
   **before** `SwapBuffers`, for each `inline_3d_rects` entry:
   `CopySubresourceRegion` the output-target sub-rect → owned keyed-mutex SBS input
   → `xrWeaveSubmitDXR` (via the unchanged Mojo bridge) → fence-wait → draw the
   woven texture back over the output at that rect. This mirrors
   `weave_compositor.cpp` step-for-step (page base already present in the target;
   overlay each woven sub-rect). Replaces the per-quad tag + `MaybeWeaveSubstitute`
   at `DrawTextureQuad`.
3. **Present unchanged** — chrome's normal `SwapBuffers`.

**Reused as-is:** the browser↔GPU Mojo weave bridge, the OpenXR weave client +
present-owner session (`bindWindow`), `EnsureSbsInput`/keyed-mutex input, the
woven-handle import + `D3DSharedFence` wait, the shared `components/displayxr/`
component, and the JS surface (`XRDisplayLayer`, `inline-3d` session, feature
detect). **Changed:** `XRDisplayLayer` reports a rect into `CompositorFrameMetadata`
instead of tagging a cc `TextureLayer`; the GPU weave moves from
`DrawTextureQuad`-substitution to a post-paint output-target overlay loop.
**Dropped:** the B3a `weave_target` bool on `cc::TextureLayer` / `TextureLayerImpl`
/ `TextureDrawQuad` / `quads.mojom`, and the `ImageContext` tag / `MaybeWeave
Substitute` substitution path.

**Open sub-questions for the redesign build (all now resolved — see §13.4):**
- Output-target readback: **resolved.** The composited output is read via
  `SkSurface::readPixels` (Ganesh) / `asyncRescaleAndReadPixelsAndSubmit` (Graphite)
  from the surface being painted — the GL output-device surface under
  `--disable-direct-composition`, or the **root render-pass backing surface** under
  DComp (a readable swap-chain-backed image). The read happens before the woven
  draw-back, so there is no read/write hazard and no scratch copy is needed.
- Drawing the woven texture back: **resolved** — drawn as a final Skia
  `drawImageRect` (`kSrc`, nearest) into the same surface at the rect, held alive
  across the flush+submit. No DComp overlay visual was needed.
- 2-view geometry (B3d) is unaffected — once eyes flow from a firing weave, the
  existing `UpdateInline3DViews` path activates; B4c look-around then becomes
  testable.

### 13.4 B4b — implemented + eyeball PASSED (2026-07-07)

The §13.3 CEF sub-rect model is **built and validated end-to-end in real `chrome`**
(branch `displayxr-inline-3d`, commits `33a7971ff5fa8` + cleanup `14efab4d45aaa`).
Per-element weave fires: the bordered canvas is glasses-free 3D while the 2D chrome
stays flat — the boundary `content_shell` (page-flatten) could not reach.

**As-built vs §13.3.** Rect transport is exactly as designed:
`XRDisplayLayer` → `XRSession::ReportInline3DRects` (getBoundingClientRect × DSF) →
`FrameWidget::SetInline3DRects` → `LayerTreeHost` commit-state →
`CompositorFrameMetadata::inline_3d_rects` (new field + `array<gfx.mojom.Rect>` mojom +
traits) → `SurfaceAggregator` → `AggregatedFrame` → `Display::DrawAndSwap` →
`SkiaOutputSurface::SetInline3DRects` → GPU-thread `MaybeWeaveOutput`. The B3a
`weave_target` cc/quads.mojom tag + the `DrawTextureQuad` substitution were **removed**.
The **readback** differs from the §13.3 "output-target sub-rect copy": Graphite is
async-only for readback and a GPU-created SharedImage came back as a `CompoundImageBacking`
(no `ProduceOverlay`), so the GPU loop does `SkSurface::readPixels` on the composited
output sub-rect and hands the CPU BGRA to a new `DisplayXRWeaveProvider::WeavePixels`
(CPU pixels → intermediate DEFAULT texture → `CopyResource` into the keyed-mutex input —
`UpdateSubresource` directly into the shared texture invalidates its NT handle). The
woven texture is drawn back with a Skia `drawImageRect`, holding the read accesses alive
across the flush+submit.

**Blink gates that had to open for inline-3d to animate** (a sensorless inline session
has no XRWebGLLayer base layer): treat a registered `XRDisplayLayer` as a valid render
state (`MaybeRequestFrame`), bypass the focus gate (`XRFrameProvider::ProcessScheduledFrame`
— a glasses-free display must weave unfocused, and gating it stalls the loop headless), and
add a minimal inline-3d `OnFrame` path (callbacks + rect report, no layer/transport submit).

**`--disable-direct-composition` RETIRED (2026-07-09) — weave on the DComp root render-pass.**
This was the last launch-flag crutch, and the earlier framing (it "needs the zero-copy D3D-texture
path") turned out to be wrong. The root cause was **where** the weave hook lived, not CPU-vs-GPU
copy. `MaybeWeaveOutput` ran only from `FinishPaintCurrentFrame` — the **GL output-device** path,
which `--disable-direct-composition` forces (`SkiaOutputDeviceGL` with a readable backbuffer
`SkSurface`). Under chrome's default DirectComposition, `renderer_allocates_images = true`, so the
composited page (2D chrome + each inline-3d canvas's flat SBS, squashed together — the CEF `pageTex`
equivalent) is drawn into the **root render-pass backing** via `FinishPaintRenderPass`, a writable
window-sized Skia surface; the output-device surface is never painted
(`SkiaOutputDeviceDComp::BeginPaint` is `NOTIMPLEMENTED`). So `MaybeWeaveOutput`'s
`scoped_output_device_paint_->sk_surface()` guard bailed and nothing wove — exactly the "binds the
window but no `process_atlas`" symptom observed on 2026-07-08.

The root render-pass backing is a readable Skia surface (under DComp it is `root_buffer_queue_`-managed
with `scanout_dcomp_surface = false`, i.e. a swap-chain-backed image, **not** a write-only DComp
surface), so the **same** composited-sub-rect readback+weave loop serves both output devices — no
zero-copy D3D-texture path was needed. Fix (`skia_output_surface_impl_on_gpu.{h,cc}`): the weave
loop was extracted into `WeaveCompositedSurface(output, out_canvas)`; `MaybeWeaveOutput` (GL) still
delegates to it unchanged, and a new `MaybeWeaveRootRenderPass` hook in `FinishPaintRenderPass`
(both the Ganesh `DrawDDL` and Graphite `insertRecording` branches) runs it on the window-sized
(`== size_`), `is_overlay` root pass. Non-root overlay passes are element-sized and skipped, and the
two paths are mutually exclusive (under GL the root uses `BeginPaintCurrentFrame`, so the render-pass
hook never matches — no double-weave; verified: `MaybeWeaveRootRenderPass` is **absent** from the GL
run's log and present in the DComp run's). **Verified 2026-07-09:** with `--disable-direct-composition`
removed, `MaybeWeaveRootRenderPass: DComp root pass 2593x1974 rects=1` fires, `canvas weave input
ready 1810x1054`, the service weaves (`leia_dp_d3d11_process_atlas weave: target=2593x1974
vp=(374,920 1810x1054)`) with zero `0x80070057`, and the inline canvas is glasses-free 3D (user
eyeball). GL path (flag present) re-verified unchanged. Chromium branch `displayxr-inline-3d` commit
`485518796a94c`. The GPU-resident weave that removes the CPU readback landed later as **B4d**
(§13.5). **All frame-path and output-device launch flags are now retired** — `run_b4.bat` is just
`--enable-inline-3d --enable-blink-features=DisplayXRInline3D
--disable-features=CalculateNativeWinOcclusion,DelegatedCompositing`.

### 13.5 B4d — GPU-resident weave (skip the per-frame CPU readback), 2026-07-09

The DComp weave path read each inline-3D sub-rect back to CPU (`SkSurface::readPixels` on Ganesh,
`asyncRescaleAndReadPixelsAndSubmit` on Graphite) and re-uploaded it via
`DisplayXRWeaveProvider::WeavePixels` — a synchronous GPU→CPU→GPU roundtrip **on the compositor's GPU
thread every frame**. The megabytes are not the point; the *sync readback stalls the GPU pipeline*,
which is invisible for a single small element but is a latency/scaling liability for the
multi-element, high-res spatial-desktop product. B4d makes the copy GPU-resident.

**Design.** `WeaveCompositedSurface` gained a `prefer_zero_copy` arg (`true` from
`MaybeWeaveRootRenderPass`, the D3D-backed DComp root pass; `false` from `MaybeWeaveOutput`, the GL
`--disable-direct-composition` path, which keeps the `WeavePixels` readback — the ANGLE backbuffer
has no extractable texture). On the zero-copy path, per rect: Skia-copy the composited sub-rect from
the output `SkSurface` into an **owned scratch `D3DImageBacking`** (`makeImageSnapshot` +
`drawImageRect`, a GPU-only copy), `ProduceOverlayForWeave` its `ID3D11Texture2D`, and hand that to
the already-present (previously dead) provider primitive `WeaveCanvas`, which
`CopySubresourceRegion`s it straight into the keyed-mutex weave input. No CPU pixels, no readback
stall. There is **no read/write hazard with the root backing's open write access** — we snapshot the
output surface (exactly as `readPixels` already did) and only `ProduceOverlay` a *separate* scratch
mailbox. Any zero-copy step failing falls back to `WeavePixels` for that rect (still correct).

**Two structural gotchas under Graphite-Dawn** (both cost a build; the "clean" route the B4c note
imagined — `ProduceOverlay(root_mailbox) -> d3d11_video_texture()` — is a dead end because the DComp
root backing is a `DXGISwapChainImageBacking`/`DCompSurfaceImageBacking` whose overlay image is the
swap chain / DComp surface, `kDCompVisualContent`, not a raw texture):
- **A `SCANOUT` scratch is not sampleable.** `SCANOUT` passes `ProduceOverlay`'s manager gate but
  forces `want_dcomp_texture` (`d3d_image_backing_factory.cc`), so the overlay image is a DComp visual
  (`kDCompVisualContent`), not a raw `ID3D11Texture2D`. So the scratch is **non-SCANOUT**, read via
  **`ProduceOverlayForWeave`** — `ProduceOverlay` minus the `SCANOUT` enforcement — restored to
  `SharedImageManager`/`SharedImageRepresentationFactory` (it shipped in B2c.2 `656af477`, was removed
  as dead code in B4b `14efab4d`, and is the *only* way to get a raw texture out of a Viz SharedImage).
- **A plain `DISPLAY`-only scratch has no overlay.** Under Graphite-Dawn a `DISPLAY_READ|DISPLAY_WRITE`
  image is claimed by `WrappedSkImageBackingFactory` (no overlay representation → `ProduceOverlay`
  null) before `D3DImageBackingFactory`. Adding **`CPU_READ`** usage (in D3D's supported set, absent
  from WrappedSkImage's Graphite set and DComp's, and *not* a `want_dcomp_texture` trigger) routes the
  scratch to a plain, overlay-exposable `D3DImageBacking` (`kD3D11Texture`).

**Verified 2026-07-09** (Chromium `displayxr-inline-3d` commit `44d39f2d939ba`): DComp run logs
`weave: GPU-resident scratch path (no CPU readback)` + `canvas tex …` (WeaveCanvas ran) + service
`leia_dp_d3d11_process_atlas weave: target=2593x1974 vp=(374,920 1810x1054)`, zero `0x80070057`, user
eyeball glasses-free 3D. GL run (`--disable-direct-composition`) unchanged: `WeavePixels` fallback,
GPU-resident marker + root-pass hook absent. The change is scoped to `WeaveCompositedSurface`
(`skia_output_surface_impl_on_gpu.{h,cc}`) plus the two additive Win-only `ProduceOverlayForWeave`
methods; the GL path's correctness is untouched.

**`--disable-features=SkiaGraphite` RETIRED (2026-07-08) — Graphite-compatible readback.**
`MaybeWeaveOutput` read the composited output sub-rect via synchronous `SkSurface::readPixels`,
which exists only on Ganesh; Graphite has no sync readback, so B4b forced Ganesh. Now
dual-backend: on Graphite, `context_state_->FlushGraphiteRecorder()` then the blocking-async
`GraphiteSharedContext::asyncRescaleAndReadPixelsAndSubmit` (submit + wait) of the sub-rect into
a local `WeaveReadPixelsContext`, feeding `async_result->data(0)`/`rowBytes(0)` to `WeavePixels`
(which already tolerates an arbitrary stride via its DEFAULT upload texture); Ganesh keeps the
sync `readPixels`. Mirrors `SkiaOutputDeviceOffscreen::ReadbackForTesting`; the woven draw-back
+ `FlushSurface` + submit were already backend-agnostic. **Verified:** with the flag removed
`SystemInfo.getInfo` reports `skia_graphite: enabled_on` (Graphite is the live backend, so the
new path runs), the weave fires end-to-end (service `leia_dp_d3d11_process_atlas weave:
target=2593x1974 vp=(374,920 1810x1054)`, zero `0x80070057`, chrome `canvas weave input ready
1810x1054`) and the inline canvas is glasses-free 3D (user eyeball). Chromium branch
`displayxr-inline-3d` commit `7db14cade7986`.

**`--disable-features=TreesInViz` RETIRED (2026-07-08) — inline_3d_rects plumbed through
LayerContext.** Chrome's default `TreesInVizInClientProcess()` forwards each frame to Viz as a
`LayerContext` display-tree update (`LayerTreeUpdate`) that carried `latency_info` +
`tracked_element_rects` but **not** `inline_3d_rects`; on the Viz side
`MakeCompositorFrameMetadata` then rebuilt metadata with an empty `inline_3d_rects`
(`active_tree()->inline_3d_rects()` is never populated on the Viz-side tree), so
`MaybeWeaveOutput` saw no rects and never wove. Fixed by carrying `inline_3d_rects` through the
update, mirroring `tracked_element_rects` exactly: a new `array<gfx.mojom.Rect> inline_3d_rects`
on `LayerTreeUpdate`; the param threaded through `LayerContext::UpdateDisplayTreeFrom` (+ base
virtual + viz/fake/test impls) and `LayerTreeHostImpl::UpdateDisplayTree` (the client passes
`compositor_frame.metadata.inline_3d_rects`); applied in `LayerContextImpl` via a new
`LayerTreeHostImpl::set_inline_3d_rects_from_client` / `inline_3d_rects_from_client_` member;
and read back in `MakeCompositorFrameMetadata`'s `trees_in_viz_in_viz_process` branch (the
renderer-side branch keeps reading `active_tree()->inline_3d_rects()`). **Verified:** with
`--disable-features=TreesInViz` removed, the weave fires end-to-end (service
`leia_dp_d3d11_process_atlas weave: target=2593x1974 vp=(374,920 1810x1054)`, zero
`0x80070057`) and the inline canvas is glasses-free 3D (user eyeball). Chromium branch
`displayxr-inline-3d` commit `6578f2719c16f`.

**`--force_high_performance_gpu` RETIRED (2026-07-07) — blocker #5 resolved.** This is a
dual-GPU laptop: the service creates the NT-shared keyed-mutex weave input on the runtime's
adapter (the high-performance NVIDIA RTX 3080, LUID `…0001b9d9`, selected by
`xrGetD3D11GraphicsRequirementsKHR`), while chrome's GPU process defaulted to the Intel iGPU
— so the service's `OpenSharedResource(NT)` on that handle failed cross-adapter
(`E_INVALIDARG` / `0x80070057`). The manual `--force_high_performance_gpu` aligned them.
That is now **automatic under `--enable-inline-3d`**: the existing DisplayXR block in
`ContentBrowserClient::AppendExtraCommandLineSwitches`
(`chrome_content_browser_client.cc` + `shell_content_browser_client.cc`) appends the
`FORCE_HIGH_PERFORMANCE_GPU` driver-bug workaround switch
(`gpu::GpuDriverBugWorkaroundTypeToString(gpu::FORCE_HIGH_PERFORMANCE_GPU)`) to the
**gpu-process** child command line — provably equivalent to the flag (the underscore
`--force_high_performance_gpu` is that workaround name copied through by
`gpu_process_host.cc`; both converge on `FORCE_HIGH_PERFORMANCE_GPU` → ANGLE `EGL_HIGH_POWER`
/ Dawn `HighPerformance` adapter selection). **Verified:** with the flag removed, the GPU
weave device's adapter LUID logged `…0001b9d9` (== runtime) and the weave fired
(`leia_dp_d3d11_process_atlas weave: vp=(374,920 1810x1054)`) with zero `0x80070057`. The
LUID-exact `--use-adapter-luid` path is an unneeded generalization — the runtime always
prefers the high-performance adapter, so forcing high-performance matches it by construction.

**Note on the DP, not the patch:** dumping the input (perfect SBS) and the woven output
(mono left-view) showed the Leia DP was in **2D mode** (`SR D3D11 display mode switched to
2D`) at that moment — 2D↔3D is tracking-driven, so it interlaces to real 3D only when a
face is tracked. Interlacing is the DP's job; the Chromium patch delivers the correct SBS
and composites the woven result regardless.

### 13.6 GPU-process SYNCHRONOUS weave — zero-lag, browser-owned (2026-07-13)

The async submit (§13.5's model: mojo `WeaveSubmit` GPU→browser with a callback, woven result
drawn one frame later) was the deadlock-safe fix for the **GPU-process sync-mojo ban** (the
`sync_call_restrictions.cc` FATAL that crashed static/official builds), but its 1-frame floor was
user-rejected ("cannot afford ANY lag"). The replacement — gated `--inline-3d-sync-weave`, async
path untouched as the default-flag fallback — moves the **OpenXR present-owner weave session into
the GPU process itself**:

- **Session creation pre-sandbox:** `content/gpu/gpu_main.cc` `PreSandboxStartup` (before
  `LowerToken()`) creates the D3D11 device + weave session and connects the runtime IPC pipe. The
  sandbox blocks *opening* the pipe, not I/O on an already-open handle (same pattern as
  MediaFoundation warming up pre-sandbox).
- **Per-frame submit is a plain synchronous call on Viz's present thread**
  (`DisplayXRWeaveGpu::SubmitSync` → `DisplayXRWeaveClient::Submit` → `xrWeaveSubmitDXR`,
  ~1 ms) — no mojo, no browser hop, no deadlock class at all. `WeaveCompositedSurface`'s 2-phase
  submit-then-take structure is unchanged and becomes automatically zero-lag once the submit
  stores its woven result synchronously in the same frame.
- **Runtime-side prerequisites (merged, PR #743 / v2.0.2):** the IPC pipe needed a Low
  mandatory-integrity label + RESTRICTED SID (Chromium's GPU process is **Low even
  pre-sandbox**), `ConnectNamedPipe` mid-connect-abort hardening, and a tagged-DXGI handle-send
  path that skips `OpenProcess` (a Low sender cannot open the Medium service). The sync input is
  a legacy DXGI keyed-mutex handle (`inputIsDxgi`), so steady-state weave transfers no NT handles.
- **Component-build gotcha:** the weave client holds the session in a process-global; as a GN
  `source_set` linked into two DLLs each got its own copy (GPU-created session invisible to the
  per-frame weave). It is now a GN `component()` with `COMPONENT_EXPORT`.

**Verified 2026-07-13** (fork commit `2ff0cda`): `GPU-process SYNC weave live … eyes_valid=1`,
zero submit failures, user-eyeballed. This also **removes the static/official crash root cause**
— the StaticDbg no-FATAL proof and official P0 re-verify are the remaining checkboxes on
displayxr-browser#12.

### 13.7 Drag phase-snap — window constrained to the DP phase grid (2026-07-13)

The interlace lattice is fixed to the physical panel, so a woven window is only phase-correct at
the positions it was woven for. Two complementary pieces (CEF-host parity, fork commits
`c188758` + `be01cb3`):

- **Per-frame phase-ref sync:** `SubmitSync` calls `SnapWindowIfMoved` →
  `xrWeaveSnapWindowRectDXR`, keeping the DP's phase reference locked to the browser window's
  current screen position.
- **Drag constraint:** a `SetWindowSubclass` on the browser's own frame window intercepts
  `WM_ENTERSIZEMOVE`/`WM_WINDOWPOSCHANGING` and **overrides `pos->x/y`** so the client-area
  origin only lands on phase-aligned positions during a drag. The DP grid is a **non-uniform 2D
  lattice** (the slanted lenticular couples X and Y), so each step queries the DP snap rather
  than replicating the grid locally. A second, snap-capable browser-process session provides the
  snap PFN; it coexists with the GPU weave session (verified — no SR recalibration break).

### 13.8 Compositor-thread weave rects — the scroll-trail fix (2026-07-13)

**Symptom:** with sync weave live, woven content still *trailed* the page on a fast scroll
(trail, not shimmer → rect lag, not lattice quantization). The rects were measured by Blink
`getBoundingClientRect`×dsf on the **main thread** and corrected on the impl thread by the
viewport `SyncedScrollOffset::Delta()`×dsf (§13.4's Phase 2) — a commit-time snapshot plus an
approximate patch that could not track the exact pixels presented.

**Fix (fork commit `228f3ec8`):** source the weave rect from cc's native **tracked-element-rects**
infrastructure, which projects a layer-relative rect through the layer's `ScreenSpaceTransform()`
**at draw time** — live property trees, current impl scroll — so the rect matches the presented
pixels exactly, every frame, nested scrollers included. It operates at the **paint-chunk** level,
which crosses the canvas squash (§13.2's finding — the canvas has no distinct cc layer/quad), and
upstream already ships the map through TreesInViz in the same `LayerTreeUpdate` as the scroll.

- **Registration:** `XRDisplayLayer` ctor/close registers/clears a `TrackedElementSubRect` on the
  canvas element under a new `viz::TrackedElementFeature::kInline3dWeave`
  (`should_add_to_compositor_frame_metadata=true`). No mojom changes anywhere — the feature enum
  travels as int32.
- **Paint gate:** an undecorated canvas never painted the background chunk that carries
  tracked-element data (`ReplacedPainter::Paint`'s canvas carve-out); extended the else-chain
  with `GetTrackedElementSubRects()`, exactly as region capture did.
- **cc quirk (cost one diagnostic build):** the tracked chunk lands on a layer with **no drawable
  content**, whose `visible_layer_rect` is empty — upstream's intersection zeroed the rect
  (`… 0x0` arriving in Viz). For the weave feature cc now skips that intersection and projects
  the raw element rect (matches the legacy unclipped `getBoundingClientRect` semantics; the weave
  path clips to the window itself).
- **Consumer seam:** `SurfaceAggregator::CollectTrackedElementRects` prefers the tracked rects
  (transformed by the same `TransformRectToDestRootTargetSpace`, excluded from the generic map so
  they don't leak into CopyOutput geometry); the legacy `metadata.inline_3d_rects` loop runs only
  when no non-empty tracked rect is present — a live fallback, with empty rects filtered so they
  can never suppress the weave. Rects are sorted (y, x, w, h) at handoff because the weave
  consumer keys per-target state by vector index. Phase 2's shift stays but only touches the
  legacy path.

**Verified 2026-07-13** (Leia, 5-element gallery, AWS box build at tag+26): five distinct
per-canvas layers each project a full-size scroll-current rect; aggregator log
`weave rects from tracked elements: n=5`; **fast-scroll trailing GONE — user: "it's PERFECT
now"**. Closes displayxr-browser#13.

### 13.9 Batched weave submit — N rects, one `xrWeaveSubmitDXR` per frame (2026-07-14)

**The scaling wall:** the §13.6 sync weave does one synchronous `xrWeaveSubmitDXR` per visible
inline-3d element per frame, serialized on Viz's present thread, ~1 ms each (measured ~965 µs).
The per-rect DP weave cost is bounded by window pixels (every submit accumulates into the ONE
shared window-sized output — §13.4's multi-element finding), so the N× overhead is entirely
per-submit *fixed* cost: runtime IPC round-trip + `OpenSharedResource` + keyed-mutex
acquire/release + fence signal. 5 elements ≈ 5 ms of the 16.6 ms budget; a scrolling wall of 3D
pictures capped out at ~8-12 visible elements.

**Fix (runtime PR #744 = `XR_DXR_weave` SPEC_VERSION 3 + fork commit tag+27):** ONE submit per
frame carrying N rects.

- **Extension:** a chained `XrWeaveSubmitRectsDXR` (`rectCount` ≤ `XR_WEAVE_SUBMIT_MAX_RECTS_DXR`
  = 32, callers chunk past it) on `XrWeaveSubmitInfoDXR::next`. The chain *switches the input
  layout contract*: absent = the legacy element-sized 2×1 SBS atlas (byte-equivalent to v2, so
  webxr_bridge / the CEF host / pre-batch browser builds run unchanged — `sizeof` of the base
  struct is stable); present = a **window-sized input with each rect's SBS content at that rect's
  own window position** (identity mapping: sample position == weave position). Spec:
  `docs/specs/extensions/XR_DXR_weave.md`.
- **Service:** the DP's `process_atlas` samples its whole SRV as the atlas (no input-offset
  param), so the batch loop first copies each rect out of the shared input into a cached
  exact-size scratch tile (`CopySubresourceRegion`, released the input keyed-mutex right after
  the copies), then runs the *existing* per-rect `process_atlas` into the shared output and
  signals the fence **once** after the loop. No DP/vendor-plugin change → no plugin-ABI gate.
  Wire: `ipc_arg_weave_submit` gains `rect_count` (0 = legacy discriminator) + a fixed
  `ipc_weave_rect[32]`.
- **Browser GPU side:** `DisplayXRWeaveGpu` grows a batch mode (`SyncMode()` **and** the runtime
  reporting weave spec ≥ 3 — the client now captures `XrExtensionProperties::extensionVersion`,
  previously unread). The per-target keyed-mutex input pool collapses into ONE window-sized
  legacy-DXGI shared input; `WeaveCanvas`/`WeavePixels` become copy-only (same copy count as
  before, dest = the element's own window position), and Viz's Phase A brackets its existing
  per-target loop with `BeginBatchSync(window_size)` … `SubmitBatchSync()`. Phase B is unchanged
  — it already imported the shared window-sized woven output once and drew each rect's region.
  Fresh woven/fence handles ride the first result entry; the rest carry 0 = reuse. The per-target
  v2 path is kept intact as the fallback for older runtimes.
- **Memory:** one window-sized input (~20 MB @ 2593×1974 BGRA) replaces N element-sized inputs.
- **Validation sample:** `displayxr-web` `samples/wall-3d/` — a 60-tile scrolling wall where an
  `IntersectionObserver` lazily creates/`close()`s one `XRDisplayLayer` per picture on viewport
  enter/leave; both the stress test and the lazy-load pattern image-heavy pages should copy.
  Throttled `VLOG(1) batch weave: n=… in …us` reports the one-submit cost.
