# Input-Provider Discovery Contract

**Audience:** Integrators shipping an input-provider plug-in (a DLL/dylib/so
exposing tracked motion controllers or other input `xrt_device`s to the
runtime), and runtime engineers maintaining the discovery path.

**Status:** v1 — §§2–4 and 6 are **implemented** (Phase 1, #823:
`xrt/xrt_input_plugin.h`, `target_input_plugin_loader.c`, the builder
arbitration, `sim_input`, and the CLI/self-test diagnostics). §5 —
`net_input` and its wire protocol — has since **shipped**
(`src/xrt/drivers/net_input/`, `DisplayXR-NetInput`). §4b — the rig
(navigation) role, the rig composer, rig-local anchoring and the
persistent host iface — is **implemented** (#1380, ADR-034 *Amendment 4*).
Still planned: the `PreferredPlugin` override (§3, noted inline), and the
per-client recenter lease under a workspace controller (§4b, ADR-035 D2).
The authoritative
rationale is `docs/adr/ADR-034-input-provider-plugins.md`. This spec
intentionally mirrors the display-processor contract
(`docs/specs/runtime/plugin-discovery.md`); where a rule is not restated
here, the DP rule applies unchanged.

**Host process.** Input providers are `dlopen`ed **in-process** — inside
`displayxr-service.exe` in service mode, and inside the app in every
in-process mode — so a provider shares the fate of whatever loaded it: a
provider that hangs or calls `exit()` takes the service and every connected
client down with it (#943). Which process *should* host them is
[ADR-035](../../adr/ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md)
decision D4.

Terminology: "motion controller" = tracked hand-held input device. Not the
workspace controller (shell) of ADR-014.

---

## 1. What an input provider is

An input provider is a dynamically-loaded module that creates one or more
`xrt_device`s carrying input state:

- **Left / right motion controllers** (first-class in v1): 6DOF pose,
  buttons/axes per a claimed interaction profile, haptic output.
- **One navigation device** (`XRT_DEVICE_TYPE_NAVIGATION`, #1380) if the
  provider drives the virtual rig — the camera the qwerty fly camera drives
  otherwise. See §4b.
- Hand-tracking sources filling the hand-tracking device roles; future
  generic trackers (same ABI, no vtable change).

Providers own their tracking stack entirely — camera access, IMU fusion,
radio/USB transport, threads. The runtime sees only `xrt_device` calls:

| Runtime call | Provider obligation |
|---|---|
| `update_inputs` | Refresh `xrt_input` array (buttons/axes) |
| `get_tracked_pose(name, at_time_ns)` | Return a predicted pose for the requested timestamp. Push timestamped samples into `m_relation_history` and predict on demand (`m_predict`); do not return "latest sample" for future timestamps. |
| `set_output` | Apply haptic events (amplitude/frequency/duration) |
| `destroy` | Tear down threads/transport |

## 2. C ABI

Header: `xrt/xrt_input_plugin.h`. Single exported symbol:

```c
xrt_result_t
xrtInputPluginNegotiate(uint32_t runtime_api_version,
                        const struct xrt_input_plugin_host_iface *host,
                        struct xrt_input_plugin_iface **out_iface,
                        uint32_t *out_plugin_api_version);
```

- ABI constant: `XRT_INPUT_PLUGIN_API_VERSION_CURRENT` = **1**. Mismatch →
  the loader REJECTS the entry before any vtable dispatch (ADR-020 rule
  3), same as the DP loader. Every struct carries `struct_size`; the
  vtable is append-only forever.
- `xrt_input_plugin_iface` v1 vtable (identity strings `id` /
  `display_name` / `vendor` / `version` precede the callbacks, as in the
  DP iface):
  - `probe(out_inst)` — cheap "is my hardware/transport present" check;
    on success writes a provider-defined instance handle (NULL is legal)
    that the loader passes back to every later call.
    `XRT_ERROR_PROBER_NOT_SUPPORTED` = clean decline.
  - `create_devices(inst, struct xrt_device **out_devices, uint32_t
    max_count, uint32_t *out_count)` — create all devices this provider
    supplies (bounded by `XRT_INPUT_PLUGIN_MAX_DEVICES` = 8).
    Each device self-describes: `device_type`
    (`XRT_DEVICE_TYPE_{LEFT,RIGHT,ANY}_HAND_CONTROLLER`, …) plus the
    interaction profile it binds (an existing profile from
    `bindings.json` — e.g. `…/khr/simple_controller` — reusing the same
    mechanism as `qwerty_device.c` / `sim_input_device.c`).
  - `destroy(inst)`.
- The DLL handle is intentionally leaked (same lifetime rule and rationale
  as the DP loader: one process, one provider, process lifetime).

## 3. Discovery

### Windows — registry

**Root:** `HKLM\Software\DisplayXR\InputProviders` (64-bit view).
Per-provider subkey, same value schema as DisplayProcessors: `Binary`
(absolute DLL path, required), `DisplayName` / `Vendor` / `Version`
(optional strings), `ProbeOrder` (DWORD; vendors 50, in-tree fallback
providers 200, missing = 100), optional `Enabled` (DWORD, 0 disables —
implemented for input providers; the DP root has no such value).
A `PreferredPlugin` override mirroring DP spec §2.1 is *planned, not yet
implemented* — v1 selection is pure ProbeOrder.

`scripts\register_dev_plugin.bat input [dll]` registers the freshly-built
sim-input (or a vendor DLL) from an elevated prompt;
`unregister-input` removes it.

### POSIX — JSON manifests

Manifest name pattern: `NNN-<name>-input-provider.json` in the same
directories the DP loader scans (`XRT_PLUGIN_SEARCH_PATH` for dev trees,
the installed plugin dir otherwise; the roots are shared via
`target_plugin_build_discovery_roots`). Schema is identical to the DP
manifest (`file_format_version` "1.0" + a `plugin` object with `id`,
`binary_path`, optional `display_name` / `vendor` / `version` /
`probe_order`). The `-input-provider.json` suffix is the router: the DP
loader skips such manifests, the input loader requires them, so both
plug-in types co-habit one directory. Per-user manifests shadow system
ones by `id`, as in the DP loader.

### Selection

Entries sorted by ProbeOrder ascending; **every provider whose `probe()`
succeeds stays resident** for the process lifetime (ADR-034 *Amendment 3* —
this replaced the v1 first-wins rule). ProbeOrder is the provider's
arbitration priority, exactly the DP loader's convention: lower wins. The
ranked list is exposed via `target_input_plugin_get_count()/get_iface(i)`;
`get_active()` aliases index 0. No registered provider is not an error: the
builder falls back to qwerty.

## 4. Role arbitration (builder contract)

In `target_builder_sim_display.c`:

Arbitration is **presence-gated, dynamic and ranked** (ADR-034
*Amendments 1 + 3*). Three role families run through the same walk: the
two hand roles below, the dynamic hand-tracking slots (§4.5), and the rig
(navigation) role (§4b). The devices are static — every claiming provider's
pair plus qwerty's is created once and all live in `xsysd->xdevs` — but
which pair holds each hand role is re-resolved for the life of the
process: per hand, the highest-priority candidate that supplies that hand
and reports `PRESENT` wins; qwerty (always present, priority
`UINT32_MAX`) is the floor. Roles therefore move provider→provider on
plug/unplug, not just provider↔qwerty.

**Profile stability (masquerade).** When the qwerty floor wins a hand on a
box that HAS ranked providers, the arbiter reports the top provider's
interaction profile instead of qwerty's own — qwerty carries
binding-profile remaps for the common controller profiles
(simple/touch/index/vive), so bindings still resolve. Combined with the
change-only event rule in oxr, a provider↔qwerty hot-swap then emits **no
`XrEventDataInteractionProfileChanged` at all**: apps see the pose source
move and nothing else. This exists because real-world WebXR content
(three.js `XRControllerModelFactory`) crashes its own render loop on
repeated profile churn — see `docs/roadmap/input-modality-switching.md`
§2. Provider-less boxes are bit-identical to before (full WMR qwerty);
a provider whose profile qwerty cannot emulate is reported honestly.

1. Input-provider loader runs **before** `t_builder_add_qwerty_input()`.
2. If the active provider supplied a left/right pair **and reports
   `XRT_INPUT_PROVIDER_PRESENCE_PRESENT`** (or predates the
   `get_presence` slot, which means "assume present"), those devices hold
   `xrt_system_roles.left/right` and their `*_profile` fields. Otherwise
   qwerty holds them. Qwerty always registers as a device, so the roles
   can come back to it.
2a. `t_input_arbiter_install()` then replaces
   `xrt_system_devices::get_roles` with the arbiter's, which re-reads
   presence (rate-limited to ~250 ms) and bumps `generation_id` whenever
   the assignment changes. The OpenXR state tracker picks that up in
   `xrSyncActions` — rebinding actions and queueing
   `XrEventDataInteractionProfileChanged` — and the IPC layer forwards
   `get_roles` to the service, so this works for out-of-process clients
   too. Net effect: arbitration is re-evaluated **per app start and
   mid-session**, including a Leap unplugged (or plugged in) while apps
   are running.
3. Override: `HKLM\Software\DisplayXR\Input\ForceQwerty = 1` (DWORD) on
   Windows; on POSIX a `force_qwerty` file in the per-user manifest dir
   (next to the DP loader's `preferred` file — first byte `'0'` = off,
   anything else = on). Forces the fallback, registry/config-gated by
   convention (not an env var). Providers are then not loaded at all,
   so behavior is bit-identical to a box with none registered.
4. **Input providers never supply the head**, and the DP's
   `set_pose_source` hook is still the single binding point. What that
   hook receives is now the runtime's own **rig composer** rather than the
   qwerty HMD directly (#1380): while the rig role sits on the qwerty
   floor the composer is a pass-through and the head pose is unchanged;
   when a provider's `NAVIGATION` device holds the role the composer owns
   alignment, continuity, recenter and composition. See §4b. The display
   plug-in contract and `sim_display` are untouched by this.
5. **Hand-tracking roles (#825 Tier 2):** the same pass fills
   `static_roles.hand_tracking.{unobstructed,conforming}.{left,right}` from
   provider devices that set `supported.hand_tracking` and carry the
   matching `XRT_INPUT_HT_*` input (unobstructed = optical, conforming =
   controller-derived; first claimant wins per role). These roles gate the
   whole `XR_EXT_hand_tracking` path — system support, tracker creation,
   joint locates. Providers without hand-tracking inputs leave them empty;
   that is a valid configuration, never a failure. These *static* roles
   are build-time gating only — they decide whether `XR_EXT_hand_tracking`
   is supported at all, and by the `xrt_system_devices` contract they never
   change. The **source** a tracker actually reads from *is* arbitrated:
   ADR-034 Amendment 3's addendum added dynamic
   `hand_tracking.{unobstructed,conforming}.{left,right}` roles to
   `xrt_system_roles`, filled by the arbiter's presence walk under the same
   `generation_id` as the other roles (`target_input_arbiter.c`), so the
   hand-tracking source follows provider presence like every other role. An
   absent optical tracker reports inactive joints, and qwerty has no hand
   tracking to fall back to anyway.
6. **`sim_input` is opt-in at run time.** Being registered is not enough
   — its `probe()` declines unless `DXR_SIM_INPUT` is set in the
   environment of the process loading the runtime. It synthesises a fixed
   motion pattern, so an un-gated registration silently displaces the
   qwerty fallback with phantom controllers. (Env var rather than the
   usual registry gate on purpose: a per-run developer switch, not
   machine configuration. Set it process-level — the runtime DLL has its
   own static-CRT environment block.)

## 4a. Pose anchoring — provider volumes are rig-relative

A provider reports poses **in its own tracking volume** and says nothing about
where that volume sits in the world. On a 3D display the sensor is on the same
desk as the panel, so the runtime anchors the volume to the **rig** — the
viewer + display + sensor assembly — not to the world (ADR-034 *Amendment 2*).

The resulting rule, which **diverges from HMD VR semantics and is not
optional**:

| Motion | Do provider poses follow? |
|---|---|
| Voluntary rig motion — WASD, mouse-look, a navigating provider (§4b) (translation **and** rotation) | **Yes** |
| Eye-tracked head parallax — viewer leaning/tilting about the panel | **No** |

Mechanically, `t_builder_add_input_provider_devices()` lists every device it
adds in `u_builder_roles_helper::rig_relative`; the builder helper then calls
`u_space_overseer_set_rig_source()` (the head *device* pose — the fly camera;
eye tracking lands later at view-pose level and never reaches it) and
`u_space_overseer_set_device_rig_relative()` per device. The space overseer
re-parents each device's tracking-origin space onto a `U_SPACE_TYPE_RIG` node
that resolves to `rig_now ∘ inverse(rig_initial)` — how far the rig has
**travelled**, captured once at build time.

Provider-facing consequences:

- **Do not compose a camera or navigation transform in the provider.** It will
  be applied twice. Report the sensor's honest reading plus your mount offset.
  This holds *especially* for a provider that also drives the rig (§4b): the
  rig role is what makes pre-compensating look attractive, and the runtime is
  about to add exactly the transform you would be subtracting.
- Your mount offset stays in **stage** space (e.g. a desk-mounted Leap at
  y≈1.45). Do not express it relative to the head. A provider that navigates
  can skip the mount offset entirely by publishing `XRT_TRACKING_TYPE_RIG_LOCAL`
  origins instead — see §4b.
- Devices that already follow the camera themselves are excluded — qwerty's
  controllers parent to the qwerty HMD (`follow_hmd`) and are never marked.
- Applies uniformly to grip/aim action spaces, `xrLocateSpace`, and the
  hand-joint base (`xrt_space_overseer::locate_device`), in-process and over
  IPC. Covered by `tests/tests_space_overseer_rig.cpp`.

## 4b. Rig (navigation) role

**Normative.** Implemented in `target_input_arbiter.c` (the role),
`target_rig_composer.c` (the composition), `u_builders.c` (rig-local
anchoring) and `target_input_plugin_loader.c` (the host iface). Rationale:
ADR-034 *Amendment 4*; issue #1380.

A provider may also drive the **rig** — the virtual camera the qwerty fly
camera drives today — by creating ONE extra device of type
`XRT_DEVICE_TYPE_NAVIGATION` alongside its controllers.

### 4b.1 The walk, and the floor

The role is arbitrated by the same presence-ranked walk as the hands, under the
same `generation_id` and over the same IPC `get_roles` forwarding:

> `xrt_system_roles::rig` is the `xsysd->xdevs` index of the navigation device
> of the **highest-priority (lowest ProbeOrder) candidate that has one and
> reports `PRESENT`**. Otherwise **-1**.

Two things differ from the hand walk:

- **At most one navigation device per provider.** The arbiter scans the
  devices a provider returned from `create_devices` in order and takes the
  first `XRT_DEVICE_TYPE_NAVIGATION` one. A second is a provider bug: the
  arbiter logs `input arbiter: '<id>' supplied more than one navigation
  device — keeping '<name>'` and ignores the rest. (Picking a different one
  per build would be worse than picking the first.)
- **The floor is an absence, not a candidate.** Qwerty supplies no navigation
  device, so `rig == -1` is not "no rig" — it means *the runtime's own fly
  camera holds it* (WASD / mouse-look). That is what every box without a
  navigating provider reports, and what an unplug falls back to.

A provider may supply a navigation device and **no** controllers; it is still a
candidate and can still win the rig. Conversely a provider with controllers and
no navigation device is skipped by this walk entirely and can never hold the
rig, whatever its ProbeOrder (`tests/tests_input_arbiter_rig.cpp`).

The arbiter normally declines to install itself when there is nothing to
arbitrate (fewer than two candidates). A navigation device is the exception:
the builder has no static seed for `roles.rig`, so a **lone** navigating
provider still forces the arbiter in. Without that, `roles.rig` would stay at
its `XRT_SYSTEM_ROLES_INIT` default of -1 and the provider would never be
reached.

### 4b.2 Presence is not validity

Presence semantics are the hands' exactly: a NULL `get_presence` (or a
`struct_size` predating the slot) means "assume present", `UNKNOWN` means not
present, and verdicts are cached ~250 ms.

Pose validity is a **different axis**, and conflating the two is the mistake
this section exists to prevent:

| | Answers | Owned by | Effect |
|---|---|---|---|
| **Presence** (`get_presence`) | "is my hardware/transport attached?" | the provider's transport thread | decides **who holds the role** |
| **Validity** (`relation_flags` on `NAVIGATION_POSE`) | "do I have navigation authority right now?" | the provider's solver | decides **whether the rig advances** |

A provider that is plugged in but has nothing to say (optical loss, a rest
state, no controller being held) clears the pose's `POSITION_VALID` /
`ORIENTATION_VALID` bits and **keeps** the role; the rig holds its last pose
and resumes from there. Flipping presence for the same condition hands the rig
to the fly camera and takes it back a frame later, which is visible as a jump
and is a provider bug.

### 4b.3 What the provider publishes

`get_tracked_pose(XRT_INPUT_GENERIC_NAVIGATION_POSE, t)` returns **N(t): the
absolute pose of the rig in the provider's own navigation frame F.**

- F is right-handed, +Y up, −Z forward, metres, gravity-aligned. Its **origin
  is private to the provider** and is never interpreted by the runtime —
  identity means "rig at F's origin", nothing more.
- N is **not** a delta, **not** a viewer or eye pose, and **un-parallaxed**.
  Eye tracking is applied later, at view-pose level, and must never enter N.
- `relation_flags`: set `POSITION_VALID | ORIENTATION_VALID` when the provider
  has navigation authority; clear them to say "hold the rig". The `*_TRACKED`
  bits are **ignored** for this input.
- The runtime guarantees nothing across separate `get_tracked_pose` calls. A
  provider serves navigation, grip/aim and joints for a requested timestamp
  from **one latched publication**, so that a head poll and an action sync in
  the same frame see a coherent set.

`XRT_INPUT_GENERIC_NAVIGATION_RECENTER` (boolean, optional) is a **durable
level with a publication timestamp**, not a pulse:

- `value.boolean = true`, `timestamp` = the monotonic time of the most recent
  reset. **Never cleared.** The composer consumes a recenter when
  `timestamp > last_consumed`.
- Publish the new timestamp **and** the post-reset N in the *same* publication
  (one atomic swap of the latched frame). No generation gating and no
  invalid-until-delivered hold are needed.
- Nothing else is signalled through this input. Two resets between polls
  collapse to one — a recenter is idempotent, and alignment uses the N
  delivered with the latest one.

The level-with-timestamp shape is **required, not stylistic**: `xrSyncActions`
sweeps `update_inputs` over every device (`oxr_input.c`), and so do the IPC
server's own device passes. A one-update pulse can therefore be consumed by
action sync before the composer ever polls the device, and the recenter would
be lost — intermittently, and preferentially on the IPC path. A durable level
is immune to how many `update_inputs` calls land in between.

### 4b.4 A navigating provider's other devices are rig-local

A provider that drives the rig **must** publish its controllers and hand joints
with `tracking_origin->type = XRT_TRACKING_TYPE_RIG_LOCAL`: poses relative to
the **display plane** — origin at the display centre, +X right, +Y up, +Z
toward the viewer, metres.

The builder (`u_builders.c::anchor_rig_local_origins`) sets such an origin's
`initial_offset` to `rig_initial`, the rig pose at system build, so the
*Amendment 2* rig delta collapses:

```
world = rig(t) ∘ inv(rig_initial) ∘ rig_initial ∘ L = rig(t) ∘ L
```

Consequences for the provider: **no standing height, no mount offset, no rig
knowledge.** Several devices may share one origin (a left/right pair normally
does) — it is anchored once, and logged once at init as `Rig-local origin
'<name>' anchored at the initial rig (x, y, z)`.

`XRT_TRACKING_TYPE_OTHER` / `_NONE` origins are untouched and keep today's
stage-anchored behaviour, mount offsets included, so no existing provider
changes. Anchoring happens after the head's own tracking origin is settled and
before the space overseer freezes each origin offset into a space; a head with
no valid pose anchors at identity.

**The recipe for a solver that already outputs world poses.** If your tracking
stack produces product poses `C_W` in its own world `W` that *already* contains
your navigation `P(t)` — which is the normal shape for a camera-drag solver —
do not try to undo it. Publish

```
N(t) = P(t)                 as NAVIGATION_POSE
L    = inv(P(t)) ∘ C_W      as the controller/joint pose, RIG_LOCAL
```

Then `world = rig(t) ∘ L = T_world_F ∘ P(t) ∘ inv(P(t)) ∘ C_W = T_world_F ∘ C_W`:
the runtime's single alignment maps your entire product world, with no
provider-side reference frame `P0` and no runtime rig knowledge on your side.

### 4b.5 What the runtime does with the role — the composer

The runtime binds a runtime-owned `xrt_device`, the **rig composer**
(`target_rig_composer.c`), as the head's pose source through the display
plug-in's existing `set_pose_source` hook. Its `XRT_INPUT_GENERIC_HEAD_POSE`
**is** `rig(t)`. The composer is not in `xsysd->xdevs`, holds no role, and is
created and destroyed by the sim-display builder.

Per poll of the head pose, in this order:

1. **Roles.** Read `xrt_system_devices_get_roles`. A change of the **holder
   index** — not every `generation_id` bump — is a handover. `rig_last` is
   left untouched: the rig continues from where it *is*, never from the new
   holder's frame. Taking the role drops the alignment (it is recomputed
   below, on the first valid N) and clears any pending recenter; handing back
   to the floor re-seeds the fly camera at `rig_last` so WASD continues from
   there. One WARN per handover, never per frame.
2. **Floor.** With `rig == -1` the composer returns the fly camera's relation
   verbatim and records it as `rig_last`. **WASD / mouse-look act only here** —
   while a provider holds the role the fly camera is not read at all.
3. **Provider.** `update_inputs` on the navigation device, then the durable
   RECENTER read, then `get_tracked_pose(NAVIGATION_POSE, at_timestamp_ns)`.
   `valid` = both VALID bits; TRACKED bits ignored.
4. **Epochs.** Recenter wins when two are due in one poll:
   - *pending recenter with timestamp r*: on the first sample that is valid
     **and** whose requested time is `>= r`, set
     `T_world_F = rig_initial ∘ inv(N)`, return `rig_initial` exactly, clear
     the pending flag. Otherwise hold `rig_last` and stay pending.
   - *not aligned* (fresh handover, or validity was lost): on the first valid
     N, `T_world_F = rig_last ∘ inv(N)`, then compose. Otherwise hold.
   - *aligned but N invalid*: drop the alignment (so the next valid sample
     re-aligns at the held rig) and hold.
   - *aligned and valid*: compose.
5. **Compose.** `rig(t) = T_world_F ∘ N(t)`, with `T_world_F` on the **left**,
   so a provider-local step `N → N ∘ T(0,0,-1)` moves the rig along its own
   forward. Emitted with `POSITION|ORIENTATION_VALID|TRACKED` and zero
   velocity.

**Continuity invariant.** At a handover, at an invalid → valid transition and
at any non-recenter re-alignment the first composed pose equals `rig_last`
exactly, because `(rig_last ∘ inv(N)) ∘ N = rig_last`. The single deliberate
jump is a recenter, which lands on `rig_initial`.

**No history.** Alignment state is piecewise-constant: a head query for a
timestamp *before* a recenter, issued after that recenter aligned, composes
with the current alignment. This is documented, not history-corrected.

Covered by `tests/tests_rig_composer.cpp` (alignment with non-identity N,
invalid-hold, the four recenter cases, handback re-seeding, hand-role churn,
concurrent polls) and `tests/tests_input_arbiter_rig.cpp` (the walk).

### 4b.6 Host iface and display geometry

`xrtInputPluginNegotiate` receives a `struct xrt_input_plugin_host_iface *`
that is **process-lifetime runtime storage** — one static, shared by every
provider in the process. A provider may retain it, and the callbacks inside it,
for as long as it lives. (It was a stack local before #1380; a provider that
kept the pointer read dead stack.)

`host->get_display_geometry(&geo)` fills `struct
xrt_input_host_display_geometry` — panel width/height in metres and the
**nominal** viewer position (display-centre origin, +Z toward the viewer). It
is geometry for a solver, never a head pose and never the tracked eyes.

Pointer lifetime and data readiness are separate, and so are their failures:

| Situation | Result | Out struct |
|---|---|---|
| Slot is NULL | — | older runtime; no geometry exists to ask for |
| `inout == NULL`, or `struct_size` too small to hold even `struct_size` | `XRT_ERROR_INPUT_UNSUPPORTED` | untouched |
| Called before the cache is published (e.g. from negotiate) | `XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY` | **untouched** — retry later |
| Cache published | `XRT_SUCCESS` | filled, prefix semantics |

The cache is populated by the system builder from the display plug-in's
`get_display_info`, **after** the head device exists and **before** any
provider's `create_devices` runs. So the data is ready from `create_devices` on
and static for the life of the system. Set `struct_size = sizeof(...)` before
the call; the runtime copies `min(your struct_size, its own)` bytes and leaves
your `struct_size` as you set it. Publication is logged once
(`input plugin loader: display geometry published to providers: …`), so a bug
report shows whether providers ever got geometry at all.

### 4b.7 Testing without hardware

`sim_input` carries a scripted navigation device so the role walk, the
handover and the recenter path are exercised in CI with no hardware — see §5
for the device and the `DXR_SIM_INPUT*` environment gates that drive it.

## 5. In-tree reference providers

| Provider | Status | Purpose |
|---|---|---|
| `sim_input` | **Shipped** (`src/xrt/drivers/sim_input/`, plug-in DLL `DisplayXR-SimInput`) | Deterministic synthetic motion controllers (circular motion, scripted button presses; `khr/simple_controller`) — hardware-free CI gate, adapted from Monado's `simulated_controller.c`. Also serves scripted 26-joint hand tracking (#825 Tier 2: curl wave over the same analytic circle, via `u_hand_simulation`) — the hardware-free test vehicle for `XR_EXT_hand_tracking`. ProbeOrder 200. **Test vehicle only — not a product path:** `probe()` declines unless `DXR_SIM_INPUT` is set (§4.6). Dev builds stage it automatically (`build_macos.sh` / `build_linux.sh`); Windows registers via `register_dev_plugin.bat input sim`. |
| `net_input` | **Shipped** (`src/xrt/drivers/net_input/`, plug-in DLL `DisplayXR-NetInput`) | Loopback-TCP-fed devices — an external tracking process feeds timestamped poses + button state and receives haptic events back (wire protocol below). Opt-in: never registered by default. |
| `ultraleap` | **Shipped, SDK-gated** (`src/xrt/drivers/ultraleap/`, plug-in DLL `DisplayXR-Ultraleap`; builds only where the Ultraleap Gemini SDK / LeapC is found — `LEAPSDK_DIR`) | Hand-as-motion-controller provider (#825 Tier 1, adapted from Monado's removed `ultraleap_v5`): palm pose → grip/aim, pinch → select, grab → menu; 26-joint sets filled for the Tier-2 `XR_EXT_hand_tracking` wiring. On Windows the SDK's `LeapC.dll` is staged **next to the plug-in** (#933) — the loader's `LOAD_WITH_ALTERED_SEARCH_PATH` then resolves it app-locally instead of from the system PATH, where the LeiaSR Platform ships its own shadowing copy. Opt-in: never registered by default. |

**`sim_input` and the rig role (#1380).** A *second*, independent opt-in,
`DXR_SIM_INPUT_NAV=1`, adds one scripted `XRT_DEVICE_TYPE_NAVIGATION` device
("Sim navigation") so the rig role of §4b runs hardware-free. Default **off**,
so every pre-existing `DXR_SIM_INPUT=1` run is byte-for-byte unchanged: no
navigation device, the rig stays on the qwerty floor. With it on, `N(t)` is a
slow figure-eight plus yaw in the provider's own frame F — a pure function of
the requested timestamp, so prediction is exact and CI is reproducible — and
the controllers switch to `XRT_TRACKING_TYPE_RIG_LOCAL`, because a provider
that navigates publishes display-plane-relative poses (§4b). Two optional
fault knobs, both off by default: `DXR_SIM_INPUT_NAV_HOLD_MS=<n>` makes the
pose report invalid for 500 ms every *n* ms (the composer's hold / re-align
path) and `DXR_SIM_INPUT_NAV_RECENTER_MS=<n>` fires a scripted reset every *n*
ms, advancing the durable `NAVIGATION_RECENTER` timestamp **and** jumping the
scripted pose in the same publication. All three are env vars, not registry
values, for the reason `DXR_SIM_INPUT` is (§4.6): per-run developer switches,
not machine configuration — and the process-level caveat applies, set them
before launching the host process.

### 5.1 net_input wire protocol (v1)

Normative definition: `src/xrt/drivers/net_input/net_input_proto.h`
(struct layouts are static-asserted); executable reference:
`scripts/net_input_feeder.py`. Summary:

- **Transport:** TCP on `127.0.0.1` ONLY (the provider binds loopback,
  never a routable interface), default port **9427**, one feeder at a
  time. All fields little-endian.
- **Handshake:** each side sends `{u32 magic = "DXRI" (0x49525844),
  u32 version = 1}` immediately after connect and validates the peer's;
  mismatch → close.
- **STATE** (feeder → provider, 72 bytes): `type=1`, hand (0=L/1=R),
  active, button mask (bit0 select, bit1 menu), battery %, feeder
  monotonic `timestamp_ns` (0 = stamp on receipt), position[3],
  orientation quat[4] (x,y,z,w), linear + angular velocity[3].
- **HAPTIC** (provider → feeder, 24 bytes): `type=2`, hand, amplitude,
  frequency (0 = unspecified), duration_ns — emitted whenever the
  runtime applies haptic feedback to the device.
- **Clock mapping:** non-zero feeder timestamps are translated into the
  provider's monotonic domain with a latency-floor offset estimate
  (running minimum with slow drift decay) and pushed into
  `m_relation_history`; `get_tracked_pose(at_time_ns)` then
  interpolates/predicts at the requested timestamp.
- **Headless round-trip test:** `displayxr-cli input haptic-test
  [seconds]` fires vibrations on the hand-role devices while
  `scripts/net_input_feeder.py --assert-haptic` streams poses in and
  asserts the haptic events arrive back.

## 6. Diagnostics

- `displayxr-cli input list [--json]` — enumerate registered providers
  (no DLL load), the predicted active one, ProbeOrder/Enabled, and the
  ForceQwerty state (mirror of `dp list`).
- `displayxr-cli selftest` — when a provider is registered and not
  ForceQwerty-overridden, asserts provider-claimed left+right
  motion-controller role devices exist with a valid interaction profile
  (`CLI_SELFTEST_BAD_INPUT` on failure), and (#825 Tier 2) that any role
  device advertising `supported.hand_tracking` actually claimed a static
  hand-tracking role. Provider *absence never fails* — qwerty keeping the
  hand roles is the normal no-provider configuration, and a provider
  without hand-tracking inputs passes with the hand-track fields `n/a`.
  Wired into the hardware-free CI gate (`build-windows.yml` registers
  sim-input and additionally asserts the check *evaluated*).
- Verification matrix: in-process AND `XRT_FORCE_MODE=ipc` (the IPC layer
  already carries `device_*` input messages; the service path enumerates
  provider devices identically — verified on the macOS service:
  the IPC client reports the provider's left/right role devices).
