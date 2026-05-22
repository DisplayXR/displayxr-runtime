# Vendor Plug-in Architecture

**Status:** Shipped. Issue #256 (in-tree plug-in shape) landed 2026-04;
issue #263 (full extraction of `drv_leia` to its own repo) landed
2026-05. See § 11 "Post-#263 state" at the bottom of this doc for the
current shape. The narrative below is preserved as historical context
for the design decisions.

**Authors:** dfattal, claude
**Target:** Restructure `drv_leia` (and `drv_sim_display`) into separately-built plug-in DLLs that `DisplayXRClient.dll` discovers at LoadLibrary time, so the runtime DLL has zero vendor-specific identifiers in its link line.

---

## 1. Problem

The "vendor-agnostic" framing in `CLAUDE.md` and `docs/guides/vendor-integration.md` is aspirational on Windows. The shipped `DisplayXRClient.dll` has these static (load-time) imports baked in:

```
SimulatedRealityCore.dll
SimulatedRealityDisplays.dll
SimulatedRealityFaceTrackers.dll
SimulatedRealityDirectX.dll
SimulatedRealityOpenGL.dll
```

These come from `drv_leia` import-lib-linking the matching SR SDK `.lib` files in `src/xrt/drivers/CMakeLists.txt:148-214`. The Windows DLL loader resolves all static imports before any code in `DisplayXRClient.dll` runs, so on a machine without SR Platform installed (i.e. without `C:\Program Files\LeiaSR\Platform\bin` on PATH), the runtime DLL **fails to load entirely** — the Khronos OpenXR loader reports no runtime available, and the app sees `xrCreateInstance` fail.

This silently invalidates two pieces of intended behavior:

1. **`drv_sim_display` fallback.** `drv_sim_display` is unconditionally built into the runtime (`src/xrt/drivers/CMakeLists.txt:316-339`, no `XRT_HAVE_LEIA_SR` gate) precisely so the runtime works on any machine. The `FORCE_SIM_DISPLAY=1` env-var override is a runtime-policy lever. Neither helps when the DLL containing them can't load.
2. **The vendor-agnostic claim.** A different display vendor cannot integrate without forking the runtime build system. Adding a second vendor today means either adding parallel `SimulatedReality*`-style static imports (which compounds the same coupling) or moving to runtime-`LoadLibrary` (which is this restructure).

macOS already has the right shape by accident: there's no Leia SR SDK for macOS, `XRT_HAVE_LEIA_SR` resolves OFF at configure time (`CMakeLists.txt:215`), `drv_leia` isn't built, only `drv_sim_display` ships, and the runtime loads on any Mac. Windows should match.

PR #253 surfaced this when removing `$INSTDIR` from system PATH. The five static SR imports happened to still resolve via SR Platform's *own* PATH entry (`C:\Program Files\LeiaSR\Platform\bin`, written by the SR Platform installer), so the PR doesn't regress. But that's load-bearing on SR Platform being a hard prereq — which on Windows it currently is, but only by accident of the link line, not by design.

## 2. Goals

In priority order:

1. **`DisplayXRClient.dll` has zero vendor identifiers in its link line.** `dumpbin /imports` shows no `SimulatedReality*` (or any other vendor name). CI asserts this.
2. **Runtime loads on any Windows machine without any vendor SDK installed.** Probe sequence picks `drv_sim_display`, the cube test apps render in SBS via the sim_display DP.
3. **Vendor SDK upgrades become plug-in releases**, not runtime releases. Bumping the bundled `SimulatedRealityVulkanBeta.dll` fallback no longer needs a runtime tag.
4. **A new display vendor's integration story is "ship a plug-in DLL + register a manifest."** No fork of the runtime build system, no PR to `src/xrt/drivers/CMakeLists.txt`.
5. **The plug-in shape mirrors the workspace-controller registration shape** (`docs/specs/runtime/workspace-controller-registration.md`). Same registry root style, same `UninstallString` cascade contract, same "runtime owns nothing vendor-specific" stance.

## 3. Non-goals (v1)

- **Out-of-process plug-ins.** Vendor crashes still take down the host app. Service-style isolation is a separate, much larger effort and is left for v2 if it ever matters.
- **Hot-swap.** Plug-ins are discovered + loaded once during `xrCreateInstance`. No reload-without-restart.
- **Android.** No DP plug-ins ship on Android today; the build system gains plug-in infrastructure but no plug-in is migrated there.
- **Multiple plug-ins active simultaneously.** v1 picks one DP plug-in per session (first-match-wins by registration order). Multi-display heterogeneous setups (Leia panel + future-vendor panel in the same box) are a v2 problem.
- **Replacing the existing in-process `XRT_HAVE_LEIA_SR` build path entirely.** v1 keeps it as a developer-build CMake option (`XRT_PLUGIN_BUILD_INPROC_FALLBACK`) so the runtime can still be statically built for debugging, but ships only the plug-in shape.

## 4. Design

### 4.1 Discovery and registration

Mirror the workspace-controller pattern.

**Registry root:** `HKLM\Software\DisplayXR\DisplayProcessors\<id>`

`<id>` is a vendor-prefixed short identifier (e.g. `leia-sr`, `sim-display`).

Subkey schema:

| Value | Type | Required | Purpose |
|---|---|---|---|
| `Binary` | REG_SZ | yes | Absolute path to the plug-in DLL. |
| `DisplayName` | REG_SZ | yes | Human-readable name; logged at probe. |
| `Vendor` | REG_SZ | no | Publisher name (reserved). |
| `Version` | REG_SZ | no | Free-form; logged at probe. |
| `UninstallString` | REG_SZ | yes | Cascade-uninstall hook. Must honor `/S`. |
| `ProbeOrder` | REG_DWORD | no | Lower runs first. Missing = 100. Vendors should use 50; sim_display registers at 200 so it's always the fallback. |

**Discovery sequence in `DisplayXRClient.dll`:**

1. At `xrCreateInstance`, before any DP-using code runs, the runtime enumerates `HKLM\Software\DisplayXR\DisplayProcessors\*`.
2. Sorts by `ProbeOrder` ascending.
3. For each: `LoadLibraryExW(Binary, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)`. Failure to load (missing vendor SDK on this box, version mismatch, etc.) → log warning, skip to next.
4. `GetProcAddress("xrtPluginNegotiate")`. Missing or returns failure → log, skip.
5. Plug-in's `xrtPluginNegotiate` runs its own probe (e.g. drv_leia checks for an SR display via the SR SDK). Negative probe → plug-in returns `XRT_PLUGIN_NO_DEVICE`, runtime skips.
6. First plug-in that returns success wins. Its returned `xrt_plugin_iface` is owned by the runtime for the session.

Failure mode if nothing matches: runtime logs WARN, falls back to a built-in null DP (or refuses to create instance — TBD in §10).

### 4.2 Plug-in ABI

**Single C ABI entry point per plug-in DLL:**

```c
// Stable across plug-in API versions; runtime never breaks this signature.
__declspec(dllexport) xrt_result_t
xrtPluginNegotiate(
    uint32_t runtime_api_version,    // XRT_PLUGIN_API_VERSION the runtime speaks
    const struct xrt_plugin_host_iface *host,   // runtime-supplied callbacks (logging, aux access)
    struct xrt_plugin_iface **out_iface,        // plug-in returns its iface
    uint32_t *out_plugin_api_version);          // plug-in fills in version it speaks
```

`runtime_api_version` + `out_plugin_api_version` let either side decline if too old/new. v1 = 1.

`xrt_plugin_iface` is a vtable of function pointers + a few flat fields:

```c
struct xrt_plugin_iface {
    uint32_t struct_size;                       // sizeof(xrt_plugin_iface) at compile time
    const char *id;                             // matches registry <id>
    const char *display_name;

    // Probe: does this plug-in want to handle the current system?
    // Runs before any DP instantiation. Cheap. Allowed to call into vendor SDK.
    xrt_result_t (*probe)(struct xrt_plugin_instance **out_inst);

    // Device + DP construction. Called only if probe() succeeded.
    xrt_result_t (*create_device)(struct xrt_plugin_instance *inst, struct xrt_device **out_dev);
    xrt_result_t (*create_display_processor)(
        struct xrt_plugin_instance *inst,
        enum xrt_graphics_api api,             // D3D11, D3D12, GL, VK, METAL
        const struct xrt_dp_create_info *info,
        struct xrt_display_processor **out_dp);

    void (*destroy)(struct xrt_plugin_instance *inst);
};
```

The DP vtable returned by `create_display_processor` is unchanged — it's the existing per-API `xrt_display_processor*` contract from `docs/guides/vendor-integration.md`. The plug-in restructure adds plumbing **around** that vtable, not changes to it.

**Header location:** `src/xrt/include/xrt/xrt_plugin.h`. Public, stable. Goes through the same ABI-bump discipline as other extension headers (memory: [[feedback_extension_struct_abi.md]] — add `struct_size` first field, never reorder existing fields, version every public struct).

### 4.3 The aux library boundary (load-bearing decision)

This is the single biggest call in the design.

Today `drv_leia` static-links `aux_util`, `aux_math`, `aux_os`, etc. As a separate DLL, the plug-in has three choices:

**Option A: Each plug-in static-links aux.** Simplest build-wise, but each plug-in gets its own copy of aux's process-global state (log sinks, `os_thread_*` state, any `static` variables). If the runtime DLL also static-links aux for its own use (it does), now there are *two* live copies of aux per process. Log output gets confusing, any singleton invariant is silently broken.

**Option B: Aux is exported from `DisplayXRClient.dll`.** Aux moves into the runtime DLL's export surface. Plug-ins link an `aux_imp.lib` import library that points at the runtime DLL. One copy of aux per process, clean state. Cost: aux's C surface becomes a stable ABI commitment.

**Option C: Hybrid — aux is split.** Pure helpers (`m_*` math, header-only macros) static-link into plug-ins. Stateful helpers (`u_logging`, `os_thread`, `u_var`) export from the runtime. This is what the Khronos OpenXR loader effectively does with its own logging.

**Recommendation: Option C.** Audit aux for state-bearing TUs; export only those. Math/macro headers stay static. Concretely:
- Export from runtime: `u_logging.c`, `u_var.c`, anything that touches `static` mutable state or registers process-global callbacks.
- Plug-ins static-link: `m_*.c`, `u_string_list.c`, `u_misc.c` (header-only or pure-functional).

The exported surface should be small enough that bumping it is rare. The ADR for this decision is the first deliverable.

### 4.4 Plug-in DLL contents (`DisplayXR-LeiaSR.dll`)

- All current `src/xrt/drivers/leia/*` source.
- The five `SimulatedReality{Core,Displays,FaceTrackers,DirectX,OpenGL}.lib` import-lib linkages move here.
- The `/DELAYLOAD:SimulatedRealityVulkanBeta.dll` + the `__pfnDliNotifyHook2` from PR #253 move here. The plug-in DLL owns its own `DllMain`.
- The bundled `SimulatedRealityVulkanBeta.dll` fallback (currently shipped by the runtime installer per `src/xrt/drivers/CMakeLists.txt:170-175`) moves to the plug-in's install set.
- Exports exactly one symbol: `xrtPluginNegotiate`.

After this, the runtime DLL's `dumpbin /imports` shows no `SimulatedReality*` at all.

### 4.5 sim_display plug-in (`DisplayXR-SimDisplay.dll`)

- All current `src/xrt/drivers/sim_display/*` source.
- Registers with `ProbeOrder=200` so it's last.
- Probe always succeeds (no hardware requirement).
- Ships in `$INSTDIR\plugins\` from the runtime installer itself (it's the vendor-neutral fallback, conceptually part of the runtime distribution).
- Exports exactly one symbol: `xrtPluginNegotiate`.

This is the first plug-in to migrate — no SDK dependency, smallest blast radius, validates the ABI before any vendor work touches it.

### 4.6 Installer split

| Today | After |
|---|---|
| Runtime installer ships `DisplayXRClient.dll` + 5 SR core DLLs + `SimulatedRealityVulkanBeta.dll` + `drv_leia` compiled into the runtime DLL. | Runtime installer ships `DisplayXRClient.dll` + `DisplayXR-SimDisplay.dll` (in `$INSTDIR\plugins\`) + registers sim_display at `HKLM\Software\DisplayXR\DisplayProcessors\sim-display`. **No SR DLLs.** |
| Leia plug-in: nothing distinct. | New installer (`DisplayXR-LeiaSR-Setup.exe`) ships `DisplayXR-LeiaSR.dll` + `SimulatedRealityVulkanBeta.dll` fallback + registers `HKLM\Software\DisplayXR\DisplayProcessors\leia-sr`. Hard-prereq-checks the runtime via `HKLM\Software\DisplayXR\Runtime\InstallPath` (same pattern as workspace-controller installers). Could ship from the SR Platform installer instead, if Leia owns that distribution channel. |

The runtime uninstaller runs cascade-uninstall over `HKLM\Software\DisplayXR\DisplayProcessors\*` — same pattern as the existing workspace-controller cascade in `installer/DisplayXRInstaller.nsi:772-808`.

### 4.7 Cross-platform

- **macOS:** `drv_sim_display` becomes `DisplayXR-SimDisplay.dylib`. Registration is via a JSON manifest in `~/Library/Application Support/DisplayXR/DisplayProcessors/` (no registry on macOS). Probe order is via a sortable filename prefix. Same ABI.
- **Linux / Android:** Build system gains plug-in infrastructure but no DPs migrate (none ship there today). The ABI is defined; future work can plug in when needed.

### 4.8 CI verification

The high-value test that demonstrates this restructure actually works:

```yaml
- name: Build runtime with NO vendor SDK present
  run: |
    unset LEIASR_SDKROOT
    scripts\build_windows.bat build
- name: Assert zero vendor imports
  run: |
    dumpbin /imports _package\bin\DisplayXRClient.dll | findstr /i "SimulatedReality" && exit 1 || exit 0
- name: Smoke test on clean box
  run: |
    set PATH=C:\Windows\System32;C:\Windows
    _package\run_cube_handle_d3d11_win.bat
    # Expects: runtime loads, sim_display DP picks SBS mode, cube renders.
```

This is the single test that the runtime is actually vendor-agnostic. If it fails, the restructure isn't done.

## 5. Sequencing

Recommended order. Each step lands as its own PR.

| # | Step | Deliverable | ~Days |
|---|---|---|---|
| 1 | **ADR for plug-in ABI + aux boundary.** | New ADR-019. PR for review. No code. | 2 |
| 2 | **Define `xrt_plugin.h` + host iface.** | Public header + first version of the host-supplied callback iface. Compiles, nothing uses it. | 1 |
| 3 | **Export aux's stateful TUs from runtime DLL.** | `u_logging` + `u_var` + `os_thread` come out of `DisplayXRClient.dll`'s exports. `aux_imp.lib` generated. Existing in-tree consumers unchanged. | 2-3 |
| 4 | **Migrate `drv_sim_display` to plug-in shape.** | `DisplayXR-SimDisplay.dll` built. Runtime can `LoadLibrary` it + negotiate. Cube apps render via sim_display through the plug-in path. | 3-5 |
| 5 | **Runtime side: registry enumeration + probe loop.** | `DisplayXRClient.dll` actually discovers plug-ins from the registry, no longer hard-references either driver. | 2 |
| 6 | **Migrate `drv_leia` to plug-in shape.** | `DisplayXR-LeiaSR.dll` built. SR SDK linkage entirely contained in plug-in. Cube apps render via Leia through the plug-in path. PR #253's DllMain hook moves here. | 3-5 |
| 7 | **Installer split.** | Runtime installer drops SR DLLs + drv_leia bits. New Leia plug-in installer (or hand off to SR Platform). Cascade uninstall verified. | 2 |
| 8 | **CI: vendor-agnostic build + assert.** | Workflow builds without SR SDK present, asserts zero vendor imports, smoke-tests sim_display path. | 1 |
| 9 | **macOS port + docs.** | sim_display dylib + manifest discovery. `docs/guides/vendor-integration.md` rewrite for plug-in shape. | 2-3 |

**Total: ~3-4 weeks** for one focused engineer. Parallelizable to ~2 calendar weeks with two.

**Critical path:** ADR → header → aux boundary → sim_display migration. Once sim_display ships end-to-end, drv_leia migration is mostly mechanical.

## 6. Backward compatibility

- The `XR_EXT_display_info`, `XR_EXT_win32_window_binding`, etc. extension headers don't change. App-facing ABI is unaffected.
- The DP vtable (`xrt_display_processor*`) doesn't change. The plug-in restructure adds plumbing around it, not changes to it. Existing vendor integration docs stay accurate at the vtable layer.
- The CMake option `XRT_HAVE_LEIA_SR` remains, but in v1 it controls whether the Leia *plug-in* gets built (not whether the runtime hard-links SR). A new option `XRT_PLUGIN_BUILD_INPROC_FALLBACK` lets developers still produce a statically-linked runtime for debugging (off by default in releases).
- The `FORCE_SIM_DISPLAY=1` runtime override stays. After the restructure it manifests as "skip leia-sr plug-in probe, fall through to sim-display."

## 7. Risks

1. **Aux state duplication (§4.3).** Picking wrong here is the most expensive mistake. Mitigation: dedicated ADR before any code lands. Verify with a stress test that creates instance, destroys, recreates, exercises logging from both DLLs.
2. **Eye-tracking listener marshalling.** `drv_leia` subscribes to `EyeTracker`/`EyePairStream` listeners on SR SDK threads. Those callbacks now fire inside the plug-in DLL and need to reach the runtime's DP-snapshot cache (memory: [[feedback_compositor_eye_pos_layering.md]]). Today they're co-located; after the split, the host iface needs a thread-safe "publish eye position" callback. Validate during step 6.
3. **Symbol naming and exports.** Multiple plug-ins both linking the same static aux helpers, both exporting the same internal symbol names, can collide if a plug-in's helpers somehow get re-exported. Discipline: every plug-in DLL exports exactly `xrtPluginNegotiate` and nothing else. Use a `.def` file + `/EXPORT:xrtPluginNegotiate` link-line guard.
4. **Probe latency.** Today drv_leia probe is in-process and fast. After: per-plug-in `LoadLibraryEx` + `GetProcAddress` + `probe()` on the `xrCreateInstance` hot path. Each plug-in's probe must stay sub-millisecond. SR-side already does — keep it that way. Logging instrumentation lands in step 5.
5. **Vendor crash isolation.** In-process plug-in still crashes the host app on a vendor-side bug. Explicit non-goal for v1; doc the limitation. Worth a follow-up issue for an out-of-process variant if it ever bites.
6. **Path-lookup oddities.** Plug-ins live in `$INSTDIR\plugins\`. Their transitive imports (SR Platform's DLLs for Leia, none for sim_display) resolve from wherever they live now — for Leia, that's `C:\Program Files\LeiaSR\Platform\bin` via PATH, *or* via `LOAD_WITH_ALTERED_SEARCH_PATH` if anchored at the plug-in's directory. Worth an explicit test on a box with stripped PATH (memory: [[reference_sr_platform_path.md]] documents the current resolution path).

## 8. Open questions

1. **Empty-set behavior.** No plug-in registered (or all probes fail) — does `xrCreateInstance` succeed with a null DP and "no 3D output" mode, or fail with a clear error? Suggest: fail with `XR_ERROR_INSTANCE_LOST` + a WARN log naming the registry root the user should populate. Leaning toward failing fast.
2. **Plug-in DLL signing.** Should plug-ins be Authenticode-signed and the runtime verify? Probably yes eventually (it's the obvious supply-chain pressure point), not v1.
3. **Multi-display heterogeneous setups** — Leia panel + future panel in the same box, each wanting its own DP. v2. Note in the ADR.
4. **macOS distribution.** Manifest-in-Application-Support is the v1 mechanism, but Apple's app sandbox may push toward bundled `.bundle` plug-ins inside the host app's resources. Re-evaluate when the first commercial macOS vendor SDK exists.

## 9. References

- `docs/guides/vendor-integration.md` — existing DP vtable contract (unchanged by this work).
- `docs/specs/runtime/workspace-controller-registration.md` — the registration pattern this plug-in shape mirrors.
- `docs/adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md` — vtable rationale; this work is the natural follow-up.
- `docs/architecture/separation-of-concerns.md` — layer boundaries that today are aspirational on Windows; this work makes them real.
- PR #253 — surfaced the static-import shape; the new `__pfnDliNotifyHook2` migrates into the Leia plug-in's DllMain.

## 10. Decision log

- 2026-05-20 — Plan drafted, no decisions yet.
- 2026-04 — Issue #256 landed: in-tree plug-in DLLs (`DisplayXR-LeiaSR.dll` from `src/xrt/drivers/leia/`, `DisplayXR-SimDisplay.dll` from `src/xrt/drivers/sim_display/`), discovered at `xrCreateInstance` via registry / JSON manifest. Runtime DLL has zero vendor static imports per ADR-019 §2.1; CI assertion in `.github/workflows/build-windows.yml` enforces.
- 2026-05-22 — Issue #263 landed: `drv_leia` extracted entirely to its own repo at [`DisplayXR/displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin). Runtime tree is now vendor-source-clean; `drv_sim_display` remains in-tree as the vendor-neutral fallback example.

## 11. Post-#263 state

The end state of this work:

- **Runtime tree (`DisplayXR/displayxr-runtime`)** — zero vendor source, zero vendor SDK fetches in CI. Builds with sim_display as the in-tree plug-in. The `XRT_PLUGIN_BUILD_INPROC_FALLBACK` option is retained as a guard against accidental re-introduction of in-tree static-link paths; it defaults OFF and is expected to be removed in a follow-up.
- **Leia plug-in (`DisplayXR/displayxr-leia-plugin`)** — owns `src/drv_leia/` (formerly `src/xrt/drivers/leia/`), its NSIS installer (`DisplayXRLeiaSRSetup-*.exe`), its CI (fetches SR SDK from this repo's `sr-sdk-v*` release), and the sr-sdk re-host from runtime's old release surface.
- **Plug-in ABI contract** — `xrt/xrt_plugin.h`, `xrt/xrt_api.h`, `target_plugin_loader.{h,c}`, `sim_display_plugin.c` stay in the runtime tree as the consumed surface for vendor plug-ins (FetchContent-pinned).
- **Onboarding** — new vendors fork the Leia plug-in repo as a template (or follow [`docs/guides/vendor-plugin-onboarding.md`](../guides/vendor-plugin-onboarding.md) for the contract). The runtime owns no vendor-specific code paths.
