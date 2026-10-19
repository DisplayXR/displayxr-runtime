---
status: Accepted
date: 2026-10-13
deciders: David Fattal
issues: []
related: [ADR-019, ADR-020, ADR-034, ADR-035, ADR-043, ADR-045, ADR-051, ADR-053]
---

# ADR-054: Component model — a trunk with peer component classes over versioned C ABIs

**Status:** Accepted (2026-10-13, owner decisions after a cross-session review of the three
plug-in classes) · supersedes nothing — this is the umbrella the class records sit under:
[ADR-019](ADR-019-vendor-plugin-aux-boundary.md) / [ADR-020](ADR-020-plugin-abi-compatibility-policy.md)
(display processors), [ADR-034](ADR-034-input-provider-plugins.md) (input providers),
[ADR-053](ADR-053-conversion-providers.md) (conversion providers) · hosting:
[ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md) ·
state: [ADR-045](ADR-045-plugins-always-loadable-and-report-platform-state.md) · surface:
[ADR-051](ADR-051-display-status-model-and-dashboard.md)

## In one paragraph

DisplayXR is a **trunk** — the OpenXR state tracker, one native compositor per graphics API,
the service and its IPC, orchestration, every policy, the CLI and dashboard, and the loaders —
with **peer component classes** plugged into it over **versioned C ABIs**. Three classes exist
today and were built one at a time, each by analogy with the last. This record writes down
what they have in common, so the fourth is built to the pattern rather than by analogy; it
sets the rule for admitting a class; and it decides the seven candidates the review raised.
One is admitted (viewer tracking), one is refused for good (display profiles), one is shaped
as a pose consumer rather than a class (spatial audio), one folds into an existing contract
(media decoders), one is deferred (cameras), and two things stay in the trunk (policy, the
qwerty floor).

## Context

What is on `main` (2026-10-13):

| Class | Negotiate symbol | ABI | Windows root (`HKLM\Software\DisplayXR\…`) | POSIX manifest / Android library | Reference impl. | Record |
|---|---|---|---|---|---|---|
| Display processor | `xrtPluginNegotiate` | 5 | `DisplayProcessors` | `*.json` / `libdxrp<NNN>_<id>.so` | `DisplayXR-SimDisplay` (`sim_display`) | ADR-019/020 |
| Input provider | `xrtInputPluginNegotiate` | 1 | `InputProviders` | `*-input-provider.json` / (not yet) | `DisplayXR-SimInput` (`sim_input`) + in-tree qwerty floor | ADR-034 |
| Conversion provider | `xrtConversionPluginNegotiate` | 1 | `ConversionProviders` | `*-conversion-provider.json` / `libdxrc<NNN>_<id>.so` | `DisplayXR-SimConversion` (`sim_conversion`) | ADR-053 (#1994, #2015) |

Headers: `xrt_plugin.h`, `xrt_input_plugin.h`, `xrt_conversion_plugin.h`
(`src/xrt/include/xrt/`). Loaders: `target_plugin_loader.c` (4,645 lines),
`target_input_plugin_loader.c` (1,153), `target_conversion_plugin_loader.c` (1,257) under
`src/xrt/targets/common/` — three separate copies of one discovery/negotiation/rejection
flow, which is how the input and conversion loaders came to grow their own manifest-suffix
and registry code and how the DP loader alone lacks an `Enabled` value.

Two things that look like classes are not. **Workspace controllers** (the shell and any
vertical) are a *process* contract over IPC — registration under `WorkspaceControllers`,
spawned on demand by the service (`docs/specs/runtime/workspace-controller-registration.md`,
ADR-016). **Per-app MCP tools** (`XR_DXR_mcp_tools`) are an app-facing extension, not a
runtime component. Neither crosses a DLL boundary into the runtime and neither is governed
here.

The review that produced this record found the classes drifting apart on exactly the things
a user or a vendor touches: the verbs differ per class (`dp list|use|reset` exists;
`input list` only; `conv list|status|use|reset`), only conversion has a licensing state
(`XRT_CONV_STATE_UNLICENSED`), only input and conversion honour `Enabled`, and each loader
reinvents the manifest and ABI-rejection path. It also surfaced a queue of candidates — eye
tracking, display profiles, audio, decoders, cameras — each argued on its own merits with no
shared admission rule. That is the gap this ADR closes.

## Decision

### 1. The trunk

The trunk is everything a component is *not*: the OpenXR state tracker; the native compositors
(D3D11, D3D12, Metal, OpenGL, Vulkan) and the service's multi-compositor pipeline; IPC; the
orchestrator and supervisor; **all policy** (mode selection, convergence, depth budget, cursor
relief, view-rig composition, arbitration); the loaders; capture and backdrops; and the user
surface (`displayxr-cli` + the dashboard). The trunk owns selection among components and owns
the truth about their state. Components own exactly one capability each and are replaceable
without touching the trunk.

### 2. What makes a class — all seven must hold

A component class is a contract the trunk honours for *any* implementer. It exists only when
every one of these holds:

1. **Own ABI.** Its own negotiate symbol and ABI major; `struct_size` on every struct that
   crosses the boundary; append-only vtables; the loader rejects a major mismatch and **falls
   through** to the next candidate (ADR-020). The ABI is the class; a class without one is a
   trunk feature.
2. **Own discovery root.** A registry key on Windows, a manifest suffix in the POSIX plug-in
   roots, a library-name prefix inside the Android APK (ADR-038). Roots are ABI-like: once
   shipped they are not renamed.
3. **A shipped reference implementation.** `sim_*` for every class, so the system runs with no
   hardware, no vendor and no model, and so CI can exercise the loader end to end.
4. **Trunk selects, component serves.** The runtime owns selection and every policy decision;
   the component owns one capability and reports what it can do. A component never chooses
   itself and never arbitrates with its peers.
5. **Always loadable, always reports state** (ADR-045). A registered component is a fact, not
   a decision; it loads without its platform present and says, in class-generic vocabulary,
   why it cannot serve right now. The service re-evaluates when the world changes.
6. **The same four verbs everywhere.** `list`, `enable|disable`, `prefer` (`use` / `reset`),
   `options` — one spelling per class in `displayxr-cli`; the CLI is the logic and the
   dashboard renders what the CLI (or the service's DIAG snapshot) returns (ADR-051). A class
   missing a verb is a bug, not a design choice.
7. **One options contract.** Per-component settings live under one `Settings\<class>\<id>`
   key (the `u_setting` machine/user stores) with a `DXR_<CLASS>_<ID>_<KEY>` environment
   override that always wins, and the component reads them **through the host interface**,
   never from the environment or from ad-hoc vendor keys (ADR-053 §5 already requires this of
   conversion providers; it becomes the rule for all).
8. **Selection scope is per class; apps express preferences, never policy** (2026-10-17). The
   trunk keeps every installed, enabled component of a class resident and arbitrates among them;
   the user sets defaults and pins in the Dashboard / CLI (`prefer`); an app may *enumerate* the
   candidates (ids and the components' own display names, never a vendor it recognises) and
   *request* one for its own use, and the request never outranks a user's pin or a machine lock.
   The scope of a selection follows the class: display processors **per screen** (`dp use --screen`),
   conversion providers **per task and per stream / job** (lift spec 5, #2178), input providers
   **per role family and per client** (ADR-034 Amendment 5). A new class must say what its scope is
   before it ships its `prefer` verb.

### 3. Admission rule

A capability earns its own class — its own ABI, root, loader entry and verbs — when **either**
of these is true:

- its **failure domain must be separable from the service** (it can crash, hang, leak or be
  unlicensed without taking composition down), **or**
- a **second implementer exists** or is contracted — a vendor who is not the display vendor
  needs an entry point.

> **Superseded by Amendment 1 (2026-10-18)** below: the "either / or" test proved too
> permissive. The three-part test and the alternatives ladder there are the admission rule;
> the two clauses above survive as *evidence*, not criteria.

Hot-path-ness does **not** decide admission. It decides **hosting**: a component that must
run on the frame thread loads in-process and is contained (the DP, ADR-035 D4); one that can
be a frame late runs in a supervised satellite reached over shared memory (conversion,
ADR-053 §5; input providers, ADR-035 D4). The class contract is hosting-agnostic by
construction — no graphics device crosses the boundary, every image is a shareable handle —
so a class moves between the two without an ABI change.

### 4. Decisions on the candidates

**4.1 Viewer-tracking providers — YES, a fourth class.** Eye tracking does two jobs that have
been conflated. *Weaving eyes* are internal to the display processor: consumed inside
`process_atlas`, phase-matched to the lens, never seen by an app. *Look-around eyes* are data
to apps: `get_predicted_eye_positions` → `xrLocateViews`. The second job may be served by the
DP (a display with its own camera) **or** by a separate provider — a webcam running
MediaPipe or YuNet, a depth camera, a wearable. Selection is trunk policy, in this order:

1. when the active DP **weaves from tracked eyes**, its eyes are used for look-around too —
   otherwise the pair the app renders for disagrees with the pair the weave consumed, and the
   image swims;
2. else the best enabled tracking provider;
3. else the fly camera.

The provider's output is a **timestamped history of eye-pair relations** (a short ring the
trunk samples at a target time), not a latest value. Prerequisite: the DP eye query must gain
a target-time parameter first (today it is "predicted" with no horizon the caller controls).
Tracking providers are hosted **out of the compositing process**, like conversion. The
webcam tracker merged in PR #1914 (`src/xrt/drivers/sim_display/sim_display_webcam_tracker.c`,
`…_webcam_tracking.c`) moves out of `sim_display` into a `sim_tracking` reference provider once
the contract lands. There is **no `set_viewer_eyes` slot on the DP**: the trunk never pushes
eyes into a display processor; a DP that weaves from eyes tracks them itself.

> **Implemented — N6 Part 1 landed in #2166 (2026-10).** The DP eye query gained the target-time
> slot `get_eye_positions_at(target_ns)` on all five per-API vtables (ADR-020 append-only, ABI
> unchanged, `XRT_DP_<API>_HAS_EYE_POSITIONS_AT`), the trunk keeps the eye-pair ring
> (`util/u_eye_history.h`) every compositor samples at its target, and `xrLocateViews` asks for the
> frame's `displayTime` in-process and over IPC. Contract:
> [eye-tracking-modes.md § Target-time query](../specs/vendor/eye-tracking-modes.md#target-time-query-adr-054-41--n6-part-1-2144).
> Part 2 (the tracking-provider class, `sim_tracking`) is still open (#2144).

**4.2 Display profile slot / reference weaver — NO, ever.** Pitch, slant and phase describe
one lens family. Parallax barriers, directional backlights, glasses-based and volumetric
displays have none of them, and a "reference weaver" in the trunk would be a lenticular
weaver with a generic name. The display processor stays the **opaque unit of display
competence**. What crosses its boundary is dims, modes and view count, tracking capability,
and status (ADR-022, ADR-045, ADR-051) — never optics.

**4.3 Spatial / directional audio — NOT a class; a registered pose consumer.** An audio
engine needs the viewer's eye pair and head centre at frame rate; it does not need to be
loaded by the runtime, and the runtime must not take a dependency on it. The shape is:

- a `HKLM\Software\DisplayXR\PoseConsumers\<id>` row, visible in the dashboard, with an
  `Enabled` flag and per-exe consent;
- a **seqlock shared-memory page** — eye pair, head centre, timestamp, confidence, tracking
  state, screen id — written by the render thread at its `FRAME_TICK` viewer publish;
- a small, session-free C client library the vendor links into its own process, with a
  handshake shaped like ADR-043 Amendment 1's `CAMERA_CONSUMER`: verified executable,
  consent, revocable, in-use indicator, kill switch.

This requires an **always-on tracking mode** — the tracker running with no OpenXR session,
with the camera indicator lit and consent granted — which is a 4.1 concern. Precedent: Dimenco
fed AudioScenic the SR eye pair this way. This record fixes the shape and the admission
reasoning only; the design note is N8.

**4.4 Media-format decoders (LIF, LVF, MV-HEVC, spatial HEIC) — a decode task family inside
the conversion contract**, not a fourth ABI. Conversion already has blob-stream in and
shared-texture out, readiness, licensing and the job lane; a decoder is a task that happens
to be lossless. The browser is the first consumer. The win is capability — formats the
browser cannot decode today — not speed.

**4.5 Camera providers — deferred** until a standalone camera (not a display's own) exists
in the field, as ADR-043 already decided. When it does, ADR-053's hosting contract is the
template, not the DP's.

**4.6 Policy, capture and backdrops — never pluggable.** Convergence, depth budget (ADR-040),
cursor relief (ADR-046), lift viewpoints (ADR-048), atlas capture (ADR-023) and the
`comp_bg2d` backdrops are trunk. A vendor that wants different policy argues for a change in
the trunk; it does not ship one.

**4.7 Qwerty stays in the trunk** as the runtime-wide input floor. The review found the floor
spelled `cand->iface == NULL` in five places in `target_input_arbiter.c`, WebXR
interaction-profile churn, and CTS dependence on it — a DLL move would touch all of that for
no user benefit. Instead: an `EmulatedControllers=0` gate alongside the existing
`ForceQwerty`, surfaced through the CLI and dashboard, so a user can turn the emulated pair
off without a code change. (Landed as N7, #2143, under the class root —
`InputProviders\Policy\EmulatedControllers`, HKCU over HKLM, not next to
`Input\ForceQwerty` — so the policy watch and the `Policy\Lock` overlay rule cover it for
free; `ForceQwerty` stays where it is. Spec: input-provider-discovery.md §4 item 3b.)

### 5. Naming

- **"display processor"** stays (it is in every header, key and log line).
- **"provider"** for input, conversion, tracking and (later) camera classes.
- **"plug-in"** (hyphenated) is the *packaging* word — the DLL/`.so`/`.dylib` a class
  implementer ships; a plug-in may carry several components of several classes (as OBS
  allows).
- CLI, dashboard and live docs are brought to this vocabulary (N3). **Registry keys and
  manifest suffixes are not renamed** — they are ABI.

## Consequences

- The fourth class is built from §2, not from the nearest existing one. Its loader entry is
  a table row in the shared loader (N2), not a fourth copy.
- Every class gets the four verbs and the `Enabled` / `Preferred` pair (N3). The DP loader,
  which today reads only `ProbeOrder`, gains `Enabled`.
- `displayxr-cli selftest` becomes the per-class conformance gate (N4): register the `sim_*`
  reference → it is selected → one round trip through the class's capability → `Enabled=0`
  falls back → `Preferred` wins → an ABI-mismatch fixture is rejected and falls through.
- Load-time provenance (N4b): a component is **signed or explicitly allowlisted**; the dev
  override (`DXR_ALLOW_DEV_PLUGIN_PATHS`, #943) stays loud-logged and is refused on elevated
  hosts.
- Licensing vocabulary (`UNLICENSED`) is extended from conversion to display and input only
  when a paid plug-in of that class is real (N5) — not pre-emptively.
- The viewer-tracking class waits on the eye-contract change (N6); nothing in 4.1 ships
  before the DP eye query carries a target time.
- Trade-off accepted: a shared loader is a large refactor of the most load-bearing file in
  the trunk (4,645 lines, the CI self-test gate runs through it). It is done behind the
  existing `selftest` and the ABI fixture, class by class, DP last.

## Next steps

| # | Item | Owner |
|---|---|---|
| N2 | Shared `target_component_loader`: one discovery/negotiate/reject/fall-through flow with a per-class table (symbol, ABI, root, suffix, prefix); the three loaders become thin | win |
| N3 | Verbs retrofit — `input enable|disable|use|reset`, DP `Enabled`, `conv set`, live re-select on change (#2025 pattern), dashboard controls; vocabulary fix in CLI/dashboard/live docs | mac |
| N4 | Per-class conformance in `displayxr-cli selftest` (sequence above) + the CI vendor-identifier guard widened from the runtime DLL's link line to every class loader | win |
| N4b | Provenance gate at load: signed OR allowlisted; loud-logged dev override; no override on elevated hosts | win |
| N5 | Licensing state for DP and input — when a paid plug-in exists | — |
| N6 | Eye contract prep (target-time query, pair-relation history), then the tracking-provider class + `sim_tracking` | — |
| N7 | `EmulatedControllers` gate + CLI/dashboard surface | — |
| N8 | Pose-feed design note (seqlock page, consumer handshake, always-on tracking mode) | — |
| N10 | Decode task family in the conversion contract; browser as first consumer | — |

## Precedents

- **Kubernetes** — CRI, CNI, CSI and device plugins are peer contracts behind one control
  plane; a contract that was too thin (device plugins) was replaced (DRA) rather than
  stretched. Our lesson: §2 is deliberately not minimal.
- **Vulkan loader** — a negotiated interface version between loader and ICD, with
  fall-through on mismatch; the model for ADR-020 and §2.1.
- **OpenXR loader** — an open conformance suite plus an adopters registry; N4 is our
  conformance, N4b our registry.
- **Android Treble** — frozen vendor interfaces so the platform and the vendor ship on
  separate cadences; the reason registry roots and suffixes are never renamed (§5).
- **OBS** — one module may register several sources/filters/outputs, each checked by struct
  size; why "plug-in" is a packaging word and a class is a contract (§5).
- **Monado** — our origin and the contrast: in-tree drivers, no ABI, every vendor a build
  option. ADR-019 left that model; this record is what replaced it.

## Alternatives rejected

- **One generic "component" ABI with a kind enum.** Collapses seven classes' worth of
  capability-specific structs into tagged unions and makes every class's ABI bump every
  other's. The shared part is the *loader*, not the vtable (N2 shares exactly that).
- **Admission by hot-path-ness.** Would have admitted display profiles (hot) and refused
  tracking providers (not hot) — the opposite of what the failure-domain and
  second-implementer tests give.
- **Audio as a class.** The runtime would load, version and supervise a vendor audio engine
  it never calls. A pose consumer gets the data with none of that coupling.
- **Decoders as a class.** Would duplicate conversion's readiness, licensing, job lane and
  hosting for a task that already fits its transport.
- **Qwerty as an input-provider DLL.** Clean on paper; in practice it removes the floor the
  arbiter, WebXR profiles and the CTS all stand on. A gate gives the user the same control.

## Amendment 1 — Admission: the three-part test, the alternatives ladder, and the applied verdicts (2026-10-18)

**Status:** Accepted (owner decision after the 2026-10-18 architecture review; agreed by the
Windows and Linux validation sessions). Replaces §3's admission rule; §2's seven properties,
§4's verdicts and §5 stand, and decision 8 (selection scope) is restated here as part of what a
proposal must declare.

### What was wrong

§3 admitted a class when **either** its failure domain had to be separable from the service
**or** a second implementer existed. Both clauses are satisfiable without a class: ADR-053's
satellite host isolates any provider's failure domain without a new ABI, and a second
implementer of an *audio engine* or a *neural model* is served by a pose consumer or a
conversion pack. Used literally, §3 would have admitted spatial audio and media decoders — the
two candidates §4 refuses. The rule and the verdicts disagreed; this amendment makes the rule
produce the verdicts.

### Decision

1. **A class is admitted only when all three hold.**
   1. **Distinct, runtime-managed responsibility.** The runtime must *invoke, schedule, select
      or arbitrate* the capability itself, and that responsibility cannot be expressed cleanly
      inside an existing class (a task, a device role, a model pack, an optional slot) or by any
      other mechanism on the ladder below.
   2. **Interchangeable implementations or independently managed domains have real value.**
      Competing suppliers, per-capability selection and fallback, distinct licensing, or an
      independently supervised failure domain — *and* that value is not already obtained by
      hosting a provider of an existing class in a satellite.
   3. **A stable vendor-neutral contract can be specified now.** Semantics, ownership,
      selection scope, timing, lifecycle, state vocabulary, security and consent, and a
      simulated reference implementation — all of it, before the first vendor binary.

   "A second implementer exists" and "the failure domain must be separable" are **evidence**
   for criteria 2 and 1 respectively, never sufficient on their own.

2. **The alternatives ladder is the mandatory analysis.** Every new-class proposal carries a
   one-page section that walks the ladder in order and says why each lower rung does not fit:
   1. an existing OS or platform facility (standard webcam capture, OS audio);
   2. application or web-SDK functionality needing no runtime coordination;
   3. runtime **policy** (convergence, depth budget, cursor, arbitration — §4.6);
   4. an internal runtime module (shared implementation, not replaceable);
   5. a data, calibration or configuration profile;
   6. a model or asset pack inside an existing engine (ADR-057: model = pack);
   7. a new **task** or role inside an existing class (decoding under conversion, #2145;
      gamepad under input);
   8. an optional capability on an existing provider (the stereo-camera slots on a display
      plug-in, ADR-043);
   9. an app-facing OpenXR extension (an API does not imply a provider class);
   10. an external registered **consumer** over IPC or shared memory (spatial audio reading
       viewer poses, §4.3);
   11. an independently hosted service or process speaking an established protocol (the
       remote conversion endpoint, ADR-057 §6; a workspace controller, the process contract
       over IPC of §2 and `workspace-controller-registration.md` — shells and verticals land
       here, not on rung 12);
   12. a new component class — only when none of the above captures the requirement.

   The rungs are a **decision menu, not mutually exclusive layers**: one capability may use
   several at once (stereo capture today is an OpenXR extension, a display plug-in's optional
   slot, a runtime-owned generic source and a web-SDK primitive together). The analysis asks
   only whether the part that would become a class is already served by a lower rung.

3. **A proposal declares its selection scope** (decision 8) and its hosting: display
   processors select per screen, conversion providers per task and per stream or job, input
   providers per role family and per client; a new class names its scope before it ships its
   `prefer` verb, and says whether it loads in-process (frame-critical, contained) or in a
   supervised satellite (§3's hosting rule is unchanged).

4. **Admission deliverables** stay what N4 demands — a shipped simulated provider, the
   per-class conformance step in CI (ABI-major rejection and fall-through, `Enabled=0`
   fall-back, `Preferred` wins), provenance classification, and the four verbs in the CLI and
   the Dashboard — plus two learned this week:
   - **one cross-class integration test** on the route the class touches (input → consumer,
     tracking → renderer → display processor, camera → rectification → browser track,
     conversion → synthesized views → compositing). Every defect the validation boxes found in
     the input batch sat on a seam between classes, not inside one.
   - **the same capture trigger on every compositing route** the class adds or extends, so a
     pixel oracle exists on every platform (the POSIX weave engines lacked one, #2207).

5. **Applied verdicts**, restated under the new test so the rule and §4 agree:
   - **Spatial or directional audio — a registered pose consumer (§4.3, ladder rung 10), not
     a class.** The runtime never invokes it (criterion 1 fails). The pose-feed design (N8)
     is one shared seqlock *pose page* primitive used in both directions — service → consumer,
     and input satellite → service — with sample-at-target-time semantics that are the *same*
     authority the display processor's eye query and `xrLocateViews` use, so the image and the
     sound field never disagree. Its lifecycle decouples consumer churn from sensor start and
     stop (minimum on-time and linger; the service's own subscription counts as a consumer;
     "tracking with no consumer" is a visible state), because sub-second sensor open/close
     cycles have wedged real tracking cameras. It also coordinates with the display platform's
     own idle teardown (a vendor tracker may disable its camera after an idle linger, 300 s on
     the first vendor; a consumer that starts after that waits for the camera to re-enable) —
     the display plug-in's "tracking without a weaver, indicator lit" mode is the prerequisite
     (displayxr-leia-plugin#409).
   - **Stereo cameras — the existing sources (rung 8 and a runtime module), not a class
     yet.** A display's own camera comes through the display plug-in's stereo-camera slots;
     ordinary USB side-by-side cameras through the runtime's vendor-neutral UVC source
     (ADR-043 Amendment 4), which must never match a sensor an active display processor
     claims. **Named trigger** (replaces "when a standalone camera exists"): a first-party app's
     broadcast SDI capture path *is* a standalone stereo source today and stays in-app (one
     consumer, monitoring semantics). A **second consumer** of that feed is the trigger to
     **re-evaluate** a camera-provider class under all three tests — not an admission. If it
     passes, the provider is hosted per ADR-053's satellite template with shareable GPU frames,
     behind the same camera manager, consent and `XR_DXR_stereo_camera` API — an internal
     change with no new consumer API.
   - **Media decoding — a task family inside conversion (rung 7, #2145).** Unchanged.
   - **Display profiles and reference weaving — inside the display processor (§4.2).**
     Unchanged.
   - **Viewer tracking — the fourth class (§4.1).** Passes all three: the trunk selects the
     pose source per screen and per viewer relationship (the eye-pair ring is keyed by the
     screen/active-viewer pair, so one tracker per monitor is not assumed), the display's own tracker and an independent provider are
     interchangeable, and the contract is specifiable (target-time eye query and the eye-pair
     ring have landed). The reference provider is a webcam-driven simulated tracker.
   - **An external tracking feed into a display processor — not a class and not yet a
     rule change.** §4.1's "no `set_viewer_eyes` on the DP" stands; a display processor that
     *declares* itself external-tracker-capable may, as a **vendor-backed experiment** with
     explicit sensor-to-screen calibration and frame-time tests, consume the selected
     viewer-pose source. The ABI is not changed speculatively.
   - **Policy — the trunk (§4.6).** Unchanged.

### Consequences

- Admission gets slower and rarer, on purpose: a one-page ladder analysis is the cost of a
  class, and most proposals end on rungs 7–11.
- The next admissions in order: the viewer-tracking class (#2144, after the external
  tracking-vendor input), the pose-page primitive (N8), input-provider satellite hosting
  (ADR-035 D4; after the satellite host's hardening, optical hand tracker first).
- Remaining per-screen residue (implicit-primary paths in render, status and hotplug) is a
  completion audit, not design; it is tracked as its own issue with a two-vendor crossing
  test on a dual-screen box.

