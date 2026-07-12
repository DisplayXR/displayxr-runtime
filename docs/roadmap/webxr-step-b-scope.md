# Step B — Minimal, rebasable Chromium patch: scope

Step B replaces the CEF offscreen-render **stand-in** (Step A,
[`displayxr-cef-host`](https://github.com/DisplayXR/displayxr-cef-host)) with a
real, minimal, **vendor-free** patch *inside Chromium*, so an ordinary web page
can make a canvas element glasses-free 3D by driving the **same shipped weave RPC**
(`XR_DXR_weave`). Background + sequencing: [`webxr-support.md`](webxr-support.md)
§2.4 Step B / §2.6. Proposed JS surface: [`webxr-displayxr-explainer.md`](webxr-displayxr-explainer.md).

This is a **multi-week, Chromium-expertise + build-infra** effort. This doc scopes
it into gated phases so a new session starts on the right first milestone (stand up
the build + a research spike) rather than patching blind.

## The red lines (non-negotiable)

1. **Zero vendor code, zero weave shader in Chromium.** The patch only does
   **handle export/import + the weave RPC call + the JS surface**. All weaving,
   eye tracking, and per-display calibration stay behind the OS runtime / vendor
   plug-in (ADR-007 / ADR-019). If a change would put a weave shader or vendor SDK
   in Chromium, it is wrong by construction.
2. **Reuse the shipped `XR_DXR_weave` RPC unchanged where possible.**
   `xrWeaveBindWindowDXR` + `xrWeaveSubmitDXR` + `xrWeaveSnapWindowRectDXR` are
   shipped (runtime v1.24.0). The patch is a *client* of that contract — the same
   one the CEF host already proves end-to-end. Extend the RPC only if a genuine gap
   appears (flag it, don't assume it).
3. **Minimal + isolated + flagged → rebasable.** Chrome rebases on a ~4-week
   cadence. The diff must be small, behind a runtime feature flag (off by default),
   and touch as few Chromium files as possible. It is a *reference implementation /
   patched distribution*, **not** a maintained product fork (see `webxr-support.md`
   §2.5).
4. **Windows-first.** Prove on Leia (Windows, D3D11 / DXGI shared handles). macOS
   (`IOSurface`) is a parallel follow-up, not part of the first milestone.

## What Step B reuses (already shipped + hardware-validated)

- **The weave RPC** (`XR_DXR_weave`): `bindWindow(hwnd)` once, then per element per
  frame `weave(sharedHandle, rect) → {wovenHandle, fence, eyes}`; window-drag phase
  lock via `xrWeaveSnapWindowRectDXR`. The DP weaves; the caller never does.
- **The CEF host as the executable reference** (`displayxr-cef-host`): the exact
  client flow Step B re-creates inside Chromium — extract a canvas sub-rect's SBS
  pair, submit, GPU-wait the fence, composite the woven sub-rect at the committed
  rect, feed the returned eyes back for off-axis rendering. **Multi-element** (N
  regions from the DOM) is already proven there; Step B inherits that model.
- **The forced-IPC / service path**: the weave service is service-mode only; the
  Chromium GPU process becomes the IPC weave client (same as the host's
  `XRT_FORCE_MODE=ipc`).

## The core architectural shift

| | Step A (CEF host) | Step B (Chromium patch) |
|---|---|---|
| Who owns present | the host (outside Chromium) | Chromium's compositor (**Viz**) |
| Page content access | `OnAcceleratedPaint` → one composited page texture | a canvas's own `SharedImage` (GPU resource) |
| Where the weave happens | host process | Chromium **GPU process** (where the SharedImage + present live) |
| Composite of woven result | host blits sub-rect over page base | Viz draws the woven quad in place of the canvas quad at its committed rect |
| JS surface | `cefQuery` strings (`rects …` / `subscribe-eyes`) | a real Blink binding (the explainer's `inline-3d` / `XRDisplayLayer`, or a thin `bindWindow`/`weave`) |

The hard part is no longer "drive the RPC" (proven) — it's **plumbing the canvas's
GPU resource out as a shared handle and injecting the woven result back into Viz's
compositing**, inside Chromium's GPU/compositor architecture, with a small diff.

## Phases (each gated; do not start the next until the prior's exit criteria pass)

### B0 — Environment + orientation (the gate)
Stand up Chromium: `depot_tools`, `gclient sync` at a pinned stable tag, build
**`content_shell`** (the minimal embedder — builds far faster than `chrome`), run it
on the Leia box. **Exit:** `content_shell` builds + runs a normal page on the Leia
machine; you can rebuild incrementally. *(Cost: large disk + multi-hour first build;
this phase is mostly infra.)*

### B1 — Integration-point spike (research; NO patch yet)
Pin the exact Chromium seams against the pinned source and write a design doc
mapping each to `file:line`:
- **Canvas GPU resource → cross-process shared handle.** Where a 2D-canvas /
  WebGL-canvas backing lands as a `SharedImage` / `Mailbox`, and how to obtain a
  **DXGI/NT shared handle** for it across the GPU-process boundary (this is the
  crux — Chromium's SharedImage is mailbox-based; a raw shared handle may need a new
  internal path).
- **Viz composite injection.** Where Viz composites a layer's quad
  (`CompositorFrame` / `RenderPass` / `*DrawQuad` / `OverlayProcessor`), so the woven
  texture can replace the canvas quad at its **committed** device-pixel rect with
  correct DOM z-order.
- **The DXR weave client in Chromium.** Which process links the OpenXR loader /
  `DisplayXRClient` and calls the RPC (almost certainly the **GPU process**), and how
  to link it without bloating or destabilizing Chromium.
- **The window HWND + committed rect.** Where to get the top-level HWND (for
  `bindWindow` + phase-snap) and the canvas's committed device-pixel rect (Blink
  layout → compositor).
**Exit:** a written design doc ([`webxr-step-b-design.md`](webxr-step-b-design.md))
pinning every seam + the chosen process boundary + the handle-export strategy + the
smallest-diff plan. **This is the most important phase — it decides feasibility and
diff size.** ✅ **Delivered** — key finding: the GPU sandbox forces the weave RPC
client into an unsandboxed process (browser, then a utility process), bridged to the
GPU-process texture via DXGI shared handles; canvas-SI export is avoided by copying
into an owned keyed-mutex input (the CEF-host pattern).

### B2 — Minimal hardcoded weave (no JS)
Prove the pipe with zero JS API: hardcode "weave the first canvas on the page" —
export its SharedImage as a shared handle, call `xrWeaveSubmitDXR`, GPU-wait, and
composite the woven quad in Viz at a hardcoded (or layout-derived) rect, in
`content_shell` on Leia. **Exit:** a known SBS canvas shows real glasses-free 3D
through `content_shell`, weaved by the DP. **Riskiest technical milestone** — it
validates handle export + RPC + Viz composite end-to-end.

### B3 — JS surface + DOM-driven multi-element
Add the minimal Blink binding (the explainer's `inline-3d` session + `XRDisplayLayer`,
or a thin `navigator.displayXR.bindWindow/weave`). Wire page-declared canvases +
their committed rects into the B2 pipe; support **N elements from the DOM** (inherit
the host's multi-element model). Off-axis projection driven by the RPC-returned eyes.
**Exit:** a real `.html` (no host) drives one+ inline-3D elements in `content_shell`.

### B4 — `chrome` + hardening + rebasability
Move from `content_shell` to `chrome`. Flag it (runtime feature flag, off by
default). Handle scroll / zoom / window-drag committed-rect tracking + phase-snap
(`xrWeaveSnapWindowRectDXR`), DPI / fractional-zoom device-pixel-exact alignment
(one pixel of drift collapses the lattice). Keep the diff minimal + isolated;
document the rebase story. **Exit:** flagged `chrome` build on Leia; a clean,
rebasable patch suitable as the standards reference implementation.

## Crux risks / unknowns (resolve in B1)

1. **SharedImage → raw shared handle** across the GPU-process boundary — the single
   biggest unknown; may require a new internal export path.
2. **Linking the DXR/OpenXR client into Chromium's GPU process** without
   destabilizing it (static-CRT, init order, the loader).
3. **Viz quad injection** at the committed rect with correct z-order, without a
   heavyweight new layer type (smallest diff that works).
4. **Committed-rect exactness** under async compositing + DPI/zoom (the Step-A
   "why it's hard" point #1/#2).
5. **Diff size vs. rebasability** — every seam touched is rebase cost.

## Build / infra prerequisites

- Chromium checkout (`depot_tools` + `gclient`), ~100+ GB, multi-hour first build;
  a machine that can also reach the Leia display for on-hardware testing.
- The shipped DXR runtime (v1.24.0+) installed + the Leia plug-in (v1.11.1+)
  registered, service running (the same setup the CEF host uses).
- Pin a specific Chromium stable tag up front (rebasability starts at B0).

## Early decisions to make (in B1)

- **JS surface:** the explainer's WebXR `inline-3d` + `XRDisplayLayer` (standards-
  aligned, more binding work) vs. a thin `bindWindow`/`weave` (faster to prototype,
  less standards-shaped). Recommend prototyping with the thin surface in B2/B3 and
  converging to the WebXR shape for the reference patch in B4.
- **Process boundary:** confirm the GPU process is where the weave client lives.
- **Accumulation:** whether the DP preserves prior sub-rects across submits into the
  one window-sized output (would let N submits → 1 fence-wait); the host serializes
  as the safe default — confirm against the service and adopt if true.

## Definition of done (Step B)

A flagged, minimal, vendor-free Chromium patch — proven in `content_shell` then
`chrome` on Leia — where a plain web page makes one or more canvas elements
glasses-free 3D via the shipped weave RPC, with correct DOM z-order, scroll/zoom/
drag phase lock, and **no weave or vendor code in Chromium**. It serves as both the
patched-distribution we ship to users and the reference implementation behind the
WICG explainer.
