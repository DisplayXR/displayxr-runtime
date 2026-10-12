# XR_DXR_cursor_depth

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_cursor_depth` |
| Spec Version | 7 |
| Type Values | `XR_TYPE_CURSOR_DEPTH_HINT_DXR` (1004999320) · `XR_TYPE_CURSOR_DEPTH_PLACEMENT_DXR` (1004999321) · `XR_TYPE_CURSOR_DEPTH_SOURCE_DXR` (1004999322, v2) · `XR_TYPE_CURSOR_DEPTH_OPTIONS_DXR` (1004999323, v3) · `XR_TYPE_CURSOR_DEPTH_DRAW_INFO_DXR` (1004999324, v5) · `XR_TYPE_CURSOR_DEPTH_DRAW_STATE_DXR` (1004999325, v5) · `XR_TYPE_CURSOR_DEPTH_LIFT_RECT_DXR` (1004999326, v6) · `XR_TYPE_CURSOR_DEPTH_LIFT_STATE_DXR` (1004999327, v6) |
| Author | The DisplayXR Project |
| Platform | All. Works on every graphics API, in-process and over IPC; solved in the state tracker. |
| Decision record | [ADR-046](../../adr/ADR-046-depth-aware-cursor.md) |

---

## 1. Overview

The OS cursor is drawn on the display plane, at zero disparity. Over content that pops out of the
glass, that is a **depth violation**: occlusion says the cursor is in front, and disparity says
it is behind. `XR_DXR_cursor_depth` lifts the cursor off the plane. The app reports the nearest
content under the cursor. The runtime returns a pose and size for a cursor sprite just in front
of that content, smoothed over time. The app draws the sprite. Content on or behind the plane
causes no conflict, so there the cursor stays the OS cursor (v4: never behind the glass).

> The **app** owns the depth (it knows its scene). The **runtime** owns placement and policy
> (where, how big, how it moves), so every app's cursor behaves the same. The **app** draws.

### Opt-in, zero cost otherwise

Enabling the extension costs nothing. The runtime does cursor work only on an `xrLocateViews`
call that chains an `XrCursorDepthHintDXR`, and it never touches the OS cursor. Later spec
versions keep this rule: any runtime GPU cost (drawing, measuring) will be a separate struct that
the app explicitly chains.

Spec v2's depth-layer source (§2.3) is the first such struct. Its GPU cost (one copy of a
cursor-sized patch of depth) is paid **only on frames whose hint chains
`XrCursorDepthSourceDXR` with `XR_CURSOR_DEPTH_SOURCE_SUBMITTED_DEPTH_DXR`**. Every other app,
including one that submits depth for other reasons, gets no extra copy, readback, fence,
allocation or dispatch. Its `xrEndFrame` pays a single bool test.

## 2. Structures

Both structures travel through **one** `xrLocateViews` call:

- the hint goes on `XrViewLocateInfo::next`;
- the placement goes on `XrViewState::next`.

### 2.1 `XrCursorDepthHintDXR` (input)

| Field | Meaning |
|---|---|
| `cursorUV` | Cursor hotspot, **canvas-normalised**: [0,1]², origin top-left, v down. The canvas is the one this locate's views frame: the window client area or texture sub-rect, or the zone rect for a zone-scoped locate. If it falls outside [0,1], the placement is inactive. |
| `hasContent` | `XR_FALSE` means nothing of the app's 3D content lies under the cursor footprint, and the cursor settles onto the display plane. |
| `nearestPoint` | The content point under the cursor **footprint** that is nearest the viewer. It must be in the same frame as the view poses this call returns, which is normally `XrViewLocateInfo::space`. |
| `cursorHeight` | Sprite height as a fraction of canvas height, as seen by the viewer. A value ≤ 0 selects 0.03. |

Chain the hint on **at most one locate per frame**. With display zones, that is the locate for
the zone the cursor is over. A malformed hint is never an error; it yields an inactive placement.

**Footprint, not the hotspot.** Take the nearest point over the whole sprite area, plus a small
dilation. If you use only the hotspot pixel, a nearer edge right next to it cuts through the
sprite: the violation returns at every silhouette.

### 2.2 `XrCursorDepthPlacementDXR` (output)

| Field | Meaning |
|---|---|
| `isActive` | `XR_FALSE` means draw nothing and show the OS cursor. This happens when no hint was chained, fewer than two views are active (2D), the cursor is off the canvas, or the view geometry is degenerate, and (spec v4) whenever the filtered disparity is above the draw threshold, −0.005: on the plane, the OS cursor is the cursor. `pose` is then identity and `height` 0. `disparity` and `targetDisparity` are still filled whenever the geometry was valid, as diagnostics. |
| `pose` | The sprite centre and orientation in the locate space. The orientation is the display plane's: draw the quad in its local XY plane, facing +Z. The position follows the anchor mode (§2.4). With the default (HYBRID) it lies on the line from the viewer through the cursor's point on the canvas while the pointer moves, and stays fixed in the locate space while the pointer is still. |
| `height` | Sprite height in locate-space units, scaled with depth so the apparent size is constant. |
| `disparity` | The filtered disparity, in eye-baseline units: 0 is the display plane and < 0 is in front. Never > 0 since v4. Diagnostic only. |
| `targetDisparity` | The unfiltered target: the content disparity minus the margin, clamped to [−0.6, 0]; 0 when the content is on or behind the plane. Diagnostic only. |

### 2.3 `XrCursorDepthSourceDXR` (input, spec v2)

Chained on `XrCursorDepthHintDXR::next`.

| Field | Meaning |
|---|---|
| `source` | `XR_CURSOR_DEPTH_SOURCE_APP_POINT_DXR` (0): v1 behaviour; the same as not chaining the struct. `XR_CURSOR_DEPTH_SOURCE_SUBMITTED_DEPTH_DXR` (1): the runtime finds the nearest content itself, in the depth the app submits with its projection layer. `XR_CURSOR_DEPTH_SOURCE_MEASURED_DISPARITY_DXR` (2, spec v7): the runtime measures the disparity under the cursor from the submitted views. |

With `SUBMITTED_DEPTH`:

- The hint's `hasContent` and `nearestPoint` are **ignored**. `cursorUV` still places the
  cursor, and `cursorHeight` still sizes the sprite and the footprint the runtime searches (the
  sprite plus 50%, as §2.1 recommends).
- The request covers **one frame**. Chain the struct on every frame's hint for as long as you
  want the service.
- The answer is asynchronous. At that frame's `xrEndFrame`, the compositor copies a cursor-sized
  patch (at most 64×64 texels) of depth from the **outermost two views** of the first projection
  layer that carries depth, without any GPU wait. The read reaches the placement one or two frames
  later. The time filter absorbs that, as it does a v1 hint from last frame's readback.

**Requirements.** Each of these, when missing, reports "no content": the cursor settles onto the
display plane, and the runtime logs a one-time warning. None is ever an error.

1. `XR_KHR_composition_layer_depth` is enabled, and every view of the projection layer chains an
   `XrCompositionLayerDepthInfoKHR`. Zone-3D layers have no depth variant, so they don't qualify.
   The runtime advertises `XR_KHR_composition_layer_depth` only when it is built with
   `XRT_FEATURE_OPENXR_LAYER_DEPTH=ON`, which is OFF by default (see ADR-046 §6). Check for the
   extension before relying on this source.
2. The projection layer's `space` is the **same `XrSpace`** as `XrViewLocateInfo::space`. The
   runtime rebuilds the point from the layer's own view poses and fovs. It does not convert
   between two app spaces.
3. The session's compositor can read submitted depth. Today that is the in-process **Metal**
   compositor. The Vulkan, D3D11, D3D12 and GL compositors, and the IPC/service path, report no
   content until they are wired.

With `MEASURED_DISPARITY` (spec v7, ADR-046 Amendment 3) — for stereo content that has **no
depth**: SBS / stereo video, stereo photos, camera pairs, legacy stereo apps:

- The hint's `hasContent` and `nearestPoint` are **ignored**; `cursorUV` and `cursorHeight` apply
  as above. One frame per request, as with `SUBMITTED_DEPTH`.
- At that frame's `xrEndFrame` the compositor copies a cursor-sized patch (the cursor footprint
  dilated by the search range, at most ≈ 64 + 2×65 work px wide, box-downsampled so a 4K-SBS view
  is matched at ≤ 960 px per view) out of the **two outermost views** of the frame's first
  projection layer. The copy is read back one or two frames later, never waited on, and
  block-matched on the CPU with the web stereo-tile cursor's matcher (`displayxr-web`
  `js/camera/disparity.js`, the same constants): zero-mean NCC, ±64 work px of disparity,
  ±2 rows, a parabolic sub-pixel fit.
- A match counts only if it is **confident**: the block has texture (luma std ≥ 6), the best NCC
  is ≥ 0.8, it beats every rival peak ≥ 3 px away by 0.08 (a repetitive pattern fails), and the
  **left-right check** passes (the matched block matched back lands within 1 px). The content is
  the nearest confident match over the cursor footprint (the tip plus a ring of 9 points around
  the arrow's body). No confident match — blue sky, a flat wall, stripes — means no content: the
  cursor stays on the display plane, i.e. the plain OS cursor.
- The measured on-screen disparity is converted with the located views (a point at disparity d
  shows on the canvas at baseline × d) and the §3 policy applies unchanged: never behind the glass.
- A frame whose patch is bit-identical to the last measured one at the same pointer (a paused
  video) reuses the last answer instead of matching again.

Requirements (each, when missing, reports "no content", never an error): the projection layer's
views frame the canvas the locate framed; the two outer views' sub-images are the same size; a
colour format the matcher reads (8-bit RGBA/BGRA, RGB10A2, RGBA16F); not a display-zones frame;
an in-process native compositor (Metal, Vulkan, GL, D3D11, D3D12). The IPC/service path reports
no content until it is wired.

**Depth conventions.** The runtime turns a depth value into a distance as follows:

- 1/z is linear in the window depth. It is `1/nearZ` at `minDepth` and `1/farZ` at `maxDepth`.
  That covers D3D, Vulkan and Metal [0,1] clip depth, and GL [−1,1] NDC under the default depth
  range.
- Reversed Z is supported. A layer marks it by giving a `nearZ` greater than its `farZ`.
- An infinite `farZ`, or for reversed Z an infinite `nearZ`, is supported.
- The far end of the mapping is the cleared background. It counts as no content.
- The nearest texel in the footprint wins, across both outer views.

### 2.4 `XrCursorDepthOptionsDXR` (input, spec v3)

Chained on `XrCursorDepthHintDXR::next`, in any order with `XrCursorDepthSourceDXR`. Optional:
**absent means HYBRID anchoring and the default margin.** It is read only behind the hint, so it
costs nothing on its own.

| Field | Meaning |
|---|---|
| `anchorMode` | Where the sprite goes laterally as the viewer's head moves. `XR_CURSOR_DEPTH_ANCHOR_MODE_HYBRID_DXR` (0, the default), `_SCREEN_DXR` (1), `_WORLD_DXR` (2). An unknown value behaves as HYBRID. |
| `margin` | How far in front of the content the cursor floats, as a disparity in eye-baseline units. ≤ 0 or non-finite selects the runtime default, 0.015 (v3 used 0.005; v4–v6 used 0.03 until 2026-10-11). |

**Anchor modes.** E is the cyclopean eye, S the canvas point under the pointer, f the unit display
normal pointing away from the viewer, e = dot(S − E, f), d the filtered disparity and
t = 1/(1 − d). The sprite's centre C is:

| Mode | C | On a head-tracked display |
|---|---|---|
| `SCREEN` | E + t·(S − E): on the cyclopean ray | The image on the glass ignores head motion. The cursor has disparity but **no motion parallax**, so while everything else parallaxes it reads as "at the glass". The v1/v2 behaviour. |
| `WORLD` | S − f·(1 − t)·e: straight in front of S along the normal | Parallaxes like content at its depth. Off-axis, it sits a millimetre or two beside the click point even while the pointer moves. |
| `HYBRID` | While the pointer **moves**: the SCREEN position, and the runtime stores that locate's line of sight: E₀ = E, S₀ = S, D₀ = e. While the pointer is **still**: C = E₀ + s·(S₀ − E₀) with s = 1 − (1 − t)·e / D₀, the point on the anchor-time line whose distance in front of the glass equals the current depth (1 − t)·e. | Exact aim while moving. While still: world-fixed under lateral head motion (correct parallax, s unchanged); a depth change slides C along the anchor-time line, so it stays on the click point for a viewer who hasn't moved; continuous on the frame the pointer stops (s = t). The only drift from the click point is from head motion while the pointer is still: about \|d\| × the head's lateral displacement, so 6 mm for 20 cm at d = −0.03 (content just in front of the glass). |

"Moves" means `cursorUV` changed by more than 1e-4 in u or v since the last moving placement.
HYBRID also re-anchors (takes the SCREEN position) on the first hinted locate and whenever the
time filter re-primes (a gap > 0.5 s, or time going backwards), whenever the **canvas plane
moves** in the locate space (S₀ more than 0.1 % of e off the current canvas plane, or the canvas
normal turns: a camera rig re-converging, a rig re-pose; a head move leaves the plane in place), and
whenever s ≤ 0 (C would be on or behind the eye). The anchor-time line of sight belongs to the canvas
it was taken against: under a still pointer EarthView's camera rig re-converged from e = 1 to 3.42,
s = 1 − 0.375 × 3.42 went negative and C sat behind the eye. The height
(`cursorHeight` × canvas height × t) and the orientation are the same in every mode. Hit testing
is unchanged: the click stays at the pointer's canvas point; the anchor mode only changes where
the sprite is drawn.

The same rule, with the same numbers, is implemented by the web SDK's depth cursor.

### 2.5 `XrCursorDepthDrawInfoDXR` (input, spec v5) — "runtime, draw the cursor"

Chained on `XrCursorDepthHintDXR::next`, in any order with the other hint structs. Chaining it
**is** the per-frame draw request (ADR-046 Amendment 2 B); a frame without it costs the runtime
nothing. The app then neither draws a sprite nor needs `XrCursorDepthPlacementDXR` (it may still
chain it, for diagnostics).

| Field | Meaning |
|---|---|
| `image` | An ordinary color swapchain this session created with `xrCreateSwapchain`, in the app's own graphics API, filled with any sprite and released at least once. The runtime samples its **last released image** (array layer 0, whole image) as **straight (non-premultiplied) alpha**, decoded per its format (an `_SRGB` format decodes). It must stay alive until the `xrEndFrame` of every frame that names it. On OpenGL the image's top is its **last** row (GL's bottom-left origin, which is how a GL app renders into it); upload a top-down bitmap bottom row first. `XR_NULL_HANDLE` = the runtime's **default look**: the current OS cursor's shape where the runtime can read it, else the classic arrow (12 × 19, white fill, dark outline, tip = hotspot). |
| `hotSpot` | The click point in the sprite, normalised, origin top-left, v down. Placed at the cursor. Ignored for the default look (it brings its own). |
| `sizeMeters` | The sprite's **height** on the panel at the display plane, metres; width follows the image aspect. Scaled with depth, so its apparent size never changes. ≤ 0 or non-finite = the OS cursor's size (the default look's own size; 19 OS points for a custom sprite). |
| `visible` | `XR_FALSE`: draw nothing. |

**Where it is drawn.** After the app's layers (projection, quad, window-space) and before the
display processor, one sprite per view tile, depth test off, never occluded, alpha-blended.
Each view's copy is projected with **that view's submitted pose and fov from the frame's first
projection layer**, so the projection layer must be in the **same `XrSpace`** as the locate
(otherwise nothing is drawn and later locates report `willDraw = XR_FALSE` until a frame's layer
is in the locate space again).

**Display zones.** One cursor per frame, in the 3D zone whose zone-scoped locate carried the
hint (the zone under the pointer; `cursorUV` is relative to that zone). It is projected with
**the views of the projection layer chained with that zone's `XrDisplayZoneDXR` (same
`zoneId`)**, drawn only inside that zone's rect as submitted, never into a 2D zone or another
3D zone. If the frame has no 3D zone with that `zoneId`, the hint did not ride a zone-scoped
locate in a zones frame, or a zone-scoped hint arrives in a frame without zones, nothing is
drawn and later locates report `willDraw = XR_FALSE` (the fallback above).

### 2.6 `XrCursorDepthDrawStateDXR` (output, spec v5)

Chained on `XrViewState::next`; filled on every locate that chains it. `willDraw` is `XR_TRUE`
only when this locate chained a hint carrying a visible `XrCursorDepthDrawInfoDXR`, the placement
is active (the §3 policy: content pops out under the cursor), and the session's compositor can
draw it, and the placement **maps to cursor pixels in this locate's views**: in front of every
view's eye, a finite, non-degenerate sprite size, and the hotspot inside at least one view's tile
(the zone, for a zone-scoped locate). `xrEndFrame` applies the same test to the submitted views; a
frame that fails it draws nothing and the next locate reports `XR_FALSE`.
**Hide the OS cursor over the canvas exactly while `willDraw` is `XR_TRUE`.** Whenever
the runtime cannot draw (another backend, a space mismatch, a zones frame without the hinted zone), it reports
`XR_FALSE`, so the fallback is always the flat OS cursor, never no cursor. The runtime never
touches the OS cursor itself.

**Backends (v5).** The in-process Metal, Vulkan (vk_native), OpenGL, D3D11 and D3D12 compositors
draw. The IPC/service path reports `willDraw = XR_FALSE` with a one-time WARN (ADR-046
Amendment 2 rollout step 3). On Vulkan the cursor is drawn after the projection, quad and
equirect2 layers but **under** window-space layers, which that compositor stamps in a later pass
(ADR-046 Amendment 2, "§B as built on Vulkan and GL"). The default look mirrors the OS cursor on
macOS and Windows (on Windows `GetCursorInfo`, or - while the app has hidden it - the class
cursor of the window under the pointer, drawn at the size the OS draws it on the panel);
elsewhere it is the classic arrow.

### 2.7 `XrCursorDepthLiftRectDXR` / `XrCursorDepthLiftStateDXR` (spec v6) — lifted content

For a 2D element lifted on a weave rect (`XR_DXR_lift`, `XrWeaveRectLiftDXR`). No app holds a
lifted rect's views and the weave path never calls `xrLocateViews`, so these are separate from
§2.5/§2.6 (ADR-046 Amendment 1).

- **`XrCursorDepthLiftRectDXR`** (input), chained on `XrWeaveRectLiftDXR::next`: draw the depth
  cursor into this rect's lifted views. Per submit: chain it on every submit for as long as you
  want it. `flags` is reserved (0). The lift stream must be created with `XrLiftDepthRequestDXR`.
- **`XrCursorDepthLiftStateDXR`** (output), chained on `XrWeaveOutputDXR::next`: `drawnLiftMask`,
  bit i = `lifts[i]` of this submit got the cursor in the output just woven. Hide the OS cursor
  over a lifted rect only while its bit is set.

The runtime reads the OS pointer itself, places the sprite with the §3 policy from the
conversion's own depth map (through the module's per-conversion relief mapping), and draws one
sprite per view before the display processor. It reports "not drawn" whenever it cannot place it
(no depth or relief mapping, pointer off the rect, on the screen plane, a workspace cursor
shown), so the fallback is always the flat OS cursor. Anchor: SCREEN today; HYBRID follows.
Backend: the D3D11 service (the only lift weave path today).

## 3. Runtime behaviour

The canvas point under the cursor is triangulated from the outermost pair of **located views**.
Every policy number is applied in disparity d = 1 − 1/t, where t is depth relative to the canvas
along the display normal measured from the cyclopean eye:

- **never behind the glass** (v4, ADR-046 Amendment 2 A.1): target = d < 0 ? max(d − margin, −0.6) : 0.
  Content on or behind the display plane leaves the cursor on the plane;
- margin: 0.015 baseline in front of the content, about 1 mm of crossed on-screen disparity at a 63–65 mm IPD (overridable with `XrCursorDepthOptionsDXR::margin`);
- lateral anchor: HYBRID unless `XrCursorDepthOptionsDXR` selects another mode (§2.4);
- clamp: d ∈ [−0.6, 0];
- draw threshold (v4, Amendment 2 A.3): `isActive` only while the filtered d ≤ −0.005 (about 0.3 mm
  on screen at a 65 mm IPD). The filter keeps running while inactive, so the drawn cursor hands
  back to the OS cursor smoothly and returns without a jump. The ADR's additional 0.5-panel-pixel
  floor is a drawing-backend concern (the placement does not know the pixel pitch); −0.005 is
  above 0.5 px on every current panel;
- rising toward the viewer: 30 ms time constant;
- sinking away: 250 ms time constant;
- a gap longer than 0.5 s snaps the filter;
- a second locate with the same `displayTime` does not advance the filter.

The full rationale is in ADR-046 §2–3. The implementation is `src/xrt/auxiliary/util/u_cursor_depth.{h,c}`.

## 4. App usage

```c
// Once: enable "XR_DXR_cursor_depth" at xrCreateInstance.

// Per frame, before rendering:
XrCursorDepthHintDXR hint = {XR_TYPE_CURSOR_DEPTH_HINT_DXR};
hint.cursorUV     = (XrVector2f){mouse_x / canvas_w, mouse_y / canvas_h};
hint.hasContent   = last_frame_hit.valid;          // from LAST frame's depth readback / raycast
hint.nearestPoint = last_frame_hit.point;          // world point, same space as the locate
hint.cursorHeight = 0.03f;

XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO, &hint};
locate.viewConfigurationType = viewConfig;
locate.displayTime = frameState.predictedDisplayTime;
locate.space = appSpace;

// Optional (v3): keep the v1/v2 screen-anchored placement instead of HYBRID.
// XrCursorDepthOptionsDXR opts = {XR_TYPE_CURSOR_DEPTH_OPTIONS_DXR};
// opts.anchorMode = XR_CURSOR_DEPTH_ANCHOR_MODE_SCREEN_DXR;
// opts.margin = 0.0f;                             // runtime default
// hint.next = &opts;

XrCursorDepthPlacementDXR cursor = {XR_TYPE_CURSOR_DEPTH_PLACEMENT_DXR};
XrViewState viewState = {XR_TYPE_VIEW_STATE, &cursor};
xrLocateViews(session, &locate, &viewState, viewCount, &viewCount, views);

// ... render the scene into every view ...

if (cursor.isActive) {
    hide_os_cursor_over_canvas();
    // Last, depth test OFF: a quad of size (height * aspect) x height at cursor.pose.
    draw_cursor_sprite(cursor.pose, cursor.height);
} else {
    show_os_cursor();
}

// Then refresh last_frame_hit for the next frame: nearest content point within
// the sprite's footprint around the mouse (depth readback without a stall, or a raycast).
```

To get the nearest point within the footprint from a depth buffer:

1. Read a small box of depth texels around the cursor, about 1.5× the sprite size, from one view
   (or the cyclopean view).
2. Take the minimum view-space depth.
3. Unproject that texel to a world point.

For splats, use the renderer's alpha-weighted expected depth, not a raycast.

**Runtime-drawn cursor (v5), the recommended adoption.** Same hint, plus the draw request; no
sprite drawing in the app:

```c
XrCursorDepthDrawInfoDXR draw = {XR_TYPE_CURSOR_DEPTH_DRAW_INFO_DXR};
draw.image      = XR_NULL_HANDLE;   // runtime default look (or your own sprite swapchain)
draw.hotSpot    = (XrVector2f){0.5f, 0.5f};   // ignored for the default look
draw.sizeMeters = 0.0f;             // the OS cursor's size
draw.visible    = XR_TRUE;
hint.next = &draw;

XrCursorDepthDrawStateDXR drawState = {XR_TYPE_CURSOR_DEPTH_DRAW_STATE_DXR};
XrViewState viewState = {XR_TYPE_VIEW_STATE, &drawState};
xrLocateViews(session, &locate, &viewState, viewCount, &viewCount, views);

if (drawState.willDraw) hide_os_cursor_over_canvas(); else show_os_cursor();
// Submit the projection layer in the same XrSpace as locate.space.
```

Reference: `test_apps/handle/cube_handle_metal_macos` (`DISPLAYXR_CURSOR_DEPTH_DRAW=runtime|app`,
`DISPLAYXR_CURSOR_DEPTH_SPRITE=custom`).

## 5. Version history

| Version | Change |
|---|---|
| 1 | Initial: app hint in, placement out, app draws (ADR-046 Phase 1). |
| 2 | `XrCursorDepthSourceDXR` (1004999322): opt-in depth-layer source. The runtime reads the nearest content from the depth submitted with the projection layer (ADR-046 Phase 3a). Metal compositor first. |
| 3 | `XrCursorDepthOptionsDXR` (1004999323): anchor mode (HYBRID / SCREEN / WORLD) and margin. HYBRID anchors the line of sight at the last pointer move (§2.4). **Defaults change, deliberately:** the default anchor is now HYBRID (was effectively SCREEN), and the default margin is 0.005 (was 0.03). Both follow testing on a real head-tracked panel: the screen-anchored cursor had disparity but no motion parallax and read as "at the glass", and 0.03 floated it a visibly detached 1–2 cm off the content, where 0.003–0.005 rests on it. An app that wants the v2 placement chains the options struct with `XR_CURSOR_DEPTH_ANCHOR_MODE_SCREEN_DXR` and `margin = 0.03`. |
| 4 | No new structs; **behaviour changes** (ADR-046 Amendment 2 A, one cursor policy everywhere). **Never behind the glass:** target = d < 0 ? max(d − margin, −0.6) : 0, so content on or behind the display plane leaves the cursor on the plane (v1–v3 let it sink onto content behind the glass, up to +0.6). **Margin back to 0.03** (the reference app's value since Phase 1). **Draw threshold:** `isActive` is now also `XR_FALSE` while the filtered disparity is above −0.005, which is the existing meaning of `isActive = XR_FALSE` (draw nothing, show the OS cursor); `disparity` / `targetDisparity` stay filled as diagnostics. HYBRID stays the default anchor. |
| 5 | **The runtime draws** (ADR-046 Amendment 2 B). New structs only: `XrCursorDepthDrawInfoDXR` (1004999324, on the hint: the sprite swapchain or `XR_NULL_HANDLE` for the default look, hotspot, size in metres, visibility) and `XrCursorDepthDrawStateDXR` (1004999325, on `XrViewState`: `willDraw`, the app's cue to hide its OS cursor). Drawn after the app's layers and before the display processor, per view, with the projection layer's own views (same-space rule). Default look: the current OS cursor shape, else the classic arrow. Metal (in-process) first; other backends report `willDraw = XR_FALSE`. App-drawn placement (v1–v4) unchanged. |
| 6 | **Lifted content** (ADR-046 Amendment 1, runtime#1907). New structs only: `XrCursorDepthLiftRectDXR` (1004999326, on `XrWeaveRectLiftDXR`: draw the cursor into this lifted rect's views) and `XrCursorDepthLiftStateDXR` (1004999327, on `XrWeaveOutputDXR`: `drawnLiftMask`). Placed from the lift conversion's depth map through the module's relief mapping (`xrt_dp_lift_depth::relief_*`), §3 policy, D3D11 service. §2.7. |
| 6 (2026-10-11) | No version bump, no struct change; **default margin halved, 0.03 → 0.015** (ADR-046 Amendment 2 A.2) after the Windows Leia panel check: 0.03 (≈2 mm crossed) floated visibly off the content, 0.015 is ≈1 mm. The margin is a default, not a contract (`XrCursorDepthOptionsDXR::margin` still overrides it), so apps need no change; one that wants the old look chains the options struct with `margin = 0.03`. |
| 7 | **Measured disparity** (ADR-046 Amendment 3, Phase 3b). No new struct: a third `XrCursorDepthSourceDXR` kind, `XR_CURSOR_DEPTH_SOURCE_MEASURED_DISPARITY_DXR` (2), for stereo content with no depth. The runtime block-matches a cursor-sized patch between the outermost submitted views (confidence-gated; low confidence = the display plane = the OS cursor). In-process Metal, Vulkan, GL, D3D11 and D3D12 compositors; IPC reports no content. §2.3. |
