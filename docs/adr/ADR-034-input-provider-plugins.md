# ADR-034: Input Providers Are a Second Plug-in Type, Not a Display-Processor Extension

**Status:** Accepted (Phase 1 implemented — #823; role arbitration amended 2026-08-15, see *Amendment 1*; rig-relative pose composition amended 2026-08-15, see *Amendment 2*)
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
