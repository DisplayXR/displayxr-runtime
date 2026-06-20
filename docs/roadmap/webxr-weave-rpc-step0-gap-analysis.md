# Step 0 — DXR Weave RPC: Phase-0 Gap Analysis

Companion to [`webxr-support.md`](webxr-support.md) **§2.3–§2.4** (the inline-3D roadmap).
Tracks issue **#625**. This document answers one question precisely, with `file:line`
citations: **what already exists** to support a window-bound `bindWindow` + `weave(...)`
service, and **what is missing**, so Phase 1 builds only the genuinely new wire plumbing and
reuses every pixel path.

> Audited from the `feat/webxr-weave-rpc` worktree (off `main`). Line numbers are accurate as
> of that base; treat them as anchors, not guarantees, and re-grep before editing.

## The target (from webxr-support.md §2.4 Step 0)

> Add a window-bound one-shot weave service to `displayxr-service`:
> `bindWindow(handle)` + `weave(shared_handle, window_relative_rect, eyes) → weaved_shared_handle`.
> It imports the handle, runs the **DP** weave at the phase derived from window-position + rect,
> and exports the weaved handle. … reuses the existing texture + window-binding + DP
> `process_atlas`/weave paths. Testable with zero browser.

Architecturally this is a **texture-class** app driven cross-process: a present-owner that owns
its OS window, hands the runtime *pre-weave* textures, lets the DP weave, and composites/presents
the result itself. The caller never weaves (ADR-007 / ADR-019).

---

## Q(a) — Is texture-class present-ownership / weave-into-a-caller-shared-texture reachable over IPC?

**No. In-process only.**

- `oxr_xrSetSharedTextureOutputRectEXT` and `oxr_xrSetSharedTextureSurround2DEXT` dispatch
  **only** to `sess->is_*_native_compositor && sess->xcn != NULL` branches (D3D11 / Metal / GL /
  VK / D3D12) and otherwise return `XR_ERROR_FEATURE_UNSUPPORTED` —
  `src/xrt/state_trackers/oxr/oxr_api_session.c:1643-1678`. There is **no** `is_service_mode` /
  `xcc` (IPC-client) branch; under IPC these calls hard-fail.
- The in-process texture path that *is* wired:
  - HWND / shared-texture binding — `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp:2213-2248`
    (texture-class stores the app HWND in `c->app_hwnd`; `c->hwnd = nullptr`).
  - DP gets the HWND + texture-present flag —
    `comp_d3d11_compositor.cpp:2424-2447` (`set_shared_texture_present`).
  - Output rect stored as `c->canvas` — `comp_d3d11_compositor.cpp:2599-2609`
    (`comp_d3d11_compositor_set_output_rect`), fed to the DP as `canvas_offset/size` at
    `comp_d3d11_compositor.cpp:1986-1992`.
- **None of this crosses IPC.** The IPC client compositor is server-creates-swapchain
  (`src/xrt/ipc/client/ipc_client_compositor.c`) — apps submit render layers into shared-memory
  slots; the server composites and presents.

**Gap:** the present-owning, weave-into-a-caller-supplied-texture model has no IPC transport today.

---

## Q(b) — How does a present-owning IPC client get the WEAVED pixels back, vs the server presenting?

**Normally the server presents.** The DP weave runs server-side
(`xrt_display_processor_d3d11_process_atlas(...)`, `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:8054`)
and the service presents its own back buffer (`swap_chain->Present(...)`, `:8116`). The client
never sees the weaved pixels.

**The one exception — and the load-bearing precedent for Step 0 — is the transparent-output path
(issue #551).** For a present-owning client that requested a transparent background, the service
*cannot* DComp-present the client's cross-process window (owner-only), so it:

1. weaves into a **server-allocated** shared NT-handle texture (`transparent_output_texture`,
   declared `comp_d3d11_service.cpp:251-257`),
2. signals a fence, and
3. **exports** the handle + the **app's own HWND** so the **client** DComp-presents it.

Wire path, end to end:
- export — `comp_d3d11_service_compositor_export_transparent_output`, `comp_d3d11_service.cpp:11261-11286`
- server handler — `ipc_handle_compositor_get_transparent_output`, `src/xrt/ipc/server/ipc_server_handler.c:5286-5328`
- proto — `compositor_get_transparent_output` (+ `_fence`), `src/xrt/ipc/shared/proto.json:692-707`
- client pull — `comp_ipc_client_compositor_get_transparent_output`, `src/xrt/ipc/client/ipc_client_compositor.c:402-438`

**But it is not the texture-class model:** the output is **server-allocated** (not caller-supplied),
**single full-frame** (no output rect / sub-rect), **no caller input texture**, **no zones**, and
**D3D11-service-only** (`#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)`, `ipc_server_handler.c:5308`).

**Gap:** Step 0 generalizes this "server weaves → present-owning client presents" handback to
(1) accept a **caller-supplied pre-weave input texture** and (2) honor a **window-relative output
rect** (one 3D zone). The handback + fence mechanics are reused as-is.

---

## Q(c) — Can eyes be injected by the caller into the weave?

**No path today** — and this is the genuinely new "external eyes" plumbing the roadmap calls out.

- `process_atlas` carries **no** eye / view / IPD / convergence fields — only geometry
  (`canvas_offset_x/y`, `canvas_width/height`, atlas/target dims) —
  `src/xrt/include/xrt/xrt_display_processor.h:124-141`.
- The DP weaves using eyes it reads **internally** from the vendor eye-tracker —
  `get_predicted_eye_positions`, `xrt_display_processor.h:151`. The eye struct
  (`xrt_eye_positions`) lives in `src/xrt/include/xrt/xrt_display_metrics.h:51-60` and is consumed
  by a **different** vtable slot, not by `process_atlas`.
- Direction of flow is **DP → app**: the compositor reads the DP's eyes and surfaces them through
  `xrLocateViews` (`src/xrt/state_trackers/oxr/oxr_session.c:1724`) so the app renders matching
  off-axis frusta. Eyes never travel app → DP.

webxr-support.md §2.3 originally listed `eyes` as a `weave()` *input*. That framing was wrong
(see the corrected §2.3): the interlace is DP-internal and reads the vendor tracker, so the
caller feeds it nothing for the weave.

**Decision (issue #625) — corrected in Phase 2:** eyes flow **runtime → caller**, matching the
existing `DP → app` direction above. `weave()` **returns** the DP's current tracked eyes (read
via `get_predicted_eye_positions`) so the present-owner drives its **own** off-axis (Kooima)
projection for the next pre-weave frame — virtual-camera motion / look-around. Phase 1 carried a
vestigial caller→weave `eyes` input (unused); **Phase 2 removed it and added the eye *return*** to
the handback (`XrWeaveOutputEXT` / `weave_submit` out). For an OpenXR-session present-owner the
same eyes are already available via `xrLocateViews`; the return value is what makes a
*session-less* caller (CEF) work.

---

## Q(d) — Does the IPC path carry per-frame dynamic zone geometry + window-relative rects?

**Partially — and ADR-007-clean.**

- **Zone layer geometry** crosses per-frame via the **shared-memory layer slots** (not proto.json):
  `XRT_LAYER_ZONE_3D` (client `src/xrt/ipc/client/ipc_client_compositor.c:1875`, server
  `src/xrt/ipc/server/ipc_server_handler.c:2860-2969`) and `XRT_LAYER_LOCAL_2D` (client `:1860`)
  write `xrt_layer_data` into `ism->slots[].layers[]`. The server composites these into a single
  window-spanning super-atlas and makes **one** `process_atlas` call — no per-zone DP handoff
  (respects ADR-007 / display-zones §7).
- A single **window-relative** locate-rebase rect also crosses per-frame via
  `session_locate_views_rig` → `ipc_view_rig_info.zone_*` (`src/xrt/ipc/shared/ipc_protocol.h:787-798`,
  `src/xrt/ipc/shared/proto.json:770-779`), consumed server-side only to rebase the Kooima/eye math.

**Gap:** there is no per-frame call by which a present-owning caller designates a caller-owned
pre-weave texture + a window-relative rect as the weave input and gets the weaved sub-rect back.
That is exactly `weave_submit`.

---

## Gap summary — what Step 0 must add (and what it reuses)

| # | Capability | Status today | Step-0 action |
|---|---|---|---|
| 1 | Present-owning IPC client binds its window for DP phase-snap | Server *has* the app HWND on the transparent-output path, but there's no explicit bind RPC | **Add** `weave_bind_window(hwnd)`; reuse `XR_EXT_{win32,cocoa}_window_binding` semantics + existing window-metrics re-snap |
| 2 | Import a **caller-supplied pre-weave** texture for the DP to weave | `swapchain_import` imports a *render* swapchain (`proto.json:637`); not a pre-weave-content-for-DP designation | **Add** `in_handles` on `weave_submit` |
| 3 | Weaved output returned as a **window-relative sub-rect** | Transparent-output returns a server-allocated **full-frame** texture (`comp_d3d11_service.cpp:11261`) | **Generalize** the handback to carry an output rect → `canvas_offset/size` for `process_atlas` |
| 4 | Caller-supplied **eyes** drive the weave | `process_atlas` has no eye param (`xrt_display_processor.h:124-141`) | **Phase 2** — `eyes` on the wire in Phase 1 but unused |
| 5 | DP weave itself (`process_atlas`), zone compositing, fence handback | **Exists** — ADR-007-clean | **Reuse wholesale** — no new weave/interlace code |

**The new code is wire plumbing, not compositing.** Phase 1 adds two RPCs and a native harness;
the DP still owns the weave, the geometry is still `canvas_offset/size`, the input is a standard
imported shared texture, and the return reuses the #551 server-allocated-texture + fence pattern.
