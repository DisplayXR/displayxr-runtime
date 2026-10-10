# ADR-046: Depth-aware cursor — opt-in only; the app knows the depth, the runtime places the cursor

**Status:** Proposed (2026-10-06) · Phase 1 implemented · Phase 3a implemented (Metal) · Amendment 1 (lifted content, 2026-10-10) accepted · spec:
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
   - reading the submitted depth layer (Phase 3a), or measuring disparity from the atlas (Phase 3b).

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
| 3a (*implemented, Metal; spec v2*) | **Depth-layer source** (`XrCursorDepthSourceDXR`, `XR_KHR_composition_layer_depth`): for apps that already submit depth with their projection layer, as engines commonly can. The app chains one struct on the hint and sends no point. The compositor copies a cursor-sized patch (at most 64×64) of the two outermost views' submitted depth, never waiting on the GPU. The state tracker turns the nearest texel into a point through that layer's own view and `nearZ`/`farZ`, and feeds it to the v1 placement. Depth is exact, and the app needs no hit-test code. See §6. | One ≤ 32 KB blit per requested frame, plus a CPU min over ≤ 8 K floats |
| 3b | **Runtime-measured depth** (`XrCursorDepthMeasureRequestDXR`): for apps that have no depth (stereo photos and video, legacy content). The compositor block-matches a cursor-sized patch between the outermost views of the submitted atlas. The measured disparity feeds the same filter. It fails safe, falling back to d = 0 on textureless or ambiguous patches. | One tiny compute dispatch |
| 3c (*Amendment 1*) | **Lifted content** (`XR_DXR_lift` weave rects): the service draws the cursor into the lifted views, placed from the conversion's own depth map and the plug-in's relief mapping. | One ≤ 64×64 depth-patch copy and one quad per view, per opted-in rect under the cursor |
| 4 | **Shell / IPC**: an opted-in client's placement is shared with the service, so the workspace controller's cursor rises onto window content, not just window surfaces. Clients that didn't opt in keep today's window-plane behaviour. | none extra |
| 5 | **Web** (*started: displayxr-web `DepthCursor` + `./cursor-depth`*): the inline3d SDK ports the same placement function. The SDK owns its views, so it needs no runtime round-trip. It gets the hint from depth readback or splat expected depth, and draws the sprite with `cursor: none` over the canvas. It is opt-in per viewer (`cursor: 'depth'`). The browser can later offer the lift depth map for converted video. | none in the runtime |

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

### 6. Phase 3a as built: the depth-layer source

**The request.** Spec v2 adds `XrCursorDepthSourceDXR`, chained on the hint, with
`source = XR_CURSOR_DEPTH_SOURCE_SUBMITTED_DEPTH_DXR`. The struct is new; no existing struct
grows. The request covers one frame, so the app chains it every frame for as long as it wants
the service.

**Zero cost when not requested, in code as well as in principle.** Each stage is gated:

| Stage | Gate | Cost for an app that didn't ask |
|---|---|---|
| `xrLocateViews` | extension enabled → hint chained → source struct chained | nothing; the source struct is looked up only behind the hint |
| `xrEndFrame` | `sess->cursor_depth_source.armed`, set only by such a locate | one bool test, and only inside the depth-layer submit path |
| Metal `layer_commit` | `req.requested \|\| in_flight` | two field tests; no buffer, no command buffer, no copy |
| compositor gate | `u_cursor_depth_patch_should_sample()` | unit-tested: a zero-initialised request returns false |

The readback buffers (three 32 KB shared buffers) are allocated on the first real request, never
before. A command buffer is created only once a copy has been encoded. An app that submits depth
for its own reasons, but doesn't request the source, gets no extra work. Its depth swapchains
were already referenced by the layer accumulator, and Phase 3a adds no reference.

**Who does what.**

1. At the armed frame's `xrEndFrame`, the state tracker records the outermost two views (0 and
   active − 1) of the first projection layer that carries depth. It records them from the
   layer **as the app submitted it**: the layer's `XrSpace`, each view's pose and fov, and each
   view's `minDepth`/`maxDepth`/`nearZ`/`farZ`. It keys the record by a tag and hands the
   compositor a one-frame request: the tag, the cursor UV, the footprint, the two view indices,
   and whether Z is reversed.
2. At commit, the Metal compositor copies each view's footprint rectangle out of the depth
   sub-image into a shared buffer, on its own command buffer, and commits it. On a later commit
   it checks that command buffer's `status`. It never calls `waitUntilCompleted`, and when all
   three slots are busy it skips the frame. Once the copy is done, it reduces each patch to its
   nearest texel on the CPU (at most 8 K floats) and publishes the tag, the texel's position
   within the sub-image, and the raw depth.
3. At the next locate, the state tracker:
   - looks up the record by tag;
   - unprojects each view's texel through that view's recorded pose and fov;
   - keeps the nearer of the two in disparity;
   - passes it to the v1 placement as if the app had sent it.

**Why the point is rebuilt in the state tracker, not in the compositor.** The poses the
compositor holds have already been through `handle_space`, so they are in the runtime's
tracking space, not the app's. The view poses the app submitted with the layer are in the
layer's own space. That is the locate space whenever the two `XrSpace` handles are equal.
v2 **requires** them to be equal. A layer in a different space gives "no content" and a one-time
WARN. Converting between two app spaces would cost a space locate per frame for a cursor.

**Depth conventions** are handled by one pure function, `u_cursor_depth_linear_depth()`. It uses
the fact that 1/z is linear in window depth for every perspective projection:

- D3D, Vulkan and Metal [0,1] clip depth, and GL [−1,1] NDC under the default depth range, all
  produce the same window-depth curve.
- Reversed Z (`nearZ > farZ`) and an infinite plane (1/∞ = 0) need no special case.
- The far end of the mapping, where a cleared buffer sits, counts as no content.

**Footprint.** The footprint is the sprite plus 50% (radius 0.75 × `cursorHeight` canvas
heights), the same rule the v1 guidance gives apps. Both outer views are searched at the
cursor's canvas UV, and the nearest texel across both wins. The patch is capped at 64×64 rather
than the 32×32 first suggested. A footprint of that size on a 4K panel's tile is about 50 texels
across, so a 32 cap would shrink the footprint and bring back the cut-through at edges that §1
exists to prevent.

**Bug found on the way.** `comp_layer_accum_projection_depth()` stored depth swapchains at
`[i + view_count]`, but both getters read `[XRT_MAX_VIEWS + i]`. So every depth lookup returned
NULL. Nothing in DisplayXR read submitted depth before, so it was latent. The fix is in the
same PR.

**Metal depth swapchains.** A `Depth32Float` swapchain now takes the private, no-IOSurface path.
A depth format can't be IOSurface-backed, or use shared storage, on macOS. This affects only
apps that create a depth swapchain.

**Build-flag caveat (needs a decision).** `XRT_FEATURE_OPENXR_LAYER_DEPTH` defaults **OFF**
(top-level `CMakeLists.txt`, because of CTS concerns about multi-view depth swapchains). So a
default build does not advertise `XR_KHR_composition_layer_depth`, and Phase 3a stays dormant.
The reference app then logs why and keeps its hit test. Turning the flag on is a separate,
CTS-visible decision, deliberately not made here. The evidence below comes from a build with
`-DXRT_FEATURE_OPENXR_LAYER_DEPTH=ON`.

**Compositors.** Metal reads the patch. vk_native, D3D11, D3D12, GL and the IPC/service path have
compile-safe stubs: `oxr_session_cursor_depth_hand_request()` returns false. They report no
content (cursor on the display plane) with a one-time WARN. Wiring vk_native is the next step,
and it can be tested on macOS through MoltenVK.

**Evidence** (macOS, sim_display SBS, `cube_handle_metal_macos`, atlas captures 1512×1646, cursor
x offset between the two stacked tiles):

| Cursor UV | App hit test (v1) | Depth-layer source (v2) |
|---|---|---|
| 0.5, 0.5 (over the cube) | 22.6 px, target d = −0.066 | 24.6 px, target d = −0.081 |
| 0.1, 0.1 (empty space) | 0 px | 0 px |

The depth-layer source places the cursor about 2 px further forward. That is expected, because
it searches every texel of the footprint box, while the app's hit test casts 9 rays (the centre
and a ring of 8). The box's corners reach √2 times further than the ring, so the box finds a
slightly nearer cube edge. A run without the request, in hint mode and with
`DISPLAYXR_CURSOR_DEPTH=0`, logs none of the Phase 3a one-time WARNs. A run with the request logs
each of them exactly once.

## Consequences

- An app that already raycasts or reads depth gets a correct, consistent cursor for one struct
  on `xrLocateViews` and one sprite draw.
- An app that doesn't opt in is byte-for-byte unaffected.
- The placement math is a pure function of the views, so the SDK port (Phase 5) can be checked
  against the C unit tests' expected values.
- The hint is a frame old. A cursor moving fast across a depth edge can show a single frame
  where the cursor sits behind the nearer content. The footprint dilation and the fast rise keep
  that to the edge itself. Phase 3a as built does not remove this lag. It reads frame N's
  submitted depth, but the result reaches the placement one or two frames later, because waiting
  on the GPU would stall the pipeline (§6). So it has the same lag, and the same mitigation.
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
  every app for a service most don't use, which violates §0. It is kept as the opt-in Phase 3b.
- **Each app does it all.** It works, and is what the demos do today. But the policy diverges
  between apps, and the math is subtle to get rig-agnostic. §2 is easy to get wrong for
  camera-rig apps.
- **Give the hint in display space (metres from the display plane).** The app would then need
  the world-to-display mapping, which camera rigs scale and move. The locate space is the one
  space the app and the runtime already share.

## Amendment 1 (2026-10-10): lifted content — the service draws, the conversion supplies the depth

**Accepted** (David, 2026-10-10). Tracking: runtime #1907, under epic #1858.

### Why lifted content needs its own path

Every earlier phase assumes the app renders the views, so the app can draw the sprite into them
(Phase 1, 3a, 5) or at least submit them (2, 3b). A 2D video lifted through `XR_DXR_lift`
breaks that assumption. The browser hands the service a **2D** frame on a lift-flagged weave
rect. The conversion module makes the views on the service's lift thread, and the service
writes them into the weave input (`lift_weave_rects_nview` / `lift_weave_rect_batch` in
`comp_d3d11_service.cpp`) and weaves. The page never holds a view it could draw into. The only
code that holds both the views and their depth is the service, at that write. That is also
where the workspace controller's cursor is already drawn (the shell pushes `hit_z` and a
sprite, and the service draws one disparity-shifted sprite per tile before the weave), so the
service draws here too.

### Decision

1. **Opt-in per lifted rect.** A struct chained on `XrWeaveRectLiftDXR::next` asks for a
   depth cursor on that rect. No struct, no work: §0 holds. The caller hides its own OS cursor
   over the rect only while the runtime reports, on the weave output, that it drew the cursor
   for that rect in the frame just woven. Every frame the runtime cannot place the cursor (no
   depth, no display mapping, cursor outside the rect, 2D fallback), it reports "not drawn" and
   the OS cursor stays. The failure mode is today's flat cursor, never no cursor.
2. **Depth source: the conversion's own depth map** (Phase 3c). It is the map from the inference
   that made the views being woven (`same_inference`). The service copies a cursor-footprint
   patch (§6's rule, at most 64×64, from the depth map's texels covering the rect's active
   region) to a small staging ring and reads it back without waiting on the GPU, a frame or two
   later, exactly like Phase 3a. The nearest texel in the patch is the content point.
3. **Display mapping from the plug-in, not invented by the runtime.** Placement needs the
   content's distance in front of the screen **as the views present it**, which depends on how
   the module rendered: convergence (including its own auto-convergence, which changes per
   frame), relief thickness, and camera model. The runtime cannot recover that from relative
   depth, which is why `depthToDisplay` stays metric-only. So `xrt_dp_lift_depth` gains a
   per-conversion **relief mapping**, appended under a new feature macro (ADR-020, no ABI bump):
   display z of a texel, in metres toward the viewer (screen = 0), as an affine function of its
   inverse decoded depth, `z = relief_scale · (1 / depth) + relief_offset`. A module that renders
   with a physical off-axis camera has this exactly. The Leia plug-in's off-axis renderer places
   normalised depth n = 1 − h at z = C − n·D, which is affine in h, and h is proportional to
   1/depth for both its relative and metric models. A module that renders dimensionless
   disparity reports no mapping, and the cursor stays flat. Because the mapping is per
   conversion, the cursor tracks the content through convergence changes instead of fighting
   them.
4. **Placement and drawing.**
   - **Placement** is §2–§3 unchanged, using the viewpoints the views were synthesized for (the
     result's ADR-048 echo): E is their midpoint, S is the cursor's point on the rect, and the
     content's z gives t and then d.
   - **Policy and smoothing:** `u_cursor_depth_target` and `u_cursor_depth_filter_step` apply
     the same margin, clamp and smoothing as every other cursor, with one filter per rect.
   - **Per-view position:** each view's sprite position is the projection of C = E + t·(S − E)
     from **that view's** viewpoint onto the rect, so it is exact for SBS and N-view alike, and
     the sprite keeps its on-screen size.
   - **Cursor position:** the service reads the OS cursor position at draw time and maps it into
     the bound window, as the workspace cursor path already does, so pointer latency is the
     weave's, not the conversion's.
5. **One drawer.** The workspace cursor pass is factored into a service helper that draws a
   sprite at per-view positions into an atlas. The shell keeps its behaviour through it, and
   lifted rects use it too.
   - **Sprite:** the system arrow for v1. The browser hides the hardware cursor, so the
     service cannot read the page's cursor shape. A caller-supplied sprite can follow in the
     same struct.
6. **Inside the shell.** A lifted rect inside a workspace client would draw a second cursor
   next to the controller's. Until Phase 4 shares placements with the controller, the service
   skips the lift cursor while a workspace controller's cursor is visible.

### Not changed

- `depthToDisplay` (`XR_DXR_lift` v3) stays metric-only. The relief mapping is an internal
  DP-contract field that serves the cursor. Exposing it to apps is a separate decision.
- No cost for any rect, stream or session that does not chain the request.
