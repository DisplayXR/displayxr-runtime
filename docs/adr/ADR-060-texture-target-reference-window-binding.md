---
status: Accepted
date: 2026-10-25
source: "#697"
---
# ADR-060: A texture target is named by a reference window and a rect; its display processor is windowless

**Status:** Accepted (2026-10-26). The seven open questions were decided on 2026-10-25 (see the end).
Implemented and first released in runtime **v2.55.0**; see [Implementation](#implementation-as-landed-2026-10-26).
Answers the design task #697 (part of #691). Builds on [ADR-033](ADR-033-placement-reports-geometry-weaver-owns-phase.md) (who reports
geometry), [ADR-047](ADR-047-multi-screen-segments-and-per-screen-views.md) Amendments 2 + 5 (per-screen segment DPs, the
windowless DP), [ADR-027](ADR-027-display-zones.md) / [ADR-031](ADR-031-remove-surround-output-rect-zones-sole-region-model.md)
(zones are the only region model) and [ADR-010](ADR-010-shared-app-iosurface-worst-case-sized.md)
(the shared texture is sized once, worst case). Spec:
[`XR_DXR_win32_window_binding` v9, texture target](../specs/extensions/XR_DXR_win32_window_binding_v9_texture_target.md)
(summarised in [`XR_DXR_win32_window_binding` §3.6](../specs/extensions/XR_DXR_win32_window_binding.md#36-texture-target-binding-v9)).

> Code anchors in Context, Decision and Consequences (`file:line`) are as of `024e1bb66`, the
> commit this ADR was written against. Where the code went is in [Implementation](#implementation-as-landed-2026-10-26).

## Context

A `_texture` session (`XR_DXR_win32_window_binding` with `sharedTextureHandle`) never presents.
The DP weaves into the app's shared texture and the app draws it in its own window
(`comp_d3d11_compositor.cpp:5953-5960` keeps the HWND only as `app_hwnd`; `:5401-5460` weaves
into `shared_rtv`). The HWND it passes does two jobs, and both are about **position**:

1. **Phase.** The DP is created on that HWND (`d3d11_dp_create`, `comp_d3d11_compositor.cpp:5852`
   `dp_hwnd = c->hwnd ? c->hwnd : c->app_hwnd`). In SDK routing the Leia weaver reads the
   window's client rect every frame and subclasses its **root** window
   (`GetAncestor(GA_ROOT)` + `SetWindowLongPtr`, LeiaSR `WeaverBaseImpl.ipp:522-534`).
2. **Window metrics.** `comp_d3d11_compositor_get_window_metrics` (`:10315-10420`) does
   `GetClientRect` + `ClientToScreen` on `app_hwnd` and derives the canvas-scoped Kooima (#396 W7)
   and the segment window `seg_window` (`:10372-10376`).

Every consumer that matters cannot give a truthful HWND for this:

- **An editor viewport** (Unity Game view, the Unreal PIE viewport, a DCC viewport) is a
  *sub-rect* of a window that also holds the editor's UI. Today Unity creates a top-level
  `WS_POPUP` **anchor window** over the viewport, only to be the HWND the DP binds to. Each anchor
  fragility (stereo collapse after `SetParent`, a subclass that outlives teardown,
  displayxr-unity#284, a session restart on re-dock) traces back to the weaver binding a window.
- **A windowless producer** (CEF, a decode target) has no window at all. The NULL-HWND path
  exists (`:5957-5959`), but it degrades: no window metrics (`:10336`), so the views are
  display-scoped, and no phase.
- **The canvas** comes from the HWND too. `d3d11_effective_canvas` (`:1204-1223`) returns the
  window's client rect only for a zones / mask / Local2D frame; otherwise the canvas is
  invalid and the DP fills the whole worst-case texture. That is why the shared-texture contract
  says "submit a zones frame every frame" (INV-5.6).

Three facts make a clean answer cheap now:

- **ADR-033 already places this.** For an offscreen or texture target, the DisplayXR
  compositor is the placement authority, and "the OS window rect would be actively wrong". The
  runtime must supply the origin through `set_present_origin`. #697 was waiting for a
  vendor to accept a position stream, and that now exists.
- **The windowless DP already exists on every Windows in-process API.** Multi-screen M6 makes
  segment DPs through `create_dp_{d3d11,d3d12,gl}_for_screen` with a NULL window, feeds them
  `set_present_origin` per segment (`comp_d3d11_segments.cpp:1039,1080`,
  `comp_d3d12_segments.cpp:963`, `comp_gl_segments.cpp:859,881`), and already runs a
  **windowless primary DP** while another screen holds the window (`primary_windowless`,
  `comp_d3d11_segments.cpp:101,335,923,1038`). The plug-in contract already says "a DP must weave
  with a NULL window" (`xrt_plugin.h:1517-1521`).
- **The vendor half has shipped to a dev box.** The Leia plug-in creates
  `SR_WEAVER_ROUTING_EXTERNAL` weavers with a NULL HWND when SR reports routing
  (`leia_sr_d3d11.cpp:546-600`; plan in `leia_sr_multi_win.h:81-133`). LeiaSR 1.39.0 on the
  Windows hardware box reports `externalRouting=1 displayBinding=1`. EXTERNAL routing and display
  binding exist only on the 1.39 line; LeiaSR < 1.39.0 does not have them (1.38.1 has
  `srWeaverSetPresentOrigin` only).

## Decision

**A texture target is named by (reference window, rect in its client area, device px). Its
DP is windowless. The runtime is the placement authority: every frame it computes the
present origin, the window metrics and the segment table from that pair.**

1. **Destination = reference window + rect.** The app (or a host library acting for it)
   names a **reference window** and a **target rect**:
   - The reference window is the top-level window whose client area contains the target
     pixels. It is the app's own window, never an anchor. The runtime only *reads* it
     (`GetClientRect`, `ClientToScreen`); it never subclasses or reparents it, and never hands
     it to a DP.
   - The target rect is where the app draws the shared texture, in that client area, in device
     pixels. It is per frame, and the frame's `xrEndFrame` value is authoritative.
   - A NULL reference window means the rect is in screen (virtual-desktop) device pixels. That
     is the form for a producer with no window, and it equals `XrWeaveWindowGeometryDXR` on
     Android.

   Why a window plus a rect, and not a screen rect: the OS reports the window's live position
   (ADR-033 row 1), so a moving window costs no extra frame of lag. Inside the window, the app is
   the only party that knows where the viewport is. Each offset term has exactly one owner, so
   no party adds a correction on top of another (the Unity double-offset lesson).

2. **One computation, three outputs.** At weave time the runtime resolves
   `target_screen = ClientToScreen(ref, rect.origin)` (or `rect.origin` when `ref` is NULL),
   with the thread pinned to `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`. The host process may
   be DPI-unaware (docs/reference/dpi-awareness.md); today's `get_window_metrics` does **not**
   pin it. From `target_screen` and `rect.size`:
   - **Phase:** `set_present_origin(target_screen − panel.origin)` on the primary DP every
     weave; per segment, the segment manager's own `present_origin_x/y`.
   - **Window metrics / Kooima:** `xrt_window_metrics.window_screen_left/top = target_screen`,
     `window_pixel_width/height = rect.size`. These are exactly the four fields #697 named
     (`:10410-10413`), so canvas-scoped Kooima and the M3 per-segment views follow with no new
     math.
   - **Segments:** `seg_window = {target_screen, rect.size}`. Spanning, the 2D monitor and
     mixed 3D/2D come from the existing segment managers. The segment canvas is the target,
     and the segment target is `shared_rtv`.

3. **The rect is the canvas.** Under a target binding the canvas is `(0, 0, rect.w, rect.h)`
   in the shared texture, every frame, with or without zones. The "submit a zones frame every
   frame" rule (INV-5.6) and the whole-texture magnification go away. Every rect in
   `XR_DXR_display_zones` (zone rects, `XrCompositionLayerLocal2DDXR`, mask tiers) is in
   **target-local** pixels: "client window" in those specs means the target. A frame without
   zones is one implicit full-target 3D canvas. With zones, they partition the target as they
   partition a window today. `xrGetWorkspaceTileSizeDXR` returns `rect.size` when standalone.

4. **The DP is windowless, and only a DP that says so is used.** A DP declares that its phase
   follows `set_present_origin` with no window through a new scanout-caps bit
   (`XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN`, carved from `xrt_dp_scanout_caps::flags`,
   `xrt_display_scanout.h:113-117`; zero ABI growth, ADR-020). The Leia DP sets it exactly when
   `leia_win_sr_plan_needs_present_origin` is true; sim-display sets it on every DP, because all
   of them honor the slot. The runtime creates the primary DP windowless
   (`create_dp_*_for_screen`, `window = NULL`) and reads the bit:
   - **set** ⟹ target-binding mode;
   - **clear** (LeiaSR < 1.39.0, an old plug-in) ⟹ the DP is destroyed and the session takes the
     **fallback**: the legacy `windowHandle` anchor if the app passed one (today's path,
     unchanged), else display-scoped weaving with a one-time WARN, and the session reports
     `windowlessPhase = XR_FALSE`.

   Ahead of session creation, apps read `XrSystemWin32TextureTargetPropertiesDXR` from
   `xrGetSystemProperties`. This lets an app skip creating an anchor it won't need. The system
   answer is a prediction (a plug-in-level hint, item 4 of the open questions). The DP bit,
   read at session creation, is the authority.

5. **No geometry generation counter in Path S, by construction.** In-process texture mode
   weaves for the rect submitted on *this* `xrEndFrame`. It does so synchronously, on the app's
   own device ordering: D3D11 in `layer_commit` (`:5401-5460`), D3D12 behind
   `gpu_wait_idle_app` (`comp_d3d12_compositor.cpp:6127-6145`), and GL by upload before return
   (`comp_gl_compositor.cpp:3414-3444`). The texture the app reads after `xrEndFrame` returns
   was woven for the rect the app just named. Nothing can be stale *within the app's layout*,
   and the reference window's screen position is read live at weave time. The one remaining
   gap, a window moving between weave and the app's own present, is the same as any window
   drag today. The runtime still publishes what it used (point 6), so a host can assert it
   instead of trusting it. A counter returns if texture mode ever goes asynchronous (IPC, or the
   ADR-059 hub with a deferred weave). Path W (`XR_DXR_weave`) keeps its own generation (v19) and
   woven-origin tag (v12 §5e).

   *As landed:* the hub's client-presents route (#2380, opt-in with `DXR_HUB=1`) keeps this
   property without a counter. It weaves on the hub device, and the caller's own context or queue
   GPU-waits the hub's release fence at the end of its frame submit, so the caller's blit orders
   after the weave. It does not CPU-wait.

6. **What the runtime used is observable.** `XrWin32TextureTargetStateDXR` is chained on
   `XrFrameState` at `xrWaitFrame` and reports the previous woven frame: whether the weave was
   windowless, the resolved screen origin, the rect, and whether any segment fell back to flat
   2D. This is a diagnostic and qualification channel (a 1:1 placement probe for editor
   viewports), not a control loop.

7. **What does not change.** Phase math stays in the DP (ADR-033): the runtime sends two
   integers. Drag snap stays the vendor's. A host that wants snap during a third-party window
   drag asks through the existing `snap_window_rect` path, never by computing phase. The
   anchor path (`windowHandle` + `sharedTextureHandle`, no target binding) stays byte-identical.
   `_handle` and `_hosted` sessions are untouched.

## Consequences

- **Unity and Unreal editor previews drop the anchor.** They pass their editor's main window
  (or NULL plus a screen rect) and the viewport rect every frame. Dock, undock and full screen
  become a reference-window change on the next frame: no session restart, no window to tear
  down (displayxr-unity#284 by construction).
- **"Texture mode writes nothing without a zone" is gone.** That INV-5.6 clause moves to the
  anchor path only.
- **Multi-monitor is inherited, not built.** A target wholly on a 2D screen gets the segment
  manager's flat left view and the lens-coverage release (#2247). A target spanning screens
  gets per-segment DPs. Wholly on a 3D screen, the windowless primary DP alone does the work.
- **The split *predicate* runs on every bound frame; the split *weave* runs only when the target
  touches another screen.** An EXTERNAL-routed weaver turns off the vendor SDK's own 2D-region
  split and lens vote (LeiaSR `WeaverBaseImpl.ipp:170-176, 793-795, 1342-1344`). It always
  weaves, as one 3D region, so the runtime's per-screen split is the *only* thing that keeps a
  non-3D monitor flat.
  - **The predicate is enough.** `comp_segments_table_is_split` (`comp_segments.c:150-169`)
    returns true for more than one segment, and for a single segment that is *not* on the
    primary screen: that covers wholly on a 2D monitor, wholly on another 3D screen, and
    spanning. It returns false only for a target wholly on the primary (3D) screen. In that one
    case the single-DP path plus a direct `set_present_origin` is exactly equivalent. The
    predicate is CPU rect math, and `comp_d3d11_segments_would_split` already evaluates it every
    weave for any windowed session with a segment window (`comp_d3d11_compositor.cpp:3157`).
  - **Forcing the weave path there would only add cost.** It adds a target-sized clear, plus a
    second crop of the whole content atlas on top of the one `d3d11_crop_atlas_for_dp` already
    did (`comp_d3d11_segments.cpp:984-1011`, one `CopySubresourceRegion` per tile). It also gives
    up zero-copy, which the record path cannot take (`:3163`). Estimate, not measured: for a 4K
    2×1 atlas that is about 2 × 16 MB of copy traffic, about 0.05-0.1 ms on a dGPU and
    0.5-0.8 ms on an iGPU, every frame, for no visible change.
  - **What R1 must change.** Today a windowless primary *is* forced through the record path,
    but only as plumbing, because "a windowless primary DP needs its present origin, which only
    the split path sends" (`comp_d3d11_segments.cpp:919-923`). R1 pushes the primary's origin on
    the single-DP branch itself (the shared-texture weave, `comp_d3d11_compositor.cpp:5447`,
    before `process_atlas`), and drops that forcing for bound sessions. Today texture mode never
    reaches the segment manager at all: it weaves in its own branch of `layer_commit`
    (`:5401-5460`), not in `d3d11_dp_weave` (`:2649`, where the segment block is, `:3116-3240`).
    So R1 adds to that branch the predicate, plus the record call with
    `target_rtv = shared_rtv`. It installs no HWND hooks (`:3130`): no DP ever holds the window.
  - The Leia DP driving its own lens under EXTERNAL is L3, to verify on hardware.
- **The DPI rule becomes load-bearing in-process.** Third-party hosts (Blender, Kit, Slicer)
  are not guaranteed per-monitor-v2 aware, so every runtime read of the reference window pins
  the thread context. The rect is defined in physical device pixels, and converting a logical
  rect is the host's job (the host library does it once).
- **Vendor surface: one flag.** A vendor whose DP can phase windowless sets one bit. A vendor
  that can't loses nothing: its users get the anchor fallback.
- **ADR-059 interaction: native D3D11 now; D3D12, GL and Vulkan via the hub (decision 3).**
  The hub side landed as #2380; see [Implementation](#implementation-as-landed-2026-10-26) for what it covers.
  - **When this was written:** the D3D11 hub refused shared-texture sessions
    (`comp_d3d11_compositor.cpp:4326-4327`, `COMP_SPLIT_REASON_SHARED_TEXTURE`). That is a scope
    cut in P1b, not a limit of the hub.
  - **Plan (2026-10-25):** the hub takes texture mode right after its P3 (the GL
    shim). Texture mode is one more presenter strategy for it: ADR-035 D3 "client-presents", where
    the hub weaves INTO the caller's shared texture and the caller presents. That is the shape the
    service's `XR_DXR_weave` route already has.
  - **So:** R1 is implemented once, in the native D3D11 compositor. D3D12, GL and Vulkan texture
    sessions get it as import shims of the hub when it takes texture mode; there is no native port.
  - **Hub-facing data.** R1 is therefore shaped as data the hub consumes unchanged, not as
    compositor-tier code: (reference window, rect) → one resolve (the shared helper) → per-frame
    `set_present_origin` + window metrics + segment window. R2's windowless DP and its scanout bit
    are the same mechanism; on the hub the bit becomes one more create-time gate token.
  - **The four interface points the hub owner named,** which R1 must not close off:
    1. The hub binds its DP by (hwnd, device) and refuses `no_hwnd`. A windowless primary DP must
       become a first-class hub mode (the `set_present_origin` path).
    2. The hub's repaint pacer presents through its DXGI target. In client-presents mode there is
       no hub Present, so a repaint re-weaves into the shared texture and signals the caller's
       fence instead.
    3. The hub's capture site writes its back buffer as `.woven.png`. In texture mode the woven
       output is the caller's texture.
    4. The per-segment zone publish uses the hub's segment table from window metrics. Segments
       derived from the rect must feed that same table.
- **Size** (in-process, Windows, excluding tests). After decision 3, about 700-900 lines,
  roughly 1 week, low-medium risk. The D3D12 and GL rows below are kept for the record only:
  they are NOT done natively, and arrive through the hub.
  | Part | Where | Est. lines |
  |---|---|---|
  | OpenXR: parse the create struct, the per-frame struct on `xrEndFrame` + locate, the system property, the state on `xrWaitFrame` | `oxr_session.c:6929-6965` (create), `oxr_session_frame_end.c:1587-1670` (next to the zones parse), `oxr_session.c:3538-3545` (zone-scoped locate) | 200-250 |
  | xrt plumbing: `xrt_session_info` fields + one per-frame compositor call | `xrt_compositor.h`, `oxr_session.c:553-603` | 60-80 |
  | Shared helper: resolve (ref, rect) under PMv2 → screen rect, metrics, present origin; unit tests | new `compositor/util/comp_texture_target_win32.{c,h}` | 150 + 100 test |
  | D3D11: windowless primary + caps check + fallback; the nine `hwnd ?: app_hwnd` geometry sites → the helper; `d3d11_effective_canvas`; `get_window_metrics`; the shared-texture branch feeds `set_present_origin` and routes a split through the segment manager into `shared_rtv` | `:5852-5862`, `:1213, 3930, 6633, 7627, 8288, 8716, 8871, 9900, 10334`, `:1204-1223`, `:10315-10420`, `:5401-5460`, `:3116-3138` | 250-300 |
  | ~~D3D12: the same shape~~ (via the hub) | `comp_d3d12_compositor.cpp:959` (canvas), `:1392-1403` (DP create), `:6127` (shared branch), `:7542` (metrics), 15 `app_hwnd` sites, `comp_d3d12_segments.cpp:963` | 250-300 |
  | ~~GL~~ (via the hub): split `c->hwnd` (today the app HWND in texture mode too, `:8167`) from the reference; metrics `:8838`; segments hook `:5120-5140`; readback upload `:3414-3444` (R7 makes this interop-first) | `comp_gl_compositor.cpp` | 250-350, highest risk |
  | sim-display: the caps bit on its D3D11 DP + the claim hint | done in R2 (`sim_display_processor_d3d11.cpp`, `sim_display_plugin.c`) | 0 |
  | Leia plug-in (L4, not this repo): the claim hint + the D3D11 DP's scanout bit | done: `displayxr-leia-plugin` `feat/l4-phase-from-present-origin` | 0 |
- **Vulkan is a follow-up.** `vk_native` has the same segment machinery (`comp_vk_native_segments.c:161,508,1255`), but no shared-texture export on Windows (R8). The struct is graphics-API-neutral, so Vulkan adopts it with no spec change.
- **macOS / Linux** keep their window bindings. The struct shape (reference handle + rect) carries over to `XR_DXR_cocoa_window_binding` / `XR_DXR_xlib_window_binding` if an editor preview needs it there. Not part of this ADR.

## Implementation (as landed, 2026-10-26)

Every change below is on `main` and in runtime **v2.55.0**, the first release that carries the
binding.

| Part | PR | Commit | What it is |
|---|---|---|---|
| R2 + R3 | #2379 | `37cc70630` | `XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN` (`xrt_display_scanout.h`, bit 1); the claim hint `XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE` (claim bit 24, copied to `xrt_dp_registry_entry::claim_apis`); the D3D11 service's windowless present-owner DP for `XR_DXR_weave`, phased from the window's origin (kill switch `DXR_WEAVE_WINDOWLESS=0`); the woven origin appended to the `weave_submit` reply (R3, diagnostic, `XR_DXR_weave` §5e); sim-display's bit and hint. Notes: [`texture-target-r2-r3-notes.md`](../roadmap/texture-target-r2-r3-notes.md). |
| R1 | #2373 | `0a040357d` | `XR_DXR_win32_window_binding` v9 (the four structs, types 1004999015-018, registered in the `openxr_includes` README; ABI snapshot regenerated); OpenXR glue `oxr_texture_target.c`; the shared resolve `compositor/util/comp_texture_target{,_win32}.h` (data in, data out, PMv2-pinned); the windowless primary DP on the native D3D11 compositor, with separate `frame` and `locate` slots; `tests_comp_texture_target`. The reference client is `cube_zones_texture_d3d11_win` with `DXR_TEXTURE_TARGET=1`. |
| Hub client-presents | #2380 | `b512dd7f1` | #2370: a third hub presenter strategy (ADR-035 D3). The hub weaves texture-class sessions into the caller's shared texture and hands it back on a fence; the caller presents. The windowless-DP rule is shared with R1 through `compositor/util/comp_dp_windowless_d3d11.h`. **Opt-in** with `DXR_HUB=1`; byte-identical with it unset. D3D11 v9 sessions and D3D12 texture sessions take it; GL is not carried yet. |
| Anchor fallback, sub-rect phase | #2406 | `946041dd2` | Under the anchor fallback, a target rect inset from the anchor's client origin used to be woven as if it started at the origin, so its phase was off by the inset. The canvas is now woven at its offset in a grow-only padded render target and copied back. Internal; no spec change. A target that cannot be corrected (left of or above the anchor, outside its client area, or past the maximum texture size) keeps the old path and logs one WARN. |
| Leia plug-in L4 | displayxr-leia-plugin#450 | `36be25cb8` | The windowless-phase claim hint, and the D3D11 DP's `PHASE_FROM_PRESENT_ORIGIN` bit when SR routes EXTERNAL. It ships in plug-in **v2.18.3**. Windowless phase needs **LeiaSR 1.39.0 or later**; on earlier LeiaSR versions the plug-in falls back cleanly to the windowed anchor path, which #2406 phases correctly for sub-rect canvases. |

**Verified on the Leia panel** (LeiaSR 1.39.0, 2026-10-25/26):
- **R2, both arms.** sim-display weaves windowless, phased from the present origin. Leia without L4 declines cleanly: no extra SR weaver, no lens flip. With L4, the EXTERNAL weaver is ready in about 1.65 s, SR casts no lens vote and installs no wndProc hook, and the plug-in drives the lens.
- **R1 phase, with `SR_SIMULATED_VIEWER`.** The windowless weave equals the anchored weave at the same window position (shift 0.00 px, NCC 1.000). A window moved by (+1, 0) or by (0, +1…+3) gives the base capture shifted by exactly that, with NCC ≈ 0.999. The panel-locked lattice is therefore followed to the pixel.
- **#2406.** An inset target on the anchor path gives NCC 0.9994 / 0.9997 against 0.589 without the fix. L4 windowless gives NCC ≥ 0.9995 across moves and an inset.
- **Seated test on a LeiaSR panel (2026-10-26).** Blender and USD Composer passed on the windowless path, with woven origins (5,122) and (139,149), the latter being client (26,82) plus rect (113,67). 3D Slicer passed geometry and placement on the texture target.
- **Test-rig rule.** SR's occlusion gate makes the *anchor* arm weave flat when another window, such as a console window, covers it. EXTERNAL returns before that check, so a windowless arm does not show it. Keep the test window TOPMOST.

**Open follow-ups:**
- #2407: re-query the scanout caps once the windowless DP is ready, and fall back to the anchor if `PHASE_FROM_PRESENT_ORIGIN` turns out to be lost (the system property is a prediction).
- #2378: the hub's client-presents route suppresses repaint, because the caller has no release fence it can wait on between frames.
- #2382: D3D12 texture sessions honour the v9 binding (sub-rect phase and target state) on the hub route. Today the reroute derives the target from the `windowHandle`'s client area.
- #2399: the native texture path's woven readback is bistable in the cube region.
- GL texture sessions on the hub (#2366). Vulkan needs a Windows shared-texture export first (R8).
- The spanning arm (a target across a 3D and a 2D screen) has not run on hardware yet; it needs a machine with a LeiaSR panel and a second monitor.
- The cocoa pair for macOS (decision 1). Not started.

## Alternatives considered

- **A screen rect only (the original #697 sketch).** It is simpler, but a host has to measure
  the window position itself every frame, which adds a frame of lag on every drag. It also
  makes the app own an offset term the OS already owns. It is kept as the NULL-reference form.
- **Keep the anchor, harden it.** Every fragility comes from the weaver binding a window it
  does not own. Hardening it means patching SR's subclass lifecycle, which ADR-033 says is not
  the runtime's problem.
- **A session-level `xrSetTextureTargetRectDXR` call.** It is not atomic with the frame's
  layers, so one frame weaves at the old rect with the new layout. The frame-chain form follows
  the established `XrDisplayZoneDXR` pattern.
- **A plug-in-level capability only.** The capability belongs to a *weaver instance*: the Leia
  plan falls back from EXTERNAL per create (`leia_sr_multi_win.h:103-119`). A plug-in-level hint
  can predict it but cannot prove it.

## Decisions on the open questions (2026-10-25)

1. **Struct home: version 9 of `XR_DXR_win32_window_binding`.** The same (reference handle,
   rect) pair is added to the other bindings only where a texture class exists:
   - `XR_DXR_cocoa_window_binding` has one (`sharedIOSurface`, Metal + GL;
     `cube_zones_texture_{metal,gl}_macos`). So macOS gets the pair in the next cocoa version,
     on the Metal segment machinery that already makes windowless DPs
     (`create_dp_metal_for_screen`, `comp_metal_segments.m:212, 1070-1079`).
   - `XR_DXR_xlib_window_binding` has **no** texture class (`XR_DXR_xlib_window_binding.h:36`),
     and the Android surface binding has none either. So Linux and Android need nothing until
     such a class exists. When one does, the pair comes with it, by the same rule.
2. **`windowHandle` alongside a target binding: allowed,** as the fallback anchor only (§6 of
   the spec).
3. **D3D11 native only; D3D12 / GL / Vulkan via the hub** (confirmed with the hub's maintainers,
   2026-10-25). R1's binding is shaped as hub-facing data. See §Consequences,
   "ADR-059 interaction".
4. **The system property is answered from a plug-in-level hint.** As implemented on the R2
   branch, the hint is a claim-mask bit, `XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE` (bit 24 of
   `xrt_display_claim::supported_apis`). That is per monitor, needs no ABI growth, and is copied
   into `xrt_dp_registry_entry::claim_apis`. The original wording was a field appended to
   `xrt_plugin_display_info`; the intent is the same. The DP's scanout bit at session creation
   stays the authority.
5. **A drag hint is reserved:** `XR_WIN32_TEXTURE_TARGET_FRAME_DRAGGING_BIT_DXR`, a flags field
   on the per-frame struct. It is advisory, and the runtime forwards it as a hint only (ADR-033:
   never phase math). It is reserved in v9, so adding a consumer later is not a struct change.
6. **R3 (the woven origin on Windows) ships now, as a diagnostic.** The condition is that it
   causes no regression: it is an appended reply field, with no behaviour change on any path
   that does not read it. It goes after R2, so that the value is exact.
7. **R2 split rule: (a).** Every windowless present-owner weave evaluates the split predicate.
   The per-screen weave runs only when the window leaves the primary 3D screen.
