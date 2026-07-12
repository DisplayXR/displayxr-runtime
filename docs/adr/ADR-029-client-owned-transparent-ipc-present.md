---
status: Accepted
date: 2026-06-12
issues: [551]
---
# ADR-029: The IPC client owns the transparent present; the DP reconstructs alpha, never bakes a background

## Context

A transparent DisplayXR app (`transparentBackgroundEnabled = XR_TRUE`, see
`XR_DXR_win32_window_binding`) wants `alpha = 0` regions of its content to show
the **live desktop** behind its window. In-process this is settled (ADR-007's DP
weaves; the native compositor configures the app HWND for `DirectComposition` +
`DXGI_ALPHA_MODE_PREMULTIPLIED`). The **IPC/service path** —
`XRT_FORCE_MODE=ipc`, WebXR/UWP, or any standalone client of
`displayxr-service.exe` — is different in two ways that this ADR resolves:

1. **Who presents.** The per-client compositor and the display processor run
   **out-of-process**, inside `displayxr-service.exe`. A process can only create
   a `DirectComposition` target (`IDCompositionDevice::CreateTargetForHwnd`) —
   and a windowed swap chain (`CreateSwapChainForHwnd`) — on a window **it owns**.
   On a client's HWND both calls return `E_ACCESSDENIED`. So the service
   physically cannot present transparently onto the app's window. (Symmetrically,
   `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` is owner-only — see §
   "Mechanics".)

2. **How the hole is filled.** The Leia DP's preferred see-through is
   *compose-under-bg*: it captures the desktop region behind the window via
   Windows Graphics Capture (WGC) and composites it **under** the app's
   premultiplied atlas before weaving, so the weaver outputs opaque RGB. Over
   IPC the captured frame is always **at least one frame stale**, and while
   dragging a window or playing video under the app the lag is obvious.

## Decision

**The IPC client owns the transparent present, and the DP only reconstructs
alpha — it never bakes a captured background when the client presents.**

### 1. Service renders to a shared texture; the client presents it

When a client is transparent and supplies its own (cross-process) HWND
(`use_client_transparent`), the service does **not** create a swap chain.
Instead it creates a **shared NT-handle texture** (`R8G8B8A8_UNORM`,
premultiplied alpha) plus a **service→client D3D11 fence**. The per-client
compositor binds that texture's RTV as its back buffer, so the weave + alpha
reconstruction land directly in it; then it signals the fence.

Two new IPC RPCs (`compositor_get_transparent_output` and
`…_output_fence`) hand the duplicated NT handles to the client. The client
(`comp_d3d11_client`) imports the texture and fence, owns a `DirectComposition`
device/target/visual on **its own** window, and on each `xrEndFrame`:
GPU-waits the fence → `CopyResource` shared→DComp back buffer → `Present` →
`Commit`. DWM blends the `alpha = 0` pixels with the live desktop. No per-frame
IPC (lockstep fence only); ~zero added lag.

This is the reusable template for **any** transparent IPC app — not specific to
the zones demo that motivated it.

### 2. `set_transparent_background(enabled, client_presents)` — the DP mode signal

The D3D11 DP vtable slot gains a `client_presents` argument (ADR-020 append-only;
the 2-arg form is retired in the same window since it had no released callers):

- `client_presents = false` (in-process / opaque-present IPC): DP owns
  see-through — compose-under-bg (WGC), chroma-key fallback if WGC is
  unavailable.
- `client_presents = true` (the §1 path): the runtime owns a transparent
  present and DWM supplies the live desktop, so the DP must **NOT**
  compose-under-bg (no WGC, no stale frame). It runs **only** the post-weave
  **alpha-gate**: a fullscreen pass that samples the original premultiplied
  atlas and, for every pixel where **all** view tiles have `alpha == 0`, writes
  `float4(0,0,0,0)` to the output so DWM shows the desktop. Pixels where any
  view is opaque keep the weaver's lenticular blend at `alpha = 1`.

The alpha-gate is decoupled from compose (`alpha_gate_should_run =
compose_should_run || client_present_mode`) and lazily builds its pipeline so
it works with no WGC capture ever created.

## Consequences

- **No background lag.** Live-desktop see-through is exactly as current as DWM
  itself; the only thing crossing IPC is the app's own woven frame.
- **Chroma-key is now a fallback, not a path.** The clean enable
  (`set_transparent_background`) never chroma-keys; `set_chroma_key` remains only
  for a DP too old to advertise the slot. Plan of record is to remove chroma-key
  once all DPs ship the slot.
- **One owner of the present per topology.** In-process: native compositor.
  IPC: the client. The service never presents onto a window it doesn't own —
  removing a class of `E_ACCESSDENIED` failures and the temptation to work
  around them.

## Mechanics / gotchas (load-bearing)

- **A fullscreen pass that *replaces* a target's alpha must force blend OFF.**
  The alpha-gate writes `float4(0,0,0,0)` to punch holes. The service zones
  composite leaves a **src-over** blend state bound; under it the gate's output
  blends as `dst*(1-0) = dst` — a **no-op**, so an opaque black weave survives
  and the desktop never shows. In-process the gate happened to inherit a
  blend-disabled state, masking the bug. The gate now sets
  `OMSetBlendState(nullptr, nullptr, 0xffffffff)` (blend-off, write-all-channels)
  before its draw. Any DP/post pass that reconstructs alpha must do the same.
- **Capture-exclusion is owner-only.** `WDA_EXCLUDEFROMCAPTURE` (so the WGC
  fallback doesn't capture the app's own window) must be set from the
  window-owning **app** process — the runtime requests it client-side via the
  `transparentBackgroundEnabled` parse in `oxr_session`. The DP (out-of-process)
  only reads affinity and tolerates a cross-process `ERROR_ACCESS_DENIED`.
- **App-side HWND requirements are unchanged** (`WS_EX_NOREDIRECTIONBITMAP` +
  null background brush; see `XR_DXR_win32_window_binding` §3.2). They apply
  identically whether the runtime or the client owns the present.

## Related

- ADR-007 (compositor never weaves), ADR-020 (plugin ABI append-only),
  ADR-027 (display zones — the motivating client).
- `XR_DXR_win32_window_binding` §3.2 (transparent-window contract, per-API matrix).
