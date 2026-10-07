# XR_DXR_cursor_depth

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_cursor_depth` |
| Spec Version | 1 |
| Type Values | `XR_TYPE_CURSOR_DEPTH_HINT_DXR` (1004999320) · `XR_TYPE_CURSOR_DEPTH_PLACEMENT_DXR` (1004999321) |
| Author | The DisplayXR Project |
| Platform | All. Works on every graphics API, in-process and over IPC; solved in the state tracker. |
| Decision record | [ADR-046](../../adr/ADR-046-depth-aware-cursor.md) |

---

## 1. Overview

The OS cursor is drawn on the display plane, at zero disparity. Over content that pops out of the
glass, that is a **depth violation**: occlusion says the cursor is in front, and disparity says
it is behind. `XR_DXR_cursor_depth` lifts the cursor off the plane. The app reports the nearest
content under the cursor. The runtime returns a pose and size for a cursor sprite just in front
of that content, smoothed over time. The app draws the sprite.

> The **app** owns the depth (it knows its scene). The **runtime** owns placement and policy
> (where, how big, how it moves), so every app's cursor behaves the same. The **app** draws.

### Opt-in, zero cost otherwise

Enabling the extension costs nothing. The runtime does cursor work only on an `xrLocateViews`
call that chains an `XrCursorDepthHintDXR`, and it never touches the OS cursor. Later spec
versions keep this rule: any runtime GPU cost (drawing, measuring) will be a separate struct that
the app explicitly chains.

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
| `isActive` | `XR_FALSE` means draw nothing and show the OS cursor. This happens when no hint was chained, fewer than two views are active (2D), the cursor is off the canvas, or the view geometry is degenerate. |
| `pose` | The sprite centre and orientation in the locate space. The orientation is the display plane's: draw the quad in its local XY plane, facing +Z. The position lies on the line from the viewer through the cursor's point on the canvas, so the cursor never slides sideways as its depth changes. |
| `height` | Sprite height in locate-space units, scaled with depth so the apparent size is constant. |
| `disparity` | The filtered disparity, in eye-baseline units: 0 is the display plane and < 0 is in front. Diagnostic only. |
| `targetDisparity` | The unfiltered target: the content disparity minus the margin, then clamped. Diagnostic only. |

## 3. Runtime behaviour

The canvas point under the cursor is triangulated from the outermost pair of **located views**.
Every policy number is applied in disparity d = 1 − 1/t, where t is depth relative to the canvas
along the display normal measured from the cyclopean eye:

- margin: 0.03 baseline in front of the content;
- clamp: d ∈ [−0.6, 0.6];
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

## 5. Version history

| Version | Change |
|---|---|
| 1 | Initial: app hint in, placement out, app draws (ADR-046 Phase 1). |
