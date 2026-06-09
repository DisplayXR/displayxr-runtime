---
status: Accepted
date: 2026-06-09
---
# ADR-025: Android Vendor Display Processors Run Out-of-Process

## Context

[ADR-019](ADR-019-vendor-plugin-aux-boundary.md) establishes the vendor-isolation
principle: vendor display processors (Leia SR and friends) ship as plug-ins behind a
stable boundary, and an application talks only to DisplayXR (the OpenXR loader →
runtime), never to a vendor SDK. On **Windows** this holds for free — the SR weaving
runs in the runtime's own module and reaches the SR service over COM/named-pipes, so
the vendor SDK never loads into the application's address space. The app is pure
OpenXR.

On **Android** the runtime ships in two deployment flavors
(`src/xrt/targets/openxr_android/build.gradle`, `flavorDimensions 'deployment'`):

- **`inProcess`** (`…openxr_runtime.in_process`, `XRT_FEATURE_SERVICE=OFF`) — the
  runtime, the vendor DP plug-in, and the vendor SDK all `dlopen` into the
  **application's own process**.
- **`outOfProcess`** (`…openxr_runtime.out_of_process`, `XRT_FEATURE_SERVICE=ON`,
  builds the `monado-service` target) — the runtime runs as a **separate service
  process**; the app connects over IPC and hands its surface across
  (`MonadoImpl.nativeAppSurface` / `nativeStartServer` / `nativeAddClient` in
  `src/xrt/targets/service-lib/service_target.cpp`).

The Android Leia bring-up (#499) and the background→resume surface-lifecycle fix
(#507) were both done on **`inProcess`**, because it is one APK with no service
lifecycle — ideal for fast iteration. But in-process **cannot** satisfy ADR-019 on
Android, for a reason intrinsic to the platform, not to our code.

### Why in-process breaks vendor isolation on Android

In the in-process flavor the vendor SDK (CNSDK) is loaded into the **app's** process.
CNSDK must reach the Leia system services — `com.leialoft.display.config` (device
config / backlight), the head-tracking service — which are separate APKs. Under
**Android 11+ package visibility**, the *calling process's manifest* must declare
`<queries>` for any package it binds or queries. Since CNSDK runs in the app's
process, it is the **app's** manifest that must carry those `<queries>` (plus the
CNSDK loader `.so` + JNI glue the aar provides). A CNSDK-free app aborts the moment
CNSDK calls `GetPackageInfo("com.leialoft.display.config")` →
`NameNotFoundException` → JNI fatal abort (observed on a nubia NP02J during #507
follow-up).

This couples the app to the vendor set known **at build time**. It defeats the
product promise — *write the app once against OpenXR; any vendor, including ones that
ship later, plugs in* — in two independent ways:

1. **Temporal.** You cannot bundle an SDK for a vendor that ships after your app is
   compiled.
2. **Combinatorial.** Bundling *N* vendors' SDKs into every app means version
   conflicts, bloat, and duplicate native libs — maintained by the app developer,
   which is backwards.

### CNSDK is already core-in-service — so this adds no new heavy process

Inspecting the CNSDK repo (`LeiaInc/CNSDK`) shows the heavy stack is **already**
out-of-process:

- `leia/core/loader/loader.service.cpp` (built under `LNK_CORE_IN_SERVICE`) hosts
  `leiaCore` **in the device-release APK** (`com.leialoft.display.config`): the
  app-side `libleiaCore-loader.so` is a thin loader that `GetPackageInfo`s that
  service APK, then loads the real `leiaCore` **from it** via an isolated classloader
  + a `leia_core_vtable` hand-off (required because `dlsym` doesn't work on a
  `System.loadLibrary`'d lib). The 0.10.56 release bundle ships **no** `leiaCore.so` —
  the tell that core-in-service is the deployed mode.
- Device config / backlight / head-tracking are **AIDL/Binder services**
  (`leia/device/android/service/.../aidl/*.aidl`, `BaseServiceConnection`), shared by
  every Leia app.

So the in-app footprint is only **loader + JNI client + `<queries>`**. The decision
below relocates *that thin client* — it does not introduce a new heavy process.

## Decision

**On Android, vendor display processors run out-of-process, in the DisplayXR runtime
service. In-process is a development/bring-up flavor only, not a shippable
application model.**

Concretely:

- An application links **only the OpenXR loader**. It carries no vendor SDK, no
  vendor `.so`, and no vendor `<queries>`. It is identical across vendors and across
  time — exactly as on Windows.
- The DisplayXR **runtime service** (`outOfProcess` flavor) hosts the native
  compositor + the vendor DP plug-in + the thin vendor-SDK client, and owns the
  vendor `<queries>` in **its** manifest
  (`src/xrt/targets/openxr_android/src/main/AndroidManifest.xml`).
- The app renders into shared, **zero-copy** swapchain buffers; the service consumes
  them and the DP weaves. Frame data does not cross the boundary as pixels.
- This applies to **every** DP, vendor or not. **`sim_display`** — the vendor-neutral
  in-tree fallback — must work out-of-process too; it is the first validation target
  precisely because it isolates the IPC / shared-buffer frame path from any vendor
  complexity.

### Resulting three-tier architecture

```
App  (pure OpenXR — nothing vendor-specific)
  └─ DisplayXR runtime SERVICE   ← native compositor + DP plug-in + thin vendor-SDK client + vendor <queries>
        └─ Leia device-release APK (com.leialoft.display.config): leiaCore .so + AIDL config/backlight   ← already out-of-process
        └─ Leia head-tracking service APK                                                                 ← already out-of-process
```

Only the **middle tier's placement** is the change. The bottom tier already exists
and already runs out-of-process.

## Why (reasoning, in order of weight)

1. **It is the only model that satisfies ADR-019 on Android.** In-process places the
   vendor SDK in the app's process, which forces the vendor `<queries>` into the
   app's manifest — coupling the write-once app to a build-time vendor set. No
   in-process arrangement avoids this; it is a property of Android package
   visibility, not of our packaging.
2. **It matches Windows.** The vendor surface lives behind the runtime boundary in
   both; the application is OpenXR-only on both. One mental model, one app contract.
3. **It is the proven XR-runtime topology.** Every production OpenXR runtime (Meta,
   SteamVR, upstream Monado — which we forked) runs out-of-process with Binder IPC +
   shared GPU buffers, at full refresh. The machinery is already in-tree from that
   heritage (see Consequences).
4. **It costs nothing extra in heavy work.** CNSDK's `leiaCore` is already hosted in
   the device-release APK; the app↔service frame path is zero-copy; the
   service↔device-service AIDL traffic is unchanged regardless of where the DP sits.
   The weave/interlace GPU pass is the same work, run in the service.

## Performance

No meaningful per-frame penalty, with the right mechanics — all present in-tree:

- App renders into **AHardwareBuffer-backed swapchain images**
  (`src/xrt/auxiliary/android/android_ahardwarebuffer_allocator.*`,
  `src/xrt/auxiliary/vk/vk_image_allocator.c`); the service **imports the same
  physical GPU memory** — no pixel copy across the boundary. Synchronization via
  shared **sync-fd fences** (`src/xrt/auxiliary/vk/vk_sync_objects.c`), not CPU
  stalls.
- Only control/metadata (poses, predicted display time, frame begin/end) crosses
  Binder — bytes, not frames — and is pipelined, so the app never blocks on the
  service per pixel.
- A glasses-free 3D display tolerates latency **better** than a head-mounted display
  (no vestibular mismatch), so the small fixed IPC control latency is a non-issue
  here even though it is already acceptable for the VR runtimes that ship this way.

The one genuinely Android-specific operational cost is the **service process
lifecycle**: aggressive OEM process freezers (e.g. the ZTE/MyOS `CpuFreezer` seen in
#507) will freeze a backgrounded service. The runtime service must be kept
alive/foregrounded appropriately. This is an implementation concern, not an objection
to the model.

## Consequences

- **Applications become vendor-clean and future-proof.** The same app binary runs
  against sim_display, Leia, or a vendor that ships next year, with no rebuild — the
  ADR-019 promise made real on Android.
- **The `outOfProcess` flavor becomes the supported/production target on Android;
  `inProcess` is retained as a dev-only iteration aid.** Docs and the app-authoring
  guidance should state this; the in-process CNSDK-aar escape hatch in
  `test_apps/cube_handle_vk_android/build.gradle` (the `-PcnsdkDir` conditional) is
  dev-only and is removed once out-of-process Leia is validated (remove-last; see
  #510).
- **Vendor `<queries>` and the thin vendor-SDK client move to the runtime service
  manifest / jniLibs.** The vendor `leiaCore` tier (`com.leialoft.display.config`) is
  untouched — DisplayXR is just another client of it.
- **The #507 surface lifecycle needs an out-of-process equivalent.** In-process,
  background→resume is driven by `oxr_session_poll` →
  `android_custom_surface_refresh_window` → `android_globals` →
  `comp_vk_native_target_sync_surface`. Out-of-process, the surface reaches the
  service via `nativeAppSurface`, so the destroy/recreate signal must be carried
  across the IPC boundary so the service compositor rebuilds its `VkSurfaceKHR`
  instead of wedging.
- **The frame-path and IPC machinery already exist** (Monado heritage):
  `src/xrt/ipc/{server,client,shared,android}`, `src/xrt/compositor/{client,multi}`,
  the AHardwareBuffer allocator, the sync objects. The work in #510 is wiring +
  validating them for the 3D-display compositor path, not building them.
- **The cube is the acceptance test.** "A CNSDK-free cube renders Leia 3D
  out-of-process" is the closed-boundary proof; until then this ADR is *Accepted but
  unvalidated on device*.

## Scope / status

- **Accepted as the architectural direction.** Implementation tracked in **#510**
  (milestones: M1 sim_display out-of-process → M1b surface lifecycle over IPC → M2
  Leia out-of-process → M3 cube zero-aar acceptance).
- Validated on device only for **in-process** today (#499/#507). On-device validation
  of the out-of-process path is the gating signal for moving this ADR's status from
  "Accepted (direction)" to fully realized; a failure there is an implementation
  regression, not a relitigation of this decision.

## Related

- [ADR-019: Aux Library Boundary for Vendor Plug-in DLLs](ADR-019-vendor-plugin-aux-boundary.md)
  — the vendor-isolation principle this enforces on Android.
- [ADR-003: Vendor abstraction via display-processor vtable](ADR-003-vendor-abstraction-via-display-processor-vtable.md)
  — the DP contract, unchanged by this work.
- `docs/architecture/separation-of-concerns.md` — the layering rule this makes
  structurally enforceable on Android.
- Issue #510 — the implementation plan, performance analysis, CNSDK-core-in-service
  findings, and cube acceptance test.
- Issue #507 / PR #509 — the in-process Android background→resume surface lifecycle
  whose out-of-process equivalent is a consequence above.
