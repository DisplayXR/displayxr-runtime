# Android weave satellite (Architecture C)

Tracking: [#1277](https://github.com/DisplayXR/displayxr-runtime/issues/1277).
Driver: large-format Android panels where apps run in freeform windows as the
primary mode — app-side weaving cannot survive a container scale (measured:
SurfaceFlinger `tr=[0.67,0][0,0.67]` on NP02J's mini-window; browser#173/#128/#186
are the evidence trail).

## Status (reconciled 2026-09-06)

**P0 and P1 are SHIPPED on `main` and device-verified; P2 is in design.** The
"design (P0 not started)" line this file used to open with was stale from
2026-08-28 and is the only thing about it that was ever untrue — the P0/P1
status sections further down have been accurate since 2026-08-29.

Everything below is **behind `debug.dxr.weave_satellite=1`, default off**;
`=0` is today's return-the-woven-texture path bit-for-bit, and every satellite
failure one-shot-latches `sat_failed` and falls back to it.

| Phase | State | Commits on `main` |
|---|---|---|
| P0 design | shipped | `9e5524c17`, `40ce7dcb5` (2026-08-28) |
| P0 satellite present + parity | **shipped, human-verified on NP02J** | `c7e27d665`, `94f66cd86` (2026-08-29) |
| P0 physical-rect weave under a container scale | **shipped, human-verified ("cube is crisp 3D")** | `7bf68b5b0` (2026-08-29) |
| P0 rotation (swapchain-only rebuild) | shipped, verified both directions | `a19aa00c5` (2026-08-29) |
| P0 status writeups | — | `48b9ae1bf`, `6ed3aee6e` (2026-08-29) |
| P1 per-window container-scale auto-derivation | **shipped, verified with zero props set** | `e777fc6b2` (2026-08-29) |
| P1 occlusion + input passthrough + full-panel overlay | **shipped, verified on device** | `54ffb5985` (2026-08-29) |
| Weave-idle overlay clear (filed as P2, landed early) | **shipped, device-verified** | `91f071770` (2026-08-30) |
| Sibling: #1278 weave-idle lens release | shipped, OS-verified | `8aa84691e` (2026-08-29) |
| Sibling: DP panel size in current orientation | shipped, human-verified | `c7be6ab44` (2026-09-01) |
| **P2** | **design — this document, below** | — |

**Who verified what.** Every "verified" above is David eyeballing the NP02J
panel in the overnight sessions of 2026-08-29/30 (the P0 acceptance quotes —
"yes clean !!", "cube is crisp 3D !" — are his), plus a 14/14 automated soak.
The satellite has never run on any other device or any other panel, and it has
never been exercised with the display in a multi-monitor or external-panel
configuration.

**Client coverage.** The only client that has ever reached the satellite is
`displayxr-browser` (an `XR_DXR_weave` present-owner). The `_hosted` /
surface-binding demos — modelviewer, gaussiansplat, mediaplayer, earthview —
**do not touch the satellite today, in either deployment**: in-process they
weave into their own surface, and forced to IPC they get a service-side
compositor that still presents into their own (scaled) surface. Closing that is
P2 item (b) below. `91f071770`'s two-client experiment (browser present-owner +
modelviewer via `force_ipc`, two slots live) is the one time both have run
concurrently, and it is what found the frozen-overlay bug that commit fixes.

**Two "while scaled, show 2D" mitigations now exist, and they differ.** The
browser's is patch `0123` option 1b, **opt-in and default OFF**
(`--inline-3d-refuse-scaled`) because of the browser#186 freeform→fullscreen
staging wedge. The runtime's landed in PR #1372 (`736db35de`, 2026-09-06,
merged): the in-process VK compositor derives "container scaled" from the
published window rect against the panel (the same bounds-exceed-the-panel tell
as P1), and while scaled collapses to tile 0, skips the weave and releases the
lens preference — correct 2D, never a double image — and resumes weaving when
the rect fits again. Device-verified on the reference tablet (scaled
mini-window → clean 2D, David's eye check; `am task resize` to on-panel bounds
→ 3D resumes; fullscreen untouched). It is always on; it needs no property.
The same PR is what makes a `_hosted` view publish its rect at all (#1367) and
scales the DP view dims to the window canvas.

## The P0-shaping finding (2026-08-28)

**The wire already carries everything the satellite needs.** On every
`xrWeaveSubmitDXR`, the service-side compositor
(`comp_multi_weave_android.c`) receives:

- the **unwoven input** — an AHardwareBuffer holding every rect's squeezed-SBS
  content at its own window position (spec v7 batch layout);
- the **rect list** (window-relative device px);
- the **window geometry** — now truthful, per browser patch 0123: the
  compositor SurfaceView's on-screen origin plus the container-scaled flag.

Architecture C's P0 is therefore **not** a new IPC design. It is a
present-mode flip inside the service: weave into a surface the *service* owns
and presents, instead of into an output buffer returned to the client.

## Second P0 reduction (recon, same night): the overlay half already ships

`Java_org_freedesktop_monado_ipc_MonadoImpl_nativeCreateServiceOverlay`
(`service_target.cpp`) is the #558 avatar-over-launcher machinery: the SERVICE
self-creates a `TYPE_APPLICATION_OVERLAY` surface with **no Activity** via
`android_custom_surface` (works from the service Context; `debug.dxr.transparent`
makes it TRANSLUCENT; `android_custom_surface_can_draw_overlays` gates it;
the #558 stale-overlay heal covers client restarts), publishes it through
`android_globals`, and the compositor presents into it. So the satellite's
present surface, permission handling, translucency and lifecycle are shipped,
field-tested code.

**P0's entire remaining delta is the weave divert** in
`comp_multi_weave_android.c`: on `debug.dxr.weave_satellite=1`, acquire the
overlay window (same `android_custom_surface` path), build a
`VK_KHR_android_surface` swapchain on it (`comp_window_android` shows the
recipe), and per submit blit the woven output into it at the window's physical
rect instead of returning `weavedTexture`. One blit + present per frame, all
inside machinery this file already owns (it has the vk bundle, the queue-lock
discipline, and the geometry via `set_window_geometry`).

## P0 scope: one browser window, satellite-presented weave

```
browser ──xrWeaveSubmitDXR──▶ service ──weave──▶ service-owned overlay Surface
   │ (unchanged wire)            │                    (full-panel, UNSCALED,
   └─ page-owned 2D shows        └─ CNSDK interlacer   above apps)
      under the overlay             viewport at the
                                    window's PHYSICAL rect
```

1. **Overlay surface.** The runtime APK already holds `SYSTEM_ALERT_WINDOW`
   (granted by every install script; the #558 overlay mode exercises it).
   `MonadoService` adds a full-panel `TYPE_APPLICATION_OVERLAY` SurfaceView —
   `FLAG_NOT_FOCUSABLE | FLAG_NOT_TOUCHABLE`, translucent — and hands its
   `ANativeWindow` to the service compositor, which already knows how to
   present to an Android surface (the hosted path). The overlay is never
   scaled by the window manager: it IS the physical-pixel canvas.
2. **Satellite weave mode** in `comp_multi_weave_android.c`: gated by
   `debug.dxr.weave_satellite=1`. When on, the weave renders into the overlay
   swapchain; `XrWeaveOutputDXR::weavedTexture` is never returned (spec v7
   already allows steady-state frames to return none, and the browser's
   over-plane skips drawing when it holds no mailbox — so the page's own 2D
   pixels show *under* the overlay with **zero browser changes**). Overlay
   unavailable ⇒ fall back to exactly today's return-the-output path.
3. **Phase becomes trivial.** The satellite weaves the full panel at (0,0):
   the interlace phase needs no per-window anchor at all. Per-window
   *placement* is the CNSDK per-window interlacer viewport
   (`set_viewport` + `set_viewport_screen_position` — the 1-core/N-interlacer
   model from the concurrent-multi-app report), fed by the geometry the wire
   already carries.
4. **Scale-aware placement.** Under the OEM mini-window the reported origin is
   physical but the size is logical; the satellite must place at the physical
   footprint (logical × scale). P0 escape hatch: `debug.dxr.satellite_scale`
   (measured 0.67 on NP02J); the durable answer — the platform exposing the
   task scale — goes on the OEM asks list
   (`docs/specs/vendor/oem-android-platform-requirements.md`).

## Deliberately OUT of P0

- **Occlusion** (another window overlapping the woven one must clip the weave)
  — P1; needs a visible-region feed or whole-panel composition.
- **Input** beyond `FLAG_NOT_TOUCHABLE` passthrough — P1.
- **N concurrent weaving windows** (policy: ADR-025, #967 conflicts) — P2.
- OEM z-order guarantees (overlay vs system chrome/caption) — recorded as an
  OEM ask, accepted as best-effort in P0.

## P0 STATUS (2026-08-29, overnight session)

**Core parity: PASSED, human-verified** — the satellite-presented weave was judged
"clean" against the in-app golden reference (NP02J, landscape fullscreen; see the
golden-standard memory/fingerprint). The bring-up found and fixed, in order:
the overlay's own origin inset (60-row phase beat), whole-output blits copying the
DP's compose-under backdrop (dark film → per-rect blits, which is also
occlusion-lite), SUBOPTIMAL-as-recreate thrash (→ tolerate; IN_USE → bounded retry),
and the decisive one — **Android's anti-tapjacking clamp composited the overlay at
alpha 0.8**, blending 20% of the under-content through the weave (per-eye
crosstalk). Dev unlock: `settings put global maximum_obscuring_opacity_for_touch
1.0`; the ship requirement (trusted overlay / per-package exemption) is now in
`oem-android-platform-requirements.md`. Diagnostic lesson that generalizes:
screencap cross-correlation proved content+placement identical, isolating the fault
to scanout composition — **screenshots cannot see HWC-level blending; dump the HWC
layer list.**

**Physical-rect weave: ACCEPTED, human-verified (2026-08-29 morning)** — with
`debug.dxr.satellite_scale=0.67` and the browser dragged to the OEM mini-window,
the cube was judged **"crisp 3D"** on NP02J: the P0 acceptance case (test 2 below),
the one no app-side weave can do. Expected artifact while the knob is set globally:
a *fullscreen* window then shows a 0.67-size woven rect over the browser's own mono
draw — the prop is a stand-in for the real per-window scale, which is the P1
auto-derivation item (fullscreen→1.0, each freeform window→its true OS scale).

**Rotation: fixed post-soak** — the overnight rotation check was a false green (it
asserted only no-failures and could not see rendering). Portrait→landscape left the
stale portrait swapchain on screen (squished duplicate over the fresh frame) because
tolerating SUBOPTIMAL had removed the only rotation signal. Fix: detect the
out-vs-overlay orientation mismatch and **rebuild only the swapchain with
`oldSwapchain` chaining** — the overlay view survives rotation via
surfaceChanged-in-place (measured), and a full view release/re-ensure collides with
the #558 single-window globals (stale published window → IN_USE latch, 121 retry
frames measured). Round-trip rebuild ~55 ms/direction, clean both ways.

**#1278 weave-idle lens release: shipped and OS-verified** on the same branch
(lens vote released 2.0 s after the last submit — the OEM backlight service logs
`Disable` — and re-asserted on the next weave). Two structural findings recorded in
the commit: the multi main loop is parked for pure present-owner clients (the pass
is now also driven from the IPC 20 Hz loop), and the release must hit the weave's
own DP directly (no dp_visibility edge exists for a present-owner).

**Overnight soak: 14/14 PASS** — overlay lifecycle ×4 (appear/teardown, no leaks),
prop toggle, idle-release ×3, rotation ×2.

## P0 acceptance test

On NP02J, `debug.dxr.weave_satellite=1`, browser at hello-cube:

1. ✅ Fullscreen: crisp 3D via the overlay (parity with today) — "yes clean !!" / golden.
2. ✅ **OEM mini-window: crisp 3D** — "cube is crisp 3D !" (scale prop 0.67) — the case no app-side weave can ever do,
   and the entire point (browser#186's wedge and #173's phase class are both
   structurally impossible here: the satellite target is never scaled and
   never resizes with the window).
3. ✅ Freeform→fullscreen→freeform cycling + rotation: stable (swapchain-only
   rebuild on orientation change; no per-cycle state in the client-facing path).
4. `debug.dxr.weave_satellite=0` restores today's behaviour bit-for-bit.

## P1 STATUS (2026-08-29)

**Per-window container-scale auto-derivation: SHIPPED, human-verified.** The
global `debug.dxr.satellite_scale` knob is demoted to a diagnostic override;
the satellite now derives the scale per window at submit:

- **The tell** (all field-measured on NP02J): an OEM mini window is a fixed
  phone-profile task (sw540dp → 1080×1685 logical) whose WM bounds are a
  hybrid — physical origin + logical size — while a SurfaceFlinger leash
  (`tr=[0.67,0][0,0.67]`) scales presentation. So the caller-reported rect
  EXCEEDS the panel while its origin lies inside; that is app-visibly unique
  to a container-scaled window. The factor itself is app-invisible (leash-only;
  a11y bounds logical-clipped; no config/settings/prop — all probed), so the
  tell selects a per-device constant: `debug.dxr.satellite_miniwindow_scale`,
  default 0.67.
- **Verified end-to-end with zero props set**: fullscreen weaves at derived
  1.0 (golden), the dragged mini window logs `PHYSICAL-RECT weave, scale
  0.670 (window 1757,236 1080x1685 vs panel 2560x1600)` and weaves crisp —
  both correct simultaneously, which the global knob could never do.
- **Ship ask filed**: `oem-android-platform-requirements.md` § *Container-scale
  visibility* — the platform must expose the presentation scale (the constant
  is correct only while the OEM mini-window scale stays fixed).
- Caveat (direction of failure): an unscaled freeform window dragged off-edge
  would trip the tell and be wrongly scale-woven; this OEM clamps mini windows
  in-panel and has no unscaled-freeform UX, so not reachable today.

**P1 occlusion + input + full-panel overlay: SHIPPED (same day).**

- **Input passthrough: verified, zero code** — FLAG_NOT_TOUCHABLE passes
  touches through the overlay to the client, and the OS unscales them into the
  window's logical space itself (verified with injected scroll: page scrolled,
  weave tracked).
- **Full-panel overlay** (`span_system_bars` on `android_custom_surface`):
  the overlay now lays out edge-to-edge (measured 2560x1600, origin 0,0)
  instead of inset below the status bar. Fixes the immersive-toggle bug ("tap
  the fullscreen browser -> broken weave"): the tap flips Chrome edge-to-edge
  (window 0,0 2560x1600), and the inset overlay's present clamped dst_y=-60 to
  0 without shifting the source — the whole weave landed 60 rows low. The blit
  path now also clips in DST space with source compensation, so partial
  off-panel rects map correctly in general.
- **Occlusion: a11y-fed window subtraction.** `WindowWatcherService`
  (AccessibilityService in the runtime APK, adb/user-enabled, OFF by default)
  serializes the interactive window list {type, layer, bounds} to
  `files/dxr_occlusion.bin` (atomic rename; the file transport crosses slot
  processes with zero IPC). The satellite subtracts occluders above the client
  from each blit rect (band decomposition, <=64 pieces): IME always; app
  windows by a11y layer vs the origin-matched client (fullscreen clients use
  the non-fullscreen-window rule); scaled occluders corrected by the same
  hybrid-bounds tell as the client scale. Verified on device: with the OEM
  split keyboard summoned over the cube, the weave clipped exactly at the
  IME's top edge (before the feed: cube drew over the keys). Watcher disabled
  or dead -> no occluders -> exactly the pre-P1 behavior.
- Dev trap: `am force-stop` on the runtime package kills the a11y watcher and
  Android only rebinds it on a settings retoggle — after a force-stop, retoggle
  `enabled_accessibility_services`.

Remaining for P2: N-window policy (ADR-025), the OEM platform asks (filed in
`oem-android-platform-requirements.md` + the KBXR OEM brief), a11y-independent
occlusion if the platform ever exposes window geometry directly.

## P2 design (2026-09-06)

P0 and P1 answered "can one window weave correctly after the window
transform?" — yes. P2 is the two things the runtime still lacks before that is
a *platform* rather than a demo: **who owns the overlay when several windows
want it**, and **how an app that is not the browser gets onto it at all**.

### (a) N-window policy — N weaving windows, one panel, one overlay

**The structural problem.** Architecture C gives each client its own satellite
*process* (`:dxr0..3`, ADR-036 D3 — four pre-declared slots, then a fallback to
the main-process service). But the satellite's present surface is a **full-panel
`TYPE_APPLICATION_OVERLAY`**, and the panel is one. Today that surface is
reached through the #558 single-window `android_globals` publish, which is why
the rotation fix had to rebuild the swapchain in place rather than
release-and-re-ensure the view: **the overlay is a panel-global resource
addressed by a process-global**. Two satellites each acquiring "the overlay"
is undefined behaviour today, and the frozen-overlay bug (`91f071770`) is the
first symptom of the class — a client that stops submitting leaves its last
woven frame painted over whoever now owns the screen.

Three shapes, from cheapest to most correct:

| | Shape | Overlay surfaces | Vendor cores | Cost / risk |
|---|---|---|---|---|
| **C-lease** | Exactly one client holds the overlay at a time; everyone else weaves into its own surface (today's in-surface path) | 1 | 1 per satellite process | Cheapest. Degrades to a *correct* picture for the loser (unscaled windows weave fine in-surface). Needs an arbiter and a hand-off. |
| **C-stack** | One overlay per satellite, N stacked translucent full-panel layers | N | N | No arbitration needed, but N full-panel GPU surfaces on an Adreno, undefined z-order between same-type overlays, and N× the anti-tapjacking/HWC blending surface. Rejected. |
| **C-single** | One overlay owned by the **main-process service** (the panel owner); satellites hand it unwoven content + geometry; it weaves every window with **one core / N interlacers** | 1 | **1 total** | The durable shape — literally ADR-035 D3's "one pipeline, one DP per panel". Needs a cross-process content hop per client per frame and the vendor 1-core/N-interlacer path (report F6: it exists in CNSDK, `leia_interlacer_set_viewport_screen_position`, and the shipping `sdk-test` runs 7 interlacers in one Activity). |

**Recommendation: C-lease now, C-single as the target.** C-lease is a policy on
top of the mechanism P0/P1 already shipped, and it is the *only* one of the
three that can be built without a vendor-side change. C-single is where this
has to end up — and note that C-single is also the only shape in which the
runtime can honestly say "one DP per panel", which every ADR already assumes.

**The lease, concretely** (ADR-035 D2's panel lease, replayed on Android and
scoped to the overlay):

1. **Who gets it.** The overlay exists to fix *one* thing: weaving under a
   container transform. So the lease goes to a client **whose container is
   scaled** (the P1 hybrid-bounds tell already computes this per window, per
   submit). An unscaled window has nothing to gain and gives it up. This
   answers the "one window scaled and another not" case directly and without a
   new signal: **the scaled window takes the overlay; the unscaled window keeps
   weaving in its own surface, and both are correct simultaneously.**
2. **Ties.** Two scaled weaving windows: the lease follows **focus** (the
   compositor slot table's `focused_slot`, ADR-035 D2 — one focus authority,
   never a second opinion). The loser weaves in-surface, i.e. shows the double
   image it shows today. This is a known, stated degradation, not a silent one:
   log it and surface it in the diag dashboard (#558-adjacent).
3. **Where it lives.** In the **main-process service**, not in a satellite —
   a satellite cannot see its peers, and the lease is exactly the thing that
   needs to. The slot broker (`SlotBrokerService`, #1053) is already the one
   main-process object every satellite talks to, so it is the natural holder.
4. **Hand-off.** Releasing the lease must clear the overlay before the new
   holder draws, or the outgoing client's last frame is left painted over the
   incoming one — the `91f071770` bug, again. `comp_multi_weave_android_satellite_clear`
   is already the primitive; the lease release calls it synchronously and the
   new holder acquires only after it returns.
5. **Lens.** Unchanged: the vendor service ORs the votes (L2 / #1039,
   device-verified refcount 2→1→2), so N weaving windows keep the panel 3D and
   the last one out turns it off. The runtime adds no arbiter *until C-single*,
   where one DP serves N clients and the runtime inherits the OR itself.
6. **Occlusion** already composes: the P1 a11y window-subtraction is per blit
   rect, so a leaseholder's rect is clipped by the non-leaseholder's window
   above it with no new input.

**Ceiling to state plainly:** four `:dxrN` slots (ADR-036 D3). N-window policy
above four is "the fifth client shares the main-process service", and that path
has never been exercised with the satellite.

### (b) The route by which an Architecture-A app reaches the satellite

The demos (modelviewer, gaussiansplat, mediaplayer, earthview) are `_hosted` or
surface-binding apps that create an in-process `comp_vk_native` compositor and
weave into their own SurfaceView. In an OEM mini-window that is *structurally*
wrong and no amount of geometry fixes it. Three candidate routes:

| | Route | Verdict |
|---|---|---|
| **(i)** | The app's session is created as an **IPC client** (`XRT_FORCE_MODE=ipc` / the #1031 per-app manifest switch), so the service composites it and presents on the satellite overlay; the app's own window stays a **placement + input anchor** | **RECOMMENDED** |
| **(ii)** | **Mid-session hand-off** in-process → satellite when a scaled container is detected | **Rejected** |
| **(iii)** | **Always-satellite on large-format devices** (device policy switch) | Folded into (i) as a *setter*, not a separate route |

**Why (ii) is rejected.** The in-process/IPC choice is made **once, at
`xrt_instance_create`** (`src/xrt/targets/openxr/target.c`), before a session,
a compositor or a window exists — ADR-036's signal table is explicit that this
is a per-process decision. A mid-session hand-off would have to tear down and
rebuild the compositor, the swapchains, the `VkDevice` binding and the vendor
core *under a live `XrSession`*, and the in-process path's whole value is that
the app's own images are used with no copy — those images live in the app's
process and cannot be re-homed into a satellite. There is no OpenXR mechanism
for it and no runtime affordance. (The in-process detect-and-degrade that
landed in PR #1372 is the *fallback* while scaled — 2D, correct — not a
hand-off: it does not move the session anywhere.)

**Why (iii) is a setter and not a route.** "Every app on this device goes out
of process" is precisely the device-wide deployment decision that ADR-036's
flavor merge deleted — it pushed apps that wanted in-process out of it. What is
legitimate is a **device-scoped allow-list**: on this panel, *these* apps run
out of process. That is (i) with the OEM holding the list.

**Why (i).** Three reasons beyond it being the only one left:

- It is **already the documented remedy** for these exact apps. `native_app_glue`
  demos abort in-process under CheckJNI (null `jobject` in `GetObjectClass`,
  `leia_cnsdk_create` → `leia_dp_factory_cnsdk` → `comp_vk_native_compositor_create`)
  because they have no Activity-typed Context for the vendor Java glue —
  ADR-036 and `android-build-guide.md` both already say: pin such an app to C
  with one line in its own manifest.
- It is **already plumbed**. `com.displayxr.force_ipc` (manifest, per-app),
  `debug.dxr.force_ipc` (sysprop) and `XRT_FORCE_MODE` all exist and are the
  sanctioned signal set.
- It **keeps the app Architecture-A-clean**. The app codes nothing: no weave
  extension, no satellite awareness, no present-owner protocol. It keeps its
  own window, its own input, its own lifecycle; the window becomes a placement
  and input anchor exactly as the browser's SurfaceView is today.

**The runtime-side delta (this is the actual P2 work).** Routing a demo to IPC
today does **not** give it the satellite. The satellite lives entirely inside
the `XR_DXR_weave` submit path (`comp_multi_weave_submit` →
`weave_satellite_ensure`/`_present` in `comp_multi_weave_android.c`), and a
demo is an `XRT_CLIENT_CLASS_APP` client that submits ordinary projection
layers, which the service composites and presents into **the client's own
surface** (`multi_compositor::session_render.target`, a `comp_target` built
from the app's `ANativeWindow`) — inside the scaled container, so still wrong.

What has to be built:

> **Satellite present for APP-class sessions.** Drive the existing
> `weave_satellite_ensure` / `weave_satellite_present` from the per-session
> render path instead of only from the weave submit path. Source: the session's
> composed image, pre-DP. Destination: the physical rect derived from
> `android_globals_get_window_screen_rect` plus the P1 container-scale tell —
> the same derivation the weave path already does, and #1367/PR #1372 is what
> makes a `_hosted` view publish that rect at all. Placement: the DP's
> per-window interlacer viewport. Gate: `debug.dxr.weave_satellite=1` **and**
> the session holds the overlay lease from (a). Fallback at every failure:
> today's in-surface present.

Note the dependency: (b) needs (a)'s lease the moment a second client exists,
and (a) is untestable without a second client — so build the lease first with
the browser + one demo, which is exactly the pair `91f071770` already ran.

**The app-side contract** — nothing an app must code:

| Tier | Setter | Scope | For |
|---|---|---|---|
| Ship | `<meta-data android:name="com.displayxr.force_ipc" android:value="true"/>` (+ optional `com.displayxr.satellite_slot`) | the app, everywhere | a demo that has decided it belongs out of process |
| Device | `ro.dxr.force_ipc=<pkg>[,<pkg>…]` | this device's build | the OEM saying "on this panel, these apps run out of process". **Allow-list only** — a bare `1`/`*` is refused, because a device-wide switch is the ADR-036 failure |
| Dev | `debug.dxr.force_ipc=<pkg>[,<pkg>…]` (or `1` for the whole device, as before) | this boot | routing a **shipped, unmodified** demo APK onto the service to test, with no rebuild |

The dev and device tiers are new in this pass and are pure policy plumbing
(`u_sandbox_route_prop_selects`, unit-tested in `tests/tests_aux_route_policy.cpp`);
they add no compositor code and change nothing when unset. `XRT_FORCE_MODE`
still wins over all of them, in both directions.

**Recommended sequencing for the demos:** exercise them over the dev sysprop
first (no demo-repo change at all), land the APP-class satellite present, then
add the manifest line to each demo repo once the picture is signed off — in
that order, because the manifest line is the one step that is a release in
someone else's repo.


## The interlacer contract under a container scale (2026-09-07, with the vendor SDK maintainers)

Settled from the interlace shader while LeiaInc/CNSDK#732 was being designed
(an earlier reading — "the interlacer must first learn to consume a scale" —
was retracted; the corrected statement is this):

- **The existing integer viewport API is sufficient.** The only viable path
  for a scaled container is the client sizing its buffer to the *on-screen*
  rect so the compositor's scale becomes 1.0. In that path buffer width equals
  the panel extent by construction, so an interlacer that uses one viewport
  width as both buffer divisor and panel extent is exactly right.
  `set_viewport(1757, 236, 732, 1137)` with a 732×1137 framebuffer *is* strict
  1:1. The only missing input is the number (the scale); the app already has
  the origin.
- A "panel scale" parameter would only matter if the buffer stayed at the
  logical size and the compositor scaled it — and that case is unfixable by
  **any** interlacer change, because bilinear resampling of an interlaced
  pattern is not invertible (S8 / #731 is the answer there, not an API).
- One real limit stands: the screen position is an integer, so a fractional
  composited origin cannot be expressed. On the reference tablet the leash
  translate is integral (1757, 236), so this is theoretical today.
- The satellite's physical-rect weave at the derived constant (P1,
  `e777fc6b2`) **is** the size-to-on-screen path, with the constant standing in
  for the missing scalar — the structurally correct shape now, not a stopgap.
- The runtime-side tell (`bounds exceed the panel`, PR #1372) answers *whether*
  a window is scaled; the scalar (*how much*) must come from a platform-trusted
  reader (#732). The app's `getLocationOnScreen()` origin is already the true
  post-scale on-screen origin, only the size is pre-scale (SurfaceFlinger:
  `geomLayerTransform` = translate (1757,236) + 0.67·bounds = `coveredRegion
  [1757,236,2481,1365]`), so #732 needs one scalar per task, not a rect.
- Static-once-settled is measured; the open/close transition is not — an
  animated scale still needs the platform-composed weave (S8) or the 2D
  fallback.

### The S9 client recipe, implemented in-app (2026-09-07, #1367)

The bullet above — *"the only viable path for a scaled container is the client
sizing its buffer to the on-screen rect so the compositor's scale becomes
1.0"* — is now implemented for the **hosted, in-process** Android path, so the
mini-window no longer needs the satellite to be crisp. Three parts, all in
`MonadoView` plus the rect sink the compositor already consumes:

1. **Get the scalar without #732.** The S9 probes on #1367 found it is already
   readable from an ordinary app uid on this firmware, two independent ways:
   the OEM test-API `ActivityManager.getDefaultWindowParamByTaskForNormalWr(taskId)`
   (returns the post-scale on-screen `Rect`; **not** blocklisted), and the
   raw-vs-local ratio of a *real* dispatched touch (`0.670000`, max residual
   1e-4 px). The API is preferred, the touch ratio is the fallback, and when
   both are present they are cross-checked once — a disagreement means
   accessibility magnification or a firmware change, and the code then keeps
   the 2D fallback rather than picking one.
   *Gating matters:* the test-API returns the **nominal** window-reply
   placement in fullscreen too, so it is read only when the container-scaled
   tell already fired.
2. **Size the buffer so the composition carries no visible resample.**
   `round(scale · logical)` is not enough: 1080 × 0.67 = 723.6 rounds to 724, so
   SF composes (1080/724) × 0.67 = **0.9994** — 0.06 %, ~0.4 px of drift across
   the window, which David's eye read as *"weaves good, but a slight double image
   in **both** eyes"*. Both eyes equally is the signature of a residual
   **resample**, not of a phase error, and the SF readback agreed (0.9994 / 1.0000).

   The requirement is only that `layout · s` land close **enough** to an integer.
   So rationalise the scale to `p/q` (**q = 100 first** — an OEM window scale is a
   round percentage; smallest-q-wins picks the wrong fraction, because the
   whole-pixel `Rect` only pins the scale to ~3e-4 and `63/94` fits that band with
   a smaller q while composing to 0.99968), then take the **largest** layout within
   q of the window whose residual is under **0.1 px**. Calibration: 0.4 px was
   visible, 0.05 px has been on the panel throughout unremarked.

   On the reference tablet that is **1079 × 1685 logical → 723 × 1129 buffer**
   (residual 0.07 / 0.05 px), leaving a **one-logical-pixel** strip on the right
   and none at the bottom. Anchor the view `TOP|LEFT` so the origin needs no offset
   math and the whole remainder falls on the far edge; paint the FrameLayout black
   behind it.

   Two rules that were tried on device and rejected, so they are not worth
   re-deriving:
   - **Snapping to a multiple of q** (1000 × 1600 → 670 × 1072) composes to exactly
     1.0000 — SF stops classifying the transform as `SCALE` at all — but costs a
     54 × 57 px black border, ~5 % of the window. David rejected the border.
   - **Overscanning** (a layout *larger* than the window, so the crop eats the
     remainder) does not work at all: a SurfaceView bigger than its window has its
     surface sized to the **visible frame**, so a 737 × 1139 buffer was mapped into
     723.6 × 1128.95 screen px — composed **0.982**, a 2 % resample, and the double
     image came straight back.

   Either way, **not** SurfaceFlinger's `displayFrame` (732×1137): that includes the
   task layer's shadow (`shadowRadius` 6 × 0.67 ≈ 4 px a side) and sizing to it
   would put an 8 px resample straight back.
3. **Publish the PHYSICAL rect** (origin unchanged — `getLocationOnScreen` is
   already post-scale; size = the buffer). Everything downstream then works in
   panel pixels: the compositor's view dims (`window × view_scale`), the
   per-window Kooima, the app's `canvasRectPx`, and the DP's screen origin. The
   rect is published only once the surface has actually come back at the new
   size, so no frame weaves at a size the buffer does not have.

SF then composes `buffer→layer (1079/723) × leash (0.67) = 0.99990` in x and
`(1685/1129) × 0.67 = 0.99996` in y — 0.07 px and 0.05 px of drift over the whole
window — and the strict 1:1 contract above holds to the eye ("it's perfect now",
David, on the reference tablet). `vk_android_update_container_scaled` needed no change:
its tell reads the published rect, so a physical rect fits the panel and it
clears itself back to weaving. The degrade remains the fallback for every case
this cannot serve — scale unmeasurable, sources disagreeing,
`debug.dxr.miniwindow_1to1 0`, a surface-binding app that has not opted in, and
a window genuinely dragged off-panel.

**Surface-binding (`XR_DXR_android_surface_binding`) apps are not covered.**
They own their own surface and their own rect publish, so opting in means doing
the same two things themselves: `setFixedSize(round(w·s), round(h·s))` on their
`SurfaceHolder`, and passing the physical rect to
`xrSetAndroidWindowGeometryDXR`. A runtime helper that hands them the measured
scalar is the obvious follow-up.

**Still open (this does not close it):** the *animated* open/close transition,
where the scale is in motion — that still needs the platform-composed weave
(S8) or the 2D fallback.

## P2 implementation plan (2026-09-07)

The section above is the *design*. This one is the executable plan: every touch
point with a `file:line` anchor as of `dcd9c22e8`, an effort estimate per step,
and the device ladder for whoever has the pad. **Nothing in it has run on
hardware** — it was written with no device access, from the source and the
shipped commits.

Line anchors move. They are given so a reader can find the seam, not as a
promise; re-grep the symbol if a number is off.

**What landed with this plan** (and nothing else — no compositor code, no
present-path change): `auxiliary/util/u_overlay_lease.{h,c}`, the lease's API
surface with an always-grant default backend and the arbitration policy as a
pure function, plus `tests/tests_aux_overlay_lease.cpp` which states #1376's
acceptance cases as facts about that function. It has **no callers**; with no
backend installed the mechanism grants every acquire, which is bit-for-bit the
shipped one-client-per-process behaviour. That is step A2 (and A5) below,
landed early because it is the only part of P2 that can be *proven* without a
device — the same argument `tests/tests_aux_route_policy.cpp` was written on.

### The as-built map both items attach to

| Thing | Where |
|---|---|
| Satellite gate (`debug.dxr.weave_satellite`) | `comp_multi_weave_android.c:621` `weave_satellite_wanted()`, prop read `:626` |
| Overlay surface bring-up (`SYSTEM_ALERT_WINDOW` → `android_custom_surface_async_start(..., span_system_bars=true)` → `VK_KHR_android_surface` swapchain) | `comp_multi_weave_android.c:891` `weave_satellite_ensure()`; surface at `:910`; `sat_failed` latch at `:1015` |
| Satellite present (blit woven output → overlay at the physical rect, present) | `comp_multi_weave_android.c:1337` `weave_satellite_present()` |
| Satellite clear (transparent-black repaint) | `comp_multi_weave_android.c:1239` `comp_multi_weave_android_satellite_clear()` |
| P1 container-scale tell | `comp_multi_weave_android.c:846` `weave_satellite_effective_scale()`; prop `debug.dxr.satellite_miniwindow_scale` read at `:881` **and again at `:1211`**, both defaulting to `0.67f` |
| Occlusion subtraction (a11y feed) | `comp_multi_weave_android.c:1039`–`:1145` |
| Rotation (swapchain-only rebuild on out-vs-overlay orientation mismatch) | `comp_multi_weave_android.c:1358` |
| The ONLY caller of the satellite today | `comp_multi_weave_android.c:2291`, inside `comp_multi_weave_submit()` (`:1635`), after the weave fence at `:2266` |
| Satellite state (per-`multi_compositor`, **not** per-process) | `comp_multi_private.h:724`–`:746` (`sat_checked`/`sat_enabled`/`sat_failed`/`sat_csurface`/`sat_surface`/`sat_swapchain`/`sat_images[8]`/`sat_off_x,y`) |
| The weave mutex the satellite runs under | `comp_multi_private.h:619`–`:620`; `weave_ensure_mutex()` `comp_multi_weave_android.c:131`. Held across the whole of `comp_multi_weave_submit()` (`:1676`–`:2308`), satellite present included |
| The satellite swapchain's image usage | `comp_multi_weave_android.c:748` — **`VK_IMAGE_USAGE_TRANSFER_DST_BIT` only**. The overlay image can be blitted into and nothing else |
| The satellite blit | `comp_multi_weave_android.c:1551` `vkCmdBlitImage`, `VK_FILTER_NEAREST`, no image view anywhere in the path |
| `export_output` suppression while the satellite presents | `comp_multi_weave_android.c:2334`–`:2335` (`satellite_live` → report no output, so the caller's over-plane draws nothing) |
| Weave-idle clear (#1278) that drives the satellite from the 20 Hz IPC loop | `comp_multi_system.c:5862` `android_window_transition_locked()`, clear call at `:5934`; external tick `multi_system_compositor_android_visibility_tick()` `:5957` |
| **The APP-class per-session render** — the function P2(b) has to reach | `comp_multi_system.c:2770` `render_session_to_own_target()` |
| …its atlas source (the composed image, **pre-DP**) | `comp_multi_system.c:3489`–`:3490` (`session_render.flip_sbs_image` / `flip_sbs_view`, declared `comp_multi_private.h:403`–`:408`, built by `ensure_session_atlas_image()` `comp_multi_system.c:1833`, called `:3232`) |
| …its DP invocation | `comp_multi_system.c:3488`–`:3502` `xrt_display_processor_process_atlas()`, preceded by `set_target_color_view()` at `:3484`–`:3485` (the #510 M2 fix — a self-submitting DP needs it or the weave is skipped) |
| …its destination | `ct->images[buffer_index]`, i.e. the **client's own** `ANativeWindow` swapchain, inside the scaled container |
| …its submit + present | `comp_multi_system.c:3588` (`vkQueueSubmit` under `vk_queue_lock`), `:3640` `comp_target_present()` |
| …the loop that drives it | `comp_multi_system.c:5970` `multi_main_loop()` → `:6075` → `render_per_session_clients_locked()` `:5349`, per-client call at `:5424` |
| The window rect the destination must be derived from | `comp_multi_system.c:2722` `update_window_screen_rect()` (called at `:3379`) → `android_globals_get_window_screen_rect()` (`android_globals.h:242`, impl `android_globals.cpp:289`) → `session_render.window_screen_{x,y,w,h,disp_w,disp_h}` + `window_rect_generation` (`comp_multi_private.h:487`–`:495`) |
| The in-process scaled degrade that already ships (tile-0 collapse + lens release) | `comp_vk_native_compositor.c:3412` `vk_android_update_container_scaled()`, applied at `:3476` and `:5676` — commit `736db35de`, whose own message says *"Only the in-process VK path is touched; the out-of-process satellite has the same exposure and is not covered here."* |
| Routing policy (already landed, never run on a device) | `u_sandbox.c:55` `u_sandbox_route_prop_selects()`, read sites `:292`/`:306`/`:317`; host tests `tests/tests_aux_route_policy.cpp` |

Two facts from that map shape everything below:

1. **The satellite is per-client state on a panel-global resource.** `sat_*`
   lives on `multi_compositor`, but the overlay it drives is one physical
   surface. In the shipped topology that is safe only because ADR-036 gives each
   client its own *process* — `android_globals.h:214`–`:217` says so in as many
   words ("Process-local and NOT keyed by client: one satellite compositor
   process serves exactly one client… If a process ever hosts several clients
   this must become per-client state"). #1376 is the arbiter that assumption has
   been standing in for.
2. **There is no focus authority on Android.** `focused_slot` is a
   Windows/D3D11 concept (`comp_d3d11_service.h:779`); the IPC layer's
   `active_client_index` (`ipc_server.h:459`) is degenerate under one client per
   satellite. #1376's tie-break *cannot be written today* without first choosing
   a focus signal — see A1.
3. **There is no per-client `ANativeWindow` on Android either.** An APP-class
   client's `comp_target` is created handle-free —
   `multi_compositor_init_session_render()` `comp_multi_compositor.c:1755`
   passes a NULL handle to `comp_target_service_create()`, which lands in
   `null_target_service_create_from_window_android()`
   (`null_compositor.c:772`, wired `:971`) → `comp_window_android_create()`,
   whose swapchain pulls the window from the **process-global**
   `android_globals_acquire_window()` (`comp_window_android.c:146`). The window
   rect is a process global too (`android_globals.cpp:255`–`:264`). Both are
   correct only because ADR-036 gives each client its own process. Nothing in
   this plan may assume a second client in the same process; two would overwrite
   each other's window *and* rect. (Nothing branches on `client_class` anywhere
   in these paths — the real split is `multi_compositor_has_session_render()`
   `comp_multi_private.h:1156` versus the weave entry points.)

**And one stale belief to retire before it costs someone an afternoon:** there
is **no #868 repaint disarm on Android**. `#868` is exclusively the
Windows/D3D11 late-weave repaint machinery
(`comp_weave_latency_win.h`, `comp_d3d11_target.cpp`, `queue_lock_layer.c`);
`grep -rn 868 src/xrt/compositor/multi/` finds nothing. The Android locking
story is two locks and no repaint arbiter: `mc->weave.mutex` and
`vk_queue_lock(vk->main_queue)`.

---

### (b) #1377 — satellite present for APP-class sessions

**The shape.** Do not blit the client's woven surface onto the overlay: under a
0.677 container the client's surface is woven at *logical* size, and rescaling a
woven image is the exact bug the satellite exists to avoid. Instead do what the
weave path already does — `weave_satellite_present()` comments it at
`comp_multi_weave_android.c:1470`: *"Physical-rect mode weaves at win*scale
already, so the output IS the on-panel size: present 1:1."* So the session's DP
must **run at the physical rect** into a runtime-owned image, and that image is
blitted 1:1.

```
app (unmodified, _hosted, routed by debug.dxr.force_ipc)
  └─ layers ─IPC─▶ multi_compositor
                     ├─ per-tile blit ──▶ flip_sbs atlas   (composed, PRE-DP)  ← the source
                     ├─ process_atlas ──▶ sat_out_image  (PHYSICAL w×h)        ← new destination
                     │                     └─ weave_satellite_present() 1:1 ──▶ overlay ──▶ panel
                     └─ tile-0 mono ────▶ ct->images[i]  (the app's own surface, LOGICAL)
                                            └─ placement + input anchor, and the
                                               picture that shows through the 0.80
                                               obscuring-opacity clamp
```

**Answering the acceptance question directly** — *"the app's own surface shows…
what?"* — **correct mono 2D**, not black and not a second woven copy. Three
reasons, in order of force:

1. Android's anti-tapjacking clamp composites the overlay at **α = 0.80** on a
   stock device (field-measured `alpha: 204`, `oem-android-platform-requirements.md`
   §R6). 20 % of whatever is underneath blends *through* the weave. Black would
   darken it; a woven copy would double-weave it into per-eye crosstalk — that
   crosstalk was the decisive P0 bring-up bug. Mono 2D is the only under-content
   that degrades gracefully.
2. It is the picture the moment `sat_failed` latches, so there is no black gap
   on fallback.
3. It keeps the window a live placement + input anchor, which is the whole
   Architecture-A-clean promise.

That is also why **B0 is a prerequisite, not an optional extra**: producing that
mono 2D on the OOP path is precisely the port of `736db35de`.

| # | Step | Anchors | Effort |
|---|---|---|---|
| **B0** | **Port the container-scaled degrade to the OOP per-session path.** `736db35de` shipped it only in `comp_vk_native`; `render_session_to_own_target()` has the same exposure and none of the fix. Same tell (window rect exceeds panel), same three effects: collapse the effective layout to tile 0, request hardware 2D + `on_pause` the session DP's lens preference, force zero-copy off. Lands standalone, testable with `weave_satellite=0`, and is *also* B5. | `comp_multi_system.c:2722` (tell input), `:2770`+ (apply); model `comp_vk_native_compositor.c:3412`, `:3476`, `:5676` | **2 d** |
| **B1** | **Make `weave_satellite_present()` source-agnostic.** It reads `mc->weave.{out_image,out_w,out_h,win_*,have_geometry}` today. Introduce `struct weave_satellite_frame {VkImage src; VkImageLayout src_layout; uint32_t src_w, src_h; int32_t win_x, win_y; uint32_t win_w, win_h; bool have_geometry;}` and pass it in; the weave-submit call site fills it from `mc->weave.*`. Pure refactor — byte-identical behaviour, reviewable on its own. Keep `sat_*` exactly where it is. **Fix the latent layout bug while here** (see the risk table): the present's `out_to_src` barrier declares `oldLayout = COLOR_ATTACHMENT_OPTIMAL` at `:1414`, but the weave submit's final `out_to_general` barrier at `:2237`–`:2246` leaves the image in `VK_IMAGE_LAYOUT_GENERAL` before the fence wait. Carrying the layout in the struct makes the mismatch impossible to reintroduce. Also re-shape `weave_satellite_effective_scale()`'s "must be called under `weave.mutex`" contract into the doc comment. | `comp_multi_weave_android.c:1337` (signature), `:1414`, `:2237`, `:2291` (existing caller), `:1239` (clear stays as-is) | **1 d** |
| **B2** | **One physical-rect derivation, two callers.** Re-shape `weave_satellite_effective_scale()` to take `(win_x, win_y, win_w, win_h)` rather than reading `mc->weave.*`, and collapse the **duplicated** `debug.dxr.satellite_miniwindow_scale` parse (`:881` and `:1211`, both defaulting `0.67f` independently) into one. Add `session_satellite_rect(mc, *x,*y,*w,*h)` reading `session_render.window_screen_*`. | `comp_multi_weave_android.c:846`, `:881`, `:1211`; `comp_multi_private.h:487`–`:496` | **1 d** |
| **B3** | **Per-session satellite output image** at **exactly** the physical size, `COLOR_ATTACHMENT | TRANSFER_SRC`, plus its render pass/framebuffer; rebuilt when the physical rect changes (rect generation is already tracked). Mirror `weave_create_output()`. Two reasons the intermediate is not optional: the overlay swapchain is created with **`VK_IMAGE_USAGE_TRANSFER_DST_BIT` only** (`comp_multi_weave_android.c:748`), so a DP can never render into it directly; and the present is a `vkCmdBlitImage` with **`VK_FILTER_NEAREST`** (`:1551`), so any size mismatch is a nearest-neighbour resample of a woven image — the exact failure the satellite exists to prevent. The Android DP is self-submitting (Leia CNSDK), so it also needs `set_target_color_view(dp, sat_out_view)` — omit it and the weave is silently skipped every frame (the #510 M2 failure mode). | new fields beside `comp_multi_private.h:403`–`:408`; model `comp_multi_weave_android.c:349` `weave_create_output()`; usage `:748`; blit `:1551`; view call `comp_multi_system.c:3484` | **2 d** |
| **B4** | **Divert `process_atlas` when the lease is held.** At the call site pass `framebuffer = sat_out_fb`, target image `sat_out_image`, `framebufferWidth/Height = phys_w/phys_h`, `canvas = 0,0,phys_w,phys_h`. **The atlas source is unchanged** — `flip_sbs_image`/`flip_sbs_view`, the composed image pre-DP, exactly as the design says. Note that this is the *content* being downscaled (logical view dims → physical destination), which is legitimate; it is only the *woven* result that must never be resampled. | `comp_multi_system.c:3484`–`:3502` | **2 d** |
| **B5** | **Client surface = tile-0 mono, same frame** (B0's machinery, now driven by "the satellite has this session" rather than by "scaled"). | `comp_multi_system.c` `submit_and_present:` block, `:3520`+ | **1 d** |
| **B6** | **Call the satellite.** After the session render's submit + fence, under `mc->weave.mutex`, call `weave_satellite_present(vk, mc, &frame, 1, &rect)`. The satellite takes `vk_queue_lock` for its own submit (`comp_multi_weave_android.c:1598`, `:1623`); the session path already does the same at `:3588`. **Ordering rules to write down, both already true and both easy to break:** (i) never take `list_and_timing_lock` inside `weave.mutex` — the #1278 comment states it at `comp_multi_system.c:5919`–`:5922`; (ii) `render_per_session_clients_locked()` runs **holding `list_and_timing_lock`** (`comp_multi_system.c:6067`–`:6076`). Taking `weave.mutex` inside it is fine — that is already the established order, proven by the #1278 pass at `:5927`, which does exactly this. What is **not** fine is calling `weave_ensure_mutex()` there: it re-locks the already-held, non-recursive `list_and_timing_lock` (`comp_multi_weave_android.c:132`). **That is a self-deadlock, not a style note.** Ensure the weave mutex at session-render init instead. | `comp_multi_system.c:3588`–`:3640`, `:6075`; `comp_multi_weave_android.c:131`, `:1598` | **1.5 d** |
| **B7** | **Gate + fallback.** `session_satellite_wanted(mc)` = `weave_satellite_wanted(mc)` **AND** `window_rect_generation != 0` **AND** `android_overlay_lease_held()` (#1376; the no-op default always grants, so B can be brought up before A lands). Any failure path already latches `sat_failed` and the next frame renders straight to `ct` — that is the fallback, unchanged. | `comp_multi_weave_android.c:621`, `:1015` | **0.5 d** |
| **B8** | **Idle + lifecycle parity.** `mc->weave.last_submit_ns` is stamped only by the weave submit (`:2274`). Stamp it from the session path too, so the #1278 pass clears the overlay when an APP-class client stops rendering, on resume, and on exit. Rotation needs nothing new: the orientation-mismatch rebuild keys off the source dims, which now follow the window. | `comp_multi_weave_android.c:2274`; `comp_multi_system.c:5926`–`:5936` | **1 d** |

**Total ≈ 12 engineer-days** of implementation, plus device bring-up. On the
P0 evidence, budget device bring-up at roughly the implementation again: P0's
own delta was "one blit" and it still cost an overnight session of
phase-beat, backdrop, `IN_USE`, and α = 0.80 discoveries.

**Deliberately out of scope, recorded so it is a decision and not an oversight:**
the post-weave 2D chrome — LOCAL_2D layers (`comp_multi_system.c:3497`), the HUD,
the workspace chrome pill, the taskbar overlays and cursor (`:3520`–`:3535`) —
stays on the client's own target and is **not** promoted onto the overlay. It is
flat 2D by construction; putting it on an unscaled full-panel surface while the
window is scaled would place it wrong. It therefore renders at container scale.
Acceptable now, revisit under C-single.

#### Risks (b)

| Risk | Why it bites | Mitigation |
|---|---|---|
| **Hot-path Vulkan under the weave mutex** | The satellite present acquires a swapchain image with a 100 ms timeout (`:1372`) and submits, all inside `mc->weave.mutex`. On the weave path that mutex is held by a *client's* synchronous IPC call; on the session path it will be held by the **service render thread**, which also drives every other client. A wedged overlay acquire stalls the panel, not one app. | Keep the 100 ms bound, and treat a timeout as "skip this frame, present the client's own target", never as a retry-in-place. Do not add work inside the mutex beyond what B1 moves there. |
| **Lens arbitration between the APP-class client and the satellite** | Two DPs now exist per client: `session_render.display_processor` (weaving into `sat_out_image`) and `mc->weave.dp` (the weave-submit path's, which the #1278 idle release pokes directly at `comp_multi_system.c:5927`). An APP-class session has no `mc->weave.dp`. The idle clear must not dereference it. | `comp_multi_weave_android_satellite_clear()` is already `dp`-free; the surrounding #1278 block guards on `mc->weave.dp != NULL` for the `on_pause` but calls `satellite_clear()` inside that same guard — **that guard must be relaxed** for APP-class, or the overlay never clears for a demo. This is a real, specific bug the plan would otherwise ship. |
| **The 0.80 obscuring-opacity clamp** | The overlay is `TYPE_APPLICATION_OVERLAY` + `FLAG_NOT_TOUCHABLE` (`android_custom_surface.cpp:191`, `:201`) — untouchable, which is what makes input passthrough free. Nothing in the runtime keeps it at full opacity: the OS clamps *any* obscuring overlay to `maximum_obscuring_opacity_for_touch`, default **0.8**, measured as `alpha: 204` in the HWC layer list. | Dev: `settings put global maximum_obscuring_opacity_for_touch 1.0` before **every** device run — without it the result is a 20 % ghost that reads as a weave bug. Ship: trusted-overlay or a per-package exemption, already filed as `oem-android-platform-requirements.md` §R6. **Screenshots cannot see this** — it is HWC-level blending; dump the layer list. |
| **B0 changes the `weave_satellite=0` baseline** | #1377 test 11 expects "today's behaviour exactly (2D-correct, scaled double image)". After B0, `weave_satellite=0` in a mini-window shows *correct 2D* instead. That is better, but it is a change. | Land B0 first, on its own, and re-baseline test 11 to "correct 2D, no double image" explicitly. |
| **Latent layout-declaration mismatch on the source image** | `weave_satellite_present()`'s `out_to_src` barrier declares `oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` (`comp_multi_weave_android.c:1414`), but the weave submit's own final barrier leaves `out_image` in `VK_IMAGE_LAYOUT_GENERAL` (`:2237`–`:2246`) before the fence wait. It works today because the driver tolerates it; a validation-layer build or a stricter driver will not, and B4 adds a *second* producer with a *third* layout. | B1 carries the layout in the frame struct. Fix it in the refactor, not later. |
| **Prop is read once per client and cached** | `weave_satellite_wanted()` latches `sat_checked`/`sat_enabled` on first call (`comp_multi_weave_android.c:621`–`:634`). `setprop debug.dxr.weave_satellite 1` mid-run does nothing for a client that already asked. | Every ladder rung that flips the prop must restart the client (and, for a satellite slot, let the slot process exit — `SATELLITE_EXIT_GRACE_MS` is 3 s, `MonadoService.kt:260`). |
| **The rect is the app's, and an off-panel drag trips the tell** | The P1 tell is "bounds exceed the panel", which an unscaled window dragged off-edge also satisfies. `736db35de` accepts the same false positive. | Unchanged direction of failure: costs 2D content, never a broken weave. Not reachable on this OEM (mini windows are clamped in-panel). |

---

### (a) #1376 — the overlay ownership lease

**The structural blocker found while planning this:** the slot broker is
**Java-only and unreachable from native code.** `grep -rn SlotBroker` over
`*.c/*.cpp/*.h/*.hpp` returns zero hits; `ISlotBroker.aidl` has exactly three
transactions (`acquireSlot` `:48`, `releaseSlot` `:51`, `getSlotCount` `:54`) and
**no callback interface**, so the broker cannot revoke anything from a holder
today. A slot record is `Owner(pkg, pid, tokens)` (`SlotBroker.kt:33`–`:34`) —
there is nowhere to hang "holds the overlay". The lease is therefore not a
policy tweak on an existing channel; the channel does not exist.

| # | Step | Anchors | Effort |
|---|---|---|---|
| **A1** | **Choose the focus signal** (design, must be first). There is none on Android (see the map above). The cheapest honest source is the one already crossing processes for free: `WindowWatcherService` serialises the a11y window list into `files/dxr_occlusion.bin` and the satellite reads it with no IPC at all. `AccessibilityWindowInfo` carries `isFocused()`/`isActive()` and the owning package. Extend that record by one bool + a package string rather than inventing a second focus authority — ADR-035 D2 is explicit that there is one. **Caveat that must be stated in the design, not discovered on device:** the watcher is **OFF by default** and is killed by `am force-stop` on the runtime package, re-binding only on a settings retoggle. With no watcher there is no focus, so the tie-break degenerates to "incumbent keeps it". | `comp_multi_weave_android.c:1039`–`:1106`; `comp_d3d11_service.h:779` (the Windows precedent); `ipc_server.h:459` | **0.5 d** |
| **A2** | **The native lease façade** — `auxiliary/android/android_overlay_lease.{h,c}`: `acquire(tag, slot, container_scaled, focused)`, `release(slot)` (synchronous), `held(slot)`, `set_revoke_cb(cb, data)`. **Default backend = always-grant**, so with one client the behaviour is bit-for-bit today's. **LANDED** (`u_overlay_lease.{h,c}`), no callers. | `auxiliary/util/u_overlay_lease.h`; to be consumed at `comp_multi_weave_android.c:621` and by B7 | **done** |
| **A3** | **The binder backend.** `ISlotBroker.aidl` grows `boolean acquireOverlayLease(int slot, IBinder token, int flags)` / `void releaseOverlayLease(int slot, IBinder token)`; a new one-way `IOverlayLeaseCallback.aidl` gives the broker the revoke path it does not have. `SlotBroker.Owner` grows `holdsOverlay`. Native reaches it through `MonadoImpl` — the only existing Java↔native service seam — mirroring `nativeWindowScreenRect` in the opposite direction. **Hard rule: no binder round-trip on the weave hot path or inside `weave.mutex`.** Acquire/release fire on transitions only; the per-frame read is a process-local atomic the revoke callback updates. | `ISlotBroker.aidl:48`,`:51`,`:54`; `SlotBroker.kt:33`,`:43`,`:102`,`:115`; `MonadoImpl.java:284`–`:368`; `service_target.cpp:325` (the pattern) | **3 d** |
| **A4** | **Synchronous clear on release.** Release calls `comp_multi_weave_android_satellite_clear()` **and waits for its present to retire** before the binder release returns. This is the `91f071770` bug's structural fix. The incoming holder's `weave_satellite_ensure()` already tolerates a lingering `BufferQueue` connection with a bounded `VK_ERROR_NATIVE_WINDOW_IN_USE` retry (120 frames), so a slow hand-off degrades to a few dropped frames rather than a latch. | `comp_multi_weave_android.c:1239`, `:930`–`:940` | **0.5 d** |
| **A5** | **The arbitration predicate, as a pure function.** `overlay_lease_winner(const struct lease_candidate *, size_t n, const char *focused_pkg) -> ssize_t`. Rules: a scaled container beats an unscaled one; among scaled, focus wins; on a tie the **incumbent keeps it** (anti-thrash — the loser's degradation is a double image, so trading the lease every few frames is worse than either steady state). Host-tested exactly like `u_sandbox_route_prop_selects` — that precedent exists specifically because Android policy decisions are pure string/number comparisons that must not need a device to be right. **LANDED** as `u_overlay_lease_select()` + `tests/tests_aux_overlay_lease.cpp` (9 cases, 54 assertions), including the one that guards P0: a lone unscaled client still takes the overlay. | `u_overlay_lease.c`; `tests/tests_aux_overlay_lease.cpp` | **done** |
| **A6** | **Diagnostics.** One WARN per lease transition naming winner, loser and reason; the loser's degradation logged as a stated outcome, never silent (the design says so; make it a line of code). Surface in the #558-adjacent diag dashboard. | `comp_multi_weave_android.c` | **0.5 d** |

**Total ≈ 6.5 engineer-days**, of which **A2 and A5 are done** (≈ 2 d) and **A3 is the only genuinely new plumbing**. Remaining ≈ 4.5 d.

#### Risks (a)

| Risk | Why it bites | Mitigation |
|---|---|---|
| **A binder call on the weave path** | The lease lives in another process. A naive `held()` implementation is a synchronous binder transaction inside the per-frame weave, under `weave.mutex`, with the panel behind it. | A3's rule: transitions only, atomic read per frame, revoke pushed not polled. Make this a review gate, not a comment. |
| **Focus does not exist yet** | A1 is a dependency, not a detail. Building A5's tie-break before A1 means unit-testing a predicate against an input nothing produces. | Land A1's feed extension first; A5's predicate takes `focused_pkg` as an argument precisely so it is testable before the feed ships. |
| **Watcher-off is the default state** | The a11y watcher is OFF by default and dies on `am force-stop`. Every ladder step below therefore has a "did you retoggle `enabled_accessibility_services`?" precondition. | State it in the ladder (it is). Design the no-focus path — incumbent keeps it — as the *specified* behaviour, not an accident. |
| **Four slots, then nothing** | ADR-036 D3 pre-declares `:dxr0..3`. The fifth client shares the main-process service, and that path has never been run with the satellite at all. | #1376 test 6 is exactly this. Worst acceptable outcome: the fifth client does not get the overlay and does not corrupt it for the other four. |
| **Two overlay creators already exist** | `service_target.cpp:234` (the #558 service overlay, `span_system_bars=false`) and `comp_multi_weave_android.c:910` (the satellite, `span_system_bars=true`) both call `android_custom_surface_async_start`, and stash the handle in *different* places — a process-global vs per-`multi_compositor`. Today they never collide only because they never run in the same process. | The lease must arbitrate the *panel*, not the process. Note it explicitly in A2's contract; C-single removes the second creator entirely. |

---

### Ordered device-validation ladder (NP02J)

Preconditions for **every** rung:

```bash
adb shell settings put global maximum_obscuring_opacity_for_touch 1.0   # else a 20% ghost reads as a weave bug
adb shell settings get secure enabled_accessibility_services            # WindowWatcher must be on; retoggle after any force-stop
adb shell wm fixed-to-user-rotation enabled                             # tablet launches must be landscape; gate on measured rotation=1
```

And two traps that will otherwise be rediscovered on every rung:

- **`debug.dxr.weave_satellite` is latched per client on first use**
  (`comp_multi_weave_android.c:621`). Flipping it mid-run does nothing — restart
  the client, and give a satellite slot process its 3 s exit grace
  (`MonadoService.kt:260`) before relaunching.
- **`am force-stop` on the runtime package kills the a11y window watcher**, and
  Android only rebinds it on a settings retoggle. Occlusion (and, after A1,
  focus) silently reverts to "none" until you retoggle
  `enabled_accessibility_services`. The failure is a *missing* clip, not an
  error.

| Rung | What | Gate |
|---|---|---|
| **0** | **The routing policy that already landed and has never run.** #1377 Stage 0 tests 1–6 (`debug.dxr.force_ipc <pkg>` targets ONE app; `1` still device-wide; `pkg*` prefixes; empty/bogus route nothing; `XRT_FORCE_MODE=native` wins; a `<pkg>:dxrN` slot matches its package). Costs nothing, blocks everything. | modelviewer logs `Hybrid mode: using IPC/service compositor`; every other app still logs `using in-process native compositor` |
| **1** | **Single-client satellite regression on current `main`.** Browser + `weave_satellite=1`, P0 tests 1–4. Establishes that the baseline this plan modifies still passes before anything is modified. | golden parity fullscreen; "crisp 3D" in the mini-window; `=0` bit-for-bit |
| **2** | **B0 alone** (`weave_satellite=0`, modelviewer via `force_ipc`). OOP port of the scaled degrade. | mini-window shows **correct 2D**, no double image; `HW_DBG_CNSDK: lens preference RELEASED`; unscaled control (`am task resize`) resumes weaving |
| **3** | **B1–B8, single client.** #1377 tests 7–9: modelviewer fullscreen at parity with its in-process golden; **modelviewer in the OEM mini-window crisp 3D** (the acceptance case); touch/drag reach the app through the overlay and the weave tracks the drag. | the app's own surface shows mono 2D under the overlay throughout |
| **4** | **Lifecycle**, #1377 test 10: rotation both directions, background/resume, app exit. | no stale overlay rect; overlay clears within the #1278 2 s idle window |
| **5** | **Re-baselined test 11:** `weave_satellite=0` with the app still routed to IPC → rung 2's picture (correct 2D), not a regression. | — |
| **6** | **#1376 with two clients.** Only now is the lease testable. #1376 tests 1–5: mixed pair (browser fullscreen unscaled + a demo in the mini-window) **both crisp simultaneously**; two scaled → focused one crisp; 20× hand-off with no frozen rect and no `IN_USE` storm; `am force-stop` the leaseholder → overlay clears and the lease moves within 2 s; lens refcount ORs (no `Disable` while either weaves, exactly one on the last exit). | — |
| **7** | **#1376 test 6** — five clients, slot exhaustion. The fifth must not corrupt the overlay for the other four. | — |
| **8** | **#1377 test 12** — repeat rungs 3–5 for gaussiansplat, mediaplayer, earthview. | — |
| **9** | **Only then**, the manifest line (`com.displayxr.force_ipc`) into each demo repo. It is a release in someone else's repo; do it once the picture is signed off, never before. | — |

Rungs 0–2 need no new runtime code beyond B0 and are the highest
information-per-hour on the ladder: rung 0 validates a merged, unexercised
policy change, and rung 2 closes a gap `736db35de` explicitly left open.

## Risks / open questions

- Does the OEM allow a `TYPE_APPLICATION_OVERLAY` surface to cover a freeform
  window's caption region? (Best-effort in P0; OEM ask otherwise.)
- Overlay present jitter vs the in-window path (the overlay present is a
  second SurfaceFlinger client; measure with the #663 pipelined-weave lens).
- Transparency: the overlay must punch through where no window weaves — the
  woven region is opaque, everything else transparent black; verify no
  full-surface GPU composition cost on the SoC (Adreno).
- Lens lifecycle rides #1278 (weave-idle release) — same residual as in-app.
