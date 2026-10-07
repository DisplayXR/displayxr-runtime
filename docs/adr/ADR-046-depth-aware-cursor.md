# ADR-046: Depth-aware cursor — opt-in only; the app knows the depth, the runtime places the cursor

**Status:** Proposed (2026-10-06) · Phase 1 implemented · spec:
[XR_DXR_cursor_depth.md](../specs/extensions/XR_DXR_cursor_depth.md) · sibling of
[ADR-040](ADR-040-rear-depth-budget.md) (the same cue conflict, at the cursor instead of the desktop)

## In one paragraph

The OS cursor is flat. It is drawn at zero disparity, on the display plane (the ZDP). When it
hovers content that pops out of the glass, it is drawn **over** pixels whose disparity says they
are **in front of** it. That is a depth violation, and it reads as a broken image. The shell
already avoids this for window surfaces by lifting its cursor to the window plane. The DisplayXR
Browser demos now lift it to the hovered 3D object. This ADR makes that a platform service for
any app that knows the depth of its content: model viewers, splat viewers, stereo media with a
depth map. **The service is opt-in, and an app that doesn't ask pays nothing.** The app reports
the nearest content point under the cursor. The runtime returns where to draw a cursor sprite,
and how big, so that it sits just in front of that content. The runtime also smooths the
movement over time, so every DisplayXR cursor behaves the same way.

## Context

### The conflict

A cursor at disparity 0 drawn over content at crossed disparity *d* < 0 gives two cues that
disagree:

- Occlusion says the cursor is in front, because it covers the content.
- Disparity says the cursor is behind, because the content is nearer.

This is the same class of conflict as rear content over a busy desktop (ADR-040), with the roles
swapped. Unlike ADR-040, it is under the user's eye all the time, wherever they are pointing.

### Who can know the depth

Only the app knows what is under the cursor. The runtime receives finished pixels and has no
scene to raycast. So "the runtime raytraces" isn't available, and doesn't need to be: an app
that renders 3D already has the answer for almost nothing.

| Content | Cheapest exact source |
|---|---|
| Meshes | Readback of a few depth-buffer texels around the cursor, without stalling (PBO plus fence, `mapAsync`, or a staging copy). A BVH raycast also works. |
| Gaussian splats | Alpha-weighted expected depth from the splat pass, read back the same way. Never raycast splats. |
| Stereo / RGB-D media | Sample the depth map. For lifted 2D content the `XR_DXR_lift` depth map already exists. |

A readback is one frame late. The time filter below absorbs that.

### Who should decide where the cursor goes

If every app invents its own margin, comfort clamp and smoothing, cursors behave differently
from app to app. Those decisions are policy about perception, which ADR-040 already established
is the runtime's to own. Drawing stays with the app, because the app owns its swapchains.

## Decision

### 0. First principle: opt-in, zero cost otherwise

The runtime does **no cursor work for anyone who didn't ask for it**, and that holds for every
later phase:

1. Enabling the extension costs nothing.
2. Cursor work happens **only** on an `xrLocateViews` call that chains an `XrCursorDepthHintDXR`.
   With no hint, there's no math, no state change and no allocation.
3. The runtime **never touches the OS cursor**. Hiding it while the depth cursor is shown is
   the app's job, in the app's window.
4. Any phase that costs the runtime GPU time gets its own explicitly chained request struct, and
   costs nothing until that struct is chained:
   - drawing the sprite (Phase 2);
   - measuring disparity from the atlas (Phase 3).

   Even when chained, that cost is bounded to a cursor-sized patch and to frames where the
   cursor is over a 3D canvas.

This is enforced structurally. The runtime code sits behind the runtime extension gate, then
behind the hint lookup.

### 1. Division of labour

- **App:** owns the content, so it supplies the depth. It reports the content point nearest the
  viewer within the cursor's footprint (the whole sprite plus a little, not just the hotspot
  pixel). Using the footprint is what stops a nearer edge beside the hotspot from cutting through
  the sprite.
- **Runtime:** owns placement and policy. It decides where along the line of sight the sprite
  goes, how big it must be to keep a constant apparent size, and how it moves over time.
- **App:** draws the sprite last, with depth testing off, at the pose it was given. Every view
  then renders it at the correct disparity.

### 2. Geometry is solved from the views, never from rig internals

The hint and the result travel through the same `xrLocateViews` call. The runtime solves
everything from the views that call has just produced:

- **S**, the canvas point under the cursor, is where the outermost pair of view rays through
  canvas point (u, v) meet. Every Kooima frustum frames the same canvas, so those rays meet.
- **E**, the cyclopean eye, is the midpoint of that pair.
- A point's depth is **t**: its distance in front of E along the display normal, divided by S's
  distance. So t = 1 on the canvas, and t < 1 in front of it.
- The sprite goes at **C = E + t·(S − E)**, with height scaled by t. Because C stays on the
  cyclopean ray, the cursor never slides sideways as it rises.

Because only located views are used, the result is correct for:

- display rigs and camera rigs;
- any m2v scale;
- display zones, where the canvas is the zone;
- any view count of 2 or more.

The unit tests check this by applying a random rigid transform plus a 12.5× uniform scale and
asserting the same placement. With one active view (2D) the result is inactive and the OS cursor
stays.

### 3. Policy works in disparity, not distance

The policy works in **d = 1 − 1/t**, in units of the eye baseline:

- on-screen disparity = baseline × d;
- d = 0 on the canvas;
- d < 0 in front of it.

The eye compares disparities, so the margin, clamp and slew rates are expressed here:

| Parameter | Default | Why |
|---|---|---|
| margin in front of content | 0.03 | about 2 mm crossed on screen at 65 mm IPD: reads as in front without looking detached |
| clamp | [−0.6, +0.6] | t ≥ 0.625: never more than about 3/8 of the way to the eye |
| rise time constant | 30 ms | never lag behind content that comes forward, or the violation is visible |
| sink time constant | 250 ms | no flicker when the footprint crosses an edge |
| stale gap | 0.5 s | after a pause the cursor snaps into place instead of gliding in |

When nothing is under the cursor, the target is d = 0 (the display plane). Content behind the
display plane gives d > 0, and the cursor settles onto it. A second locate in the same frame
(several zones) doesn't advance the filter.

### 4. Phases

| Phase | What | Runtime cost while requested |
|---|---|---|
| **1 (this ADR, implemented)** | `XR_DXR_cursor_depth` v1: app hint in, placement out, app draws. Pure placement module `u_cursor_depth` with tests; state-tracker fill in `xrLocateViews`. Works in-process and over IPC, on every graphics API. | A few dozen flops per locate |
| 2 | **Runtime-drawn cursor** (`XrCursorDepthDrawRequestDXR`): the compositor draws the sprite into the atlas before the display processor, reading the OS cursor position at the last moment before compositing. For apps that would rather not draw. | One quad per view |
| 3 | **Runtime-measured depth** (`XrCursorDepthMeasureRequestDXR`): for apps that have no depth (stereo photos and video, legacy content). The compositor block-matches a cursor-sized patch between the outermost views of the submitted atlas. The measured disparity feeds the same filter. It fails safe, falling back to d = 0 on textureless or ambiguous patches. | One tiny compute dispatch |
| 4 | **Shell / IPC**: an opted-in client's placement is shared with the service, so the workspace controller's cursor rises onto window content, not just window surfaces. Clients that didn't opt in keep today's window-plane behaviour. | none extra |
| 5 | **Web**: the inline3d SDK ports the same placement function. The SDK owns its views, so it needs no runtime round-trip. It gets the hint from depth readback or splat expected depth, and draws the sprite with `cursor: none` over the canvas. It is opt-in per viewer (`cursor: 'depth'`). The browser can later offer the lift depth map for converted video. | none in the runtime |

### 5. Reference adoption and evidence

`cube_handle_metal_macos` is the first app to use the extension. Each frame it hit-tests rays
from the two outer views through a ring of footprint points against the cube, sends the nearest
hit as the hint, and draws a crosshair at the returned pose with depth testing off. The OS cursor
is hidden while the crosshair is shown. To opt out, set `DISPLAYXR_CURSOR_DEPTH=0`. For headless
checks, `DISPLAYXR_CURSOR_DEPTH_UV=u,v` scripts the cursor position. Atlas captures on macOS
(sim_display, 2 views) gave:

| Cursor UV | Over | Cursor disparity between views | Meaning |
|---|---|---|---|
| 0.5, 0.5 | the cube | −23.4 px; the cube's nearest edge there is about −15 px | in front of the cube |
| 0.5, 0.65 | the floor grid, which the app doesn't report as content | 0 px | on the display plane |
| 0.1, 0.1 | empty space | 0 px | on the display plane |

In every capture, the midpoint of the two cursor images sat at the requested u, and both sat at
the same v. So the cursor rises along the line of sight and doesn't drift sideways.

## Consequences

- An app that already raycasts or reads depth gets a correct, consistent cursor for one struct
  on `xrLocateViews` and one sprite draw.
- An app that doesn't opt in is byte-for-byte unaffected.
- The placement math is a pure function of the views, so the SDK port (Phase 5) can be checked
  against the C unit tests' expected values.
- The hint is a frame old. A cursor moving fast across a depth edge can show a single frame
  where the cursor sits behind the nearer content. The footprint dilation and the fast rise keep
  that to the edge itself. Phase 3 measures the current frame, so it doesn't have this lag.
- **Head motion and look-around.** The sprite sits on the cyclopean ray through the cursor's
  canvas point. So each eye sees it on the glass at S ∓ (baseline/2)·d, which doesn't depend on
  where the head is (unit-tested across three head poses). The cursor's image on the panel
  therefore cannot swim with tracking motion or jitter. Only the content under the line of sight
  changes as the user looks around, and the hit test is redone every frame. This is deliberate
  SCREEN anchoring, not world anchoring. A mouse is a 2D screen-space device, so the cursor slides
  over a surface under head motion instead of sticking to it. If it stuck to the surface, the
  click target would drift on the glass as the head moved.
- An app-drawn cursor shows the app's frame latency, not the hardware cursor's. Phase 2's late
  cursor read narrows that but cannot remove it. This is inherent to any cursor that has
  disparity.

## Alternatives considered

- **Runtime raycasting.** The runtime has no scene, so this is impossible.
- **Always-on atlas measurement.** It would serve every app with no cooperation, but it charges
  every app for a service most don't use, which violates §0. It is kept as the opt-in Phase 3.
- **Each app does it all.** It works, and is what the demos do today. But the policy diverges
  between apps, and the math is subtle to get rig-agnostic. §2 is easy to get wrong for
  camera-rig apps.
- **Give the hint in display space (metres from the display plane).** The app would then need
  the world-to-display mapping, which camera rigs scale and move. The locate space is the one
  space the app and the runtime already share.
