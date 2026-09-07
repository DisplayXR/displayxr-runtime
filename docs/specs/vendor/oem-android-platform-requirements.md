---
status: Active
owner: David Fattal
updated: 2026-09-06
issues: [1038, 1031, 1073, 1087, 1090, 1277]
adr: ADR-036
code-paths:
  - src/xrt/auxiliary/android/android_custom_surface.cpp
  - src/xrt/auxiliary/android/src/main/java/org/freedesktop/monado/auxiliary/MonadoView.java
  - src/xrt/compositor/util/comp_bg2d.c
  - src/xrt/compositor/util/comp_bg2d_capture.c
  - src/xrt/compositor/multi/comp_multi_system.c
  - src/xrt/compositor/multi/comp_multi_weave_android.c
  - src/xrt/compositor/vk_native/comp_vk_native_compositor.c
  - scripts/android-oem-probe.sh
  - scripts/android-sidebyside.sh
  - scripts/android_bg_capture.sh
---

# OEM / ODM platform requirements for an Android 3D display running DisplayXR

## Summary

This is the platform-side contract for an OEM or ODM bringing up an Android
device with a **light-field / autostereoscopic panel** that is to run DisplayXR.
It is written to be actionable without talking to us: every ask states the exact
mechanism, the user-visible consequence of not having it, the degraded fallback
DisplayXR actually ships in its absence, and an acceptance test you can run on a
bench unit.

**The platform asks that can be measured over adb are scripted.**
[`scripts/android-oem-probe.sh`](../../../scripts/android-oem-probe.sh) runs them
in about a minute against an attached device and prints PASS / FAIL / INFO per
ask with the raw evidence — R6, S9, R8 and S4 today, with S2 and S6 reporting the
read-only half and pointing at the manual test. It is read-only with respect to
device configuration: it never installs, uninstalls, roots, or writes a setting
or system property, and it force-stops every app it launched. Run everything
(`./scripts/android-oem-probe.sh`) or one ask at a time
(`--only r6`); it exits non-zero on any FAIL, so it drops straight into a
firmware acceptance gate. Each ask's "Acceptance test" block below gives the
exact invocation and the expected PASS output.

Two things are deliberately kept apart, because they land on different desks:

- **Vendor-SDK asks** — changes inside the display vendor's SDK and its
  pre-installed device services. Most of these are already **written and in
  review upstream**; the OEM's job is to *pick up a firmware image that contains
  them*, not to implement them. Marked **[VENDOR — SOLVED]** or
  **[VENDOR — OPEN]**, with the upstream change listed in §7.

  **Read "SOLVED upstream" precisely.** It means *the fix exists upstream and has
  been proven on a bench unit* — not that it is in a release you can pull. As of
  this revision only **R4** is actually **merged**; R1, R2, R3 and R5 are open
  pull requests. §7 states merged-vs-open per ask, and that column is the one an
  OEM should plan against.
- **Platform asks** — changes that only the OEM/ODM can make, because they live
  in the AOSP fork, in the signing story, in SurfaceFlinger, in the window
  manager, or in the PowerHAL. Marked **[PLATFORM]**. These are the ones that
  genuinely need an engineer on your side.

Tiering:

| Tier | Meaning |
|---|---|
| **REQUIRED** (`R*`) | Without it the device is *broken* for its intended use — an app dies, the 3D is flat, or one app destroys another's session. Not a quality issue. |
| **STRONGLY RECOMMENDED** (`S*`) | Feature-gating. The device works; a whole capability (transparent apps, click-through, multi-panel, frame-pacing headroom) does not. |
| **NICE-TO-HAVE** (`N*`) | Ergonomics, observability, headroom. |

### The asks at a glance

| # | Ask | Tier | Owner | Status |
|---|---|---|---|---|
| **R1** | Vendor display services discoverable by a **neutral intent action** | REQUIRED | VENDOR (+ firmware pickup) | Fix written, proven on device; **PR open** |
| **R2** | **Thread-safe multi-client registration**, per-client eviction in the head-tracking service | REQUIRED | VENDOR (+ firmware pickup) | Fix written, measured on device; **PR open** |
| **R3** | **Per-client tracking engine config** (no global last-writer-wins) | REQUIRED | VENDOR (+ firmware pickup) | Fix written; **PR open** |
| **R4** | Device orientation from **`getRealMetrics()`**, not the window-adjusted metrics | REQUIRED | VENDOR (+ firmware pickup) | **MERGED** upstream 2026-08-27 |
| **R5** | The **multi-client lens/backlight tier is the sole writer**; legacy tiers deprecated | REQUIRED | VENDOR (+ firmware pickup) | Contract verified; deprecation PR open |
| **R6** | **1:1 panel pixels** — no compat scaling, WM bounds == composited layer | REQUIRED | **PLATFORM** | **OPEN — now MEASURED** on the OEM mini-window path |
| **R7** | **Camera arbitration through the tracking service**; no power-gating of the tracking camera | REQUIRED | PLATFORM + VENDOR | Works today — don't regress |
| **R8** | Process / service policy: non-isolated slots, FGS type, freezer, app-op persistence | REQUIRED | **PLATFORM** | Works today — don't regress |
| **S1** | **SurfaceFlinger exclude-uid capture filter** (or a platform-signed capture host) | STRONGLY REC. | **PLATFORM** | **OPEN — headline ask** |
| **S2** | **Per-PIXEL** click-through at full opacity (per-region touchability); *plus*, for apps that must keep a foreground Activity, an untrusted-touch exemption + scoping the per-Activity input sink | NICE TO HAVE | **PLATFORM** | **NARROWED** — per-FRAME click-through at full opacity is SOLVED on stock via a tight touchable overlay (#1110); only per-pixel precision and the foreground-Activity case remain |
| **S3** | ~~Wait-semaphore hook into the vendor interlacer~~ | — | VENDOR | **NOT AN ASK — withdrawn.** The hook already existed; we were passing NULL. Documentation-only PR open |
| **S4** | **ADPF / PowerHAL hint sessions** (`APerformanceHint_createSession`) | STRONGLY REC. | **PLATFORM** | OPEN — unsupported on the reference device |
| **S5** | Ship the **background-capture service** in firmware, auto-started | STRONGLY REC. | VENDOR + firmware | PRs open |
| **S6** | Window **move is atomic with the buffer**; a drag affordance exists | STRONGLY REC. | **PLATFORM** | OPEN |
| **S7** | **Per-display** tracking/lens config (multi-panel) | STRONGLY REC. | VENDOR | OPEN |
| **S8** | A **platform-composed weave** (end state), or — now — a **full-opacity, device-composited privileged overlay** for the runtime's weave satellite | STRONGLY REC. | **PLATFORM** | OPEN — the intermediate form is measured and shipping behind a flag |
| **S9** | Expose the **container scale + composited on-screen rect** per window (or a manifest opt-out from mini-window scaling) | STRONGLY REC. | **PLATFORM** | OPEN — the information, not the capability, is what is missing |
| **N1** | Observability: log the applied engine config **and** the client that set it | NICE | VENDOR | PR open |
| **N2** | Document the **classloader-parent contract**; add an explicit `classLoader` field | NICE | VENDOR | Docs PR open |
| **N3** | Capture protocol: panel extent in header, configurable width/rate, zero-copy path | NICE | VENDOR | Runtime consumer side landed; vendor producer PR open |
| **N4** | Do not force `OVERRIDE_SANDBOX_VIEW_BOUNDS_APIS` | NICE | **PLATFORM** | OPEN |
| **N5** | GPU headroom: context slots, `VK_KHR_global_priority`, timeline semaphores | NICE | **PLATFORM** | OPEN |
| **N6** | Clean teardown in the vendor core (no thread join that never returns) | NICE | VENDOR | **Join hang FIXED** upstream 2026-09-06, hardware-proven and in a released SDK; the teardown `vkDestroyImage` SIGSEGV is still open |

---

## 1. Scope, and the two architectures the platform has to support

DisplayXR is an OpenXR runtime. On Android it renders each application window
into a **canvas**, hands the multi-view atlas to the display vendor's *display
processor* (the interlacer / weaver), and the woven result is presented into the
app's own `Surface`. The runtime never weaves; the vendor SDK does
([ADR-007](../../adr/ADR-007-compositor-never-weaves.md)).

Two deployment shapes exist, and the platform must support **both**
([ADR-036](../../adr/ADR-036-android-per-window-compositor-instances.md)):

- **Architecture A — in-process.** The compositor and the vendor SDK core run
  **inside the application's own process and uid**. This is the target shape: the
  app owns a plain `TYPE_APPLICATION` window, so there is no system overlay, no
  anti-tapjacking clamp, and no cross-process surface hop. Every per-uid platform
  policy (package visibility, camera, permissions) applies to *the app*, not to a
  privileged service. **This is what makes R1 fatal rather than cosmetic.**
- **Architecture C — satellite.** A pre-declared, non-isolated `android:process`
  slot per app hosts the compositor out of process. Used for the browser GPU
  process and as a fallback.

The reference bring-up device for everything measured below is a **1600×2560
portrait-natural panel on Android 13** (referred to as "the reference device"),
run both fullscreen and in the OEM's small-window ("mini window" / recents-card
freeform) mode, in both panel orientations. Where a behaviour is that device's
rather than AOSP's, it is called out — those are the entries an OEM should read
as *"avoid or document"*, not as *"reproduce"*. The two window shapes matter
independently: several asks here are satisfied fullscreen and fail in a small
window, which is the failure mode a demo never shows and a user meets in the
first minute (R6, S8, S9).

---

## 2. REQUIRED

### R1 — Vendor display services must be discoverable by a neutral intent action

**Owner:** vendor SDK + OEM firmware pickup · **Status:** fix written upstream
and **proven on a retail unit**; the pull request is still **open** (§7) ·
**Traces to:** L7 / ADR-036 D5

**Mechanism.** Every pre-installed vendor display service (head tracking, display
configuration, backlight/lens control) must declare a **vendor-neutral intent
filter** in addition to its existing ones:

```xml
<service android:name=".HeadTrackingService" android:exported="true">
    <intent-filter>
        <action android:name="org.displayxr.action.VENDOR_DISPLAY_SERVICE"/>
    </intent-filter>
    <!-- existing filters unchanged -->
</service>
```

A DisplayXR application then declares only:

```xml
<queries><intent>
    <action android:name="org.displayxr.action.VENDOR_DISPLAY_SERVICE"/>
</intent></queries>
```

Explicit-component `bindService()` continues to work once visibility is granted
this way — verified on a retail unit, not assumed.

**Consequence if absent.** Android package visibility is enforced **per calling
uid**. Under Architecture A the vendor core loader runs in the *app's* uid; it
calls `PackageManager.getPackageInfo(<vendor service package>)`, receives
`NameNotFoundException`, **does not clear the pending exception**, and makes one
further JNI call with the resulting NULL jobject. Under CheckJNI that is an
`abort`. The app **dies at `xrCreateSession`**, from inside closed vendor code,
with a stack that names `GetObjectClass` and mentions neither a package nor a
manifest. This is not degradation — it is an unattributable crash in every
third-party app. Without the neutral action the *only* alternative is that every
application on the device hard-codes the vendor's package names in its manifest,
which is unshippable for a neutral SDK.

**DisplayXR fallback (what we ship today).** Three layers, none sufficient alone:
(1) every DisplayXR client APK carries the two literal
`<queries><package android:name="…"/></queries>` lines; (2) the runtime clears any
inherited pending exception and NULL-checks its `Context` before `GetObjectClass`,
so we are never the component that turns someone else's unhandled exception into
our crash; (3) a host-interface slot `android_package_is_visible()` lets the
plug-in probe every package its SDK will bind **before** handing control to the
vendor loader, and fail cleanly with a log line naming the exact missing
`<package>` lines. Once execution is inside the vendor loader nothing is
catchable, so asking first is the only defence.

**If the neutral action is not adopted — the fallback contract.** An OEM that
ships without the `org.displayxr.action.VENDOR_DISPLAY_SERVICE` filters MUST
instead **publish the exact package names** of the vendor display-configuration
service and the vendor head-tracking service in its SDK documentation, and state
that they are a per-app manifest obligation. The names are then not an
implementation detail an app may ignore: because visibility is enforced per
*calling* uid and the vendor loader runs in the app's uid under Architecture A,
**every client APK's own manifest** must carry

```xml
<queries>
    <package android:name="<vendor display-configuration service package>"/>
    <package android:name="<vendor head-tracking service package>"/>
</queries>
```

The runtime APK declaring them is **not** sufficient — visibility does not
inherit across uids. Omitting them aborts the process roughly 150 ms after the
plug-in loads, during head-tracking bring-up, with a CheckJNI
`java_object == null` in `GetObjectClass` and no mention of a package or a
manifest anywhere in the stack. Architecture C (satellite / out-of-process) is
unaffected, because there the vendor core runs in the pre-declared service
process whose manifest already carries the block. This is the whole reason L7 is
a REQUIRED ask rather than an ergonomic one: the neutral action is what lets an
app declare `<queries><intent>` and never name a vendor package
([ADR-036 D5](../../adr/ADR-036-android-per-window-compositor-instances.md)).
The client-side rule is restated for app authors as **INV-11.9** in
[`docs/guides/displayxr-app-rules.md` §11](../../guides/displayxr-app-rules.md#11-android--pixel-exactness-rules).

**Evidence.** Three shipping demo apps hit exactly this abort when run in-process
on the reference device and were fixed only by adding the two `<package>` lines
to their own manifests — `displayxr-demo-modelviewer#97`,
`displayxr-demo-mediaplayer#49`, `displayxr-demo-earthview#42` (all merged
2026-08-22). Three independent apps making the same mistake is the argument that
this cannot be left to per-app diligence.

**Acceptance test.**

```bash
# 1. The action is registered by the firmware's services.
adb shell dumpsys package | grep -B2 -A6 'org.displayxr.action.VENDOR_DISPLAY_SERVICE'
#    expect: head-tracking, backlight-multi-client and config-read-write services listed

# 2. A client APK carrying ZERO vendor package strings still works.
aapt2 dump xmltree app.apk --file AndroidManifest.xml | grep -ci 'com\.<vendor>'   # expect 0
adb install -r app.apk && adb shell am start -n <pkg>/.MainActivity
adb logcat | grep -E 'Successfully initialized in-service library|ClientHello|OnServiceConnected'

# 3. Negative control: the same APK with NO <queries> block at all must abort with
#    NameNotFoundException — proving visibility is genuinely enforced on this build
#    and that (2) is not an artefact of a permissive ROM.
```

---

### R2 — Head-tracking service: thread-safe client registration, per-client eviction

**Owner:** vendor SDK + OEM firmware pickup · **Status:** fix written upstream
and **measured on device**; the pull request is still **open** (§7) ·
**Traces to:** L-a / L-b

**Mechanism.** The head-tracking service must, for N concurrent client processes:

1. **Allocate client ids atomically** — or key the client map by the `IClient`
   binder rather than by a counter. The pre-fix code read-and-incremented a plain
   `int32_t` *outside* the mutex, and inserted with `emplace`, which does not
   overwrite. Two clients that race to a duplicate id leave the second silently
   absent from the broadcast map while holding a valid id and successful outgoing
   calls.
2. **Remove only the connection that went away.** `Service.onUnbind` must not
   call a collective `RemoveAll()`. Per-client removal belongs on a
   `linkToDeath` / binder-death path; collective removal belongs only in
   `onDestroy`.
3. **Log the server-assigned client id on both sides**, so this entire class of
   bug is self-diagnosing from a single logcat.

**Consequence if absent.** Two applications launched together — the normal case
for a multi-window 3D device — can end up with one of them *looking* connected
and receiving **zero** head poses: its 3D renders with a stale or default viewer,
i.e. wrong-eye / double image, with no error anywhere. And any transient unbind
that Android treats as the last one evicts **every** client, so one app closing
takes head tracking away from all the others, cross-process, with no notification
to the survivors.

**DisplayXR fallback.** None at the runtime level — this is entirely inside the
service. The only mitigation we ship is defensive: the display plug-in expires a
latched face after a wall-clock timeout rather than holding the last good pose
forever, which converts "silently frozen head" into "tracking lost", which the
app can at least react to.

**Acceptance test.**

```bash
# Stage both apps frozen, thaw together, 5 runs. Expect DISTINCT client ids every
# run and IDENTICAL delivered frame counts per client.
adb shell 'am start -n A/.Main; am start -n B/.Main'
adb shell 'kill -STOP <pidA>; kill -STOP <pidB>; kill -CONT <pidA>; kill -CONT <pidB>'
adb logcat | grep -E 'ClientHello|ServerHello|client id'

# Kill one; the survivor must keep receiving.
adb shell am force-stop A
adb logcat | grep -E 'Client [0-9]+ has died|OnClientUpdate'   # exactly one client removed
adb shell dumpsys activity services <head-tracking-pkg>        # survivor still AppBindRecord
```

Measured on the reference device after the fix: 5/5 runs distinct ids;
252/252, 939/939, 249/249, 1203/1203, 255/255 frames delivered per client pair;
kill-one leaves the survivor receiving (455 further frames over ~23 s).

---

### R3 — Tracking engine configuration must be per-client, not global

**Owner:** vendor SDK + OEM firmware pickup · **Status:** fix written upstream;
the pull request is still **open** (§7) · **Traces to:** L1

**Mechanism.** Face-detector backend, device orientation, tracked-eye selection,
IPD, face count, frame rate and log level must be **scoped or aggregated per
client**, the way "tracking started" and "preferred fps" already are (`any_of` /
`max`). The pre-fix service re-applied one **global** engine config on every
`ClientHello`, and recomputed nothing on disconnect.

**Consequence if absent.** The last application to start silently reconfigures
tracking **for every other running application**. A media app that asks for a
lower detector rate degrades a game's head tracking; an app that sets a device
orientation breaks everyone else's. It is invisible: nothing logs the applied
values or who supplied them (see N1).

**DisplayXR fallback.** Our display plug-in deliberately **writes no tracking
config at all** — which means we can never be the app that stomps someone else,
but equally means we can never ask for anything (a higher rate, a specific
detector). Any *other* vendor-SDK app on the device still stomps us.

**Acceptance test.** With N1's logging in place: run two clients requesting
different `preferredFps`; the applied aggregate must equal the max, and must
**recompute downward** when the higher client disconnects. A client that
configures nothing must not change the aggregate.

---

### R4 — Device orientation must come from `getRealMetrics()`

**Owner:** vendor SDK + OEM firmware pickup · **Status:** **MERGED upstream**
2026-08-27, validated A/B/A on device — the only ask on this list that is in an
SDK release · **Traces to:** the orientation defect found during
in-process freeform bring-up

**Mechanism.** The vendor SDK's orientation helper must derive the **device**
orientation from `Display.getRealMetrics()` (or the display's `mRotation`), never
from `getDefaultDisplay().getMetrics()`, which is **window-adjusted** in an app
process. It should additionally expose an explicit setter
(`leia_core_set_device_orientation()` in the reference SDK) so a host that knows
better can override.

**Consequence if absent.** A portrait-*shaped* freeform window on a
landscape-oriented panel makes the SDK report `Portrait` for the **device**.
Two things then break at once: the face detector is handed the wrong rotation and
**never finds a face**, and the interlacer's rotation is wrong so the weave
renders **flat**. The user-visible symptom is "windowed 3D apps are 2D and
head tracking is dead", while the identical app fullscreen is perfect. Under
Architecture C the service context has no window so it is immune — which makes
this a defect that appears exactly when you move to the in-process architecture.

**DisplayXR fallback.** There is **no client-side fix**:
`createDisplayContext()` was measured and still flips. All we ship is a tripwire
— the plug-in logs a WARN when the SDK's reported device orientation disagrees
with the panel's real one, so a bug report reads out instead of needing to be
reproduced.

**Acceptance test.** On a landscape-oriented panel, launch an in-process 3D app
in a **portrait-shaped** freeform window. Expect: face detected within 1 s, weave
visibly 3D. Then resize to landscape-shaped and back — behaviour must not change
(the A/B/A that validated the fix).

---

### R5 — The multi-client lens/backlight tier must be the sole writer

**Owner:** vendor SDK + OEM firmware pickup · **Status:** contract verified on
device; the deprecation of the legacy tiers is an open upstream PR ·
**Traces to:** L2 / L3, ADR-036 D7

**Mechanism.** Panel 2D/3D lens state is a **per-window preference aggregated by
the vendor**, never a global command from an app. Concretely:

- The **multi-client** control service is a bind-refcount arbiter: `onBind` /
  `onRebind` == "this client wants 3D"; `onUnbind`, delivered only when the last
  client disconnects, == "nobody wants 3D, flatten the panel". That is already an
  OR-of-votes and it is correct.
- The **legacy** tiers must be deprecated and must never be reachable from an app
  process. The legacy control service forces a **global** `MODE_2D` and
  `stopSelf()` on unbind; the in-app utility path does the same. Either one lets a
  single closing app flatten the panel underneath a still-running 3D app.
- Residual gap (**still open**): a *bound* client cannot say "I am 2D right now".
  A mixed set of 2D and 3D windows is therefore inexpressible. The fix is a
  per-client preference + OR-refcount + an admin force-2D; the desktop SDK's
  switchable-hint protocol ports directly.

**Consequence if absent.** Closing or backgrounding one window drops the whole
panel to 2D under every other 3D app on screen. On the legacy tiers this happens
*globally* and silently.

**DisplayXR fallback.** The runtime treats the lens as a **preference**: a hidden
window releases its preference (drops the refcount) rather than commanding 2D,
and a per-session convergent single-writer serialises it. Verified on the
reference device: refcount 2 → 1 → 2 with the panel never flattening, and 2 → 0
flattening exactly once.

**Acceptance test.**

```bash
# Two 3D windows up.
adb shell dumpsys activity services <backlight-svc>   # expect refCount=2
# Hide one:
adb logcat | grep setBacklightMode                    # must NOT fire; refCount 2 -> 1
# Close both:
adb logcat | grep 'setBacklightMode:false'            # fires exactly once
```

---

### R6 — Freeform / multi-window must map 1:1 to panel pixels

**Owner:** **PLATFORM** · **Status:** **OPEN — and, as of 2026-09-06, measured
rather than inferred** · **Traces to:** report §6b pixel-exactness rules, #1087,
[#1367](https://github.com/DisplayXR/displayxr-runtime/issues/1367), ADR-036 D6

**Mechanism.** A woven frame is an **interlaced** image: view assignment is
per-subpixel. Any resampling between the app's swapchain image and the panel
destroys it. The platform must therefore guarantee, for a DisplayXR window:

1. **No compat scaling.** `WindowState.mGlobalScale` (= `mCompatScale *
   mOverrideScale`) must be exactly 1.0 — no size-compat mode, no letterboxing,
   no `DOWNSCALED` override, no "compatibility treatment to scale windows" of the
   kind desktop windowing introduces. Server-side compat scaling is **invisible to
   the client**, so the app cannot detect or correct it.
2. **`currentExtent` is honoured.** A Vulkan swapchain created with
   `imageExtent == surfaceCapabilities.currentExtent` must be presented
   unscaled.
3. **`getBufferTransformHint()` is honest**, so the app can pre-rotate rather
   than have the composer rotate (and resample) the buffer.
4. **WM bounds equal the composited layer position.** The window rect the app
   reads (`View.getLocationOnScreen()`, which feeds the interlacer's on-panel
   phase origin) must be the position at which SurfaceFlinger actually composites
   the layer.

**Measured, 2026-09-06: the mini-window / recents-card freeform path scales the
whole task.** This is no longer an inference from authoring rules — it was read
straight out of SurfaceFlinger and the window manager on the reference device
(landscape-oriented 2560×1600 panel, Android 13), with an in-process `_hosted`
3D app put into freeform by the **recents-card affordance**, which is the
device's normal user-facing route into a small window:

| Probe | Reading |
|---|---|
| SF task layer transform | `geomLayerTransform (ROT_0) (SCALE TRANSLATE)` — a **scale**, not a resize |
| SF `displayFrame` | `[231 1753 1368 2485]` in natural-portrait display coordinates (1137×732 there) |
| App buffer | **1080×1685** logical, so the on-screen footprint is 732×1137 in the app's own orientation — **scale ≈ 0.677** |
| SF composition | `forceClientComposition=true` — the resample is a **GPU bilinear** filter |
| WM `mBounds` | `Rect(1757, 236 - 2837, 1921)` — 1080×1685 (**logical size**) at a **physical origin**, and the rect **extends past the 2560×1600 panel in both axes** |
| Exposed setting | `settings list global` shows `enable_freeform_support=1` and `force_resizable_activities=1`, and **nothing at all for the scale** |

Three things follow, and they are what make this a REQUIRED ask rather than a
quality complaint:

1. **The weave cannot survive it.** The app presents a correctly interlaced
   buffer and the compositor bilinearly resamples it afterwards. Bilinear
   filtering of an interlaced pattern **is not invertible** — neighbouring views
   are averaged into each other — so there is no pre-compensation an application
   can apply at any precision. The 3D is destroyed, not softened.
2. **The scale is invisible to the app.** The window bounds are a *hybrid*
   (physical origin, logical size), the `Configuration` carries no compat-scale
   field, accessibility bounds are logical-clipped, and no setting or system
   property carries the factor — all probed. See **S9**, which asks for exactly
   this information.
3. **It is an OEM addition, not AOSP behaviour.** Stock AOSP desktop windowing
   (the Android 15 QPR / 16 desktop-mode lineage) composes a resizable app's
   freeform window **1:1**; a whole-task presentation scale is a
   device-specific window-container policy layered on top of it.

**Behaviours an OEM should avoid or explicitly document** — all observed on the
reference device:

- **`am task resize` desyncs WM from SF.** After a task resize, WM bounds
  reported `300,500` while the HWC display frame put the layer at logical
  `(0,0)` — the weave phase referenced ~500 px away from the actual pixels, which
  reads to a user as an **eye swap / inverted depth**. Transient WM↔SF desync;
  a real user drag may not reproduce it. If your WM has this, say so, because it
  makes the obvious test tooling unfaithful (#1087).
- **Freeform with no title bar and no user drag.** On the reference device
  freeform windows have neither, so window placement is **launch-time only** and
  drag-phase behaviour cannot be exercised at all. If you ship freeform on a 3D
  panel, ship a drag affordance (see S6).
- **Freeform gated off by default.** The reference device needed
  `settings put global enable_freeform_support 1` and `force_resizable_activities 1`.
  A multi-window 3D device should ship with freeform available to the user.

**Consequence if absent.** Any global scale ≠ 1 does not make the image soft — it
**destroys the 3D**: the interlace pattern is resampled, views cross-contaminate,
and the panel shows ghosting or collapses to 2D. A WM↔SF position desync puts the
weave's phase origin somewhere other than the pixels, producing inverted or
swapped depth.

**DisplayXR fallback.** An authoring rule set (INV-11.1…11.8) plus a linter that
fails an app which calls `SurfaceHolder.setFixedSize`, declares a fixed
orientation or aspect, uses a `TextureView`, or ships a stale `targetSdk` — i.e.
we remove every *client-side* cause of compat scaling, and cannot address the
server-side ones. For placement we ship a **SIGSTOP-pre-size launch recipe**
(`scripts/android-sidebyside.sh`): `am start --windowingMode 5` twice
(first launch lands fullscreen, the second flips the task to freeform), `SIGSTOP`
the app process *before its surface exists*, `am task resize`, then `SIGCONT` —
so the resize provably precedes surface creation and the desync window is never
entered. For moves we poll `View.getLocationOnScreen()` from a `Choreographer`
callback and re-weave on change, which costs ≥ 1 frame of wrong phase.

For the container-scaled mini window specifically we ship **two different
responses, neither of them a fix**:

- *Weave at the physical size.* The runtime's weave satellite infers the scale
  from the hybrid-bounds tell (the reported rect exceeds the panel while its
  origin lies inside it), then sizes its output and the display processor's phase
  rect in **physical** pixels so the compositor's own resample lands on an image
  that was woven for the pixels it will actually occupy
  ([#1277](https://github.com/DisplayXR/displayxr-runtime/issues/1277) P0/P1,
  `comp_multi_weave_android.c`). This works, and it is **fragile by
  construction**: the factor is a per-device constant, correct only while the
  OEM's mini-window scale stays fixed and static. One firmware tweak to the scale
  silently breaks 3D in every mini window, with no error anywhere.
- *Present 2D.* The in-process hosted path detects the same tell and drops the
  window to 2D rather than show a broken weave
  ([#1367](https://github.com/DisplayXR/displayxr-runtime/issues/1367)); the
  browser takes the same route on its Android surface. Honest, and it means the
  device's own small-window affordance turns 3D off.

**Acceptable implementations** — any one of these closes R6 for the scaled-window
case:

- **(a) Don't scale a resizable 3D app**, or offer an unscaled freeform mode
  beside the scaled mini window. Cheapest, and it matches stock AOSP.
- **(b) Expose the scale and the composited on-screen rect** to the app owning
  the task, so the runtime can size its buffer to the on-screen pixel count and
  the container's scale collapses to 1.0. This is **S9**, and it is the smallest
  change: what is missing is the *information*, not any capability — the vendor
  SDK's interlacer is already told an integer on-panel viewport and screen
  position, and at 1:1 the on-screen rect **is** integral panel pixels, so no
  interlacer API changes. Note the limit: 1:1 sizing only covers a **static**
  scale. A scale that *animates* — mid-resize, mid-transition — changes every
  frame and cannot be chased, so that case still falls to (c) or to the 2D
  fallback.
- **(c) Compose the weave in the platform**, so no app-visible geometry has to be
  correct at all. This is **S8**, the end state.

**Acceptance test.**

```bash
# 1. Scale must be 1.0 for the app's window.
adb shell dumpsys window windows | grep -A20 <pkg> | grep -E 'mGlobalScale|mCompatScale|sizeCompat'

# 2. WM bounds == composited layer frame.
adb shell dumpsys window windows | grep -A5 <pkg> | grep -i bounds
adb shell dumpsys SurfaceFlinger | grep -A8 <pkg>          # compare the layer's frame

# 3. Pixel exactness, objectively: render a 1-px vertical black/white checker,
#    screencap, and confirm the readback is still a 1-px checker (no resampling).
adb exec-out screencap -p > shot.png

# 4. THE MINI-WINDOW TEST. Launch a resizable 3D app, then put it into a small
#    window using the device's own affordance (the recents card / mini-window
#    control), not `am task resize` — the user-facing path is the one that
#    matters. Then:
adb shell dumpsys SurfaceFlinger | grep -A12 <pkg> | grep -E 'geomLayerTransform|displayFrame|forceClientComposition'
#    PASS: no SCALE in the layer transform, and displayFrame dimensions equal the
#          app's buffer dimensions.
#    FAIL: 'SCALE' present, or displayFrame != buffer size -> the weave is being
#          resampled and the 3D is gone.
adb shell dumpsys window windows | grep -A5 <pkg> | grep -i bounds
#    PASS: bounds are physical, and bounds + origin lie INSIDE the panel.
#    FAIL: a rect that extends past the panel is the hybrid-bounds tell of a
#          container scale.
```

A device that passes (3) in fullscreen but fails it in freeform has compat
scaling on the freeform path — that is the exact failure this requirement exists
to catch. Step 4 is the same failure reached through the affordance a user
actually touches, and it is the one the reference device fails today.

**Acceptance test, automated:** `scripts/android-oem-probe.sh --only r6`
(`--app <pkg>` for your own resizable 3D app). It performs step 4 end to end —
launch, recents, the card's freeform affordance — and prints the measurement.
PASS looks like:

```
  SF task #<id> leash transform          (ROT_0) (TRANSLATE)
    leash scale (sx, sy)                 1.000000, 1.000000
    leash displayFrame                   [x0 y0 x1 y1]
    leash forceClientComposition         false
  app layer                              SurfaceView ... (BLAST)#<n>
    displayFrame                         [x0 y0 x1 y1]  = WxH on screen
    buffer / geomLayerBounds             WxH
    measured scale (x, y)                1.0000, 1.0000
    forceClientComposition               false
  WM task bounds                         Rect(...) against a <panel> panel

  PASS -- the app's buffer maps 1:1 to panel pixels in the mini window.
```

Any measured scale other than 1.0000 in either axis is a FAIL and the script
exits non-zero. Both the task leash and the app's own layer are reported,
because the scale and the client-composition fallback land on **different**
layers — the reference device shows `(SCALE TRANSLATE)` with ~0.67 and
`forceClientComposition=true` on the **leash**, while the app's own layer stays
`false`. Note also that SurfaceFlinger reports every frame in
**natural-orientation** display coordinates, so on a landscape-oriented,
portrait-natural panel the frames look transposed against the WM bounds; the
probe detects that and says `(axes swapped)` rather than reporting two
disagreeing scale factors.

---

### R7 — Camera arbitration through the tracking service

**Owner:** PLATFORM + vendor · **Status:** works on the reference device — treat
as a non-regression requirement

**Mechanism.** The head-tracking camera must be opened by **the tracking service
and only the tracking service**, which then broadcasts poses to N app clients.
An *exclusive* camera at the HAL level is perfectly fine — desirable, even —
**because the service owns it**. What must not happen is applications competing
for the camera directly, or the platform handing the tracking camera to a
foreground app's camera request.

Additionally: **do not power-gate or autosuspend the tracking camera.** On a
sibling platform, USB autosuspend crashed the camera firmware outright and had to
be disabled by rule. The tracking camera is a always-on sensor, not a
user-initiated peripheral.

**Consequence if absent.** Only one application can be head-tracked at a time;
the second gets a camera-open failure or silently untracked (flat, or wrong-eye)
3D. On a multi-window 3D device that is the whole product.

**DisplayXR fallback.** None — the runtime never touches the camera by design
(the vendor plug-in receives poses through the service). If the service cannot
serve N clients, N-app 3D is simply unavailable.

**Acceptance test.**

```bash
adb shell dumpsys media.camera | grep -i 'client\|package'   # exactly ONE client: the tracking service
# with two 3D apps running, both must report tracked faces:
adb logcat | grep -iE 'face|tracking'
```

---

### R8 — Process, service and app-op policy

**Owner:** **PLATFORM** · **Status:** works on the reference device — treat as a
non-regression requirement · **Traces to:** report §6b

**Mechanism.** Four platform policies DisplayXR depends on:

1. **Non-isolated, pre-declared process slots.** `isolatedProcess` cannot reach
   SurfaceFlinger or gralloc (sepolicy `isolated_app_all.te`), and
   `bindIsolatedService` / `externalService` are isolated. An out-of-process
   compositor satellite must therefore be a **static, non-isolated
   `android:process`** slot (the same pattern browsers use for
   `SandboxedProcessService0..N`). Do not force isolation on services bound by an
   app in its own package.
2. **Foreground-service type.** Android 14 makes an FGS type mandatory;
   `specialUse` is the only honest type for an always-on compositor or a display
   service. It must be grantable without a Play-policy-shaped justification on a
   device whose *purpose* is this.
3. **Freezer.** A process is frozen at `curAdj >= 900` with a 10 s debounce, and
   **a synchronous binder call into a frozen process kills it**. A bound service
   inherits its client's importance, and DisplayXR binds with
   `BIND_AUTO_CREATE | BIND_IMPORTANT | BIND_ABOVE_CLIENT` precisely for this
   reason. An OEM battery/"app-standby" layer that freezes the **vendor display
   services**, or that ignores `BIND_IMPORTANT`, will kill live 3D sessions.
4. **App-op persistence.** `SYSTEM_ALERT_WINDOW` (and any app-op an overlay path
   depends on) must survive a reinstall/update of the holding package. Silent
   revocation on reinstall costs a full day of misdiagnosis every time it
   happens — we lost one to exactly that. Note also that Android 15 narrows the
   `SYSTEM_ALERT_WINDOW` background-start exemption to a **visible** overlay,
   which affects any service-owned overlay path.

**Consequence if absent.** Satellites that cannot reach the compositor at all
(1); a service that cannot legally run (2); sessions killed mid-frame or a frozen
vendor service taking down its clients (3); transparency and overlay features
that silently stop working after an app update (4).

**DisplayXR fallback.** We already bind with the importance flags, declare static
slots, drop `IBinder`s on unbind so we never make a sync call into a frozen peer,
and re-grant the overlay app-op from our install script with a loud warning. None
of that survives a platform policy that overrides it.

**Acceptance test.** Run a 3D session, background it for 15 minutes with the
screen on, and confirm the session and the vendor services are still alive
(`adb shell dumpsys activity processes | grep -E '<pkg>|<vendor-svc>'`, check
`adj` and frozen state). Update the app in place; confirm the overlay app-op
survives (`adb shell cmd appops get <pkg> SYSTEM_ALERT_WINDOW`).

**Acceptance test, automated (the snapshot half):**
`scripts/android-oem-probe.sh --only r8` reads the runtime service's process
state in one shot — resident, non-isolated, declared foreground-service type,
`oom adj`, and the freezer triple. PASS looks like:

```
  process                            pid <n>, uid <n>, resident
  oom adj                            oom adj: max=1001 curRaw=200 setRaw=200 cur=200 set=200
  foreground services                mHasForegroundServices=true
  freezer                            isFreezeExempt=false isPendingFreeze=false isFrozen=false
  manifest foregroundServiceType     0x12 = mediaPlayback|connectedDevice

  PASS -- resident, non-isolated, holding a foreground service, not frozen.
```

`isFrozen=true` is a FAIL. The 15-minute soak and the app-op-survives-update
check above are still manual — the probe reports the instant, not the policy over
time.

---

## 3. STRONGLY RECOMMENDED

### S1 — A display capture that can exclude the caller's own layer

**Owner:** **PLATFORM** · **Status:** **OPEN — this is the headline OEM ask.**
The runtime-side work it gates is done ([#1073](https://github.com/DisplayXR/displayxr-runtime/issues/1073),
closed 2026-08-21) and, since S3 was withdrawn, cost no longer gates compose-under
either — content correctness is the only thing still waiting on the platform ·
**Traces to:** L10 (superseded) → **L12**, #1073

**Why this exists at all.** A 3D weave assigns views **per subpixel**, while RGBA
carries one alpha **per pixel**. Along a transparent app's silhouette, and inside
the parallax de-occlusion band, no single alpha value is correct — so a
post-weave alpha gate necessarily leaves a **chromatic fringe that widens with
pop-out**. Windows and Linux avoid this entirely by compositing a captured
background **under each view before the weave**. The compose pass and the
delivery slot (`set_background_2d`, DP slot 16) already exist on every DisplayXR
backend including Android. The missing piece is purely a **background-image
producer**, and on Android there is no public API that produces it.

**What does not work** (all verified against AOSP `android13-release` — this
matters, because the obvious phrasings of the ask name APIs that do not exist):

| Attempt | Why it fails |
|---|---|
| `MediaProjection` + `VirtualDisplay` | Captures the composited display *including* our layer → converges to black in a few frames. |
| `FLAG_SECURE` on our window | SurfaceFlinger **blanks** the layer on a non-secure output rather than omitting it — we capture an opaque black rectangle exactly where we need background. |
| `setSkipScreenshot` / `setPrivacySensitive` | `@hide`, blocklisted, and their coverage of the display-mirroring path is unverified. |
| Android 14 single-app capture | Requires fresh user consent per `createVirtualDisplay`, cannot follow z-order or the launcher, needs a `mediaProjection` FGS. |
| `captureDisplay(...)` + `setExcludeLayers(...)` | **Not a real API combination.** `setExcludeLayers` is on `LayerCaptureArgs`, and `captureLayers` needs a display-root `SurfaceControl` that only the window manager owns. |
| `/system/priv-app` + `privapp-permissions` | Grants **nothing**: `READ_FRAME_BUFFER` is `signature\|recents`, `CAPTURE_VIDEO_OUTPUT` and `ACCESS_SURFACE_FLINGER` are `signature`. **None carries `privileged`.** Only the platform signature or the `recents` role hold them. |
| `WallpaperManager.getDrawable()` | Returns the *default* wallpaper on 13 and throws `SecurityException` unconditionally from 14 without `MANAGE_EXTERNAL_STORAGE`. Live wallpapers unreadable. |
| `AccessibilityService.takeScreenshot()` | ~3 Hz rate limit, still captures our own overlay, and repurposing accessibility is a store-policy violation. |
| `screencap` / `screenrecord` | Shell-only — and **not** blind to `TYPE_APPLICATION_OVERLAY`: a `screencap` taken with a transparent overlay up contains the overlay in full. |

So the correct statement of the problem is: **"capture the display except my own
layer" is not expressible from outside `system_server` at any privilege tier.**
More privilege does not fix it; it is an API-shape limit.

**Mechanism — pick one. (b) is much cheaper and is our recommendation.**

**(a) Host the capture in a component that is platform-signed *and* can obtain
the display-root `SurfaceControl`**, exposing
`captureLayers(root, excludeLayers = [caller's SurfaceControl])`. This is the
fully correct primitive, and it is a real platform component.

**(b) Add an exclude-uid predicate beside the existing uid-*inclusion* filter in
SurfaceFlinger's layer traversal, and expose it through `CaptureArgs`.** The
inclusion filter (`CaptureArgs.setUid(long)`, *"skip any surfaces that don't
belong to the specified uid"*) is already implemented and already threaded
through `DisplayCaptureArgs`; the inverse is the same predicate negated. With
it, a platform-signed helper — or any `READ_FRAME_BUFFER` holder of any shape —
can answer `captureBehind(excludeUid = caller)` correctly and completely,
wallpaper included, with no window-manager involvement.

**A trap if you intend the existing uid-inclusion filter to be the answer.**
SurfaceFlinger composites a display screenshot over a fill layer whose alpha
comes from `RenderArea::CaptureFill`, and `DisplayRenderArea` uses **`OPAQUE`**.
Every per-uid capture therefore returns fully opaque — *black* wherever that uid
drew nothing — so a union of several per-uid captures collapses to the **last**
one. Measured: a two-uid union (system-ui + launcher) produced the launcher's
icons and dock in exactly the right place over pure black, wallpaper gone; and a
control union with a uid that owns no layer at all **wiped the wallpaper to
black**, which an empty capture cannot do unless the fill is opaque. Note the
failure mode *looks correct* — right icons, right registration — so RGB
inspection alone can never settle it; only alpha can. If you want the multi-uid
path to work, `CaptureFill` must be `CLEAR` (or flag-selectable).

**Whichever you choose, the delivery shape is settled** and needs no discussion:
`AHardwareBuffer` + release fence + sequence number + **the panel extent the
capture was taken against**, rate-limited server-side (our desktop analogue
throttles to ~15 Hz by default), discovered via the R1 neutral intent action.
A reference implementation of the service and its AIDL exists upstream (§7).

**Consequence if absent.** Transparent 3D applications — the avatar-over-launcher
class, and any app that wants to float 3D content over the live screen — show a
coloured rim along every silhouette that grows with pop-out, and cannot compose
under a **dynamic** background at all. On a rotation, the shipped fallback bakes
whatever was on screen (including our own app) into the backdrop.

**DisplayXR fallback (what we actually ship).** A **capture-once** producer: the
display is captured **before** the transparent window exists, handed to the
runtime over an abstract unix socket (`@displayxr.bg2d`) as a plain RGBA stream
(512×320 at ≤ 10 Hz), and fed into `set_background_2d`. The consumer is a
*listener*, so a producer that never appears, dies or restarts costs nothing —
no frame simply means no background, byte-for-byte the pre-feature path. Because
a whole-display capture of a *running* consumer has **no feedback-free gap**
(measured: the first frame after a rotation already contains our woven content —
SurfaceFlinger keeps the layer's last buffer latched straight through), the
`once` mode re-captures in the window **between one consumer session ending and
the next beginning**, which is where a rotation lands in practice for an
orientation-locked app. That is the "rotation re-shoot with bounded self-bake"
we live with: correct for a static background (the launcher), wrong for an
arbitrary changing one. The exclude-uid filter is the clean cure for both.

**Acceptance test.**

```bash
# With the app's translucent 3D window up and visible:
#   request a capture with excludeUid = <app uid>
# PASS requires ALL of:
#   1. zero app pixels in the returned frame (diff against a capture with the app
#      force-stopped: the band region must be identical);
#   2. the background COMPLETE — wallpaper present, not black. Check the ALPHA
#      channel, not RGB: "nothing drawn" and "drew black" are the same colour;
#   3. run continuously at 10 Hz across a display rotation — no recursion in the
#      de-occlusion band, and the backdrop re-registers within 2 frames.
```

Enable the runtime's backdrop-only debug view during this test so the composed
backdrop is directly readable by `screencap`.

---

### S2 — Per-pixel click-through (and the foreground-Activity case)

**Owner:** **PLATFORM** · **Status:** **NARROWED — no longer needed for
click-through as such.** Measured end-to-end on the reference device (Android 13 /
SDK 33) on 2026-08-20, twice. The first pass, against the Architecture-A avatar's
**Activity** window, found it blocked three ways and turned up a second,
independent blocker the original ask did not cover (*The second blocker* below).
The second pass, against a **tight, full-opacity, TOUCHABLE
`TYPE_APPLICATION_OVERLAY` with no Activity of ours in the foreground task**,
**worked on stock** — full opacity *and* click-through, no grant, no allowlist,
no firmware change (*The measured escape* below). We now ship that.

What is left of this ask is genuinely narrower and should be read as two separate
things: **per-pixel** precision (mechanism 3 — still blocked, still wanted), and
click-through for apps that must keep a **foreground Activity** (mechanisms 1/2/4
— now a nice-to-have, not a blocker).

**Update 2026-08-21 — how far we got without you, and what that leaves.** Human
review of the shipped overlay build made the residual concrete: click-through
works outside the overlay's frame, but every transparent pixel *inside* it still
swallows the tap, and the frame was a full-band 1200×1600 rectangle. Since an
overlay's touchable region **is** its frame (mechanism 3 is blocked for overlay
windows too — the blocklist is per-API, not per-window-type), the only lever left
was to shrink the frame onto the silhouette.

**Correction (2026-09-06): that shrink was tried, and reverted.**
[displayxr-demo-avatar#67](https://github.com/DisplayXR/displayxr-demo-avatar/pull/67)
drove the frame width from the character's bounding box (1200×1600 → 832×1600,
~31 % less screen eaten) on the argument that only the *height* axis feeds back
into the render. That argument held for the character and **not** for its 2D
speech bubble, which is sized as a fraction of the canvas *pixel width* — so
narrowing the frame shrank the bubble, which shrank the union box, which narrowed
the frame again. Human verdict on the staged build was a flickering avatar, a
minuscule bubble and black fringes, and it was reverted the same day
([#68](https://github.com/DisplayXR/displayxr-demo-avatar/pull/68)); the overlay
box is now a fixed, tunable aspect
([#71](https://github.com/DisplayXR/displayxr-demo-avatar/pull/71)).

**The lesson strengthens the ask rather than weakening it.** Under Architecture A
the overlay frame is simultaneously the input region *and* the render canvas, so
shrinking it to shape input changes what is rendered — the two are the same
number, and any app whose layout depends on canvas width has a feedback loop
waiting for it. A **separate** per-region touchable declaration (mechanism 3) has
no such coupling: it shapes input without touching the canvas. So the shipped
dead area is the **full overlay rectangle**, not a tightened bounding box.

Mechanism 3 therefore still buys a **bounded** improvement rather than an
unblocking — the residual is the transparent area of one rectangle. The
only stock alternative is an in-app `AccessibilityService` re-dispatching
misdirected taps ([#1114](https://github.com/DisplayXR/displayxr-runtime/issues/1114)),
which costs a user-visible accessibility grant, ~50–100 ms of added tap latency
and Play-policy exposure — acceptable for a sideloaded or OEM-bundled demo, not
for a shipping product. **That trade is the argument for mechanism 3.** ·
**Traces to:** L13 · **Evidence:**
`displayxr-demo-avatar/docs/android-input-passthrough.md`, PRs
[displayxr-demo-avatar#65](https://github.com/DisplayXR/displayxr-demo-avatar/pull/65)
(blocked) and
[displayxr-demo-avatar#66](https://github.com/DisplayXR/displayxr-demo-avatar/pull/66)
(shipped), trade study
[displayxr-runtime#1110](https://github.com/DisplayXR/displayxr-runtime/issues/1110)

**Mechanism.** Allow a designated package (the runtime, or a signed DisplayXR
application) to present a window that is **fully opaque** *and* passes touches
through everywhere except a declared sub-region. Any one of these satisfies it:

1. **Trusted-overlay bit.** Let the package set the trusted-overlay property on
   its window (`SurfaceControl.Transaction#setTrustedOverlay`, today `@hide` and
   effectively `ACCESS_SURFACE_FLINGER`-gated), which exempts it from the
   untrusted-touch opacity rule.
2. **Untrusted-touch allowlist.** Allowlist the package in the untrusted-touch
   blocking policy (the `input.block_untrusted_touches` /
   `ALLOW_UNTRUSTED_SIMPLE_TOUCH_EVENTS` app-op surface), so an overlay above the
   0.8 opacity threshold is not clamped.
3. **Public per-region touchability.** Promote the per-region touchable API to
   the public SDK, or unblock it for the package:
   `ViewTreeObserver.addOnComputeInternalInsetsListener` +
   `InternalInsetsInfo.touchableRegion`. **Measured on the reference ROM:** the
   setter reflects (`setTouchableInsets(I)V` — *"unsupported, reflection,
   allowed"*) but the field does not — *"`max-target-r`, reflection, **denied**"*
   — so an app targeting API ≥ 31 can never fill the region in. Blocklisted on
   Android 13, not merely 14; the `VMRuntime.setHiddenApiExemptions`
   double-reflection bootstrap is blocked too. No application-side route exists.

4. **Scope the per-Activity input sink** for the package — see below. Without
   this, mechanisms 1–3 are **necessary but not sufficient** for an app that
   keeps a foreground Activity. (An app that does not — see *The measured
   escape* — needs none of 1, 2 or 4.)

**The second blocker — `ActivityRecordInputSink` (new, and decisive).** Android
12L+ parks an `ActivityRecordInputSink` immediately beneath **every** Activity,
expressly to stop touches reaching a *different-uid* activity below. Measured
with the avatar's own window frame deliberately shrunk to a centred 55 % slab:

```
13: name='… avatar_vk_android/.MainActivity',   frame=[0,576][1600,1984]
                                                touchableRegion=[0,576][1600,1984]
14: name='… ActivityRecordInputSink …/.MainActivity',
        inputConfig=NO_INPUT_CHANNEL | NOT_FOCUSABLE, alpha=1.00,
        touchableRegion=[-24000,-15999][27199,16000]      ← whole display
19: name='… QuickstepLauncher',                 frame=[0,0][1600,2560]
```

A tap on a launcher icon **outside** the slab was swallowed: focus stayed on the
avatar, the launcher never reacted, and — the tell — **no `Untrusted touch due to
occlusion` warning was logged**, so this is not the opacity policy at all. The
sink has `NO_INPUT_CHANNEL`, so the dispatcher selects it as the touched window
and drops the event silently, and its region is **display-wide and independent of
our frame** — shrinking the window cannot shrink the sink. (For *backgrounded*
activities the same sink carries `NOT_TOUCHABLE`; ours does not, i.e. ours is
live.)

Two consequences an OEM has to act on:

- **The sink sits in front of the untrusted-touch policy.** Granting mechanism 1
  or 2 alone would change nothing for an Architecture-A app. The ask is therefore
  *both*: an untrusted-touch/trusted-overlay exemption **and** scoping or
  disabling `ActivityRecordInputSink` for the designated package.
- **On stock Android, click-through and an Architecture-A ACTIVITY window are
  mutually exclusive** — by design, not by omission. In-app input is unaffected;
  only cross-uid pass-through to the desktop is blocked. (This was originally
  written as "Architecture A's own-window". That was too strong: the constraint
  is the *Activity*, not the app owning its window — see *The measured escape*.)
- **A merely paused Activity is not enough, and a backgrounded one is.**
  Re-measured for #1110: after `moveTaskToBack(true)` our sink reports
  `NO_INPUT_CHANNEL | NOT_VISIBLE | NOT_FOCUSABLE | NOT_TOUCHABLE` — inert — and
  taps fall through normally, while the Activity (and any thread it hosts) stays
  alive. Anything that brings it back to the foreground (Recents, a re-launch)
  silently re-arms the sink, so an app in this topology has to step back out on
  every `onResume`.

Two collateral findings worth carrying:

- A touch-**modal** window's touchable region is the **whole display** however
  small its frame is (`WindowState.getSurfaceTouchableRegion` widens it unless
  `FLAG_NOT_TOUCH_MODAL` or `FLAG_NOT_FOCUSABLE` is set). Any region or frame
  work is a no-op without that flag; we hit it and it cost a debugging cycle.
- Untrusted-touch occlusion tests the obscuring window's **frame**, never its
  touchable region — so region-shaping alone could never have satisfied the
  opacity rule even if the API had been reachable. An Activity window is
  `touchOcclusionMode=BLOCK_UNTRUSTED`, the mode **no** window alpha softens;
  only `TYPE_APPLICATION_OVERLAY` gets `USE_OPACITY` and its ≤ 0.80 escape.
  (Confirmed on device: the only `USE_OPACITY` window present is the OEM's own
  `FloatAssist`; `maximum_obscuring_opacity_for_touch = 0.8`.)

**Consequence if absent — as originally written, and where it was wrong.** The
original framing was that a transparent 3D overlay must choose between two broken
states:

- **With `FLAG_NOT_TOUCHABLE`** (full passthrough — mandatory for a
  service-owned **full-screen** overlay, or it eats every tap including the
  launcher and system dialogs): Android's anti-tapjacking rule clamps the window
  to **≤ 0.80 alpha**. That is a permanent **20 % background ghost over every
  pixel** of the app, including fully opaque ones, and it dims the weave.
  **Compose-under (S1) does not fix this** — it is a window-policy artefact, so
  the two must not be conflated.
- **Without `FLAG_NOT_TOUCHABLE`**: a **full-screen** overlay consumes every
  touch on the panel. The device is unusable behind the app.

Both bullets are still true **for a full-screen overlay**. The dilemma is an
artefact of the *size*, not of the window type — which is what the measurement
below established. Two corrections worth stating plainly, because both were
carried the other way for a while:

- The 0.80 was **not** only self-imposed in our own `LayoutParams`. The platform
  applies it: requesting `alpha=1.0` on a `FLAG_NOT_TOUCHABLE`
  `TYPE_APPLICATION_OVERLAY` yields `alpha=0.80` in `dumpsys input` and a
  measurable blend on screencap (probe magenta `(255,0,255)` renders as
  `(229,10,210)`). The same window made **touchable** keeps `alpha=1.00` and
  screencaps as exactly `(255,0,255)`.
- `USE_OPACITY` is not a tax `TYPE_APPLICATION_OVERLAY` pays. It is the only
  alpha-based escape hatch that exists; an Activity window is `BLOCK_UNTRUSTED`,
  which no alpha softens. **The overlay type is the permissive one.**

**The measured escape (what we now ship) — a tight, TOUCHABLE overlay with the
Activity out of the foreground task.** Measured for #1110 on 2026-08-20, first
with a throwaway probe APK and then in the avatar itself. Three properties,
each clearing exactly one wall, and none of them a grant:

| Property | Wall it clears |
|---|---|
| **Tight frame** | Untrusted-touch occlusion is evaluated **at the touch point** against the **frames** of the windows above. A window that does not contain the tap contributes nothing — so the 0.80 rule is simply never reached for taps outside it. |
| **Touchable** (no `FLAG_NOT_TOUCHABLE`) | Inside its own frame the overlay is the **touched** window, not an obscuring one, so the opacity policy is never consulted for it and alpha stays 1.00. It is also what stops the platform clamping us to 0.80. |
| **No Activity of ours in the foreground task** | `ActivityRecordInputSink`. `moveTaskToBack(true)` suffices — the sink goes `NOT_VISIBLE|NOT_TOUCHABLE`. |

Probe result on the reference device, launcher in the foreground, overlay 600×600 centred, no
Activity of ours anywhere:

```
name='… com.displayxr.probe', inputConfig=NOT_FOCUSABLE | PREVENT_SPLITTING,
  alpha=1.00, frame=[470,980][1070,1580], touchableRegion=[470,980][1070,1580],
  ownerUid=10483, touchOcclusionMode=USE_OPACITY      ← untrusted, NOT TRUSTED_OVERLAY

tap INSIDE  -> our window receives the MotionEvent, no log line
tap OUTSIDE -> the launcher acts on it (App Center opens), no 'Untrusted touch'
screencap   -> overlay pixels are exactly (255,0,255): ZERO launcher contribution
```

The same three properties in the avatar demo
([displayxr-demo-avatar#66](https://github.com/DisplayXR/displayxr-demo-avatar/pull/66))
reproduce it with the real weave running: `alpha=1.00`,
`frame=[0,680][1600,1880]`, ~40 fps, and a tap at `(430,1006)` — the exact
coordinate #65 recorded as dead — opens App Center.

**Costs, honestly.** `SYSTEM_ALERT_WINDOW` (a user-granted special app access,
reset on every reinstall) and a foreground service, since the Activity has to
leave the foreground task and the in-process vendor face tracker's camera open
would otherwise become a *background* camera access that fails silently
(`foregroundServiceType=camera`). Also measured: while certain system screens are
up (Settings pages that set `HIDE_NON_SYSTEM_OVERLAY_WINDOWS`) the platform
forces the overlay `NOT_VISIBLE`; it returns on its own.

**Nothing from the app-owns-its-surface model is given up.** Window type and
handoff class are **orthogonal axes**: `XR_DXR_android_surface_binding` takes any
`ANativeWindow`, so the app still owns the Surface across background/resume,
still feeds `xrSetAndroidWindowGeometryDXR` per frame (`getLocationOnScreen`
works on any attached view, overlay windows included), and per-uid platform
policy still lands on the app rather than a privileged service.

**What is genuinely still blocked: per-PIXEL click-through.** The escape above is
per-**frame**. A tap on a transparent corner inside the avatar's bounding box
still hits the avatar. Mechanism 3 is what fixes that, and it is blocked for
**overlay** windows exactly as it is for Activity windows — re-measured for
#1110, same `NoSuchFieldException: touchableRegion` at targetSdk 31. The
blocklist is per-API, not per-window-type. A tight frame shrinks the dead area
from the whole panel to the avatar's bounding box, which turns this from a
blocker into a polish item.

**Superseded fallback (kept for the record, and still the no-SAW path).**
**Architecture A with a plain `TYPE_APPLICATION` translucent window** (the theme
alone is silently ignored — it needs `SurfaceHolder.setFormat(TRANSLUCENT)` as
well). No system overlay, so no clamp: the 20 % ghost is gone and transparency is
correct, but click-through is gone too, because a normal app window consumes
touches over its whole rect. This is what the avatar shipped between #64 and #66,
and it remains the automatic fallback when the user has not granted
`SYSTEM_ALERT_WINDOW`. The inert punch-through machinery from #65 (silhouette →
touchable-region bands, `FLAG_NOT_TOUCH_MODAL`) still stands as the executable
form of mechanism 3, for the day it is reachable.

**Acceptance test.**

```bash
# With the exemption in force, a window at alpha 1.0 declaring a NON-EMPTY
# touchable region (e.g. the avatar silhouette):
adb shell dumpsys window windows | grep -A25 <pkg> | grep -iE 'alpha|touchable|trusted'
#   expect: alpha 1.0, no clamp applied, the declared touchable region echoed
# 1. Tap INSIDE the region  -> the app receives the MotionEvent.
# 2. Tap OUTSIDE the region -> the launcher (or app behind) receives it; the
#    DisplayXR app receives nothing.
# 3. screencap: opaque app pixels contain ZERO launcher contribution
#    (with the clamp, an 0.80 blend is trivially measurable against a known
#    background colour).

# AND, for an Architecture-A (Activity-owned) window, the sink must be scoped:
adb shell dumpsys input | grep -A1 'ActivityRecordInputSink.*<pkg>'
#   expect: absent, or NOT_TOUCHABLE, or a touchable region that does not cover
#           the panel. A live sink (NO_INPUT_CHANNEL | NOT_FOCUSABLE with a
#           display-wide region) silently eats step 2 with nothing in logcat.
```

---

### S3 — ~~A wait-semaphore hook into the vendor interlacer~~ — **WITHDRAWN**

**Owner:** vendor SDK · **Status:** **NOT AN ASK.** Retained here because earlier
revisions of this document, and the L-series it came from, asserted the opposite —
an OEM who read those should stop carrying this item. · **Traces to:** L11

**What the ask said, and why it was wrong.** L11 claimed the interlacer's Vulkan
post-process entry point exposed no way to express the compose→weave dependency
on the GPU, so the compose-under pass had to **CPU-wait a `VkFence`** before the
self-submitting display processor sampled the composed atlas. The premise was
false: that entry point has taken a wait semaphore since 2023, and it is honoured —
it is attached to the submit whose command buffer samples the source views. **We
were passing NULL.** The documentation described the parameter in terms that read
as swapchain interop, which is how the misreading happened; correcting the
documentation is the only vendor-side change, and it is an open docs-only PR (§7).

**What it cost, and what fixing it recovered.** Measured on the reference device,
CPU time on the render thread in the compose pass, 300-frame averages, same
binary and same session for the A/B:

| Path | With the CPU fence wait | With the semaphore |
|---|---|---|
| In-process | 1.47–1.50 ms | **0.130–0.150 ms** |
| IPC + service overlay | 2.6–2.9 ms | **0.181–0.198 ms** |

A 10–14× reduction against a ≤ 1 ms bar, with a 60 s soak clean on both paths
(zero `VK_ERROR_*`, zero validation lines). **Compose-under transparency is no
longer gated on cost** — what still gates it is **S1**, the capture that can
exclude the caller's own layer, which is a content-correctness problem and a
platform ask.

**The generalisable lesson for an OEM.** The expensive part of this was not a
missing capability — there wasn't one — it was carrying a wrong reading of a
documented parameter long enough for it to reach a requirements list. Where a
vendor SDK's synchronisation contract is load-bearing, stating it explicitly in
the header is worth more than any new API.

---

### S4 — ADPF / PowerHAL hint-session support

**Owner:** **PLATFORM** · **Status:** OPEN — **unsupported on the reference
device.** Our side of it is finished and the workaround shipped
([#663](https://github.com/DisplayXR/displayxr-runtime/issues/663), closed
2026-06-30); the platform gap is unchanged · **Traces to:** #663

**Mechanism.** Implement the PowerHAL `IPower` **hint session** interface so
`APerformanceHint_createSession()` succeeds and reported actual/target durations
actually steer the governor. On the reference device
`APerformanceHint_getManager()` returns non-NULL but
`APerformanceHint_createSession()` returns **NULL** — the ADPF surface exists and
does nothing.

**Consequence if absent.** A weave-bound render loop has no way to tell the
governor its deadline. The GPU idles between bursts, DVFS downclocks, the weave
gets slower, which idles the GPU more — a measurable feedback loop. On the
reference device this cost roughly a third of the frame rate before it was worked
around (~34 fps against a ~6 ms render).

**DisplayXR fallback.** We **pipeline the weave by exactly one frame**
(`DXR_ANDROID_PIPELINE_WEAVE`, on by default on Android): the fence wait is
deferred to the top of the next frame so the weave overlaps the compositor's
pacing sleep and the GPU stays fed and clocked. That recovered ~34 → ~46 fps and
dropped the app's own render time 6.1 → 4.7 ms — i.e. the workaround *proves* the
clocks were the problem. Android's preferred image count is also raised 2 → 3.
The ADPF code path ships and no-ops gracefully (`dlsym`'d, so it builds under
minSdk 29).

**Acceptance test.**

```bash
# On the device, from a render thread:
#   APerformanceHint_getManager()   -> non-NULL
#   APerformanceHint_createSession() -> NON-NULL   <-- this is the test
# Then: disable DXR_ANDROID_PIPELINE_WEAVE and confirm frame rate is unchanged
# (i.e. the hint session, not the pipelining, is now holding the clocks up).
```

**Acceptance test, automated:** `scripts/android-oem-probe.sh --only s4`. The
runtime already performs exactly the call above and logs the outcome, so the
probe reads the answer out rather than re-implementing it: PASS on
`ADPF: perf-hint session created`, FAIL on `ADPF: createSession failed` or
`no PerformanceHintManager`. It also reports whether the `IPower` AIDL service is
registered at all, and SurfaceFlinger's own `use_adpf_cpu_hint`. If no `ADPF:`
line is in the log buffer the verdict is INFO, not a guess — run an
out-of-process (service-compositor) session and re-read.

---

### S5 — Ship the background-capture producer in firmware, auto-started

**Owner:** vendor + OEM firmware · **Status:** upstream PRs open

**Mechanism.** The capture service described in S1 must ship **inside the
firmware image**, auto-started by its host package (not launched by hand from a
shell), discovered via the R1 neutral action, and feature-flagged so a build
without the platform concession simply omits it. Its wire protocol must carry the
**panel extent the capture was taken against** in the frame header, so a consumer
can drop a capture from the wrong orientation instead of silently stretching it
across the de-occlusion band.

**Consequence if absent.** Compose-under is a lab demo requiring a shell command
per boot, so no shipping application can rely on it.

**DisplayXR fallback.** A developer launch script (`scripts/android_bg_capture.sh`)
that starts the daemon at shell/root uid on an engineering build.

**Acceptance test.** Cold-boot the device, launch a transparent 3D app, and
confirm the backdrop is present with no shell interaction — and that the runtime
logs one line naming the cause if the capture's panel extent disagrees with the
window's.

---

### S6 — Window moves atomic with the buffer; a drag affordance

**Owner:** **PLATFORM** · **Status:** OPEN · **Traces to:** report §6b
window-move finding, ADR-036 D6

**Mechanism.** A *pure* window move (no size change) currently produces **no
`IWindow.resized`** — `WindowFrames.didFrameSizeChange` compares width and height
only. It goes out as a `oneway IWindow.moved`, and the client only updates
`mAttachInfo.mWindowLeft/Top`: no layout, no invalidate, **no public callback**,
and `SurfaceView.positionChanged` derives from `getLocationInWindow()`, which has
not changed. Meanwhile **SurfaceFlinger repositions the layer with the old
buffer**. Two asks:

1. Deliver a **position callback** to the client on a pure move (or make
   `getBufferTransformHint`-style plumbing carry position), so an app can re-weave
   for the new on-panel phase.
2. Make the reposition **atomic with the next buffer** for a window that has
   opted in — the BLAST drag-*resize* path already does this for resizes; a 3D
   panel needs it for moves.

And: if the device ships freeform, ship a **title bar or an equivalent drag
affordance**. On the reference device there is none, so a user cannot move a
window at all and placement is launch-time only.

**Consequence if absent.** Every drag shows a **stale interlace phase for its
entire duration** — the pixels are woven for where the window used to be, so the
3D visibly breaks up while moving and snaps back on release.

**DisplayXR fallback.** Poll `View.getLocationOnScreen()` from a `Choreographer`
callback and re-weave when it changes. That costs at least one frame of wrong
phase per move step and cannot be made exact, because the move and the buffer are
not atomically synchronised. (On desktop the equivalent is solved weaver-side by
phase snapping in the window procedure; Android has no such hook.)

**Acceptance test.** Drag a 3D window across the panel by touch and record the
screen with an external camera: the weave must remain correct throughout, with no
break-up during motion.

---

### S7 — Per-display tracking and lens configuration (multi-panel)

**Owner:** vendor SDK · **Status:** OPEN · **Traces to:** L6

**Mechanism.** The vendor SDK's orientation helper and configuration are
process-global and ignore `displayId`; the core and interlacer take no display
identifier at creation. For a device with more than one 3D panel — or a 3D panel
plus an external display — configuration must be **per display**, and the core /
interlacer must be told which one they serve.

**Consequence if absent.** Multi-panel 3D is unavailable: a second panel gets the
first panel's calibration, orientation and lens state.

**DisplayXR fallback.** The runtime's multi-display routing exists and is
vendor-agnostic ([ADR-015](../../adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md)),
so the runtime half is ready; it simply has nothing to route to.

**Acceptance test.** With two panels attached, a window on each must weave with
its own panel's calibration and phase, and rotating one must not disturb the
other.

---

### S8 — A weave the platform composes (end state), or a full-opacity privileged overlay (now)

**Owner:** **PLATFORM** · **Status:** OPEN. The intermediate form is measured and
ships behind a flag; the end state is a platform design ask ·
**Traces to:** [#1277](https://github.com/DisplayXR/displayxr-runtime/issues/1277),
R6, S1, S2, S6

**The end state.** The display pipeline should treat **"3D atlas + geometry"** as
a **layer type**, and weave it **at scanout** — downstream of all compositing,
the way HDR and protected layers are already handled. The compositor keeps doing
what it does: scale the window, animate the transition, place the caption,
rotate the display. The weave happens after all of that, against the panel's own
pixel grid, so it is correct by construction for **any** window, at **any** scale,
during **any** animation.

**Why this is necessary and not an optimisation.** Every other ask on this list
is a way of keeping a *pre-composed* weave from being disturbed downstream: R6
asks the compositor not to scale it, S9 asks it to say by how much it will, S6
asks it to move it atomically. Each of those closes one path. None of them can
close the general case, for one structural reason: **bilinear filtering of an
interlaced pattern is not invertible.** Neighbouring views are averaged into each
other, so no pre-compensation exists at any precision — an app cannot render a
buffer that survives an arbitrary resample. And the cases the other asks cannot
reach are real and ordinary:

- **An animated scale.** A window mid-resize or mid-transition changes its scale
  every frame. Even a perfect S9 — the app is told the exact scale — cannot be
  consumed in time to render for it; the app is always a frame behind a moving
  target. Today those frames are simply wrong, or 2D.
- **Any compositor effect** — a shrink into recents, a cross-fade, a magnifier,
  a picture-in-picture. All of them resample.
- **Windows that are not the app's own.** Nothing an application does can make a
  weave correct in a surface it does not own.

Under a platform-composed weave none of these are special cases; they stop being
a list.

**The intermediate form, which is what we ask for now: a platform-privileged
full-panel overlay, composed at full opacity.** The runtime already ships a
**weave satellite** ([#1277](https://github.com/DisplayXR/displayxr-runtime/issues/1277))
that presents the woven result for one or more app windows through a
service-owned full-panel `TYPE_APPLICATION_OVERLAY`. That gets most of the end
state's benefit — the weave is computed after the window transform is known,
against panel coordinates — without any change to the display pipeline. It needs
three things from the platform:

1. **Full opacity.** The overlay must carry `FLAG_NOT_TOUCHABLE`, because input
   has to reach the apps beneath it; stock Android's anti-tapjacking policy then
   clamps it to **0.8 window alpha**, blending 20 % of whatever is underneath
   into the woven output. On a lenticular panel that reads as per-eye crosstalk
   and destroys the weave. There is no app-side workaround: the clamp is applied
   in composition, after the runtime has done everything right.
   *Field-measured on the reference device: the hardware composer reports the
   overlay at `alpha: 204` against the required 255; lifting the clamp restored a
   weave indistinguishable from the in-app golden reference on the same panel.*
2. **Device (HWC) composition.** The overlay is device-composited on the
   reference device today. Forcing it through GPU client composition adds latency
   and re-introduces exactly the resample risk R6 exists to prevent.
3. **Predictable z-order** against system chrome and window captions, so the
   satellite covers the windows it is weaving for and nothing else.

**Acceptable implementations.**

*For the intermediate form (any one):*

- Grant the runtime's overlay **trusted-overlay status** (`setTrustedOverlay`).
- **Per-package exemption** from the obscuring-opacity clamp for the runtime's
  package.
- `maximum_obscuring_opacity_for_touch = 1.0` **scoped to the runtime's
  windows**. The device-global form
  (`settings put global maximum_obscuring_opacity_for_touch 1.0`) works but
  weakens tapjacking protection for every app on the device — a per-package grant
  is the right shape, and the runtime is a natural trust anchor, being the
  component the 3D experience ships through.

*For the end state:* a display-pipeline layer type carrying the multi-view atlas
plus its geometry, woven at scanout by the vendor display processor. This is a
platform + vendor co-design, not something an OEM can adopt unilaterally; the
vendor-side counterpart is filed with the display SDK (§7).

*What the end state's display service must be given* — the contract, derived
from what the vendor interlacer actually reads (reviewed with the display-SDK
maintainers, 2026-09-07), so that "weave at scanout" is a specification and not
a slogan:

- **Per panel, static** (all of it already exists in the served panel
  calibration today): panel resolution and pixel pitch, the panel's natural
  orientation, the lens geometry (lens pitch in pixels, slant, view count, the
  distance-over-pitch term and the centre phase), the correction maps and
  polynomials when present, the RGB sub-pixel shifts, and gamma.
- **Per window, per composite:** the app's **unwoven** multi-view atlas (this
  replaces the woven buffer as the app's output, so the atlas format — tile
  layout, view count, view order — becomes a defined contract instead of an
  implicit agreement between an app and its own interlacer); the composited
  **on-screen rect in panel pixels** (the quantity S9 asks for, now on the
  correct side of the boundary, because the compositor knows it and the app
  never can); and the current display rotation relative to the natural
  orientation.
- **Per frame, at scanout:** the viewer's eye position in panel-relative
  millimetres, **predicted to the scanout instant**. This moves prediction from
  "the app must guess when its frame will show" to "the compositor knows when it
  is showing", which removes a whole class of fixed-latency error — but it means
  the face-tracking service must be reachable by the display service at
  scanout, or the app must forward a timestamped predicted eye position.

The interlacer's mapping is then buffer pixel → unit coordinates → the on-screen
rect → floor to an integer panel pixel → phase from that pixel, the lens
geometry and the viewer distance. Because the panel pixel is computed from the
*final* composited position, every window transform — scale, move, animation —
is correct by construction, which is exactly what no in-app weave can offer.

Two things this does **not** change, stated so nobody re-derives them: a woven
buffer that the compositor scales afterwards is unrecoverable by any interlacer
change (bilinear resampling of an interlaced pattern is not invertible), which
is why the intermediate form sizes the buffer to the on-screen rect instead;
and the existing integer viewport API is sufficient for that intermediate form
— the missing input is the scale (S9), not a new API.

**Consequence if absent.** Without the intermediate form, the satellite path is
unusable on a stock policy — a permanent 20 % ghost over every pixel, which is
worse than no satellite at all. Without the end state, R6, S6 and S9 remain a
permanent maintenance surface: every new window effect the platform ships is a
new way for the weave to be resampled, discovered in the field.

**Acceptance test.**

```bash
# Intermediate form, with the weave satellite presenting:
adb shell dumpsys SurfaceFlinger | grep -A10 'displayxr.*overlay' | grep -iE 'alpha|composition'
#   expect: alpha 255 (not 204), composition DEVICE (not CLIENT)
# Then, by eye on the panel: the satellite's weave must be indistinguishable
# from the same content woven in-app and presented fullscreen.

# End state: run the same 3D window through a resize animation and a transition
# to recents. The weave must stay correct for every frame of the animation --
# which is precisely what no pre-composed weave can do.
```

---

### S9 — Expose the container scale and the composited on-screen rect

**Owner:** **PLATFORM** · **Status:** OPEN · **Traces to:** R6,
[#1277](https://github.com/DisplayXR/displayxr-runtime/issues/1277) P1,
[#1367](https://github.com/DisplayXR/displayxr-runtime/issues/1367)

**Mechanism.** When the platform presents a task at other than 1:1 — the OEM
"mini window" / recents-card freeform mode that scales a whole task through a
SurfaceFlinger leash — the app owning that task must be able to learn its
**effective presentation scale, or equivalently its true physical on-screen
rect**, and be notified when either changes.

**This ask is for information, not capability, and that is what makes it cheap.**
The vendor SDK's interlacer is already told an **integer on-panel viewport and
screen position** — that parameter exists precisely so the weave can be placed at
the correct absolute panel position. At 1:1 the composited on-screen rect **is**
integral panel pixels, which is exactly the shape that API already expects. So a
client that learns it is being composited to 732×1137 renders at 732×1137, the
container's scale becomes 1.0, there is no resample, and today's interlacer weaves
it correctly with **no API change on either side**. The work is plumbing a number
that SurfaceFlinger already holds out to the app that needs it.

**Its limit, stated up front.** 1:1 sizing covers a **static** container scale.
An **animated** scale — mid-resize, mid-transition — changes per frame and cannot
be chased by a client rendering a frame ahead. That case needs **S8** (a
platform-composed weave) or the 2D fallback; S9 does not claim it.

**Consequence if absent.** A weave maps individual subpixels to lenticular
lenses, so it must know the window's physical pixel footprint *exactly* — being
wrong by a few pixels per thousand lands subpixels on the wrong lens. The scale
is invisible by every route probed on the reference device (see R6's table:
hybrid bounds, no `Configuration` field, logical-clipped accessibility bounds, no
setting or property). The runtime therefore **infers** it from the hybrid-bounds
tell and applies a per-device constant — correct only while the OEM's mini-window
scale stays fixed. One firmware change to that scale silently breaks 3D in every
mini window, with nothing in any log to say why. That fragility is what this ask
removes.

**DisplayXR fallback.** The inference described above
(`comp_multi_weave_android.c`, `weave_satellite_effective_scale()`): the tell is
that the caller-reported rect **exceeds the panel** while its origin lies inside
it — app-visibly unique to a container-scaled window on this platform — after
which a per-device constant is applied. Its direction of failure is documented in
the code: an *unscaled* freeform window dragged off-edge would trip the same tell
and be wrongly scale-woven. That is a fallback, not a design.

**Acceptable implementations** (any one):

- Expose the **effective presentation scale** of a task to the app owning it —
  via a queryable API or the window `Configuration` — with change notifications.
- Report the task's **true physical bounds** in `WindowMetrics`.
- Guarantee **1:1 presentation (no leash scale)** for packages that request it —
  e.g. a manifest property by which an app that declares `resizeableActivity`
  opts out of mini-window scaling.

**Acceptance test.**

```
Launch a resizable 3D app via the device's recents freeform / mini-window
affordance. PASS requires EITHER:
  (a) the app's buffer maps 1:1 to panel pixels -- no SCALE in the layer
      transform, displayFrame dimensions == buffer dimensions; OR
  (b) the app can READ the scale (or its true on-screen rect) from a documented
      API, the value matches SurfaceFlinger's displayFrame, and a change to the
      window delivers a notification before the next frame is rendered.
```

**Acceptance test, automated:** `scripts/android-oem-probe.sh --only r6,s9`
(S9 reads R6's measurement, so run the two together). The check is always
**INFO** — it reports what each app-visible source says, and the ask is closed by
(a) `r6` passing, or (b) one of those sources carrying the number. On a device
that satisfies neither it prints:

```
  dumpsys window: scale fields         0 occurrence(s)   <- the server-side scale is not reported at all
  WM bounds reported to the app        Rect(...)   (origin physical, size logical -- a hybrid)
  actual composited rect               [x0 y0 x1 y1]
  effective scale, measured            0.6704, 0.6700
  settings global (freeform)           enable_freeform_support=1 force_resizable_activities=1
  system properties                    <none carrying the scale>
```

---

## 4. NICE-TO-HAVE

**N1 — Observability in the tracking service.** Log the **applied** engine config
*values* and the client id that supplied them, on both the server and client
sides. Today an aggregate config change is untestable from a logcat, which is why
R3 could only be verified as a non-regression at first. Cheap; makes an entire
bug class self-diagnosing. *(Vendor; PR open.)*

**N2 — Classloader-parent contract.** The vendor core loader builds its
`DexClassLoader` with the loader `Context`'s classloader as the **parent**. That
single property is what lets the DisplayXR runtime host the vendor Java glue so
that application APKs carry **zero** vendor classes and zero vendor `.so` files.
Treat it as a **documented, supported contract** rather than an implementation
detail, and ideally add an explicit `classLoader` field beside `context` on the
core-load struct, so an Activity can keep serving Activity-typed calls
(orientation limits, permission dialogs) without a second `Context`. *(Vendor;
docs PR open, ABI-changing field deferred.)*

**N3 — Capture protocol polish.** Panel extent in the frame header — the runtime
consumer side has landed, the vendor producer PR is still open (§7);
configurable capture width and rate — the shipped default of 512 px against a
1600 px panel is a 3.1× downscale and reads as a soft patch inside the
de-occlusion band; and the zero-copy `AHardwareBuffer` delivery path for when the
producer is in-platform and rate starts to matter. *(Vendor.)*

**N4 — Do not force `OVERRIDE_SANDBOX_VIEW_BOUNDS_APIS`.** With it on,
`View.getLocationOnScreen()` returns **window-relative** coordinates, which
silently zeroes the weave's on-panel phase origin. If the ROM enables it by
default, the opt-out
(`PROPERTY_COMPAT_ALLOW_SANDBOXING_VIEW_BOUNDS_APIS = false`) must be honoured.
*(Platform.)*

**N5 — GPU headroom.** Firmware-bounded GPU context slots are a real ceiling for
N concurrent compositors (order 4–31 depending on the GPU family); expose
`VK_KHR_global_priority` and degrade gracefully on `NOT_PERMITTED`. And Android's
`SYNC_FD` semaphores are **binary and temporary-import only** — a timeline path
would remove a per-frame import from every cross-process weave. *(Platform.)*

**N6 — Clean vendor-core teardown.** *(Vendor. The first half is **fixed
upstream and released**; the second is still open.)*

**The join hang is solved.** Core release used to join an internal thread that
never exits — the input-noise measurement thread of the eye-pair predictor looped
`while(true)` with no stop condition while the predictor's destructor joined it
unconditionally — so the SDK's core-release entry point
(`leia_core_release()` in the reference SDK) blocked in `pthread_join` forever
and an in-process host had to `_exit()` rather than shut down cleanly. It was present
in every SDK drop from the one that introduced the noise-adaptive filter, and
enabled by default on Android, so **every** Android consumer had an unjoinable
predictor. The user-visible shape was an app that never draws again after an
activity relaunch — the recents freeform toggle was the reliable way to reach it:
`NativeActivity.onDestroy` blocks the Java main thread waiting for `android_main`,
which is inside `xrDestroySession` → core release, ending in
`APP TRANSITION TIMEOUT` and an ANR.

The upstream fix applies the pattern the same destructor already used for its
*output* noise thread — an atomic stop flag set before the thread starts, checked
by the loop, cleared before the join. It **merged 2026-09-06 and ships in the SDK
release cut the same day** (§7), and it is hardware-proven: on the reference
device the release now returns in **~1.07 s** (bounded by one measurement period,
because the loop checks the flag after its sleep) and the relaunch that used to
wedge completes normally. The runtime side ships the matching defence for
unfixed cores still in the field: the display plug-in bounds the core-release
call with a **3 s watchdog** that detaches, skips the library release and leaks
the core rather than let a vendor shutdown hang block `xrDestroySession` — the
plug-in release that carries it is named in §7.

**What remains open** is the other half: an intermittent SIGSEGV in
`vkDestroyImage` during interlacer release. Architecture C hides it behind
process death; Architecture A cannot.

---

## 5. Cross-reference: the L-series

The asks were originally tracked as an "L-series" of vendor limitations on
[#1038](https://github.com/DisplayXR/displayxr-runtime/issues/1038). Mapping, and
what is genuinely still open:

| L | Subject | Here | Owner | Status |
|---|---|---|---|---|
| L1 | Global last-writer-wins tracking config | **R3** | Vendor | Fix written; **PR open** |
| L2 | Backlight is a binary bind-refcount | **R5** | Vendor | Contract verified; "bound but 2D" residual open |
| L3 | Legacy backlight tiers force global 2D | **R5** | Vendor | Deprecation PR open |
| L4 | One core per process | — | — | **Not asked** — only the rejected Architecture B needed it |
| L5 | Core release joins a thread that never exits | **N6** | Vendor | **FIXED** — merged upstream 2026-09-06 and released; measured ~1.07 s to release on the reference device |
| L6 | No multi-display | **S7** | Vendor | OPEN |
| L7 | Services discoverable only by package name | **R1** | Vendor + firmware | Fix written, proven on device; **PR open** |
| L8 / L9 | Desktop-SDK phase origin / per-process calibration | — | — | Desktop SDK only; out of scope for an Android OEM |
| L-a | Client-id race in registration | **R2** | Vendor | Fix written; **PR open** |
| L-b | `onUnbind` evicts every client | **R2** | Vendor | Fix written; **PR open** |
| L10 | "`captureDisplay` + `setExcludeLayers`, or priv-app" | **superseded by L12** | — | Both halves were **wrong**; see S1 |
| L11 | "No wait-semaphore into the interlacer" | **S3** | Vendor | **CLOSED — the premise was wrong.** The hook existed; we passed NULL. No SDK ask; docs-only PR open |
| L12 | No exclude-uid filter in SurfaceFlinger | **S1** | **PLATFORM** | **OPEN — headline** |
| L13 | Per-region touchability is reflection-blocklisted (per-API, overlay windows too) → no **per-pixel** click-through. Full-opacity **per-frame** click-through needs no vendor change: a tight, TOUCHABLE `TYPE_APPLICATION_OVERLAY` with no foreground Activity delivers it on stock (#1110, shipped in avatar#66). `ActivityRecordInputSink` only bites apps that keep a foreground Activity | **S2** | **PLATFORM** | **NARROWED** — needed only for pixel-precise regions / Activity-window apps |
| — | Orientation from window-adjusted metrics | **R4** | Vendor | **MERGED** upstream 2026-08-27 |

---

## 6. What is *not* an ask

Stated explicitly, so an OEM does not spend effort on them:

- **Per-channel alpha, or any other post-weave transparency scheme.** The
  subpixel-vs-pixel mismatch is structural; a post-weave gate can only relocate
  the error. Compose-under (S1) is the only correct fix.
- **A shared-surface / single-compositor overlay as the *default* app model.**
  DisplayXR's Android model is one compositor instance **per window**
  ([ADR-036](../../adr/ADR-036-android-per-window-compositor-instances.md) D1).
  **Revised 2026-09-06:** an earlier revision said no platform support was needed
  here at all, on the grounds that the shared-surface overlay was only an optional
  workspace *mode*. That is no longer accurate — the weave satellite is a
  service-owned full-panel overlay, it is how a container-scaled or animated
  window can be woven correctly at all, and it **does** need the opacity and
  composition guarantees in **S8**. What remains not-an-ask is making it the
  default app model; per-window compositors stay the default.
- **Root, or a permanently unlocked bootloader.** Everything above is expressible
  in a signed retail firmware image. The developer-mode daemons we ship exist only
  because the platform concessions are missing.
- **A DisplayXR-specific HAL.** The runtime talks to the vendor display SDK; it
  never talks to a HAL directly. **S8's end state is not an exception**: the
  weaving layer type it asks for is composed by the *vendor's* display processor
  inside the platform's own pipeline, exactly as HDR and protected layers are —
  DisplayXR gains no HAL surface of its own from it.

---

## 7. Firmware pickup checklist

Everything marked **[VENDOR]** above is code that already exists upstream in the
reference vendor SDK. An OEM's obligation is to ship a firmware image whose
pre-installed display services are built from a version **at or after** the
change, and to keep them updatable.

Statuses below were re-verified against the upstream tracker on **2026-09-06**.
Two changes are **merged and released** — **R4** and the join-hang half of
**N6**; everything else marked "PR" is an **open** pull request, so an OEM
planning a firmware spin should treat those as *available to cherry-pick*, not
*available in a release*.

| Ask | Upstream change (LeiaInc/CNSDK) | Upstream state | Notes |
|---|---|---|---|
| R1 | PR #699 (+ AAR `<queries>` switched to intent form) | **open** | Proven on device; also removes the vendor package names from every consuming app's manifest |
| R2 | PR #697 | **open** | Atomic registration + `linkToDeath` eviction + client-id logging |
| R3 | PR #698 (+ #713 for N1 observability) | **open** | Aggregate held; recomputed on disconnect |
| R4 | PR #716 (issue #715) | **MERGED 2026-08-27** | A/B/A validated on device; present from the SDK release that carries it |
| R5 | PR #714 (issues #702, #703) | **open** | Deprecate legacy tiers; document bind-as-preference |
| S3 | PR #720 | **open**, docs only | **No code change.** Specifies the Vulkan post-process semaphore contract that already existed — the correction that withdrew this ask |
| S5 / S1 producer | PRs #717, #718, #719 | **open** | Capture service, multi-uid composite, protocol v2 (panel extent) |
| S7 | Issue #704 | open | Per-display config |
| S8 | Issue #731 | open | Long-term: weave after the window transform / at scanout — the vendor-side half of the end state |
| S9 | Issue #732 | open | Near-term: expose the composited on-screen rect + container scale per window. Confirms no interlacer API change is needed |
| N1 | PR #713, issue #709 | open | |
| N2 | PR #701, issue #710 | open | |
| N6 | **PR #730** (issue #694) — the core-release join hang; issue #696 — the teardown SIGSEGV | **#730 MERGED 2026-09-06**, released in **CNSDK 0.10.67** (same day); #696 open | The join-hang half needs no cherry-pick — take 0.10.67 or later. Proven on device: release returns in ~1.07 s and the freeform relaunch no longer wedges. Runtime-side watchdog: `displayxr-leia-plugin` [#226](https://github.com/DisplayXR/displayxr-leia-plugin/pull/226), shipped in v2.6.15 |
| — | PRs #700, #712 | open | Build fixes needed to compile the services standalone on NDK 26 |

Two build notes for whoever integrates them, because both cost a day the first
time: the Gradle external-native-build can leave `.cxx/**/CMakeFiles/` empty on a
**fresh** worktree (replay the command in `metadata_generation_command.txt` by
hand, then re-run Gradle); and the head-tracking server library needs a per-target
export symbol list to link standalone on NDK 26.

A validation note: the reference services are release-signed with keys held in
the SDK repository, and their certificate hashes match the ones already on retail
units, so a bench unit can be updated with `adb install -r` — **no platform key,
no OTA, no root, no downgrade flag** — and rolled back with
`pm uninstall -k --user 0 <pkg>`. That is the fastest way to A/B any of the
vendor asks before committing to a firmware spin.

---

## 8. References

- [`docs/adr/ADR-036-android-per-window-compositor-instances.md`](../../adr/ADR-036-android-per-window-compositor-instances.md)
  — the architecture these requirements serve (D1 per-window compositors, D2
  Architecture A, D5 neutral discovery, D6 window origin, D7 lens as preference)
- [`docs/roadmap/android-concurrent-multi-app.md`](../../roadmap/android-concurrent-multi-app.md)
  — the platform survey (§6b constraints, §11 vendor limitations, §14 PoC ladder)
- [`docs/roadmap/android-transparency-compose-under.md`](../../roadmap/android-transparency-compose-under.md)
  — the capture landscape in full, and what the shipped fallback does
- [`docs/architecture/transparency-modes.md`](../../architecture/transparency-modes.md)
  — live/baked × shaped/unshaped vocabulary
- [`docs/specs/vendor/eye-tracking-modes.md`](eye-tracking-modes.md) — the
  MANAGED/MANUAL contract a vendor SDK must honour
- [`docs/specs/vendor/display-processor-interface.md`](display-processor-interface.md)
  — the display-processor vtable the vendor plug-in implements
- [`docs/specs/vendor/multi-display-vendor-requirements.md`](multi-display-vendor-requirements.md)
  — the multi-display half of the vendor contract (S7's runtime side)
- [`docs/guides/vendor-plugin-onboarding.md`](../../guides/vendor-plugin-onboarding.md)
  — zero-to-shipping for a new display vendor
- [`scripts/android-oem-probe.sh`](../../../scripts/android-oem-probe.sh) — the
  scripted acceptance probe for the platform asks above (`--list` for the
  sub-checks, `--only <id>` to run one)
- [`docs/roadmap/android-weave-satellite.md`](../../roadmap/android-weave-satellite.md)
  — the weave satellite (S8's intermediate form) as designed and measured,
  including the container-scale derivation S9 exists to replace
- Issues: [#1038](https://github.com/DisplayXR/displayxr-runtime/issues/1038)
  (the L-series), [#1031](https://github.com/DisplayXR/displayxr-runtime/issues/1031)
  (concurrent multi-app epic), [#1073](https://github.com/DisplayXR/displayxr-runtime/issues/1073)
  (compose-under transparency, closed), [#1087](https://github.com/DisplayXR/displayxr-runtime/issues/1087)
  (WM↔SF desync), [#1090](https://github.com/DisplayXR/displayxr-runtime/issues/1090),
  [#663](https://github.com/DisplayXR/displayxr-runtime/issues/663) (ADPF finding, closed),
  [#1110](https://github.com/DisplayXR/displayxr-runtime/issues/1110) (click-through
  trade study, closed), [#1277](https://github.com/DisplayXR/displayxr-runtime/issues/1277)
  (weave satellite epic), [#1367](https://github.com/DisplayXR/displayxr-runtime/issues/1367)
  (hosted window rect under a scaled container)
