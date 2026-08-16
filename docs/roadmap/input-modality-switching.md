# Input-modality switching — infrastructure review

**Status:** review / proposal (2026-08-15). Prompted by a hot-swap black
screen when a Leap Motion was unplugged mid-session in a Chrome WebXR session.

Goal, in David's words: *"review our entire infra to deal with switching input
controllers. Leap is one modality, tomorrow we might want to add Quest
controllers or other input hw. When none is plugged in we revert to qwerty. The
switch needs to be hot — should not crash the app. If we need to define a
hierarchy for controllers like we do for display DP (Leia SR, sim display today
with Leia winning if present) then so be it."*

## 1. What exists today (three separate mechanisms)

Input switching is **not one system** — it is three layers with different
lifetimes, and only the middle one is actually dynamic. Naming them is half the
review.

| Layer | File | Lifetime | Dynamic? |
|---|---|---|---|
| **A. Provider selection** — which provider DLL is active | `target_input_plugin_loader.c` (`input_discover_active`) | Chosen **once**, at first `xrCreateInstance`, by ProbeOrder | **No** — one winner for the whole process |
| **B. Controller role arbitration** — which pair holds `left`/`right` | `target_input_arbiter.c` | Re-resolved every `xrSyncActions`, gated on `get_presence()` | **Yes** — hot, presence-gated (ADR-034 Amdt 1) |
| **C. Hand-tracking roles** — which device serves `XR_EXT_hand_tracking` | `target_builder_input_provider.c` → `static_roles` | Set **once**, at system build | **No** — static by the `xrt_system_devices` contract |

The confusion the user is running into is that **B works and A/C do not**, so
"switching controllers" is only half-implemented:

- Leap unplugged → **B** correctly hands `left`/`right` to qwerty (verified in
  the 22:53 service log: generations 1→2→3→4 all fired on plug/unplug).
- But **A** picked exactly one provider DLL at load and will never reconsider,
  and **C** still points the hand-tracking roles at the (now-absent) Ultraleap
  devices.

### The DP analogy the user cites is actually *stronger* than input today

Display processors: all registered DPs are considered and **Leia wins if
present, else sim** — a presence-ranked choice. Input does **not** do this. It
picks the first provider that `probe()`s at load and stops. Register a
hypothetical Quest provider at ProbeOrder 40 next to Ultraleap at 50 and:

- Quest is chosen at load because it probed first.
- Unplug Quest, Ultraleap still attached → roles fall to **qwerty**, *not* to
  Ultraleap, because Ultraleap's DLL was never activated.

So "a hierarchy like the DP" is not a nice-to-have; it is the missing piece that
makes multi-modality work at all.

## 2. The black-screen bug (concrete, reproduced)

Timeline from `DisplayXR_displayxr-service.exe.20600_2026-08-15_22-53-31.log`
and the matching Chrome AppContainer log:

```
22:55:11.9  replug   → presence PRESENT → gen 3 provider   (frames flowing, "took over ok")
22:55:15.8  unplug   → presence ABSENT  → gen 4 qwerty
22:55:16.0  gen 4 committed
22:55:16.0  ── Chrome's LAST OpenXR call (xrGetCurrentInteractionProfile) ──
22:55:16 … 22:55:22   NO layer_commit, NO IPC SR, NO weave   ← black screen (6 s)
22:55:22.7  user presses ESC to escape it → teardown
```

The frame loop stopped the instant the controller role flipped
provider→qwerty **for the second time**, and Chrome stopped making *every*
OpenXR call (rendering included), not just input calls. The session is the
plain legacy WebXR→OpenXR route — the bridge sideband is not involved.

**Isolated 2026-08-15 — the runtime is exonerated.** Method: real
plug/unplug is scriptable via `Disable-PnpDevice`/`Enable-PnpDevice` on
`USB\VID_2936&PID_1206` (a tracking-service stop/start is NOT equivalent —
LeapC masks it; only USB-level removal fires `DeviceLost`). Findings, three
independent runs:

- **Native app** (`cube_handle_d3d11_win --actions --hands`,
  `XRT_FORCE_MODE=ipc`): four role flips (gen 2→5), presence and roles
  correct, **unbroken 60 fps** through every flip. Runtime hot-swap is clean.
- **Chrome WebXR**: freezes on the **third flip** (second provider→qwerty),
  three-for-three across runs, even after the runtime was fixed to emit exactly
  **one** `XrEventDataInteractionProfileChanged` per change-set (it previously
  pushed one per subaction path — a burst of identical events; fixed on this
  branch as spec hygiene, but it did not cure the freeze).
- **Stack dump of the frozen Chrome XR process** (cdb, 49 threads): **zero
  frames in DisplayXR code**; Chrome's `OpenXrRenderLoop` thread idle in its
  own `WaitForSingleObjectEx`. Chrome simply stopped scheduling frames — it is
  not stuck in any `xr*` call.

**Root cause CONFIRMED from the page console (2026-08-16):**

```
Uncaught TypeError: Cannot read properties of undefined (reading 'quaternion')
    at XRControllerModelFactory.js:117
    at XRControllerModel.updateMatrixWorld (XRControllerModelFactory.js:98)
    at Scene.updateMatrixWorld (three.core.js:14388)
```

three.js's `XRControllerModelFactory` re-adds a **cached** motion-controller
model whose visual-response scene nodes were disposed on the earlier removal;
the throw inside `renderer.render()` kills the page's animation loop — hence
zero further OpenXR calls and the black screen. That is why it is always the
*third* flip: add(provider) → swap(qwerty, fresh model) → swap(provider,
**stale cached model**) → throw. A three.js bug (worth filing upstream), not
Chrome C++ and not the runtime; any page using `XRControllerModelFactory` —
i.e. most three.js WebXR content — is exposed to it on ANY runtime that hot
swaps interaction profiles.

Interaction-profile transitions per `xrGetCurrentInteractionProfile` in that
run: provider = `khr/simple_controller`, qwerty = `microsoft/motion_controller`.
A genuine profile change, correctly signalled — so an app that ignores
`XrEventDataInteractionProfileChanged` is within spec but will be surprised, and
Chrome does not ignore it.

## 3. Proposed target design

Keep the three layers, but make **A** presence-ranked like the DP loader and
make **C** track **B**. One rule end to end: *the highest-priority modality
whose hardware is present holds each role; qwerty is the floor.*

### A. Multi-provider, presence-ranked activation (the "hierarchy") — **SHIPPED 2026-08-15**

Implemented as designed below (ADR-034 Amendment 3) and hardware-validated:
`ultraleap(50) → sim-input(200) → qwerty` fell through the ranks on a real
USB unplug and climbed back on replug, frames unbroken; the two-candidate
config (no sim-input) still lands on qwerty.

- Load **every** registered provider that ABI-passes, not just the first to
  probe. (Today the loader stops at the first success — change it to collect
  all successes, keeping the DLLs resident.)
- Priority = ProbeOrder ascending (lower wins), exactly the DP convention, so
  "Leia-beats-sim" has a one-for-one input analogue: e.g. `quest=40`,
  `ultraleap=50`, `net_input=90`.
- The arbiter (**B**) generalises from a provider-vs-qwerty **pair** to an
  ordered **list** of candidate pairs + qwerty as the floor. On each resolve it
  walks the list in priority order and takes the first whose `get_presence()`
  is `PRESENT`; if none, qwerty. This is a small change to
  `refresh_roles_locked()` — swap the single `provider_holds` boolean for a
  priority walk.
- Everything downstream (generation bump, `XrEventDataInteractionProfileChanged`,
  IPC `get_roles` forwarding) already works and is untouched.

### C. Arbitrate hand-tracking roles alongside controllers — **SHIPPED 2026-08-16**

Implemented as option (1) below: `xrt_system_roles` gained dynamic
hand-tracking indices (same generation counter, rides the existing IPC
`get_roles` forwarding for free), the arbiter walks the same presence-ranked
candidates per HT slot, and the OpenXR hand tracker re-resolves its
data-source devices from the session's cached roles whenever the generation
moves. Static `static_roles.hand_tracking` remains the build-time seed and
still gates extension support. Hardware-validated: with
`ultraleap → sim-input → qwerty` registered, unplugging the Leap switched the
live `XR_EXT_hand_tracking` joints to sim-input's synthetic hands mid-session
(eyeballed on the win box) and back on replug.

`static_roles.hand_tracking` is static by the current `xrt_system_devices`
contract, which is why it cannot move today. Two options:

1. **Follow B** — when the winning modality changes, also repoint the
   hand-tracking roles (needs the roles to become dynamic, or a targeted
   "hand-tracking generation" mirror). Correct for a world where controllers
   and hand tracking come from different providers.
2. **Leave static, degrade safely** — an absent tracker already reports
   `is_active=false`; qwerty has no hand tracking, so there is nothing to fall
   back to. Acceptable *only* while every hand-tracking source is also a
   controller source (Ultraleap is both).

Recommend (1) for the target design, (2) as the honest interim. ((2) was
cleared as a freeze suspect — the native app validates flips with HT roles
static.)

### Mitigation candidate born from the §2 investigation — uniform profile

Real-world WebXR content demonstrably breaks on repeated interaction-profile
changes. The hierarchy design should weigh presenting a **uniform interaction
profile across all hand-role candidates** — qwerty emulating the same profile
as the active provider class — so a hot-swap becomes a pose-source change
only: no profile change, no `inputsourceschange`, nothing for fragile apps to
mishandle. Cost: a future provider with a rich profile (Quest →
`oculus/touch`) would be flattened to the common profile while qwerty remains
its fallback, losing bindings. A middle path: suppress the event only when the
old and new profile are identical (already implied by the change-only event
fix), and make the *uniform profile* an opt-in per provider pairing.

### Hot-swap must not stall the client (the load-bearing requirement)

Whatever the arbitration, the transition has to be a **role/profile change the
app survives without a render hitch**. Acceptance criteria:

- A native handle app renders continuously across N plug/unplug cycles — no
  frame-loop gap in the service log, no black frame.
- A Chrome WebXR session survives the same. If Chrome's JS loop is the fragile
  part, the runtime should still emit the transition cleanly and the sample
  (`webxr-bridge/sample`) should be hardened to re-enumerate input sources
  without dropping its `requestAnimationFrame` loop.

## 4. Recommendation / open decision

The generalisation of A+B is mechanical and low-risk and I recommend doing it.
The open call is **C** (arbitrate hand-tracking roles now, or defer) and the
**priority source** (reuse ProbeOrder, or a dedicated `Priority` key). Both are
in the question posed to David alongside this review.

The §2 freeze is the immediate correctness bug and is gated on a native-app
reproduction before any fix.

## 5. Files that would change

- `target_input_plugin_loader.c` — collect all ABI-passing providers, expose an
  ordered list instead of a single `get_active`.
- `target_input_arbiter.c` — priority walk over N candidate pairs + qwerty floor.
- `target_builder_input_provider.c` — register every provider's devices; note the
  full candidate list to the arbiter.
- `xrt_system_devices` / `oxr` — only if C-option-1 (dynamic hand-tracking roles)
  is chosen.
- ADR-034 — a third amendment recording the multi-provider hierarchy.

## Related

- ADR-034 (+ Amendments 1 & 2) · `docs/specs/runtime/input-provider-discovery.md`
- #823 provider channel · #825 hand tracking Tier-2 · #941 idle watchdog
- Preceding fixes this session: rig-relative composition; unplug-while-idle
  presence (both on `fix/rig-relative-input-poses`).
