# XR_DXR_weave — Window-Bound Synchronous Weave Service

| Field | Value |
|---|---|
| **Extension Name** | `XR_DXR_weave` |
| **Spec Version** | 9 |
| **Extension Type** | Instance extension (service path only — Windows/D3D11, macOS/comp_multi-Vulkan #759, Android/comp_multi-Vulkan #1036) |
| **Header** | `src/external/openxr_includes/openxr/XR_DXR_weave.h` (canonical; auto-syncs to `displayxr-extensions`) |
| **Status** | Provisional (`1004999190–198` type block, pending Khronos registry; `199` reserved, see §2c; v9 additions in a fresh `1004999240–249` decade) |
| **Design history** | `docs/roadmap/webxr-step-b-design.md` §13.6–13.9, `docs/roadmap/android-concurrent-multi-app.md` F11/§10.4, issues #625, #774, #1031/#1036, browser#88, browser#103 |

## 1. What it is

A weave *service* for **present-owners**: callers that own their OS window and present
themselves (a browser, the CEF host, the WebXR bridge), but want the runtime's display
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

## 3. Why batch (the scaling wall)

Each submit carries a fixed cost independent of the rect area: the runtime IPC round-trip,
`OpenSharedResource` on the input, the keyed-mutex acquire/release, and the fence signal —
~1 ms wall clock measured on the synchronous GPU-process path. Per-element submits serialize
that N× on the caller's present thread, capping a page at ~8-12 visible woven elements. The
DP weave itself is bounded by window pixels (all sub-rects accumulate into the one
window-sized output), so ONE submit carrying N rects makes 50 visible tiles cost ≈ 1.

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
| `XR_ERROR_RUNTIME_FAILURE` | The service was reached over a healthy connection and **refused this call** — most commonly `xrWeaveSubmitDXR` losing the input keyed-mutex race (the service gives the input `AcquireSync` 4 ms, §4) while the caller is still writing. | **Yes — retry on the next frame.** This is the expected steady-state miss and must not be treated as fatal. |
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
| `xrWeaveSnapWindowRectDXR` | vendor DP lattice snap | identity (no VK DP snap slot yet) |

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
  that is correct, and it is what macOS already ships.

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

§4b arrived first, and on its own would not have earned a bump — the entry points simply
started reporting a dead connection with the same `XR_ERROR_INSTANCE_LOST` every other
IPC-backed OpenXR call already used, and §4b wrote down a contract that was previously
implicit. §4c's entry point + struct settle it: **v9**.

## 7. Consumers

| Consumer | Path | Layout used |
|---|---|---|
| DisplayXR Browser (Chromium fork) | GPU-process sync weave | Batch (v3) when the runtime reports spec ≥ 3; per-element legacy loop otherwise |
| CEF weave host (Step A) | Browser-process sync | Legacy |
| `displayxr-webxr-bridge` | Service client | Legacy |
| DisplayXR Browser on Android | Chromium GPU process → satellite compositor (ADR-036 D3) | Batch (v3/v7) — AHardwareBuffer handles + published window geometry |

When changing the header, byte-sync every consumer's vendored copy and rebuild it
(`third_party/displayxr` in the fork) — coupled-PR order: runtime → extensions auto-sync →
consumers.
