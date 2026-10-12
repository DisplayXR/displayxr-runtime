# ADR-046: Depth-aware cursor — opt-in only; the app knows the depth, the runtime places the cursor

**Status:** Proposed (2026-10-06) · Phase 1 implemented · Phase 3a implemented (Metal) · spec v3 anchor modes (2026-10-07) · Amendment 1 (lifted content, 2026-10-10) accepted · Amendment 2 (one policy, runtime draws, app chooses the look, 2026-10-11) accepted, §B built on Metal, Vulkan, GL, D3D11 and D3D12 (spec v5) · Amendment 3 (measured-disparity fallback, 2026-10-11) accepted · spec:
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
- The sprite's height is scaled by t, so its apparent size is constant. Where it goes
  laterally is the anchor mode (§7). The default, HYBRID, puts it at **C = E + t·(S − E)**
  while the pointer moves: on the cyclopean ray, so it never slides sideways as it rises.

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
| margin in front of content | 0.015 | about 1 mm crossed on screen at 63–65 mm IPD: reads as in front without looking detached. Spec v3 briefly lowered it to 0.005 after a head-tracked-panel test; Amendment 2 A.2 restored 0.03, then halved it to 0.015 (2026-10-11). Apps can override it (`XrCursorDepthOptionsDXR::margin`) |
| clamp | [−0.6, +0.6] — **superseded by Amendment 2: [−0.6, 0], never behind the glass** | t ≥ 0.625: never more than about 3/8 of the way to the eye |
| rise time constant | 30 ms | never lag behind content that comes forward, or the violation is visible |
| sink time constant | 250 ms | no flicker when the footprint crosses an edge |
| stale gap | 0.5 s | after a pause the cursor snaps into place instead of gliding in |

When nothing is under the cursor, the target is d = 0 (the display plane). ~~Content behind the
display plane gives d > 0, and the cursor settles onto it.~~ Amendment 2: content on or behind
the plane leaves the cursor on the plane. A second locate in the same frame
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
| 0.5, 0.5 | the cube | −23.4 px with the original 0.03 margin; the cube's nearest edge there is about −15 px (for spec v3's 0.005 margin, see §7) | in front of the cube |
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

### 7. Spec v3: anchor modes, and why HYBRID is the default

**What the tester saw.** On a real head-tracked panel, the v1/v2 cursor read as "at the glass"
even when its disparity put it on the content. The cause is a cue conflict of its own. The sprite
sat on the cyclopean ray through the pointer's canvas point (§2), so each eye's image of it on the
glass was S ∓ (baseline/2)·d whatever the head did. That is disparity with **no motion parallax**.
On a head-tracked display everything else parallaxes as the head moves, so a cursor that doesn't
reads as lying on the one surface that also doesn't: the glass. A world-fixed cursor read
correctly, but sat a millimetre or two beside the click point.

**The modes** (`XrCursorDepthOptionsDXR::anchorMode`, a new struct chained on the hint; no
existing struct grows). With f the display normal away from the viewer and e = dot(S − E, f):

| Mode | Sprite centre C | Aim | Parallax |
|---|---|---|---|
| SCREEN (v1/v2) | E + t·(S − E) | exact, always | none: reads as at the glass |
| WORLD | S − f·(1 − t)·e | ~1–2 mm off for an off-axis viewer, always | correct |
| **HYBRID (default)** | moving: SCREEN, and store the line of sight (E₀, S₀, D₀ = e); still: E₀ + s·(S₀ − E₀), s = 1 − (1 − t)·e / D₀ | exact while moving, and on the click point while still unless the head moves | correct while still |

"Moving" means `cursorUV` changed by more than 1e-4 since the last moving placement. HYBRID
re-anchors on the first hinted locate and whenever the time filter re-primes; the anchor lives
beside the filter in the session and is touched only by a hinted locate, so §0 holds.

**The anchor is the line of sight at the last pointer move, not a point.** While the pointer is
still, C is the point on the anchor-time line E₀ → S₀ whose distance in front of the glass,
(1 − s)·D₀, equals the current depth (1 − t)·e. Three properties follow, all unit-tested:

- **Continuity:** on the frame the pointer stops, the geometry is the anchor's, so s = t and C is
  the last moving placement exactly. No jump.
- **Parallax:** a lateral head move at fixed t leaves e, and so s and C, unchanged. The sprite is
  world-fixed and parallaxes like content at its depth.
- **No drift from depth changes:** when the content under a still pointer changes depth, C slides
  along the anchor-time line, so from the eye that aimed it, it stays on the click point.

An earlier draft of the rule stored the line's *foot* on the glass and lifted C from it along the
display normal. The headless captures below exposed the flaw: a depth change under a still
pointer then moved the sprite off the click point by about |Δd| × the viewer's lateral offset
(27 px for sim_display's viewer, 0.10 m above the canvas centre, when the scripted pointer
anchored at d = 0 on frame 1 and the cursor then rose to the cube). The web SDK takes the same
line-of-sight rule (inline3d 1.37.1).

**Why HYBRID beats both pure modes.** While the pointer moves, the user is aiming, and the eye
tracks the cursor, not the head; there SCREEN's exact aim is what matters and a parallax deficit
over a few frames is invisible. While it is still, the user is looking at the scene, often moving
their head to see around it, and the cursor must parallax like the content it rests on; there
WORLD's behaviour is right. The only cost is that the drawn sprite drifts from the click point as
the **head** moves while the pointer is still: its image on the glass moves by about |d| × the
head's lateral displacement. For content near the glass (d ≈ −0.005 with the new margin), 20 cm of
head motion moves it about 1 mm. The next mouse movement re-aims it exactly. Hit testing is
unchanged: the click is always at the pointer's canvas point.

**Evidence** (macOS, sim_display SBS, `cube_handle_metal_macos`, `DISPLAYXR_CURSOR_DEPTH_UV=0.5,0.5`,
atlas 1512×1646, two tiles of 1512×823 stacked; disparity = cursor x in the right-eye tile minus
the left-eye tile; the cube's nearest edge under the cursor is about −15 px). The scripted pointer
never moves, so HYBRID anchors on frame 1 at d = 0 and the cursor then rises to the cube with the
pointer still: the depth-change case.

| `DISPLAYXR_CURSOR_DEPTH_ANCHOR` | Target d | Cursor disparity | Cursor y in tile (pointer at 411) |
|---|---|---|---|
| unset (options not chained → HYBRID) | −0.040 | −16.6 px | 411 |
| `hybrid` | −0.040 | −16.6 px | 411 |
| `screen` | −0.040 | −16.6 px | 411 |
| `world` | −0.040 | −16.6 px | 438 |
| *foot-anchored draft of HYBRID* | *−0.040* | *−16.6 px* | *438* |
| *before v3: 0.03 margin, SCREEN (§5)* | *−0.066* | *−23.4 px* | — |

All sit just in front of the cube's edge, about 7 px nearer it than with the old margin. HYBRID
stays on the click point like SCREEN; WORLD sits 27 px below it for this off-axis viewer. In every
capture the midpoint of the two cursor images is at the requested u (0.4995).

**A deliberate default change.** An app that doesn't chain the options struct gets HYBRID and
the 0.005 margin, which is different from v2. It is tester-driven, both changes go the same way
(the cursor reads as on the content), and an app that wants the old placement chains
`XR_CURSOR_DEPTH_ANCHOR_MODE_SCREEN_DXR` with `margin = 0.03`. The web SDK's depth cursor
implements the identical rule with the same numbers.

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
- **Head motion and look-around (revised in spec v3, §7).** v1/v2 anchored the sprite to the
  screen: on the cyclopean ray, so each eye saw it on the glass at S ∓ (baseline/2)·d whatever
  the head did (still unit-tested, now as the SCREEN-mode property). The reasoning was that a
  mouse is a 2D screen-space device and its click target must not drift. Testing on a real
  head-tracked panel showed the price: a sprite with disparity but no motion parallax conflicts
  with everything around it that does parallax, and reads as at the glass. The default is now
  HYBRID: screen-anchored while the pointer moves (exact aim), and anchored to that line of
  sight while it is still (correct parallax, unit-tested: the sprite does not move in locate
  space as the head moves laterally).
  The click target never drifts, because hit testing stays at the pointer's canvas point. Only
  the drawn sprite drifts, by |d| × head displacement (about 1 mm for 20 cm of head motion over
  content near the glass), and only until the mouse next moves. A depth change under a still
  pointer does not drift it: the anchor is the line of sight, not a point (§7). SCREEN and WORLD remain
  available through `XrCursorDepthOptionsDXR`.
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

## Amendment 2 (2026-10-11): one cursor policy everywhere, the runtime draws, the app chooses the look

**Accepted** (David, 2026-10-11). It supersedes §3's clamp and every per-implementation tuning,
and turns Phase 2 (runtime-drawn cursor) into the default way apps adopt the cursor.

### Why

By 2026-10-11 five implementations had diverged:
- the runtime placement (`u_cursor_depth`, Phase 1);
- the SDK's `DepthCursor` / `cursor: 'depth'` on `./model` and `./splat`;
- the SDK's stereo-tile cursor on `./call` and `./camera` (web #159/#162);
- the lifted-video cursor (Amendment 1, #1908);
- the shell's workspace cursor.

Two of them could sink the cursor **behind** the glass. Margins ranged from 0.005 to 0.03 of the
eye baseline. Three different sprites were in use. A tester on a head-tracked panel also showed
that a cursor with disparity but no motion parallax reads as being at the glass (#1847). Apps,
meanwhile, had to hit-test, draw a sprite and manage the OS cursor themselves, which is why no
demo had adopted the cursor.

### Decision

#### A. One policy, for every DisplayXR depth cursor

Every implementation listed above follows these rules, with the same defaults. Where a pure
module exists (`u_cursor_depth` in the runtime, `./cursor-depth` in the SDK), it is the single
implementation that the others call.

1. **Never behind the glass.** The cursor is either **on the screen plane** or **in front of
   it**. It moves only to stay ahead of content that comes **out** of the screen. With `d` as
   the content's disparity under the cursor footprint (eye-baseline units, < 0 in front):

   `target = d < 0 ? max(d − margin, d_min) : 0`

   Content on or behind the glass leaves the cursor on the glass. This replaces §3's
   `[−0.6, +0.6]` clamp with `[−0.6, 0]`. Rationale: a cursor behind the glass sits behind the
   page, the window chrome and the desktop, which recreates the depth conflict it exists to
   remove.
2. **Margin 0.015 eye baselines**, about 1 mm of crossed on-screen disparity at a 63–65 mm IPD.
   Implementations expose it as a `margin` option and keep 0.015 as the default.
   *Halved 2026-10-11 after the Windows Leia panel check: 0.03 floated visibly off the content.*
   (As first written, A.2 was 0.03, about 2 mm, the value the reference app
   `cube_handle_metal_macos` had shown since Phase 1.)
3. **On the plane, it is the OS cursor.** Nothing is drawn while the filtered disparity is above
   the **draw threshold**, −0.005 baselines (about 0.3 mm on screen) and never less than 0.5 px
   on the panel. While nothing is drawn, the OS cursor is the cursor, untouched. The drawn
   cursor exists only while content pops out under it, and the filter hands it back smoothly.
4. **Filter unchanged** (§3): rise 30 ms, sink 250 ms, stale 0.5 s.
5. **Anchor: hybrid, line of sight.**
   - While the pointer moves, the sprite sits on the cyclopean ray through the pointer's canvas
     point, exactly over the click point.
   - While the pointer is still, the runtime keeps the line of sight from the last pointer move
     (E₀ → S₀) and places the sprite on it at the current depth: the point whose distance in front
     of the glass is (1 − t)·eyeToCanvas.

   A head move leaves the sprite fixed in the world, so it parallaxes like content. A depth
   change slides it along that line, so it stays on the click point for the viewer who placed
   it. It is continuous at the moment the pointer stops. Screen-anchored and world-anchored modes
   remain options. This rule is the one in #1847 and SDK 1.37.1; Amendment 1's lifted-video
   cursor adopts it in its next revision.
6. **Default look: the cursor the user already has.** The drawn sprite mirrors the **current OS
   cursor shape** for that window (arrow, hand, I-beam, or whatever the app or page set). It is
   drawn at the OS cursor's physical size (its pixel size times the panel's pixel pitch) and
   scaled by t, so its apparent size never changes. Where the shape cannot be read, the
   fallback is the classic arrow: 12 × 19 px, white with a dark outline (web #162's).

#### B. The runtime draws; the app chooses the look (same split as the shell)

The shell already works this way (`XR_DXR_spatial_workspace` v13+): the controller owns the
sprite and the depth, and the runtime owns the drawing. Apps get the same contract in
`XR_DXR_cursor_depth`:

1. **Request.** A new struct chained on `XrCursorDepthHintDXR` asks the runtime to draw:
   - `XrSwapchain image`: an ordinary swapchain the app created with `xrCreateSwapchain`, in its
     own graphics API, filled with any sprite (static, animated, a different shape per state).
     `XR_NULL_HANDLE` means the default look from A.6.
   - `XrVector2f hotSpot`: normalised UV of the click point inside the sprite.
   - `float sizeMeters`: physical size; 0 = the OS cursor's size.
   - `XrBool32 visible`.

   Because the app fills the swapchain in its own API, the runtime never has to create or upload
   textures per graphics API.
2. **Report.** A new struct chained on `XrViewState` says whether the runtime will draw the
   cursor in the frame being located. The app hides its OS cursor exactly while that is true
   (or lets the `displayxr-common` helper do it). The runtime still never touches the OS cursor
   (§0). Any frame the runtime cannot place the cursor, it reports "not drawn", so the fallback
   is always the flat OS cursor and never no cursor.
3. **Where it is drawn.** After the app's layers and before the display processor, one sprite
   per view at the placement pose, on top and never occluded.
   - **In-process:** each native compositor draws it as a runtime-owned quad. Every backend
     already composites quad layers in stereo.
   - **Service / IPC:** through the drawer Amendment 1 factored out (`svc_draw_cursor_quads`).
     That one path serves the shell, lifted rects and apps.
4. **Depth sources unchanged:** the app's hint point (§1), the submitted depth layer (§6,
   Phase 3a), or the conversion's depth map (Amendment 1). The app picks one. The policy in A is
   applied to whichever it picks.
5. **App-drawn placement stays supported.** The v1–v3 placement output is not removed. An app
   that wants to draw the cursor itself still can, with the same policy.
6. **§0 holds.** No request, no work: no sprite, no copies, no state. The draw request is per
   frame.

#### C. Web

- The SDK's `DepthCursor`, `cursor: 'depth'` on `./model` and `./splat`, and the stereo-tile
  cursor on `./call` and `./camera` all use the shared `./cursor-depth` core with the policy in
  A and the arrow look. The SDK's crosshair is retired.
- Later, the browser feeds the page's computed CSS `cursor` (including `url(...)` images) to the
  runtime as the sprite (B.1). Pages then style the depth cursor with plain CSS, and the SDK
  needs no cursor code for it.

#### §A as built in the runtime (#1847, `XR_DXR_cursor_depth` spec v4)

- `u_cursor_depth_target()` is A.1 verbatim; the upper clamp is 0 whatever the tuning says
  (`max_disparity` defaults to 0 and a value > 0 is treated as 0). Margin default 0.015 (A.2; 0.03 until 2026-10-11),
  still overridable through `XrCursorDepthOptionsDXR::margin`.
- A.3 is `u_cursor_depth_should_draw()` (threshold −0.005). The state tracker reports it as
  `XrCursorDepthPlacementDXR::isActive`, whose meaning was already "draw nothing, show the OS
  cursor", so no struct changed. The filter and the HYBRID anchor step on every locate, drawn or
  not, so the hand-back is smooth. `disparity` / `targetDisparity` stay filled as diagnostics.
- The 0.5 px floor is left to the drawing backend (Part B): the placement does not know the
  panel's pixel pitch, and −0.005 baselines (~0.33 mm at 65 mm IPD) is above 0.5 px on every
  current panel.
- A.5 (hybrid line-of-sight anchor) was already the v3 default and is unchanged.

#### §B as built on Metal (`XR_DXR_cursor_depth` spec v5)

- **API.** `XrCursorDepthDrawInfoDXR` (on the hint: `image`, `hotSpot`, `sizeMeters`, `visible`)
  and `XrCursorDepthDrawStateDXR` (on `XrViewState`: `willDraw`). New structs only. The app's
  sprite is sampled as **straight alpha** from its last released image (the shell cursor's
  convention). `sizeMeters` is the sprite's height at the glass; ≤ 0 = the OS cursor's size.
- **State tracker.** A hinted locate that chains the draw request runs the placement even when
  the app did not chain `XrCursorDepthPlacementDXR`. When the placement is active it records the
  locate space, hotspot point C, display-plane orientation and one *unit* = canvas height × t,
  and reports `willDraw`. At `xrEndFrame` (one bool test otherwise) it maps C into **each view of
  the frame's first projection layer, with the poses and fovs the app submitted**, as an affine
  map per view (where C lands in the tile, and where one unit along the sprite's X and Y goes).
  The map is exact because every Kooima image plane is parallel to the display plane, which the
  sprite lies parallel to. Same-space rule as §6: a layer in another space, a zones frame, or a
  frame without a projection layer draws nothing and makes the next locates report
  `willDraw = XR_FALSE` (one-time WARN) until a frame is drawable again, so the fallback is always
  the OS cursor. Pure helpers in `u_cursor_depth` (`u_cursor_draw_*`), unit-tested, including
  the compositor gate `u_cursor_draw_should_draw()` (false for a zero-initialised request).
- **Metal compositor.** The request is consumed by the next commit (one field test otherwise).
  The cursor is drawn **inside the atlas encoder, after the projection, quad and window-space
  layers and before the display processor**: one alpha-blended triangle strip per tile, depth
  off, scissored to the tile, through its own small shader library and four pipelines (straight /
  premultiplied × UNORM / `_sRGB` target) built on the first request. A frame carrying the cursor
  never takes the zero-copy path. Because the draw is inside the atlas pass, the
  `/tmp/dxr_atlas_trigger` dump shows it.
- **Default look (A.6).** The current OS cursor is mirrored: when the commit runs on the main
  thread (AppKit is main-thread-only; the `cube_*_macos` apps pump events and render there),
  `[NSCursor currentCursor]`'s image is rasterised into a small premultiplied `_sRGB` texture,
  only when the cursor object changes, with its hotspot and its size in points × the panel's
  metres per point (pixel pitch × backing scale). Off the main thread the last snapshot is
  kept, else the classic arrow (12 × 19, white fill, dark outline, rasterised procedurally,
  4 × 4 supersampled). `DXR_CURSOR_DEFAULT_LOOK=arrow` forces the arrow.
- **Not yet:** the service (`svc_draw_cursor_quads`) reports
  `willDraw = XR_FALSE`; display zones (since drawn, runtime#1949, below); multi-screen routed frames draw per tile without the
  segment routing on Metal (Vulkan and GL skip them); the 0.5 px floor of A.3 is not applied
  (−0.005 is above it on current panels).

#### §B as built on Vulkan and GL (`XR_DXR_cursor_depth` spec v5, unchanged)

- **One default look for every backend.** The Metal compositor's default-look code moved to
  `compositor/util/comp_cursor_look.{h,c}` (+ `comp_cursor_look_macos.m`), a small library the
  Metal, vk_native and GL compositors all link: it picks the look (mirrored `[NSCursor
  currentCursor]` on the main thread, else the last snapshot, else the procedural arrow from
  `u_cursor_draw_arrow_rgba()`), rasterises it once per change into premultiplied, sRGB-encoded
  RGBA8, and reports aspect, hotspot and size in points plus the main screen's backing scale.
  Each compositor only uploads the pixels when the look's `generation` moves.
  `DXR_CURSOR_DEFAULT_LOOK=arrow` forces the arrow on all three. **OS-cursor mirroring is
  macOS-only:** Linux (`XFixesGetCursorImage`) and Windows (`GetCursorInfo` + `GetIconInfo`)
  draw the arrow; mirroring there is future work.
- **State tracker.** Additive only: `oxr_session_cursor_draw_supported()` /
  `_hand_request()` gained the `is_vk_native_compositor` and `is_gl_native_compositor` arms.
  The placement, the per-view map and the zero-cost gate are the Metal ones.
- **vk_native.** The request is consumed by the next commit (one field test otherwise) and
  resolved there into tile-normalised corners per view (`u_cursor_draw_corners`). A cursor frame
  never zero-copies and always takes the renderer's **compose render pass** (the blit fast path
  cannot blend): the cursor is the pass's last draw, one quad per view tile through the pass's
  own pipelines (the QUAD vertex path with an affine MVP built from three corners; straight or
  premultiplied blend), scissored to the tile, depth off. Because the pass renders into the
  private `_SRGB` compose target and publishes to the atlas with a raw copy, the sprite blends
  in linear light, an `_SRGB` sprite is decoded on sample (the same single source-view choice as
  every layer, `compose_source_view()`), and the atlas stays ENCODED — never double-encoded. The
  default look is an `R8G8B8A8_SRGB` image (UNORM under the legacy colour hatch) uploaded
  through a staging buffer inside the same command buffer. Nothing is created before the first
  request; the descriptor pool carries one extra set for the cursor. **Ordering differs from
  Metal:** vk_native stamps window-space layers in a later, weave-time pass (several call sites,
  some on the async weave hand-off), so the cursor is drawn over projection, quad and equirect2
  layers but **under** window-space layers. Both the projection-only and the post-compose
  `$TMPDIR/displayxr_atlas_trigger` captures show it.
- **GL.** Drawn after the window-space layers, before the DP, into whatever the atlas pass is
  painting: the private `GL_SRGB8_ALPHA8` compose target with `GL_FRAMEBUFFER_SRGB` on, or the
  ENCODED atlas directly. The source is bound through `gl_bind_layer_source()` (decode iff
  composing), the same rule as every layer; a cursor frame counts as one more sub-rect layer in
  `gl_frame_takes_fast_path()` (a scene-linear UNORM sprite forces the compose path when it
  exists). A small program (corners as uniforms, `gl_VertexID` strip), per-tile viewport +
  scissor, blend state restored. A GL sprite is sampled bottom-up (its top is its last row —
  the Metal compositor's GL-source flip). A layered (`arraySize > 1`) GL sprite is not drawn
  yet (one-time WARN). On Apple's GL 4.1 there is no `glCopyImageSubData`, so every macOS GL
  frame composes in encoded space, cursor included; the linear compose-target path was not run
  here.
- **Measured** (macOS, sim-display Cropped SBS, cube reference apps at `cursorUV` 0.5,0.5 over
  the cube, `DXR_CURSOR_DEFAULT_LOOK=arrow`): GL −23.6 px in the 1512 × 1646 atlas (Metal −24.0
  on the same app geometry), vk_native −22.0 px in its 1280 × 1440 atlas; nothing drawn at
  0.1,0.1; the arrow's fill reads (255,255,255) and its outline (5,5,8), the custom ring
  (0,220,255) — exactly the authored bytes, so no encode is lost or doubled.
- **Known issue (GL on macOS, not caused by the draw):** `cube_handle_gl_macos` crashes in
  roughly 1 of 5 launches once the depth cursor is ACTIVE — in Apple's GL driver, on the #868
  repaint thread (`gl_repaint_thread` → `glBlitFramebuffer`) or in the app's own draw. It
  follows the app's `[NSCursor hide]`, not the runtime draw: app-draw mode crashes too; the
  runtime draw with the OS cursor left visible (0/10), `DXR_WEAVE_REPAINT=0` (0/8) and the
  extension off (0/20) do not. The GL cube never hid the OS cursor before this adoption, so this
  is a latent repaint-thread / AppKit race in the GL compositor surfacing — runtime#1947. Until it
  is fixed the GL cube's depth cursor is **opt-in** (`DISPLAYXR_CURSOR_DEPTH=1`); the Metal and
  Vulkan cubes stay default-on.
- **Found in passing (Metal, #1941):** on the Vulkan-app-on-Metal fallback a custom sprite is
  handed over as the client swapchain and cast to the native Metal type — runtime#1948.

#### §B as built in display-zones frames (`XR_DXR_cursor_depth` spec v6, unchanged; runtime#1949)

- **One cursor per frame, in the zone under the pointer.** The hint already rides the
  zone-scoped locate of that zone (§2.1 of the spec): the zone rect is that locate's canvas, so
  `cursorUV`, the placement and its *unit* (zone height × t) are all zone-relative. The arming
  locate now also records the `XrDisplayZoneDXR::zoneId` it carried.
- **State tracker.** In a zones frame `xrEndFrame` maps the placement through the views of the
  frame's projection layer whose chained `XrDisplayZoneDXR` has that `zoneId` (its submitted
  poses and fovs, same-space rule unchanged), so the map is ZONE-normalised; the request
  (`u_cursor_draw_request`) gains `zone_valid` / `zone_id` / `zone_rect` (the rect as SUBMITTED —
  authoritative per ADR-027). No such zone, a zone-scoped hint in a frame without zones, or a
  zones frame whose hint was not zone-scoped: nothing drawn, `willDraw = XR_FALSE` on the next
  locates, one-time WARN — the same OS-cursor fallback as before. No new struct, no spec bump:
  `willDraw` already tells the app frame by frame.
- **Compositors (Metal, vk_native, GL).** One pure helper, `u_cursor_draw_into_zone()`, re-expresses
  the zone-normalised views in the tile exactly the way each compositor places the zone's own
  layer (zone rect scaled by tile / window), and returns the zone's sub-rect as a clip; the draw
  is scissored to tile ∩ zone, so a cursor at a zone edge never bleeds into a 2D zone or a
  neighbouring 3D zone. `u_cursor_draw_canvas_px_h()` sizes the sprite against the zone's height
  (the canvas its unit was built for). A non-zone request is untouched (clip = whole tile).
  Zero cost unchanged: all of it runs only on a requested frame.
- **D3D11 / D3D12** (runtime#1944) use the same two helpers in their resolve step (`u_cursor_draw_into_zone` with the window dims the renderer scales `zone_3d.rect` by; the scissor is tile box ∩ the returned clip), skipping only on a zones-frame / zone-request mismatch.
- **Reference app.** `cube_zones_{metal,vk}_macos` adopt `dxr::CursorDepth` via
  `test_apps/common/cursor_depth_zones.h`: pick the topmost 3D zone under the pointer, hand the
  helper the pointer in that zone's UV, chain the hint only on that zone's locate.

#### §B as built on D3D11 and D3D12 (in-process; `XR_DXR_cursor_depth` spec v5)

- **Same contract as Metal.** The state tracker hands `comp_d3d11/12_compositor_set_cursor_draw_request`
  the same one-frame `u_cursor_draw_request`; the commit consumes it with one field test. A cursor
  frame never zero-copies. The compositor resolves the request into per-tile corners
  (`u_cursor_draw_corners`) — sprite height in metres ÷ the canvas height in metres (canvas px ×
  the panel's pixel pitch from the window metrics) — and hands them to its renderer before the
  projection pass. The renderer draws them **last in the window-space pass**: after the
  projection, quad and window-space layers, before the compose publish and (D3D12) the atlas's
  final barrier, so before the display processor. One alpha-blended triangle strip per tile
  (straight for the app's sprite, premultiplied for the default look), depth off (no DSV), no
  culling, scissored to the tile (∩ the zone in a display-zones frame — see the zones section
  above). Multi-screen routed frames draw nothing (one-time WARN). `willDraw` is gated in the
  state tracker (`u_cursor_draw_placement_drawable`, runtime#1951) for every backend alike.
- **Shaders.** One shared HLSL pair, `d3d_shared/comp_cursor_shaders.h` (corners in b0 — a
  D3D11 constant buffer, 16 D3D12 root constants on the existing quad root signature), compiled
  with `D3DCompile` on the first cursor frame only, like every other in-process shader here. A
  `DXR_LAYERED` variant samples array layer 0 of a layered sprite. D3D12 builds six pipelines
  (`[straight | straight layered | premultiplied][_SRGB target]`), since a D3D12 PSO bakes its
  blend and RTV format.
- **Colour (ADR-044).** The cursor takes the frame's treatment: on a composing frame it is drawn
  into the private `_SRGB` compose target through the source's format-honest view (decoding), on
  a fast-path frame straight into the encoded atlas through the non-decoding view. A
  non-`_SRGB` app sprite (scene-linear, owes the encode) makes the frame compose; the cursor is
  not counted as a contributing layer, so a fast-path frame blends the sprite's anti-aliased
  edges in encoded space, as on Metal. The default look is uploaded once per shape into a
  TYPELESS texture with a UNORM and an `_SRGB` view, so the white arrow is white either way.
- **Default look (A.6) — the shared `comp_cursor_look`** (the same entry point Metal, Vulkan and
  GL use), whose Windows half is `comp_cursor_look_win.c`: `GetCursorInfo()` → `HCURSOR`, drawn
  with `DrawIconEx` into a 32-bit DIB **twice, over black and over white**, and recovered by
  `u_cursor_draw_unblend` (alpha = 1 − (white − black); the render over black is the
  premultiplied colour). That reads every cursor kind the same way — 32-bit alpha, colour + AND
  mask, monochrome AND/XOR — and turns XOR (invert) pixels into opaque white. Drawn at the size
  the OS draws it on this panel: `GetSystemMetricsForDpi(SM_CYCURSOR, dpi)` with the window's
  `GetDpiForWindow` (passed as the look's backing scale, DPI / 96), or the image's own size when
  larger (accessibility pointer sizes); its `height_pt` is that size in 96-DPI points, so the
  physical size is `height_pt` × DPI / 96 × the panel's pixel pitch. Cached by `HCURSOR` + scale,
  so it re-rasterises only on a shape change. `GetCursorInfo` is global, so the compositor thread
  may call it. While the app has hidden its cursor with `SetCursor(NULL)` — which it does as soon
  as a locate reports `willDraw`, i.e. on most drawn frames — the shape is the **class cursor of
  the window under the pointer**, else the last shape, else `IDC_ARROW`. With
  `DXR_CURSOR_DEFAULT_LOOK=arrow`, or if the cursor cannot be read: the shared classic arrow.
- **App sprite.** Straight alpha, last released image, hotspot as given; `sizeMeters` ≤ 0 = 19 px
  at the window's DPI × pixel pitch (the classic arrow's size, as Metal's 19 pt). D3D12 dips the
  image RENDER_TARGET → PIXEL_SHADER_RESOURCE → RENDER_TARGET around the draw, as for
  window-space layers.
- **Reference apps.** `cube_handle_d3d11_win` / `cube_handle_d3d12_win` adopt v5 the way
  `cube_handle_metal_macos` does (analytic cube hit test → hint; `DISPLAYXR_CURSOR_DEPTH_DRAW`,
  `_SPRITE=custom`, `_UV`, `DISPLAYXR_CURSOR_DEPTH=0`), hiding the OS cursor over the client
  area exactly while `willDraw` (`WM_SETCURSOR` → `SetCursor(NULL)`). Their `app` mode keeps the
  OS cursor and logs the placement; it draws no in-app crosshair.

### Rollout

1. **Groundwork:**
   - the policy in A in `u_cursor_depth` (#1847, rebased onto A);
   - the policy in A in the SDK core, with the stereo-tile cursor folded onto it;
   - the lifted-video cursor (#1908) picks up A by calling `u_cursor_depth`.
2. **B on Metal** with `cube_handle_metal_macos`, in both modes: mirrored OS cursor and a custom
   sprite.
3. **B on D3D11, D3D12, GL, Vulkan and the service**, plus a `displayxr-common` helper for the
   app side: pointer → canvas UV on Win32, Cocoa, X11/Wayland and Android; the request; and
   hiding the OS cursor per the report.
4. **Adoption:**
   - the `cube_*` reference apps;
   - the demos: modelviewer, gaussiansplat, avatar, earthview, mediaplayer;
   - the Unity and Unreal plug-ins;
   - the browser's CSS-cursor feed (C);
   - the shell (Phase 4).

### Not changed

- §0 (opt-in, zero cost otherwise), §1 (division of labour) and §2 (geometry from the views).
- Amendment 1's API. Its policy constants come from A once it calls the shared module.

## Amendment 3 (2026-10-11): measured disparity, an opt-in fallback for stereo content with no depth

**Accepted** (David, 2026-10-11). This is Phase 3b, made concrete.

### Why

Most cursor surfaces have an exact depth source:
- native, Unity and Unreal apps, and three.js / PlayCanvas / Spark pages have a depth buffer
  (a hint point or a submitted depth layer);
- lifted 2D→3D content has the conversion's depth map (Amendment 1).

**Traditional stereo content has none:** SBS / stereo video in the mediaplayer, stereo photos,
stereo camera tiles and legacy stereo apps. Its only depth cue is the disparity between the views
the app submits. Some apps already estimate it; Holocall computes a low-res map for
auto-convergence and can feed it through the hint path (§1) with no runtime work. Apps that have
nothing need the runtime to measure it.

### Decision

1. **Measure a patch, never a map.** The cursor needs one number: the nearest *confident*
   disparity under its footprint. The runtime block-matches a small patch around the pointer
   (≈ 64 × 32 px, the cursor footprint dilated) between the **two outermost views of the
   submitted atlas**, over a bounded disparity range (crossed side; ≈ ±64 px), on the GPU in the
   compositor's cursor pass, before the DP. The result is read back asynchronously a frame or two
   later, the same pattern as Phase 3a and Amendment 1, and **never stalls**.

   Budget: well under 0.1 ms of GPU per measured frame. A full-frame depth map is out of scope:
   it costs orders of magnitude more and the cursor would discard almost all of it.
2. **Work in on-screen pixels.** Stereo media has no scene geometry, so none is invented. The
   measured on-screen disparity (px, panel scale) becomes the content disparity directly, and
   §A.1 applies in pixels:

   `target = measured < 0 ? max(measured − margin_px, cap_px) : 0`

   `margin_px` is A.2's 0.015 eye baselines converted with the panel's pixel pitch, the same
   conversion the web stereo-tile cursor uses. Nothing ever goes behind the glass.
3. **Low confidence ⇒ the screen plane.** A match is used only if it passes all three checks:
   - a distinct best match (best-to-second-best cost ratio);
   - left-right consistency (L→R and R→L agree within 1 px);
   - enough texture in the patch (variance above a floor).

   The content disparity is the nearest **confident** match in the footprint. With no confident
   match (blue sky, a flat wall, repetitive texture), the target is **0**: the cursor sits on the
   screen plane, which under §A.3 means the plain OS cursor, with nothing drawn. The existing
   fast-rise / slow-sink filter smooths the hand-off.
4. **Opt-in, and only while useful (§0).** It is requested with the existing
   `XrCursorDepthSourceDXR` and a new kind, `XR_CURSOR_DEPTH_SOURCE_MEASURED_DISPARITY_DXR`
   (spec v7, no new struct). The runtime measures only on frames that chain the request, while the
   pointer is over the 3D content, and at most once per new submitted frame (a paused video is not
   re-measured). No request means nothing is measured, allocated or read back.
5. **Same algorithm as the web.** The web stereo-tile cursor (`displayxr-web`
   `js/camera/pair-cursor.js` and its matcher) already block-matches page-side. The runtime ports
   its algorithm and constants, so web and native stereo cursors agree. Shared numbers live in
   `u_cursor_depth`.
6. **Where:** every native compositor's cursor pass and the service (IPC / shell clients), so it
   also serves stereo content shown through the service.

### Evidence required before merge

- Cost on the mediaplayer for three cases: cursor off the video, over the video, and extension
  disabled. Frame time and GPU time per frame, at 1080p and 4K SBS.
- Correctness on a stereo photo with known disparities: textured foreground (confident, drawn in
  front), sky (low confidence, OS cursor), and a repetitive pattern (rejected).

### Not changed

§0, §A (front-only, margin, threshold, hybrid anchor) and §B (the runtime draws, the app chooses
the look). This adds a depth source; it doesn't change policy.
