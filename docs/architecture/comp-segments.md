# Window segments: one window, one DP per screen

*How the compositor weaves a window that spans several screens (multi-screen M2).
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
  woven by that DP, which also keeps owning everything view-related: eye positions,
  window metrics, Kooima. M2 renders **one view pair** for the whole window; per-segment
  views are M3.
- Every **other screen** gets a segment DP from that screen's registry factory:
  `xrt_plugin_iface::create_dp_vk_for_screen` with an `xrt_screen_binding`
  (monitor id, desktop rect, native px, mm, serial) — only that slot: a plug-in
  without it gets a flat 2D view on its other screens, never a plain `create_dp_vk`
  DP (which would describe the wrong panel and may clear the whole target). It is
  created against the #868 runtime-owned queue, like the primary.
  It is windowless (NULL window): its phase is `set_present_origin`, fed per frame,
  ADR-033. A plug-in's DP made for a screen describes that screen
  (`get_display_dimensions` / `get_display_pixel_info`).
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
- **One vendor**: all screens must belong to the primary screen's plug-in. A
  mixed layout (a Leia DS1 next to a sim_display laptop panel) keeps the single-DP
  path; mixing vendors is M4.
- Not segmented: zero-copy frames, a self-submitting DP or one without a render
  pass, a session pinned with `XrSessionDisplayBindingDXR`, the shared-texture path.
- `DXR_SEGMENTS=0` turns it off.
- Capture: with the window split, the post-compose atlas capture also writes each
  segment's DP input as `<stem>.seg<i>.png`.

## What M3 adds

Per-segment **views**: `xrLocateViews` returns a view pair per segment (eyes from
that segment's DP, Kooima from the segment rect relative to its own screen),
`XrViewDisplayBindingsDXR` tells the app which views belong to which segment, and the
compositor routes each segment's tiles to its DP instead of cropping one shared
pair. The segment table, the lifecycle and the per-segment DPs here are what it
builds on.
