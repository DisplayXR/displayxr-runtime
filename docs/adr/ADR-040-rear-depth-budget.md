# ADR-040: Rear depth budget — the runtime owns the policy, the plug-in owns pixels, the app owns geometry

**Status:** Accepted (2026-09-05) · epic
[#1363](https://github.com/DisplayXR/displayxr-runtime/issues/1363) · introduces
[`XR_DXR_depth_budget`](../specs/extensions/XR_DXR_depth_budget.md) · appends one optional
per-API display-processor slot under the [ADR-020](ADR-020-plugin-abi-compatibility-policy.md)
append-at-end rule · related:
[ADR-003](ADR-003-vendor-abstraction-via-display-processor-vtable.md),
[ADR-019](ADR-019-vendor-plugin-aux-boundary.md),
[ADR-024](ADR-024-raw-vs-render-ready-views.md),
[ADR-029](ADR-029-client-owned-transparent-ipc-present.md)

## Context

### The rule we ship today, and why it exists

A transparent-mode app draws over the desktop. Its content in front of the display plane — the
zero-disparity plane (ZDP) — is unambiguous: negative disparity says "in front", occlusion says
"in front", and the two agree. Content *behind* the ZDP is not. It carries positive disparity,
which says "behind the screen", while it is simultaneously drawn **over** desktop pixels that
sit at exactly zero disparity, which says "in front of them". Disparity and occlusion contradict
each other, and the result reads as broken rather than as depth.

Every shipping transparent app therefore clips its far plane at the ZDP (`far = ez`), so nothing
renders behind the screen at all. The model viewer, the gaussian-splat renderer and the avatar
each hand-roll the same rule.

### Why the rule is stricter than the problem

The conflict needs an occlusion cue to be *readable*, and on a horizontally-interlaced 3D panel
the cue that matters is **horizontal** luminance structure: vertical edges, text, icons, window
borders. A background with no horizontal structure — a flat wallpaper, a vertical gradient,
horizontal stripes — presents nothing for the disparity to contradict. Rear content over such a
background looks correct.

So today's rule is a worst case applied unconditionally. The information needed to relax it —
what the desktop under the app's canvas actually looks like — exists on the machine. It is simply
not on any path that reaches the app.

### Who is allowed to know what

Three parties could act on it, and only one of them should:

- **The app** could capture the desktop itself. That duplicates a capture per app, pays its cost
  per app, and produces a different policy in every app that tries.
- **The display-processor plug-in** already has the pixels on Windows (it captures the
  background for compose-under transparency). But a plug-in deciding what is perceptually
  acceptable puts perception policy in vendor code: it cannot be unit-tested from this repo, and
  two vendors would answer the same question differently.
- **The runtime** owns exactly this class of decision already — it is where the projection, the
  view rig and the mode policy live, and it is the one layer every app and every vendor shares.

## Decision

**The runtime computes an advisory *rear depth budget* per session per frame and hands it to the
app. The display-processor plug-in supplies pixels and nothing else. The app applies the budget
to its own geometry.**

> The runtime owns the rear-depth **policy**, the display processor owns **pixels**, the app owns
> **geometry**.

"Rear depth budget" means: how far behind the ZDP this app may render right now. Units are **vH**
(virtual display heights — the existing far-offset convention in the demos), with metres carried
alongside for convenience: `0` = clip at the ZDP, `>= 1000` = unrestricted. This vocabulary is
additive; the present / repaint / weave vocabulary is untouched.

The mechanism is four layers, each of which can be reasoned about — and tested — alone.

### 1. Background source — an optional per-API display-processor slot

A new slot, `get_background_preview`, appended at the end of **each per-API DP variant**
(`d3d11`, `d3d12`, `gl`, `vk`; `metal` carries the slot too, though an alpha-native Metal DP may
leave it NULL). It hands back a small BGRA8 **CPU** preview (<= 512 px on the long side) of the
desktop region under the app canvas, plus a monotonic `generation` and the canvas-normalised
rectangle the preview covers. Returning `false` — or leaving the slot NULL — means "no source
right now", which the runtime treats as the conservative case.

It is appended to the *variant*, never to the base `xrt_display_processor`, because a variant
embeds the base by value: growing the base moves every variant slot and misdispatches calls in an
already-built plug-in. That is the silent break ADR-020 exists to prevent. Being purely
appended, this is not an ABI major bump.

The first integration is Leia SR, which already runs a throttled window capture and can produce
the preview as a downsample of a capture it is doing anyway.

### 2. Analysis — vendor-neutral, in the runtime, pure

`u_bg_neutrality` is a pure C function over a BGRA8 buffer and an ROI: no GPU, no OS calls, no
logging, no state. It computes luma `Y = 0.299R + 0.587G + 0.114B` and looks **only at
horizontal** differences `|Y(x+1,y) - Y(x,y)|`. Vertical differences are deliberately ignored —
a vertical gradient is horizontally uniform, and horizontal uniformity is precisely the property
that makes a background depth-neutral. It reports an edge fraction, a worst-column edge density
(which catches a single vertical window border that an area average would drown out), a combined
`cue_energy` in 0..1, and a `neutral` verdict.

Because it is pure, it is unit-tested from fabricated buffers — solid colour, vertical gradient,
smooth horizontal gradient, one 1-px vertical line, a pseudo-text patch — with no display, no
plug-in and no hardware.

### 3. Policy — a hysteretic, ramped state machine in the runtime

`u_rear_budget` is one instance per native-compositor session (in-process) or per service client
(IPC). It consumes the session's transparency, whether the session runs under a workspace
controller, source availability, the latest analysis result, and an env override
(`DXR_REAR_BUDGET` = `clip` | `open` | `auto`, default `auto`).

| State | `farOffsetVH` |
|---|---|
| `UNRESTRICTED_OPAQUE` — the session is not transparent | 1000 |
| `UNRESTRICTED_WORKSPACE` — transparent, under a workspace controller | 1000 (today's behaviour) |
| `OPEN` — transparent, standalone, background neutral | ramps 0 → 1000 |
| `CLIPPED_BUSY_BACKGROUND` | ramps → 0 |
| `CLIPPED_NO_SOURCE` — no preview available | 0 |
| `FORCED` — env override | 0 or 1000 |

Three properties of the dynamics are load-bearing:

- **Asymmetric hysteresis.** Neutral must hold continuously for a dwell (default 400 ms) before
  the budget opens; a single busy sample closes it (default 100 ms). A visible occlusion conflict
  is worse than a missing rear volume, so the machine is biased toward closing.
- **Ramped, not switched.** `farOffsetVH` is time-ramped (ease-out, ~300 ms open / ~150 ms
  close), so the app's clip plane *slides*. Apps apply the value as-is; app-side smoothing would
  fight the runtime's.
- **An unchanged generation is not staleness.** Capture sources deliver only on desktop change, so
  a frozen generation means the last verdict still holds; only the source withdrawing or flagging
  its preview invalid closes the budget.

### 4. App channel — a new extension, not a grown struct

`XR_DXR_depth_budget` (SPEC_VERSION 1). The app chains `XrRearDepthBudgetDXR` on
`XrViewState::next` in `xrLocateViews`, beside `XrViewDisplayRawDXR`, and the runtime fills
`farOffsetVH`, `farOffsetMeters`, the state, and `backgroundCueEnergy` on every locate. State
transitions also raise `XrEventDataRearDepthBudgetStateChangedDXR`.

Enabling the extension is the app's **opt-in**, and it is also the runtime's gate: the preview
fetch and the analysis run only for sessions that enabled it *and* are transparent *and* are
standalone. An app that never asks costs nothing.

Consumers close the loop through one shared helper (`dxr::ClipPolicy` in `displayxr-common`)
rather than each re-deriving the arithmetic, with a fallback that reproduces today's rule exactly
when the runtime lacks the extension.

## Consequences

- **Transparent apps get a rear volume on a quiet desktop** — the thing they currently cannot
  have at all — without any app learning what a desktop is.
- **Nothing regresses when no source exists.** A vendor plug-in without the slot, a platform
  without a capture path, or a session under a workspace controller all resolve to today's exact
  behaviour. The failure mode of every layer is "today".
- **The perceptual rule is testable.** Both `u_bg_neutrality` and `u_rear_budget` are pure and
  hardware-free, so the interesting judgements are covered by `ctest` rather than by an eyeball.
- **Vendors owe pixels, not opinions.** A new vendor implementing the slot inherits the shipped
  policy verbatim; there is no per-vendor perceptual behaviour to reconcile.
- **New cost is bounded and gated.** The preview is produced at the vendor's existing capture
  throttle (<= 15 Hz), analysed at most every 66 ms, over <= 512 px, on the CPU, and only for
  opted-in transparent standalone sessions.
- **Reach is narrow in v1, on purpose.** Only the Windows vendor-DP source exists, and the ROI is
  the whole canvas rather than the region under the model. Each is a tracked follow-up
  ([#1365](https://github.com/DisplayXR/displayxr-runtime/issues/1365)); none changes the layering
  above. The compositor reach has since widened: the API-agnostic half of the runner was factored
  into `comp_rear_budget` and D3D11, Vulkan and D3D12 all drive it, so they run one policy rather
  than three that resemble each other. GL and Metal still report `CLIPPED_NO_SOURCE`.
- **The source follows the weaver, not the graphics API.** Under the hybrid output-device split
  ([ADR-039](ADR-039-one-fill-engine-for-every-tier.md)) a Vulkan session weaves through the **D3D11**
  display processor on the scanout adapter, and that is the plug-in holding the desktop capture —
  so that is the one polled. Because the split is default-ON at every tier, treating it as a
  carve-out would have meant the budget never opening on the shipping Vulkan path at all. The
  policy instance stays on the compositor, so retiring the split mid-session changes which slot is
  asked and nothing else.
- **The app owns WHERE, the runtime still owns WHETHER (v2).** The canvas-wide ROI turned out to be
  the feature's biggest false negative in practice — the first panel run read a busy verdict off an
  *empty* window's own menu bar while the model sat elsewhere. `XrContentBoundsDXR`
  (`SPEC_VERSION` 2) lets the app report where its content projects, and the analysis measures only
  that region, dilated by the band the conflict actually lives in. This does **not** move policy
  into the app: the bounds are a hint about geometry — the one thing the app is the authority on —
  and every way of getting them wrong (absent, stale, malformed, off-canvas) falls back to the
  canvas-wide v1 region, never to "neutral". An unmeasurable region must not be able to open the
  budget.
- **A hint the app can get wrong is a hint the runtime must bound (v2).** `XrContentBoundsDXR` is
  normalised to the app **window's client rect**, and a zoned app that projects in the zone view
  and forgets to rebase through its zone rect reports a rect reaching outside its 3D zone — into a
  Local2D 2D band, whose desktop pixels the content can never occlude. Nothing about that rect is
  malformed, so none of the fallbacks above fire and the wrong verdict reads authoritative
  ([#1365](https://github.com/DisplayXR/displayxr-runtime/issues/1365), seen on the panel with a
  zoned Unity app). So the runtime intersects the region with the union of the frame's **3D display
  zones** — a fact of the frame, not a claim by the app — and a frame with no zones means the whole
  canvas *is* the zone, leaving every full-window app untouched. The consequence worth naming is
  that this **narrows the blast radius of a wrong hint without making the hint optional**: bounds
  entirely outside every 3D zone measure the zone union and log once naming the app's rebase, so
  the app bug stays visible instead of being absorbed into a plausible-looking number.
- **A rectangle is the wrong shape for a character, and the app is the wrong place to fix that.**
  The two ways v2 goes wrong in practice are both *shape* problems, not policy ones: an engine's
  skinned-mesh bounds are the import-time box over the whole animation set (a T-pose with the arms
  out) unless the app opts into per-frame bounds, and unioning scenery into the AABB either widens
  the rect to the whole zone or, with a backdrop quad whose corners sit behind the eye, collapses it
  to "unknown" and the whole-window fallback. Both over-report silently: nothing about the value
  looks malformed. The zone clamp bounds the damage, and §6 names the two traps, but the real answer
  is a **mask-based ROI (v3)** — the runtime already rasterises per-frame masks for the zone wish, so
  the shape is available without the app computing anything new. A larger rectangle is not the fix.
- **One threshold cannot both admit and reject.** `u_bg_neutrality` reports `neutral` as `cue < 1.0`,
  and the panel found a background parked at 0.93–0.97: the dwell was served, the budget opened, the
  next sample crossed 1.0, it closed after the grace, for seconds. The dwell and the close grace are
  hysteresis on the **time** axis and cannot damp that, because both sides of the flap were
  legitimate under their own rule. `open_cue_max` (default 0.85) adds hysteresis on the
  **measurement** axis: a sample must be below it to count toward opening, at or above 1.0 to count
  toward closing, and the band between holds whatever verdict is current. The general lesson is the
  one worth keeping — a state machine whose entry and exit conditions are the *same* comparison has
  no stable region, however much time you put around it.

- **A new advisory value reaching apps means a new way for apps to disagree.** The ramp is the
  mitigation: because the runtime hands over an already-smoothed value and asks apps to apply it
  raw, two apps on one display move their clip planes together.

## Alternatives considered

**The display processor returns a verdict** ("background is busy") instead of pixels. Rejected:
it moves perception policy into vendor code, where it cannot be unit-tested from this repo and
where two vendors will inevitably answer differently. The pixels are the vendor's; the judgement
is not.

**The runtime imports the display processor's shared background texture.** Rejected: it requires
D3D11 resource import plus cross-device fences in four separate runtime compositors, all to serve
a ~15 Hz job. A small CPU `Map` of a downsampled staging texture buys the same information for a
fraction of the machinery.

**Horizontally blur the background under the model** so the conflicting cue is destroyed rather
than measured. Rejected: it does not survive the shipping live+shaped transparency hybrid.
Flat-transparent pixels are alpha-gated to the *live* desktop, so a blur could only ever appear
inside the de-occlusion band — exactly where it does not solve the problem.

**Grow `XrViewDisplayRawDXR` with the new fields** instead of adding an extension. Rejected as a
struct-size ABI trap: a runtime writing the new fields into a struct declared by an app compiled
against the older header writes past the end of that app's allocation.

**Let each app capture the desktop itself.** Rejected: capture cost multiplied per app, and a
different (and differently wrong) policy in each one.

**A binary open/clip with no ramp.** Rejected on the same grounds the frame-pacing work reached:
a clip plane that pops is more objectionable than one that arrives late. The dwell + ramp cost
nothing the state machine was not already tracking.
