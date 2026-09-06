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


## Risks / open questions

- Does the OEM allow a `TYPE_APPLICATION_OVERLAY` surface to cover a freeform
  window's caption region? (Best-effort in P0; OEM ask otherwise.)
- Overlay present jitter vs the in-window path (the overlay present is a
  second SurfaceFlinger client; measure with the #663 pipelined-weave lens).
- Transparency: the overlay must punch through where no window weaves — the
  woven region is opaque, everything else transparent black; verify no
  full-surface GPU composition cost on the SoC (Adreno).
- Lens lifecycle rides #1278 (weave-idle release) — same residual as in-app.
