# ADR-036: Android: per-window compositor instances; the workspace overlay is an optional mode

**Status:** Accepted (direction; implementation tracked in epic #1031, gated on the PoC-0 hardware proof #1032 — nothing here is shipped at the date below)
**Date:** 2026-08-18

## Context

On Windows, N DisplayXR apps run side by side and every one of them is woven. The
isolation is total and requires no runtime arbitration at all: **1 session → 1
compositor → 1 display processor → 1 weaver → 1 vendor context**, with separate
devices, swapchains, present loops and windows. The only thing N apps share is the
per-display *vendor service* tier — the tracking camera, the calibration, and the
lens/backlight state — and those services already arbitrate among their clients.

On Android today exactly one app is visible at a time. The reason is not IPC, not the
vendor SDK, and not the compositor: it is an inherited Monado singleton.
`android_globals` holds **one unkeyed `ANativeWindow`**; `passAppSurface(Surface)`
carries no client id, so the last connecting client overwrites it, and
`clearAppSurface()` invalidates it for everyone. `comp_multi` already iterates *every*
client each frame, rendering each into its own target with its own DP
(`render_per_session_clients_locked`) — the multi-session loop exists; the surface
plumbing does not.

Two further facts, established by the survey in
[`docs/roadmap/android-concurrent-multi-app.md`](../roadmap/android-concurrent-multi-app.md),
change what the answer should be:

1. **Android runs the compositor out-of-process by policy, not necessity.**
   [ADR-025](ADR-025-android-vendor-dp-out-of-process.md) chose the service split
   because Android 11+ package visibility forces the *calling* process's manifest to
   name the vendor packages, which would couple every app to a build-time vendor set.
   That is a real platform rule — but it is a **packaging** rule. The in-process flavor
   still builds, is the only flavor CI builds, and rendered head-tracked vendor 3D
   in-process during the Android bring-up (#499/#507). The Khronos Android loader
   `dlopen`s the runtime into the app process by design.
2. **The vendor stack was built for N clients in N processes.** The head-tracking
   service is genuinely multi-client (client map, broadcast to all, aggregation, no
   cap); the camera is opened once by that service; the backlight service is
   binder-refcounted across processes. What CNSDK *has* that the Windows SDK lacks is a
   per-interlacer **on-panel phase origin** (`leia_interlacer_set_viewport_screen_position`),
   which is exactly the primitive N windows on one panel need. What it lacks is
   per-client tracking config and a per-client backlight preference.

Meanwhile the in-flight roadmap item **#967d / #1006** plans the opposite shape for
Android: a service-owned fullscreen shared surface with **one** DP compositing N
clients at workspace poses. That is a spatial-desktop *overlay*. It is the right shape
when a workspace controller owns the screen; it is the wrong default on a platform
whose OS is already the window manager.

### Options considered

- **A — in-app compositor.** Each app process hosts the session, `comp_vk_native`, its
  swapchains, its DP with one vendor core, its own SurfaceView and its own interlacer
  with its own phase origin. Talks IPC only to the vendor services.
- **B — one multi-session service.** Generalise the existing service: keyed surfaces,
  keyed globals, per-client transition/pause policy, and a plug-in refactor to one
  vendor core driving N interlacers.
- **C — one satellite compositor process per app.** K pre-declared, non-isolated
  `android:process` slots; each satellite runs today's out-of-process code unchanged
  for exactly one client.
- **B′ — shared-surface overlay** (#967d): one service-owned fullscreen surface, one
  DP, N clients composited at workspace poses.

## Decision

Adopt **one runtime abstraction** and pick its Android deployment. Seven decisions:

### D1 — The abstraction is one compositor instance per window

A *compositor instance* owns everything that belongs to a window: swapchains, target
and pacing, the DP/interlacer instance, the window's phase origin, fences, visibility,
and its own Kooima frustum. Instances share **only per-display vendor services**:
the tracking camera and pose stream, the calibration/display config, and the
lens/backlight state (refcounted, never owned).

This is the Windows behaviour stated as an invariant rather than an accident, and it
holds on every platform. Two corrections it makes explicit: the *predictor* is per
instance (per core), not shared; and lens/backlight is a per-instance **preference**
resolved by refcount, never per-instance state.

### D2 — The Android target deployment is in-app (Architecture A)

The compositor, the vendor DP and one vendor core run **in the app's own process**.
This is the Windows model verbatim and the loader's native shape; it is immune to the
process freezer (the compositor *is* the foreground app); it needs no hop for the
per-frame window origin; and it needs no runtime-side arbiter, because the OS is the
window manager and the vendor services already arbitrate what is genuinely
display-global.

**This supersedes ADR-025's "in-process is a development/bring-up flavor only."**
ADR-025's *reasoning* stands unchanged — an app must never carry vendor `<queries>`,
vendor `.so`s or vendor glue — but D5 below satisfies that requirement without moving
the compositor out of the app. ADR-025 carries an amendment note pointing here; its
history is not rewritten.

> **Amendment 1 (2026-08-18) — the spike ran; the Java-glue half is real (#1037).**
> Architecture A was exercised end-to-end on an NP02J: a cube with **zero `com.leia.*`
> classes, zero vendor `.so` and zero vendor package names in its APK** weaved through
> the Leia DP **in its own process**. The mechanism that made it possible is now on
> `main`:
>
> - **`xrt_plugin_host_iface::get_android_class_host_context`** — an Android `Context`
>   whose `getClassLoader()` resolves classes shipped in the *runtime's* APK, carved
>   out of `reserved[]` (no ABI bump, ADR-020) and implemented in
>   `target_plugin_loader.c` with `createPackageContext(<runtime pkg>,
>   CONTEXT_INCLUDE_CODE|CONTEXT_IGNORE_SECURITY)`, mirroring how the runtime already
>   hosts `org.freedesktop.monado.ipc.Client`. Class loading only: Activity-typed
>   vendor calls keep using `get_android_activity`. Contract:
>   [`docs/reference/xrt_plugin_iface.md`](../reference/xrt_plugin_iface.md#the-host-iface).
> - **The `inProcess` runtime flavor now carries the vendor Java-glue AAR** the way
>   `outOfProcess` already did — still gated on `cnsdk.dir`, so default/CI builds stay
>   vendor-free per ADR-019.
>
> Root cause confirmed in vendor source: the core loader builds its `DexClassLoader`
> with `context.getClassLoader()` as the parent, so **the Context is the sole injection
> point** — nothing else had to move.
>
> Two spike findings that change the plan rather than the decision:
>
> - **Main-thread DP creation is not a blocker.** Measured in-process, DP creation ran
>   on the `native_app_glue` `android_main` thread (≠ the process main thread) and the
>   vendor core initialised fine. The real requirement is "the calling thread has a
>   prepared `ALooper` that is pumped afterwards". The genuine gap is an app that calls
>   `xrCreateSession` on a bare pthread render thread (the engine shape).
> - **Multi-window A is blocked on the runtime-spawned SurfaceView.** Staging two apps
>   side by side crashed with `NullPointerException` in
>   `SurfaceView.onAttachedToWindow` → `MonadoView.onAttachedToWindow`: the view is
>   added straight to the `WindowManager`, so it has no `ViewParent` to
>   `requestTransparentRegion` on. This is exactly why a real
>   `XR_DXR_android_surface_binding` (app-provided `ANativeWindow`) has to land and the
>   runtime-spawned SurfaceView demoted to the `_hosted` fallback. Tracked in #1037.
>
> Still open in-process after this amendment: the window-rect feed for
> `comp_vk_native` (the D6 contract in-process), and a `comp_vk_native` audit for
> per-process statics.

### D2, Amendment 2 — the surface binding ships; multi-window A is unblocked (2026-08-18, #1037)

`XR_DXR_android_surface_binding` is
[implemented and specified](../specs/extensions/XR_DXR_android_surface_binding.md).
The app owns and passes its `ANativeWindow`; the runtime-spawned SurfaceView survives
only as the `_hosted` fullscreen fallback, with the `ViewParent` NPE named at the site.
Two runtime functions carry what changes during a session and Android reports to nobody
but the app: `xrSetAndroidSurfaceDXR` (the Surface dies and is reborn on every
background/resume) and `xrSetAndroidWindowGeometryDXR` (**the in-process half of D6** —
window rect + panel extent, once per frame from a `Choreographer` callback).

The in-process compositor consumes that rect twice, exactly mirroring what the
out-of-process path already did: `set_window_screen_rect` before `process_atlas` for
the weave phase (#1033), and `get_window_metrics` for the per-window Kooima canvas +
eye rebase (#1034). Both degrade to the display-scoped behaviour that shipped before
when no rect is published.

Two in-process cubes now weave side by side on the NP02J at their own origins — the
first time Architecture A has run multi-window on hardware. The D6 contract is
therefore satisfied on both deployments.

One consequence worth recording, because it is the inverse of what Amendment 1 removed:
a client APK now carries **zero vendor classes and zero vendor `.so`**, but still needs
two vendor `<queries><package>` lines, because package visibility is enforced per
calling UID and the runtime declaring them does not help. They were previously
invisible — injected by the vendor AAR's own manifest — so removing the AAR is what
exposed them. D5 / L7's neutral intent action removes them for good.

### D3 — Satellite-per-app (Architecture C) is the sanctioned out-of-process deployment

Architecture C is *the same abstraction*, with the process boundary one step out: K
pre-declared `android:process=":dxrN"` slots (non-isolated — an `isolatedProcess`
cannot reach SurfaceFlinger or gralloc), one client each, brokered by the runtime
service. It has three standing jobs:

1. **The PoC vehicle (#1032).** It proves N vendor cores in N processes on hardware
   using today's code — which is Architecture A's only real unknown too.
2. **The fallback.** If the vendor package-visibility convention (D5) stalls, C ships
   concurrency with ADR-025 intact.
3. **The browser host.** `displayxr-browser`'s GPU process should reach a satellite
   over the runtime IPC rather than `dlopen` vendor code inside Chromium.

Roughly 80% of the work — the window-origin feed and DP slot, per-window Kooima,
backlight hygiene, `XR_DXR_weave` on Android, the pixel-exactness rules — is common to
A and C, which is what makes keeping both cheap.

> **Amendment 1 (2026-08-18) — D3 is realised: N slots + a broker.** The PoC-0 gate
> (#1032) passed on an NP02J, and the mechanism it proved has been productised
> (#1031 sub-issue, "satellite compositor process per app"). As built:
> **`MonadoServiceSlot0..N-1`** (`android:process=":dxrN"`, non-isolated, exported,
> same FGS type as `MonadoService`), `N = 4` by default and generated from one Gradle
> constant with a manifest consistency check; assignment is decided by a
> **`SlotBroker` in the runtime's main process**, reached over a dedicated
> `ISlotBroker` binder — *not* `IMonado`, whose stub starts a full runtime server in
> its constructor and would therefore spin up a compositor in a process that owns no
> window. Policy: a package that already owns a slot keeps it, then a pin
> (`debug.dxr.slot` / the client's `com.displayxr.satellite_slot` meta-data) if free,
> then the lowest free slot, then **-1 → the client falls back to the single
> main-process service**, i.e. the pre-slot single-window behaviour. Ownership is
> tracked by `linkToDeath` on a client-supplied token, so a crashed app frees its slot;
> a satellite `stopSelf()`s on `onUnbind` (one client per satellite by construction)
> and hard-exits after a bounded grace period, because the vendor core-release path can
> hang (displayxr-leia-plugin#39). The PoC's package-name hash is gone.

### D4 — The shared-surface path (#967d) is a workspace MODE, never the Android default

The one-surface/one-DP shape is entered **only when a registered workspace controller
activates it**, exactly as on Windows
([`docs/specs/runtime/workspace-controller-registration.md`](../specs/runtime/workspace-controller-registration.md)):
the controller registers, claims the panel lease, and the runtime switches the display
to a single runtime-owned surface it composites N clients into at workspace poses. When
no controller is active — the default on a phone, a tablet or desktop windowing — every
app keeps its own Surface, its own instance and its own DP, and SurfaceFlinger
composites them.

The two shapes coexist by the same rule Windows already uses: a workspace overlay is a
*policy client* of a service-owned mechanism, not the mechanism. #967's D-1/D-2/D-3
(banner truth, bounded waits, GPU work out of `list_and_timing_lock`) are unaffected —
they help every path.

### D5 — Package visibility is a vendor-neutral intent action, not a package name

Vendor display services advertise themselves with

```xml
<intent-filter><action android:name="org.displayxr.action.VENDOR_DISPLAY_SERVICE"/></intent-filter>
```

and every DisplayXR app carries a **single** declaration, supplied by the `displayxr`
AAR's manifest, never hand-written by the app author:

```xml
<queries><intent><action android:name="org.displayxr.action.VENDOR_DISPLAY_SERVICE"/></intent></queries>
```

Matching by intent action grants *package-level* visibility, so this removes both
couplings ADR-025 objected to: **temporal** (a vendor that ships after the app is
compiled is already visible) and **combinatorial** (one declaration, not N vendor SDKs
bundled per app). The app still carries no vendor `.so` and no vendor package name.
The remaining in-app vendor footprint — Java glue — is hosted by the runtime's own
classloader, the way the runtime already hosts `org.freedesktop.monado.ipc.Client`.

This requires a vendor manifest change (external dependency L7 below) and one on-device
verification: that an explicit-component `bindService` still succeeds after
intent-query visibility.

> **Amendment 1 (2026-08-18) — verified on a retail device (#1037).** The
> intent-action form works, and the verification D5 asked for passed.
>
> - **Correction to the premise:** an app was never expected to hand-write the vendor
>   `<queries>` in the first place — the *vendor AAR's own manifest* declares
>   `<queries><package …/></queries>` and the manifest merger injects them into every
>   app that links it. ADR-025's coupling is **transitive from the vendor AAR**, which
>   is why hosting the glue in the runtime APK (D2 Amendment 1) removes it at the root.
> - Stripping the vendor `<package>` entries while keeping the AAR classes reproduced
>   ADR-025's abort exactly: `PackageManager$NameNotFoundException` inside
>   `GetPackageInfo` → `Runtime aborting`.
> - With **zero vendor package names** in the shipped binary manifest (confirmed with
>   `aapt2 dump xmltree`) and only a `<queries><intent><action …/></queries>`,
>   everything worked: `getPackageInfo` succeeded, the core initialised, head tracking
>   connected, and — the verification this decision called for — **an
>   explicit-component `bindService` to the backlight service still succeeded after
>   intent-query visibility**. The vendor services are `SYSTEM` but
>   `forceQueryable=false` and the app was `targetSdk=31`, so visibility genuinely
>   applied; the result is not an artefact.
>
> What remains is only the **action string**: today's filters are vendor-named, so L7
> (the `org.displayxr.action.VENDOR_DISPLAY_SERVICE` filter) is still the ask, and the
> `displayxr` AAR that carries the single `<queries><intent>` ships once L7 lands.

### D6 — Window origin is a contract; phase stays the weaver's

Every compositor instance receives **its window's on-panel origin every frame**, via a
new append-only DP vtable slot `set_window_screen_rect(x, y, w, h, display_id)` under
the [ADR-020](ADR-020-plugin-abi-compatibility-policy.md) rules (`struct_size`
negotiation; plug-ins that do not implement it keep working).

Android forces this. A pure window **move** raises no resize — only a
`oneway IWindow.moved` — and SurfaceFlinger repositions the layer with the *old*
buffer, so the interlace phase goes stale for the whole drag. The client samples
`View.getLocationOnScreen()` from a `Choreographer` callback (opting out of OEM
view-bounds sandboxing) and feeds the compositor directly (A) or over the existing
window-metrics IPC channel (C). Windows solves the same problem by subclassing the
HWND inside the weaver; Android has no such hook, so the runtime reports geometry
instead.

[ADR-033](ADR-033-placement-reports-geometry-weaver-owns-phase.md) is unchanged and is
the reason this is shaped as a report: the party that owns placement reports geometry;
the weaver still owns everything phase, including snapping.

### D7 — The panel's 3D lens is a per-window *preference*, aggregated by the vendor

The switchable 3D lens is display-global, but with N windows on one panel no single
compositor instance may command it. `xrt_display_processor::on_pause` therefore means
**"this session's window is not visible: stop weaving and RELEASE your lens
preference"** — never "force the panel to 2D"; `on_resume` re-asserts; `destroy`
implies `on_pause`. The vendor arbiter ORs the votes, so "last one out turns the light
off" — [#563](https://github.com/DisplayXR/displayxr-runtime/issues/563) — falls out
for free. The contract is written into `xrt_display_processor.h` (#1039).

The runtime owns the *when*: visibility is `live output surface AND session active`,
keyed on surface validity (`MonadoView.surfaceDestroyed` → `Client.clearAppSurface` →
`android_globals_clear_window`) and NEVER on Activity `onPause` — in multi-window only
one Activity is top-resumed while both windows weave. It is evaluated per client, once
per frame, from a single writer in `comp_multi_system::android_window_transition_locked`.

On the Leia/CNSDK stack that vote is literally the binder bind-refcount of
`BacklightMultiClientControlService`, and `leia_core_enable_3d(false)` on that tier is a
pure unbind — so no new vendor API is needed there and `debug.dxr.multiapp` (the #151
interim hold) is deprecated. It IS needed on the legacy single-client tiers, where the
same call forces a global `MODE_2D`: limitation **L2/L3**, tracked in #1038.

Known gap: a window that is fully *occluded* but whose surface still exists produces no
signal — SurfaceFlinger does not throttle occluded layers — so it keeps weaving and
keeps its vote.

## Alternatives considered

**B — one multi-session service — rejected as the primary.** It is *more* runtime work
than either A or C and buys weaker isolation. It needs keyed surfaces and keyed globals
throughout, per-client transition/pause/overlay policy, the #967b/c bounded-wait and
lock work as a hard prerequisite, and a vendor plug-in refactor to one core driving N
interlacers. In exchange it concentrates N apps behind **one render thread and one
queue** (head-of-line blocking), in **one failure domain**, on a device whose OS
already composites windows — and it depends on a freezer-exposed bound service. Its
only structural wins are one vendor core and the lowest memory, which do not pay for
that. It is not deleted: if PoC-0 shows N cores in N processes cannot coexist on
hardware, B becomes the fallback, because "one core, N interlacers" is the shape the
vendor's own multi-window sample already ships.

**B′ — the shared-surface overlay as the default — rejected**, and re-scoped to D4. It
replaces the platform's window manager with ours, which is right for a spatial desktop
and wrong for a phone.

**Status quo (one visible app) — rejected.** It is not a design; it is an unkeyed
global, and it also blocks the browser port.

## Consequences

**Positive**

- One abstraction, four platforms: what Windows does by accident becomes the stated
  invariant, and the Android work is mostly filling in the pieces Windows gets from the
  OS (per-window phase origin, per-window Kooima).
- App-level fault, stall and lifecycle isolation come from the platform rather than
  from runtime arbitration code we would have to write, review and keep correct.
- ~80% of the effort is shared between the target (A) and the fallback/PoC (C), so the
  hedge is nearly free.
- `displayxr-browser` gets a host that never puts vendor code inside Chromium.
- The workspace overlay stops being a competing default and becomes a mode, so #967's
  genuinely valuable parts (D-1/D-2/D-3) can land without deciding the Android shape.

**Costs and risks**

- **PoC-0 is a real gate.** N vendor cores in N processes is designed for but never
  exercised. #1032 must pass before D2/D3 work is committed; a failure re-opens B.
- **D5 depends on a vendor manifest change** (L7). Until it lands, ADR-025's coupling
  stands and C is the only shippable concurrency.
- **Vendor faults move into the app process under A** — a hang or crash in the vendor
  SDK takes the app down. This is precisely the risk Windows apps already accept, and
  C contains it by process death; a known core-release thread-join hang
  (leia-plugin#39) needs `_exit` on teardown under A.
- **Per-app cost under C**: roughly 30–60 MB and one GPU context per app, with a static
  slot count. GPU context slots are firmware-bounded on mobile parts.
- **Mixed 2D/3D windows are not expressible** until the backlight service grows a
  per-client preference (L2). Today a bound client cannot say "I am 2D right now".
- **Pixel exactness is now an app-authoring obligation on Android** (resizable, no
  fixed orientation or aspect, no `setFixedSize`, SurfaceView only, honour the
  buffer-transform hint, sRGB only) — enforced by the app linter (#1035), because
  server-side compat scaling is invisible to the client and silently destroys the
  weave.
- **Multi-display stays unsolved** on Android either way; the vendor stack knows only
  one physical display (L6).

**What this ADR does not decide:** the wire encoding of the surface-binding extension
(`XR_DXR_android_surface_binding` spec follows), the satellite slot-broker protocol,
the Chromium GPU-process ↔ satellite connection, or whether `compositor/multi/` is
ported or replaced under Architecture A.

## Amendment (2026-08-19) — the flavor merge: D2 and D3 coexist in one APK (#1031)

D2 (Architecture A, in-app) and D3 (Architecture C, satellite) were both realised
and both device-proven — but never **at the same time on one device**, because the
two Gradle product flavors were mutually exclusive. `inProcess`
(`XRT_FEATURE_SERVICE=OFF`) and `outOfProcess` (`ON`) shipped different
`applicationId`s and both declared the Khronos `org.khronos.openxr.runtime_broker`
ContentProvider authority, so only one could be installed. Whichever was installed
decided the deployment for **every** app on the device: with `inProcess` there were
no satellites, no slot broker and no `XR_DXR_weave`; with `outOfProcess` every
single app was pushed out of process, including ones that wanted A. That is the
"blocker" noted in D2 Amendment 1 ("only one runtime flavor installable at a time"),
and it made "A is the target, C is the sanctioned out-of-process deployment" an
either/or in practice rather than the two-deployments-of-one-abstraction this ADR
describes.

**The flavor dimension is deleted. There is one hybrid runtime APK.** It builds
with `XRT_FEATURE_SERVICE=ON` *and* `XRT_FEATURE_HYBRID_MODE=ON`, so
`openxr_displayxr.so` carries the in-process native compositor and the IPC client
together, and `xrt_instance_create` chooses **per process** — exactly the model
Windows' `DisplayXRClient.dll` has always used. Service, slot broker, satellite
`:dxrN` slots and the watchdog are unchanged and still there; an in-process app
simply never binds them.

**In-process (A) is now the default**, which is what D2 says the Android target
deployment is. A client opts into IPC (C) by one of four signals, in this order:

| Signal | Shape | For |
|---|---|---|
| `XRT_FORCE_MODE=ipc` / `=native` | env, or the `debug.xrt.XRT_FORCE_MODE` / `debug.dxr.force_ipc` system properties | dev + test; wins over everything, both directions |
| `<meta-data android:name="com.displayxr.force_ipc" android:value="true"/>` | the app's own manifest | the per-app, build-time switch — pairs with `com.displayxr.satellite_slot` |
| `XR_DXR_weave` enabled | capability | present-owners (#1036); weave on Android exists only in the service compositor, so this needs no configuration at all |
| an adopted service socket | `ipc_client_connection_adopt_fd` / `DXR_IPC_FD` | the browser's GPU process (#1056) |

`applicationId` stays `org.freedesktop.monado.openxr_runtime.out_of_process` —
the name is now historical, but it is what every shipped demo declares in
`<queries>` and hands to `createPackageContext`, so the merged runtime upgrades
in place.

**Migration consequence.** Because every app was previously forced out of
process by the installed flavor, in-process becomes a *new* path for apps that
were never ported to A. Confirmed on device with the `native_app_glue` demos
(`android_main` render thread, no first-class surface binding): the vendor core
loader aborts under CheckJNI on a null jobject in `GetObjectClass`
(`leia_cnsdk_create` → `leia_dp_factory_cnsdk` →
`comp_vk_native_compositor_create` → `oxr_session_populate_vk_native`) — the app
has no Activity-typed Context to hand the vendor Java glue in its own process.
That is the A-readiness gap D2 Amendment 1 already names, surfaced rather than
introduced. The per-app `com.displayxr.force_ipc` meta-data exists precisely so
such an app pins itself to C with a one-line change in its own repo until it is
ported.

Nothing in D1–D7 changes. What changes is that the choice between D2 and D3 stops
being a **device-wide install-time** decision and becomes a **per-application**
one, so an Arch-A app, a second Arch-A app and an Arch-C present-owner can weave
concurrently on one panel. D5's vendor-neutral visibility (L7) is still the
outstanding item for A; it is unaffected by the merge.

## Amendment (2026-08-20) — D5 is a *hard prerequisite* for Architecture A, not a tidiness goal (#1079)

D5 framed the vendor `<queries><package>` lines as an ergonomics problem: they are
vendor names leaking into a neutral client manifest, and a neutral intent action
would remove them. That framing understated the stakes. **Without those lines an
in-process app does not degrade — it dies.**

**The failure.** Package visibility is enforced per *calling uid*. Out-of-process
the vendor DP ran in the runtime service's uid, whose manifest declares the vendor
service packages, so a client manifest never needed them. In-process (D2) the
identical code runs in the **app's** uid. The vendor core loader calls
`PackageManager.getPackageInfo(<vendor service>)`, gets `NameNotFoundException`,
**does not clear it**, and makes one more JNI call with the resulting NULL
jobject — which trips CheckJNI and aborts the process from inside closed vendor
code, at `xrCreateSession`. The stack names `GetObjectClass` inside the vendor
core loader and mentions neither a package nor a manifest.

Note what this is *not*: `createPackageContext(<runtime pkg>, INCLUDE_CODE |
IGNORE_SECURITY)` (D2's class-host Context, #1061) fixes class **loading** only.
It transfers no visibility, and cannot — visibility is a property of the uid, and
the uid is the app's by construction under Architecture A. The runtime cannot
grant it on the app's behalf.

**What changed here.** Three layers, because no single one of them suffices:

1. **The app declares the packages.** This is the actual unblocker and it can
   only live in the client manifest. Every Android demo now carries the same two
   `<package>` lines the runtime's own test apps have carried since #1037.
2. **The runtime stops contributing pending exceptions.** `context_package_name`
   now NULL-checks its Context and clears any inherited pending exception before
   `GetObjectClass`. Under CheckJNI either condition is an abort, not an error
   return — so this is about the runtime never being the component that turns
   someone else's unhandled exception into *our* crash.
3. **The runtime lets a plug-in ask first.** New host-iface slot
   `android_package_is_visible(const char *)`
   (`XRT_PLUGIN_HOST_HAS_ANDROID_PACKAGE_VISIBILITY`), carved out of `reserved[]`
   exactly as `get_android_class_host_context` was — `struct_size` is unchanged,
   the slot was previously zero, no ABI major. A plug-in should probe every
   package its SDK will bind **before** handing control to the vendor loader, and
   on a miss fail cleanly (the loader already degrades to no-DP) while logging the
   exact `<package>` lines the app is missing. Once execution is inside that
   loader nothing can be caught, so asking first is the only available defence.

D5 still stands and is still worth doing — a neutral
`org.displayxr.action.VENDOR_DISPLAY_SERVICE` action deletes layer 1 outright and
makes layers 2 and 3 belt-and-braces. Until it lands, treat the two `<package>`
lines as **required boilerplate for any in-process Android DisplayXR client**, and
layer 3 as what makes forgetting them survivable and self-diagnosing rather than
a hard abort in someone else's code.

## External dependencies (vendor asks)

Tracked runtime-side in **#1038**; anchors in report §11.

| # | Ask | Needed for |
|---|---|---|
| **L1** | Head-tracking **config** (orientation, tracked eyes, IPD, face count, fps, log level) is service-global, last-writer-wins — per-client scoping or aggregation, the way start/fps already aggregate. Runtime-side mitigation: never write config. | A and C (N cores) |
| **L2** | The backlight multi-client service is a **binary bind-refcount** — a bound client cannot express "I am 2D right now". Add a per-client preference + OR-refcount + admin force-2D; the Windows SDK's switchable-hint protocol ports directly. | mixed 2D/3D windows |
| **L7** | Vendor services are discoverable **only by package name** — add the `org.displayxr.action.VENDOR_DISPLAY_SERVICE` intent filter to the head-tracking and display-config service manifests. **Mechanism proven on device (D5 Amendment 1, #1037); only the action string waits on the vendor.** | **D5 / Architecture A** |

## References

- [`docs/roadmap/android-concurrent-multi-app.md`](../roadmap/android-concurrent-multi-app.md)
  — the survey this decision is drawn from (findings F1–F11, architectures A/B/C, code
  changes §10, vendor limitations §11, PoC ladder §14).
- [`docs/architecture/comp-multi-one-pipeline.md`](../architecture/comp-multi-one-pipeline.md)
  — #967; D-4/D-5 re-scoped by D4 above.
- [`docs/specs/runtime/workspace-controller-registration.md`](../specs/runtime/workspace-controller-registration.md)
  — how a controller registers and claims the panel; the entry condition for the
  workspace overlay mode.
- [ADR-019](ADR-019-vendor-plugin-aux-boundary.md) (vendor plug-in boundary),
  [ADR-020](ADR-020-plugin-abi-compatibility-policy.md) (append-only vtable slots),
  [ADR-025](ADR-025-android-vendor-dp-out-of-process.md) (superseded on deployment by
  D2; its isolation requirement satisfied by D5),
  [ADR-028](ADR-028-display-mode-recipe-vs-hardware-state.md) (mode recipe vs hardware
  state), [ADR-033](ADR-033-placement-reports-geometry-weaver-owns-phase.md) (placement
  reports geometry, the weaver owns phase — unchanged by D6),
  [ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md)
  (service-owned arbitration; its D8 "Android alignment" is narrowed to the workspace
  overlay mode by D4).
- Epic #1031; sub-issues #1032 (PoC-0 gate), #1033, #1034, #1035, #1036, #1037, #1038;
  #1006 / #967 (re-scoped); #510, #528, #663.
