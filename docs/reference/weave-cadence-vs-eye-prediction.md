# Weave cadence vs. eye prediction

Four runtime mechanisms shape when and how often a weave happens; two vendor-side mechanisms
(late latching, the eye predictor) shape *which eye position* that weave uses. They are
constantly confused for each other. This file says what each one is, which platforms actually
have it, and — because the answer differs sharply — what that means for the Android/CNSDK
prediction work.

Companion documents:
- [`motion-to-photon-levers.md`](motion-to-photon-levers.md) — every runtime knob, its default, and the per-topology table. **That file is the authority on the levers themselves**; this one is about how they relate to prediction.
- `displayxr-leia-plugin/docs/late-latching.md` — the vendor half of late latching.

## The law

Perceived smoothness on a 3D panel is **cadence stability of fresh weaves**. Every vblank must
carry a weave computed from the current tracked eye position; a vblank carrying a stale weave
freezes parallax and then jumps it. Critically, a *varying* rate of such misses reads as judder
even at a high average rate — measured, a **steady 33 beats an oscillating 28–50**. The eye
grades stability, not average rate (ADR-039 §Context; evidence chain on #1257 / #1264).

Everything below is either serving that law or is a vendor mechanism that people mistake for
something that serves it.

## The four runtime mechanisms

| | What it changes | One-line mechanism |
|---|---|---|
| **Late weave** | *When* the weave runs | Weave just before scanout instead of at submit. VK 96→17 ms, D3D12 62→17, D3D11 32→17. |
| **Repaint** (#868) | *How often* it runs | Re-weave the **last atlas** at display rate with a **fresh eye pose**. App at 15 fps → panel still gets ~60 correctly-phased weaves. |
| **Slot partition** (#1257) | *Who decides when* | Instead of guessing when the app will present, `xrWaitFrame` releases it every Dth vblank and repaint owns the rest. Steady by construction. |
| **Adapter split** (#918 / ADR-039) | *Who executes the weave* | Move weave + present + DP + repaint loop onto a decoupled fill engine with its own device. |

The adapter split is the one whose meaning changed. It began as a hybrid-laptop fix (kill the
cross-adapter copy). ADR-039 established that **the copy was never the load-bearing property** —
the fill arm's *headroom* is: ~400 ticks/s at 1.8–2.3 ms against in-process arms' 100–205/s with
17–23 ms holes. In-process loops had the same gate code and could not execute it. Hence
`same_adapter` stopped being a reason to decline, and single-GPU boxes get the engine too.

## How these differ from late latching

Late latching re-samples the eye position at submit time and **patches the vertex buffer of
frames already queued but not yet executed**.

| | Late latching (vendor) | Repaint + late weave (runtime) |
|---|---|---|
| Unit of work | edits *one already-recorded* frame | produces an **entirely new weave** |
| Window | only what is genuinely in flight | every refresh, unconditionally |
| Precondition | frames must be queued. If the compositor waits on its submit fence, **there is nothing to patch and it is a silent no-op** | none |
| Coverage | D3D11/GL automatic, VK needs our submit hook, **D3D12 is a stub returning `SR_SUCCESS` and doing nothing** | every path that has a repaint loop |
| Verified? | **No — unverified on every backend.** The single positive D3D11 result was withdrawn (the detector measured weaving, not latching), and the dot test cannot discriminate on a hybrid laptop at all | yes, repeatedly, incl. eyeball sign-offs |

The plug-in's own doc draws the conclusion: repaint may make late latching **redundant by
construction**, because the staleness late latching exists to remove is already removed every
refresh.

The same relationship one level up: `DXR_DEFER_PRESENT` was the opposite strategy — deliberately
let frames queue so a re-sampling DP would have something to correct. **Measured null twice**, and
deleted.

## What actually exists per platform

This is the part that is routinely assumed rather than checked.

| Mechanism | Windows | Android |
|---|---|---|
| Late weave | on (DXGI stats / `present_wait`) | **DORMANT — measured.** The code is cross-platform, but pacing needs `VK_KHR_present_wait` and the NP02J's Adreno does not expose it: the runtime logs `presentId/presentWait: false (late-weave pacing)` at session start |
| Repaint (#868) | on | **on** — the `vk_native` repaint loop is platform-neutral; it starts given a target and a queue tier |
| Slot partition (#1257) | opt-in, split-gated | **refused** — `part_tier_ok` sits inside `#ifdef XRT_OS_WINDOWS` (`comp_vk_native_compositor.c:1436`) |
| Adapter split (ADR-039) | **default on** | **absent** — `#ifdef XRT_OS_WINDOWS` |
| #206 forward horizon | on (D3D11, D3D12, split) | **absent** — see below |

### #206 is Windows-only, structurally

The forward horizon is computed in `comp_weave_latency_win.h`, whose entire body is inside
`#ifdef _WIN32`. Its three call sites are `comp_d3d11_compositor.cpp:2386`,
`comp_d3d12_compositor.cpp:3732`, and `comp_vk_native_split.cpp:2253` (the Windows split arm).
**There is no Android call site**, and the CNSDK display processor does not wire the
`set_predicted_scanout` slot at all (`leia_display_processor_cnsdk.cpp:2566–2585`).

So on Android the prediction horizon is entirely CNSDK-internal, and the plug-in's ~30-call
CNSDK surface exposes **no setter for it** — there is no `srWeaverSetLatency` equivalent.

## Why #206 was invisible on Windows — the arithmetic

The horizon the predictor must cover is not the weave latency. It is:

```
H_total = (camera capture → face sample available)  +  (weave → photons)
              the vendor pipeline, ~tens of ms            what we control
```

Late weave cut the second term from 32–96 ms to ~17 ms. Against a camera pipeline of, say, 40 ms
that is 80→57 ms — a ~29 % cut, not the ~60 % the weave-side number suggests in isolation.

**#206 made the horizon accurate; it did not make it short.** It replaced an EMA of a
non-constant quantity (±~8 ms of horizon error per weave at 28–50 Hz breathing) with an exact
per-weave forward value. That is a correctness fix on the *small* term of a sum dominated by the
term we do not control — which is a sufficient explanation for the field observation that #206
produced no visible 3D improvement while **stable weave cadence was the thing that mattered**.

The general form, worth keeping: *a cadence fix changes what the predictor is asked to do; a
horizon fix only changes how accurately it is told to do it.* When the input cadence is
unstable, no horizon accuracy rescues it — which is also why the adapter split "fixed tracking"
without touching tracking at all.

## The Android/CNSDK picture, as built

Established by reading CNSDK source directly (`~/Documents/GitHub/CNSDK`, 0.10.61), not inferred
from the plug-in's call sites.

### What the weave actually consumes: a predicted face POINT

`leia_interlacer::Interlace` does `interlaceParams.face = GetPrimaryFace()`
(`leia/sdk/interlacer.cpp:527`), and that single `vec3` reaches the weave shader as
`_faceX` / `_faceY` / `_faceZ` (`:1309-1311`). **The lenticular phase is driven by one predicted
point, not an eye pair.**

The predicted eye *pair* is fetched on the same call and cached as
`mLeftEyePredicted` / `mRightEyePredicted` — but it reaches only the **calibration** shader
(`_leftEyePred` / `_rightEyePred`, `:1316-1319`), gated on `mCalibrationMode != NONE`. It does not
influence a normal weave.

Three distinct quantities, routinely conflated:

| Quantity | Who consumes it | Reachable from our process |
|---|---|---|
| **Predicted face point** | **the weave** — lenticular phase | **yes**, `leia_core_get_primary_face` |
| Predicted eye pair | calibration shader only | no C-ABI getter |
| Lookaround eyes | the app's rendered parallax | yes, `experimental_leia_core_get_lookaround_eyes` |

`mUseLookaroundFace` (which would make the weave follow the lookaround midpoint instead) defaults
false and is set only from CNSDK's own debug menu — so on our path it is never on.

### The horizon is a fixed device constant

`GetPrimaryFace` obtains its point from
`faceTracking.GetFace(deviceConfig.facePredictLatencyMs, request)` (`interlacer.cpp:2821`).

`facePredictLatencyMs` is **hardcoded 40.0 ms** on Android (`leia/device/android/androidDevice.cpp:156`)
and on Windows (`windowsDevice.cpp:60`), or parsed from a v4 device config's JSON field
`"systemDelay"` (`configVersion4.h:146`).

**CNSDK extrapolates to a constant 40 ms regardless of the actual weave-to-photon time.** At panel
rate that is roughly right. At 20-30 fps the true horizon is both longer and *varying*, so the
predictor systematically under-predicts by an amount that grows and breathes with the app's
cadence.

> **This is a mechanically sufficient explanation for "a slow app gives jittery 3D in spite of the
> prediction": prediction does not rescue a slow app because nothing tells it the app is slow.**
> It is the Android analogue of precisely what #206 fixed on Windows.

### The #206 mechanism ports with no CNSDK change

`facePredictLatencyMs` is **runtime-writable over the public C ABI** —
`leia_device_config_set_f32(LEIA_DEVICE_CONFIG_PROPERTY_FACE_PREDICT_LATENCY_MS, …)`
(`leia/core/deviceConfig.cpp:72-84`). CNSDK's own debug menu drives it as a 0-100 ms "Delay"
slider (`debugMenu.cpp:1304`).

So the Windows #206 design — compute the forward weave-to-scanout horizon from the vblank grid and
push it per weave — transfers directly: same computation, `set_f32` instead of
`set_predicted_scanout`.

**Verify before relying on it:** whether `leia_core_get_device_config` returns a live handle or a
copy, and whether a per-weave write is safe against the reader in `GetFace`. Neither is documented.

### Camera-pipeline timestamps exist and are exported

`experimental_leia_core_get_face_tracking_profiling` yields
`struct leia_headtracking_frame_profiling` (`leia/headTracking/common/types.h:94`):

```c
int64_t cameraExposureTime;     // capture
int64_t faceDetectorStartTime;
int64_t faceDetectorEndTime;
int64_t apiTimestamp;           // engine hands the frame to the user
```

Clock domain is `LEIA_TIMESTAMP_SPACE_SYSTEM` — `SystemClock.elapsedRealtime` on Android, stamped
at the **start of exposure** (`leia/common/types.h:16-31`).

The camera term of `H_total` is therefore directly measurable with **no custom build**, and the
detector's own cost is separable from queueing. The plug-in currently reads none of this: its
frame listener stamps `os_monotonic_get_ns()` at callback arrival, which is delivery time.

### Where a custom build IS required

Only for the predicted eye **pair** — private members with no getter
(`interlacer.private.hpp:541-542`); the exported eye accessors are `get_non_predicted_eyes` and
`get_lookaround_eyes` only. Adding `experimental_leia_core_get_predicted_eyes` is a ~15-line mirror
of the non-predicted one, since `FaceRequest` already carries the fields, and we hold admin on
`LeiaInc/CNSDK` with an unprotected `main` and keystores that let our builds install on the NP02J.
Cheap — but per the table above, **not on the path to the stability question.**

### CORRECTION: the predicted face IS observable — the "structural pred=0" claim was wrong

An earlier draft of this file (and a stale comment in `leia_cnsdk.cpp`) said the core's face
accessors are empty by construction under `LEIA_FACE_TRACKING_RUNTIME_IN_SERVICE`, and concluded
the NP02J could not serve a prediction stream at all. **That is wrong.** From
`FaceTracking::GetFace` (`leia/sdk/faceTracking.cpp:393-532`):

- `predictedPosition` is filled from the predicted pair if `havePredictedEyePair`, **else** from
  `currentFace.point` if `isPointSet`, else `return false`.
- `lookaroundLeft/Right` are filled **only** if `havePredictedEyePair`, else `return false`.
- Both are computed inside the same `if (currentFace.isPointSet)` block.

So `predictedPosition` has a strictly *weaker* precondition than lookaround: **if lookaround
returns true, `get_primary_face` cannot return false.** `pred=0` therefore only ever means *no
face in frame* — never a structural emptiness. The measurement study is runnable on this device.

The trap that produced the wrong conclusion is worth keeping: with nobody at the camera, every
source reads zero, and a `HW_FACE: listener=0 pred=0 nonpred=0` line looks identical to broken
plumbing. It caught two separate sessions on the same day. **Get a face in frame before
concluding anything from those zeros.**

Note also that `HW_FACE` is silent whenever the eye-pair path succeeds — the DP only calls
`get_primary_face` as its fallback. Absence of the line is not absence of tracking.

### Verified on hardware (NP02J, CNSDK 0.10.62, 2026-08-30)

| Finding | Evidence |
|---|---|
| **The horizon really is 40.0 ms** | `HW_PREDICT: weave prediction horizon 40.0 ms (device default)` — read live from the device config, not inferred from source |
| **The horizon is writable and the write lands** | `40.0 ms -> 75.0 ms requested (device reports 75.0 ms after sync)`, verified through a *fresh* config handle; weaving healthy |
| **Late weave is dormant** | `presentId/presentWait: false (late-weave pacing)` |
| **…but the alternative timing source EXISTS** | Device survey: `device extensions=113 \| VK_GOOGLE_display_timing=1 VK_KHR_present_wait=0 VK_KHR_present_id=0`. Adreno offers no present_wait, but it *does* offer `VK_GOOGLE_display_timing` — so late weave on Android is a wiring job, not a hardware limit |
| **Repaint is on tier 1** | `#868: requesting a runtime-owned queue (family 0 index 1)` — Adreno hands us a real second queue, which is the *best* tier |
| **The missing VK layer is a non-issue** | The log says `VK_LAYER_DXR_queue_lock not found`, but the layer is deliberately desktop-only (`if(XRT_HAVE_VULKAN AND NOT ANDROID)`, `targets/CMakeLists.txt:21`) and exists only to serialise submits on **single-graphics-queue** GPUs. It is the fallback for hardware that cannot do what this pad just did. The log line reads as a defect and is not one |

### RESULT: the grid shipped, and it reached panel rate

Implemented on `feat/android-late-weave-display-timing` (five commits) and measured on the
NP02J. The scope below is what was planned; this is what happened.

| Arm | fps | Note |
|---|---|---|
| Open-loop (assumed 60 Hz) | 34.0 / 34.1 / 34.0 | Remarkably stable across runs |
| Grid pacing, panel steady at 59.86 Hz | **59.9** | **Panel rate.** A ~28 fps app, a panel-rate weave |
| Grid pacing, panel boosting to 119.71 Hz | 39.3 / 39.5 | *Worse* than the 60 Hz case — see the clamp |

**Human verdict, after the clamp: _"looks much better than before, fluid and tracking feels
faster."_** That is the acceptance criterion that matters — the five gap-filling repaint variants
in [`motion-to-photon-levers.md`](motion-to-photon-levers.md) all failed on exactly this, not on
their counters.

**Note what was NOT changed to earn "tracking feels faster": nothing about prediction.** No
horizon was pushed, no eye-source touched. Only *when* weaves happen changed — which is the
cadence-over-horizon thesis at the top of this file, confirmed from the other direction.

#### The clamp, and why it is a stability fix

Following the panel's touch-triggered 60 → 120 boost asks for a weave every 8.35 ms while a weave
costs ~21 ms of GPU at these clocks — a ~2.5× overcommit. Submissions back up until a fence inside
the vendor weaver stops signalling, and the session **hangs**. Captured with `debuggerd`:

```
repaint thread  vk_dp_weave_and_present -> process_atlas_weave
                -> libleiaCore-impl -> vkWaitForFences      [BLOCKED]
app thread      vk_compositor_layer_commit_locked -> os_cond_wait  [waiting on it]
```

The boost arm was first measured as merely *slower* and written up as a performance cost with
clamping as future work. That was the wrong severity. The repaint period is now floored at the
assumed one: the grid corrects the period we already planned for (59.86 against an assumed 60.00,
and any mode change *downward* in rate) and refuses to chase a rate with no measured budget behind
it.

#### Latent bug this exposed (unfixed)

`layer_commit` waits on the repaint thread with an **unbounded `os_cond_wait`**. That is what
turned a stalled weave into a frozen app rather than a dropped frame, and it violates the
no-unbounded-work principle in [`workspace-stability.md`](workspace-stability.md) (#925). Any
future weave stall — vendor bug, driver hiccup, lost fence — hangs the app the same way.

#### The panel rate cannot be pinned on this device — three mechanisms, three nulls

The dominant remaining cadence instability is the platform moving the panel between 59.86 and
119.71 Hz on interaction (interval CoV ~34-47% in *every* scheduling arm measured). Every standard
Android way to stop it was tried and **accepted-then-ignored**:

| Mechanism | Layer | Result |
|---|---|---|
| `ANativeWindow_setFrameRate(60, FIXED_SOURCE, CHANGE_FRAME_RATE_ALWAYS)` | native | returns success, panel still switches |
| `WindowManager.LayoutParams.preferredDisplayModeId` | Java | applies with no exception, panel still switches |
| `Settings.System peak/min_refresh_rate` | system | *does* flip `dumpsys display` to mode 4 (60 Hz) at idle — still overridden under touch |

The likely reason, from `getprop`:

```
ro.surface_flinger.use_content_detection_for_refresh_rate  true
ro.vendor.feature.over_scroll_adjust_fps_enabled           true
ro.vendor.feature.zte_feature_fps_control_vk_cmdcount      true
ro.vendor.feature.zte_feature_fps_control_draw_call        true
ro.vendor.feature.zte_feature_fps_control_limit_frame      true      (+4 more)
```

A vendor fps-control stack inspecting **Vulkan command counts and draw calls**, above
SurfaceFlinger's own content detection. It re-decides the rate from observed GPU work every frame,
which is why declaring a content rate and requesting a display mode are both honoured and then
overridden. All read-only props.

**UPDATE — the Settings UI toggle DOES work, and it disproved the hypothesis.** Setting Screen
refresh rate to 60 by hand pins the panel: the grid saw exactly one value, `16.707 ms (59.86 Hz)`,
across a 12-swipe touch-heavy run, against 3-9 switch events in every prior run. **But the jitter
was unchanged** — 34.3 fps, SD 13.80 ms, CoV 47.3%, statistically identical to unpinned. So panel
switching was a real defect and *not* the dominant one; with the panel provably steady the ~47%
interval CoV remains. The claim earlier in this file that pinning was "likely worth more than any
scheduling change" is wrong. Where the variance actually comes from — app frame delivery, the
repaint loop's own scheduling, or GPU contention — is the open question.

**Classification: not fixable from the runtime or the vendor plug-in on this device.** The
practical answer is to RECORD the rate rather than fight it, so a mid-run switch is visible as
itself instead of surfacing as unexplained variance — it is the discriminator between a prediction
error and a cadence discontinuity. The one untried lever is the device's own Settings UI "Screen
refresh rate" toggle, which neither layer can reach programmatically.

Note this is a *device* finding, not an Android one. `preferredDisplayModeId` remains the correct
mechanism and would be expected to work on hardware without this vendor stack.

#### THE ROOT CAUSE: the GPU governor, not the repaint schedule

Measured with `[FRAME_STAGES]` + the per-kind census, same app, same panel (pinned
59.86 Hz), only the GPU governor changed:

| | GPU @ 295 MHz (default) | GPU @ 680 MHz (pinned) |
|---|---|---|
| app frame interval | 51.53 ms (19.4 fps) | **28.45 ms (35.2 fps)** |
| repaints fired | 97 per report | **0** |
| on-screen interval CoV | **61%** | **16%** |
| weaves that are stale replays | half of them | none — `app n` == `all n` |

At the higher clock the app is fast enough that the legacy gate's two-period quiet
window never elapses, so **no repaint ever fires**, and the on-screen stream is simply
the app's own cadence. The 41/18 alternation does not need to be scheduled better; at
adequate clock it does not exist.

That reframes every negative result on this branch. Grid pacing, the slot partition,
the phase hold and fill mode were all attempts to place a repaint that only exists
because the app is slow — and the app is slow because the GPU sits at 295 MHz of a
possible 680. The scheduling work was optimising a symptom.

Per-stage decomposition at default clock also kills the "the weave is expensive" idea:

```
pre 3.03   preflush 1.36   weave 0.39   postwait 3.29   composite 0.84   present 0.52
```

`weave` — the vendor's `process_atlas`, SDK-internal submit and wait included — is
**0.39 ms, about 4% of the fire**, matching the plug-in's own 0.42-0.48 ms measurement
of `do_post_process`. An earlier figure of 7.69 ms for this stage came from a wedged
FORCE run and was degenerate; it is withdrawn. The runtime's own `pre` + `postwait`
are two thirds of the cost.

**So the lever is the governor**, and it is the same class of finding as the
head-tracking service's cpuset confinement: a platform scheduling decision, not
runtime code. ADPF is unsupported on this device (#663), which is why nothing in the
runtime is currently able to ask for the clock it needs.

#### Still open

- **A sustainable-rate budget**, so the boost can be followed rather than refused.
- **The late-weave tier itself.** The grid now exists and is trusted; nothing yet uses it to weave
  *later*, only to pace repaint. That is the latency half, still unclaimed.
- **The forward horizon** (#206 on Android), which the same grid can now compute — and which is
  what would finally give `facePredictLatencyMs` a real number to hold.

### Activating late weave on Android — scope

The hardware has a timing source; the runtime just cannot consume it on the path Android runs.
Three pieces, in order:

1. **Enable the extension.** `VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME` goes in
   `oxr_vulkan.c`'s `optional_device_extensions[]`, next to the `present_id`/`present_wait` pair.
   That list is filtered against what the driver reports, so adding it is safe on devices that
   lack it. Without this the app never enables it and the entry points stay dead — availability
   (what the survey measured) is not the same as *enabled*.
2. **Add a pacing tier** in `comp_vk_native_target.cpp` alongside the `present_wait` one.
   **This is not a drop-in substitution.** `vkWaitForPresentKHR` *blocks* until a present reaches
   glass — a sync point. `VK_GOOGLE_display_timing` is *retrospective*:
   `vkGetPastPresentationTimingGOOGLE` reports when past presents actually landed and
   `vkGetRefreshCycleDurationGOOGLE` gives a measured refresh period. You do not wait on it; you
   build a **vblank grid** from it and sleep to the next slot.
3. **Reuse the Windows model, don't invent one.** That grid-from-retrospective-statistics shape is
   exactly what `comp_weave_latency_win.h` already does with DXGI frame statistics. Porting its
   logic rather than writing a second one gives **#206's forward horizon on Android for free** —
   the same computation, a different source — which is in turn what a real fix for the CNSDK
   horizon would push through `set_f32`.

So the chain closes: display timing → vblank grid → late weave *and* a real per-weave horizon →
a horizon worth writing into `facePredictLatencyMs`. Note the existing consumer of
`vkGetPastPresentationTimingGOOGLE` (`comp_target_swapchain.c`) is the legacy Monado `main`
compositor, which `vk_native` does not use — it is prior art to read, not a path to route through.

### Two regimes that will corrupt a horizon A/B

- **Device motion disables prediction entirely.** `IsExcessiveMotionDetected` gates the whole
  predict block on a device-acceleration threshold; above it CNSDK serves the raw point. Handheld,
  the predicted and un-predicted paths converge and the horizon knob does nothing. **Prop the pad**
  for any horizon characterisation.
- **Low light scales prediction down.** `_eyePairPredictor.predictionScale` is derived from the
  frame's lux against `luxAdaptationThreshold`. Hold lighting constant across arms.

### The build-version trap

The plug-in must be built against the CNSDK the *device* runs, not the newest one on the box.
A 0.10.64-built plug-in loaded fine on a 0.10.62 device — it reported its identity, created the
system, and then **wove nothing at all**, with no version error anywhere in the log. Confirm the
match from the on-device loader line (`[libleiaCore-loader.so] version: 0.10.62 (<hash>)`) and
build against the release whose hash matches. Debug × Debug is also required, per the #1243 ABI
fingerprint guard.

## The measurement plan

Goal: compare the weave's prediction against retrospective ground truth, and split the error into
*horizon error* (extrapolating to the wrong time) and *model error* (right time, wrong
extrapolation). Given the fixed 40 ms constant above, the working hypothesis is that the error is
overwhelmingly the first kind — which is a much easier fix than the second.

### Phase 0 — the horizon sweep (instrument shipped, sweep pending)

`leia-plugin` branch `feat/cnsdk-predict-horizon-probe` adds the knob. It is inert unless the
property is set, and it applies once per session at worker init — never per weave, because the
only call that makes the write land pauses every interlacer's rendering.

```bash
adb shell setprop debug.dxr.leia.predict_ms 75     # 0..200; empty string = device default
adb shell am force-stop <app> ; adb shell am force-stop org.freedesktop.monado.openxr_runtime.out_of_process
# relaunch the app, then:
adb logcat -d | grep HW_PREDICT
```

**Protocol.** Pad **propped, not handheld** (see the motion regime above), lighting constant,
viewer moving laterally and continuously — a stationary viewer cannot produce prediction error
however wrong the horizon is. Sweep `{0, 20, 40, 60, 80}` with the app at panel rate, then repeat
with the app slowed.

**The falsifiable prediction.** At panel rate the optimum should sit near the 40 ms default. As
the app slows, the optimum should move *up* by roughly the added weave→photon time (≈ one frame
period per dropped frame; at ~20 fps expect the best value somewhere near 70–75 ms). **If the
optimum tracks the app's frame rate, the constant is the bug.** If it does not move, the
hypothesis is wrong and the answer is in the model rather than the horizon — go to Phase 1.

**What this cannot settle.** A static sweep tests whether the *mean* horizon is wrong. It cannot
separate mean error from cadence-driven *variance*; both read as judder and have different fixes.

### Prerequisites

1. **Get a face in frame, then confirm the sources are live.** Per the correction above the
   NP02J does serve the prediction stream; `pred=0` means no face, not broken plumbing. Force the
   `HW_FACE` line to appear at all with `debug.dxr.leia.lookaround_eyes=0` (live flip, no restart),
   and restore it to `1` afterwards.
2. **Wire `experimental_leia_core_get_face_tracking_profiling`.** It is already reachable; the
   plug-in simply does not call it. This is what makes the camera term measurable rather than
   assumed.
3. **Bracket the weave with two reads of `leia_core_get_primary_face`** — immediately before and
   after `process_atlas`. The spread between them bounds the fidelity of using that call as a proxy
   for what the interlacer read internally, instead of assuming it.

### Three streams, one clock

`SystemClock.elapsedRealtime` is the join clock, since CNSDK's timestamps are already in it.

| Stream | Row | Source |
|---|---|---|
| **A — ground truth** | `cameraExposureTime`, `faceDetectorStart/EndTime`, `apiTimestamp`, face xyz, roll, eyePoints | frame listener + `get_face_tracking_profiling` |
| **B — prediction** | `t_weave`, predicted face point (before/after bracket) | `leia_core_get_primary_face` around `process_atlas` |
| **C — scanout** | `t_photons` for that weave | present timing, if the Android path can supply it |

### What the analysis computes

- **Actual horizon** `H = t_photons − cameraExposureTime`, and its variance. Compare directly
  against the 40 ms constant CNSDK assumed. The *gap* and the *variance of the gap* are the two
  headline numbers.
- **Prediction error** — predicted point at `t_photons` minus ground truth interpolated to
  `t_photons` from stream A. Only knowable retrospectively, hence an offline join.
- **Decomposition** — re-run the extrapolation offline with the correct per-weave `H`. Error that
  vanishes was horizon error (fix: push the real horizon via `set_f32`); error that survives is
  model error (fix: the predictor itself).
- **Correlation against cadence, not rate** — the hypothesis is that error tracks cadence
  *instability*, not mean weave rate.

### Sessions worth recording

Vary one thing at a time: viewer stationary vs. brisk lateral motion; app at panel rate vs.
deliberately slowed; repaint on vs. off. A stationary viewer cannot produce prediction error
however broken the predictor is — the same trap that invalidated the late-latching dot test.

## See also

- [`motion-to-photon-levers.md`](motion-to-photon-levers.md) — the levers, defaults, per-topology table
- [`../adr/ADR-039-one-fill-engine-for-every-tier.md`](../adr/ADR-039-one-fill-engine-for-every-tier.md) — why headroom, not the copy, is the split's property
- [`../adr/ADR-007-compositor-never-weaves.md`](../adr/ADR-007-compositor-never-weaves.md) — the compositor never weaves; the DP does
- `displayxr-leia-plugin/docs/late-latching.md` — the vendor half, and why its verification is a trap
- `displayxr-leia-plugin/docs/cnsdk-c-abi-surface.md` — the CNSDK calls the Android arm makes
