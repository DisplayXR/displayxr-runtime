---
status: Accepted
date: 2026-08-26
---
# ADR-038: On Android the Vendor Plug-in Ships Inside the Runtime APK

## Context

On desktop, a vendor display processor is a separately-installed artifact —
`DisplayXR-LeiaSR.dll` from its own installer on Windows, `displayxr-leia-sr_*.deb`
on Linux — discovered at `xrCreateInstance`. The runtime binary carries zero vendor
code, and CI asserts it ([ADR-019](ADR-019-vendor-plugin-aux-boundary.md);
`build-windows.yml`'s "zero vendor identifiers" check).

Android had no equivalent, because Android had no delivery story at all: the runtime
APK was a 14-day CI artifact carrying only `libdxrp200_sim_display.so`, and the vendor
plug-in had no build job. Every working device was hand-assembled. Closing that
(#1212) forces the question this ADR answers: **where does the vendor `.so` live on
Android?**

Drifting into an answer was the risk. Bundling is convenient, and convenience is not
a rationale.

## Decision

**One gradle project, two build outputs.**

| variant | contents | purpose |
|---|---|---|
| `DisplayXR-Runtime-<ver>-android-arm64.apk` | `openxr_displayxr.so` + `libdxrp200_sim_display.so` | vendor-neutral; keeps the ADR-019 guard; hardware-free CI smoke build |
| `DisplayXR-Runtime-Leia-<ver>-android-arm64.apk` | the above + `libdxrp050_leia_cnsdk.so` + the CNSDK Java glue and its two transitive `.so` | the APK that can weave on a Leia device |

Selected in CI by a build matrix, **not** by gradle product flavors. Both are attached
to `v*` releases. The plug-in inside the vendor variant is resolved from
`versions.json`'s `leia_plugin` field, so *(runtime tag, versions.json)* determines
exactly what is in the APK.

## Rationale

### 1. A separate vendor APK is not implementable today

The Android plug-in loader resolves its search root by calling `dladdr()` on its own
code and taking that directory — `/data/app/<runtime-pkg>-<hash>/lib/arm64-v8a/`. It
scans that directory and nothing else. `target_plugin_loader.c` says so in its own
docstring: multi-APK vendor plug-ins are *"a v2 problem requiring PackageManager
queries via JNI — out of scope for v1."*

So a separate vendor APK is not a packaging choice we can make; it is unimplemented
loader work. Choosing it would block the entire delivery story behind a feature nobody
has scoped.

### 2. The vendor's Java glue already lives in the runtime APK

This is the stronger argument, and it predates this ADR. CNSDK loads its real core
through a `DexClassLoader` whose parent comes from the `Context` it is handed, so
`com.leia.*` glue must resolve from the **runtime APK's** classloader. That is exactly
what `xrt_plugin_host_iface::get_android_class_host_context` was added to do
(#1037, [ADR-036](ADR-036-android-per-window-compositor-instances.md) D2): the loader
hands the plug-in a `createPackageContext(<runtime pkg>, INCLUDE_CODE|IGNORE_SECURITY)`
Context. The runtime APK has carried the vendor AAR ever since, and its manifest
already declares the vendor `<queries>`.

The vendor's *Java* half is therefore already in the runtime APK by design. Putting
the native half somewhere else would be the inconsistent option.

### 3. What is bundled is a shim, not the SDK

Verified from the binaries. `libleiaCore-loader.so` builds a `DexClassLoader` over the
package `com.leialoft.display.config`, calls `getNativeLibraryDir`, and `dlopen`s
`libleiaCore-impl.so` **out of that installed on-device package**. The plug-in's entire
non-system dependency closure is one library:

```
libdxrp050_leia_cnsdk.so  NEEDED → liblog libandroid libvulkan libleiaCore-loader libm libdl libc
```

So the real core, face tracking and per-device calibration are never redistributed —
they are installed on the device, exactly as Windows assumes an installed
`LeiaSR_runtime.dll`. What rides along is ~1.1 MB of loader shim plus the Java glue:
the Android equivalent of an *import library*, not of the SR runtime.

It has to ride along at all only because Android has no system-wide search path for
third-party native libraries, so a `DT_NEEDED` must resolve from the loading APK's own
`nativeLibraryDir`. That is a platform mechanic, not an architectural concession.

### 4. Keeping a neutral variant preserves what ADR-019 protects

ADR-019's concern is that the runtime must not *depend on* vendor code. The neutral
APK is the standing proof that it does not: it builds, installs and self-tests with no
vendor material, and CI hard-fails if any appears in it. The vendor variant is a
*packaging* of the same runtime with a plug-in beside it — the same relationship the
Windows installer creates on disk, expressed in the only container Android's loader
can see.

## Consequences

- **Two APKs to publish, and they must not be confused.** Installing the neutral APK
  on a Leia device yields a device that installs and mostly self-tests fine and cannot
  weave. `scripts/install-android.sh` warns when the APK name lacks `-Leia`, and the
  `vendor_dp` self-test check (#1212) fails when a better-ranked plug-in was present
  and failed to load.
- **N vendors would mean N APK variants.** Acceptable at one vendor, and it becomes
  the forcing function for the PackageManager-based discovery in §1 when a second
  arrives. Revisit this ADR then, not before.
- **The runtime repo's CI needs vendor SDK access** for the vendor variant. It
  soft-skips when the token is absent, so forks and external contributors still get a
  green neutral build.
- **CNSDK is redistributed as incorporated material only.** Leia's Creator Toolkit
  licence permits distribution "as incorporated into your Products" (§3) and forbids
  standalone distribution (§4b), so the CNSDK zip is never republished; the packaging
  step hard-fails if CNSDK material reaches a plug-in release asset.
- **The CNSDK version becomes an untracked input unless recorded.** `versions.json`
  cannot hold it (its membership rule is "installable released asset", and CNSDK is not
  ours), so the pin lives in `downstream-pins.json`'s `sdk_pins`.

## Alternatives rejected

**Separate vendor APK.** Matches the desktop model literally and ADR-019 most
directly. Rejected as unimplemented, not as undesirable — see §1. It is the right
destination if a second Android vendor appears.

**One APK, always vendor-carrying.** Simplest to build and to explain. Rejected
because it deletes the neutral variant, and with it both the ADR-019 CI guard on
Android and any hardware-free build for contributors without vendor SDK access.

**Gradle product flavors instead of a CI matrix.** Rejected on precedent: #1031
removed the `inProcess`/`outOfProcess` flavors precisely because two flavors declared
the same `org.khronos.openxr.runtime_broker` ContentProvider authority and so could
never be installed side by side. Re-introducing a flavor dimension would re-create
that problem. A CI matrix over one gradle project has neither issue.

## Amendment 1 — Publish ONE APK; the neutral build stays, as a CI artifact (2026-08-28)

**Decision: the vendor-carrying APK is the only published Android release asset.** The
vendor-neutral variant continues to be *built* on every release; it is no longer
*attached* to one.

This reverses the "One APK, always vendor-carrying" rejection above, because that
rejection rests on a step the original ADR did not separate: it assumed *not publishing*
the neutral variant meant *not building* it. It does not. Keeping the neutral build as a
CI artifact preserves both reasons it was rejected for — the ADR-019 guard on Android
still has a vendor-free binary to assert against, and forks without vendor SDK access
still get a green build via the existing soft-skip.

### Why the second APK was never load-bearing at run time

The runtime already does what the neutral variant was published to provide. Both plug-ins
ship *inside* the vendor APK:

```
lib/arm64-v8a/libdxrp050_leia_cnsdk.so     ProbeOrder 050
lib/arm64-v8a/libdxrp200_sim_display.so    ProbeOrder 200
```

and `target_plugin_loader.c` states the resolution rule: *"Discovery attempts candidates
in ascending ProbeOrder and returns on the first success."* On a device with no vendor
services the vendor probe declines, discovery continues, and sim_display claims the
system. Choosing an APK at download time was therefore choosing something the loader
already decides correctly at run time, with better information.

Measured cost of carrying the vendor payload everywhere (v2.14.10): **33 MB neutral vs
36 MB vendor**, i.e. ~3.4 MB of APK delta — `libleiaSDK-jni.so` (6.4 MB),
`libdxrp050_leia_cnsdk.so` (1.9 MB) and `libleiaCore-loader.so` (1.1 MB) uncompressed.
`libleiaCore-impl.so`, the actual engine, is never shipped; it is `dlopen`'d from the
on-device package. The APK carries a **shim**, not an SDK.

### What this buys

One asset to link, one to sideload on a device with no adb, and no "which APK do I
download?" question in front of a first-time user — which is the first thing the install
guide currently has to explain.

### What it costs, and what must move with it

1. **Vendor manifest material reaches every device.** The vendor AAR merges real
   components into the runtime manifest (`com.leia.sdk.internal.DebugActivity`) plus
   vendor `<queries>` entries. Functionally inert on non-vendor hardware, but it is the
   same redistribution question that governs multi-vendor packing, and it should be
   answered per vendor rather than assumed.
2. **The silent-fallback failure mode becomes the common one.** Today "you installed the
   neutral APK" explains a 2D screen. With one APK, a vendor plug-in that fails to load is
   indistinguishable from correct sim_display fallback — a green PASS over a black screen,
   the exact case the `vendor_dp` self-test (#1212) exists to catch. That check moves from
   backstop to primary diagnostic and **must** be surfaced in the dashboard's default view
   before this lands, not left to whoever thinks to run a self-test.

### Scope

This amendment changes *what is published*, not the packaging model. §1 still holds: a
separate vendor APK remains unimplemented loader work, and a second Android vendor is
still the trigger to build PackageManager-based discovery — at which point the published
artifact question should be revisited again.

Tracking: [#1265](https://github.com/DisplayXR/displayxr-runtime/issues/1265).
