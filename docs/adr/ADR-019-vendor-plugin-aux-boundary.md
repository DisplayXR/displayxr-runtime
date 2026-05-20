---
status: Proposed
date: 2026-05-20
---
# ADR-019: Aux Library Boundary for Vendor Plug-in DLLs

## Context

`docs/roadmap/vendor-plugin-architecture.md` proposes restructuring `drv_leia` and `drv_sim_display` from in-tree static links of `DisplayXRClient.dll` into separately-built plug-in DLLs the runtime discovers via `LoadLibraryExW` at `xrCreateInstance`. The motivation is in the plan; this ADR is concerned with a single sub-decision the plan calls out as load-bearing (§4.3): how the aux library — `aux_util`, `aux_os`, `aux_math` and friends, all currently built as `STATIC` archives — should be linked across the new DLL boundary.

Today every binary in the tree (`DisplayXRClient.dll`, `displayxr-service.exe`, `displayxr-cli.exe`, the cube test apps in their `_handle`/`_texture` shapes) static-links aux. Because those binaries each live in their own process, every binary having its own private copy of aux's file-scope state has been a non-issue: log sinks, debug-variable registries, metrics streams, ID generators — duplicating them across processes is what we want.

The plug-in restructure breaks that assumption. After the change, **two DLLs in the same process** — `DisplayXRClient.dll` and `DisplayXR-LeiaSR.dll` (or `DisplayXR-SimDisplay.dll`) — will both contain code that calls into aux. Whatever the link line says, the C language's rules are not negotiable: a file-scope `static` variable inside a translation unit packaged into a static archive becomes a per-DLL private when that archive gets linked into multiple DLLs. Picking the wrong sharing strategy here corrupts logging, debug-var tracking, metrics, and ID generation in ways that surface only at runtime and look like ghost bugs.

The plan offers three options (A/B/C) and recommends C. The recommendation isn't binding until we've audited aux for what's actually state-bearing and confirmed empirically that the failure mode happens the way the C language says it does. That's this ADR.

### Why the linker behavior is not negotiable

`DisplayXRClient.dll` as shipped today exports exactly one symbol:

```
$ dumpbin /exports _package\bin\DisplayXRClient.dll
    ordinal hint RVA      name
          1    0 000788A0 xrNegotiateLoaderRuntimeInterface

  1 number of functions
  1 number of names
```

Every aux function (`u_log`, `u_log_set_sink`, `u_var_add_*`, `u_metrics_*`, `u_limited_unique_id_get`, …) is linker-private to the DLL. There is no way for a second in-process DLL to *reach* the runtime's aux state. If a plug-in needs to share that state, the runtime must export an interface to it; static-linking aux into the plug-in cannot work. This isn't a Windows quirk — POSIX `dlopen` semantics produce the same result with the same `STATIC` archives.

### Three options, restated concretely

**Option A — each plug-in static-links aux.**
`DisplayXR-LeiaSR.dll` and `DisplayXR-SimDisplay.dll` each contain their own copy of `aux_util.lib`, `aux_os.lib`, `aux_math.lib`. The runtime DLL keeps its copy. **Consequence:** every state-bearing TU now exists in 2-N independent copies per process.

**Option B — export all of aux from the runtime DLL.**
`DisplayXRClient.dll` declares `aux_util` (etc.) as DLL-exported. Plug-ins link an import library that points at the runtime DLL. **Consequence:** one copy of every aux symbol per process. The entire aux C surface — currently ~40 TUs and growing — becomes part of the runtime DLL's stable ABI.

**Option C — hybrid. State-bearing TUs export from the runtime DLL; pure TUs static-link into plug-ins.**
The runtime DLL exports the few TUs that contain process-singleton state. Everything else (math, containers, formatters, instance-based device/system helpers) static-links into each plug-in as today. **Consequence:** one copy of state-bearing aux per process; the exported ABI surface is the small, identifiable list defined in this ADR.

This is structurally what the Khronos OpenXR loader does with its own logging: the loader exposes a logging entry point that runtime + layer plug-ins all call through, but the loader does not export its math/utility helpers.

## Audit

Every TU in `src/xrt/auxiliary/util/` and `src/xrt/auxiliary/os/` that compiles into `aux_util`, `aux_util_sink`, `aux_util_process`, `aux_os`, or `aux_os_ble` was classified by inspecting (a) whether it contains file-scope mutable state, function-scope mutable statics that drive process-wide invariants, or registration patterns that imply a single shared registry, vs (b) whether it is purely instance-based, pure-functional, or const-only.

### State-bearing (must be single-copy-per-process)

| TU | State | Failure mode if duplicated |
|---|---|---|
| `u_logging.c:83-84` | `static u_log_sink_func_t g_log_sink_func; static void *g_log_sink_data;` | Custom sink registration is per-DLL. `u_log_set_sink` called from the runtime does not affect the plug-in's `u_log` dispatch. |
| `u_file_logging.c:31-32` (Windows) | `static FILE *g_log_file; static int g_initialized;` Calls `u_log_set_sink(file_logging_sink, NULL)` at `u_file_logging_init()`. | Each DLL's `u_log` triggers its own `u_file_logging_init`, opens its own `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.<pid>_<ts>.log` (timestamps differ → two files), and registers `file_logging_sink` only into its own `g_log_sink_func` copy. **The "one log per process" invariant in `docs/reference/debug-logging.md` breaks silently.** |
| `u_var.cpp:77` | `static class Tracker gTracker;` — debug-variable registry: `unordered_map<ptrdiff_t, Obj>` of tracked roots + mutex + on/tested flags. | Debug-GUI variables (`u_var_add_*`) registered by the plug-in never appear in the runtime's tracker, and vice versa. Half the debug surface goes missing. |
| `u_metrics.c:23-26` | `static FILE *g_file; static struct os_mutex g_file_mutex; static bool g_metrics_initialized, g_metrics_early_flush;` | Two metrics files opened with overlapping timeline; the protobuf record sequence is split across them. |
| `u_trace_marker.c:38-39` (when `U_TRACE_PERCETTO` enabled) | `static enum u_trace_which static_which; static bool static_inited;` + Percetto's `PERCETTO_CATEGORY_DEFINE` / `PERCETTO_TRACK_DEFINE` macros expand to file-scope storage. | Percetto registration runs twice; tracks duplicate; trace consumer sees garbage. |
| `u_limited_unique_id.cpp:20` | `static std::atomic_uint_fast64_t generator;` — process-unique ID source. | Plug-in and runtime each return 1, 2, 3, … independently. The header comment says "process unique"; the property holds inside one DLL only. |
| `u_pacing_app.c:768` | function-scope `static int64_t session_id_gen = 0;` inside `paf_create`. | Two app-pacer factories (one per DLL) hand out colliding session IDs to the metrics layer. |
| `u_hud.c:100` | `static bool g_hud_visible;` | HUD toggle is per-DLL. Plug-in cannot read or update the runtime's HUD visibility. Currently benign (HUD is rendered only by the compositor in the runtime DLL), but a foot-gun if any plug-in code consults the flag. |
| `u_sink_converter.c:109` | `static uint32_t lookup_YUV_to_RGBX[256][256][256]` (16 MB) — gated by `#ifdef USE_TABLE`, currently inactive in the tree (`#undef USE_TABLE` two lines earlier). | If `USE_TABLE` is ever defined, each DLL gets its own 16 MB lookup, lazy-populated independently. Cosmetic (results correct), but 16 MB × N DLLs is real RSS. Flagged so we don't lose track if `USE_TABLE` ever re-enables. |
| `os_macos.c:21` (macOS only) | `static atomic_bool g_macos_window_closed;` Read by `comp_multi`, written by `st_oxr`. | Cross-module by design. macOS doesn't have a plug-in DP today (sim_display is the only macOS DP and its window-closed handling is part of the same DLL it always was), so this only becomes a concern when macOS gets a plug-in DP. Noted for future. |

### Pure / instance-based (safe to per-DLL static-link)

These TUs have no file-scope mutable state and no shared-registry patterns. Their function behavior is identical whether each DLL has its own copy or there's one shared copy. They are listed by group, not individually, to keep the table readable:

- **Math and pure helpers:** all of `aux_math` (`m_*.c`); `u_format.c`, `u_pretty_print.c`, `u_misc.c`, `u_visibility_mask.c` (only `static const` data), `u_handles.c`, `u_bitwise.c`, `u_truncate_printf.h`, `u_json.c`.
- **Container / value-type helpers:** `u_hashmap.cpp`, `u_hashset.cpp`, `u_id_ringbuffer.cpp`, `u_deque.cpp`, `u_vector.cpp`, `u_string_list.cpp`, `u_template_historybuf.hpp` (header-only).
- **Instance-based device / system / session / space:** `u_device.c`, `u_system.c`, `u_system_helpers.c`, `u_session.c`, `u_space_overseer.c`, `u_builders.c`, `u_prober.c`, `u_config_json.c`, `u_frame.c`, `u_distortion.c`, `u_distortion_mesh.c`, `u_hand_tracking.c`, `u_hand_simulation.c`, `u_autoexpgain.c`, `u_resampler.c`, `u_tracked_imu_3dof.c`, `u_imu_sink_split.c`, `u_imu_sink_force_monotonic.c`.
- **Pacing factories (excluding `u_pacing_app.c:paf_create`'s static counter):** `u_pacing_compositor.c`, `u_pacing_compositor_fake.c`.
- **Thread pool:** `u_worker.c`, `u_worker.cpp` — instance-based.
- **Sink chains (the `aux_util_sink` archive):** `u_sink_combiner.c`, `u_sink_converter.c` (modulo the dormant `USE_TABLE` above), `u_sink_deinterleaver.c`, `u_sink_force_genlock.c`, `u_sink_queue.c`, `u_sink_simple_queue.c`, `u_sink_quirk.c`, `u_sink_split.c`, `u_sink_stereo_sbs_split.c` — all instance-based.
- **Debug env-var caching:** `u_debug.c` — the `DEBUG_GET_ONCE_*` macros in `u_debug.h` expand to function-scope `static bool gotten; static T stored;` caches. Each TU-instance has its own cache, and each DLL therefore has its own caches. **Functionally equivalent** because env vars don't change at runtime; each DLL converges to the same cached value on first read.
- **Process helpers (`aux_util_process`):** `u_process.c` — pure.
- **OS helpers:** `os_time.cpp`, `os_threading.h` (header-only macros), `os_hid_hidraw.c` (instance-based), `os_ble_dbus.c` / `os_ble_stubs.c` (instance-based), `os_display_edid_win32.c` / `os_display_edid_stubs.c` (enumeration; `static const GUID GUID_DEVCLASS_MONITOR_LOCAL` is const).
- **Windows / Linux-only helpers:** `u_windows.c`, `u_linux.c`, `u_win32_com_guard.cpp` — pure.
- **Generated:** `u_git_tag.c` — single `const char *` of the git description. Per-DLL copies are identical at build time.

### Sanity-check at the call sites

`drv_leia` — the first plug-in candidate — has **260** call sites of `U_LOG_*` macros across 13 files and uses `u_var_add_*` in `leia_device.c`. Whatever sharing strategy we choose has to keep those working without touching the call sites. Both code paths point at state-bearing TUs in the table above, so Option A is ruled out by direct contradiction with `drv_leia` already in the tree.

### Empirical anchor

The audit conclusion is anchored by `dumpbin` output already quoted under "Why the linker behavior is not negotiable" above: `DisplayXRClient.dll` ships with one exported function. Every state-bearing aux symbol the audit identifies is linker-private inside that DLL today. No tweak to the link line of a hypothetical plug-in DLL can reach into those symbols. A separate from-scratch verification build is therefore unnecessary at this stage — the question is settled by the current binary layout combined with the C language's linkage rules. A *bigger* empirical test belongs at the end of Step 4 in the plan (sim_display migration) and is the gating signal that the chosen boundary actually behaves: a single end-to-end run that emits log lines from both the runtime DLL and the sim_display plug-in DLL and confirms the lines land in one log file, in source order. That stress test is captured under "Verification plan" below.

## Decision

**Option C.** The runtime DLL (`DisplayXRClient.dll`) exports a single, deliberately small surface — the state-bearing TUs identified by the audit. Pure / instance-based TUs continue to static-link into each plug-in DLL. Plug-ins link an `aux_imp.lib` import library that resolves the exported surface to the runtime DLL.

Reasoning, in order of weight:

1. **Option A is ruled out by the audit.** `drv_leia` already calls `U_LOG_*` 260 times and `u_var_add_*` in `leia_device.c`; both paths hit file-scope-static state in `u_logging.c` / `u_file_logging.c` / `u_var.cpp`. Letting the plug-in have its own private copy of that state silently breaks the project's "one log file per process" debug invariant — a regression that wouldn't be visible until a vendor opens a support case complaining their U_LOG lines aren't in the runtime's log.

2. **Option B is overcommitted relative to need.** The aux surface is ~40 TUs and growing — most of them helper math, container code, and instance-based device/system helpers that nobody benefits from sharing state on. Committing the whole thing to DLL-exported ABI status forces every aux change to consider downstream-plug-in compatibility, including changes to TUs whose contract is purely "you call this function and it returns the value." That's friction with no upside. The "shouldn't grow much" property is something the surface needs to *earn* — Option C earns it by including only the TUs whose state-bearing role can be named.

3. **Option C's exported surface is bounded and small.** The table in the audit names ten state-bearing TUs total, of which two (`u_sink_converter.c`'s dormant lookup table, `u_hud.c`'s benign visibility flag) are flagged as latent and don't need to be exported at v1. The remaining eight TUs translate to roughly 30-50 exported C functions plus the `xrt_limited_unique_id_t` value type. That's small enough that bumping its ABI is a notable, deliberate event — the discipline we already follow for extension headers ([[feedback_extension_struct_abi.md]]).

4. **Precedent.** The Khronos OpenXR loader already does exactly this: the loader exports a logging interface (`xrCreateApiLayerInstance`'s `XrNegotiateApiLayerRequest` carries logger function pointers) but doesn't export its container or math helpers. Plug-in architectures with the same shape (Vulkan loader, Mesa drivers) consistently land on the same hybrid. The decision repeats their reasoning rather than discovering it from scratch.

## Exported surface (the v1 ABI commitment)

`DisplayXRClient.dll` exports the symbols listed below. They form the contents of `aux_imp.lib` (Windows) / the published `.so` interface (Linux/macOS). **Adding to this list is non-breaking. Removing or changing the signature of anything on this list is an ABI break and requires the same discipline as a `xrt_dp_create_info`-style versioned struct bump.**

From `util/u_logging.h`:
- `u_log`
- `u_log_xdev`
- `u_log_hex`
- `u_log_xdev_hex`
- `u_log_print_result`
- `u_log_set_sink`
- `u_log_get_global_level`

From `util/u_file_logging.h` (Windows):
- `u_file_logging_init`
- `u_file_logging_write_raw`
- `u_file_logging_shutdown`

From `util/u_var.h` (public C API — full enumeration deferred to Step 2 of the plan when the header is touched):
- `u_var_add_root`
- `u_var_remove_root`
- `u_var_visit`
- `u_var_force_on`
- All `u_var_add_<kind>` setters (~25 functions)

From `util/u_metrics.h`:
- `u_metrics_init`
- `u_metrics_close`
- `u_metrics_is_active`
- `u_metrics_write_session_frame`
- `u_metrics_write_used`
- `u_metrics_write_system_frame`

From `util/u_trace_marker.h` (only when `U_TRACE_PERCETTO` is on — built unconditionally as exported stubs otherwise):
- `u_trace_marker_setup`
- `u_trace_marker_init`

From `util/u_limited_unique_id.h`:
- `u_limited_unique_id_get`

From `util/u_pacing.h`:
- `u_pa_factory_create` (the one whose `paf_create` callback owns the static `session_id_gen` counter)

macOS-only (for the future when a macOS plug-in appears) — from `os/os_macos.c`:
- `oxr_macos_window_closed`
- `oxr_macos_reset_window_closed`
- `oxr_macos_set_window_closed`

Everything else that today lives in `aux_util`, `aux_util_sink`, `aux_util_process`, `aux_os`, `aux_os_ble`, `aux_math` continues to static-link into whichever target consumes it. Plug-ins get their own per-DLL copies of those TUs, which is functionally equivalent to sharing them.

### Things explicitly out of scope for v1

- **Sharing `u_hud.c`'s `g_hud_visible` flag.** Today the HUD is rendered exclusively by code in the runtime DLL; plug-ins do not consult it. If a future plug-in wants to participate in HUD rendering, that's an extension-API conversation, not an aux-export conversation. Re-evaluate when the need is concrete.
- **Sharing `u_sink_converter.c`'s `lookup_YUV_to_RGBX` table.** Currently dormant (`#undef USE_TABLE`). If it's ever re-enabled, the 16 MB-per-DLL cost is the prompt to revisit, and the fix at that time is to move the converter behind the export boundary.
- **Sharing aux state on macOS.** `os_macos.c`'s three accessors are listed above for completeness but are not load-bearing in v1 because no macOS plug-in exists. The boundary is being set up correctly for the future; we don't need to ship a macOS plug-in build to validate it.

## Verification plan

The audit + binary inspection (above) settle the *design* question. The remaining empirical question is whether the implementation actually produces a single log sink, single var registry, and single metrics stream across the runtime ↔ plug-in DLL boundary once Step 3 of the plan ("Export aux's stateful TUs from runtime DLL") lands. That verification is a deliverable of Step 3 itself, not this ADR — but the test it must pass is specified here so the implementation has a clear gate.

The stress test, gated to land with Step 3 or Step 4 (whichever ships the first plug-in DLL):

1. Build a probe TU inside `tools/aux_boundary_check/` (throwaway, not shipped) that exports a single `aux_probe_log_and_register(const char *tag)` function. The function:
   - Emits one `U_LOG_W("aux_probe[%s]: hello", tag);` line.
   - Calls `u_var_add_root(&probe_state, "aux_probe", false)` and `u_var_add_u32(&probe_state, &counter, "counter")` to register a debug-tracked variable.
   - Increments `counter` and calls `u_metrics_write_session_frame(...)` with a stub record.
   - Calls `u_limited_unique_id_get()` and returns the value through an `out` pointer.
2. Build that TU into two artifacts:
   - `aux_probe_inproc.lib` — static-linked into `displayxr-cli.exe` for a baseline.
   - `aux_probe_plugin.dll` — separately-built DLL that links `aux_imp.lib`, exports the same function.
3. Add a CLI subcommand `displayxr-cli aux-probe` that:
   - Calls the in-process `aux_probe_log_and_register("inproc")`.
   - `LoadLibraryExW("aux_probe_plugin.dll", ..., LOAD_WITH_ALTERED_SEARCH_PATH)`.
   - `GetProcAddress("aux_probe_log_and_register")`.
   - Calls it with tag `"plugin"`.
   - Reports the two `u_limited_unique_id_get()` values it received.
4. Pass criteria:
   - Exactly **one** `%LOCALAPPDATA%\DisplayXR\DisplayXR_displayxr-cli.<pid>_<ts>.log` is created. Both `aux_probe[inproc]: hello` and `aux_probe[plugin]: hello` lines appear in it, in submission order.
   - `XRT_TRACK_VARIABLES=true` shows both `aux_probe.counter` instances (with disambiguating `aux_probe#1` / `aux_probe#2` naming from `Tracker::getNumber`) in a single `u_var_visit` walk from the host.
   - The two `u_limited_unique_id_get()` return values are distinct *and* monotonically related (the second is greater than the first), proving they came from the same `generator` atomic.
   - `dumpbin /imports aux_probe_plugin.dll | findstr DisplayXRClient` shows the expected import set (the eight TUs' worth of symbols listed above). Nothing else of aux's surface should appear.

A failure on any of those four anchors is a regression in the boundary's implementation, not a relitigation of this ADR.

## Consequences

- **`drv_leia` and `drv_sim_display` migrate to plug-in DLLs without changing their U_LOG / u_var / u_metrics call sites.** Source compatibility for the existing 260 `U_LOG_*` call sites in drv_leia is preserved; only the link line changes.
- **`DisplayXRClient.dll`'s export count goes from 1 to roughly 50.** The Khronos loader interface (`xrNegotiateLoaderRuntimeInterface`) stays the one OpenXR-facing export; the new ~50 are explicitly for in-process plug-in consumption and live behind the `aux_imp.lib` import library. They're not part of any OpenXR-spec surface and do not need to satisfy any external contract.
- **The CI `findstr /i SimulatedReality` guard from §4.8 of the plan continues to assert what it asserts.** Aux exports don't introduce vendor identifiers; the runtime's link line remains vendor-agnostic.
- **One copy of log / var / metrics state per process.** The "one log per process" debug invariant becomes a structural property again, not a coincidence of in-tree static linking.
- **`aux_util.lib` as built today doesn't disappear.** Internal consumers in the runtime DLL still static-link it the same way; the exports are layered on top via a small `__declspec(dllexport)` wrapper TU inside the runtime target. Plug-ins use `aux_imp.lib`. macOS uses the corresponding visibility annotations on the shared library. Linux uses `-fvisibility=hidden` + explicit `__attribute__((visibility("default")))`. Existing in-tree consumers (test apps that link aux_util directly) keep working unmodified; the boundary only matters for the new plug-in DLLs.
- **Headers in `src/xrt/include/xrt/util/` and `src/xrt/auxiliary/util/` describing the exported TUs gain `XRT_API_FUNC` / `XRT_API_VAR` macros** to tag the exported symbols. The macros expand to `__declspec(dllexport)` when building the runtime DLL, `__declspec(dllimport)` when building plug-ins, and nothing for internal consumers and test apps that static-link. The exact macro shape is a Step 2 deliverable (the `xrt_plugin.h` work) — this ADR commits to the **set** of symbols, not the spelling.
- **Bumping the exported aux surface is treated as ABI work**, with the same discipline as extension struct bumps ([[feedback_extension_struct_abi.md]]): never reorder, additions go at the end, no signature changes on existing exports without a versioned wrapper.
- **Vendor SDK upgrades become plug-in releases (plan goal #3) without requiring a runtime release**, because the plug-in's link to aux is via the runtime's stable import library, not the runtime's internal aux source.

## Related

- `docs/roadmap/vendor-plugin-architecture.md` — the parent plan (§4.3 is the option list this ADR resolves).
- `docs/specs/runtime/workspace-controller-registration.md` — the registry-driven plug-in shape this discovery mechanism mirrors.
- `docs/adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md` — the DP vtable contract that is unchanged by this work.
- `docs/architecture/separation-of-concerns.md` — the layering rule that this work makes structurally enforceable on Windows.
- `docs/reference/debug-logging.md` — the "one log per process" invariant this ADR's exported surface preserves.
- Memory `[[feedback_extension_struct_abi.md]]` — the ABI-bump discipline applied to the exported aux surface.
- Memory `[[feedback_compositor_eye_pos_layering.md]]` — sibling sharing-state-across-DLL concern; same family of problem, separate fix.
