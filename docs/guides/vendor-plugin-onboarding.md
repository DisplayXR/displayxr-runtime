# Vendor Plug-in Onboarding

This guide walks a new 3D-display vendor from zero to a shipping DisplayXR-compatible plug-in. After ADR-019 / #263 (May 2026), every vendor integration is an **external project**: you don't touch this repo, you ship a plug-in DLL from your own repo and an installer that registers it with the runtime at install time.

If you're maintaining a vendor integration that was historically in-tree (the way `drv_leia` was before #263), see the [legacy in-tree guide](../archive/vendor-integration-historical.md) — kept for historical context. Everything below assumes the post-#263 model.

> **Canonical examples** (Leia SR is used throughout as the *example vendor* — substitute your own vendor name, SDK, and `<id>`)
> - [`DisplayXR/displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin) — a full worked vendor integration (the first vendor on the platform); the recommended starting point. Fork the repo, swap `drv_leia/` for `drv_<your-vendor>/`, point the installer at your `<id>`.
> - `src/xrt/drivers/sim_display/sim_display_plugin.c` (in this repo) — vendor-neutral software-only plug-in. Smaller, no vendor SDK, useful as a minimal-shape reference.

## 1. What you ship

Two artifacts make a complete vendor integration:

1. **A plug-in DLL** (Windows `.dll` / macOS `.dylib` / Linux `.so`) named `DisplayXR-<YourVendor>.dll`, implementing [`xrt_plugin_iface`](../reference/xrt_plugin_iface.md) and exporting exactly one C symbol: `xrtPluginNegotiate`.
2. **An installer** that drops the DLL at a well-known path and registers it under `HKLM\Software\DisplayXR\DisplayProcessors\<your-id>` (Windows; POSIX uses a JSON manifest — see §4).

The runtime DLL has zero vendor identifiers in its own link line (`dumpbin /imports DisplayXRClient.dll` returns nothing matching vendor SDK names — there's a CI tripwire that fails any change which regresses this). All vendor SDK static imports live in **your** plug-in DLL.

## 2. What you consume from the runtime

Your plug-in build links against the runtime's public ABI surface. None of this needs to be vendored — the runtime tree provides headers + import lib via CMake `FetchContent`.

### Headers (public C ABI — every plug-in needs these)

| Header | Purpose |
|---|---|
| `xrt/xrt_plugin.h` | The iface contract — `xrt_plugin_iface`, `xrtPluginNegotiate`, `xrt_plugin_display_info`. Versioned via `XRT_PLUGIN_API_VERSION_CURRENT`. |
| `xrt/xrt_api.h` | `XRT_API_FUNC` decoration — expands to `__declspec(dllimport)` when `XRT_USING_RUNTIME_DLL` is defined in the plug-in build. |
| `xrt/xrt_device.h` | Abstract device interface (you return one of these from `create_device`). |
| `xrt/xrt_display_processor.h` + per-API headers (`xrt_display_processor_d3d11.h`, etc.) | DP vtable contract per graphics API. |
| `xrt/xrt_results.h` | `xrt_result_t` + the standard error codes. |
| `aux/util/u_logging.h`, `aux/util/u_var.h`, `aux/util/u_metrics.h`, … | Aux utilities (see "DLL surface" below). |

### Runtime DLL import library — `DisplayXRClient.lib`

Link against this to resolve `XRT_API_FUNC`-decorated symbols at load time. The runtime's exported aux surface (per [ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md)):

- `u_log_*` — per-process file logger (`%LOCALAPPDATA%\DisplayXR\` on Windows)
- `u_var_*` — debug variable tracking
- `u_metrics_*` — frame timing
- `u_trace_marker_*` — Perfetto tracing
- `u_limited_unique_id_get` — process-wide unique IDs
- `u_pa_factory_create` — pacing factory

Define `XRT_USING_RUNTIME_DLL` in your plug-in's compile flags so `XRT_API_FUNC` expands correctly. See `installer/CMakeLists.txt` in the Leia plug-in repo for the canonical setup:

```cmake
target_compile_definitions(DisplayXR-LeiaSR PRIVATE XRT_USING_RUNTIME_DLL)
target_link_libraries(DisplayXR-LeiaSR PRIVATE
    $<TARGET_LINKER_FILE:${RUNTIME_TARGET}>   # DisplayXRClient.lib
    xrt-interfaces aux_util aux_os aux_math aux_vk drv_includes
)
```

The non-exported static aux helpers (`xrt-interfaces`, `aux_util`, etc.) are pulled in by the linker on a per-`.obj` basis — only the bits your plug-in actually references end up in your DLL.

### Shared third-party DLLs from the runtime install dir

The runtime installer ships several DLLs in `C:\Program Files\DisplayXR\Runtime\` that are **not** transitive dependencies of `DisplayXRClient.dll` itself — they're a shared surface for downstream consumers:

- `openxr_loader.dll`
- `pthreadVC3.dll`
- `cjson.dll`

If your plug-in dynamically imports any of these, they resolve through standard exe-directory DLL search from `$RuntimeInstall\` (your plug-in's installer drops `DisplayXR-<You>.dll` into `$RuntimeInstall\Plugins\<your-id>\`, two directory levels into the same install tree).

**Do NOT re-ship these DLLs in your plug-in installer** — bundling duplicates risks version skew across the install tree. The runtime owns their lifecycle.

## 3. Building from the template

The fastest path:

```bash
gh repo fork DisplayXR/displayxr-leia-plugin --clone=true --remote=true displayxr-vendor-XXX-plugin
cd displayxr-vendor-XXX-plugin
```

Then:

1. **Rename `src/drv_leia/` → `src/drv_<vendor>/`** and replace its contents with your SDK glue: device init, hardware probe, weaver per-API, eye tracking listener.
2. **Rewrite `src/drv_<vendor>/<vendor>_plugin.c`** against `xrt_plugin_iface` — see the [iface reference](../reference/xrt_plugin_iface.md) for which callbacks are required vs optional and what each one must produce.
3. **Update `CMakeLists.txt`** — rename the `add_library(DisplayXR-LeiaSR SHARED …)` target to `DisplayXR-<YourVendor>`. The `XRT_USING_RUNTIME_DLL` + linker settings stay the same.
4. **Update `installer/DisplayXR<YourVendor>Installer.nsi`** — change the registry `<id>` from `leia-sr` to your own, the install dir from `LeiaSR` to your vendor name, and the `DisplayName` / `Vendor` strings.
5. **Update `scripts/build-windows.bat`** — point the vendor SDK download URL at your vendor's SDK release artifact. Replace the `LEIASR_SDKROOT` env-var with `<VENDOR>_SDKROOT`.

The runtime `FetchContent` setup at the top of the plug-in repo's `CMakeLists.txt` does NOT need editing — your plug-in still consumes the same runtime ABI.

## 4. Discovery contract

At `xrCreateInstance` time the runtime walks the registered plug-ins, sorts them by `ProbeOrder` (lower runs first), `LoadLibraryExW`s each, resolves `xrtPluginNegotiate`, calls it, then calls the returned `iface->probe()`. The first plug-in whose `probe()` returns `XRT_SUCCESS` claims the system; the rest are skipped silently.

### Windows: registry-based

Your installer writes these values under `HKLM\Software\DisplayXR\DisplayProcessors\<your-id>`:

| Value | Type | Purpose |
|---|---|---|
| `Binary` | `REG_SZ` | Absolute path to your plug-in DLL |
| `DisplayName` | `REG_SZ` | Human-readable name, logged at probe |
| `Vendor` | `REG_SZ` | Your company name |
| `Version` | `REG_SZ` | Plug-in version (e.g. `1.0.0`) |
| `ProbeOrder` | `REG_DWORD` | Discovery priority — see below |
| `UninstallString` | `REG_SZ` | Quoted full path to your `Uninstall.exe`; the runtime's cascade-uninstaller invokes this with `/S` when the runtime is uninstalled |

`<your-id>` is a short kebab-case identifier matching `iface->id` (the string the plug-in returns from `xrtPluginNegotiate`). The Leia plug-in uses `leia-sr`; sim-display uses `sim-display`.

### POSIX: JSON manifest-based

The runtime reads manifests from:
- macOS: `~/Library/Application Support/DisplayXR/DisplayProcessors/`
- Linux: `${XDG_DATA_HOME:-~/.local/share}/DisplayXR/DisplayProcessors/`

Plus any directory in `XRT_PLUGIN_SEARCH_PATH`. Each `*.json` manifest:

```json
{
    "id": "your-id",
    "display_name": "Your Vendor Display",
    "vendor": "Your Company",
    "version": "1.0.0",
    "probe_order": 50,
    "binary": "/absolute/path/to/DisplayXR-YourVendor.dylib"
}
```

The full discovery contract — registry layout, JSON schema, env-var overrides, fallback search order — is documented in [`docs/specs/runtime/plugin-discovery.md`](../specs/runtime/plugin-discovery.md).

### `ProbeOrder` convention

| Range | Meaning | Examples |
|---|---|---|
| 1–99 | Vendor with hardware probe — claims the system when its hardware is present, declines cleanly otherwise | Leia SR = 50 |
| 100–199 | Reserved | — |
| 200–254 | Vendor-neutral fallback that always claims if reached | sim-display = 200 |

Pick a value in 1–99 if your `probe()` consults a vendor SDK to detect connected hardware and returns `XRT_ERROR_PROBER_NOT_SUPPORTED` cleanly when absent. Pick something close to 200 if your plug-in is meant to handle "any machine, no specific hardware" cases (rare for vendor plug-ins).

## 5. Installer contract

The vendor plug-in installer is independent of the runtime installer — it has its own version cadence, its own release flow, its own NSIS script (Windows) or `.pkg` builder (macOS).

### Hard prereq: the runtime must already be installed

Your installer's first action should be:

```nsis
ReadRegStr $0 HKLM "Software\DisplayXR\Runtime" "InstallPath"
${If} $0 == ""
    MessageBox MB_OK|MB_ICONSTOP "DisplayXR Runtime is required. Install it first from https://github.com/DisplayXR/displayxr-runtime/releases then retry."
    Abort
${EndIf}
```

This prevents "I installed the plug-in but nothing happens" support tickets — without the runtime, `DisplayXRClient.dll` doesn't exist and your plug-in's import fails at load time.

### Install dir convention

`$RuntimeInstall\Plugins\<YourVendorId>\` — e.g. `C:\Program Files\DisplayXR\Plugins\LeiaSR\`.

Drop the following into that directory:
- Your plug-in DLL (`DisplayXR-<YourVendor>.dll`)
- Any vendor SDK runtime DLLs you license to redistribute (e.g. the Leia plug-in bundles `SimulatedRealityVulkanBeta.dll` because it's not in the SR Platform install set; everything else comes from the SR Platform installer separately)
- An `Uninstall.exe` (NSIS generates this automatically)

Do **not** drop anything into `$RuntimeInstall\` (the parent). That's runtime-owned.

### `UninstallString` for cascade-uninstall

The runtime's uninstaller has a **cascade-uninstall** pass: it walks `HKLM\Software\DisplayXR\DisplayProcessors\*`, reads each entry's `UninstallString`, and runs it silently before uninstalling its own files. This is how the runtime cleans up vendor plug-ins when the user uninstalls the runtime.

Your installer **must** register `UninstallString` correctly:

```nsis
WriteRegStr HKLM "Software\DisplayXR\DisplayProcessors\<your-id>" \
    "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
```

The quoted form (with embedded double-quotes) is the convention the runtime's cascade-uninstaller expects.

### Reference installer

[`displayxr-leia-plugin/installer/DisplayXRLeiaSRInstaller.nsi`](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/installer/DisplayXRLeiaSRInstaller.nsi) is the canonical reference. Lift it wholesale; rename `LeiaSR` → your vendor name, change the registry `<id>`, and you're 90% there.

## 6. Vendor-specific concerns

### SDK redistribution

If your vendor SDK's license allows redistribution, bundle the runtime DLLs in your installer. The end-user's experience is one installer click.

If it doesn't, document the SDK as a hard prereq in your installer's pre-install check (similar to how the Leia plug-in's installer requires its vendor SDK platform installer to be run first, separately).

### Eye-tracking mode

`xrt_plugin_display_info.supported_eye_tracking_modes` is a bitmask:
- bit 0 (`0x1`) — `MANAGED` (vendor SDK predicts eye positions; the runtime queries them per frame)
- bit 1 (`0x2`) — `MANUAL` (the app submits eye positions via `XR_EXT_display_info`)

Declare both bits if your SDK supports both modes; declare just one if not. A typical hardware DP is `MANAGED`-only; sim-display is `MANUAL`-only. The `default_eye_tracking_mode` field picks which mode the runtime uses for sessions that don't explicitly opt in.

### Per-API DP factories

Only fill in the `create_dp_<api>` factories your SDK actually supports. NULL means "this graphics API isn't supported on this platform by this plug-in." The runtime gracefully falls back to the sim-display DP for any API your plug-in doesn't cover, so a vendor with only D3D11 + D3D12 weavers can still ship a useful plug-in — Vulkan / OpenGL / Metal apps just transparently use sim-display while DX apps use your weaver.

### Plug-in lifetime + threading

- `xrtPluginNegotiate` is called exactly once per process, at first `xrCreateInstance`.
- `probe()` is called on the `xrCreateInstance` hot path — keep it sub-millisecond. If your hardware probe takes longer, cache the result statically.
- `create_device`, `get_display_info`, and `set_pose_source` are called on the runtime's main thread.
- The DP factories (`create_dp_<api>`) are called on the compositor's session-create thread.
- `destroy()` is called on the runtime's main thread at instance teardown.

## 7. Testing your plug-in

### Smoke tests: cube apps in this repo

The runtime ships standalone cube apps under `test_apps/`. Install your plug-in (via your installer), then launch any of:

```
test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
test_apps\cube_handle_d3d12_win\build\cube_handle_d3d12_win.exe
test_apps\cube_handle_gl_win\build\cube_handle_gl_win.exe
test_apps\cube_handle_vk_win\build\cube_handle_vk_win.exe
```

The cube renders through your weaver per API. The per-process runtime log (`%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.<pid>_<ts>.log`) shows which plug-in claimed the system:

```
[try_load_one] plugin loader: active plug-in: id=<your-id> name='<your-name>' …
```

If your plug-in's `probe()` is declining when it shouldn't, set `XRT_PLUGIN_DEBUG=1` to log probe outcomes per registered plug-in.

### Full workspace test: DisplayXR Shell

Install the DisplayXR Shell ([`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases)), then launch a cube via the shell:

```
"C:\Program Files\DisplayXR\Runtime\displayxr-shell.exe" path\to\cube.exe
```

The shell drives the runtime through the OpenXR workspace extensions, so cube-in-shell exercises a more complete path than standalone cube.

### Optional: link to the vendor list

Add a short `docs/vendors/<your-vendor>.md` to the runtime repo via PR — a one-page summary of your plug-in (link to your repo, supported APIs, eye-tracking mode, installer link). The runtime maintainers will review + merge as a docs-only change.

## Related

- [Plug-in iface reference](../reference/xrt_plugin_iface.md) — per-method contract for `xrt_plugin_iface`
- [Plug-in discovery spec](../specs/runtime/plugin-discovery.md) — registry / JSON manifest formats, env var overrides
- [ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md) — why the runtime DLL holds zero vendor identifiers and how the aux import-library boundary works
- [`XR_EXT_display_info` spec](../specs/extensions/XR_EXT_display_info.md) — display info + eye-tracking mode contract
- [Eye tracking modes spec](../specs/vendor/eye-tracking-modes.md) — MANAGED vs MANUAL contract
- [Legacy in-tree integration model](../archive/vendor-integration-historical.md) — historical reference for the pre-#263 in-tree integration shape
