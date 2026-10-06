# XR_DXR_weave — Window-Bound Synchronous Weave Service

| Field | Value |
|---|---|
| **Extension Name** | `XR_DXR_weave` |
| **Spec Version** | 17 |
| **Extension Type** | Instance extension (service path — Windows/D3D11, macOS/comp_multi-Vulkan #759, Android/comp_multi-Vulkan #1036, desktop Linux/comp_multi-Vulkan dma-buf #1699 when the service carries its engine; the snap and its bulk grid form also work in-process on desktop Linux, §5c / #1588 / #1723) |
| **Header** | `src/external/openxr_includes/openxr/XR_DXR_weave.h` (canonical; auto-syncs to `displayxr-extensions`) |
| **Status** | Provisional (`1004999190–198` type block, pending Khronos registry; `199` reserved, see §2c; v9+ additions in a fresh `1004999240–249` decade — 240 v9, 241–245 v10, 246 v11, 247–248 v12) |
| **Design history** | `docs/roadmap/webxr-step-b-design.md` §13.6–13.9, `docs/roadmap/android-concurrent-multi-app.md` F11/§10.4, issues #625, #774, #1031/#1036, browser#88, browser#103, #1699, #1723, browser-pvt#180 |

## 1. What it is

A weave *service* for **present-owners**: callers that own their OS window and present
themselves (a browser, the CEF host), but want the runtime's display
processor to weave sub-rects of their window for them. The caller never weaves
(ADR-007/ADR-019): it hands the runtime pre-weave side-by-side stereo pixels + window-relative
rect(s) and composites back a weaved shared texture, gated on a fence. Eyes flow **out**
(runtime → caller) so the caller can render its next frame's off-axis (Kooima) projections;
the interlace itself reads the vendor's tracker DP-internally.

Five entry points:

- `xrWeaveBindWindowDXR(session, hwnd)` — bind the present-owner's window (phase reference).
- `xrWeaveBindWindow2DXR(session, bindInfo)` — (v7) the chainable form of the same bind, so
  the caller can attach an `XrWeaveWindowGeometryDXR` giving the client area's absolute
  on-screen origin + size + display id. Required on Android; optional elsewhere.
- `xrWeaveSubmitDXR(session, submitInfo, output)` — synchronous weave; returns dims, fence
  value, tracked eyes; hands back the shared woven-texture/fence HANDLEs on the first call and
  on re-allocation (resize).
- `xrWeaveSnapWindowRectDXR(session, origin, target, snapped)` — drag-time phase snap
  (window-position constraint against the DP's interlace lattice).
- `xrWeaveSetScreenFlatRegionsDXR(session, rectCount, screenRects)` — (v8) latch the screen
  regions that must stay physically flat, so the per-region hardware wish excludes them (§2c).

Only the out-of-process (service/IPC) path implements it; in-process sessions report
`XR_ERROR_FEATURE_UNSUPPORTED`.

**One exception, since #1588: `xrWeaveSnapWindowRectDXR` also works in-process.** It is the
only entry point here that moves no pixels and owns no transport — it is a pure query on the
display processor, asking where a window may *land*. The party that needs the answer is
whoever owns the window, and on desktop Linux that is the app itself (see §5c). Requiring a
weave service to ask a question the service is not involved in would put the answer out of
reach of its only caller.

## 2. The two input-layout contracts (v3)

`xrWeaveSubmitDXR` accepts **two mutually exclusive input layouts**, selected by the presence
of a chained `XrWeaveSubmitRectsDXR` on `XrWeaveSubmitInfoDXR::next`:

| | Chain **absent** (legacy, v1/v2 behavior) | Chain **present** (batch, v3) |
|---|---|---|
| `inputTexture` size | The element's rect size | The bound window's client size |
| Content layout | The whole texture is one 2×1 SBS atlas (left view = left half) | Each rect's SBS content sits **at that rect's own window position** (identity mapping; each rect region is itself squeezed SBS) |
| Rect source | Base `rect` field | `rects[0..rectCount)`; base `rect` ignored |
| Weave calls | One sub-rect | Every rect, into the same window-sized output |
| Fence | One signal | **One** signal after the last rect |
| Eyes | Once per call | Once per call |

A batch with `rectCount == 1` is **not** equivalent to a legacy submit — the input layouts
differ. The legacy path is byte-equivalent to spec v2, so pre-v3 consumers run unchanged
(`sizeof(XrWeaveSubmitInfoDXR)` is stable; the chained struct is purely additive).

```c
#define XR_WEAVE_SUBMIT_MAX_RECTS_DXR 32

typedef struct XrWeaveSubmitRectsDXR {
    XrStructureType    type;      // XR_TYPE_WEAVE_SUBMIT_RECTS_DXR (1004999192)
    const void*        next;
    uint32_t           rectCount; // 1..XR_WEAVE_SUBMIT_MAX_RECTS_DXR
    const XrRect2Di*   rects;     // window-relative, device px, y-down
} XrWeaveSubmitRectsDXR;
```

`rectCount` outside `1..32` (or `rects == NULL`) is `XR_ERROR_VALIDATION_FAILURE`. Callers
with more visible elements split into multiple batched submits; the weave fence is one
monotonic timeline, so waiting the last chunk's fence value covers all chunks.

## 2b. The N-view atlas layout (v6, #774)

A chained `XrWeaveSubmitLayoutDXR` supersedes both layouts above with the one every
other DisplayXR app already uses, making a present-owner an ordinary N-view client.

```c
typedef struct XrWeaveSubmitLayoutDXR {
    XrStructureType    type;              // XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR (1004999194)
    const void*        next;
    uint32_t           viewCount;         // == tileColumns * tileRows
    uint32_t           tileColumns;
    uint32_t           tileRows;
    uint32_t           contentViewWidth;  // windowWidth  * activeMode.viewScaleX
    uint32_t           contentViewHeight; // windowHeight * activeMode.viewScaleY
} XrWeaveSubmitLayoutDXR;
```

**Sizing (once, from the display).** `inputTexture` is worst-case-sized across every
rendering mode — `max(tileColumns · viewScaleX · displayWidth) × max(tileRows · viewScaleY
· displayHeight)`, spanning both orientations for modes flagged `CAN_ROTATE`. Sizing from
the **display** rather than the current window is what lets the caller resize and go
fullscreen without reallocating (ADR-010). This max is **not** bounded by the display
size: a mode with `viewScaleX = 1.0, tileColumns = 2` yields a `2W × H` atlas.

**Filling (per frame, from the window).** Tiles are packed **contiguously from the
top-left** at `(contentViewWidth, contentViewHeight)` — tile *v* at
`((v % tileColumns)·contentViewWidth, (v / tileColumns)·contentViewHeight)`. The stride is
the **content** size, not `atlasWidth / tileColumns` (that is the shell/multi-compositor
invariant, see `multiview-tiling.md`). Each visible element is drawn inside tile *v* at its
own window position scaled by `viewScaleX/Y`. Everything right of / below the packed region
is dead space the runtime never reads.

**Runtime behaviour.** No SBS scratch and no per-rect unpack blits. The packed region is
handed to the display processor directly when it exactly fills the active mode's atlas
(zero-copy per ADR-030 — rare: only the worst-case-achieving mode, at fullscreen),
otherwise **one** box copy crops it first (contiguous packing means a single rectangle, not
the per-tile gather the `xrEndFrame` path needs). `rects` degrade to a scope hint (zone /
wish-mask publication — §2c, caller draw-back), so `XR_WEAVE_SUBMIT_MAX_RECTS_DXR` no longer
bounds elements per frame, and `firstChunk` has nothing to clear on this path —
transparency between elements is carried by the caller's own atlas alpha.

Omitting the chain keeps v3/v4/v5 behaviour byte-for-byte.

**Compatibility — gate on `extensionVersion >= 6`, and gate the ASSEMBLY, not just the
chain.** v6 is additive, so it is *invisible* to an older runtime: a v5 runtime silently
skips the unknown `XrWeaveSubmitLayoutDXR`, then takes the batch path and interprets
`inputTexture` as **window-sized with each rect's content squeezed SBS at its own window
position**. Handing that runtime a display-worst-case-sized N-view atlas is a silent
misinterpretation — different dimensions *and* a different content model — not a graceful
fallback. The chain therefore cannot self-negotiate; `XrExtensionProperties::extensionVersion`
is the only signal that the runtime will honour the layout. A caller supporting both must
assemble the **v3/v4/v5 per-rect SBS input** when the runtime reports `< 6`, and the N-view
atlas only when it reports `>= 6`.

## 2c. Per-region hardware wish (v8, browser#88)

Until v8 the weave path drove the panel's physical 3D element **all-or-nothing**: a
present-owner with one woven element held the *whole* panel behind the lens, so the flat 2D
around it was viewed through a lenticular it did not want (the shipped ghosting). v8 lets the
caller name the regions that are **flat**, from which the runtime derives a per-region
hardware **wish**:

```
wish = union(submitted weave rects) − union(flat rects)
```

That is the same wish the `XR_DXR_display_zones` path already publishes (ADR-027 Decision 5),
now reachable from the weave path. Two ways to declare flat, differing only in lifetime and
coordinate space:

| | Per-submit — `XrWeaveSubmitFlatRegionsDXR` | Sticky — `xrWeaveSetScreenFlatRegionsDXR` |
|---|---|---|
| Delivery | chained on `XrWeaveSubmitInfoDXR::next` | its own entry point |
| Coordinates | window-relative device px, y-down (the space of `XrWeaveSubmitRectsDXR::rects`) | absolute **physical screen** px, clipped to the bound window's client area |
| Lifetime | that one submit | latched until the next call — a **SET**, not an add; `rectCount 0` clears |
| Max rects | `XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR` = 16 | `XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR` = 8 |
| Applied | with the submit it rides on | **immediately** — a live wish is re-rastered and republished before the call returns |
| Fits | content that moves every frame (a scrolled page's flat bands) | screen-anchored furniture (a toolbar / tab strip) that must stay flat whatever a frame submits |

The two lists **compose** — both are subtracted. Rects may overlap each other and the weave
rects; subtraction is by area, not by list position. A sticky rect naming panel area outside
the window is clipped away (the wish is published window-anchored), not an error.

```c
#define XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR     16
#define XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR 8

typedef struct XrWeaveSubmitFlatRegionsDXR {
    XrStructureType    type;      // XR_TYPE_WEAVE_SUBMIT_FLAT_REGIONS_DXR (1004999198)
    const void*        next;
    uint32_t           rectCount; // 0..XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR (0 = no flat regions)
    const XrRect2Di*   rects;     // window-relative flat regions, device px, y-down
} XrWeaveSubmitFlatRegionsDXR;

XrResult xrWeaveSetScreenFlatRegionsDXR(XrSession session,
                                        uint32_t rectCount, const XrRect2Di* screenRects);
```

**Advisory and hardware-only**, in exactly the sense of ADR-027 Decision 6 / ADR-030: it
moves the physical 3D element and nothing else. The woven pixels are bit-identical with and
without it — nothing here gates, masks, crops or reorders content — and a runtime or vendor
that ignores the wish is still conformant (its panel is simply 3D where the caller asked for
flat, i.e. the pre-v8 behaviour). A DP with no per-region capability quantizes the wish to
its whole-panel any-nonzero default, which *is* pre-v8. So a caller may always send the flat
lists and never has to ask whether the panel can honour them.

**Rounding always errs toward 3D.** Where a rect edge falls between the hardware's switch
cells, ON (weave) rects are ceiled outward to the cell boundary and flat rects floored
inward. The asymmetry is deliberate: marking a working 3D region flat is a mono regression,
whereas leaving a flat region 3D is only the pre-v8 ghosting. A caller with more flat regions
than the cap merges them into bounding boxes — which errs toward *flat*, so it must merge
only regions that are all flat.

**Compatibility — no version gate needed** (unlike v6). Omitting both is byte-for-byte
pre-v8, and a pre-v8 runtime skips the unknown chained struct and behaves exactly as it does
today, so the chain degrades gracefully rather than being misread. Only the sticky entry point
needs a check: it does not resolve through `xrGetInstanceProcAddr` on a pre-v8 runtime.

`1004999199` is **reserved** for a possible v9 per-submit wish **mask** — an R8 texture handle
in place of a rect list, for callers whose flat geometry is not rectangular (rounded corners,
arbitrary CSS clip paths). Do not assign it to anything else.

**Runtime side.** The weave path publishes the wish through the **same**
`publish_local_zone_mask` channel the display-zones path already used, so no DP/plug-in ABI
change was needed. It is published atomically with the frame (after the atlas + overlay
composite, before the fence signal) and withdrawn on teardown. On a path whose weave engine
publishes no wish, or against a DP without the slot, the flat lists are simply inert — which
the advisory contract makes conformant.

## 2d. Overlay unchanged (v14, ADR-027 Amendment)

Since the ADR-027 Amendment a display processor may composite the v4 overlay **inside** the
weave and band-limit it for its lens, so flat 2D under an active lens stops aliasing per eye.
That filter is per-pixel work over the whole layer; for a page that has not changed it is the
same result every frame. `XrWeaveSubmitOverlayUnchangedDXR` (chained next to
`XrWeaveSubmitOverlaysDXR`) lets the caller say so:

```c
#define XR_TYPE_WEAVE_SUBMIT_OVERLAY_UNCHANGED_DXR ((XrStructureType)1004999290)

typedef struct XrWeaveSubmitOverlayUnchangedDXR {
    XrStructureType          type;             // XR_TYPE_WEAVE_SUBMIT_OVERLAY_UNCHANGED_DXR
    const void*              next;
    XrBool32                 overlayUnchanged; // same overlay pixels as the previous accepted submit
} XrWeaveSubmitOverlayUnchangedDXR;
```

- **Advisory, per submit, no latch.** Omitted or `XR_FALSE` = "may have changed": always
  correct, only slower. The runtime consumes it with the submit it rides on.
- **Lying costs one frame.** `XR_TRUE` for an atlas that did change may show the previous 2D
  for that frame. The DP still re-derives on its own when the texture or its size changes or
  the viewer moves enough to matter.
- **Content-only.** The overlay is composited every frame either way; this only gates
  re-deriving work from it.
- D3D11 service path (`set_overlay_2d(..., layer_unchanged)`) and the desktop-Linux Vulkan
  weave engine (the appended VK DP slot `xrt_display_processor_vk::set_overlay_2d`, same
  argument); the macOS / Android Vulkan weave engines accept and ignore it until they call that
  slot.

### Filter strength (v15)

The lens filter trades aliasing against softness, and the best point depends on the content
(text and UI want more filtering, photos and video less). `XrWeaveSubmitOverlayFilterDXR`
(1004999291), chained next to the overlay, sets it per submit:

```c
typedef struct XrWeaveSubmitOverlayFilterDXR {
    XrStructureType          type;           // XR_TYPE_WEAVE_SUBMIT_OVERLAY_FILTER_DXR
    const void*              next;
    float                    filterStrength; // 0 = no lens filtering .. 1 = the DP's full filter
} XrWeaveSubmitOverlayFilterDXR;
```

- **Omit it unless you know your content.** Omitted = the display processor's own default, a
  vendor value tuned by eye on its panel (Leia SR: 0.6). The runtime passes "not set", never a
  number of its own.
- `filterStrength` outside [0,1] or NaN is `XR_ERROR_VALIDATION_FAILURE`.
- Per submit, no latch. Carried to the DP by the appended slot
  `set_overlay_2d_filter_strength` (D3D11, and the VK variant on desktop Linux; negative =
  default), called before `set_overlay_2d` each frame the layer goes to the DP.



Each submit carries a fixed cost independent of the rect area: the runtime IPC round-trip,
`OpenSharedResource` on the input, the keyed-mutex acquire/release, and the fence signal —
~1 ms wall clock measured on the synchronous GPU-process path. Per-element submits serialize
that N× on the caller's present thread, capping a page at ~8-12 visible woven elements. The
DP weave itself is bounded by window pixels (all sub-rects accumulate into the one
window-sized output), so ONE submit carrying N rects makes 50 visible tiles cost ≈ 1.

## 2e. The runtime presents (v17)

A present-owner whose v4 overlay is its whole 2D page (§2d, "whole-page overlay") gets back, in
the woven output, its **complete** frame. Before v17 it copied that frame over its own back buffer
and presented: a full-window read and write per frame (about 2 × 31 MB at 3840×2040), on top of the
runtime's own write of the woven texture. With `XrWeaveSubmitPresentSurfaceDXR` the runtime
presents the frame itself, on a DirectComposition surface the caller shows as a visual of its own.

```c
#define XR_TYPE_WEAVE_SUBMIT_PRESENT_SURFACE_DXR ((XrStructureType)1004999292)
#define XR_TYPE_WEAVE_PRESENT_STATUS_DXR         ((XrStructureType)1004999293)

typedef struct XrWeaveSubmitPresentSurfaceDXR {   // chained on XrWeaveSubmitInfoDXR
    XrStructureType          type;
    const void*              next;
    void*                    compositionSurface;  // DCompositionCreateSurfaceHandle NT handle, CALLER's process
} XrWeaveSubmitPresentSurfaceDXR;

typedef struct XrWeavePresentStatusDXR {          // chained on XrWeaveOutputDXR
    XrStructureType          type;
    void*                    next;
    XrBool32                 presented;           // XR_TRUE: this submit's frame was presented on the surface
} XrWeavePresentStatusDXR;
```

**The caller's side.** The caller sets up the surface once:

1. Create a surface handle with `DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, …)`.
2. Make a composition surface from it (`IDCompositionDevice::CreateSurfaceFromHandle`).
3. Set that surface as the content of a visual **above** its root.
4. Pass the same handle on every eligible submit.

Each frame, the visual is shown only when `presented` is `XR_TRUE`. The show/hide happens in the
**same** composition commit that presents the caller's own root for that frame:
- **presented:** the visual is shown, and the root content under it no longer matters;
- **declined:** the visual is hidden, and the root carries the copy-back of the woven texture, as
  before v17.

This is the same producer/consumer split Chromium uses for Media Foundation video overlays, with
the roles of the two processes swapped.

**Handles are pulled, not pushed.** The handle is valid in the caller's process.
- A sandboxed, low-integrity caller (Chromium's GPU process) cannot `OpenProcess` the service, so
  the service duplicates the handle *from* the caller.
- That needs the caller's process DACL to grant the user `PROCESS_DUP_HANDLE`, which the browser's
  GPU process already grants pre-sandbox.
- The caller keeps ownership.

**The service's side.**
- `IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle` on the service's render device:
  R8G8B8A8_UNORM (the same encoded bytes the woven texture would hold), premultiplied alpha,
  flip-sequential, two buffers, `FRAME_LATENCY_WAITABLE_OBJECT`, maximum frame latency 1.
- The swap chain is kept across frames. It is recreated when the surface changes (kernel-object
  identity) and resized with the bound window's client area.
- The weave renders straight into its back buffer and the service presents it with `Present(1, 0)`.

**Every pixel is written.** A surface is used only on a submit whose woven canvas is the whole
window (batch or v6 layout) **and** that clears it (`firstChunk = XR_TRUE`). A frame of more than
`XR_WEAVE_SUBMIT_MAX_RECTS_DXR` rects is several submits and is never presented.

**Pacing (#924).**
- The service waits for the frame-latency token for at most **4 ms**, under `render_mutex`, the
  budget every other weave acquire uses. No token means the present is declined.
- Holding the token is what keeps `Present` from blocking on a jammed flip chain.
- If a submit takes the token and is then refused before presenting, the token is **kept** for the
  next submit. The waitable is a semaphore, and dropping a token would leave the chain one present
  short forever.

**Declined, refused, failed.**
- **Declined:** a successful submit that did not present (ineligible, no token, the swap chain
  could not be made) weaves into the woven texture exactly as if the struct were absent, and
  reports `presented = XR_FALSE`.
- **Refused:** a *refused* submit (`XR_ERROR_RUNTIME_FAILURE`, §4b) presents nothing. The surface
  keeps showing the last presented frame, so a caller whose visual is shown can treat a refusal as a
  free **hold**.
- **Failed present:** a `Present` that fails refuses the submit, and the swap chain is dropped until
  the surface changes.
- A presented submit does not write the woven texture. The woven texture therefore holds the last
  frame that was **not** presented.

**Gate.** `extensionVersion >= 17`, with the status struct zero-initialised. An older runtime
skips both and never presents.

**Instrumentation.**
- With `DXR_WEAVE_GPU_TIMING=1` the service's 5 s `weave timing` line gains
  `present(n= declined= token_wait avg/max call avg/max queue avg/max)`: the token wait, the
  `Present` CPU time, and presents queued but not yet on screen (from `GetFrameStatistics`, when
  the composition chain reports it).
- The decline reason (`weave v17: present surface …`) logs on change only.
- The `%TEMP%\dxr_weave_dump_trigger` dump is named `dxr73_weave_output_presented` for a presented
  frame.

**Browser-free check.** `weave_rpc_probe_d3d11_win --present` does exactly the caller's part with
its own window and logs presented/declined every 120 frames. Its own `weave_probe_trigger` dump
reads the woven texture, which a presented frame does not write, so use the service's dump trigger
to see a presented frame.

## 4. Service semantics (implementation notes)

- The output texture is sized to the bound window's client area and is **cleared only when a
  submit sets `firstChunk`** (v5): otherwise each weave writes only its sub-rect(s), so all
  elements accumulate at their window positions. Stale regions from closed elements are then
  harmless — the caller's draw-back composites only current rects.
- The DP's `process_atlas` samples its whole SRV as the atlas (no input-offset parameter),
  so the batch path copies each rect out of the window-sized input into an exact-size
  scratch tile before the per-rect weave. The input's keyed mutex is released right after
  those copies — the caller can begin writing the next frame while the DP weaves.
- The input keyed mutex uses key 0 = "caller done writing, runtime may read"; it is the
  input-ready guarantee (the service imports no caller fences).
- A **legacy DXGI** shared input handle (`inputIsDxgi = XR_TRUE`) crosses the runtime IPC
  low-bit-tagged with no `OpenProcess` — required for Low-integrity sandboxed callers
  (Chromium's GPU process; see #743). NT handles remain supported for Medium callers.

## 4b. Error codes and connection loss

All five *session*-level entry points share one error contract. (The v9
instance-level `xrWeaveExportIpcConnectionDXR` has no session to lose; its errors are in
§4c.)

| Result | When | Is the session still usable? |
|---|---|---|
| `XR_ERROR_VALIDATION_FAILURE` | A chained struct is out of contract (`rectCount` out of range, a NULL `rects`, a zero-sized `clientSize`, an inconsistent `XrWeaveSubmitLayoutDXR`, a handle kind this platform cannot accept). | Yes — caller bug, nothing was submitted. |
| `XR_ERROR_FEATURE_UNSUPPORTED` | The session is in-process. The weave service exists only on the out-of-process (service/IPC) path. | Yes — and it will never succeed on this session. |
| `XR_ERROR_RUNTIME_FAILURE` | The service was reached over a healthy connection and **refused this call** — most commonly `xrWeaveSubmitDXR` losing the input keyed-mutex race (the service gives the input `AcquireSync` 4 ms, §4) while the caller is still writing. A chained v4 overlay that cannot be read this frame (import failed, or its own 4 ms acquire missed) refuses the call the same way, **before** the woven output is touched — so an accepted submit that chained an overlay always means the overlay was composited (by the DP inside the weave, or by the runtime after it; ADR-027 Amendment), and a refused one leaves the previous woven output intact for the caller to keep presenting. | **Yes — retry on the next frame.** This is the expected steady-state miss and must not be treated as fatal. |
| `XR_ERROR_INSTANCE_LOST` | The IPC connection to the service is **gone** (the service exited, crashed, or was restarted under a live client). | **No.** The session is marked lost. |
| `XR_ERROR_SESSION_LOST` | Any call *after* an `XR_ERROR_INSTANCE_LOST`. | No. |

The `INSTANCE_LOST` / `SESSION_LOST` behaviour is what every other IPC-backed OpenXR
call in the runtime (`xrEndFrame`, `xrLocateViews`, `xrSyncActions`, `xrPollEvent`) has
always done; the weave entry points joined it in browser#103, having previously reported
a broken pipe as an ordinary `XR_ERROR_RUNTIME_FAILURE` — indistinguishable from the
transient refusal above, so a weave-only present-owner could not tell "retry next frame"
from "your connection is dead".

**Recovery is a new instance, not a rebind.** There is no partial re-attach: a caller that
sees `XR_ERROR_INSTANCE_LOST` destroys its session + instance and runs the whole
`xrCreateInstance` → `xrCreateSession` → `xrWeaveBindWindow2DXR` sequence again, then
re-asserts its sticky state (`xrWeaveSetScreenFlatRegionsDXR`, window geometry) and
re-exports the woven texture/fence HANDLEs on the first submit of the new session. Back
off between attempts — a service that is crash-looping must not be met with a reconnect
storm — and never mark the failure permanent: a service restart self-heals.

**Version skew is a distinct outcome.** If the installed runtime was upgraded under a
running caller, the re-`xrCreateInstance` fails the client-library ↔ service git-tag gate
and returns `XR_ERROR_RUNTIME_VERSION_SKEW_DXR`
(`src/external/openxr_includes/openxr/XR_DXR_result_codes.h`, browser#103) rather than a
generic `XR_ERROR_RUNTIME_FAILURE`, so a caller can log the real remedy — *relaunch the
application* — instead of retrying blind. It is still not permanent: drop to a long tail
rather than switching off, because a rollback (or an installer that has finished writing)
recovers without a relaunch. A runtime that predates the code reports
`XR_ERROR_RUNTIME_FAILURE` here, indistinguishable from any other create failure.

## 4c. Brokering a connection to a sandboxed sibling (v9, browser#103)

`xrWeaveExportIpcConnectionDXR` exists for one shape of embedder: **the process that
renders is not the process that can reach the runtime's IPC transport.**

```c
XrWeaveIpcConnectionDXR conn = { XR_TYPE_WEAVE_IPC_CONNECTION_DXR };
xrWeaveExportIpcConnectionDXR(instance, &conn);   // browser process
// ship conn.handle (Windows) / conn.fd (POSIX) to the sandboxed process
```

and, in the receiving process, before its own `xrCreateInstance`:

```c
ipc_client_connection_adopt_handle(h);   // Windows; exported from the runtime DLL
ipc_client_connection_adopt_fd(fd);      // POSIX / Android (#1056)
// or: DXR_IPC_HANDLE=<decimal> / DXR_IPC_FD=<n> in the environment
```

Chromium is the motivating case on both platforms. On Android the renderer/GPU process
has no usable Java world, so it cannot run the AIDL connect (#1056). On Windows the GPU
process runs a `USER_LIMITED` restricted token whose restricted-SID list matches neither
ACE on the service pipe's security descriptor, so `CreateFileA` on the pipe returns
`ACCESS_DENIED` — measured, not assumed (browser#103 experiment E0) — while the
unsandboxed browser process opens it routinely.

Three rules make this correct rather than merely convenient:

1. **The export does not handshake.** Connection setup is *connect → shared-memory
   transfer → git-tag check → client description*. Only the connect happens in the
   exporter. The other three MUST run in the adopting process: the shared memory has to
   be mapped in the adopter's address space, and the identity the service settles has to
   be the adopter's. An exporter that handshakes hands over a connection belonging to
   itself.
2. **The adopter declares itself.** On Windows the service derives both the
   handle-duplication target and the peer's integrity level from
   `GetNamedPipeClientProcessId`, i.e. from whoever *opened* the pipe. Under a brokered
   handle that is the exporter, so shared memory, the woven texture and the fence would
   all be duplicated into the wrong process and the weave would report success while the
   caller imported nothing. So an adopted connection sends a **peer declaration** naming
   its own pid as the very first message, before any handle crosses; the service accepts
   it only when the opener's integrity level is **at least** the declared target's — a
   Medium browser may delegate down to its own Low sandboxed child, a Low process may
   never escalate. A refused declaration is not fatal: the connection continues with
   opener attribution, exactly as if nothing had been declared. Details in
   `docs/architecture/service-architecture.md`.
3. **The exporter's own connection is untouched.** Each export opens a new endpoint.

The endpoint is consumed **once**, by the next connection setup in the adopting process,
and the adopter duplicates it — the transferring code keeps ownership of what it passed.
This is not a security boundary: a handle or fd is a capability, and whoever can call
these functions is already inside the process. The receiving process already held this
exact capability in Chromium's case (it is handed the connection before its sandbox is
lowered and keeps it for the browser's lifetime); the broker refreshes an existing
capability rather than granting a new one.

`xrWeaveExportIpcConnectionDXR` is **instance-level**, not session-level: a broker is not
required to be a present-owner, and may export before it ever creates a session. It
reports `XR_ERROR_FEATURE_UNSUPPORTED` on an instance that is provably in-process, and
`XR_ERROR_RUNTIME_FAILURE` when no endpoint could be opened. It is not implemented on
Android, where the connect is Java-side and needs a `Context` the runtime only sees at
`xrCreateInstance`; an Android embedder brokers with the shipped Java connect + `DXR_IPC_FD`
instead.

## 5. macOS platform mapping (#759)

The macOS service (comp_multi + null compositor, Vulkan/MoltenVK) implements the same bind /
submit / snap contract with these platform substitutions (`comp_multi_weave_macos.c`):

| Contract point | Windows (D3D11 service) | macOS (comp_multi Vulkan) |
|---|---|---|
| `windowHandle` | HWND (DP phase snap + `GetClientRect` sizing) | opaque id, stored only (sim/anaglyph has no lattice) |
| `inputTexture` | D3D11 NT / legacy-DXGI shared HANDLE | **IOSurfaceRef** (crosses IPC as a global IOSurfaceID) |
| `inputIsDxgi` | selects legacy-DXGI open path | ignored |
| Input-ready sync | keyed mutex `AcquireSync(0)` | caller completes GPU writes **before** `xrWeaveSubmitDXR` |
| Output sizing | bound window client rect | batch: **input IOSurface dims** (the v3 input is window-client-sized by contract); legacy: rect offset+extent |
| `weavedTexture` | shared NT HANDLE (caller `CloseHandle`s) | retained IOSurfaceRef (caller `CFRelease`s) |
| `fence` / `fenceValue` | shared D3D fence, GPU-wait | **no fence — completion is SYNCHRONOUS**: `xrWeaveSubmitDXR` returns after the weave finished on the GPU. `fence` stays NULL; `fenceValue` is a plain monotonic counter |
| `xrWeaveSnapWindowRectDXR` | vendor DP lattice snap | identity (the VK DP snap slot exists since #1588, but sim/anaglyph has no lattice to snap to) |

The batch algorithm is identical (all rects blitted into ONE window-sized 2×1 SBS scratch, ONE
`process_atlas` per submit). Verification harness: `test_apps/probes/weave_probe_vk_macos`
(headless; CPU-checks the sim anaglyph weave — left-eye-white → red, right-eye-white → cyan).

## 5b. Android platform mapping (#1036)

Android runs the **same** comp_multi Vulkan weave engine as macOS
(`comp_multi_weave_android.c`), with AHardwareBuffer in place of IOSurface. The batch and
N-view algorithms are byte-identical; only the transport, the sync contract's plumbing and
the geometry source differ.

| Contract point | Windows (D3D11 service) | macOS (comp_multi Vulkan) | Android (comp_multi Vulkan) |
|---|---|---|---|
| Window binding | HWND (`GetClientRect` + DP phase snap) | opaque id, stored only | **no handle** — `xrWeaveBindWindow2DXR` + chained `XrWeaveWindowGeometryDXR` |
| Window geometry | derived from the HWND | derived from the input surface dims | **explicit, caller-published**; forwarded to the DP's `set_window_screen_rect` slot (ADR-036 D6 / #1033) |
| `inputTexture` | D3D11 NT / legacy-DXGI shared HANDLE | IOSurfaceRef (global IOSurfaceID on the wire) | **`AHardwareBuffer *`** (the buffer itself crosses the socket) |
| Handle kind | implied + `inputIsDxgi` | implied | declarable via `XrWeaveSubmitHandlesDXR` (v7) on all three |
| Input-ready sync | keyed mutex `AcquireSync(0)` | caller finishes writes before submit | caller finishes writes before submit |
| `weavedTexture` | shared NT HANDLE (caller `CloseHandle`s) | retained IOSurfaceRef (caller `CFRelease`s) | **`AHardwareBuffer *`** the runtime allocated (caller `AHardwareBuffer_release`s) |
| `fence` / `fenceValue` | shared D3D fence, GPU-wait | no fence — completion is SYNCHRONOUS | no fence — completion is SYNCHRONOUS (bounded 1 s wait server-side) |
| `xrWeaveSnapWindowRectDXR` | vendor DP lattice snap | identity | identity (the app does not drag its own window; phase comes from the geometry slot) |

Notes that only bite on Android:

- The vendor DP is a **pure offscreen weaver** — it takes no `ANativeWindow` and renders into
  the VkImage the runtime hands it, so a weave client never competes for the app's Surface.
  Its async init must still be kicked off from a Looper-bearing thread, so DP creation hops to
  the service main thread (#510 M2).
- Client class is `PRESENT_OWNER` (quota 2), declared from the enabled extension set and
  verified service-side (#960 / ADR-035 D1).
- The AHardwareBuffer IPC transport now frames an explicit handle count, because
  `AHardwareBuffer_recvHandleFromUnixSocket` blocks: a receiver expecting one buffer that the
  sender did not send would hang rather than fail.
- **Follow-up, not a correctness gap:** an fd-based (`sync_file`) acquire/release pair would let
  the submit return before the GPU finishes. Today's synchronous contract is the simplest one
  that is correct, and it is what macOS already ships. v10 defines that pair (§5d) but accepts
  it on desktop Linux only; bringing it to Android is a later, additive step.

## 5c. Desktop Linux platform mapping — the snap (#1588), and a full weave platform (#1699)

Desktop Linux became a **full weave platform in v10** (§5d): the transport, the fences and the
wire exist, and a service built with the desktop-Linux `comp_multi` weave engine
(`comp_multi_weave_linux.c`, CMake `XRT_FEATURE_COMP_MULTI_WEAVE_LINUX`) serves bind, submit
and output export exactly as macOS and Android do. **A service built without that engine is
still honest about it:** `xrWeaveSubmitDXR` then reports `XR_ERROR_FEATURE_UNSUPPORTED`
(permanent — not the retryable `XR_ERROR_RUNTIME_FAILURE`), and the service closes every fd it
was sent. `xrWeaveExportIpcConnectionDXR` is unchanged by v10. An in-process session reports
`XR_ERROR_FEATURE_UNSUPPORTED` for everything but the snap, as on every platform.

The rest of this section is the snap, which predates v10 and works either way.

What Linux does have is the problem the snap exists to solve. A windowed weave anchors its
interlace phase to the window's absolute position on the panel, so dragging the window walks
the phase across the lens pitch a few pixels at a time and the 3D breaks up (#1588: ~91
distinct origins over one 8-second drag). The cure is *invariance* — the window may only
land on lattice points, so the pattern is identical at every drag position.

| Contract point | Windows (D3D11 service) | Desktop Linux (in-process Vulkan) |
|---|---|---|
| Who intercepts the move | the runtime's own window proc (`WM_WINDOWPOSCHANGING`) | **the app**: X11 gives a client no hook into the WM's drag, so an undecorated handle app owns its drag and calls this entry point per motion event |
| Session class | out-of-process present owner | **in-process** (`_handle`); the IPC route also exists for a service present owner |
| DP slot | `xrt_display_processor_d3d11::snap_window_rect` (slot 18) | `xrt_display_processor_vk::snap_window_rect` (appended, `XRT_DP_VK_HAS_SNAP_WINDOW_RECT`) |
| Every other entry point | implemented | in-process: `XR_ERROR_FEATURE_UNSUPPORTED`; service: implemented when the service carries the Linux engine (§5d), `XR_ERROR_FEATURE_UNSUPPORTED` from submit otherwise |
| Wayland | n/a | the compositor owns the move and never tells the client where it went, so snapping is impossible by construction — the call still resolves and returns the target unchanged |

Semantics are the Windows ones verbatim: screen pixels in and out, only the top-left is
snapped, the extent passes through. **Both coordinates matter.** A lenticular lattice is
slanted, so the quantity a vendor holds invariant is generally `x + slant·y`, not `x` alone —
an `x` that comes back changed for an unchanged `y`, or vice versa, is correct behaviour and
a caller must apply both.

Two properties of the frame are worth stating, because both are easy to get wrong in a way
that still looks like it works:

- **`origin` and `target` must be in the SAME frame; which frame is irrelevant.** Only the
  displacement is used — the vendor canonicalises `target - origin`, snaps from (0,0) and
  re-adds the origin, so a constant common to both cancels. The lattice is anchored to the
  drag origin, not the panel. Desktop-absolute is the documented choice here and the runtime
  converts nothing on any platform; mixing the two frames between arguments is the one
  genuine error.
- **Device pixels, never logical ones.** Translation cancels, a scale factor does not. On a
  fractionally-scaled output a logical-pixel displacement is multiplied by the scale and
  snaps to the wrong lattice point — plausibly, and silently.

`XR_SUCCESS` means the drag did not disturb the phase, **not** that the phase is good: a
snap preserves whatever phase the window had at `origin`. A correct result also never moves
the window more than ~2 px in canonical space.

A display processor that does not implement the slot — `sim_display`, and every plug-in built
before #1588 — reads as identity: the snapped rect equals the target, the call succeeds, and
the app places its window where it asked to. That is what makes the extension safe to
advertise before any vendor has shipped the slot.

**The runtime feeds the window's TRUE origin and never quantises it.** Only the window owner
may move a window, so only the window owner may snap one — through this call, before the
move. Everything downstream reports where the window actually is.

That rule is empirical. A Vulkan build briefly re-snapped the origin it fed
`set_present_origin` (`vk_update_present_origin`), reasoning that it could only ever correct
a metrics read that caught an already-snapped window mid-flight. On the DS1 with a real Leia
DP, a programmatic 16-step drag disagreed: the fed origin diverged from the true window
origin on **12 of 13 logged moves, by up to 2 px** — e.g. the window genuinely at
panel-relative (726, 314) while the weaver was told (726, 316). Two failures, both
structural: the compositor anchored each snap on its own previous output rather than on the
drag origin the owner used, so it ran a second snap chain that drifted; and it could not
distinguish "our read caught the window mid-flight" from "the window is genuinely
off-lattice", which is precisely the premise such a guard needs. It has been removed. The
on-change log line now always prints the window's own origin:
`present origin: (x, y) = window … - panel …`.

For completeness, what the same hardware run says about the app-side snap, which is the
mechanism that works: 15 of 16 moves snapped, every snapped point inside the vendor's ±2 px
per-axis search radius (max excursion exactly 2), and all 15 sharing one slanted-lattice
phase (circular concentration 0.974 against a slanted fit; a plain x-only residue test fails,
as it must for a slanted lattice).

### Bulk: `xrWeaveSnapWindowGridDXR` (v11, #1723)

```c
#define XR_TYPE_WEAVE_SNAP_GRID_INFO_DXR   ((XrStructureType)1004999246)
#define XR_WEAVE_SNAP_GRID_MAX_POINTS_DXR  (1024u * 1024u)
#define XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR    1024u
#define XR_WEAVE_SNAP_GRID_NO_DELTA_DXR    (-128)

typedef struct XrWeaveSnapGridInfoDXR {
    XrStructureType          type;        // XR_TYPE_WEAVE_SNAP_GRID_INFO_DXR
    const void* XR_MAY_ALIAS next;
    XrRect2Di                originRect;  // drag-start window rect (offset used)
    XrOffset2Di              firstTarget; // proposed top-left of grid point (0, 0)
    XrExtent2Di              step;        // grid pitch, device px, each >= 1
    uint32_t                 countX;
    uint32_t                 countY;
} XrWeaveSnapGridInfoDXR;

typedef struct XrWeaveSnapGridPointDXR { int8_t dx; int8_t dy; } XrWeaveSnapGridPointDXR;

XrResult xrWeaveSnapWindowGridDXR(XrSession session, const XrWeaveSnapGridInfoDXR* gridInfo,
                                  uint32_t pointCapacityInput, uint32_t* pointCountOutput,
                                  XrWeaveSnapGridPointDXR* points, XrBool32* declined);
```

**Why it exists: round trips.** The Wayland drag lattice
([wayland-window-geometry.md §8](../runtime/wayland-window-geometry.md#8-phase-snapped-drag--the-drag-lattice-extension-version-6))
is built at every title-bar press by asking the snap about a 129 × 129 grid of displacements:
16,641 calls, plus a small search around any answer the compositor cannot place. In-process
that costs 1.6–6 ms. For an IPC client every call is one service round trip, and a press
measured **2,297–2,503 ms** on the Linux dev box — the client's frame loop stalls and the
`xdg_toplevel.move` that follows goes out with a stale serial. The same probe through this
call is one round trip: the runtime runs the loop next to the display processor.

**Semantics.** Point (i, j) proposes `firstTarget + (i · step.width, j · step.height)` and is
snapped from `originRect` exactly as `xrWeaveSnapWindowRectDXR` would snap it — same frame
rules (any frame, as long as origin and targets share it; device pixels), same answer. The
result is row-major (index `j · countX + i`) and each entry is the snapped top-left **minus**
the proposed one. Standard two-call idiom (`pointCapacityInput` 0 returns `countX · countY`
and evaluates nothing). Everything is validated before anything is evaluated: each count
1..`XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR`, their product ≤ `XR_WEAVE_SNAP_GRID_MAX_POINTS_DXR`,
steps ≥ 1, every target within ±2²⁸ px — `XR_ERROR_VALIDATION_FAILURE` otherwise.

**Encoding: two int8 per point, and why not less.** The obvious smaller encodings do not
carry what the one consumer uses. The lattice probe (displayxr-common
`dxr_wl_lattice::probe`) reads, for each grid point, the DP's snapped position itself: it
maps it back to a logical displacement, keeps it if the compositor can place it, and
otherwise searches ±2 logical px around **that** position for one the DP leaves unchanged.
The table entry it keeps is a position, and the search is centred on a position; a
fixed/reachable bit per point would force the caller to re-ask the DP where to search.
int8 is generous rather than tight: a correct snap stays within the vendor's ~2 px search
radius (§5c above), so a delta outside ±127 means a broken snap, and it is reported as
`XR_WEAVE_SNAP_GRID_NO_DELTA_DXR` in both fields instead of being clamped into a wrong but
plausible position (the caller asks that one point per point). A 129 × 129 grid is 33 KB.

**Declined.** `*declined` is `XR_TRUE` when the display processor produced no snap — no snap
support, no usable viewing distance yet (a vendor SDK's "declined"), the service's DP
not up before its first submit. The loop stops at the first such answer and every point is
(0, 0): a caller that ignores the flag degrades exactly like the per-point call, which hands
the target back on a decline. A caller that honours it drags unconstrained, because there
is no phase to protect.

**Where it runs.** Wherever the per-point call does, through the same per-point function, so
the two can never disagree: in-process on desktop Linux (`comp_vk_native_compositor_snap_window_rect`),
the `comp_multi` weave engine behind IPC (`comp_multi_weave_snap_window_rect` → the engine
DP's `snap_window_rect` slot) on desktop Linux, macOS and Android, and the D3D11 service on
Windows. `XR_ERROR_FEATURE_UNSUPPORTED` exactly where the per-point call reports it. **No new
display-processor slot:** the runtime loops the existing per-point slot (one shared loop,
`u_snap_grid.c`), so no vendor change is needed and the lens lattice is still never
published — the result is the same derived answer set the app already sends to the
compositor extension.

**Wire.** One varlen IPC call, `weave_snap_window_grid`: the fixed message carries the grid
(eight integers — well inside the 1024-byte message budget), the reply header carries
`{result, declined, point_count}`, and the points follow as one `2 · point_count`-byte
payload on the same connection. No chunking and no shared-memory path is needed: the
transport (a `SOCK_STREAM` socket; a blocking message-mode pipe on Windows) carries a
variable-length reply the way `device_get_visibility_mask` and `space_locate_spaces`
already do. A rejected grid is reported in `result` over a healthy pipe (never as
`XRT_ERROR_IPC_FAILURE`, which would mark the session lost).

**Cost.** Server-side the loop costs what the in-process probe always did (per-point DP
calls; the service takes the weave engine's mutex per point, so a submit is never blocked
for the whole grid). Headless against a worktree service with `sim_display`
(`weave_probe_vk_linux`), 129 × 129: **1.4–2.7 ms** in one call, against 236–332 ms for
the same points one call each on an idle box (the 2.4 s figure above was measured with a
live weave on the service). The maximum 1024 × 1024 grid, a 2 MB reply, took ~140 ms. The
vendor per-point cost is a single SDK query, and 16,641 of them (plus the search) are what
the in-process 1.6–6 ms figure measures — so a server-side loop costs the service a few
milliseconds per press, not seconds.

**Consumers.** displayxr-common v2.24.0 gives its Linux window helper a bulk seam next to
the per-point `SnapWindowOriginFn`: `set_snap_grid_provider(SnapWindowGridFn, ud)`, backed by
`DxrWeaveSnap::grid_callback` (compiled only against spec ≥ 11 headers). The helper owns the
grid's shape — `dxr_wl_lattice::plan_grids` names exactly the grids its probe will ask — and
builds the table with `probe_via_grid` on its worker, so the table is the per-point table by
construction: one call at an integer output scale, one per residue class at a fractional
one, anything uncovered per point. The Linux test apps install both providers (the grid one
only when `grid_available()`); `weave_present_vk_linux --lattice-selftest` builds one table
per scale through that path against a live service and checks it equals the per-point
table. Headless against a worktree service (`sim_display`, `SIM_DISPLAY_INTERLACE_PERIOD=8`):
100 % is 1 call / 5 ms, 200 % 1 call / 26 ms, 150 % 16 calls / 112 ms, against 16,641,
16,641 and 159,354 per-point calls (71 ms, 75 ms, 1.85 s).

## 5d. Desktop-Linux dma-buf transport and sync_file fences (v10, #1699)

A file descriptor is an `int`, not a pointer, and a dma-buf carries no dimensions, format or
tiling of its own. v10 therefore does not squeeze it into `inputTexture` (whose `void*` stays for
ABI); it adds typed chains. Everything in this section is **desktop Linux only** as far as input
goes; the two output chains are portable (a platform with nothing to put in them writes `-1`).

```c
#define XR_WEAVE_DMABUF_MAX_PLANES_DXR 4
#define XR_WEAVE_HANDLE_KIND_DMABUF_DXR    5   // XrWeaveHandleKindDXR
#define XR_WEAVE_HANDLE_KIND_OPAQUE_FD_DXR 6

typedef struct XrWeaveDmabufDescDXR {          // XR_TYPE_WEAVE_DMABUF_DESC_DXR (1004999241)
    XrStructureType type; const void* next;    // chain on XrWeaveSubmitInfoDXR
    int32_t  fd;                               // runtime-owned on XR_SUCCESS
    uint32_t width, height;
    uint32_t drmFourcc;                        // DRM_FORMAT_*
    uint64_t drmModifier;                      // verbatim; never DRM_FORMAT_MOD_INVALID
    uint32_t planeCount;                       // 1..4, all planes are offsets into fd
    uint32_t offsets[4], strides[4];
    uint64_t bufferId;                         // stable buffer identity for the import cache; 0 = none
} XrWeaveDmabufDescDXR;
// XrWeaveOverlayDmabufDescDXR (1004999242): identical fields, describes the overlay atlas.

typedef struct XrWeaveOutputDmabufDXR {        // XR_TYPE_WEAVE_OUTPUT_DMABUF_DXR (1004999243)
    XrStructureType type; void* next;          // chain on XrWeaveOutputDXR
    int32_t  fd;                               // caller-owned; -1 on steady-state frames
    uint32_t width, height, drmFourcc;
    uint64_t drmModifier;
    uint32_t planeCount, offsets[4], strides[4];
    uint64_t size;                             // allocation size in bytes
} XrWeaveOutputDmabufDXR;

typedef struct XrWeaveSubmitSyncDXR {          // XR_TYPE_WEAVE_SUBMIT_SYNC_DXR (1004999244)
    XrStructureType type; const void* next;    // chain on XrWeaveSubmitInfoDXR, with a dma-buf input
    int32_t acquireFenceFd;                    // sync_file, -1 = none; runtime-owned on XR_SUCCESS
} XrWeaveSubmitSyncDXR;

typedef struct XrWeaveOutputSyncDXR {          // XR_TYPE_WEAVE_OUTPUT_SYNC_DXR (1004999245)
    XrStructureType type; void* next;          // chain on XrWeaveOutputDXR
    int32_t releaseFenceFd;                    // EVERY frame; caller-owned; -1 = already complete
} XrWeaveOutputSyncDXR;
```

**Two structure types for one layout.** The input and the overlay are both described by the
same fields, but they travel in one `next` chain, and a structure type appears in a chain once.
`XrWeaveOverlayDmabufDescDXR` exists only for that; it requires the `XrWeaveSubmitOverlaysDXR` it
describes (whose `overlayTexture` is then ignored).

**Handle kinds.**

| Kind | What `inputTexture` is | Where it works |
|---|---|---|
| `PLATFORM_DEFAULT` | on desktop Linux: `OPAQUE_FD`, unless an `XrWeaveDmabufDescDXR` is chained, which selects `DMABUF` | everywhere |
| `DMABUF` (5) | ignored — the fd and its layout are in `XrWeaveDmabufDescDXR` | desktop Linux; any producer (GL/EGL, Vulkan, GBM), any driver the service's GPU can import from |
| `OPAQUE_FD` (6) | `(void*)(intptr_t)fd`, a Vulkan `OPAQUE_FD` memory export | desktop Linux; **same driver and device** as the service only, since it names no format or tiling. The pre-v10 Linux behaviour, kept for a Vulkan producer on the service's own GPU |

A dma-buf input takes only a dma-buf overlay; `DMABUF` without a descriptor, a descriptor with
any kind but `DMABUF`/`PLATFORM_DEFAULT`, or any v10 input chain on another platform is
`XR_ERROR_VALIDATION_FAILURE`. `drmModifier == DRM_FORMAT_MOD_INVALID` (implicit) is rejected: a
producer resolves it first (`LINEAR` = 0).

**Lifetimes.** The woven dma-buf follows `weavedTexture`: delivered on the first successful
submit and on every reallocation, `fd = -1` in between. The release fence does **not**: it is a
fresh `sync_file` **every frame**, because a sync_file signals once. So a caller imports the
output buffer once per allocation but receives, waits and closes one release fd per submit.
`fenceValue` stays the monotonic per-submit counter it is on macOS and Android; `fence` is never
set on Linux.

**Fences.**

- *Acquire* (in). A sync_file that signals when the caller's GPU has finished **writing the
  input (and overlay) and reading the previous woven output** — one fence covers both hazards.
  The runtime waits it on the GPU before touching either, so the caller need not finish its GPU
  work before submitting. Absent / `-1` = the caller already finished (the v9 contract).
- *Release* (out). A sync_file that signals when this submit's woven output is complete; the
  caller waits it (GPU import or `poll`) before sampling the output. `-1` = the submit completed
  synchronously. **Without `XrWeaveOutputSyncDXR` the runtime keeps the v9 contract for the
  caller**: it waits the release fence itself (bounded 1 s) before `xrWeaveSubmitDXR` returns.

**FD ownership — the Vulkan external-handle rule.** Every fd the caller passes **in** (`fd`,
`acquireFenceFd`) is the runtime's once `xrWeaveSubmitDXR` returns `XR_SUCCESS`, and stays the
caller's on any other result — including `XR_ERROR_RUNTIME_FAILURE` (a refused frame) and
`XR_ERROR_INSTANCE_LOST`. A caller that wants to keep a buffer passes a `dup()`. Every fd handed
**out** (`XrWeaveOutputDmabufDXR::fd`, `releaseFenceFd`) is the caller's to close.

**An overlay that cannot be imported refuses the submit (v16).** When an overlay is chained
(`XrWeaveOverlayDmabufDescDXR`, or a stage-A `OPAQUE_FD` overlay) and the service cannot import
it, the submit fails with `XR_ERROR_RUNTIME_FAILURE` **before the woven output is touched**: the
previous accepted submit's output is left exactly as it was, and the fds stay the caller's (the
rule above). This is the D3D11 service's rule (an overlay it cannot open or acquire refuses the
submit before the output is cleared), and it is what lets a caller read "accepted with the overlay
chained" as "the overlay was composited". A caller that hands the whole of its 2D over as the
overlay (a browser's whole-page overlay) relies on it: it can then HOLD its last composed frame on
a refused submit instead of presenting a frame whose 2D is missing. Desktop-Linux runtimes before
v16 dropped the unreadable overlay and wove anyway, so such a caller gates on
`extensionVersion >= 16`.

**The import cache.** An fd is not an identity: every hand-off is a new number. The service
keys its input import cache on `bufferId` when non-zero and on the fd's `(st_dev, st_ino)`
otherwise, so a producer rotating a small pool re-uses imports instead of re-importing each
frame. Two different buffers must never share a non-zero `bufferId` within a session.

| Contract point | macOS / Android (comp_multi Vulkan) | Desktop Linux (comp_multi Vulkan, v10) |
|---|---|---|
| Window binding | opaque id / explicit geometry | explicit geometry (`xrWeaveBindWindow2DXR` + `XrWeaveWindowGeometryDXR`), device pixels, desktop-absolute; required under Wayland, where the compositor never tells the client its position |
| `inputTexture` | IOSurfaceRef / `AHardwareBuffer *` | ignored under `DMABUF` (the fd is in `XrWeaveDmabufDescDXR`); `(void*)(intptr_t)fd` under `OPAQUE_FD` |
| Handle kind | `IOSURFACE` / `AHARDWAREBUFFER` | `DMABUF` or `OPAQUE_FD` |
| Input-ready sync | caller finishes writes before submit | acquire `sync_file` (GPU wait), or caller finishes writes before submit |
| `weavedTexture` | runtime-allocated IOSurfaceRef / `AHardwareBuffer *` | NULL when `XrWeaveOutputDmabufDXR` is chained (the dma-buf is there); an `OPAQUE_FD` as `(void*)(intptr_t)fd` otherwise |
| `fence` / `fenceValue` | no fence, synchronous; `fenceValue` monotonic | `fence` never set; per-frame release `sync_file` in `XrWeaveOutputSyncDXR`, or synchronous without it; `fenceValue` monotonic |
| `xrWeaveSnapWindowRectDXR` | identity | the Vulkan DP's `snap_window_rect` (§5c) |
| Without the service's engine | n/a | submit: `XR_ERROR_FEATURE_UNSUPPORTED` |

**Wire (runtime-internal, for reviewers).** Two IPC calls, not a grown `weave_submit`, so the
shipping Windows/macOS/Android wire is byte-identical: `weave_submit_dmabuf` (the unchanged
`ipc_arg_weave_submit` + a 136-byte `ipc_arg_weave_dmabuf`, 968 of the 1024-byte message; in
handles `[input, overlay?, acquire?]`, one out handle — the release fence) and
`weave_get_output_dmabuf` (the output's layout beside its fd). The service parks the fds it sends
out and closes them at the next dma-buf weave call or at client teardown, since the transport
duplicates rather than transfers. Engine contract: `src/xrt/include/xrt/xrt_weave_dmabuf.h`.

**Off-panel 2D (#1654).** The part of the bound window that the published geometry puts off the
panel comes back flat, not woven: the runtime paints each off-panel band of the output with the
flat view of the submitted content (§5f's table: for a stereo pair the left view — a batch rect's
left half unsqueezed; for v6 with more views, the view nearest the display axis), in register with the window and under the v4 overlay, and skips the weave entirely
for a window wholly off the panel — so a caller composites the output as usual and never clips it
itself. `DXR_SPAN_2D=0` restores the fully woven output.

**Flat regions (v8) on desktop Linux.** With no per-region lens on any desktop-Linux display
processor to wish to, the runtime instead paints the union of the submit's
`XrWeaveSubmitFlatRegionsDXR` rects and the latched `xrWeaveSetScreenFlatRegionsDXR` rects
(clipped to the bound window, taking effect at the next submit) flat in the woven output — the
input pixels 1:1 for a batch submit, the flat view (§5f) for v6 — a platform exception to §2c's
"pixels unaffected" that `DXR_WEAVE_FLAT_2D=0` switches off.

## 5e. Woven origin per output — present-owner move sync (v12, browser-pvt#180)

```c
#define XR_TYPE_WEAVE_OUTPUT_ORIGIN_DXR         ((XrStructureType)1004999247)
#define XR_TYPE_WEAVE_WINDOW_LOGICAL_ORIGIN_DXR ((XrStructureType)1004999248)

typedef struct XrWeaveWindowLogicalOriginDXR {   // IN: chain on XrWeaveBindWindowInfoDXR
    XrStructureType          type;
    const void* XR_MAY_ALIAS next;
    XrOffset2Di              logicalOrigin;      // surface top-left, compositor-global LOGICAL px
    float                    scale;              // output scale used for logical -> device px
} XrWeaveWindowLogicalOriginDXR;

typedef struct XrWeaveOutputOriginDXR {          // OUT: chain on XrWeaveOutputDXR, every frame
    XrStructureType    type;
    void* XR_MAY_ALIAS next;
    XrBool32           valid;                    // a geometry was bound for this output's weave
    XrOffset2Di        wovenOriginOnScreen;      // absolute physical px, as XrWeaveWindowGeometryDXR
    XrBool32           logicalValid;             // that bind also carried a logical origin
    XrOffset2Di        wovenOriginLogical;       // verbatim
    float              logicalScale;             // verbatim
    uint64_t           serial;                   // monotonic per woven output; 0 = not reported
} XrWeaveOutputOriginDXR;
```

**Why.** On native Wayland a drag is *move-synchronised* (GNOME extension version 9,
`docs/specs/runtime/wayland-window-geometry.md` §9): the extension holds the window's actor at
the position the buffer it is about to paint was woven for. It learns that position from a tag
on the commit — a 1×1 synchronised subsurface at `(x mod 256, y mod 256)` of the **logical**
content origin. A window-bound runtime session tags its own commits. A present-owner commits the
woven pixels itself, so it has to tag them itself, and until v12 nothing told it which origin a
given output was woven for: v7 geometry only flows *in*, and a re-bind (every move) and the weave
that consumes it are not the same moment.

**Which origin.** The one the **weave** used: the geometry the runtime fed the display
processor's phase slot (`set_present_origin`) for that weave, snapshotted inside the weave under
the engine lock that also serialises every bind, and committed only when the weave goes out —
the service-side counterpart of the in-process compositor's per-weave
`comp_vk_native_wl_geom_last_rect` capture. Today the weave runs synchronously inside
`xrWeaveSubmitDXR`, so this equals the last `xrWeaveBindWindow2DXR` that returned before the
submit; the contract is written against weave time so an asynchronous weave keeps it.

**Units.** `wovenOriginOnScreen` is the bound `windowOriginOnScreen`, echoed (absolute physical
px, y-down). The logical origin is **not derived** by the runtime: the service knows no output
scale (a Wayland client is never told its position — the present-owner learns it, in logical
px, from the extension, and converts it to device px itself), and converting device px back to
logical would be a fresh rounding, which is exactly how a 150 % output lands one pixel off
(#1609). So the caller states its logical origin and the scale it converted with on the same
bind (`XrWeaveWindowLogicalOriginDXR`, requires the geometry chain, scale finite and in
(0, 64]); the runtime latches the pair with that bind's device geometry and echoes both per
output. **No conversion is applied in either direction.** A bind without the logical chain
clears the latch, so `logicalValid` is false until the next bind that carries one.

**The tag stays out of the API.** The presenter takes `wovenOriginLogical` mod 256 itself
(non-negative modulo, so a window above/left of the stage origin still encodes) and tags the
commit that presents that output — only when `logicalValid`. The modulus must equal the
runtime's `COMP_VK_NATIVE_WL_MOVE_SYNC_TAG_MOD` and the extension's `MOVE_SYNC_TAG_MOD` (256).

**Serial.** Increases with every woven output of the session (first output ≥ 1); two equal
serials are the same output. With the single reused desktop-Linux output allocation, "an output"
is one submit's weave into it.

**Where it is filled.** The desktop-Linux dma-buf submit path (`XrWeaveDmabufDescDXR`). Every
other path of a v12 runtime writes `valid = logicalValid = XR_FALSE`, `serial = 0`; the IN chain
is accepted everywhere and dropped outside desktop Linux. A pre-v12 runtime never touches the
OUT struct, so a caller zero-initialises it (or gates on `extensionVersion >= 12`).

**Wire (runtime-internal).** Both additions are **appended**, leaving every pre-v12 field at its
offset: `weave_set_window_geometry` gains `logical_valid, logical_x, logical_y, logical_scale`
after `display_id`, and `weave_submit_dmabuf`'s reply gains a trailing 40-byte
`ipc_weave_woven_origin` after `eyes` (per frame — not on `weave_get_output_dmabuf`, which a
client calls only when the output is (re)allocated). Each side converts field by field between
the wire POD and `xrt_weave_woven_origin`; nothing is byte-copied across layouts. The wire has no
per-call version negotiation: messages are fixed-size on a `SOCK_STREAM` socket, and a client
library and service built from different trees are refused at connect by the `u_git_tag` gate
(`XRT_ERROR_IPC_VERSION_SKEW`), so a v11 client never meets a v12 service or vice versa.
`tests/tests_ipc_proto.py` pins the append-only order.

**Verified** headless (`weave_present_vk_linux --headless N`, sim_display through the dev
service): the app re-binds a stepped origin every 4 frames with a logical origin attached, and
checks on every output that the reported origin equals the last bind, the logical pair is echoed
verbatim and the serial strictly increases.

## 5f. One flat view while the panel is in hardware 2D (v13)

```c
#define XR_TYPE_WEAVE_SUBMIT_MONO_IN_2D_DXR ((XrStructureType)1004999249)

typedef struct XrWeaveSubmitMonoIn2DDXR {
    XrStructureType          type;     // XR_TYPE_WEAVE_SUBMIT_MONO_IN_2D_DXR
    const void* XR_MAY_ALIAS next;
    XrBool32                 monoIn2D; // XR_TRUE: one view flat, no weave, while hardware is 2D
} XrWeaveSubmitMonoIn2DDXR;
```

**Why.** `xrRequestDisplayModeDXR` is hardware-only (`XR_DXR_display_info`): 2D over woven
content turns the lens off and keeps the weave, so every view shows interlaced through no lens,
soft and doubled. Apps build fade-to-flat transitions on exactly that, so it stays the default.
A present-owner that drops the lens for its OWN reason — the browser holding the panel 2D while
one of its menus is open over an inline-3D page — wants the content flat and sharp instead, as if
a 2D rendering mode were active, without the page being told anything.

**Why not a 2D rendering mode.** `xrRequestDisplayRenderingModeDXR` is the page's channel and is
display-global: on Windows the service broadcasts `XrEventDataRenderingModeChangedDXR` to every
session and re-forces a 3D mode for a weave client that keeps submitting; on desktop Linux the
content mode a present-owner pushes is recorded and read by nothing in the weave engine, and its
seed at `xrBeginSession` is whatever mode the head device holds. Neither weave engine reads the
content mode at all. A per-submit flag scoped to the weave keeps the page's mode, the events and
every other session out of it.

**Semantics.** Chained on `XrWeaveSubmitInfoDXR`. While `monoIn2D` is `XR_TRUE` **and** this
session's hardware display state is 2D — the state the runtime last confirmed to it, the one its
`XrEventDataHardwareDisplayStateChangedDXR` events report — the runtime skips the weave and paints
the whole output with ONE view of the submitted content at full output resolution, in register with
the window. The path is the off-panel bands' (§5d): the whole output becomes one band, painted from
the same view the bands use, so the two flat paths always match. Flat regions and the v4 overlay are
applied on top as usual.

**Mono follows the runtime's display-processor-CONFIRMED hardware-2D state for this session, not
the request.** It starts on the first submit after the display processor has accepted the 2D
request, and it ends on the first submit after the confirmed state is 3D again — no stale flat frame
after a release, no woven frame while held. A 2D request the display processor rejects never
starts it.

**Which view.** One rule for every flat path of the engine (`u_weave_flat_view_index`):

| submitted views | flat view |
|---|---|
| 1 or 2 — every batch submit, and a 2-view v6 layout | **view 0, the LEFT view** (a batch rect's left half, unsqueezed). Deterministic: both eyes sit about equally far from the display axis, so a nearest-axis rule would flip between them as the head moves and the flat image would jump by the disparity. |
| more than 2 (v6) | the view whose eye is **nearest the display axis** — smallest \|x\| of the per-view eye offset the display processor reports; a tie goes to the lower index |
| more than 2, no per-view eye offsets | the centre index, `(N − 1) / 2` |

- In hardware 3D the struct has no effect: the output is woven. A caller can therefore chain it for
  the whole of its own 2D hold without racing the lens, and a 2D request the display processor
  rejected keeps the weave.
- Per submit, no latch. Omitting the struct, or `monoIn2D = XR_FALSE`, is the pre-v13 behaviour.
- Nothing is reported to the session: no event, no change to the active rendering mode, to
  `xrLocateViews`, or to the hardware state.

**Platforms.** The desktop-Linux weave engine implements it (`comp_multi_weave_linux.c`,
`weave_run_frame`; the IPC submit carries it as `ipc_arg_weave_submit::mono_in_2d`). Windows,
macOS and Android accept and ignore it — the output stays woven — as a pre-v13 runtime does with
any unknown chained struct. One `weave(v13)` WARN in the service log per edge.

## 6. Version history

| Version | Change |
|---|---|
| 1 | Initial: bindWindow + per-element submit + snap (pre-rename numbering carried over). |
| 2 | `inputIsDxgi` legacy-DXGI handle tagging (Low-integrity GPU-process callers, #743). |
| 3 | `XrWeaveSubmitRectsDXR` batched submit — N rects, one call, one fence (#744). |
| 4 | `XrWeaveSubmitOverlaysDXR` DP-composited premul-RGBA 2D overlay atlas (browser#18). |
| 5 | `XrWeaveSubmitInfoDXR::firstChunk` — coherent whole-window output (browser#22). |
| 6 | `XrWeaveSubmitLayoutDXR` N-view worst-case atlas layout (#774). |
| 7 | `XrWeaveSubmitHandlesDXR` handle kinds + `xrWeaveBindWindow2DXR` / `XrWeaveWindowGeometryDXR` explicit window geometry; **Android** support (#1036). |
| 8 | `XrWeaveSubmitFlatRegionsDXR` + `xrWeaveSetScreenFlatRegionsDXR` — per-region hardware wish on the weave path (browser#88). |
| 9 | `xrWeaveExportIpcConnectionDXR` + `XrWeaveIpcConnectionDXR` — brokering a runtime IPC endpoint to a sandboxed sibling process (§4c); plus §4b, the error table making a dead connection report `XR_ERROR_INSTANCE_LOST` / `XR_ERROR_SESSION_LOST` (browser#103). |
| 10 | Desktop-Linux dma-buf transport (§5d, #1699): `XR_WEAVE_HANDLE_KIND_DMABUF_DXR` / `_OPAQUE_FD_DXR`, `XrWeaveDmabufDescDXR` + `XrWeaveOverlayDmabufDescDXR` in, `XrWeaveOutputDmabufDXR` out, `sync_file` fences `XrWeaveSubmitSyncDXR` (acquire) / `XrWeaveOutputSyncDXR` (release, per frame). Desktop Linux becomes a full weave platform when the service carries its engine. |
| 11 | `xrWeaveSnapWindowGridDXR` + `XrWeaveSnapGridInfoDXR` / `XrWeaveSnapGridPointDXR` — bulk grid snap: the per-point snap evaluated over a grid by the runtime, one call (one IPC round trip) instead of one per point (§5c, #1723). |
| 12 | `XrWeaveOutputOriginDXR` (out, per frame: the origin each woven output was woven for + a serial) and `XrWeaveWindowLogicalOriginDXR` (in: the caller's logical origin + scale, echoed verbatim) — so a present-owner can tag its own Wayland commit for move-synchronised drag (§5e, browser-pvt#180). |
| 13 | `XrWeaveSubmitMonoIn2DDXR` — one flat view (stereo: the left view; N > 2: the view nearest the display axis) instead of the weave while the session's hardware state is 2D (§5f). Desktop Linux; accepted and ignored elsewhere. |
| 15 | `XrWeaveSubmitOverlayFilterDXR` — the caller's lens-filter strength for the 2D overlay, 0..1; omitted = the DP's own default (§2d). |
| 16 | No new structure: on desktop Linux a submit whose chained overlay the engine cannot import is **refused before the woven output is touched** (§5d), as the D3D11 service always did; before v16 the Linux engine dropped the overlay and wove anyway. A desktop-Linux caller that hands its whole 2D over as the overlay gates on `extensionVersion >= 16`. The macOS and Android engines still drop an unreadable overlay. |
| 14 | `XrWeaveSubmitOverlayUnchangedDXR` — the v4 overlay atlas holds the previous accepted submit's pixels, so a DP may reuse its lens prefilter of the 2D layer (ADR-027 Amendment; §2d). |
| 17 | `XrWeaveSubmitPresentSurfaceDXR` + `XrWeavePresentStatusDXR` — the runtime presents the woven frame itself, through a swap chain on a DirectComposition surface handle the caller owns and shows as a visual; the handle is pulled from the caller; frame-latency token waited at most 4 ms; declined = the old woven texture (§2e). Windows D3D11 service; elsewhere never presented. |

**v17 is a bump**: two structure types a caller must test for (gate on
`extensionVersion >= 17`). An older runtime skips both; a caller that zero-initialised the status
reads `presented = XR_FALSE`, keeps its visual hidden and copies the woven texture back, which is
the declined path anyway.

**v16 is a gate for one kind of caller**: one whose overlay IS its 2D (a whole-page overlay)
must not send it to an older desktop-Linux runtime, which would weave without it and return
success. A caller whose overlay is decoration over content it presents anyway needs no gate.

**v15 needs no gate either**: omitting the struct, or a runtime/DP that predates it, means
the display processor's default strength.

**v14 needs no gate**: a pre-v14 runtime skips the unknown struct, which is the "may have
changed" behaviour, so a caller can always send it.

**v13 is a bump**: one structure type a caller gates on `extensionVersion >= 13`. Against an
older runtime the struct is skipped and the output stays woven, which is also what every
non-Linux v13 runtime does — so the gate decides only whether the caller can expect the flat view.

**v12 is a bump**: two structure types a caller must be able to test for. An older runtime skips
both chains — the logical origin is simply not latched, and the OUT struct keeps whatever the
caller initialised it to, which is why a caller zero-initialises it.

**v11 is a bump** for the same reason v10 is: a new entry point and structure type a caller
must be able to test for. A caller gates `xrWeaveSnapWindowGridDXR` on `extensionVersion >= 11`
(or simply on `xrGetInstanceProcAddr` resolving it) and falls back to the per-point call.

**v10 IS a bump**, unlike #1588 below: it adds enums, structure types and fields a caller must be
able to test for. A caller gates the dma-buf chains on `extensionVersion >= 10`: an older runtime
skips unknown chained structs, so it would read the (NULL) `inputTexture` as the input instead —
the same silent-misread shape §2b warns about for v6.

**Desktop Linux availability (#1588) is deliberately NOT a version bump.** No entry point,
struct, enum or parameter changed — a platform that reported `XR_ERROR_EXTENSION_NOT_PRESENT`
now advertises the extension and implements one of its six calls. An app discovers that the
only way it ever could, through `xrEnumerateInstanceExtensionProperties` plus the per-call
`XR_ERROR_FEATURE_UNSUPPORTED` that §5c tabulates; a `SPEC_VERSION` bump would tell a
Windows or Android app that something about the API had changed, which would be false.

§4b arrived first, and on its own would not have earned a bump — the entry points simply
started reporting a dead connection with the same `XR_ERROR_INSTANCE_LOST` every other
IPC-backed OpenXR call already used, and §4b wrote down a contract that was previously
implicit. §4c's entry point + struct settle it: **v9**.

## 7. Consumers

| Consumer | Path | Layout used |
|---|---|---|
| DisplayXR Browser (Chromium fork) | GPU-process sync weave | Batch (v3) when the runtime reports spec ≥ 3; per-element legacy loop otherwise. Whole-page overlay frames are presented by the runtime on a composition surface the browser shows above its root, when the runtime reports spec ≥ 17 (§2e) |
| CEF weave host (Step A) | Browser-process sync | Legacy |
| DisplayXR Browser on Android | Chromium GPU process → satellite compositor (ADR-036 D3) | Batch (v3/v7) — AHardwareBuffer handles + published window geometry |
| DisplayXR Browser on desktop Linux (planned, #1699) | Chromium GPU process (GL/EGL) → comp_multi service | v10 dma-buf + `sync_file` fences; gate on spec ≥ 10. Move-synchronised Wayland drag: v12 woven origin (§5e), gate on spec ≥ 12 |

When changing the header, byte-sync every consumer's vendored copy and rebuild it
(`third_party/displayxr` in the fork) — coupled-PR order: runtime → extensions auto-sync →
consumers.
