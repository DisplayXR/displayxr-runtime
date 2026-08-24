# Vendor Plug-in Onboarding

This guide walks a new 3D-display vendor from zero to a shipping DisplayXR-compatible plug-in. After ADR-019 / #263 (May 2026), every vendor integration is an **external project**: you don't touch this repo, you ship a plug-in DLL from your own repo and an installer that registers it with the runtime at install time.

If you're maintaining a vendor integration that was historically in-tree (the way `drv_leia` was before #263), see the [legacy in-tree guide](../archive/vendor-integration-historical.md) — kept for historical context. Everything below assumes the post-#263 model.

> **Where to start**
> - [`DisplayXR/displayxr-vendor-template`](https://github.com/DisplayXR/displayxr-vendor-template) — **the recommended starting point.** A buildable, ABI-correct, **vendor-SDK-free** starter kit: it's a plug-in skeleton (`drv_example`, D3D11 + Vulkan stub weavers) that consumes only the public runtime ABI (pinned via FetchContent). Generate a repo from it, rename `drv_example` → `drv_<vendor>`, replace the stub weave. See [§3](#3-building-from-the-template).
> - [`DisplayXR/displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin) — a **full worked vendor integration** (the first vendor on the platform) to read for reference. It builds against a proprietary vendor SDK, so it's not a clean *starting* point — use it to see how a real weaver, eye-tracking listener, and installer come together.
> - `src/xrt/drivers/sim_display/sim_display_plugin.c` (in this repo) — the runtime's in-tree neutral reference. Smaller, no vendor SDK; useful as a minimal-shape reference (can't be forked as a standalone plug-in repo — that's what the template is for).

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
  for present-owning apps through `XR_DXR_weave`.

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

## Related

- [Plug-in iface reference](../reference/xrt_plugin_iface.md) — per-method contract for `xrt_plugin_iface`
- [Plug-in discovery spec](../specs/runtime/plugin-discovery.md) — registry / JSON manifest formats, env var overrides
- [ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md) — why the runtime DLL holds zero vendor identifiers and how the aux import-library boundary works
- [`XR_DXR_display_info` spec](../specs/extensions/XR_DXR_display_info.md) — display info + eye-tracking mode contract
- [Eye tracking modes spec](../specs/vendor/eye-tracking-modes.md) — MANAGED vs MANUAL contract
- [ADR-027](../adr/ADR-027-display-zones.md) + [`XR_DXR_display_zones`](../specs/extensions/XR_DXR_display_zones.md) — the zones contract a region-scoped hardware weaver implements
- [Legacy in-tree integration model](../archive/vendor-integration-historical.md) — historical reference for the pre-#263 in-tree integration shape
