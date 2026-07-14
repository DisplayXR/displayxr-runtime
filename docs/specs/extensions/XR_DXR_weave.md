# XR_DXR_weave — Window-Bound Synchronous Weave Service

| Field | Value |
|---|---|
| **Extension Name** | `XR_DXR_weave` |
| **Spec Version** | 3 |
| **Extension Type** | Instance extension (Windows / D3D11 service path only) |
| **Header** | `src/external/openxr_includes/openxr/XR_DXR_weave.h` (canonical; auto-syncs to `displayxr-extensions`) |
| **Status** | Provisional (`1004999190–192` type block, pending Khronos registry) |
| **Design history** | `docs/roadmap/webxr-step-b-design.md` §13.6–13.9, issue #625 |

## 1. What it is

A weave *service* for **present-owners**: callers that own their OS window and present
themselves (a browser, the CEF host, the WebXR bridge), but want the runtime's display
processor to weave sub-rects of their window for them. The caller never weaves
(ADR-007/ADR-019): it hands the runtime pre-weave side-by-side stereo pixels + window-relative
rect(s) and composites back a weaved shared texture, gated on a fence. Eyes flow **out**
(runtime → caller) so the caller can render its next frame's off-axis (Kooima) projections;
the interlace itself reads the vendor's tracker DP-internally.

Three entry points:

- `xrWeaveBindWindowDXR(session, hwnd)` — bind the present-owner's window (phase reference).
- `xrWeaveSubmitDXR(session, submitInfo, output)` — synchronous weave; returns dims, fence
  value, tracked eyes; hands back the shared woven-texture/fence HANDLEs on the first call and
  on re-allocation (resize).
- `xrWeaveSnapWindowRectDXR(session, origin, target, snapped)` — drag-time phase snap
  (window-position constraint against the DP's interlace lattice).

Only the out-of-process (service/IPC) path implements it; in-process sessions report
`XR_ERROR_FEATURE_UNSUPPORTED`.

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

## 3. Why batch (the scaling wall)

Each submit carries a fixed cost independent of the rect area: the runtime IPC round-trip,
`OpenSharedResource` on the input, the keyed-mutex acquire/release, and the fence signal —
~1 ms wall clock measured on the synchronous GPU-process path. Per-element submits serialize
that N× on the caller's present thread, capping a page at ~8-12 visible woven elements. The
DP weave itself is bounded by window pixels (all sub-rects accumulate into the one
window-sized output), so ONE submit carrying N rects makes 50 visible tiles cost ≈ 1.

## 4. Service semantics (implementation notes)

- The output texture is sized to the bound window's client area and **never cleared**:
  each weave writes only its sub-rect(s), so all elements accumulate at their window
  positions. Stale regions from closed elements are harmless — the caller's draw-back
  composites only current rects.
- The DP's `process_atlas` samples its whole SRV as the atlas (no input-offset parameter),
  so the batch path copies each rect out of the window-sized input into an exact-size
  scratch tile before the per-rect weave. The input's keyed mutex is released right after
  those copies — the caller can begin writing the next frame while the DP weaves.
- The input keyed mutex uses key 0 = "caller done writing, runtime may read"; it is the
  input-ready guarantee (the service imports no caller fences).
- A **legacy DXGI** shared input handle (`inputIsDxgi = XR_TRUE`) crosses the runtime IPC
  low-bit-tagged with no `OpenProcess` — required for Low-integrity sandboxed callers
  (Chromium's GPU process; see #743). NT handles remain supported for Medium callers.

## 5. Version history

| Version | Change |
|---|---|
| 1 | Initial: bindWindow + per-element submit + snap (pre-rename numbering carried over). |
| 2 | `inputIsDxgi` legacy-DXGI handle tagging (Low-integrity GPU-process callers, #743). |
| 3 | `XrWeaveSubmitRectsDXR` batched submit — N rects, one call, one fence (#744). |

## 6. Consumers

| Consumer | Path | Layout used |
|---|---|---|
| DisplayXR Browser (Chromium fork) | GPU-process sync weave | Batch (v3) when the runtime reports spec ≥ 3; per-element legacy loop otherwise |
| CEF weave host (Step A) | Browser-process sync | Legacy |
| `displayxr-webxr-bridge` | Service client | Legacy |

When changing the header, byte-sync every consumer's vendored copy and rebuild it
(`third_party/displayxr` in the fork) — coupled-PR order: runtime → extensions auto-sync →
consumers.
