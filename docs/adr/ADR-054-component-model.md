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

### 3. Admission rule

A capability earns its own class — its own ABI, root, loader entry and verbs — when **either**
of these is true:

- its **failure domain must be separable from the service** (it can crash, hang, leak or be
  unlicensed without taking composition down), **or**
- a **second implementer exists** or is contracted — a vendor who is not the display vendor
  needs an entry point.

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
