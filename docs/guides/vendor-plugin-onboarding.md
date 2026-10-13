# Vendor Plug-in Onboarding

This guide walks a new 3D-display vendor from zero to a shipping DisplayXR-compatible plug-in. After ADR-019 / #263 (May 2026), every vendor integration is an **external project**: you don't touch this repo, you ship a plug-in DLL from your own repo and an installer that registers it with the runtime at install time.

If you're maintaining a vendor integration that was historically in-tree (the way `drv_leia` was before #263), see the [legacy in-tree guide](../archive/vendor-integration-historical.md) — kept for historical context. Everything below assumes the post-#263 model.

> **Where to start**
> - [`DisplayXR/displayxr-vendor-template`](https://github.com/DisplayXR/displayxr-vendor-template) — **the recommended starting point.** A buildable, ABI-correct, **vendor-SDK-free** starter kit: it's a plug-in skeleton (`drv_example`, D3D11 + Vulkan stub weavers) that consumes only the public runtime ABI (pinned via FetchContent). Generate a repo from it, rename `drv_example` → `drv_<vendor>`, replace the stub weave. See [§3](#3-building-from-the-template).
> - [`DisplayXR/displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin) — a **full worked vendor integration** (the first vendor on the platform) to read for reference. It builds against a proprietary vendor SDK, so it's not a clean *starting* point — use it to see how a real weaver, eye-tracking listener, and installer come together.
> - `src/xrt/drivers/sim_display/sim_display_plugin.c` (in this repo) — the runtime's in-tree neutral reference. Smaller, no vendor SDK; useful as a minimal-shape reference (can't be forked as a standalone plug-in repo — that's what the template is for).
> - **Shipping a 2D→3D model rather than a display?** That is a conversion provider, a separate plug-in type with its own starter kit, [`displayxr-conversion-template`](https://github.com/DisplayXR/displayxr-conversion-template). Go straight to [§9](#9-conversion-providers--the-third-plug-in-type).

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

If your plug-in dynamically imports any of these, they resolve from the runtime directory, **not** from your own: your plug-in lives in a **sibling** directory (`C:\Program Files\DisplayXR\Plugins\<YourVendor>\`, next to `C:\Program Files\DisplayXR\Runtime\`, not inside it). The loader uses `LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)`, which searches your plug-in's directory first and then the standard order: modules the host already loaded (`DisplayXRClient.dll` always is, with whatever it imports from the runtime directory), the host executable's directory (only the service lives in the runtime directory), then `PATH`. The runtime installer does **not** add its directory to `PATH`, so an in-process app host finds only what is already loaded. Do not rely on `PATH` for your own vendor dependencies either: a long-lived service keeps the `PATH` it started with, so a directory added by a later install is invisible to it until it restarts. Resolve them by full path instead (e.g. `/DELAYLOAD` + a delay-load hook).

**Do NOT re-ship these DLLs in your plug-in installer** — bundling duplicates risks version skew across the install tree. The runtime owns their lifecycle.

## 3. Building from the template

The fastest path is to generate your repo from
[`displayxr-vendor-template`](https://github.com/DisplayXR/displayxr-vendor-template)
— a **vendor-SDK-free** starter kit that already builds against the pinned runtime
ABI (D3D11 + Vulkan stub weavers, installer, CI):

```bash
gh repo create <your-org>/displayxr-vendor-XXX-plugin \
    --template DisplayXR/displayxr-vendor-template --private --clone
cd displayxr-vendor-XXX-plugin
scripts\build-windows.bat all          # builds the example plug-in as-is, no SDK
```

That first build works with **no vendor SDK** — confirm it's green before you
touch anything, so you have a known-good baseline. Then make it yours:

1. **Rename `src/drv_example/` → `src/drv_<vendor>/`** and replace the stub
   contents with your SDK glue: device init, hardware probe, weaver per-API, eye
   tracking listener.
2. **Rewrite `src/drv_<vendor>/example_plugin.c`** against `xrt_plugin_iface` —
   see the [iface reference](../reference/xrt_plugin_iface.md) for which callbacks
   are required vs optional and what each one must produce. The stub already
   returns a valid vtable; swap the bodies.
3. **Replace the stub weave** in `example_processor_d3d11.cpp` /
   `example_processor_vk.c` with your real `process_atlas` — this is the one
   function that makes the plug-in yours.
4. **Update `CMakeLists.txt`** — rename the `DisplayXR-ExampleVendor` target to
   `DisplayXR-<YourVendor>`. The `XRT_USING_RUNTIME_DLL` + linker settings and the
   `DXR_RUNTIME_GIT_TAG` pin stay as-is (bump the pin only when you adopt a newer
   runtime ABI major).
5. **Update `installer/DisplayXRExampleVendorInstaller.nsi`** — change the registry
   `<id>` to your own, the install dir from `ExampleVendor` to your vendor name, and
   the `DisplayName` / `Vendor` strings.
6. **Add your SDK to `scripts/build-windows.bat`** if you need one — the template
   builds with none, so add the download/`<VENDOR>_SDKROOT` wiring only when your
   weaver depends on a proprietary library.

The runtime `FetchContent` setup at the top of `CMakeLists.txt` does NOT need
editing — your plug-in consumes the same runtime ABI. (Read
[`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin) as a
full worked example of steps 1–3 against a real SDK.)

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
| `UninstallString` | `REG_SZ` | Optional, informational: quoted full path to your `Uninstall.exe`. The runtime never runs it — your plug-in is removed only by its own uninstaller (Add/Remove Programs) |
| `Enabled` | `REG_DWORD` | **Do not write it.** 0 = the runtime never loads your plug-in, in any scan; it is the user's / administrator's switch (`displayxr-cli dp enable\|disable <id>`, per user in HKCU by default — which overrides your HKLM row — or `--machine`). Your installer leaves it alone on install and upgrade, and never touches HKCU on uninstall (spec §2.1 / §5, ADR-054 N3b) |

`<your-id>` is a short kebab-case identifier matching `iface->id` (the string the plug-in returns from `xrtPluginNegotiate`). The Leia plug-in uses `leia-sr`; sim-display uses `sim-display`.

`displayxr-cli dp list [--json]` shows your row as the loader sees it — ProbeOrder, `Enabled` and where it came from, the `PreferredPlugin` override, the provenance verdict (signed / allowlisted / unverified, ADR-054 N4b) and `*` on the plug-in the loader would select — without loading any DLL; `dp use <id>` pins yours ahead of ProbeOrder for the current user (`--machine` for HKLM). A running service picks up a change to either value at once (a secondary screen re-probes now, the primary at the next session; `plugin-discovery.md` §4.2a).

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

An optional boolean `enabled` (`false` = the runtime never loads the plug-in) is the POSIX switch; there is no per-user `Enabled` store there, so `dp enable|disable` points at this field. Ship your manifest without it.

The full discovery contract — registry layout, JSON schema, env-var overrides, fallback search order — is documented in [`docs/specs/runtime/plugin-discovery.md`](../specs/runtime/plugin-discovery.md).

### Settings — the one options contract (ADR-054 decision 7)

Your plug-in's own knobs (a debug overlay, a tracker tuning, a vendor-SDK toggle) go through **one** contract, the same for every component class, and you read them **only through the host iface** — never `getenv`, never an ad-hoc registry key of your own. In the service or a satellite the environment is the host's, not the user's shell, and only this path is reachable by the CLI and the Dashboard.

- **Read:** `host->get_setting(iface->id, "KEY", buf, cap)` (`xrt_plugin_host_iface::get_setting`; `#ifdef XRT_PLUGIN_HOST_HAS_GET_SETTING`, NULL-check it — an older runtime leaves it NULL; the pointer lives for the process, so copy it at negotiate and read at init, then cache). The runtime resolves `DXR_DP_<ID>_<KEY>` in the hosting process's environment (always wins) → the per-user store → your row's `Settings\<KEY>` (HKLM, or the POSIX manifest's `settings` object) → nothing, in which case keep your compiled default. Values are strings; you parse.
- **Declare:** fill the appended `iface->describe_settings` slot (`#ifdef XRT_PLUGIN_IFACE_HAS_DESCRIBE_SETTINGS`) with a `static const struct xrt_component_setting_desc[]` of `{key, type, default, help}` so `displayxr-cli dp options <your-id>` lists them with their effective value and source. POSIX manifests may carry the same as a `settings_schema` array.
- **Write (users / IT):** `displayxr-cli dp set <your-id> KEY value [--machine]` / `dp unset`. Your installer may seed `Settings\<KEY>` on your HKLM row (a `REG_SZ`); it never writes HKCU.

`drivers/sim_display/sim_display_settings.c` is the worked example (36 keys behind one shim, cached per process), and the spec is [`plugin-discovery.md` §2.3](../specs/runtime/plugin-discovery.md). The in-tree reference plug-ins are guarded by CI against any `getenv` outside that shim; copy the shape.

### `ProbeOrder` convention

| Range | Meaning | Examples |
|---|---|---|
| 1–99 | Vendor with hardware probe — claims the system when its hardware is present, declines cleanly otherwise | Leia SR = 50 |
| 100–199 | Reserved | — |
| 200–254 | Vendor-neutral fallback that always claims if reached | sim-display = 200 |

Pick a value in 1–99 if your `probe()` consults a vendor SDK to detect connected hardware and returns `XRT_ERROR_PROBER_NOT_SUPPORTED` cleanly when absent. Pick something close to 200 if your plug-in is meant to handle "any machine, no specific hardware" cases (rare for vendor plug-ins).

Within a range, pick a value **distinct from every plug-in or provider you expect to coexist with** — lower wins, and equal values are broken deterministically by id (`strcmp`), which is an alphabetical accident, not a decision. The input-provider loader logs a WARN naming both ids when it has to do that (#1466). On the input side the in-tree Ultraleap provider ships at **50** and `sim_input` at **200**.

## 5. Installer contract

The vendor plug-in installer is independent of the runtime installer — it has its own version cadence, its own release flow, its own NSIS script (Windows) or `.pkg` builder (macOS). **Nothing may depend on the order in which the runtime, your plug-in and your vendor platform software are installed or uninstalled** (epic [#1803](https://github.com/DisplayXR/displayxr-runtime/issues/1803)).

### Your plug-in owns its registration

The `HKLM\Software\DisplayXR\DisplayProcessors\<your-id>` subkey is the runtime's public contract, and it is **yours**: your installer writes it, your uninstaller deletes it, and nobody else touches it.

- **The runtime never removes it.** The runtime installer writes and deletes only its own `sim-display` subkey; it never runs your uninstaller, never deletes your subkey, and never deletes files outside its own directory. Uninstalling the runtime leaves your plug-in installed and registered; reinstalling the runtime picks it up again without reinstalling the plug-in.
- **You may be installed before the runtime.** Your subkey can pre-exist the runtime; the runtime adopts any registered plug-in it finds. Your installer must therefore **not** require the runtime: install your files, write your subkey (`SetRegView 64` first — the `DisplayProcessors` key may not exist yet; `WriteRegStr` creates it), and finish. Your plug-in is simply not loaded until a runtime is present, because only the runtime loads it.
- **Version floor only when a runtime is present.** If you need a minimum runtime version, check it only when `HKLM\Software\DisplayXR\Runtime` has an `InstallPath`; with no runtime, install anyway.
- **Do not require your vendor platform software either.** A plug-in must load without it and decline (or report its state) cleanly, so that the platform software can be installed before or after you.

### Stopping and starting the service

`displayxr-service.exe` maps every registered plug-in DLL, so your installer and uninstaller must release it before replacing or deleting your files — and must never leave it running elevated:

- Release your files with **Windows Restart Manager** (`RmStartSession` / `RmRegisterResources` on your DLLs / `RmGetList` / `RmShutdown(RmForceShutdown)` / `RmRestart`). The service registers for restart and exits on the close query. This is generic Windows; no DisplayXR-specific code.
- If you must start the service yourself (Restart Manager found nothing to restart and a runtime is present), start it **through `explorer.exe`** (`Exec 'explorer.exe "<runtime InstallPath>\displayxr-service.exe"'`), never with a plain `Exec` from your elevated installer: an elevated service cannot be reached by normal-integrity apps.
- If no runtime is installed, start nothing.

### Install dir convention

`C:\Program Files\DisplayXR\Plugins\<YourVendor>\` (NSIS: `$PROGRAMFILES64\DisplayXR\Plugins\<YourVendor>`) — a **sibling** of the runtime's `C:\Program Files\DisplayXR\Runtime\`, not a subdirectory of it. The runtime uninstaller only ever removes `C:\Program Files\DisplayXR` non-recursively and only when it is empty, so your directory survives it.

Drop the following into that directory:
- Your plug-in DLL (`DisplayXR-<YourVendor>.dll`)
- Any vendor SDK runtime DLLs you license to redistribute and that your platform software does not install
- An `Uninstall.exe` (NSIS generates this automatically)

Do **not** drop anything into `C:\Program Files\DisplayXR\Runtime\`. That's runtime-owned and is deleted with the runtime.

### Uninstall

Your uninstaller is run by the user (Add/Remove Programs) or by a bundle — never by the runtime. It must:

- Honor `/S` (silent mode) and never show a modal under it (`MessageBox ... /SD <default>`).
- Work with or without the runtime installed.
- Release your DLL (Restart Manager, as above) instead of leaving it mapped, then delete your files.
- Delete your own `<your-id>` subkey; leave every other subkey, and the runtime's files, alone.

`UninstallString` under your subkey is optional and informational (it mirrors your Add/Remove Programs entry); the runtime does not execute it.

### Provenance: sign it, or allowlist it (ADR-054 N4b)

Since PR B the runtime's provenance gate **enforces by default on Windows**: at
load, every plug-in DLL must be **Authenticode-signed** (any trusted root — no
publisher pinning) or have its **SHA-256 under
`HKLM\Software\DisplayXR\Trust\Allowlist`** (value name = lowercase hex digest,
data = a note). Otherwise the loader logs `REFUSING unverified binary …` and falls
through to the next display processor — your users see `sim-display`, not you.
A path is never a provenance on Windows: living under `Program Files` proves
nothing.

- **Signed releases need nothing else.** This is the expected shape of a shipping
  vendor plug-in.
- **If your installer can ship an unsigned build** (CI artifacts, developer
  installers, a release your signing step missed), it must write the row for the
  exact bytes it installs — compute the digest at packaging time, never by hand —
  and its uninstaller must delete the same row and only that row. The runtime
  installer does this for its own sim-display DLL: `installer/gen_allowlist_nsh.cmake`
  generates `!macro DXR_ALLOWLIST_WRITE` / `DXR_ALLOWLIST_REMOVE` from the packed
  file, and `DisplayXRInstaller.nsi` inserts them next to the registration. Copy
  that pattern.
- **Dev boxes:** `scripts\register_dev_plugin.bat leia <path>` registers *and*
  allowlists; `DXR_ALLOW_DEV_PLUGIN_PATHS=1` still admits a build-tree DLL from a
  normal prompt, never from an elevated process (the installed service after an
  installer restarted it is elevated).
- **Escape hatch, one release:** `HKLM\Software\DisplayXR\Trust` value `Enforce`
  (DWORD) = 0 returns the gate to audit mode on that box. Do not ship that in an
  installer.

Contract text: `docs/specs/runtime/plugin-discovery.md` §4.3.

### Reference installer

[`displayxr-vendor-template`](https://github.com/DisplayXR/displayxr-vendor-template)'s `installer/` is the starting point. The full worked integration linked at the top of this guide is being brought in line with this contract under [#1803](https://github.com/DisplayXR/displayxr-runtime/issues/1803); where the two differ, this section wins.

## 6. Vendor-specific concerns

### SDK redistribution

If your vendor SDK's license allows redistribution, bundle the runtime DLLs in your installer. The end-user's experience is one installer click.

If it doesn't, the user installs your vendor platform software separately — **before or after** your plug-in, in either order. Do not make it an installer prerequisite: your plug-in must load without it (resolve its DLLs lazily, e.g. `/DELAYLOAD` + a delay-load hook that finds them by full path from the platform's own registry entry) and decline cleanly until it is present.

### Eye-tracking mode

`xrt_plugin_display_info.supported_eye_tracking_modes` is a bitmask:
- bit 0 (`0x1`) — `MANAGED` (vendor SDK predicts eye positions; the runtime queries them per frame)
- bit 1 (`0x2`) — `MANUAL` (the app submits eye positions via `XR_DXR_display_info`)

Declare both bits if your SDK supports both modes; declare just one if not. A typical hardware DP is `MANAGED`-only. Declare `0` if your display has no eye tracker at all — sim-display does this (its positions are nominal, not tracked). The `default_eye_tracking_mode` field picks which mode the runtime uses for sessions that don't explicitly opt in.

### Per-mode tracking capability (`mode_flags`, ABI v3 / #441)

Every `xrt_rendering_mode` your `create_device` device exposes carries a `mode_flags`
bitmask in the **vendor-provided (MUST set)** section:

- `XRT_RENDERING_MODE_FLAG_HAS_TRACKING` (`1u << 0`) — set on each mode that consumes live
  eye tracking (typically your 3D modes; optionally a "2D tracked" mode where content is
  flat but the viewer remains tracked).
- Zero-init = untracked = the safe default; `reserved[]` MUST stay zeroed.

The runtime forces the app-visible `isTracking` to false whenever the active mode is
untracked, and surfaces the flag to apps via the chained
`XrDisplayRenderingModeTrackingInfoDXR` (header v14). **Consistency rule:**
`supported_eye_tracking_modes != 0` ⇔ at least one mode sets `HAS_TRACKING` — the runtime
logs a one-shot WARN at init if violated. Full contract:
`docs/specs/vendor/eye-tracking-modes.md`.

### Per-API DP factories

Only fill in the `create_dp_<api>` factories your SDK actually supports. NULL means "this graphics API isn't supported on this platform by this plug-in." The runtime gracefully falls back to the sim-display DP for any API your plug-in doesn't cover, so a vendor with only D3D11 + D3D12 weavers can still ship a useful plug-in — Vulkan / OpenGL / Metal apps just transparently use sim-display while DX apps use your weaver.

### Displays that weave in hardware (FPGA / ASIC)

Everything above assumes your plug-in runs the lens math on the GPU. If your
display does the weave **in its own silicon** — an FPGA or ASIC on the scaler
board, fed an ordinary video frame — your plug-in is *smaller*, not bigger, but
it has to declare one thing the GPU-weaving shape never had to.

**`process_atlas` becomes a repack, not a weave.** You are not producing final
subpixels; you are producing the packed frame your chip expects, and the chip
weaves it during scanout. The layout is not a new concept — it is the tile
geometry you already declare per rendering mode:

| Chip input | `view_count` | `tile_columns` × `tile_rows` | `view_scale_x`, `view_scale_y` |
|---|---|---|---|
| Side-by-side, half width | 2 | 2 × 1 | 0.5, 1.0 |
| Top-and-bottom, half height | 2 | 1 × 2 | 1.0, 0.5 |
| Full-resolution frame packing | 2 | 1 × 2 | 1.0, 1.0 (needs a double-height video timing — see below) |
| N-view quilt | N ≤ 8 | any grid | 1/cols, 1/rows |

`XRT_MAX_VIEWS` is **8**; it is embedded by value in `xrt_device`, so a chip
wanting more views than that is a runtime ABI change, not a plug-in.

The reference implementation is already in the tree: `sim_display`'s
`shaders/squeezed_sbs.frag` (2 × 1), `shaders/sbs.frag` (1 × 2) and
`shaders/quad.frag` (2 × 2 quilt) are exactly these repacks.

**Declare the scale ONCE.** The numbers in that table are your mode table's
`view_scale_x/y`, and they — not `xrt_plugin_display_info::recommended_view_scale_*`
— are the single source of truth for view, tile and atlas sizing. Leave the
`recommended_view_scale_*` scalar at `0` (the runtime then derives it from your
modes, which is what the template does) or derive it from the same numbers as the
active 3D mode; never compute it a second time. They feed different consumers —
the scalar sizes `XrViewConfigurationView.recommended*`, the mode table sizes the
tiles, the atlas and the compositor grid — so a scalar that disagrees with (and is
larger than) your mode's scale declares an atlas too small to hold your own tiles.
Full contract: [`xrt_plugin_iface.md`](../reference/xrt_plugin_iface.md#get_display_info).

**A free optimization.** Declare `2D = 1×1 @ 1.0,1.0` and `3D = 2×1 @ 0.5,1.0`
and the worst-case swapchain envelope becomes `W × H`, which *both* modes fill
exactly — so `u_tiling_can_zero_copy()` fires full-screen in both and the app's
swapchain reaches your DP with no crop at all. A `0.5 × 0.5` 3D mode never
achieves this. See [ADR-030](../adr/ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md).

#### Declare your weave scope — this is the required part

The runtime cannot infer how much of the panel your chip transforms, and it
changes what presentations can be correct. Implement `get_scanout_caps` on each
`create_dp_<api>` vtable you ship (`xrt/xrt_display_scanout.h`):

| `weave_scope` | Your chip | Windowed apps |
|---|---|---|
| `XRT_DP_WEAVE_SCOPE_CANVAS` | GPU weaver — you produce final pixels for the canvas you were handed | native; nothing to do |
| `XRT_DP_WEAVE_SCOPE_REGION` | takes a "weave only this rect, pass the rest through" descriptor | native — see below |
| `XRT_DP_WEAVE_SCOPE_SCANOUT` | transforms the whole incoming frame; no rect | require a panel-scoped presentation |

Leaving the slot NULL means `CANVAS`, which is why no existing plug-in needs to
change, rebuild, or bump ABI. Declare honestly: `SCANOUT` is what makes the
runtime say plainly in the log that a windowed session cannot resolve, instead
of shipping a frame your chip will shred into crosstalk with nothing anywhere
to explain it.

**A `REGION` chip is an [ADR-027](../adr/ADR-027-display-zones.md) zones DP.**
That is the whole implementation, and it needs no other new mechanism:

- `get_local_zone_caps` → `zone_grid_width/height` is your chip's addressable
  region granularity;
- `publish_local_zone_mask` → the runtime hands you a per-pixel wish mask
  **already anchored in panel pixels** (`screen_x/y/w/h`); quantise to your
  cells and push the region over your sideband channel;
- `process_atlas` → write the packed frame into the canvas, which is already at
  window resolution with your declared tile geometry;
- `snap_window_rect` → return the nearest placement your lens phase / cell grid
  actually supports. The runtime calls it for placement, drag and resize, and
  for present-owning apps through `XR_DXR_weave`. It exists on **both** the
  D3D11 variant (slot 18) and the Vulkan one (appended, #1588) with the same
  signature, so one body serves both. Snap on the invariant your lattice
  actually has — with a slant that is `x + slant·y`, not `x` — and expect to be
  asked once per pointer-motion event, so keep it arithmetic. **Use only the
  displacement** (`target - origin`), snap that, and re-add the origin: the
  runtime does not promise any particular absolute frame, only that both points
  are in the same one, and anchoring to the drag origin is what makes that safe.
  Preserve the phase the window had rather than searching for a better one — a
  drag must not re-phase mid-motion.

**A `SCANOUT` chip needs the runtime to own the whole panel.** That is the
fullscreen, panel-native composition the service compositor already performs
for a workspace: one fullscreen window at native panel resolution, a combined
atlas at native display dims with per-window slot rects, one present. Under a
workspace controller a scanout-scoped plug-in works with no further runtime
support. What it cannot do is float a single app over the Windows desktop —
there is no supported way to make the OS compositor emit a packed scanout, and
your DP is not handed the desktop's pixels.

#### Telling the panel a frame is 3D

Do not plan on HDMI signalling. The 1.4a Vendor-Specific InfoFrame carries a
`3D_Structure` field, but no userspace process can emit one — it is driver
territory, and consumer GPU drivers no longer expose it. Working options, in
rough order of how often they are used:

1. **Sideband command** — USB HID / CDC-serial / I²C to the scaler board, or
   DDC-CI. Drive it from `request_display_mode()` and report the result from
   `get_hardware_3d_state()`.
2. **In-band watermark** — a reserved row or corner block of magic pixels your
   FPGA sniffs. Free to implement: your DP owns every pixel of the output
   target during `process_atlas`.
3. **A dedicated EDID timing** that is implicitly 3D.

The mode flip is already choreographed: the runtime broadcasts
`XrEventDataRenderingModeChangedDXR`, holds a curtain while clients ack, and
polls `get_hardware_3d_state` through your hardware transition before lifting
it. Your sideband command slots into that; you do not need your own.

#### What the runtime will not do for you

- **Set the display timing.** Nothing in the runtime changes display modes. If
  your 3D mode needs a special timing (frame packing at double height, a
  different refresh), your installer or vendor service establishes it.
- **Give you depth.** `process_atlas` is colour-only; there is no depth surface
  anywhere in the DP ABI. A chip that synthesises views from 2D + depth has no
  path today — open an issue before you build against one.
- **Present more than once per frame.** A frame-sequential chip that needs L and
  R in consecutive fields has no way to request a second present. Running the
  compositor at 2× and alternating in `process_atlas` works, but there is no
  parity or genlock signal — treat it as a prototype, not a product.

#### Testing before hardware exists

`sim_display` doubles for all of it, on any machine:

```bat
set SIM_DISPLAY_FORCE_MODE=3        REM pin Squeezed SBS — a real 2x1 packed frame
set SIM_DISPLAY_WEAVE_SCOPE=region  REM or scanout: claim a scope, exercise the routing
```

`SIM_DISPLAY_WEAVE_SCOPE` changes only what the DP *claims*, which is the
surface under test. Grep your app's log for `DP weave scope:` to see what the
runtime read, and for the follow-up warning when a scanout-scoped DP lands on a
windowed path.

### Optional: per-screen status for the dashboard

Implement `get_screen_status` (ADR-051 D2, `#ifdef XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS`) to
give each screen you claim a row in `displayxr-cli status` and the DisplayXR Dashboard: ready /
verified / calibrated, tracker and lens state, model and serial, up to eight warnings shown
verbatim, and an optional command line that opens your own dashboard on that screen. It must be
passive — no tracker / lens / display handle, no per-call SDK instance, < 20 ms, pollable every
2 s — see [`xrt_plugin_iface.md`](../reference/xrt_plugin_iface.md#get_screen_status-display-dashboard-adr-051-d2).
Leave it NULL and the dashboard shows "no vendor status".

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
test_apps\handle\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
test_apps\handle\cube_handle_d3d12_win\build\cube_handle_d3d12_win.exe
test_apps\handle\cube_handle_gl_win\build\cube_handle_gl_win.exe
test_apps\handle\cube_handle_vk_win\build\cube_handle_vk_win.exe
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

Document your implementation internals in your plug-in repo's `docs/`, then add a row to the table in the runtime repo's `docs/vendors/README.md` via PR — vendor name, link to your repo + docs, supported APIs. The runtime maintainers will review + merge as a docs-only change. (Example: [displayxr-leia-plugin/docs/](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/docs/README.md).)

## 8. Input providers — the other plug-in type

Everything above is the **display** side. If what you ship is a *tracking* system — controllers, hands, a camera-fused pose — you implement a different contract: [`xrt_input_plugin_iface`](../reference/xrt_input_plugin_iface.md), entry point `xrtInputPluginNegotiate`, discovery root `HKLM\Software\DisplayXR\InputProviders` (POSIX: `NNN-<name>-input-provider.json` manifests). Same ProbeOrder convention, same ABI discipline, same installer shape — a different vtable, versioned and shipped independently, because a tracking vendor is not a display vendor ([ADR-034](../adr/ADR-034-input-provider-plugins.md)). Full contract: the [input-provider discovery spec](../specs/runtime/input-provider-discovery.md).

### Adding navigation

A provider may also drive the **rig** — the virtual camera the user flies around with, which the runtime's own WASD / mouse-look fly camera drives otherwise. This is a role you win by presence, exactly like the hands; it is not a hook you bind and not a session you open. Design: [ADR-034 *Amendment 4*](../adr/ADR-034-input-provider-plugins.md#amendment-4--navigation-is-a-role-composed-by-the-runtime-2026-09-11). Normative rules: discovery spec [§4b](../specs/runtime/input-provider-discovery.md).

The whole checklist:

1. **One extra device.** In `create_devices`, return one more `xrt_device` with `device_type = XRT_DEVICE_TYPE_NAVIGATION`, alongside your controllers. Never more than one — the runtime keeps the first and warns about the rest. A navigation-only provider (no controllers) is legal.
2. **One required input: `XRT_INPUT_GENERIC_NAVIGATION_POSE`.** `get_tracked_pose` on it returns **N(t): the absolute pose of the rig in your own navigation frame F** — right-handed, +Y up, −Z forward, metres, gravity-aligned. F's origin is private to you and is never interpreted; the runtime aligns to it once, at an epoch, and holds the result. Not a delta, not a viewer/eye pose, never parallaxed. Serve the requested timestamp from your latched publication.
3. **Validity is your authority switch.** Set `POSITION_VALID | ORIENTATION_VALID` while you have navigation authority; clear them to mean *hold the rig* (optical loss, a rest state, nothing held). The `*_TRACKED` bits are ignored here. Do **not** express the same condition through `get_presence` — presence is transport ("am I attached?"), and flipping it hands the rig to the fly camera and back, which the user sees as a jump.
4. **Optional reset: `XRT_INPUT_GENERIC_NAVIGATION_RECENTER`.** A *durable level with a timestamp*: `value.boolean = true`, `timestamp` = the time of your most recent reset, **never cleared**. Publish the new timestamp and the post-reset N in the same atomic publication. A pulse does not work — `xrSyncActions` sweeps `update_inputs` over every device and would eat it.
5. **Publish your controllers rig-local.** Set their `tracking_origin->type = XRT_TRACKING_TYPE_RIG_LOCAL` — poses relative to the display plane (origin at the display centre, +X right, +Y up, +Z toward the viewer, metres). The runtime anchors that origin at the initial rig, so you need no standing height, no mount offset and no knowledge of where the rig is. If your solver already emits product poses `C_W` in its own navigated world, publish `L = inv(P(t)) ∘ C_W` with `N(t) = P(t)` and the runtime's single alignment maps your whole world.
6. **Never pre-compensate.** Do not compose your navigation (or its inverse) into the controller poses you publish. The runtime applies its own rig delta to every provider device, so a pre-compensated pose subtracts a transform the runtime is about to add. It fails as slow drift, not as an error. This is the single most expensive mistake on this contract.
7. **Geometry, if your solver needs it.** `host->get_display_geometry(&geo)` gives panel width/height and the **nominal** viewer position. The host pointer you get at `xrtInputPluginNegotiate` is process-lifetime storage and may be retained; the *data* is only ready from your `create_devices` call onward, and an earlier call returns `XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY` and leaves your struct untouched. These are nominal numbers for sizing a working volume — never a head pose, never the tracked eyes.
8. **Keep all gesture policy on your side.** Thresholds, momentum, rest detection, arm-stretch mapping, which button starts a drag: yours. The runtime owns only alignment, continuity across handovers, recenter and composition — and will not grow an opinion about any of the former.

**Testing it, before and without your hardware:**

- Build the runtime from source and register the in-tree `sim_input` provider (`scripts\register_dev_plugin.bat input sim`, elevated). It carries a scripted navigation device behind `DXR_SIM_INPUT` — see the discovery spec §5 — so you can watch the role walk, a handover and a recenter happen with nothing plugged in, and compare your provider's behaviour against a known-good one at a different ProbeOrder.
- Register your provider at a **lower** ProbeOrder than `sim_input` (200), and at a value distinct from every provider you expect to coexist with — the in-tree Ultraleap provider ships at 50, and an equal value is broken by id and logged, not by your intent. Then confirm you win the rig while present and that it falls back — first to `sim_input`, then to the fly camera — as each goes absent.
- `displayxr-cli selftest` runs the real loader and arbitration path with no GPU, window or app: use it to check your DLL loads, ABI-passes, probes, and that the roles land where you expect. Provider *absence* never fails the check — only a provider that could not be dispatched does.
- `displayxr-cli input list [--json]` enumerates registered providers (ProbeOrder, Enabled + its source, the PreferredPlugin override, the one ranked first) and the ForceQwerty state without loading any DLL; `input enable|disable <id>` and `input use <id>` / `input reset` write the per-user `Enabled` / `PreferredPlugin` values the loader honours (`--machine` for HKLM; ADR-054 verbs, N3a); `displayxr-cli input haptic-test` re-resolves roles every iteration and prints each `generation_id` change, so it doubles as a live view of arbitration flipping.
- Run the same checks a second time with `XRT_FORCE_MODE=ipc` and a running `displayxr-service.exe`. Roles, the rig and the composer all live service-side there, and the durable-recenter rule exists precisely because that path polls differently.
- `HKLM\Software\DisplayXR\Input\ForceQwerty = 1` skips input providers entirely — the fastest way to prove a symptom is yours.

## 9. Conversion providers — the third plug-in type

Everything from §1 to §7 is the **display** side, and §8 the tracking side. If what you ship is a **2D→3D model** — depth from an image or a video, stereo or N views from one image, disparity from a stereo pair, Gaussian splats from a photo — you write a **conversion provider**: contract [`xrt_conversion_plugin.h`](../../src/xrt/include/xrt/xrt_conversion_plugin.h), entry point `xrtConversionPluginNegotiate`, discovery root `HKLM\Software\DisplayXR\ConversionProviders` (POSIX: `NNN-<id>-conversion-provider.json` manifests). A model vendor is not a display vendor: there is no weaver, no device, no eye tracking and no display-processor vtable here, and you can skip §3–§7 above. The runtime owns every thread, queue, policy and app-facing API (apps reach you through [`XR_DXR_lift`](../specs/extensions/XR_DXR_lift.md) and never learn which provider served them); your provider does **pixels and units only** ([ADR-053](../adr/ADR-053-conversion-providers.md)). The normative contract is the [conversion-provider discovery spec](../specs/runtime/conversion-provider-discovery.md); this section is the walk-through.

> **Where to start:** [`DisplayXR/displayxr-conversion-template`](https://github.com/DisplayXR/displayxr-conversion-template) — a buildable, dependency-free provider skeleton: one CPU `DEPTH_MONO` capability that returns a ramp, a settings example, a **fake host** that drives the provider through the same call sequence the runtime uses (no runtime install needed), dev-box registration scripts for Windows and POSIX, and CI on Windows, Linux and macOS. It builds with plain CMake against a vendored copy of the header. The in-tree `src/xrt/drivers/sim_conversion/` (every task, scriptable states) is the full reference; it cannot be forked as a standalone repository, which is what the template is for.

### 9.1 What you ship

1. **A provider DLL** (`.dll` / `.so` / `.dylib`), e.g. `DisplayXR-<Vendor>Conversion.dll`, exporting exactly one C symbol, `xrtConversionPluginNegotiate`. Its ABI major, `XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT` (1), is independent of the display (`XRT_PLUGIN_API_VERSION_CURRENT`) and input ABIs; the loader rejects a different major before calling anything ([ADR-020](../adr/ADR-020-plugin-abi-compatibility-policy.md)).
2. **An installer** that drops it under `C:\Program Files\DisplayXR\Plugins\<Vendor>\` and writes the `ConversionProviders\<id>` row (§9.4), honouring the update contract in §9.8.

**Ship a separate DLL, even if you are also a display vendor.** One DLL may export both `xrtPluginNegotiate` and `xrtConversionPluginNegotiate` and be registered under both roots, but its display half keeps it mapped in `displayxr-service.exe`, so every update of it still closes the service and drops every client session. A separate provider DLL runs in its own supervised `displayxr-conversion-host.exe` and updates with no session lost. A display vendor whose plug-in still serves lift through the frozen display-processor lift slots registers its new provider with `Supersedes = dp-lift`, which hides the runtime's legacy adapter row.

The header is self-contained: `xrt/xrt_conversion_plugin.h` + `xrt/xrt_results.h` + the C standard headers. Linking `DisplayXRClient.lib` is optional and buys only the [ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md) logging surface (`u_log_*`), which both hosts export; a provider that does not log through it needs no runtime library at all.

### 9.2 The ABI in one page

| Slot | Thread / class | What it does |
|---|---|---|
| `xrtConversionPluginNegotiate` | the loader, once | store the host iface; return your iface and your compiled `XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT` |
| `get_provider_status` | any, concurrent | are your dependencies there? (ADR-045 shape: the inference runtime, a driver — never a display) |
| `probe` | control, ≤ 100 ms | create the instance — **no model load, no network**. `XRT_ERROR_PROBER_NOT_SUPPORTED` is a clean decline |
| `describe_cap` | any, concurrent | **one capability per call**; `out == NULL` returns the count. The runtime zero-fills the struct and sets `struct_size`; write only within it |
| `get_generation` | any, concurrent | changes whenever `describe_cap` would answer differently |
| `get_state` | any, concurrent, µs | readiness per `cap_id`, from memory, allocation-free (§9.6) |
| `warm` / `evict` | control | load / unload the model. `warm` may block for as long as it takes, polling `is_cancelled` at least every 250 ms and publishing progress through `get_state` |
| `stream_get_output_desc` | any, concurrent, pure | the target extents and formats for a given input extent |
| `stream_create` / `stream_convert` / `stream_destroy` | a stream worker | the per-frame lane: the runtime does latest-wins, priorities and timestamps; you do one inference per `stream_convert`, writing into **runtime-allocated** targets sized by your `stream_get_output_desc` |
| `job_execute` | the job worker | the one-shot lane (photo → splats, seconds to minutes): decoded pixels in, blobs out through runtime sinks, progress + cooperative cancel |
| `destroy` | control | **may never be called**: the host can be killed at any instant |

**Threading: one call in flight per provider** by default, so a provider on a single-threaded library needs no locks; only the five read-only slots above marked "concurrent" may overlap anything, and they answer from memory. Overlap is opt-in per capability (`max_concurrent`, `XRT_CONV_CAP_LANES_CONCURRENT`, `XRT_CONV_CAP_WARM_CONCURRENT`). No slot is ever called from a render, compositor or IPC thread. Full table: discovery spec §2.3.

**Struct rules** (discovery spec §2.2): every top-level struct starts with `struct_size` and is append-only; read and write only what `struct_size` covers (`XRT_CONV_COVERS`). The embedded structs (`xrt_conv_image`, `xrt_conv_sync`, `xrt_conv_depth_info`, `xrt_conv_views_info`, `xrt_conv_splat_info`, `xrt_conv_stereo_calibration`) are **frozen**, pinned by `sizeof` asserts in the header. Strings are fixed-size arrays; enumerations are `uint32_t` + `#define`s.

**A capability** is one (task × model × backend × device): `cap_id` (non-zero, yours, stable for the instance's life), `task` (`XRT_CONV_TASK_*`), lanes, input kinds, outputs, flags, the memory and sync kinds you import, limits the runtime enforces (`min_views` / `max_views` ≤ 8, `max_streams`, `max_input_edge`), the output contract (depth kind, measure, intrinsics source), licence class (§9.7), and cost estimates. Two rules are hard: every `VIEWS_*` capability sets `XRT_CONV_CAP_EXPLICIT_VIEWPOINTS` (selection refuses one that does not), and every capability imports `XRT_CONV_MEM_CPU` at least on the job lane — the runtime copies any other memory kind to CPU for you, so a CPU-only provider is a complete provider. A provider whose library fixes one model per process declares **one** capability whose variant follows the quality hint, not several mutually exclusive ones.

### 9.3 The hosting-agnostic rules

The same DLL runs in the satellite host (the default for every registered provider) or, as a debugging escape hatch, inside the service (`Hosting=in-service`). It must not be able to tell them apart. The full table is discovery spec §2.4; the ones that bite:

- **No runtime device.** Create your own (D3D11/12, Vulkan, Metal, an inference session) on the adapter `host->get_adapter_hint` names, and report it in `capability.device_luid`. Every image you see is runtime-allocated; you **import, never export**, and cache imports by `import_key`.
- **No windows on non-pumping threads**, no `SetDllDirectory` / `SetCurrentDirectory` / `putenv` / `exit` / signal handlers / DPI changes. A dependency that insists on a hidden window gets `HWND_MESSAGE` or a pumping thread; otherwise Restart Manager and logoff hang.
- **Load dependencies by absolute path** and **never statically import your inference runtime**: the provider DLL must load (and report `ABSENT`) when that runtime is missing. Delay-load it, or `LoadLibraryExW` it by full path with backslashes.
- **Configuration only through `host->get_setting`** (§9.5), never `getenv`: in a satellite the environment is the host's.
- **Network only inside `warm`**, only on capabilities flagged `XRT_CONV_CAP_NEEDS_NETWORK_TO_WARM`; never in `stream_convert` or `job_execute`. Model bytes from the shared store come through `host->model_resolve` / `model_request`; you never write the store.
- **Medium integrity, interactive session**; licence activation must work non-elevated.
- **No policy.** Viewpoints, convergence, the depth budget and the cursor are the runtime's. Render exactly `params.viewpoints_xyz`; when `params.convergence_mode` is `DECODED`, put exactly that plane at zero parallax, in the decoded units of your own depth output. You may *suggest* a convergence or a strength; policy ignores the suggestion unless your provider has been validated against the runtime's stereo ground-truth corpus.
- **Honest units.** Report the depth kind you actually produce (metric, metric-inverse, affine depth, affine inverse depth, affine disparity, pixel disparity), the per-frame range **before** any normalisation, and intrinsics with their source. Unknown is zero; never invent a scale, focal length or baseline.

### 9.4 Registration

**Windows:** your installer writes `HKLM\Software\DisplayXR\ConversionProviders\<id>` (64-bit view; NSIS `SetRegView 64`). `<id>` is a short kebab-case string equal to `iface->id`.

| Value | Type | Notes |
|---|---|---|
| `Binary` | `REG_SZ` | absolute DLL path, backslashes. Required |
| `DisplayName` | `REG_SZ` | required |
| `Vendor` | `REG_SZ` | informational |
| `ProbeOrder` | `REG_DWORD` | lower wins: **50** for a vendor provider; 100 default; 150 is the legacy `dp-lift` row, 200 the open-model provider, 250 the sim. Pick a value distinct from every provider you expect to coexist with |
| `Enabled` | `REG_DWORD` | 0 disables |
| `Hosting` | `REG_SZ` | omit (= `auto` = satellite). `in-service` only for debugging |
| `Supersedes` | `REG_SZ` | `dp-lift` if your display plug-in also serves lift through the legacy slots. It hides only that row; it never affects selection among other providers |
| `Packs` | `REG_MULTI_SZ` | absolute paths of your model-pack manifests, if you use the shared model store |
| `Settings\<KEY>` | `REG_SZ` | your settings (§9.5) |
| `UninstallString` | `REG_SZ` | informational |
| `Version` | `REG_SZ` | **written last**: it is the "row complete" marker the service re-describes on (§9.8) |

**POSIX:** a manifest named `NNN-<id>-conversion-provider.json` (`NNN` = ProbeOrder) in a plug-in root (`XRT_PLUGIN_SEARCH_PATH`, or the per-user / system roots of [`plugin-discovery.md`](../specs/runtime/plugin-discovery.md) §3). The suffix routes it to the conversion loader; the display and input loaders skip it.

```json
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "example-conversion",
        "display_name": "Example Conversion Provider",
        "vendor":       "Example Inc.",
        "version":      "0.1.0",
        "binary_path":  "/opt/example/lib/DisplayXR-ExampleConversion.so",
        "probe_order":  50,
        "settings":     { "RAMP_DIRECTION": "vertical" }
    }
}
```

Optional keys: `enabled` (bool), `hosting`, `supersedes`, `packs` (array of paths).

**Selection** is the runtime's, per task, at each stream or job create: per-task user preference (`displayxr-cli conv use`), then ProbeOrder, then a direct capability before a composed pipeline, then id. **Readiness never reorders it**: a first choice that is installed, downloading, warming or evicted is waited for; only an absent, disabled, unlicensed, incompatible or failed one falls through to the next candidate (discovery spec §3.4).

### 9.5 Settings

`host->get_setting(key, buf, size)` resolves, in the **hosting** process:

1. `DXR_CONV_<ID>_<KEY>` in that process's environment, where `<ID>` is your id upper-cased with `-` → `_` (`example-conversion` → `DXR_CONV_EXAMPLE_CONVERSION_RAMP_DIRECTION`). The host inherits the service's environment, and the service auto-starts from the logon environment — a variable set in an app's shell never reaches either;
2. else `ConversionProviders\<id>\Settings\<KEY>` (Windows) or the manifest's `settings` object (POSIX).

This replaces per-vendor environment knobs and ad-hoc registry keys. Read settings in `probe` or `warm`, not per frame.

### 9.6 Readiness and the version gate

`get_state(cap_id)` answers from memory with one of the states below, plus `failure_reason`, `retryable`, `progress` and a human-readable `detail` that is shown verbatim and never parsed. Call `host->notify_changed(cap_id)` whenever it changes (0 = re-describe everything); the runtime re-reads on its own thread.

| Situation | Report | Why it matters |
|---|---|---|
| present, model cold | `INSTALLED` | consumers wait; a create or prewarm calls `warm` |
| loading, compiling, first inference | `WARMING` with progress | waited for |
| model bytes being fetched by the host | `DOWNLOADING` with progress | waited for |
| converts now | `READY` | |
| unloaded after idle | `EVICTED` | waited for; the reload is reported `WARMING` |
| your inference runtime or other dependency is **not installed** | `ABSENT` | falls through to the next provider |
| it is installed but **older than your floor** | **`FAILED` + `XRT_CONV_FAIL_VERSION`**, never `FAIL_MODEL` | the dashboard must say "too old", not "missing" |
| model files missing, bad hash, unsupported | `FAILED` + `FAIL_MODEL` | |
| GPU / OS / driver unusable | `INCOMPATIBLE` (or `FAILED` + `FAIL_PLATFORM` when discovered at warm) | |
| licence missing or refused | `UNLICENSED` (or `FAILED` + `FAIL_LICENCE`) | |
| not enough memory | `FAILED` + `FAIL_RESOURCES` | |
| device lost during a convert | return a device-lost result | the runtime marks it `FAILED(DEVICE_LOST, retryable)` and re-warms; never retry silently inside a convert |

Set `retryable` when a later `warm` may succeed. Version-gate extended entry points (an optional feature of a newer runtime, for example) inside the provider: declare the capability only when the loaded runtime has them, and bump `get_generation` if that changes.

### 9.7 Licence classes

| Class | Meaning | Selected |
|---|---|---|
| `PERMISSIVE` | open licence, commercial use permitted | always |
| `COMMERCIAL` | proprietary, licensed for commercial use | always |
| `RESEARCH_ONLY` | research / evaluation only | only after the user opts in (`AllowResearchOnly` = 1); **signed installers never write it** |
| `UNKNOWN` | | treated as `RESEARCH_ONLY` |

`commercial_ok` is an **AND** over the model, every dependency it was derived from, and the inference runtime you load: a provider cannot advertise a commercial class over a research-only runtime. Put the SPDX identifier in `licence_spdx` (`LicenseRef-<owner>` for a proprietary licence).

### 9.8 The installer contract: close the host, never the service

The display-side rules of §5 apply (you own your row, you may be installed before the runtime, silent uninstall, never start anything elevated). What changes is the update. Your DLL is mapped only by its `displayxr-conversion-host.exe`, so:

1. Create **and own** `Global\DisplayXR-ConvMaintenance-<id>` before touching anything; hold it to the end. While it is held the service spawns no host for `<id>`.
2. Restart Manager on **your provider's files**: `RmStartSession`, `RmRegisterResources`, `RmGetList` (it lists the host, not the service), `RmShutdown`. The host exits 20 ("closed for update"); the provider's tasks report `WARMING(updating)`; every app session and stream stays alive.
3. Replace the files; write the row with **`Version` last**.
4. `RmEndSession`. **Start nothing**: not the host, not the service. Do not stop the service either.
5. Release the mutex. The service respawns the host on demand; tasks go back to `READY` with no client action.

```nsis
; In the section that replaces the files (create and release on the same thread):
; own the maintenance mutex before touching anything.
System::Call 'kernel32::CreateMutexW(p 0, i 1, w "Global\DisplayXR-ConvMaintenance-example-conversion") p .r9'
; ... Restart Manager on the provider files, copy files, WriteRegStr ... "Version" last ...
System::Call 'kernel32::ReleaseMutex(p r9)'
System::Call 'kernel32::CloseHandle(p r9)'
```

Uninstall is the same without the respawn. On POSIX there is no Restart Manager and no mutex: replace the library by rename. Full contract, the as-built runtime behaviour per step and the exit-code table: discovery spec §3.8 and §4.4. An installer that still closes the service keeps working, at the cost of every live session — which is the cost this contract exists to remove.

### 9.9 Testing without (and with) the runtime

1. **The template's fake host**, no runtime needed: it loads your DLL the way the loader does (absolute path, the one symbol), negotiates against a stub host iface that answers `get_setting` from its command line, then probes, walks `describe_cap` with a deliberately short `struct_size` as well as the full one, polls `get_state`, warms with a cancel callback, creates a CPU stream, converts a test image and checks the output and the `depth_info` units. Run it in your CI on every OS you ship.
2. **Register on a dev box** with a from-source runtime: `scripts\register_dev_plugin.bat conv C:\path\to\DisplayXR-<Vendor>Conversion.dll` (elevated; ProbeOrder 50; writes `Version` last), or the template's own `scripts/register-dev.{bat,sh}`. POSIX: drop the manifest into a directory on `XRT_PLUGIN_SEARCH_PATH`.
3. **`displayxr-cli conv list [--json]`** prints every provider × capability, the selection order with a `selected` column and the reason each row was kept or dropped; **`conv status [--watch]`** prints readiness live. With no service running both fall back to a headless pass that negotiates, probes and describes but never warms, and say so (`source: headless`).
4. **`displayxr-cli selftest`** includes a `conversion_providers` check: ABI, a well-formed `describe_cap` (known task, lanes set, `min_views ≤ max_views ≤ 8`, `VIEWS_*` ⇒ `EXPLICIT_VIEWPOINTS`, unique `cap_id`, known licence class) and in-range states. A malformed descriptor exits **13**; a missing provider never fails it.
5. **Through the service**: with your row registered and the service running (non-elevated), a lift client reaches your provider in its satellite host. `displayxr-cli lift probe <image> --mode depth` drives a depth stream (it asks for video content, which is served by `DEPTH_VIDEO`; a `DEPTH_MONO`-only provider is reached by a photo depth stream or a spec-4 `XR_LIFT_TASK_DEPTH_MONO_DXR` stream). The host writes `%LOCALAPPDATA%\DisplayXR\DisplayXR_conversion-host_<id>.<pid>_<ts>.log`, and its WARN lines appear in the service log prefixed `[conv:<id>]`.
6. **A known-good comparison**: `scripts\register_dev_plugin.bat conv sim` registers the in-tree `sim-conversion` provider (FALLBACK, ProbeOrder 250, enabled with setting `ENABLE` = 1). Its knobs script every readiness state, latency, hangs and device loss, so you can see what the runtime does with each before your provider produces it.
7. **The update path**: take the maintenance mutex, run a Restart Manager session on your DLL with a stream live, and check that `RmGetList` names the host only, the stream survives, and the task returns to `READY` after you release the mutex.

## Related

- [Plug-in iface reference](../reference/xrt_plugin_iface.md) — per-method contract for `xrt_plugin_iface`
- [Plug-in discovery spec](../specs/runtime/plugin-discovery.md) — registry / JSON manifest formats, env var overrides
- [ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md) — why the runtime DLL holds zero vendor identifiers and how the aux import-library boundary works
- [`XR_DXR_display_info` spec](../specs/extensions/XR_DXR_display_info.md) — display info + eye-tracking mode contract
- [Eye tracking modes spec](../specs/vendor/eye-tracking-modes.md) — MANAGED vs MANUAL contract
- [ADR-027](../adr/ADR-027-display-zones.md) + [`XR_DXR_display_zones`](../specs/extensions/XR_DXR_display_zones.md) — the zones contract a region-scoped hardware weaver implements
- [Input-provider iface reference](../reference/xrt_input_plugin_iface.md) + [discovery spec](../specs/runtime/input-provider-discovery.md) — the other plug-in type: controllers, hand tracking, and the rig (navigation) role (§8)
- [Conversion-provider discovery spec](../specs/runtime/conversion-provider-discovery.md) + [ADR-053](../adr/ADR-053-conversion-providers.md) + [`XR_DXR_lift`](../specs/extensions/XR_DXR_lift.md) — the third plug-in type: 2D→3D models (§9); starter kit [`displayxr-conversion-template`](https://github.com/DisplayXR/displayxr-conversion-template)
- [Legacy in-tree integration model](../archive/vendor-integration-historical.md) — historical reference for the pre-#263 in-tree integration shape
