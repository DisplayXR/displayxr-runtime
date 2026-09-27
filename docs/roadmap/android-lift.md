# XR_DXR_lift on Android

**Status:** step 1 landed on `feat/lift-android` (vendor-neutral runtime side + sim fake +
probe). **Nothing has run on a device yet.** Spec: [`XR_DXR_lift` § Android](../specs/extensions/XR_DXR_lift.md#android);
decision record: [ADR-042](../adr/ADR-042-vendor-2d3d-conversion-supersedes-default.md).

## Why this exists

The Windows service exposes a vendor 2D→3D module through `XR_DXR_lift`, and the DisplayXR
Browser prefers it over its open default whenever the runtime reports it READY (ADR-042). The
Android browser weaves through the same service contract (`XR_DXR_weave`, AHardwareBuffer in /
out), so the same lift contract lets it hand inline 2D video to the panel's own converter
instead of running an open depth estimator in the page.

## Design (step 1)

```
 IPC thread (present-owner)                 lift thread (JavaVM-attached)        service main thread
 ─────────────────────────                  ────────────────────────────         ───────────────────
 lift_weave_rects  → latch on mc                                                  create_dp_vk_lift()
 weave_submit:                              activate ──(marshal factory)──────▶   (Looper-bound init)
   snapshot rect ─▶ mailbox slot (AHB)      rounds: HIGH / NORMAL rr / LOW 1-in-4
   pin latest ring slot ─▶ blit into        take pending ─▶ lift_convert(DP)
     SBS scratch / crop at the CURRENT rect   ─▶ copy DP output ─▶ ring slot (AHB)
   weave (one process_atlas) ─▶ fence         ─▶ publish
   commit snapshot, unpin
 xrSubmitLiftFrameDXR: import caller AHB ─▶ snapshot ─▶ commit
 xrAcquireLiftResultDXR: ring slot ─▶ export AHB (wait) ─▶ reply
```

- **One lift module per service process** (`comp_multi_lift_android.c`, hung off the
  `multi_system_compositor`): the main service and every satellite slot process each have
  their own, created on the first lift call and destroyed before the Vulkan device.
- **State machine unchanged.** `util/u_lift_mailbox` (latest-wins mailbox, pinned output ring,
  round scheduler, snapshot cap) is reused byte-for-byte; the Android file is only the GPU and
  thread plumbing around it.
- **Every slot is an AHardwareBuffer-backed VkImage** on the service device (input slots are
  `CPU_READ_OFTEN` too), so a vendor module can take the input on the GPU, through
  `AHardwareBuffer_lock`, or as an EGLImage, with no extra copy.
- **Sync v1 = CPU-drained.** The service device has one graphics queue, so there is no
  dedicated lift queue: lift copies go to `main_queue` under its lock and each is fence-waited
  before its slot changes hands (commit / publish / unpin). The weave records its snapshot and
  result blits into its own command buffer, which it already waits. No semaphores or sync fds
  cross the plug-in boundary; the DP returns only when its output is complete.
- **Eyes.** The lift DP has no tracker session; TRACKED viewpoints come from the eyes the
  latest weave submit returned (≤ 500 ms old). A non-weaving session converts without explicit
  viewpoints (module default).
- **Kill switch / knobs** are system properties (`debug.dxr.lift*`), because `getenv` does not
  reach the Android service process.

### Memory contract (plug-in ABI, ADR-020 append)

Five slots appended to `xrt_display_processor_vk` under `XRT_DP_VK_HAS_LIFT`, plus
`xrt_plugin_iface::create_dp_vk_lift` (`XRT_PLUGIN_IFACE_HAS_VK_LIFT_FACTORY`). No ABI bump.

| | Owner | Shape | Valid |
|---|---|---|---|
| input | runtime | RGBA8 `AHardwareBuffer`, exactly `w x h`, `GPU_SAMPLED_IMAGE \\| GPU_COLOR_OUTPUT \\| CPU_READ_OFTEN`, + its `VkImage` (GENERAL) on the shared device; writes complete | for the call |
| output | DP | `AHardwareBuffer` + its `VkImage` (GENERAL), or buffer only (runtime imports, cached by pointer); `AHARDWAREBUFFER_FORMAT_*`; writes complete on return | until the stream's next call |
| blob | DP | bytes + `XRT_DP_LIFT_BLOB_*` | until the stream's next call |

**Result buffers the client receives** (the per-stream export `AHardwareBuffer`): DEPTH is
allocated `GPU_SAMPLED_IMAGE | CPU_READ_OFTEN`, so a client reads depth values on the CPU —
`AHardwareBuffer_lock(buf, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &ptr)`, row stride
(in pixels) from `AHardwareBuffer_describe`, depth in R — with no Vulkan readback of its own
(the browser's `/lift/depth`, patch 0238). SBS / NVIEW exports stay **GPU-only**: they are
only sampled, and a CPU-read usage is not free — gralloc may pick a linear or uncached layout
for it, costing sampling bandwidth on every weave. GAUSSIANS results are bytes already.

No fallback to `create_dp_vk`: on Android the ordinary factory starts the vendor's whole display
stack (tracker, backlight) just to learn that a module is absent.

`XRT_DP_VK_HAS_LIFT` is a new macro in `xrt_display_processor_vk.h`, so the next runtime
release trips the `features` repin gate of every track that diffs that header — today the
Leia **Linux** track ([downstream-pin-bump](../specs/runtime/downstream-pin-bump.md)). That PR
changes nothing for Linux (no Linux caller of the slots); the Leia **Android** track is `abi`
and will NOT move on its own — the Media SDK work below must repin it by hand.

## Sequencing

1. **Fake first (this step).** sim_display's `create_dp_vk_lift` + CPU fake, selectable beside
   the real Leia weaver with `debug.dxr.lift.plugin=sim_display`; proven by the
   `weave_client_vk_android` probe (weave-rect + explicit paths). Device run pending.
2. **Browser.** Android arms of browser patch 0225 (the lift consumer: READY → use the runtime's
   module, `XrWeaveSubmitLiftRectsDXR` on inline video) and the 0221 Java menu entry that
   toggles it — against the fake, before any vendor code exists.
3. **Plug-in module.** A `create_dp_vk_lift` in the Leia plug-in wrapping the Media SDK (below).

## The Leia Media SDK (what the plug-in will wrap)

Facts the plug-in design has to live with:

- It is a **Java module loaded through a `DexClassLoader`**, not a native library — the plug-in
  reaches it over JNI from the (JavaVM-attached) lift thread, and loads it once from the factory
  on the service main thread.
- Its pipeline is **GLES / CPU**, not Vulkan.
- For **video** it produces a **fixed 2560x1600 SBS** output — the panel's own resolution —
  whatever the input size.
- **No viewpoints for video**: it synthesizes for its own fixed rig; the runtime's TRACKED eyes
  cannot steer it. (Honouring explicit viewpoints is "best effort" in the slot contract already.)
- **~5 frames of delay** input → output — larger than the one-conversion lag the Windows module
  has; the latest-wins mailbox absorbs it (the weave keeps weaving the newest result at the
  current rect), but `typical_latency_ns` should say so.
- **Devices: NP02J, K68, LPD-20W only.** Everywhere else the plug-in reports no module.

Planned shape: input via the SDK's `SurfaceTexture` (the plug-in draws the runtime's input
AHardwareBuffer into it, GLES); output to a `Surface` backed by an `ImageReader` of
AHardwareBuffers, whose latest image the plug-in returns from `lift_convert` as
`out_buffer` + a `VkImage` it imported (or no `VkImage`, letting the runtime import it — the
path the fake exercises). The fixed 2560x1600 output is fine: the weave stretches the result
into the rect's current position either way.

## Building and installing a dev runtime on a Leia device

The fake has to run beside the REAL weave to prove "flat → 3D on the lenticular", so the dev
runtime must be the **Leia variant** (it bundles `libdxrp050_leia_cnsdk.so` + the CNSDK Java
glue) — the plain dev APK carries only sim_display, whose weave is a blend. Mirror of the
`leia` leg of `.github/workflows/build-android.yml`:

```bash
# payload: the plug-in pinned in versions.json + the CNSDK release equal to the DEVICE's
# CNSDK services (loader == plug-in == services, not a floor) — v0.10.69 on the NP02J today.
mkdir -p _leia/_plugin _leia/_cnsdk_zip _leia/_cnsdk
gh release download "$(python3 -c "import json;print(json.load(open('versions.json'))['leia_plugin'])")" \
   -R DisplayXR/displayxr-leia-plugin -p 'displayxr-leia-cnsdk-*-android-arm64-v8a.tar.gz' -D _leia/_plugin
tar -xzf _leia/_plugin/*.tar.gz -C _leia/_plugin
gh release download v0.10.69 -R LeiaInc/CNSDK -p 'cnsdk-android-[0-9]*.zip' -D _leia/_cnsdk_zip  # needs LeiaInc read
unzip -q _leia/_cnsdk_zip/*.zip -d _leia/_cnsdk
JNI=src/xrt/targets/openxr_android/src/main/jniLibs/arm64-v8a          # gitignored
mkdir -p $JNI && cp _leia/_plugin/*/arm64-v8a/libdxrp050_leia_cnsdk.so $JNI/

# RELEASE build type, not debug: the plug-in is a Release build, and the #1243 vk_bundle ABI
# guard refuses a Release plug-in's Vulkan factory in a Debug runtime (os_mutex NDEBUG fields)
# — the session would run unwoven.
./gradlew :src:xrt:targets:openxr_android:assembleRelease -PdxrForceVendoredCjson \
   -PcnsdkDir=$PWD/_leia/_cnsdk/cnsdk-android-0.10.69+193.8291a2e -PcnsdkBuild=0.10.69+193.8291a2e
rm -rf src/xrt/targets/openxr_android/src/main/jniLibs   # or later DEBUG builds bundle a refused plug-in

# No release keystore off CI (it is a GitHub secret): sign with the local debug key.
BT=~/Library/Android/sdk/build-tools/34.0.0; D=src/xrt/targets/openxr_android/build/outputs/apk/release
$BT/zipalign -f -p 4 $D/openxr_android-release-unsigned.apk /tmp/aligned.apk
$BT/apksigner sign --ks ~/.android/debug.keystore --ks-pass pass:android --key-pass pass:android \
   --ks-key-alias androiddebugkey --out $D/openxr_android-release-leia-devsigned.apk /tmp/aligned.apk
```

**Known cost of a dev install: the runtime's app data and grants are reset.** The released
runtime is signed with the org release key, so a debug-key APK cannot update it: `adb install`
fails with `INSTALL_FAILED_UPDATE_INCOMPATIBLE` and the package must be **uninstalled** first.
That wipes the runtime package's data and its grants — the overlay permission
(`SYSTEM_ALERT_WINDOW`) and the launch-once broker state. Use the install script, which does
the uninstall and restores both:

```bash
./scripts/install-android.sh --force-reinstall \
    src/xrt/targets/openxr_android/build/outputs/apk/release/openxr_android-release-leia-devsigned.apk \
    test_apps/weave/weave_client_vk_android/build/outputs/apk/debug/weave_client_vk_android-debug.apk
```

Going back to the release is the same in reverse (`--force-reinstall` with the released
`DisplayXR-Runtime-Leia-*.apk`). Only CI (the `ANDROID_KEYSTORE_*` secrets) can produce an
in-place update; there is no release keystore on a dev box.

**Eyeballing the fake on the panel needs a tracked face.** With no face in view the Leia SDK
drops the weave to its 2D fallback (a single view), so the lifted rect looks flat whatever the
lift produced. Sit at the tablet, or force the light-field on with `adb shell setprop
debug.dxr.overlay 1` (clear it afterwards). The same applies to a buffer dump: a uniformly
single-view row there is the no-face fallback, not a lift failure.

**Buffer dumps come from the PROBE APP, not the runtime.** `debug.dxr.weave.dump` is read by
`weave_client_vk_android` (the present-owner test app) only — neither the runtime service nor
the Leia plug-in implements it. The app reads back the pre-weave input it handed the runtime and
the woven output the runtime returned, in its own process, and writes
`/sdcard/Android/data/com.displayxr.weave_client_vk_android/files/weave_in.ppm` and
`weave_out.ppm` (one dump per distinct non-zero value; `adb pull` them). There is no runtime-side
lift/weave dump in step 1: the weave's input (caller-allocated) and output (runtime,
GPU-only) buffers are not CPU-readable, so a service-side dump needs a Vulkan readback plus a
writable path in the runtime package's external files dir — a follow-up
(`debug.dxr.lift.dump`), not free. The lift INPUT slots are CPU-readable (`CPU_READ_OFTEN`), so
that part of such a dump would be cheap.

## Follow-ups (not in step 1)

- **Letterbox crop** on Android: port the HLSL row/column profile pass to GLSL / compute.
- **Box-filtered cap**: the snapshot downscale is one linear blit; a 4-tap filter like D3D11's.
- **Sync fds** instead of CPU drains: an acquire fd on submit (the weave's own follow-up) and a
  release fd on the acquire path, so neither side waits on the CPU.
- **v2 `handleKind`** on `XrLiftFrameSubmitInfoDXR` / `XrLiftResultDXR`, mirroring
  `XrWeaveSubmitHandlesDXR`, so a wrong-platform handle is a validation error.
- **Dedicated lift queue** if the service device ever exposes a second queue.
- **Eyes for non-weaving sessions** (a per-session DP's predicted eyes).
