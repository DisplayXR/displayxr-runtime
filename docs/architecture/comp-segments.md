# Window segments: one window, one DP per screen

*How the compositor weaves a window that spans several screens (multi-screen M2), and
how each screen gets its own views (M3).
Companion to ADR-047 (`docs/adr/ADR-047-multi-screen-segments-and-per-screen-views.md`, lands with PR #1849)
decision D2 and the multi-screen plan (`docs/roadmap/multi-screen.md`, lands with PR #1849).
Code: `src/xrt/compositor/util/comp_segments.{h,c}` (backend-agnostic math and
policy, unit-tested by `tests/tests_comp_segments.cpp`) and
`src/xrt/compositor/vk_native/comp_vk_native_segments.{h,c}` (the Vulkan
in-process compositor's use of it).*

## What a segment is

A **screen** is an OS monitor in the per-monitor DP registry; each one is won by
a plug-in, which can make a display processor (DP) for it. A **segment** is the
part of a window's canvas that lands on one screen:

```
segment = canvas ∩ screen
```

Each segment has:

| field | meaning |
|---|---|
| `window_rect` | the segment in **window pixels** — the canvas handed to `process_atlas` |
| `screen_rect` | the segment in **that screen's pixels** — where it lands on the panel |
| `present_origin` | the window's origin relative to that screen (`set_present_origin`; may be negative). The DP adds the canvas offset: `present_origin + window_rect.origin = screen_rect.origin`, the phase input |
| `screen_1to1` | whether window pixels reach that screen unresampled (desktop rect == device mode), or unknown |

The table is ordered left to right by desktop position (the order M3's
per-segment views will use). The rect math is `u_multi_display_compute_slices`,
half-open, so a window flush to a seam belongs to exactly one screen.

## Who weaves what

- The **primary screen** is the system-default screen, the one the session's own
  DP (`comp_vk_native_compositor::display_processor`) was made for. Its segment is
  woven by that DP. Its eyes feed that segment's views (M3, below).
- Every **other screen** gets a segment DP from that screen's registry entry —
  **whichever plug-in won that screen** (M3 lifted M2's one-vendor rule: the entry carries
  the owning plug-in's iface + instance, kept resident by the loader as a claim source,
  M0): its ABI-checked factory
  `xrt_plugin_iface::create_dp_vk_for_screen` with an `xrt_screen_binding`
  `xrt_plugin_iface::create_dp_vk_for_screen` with an `xrt_screen_binding`
  (monitor id, desktop rect, native px, mm, serial) — only that slot: a plug-in
  without it gets a flat 2D view on its other screens, never a plain `create_dp_vk`
  DP (which would describe the wrong panel and may clear the whole target). It is
  created against the #868 runtime-owned queue, like the primary.
  It is windowless (NULL window): its phase is `set_present_origin`, fed per frame,
  ADR-033. A plug-in's DP made for a screen describes that screen
  (`get_display_dimensions` / `get_display_pixel_info`). Every segment DP follows the
  session's 2D/3D mode (`request_display_mode`, on creation and on every change).
- A window entirely on the primary screen — the common case — never leaves the
  single-DP path: `comp_segments_table_is_split()` is false and the frame is
  byte-for-byte what shipped before.

## The frame (split path)

`comp_vk_native_segments_record()`, called where the single-DP `process_atlas`
was:

1. **Clear** the target (whatever no segment covers would otherwise be undefined).
2. **Crop per segment.** The atlas holds the whole canvas; each woven segment gets
   its own image with exactly its views (each tile cut to the segment, edges rounded
   independently so neighbours share the seam column). Crop before the DP is the
   law (ADR-030): a DP's atlas holds exactly its canvas.
3. **Weave**, primary first, then left to right. Each DP gets `canvas = segment`;
   between two DPs the target goes `PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL`
   with its contents preserved.
4. **Flat 2D** for every segment that cannot be woven and for canvas no segment
   covers: one view (the middle one, the left eye of a stereo pair), scaled from the
   view tile to the window rect — the same content and seam registration as the
   shipped #1654 band.

It leaves the target in `PRESENT_SRC_KHR`, exactly as the single-DP path does, so
the Local2D composite, the HUD and the present after it are unchanged.

## Who sets viewport and scissor

The **DP**, from the canvas. A Vulkan DP records its own render pass, so the
compositor cannot set dynamic state inside it; what it guarantees is the canvas
argument, the cropped atlas and the target layout (`COLOR_ATTACHMENT_OPTIMAL` on
entry). The contract a segment DP must honour (`create_dp_vk_for_screen` doc):

- **render area, viewport AND scissor = the canvas.** A windowless weaver's default
  scissor is its panel rect, so a backbuffer wider than the panel is clipped unless
  both are set (multi-screen plan, risk 7);
- **not the first writer**: preserve the target outside the canvas.

sim_display implements both (a screen-bound DP uses a render-pass variant starting
in `COLOR_ATTACHMENT_OPTIMAL` with the clear confined to the render area). A DP
built before M2 may still clear its whole target, which is why the primary runs
first.

**sim_display behaviour change for every caller, on purpose.** sim's viewport and
scissor now follow the canvas for the unbound (primary) DP too — the split path
needs it, and it is what the `process_atlas` contract always said. It is identical
to before whenever the canvas is the whole target (every single-screen in-process
frame); it differs when the canvas is smaller — the one-or-two-frame window-shrink
transient before the swapchain is recreated, the shared-texture path, and a
`comp_multi` zone canvas — where sim used to stretch the canvas-sized atlas across
the whole target. The interlace phase does NOT change for the unbound DP: it stays
`canvas_offset_x`; only a screen-bound segment DP takes its phase from
`set_present_origin`.

## The 1:1 policy

A lenticular weave is a ~1-pixel-period pattern: if the display server resamples
the window on its way to the panel, every view leaks into its neighbour
(#1595, #1831 — refuse rather than resample). Anaglyph, side-by-side and blend do
not care. So the gate is per segment and per DP capability
(`comp_segments_decide`):

| DP for the segment | screen 1:1 | rendered |
|---|---|---|
| none | — | flat 2D |
| needs 1:1 | no | flat 2D |
| needs 1:1 | yes / unknown | woven |
| tolerates a resample | any | woven |

"Unknown never degrades." The capability is `XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE`
in `xrt_dp_scanout_caps::flags`, read through the existing `get_scanout_caps` slot
(the flags word was `reserved[0]`, so every older plug-in reads as "needs 1:1").
The primary segment keeps the session-level gates, which now also let a tolerant
primary DP weave (the X11 gate of #1831 and the resample reasons of the Wayland
gate of #1595). On `ds1-linux` both monitors are resampled under XWayland, which is
why M2 could not be shown there without this.

## Lifecycle

Segment DPs follow the table with hysteresis (`comp_segments_lifecycle`): created
once a segment has existed for 2 consecutive metric updates, destroyed once it has
been gone for 30 (~0.5 s at 60 Hz), so a drag back and forth across a seam never
rebuilds DPs. A failed create is not retried until the segment goes away. Until a
DP exists the segment is flat 2D. A table change is logged once at INFO
(`segments: window … -> N segment(s); [i] screen … canvas … origin …`); a DP create
or destroy is one WARN.

## Scope and switches

- In-process Vulkan compositor, desktop Linux, **X11/XWayland**: root coordinates
  are desktop-absolute, the same space as the registry's RandR rects. Native Wayland
  stays primary-only — the geometry payload does not describe the other monitors
  (`docs/specs/runtime/wayland-window-geometry.md` §5).
- **Mixed vendors** (M3): a Leia DS1 next to a sim_display laptop panel is
  segmented like any other layout; each segment DP comes from its own screen's plug-in
  and the table logs one INFO line naming the vendors. A plug-in whose VK factory was
  refused at load (vk_bundle ABI, #1243) gets no segment DP: that segment is flat 2D.
- Not segmented: zero-copy frames, a self-submitting DP or one without a render
  pass, a session pinned with `XrSessionDisplayBindingDXR`, the shared-texture path.
- `DXR_SEGMENTS=0` turns it off (and keeps `PRIMARY_MULTIVIEW_DXR` at the pre-M3 view count).
- Capture: with the window split, the post-compose atlas capture also writes each
  segment's DP input as `<stem>.seg<i>.png`.

## Windows / D3D11 (M6)

The in-process D3D11 compositor carries the same model in `d3d11/comp_d3d11_segments.{h,cpp}`,
a line-for-line twin of the Vulkan manager with the GPU work in D3D11 terms
(`CopySubresourceRegion` crops, `ClearRenderTargetView`, a renderer rect blit for flat 2D, no
deferred-release list because the immediate context executes in order). What differs on Windows:

- **Two appended ABI slots, no version bump** (ADR-020): `xrt_plugin_iface::create_dp_d3d11_for_screen`
  (the D3D11 twin of the Vulkan per-screen factory) and `xrt_display_processor_d3d11::set_present_origin`.
  A plug-in without them gets flat 2D on its other screens, exactly like Vulkan.
- **The window.** The real HWND, and so the vendor's drag phase-snap, belongs to the DP of the
  screen holding the majority of the window (ADR-047 Amendment 2); every other screen's DP is
  windowless and phases from `set_present_origin` + the canvas offset. It starts on the primary DP.
  `comp_segments_owner_update` (pure, unit-tested) calls a hand-off when another screen beats the
  owner by ≥ 20 % of the on-screen area for 0.5 s; `hwnd_handoff` then makes a windowless
  replacement for the old owner (the primary is swapped through the compositor's `swap_primary`
  hook, which re-sends transparency / mode / eye-tracking mode and guards the exchange with
  `dp_swap_mutex`), destroys the old owner's DP, and makes the new owner's with the HWND — rolling
  back on failure — between two weaves. While the primary is windowless the split path runs even
  for a window back on the primary alone (it needs its present origin) until the hand-back. One
  WARN per flip: `segments: window handle handed from '<dev>' to '<dev>' …` (or `… FAILED …`).
  Only sessions with a real window hand off (not the shared-texture path). The vendor weaver reads
  the current D3D11 viewport **and scissor** and writes only inside them, so the segment path sets
  both to the segment before every `process_atlas`, and the back buffer is re-bound before every DP.
- **Units.** The window rect comes from `ClientToScreen` in a per-monitor-DPI-aware process, i.e.
  device px — the same space as the registry's monitor rects. A DPI-virtualised origin would weave on
  the wrong lattice and look plausible (`docs/reference/dpi-awareness.md`).
- **The untracked screen.** The SR tracker follows the runtime's active display only, so a weaver
  bound to any other display sees no face and would fall back to 2D. The Leia plug-in pins that
  weaver to its nominal viewer and leaves the lens to the active panel (David, 2026-10-07; SR D3/D4
  move both per device later). The runtime does nothing special: the DP owns it.
- **Under the #918 weave-on-scanout split** (ADR-039 — the default on hybrid boxes, and engaged even
  on one adapter) the window is segmented the same way, on the device that presents: the segment
  manager, its DPs and its crop textures live on the OUTPUT device (`d3d11_out_device()` /
  `d3d11_out_context()`, which off the split are the app's), the DPs' input is the egress slot being
  woven (the output-side copy of the composed atlas, already cropped to the box that slot was painted
  at), and the flat-2D fill draws through the output composite unit (`comp_d3d11_outcomp_blit_rect`,
  the renderer's blit re-made on that device). Same function, same table, same per-segment viewport /
  scissor / present origin, same #1863 routing (the mosaic is painted on the app device and crosses the
  bridge like any atlas). A slot one frame behind a resizing window is cropped at its own view size
  against the live canvas — the single-DP split weave's one-frame lag, no more. A split session logs
  one WARN when its segment manager is made (`segments: under the #918 output-device split …`).
- **Not segmented on Windows:** zero-copy frames, a zones / Local2D frame (any non-whole-window
  canvas), the shared-texture path, a pinned session, `DXR_SEGMENTS=0`.
  The service has its own section below (*Service / IPC path*).
- **sim_display** implements both slots on D3D11 too, so a two-monitor Windows box with no vendor
  hardware exercises the split path (anaglyph on both halves).
- **Per-segment views (M3) on Windows** ride the same state-tracker path as Linux: `oxr_system` counts
  the registry's D3D11 factories for the view-set capacity, `xrLocateViews` reads
  `comp_d3d11_compositor_get_segment_metrics` (the last weave's table + each segment's eyes, the
  secondary screen's from its own DP — on a Leia panel the pinned nominal viewer), `xrEndFrame` hands
  the routing to `comp_d3d11_compositor_set_view_routing`, and the renderer paints each segment's
  views at that segment's rect inside every tile (the mosaic; quads and equirect2 once per segment
  with that segment's camera). A routed frame is never zero-copy. This is what makes an untracked
  second panel render from its own viewer instead of the tracked panel's.

## macOS / Metal

The in-process Metal compositor carries the same model in `metal/comp_metal_segments.{h,m}`, a twin
of the D3D11 manager with the GPU work in Metal terms (a clear pass, blit-encoder crops, a small
sub-rect blit pipeline for flat 2D; every DP encodes its own render pass on the frame's one command
buffer, in order). What differs on macOS:

- **Two appended ABI slots, no version bump** (ADR-020): `xrt_plugin_iface::create_dp_metal_for_screen`
  and `xrt_display_processor_metal::set_present_origin`. A plug-in without them gets flat 2D on its
  other displays. What a plug-in implements is spelled out in `docs/reference/xrt_plugin_iface.md`.
- **Units — the macOS-specific part.** The registry holds each display's `CGDisplayBounds` in
  top-down **points** plus its native backing px, and the window's content view is read in the same
  points (AppKit's bottom-up frame flipped against the main display). The table is cut in points,
  so a seam is exact whatever the scales, then converted per segment:
  - the **canvas** (`window_rect`) → the drawable's px (points × the drawable's scale, edges rounded
    independently so neighbours share the seam column);
  - the **present origin** → that display's own backing px relative to its `CGDisplayBounds`
    origin, chosen so `origin + canvas offset` = the segment's top-left on the panel even when the
    window's backing scale differs from the display's. That is exactly what the Leia SR Metal
    weaver's `srWeaverSetPresentOrigin` takes, so a per-screen DP forwards it and never recomputes
    it from a window;
  - **1:1** → the display's backing scale equals the drawable's AND its points × scale equal its
    native mode. A "looks like" scaled mode (the WindowServer downsamples the 2x backing) or a
    window straddling a 1x and a 2x display (the window has ONE backing scale; the other display
    gets it resampled) is not 1:1, and a lenticular weave is refused there (flat 2D);
  - the **published M3 metrics** → points × the drawable's scale throughout: a window pixel on
    display *i* covers 1/scale points of it, so the per-pixel physical pitch the view math uses is
    right on every display.
- **The window.** The session's DP keeps the app's NSView (it is the system-default display's DP).
  Every other display's DP is windowless.
- **Not segmented on macOS:** the shared-IOSurface (`_texture`) path, a zones / Local2D / mask frame
  or an output rect (any non-whole-window canvas), a session with no DP, a pinned session,
  `DXR_SEGMENTS=0`, the service / IPC path. Unlike D3D11, a zero-copy frame IS segmented (the crop
  reads the app's swapchain at the same tile stride); a routed (M3) frame is never zero-copy.
- **Per-segment views (M3)** ride the same state-tracker path: `oxr_system` counts the registry's
  Metal factories for the view-set capacity, `xrLocateViews` reads
  `comp_metal_compositor_get_segment_metrics`, `xrEndFrame` hands the routing to
  `comp_metal_compositor_set_view_routing`, and the projection pass paints each segment's views at
  its rect inside every tile. Quads and equirect layers are NOT routed on Metal yet (drawn once per
  tile with the tile's camera, as unsegmented).
- **sim_display** implements both slots on Metal (a bound DP loads the target and scissors to its
  canvas; every Metal sim output tolerates a resample, since INTERLACED falls back to anaglyph on
  this backend, #817), so a two-display Mac with no vendor hardware exercises the split path.
- **"Displays have separate Spaces" clips a straddling window.** With that macOS setting ON (the
  default: `com.apple.spaces` `spans-displays` absent or false) the WindowServer shows a window
  spanning displays only on the display holding most of it; the other segments are rendered and
  woven but never reach their panel. The runtime logs a one-shot WARN the first time a window
  resolves to ≥ 2 segments under it (no behaviour change); turn it off in System Settings → Desktop
  & Dock → Mission Control (requires logout). `DXR_TEST_SEPARATE_SPACES=0|1` overrides the read.
- Test knob: `cube_handle_metal_macos` honours `DXR_TEST_WINDOW_RECT=x,y,w,h` (content rect, top-down
  points) and, once at frame `DXR_TEST_WINDOW_MOVE_FRAME` (default 300), `DXR_TEST_WINDOW_RECT2`.

## Per-segment views (M3)

Under `PRIMARY_MULTIVIEW_DXR` each segment gets its **own** views instead of a crop of
one shared set (contract: `docs/reference/view-configuration-model.md` § *Per-segment
views*; API: `XrViewDisplayBindingsDXR` in `XR_DXR_display_info` v22).

1. **Publish.** Each weave that takes the split path publishes its table as
   `xrt_segment_metrics` (`comp_vk_native_compositor_get_segment_metrics`): per segment
   the screen id, rect in window and screen px, the screen's desktop rect, physical
   size and nominal viewer, and whether it is woven. Eyes are predicted at query time
   — the primary from the session DP, the others from their segment DPs
   (`comp_vk_native_segments_get_eyes`, guarded against the weave destroying them).
   Nothing is published while the session is collapsed to flat 2D (#1595/#1831), for
   zero-copy frames or when the window is on the primary screen only.
2. **Locate.** `oxr_session_locate_views` locates one view set per segment and records
   which views went where (`xrt_segment_view_routing`); `xrEndFrame` hands it to the
   compositor (`comp_vk_native_compositor_set_view_routing`) with the frame.
3. **Route — one atlas, per-segment tile ranges, as a mosaic.** The atlas layout does
   not change: `cols × rows` tiles of `canvas × scale`, so `u_tiling` and the
   worst-case sizing stay honest and a single-segment frame is byte-for-byte the old
   one. What changes is what a tile holds: local view `j` of segment `k` is placed at
   segment `k`'s rect inside tile `j` (`comp_vk_native_eff_layout::route`, both the
   blit and the compose paths). The rect comes from `comp_segments_tile_rect`, the
   mapping the crop (step 2 of *The frame*) reads with, so cropping segment `k` out of
   every tile yields exactly its own views — the M2 split path runs unchanged and each
   DP weaves its own frustum. A routed frame never zero-copies (the mosaic is built by
   the renderer, not by the app). Chosen over per-segment sub-atlases because it keeps
   one atlas, one crop path, one capture path.
4. **Capture.** `displayxr_atlas.seg<i>.png` is each segment's DP input, i.e. that
   segment's own views.

What stays one view set (cropped per segment, as in M2): `PRIMARY_STEREO`/`MONO`
sessions (framed from the screen holding most of the window), camera-rig and
zone-scoped locates, a window covering more than `XRT_MAX_SEGMENTS` (2) screens. A
frame whose routing no longer matches (the mode changed between the locate and the
commit) is drawn unrouted for that frame. Under per-segment views, canvas that lies on
no screen is not covered by any view set and stays black (M2 painted it from the shared
set). Quads and equirect2 layers are drawn once per segment in each tile, with that
segment's view camera, viewport and scissor confined to the segment's rect (the compose
pass; the blit fallback draws no quads at all).

**IPC.** See *Service / IPC path* below: the D3D11 service publishes the same table for
a direct client, and the client frames per-segment views from it exactly as above.

## Service / IPC path (Windows, ADR-047 Amendment 2)

An `_ipc` client (`XRT_FORCE_MODE=ipc`, or any app under a running service) is composited by
`displayxr-service`. The same segment manager weaves its window there:

- **Opt-in.** At session create the client sends `compositor_segments_enable(pin)` — only when
  its system's set capacity is above 1, i.e. the service's registry has two screens with a
  D3D11 DP factory (`oxr_system` counts the by-value copy of the service's registry; the
  pointers are only NULL-tested). The service answers with its OWN screen list (its instance,
  the one `xrEnumerateDisplaysDXR` reports) and keeps a heap copy per client. A single-screen
  box never sends it; a client that did not send it is never segmented.
- **Where it weaves.** Only the direct single-client pipeline (`pipeline_default_policy_render`)
  for an `APP_HWND` presenter — the app's own window, the only kind that can sit across a seam.
  `pipeline_segments_weave` replaces that frame's single `process_atlas`: the client's manager
  (`comp_d3d11_segments`, linked from the `comp_d3d11_segs` library), made on `svc_out_device`
  (the output device under the #918 split), with the panel DP — bound to the app's HWND — as
  the primary and the client's crop (or the egress slot) as the atlas. The window rect is the
  app HWND's client area from `ClientToScreen` in the per-monitor-DPI-aware service, i.e.
  device px. One client's segment DPs exist at a time: a focus change releases the outgoing
  presenter's, and a table older than 250 ms (the client no longer weaves directly — focus
  moved, the workspace took over) is not handed out, so that client locates one view set. A hosted client presents into the service's window on the panel, a
  present-owner / client-texture client weaves on its own thread and a zones frame keeps the
  single DP: none is segmented.
- **Compose / shell mode** (`multi_compositor_render`) is unchanged: one presenter, the
  service window on the panel, so a workspace window is never segmented and its table stays
  empty. Per-screen workspace presenters are a separate decision.
- **Per-segment views.** `xrLocateViews` fetches the table with `compositor_get_segment_metrics`
  (a round trip per locate, only for an opted-in session): the service fills each segment's
  eyes at query time — the primary from the panel DP, the others from their segment DPs. The
  client then runs the in-process locate unchanged, with two IPC-only differences: the
  segment's client-side Kooima FOVs are kept over the device's single-set answer, and a
  segment locate never takes the server rig call (`locate_views_rig`), which knows one view
  set. `xrEndFrame` sends the routing with `compositor_set_view_routing`, change-only (the
  service keeps the last one). The service lays a routed frame into the client's atlas as the
  mosaic (`comp_segments_route_place`, `util/comp_segments_route.h`, unit-tested by
  `tests/tests_segments_ipc.cpp`); a routed frame never zero-copies.
- **Not yet on the service path:** quads / equirect / cylinder layers in a routed frame are
  drawn once per tile with that tile's camera (in-process draws them per segment), and a
  second projection layer is routed only where the per-tile pass visits it (segment 0).

