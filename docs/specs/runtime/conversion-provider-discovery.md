# Conversion-Provider Discovery Contract

> **Status: Implemented — ADR-053 (Accepted). The P1a conversion core landed in #1994 / #2015 and ships from v2.42.0 (core, loader, `dp-lift`, the sim provider); P1b and P2 (the satellite host) are implemented; §3.8 is the installer contract from P3.** Open follow-ups: live re-select of `Enabled` / `PreferredProvider` in a running core (#2051) and the shared options contract (#2050) under the N3 epic #2047.
> Header: [`src/xrt/include/xrt/xrt_conversion_plugin.h`](../../../src/xrt/include/xrt/xrt_conversion_plugin.h) (design record, kept for history: [`conversion-providers-headers-draft.md`](../../roadmap/conversion-providers-headers-draft.md) §1). Writing a provider: [`vendor-plugin-onboarding.md` §9](../../guides/vendor-plugin-onboarding.md#9-conversion-providers--the-third-plug-in-type) and the `displayxr-conversion-template` starter kit.

**Audience:** integrators shipping a conversion-provider plug-in (a DLL / dylib / so that turns 2D images into depth, views or Gaussian splats for the runtime), and runtime engineers maintaining the conversion core, its loader and its host.

This spec deliberately mirrors the display-processor contract ([`plugin-discovery.md`](plugin-discovery.md)) and the input-provider contract ([`input-provider-discovery.md`](input-provider-discovery.md)). Where a rule is not restated here, the display-processor rule applies unchanged. Rationale lives in [ADR-053](../../adr/ADR-053-conversion-providers.md); this document is the contract.

`file:line` references point at `main` @ `ec6aee48b` and mark the code each rule replaces or reuses.

---

## 1. What a conversion provider is

A conversion provider is the **third plug-in type**, next to display processors (ADR-019, ADR-020) and input providers (ADR-034). It turns 2D into 3D: depth from one image or a video, disparity from a rectified stereo pair, N views from one image (or from an image plus depth), and Gaussian splats from a photo or a stereo pair. A conversion vendor is not necessarily a display vendor, so the type has its own header, entry symbol, ABI constant, discovery root and release cadence (ADR-053 decision 1).

The runtime owns every thread, queue, policy and app-facing API. The provider does **units and pixels only**.

### 1.1 Tasks

A provider exposes a fixed vocabulary of **tasks** as **capabilities**. One capability is one (task × model × backend × device). Task values are stable ABI (`XRT_CONV_TASK_*`).

| Value | Task | In → out | Lane |
|---|---|---|---|
| 1 | `DEPTH_MONO` | one image → depth (stateless) | stream, job |
| 2 | `DEPTH_VIDEO` | frames → depth; temporal, reset on cut / seek | stream |
| 3 | `DISPARITY_STEREO` | rectified pair + calibration → disparity (+ confidence) | stream, job |
| 4 | `VIEWS_MONO` | image → N ≥ 2 views (+ same-inference aux depth) | stream |
| 5 | `VIEWS_FROM_DEPTH` | image + depth → N views (DIBR + inpaint) | stream |
| 6 | `SPLATS_MONO` | photo → 3DGS blob | job |
| 7 | `SPLATS_STEREO` | pair + calibration → metric 3DGS blob | job |
| 8 | `SPLATS_VIDEO` | **reserved**: value only, no code in v1 | — |
| 9 | `INPAINT` | **reserved**: value only, no code in v1 | — |
| 10..63 | reserved | | |

**Not provider tasks:** convergence summaries, auto-convergence, the depth budget and the cursor. Those are runtime policy (§1.5).

**Pipelines are composed by the runtime** (ADR-053 decision 3). A depth-only provider still yields views: the runtime chains its depth capability into the runtime's reference view-synthesis stage (§6.2). A provider never calls another provider.

### 1.2 Two lanes

| Lane | Shape | Provider slot |
|---|---|---|
| **Stream** | per-frame, latest-wins mailbox, priority classes, per-stream temporal state, `source_time` echoed | `stream_create` / `stream_convert` / `stream_destroy` |
| **Job** | one-shot, decoded pixels in, blobs out, progress + cancel, seconds to minutes | `job_execute` (one blocking call) |

Both lanes are scoped to the client connection. The app-facing job API is submit / poll / cancel / collect; the runtime builds it on top of the single blocking `job_execute`, on its own job worker. Whether a job may overlap that provider's streams is the provider's declaration (§2.3). Jobs take **decoded pixels** (a CPU or GPU image), never a file path; the service never decodes media. Clients downscale to `maxInputExtent` before submitting; a CPU job input is capped at 256 MiB (ADR-053 decision 3).

### 1.3 Hosting-agnostic

The same provider binary runs **inside the service** or inside a supervised **conversion-host satellite** (`displayxr-conversion-host`, ADR-035 D4/D5 shape), unchanged. The provider never receives a runtime graphics device and never exports memory: every image is runtime-allocated and arrives as an OS shareable handle or as CPU memory (`xrt_conv_image`). §2.4 makes the rules normative (ADR-053 decision 5).

### 1.4 Naming: "conversion" inside, "lift" outside

Inside the runtime — headers, loader, registry, CLI, ADR — the word is **conversion** (`xrt_conversion_plugin.h`, `ConversionProviders`, `displayxr-cli conv`). App-facing, it stays **lift**: apps reach providers through `XR_DXR_lift` spec 4 and 5 (ADR-053 decision 8). There is no `XR_DXR_conversion` extension. Apps never see, name or select a provider; provider id, backend and version strings are diagnostics only (CLI and status snapshot). A browser exposes only a coarsened waiting / ready / unavailable state to pages.

### 1.5 What a provider must NOT contain

| Not in a provider | Owned by |
|---|---|
| Viewpoint / app-rig policy | runtime (ADR-048); the provider renders exactly `params.viewpoints_xyz` |
| Convergence policy | runtime (rt#1978). A provider may only *suggest* (`suggested_convergence`, `suggested_strength`); ignored unless corpus-validated |
| Depth budget | runtime (ADR-040 plus a new budget stage using the per-frame range) |
| Cursor relief | runtime (ADR-046 A1); the provider reports the generic `relief_*` field |
| OpenXR types | the state tracker. The header is self-contained: `xrt_results.h` + `<stdint.h>` / `<stddef.h>` / `<stdbool.h>` |
| Windows (top-level or hidden on a non-pumping thread) | nobody (§2.4) |
| Network on the convert path | the host fetches model bytes; network only inside `warm` (§2.4) |

---

## 2. C ABI

Full header: [`conversion-providers-headers-draft.md` §1](../../roadmap/conversion-providers-headers-draft.md). Reference: `docs/reference/xrt_conversion_plugin_iface.md` (lands with P1).

### 2.1 Entry symbol and version

```c
xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version);
```

- `XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME` = `"xrtConversionPluginNegotiate"`.
- ABI constant `XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT` = **1**, independent of `XRT_PLUGIN_API_VERSION_CURRENT` and `XRT_INPUT_PLUGIN_API_VERSION_CURRENT`. Same major is compatible; a different major is rejected by the loader before any dispatch (ADR-020 rule 3).
- The provider instance returned by `probe` **is** the module container. There is no `create_module`: capabilities are addressed by a provider-chosen non-zero `cap_id`, stable for the instance's life, and `get_state` / `warm` / `evict` take a `cap_id`.
- **`describe_cap(inst, index, out)` returns one capability per call**; `out == NULL` returns the count. There is no array out-parameter, so a runtime and a provider built with different `sizeof(capability)` never mis-stride.

### 2.2 Struct rules (ADR-020, plus)

- **`struct_size` on every top-level struct, set by the caller.** For an out-struct the owner (the runtime, for `describe_cap`, `get_state` and results) pre-sizes and zero-fills it, and the provider writes only within it (the rule at `xrt_dp_lift.h:22-24`).
- **Embedded structs are frozen.** `xrt_conv_sync`, `xrt_conv_image`, `xrt_conv_depth_info`, `xrt_conv_views_info`, `xrt_conv_splat_info` and `xrt_conv_stereo_calibration` carry no `struct_size`, never grow, and are pinned by `sizeof` / `offsetof` asserts on x64 and arm64 (and armv7 if the APK ships it). Growth happens only at the tail of a top-level struct or through a new iface slot.
- **Append-only, forever.** The iface vtable, the host iface and every top-level struct grow only at the end. Each appended iface slot gets an `XRT_CONVERSION_PLUGIN_HAS_<X>` macro, so the downstream `features` repin gate sees it. Host-iface slots are carved from `reserved[10]` without a major bump.
- **Whole-field coverage.** `XRT_CONV_COVERS(ptr, TYPE, FIELD)` and `XRT_CONV_IFACE_HAS(iface, FIELD)` mirror `XRT_INPUT_PLUGIN_IFACE_HAS` (`xrt_input_plugin.h:119-122`).
- **Flat out-structs.** Strings are fixed-size `char[]`; no pointer in an out-struct outlives the call (the iface's `id` / `display_name` / `vendor` / `version` excepted). Images never cross a process boundary as bytes: the satellite proxy re-materialises them per hosting.
- **Enumerations are `uint32_t` + `#define`s**, never a C `enum` (implementation-defined size; crosses IPC).
- **One set of values.** `XRT_CONV_STATE_0..7` are numerically identical to `XRT_DP_LIFT_MODULE_*` (`xrt_dp_lift.h:130-137`); 8..10 (`INSTALLED`, `DOWNLOADING`, `EVICTED`) are new. The plug-in values are canonical: the spec-4 app enums for state, depth kind, measure, pixel format, views layout, licence class and blob format use the same numbers. `oxr_plugin_stub.c` asserts every pair.
- **Ceilings.** `XRT_CONV_MAX_VIEWS` = 8, equal to `XR_DXR_lift`. `XRT_CONV_VIEWS_ATLAS` is reserved; the runtime picks ARRAY (or ATLAS once defined) when `N × width` exceeds the texture limit. Raising the ceiling is a later slot. `XRT_CONV_MAX_CAPABILITIES` = 32 per provider; `XRT_CONV_MAX_FRAME_IMAGES` = 4.
- **Runtime-allocated images**, inputs **and** outputs. The provider imports and never exports. In satellite mode the host process, which is runtime code, allocates outputs on the capability's `device_luid` and the service pulls the handles (§4.4), so "runtime-allocated" holds in both hostings.

### 2.3 Threading contract (normative)

**Default: one call in flight per provider.** Four read-only slots are exempt: they stay concurrent with everything and answer from memory. Everything else is serialised per provider unless a capability opts in.

| Slot | Caller thread | May block | Concurrency | Budget |
|---|---|---|---|---|
| `xrtConversionPluginNegotiate` | the loader | no | once per process | ≤ 100 ms |
| `get_provider_status` | ANY, no instance | no | **concurrent** | ≤ 100 ms (ADR-045, `xrt_plugin.h:1268-1278`) |
| `describe_cap`, `get_generation` | ANY | no | **concurrent** | ≤ 100 ms |
| `get_state` | ANY | **never** | **concurrent** | µs; allocation-free |
| `stream_get_output_desc` | ANY | no | **concurrent**, pure | µs |
| `probe` | CTRL | no | once | ≤ 100 ms; no model load, no network |
| `warm`, `evict` | CTRL | yes | serialised; `warm` overlaps converts only with `WARM_CONCURRENT` | `warm` unbounded; polls `is_cancelled` ≤ 250 ms |
| `stream_create`, `stream_destroy` | SW (the stream's worker) | briefly | serialised | ≤ 500 ms |
| `stream_convert` | SW | one inference | serialised; up to `max_concurrent` per cap when declared | bracketed by the stall watchdog on every call |
| `job_execute` | JW (one per provider) | minutes | serialised; overlaps streams only with `LANES_CONCURRENT` | polls `is_cancelled` ≤ 250 ms |
| `destroy` | CTRL | yes | **may never be called** | — |
| host callbacks | any provider thread | no | thread-safe | non-blocking |

**ANY** = any runtime thread; **CTRL** = the per-provider control thread; **SW** = a stream worker; **JW** = the per-provider job worker.

- **Opt-ins**, per capability: `max_concurrent` lets converts overlap; **`XRT_CONV_CAP_LANES_CONCURRENT`** (bit 7) lets a job overlap streams; **`XRT_CONV_CAP_WARM_CONCURRENT`** lets `warm` overlap converts. Without them the runtime serialises, and the stall is reported in the stream's stats (`blocked_by_job`, `blocked_by_warm`) with an event. A job on a provider without `LANES_CONCURRENT` is routed to another provider of the task when one exists.
- **Cancel is a callback**, `bool (*is_cancelled)(void *ctx)`, in `xrt_conv_job_control` and as a `warm` parameter. `warm` and `job_execute` block on **runtime-owned** threads; there are no provider-owned activation threads.
- **Runtime obligations.** No slot is ever called from a render, compositor or IPC-dispatch thread, or with `render_mutex` or any conversion lock held. Eye positions reach the provider through `params`, read from the eyes snapshot of the display processor that owns the stream's rect — one snapshot per live display processor, published from its eye listener or a timer, with a generation counter. The per-conversion `render_mutex` take through `eyes_fn` (`comp_d3d11_service.cpp:~30896`) is retired.
- **Aux depth is an output of `stream_convert`**, not a separate slot: the runtime allocates every requested target and the provider fills each one in the same inference.

### 2.4 Hosting-agnosticism rules (normative)

Each rule must hold in both hostings.

| Area | Rule |
|---|---|
| Devices | MUST NOT expect a runtime device or context. Creates its own (D3D11/12, Vulkan, Metal, an inference session) on the adapter `get_adapter_hint` names — by default the service's ingest adapter — and reports it in `capability.device_luid`. Cross-adapter hosting is cut from v1: a capability on another adapter is served through CPU staging (`CONV_CPU_STAGING`). |
| Memory | MUST import only, never export. Caches imports by `import_key`; drops an entry on `stream_destroy` or when unseen for 64 frames. MUST NOT hold a handle past a call unless it duplicated it. |
| CPU path | Every capability MUST import `XRT_CONV_MEM_CPU`, at least on the job lane (GL / Metal apps, CI on WARP, remote hosts). |
| Threads | MAY create internal threads. MUST NOT require the caller's thread to pump messages. Initialises COM only on its own threads. |
| Windows | MUST NOT create a top-level or hidden window on a non-pumping thread. A dependency that needs one gets `HWND_MESSAGE`, or a thread that pumps at ≤ 250 ms (Restart Manager and logoff hang otherwise, `service-architecture.md:247`). |
| Process state | MUST NOT call `SetDllDirectory`, `SetCurrentDirectory`, `SetUnhandledExceptionFilter`, `timeBeginPeriod`, change DPI awareness, `putenv`, `exit` / `abort`, or install signal handlers. Errors are returned. |
| DLL loading | Dependencies load by absolute path (`LoadLibraryExW` + `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`, backslashes), never via `PATH`. MUST NOT statically import the vendor conversion runtime: the provider DLL loads without it (ADR-045 rule 1, `plugin-discovery.md` §4.1). |
| Files | Holds files open only while needed. Writes only to `XRT_CONV_PATH_SCRATCH`, atomically (temp + rename). MUST NOT write the model store; the host does. |
| Lifetime | MAY be killed at any instant without `destroy`, or live for weeks. No per-frame leaks. |
| Elevation | Medium integrity, interactive user session. Licence activation MUST work non-elevated. |
| Network | Only inside `warm`, only on caps flagged `NEEDS_NETWORK_TO_WARM`. **Never** in `stream_convert` / `job_execute`. Model bytes come through `model_request`. |
| Configuration | Only through `host->get_setting`, never the provider's own `getenv` (in a satellite the environment is the host's). The host resolves a key from `Settings\<KEY>` (§3.1), overridable by `DXR_CONV_<ID>_<KEY>` in the service's environment (§3.2). |
| Logging | ADR-019 aux surface (`u_log*`), exported by both hosts. |
| GPU loss | Return the DEVICE_LOST class. The runtime marks the cap `FAILED(DEVICE_LOST, retryable)` and re-warms on CTRL. No silent retry inside a convert. |
| Latency | `get_state`, `describe_cap` and `stream_get_output_desc` answer from memory; IPC replies sit on them. |

**The runtime guarantees in return:** the §2.3 threads and never a render thread; a fence on every GPU image, or `SYNC_NONE` meaning the producer finished before hand-over; keyed mutexes only on the client → service input import (as today), never service ↔ host; stable `import_key`s; targets sized exactly per `stream_get_output_desc`; a CPU copy for any memory kind the provider cannot import; a stall watchdog that names provider + slot on every call.

### 2.5 Semantics the header cannot carry

- **Honesty.** Unknown means zero or the sentinel — never an invented intrinsic, baseline, focal or scale. `ASSUMED` intrinsics are legal only when labelled.
- **Viewpoints.** Every `VIEWS_*` capability MUST set `EXPLICIT_VIEWPOINTS`; selection refuses one that does not. `viewpoints_xyz` is authoritative; the provider never reads a tracker.
- **Convergence.** `params.convergence_mode == DECODED` hands the provider the zero-parallax plane in the **decoded units of its own depth output** (the mirror of `depth_info.convergence_value`; polarity follows the depth kind), and also means "provider auto-convergence off"; the provider never normalises it again. `AUTO` means no plane was given. The runtime maps an app's [0,1] "relative depth at glass" onto decoded units with the last result's range, so the runtime's estimator and the provider agree on what the value means. A provider's own estimates go in `suggested_convergence` / `suggested_strength` (declared by `CAP_SUGGESTS_CONVERGENCE`); **policy ignores them unless the provider is validated against the stereo ground-truth corpus** (a vendor module's own auto-convergence measured about twice the error of the runtime rule on that corpus).
- **Temporal state is per stream.** A provider whose library shares temporal state across streams resets it at stream boundaries, or declares `max_streams = 1` on its `TEMPORAL` caps.
- **Stream limits** are the selected provider's `max_streams` per task; spec-3 `maxStreams` reports `VIEWS_MONO`'s.
- **A process-wide model choice** is one capability whose variant follows `qualityHint`, not a set of capabilities that exclude each other.
- **`XRT_CONV_INPUT_ENCODED`** is reserved.

---

## 3. Discovery

### 3.1 Windows — registry

**Root:** `HKLM\Software\DisplayXR\ConversionProviders` (64-bit view; NSIS `SetRegView 64`). One subkey per provider, `<id>`.

| Subkey value | Type | Req | Meaning |
|---|---|---|---|
| `Binary` | REG_SZ | yes | absolute DLL path |
| `DisplayName` | REG_SZ | yes | |
| `Vendor`, `Version`, `UninstallString` | REG_SZ | no | informational, as in `plugin-discovery.md` §2. Installers write `Version` **last** (§3.8) |
| `ProbeOrder` | DWORD | no | default 100; bands in §3.3 |
| `Enabled` | DWORD | no | 0 disables (input-provider precedent) |
| `Hosting` | REG_SZ | no | `auto` (default) / `in-service` / `satellite`, for plug-in providers. P1 honours `in-service` only; from P2, `auto` = satellite for every out-of-tree DLL. The legacy adapter is always in-service (§4.5) |
| `Supersedes` | REG_SZ | no | `dp-lift` hides the legacy adapter row (§4.5). Hides only the named id; never affects selection among other providers, so a vendor provider whose module is absent still falls through to the next candidate |
| `Packs` | REG_MULTI_SZ | no | absolute paths of the provider's model-pack manifests (§5) |
| `Settings\*` | REG_SZ | no | read through `host->get_setting`; replaces per-vendor ad-hoc knob keys and sidesteps the logon-environment trap. Since N3c the machine tier of the one options contract (§3.9): a per-user HKCU `Settings\<KEY>` overrides it (`conv set <id> <KEY> <value>`), `DXR_CONV_<ID>_<KEY>` (§3.2) overrides both |

**Root values** (on `ConversionProviders`). **HKCU overrides HKLM** where noted, unless HKLM `Policy\Lock` = 1, which makes every HKLM value win (kiosks, IT policy).

| Root value | Type | Default | Meaning |
|---|---|---|---|
| `PreferredProvider\<task>` | REG_SZ | — | `"<provider-id>[/<cap_id or model_id>]"`, task names as `views-mono`, `depth-video`, … Written by `displayxr-cli conv use` to HKCU (per user, no admin, as `dp use`); `--machine` writes HKLM. |
| `AllowResearchOnly` | DWORD | 0 | 1 lets selection consider `RESEARCH_ONLY` and `UNKNOWN` licence classes. HKCU or HKLM. **Signed installers never write it**, and never pre-fetch a non-commercial blob. |
| `Prewarm\<task>` | DWORD | 1 for `views-mono` and `depth-video`; else 0 | which tasks are prewarmed (§4.2); 0 opts out |
| `PrewarmOn` | REG_SZ | `interest` on mains power / desktop; `first-create` on battery | `interest` / `first-create` / `start` (kiosk opt-in); §4.2 |
| `KeepAliveSec` | DWORD | **0 = never** | idle eviction of prewarm-task models. Job-only models evict after **600 s** idle. Nothing is evicted while a stream or job is bound. A reload is always reported WARMING with progress |
| `Policy\Lock` | DWORD | 0 | HKLM only; 1 makes HKLM win over HKCU for every value above |

### 3.2 Kill switches and overrides

Read in the **service's own environment** (and the host's, for a satellite). The service auto-starts from the logon environment, so a variable set on a client never reaches it — the trap in `CLAUDE.md` § *The service reads its OWN environment*.

| Variable | Effect |
|---|---|
| `DXR_CONV=0` | No conversion service. `DXR_LIFT=0` (`d3d11_lift.cpp:780-786`) stays as an alias. |
| `DXR_CONV_PROVIDERS=0` | Load no plug-in providers; only the built-in legacy adapter remains. |
| `DXR_CONV_EXCLUSIVE=<id>` | Load only `<id>` (CTS, CI). Mirrors `DXR_PLUGIN_EXCLUSIVE` (`plugin-discovery.md` §2.2). |
| `DXR_CONV_<ID>_<KEY>` | Overrides provider setting `<KEY>`; `<ID>` is the provider id upper-cased with `-` → `_` (e.g. `DXR_CONV_SIM_CONVERSION_ENABLE`). |

### 3.3 ProbeOrder bands

One convention across plug-in types (ADR-034 *Amendment 3*): lower wins.

| Band | Use |
|---|---|
| 0 | reserved |
| 50 | vendor providers |
| 100 | default when missing |
| 150 | legacy `dp-lift` adapter (synthetic row) |
| 200 | the open-model provider |
| 250 | `DisplayXR-SimConversion` (FALLBACK) |

The reference view synthesis is a pipeline stage, not a row (§6.2).

### 3.4 Selection (deterministic)

Selection runs per task, at stream / job create (ADR-053 decision 2).

1. **Candidates.** Every capability with the task, from providers that are enabled, negotiated at ABI 1 and probed OK, and whose provider status is not `PLATFORM_ABSENT` or `INCOMPATIBLE` — plus every runtime-composable pipeline (e.g. `VIEWS_MONO` = a depth capability + the reference view-synthesis stage).
2. **Filter.** Licence gate (`AllowResearchOnly`), then input kind, lane, `min_views ≤ view_count ≤ max_views`, and `EXPLICIT_VIEWPOINTS` for `VIEWS_*`.
3. **Order.**
   - `PreferredProvider\<task>`: HKCU, then HKLM (HKLM only under `Policy\Lock`).
   - ProbeOrder ascending.
   - A direct capability before a composed pipeline.
   - Provider-id `strcmp` tie-break, with one WARN (the #1466 rule).
   - `cap_id` within a provider.
   - FALLBACK providers sort last within the ProbeOrder step; an explicit per-task preference (step 1) still wins.
4. **Readiness never reorders.** The first candidate is selected while it is `INSTALLED`, `DOWNLOADING`, `WARMING` or `EVICTED`, and consumers wait. Every other state — `ABSENT`, `INCOMPATIBLE`, `UNLICENSED`, `DISABLED` and **any** `FAILED`, retryable or not — falls through to the next candidate for new creates, logged as a WARN. The preferred capability is selected again at the next create once it recovers. A READY lower-ranked capability never pre-empts a warming preferred one; that would rebuild the session-long-fallback bug inside the runtime.
5. **One rebinding rule.** A live stream never moves, except on a terminal failure of its provider or a host binary drain (§4.1). A move is signalled by the result's `discontinuity` flag plus the task-state event; there is no per-stream provider-changed event. A preference change applies to the next create.
   **Live policy change (ADR-054 N3d, #2051).** A `PreferredProvider\<task>`, `AllowResearchOnly` or per-user `Enabled` write reaches the running core without a restart: the service re-reads the policy, hands the preferences to the core (`conv_core_set_policy` — the core keeps its own copy of the policy, so a re-read alone changes nothing) and re-applies every row's effective `Enabled` (HKLM with the HKCU override, the same resolution the headless `conv list` prints). The next stream / job create then selects under the new policy; `conv_gen` moves, so `conv status --watch` prints a line. The trigger is the DIAG call `system_request_component_reselect(conversion)` the CLI makes after `conv use|reset|enable|disable`, or the service's component-policy watch on the root for an edit made without the CLI (§4.1).
6. **`requiredOutputs`** is checked against the capability's describe-time declared outputs, which may be provisional. A create is refused (`FEATURE_UNSUPPORTED`) only when the capability is READY and still lacks the output; there is no silent re-selection.
7. **Visibility.** `displayxr-cli conv list` prints the full ordering with a `selected` column and the reason each row was kept or dropped.

### 3.5 POSIX — JSON manifests

Manifest name: `NNN-<id>-conversion-provider.json`, in the shared roots from `target_plugin_build_discovery_roots` (`target_plugin_loader.h:348`). The schema is the DP manifest plus optional `enabled`, `hosting`, `supersedes`, `packs[]` and a `settings` object. Per-user manifests shadow system ones by `id`.

The suffix is the router, so all three plug-in types share one directory:

| Loader | Rule |
|---|---|
| DP loader | skips `-conversion-provider.json`, beside its input skip (`target_plugin_loader.c:2511-2517`) |
| Input loader | already *requires* `-input-provider.json` (`target_input_plugin_loader.c:807-808`); a unit test pins that conversion manifests stay invisible to it |
| Conversion loader | requires `-conversion-provider.json` |
| Every loader | skips `<pack>.models.json` — a model pack (§ model store) is not a manifest. Packs ship beside the provider that lists them in `packs[]`, i.e. in this same directory; before this rule the DP loader read each one as a malformed DP manifest (`target_plugin_classify_manifest`, `target_component_loader.c`) |

Preferred provider: `DXR_CONV_PREFERRED_<TASK>` in the environment, or a `conversion-preferred.json` in the per-user dir (the `plugin-discovery.md` §3.3 pattern).

### 3.6 Android — `libdxrc<NNN>_<id>.so` and the `:conversion` process

Providers ship **inside the runtime APK** (`jniLibs/<ABI>/`, the ADR-038 shape) as `libdxrc<NNN>_<id>.so` — "c" for conversion. `libdxrp` stays the DP prefix (`target_plugin_loader.c:1856-1857`, sweep at `:2005`); the 7-byte prefixes differ, so neither loader can claim the other's file. The filename parse (`:1850-1880`) is factored to take the prefix. ProbeOrder stays in the filename, because `jniLibs` JSON is unreadable at load time (`plugin-discovery.md` §3.2).

The conversion core and every provider live in **one dedicated `:conversion` bound service** of the runtime APK (ADR-053 decision 5), an exported service guarded by a signature permission. Per-client `:dxrN` satellites and in-process apps connect to it (binder bootstrap, then a socketpair); `:dxrN` binds with importance inheritance so `:conversion` stays foreground while lifted content is visible. If `:conversion` is frozen or killed mid-inference, the call counts against the stall watchdog like any other: the stream keeps its last published result and the task reports `WARMING(restarting)`. Hosting the core in each `:dxrN` would load one model per app, the shape ADR-049 rejected. The app-side runtime never maps a `libdxrc` file. On Android the in-service / satellite distinction collapses: `:conversion` *is* the host. Providers in a separate vendor APK are deferred.

### 3.7 Combined display + conversion DLL

The ABI allows one DLL to export both `xrtPluginNegotiate` and `xrtConversionPluginNegotiate`. It is registered twice — one row under `DisplayProcessors\<id>`, one under `ConversionProviders\<id>`, same `Binary` (ADR-053 decision 1).

- `plugin-discovery.md` §4's "exactly one export" (`:611`) becomes **exactly one export per plug-in type implemented**, from {`xrtPluginNegotiate`, `xrtInputPluginNegotiate`, `xrtConversionPluginNegotiate`}, and nothing else. CI checks the exact parsed export set per DLL (`build-windows.yml:316-333`); POSIX gets `conversion_plugin_exports.version` plus a two-symbol variant.
- A satellite host loads **only** the conversion negotiate symbol of a combined DLL. It never calls the display negotiate and never probes the display half.
- In-service, both loaders map the same file; the OS refcounts it. The two vtables run on disjoint threads, so shared mutable state needs the DLL's own locking.
- **A combined DLL keeps the vendor files locked in the service** (the display half is mapped there), which is exactly what satellite hosting is meant to end. **Separate DLLs are the recommended shape**, and the vendor onboarding guide says so plainly.

### 3.8 Registration lifetime and the installer contract

[`plugin-discovery.md` §5](plugin-discovery.md#5-registration-lifetime-no-cascade-uninstall) applies verbatim: no cascade uninstall; vendor rows survive the runtime; a provider may be installed before the runtime; Restart Manager instead of kill; start through `explorer.exe`, never elevated. A provider's installer owns its `ConversionProviders\<id>` subkey (or its POSIX manifest) the way a display plug-in owns its `DisplayProcessors\<id>` subkey, and touches nothing else.

**Close the host, never the service.** A separate conversion-provider DLL is satellite-hosted (`Hosting` absent or `auto`, §3.1), so the only process that maps it is that provider's `displayxr-conversion-host.exe`; the service never loads a satellite provider's DLL. Restart Manager therefore lists the host, and closing the host costs no client session and no stream.

**The update sequence** (normative for an installer that replaces or removes the provider's files):

| Step | Installer | Runtime |
|---|---|---|
| 1 | Create and **own** the named mutex **`Global\DisplayXR-ConvMaintenance-<id>`** (`CreateMutexW(NULL, TRUE, …)`; `<id>` = the registry subkey and `iface->id`). Hold it until step 6 | The service polls the name about once a second. "Held" means it exists and cannot be acquired, **or** it exists and the service cannot open it (an elevated installer's default DACL). While it is held the supervisor spawns no host for `<id>`: no first spawn, no backoff respawn, no update respawn |
| 2 | `RmStartSession`, `RmRegisterResources` on the provider's files, `RmGetList` | `RmGetList` names `displayxr-conversion-host.exe` (one process per provider binary), never `displayxr-service.exe` — unless the files are also mapped by a display plug-in (§3.7) or by an in-service hosting (`Hosting=in-service`) |
| 3 | `RmShutdown` (graceful first; `RmForceShutdown` only if that fails) | The host's hidden top-level window takes `WM_QUERYENDSESSION` carrying `ENDSESSION_CLOSEAPP` (also `WM_ENDSESSION`, `WM_CLOSE`) and the host exits **20**, "closed for update"; a 3 s backstop exits 20 even if teardown stalls. Exit 20 is neither a crash nor a backoff step. The provider's tasks report `WARMING` with detail `updating`; streams keep their handles and their last result. A forced kill (any other exit code) takes the crash path instead, `WARMING(restarting)`, and that respawn also waits for the mutex |
| 4 | Replace or delete the files; write or delete the registry row with **`Version` last** (the "row complete" marker). POSIX: write the manifest atomically (temp + rename) | Nothing respawns on a timer. The host never calls `RegisterApplicationRestart`, so `RmRestart` restarts nothing for it |
| 5 | `RmEndSession`. **Do not start the host, and do not start or stop the service** | — |
| 6 | Release and close the mutex | With the mutex free, the supervisor respawns the host when there is demand (a bound task, a stream, a job or a prewarm), or when the row or manifest changes; the core re-describes, re-warms and re-opens every provider stream (one result flagged `discontinuity`). Tasks go `WARMING` → `READY` with no client action |

Code: the mutex poll and spawn gate are `conv_host_proxy.c` + `conv_supervisor.c` (`conv_supervisor_set_maintenance`); the session-end window and the 3 s backstop are `conv_host_os_win.c` (POSIX: `SIGTERM`, `conv_host_os_posix.c`); the exit codes are `conv_host_wire.h`; the `WARMING(updating)` report is `conv_core.c` / `conv_readiness.c`; the respawn on a row change is `service_conversion.c` → `conv_host_proxy_config_changed`.

- **Exit codes** the supervisor reads are in §4.4. Codes 10–14 are terminal (`FAILED` with a matching reason) until the row changes or a retry; 20 is the update path above; anything unlisted is a crash.
- **Uninstall** is the same sequence without a respawn: the row is gone, and the provider's tasks fall through to the next candidate at the next create (§3.4). A live stream on the removed provider moves once, flagged `discontinuity` (§3.4 rule 5).
- **Old installers that close the service** keep working at the old cost (every session is lost). Nothing in the runtime depends on the mutex being taken; it only prevents a respawn into a half-written install.
- **A combined display + conversion DLL** stays mapped in the service by its display half (§3.7), so for it the service is still what Restart Manager lists. Separate DLLs are the recommended shape for exactly this reason.
- **Lift that still runs through display-plug-in slots** (`dp-lift`, §4.5) stays in the service. Until a vendor ships its separate provider with `Supersedes = dp-lift`, that vendor's display-plug-in installer keeps closing the service.
- **POSIX** has no Restart Manager and no maintenance mutex (the supervisor reads it as always free). A package manager replaces files by rename under a running host, which keeps the old mapping until its next restart; a host asked to exit for an update (`SIGTERM`) exits 20 as on Windows.

**As built and measured (#2056).** What the runtime does at each step above, beyond the table:

- **Mutex probe.** Once a second while the host runs, and on every supervisor pass while the host is down with a respawn due, so a spawn never acts on a stale answer (the host's exit 20 follows the installer's `CreateMutexW` within milliseconds).
- **`RmGetList`** (measured on Windows with the shipping host and a provider DLL): one entry per host process, `ApplicationType = RmOtherWindow` (its hidden session-end window), `bRestartable = FALSE`; never the service process.
- **Graceful close.** `RmShutdown(0)` succeeds: the host logs `WM_QUERYENDSESSION … ENDSESSION_CLOSEAPP` and exits 20. It closes its control channel just before it exits; the supervisor gives that exit 2 s to land before it treats a closed channel as a fault, so the code stays 20 and is never overwritten by a kill's 30.
- **`WARMING(updating)`.** Only the provider's READY / WARMING rows move (detail `updating` in `displayxr-cli conv` and the status snapshot); INSTALLED, EVICTED and terminal rows are untouched. No crash record, no backoff, no crash-loop accounting. The frame in flight when the host exits is its one casualty.
- **No waits on a host that is down** (backoff, update, mutex held, killed, failed): the proxy refuses commands at once (measured well under 1 ms) instead of queueing them; the stream lane takes no frame from the mailbox meanwhile, so frames stay pending, latest-wins, exactly as while any task is WARMING, and are never counted failed. A refusal still counts as demand.
- **Respawn.** Owed when the closed host had an open stream, a call in flight or a warmed model; otherwise the next demand spawns it. A changed `Version` / `Binary` clears the update state, and the spawn still waits for the mutex. The fresh host is described, the core re-warms the `updating` rows, and every provider stream re-opens on it (an implicit reset).
- **Forced kill.** Measured: `RmShutdown(RmForceShutdown)` against a host that owns a non-pumping window returned `ERROR_SUCCESS` after 35.2 s, and the exit code Windows reported for the killed host was **0**. The core follows the supervisor's classification, never the raw code: an exit 0 the service did not ask for is abnormal, so the tasks go `WARMING(restarting)` with backoff and crash-loop accounting, and the respawn waits for the mutex.
- **POSIX:** `SIGTERM` to the host is the close-for-update request (exit 20, same 3 s backstop); the service never sends it (it closes the host over the channel, or `SIGKILL`s it). With no mutex, the respawn follows demand alone, which is why the manifest and the binary are replaced by rename.

Tests: `tests_conv_proxy` "exit 20 = WARMING(updating); no respawn while the maintenance mutex is held" (every OS, scripted host; includes the sub-millisecond refusal), "RmForceShutdown of a host that cannot answer is the crash path" (Windows), "update flow on the real host" (the shipping host + the sim provider DLL under a real Restart Manager session on Windows, `SIGTERM` on POSIX); `tests_conv_readiness` "host closed for update".

**Does the service ever map the files? (roadmap §11.2, answered 2026-10-13.)** No *weaving* path does: per-window display processors never call the lift slots; the only caller is the lift thread's dedicated lift DP (`d3d11_lift.cpp`, created through `create_dp_d3d11_lift` or the NULL-window factory), which is the `dp-lift` adapter's backend (§4.5). But that adapter **does** load the vendor module into the service: the shipping display plug-in loads its conversion module (and the module's inference runtime) on the first `lift_get_caps`, into process-global state, and never unloads it. So:

- **While `dp-lift` serves** — once anything has activated it in this service process (a warm, a stream create, a v3 stream through the adapter) — `RmGetList` on the module's files **lists `displayxr-service.exe`**, and keeps listing it until the service restarts, even after a provider with `Supersedes = dp-lift` is installed.
- **Once a provider with `Supersedes = dp-lift` is registered when the service starts**, the adapter is hidden, never selected, never warmed, and the module is never mapped into the service: only the host is listed. (#2056 removed the last side door: a v3 `lift_get_properties` used to activate the legacy lift DP to look for the GAUSSIANS mode even when `dp-lift` was hidden; it now only peeks at an already-activated module.)
- **A combined display + conversion DLL** (§3.7) is always listed under the service, because its display half is mapped there.

**What a vendor installer must do.** Hold the mutex, register *every* file it will replace (the provider DLL, the conversion module, the module's runtime), and act on what `RmGetList` reports. If only `displayxr-conversion-host.exe` is listed, the update costs nothing (above). If **`displayxr-service.exe` is listed** — the first install of a separate provider on a box where `dp-lift` already loaded the module, an older plug-in, or a combined DLL — follow the service's own Restart Manager contract ([`service-architecture.md` §1.5](../../architecture/service-architecture.md#15-restart-manager--the-contract-for-third-party-installers-windows)): `RmShutdown(RmForceShutdown)` (the module's non-pumping window makes `0` fail), replace, `RmRestart` (the service comes back non-elevated with `--rm-restart`), at the old cost (every session `XR_ERROR_INSTANCE_LOST`). The same session handles both cases; the installer never `taskkill`s either process.

The runtime packages **ship and register `sim-conversion`** (ADR-054 §2 clause 3 — every class ships its reference): the Windows installer writes the `sim-conversion` row (ProbeOrder 250, `Packs` = its fake pack, Version last) and allowlists the DLL, the `.deb` and tarball ship `DisplayXR-SimConversion.so` with `250-sim-conversion-conversion-provider.json`, the macOS `.pkg` likewise. It stays inert until `ENABLE` = 1 (FALLBACK, setting-gated, §6.1), so a user install gains a listed-but-disabled row and nothing else; `register_dev_plugin.bat conv sim` repoints that row at a dev build.

---

### 3.9 Component settings — the one options contract (ADR-054 decision 7, N3c)

Every component class — display processors (plugin-discovery.md §2.3), input
providers (input-provider-discovery.md § *Component settings*), conversion
providers — carries its per-component settings the same way, and a provider
reads them **only through its host iface**, never from its own environment
(§2.4; `xrt_conversion_plugin_host_iface::get_setting(key, buf, cap)` keeps
its keyless v1 shape — the host binds the provider — and the shared store
behind it is `xrt/xrt_component_settings.h`'s). This section is the
conversion wording of the contract the §3.1 `Settings\*` row and §3.2
`DXR_CONV_<ID>_<KEY>` already described; what N3c adds is the **per-user tier**
and the verbs.

| Tier | Where | Wins over |
|---|---|---|
| 1 — environment | `DXR_CONV_<ID>_<KEY>` in the **hosting** process's environment (the service's, or the satellite host's; §3.2); `<ID>` = the id upper-cased, `-` → `_` (`DXR_CONV_SIM_CONVERSION_ENABLE`). Android: the `debug.xrt.` system property of the same name. A set-but-empty variable is unset. | everything — the `u_setting` rule |
| 2 — user | Windows: `HKCU\Software\DisplayXR\ConversionProviders\<id>\Settings\<KEY>` (`REG_SZ` or `REG_DWORD`). POSIX: `<id>.settings.json` in the per-user manifest dir (a flat object `{"KEY": "value"}`). | tier 3 — unless HKLM `ConversionProviders\Policy\Lock` = 1 (§3.1), which hides the user tier (never the environment) |
| 3 — machine / manifest | Windows: `HKLM\…\ConversionProviders\<id>\Settings\<KEY>` (64-bit view, §3.1). POSIX: the manifest's `settings` object (§3.5). | the provider's compiled default |

Values are strings; the provider parses. Keys are `[A-Za-z0-9_]`, not
digit-first, matched case-insensitively and conventionally upper-case.
**Only `Settings\*` is ever read from HKCU / the per-user file**: a per-user
store can change what a registered provider is *told*, never which binary
loads. `u_setting_is_managed`'s allow-list is a different store this contract
never touches.

**Verbs** — one spelling across `dp` / `input` / `conv`
(`cli_component_options.c`; §7.1):

| Verb | What |
|---|---|
| `displayxr-cli conv options <id> [--json]` | Every key the provider declares (`xrt_conversion_plugin_iface::describe_settings`, appended past `destroy`, `XRT_CONVERSION_PLUGIN_HAS_DESCRIBE_SETTINGS`; or the POSIX manifest's `settings_schema` array `[{"key","type","default","help"}, …]`) unioned with every key the stores hold: key, type, default, effective value and its source (`environment` / `user` / `machine` / `manifest` / `absent`). Loads + negotiates the registered binary (no probe, no `describe_cap`) to ask it; the built-in `dp-lift` adapter has no row and no settings. |
| `conv set <id> <KEY> <value> [--machine]` | Writes the per-user store (no admin) or, with `--machine`, the HKLM 64-bit view (admin). Refused on POSIX with `--machine` — the manifest's `settings` object is the machine store there and is edited by hand. An unregistered id or an invalid key is refused with nothing written. After the write the CLI reads the key back and prints *Effective now:* the value the provider will actually see and why — including when the environment or `Policy\Lock` overrides what was just written. |
| `conv unset <id> <KEY> [--machine]` | Removes it from that store; absent is success. |

**When it applies.** The running service re-describes on its registry watch
(§4.1), re-reading tiers 1 and 2 live through its per-provider host, so a
`conv set` reaches the next re-describe / next stream or job create; a live
stream never moves. (Display processors and input providers read at init
instead: next session / service start.)

**Reference implementation.** `sim-conversion`'s `ENABLE` (§6.1): `conv set
sim-conversion ENABLE 1` makes `conv list` negotiate and select it; with
`DXR_CONV_SIM_CONVERSION_ENABLE=0` in the service's environment `conv
options` reports `source: environment` and the provider declines again.

## 4. Lifetime and hosting

### 4.1 Loading policy

- **Every registered provider is negotiated, probed and described** once per hosting process (the service, or the provider's own host). Descriptions fill the service's capability table — one row per provider × capability — **before any model load**, so a create is validated against what a provider *can* do while it is still cold.
- **Persisted describe cache.** The service stores the last describe result per (binary path, size, mtime) and reloads it at start, so the table is never empty after the first run and hosts **spawn lazily**: on first demand, on a cache miss, or on a re-describe trigger.
- **In-process apps never load conversion providers.** They reach the service over the ADR-049 lift link, generalised to every graphics API. One model per machine. This is a deliberate asymmetry with display processors and input providers. With no service, in-process sessions report the task absent; the headless CLI may host a provider in-process for `conv probe` and selftest only.
- **Re-describe triggers:** `notify_changed(0)`; a `get_generation` change; a change under the HKLM or HKCU root — delivered by the service's **one** component-policy watch thread shared with the display and input classes (`service_component_watch.c`: `RegNotifyChangeKeyValue` on every class root, debounced 500 ms; a 2 s mtime poll of the per-user manifest dir on POSIX — not inotify / FSEvents, see the file comment); the DIAG reselect call; a model download completing; a 30 s safety poll. A root change also re-reads the policy and applies it live (§3.4 rule 5).
- **No live swap** (ADR-045). A capability with live streams is never replaced. A new preference applies to the next stream create. A changed binary on a satellite is drained to a fresh host at a frame boundary (the one move §3.4 rule 5 allows); in-service it is adopted at the next service restart (handles are leaked by design) and logged once.

### 4.2 Prewarm and keep-alive

- **Trigger:** a **`conv_hint_interest(tasks)`** message the client sends when an instance enables `XR_DXR_lift` — IPC sessions at `xrCreateInstance`, in-process sessions when the lift link opens. The `PrewarmOn` policy (§3.1) decides what the service does with it: `interest` prewarms on the hint (default on mains power / desktop), `first-create` waits for the first stream or job create (default on battery), `start` prewarms at service start (kiosk opt-in). Meanwhile readiness is honest: apps see ACTIVATING, never UNAVAILABLE.
- **What:** CTRL calls `warm` on the *selected* capability of each `Prewarm\<task>` (default `views-mono`, `depth-video`).
- **Keep-alive:** a prewarm-task model is never evicted on idle (`KeepAliveSec=0`); a job-only model evicts after 600 s idle; nothing evicts while bound. There is no VRAM-pressure eviction in v1. On Linux the socket-activated service exits after its last client (`service-architecture.md` §1.1), which bounds keep-alive: the first client after that cold-warms.
- **Backoff:** `FAILED` + retryable backs off from 15 s, doubling to 10 min. A network failure blocks nothing and shows its reason in the tray.

### 4.3 Readiness

Readiness is per (provider, capability). The runtime keeps the FSM; the provider answers `get_state` from memory (ADR-053 decision 6).

| State | Value | Meaning | Selection |
|---|---|---|---|
| `UNKNOWN` | 0 | not yet described | — |
| `READY` | 1 | resident; converts now | selectable |
| `WARMING` | 2 | load / licence / compile / restart; progress valid | **waited for** |
| `ABSENT` | 3 | dependency or model not installed, or its download forbidden by policy | falls through |
| `INCOMPATIBLE` | 4 | installed but unusable (version / OS / GPU / driver) | falls through |
| `UNLICENSED` | 5 | | falls through |
| `FAILED` | 6 | `failure_reason` says which; `retryable` says whether a later `warm` may succeed | falls through for new creates |
| `DISABLED` | 7 | configuration | falls through |
| `INSTALLED` | 8 | present and cold; `warm` makes it READY | waited for |
| `DOWNLOADING` | 9 | model bytes in flight (host-side); progress valid | waited for |
| `EVICTED` | 10 | was READY; unloaded on idle | waited for |

**Non-terminal** below means one of the four waited-for states.

**Transitions.**

| From | Event | To |
|---|---|---|
| — | described, task declared | `INSTALLED` |
| — | binary missing / ABI rejected | `ABSENT` (reason) |
| `INSTALLED` / `EVICTED` | warm requested (prewarm, create, `conv warm`), model blobs missing | `DOWNLOADING` |
| `INSTALLED` / `EVICTED` | download forbidden by policy | `ABSENT(policy)` |
| `INSTALLED` / `EVICTED` / download done | model present | `WARMING` |
| `INSTALLED` / `EVICTED` | model estimate exceeds the adapter's memory | `FAILED(RESOURCES)`, not retried until the budget changes |
| `WARMING` | provider ready | `READY` |
| `READY` | idle past keep-alive (§4.2) | `EVICTED` — never while a stream or job is bound |
| `READY` / `WARMING` | host crash or watchdog kill | `WARMING` (`restarting`, attempt n); streams survive |
| any | crash-loop cutoff (5 abnormal exits in 10 min) | `FAILED(crash_loop)` |
| `WARMING` | licence / platform / network failure | `FAILED(reason)` |
| `WARMING` | the provider's inference runtime is present but older than the provider's floor | `FAILED(VERSION)`, never `FAILED(MODEL)`: the dashboard must say "too old", not "missing" |
| any | `conv disable` / `Enabled=0` | `DISABLED` |

**Retry triggers.** A registry change; `displayxr-cli conv warm`; a create for a task whose only candidates failed more than 30 s ago; the backoff timer for network-class failures; 30 min after a crash-loop cutoff.

**No latch.** The `activated` flag that leaves lift unavailable until a service restart (`d3d11_lift.cpp:847-858`) is gone. State is always the provider's current answer. "Nothing available" is a state with a reason and a retry trigger, never a terminal latch.

**Consumer rule.** A non-terminal state means wait or poll; the runtime never answers "unavailable" for a state that will become READY. Every transition bumps a generation, pushes a state-change event to bound connections, and logs one WARN.

**Spec-3 mapping** — the only coarse mapping; the header carries no coarse-state helper. `XrLiftPropertiesDXR` reports:

| Spec-3 state | When |
|---|---|
| `READY` | any advertised mode's backing task is READY |
| `ACTIVATING` | otherwise, while discovery has not settled (some registered provider neither described nor terminal) **or** any backing task is non-terminal — even before any mode bit is set |
| `UNAVAILABLE` | otherwise |

Golden test: properties queried on a cold boot before the first describe return ACTIVATING.

### 4.4 In-service and satellite hosting

Both hostings implement the same service-side **host interface** (describe, warm / evict, stream open / reset / close, submit, job submit / cancel, register image, heartbeat, shutdown; callbacks for state, completion, progress and log). The provider ABI cannot tell them apart.

**In-service (P1).** The provider DLL is loaded in the service with one exec thread per provider, a single-threaded executor that behaves exactly like a satellite. It still receives shared handles, opened once per slot lifetime. Used by the legacy adapter (always), the sim provider in P1, and any row with `Hosting=in-service`. Cost: a provider crash, `exit()` or heap corruption takes the service and every app down; a hung provider can only be abandoned (`FAILED(hung)`) until the service restarts.

**Satellite (P2).** `displayxr-conversion-host.exe --provider <id>`, one per provider DLL, spawned lazily by the service's supervisor in a job object (`KILL_ON_JOB_CLOSE`; parent-death signal on POSIX), at medium IL — it exits if elevated. The host writes its own minidumps. A host with no bound task and no prewarm exits after **10 min** idle. P2's test vehicle is the sim provider's D3D11 NT-handle + fence path plus the CI kill tests.

- **Channels.** Control over a named pipe / AF_UNIX socket ACL'd to the service user; data over a pair of shm rings per lane with one wake signal each way. Ring records carry image indices, never handles.
- **Handles.** The service holds the only cross-process right: it **pulls** host-allocated output handles with `DuplicateHandle(host → self)` through its own handle on the host process. The host never holds a handle to the service process. POSIX: `SCM_RIGHTS` both ways.
- **GPU sync.** Shared fence values; keyed mutexes are never used service ↔ host. **The service never enqueues a GPU wait on a host-signalled fence on any compositor context.** It observes completion from the CPU on the dispatcher thread (`SetEventOnCompletion` or a fence poll, with a timeout) before it consumes a host output; the client-facing export path follows the same rule. CI case: kill the host between submit and signal; the compositor frame rate is unaffected.
- **Failure.** A crash fails in-flight work, reports `WARMING(restarting)`, keeps stream handles and the last published result, and respawns with backoff (0.5 s doubling to 30 s). Exit 20 respawns only per §3.8.
- **Exit codes** (`conv_host_wire.h`; an own namespace, unrelated to selftest's exit 13):

  | Code | Meaning | Supervisor |
  |---|---|---|
  | 0 | orderly: a requested shutdown, or the 10 min idle exit | not a crash |
  | 2 | bad command line | `FAILED`, not retryable |
  | 10 | ABI reject; also a refused hello (git-tag or wire-family mismatch) | `FAILED` until the row changes or a retry |
  | 11 | negotiate or describe failed | same |
  | 12 | licence (reserved: an unlicensed capability is reported as state, not as an exit) | same |
  | 13 | no usable device on `--adapter-luid` | same |
  | 14 | refused to run elevated | same |
  | 15 | the inherited control channel is unusable | crash accounting |
  | 20 | closed for update (§3.8) | no backoff, no crash accounting; respawn only per §3.8 |
  | 30 | killed by the supervisor (call watchdog, hello timeout, heartbeat silence) | crash accounting |
  | other | crash; the host wrote a minidump | backoff, crash-loop cutoff |
- **Logs:** `DisplayXR_conversion-host_<id>.<pid>_<ts>.log`; WARN+ lines are forwarded into the service log, prefixed `[conv:<id>]`.

### 4.5 The legacy DP-lift adapter (`dp-lift`)

A runtime-internal provider with no DLL and no negotiate: a synthetic row at **ProbeOrder 150**. It wraps whatever the lift-only factory resolves to today — the active display plug-in's `create_dp_d3d11_lift` (`xrt_plugin.h:1261`, plumbed at `target_instance.c:136-143`), else the ordinary D3D11 factory with a NULL window (`comp_d3d11_service.cpp:35798-35799`, `d3d11_lift.cpp:837-839`) — and its D3D11 lift slots (BASE+25..29, +33).

- **Hosting: in-service, always.** It needs the display plug-in, which never leaves the service (ADR-035 D4), and the lift-only factory assumes the service's context. It ignores `Hosting`.
- **Hidden** when a registered provider declares `Supersedes = dp-lift`, so a vendor's new provider and its old DP slots are never listed twice.
- **Re-resolved on every retry** from the plug-in that registered it, never from the per-window DP factory; a display plug-in swap (`refresh_display_processors_cb`, `target_instance.c:565`) re-describes it.

**Static describe.** If the active display plug-in exposes the lift-only factory, or `create_dp_d3d11` with `XRT_DP_D3D11_HAS_LIFT`, the adapter declares `DEPTH_MONO`, `DEPTH_VIDEO` and `VIEWS_MONO` (min 2, max 8) as **INSTALLED** with provisional outputs, without calling the factory. It refines the declaration after the first `lift_get_caps`, and reports `ABSENT` only after the factory refuses or the module reports ABSENT. "Not yet asked" is never terminal.

**Capability refinement** from `lift_get_caps` (polled ≤ 1 Hz while not READY):

| Legacy mode bit (`xrt_dp_lift.h:42-45`) | Capability (`cap_id`) | Mapping |
|---|---|---|
| `DEPTH` | `DEPTH_MONO` (1) + `DEPTH_VIDEO` (2), stream | `content_hint` PHOTO / VIDEO. RELATIVE → `AFFINE`, METRIC → `METRIC`. `TEMPORAL` on cap 2 only |
| `SBS`, `NVIEW` | one `VIEWS_MONO` (3), min 2, max = `caps.max_views` (2 when only `SBS`) | `outputs = VIEWS` (+ `DEPTH` iff `AUX_DEPTH`); aux kind from `aux_depth_semantics` + encoding. `max_views` is now **enforced** |
| `GAUSSIANS` | none | no shipping plug-in implements the blob slot |

All three capabilities: `licence_class = UNKNOWN`, `commercial_ok = 0`, **exempt from the `AllowResearchOnly` gate** (grandfathered: the user installed that module before the gate existed; the module's own licence string from the plug-in's detail text is shown in the dashboard, and a shipping module reads "internal use only", so COMMERCIAL would be a lie); `backend` and `model_version` from the caps; `max_streams` from the caps (0 → 4); `import_memory_kinds = CPU | NT_HANDLE`; `EXPLICIT_VIEWPOINTS` + `OFFAXIS_CAMERA` only when the plug-in's `struct_size` covers the ADR-048 policy fields. They share one module state and exclude nothing: depth and views streams run at once, as today.

**State is a plain copy:** `caps.module_status` when non-zero (values asserted equal); else READY → READY, ACTIVATING → WARMING, UNAVAILABLE → ABSENT. Terminal states are re-polled at 0.1 Hz. Depth metadata maps onto `xrt_conv_depth_info` field by field with `intrinsics_source = ASSUMED`; `range_*` stays 0 because the legacy contract cannot express it.

**Deprecation path.**

| Step | When | What |
|---|---|---|
| Freeze | P1 | Nothing is ever appended to `xrt_dp_lift.h`, the D3D11 lift slots or `create_dp_d3d11_lift`. A header banner says so; the header stays in `downstream-pins.json` `feature_surface` so a stray edit still trips the repin gate. Frozen as dead ABI under `XRT_PLUGIN_API_VERSION` 5. |
| Parallel | P3 | The vendor ships a separate conversion provider with `Supersedes = dp-lift`, satellite-hosted; it may keep filling the DP slots for older runtimes (ABI 5 unaffected). |
| Deprecate | first runtime release after that | one WARN per service start while the adapter is active: "display plug-in lift slots are deprecated — ship a conversion provider". |
| Remove | the next `XRT_PLUGIN_API_VERSION` major (**v6**), whenever it happens for its own reasons | the slots leave with the v6 layout; `xrt_dp_lift.h` is deleted. No lift-only major bump: dead appended slots cost nothing (ADR-020 §2). |

---

## 5. Model packs (schema 2)

Schema 2 replaces the browser's `models.json` schema 1. The browser's store is deleted in P4, so schema 2 carries no schema-1 compatibility block.

### 5.1 Ownership and location (ADR-053 decision 7)

| Thing | Owner | Where / rule |
|---|---|---|
| Store root (blobs + stamps) | **the runtime** (service / host); the runtime installer creates it | Resolved in order: `HKLM\Software\DisplayXR\Models\Path`, then `HKLM\Software\DisplayXR\Browser\ModelsPath` (so installed blobs are not re-downloaded), then `%ProgramData%\DisplayXR\Models`. The browser installer writes both keys for as long as it ships models. The browser uninstaller must never delete the store once the `Models` key exists (it deletes `blobs\` today); the runtime treats a wiped store as `DOWNLOADING`, never as a latched failure |
| Layout, write protocol, read check, ACL | unchanged from the browser's model-distribution contract | `blobs/<sha256>`; `<sha256>.<nonce>.part`; `.ok` stamp written **last**; read check = stamp + size; ACL `*S-1-5-32-545:(OI)(CI)(RX)` + `(CI)(WD,AD)` by SID |
| Load check | **the host** | verifies sha256 on the first load of a blob per process (inside warm-up), and rejects a blob whose stamp owner SID is not the expected writer |
| Pack manifests | **each provider ships its own** `<pack>.models.json` | registered via `Packs` (registry) / `packs[]` (POSIX manifest) |
| Download | **the host**, on `model_request` | Range resume; follow redirects; 200-instead-of-206 restarts; verify size then sha256; rename; stamp last. Shows as `DOWNLOADING`. Concurrent requests for one sha256 share one download. Asks before > 100 MB on metered networks; a refused or policy-forbidden download is `ABSENT(policy)` |
| Pre-fetch | the installer, as a SYSTEM writer | only `prefetch: true` commercial blobs |
| Variant choice | **the provider** | picks a variant for its backend and device, then asks by ref |
| GC | manual | never automatic; `displayxr-cli conv models prune` deletes blobs no registered pack references |

### 5.2 Schema

```jsonc
{
  "schema": 2,
  "pack": "dxr-open-depth",                 // unique; == the registered pack id
  "generated": "2026-10-12",
  "provider": "open-conversion",            // consuming provider id (informational)
  "blobBaseUrl": "https://github.com/DisplayXR/displayxr-models/releases/download/blobs",
  "models": [{
    "name": "depth-large",                  // unique in the pack
    "version": "1.0.0",                     // bump on any byte change; never re-point name@version
    "task": "depth-mono",                   // depth-mono | depth-video | disparity-stereo | views-mono |
                                            //   views-from-depth | splats-mono | splats-stereo | aux:<x>
    "pipeline_tag": "depth-estimation",     // optional
    "io": {
      "input":  { "color_space": "srgb", "range": [0, 1], "resolution": "dynamic-multiple-of-14",
                  "native_long_edge": 770, "temporal_window": 1, "intrinsics_in": "optional" },
      "output": { "depth_kind": "metric", "measure": "planar", "intrinsics_out": "estimated",
                  "confidence": false, "coords": "opencv", "splat_format": null }
    },
    "license": {
      "spdx": "MIT", "link": "https://…", "commercial": true, "attribution": "…",
      "dependencies": [{ "name": "backbone", "spdx": "Apache-2.0", "commercial": true }]
    },
    "provenance": { "source_url": "https://…", "base_model": "…", "converted_by": "…" },
    "prefetch": false,                      // in the installer's default pre-fetch set
    "variants": [{
      "id": "gpu-fp16",
      "backend": "<inference-runtime>",     // an inference runtime id, or vendor:<x>
      "ep": ["<execution-provider>"],       // acceptable execution providers, preference order
      "precision": "fp16",
      "os": ["windows>=10.0.19041"],
      "min_vram_mb": 2048, "min_ram_mb": 3000, "min_driver": null,
      "est_warmup_ms": 2500, "est_latency_ms": { "770": 1300 },
      "files": [{ "role": "model", "format": "onnx", "sha256": "<64 hex>", "size": 715220757 }]
    }]
  }],
  "defaults": { "depth-mono": { "low": "depth-small", "medium": "depth-medium", "high": "depth-large" } }
}
```

### 5.3 Rules

- **Commercial is an AND, over the runtime too.** `license.commercial` is the AND over the model, every `dependencies[]` entry and the inference runtime the provider loads. Any `false` or missing value makes the capability `RESEARCH_ONLY`, selectable only with `AllowResearchOnly`.
- **Trust is the hash, not the stamp.** The host re-hashes a blob on first load per process (cached by file id + mtime + size) before handing it to the inference runtime. The stamp + size check alone is not trusted: the store's inherited ACL lets any local user plant a same-size blob and its stamp.
- **Signing.** A signed installer MUST NOT pre-fetch a non-commercial blob; the open-provider repo's CI asserts it.
- **Blob identity.** `sha256` + `size` identify a blob; variants may share blobs.
- **URL.** `<blobBaseUrl>/<sha256>.<format>`.
- **Validation.** `scripts/check_model_pack.py` (open-provider repo) checks the schema and the licence AND; `--verify-store` checks a store against its packs.

### 5.4 The store as built (P4, #2108)

`src/xrt/conversion/conv_model_store.{c,h}` (target `displayxr::conversion_store`) is the one
implementation; both hosts, the CLI and the installer pre-fetch run it.

**Root.** `HKLM\Software\DisplayXR\Models\Path` → `HKLM\Software\DisplayXR\Browser\ModelsPath`
→ `%ProgramData%\DisplayXR\Models` (64-bit registry view); POSIX `$XDG_DATA_HOME/displayxr/models`
(default `~/.local/share`) when writable, else the packaged root `/var/lib/displayxr/models`. `DXR_CONV_MODELS_ROOT`, in the environment of the process that
owns the store, overrides all of them (tests and dev boxes only; the service reads its own
environment, not a client's). Layout:

```
<root>/displayxr-runtime.owner        the runtime claims the root (installer, or the store when it creates it)
<root>/packs/<pack>.models.json       a pack this store provisioned: an absent default set is then "evicted"
<root>/blobs/<sha256>                 bytes; <sha256>.ok stamp written last; <sha256>.<nonce>.part in flight
```

**Packs.** Each registered provider's `Packs` (registry `REG_MULTI_SZ`) / `packs[]` (manifest),
a path relative to the provider binary's directory unless absolute. A relative `blobBaseUrl`
resolves against the manifest's directory (`file://`); `https://` downloads use WinHTTP on
Windows (redirects followed); POSIX has no network downloader yet (P5), so a missing blob there
is `FAILED(NETWORK)` unless its pack is served from `file://`. The host ops carry no provider
context, so a host's store serves the union of the packs it was configured with: the service
every registered provider's, a satellite its own provider's.

**`model_resolve(ref)`** never hashes and never touches the network (a stat of the blob and its
stamp). `ref` is `<pack>/<model>[/<variant>]` (first variant when omitted) or a bare sha256.
`xrt_conv_model_location::state`:

| state | meaning | provider does |
|---|---|---|
| `READY` | every file present, stamped, and hashed **by this process** (cache key: file id + mtime + size); `path` = the variant's `model` file | load `path` |
| `INSTALLED` | on disk and stamped, not yet verified by this process | `model_request` (warm-up verifies it) |
| `WARMING` | the store's worker is hashing it | wait |
| `DOWNLOADING` | in flight; `progress` = bytes / total over the model's files | report `DOWNLOADING` with that progress |
| `ABSENT` | not in the store; `failure_reason = XRT_CONV_FAIL_POLICY` when a request was refused by policy | report `ABSENT` with that reason (the core shows `ABSENT(policy)`) |
| `FAILED` | `NETWORK` (retryable: a new request retries) or `MODEL` (bytes failed size / hash) | report `FAILED(reason)` |

**`model_request(ref)`** queues verification or download of every file that is not `READY` on
the store's one worker thread; concurrent requests for one sha256 share one job. It returns
false for an unknown ref or a policy refusal.

**Policy.** `AllowDownloads` (DWORD under `ConversionProviders`, HKCU over HKLM unless
`Policy\Lock`; default 1; `DXR_CONV_ALLOW_DOWNLOADS=0` in the hosting process forces it off): a
missing blob with downloads off is `ABSENT(POLICY)`. A model whose `license.commercial` AND is
false is fetched **and handed out** only with `AllowResearchOnly`; withdrawing the opt-in makes a
present blob `ABSENT(POLICY)` again. `conv models fetch --installer` never fetches one, whatever
the opt-in says.

**Tamper rejection.** A stamped blob of the right size passes the browser's read check; the store
does not trust it. The first `model_request` per process re-hashes it; a mismatch deletes the blob
and its stamp and fetches it again (`FAILED(MODEL)` when downloads are off). A blob replaced after
this process verified it changes its cache key and drops back to `INSTALLED`. Downloaded bytes are
hashed while they stream; a part that fails size or hash is deleted and never renamed.

**ACL (Windows).** A root the store (or `conv models init`) creates gets a protected DACL:
SYSTEM and Administrators full; `BUILTIN\Users` `(OI)(CI)` read + execute and `(CI)` create
files / create folders (the service and the browser run as the logged-in user); `CREATOR OWNER`
full on what a writer creates (so a writer can rename and clean up its own parts); `ALL
APPLICATION PACKAGES` `(OI)(CI)` read (an AppContainer reader). Users therefore cannot overwrite,
append to or delete a blob another writer made. An existing root's ACL is left alone.
`tests_conv_store` checks the effective rights of each SID.

**Readiness.** A provider that learns at warm-up that its model is missing reports `DOWNLOADING`;
the core moves the row `WARMING → DOWNLOADING` and on to `WARMING` / `READY` as the provider
reports. A provider `ABSENT` with `XRT_CONV_FAIL_POLICY` becomes `ABSENT(policy)` (a registry or
policy change lifts it). Progress bumps the status generation at most at 2 Hz and only when it
moved 5 % (or reached 100 %), so `status --watch` and the dashboard stay quiet during a download.

**Snapshot.** The CLI adds `conv_model_store` (`root`, `source`) and `conv_packs[]` to every
snapshot it prints, read from disk (no IPC): `display-dashboard.md` §3.3.

**CLI.** `conv models list [--json]` (per pack and model: on disk, stamped, in flight),
`verify` (re-hash every referenced blob; a tampered one is deleted; exit 1 when any is),
`prune` (delete blobs no registered pack references), `fetch <pack-url|path> [--prefetch]
[--model <name>] [--installer]` (the write protocol on the calling thread; `--installer` =
nonce `installer`, stamped blobs skipped without hashing, non-commercial models never),
`init [--root <dir>]` (create the root with its ACL and marker). `fetch --prefetch` with no URL
pre-fetches every registered pack's `prefetch` models.

---

## 6. Reference provider and stage

### 6.1 `DisplayXR-SimConversion` (hardware-free)

`src/xrt/drivers/sim_conversion/`, shipped as `DisplayXR-SimConversion.{dll,so,dylib}` and `libdxrc250_sim_conversion.so`, id `sim-conversion`. ProbeOrder 250 with `XRT_CONV_PROVIDER_FLAG_FALLBACK`: never selected over a non-fallback capability. `probe` declines unless setting `ENABLE` = 1 (`Settings\ENABLE`, or `DXR_CONV_SIM_CONVERSION_ENABLE=1`), the sim-input pattern. It ports today's DP-side fake (`sim_display_lift_d3d11.cpp`); `SIM_DISPLAY_FAKE_LIFT` stays an alias for one release, then the fake is deleted. It is P2's satellite test vehicle.

**CPU compute.** Every capability imports `XRT_CONV_MEM_CPU` and computes on the CPU, so `selftest` and the CI `Runtime` job run on a GPU-less runner or WARP with no shader compiler. `DETERMINISTIC` is set, enabling golden-byte tests. A D3D11 import path exists only to exercise NT-handle + fence sync on WARP, in-service and across the satellite boundary.

| Task (`cap_id`) | Lane | Fake output |
|---|---|---|
| `DEPTH_MONO` (1) | stream + job | R32F vertical ramp, `AFFINE`, 0 at the top; `range_min/max` filled |
| `DEPTH_VIDEO` (2) | stream | ramp × (1 + 0.1·sin(frame_id)), `TEMPORAL`; `FRAME_RESET` snaps the phase to 0 (observable) |
| `DISPARITY_STEREO` (3) | stream + job | constant `infinity_shift_px + 4` px, `STEREO_DISPARITY_PX`; a METRIC job with `baseline_m == 0` is refused (never-defaulted rule) |
| `VIEWS_MONO` (4) | stream | N ≤ 8 shifted copies, ROW or ARRAY; shift from `viewpoints_xyz.x / rect_width_m`; aux depth = metric ramp 1→2 m; `suggested_convergence = 0.5` |
| `VIEWS_FROM_DEPTH` (5) | stream | depth-scaled shift; exercises runtime pipeline composition |
| `SPLATS_MONO` (6) | job | PLY or a 3-splat SPZ; `metric = 0`, intrinsics `ASSUMED` |
| `SPLATS_STEREO` (7) | job | as (6); `metric = 1` only when `baseline_m > 0` |

**Knobs** (setting keys read through `get_setting`, so they reach the service and the host; env form `DXR_CONV_SIM_CONVERSION_<KEY>`):

| Key | Exercises |
|---|---|
| `WARM_MS` (0) | WARMING with progress 0→1 |
| `DOWNLOAD_MS` | DOWNLOADING: `warm` first calls `model_request("sim/fake")` against a local fake pack (1 KiB blob, `file://` base URL) — the host's store write protocol without network |
| `STATE_<cap>` = `absent` \| `incompatible` \| `unlicensed` \| `failed:<reason>` \| `disabled` \| `installed` | the scripted state machine: every terminal and cold state, and fall-through in selection |
| `FLAP_MS` | READY ↔ EVICTED square wave with `notify_changed`: events and the no-live-swap rule |
| `LATENCY_MS` (8) | per-convert delay |
| `JOB_MS` (2000), `JOB_FAIL_AT` | job progress, cancel (polled every 50 ms), mid-job failure |
| `HANG_MS` | the stall watchdog naming provider + slot; host restart |
| `LICENCE=research` | the `AllowResearchOnly` gate |
| `DEVICE_LOST_EVERY=N` | `FAILED(DEVICE_LOST)` → re-warm |

It writes only within `struct_size`; `tests/tests_conversion_abi.cpp` feeds it every truncation of every out-struct.

### 6.2 The reference view-synthesis stage

A runtime **pipeline stage**, never a provider row and never selectable. It turns image + depth into N views so every depth capability yields views (a composed pipeline, §3.4), and it is the A/B baseline; it does not compete with a provider's own `VIEWS_MONO`. Apps see it only through the task's `COMPOSED` output bit. **Acceptance bar:** match or beat the browser's current in-page view synthesis on the lift panel corpus (real media), including temporal stability; its hole-fill is ported from there, not written anew.

---

## 7. Diagnostics

### 7.1 `displayxr-cli conv`

Verbs (ADR-053); `lift caps|probe` stay as aliases.

| Verb | Path | What |
|---|---|---|
| `conv list [--json]` | DIAG IPC; headless describe as fallback | providers × capabilities, selection order, `selected`, reason per row |
| `conv status [--watch]` | DIAG | readiness, streams, jobs |
| `conv use <task> <id>[/<cap>] [--machine]` / `conv reset <task>\|all` | registry (HKCU; HKLM with `--machine`) | per-task preference, like `dp use --screen` |
| `conv warm <task>\|<id>/<cap> [--wait]` / `conv evict <task>` | DIAG → service | explicit prewarm / unload; `evict` is accepted only on the CLI's own DIAG connection |
| `conv enable\|disable <id>` | registry | `Enabled` |
| `conv options <id> [--json]` / `conv set <id> <KEY> <value> [--machine]` / `conv unset <id> <KEY> [--machine]` | registry (HKCU; HKLM with `--machine`) / the POSIX per-user `<id>.settings.json`; `options` negotiates the binary headless | the one options contract (§3.9, N3c): declared + stored keys with value and source; `Settings\<KEY>` writes |
| `conv probe <task> <file\|dir>` | APP connection, stream lane | today's `lift probe` |
| `conv run <task> <in> <out> [--provider]` | APP connection, job lane | file → file; the CLI decodes and encodes files itself (the service never does); MCP `convert_file` backend |
| `conv jobs` / `conv cancel <id>` | DIAG | |
| `conv models list\|verify\|prune\|fetch\|init` | local + registry | model store (§5.4) |

The CLI never loads a second model instance. When a service is reachable it asks it; otherwise it loads providers headless and calls `describe_cap` / `get_state` only, **never `warm`** (today's `probe_lift_caps_d3d11`, `cli_query.c:270-347`, which instantiates the module on WARP and waits up to 8 s, is retired). MCP `convert_file` and `get_conversion_status` are CLI-backed (ADR-051 D4).

### 7.2 Selftest

New check **`conversion_providers`** (`cli_query.c` check table `:2918-3087`):

- Verifies ABI 1, a well-formed `describe_cap` (known task, `lanes ≠ 0`, `min_views ≤ max_views ≤ 8`, `VIEWS_*` ⇒ `EXPLICIT_VIEWPOINTS`, known licence class, unique `cap_id`) and in-range states.
- **Absence never fails** — no provider registered, or no service, passes.
- A malformed descriptor fails with the new exit code **`CLI_SELFTEST_BAD_CONVERSION = 13`**.
- `lift_caps` / exit 11 stays and reads the `dp-lift` row.
- CI registers `sim-conversion` with `ENABLE` = 1 and requires `conversion_providers.count ≥ 1` in `selftest --json`.

As built (#2006): the pass is `target_conv_status` (targets/common), shared with `conv
list|status` and the headless status snapshot. `selftest --json` carries a
`conversion_providers` object (`evaluated`, `count`, `registered`, `negotiated`,
`malformed_rows`, `bad_descriptors`, `task_rows`, `dp_lift_declared`, `note`, `providers[]`
in the §7.3 row shape) next to the `conversion_providers` row of `checks[]`. A malformed
*registry row* (no `Binary`, bad JSON) and a loader reject (ABI, missing binary) are
reported, never failed — only a malformed *descriptor* or an out-of-range `get_state` is.
`lift_caps` no longer instantiates anything headless: it reports the static `dp-lift` row
and, when a service is listening, the service's live caps over one DIAG
`lift_get_properties` (`cli_lift_service.c`). The CI gate on `count ≥ 1` runs only when the
sim provider DLL was built and registered (until #2002 makes that unconditional).

### 7.3 Status snapshot (ADR-051)

`xrt_status_snapshot` gains `conv_providers[]` (id, name, vendor, version, load, status state + hint, fallback, probe_order, hosting `in_service` / `satellite` / `android_service`, host pid, restarts, last exit code, adapter, backend, VRAM used, licence class) and per-task rows (provider index, task, cap_id, state, reason, progress, selected, licence class, backend, model id for diagnostics, typical latency, streams, queued jobs). Rows are fetched **by index** over DIAG to stay under the 1 KB DIAG limit. Keys are only added, so the schema number stays **1**. A `conversion` generation counter bumps on every readiness transition, so `status --watch` and the dashboard update live. New warning codes: `CONV_CRASH_LOOP`, `CONV_RESEARCH_MODEL_ENABLED`, `CONV_TASK_UNSERVED`, `CONV_CPU_STAGING`.

As built (#2006): the row types are `xrt_status_conv_provider` / `xrt_status_conv_task`
(`xrt_display_status.h`), fetched with `system_get_conv_provider_row(index)` /
`system_get_conv_task_row(index)`; the generation counter is
`xrt_status_generation::conversion`. The JSON shape and warning rules are in
[`display-dashboard.md`](../../roadmap/display-dashboard.md) §3.2 / §4. The service fills
the rows from a registered conversion status source (`ipc_server_set_conv_status_source`),
which the core's service object (#2003) registers; with none registered the service reports
zero rows and `conv list` falls back to the headless describe, saying so.

### 7.4 Logging

Per [`debug-logging.md`](../../reference/debug-logging.md):

| Event | Level |
|---|---|
| provider discovered / rejected (ABI, missing binary) | WARN, once per change |
| readiness transition per (provider, capability), with reason | WARN per transition, never per poll |
| host spawn / exit (code) / backoff / crash-loop cutoff / idle exit | WARN |
| selection at create (task → provider, why), fall-through | WARN, once per stream |
| watchdog breach (names provider, slot, duration) | WARN, once per breach |
| CPU-staging fallback, adapter mismatch | WARN, once per stream |
| per-stream stats (rate, latency, drops, failures, `blocked_by_job`, `blocked_by_warm`) | INFO, throttled to 10 s |
| anything per frame | never |

---

## 8. Gates and tooling checklist

| # | Item | Where | Change |
|---|---|---|---|
| 1 | Tripwire | `oxr_plugin_stub.c` (DP asserts `:34-48`) | entry literal; `XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT == _1`; host-iface size; slot offsets; frozen embedded-struct `sizeof` / `offsetof` per architecture; `XRT_CONV_STATE_0..7 == XRT_DP_LIFT_MODULE_*`; every plug-in ↔ spec-4 app enum pair; CONTENT / AXIS / VIEWPOINT / BLOB_PLY,SOG parity; `xrt_conv_provider_status` == `xrt_plugin_platform_status` (`xrt_plugin.h:399`) |
| 2 | Repin surface | `downstream-pins.json` | `feature_surface.headers += xrt_conversion_plugin.h`; open-provider track (`bump_when: abi` on the conversion constant); vendor-template `conversion` track |
| 3 | Per-constant ABI | `scripts/downstream_pin_bump.py` (`:123`, `:146`) | `abi_at_tag(tag, header, macro)`; per-track `abi_constant` |
| 4 | ABI gate | `scripts/check_plugin_abi.py` (`:51-85`), `versions-bump.yml:128-176` | `--field` → (repo, header, constant) via an `abi_gate` table |
| 5 | `versions.json` | + installer mirror | `open_conversion` field |
| 6 | Selftest | `cli_query.c`, `cli_query.h:58-138` | `conversion_providers`, exit 13 (§7.2) |
| 7 | CI | `build-windows.yml:316-333, 383-402`, Linux, macOS | exact export sets; register sim-conversion; POSIX `250-sim-conversion-conversion-provider.json`; host-kill-between-submit-and-signal case (§4.4) |
| 8 | Dev registration | `register_dev_plugin.bat` (`:43-47`) | `conv <dll>` (ProbeOrder 50), `conv sim`, `unregister-conv`; `dev-setup.bat --all` runs `conv sim` |
| 9 | CLI | new `cli_cmd_conv.cpp`; `cli_main.c:95-131` | §7.1 |
| 10 | Snapshot | `xrt_display_status.h` | §7.3; dashboard moves off `info --json` `lift_caps` |
| 11 | MCP | `displayxr-mcp` | `convert_file`, `get_conversion_status` via the CLI |
| 12 | Extensions index | `docs/specs/extensions/index.json` | lift note update for spec 4 / 5 |
| 13 | Docs | this spec; `docs/reference/xrt_conversion_plugin_iface.md` | |
| 14 | Onboarding | `vendor-plugin-onboarding.md` §9 (P3) | separate DLL, §2.4 rules, ProbeOrder 50, `Supersedes`; "close the host, never the service", `Version` written last, the `ConvMaintenance` mutex (§3.8); the `displayxr-conversion-template` starter kit |
| 15 | Layering | `separation-of-concerns.md` | "Conversion provider" layer row + must-NOT list (§1.5); Vendor Isolation Rule covers conversion-vendor identifiers, CI link-line assert extended |
| 16 | ADR | ADR-053 | ADR-042 and ADR-049 marked "amended by" |
| 17 | Tests | `tests_conversion_abi.cpp`, `tests_conversion_select.cpp`, loader suffix tests | truncation, assert parity, selection (preference, `Policy\Lock`, licence, ties, fall-through on any FAILED, no live swap), cold-boot spec-3 ACTIVATING golden |
| 18 | Provider template | `displayxr-conversion-template` (its own repository: a model vendor is not a display vendor) | a CPU-only `DEPTH_MONO` provider, a fake host for hardware-free checks, dev registration (registry row / POSIX manifest), CI on Windows, Linux and macOS |
| 19 | Loader | new `target_conversion_plugin_loader.{c,h}` | reuses `target_plugin_path_check`, `…_preload_runtime_core_dll`, `…_build_discovery_roots`, `…_sanitized_preload`; enumerate / try_load per `target_input_plugin_loader.c:331-566`; Android parse takes a prefix |
| 20 | Service wiring | `d3d11_lift.cpp` `lift_activate` (`:777-871`), `comp_d3d11_service.cpp` `svc_lift` (`:35787-35807`) | P1a: the core behind the existing lift thread, the adapter wrapping the same DP calls; P1b: file split and deletions one release later. Mailbox, export and weave paths unchanged; v3 wire byte-for-byte |

### References

- [ADR-053](../../adr/ADR-053-conversion-providers.md) (Accepted) — conversion providers.
- [`conversion-providers-headers-draft.md`](../../roadmap/conversion-providers-headers-draft.md) — `xrt_conversion_plugin.h`, `XR_DXR_lift` spec 4 / 5, the `conv_*` wire family.
- [`plugin-discovery.md`](plugin-discovery.md) — display-processor discovery; §4 DLL contract, §5 registration lifetime, §6 version negotiation.
- [`input-provider-discovery.md`](input-provider-discovery.md) — the second plug-in type; shape of this spec.
- [ADR-019](../../adr/ADR-019-vendor-plugin-aux-boundary.md), [ADR-020](../../adr/ADR-020-plugin-abi-compatibility-policy.md), [ADR-034](../../adr/ADR-034-input-provider-plugins.md), [ADR-035](../../adr/ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md), [ADR-038](../../adr/ADR-038-android-vendor-plugin-ships-in-the-runtime-apk.md), [ADR-040](../../adr/ADR-040-rear-depth-budget.md), [ADR-042](../../adr/ADR-042-vendor-2d3d-conversion-supersedes-default.md), [ADR-045](../../adr/ADR-045-plugins-always-loadable-and-report-platform-state.md), [ADR-046](../../adr/ADR-046-depth-aware-cursor.md), [ADR-048](../../adr/ADR-048-lift-viewpoint-policy.md), [ADR-049](../../adr/ADR-049-in-process-lift-rides-the-service.md), [ADR-051](../../adr/ADR-051-display-status-model-and-dashboard.md).
- [`service-architecture.md`](../../architecture/service-architecture.md) — threads, locks, failure domains; the satellite shape.
