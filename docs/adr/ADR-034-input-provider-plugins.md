# ADR-034: Input Providers Are a Second Plug-in Type, Not a Display-Processor Extension

**Status:** Accepted (Phase 1 implemented — #823; role arbitration amended 2026-08-15, see *Amendment 1*; rig-relative pose composition amended 2026-08-15, see *Amendment 2*; presence-ranked hierarchy — all claiming providers stay resident, roles follow presence — amended 2026-08-16, see *Amendment 3*, whose addendum makes the hand-tracking source follow presence too; navigation became a third arbitrated role, composed by the runtime — amended 2026-09-11, see *Amendment 4*). The process that **hosts** input providers is not decided here: today they load in-process, and moving them out is [ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md) D4.
**Date:** 2026-08-02

## Context

DisplayXR inherited Monado's complete VR input stack and kept it: the OpenXR
action system (`oxr_api_action.c`, `oxr_input.c`), the full interaction-profile
table (`auxiliary/bindings/bindings.json`), the `xrt_device` input/haptics
vtable, all five reference spaces, and real 6DOF action-space location over
IPC. What the fork removed was one layer down — the 34 headset hardware
drivers and the OpenVR state trackers. The one in-tree input source today is
the qwerty driver, which the sim-display builder deliberately wires up as an
emulated left/right motion-controller pair
(`target_builder_qwerty_input.c`).

This leaves a gap: **an externally-shipped tracking system has no supported
way to surface real motion controllers to the runtime.** The vendor plug-in
interface (`xrt_plugin.h`, ADR-019) covers exactly the display side — probe,
one head device, display info, eye tracking, a pose-source hook — and
nothing else. Concrete demand exists: camera+IMU tracked consumer
controllers (Joy-Con-class hardware tracked by a display's on-board camera),
phone-as-controller systems, and future hand-tracking sources all need to
feed 6DOF pose + buttons + haptics into the action system so unmodified
OpenXR titles can consume them.

Terminology note: throughout this ADR, "motion controller" means a tracked
hand-held input device. It is unrelated to the *workspace controller*
(shell) concept of ADR-014/016/018/024.

### Options considered

1. **Extend the display-processor vtable** (append `create_input_devices` to
   `xrt_plugin_iface` under ADR-020's append-only rule).
2. **A second, independent plug-in type** with its own header, negotiation
   entry point, ABI version, and discovery root.
3. **A wire protocol only** (runtime listens on loopback; external process
   feeds poses) with no plug-in ABI at all.

## Decision

**Input providers are a second plug-in type** — a separate contract in
`xrt/xrt_input_plugin.h` with its own `xrtInputPluginNegotiate` entry point,
its own `XRT_INPUT_PLUGIN_API_VERSION_CURRENT` (starting at 1), and its own
discovery root (`HKLM\Software\DisplayXR\InputProviders` on Windows; JSON
manifests on POSIX). Discovery, ProbeOrder semantics, and ABI gating mirror
the display-processor loader (`docs/specs/runtime/plugin-discovery.md`,
ADR-020) and share its loader plumbing.

A provider exposes **N `xrt_device`s** (left/right motion controllers
first-class; the device self-describes via `device_type` and its claimed
interaction profile, so generic trackers and hand-tracking sources fit later
without vtable changes). Devices are ordinary `xrt_device`s: pose via
`get_tracked_pose` (providers are expected to push timestamped samples into
`m_relation_history` and predict on demand), input via `update_inputs`,
haptics via `set_output`. The provider owns its own threads and transport.

**Role arbitration:** the system builder loads input providers before the
qwerty fallback. If a provider supplies a left/right pair *and its
hardware is actually present*, it claims `xrt_system_roles`; qwerty still
registers (debug value) and takes the hand roles whenever the provider
does not — see *Amendment 1* for the presence gate, which replaced the
original unconditional "provider wins" rule. A registry override
(`HKLM\Software\DisplayXR\Input\ForceQwerty`) forces the fallback for
debugging — a registry gate, not an env var, per project convention. v1
activates a **single provider** (first successful probe in ProbeOrder
wins, exactly like the DP loader); multi-provider composition is
deferred.

**Hand-tracking role arbitration (#825 Tier 2):** the same pass also fills
the static hand-tracking roles
(`xrt_system_devices::static_roles.hand_tracking.{unobstructed,conforming}.{left,right}`),
which gate `XR_EXT_hand_tracking` (system support, tracker creation, and
joint locates all resolve through them). Devices self-describe here too:
`supported.hand_tracking` gates, and the present `XRT_INPUT_HT_*` input
names say which hand and which data source (unobstructed = optical,
conforming = controller-derived) the device serves — one device may claim a
controller role, hand-tracking roles, both (ultraleap, sim_input), or
neither. First claimant wins per role, mirroring the controller rule.
Absence is normal: a provider whose devices carry no hand-tracking inputs
(net_input feeder) leaves the roles empty and `XR_EXT_hand_tracking`
reports unsupported, exactly as before ADR-034.

Option 3 is not rejected — it is demoted to *inside* a provider: the
reference `net_input` provider wraps a documented loopback wire protocol
(derived from Monado's removed `remote` driver), so processes that cannot
ship a DLL still have a path. But the runtime-facing contract is the plug-in
ABI, not the socket.

## Consequences

- **A tracking vendor is not a display vendor.** The two contracts version,
  ship, and get certified independently. A display OEM's plug-in never grows
  input obligations; an input vendor never touches weaving. This is the main
  argument against option 1, which would have coupled the two vendor
  populations to one vtable's evolution and forced every DP vendor to
  understand (and ABI-track) input semantics they don't implement.
- **The runtime core stays driver-free** (ADR-019 discipline extends to
  input): in-tree providers (`sim_input`, `net_input`) build as plug-in
  DLLs like sim-display; restored Monado driver code is vendored into
  providers, never linked into the runtime.
- **The intact Monado input stack becomes load-bearing.** No in-repo app
  exercises the action system today; the first real provider will be its
  first real consumer, and latent bugs surface then — this is accepted and
  is exactly why the sim provider + an actions-mode test app ship in the
  same phase.
- **DisplayXR becomes a two-sided interface**: display vendors on one side,
  input vendors on the other, with the same discovery/ABI story on both.
  The vendor-onboarding narrative extends naturally.
- Costs: a second loader path to maintain (mitigated by sharing plumbing),
  a second ABI to police under ADR-020, and an arbitration policy in the
  builder that must stay predictable as providers multiply.

## Amendment 1 — Role arbitration is presence-gated and dynamic (2026-08-15)

**Status:** Accepted. Supersedes the "provider wins for the process
lifetime" reading of *Role arbitration* above.

### What was wrong

Phase 1 arbitrated **once, at system build**, and asked only "did a
provider create devices". Nothing anywhere asked whether the hardware
behind those devices existed. The consequences were not hypothetical —
both were observed on a dev box after #825 landed:

- The Ultraleap provider creates its devices whether or not a Leap Motion
  is plugged in (`LeapOpenConnection` is asynchronous and succeeds against
  a service with no device). Registered-but-unplugged therefore displaced
  qwerty permanently and the user had **no controllers at all** — the
  regression was worst for hosted and legacy WebXR apps, which have no
  window of their own and depend on the qwerty fallback.
- With Ultraleap disabled, ProbeOrder fell through to `sim_input`, whose
  synthetic motion pattern then drove the hand roles: **phantom
  controllers** sweeping through the scene with nothing behind them.

The deeper problem was that `xrt_input_plugin_iface` had no way to ask
the question. `probe()` answers "should I be loaded", which is a
registration decision made once; there was no liveness signal at all.

### Decision

1. **A liveness slot on the ABI.** `xrt_input_plugin_iface::get_presence`
   returns `xrt_input_provider_presence`
   (`UNKNOWN` / `ABSENT` / `PRESENT`) — a non-blocking read of state the
   provider's own transport thread maintains. Appended at the end of the
   vtable under `struct_size` cover, so this is **additive**: the API
   major stays 1 and providers built before it keep loading. A NULL slot
   (or a `struct_size` that predates it) means "assume present", which is
   exactly the Phase-1 behaviour, so no provider is broken by the change.
   `UNKNOWN` is read as *not yet present* — a provider that can never
   tell should leave the slot NULL rather than return it.

   Presence is deliberately NOT "is a hand currently visible". Hardware
   that is attached but sees nothing reports `PRESENT` and simply
   deactivates its inputs; making an empty tracking volume flip the roles
   would bounce controllers back to qwerty every time the user's hands
   left the frame.

2. **Roles become dynamic; devices do not.** Both candidate pairs — the
   provider's and qwerty's — are created once at system build and both
   stay in `xrt_system_devices::xdevs`, exactly as before. Only the
   *role assignment* moves. `target_input_arbiter.c` installs its own
   `xrt_system_devices::get_roles`, which re-reads presence and points
   `xrt_system_roles::left` / `::right` at whichever pair should own the
   hands, bumping `generation_id` on each flip.

   This answers "re-evaluate per app start" without inventing a
   session-create hook: the OpenXR state tracker already re-reads roles in
   `xrSyncActions`, rebinds actions and queues
   `XrEventDataInteractionProfileChanged` when the generation moves, and
   the IPC layer already forwards `get_roles` to the service. So
   arbitration is re-run per app start **and** mid-session on plug/unplug,
   over IPC too, with no new plumbing and nothing to poll.

   The **static** hand-tracking roles are not arbitrated — they cannot be,
   by the `xrt_system_devices` contract, and there is nothing to arbitrate
   with: qwerty has no hand tracking. An absent optical tracker reports
   inactive joints, which is what `XR_EXT_hand_tracking` expects.

3. **`sim_input` leaves the product path.** It was a #825 debugging aid.
   Its `probe()` now declines unless `DXR_SIM_INPUT` is set in the
   environment, so registration alone can no longer put synthetic
   controllers in front of a user, and `register_dev_plugin.bat input`
   requires the literal `sim` argument instead of defaulting to it. An
   env var rather than the usual registry gate on purpose: this is a
   per-run developer switch, not machine configuration.

4. **Absence is not a fault.** `displayxr-cli selftest` treats "provider
   registered but its hardware is absent" and "every provider declined"
   as passes — qwerty holding the roles is the correct outcome in both.
   Only a provider that could not be *dispatched* (missing entry point,
   ABI-major mismatch) still fails, which the loader now reports
   separately from a clean decline.

### Consequences

- **The fallback is real again.** Alt/Ctrl + mouse controller actuation
  (`qwerty_win32.c`) is restored for every app class that relies on it.
- **Interaction profiles change at runtime.** qwerty is
  `XRT_DEVICE_WMR_CONTROLLER`, most providers are
  `XRT_DEVICE_SIMPLE_CONTROLLER`, so a flip is a genuine profile change.
  That path already existed for dynamic roles; this is its first
  in-tree producer, and apps that ignore
  `XrEventDataInteractionProfileChanged` will notice.
- **Providers own an honest presence answer.** A provider that reports
  `PRESENT` unconditionally is back to the Phase-1 failure mode. This is
  now the main thing to check when onboarding an input vendor.
- Interaction with #941 (the Ultraleap idle-disconnect watchdog): the
  watchdog now only fires while presence is `PRESENT`. With no hardware
  there is no tracking model to stop paying for, and staying connected is
  what lets the provider *see* a device get plugged in — a closed
  connection can never report one, so presence would otherwise pin at
  `ABSENT` forever. Across an idle disconnect presence is frozen, not
  cleared: the runtime closed the connection, the user did not unplug the
  device.

## Amendment 2 — A provider's tracking volume is bolted to the rig, not to the world (2026-08-15)

**Status:** Accepted. Adds a rule the original text left implicit, and one
that **deliberately diverges from HMD VR semantics**.

### The rule

On a 3D display the viewer, the panel and the provider's sensor are **one
physical assembly** — the *rig*. A Leap Motion sits on the same desk as the
display. So:

- **Voluntary rig motion — tracked input MUST follow, translation AND
  rotation.** Mouse-look yaws the camera; if the hands keep pointing the old
  way you cannot point at what you are looking at. WASD walks the camera; if
  the hands stay behind they leave the screen entirely.
- **Eye-tracked head parallax — tracked input MUST NOT follow, above all not
  rotation.** The user's real hand is physically above the sensor on the desk.
  If it swings when they lean or tilt, it destroys the one thing a 3D display
  gets for free — that your hands are where your hands actually are — and it
  fights the Kooima parallax that makes the display work.

In HMD VR, world-fixed hands are the correct answer and this rule would be
wrong. On a 3D display it is inverted, because the display does not travel
with the head. Every provider author inherits this; it is not optional
polish.

### Why the provider does not implement it

A provider's job stays exactly what Phase 1 said it was: report physically
honest poses in **its own tracking volume**. How that volume is anchored to
the world is a navigation question, and navigation is the runtime's business.
Putting rig-awareness in the provider would leak navigation semantics across
the vendor boundary (ADR-019) and make every future provider reimplement it —
differently.

### Decision

Composition happens **once, in the runtime's space graph**, so it applies
uniformly to grip/aim action spaces, `xrLocateSpace`, and the hand-joint base
(`xrt_space_overseer::locate_device`) — in-process and over IPC alike.

1. **The rig source is the head *device* pose**, not the view pose. Eye
   tracking is applied later, at view-pose level, and never reaches the head
   device pose (that is the fly camera qwerty drives). Composing against the
   device pose therefore excludes parallax *for free* — there is no filter to
   get wrong.

2. **The composition is a travelled DELTA, not head-parenting:**

   ```
   world = (rig_now ∘ inverse(rig_initial)) ∘ device_volume
   ```

   Re-expressing the volume relative to the head would double-count the
   standing height — the volume is anchored in stage space (an Ultraleap mount
   offset is y≈1.45) while the head sits at y≈1.6 — and hang the hands off the
   viewer's face. Moving by how far the rig has *travelled* is standard VR
   locomotion semantics (move the player, not the hands) and yields the rule
   exactly.

3. **Mechanically:** `u_space_overseer` gains a `U_SPACE_TYPE_RIG` node
   parented to the root, whose relation resolves to that delta, plus
   `u_space_overseer_set_rig_source()` /
   `u_space_overseer_set_device_rig_relative()`. The builder re-parents each
   provider device's tracking-origin space onto that node
   (`u_builder_roles_helper::rig_relative`, filled by
   `t_builder_add_input_provider_devices`). `rig_initial` is captured once,
   when composition is armed at build time.

4. **Qwerty is deliberately excluded.** Its controllers already compose
   against the qwerty HMD (`qwerty_device.c`, `follow_hmd`) — in the qwerty
   model the "HMD" *is* the rig. Marking them too would move them twice. Only
   a device whose space sits directly on the root may be marked; the setter
   refuses anything else, and refuses the rig source itself.

### Consequences

- **Provider poses are no longer world-fixed.** A provider author reading only
  Phase 1 would not expect this. It is the price of the volume being physically
  attached to a display that moves.
- **The delta is a pose, not a velocity.** Reported linear/angular velocities
  do not include the rig's own motion. Nothing consumes them for hands today;
  if something does, this is where to fix it.
- **Inert until used.** With no provider device flagged, the rig node is never
  created and the graph is byte-for-byte the old one — qwerty-only boxes and
  every non-provider device are untouched.
- **Recentering the rig carries the hands**, which is correct: a recenter is
  voluntary.
- Covered by `tests/tests_space_overseer_rig.cpp`, which pins both halves of
  the rule — including that an *un*flagged device stays world-fixed, so the
  divergence from HMD semantics stays a deliberate opt-in.

## Amendment 3 — Provider selection is a presence-ranked hierarchy (2026-08-15)

**Status:** Accepted. Supersedes "v1 activates a single provider (first
successful probe in ProbeOrder wins)" in *Role arbitration* above.

### What was wrong

Amendment 1 made the *roles* dynamic but left *provider selection* a
one-shot: the loader stopped at the first successful probe, so exactly one
provider DLL was ever resident. With two modalities registered (say Quest
controllers at ProbeOrder 40 and an Ultraleap at 50), unplugging the Quest
pair dropped the hands straight to qwerty — the Ultraleap was never even
loaded. The DP loader never had this problem: it consults every registered
plug-in and ranks them, which is exactly why "Leia wins if present, else
sim" works for displays.

### Decision

1. **The loader keeps every claiming provider resident.**
   `target_input_plugin_loader.c` collects all providers that load,
   ABI-pass and probe (ProbeOrder ascending), instead of returning the
   first. `target_input_plugin_get_count()/get_iface(i)/get_instance(i)/
   get_priority(i)` expose the ranked list; `get_active()` survives as an
   alias for index 0 (diagnostics).

2. **The builder creates every provider's devices.** All candidate pairs
   live in `xsysd->xdevs` for the process lifetime, exactly as Amendment 1
   established for the single pair. Hand-tracking roles are claimed
   first-claimant-wins across all providers, in priority order.

3. **The arbiter walks a ranked candidate list.** Qwerty is a candidate
   like any other — NULL iface (the keyboard is always present), priority
   `UINT32_MAX` — so the rule collapses to one sentence: *each hand goes
   to the highest-priority candidate that supplies that hand and reports
   PRESENT.* The walk is per-hand, so a one-handed provider leaves the
   other hand to the next rank rather than dragging it down. Presence
   verdicts are cached per candidate (~250 ms), generations bump exactly
   as before, and the IPC path is untouched.

### Consequences

- **Priority = ProbeOrder**, one convention across both plug-in types.
  Vendors already understand it from the DP side.
- **Roles can now move provider→provider**, not just provider↔qwerty. An
  app sees the same `XrEventDataInteractionProfileChanged` machinery
  either way.
- Hardware-validated on the win box: `ultraleap(50) → sim-input(200) →
  qwerty` fell through the ranks on a real USB unplug and climbed back on
  replug, frames unbroken; the two-candidate config (no sim-input)
  falls to qwerty as before.
- Every resident provider's DLL stays loaded even while absent — the cost
  of being able to see its hardware arrive. Providers that dislike this
  should decline in `probe()` (registration is opt-in per box anyway).

### Amendment 3 addendum — hand-tracking roles are dynamic too (2026-08-16)

The static hand-tracking roles stay exactly as Amendment 1 described —
build-time seed, first-claimant-wins across providers, gating
`XR_EXT_hand_tracking` support. What changed: `xrt_system_roles` now also
carries **dynamic** hand-tracking indices (`hand_tracking.{unobstructed,
conforming}.{left,right}`), covered by the same `generation_id` and riding
the existing IPC `get_roles` forwarding unchanged. The arbiter fills them
with the same presence walk, per slot; there is no qwerty floor here (the
keyboard has no joints), so with every carrier absent a slot parks at -1
and the tracker reports inactive joints. The OpenXR hand tracker
re-resolves its data-source devices from the session's cached roles
whenever the generation moves (`oxr_hand_tracker_resolve_sources`), so the
joint source hot-follows provider presence exactly like the controllers.
Hardware-validated: Leap unplug switched live joints to sim-input's
synthetic hands mid-session and back on replug.

## Amendment 4 — Navigation is a role, composed by the runtime (2026-09-11)

**Status:** Accepted. Adds a third arbitrated role alongside the hands, and
answers the question *Amendment 2* raised but deliberately did not settle:
who is allowed to move the rig.

### What was missing

Amendments 1–3 turned the **hands** into a role: ranked, presence-gated,
re-read on every `xrSyncActions`, and moved between claimants without
anyone's cooperation. The **rig** — the virtual camera the runtime's own fly
camera (WASD / mouse-look) drives — was not a role at all. It was a wire: the
display plug-in's `set_pose_source` hook, bound once at system build to the
fly camera and never re-read. So a tracking vendor whose *product* is
navigation (a controller drag, an IR-fused camera pose) had nowhere to plug
in, and the first external consumer to want one — an external navigation-provider consumer — carried a
private runtime fork instead.

That prototype is worth recording, because all three of its moves are traps
the next vendor would walk into independently:

- **Three new vtable slots** (`begin_navigation` / `get_navigation` /
  `end_navigation`) — a session-ownership protocol sitting next to the
  arbiter, with its own begin/end lifecycle, no presence gate and no
  `generation_id`. Two ownership protocols in one process disagree
  eventually, and the one without arbitration is the one that loses
  silently: nothing ranks a second claimant, and nothing hands the rig back
  when hardware is unplugged mid-drag.
- **Rebinding `set_pose_source`** from a runtime-side adapter — a one-shot
  display-plug-in hook repurposed as a live ownership channel. It makes
  every display vendor's contract grow an input obligation it should never
  carry (the argument against option 1 at the top of this ADR, recurring),
  and it still has no answer for "two providers, one rig".
- **The provider pre-cancelling the rig delta.** To keep its hands landing
  in the right place while it drove the camera, the provider composed the
  *inverse* of its own navigation into the controller poses it published.
  This is the *Amendment 2* "applied twice" trap seen from the other end:
  the runtime composes `rig_now ∘ inv(rig_initial)` onto every provider
  device, so a provider that pre-compensates is subtracting a transform the
  runtime is about to add. It looks correct for exactly as long as both
  sides agree about a number neither of them owns, and it fails invisibly —
  as drift, not as an error.

The shape of the fix was already in the tree. Navigation should be the same
kind of thing the hands are.

### Decision

1. **The rig is a role, arbitrated exactly like the hands.** A provider that
   can navigate creates ONE additional device of type
   `XRT_DEVICE_TYPE_NAVIGATION`, carrying `XRT_INPUT_GENERIC_NAVIGATION_POSE`
   and optionally `XRT_INPUT_GENERIC_NAVIGATION_RECENTER`. Its controllers
   stay controllers. `xrt_system_roles::rig` is then the index of the
   navigation device belonging to the highest-priority (lowest ProbeOrder)
   candidate that has one and reports `PRESENT` — the same walk, the same
   ~250 ms presence cache, the same `generation_id`, the same IPC
   forwarding, in `target_input_arbiter.c`.

   **The floor is an absence, not a candidate.** The fly camera supplies no
   navigation device, so `rig == -1` does not mean "no rig": it means *the
   runtime's own fly camera holds it*. That is the state every box without a
   navigating provider is in, and the state an unplug falls back to, with no
   separate teardown path to get wrong.

2. **A runtime-owned composer — not the provider — is the head's pose
   source.** The builder creates one `xrt_device` whose
   `XRT_INPUT_GENERIC_HEAD_POSE` is the rig pose `rig(t)`, and binds *it*
   through the existing `set_pose_source` hook in place of the fly camera.
   The display-plug-in contract is untouched — this is the same one hook it
   always had — and the composer is not in `xrt_system_devices::xdevs` and
   holds no role of its own.

   While the role sits on the floor the composer is a pass-through and the
   head's pose is byte-for-byte what it was before this amendment. While a
   provider holds it:

   - **Alignment.** At an epoch `h` the runtime computes one rigid transform
     `T_world_F = rig(h) ∘ inv(N(h))` — world ← the provider's own
     navigation frame — and thereafter `rig(t) = T_world_F ∘ N(t)`.
     Equivalently `rig(t) = rig(h) ∘ inv(N(h)) ∘ N(t)`: a delta taken in the
     provider's frame and applied in the rig's local coordinates. A
     non-identity `N(h)` (the provider's frame origin is private and
     arbitrary) is absorbed into the alignment, and a provider-local step —
     `N → N ∘ T(0,0,-1)` — moves the rig along its **own** forward, which is
     what a drag has to mean.
   - **Epochs.** Exactly three: the rig **holder** changes; `N` transitions
     invalid → valid; a newer recenter timestamp arrives. Deliberately *not*
     every `generation_id` bump — hand-role churn (an optical tracker
     unplugged, a controller hotplugged) shares that counter, and re-aligning
     on it would swallow whatever navigation step happened in the same poll.
   - **Continuity is an invariant, not a convention.** At a handover, at an
     invalid → valid transition and at any non-recenter re-alignment the
     first composed pose is *exactly* the last one returned, because
     `T_world_F ∘ N = (rig_last ∘ inv(N)) ∘ N`. While `N` is invalid the rig
     holds its last pose rather than snapping. There is one deliberate jump
     in the whole design, and it is the recenter.
   - **Recenter is durable, not a pulse.** `NAVIGATION_RECENTER` is a
     level-with-timestamp boolean: value `true`, timestamp = the moment of
     the most recent reset, never cleared. The composer keeps the last
     consumed timestamp and acts on `timestamp > last_consumed`. This is
     forced by the action system, not chosen for elegance: `xrSyncActions`
     sweeps `update_inputs` over **every** device (and so do the two IPC
     server paths), so a one-update pulse can be consumed by action sync
     before the composer ever polls, and the recenter would simply never
     happen — intermittently, and only on some paths. With a durable level,
     how many `update_inputs` calls land in between is irrelevant, and two
     resets between polls collapse to one (a recenter is idempotent).
   - **The recenter target is `rig_initial`** — the rig pose at system
     build, "home" — after which the runtime re-aligns there. Hands follow,
     which is correct and is exactly what *Amendment 2* says: a recenter is
     voluntary motion.
   - **Exclusivity.** The role holder is the only navigation source.
     WASD / mouse-look act **only** while the fly camera holds the role;
     while a provider holds it the fly camera is not read at all, and it is
     re-seeded at the current rig on handback so it continues from there
     instead of from wherever its own integrator sat while unread.

3. **A rig-local frame that removes the runtime's constants from the
   provider.** A provider that navigates publishes its controllers and joints
   with `tracking_origin->type = XRT_TRACKING_TYPE_RIG_LOCAL`: poses relative
   to the **display plane** — origin at the display centre, +X right, +Y up,
   +Z toward the viewer, metres. The builder anchors such an origin at
   `rig_initial`, so the *Amendment 2* delta collapses to

   ```
   world = rig(t) ∘ inv(rig_initial) ∘ rig_initial ∘ L = rig(t) ∘ L
   ```

   and the provider needs to know neither the standing height nor its own
   mount offset in stage space. A solver that already produces product poses
   `C_W` in its own navigated world publishes `L = inv(P(t)) ∘ C_W` next to
   `N(t) = P(t)`, and the single alignment maps the whole product world at
   once — no provider-side reference frame, no runtime rig knowledge.
   `XRT_TRACKING_TYPE_OTHER` / `_NONE` origins keep today's stage-anchored
   behaviour with their mount offsets intact, so no existing provider
   changes.

4. **The host iface becomes real storage, plus nominal display geometry.**
   Both loader paths used to hand `xrtInputPluginNegotiate` a **stack-local**
   host iface, so a provider that retained the pointer — a reasonable thing
   to do, and what the first navigating provider did — read dead stack the
   moment negotiate returned. There is now one persistent host iface per
   process, shared by every provider in it, and its first real callback is
   `get_display_geometry`: the panel size and the **nominal** viewer
   position, cached from the display plug-in's display info before any
   provider creates devices. Nominal is the whole point — it is geometry for
   a solver (how big the working volume is, where the plane sits), never a
   head pose and never the tracked eyes. Pointer **lifetime** and data
   **readiness** are different questions, so a call made too early gets its
   own result code and leaves the caller's struct untouched, rather than
   being confused with "this runtime has no geometry to give".

### Consequences

- **What a vendor does to navigate:** create one `NAVIGATION` device;
  answer `NAVIGATION_POSE` with the absolute pose of the rig in your own
  frame, un-parallaxed; clear that pose's validity bits to mean "hold the
  rig"; publish a durable recenter timestamp if you have a reset gesture;
  mark your controllers `RIG_LOCAL`. That is the entire surface. There is no
  session to open, no start epoch handed to you, and no initial rig to
  reason about.
- **What a vendor must never do:** compose navigation into the poses it
  publishes (it will be applied twice — the same rule *Amendment 2* states,
  and the rig role is precisely what makes breaking it tempting); report a
  delta, a viewer pose, or an eye-tracked pose as `NAVIGATION_POSE`; or use
  validity as a presence signal and presence as a validity signal.
  **Presence is transport, validity is authority**: a plugged-in provider
  with nothing to say keeps the role and clears validity, while a provider
  that reports `ABSENT` because it momentarily lost optical lock hands the
  rig to the fly camera and takes it back a frame later, which is visible.
- **No gesture policy enters the runtime.** Thresholds, momentum, rest
  detection, arm-stretch mapping, which button starts a drag — all of it
  stays in the provider. The runtime owns alignment, continuity, recenter
  and composition, and nothing else. Symmetrically, nothing vendor-specific
  entered the runtime for this: the first consumer and the second plug in
  through the same four things above.
- **Eye-tracked parallax still never reaches `rig(t)`.** The composer answers
  the head *device* pose; eye tracking lands later, at view-pose level. That
  is what keeps *Amendment 2*'s divergence from HMD semantics free rather
  than filtered.
- **Under a workspace controller, recentering is a leased action.** v1 is
  native / standalone: the composer and the role live once per system, in
  the service when there is one. Arbitrating *which client* may recenter the
  shared rig is [ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md)
  D2's "space origin / recenter" lease, and is the follow-up — a background
  app must not be able to move everyone's camera.
- **Alignment state is piecewise-constant, with no history.** A head query
  for a timestamp *before* a recenter, issued after that recenter has
  aligned, composes with the current alignment. Correcting it would mean
  keeping an alignment timeline for the sake of queries nobody makes in
  anger; this is documented instead.

Alternatives rejected along the way, beyond the prototype above:

- **The runtime implements grab-drag itself**, from a grip button plus a
  controller pose. It needs nothing new on the ABI, and it is wrong for the
  same reason the vtable slots were: it makes the runtime the author of a
  gesture policy — where the drag anchors, what happens at arm's length,
  when a hand at rest stops driving — that only the vendor's solver has the
  signals to get right, and it hard-codes one interaction for every future
  input device.
- **A navigation *delta* input** instead of an absolute pose. It removes the
  alignment step, and with it every property that makes handover safe:
  deltas cannot be resampled at a requested timestamp, cannot be dropped
  without accumulating error, and leave the runtime nothing to hold when
  tracking goes invalid.
- **Letting the provider own the head pose directly.** The shortest path,
  and the one that ends this ADR: parallax, the fly-camera fallback and the
  *Amendment 2* composition all live behind that pose.
