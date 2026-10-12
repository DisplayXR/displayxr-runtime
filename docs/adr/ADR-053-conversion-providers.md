---
status: Proposed
date: 2026-10-12
deciders: David Fattal
issues: []
related: [ADR-019, ADR-020, ADR-034, ADR-035, ADR-042, ADR-045, ADR-048, ADR-049, ADR-051]
---

# ADR-053: Conversion providers — 2D→3D models are a third plug-in type, the service composes them, apps see tasks

**Status:** Proposed (2026-10-12) · supersedes the plug-in contract of
[ADR-042](ADR-042-vendor-2d3d-conversion-supersedes-default.md) §5 (the display-processor
lift slots) and its consumer "open default" clause; generalises
[ADR-049](ADR-049-in-process-lift-rides-the-service.md) to every graphics API · keeps
[ADR-048](ADR-048-lift-viewpoint-policy.md) (viewpoint policy) untouched · plan and phases:
[roadmap/conversion-providers.md](../roadmap/conversion-providers.md) · plug-in contract:
[specs/runtime/conversion-provider-discovery.md](../specs/runtime/conversion-provider-discovery.md)
· header drafts: [roadmap/conversion-providers-headers-draft.md](../roadmap/conversion-providers-headers-draft.md)

## In one paragraph

Turning 2D content into 3D — depth from an image or a video, stereo or N-view synthesis,
disparity from a stereo pair, Gaussian splats from a photo — is done today by one vendor
module reached through six slots appended to the **display processor's** D3D11 vtable, plus an
open default that the browser and the web SDK run **inside the page** with their own model
store. This ADR makes conversion models a **third plug-in type** next to display processors
(ADR-019) and input providers (ADR-034): their own C ABI, discovery root and loader; several
providers resident at once, each declaring **tasks** it can serve; the service owning
selection, readiness, scheduling, pipelines and policy; providers doing pixels and units; and
the app-facing `XR_DXR_lift` extension growing additively so a shipped consumer keeps working
byte-for-byte while new consumers address tasks. Providers are hosted behind a
hosting-agnostic contract so they move out of the service into a supervised satellite with
no ABI change. The open models become a provider; no app ships a converter of its own.

## Context

What exists and what is wrong with it, as found on `main` and in the consumers
(2026-10-12):

- **Lift is a display-processor capability.** The conversion module is reachable only through
  slots on `xrt_display_processor_d3d11` and a lift-only factory on the display plug-in's
  iface. A conversion vendor that is not a display vendor has no entry point; a display vendor
  cannot ship a conversion module without shipping a display plug-in. The one shipping module
  serves depth and two-view synthesis; nothing serves N views or splats.
- **One module per machine, chosen by display probing.** The active display plug-in's factory
  is captured once; there is no enumeration, no identity beyond a backend string, no choice
  between modules, and no second module. The CLI even instantiates a second copy of the module
  in its own process to report readiness.
- **Windows and D3D11 only.** Every slot takes D3D11 resources. In-process sessions reach the
  service's module over a lift-only connection (ADR-049) — on Windows, for D3D11 and D3D12
  only. The Vulkan players and the engine plug-ins on Metal or Vulkan cannot use it.
- **Activation is lazy and latches.** The module warms on the first query; a consumer that
  asked before warm-up read "unavailable" and silently converted in the page for the whole
  session, at visibly lower quality, with no runtime stream in the log. "No module" is latched
  until the service restarts.
- **The open default lives in the consumer.** The browser installs a 1.65 GB model set, with
  the manifest and hash verification implemented three times (installer, browser, web SDK),
  runs an inference runtime single-threaded in the renderer, and hand-writes view synthesis
  and splat generation in WebGL. About eight thousand lines exist only because the runtime
  could not provide the service.
- **The vendor conversion runtime is process-global.** Its model choice is fixed per process,
  its temporal state is shared across streams, and its files stay mapped in the service, so a
  vendor installer must close the service to update, and idle unloading was tried and reverted
  because the reload stall was unacceptable.
- **Policy leaked into the model.** Convergence came from the model's saliency and jumped;
  relative depth is min-max normalised per frame so the depth budget is lost; intrinsics are
  assumed, not reported as such. ADR-048 already moved viewpoints into the runtime; the rest
  of the policy needs the same move, which needs honest units from the provider.
- **No job lane.** Photo → splats takes seconds and today runs on the same thread as video
  streams, behind a two-call blob latch with no progress or cancel and a 256 MiB cap.

The precedent for the fix is exact: ADR-034 made input providers a second plug-in type with
its own header, negotiate symbol, ABI constant, registry root and loader, sharing the
display loader's plumbing. ADR-043 deferred a camera-provider type only because every camera
source today is a display's own; ADR-045 already lists "a vendor conversion runtime" as an
independently installed component.

## Decision

### 1. A third plug-in type

A **conversion provider** is a DLL with its own negotiate symbol, ABI major
(`XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT`, starting at 1, policed under ADR-020), discovery
root (`ConversionProviders` on Windows; `NNN-<id>-conversion-provider.json` manifests in the
shared POSIX roots; a distinct library prefix in the runtime APK on Android) and loader that
reuses the display loader's shared helpers. A display plug-in DLL may also export the
conversion symbol and be registered under both roots, but separate DLLs are the recommended
shape (see decision 5).

The display-processor lift slots and the lift-only factory become a runtime-internal
**legacy adapter** provider. They are frozen under plug-in ABI 5 (never extended again) and
removed at the next ABI major.

### 2. Tasks, not modes; several providers, one selected per task

A provider declares **capabilities**: one row per (task × model × backend × device), each with
its input kinds, output contract, limits, licence class and cost estimates. The task
vocabulary is fixed in the ABI: monocular depth, video depth, stereo disparity, mono → N views,
image + depth → views, photo → splats, stereo → metric splats, with sequence and inpaint
reserved. Convergence summaries, auto-convergence, the depth budget and the cursor are
**not** tasks: they are runtime policy.

All registered providers are loaded and described at load; models stay cold until needed. The
runtime selects one capability per task, deterministically: per-task user preference, then
ProbeOrder (vendor band before the open band), then a stable tie-break; research-only
licences are considered only after an explicit opt-in that signed installers never write.
A provider's licence class covers the inference runtime it loads as well as its models, so
a provider cannot advertise a commercial class over an internal-use runtime; the legacy
adapter is grandfathered as unknown and exempt, because its module was installed before the
gate existed.
**Readiness never reorders selection**: a first choice that is installed, downloading,
warming or evicted is waited for; a new stream or job falls through to the next candidate
only when the first choice is absent, disabled or **failed for any reason** (a download the
policy forbids, a licence that cannot activate offline, a model that cannot fit), and the
preferred provider is tried again at the next create. A live stream never moves except on a
terminal failure of its provider or a host update, and the move is signalled. Apps never
choose a provider; provider identity is reported by the CLI and the status snapshot, not by
the app API, and a browser exposes only a coarse waiting / ready / unavailable to pages.

### 3. Two lanes, pipelines composed by the runtime

- **Streams** keep ADR-042's shape: a latest-wins mailbox per stream, priority classes,
  temporal state with an explicit discontinuity flag.
- **Jobs** are one-shot conversions with progress, cooperative cancel and results delivered as
  chunked blobs or into a caller-opened file handle — never a file path, and the service never
  decodes media. Jobs run on their own worker so a splat job can no longer stall a video
  stream.

The runtime composes each stream from stages: snapshot and crop, viewpoint policy, the
provider, a **vendor-neutral reference view synthesis** (so a depth-only provider still yields
views; a runtime stage, never a selectable provider, and it must match or beat the browser's
current in-page view synthesis on the lift test corpus before it replaces it), depth decode and
per-frame range, policy observers, publish. Scheduling is per provider host, so streams on
different providers no longer serialise behind one another. The default is one call in
flight per provider; concurrency of jobs, converts and warm-up is opt-in per capability.

### 4. Policy stays in the runtime; providers do pixels and units

Viewpoints (ADR-048), convergence (runtime auto-convergence, in flight), the depth budget and
the cursor's relief mapping are runtime stages. A provider must honour explicit viewpoints and
accept a caller-set convergence exactly; the runtime hands it that plane in the provider's own
decoded depth units, so estimator and provider cannot disagree on normalisation. A provider
may only *suggest* a convergence or a disparity budget, and the runtime ignores the suggestion
unless that provider has been validated against the stereo ground-truth corpus. The budget is
derived from the content's range only for metric depth; for affine depth, whose scale is
unknown by definition, it is a strength-scaled cap. It reports units
honestly: depth kind (metric, metric-inverse, affine depth, affine inverse depth, affine or
pixel disparity), measure, scale and shift, near/far, the **per-frame range before any
normalisation**, intrinsics with their source (given, estimated or assumed), coordinate
convention. Absent means unknown; a scale, baseline or focal length is never invented.

### 5. Hosting-agnostic contract; providers leave the service

The provider never receives a runtime graphics device. Every image it sees is runtime-allocated
and arrives as a shareable handle (NT handle, Vulkan external memory, IOSurface,
AHardwareBuffer, dma-buf) or CPU memory with an explicit sync; it imports and never exports.
Blocking calls (warm, convert, job) run on runtime-owned threads with a cancel callback; the
provider owns no activation thread and must not require a pumped window. Configuration is
read through the host, never the environment. Every struct that crosses the boundary is
either a top-level struct with `struct_size` or a frozen embedded struct pinned by a size
assertion; capabilities are described one at a time, so nothing in the ABI strides over a
growable element.

With that contract the same DLL runs **in the service** or **in a supervised conversion-host
satellite** (one per provider DLL; a job object, restart with back-off and a crash-loop
cutoff; medium integrity, never elevated). The satellite is the design target for every
out-of-tree provider: a crash, hang, heap corruption or DLL conflict in a model runtime stays
out of the compositing process; a vendor installer closes the host, never the service, so the
file-lock problem ends; and memory is accounted per process. Two rules keep the isolation
real: the service never enqueues a GPU wait on a fence the host signals on any compositor
context (it observes completion from the CPU first), and the service holds the only
cross-process handle right (it pulls the host's handles; the host never holds a handle to the
service process). The **legacy adapter stays in the service**: it is reached through the
display plug-in's own entry point, which ADR-035 D4 keeps in-process, so the satellite's
benefits arrive with the first separate provider DLL, not before. ADR-035 confirmed the
display processor stays in-process because it is on the per-frame hot path; conversion is
explicitly off that path (ADR-042 §3), so the opposite conclusion holds for providers. On
Android, the conversion core and its providers live in one dedicated bound service of the
runtime APK, reached by every per-client satellite and in-process app — never one model per
app.

### 6. Readiness is a state machine with events, and the consumer waits

Each (provider, task) is `installed`, `downloading`, `warming`, `ready`, `evicted`, `failed`
(with a reason) or `disabled`, with progress where it applies; "not yet asked" is never a
terminal state, and the legacy adapter declares its tasks as installed from the display
plug-in's static capabilities before any module has loaded. A spec-3 caller reads
*activating* while discovery is unsettled or any backing task is non-terminal, never
*unavailable*, because the shipped browsers never re-poll *unavailable*. The runtime prewarms
the selected provider for the default tasks when a client signals interest in the extension
(on battery: at the first stream); a reload is always reported as warming; a model bound to a
prewarm task is never evicted on idle. State changes are pushed as events in addition to
polling.

The failure this closes was seen live: a consumer queried readiness once before the module
was ready, cached the answer, and converted in-page at visibly lower quality for the whole
session with no stream in the service log. The consumer contract therefore also says: never
cache a non-ready answer beyond one poll interval.

The consumer contract replaces ADR-042's "open default": **a non-ready task is a reason to
wait, never a reason to switch paths.** There is no in-app converter; the open models are a
provider.

### 7. Graphics-API-neutral transport and the open provider

The transport is per platform and API — what exists today for D3D11 and D3D12 on Windows, and
Vulkan external memory on Linux and Android, IOSurface on macOS, with CPU as the universal
fallback — chosen per stream as the cheapest pairing of the source's memory kind and the
provider's declared imports. ADR-049's lift-only connection generalises to every in-process
session on every API wherever a service runs.

An **open-model provider** ships from its own public repository on a standard inference
runtime, with a model-pack manifest (content-addressed blobs, per-backend variants, SPDX
licence, a commercial flag that is the AND over the model and its dependencies, size, memory
and warm-up estimates). The model store becomes runtime-owned, adopting the browser's
existing store and layout but trusting the content hash rather than the stamp, re-hashing
each blob once per process before it reaches the inference runtime; a store wiped by an
older uninstaller is re-downloaded, never a latched failure; the host downloads on demand and reports progress through
the readiness state.

### 8. The app API grows in place

`XR_DXR_lift` moves to spec 4 (task vocabulary, provider and task enumeration, per-task
readiness and events, task-typed streams, platform-neutral images with explicit sync, the
depth and views result contracts, a discontinuity flag) and then spec 5 (jobs, splats with a
camera block, stereo and depth inputs, binding a stream to a runtime stereo camera). Every
spec-3 struct, enum value and behaviour is preserved; a spec-3 caller is served by a fixed
mapping of its four modes onto tasks. A new extension name was rejected because the browser's
GPU process enables lift once in a pre-sandbox instance it cannot re-create, and because the
weave-rect and cursor chains take a lift stream handle. Internally the word is "conversion";
to apps it stays "lift".

## Consequences

- **+** Any vendor — display or not — and the open models plug in the same way; the best
  available provider per task is selected without the app knowing which.
- **+** One model instance per machine, shared across every app, with cross-app priorities
  that mean something (ADR-049's goal, now independent of the display plug-in).
- **+** The browser and web SDK delete their model store and in-page converter; model
  provisioning is implemented once, by the runtime side, with licence data attached.
- **+** The silent-fallback class of bug is closed by construction: readiness is explicit,
  pushed, and never reorders selection.
- **+** A model runtime's crash, hang or update no longer touches compositing; vendor
  installers stop closing the service.
- **+** Policy (viewpoints, convergence, budget, cursor) has honest inputs on every provider.
- **+** Hardware-free end to end: a CPU-only simulated provider exercises every state, both
  lanes and the satellite path in CI.
- **−** A third loader, ABI and release gate to police (ADR-034's cost, again); a new process
  to supervise; one more public repository and a `versions.json` field.
- **−** Providers whose runtime returns its own buffers pay one extra GPU copy (runtime
  allocates every image); the satellite adds two ring writes and two wakes per conversion
  (tens of microseconds against tens of milliseconds).
- **−** The display-processor lift slots become dead ABI for the rest of ABI 5, and the
  legacy adapter keeps the vendor module in the service until a separate provider ships.
- **−** Until the non-Windows transports land, the extension reports every task as
  unreachable from those sessions — honestly, rather than unsupported.

## Alternatives rejected

- **Keep lift inside the display-processor contract and append more slots.** Couples a second
  vendor population to the display vtable's evolution, keeps one module per machine chosen by
  display probing, and cannot express several providers or non-D3D11 hosts (the same argument
  as ADR-034 option 1).
- **A model-runtime abstraction instead of a plug-in type** (the runtime hosts an inference
  engine and loads model files directly). It cannot host a vendor's proprietary runtime, and
  it puts every engine's dependencies into the service. The open provider *is* that engine,
  behind the same contract as a vendor's.
- **Providers in the app process** (ADR-049 options A and B). One model per lifting process,
  with duplicated VRAM, warm-up and schedulers; rejected there and still rejected here.
- **Providers in the service forever.** Simplest, but it keeps the file-lock and crash-domain
  problems that caused the idle-unload revert and the Restart Manager work; and two inference
  runtimes in one process is a DLL conflict nobody can fix from the runtime.
- **A new `XR_DXR_conversion` extension with lift as a shim.** See decision 8.
- **Prefer a ready provider over a warming higher-ranked one.** Timing-dependent output; the
  browser's silent fallback rebuilt inside the runtime.
- **File paths in the job API.** Confused-deputy exposure from low-integrity clients and every
  codec vulnerability inside the always-on service.

## Phases

P0 this ADR and its docs; P1a the core behind the existing lift thread (the legacy adapter
wraps the same calls; no file split), with the simulated provider and the spec-3 bug fixes;
P1b the file split, one release later; P2 the satellite host, tested with the simulated
provider; P3 lift spec 4, the vendor's provider DLL hosted in the satellite from day one, and
the installer contract change; P4 the open provider and the browser deletions; P5 non-Windows
transports and the Android service; P6 lift spec 5 and the Vulkan/engine consumers. Cut from
v1 without harming the architecture: memory-budget admission, a result cache, cross-adapter
hosting, provider enumeration in the app API, and the reserved tasks. Deliverables, tests,
risks and the must-verify list per phase:
[roadmap/conversion-providers.md](../roadmap/conversion-providers.md).

## Amendments to earlier records

- **ADR-042**: §1 ("a READY vendor module supersedes the open default") and the consumer
  contract's open-default rows are superseded by decision 6; §5 (the plug-in contract) is
  superseded by decision 1 and frozen as the legacy adapter. §2–4 (pre-weave results, the
  asynchronous mailbox, scheduling as runtime policy) stand.
- **ADR-049**: the lift-only connection applies to every in-process session on every graphics
  API; its D3D12 submit-fence follow-up is resolved by the spec-4 image sync kinds; its
  adapter-LUID follow-up is resolved by per-task export capability data.
- **ADR-045**: the conversion runtime it names as a fourth independently installed component
  is now a plug-in type with its own platform state.
