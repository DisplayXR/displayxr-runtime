# New-session prompt — fix runtime #528 (OOP compositor wedges on the client's abandoned surface)

Paste the block below as the first message of a fresh session. The nubia NP02J
(Pad 3D II) must be connected via adb. Context that produced it: building the
media-player Android leg surfaced this as the root blocker for **any** in-app
system UI (file picker, permission dialog, share sheet) on the OOP runtime.

---

## Mission

Fix **DisplayXR/displayxr-runtime#528**: the out-of-process runtime's compositor
**wedges on the client app's abandoned Android surface** after the client
backgrounds (e.g. to show a SAF file picker) and returns. The runtime keeps
producing frames into the **destroyed** `ANativeWindow`/BlastBufferQueue instead
of re-acquiring the new surface, so the client stalls in `xrWaitFrame` and the
panel freezes on the last pre-background frame. The only recovery today is a
full client cold-restart.

This blocks the media-player's file loader (DisplayXR/displayxr-demo-mediaplayer,
issue #14 area) and any sub-activity flow. Validate the fix with that picker.

## Symptom (confirmed on the NP02J)

After the client backgrounds and returns, logcat spams (35k+ lines/session):
```
E BufferQueueProducer: [SurfaceView[com.displayxr.mediaplayer_vk_android/...MainActivity]#2(BLAST Consumer)2](id:...,api:1,p:568,c:<client>) connect: BufferQueue has been abandoned
```
`p:568` = the runtime (`org.freedesktop.monado.openxr_runtime.out_of_process`).
`MonadoView` logs a fresh `surfaceChanged, w=2560 h=1600` on return, but the
runtime never re-acquires it.

## Where the bug is (already scouted — verify, don't assume)

The runtime **already has** generation/valid surface re-sync plumbing, but the
OOP present target doesn't consume it:

- `src/xrt/auxiliary/android/android_globals.cpp` — has
  `android_globals_set_window()` (sets window, `valid=true`, `generation++`),
  `android_globals_clear_window()` (keeps the pointer but `valid=false`,
  `generation++` — "the next re-sync tears the surface down"), and
  `android_globals_get_window_state(out_window, out_generation, out_valid)`. The
  comments explicitly anticipate "the compositor's surface re-sync."
- `src/xrt/compositor/main/comp_window_android.c` — the OOP present target.
  `comp_window_android_init_swapchain()` pulls the window **ONCE** via the
  legacy `android_globals_get_window()` and creates a `VkSurfaceKHR`; there is
  **no re-sync** against `get_window_state`'s generation/valid, and
  `comp_window_android_flush()` looks like a no-op. So when the surface is
  abandoned it keeps presenting into the dead one.
- `src/xrt/auxiliary/android/.../MonadoView.java` (`surfaceDestroyed` ~L320,
  `surfaceChanged`/`surfaceCreated`) and
  `src/xrt/ipc/android/.../SurfaceManager.kt` — the UI-thread surface callbacks.
  **Verify** that `surfaceDestroyed` actually calls
  `android_globals_clear_window()` and `surfaceChanged`/`Created` on return push
  the **new** window via `android_globals_set_window()` across IPC to the
  service process — the abandoned-queue spam suggests one or both halves aren't
  wired through to the service's `android_globals`.

## Likely fix shape (engineer to confirm by tracing)

Make the OOP compositor **follow the Android surface lifecycle**, using the
existing generation/valid channel — end to end across the IPC boundary:

1. **Client/UI side:** on `surfaceDestroyed`, invalidate
   (`android_globals_clear_window`) and propagate to the **service** process
   (IPC); on `surfaceChanged`/`surfaceCreated`, push the new `ANativeWindow` +
   bump generation to the service.
2. **Compositor present loop / `comp_window_android`:** each frame (or on a
   present that returns `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR`), check
   `android_globals_get_window_state`. On `valid==false` or a generation change:
   `vkDeviceWaitIdle`, destroy the swapchain + `VkSurfaceKHR`, stop presenting
   (don't spin into the dead queue). On `valid==true` with a new window:
   recreate the surface + swapchain at the new size and resume. The session
   should ride this through STOPPING→…→READY so the client `xrBeginFrame`s again
   without a process restart.
3. Make sure a present returning OUT_OF_DATE is **handled** (recreate), not
   ignored — that's what turns into the 35k-line abandoned-queue spin.

Keep it vendor-neutral (this is the compositor/window layer, not the DP). The
DP (Leia) rotates the interlace separately and shouldn't need changes.

## Related

- **#507** — Android background→resume freeze (same surface-resync family; that
  fix was for the client's own background/resume, this is the **sub-activity**
  round-trip where a *new* surface is created).
- **#510** — OOP service-mode bring-up (where `comp_window_android` was ported).
- **#523** — vendor CPU-freezer freezing the backgrounded OOP service (separate;
  promote to a foreground service). On the NP02J, keep the runtime unfrozen
  during testing via *ZTE Smart app optimization → DisplayXR runtime → always
  allow background activity* (standard exemptions don't work on this skin).

## Build / deploy / validate (NP02J)

Build the runtime + push the service APK per the Android build path (see
`docs/getting-started/building.md` / the Android CI workflow). Then reproduce
with the media player's SAF picker:

1. Install the media player APK (`displayxr-demo-mediaplayer`, branch merged to
   `main`): `./gradlew :android:assembleDebug` then `adb install -r -d …`.
   Re-enable the picker entry point (it's wired but currently sidestepped by a
   `TEMP` `default.mp4` auto-load in `bring_up()` — tap the screen / Load button
   opens `ACTION_OPEN_DOCUMENT`).
2. Start the OOP runtime via its LAUNCHER (NOT start-service):
   `adb shell monkey -p org.freedesktop.monado.openxr_runtime.out_of_process -c android.intent.category.LAUNCHER 1`,
   settle ~3 s, confirm `adb shell pidof …out_of_process` is non-empty.
3. Launch the app, open the picker, pick a file, return.
   - **Before the fix:** panel freezes; `BufferQueue has been abandoned` spam.
   - **After the fix:** the session resumes (no restart), the picked media plays.
4. You cannot screencap the VK weave overlay — read logcat for the surface
   lifecycle (`surfaceDestroyed`/`surfaceChanged`, surface recreate logs, no
   abandoned-queue spam, session state →5) and ask the user to eyeball the panel.
   OOP runtime lifecycle is flaky — launch fresh, settle, don't over-thrash.

Read first: `docs/adr/ADR-025-*` (OOP), the `comp_window_android.c` header
comment (the #510 port note), and `android_globals.h` (the surface-state API
contract). Issue #528 has the full repro + log signature.
