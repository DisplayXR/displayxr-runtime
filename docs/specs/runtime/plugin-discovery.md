# Plug-in Discovery Contract

**Audience:** Vendor integrators shipping a display-processor plug-in DLL,
and runtime engineers maintaining the discovery path.

**Status:** v1. Windows shipping. macOS shipping as of issue #267 —
runtime dylib has zero `sim_display_*` symbols in its link line, the
`DisplayXR-SimDisplay.dylib` plug-in is discovered via the JSON
manifest path described in §3, and `scripts/build_macos.sh` packages
+ wires up the plug-in for dev runs via `XRT_PLUGIN_SEARCH_PATH`.
Linux **ships** as well: the same loader code (POSIX branch in
`target_plugin_loader.c`), hardware-validated on Vulkan + X11, with the
plug-in installed from the `.deb` attached to every `v*` release (#781).

The loader also runs inside `displayxr-service.exe`, not only inside apps:
a display processor is an **in-process, shared-fate component of the
service** (see `docs/architecture/service-architecture.md` §1.4 / §5) —
if it hangs, calls `exit()`, or corrupts the heap, the service and every
connected client go with it. For the same reason a development build must
never register a plug-in DLL living in a worktree or build directory under
a live service (#943): the service holds the DLL and its stale code for its
whole lifetime.

This document is the **runtime ↔ plug-in discovery contract**. The
C-ABI side — the negotiation entry point, the `xrt_plugin_iface` vtable,
the `xrt_plugin_display_info` struct — lives in `xrt/xrt_plugin.h`. The
architectural rationale lives in
`docs/adr/ADR-019-vendor-plugin-aux-boundary.md`. The broader plan is in
`docs/roadmap/vendor-plugin-architecture.md`.

---

## 1. Lifecycle

For each process that loads `DisplayXRClient.dll` (any OpenXR app, the
DisplayXR shell, …):

1. **First call to `xrCreateInstance`** triggers the runtime-side loader
   (`target_plugin_loader.c`).
2. Loader **enumerates registered plug-ins** from the platform's
   discovery root (Windows registry / POSIX manifest dir; see §2-3).
3. Entries are **sorted by `ProbeOrder` ascending** (lower runs first;
   missing defaults to 100). Vendors publish at **50**, the
   sim-display fallback at **200**.
   - **`PreferredPlugin` override (§2.1 / §3.3):** if the user has pinned a
     plug-in id, the loader tries that entry **first**, before the sorted
     order. A stale or failed preference falls through to the normal order,
     so it can never brick discovery. The override is **sticky for the
     process** — the mid-install re-scan (step 5 below / `refresh_active`)
     will not auto-adopt a different plug-in while a preference is set.
     Because the loader is **one-shot per process**, changing the
     preference takes effect on the **next** process: a running service must
     be **restarted** for the switch to apply to live sessions.
4. For each entry, in order:
   - `LoadLibraryExW` / `dlopen` the DLL/`.dylib`.
   - Resolve the single exported symbol `xrtPluginNegotiate`.
   - Call it with `XRT_PLUGIN_API_VERSION_CURRENT` + a host iface. The
     plug-in returns its own `xrt_plugin_iface *` and the API version
     it speaks. Version mismatch → `XRT_ERROR_PROBER_NOT_SUPPORTED`,
     skip.
   - If the iface carries `get_platform_state` (§4.1), call it and record
     the plug-in's platform state + hint against the entry — **before**
     `probe()`, so a plug-in about to decline can still say why. The state
     is advisory: it never stops the loader from calling `probe()`.
   - Call `iface->probe(&inst)`. `XRT_ERROR_PROBER_NOT_SUPPORTED` is a
     clean "no matching device" decline (logged at INFO); any other
     `XRT_ERROR_*` is a hard failure (logged at WARN). Either way, the
     loader skips to the next entry.
   - First plug-in whose `probe()` returns `XRT_SUCCESS` **wins**. The
     loader caches `(iface, inst)` for the process lifetime; subsequent
     registry entries are not attempted.
5. The DLL handle is intentionally leaked. The cached iface's function
   pointers feed into `xrt_system_compositor_info`'s
   `dp_factory_*` fields, which the compositor calls on `xrCreateSession`
   (potentially long after `xrCreateInstance` returned). Unloading the
   plug-in would invalidate those pointers; one process, one plug-in,
   for the process's lifetime.
6. If no entries claim the system (registry empty, every probe declined),
   `target_plugin_get_active()` returns `NULL` and the sim_display
   builder fails `XRT_ERROR_DEVICE_CREATION_FAILED` — apps see
   `xrCreateSession` fail. The runtime no longer has any in-tree
   static-link fallback (removed in #287); install at least one
   DisplayProcessor plug-in DLL (e.g. the in-tree
   `DisplayXR-SimDisplay.dll`) for the runtime to be usable.

### 1.1 The per-monitor DP registry follows the active plug-in

Two things pick the display processor: the scalar `dp_factory_*` fields
(always the **active** plug-in from step 4) and the per-monitor
`xrt_dp_factory_registry` built from every registered plug-in's
`probe_displays()` claims. In-process D3D11/D3D12/VK/Metal read the scalar;
in-process GL and the D3D11 service compositor read the registry. So the
**active plug-in wins any monitor it claims** in the registry too
(`target_plugin_resolve_displays`, #1521) — which means `ProbeOrder`
forcing (`scripts\run_cts.ps1 -Plugin <id>`, `register_dev_plugin.bat`) and
`displayxr-cli dp use <id>` both force the *weaving* DP, not just the head
device. Claim **confidence** decides only among the monitors the active
plug-in does **not** claim, so the sim-display `FALLBACK` backstop and
future multi-vendor routing are unaffected. `PreferredPlugin` (§2.1) is
resolved first and outranks both.

---

## 2. Windows: registry-driven discovery

**Discovery root:** `HKLM\Software\DisplayXR\DisplayProcessors`

The runtime reads from the 64-bit view (NSIS is 32-bit, which would
otherwise redirect into `HKLM\Software\WOW6432Node\DisplayXR\…`).
Vendor installers MUST use the 64-bit view too — `SetRegView 64` in NSIS,
`KEY_WOW64_64KEY` in Win32 calls.

**Per-plug-in subkey:** `<id>` is a vendor-prefixed short identifier in
kebab-case ASCII. Example layout:

```
HKLM\Software\DisplayXR\DisplayProcessors
├── sim-display           (ProbeOrder=200, ships in runtime installer)
└── <vendor-id>           (ProbeOrder=50,  ships in the vendor's own plug-in installer)
```

**Subkey values:**

| Value             | Type        | Required | Purpose                                                                                                                                  |
| ----------------- | ----------- | -------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `Binary`          | `REG_SZ`    | yes      | Absolute path to the plug-in DLL. Spaces in the path are allowed; the runtime does no shell-style splitting.                             |
| `DisplayName`     | `REG_SZ`    | yes      | Human-readable name; logged at probe attempt and at successful negotiate.                                                                |
| `Vendor`          | `REG_SZ`    | no       | Publisher name (e.g. `"Leia Inc."`). Logged at probe attempt.                                                                            |
| `Version`         | `REG_SZ`    | no       | Free-form vendor version string. Logged at successful negotiate.                                                                         |
| `UninstallString` | `REG_SZ`    | no       | Optional, informational (mirrors the plug-in's Add/Remove Programs entry). The runtime never executes it — see §5.                       |
| `ProbeOrder`      | `REG_DWORD` | no       | Lower runs first. Missing defaults to 100. Vendors should use 50; sim-display uses 200 so it's always the fallback.                      |

**ProbeOrder convention:**

| Range | Use                                                                                  |
| ----- | ------------------------------------------------------------------------------------ |
| `0`   | Reserved for future "highest priority override" plug-ins.                            |
| `50`  | Vendor display plug-ins (real hardware).                                             |
| `100` | Default when `ProbeOrder` is missing. Use only if you genuinely don't care.          |
| `200` | The sim-display fallback. No real-hardware plug-in should use this or higher.        |

**Sample install (PowerShell)** — `acme-panel` is a placeholder vendor id.
Vendor plug-ins install into a **sibling** of the runtime directory,
`C:\Program Files\DisplayXR\Plugins\<Vendor>\`, never inside
`C:\Program Files\DisplayXR\Runtime\`:

```powershell
$dir = "C:\Program Files\DisplayXR\Plugins\Acme"
$key = "HKLM:\Software\DisplayXR\DisplayProcessors\acme-panel"
New-Item -Path $key -Force | Out-Null   # also creates DisplayProcessors if no runtime is installed yet
New-ItemProperty -Path $key -Name "Binary"          -Value "$dir\DisplayXR-Acme.dll"  -PropertyType String -Force
New-ItemProperty -Path $key -Name "DisplayName"     -Value "DisplayXR Acme Panel"     -PropertyType String -Force
New-ItemProperty -Path $key -Name "Vendor"          -Value "Acme Displays"            -PropertyType String -Force
New-ItemProperty -Path $key -Name "Version"         -Value "1.0.0"                    -PropertyType String -Force
New-ItemProperty -Path $key -Name "UninstallString" -Value "`"$dir\Uninstall.exe`""   -PropertyType String -Force
New-ItemProperty -Path $key -Name "ProbeOrder"      -Value 50                         -PropertyType DWord  -Force
```

**Sample install (NSIS)** (`InstallDir "$PROGRAMFILES64\DisplayXR\Plugins\Acme"`):

```nsi
SetRegView 64
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\acme-panel" "Binary"          "$INSTDIR\DisplayXR-Acme.dll"
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\acme-panel" "DisplayName"     "DisplayXR Acme Panel"
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\acme-panel" "Vendor"          "Acme Displays"
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\acme-panel" "Version"         "${VERSION}"
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\acme-panel" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
WriteRegDWORD HKLM "Software\DisplayXR\DisplayProcessors\acme-panel" "ProbeOrder"      50
```

The registration may be written before the runtime is installed; the
runtime adopts it when present (§5).

The runtime installer's reference implementation (sim-display, no
`UninstallString`) lives in
`installer/DisplayXRInstaller.nsi` — the section labeled "Vendor
plug-in: sim-display".

### 2.1 `PreferredPlugin` override

A single **root-level** value selects a plug-in to try before the
`ProbeOrder` sort. It is **not** a per-subkey value — it lives directly
under the discovery root:

| Location                                              | Value            | Type     | Purpose                                                                                   |
| ----------------------------------------------------- | ---------------- | -------- | ----------------------------------------------------------------------------------------- |
| `HKLM\Software\DisplayXR\DisplayProcessors` (root)    | `PreferredPlugin`| `REG_SZ` | The `<id>` of the plug-in to attempt first. Empty / absent = normal `ProbeOrder` discovery. |

Semantics: if the named id is registered and its `probe()` succeeds, it
wins regardless of `ProbeOrder`. If the id is unregistered or fails to
load, discovery silently falls back to the normal `ProbeOrder` order. The
override is **sticky** — the mid-install re-scan never swaps it out — and,
because the loader is one-shot per process, a change takes effect only for
processes started afterward (restart the service to switch a live session).

This is the non-destructive replacement for the old "delete the vendor
registry key to force sim-display" workaround. Manage it with
`displayxr-cli dp use <id>` / `dp reset` (or the Control Panel); both write
the 64-bit view (HKLM → administrator rights required). `FORCE_SIM_DISPLAY`
is unrelated — it only nudges *builder* priority, not which *plug-in* loads.

```powershell
# Force sim-display, then restore normal discovery:
New-ItemProperty -Path "HKLM:\Software\DisplayXR\DisplayProcessors" -Name "PreferredPlugin" -Value "sim-display" -PropertyType String -Force
Remove-ItemProperty -Path "HKLM:\Software\DisplayXR\DisplayProcessors" -Name "PreferredPlugin"
```

### 2.2 `DXR_PLUGIN_EXCLUSIVE` — load one plug-in and nothing else

`PreferredPlugin` and `ProbeOrder` decide which plug-in **wins**. Neither
decides which plug-ins are **loaded**: the per-display claim registry
(§ADR-015 / #69) asks *every* registered plug-in for its claims, so on a box
with a vendor plug-in installed that DLL — and its whole dependency chain — is
`LoadLibrary`d into the process even when sim-display is the active DP.

`DXR_PLUGIN_EXCLUSIVE=<id>` (process environment, all platforms) makes
discovery skip every entry whose id is not `<id>`, **before** any
`LoadLibrary`/`dlopen`. It applies to the active-plug-in walk, the
display-claim collection and the mid-install refresh, and it outranks
`PreferredPlugin` (honoring a preference would load the very DLL the caller
asked to keep out).

This exists for the CTS lanes (#1545, #1523). A `-G d3d11` conformance run on
a box with the Leia SR plug-in installed pulls `SimulatedRealityOpenGL.dll` →
`opengl32` → the NVIDIA GL ICD into the process purely through claim
collection, and the `multithreading` case then faults in an NV ICD worker
thread. `scripts/run_cts.ps1 -Plugin <name>` sets it for the run.

Semantics differing from `PreferredPlugin`, deliberately:

- **No fallback on a miss.** An id nothing is registered under loads *nothing*;
  the loader emits one WARN naming the value and every registered id. A
  silent fallback would defeat the only purpose of the variable.
- **Exact match** on the id (registry subkey name on Windows, manifest `id` on
  POSIX), same comparison `PreferredPlugin` uses.
- **Skipped ≠ rejected.** Excluded entries are not counted by the #1212
  better-ranked-candidate tally, so `displayxr-cli selftest` stays green.
- **Not machine state.** Unset = today's behaviour; nothing is written
  anywhere, and a killed run leaves no trace. `displayxr-cli dp list` still
  enumerates everything that is registered.

Read with CRT `getenv`, like `XRT_PLUGIN_SEARCH_PATH` and
`XRT_PREFERRED_PLUGIN_ID` — set it in the environment the process **inherits**
(a launcher, or the parent shell before `Start-Process`). An in-process client
calling `SetEnvironmentVariableW` after startup is silently ignored; see
[adapter selection](../../reference/adapter-selection.md) § *The `getenv()`
caveat*.

---

## 3. Per-platform filesystem discovery

POSIX (macOS / Linux) and Android both `dlopen` plug-in `.so`s from the
filesystem but use different shapes for the per-entry metadata.

### 3.1 POSIX: JSON-manifest discovery (macOS / Linux)

> **Implementation status:** shipping on macOS (issue #267) and on Linux
> (same code path; hardware-validated, `.deb` on every release, #781).
> The `XRT_PLUGIN_SEARCH_PATH`
> env var (colon-separated directory list) overrides the default
> search roots — used by `scripts/build_macos.sh`-generated
> `run_*.sh` scripts to point at the dev tree's
> `_package/DisplayXR-macOS/lib/displayxr/plugins/` without polluting
> `~/Library/Application Support/`.

**Discovery root (macOS):**
`~/Library/Application Support/DisplayXR/DisplayProcessors/`

**Discovery root (Linux):**
`${XDG_DATA_HOME:-~/.local/share}/DisplayXR/DisplayProcessors/`

Each plug-in publishes a single JSON manifest file in that directory.
**Filename convention:** `<probe_order>-<id>.json`, three-digit
zero-padded probe-order prefix. The runtime sorts filenames
lexicographically (no JSON parsing for ordering), so `050-leia-sr.json`
runs before `200-sim-display.json`. The probe-order field inside the
JSON is the source of truth; the filename prefix is the discovery hint
that lets the loader avoid round-tripping JSON parses just to sort.

**Manifest shape:**

```json
{
  "file_format_version": "1.0",
  "plugin": {
    "id":           "leia-sr",
    "display_name": "DisplayXR Leia SR",
    "vendor":       "Leia Inc.",
    "version":      "1.35.0.2011",
    "binary_path":  "/usr/local/lib/displayxr/plugins/DisplayXR-LeiaSR.so",
    "probe_order":  50,
    "uninstall_command": "/usr/local/lib/displayxr/plugins/uninstall-leia-sr.sh"
  }
}
```

Fields mirror the Windows registry schema:

| JSON field            | Windows analogue   | Required | Purpose                                                                              |
| --------------------- | ------------------ | -------- | ------------------------------------------------------------------------------------ |
| `file_format_version` | n/a                | yes      | `"1.0"` for v1 manifests. Loader rejects unknown versions.                            |
| `plugin.id`           | `<subkey>`         | yes      | Vendor-prefixed short identifier; matches the filename's `<id>` segment.              |
| `plugin.display_name` | `DisplayName`      | yes      | Logged at probe / negotiate.                                                          |
| `plugin.vendor`       | `Vendor`           | no       | Publisher name.                                                                       |
| `plugin.version`      | `Version`          | no       | Free-form version string.                                                             |
| `plugin.binary_path`  | `Binary`           | yes      | Absolute path to the `.so` / `.dylib`. Spaces allowed.                                |
| `plugin.probe_order`  | `ProbeOrder`       | no       | Default 100. Sim-display=200, vendors=50.                                             |
| `plugin.uninstall_command` | `UninstallString` | no     | Informational only; the runtime never runs it (§5).                                   |

Discovery roots are searched in priority order — the per-user root
above first, then the packaged system roots below. Per-user entries with
the same `<id>` shadow system entries.

**System roots (Linux), searched after the per-user root:**

| Root                                       | Who writes it                                                                 |
| ------------------------------------------ | ----------------------------------------------------------------------------- |
| `/usr/lib/displayxr/plugins/`              | The **built-in default** for packaged installs. The runtime `.deb` drops the sim-display `.so` + `200-sim-display.json` here; vendor plug-in `.deb`s drop their `.so` + `050-*.json` alongside. This is the POSIX analogue of the Windows `DisplayProcessors` registry root, and it is what makes an installed box need **no `XRT_PLUGIN_SEARCH_PATH`** (#781). |
| `/usr/local/share/displayxr/DisplayProcessors/` | A distro/local packager convention.                                      |
| `/usr/share/displayxr/DisplayProcessors/`  | A distro packager convention.                                                 |

When `XRT_PLUGIN_SEARCH_PATH` is set it is prepended (highest priority), so
a dev build still overrides an installed one; when it is unset, discovery
falls through to `/usr/lib/displayxr/plugins/` and the install is
self-sufficient.

### 3.2 Android: convention-driven discovery

> **Implementation status:** shipping (initial Android branch landed
> with the Android M7 milestone). Single-vendor case: the plug-in
> ships in the runtime APK's `jniLibs/<ABI>/`. Multi-APK discovery
> (separate vendor APKs each shipping plug-ins) is a v2 problem; see
> "Non-goals" below.

**Why convention, not JSON manifests:** Android's package installer
extracts only `.so` files from a APK's `jniLibs/<ABI>/` into the
on-disk `lib/<ABI>/`. Any `.json` shipped in `jniLibs/` stays trapped
inside `base.apk` and would need `AAssetManager` + a `JNIEnv` to read —
JNI plumbing the runtime loader has no access to at `xrCreateInstance`
time. The `xrt_plugin_iface` returned by `xrtPluginNegotiate` already
carries `id` / `display_name` / `vendor`; the only load-bearing field
manifests add is `ProbeOrder`, which we encode in the filename.

**Filename convention:** `libdxrp<NNN>_<id>.so` — `<NNN>` is the
three-digit zero-padded ProbeOrder, `<id>` matches `iface->id` the
plug-in returns at negotiate. Examples:

```
libdxrp050_leia_cnsdk.so   (vendor plug-in, ProbeOrder=50)
libdxrp200_sim_display.so  (fallback, ProbeOrder=200)
```

Lexicographic sort on filename gives probe-order ascending for free —
same trick the POSIX `050-leia-sr.json` filename convention uses (§3.1).
`<id>` is an alphanumeric / underscore identifier; the loader strips
`libdxrp<NNN>_` and `.so` from the basename to recover it. Plug-ins
**must** match the iface's `id` to the filename's `<id>` segment for
log correlation; mismatch is allowed but produces a confusing
"id=foo path=libdxrp050_bar.so" log line.

**Discovery root (priority order):**

1. `$XRT_PLUGIN_SEARCH_PATH` — single dir, dev/emulator override
   (no colon splitting on Android — single root is sufficient and
   removes a footgun on a platform where `:` does appear in paths).
2. `dirname(dladdr(&loader_internal_symbol))` — the runtime `.so`'s
   own lib dir, which on a real device is
   `/data/app/<runtime-pkg>-<hash>/lib/<ABI>/`. Plug-ins shipped in
   the runtime APK's `jniLibs/<ABI>/` land here automatically.

The loader uses `dladdr` on a static symbol inside itself to recover
its loaded path, then takes the dirname — there is no
hardcoded `/data/app/...` path and no dependency on a known package
name.

**Field source-of-truth:**

| Field         | Source                                                                 |
| ------------- | ---------------------------------------------------------------------- |
| `id`          | filename `<id>` segment, surfaced via `iface->id` after negotiate.     |
| `probe_order` | filename `<NNN>` segment.                                              |
| `display_name`, `vendor` | `iface->display_name`, `iface->vendor` (post-negotiate).    |
| `version`     | Not surfaced in v1 (no install record to compare against).             |

**Host iface on Android.** Beyond discovery, the loader hands each plug-in a
`struct xrt_plugin_host_iface` carrying the host's `JavaVM`, its
Activity/Service `Context`, and — since #1037 / ADR-036 D2 — a **class-host
`Context`** whose `getClassLoader()` resolves classes shipped in the *runtime's*
APK, so a plug-in running in the **app's** process can load vendor Java glue the
app does not bundle. The runtime package is derived from the same
`dladdr`-recovered lib dir used as the discovery root above. Contract:
[`docs/reference/xrt_plugin_iface.md` § The host iface](../../reference/xrt_plugin_iface.md#the-host-iface).

**Non-goals (v1):**

- Multi-APK discovery: scanning `PackageManager` for separate vendor
  APKs that each ship a plug-in. Requires JNI plumbing
  (`AAssetManager`, `getPackageInfo(GET_META_DATA)`) and likely a
  metadata convention on `<service>`/`<receiver>` tags in
  `AndroidManifest.xml`. Open question for when a second Android
  vendor materializes.
- Separate uninstall (§5): each plug-in `.so` shipped in the runtime
  APK is uninstalled by the OS as part of the APK uninstall. Vendor
  APK plug-ins (v2) are uninstalled by the OS with their own APK.

### 3.3 `PreferredPlugin` override (POSIX)

The §2.1 override has a POSIX equivalent with the same "try this id first,
fall through on miss/failure, sticky for the process" semantics:

1. **`XRT_PREFERRED_PLUGIN_ID` env var** — wins when set; a dev/CI override
   that needs no writable filesystem (mirrors `XRT_PLUGIN_SEARCH_PATH`).
2. **A `preferred` file** in the per-user manifest dir
   (`~/Library/Application Support/DisplayXR/DisplayProcessors/preferred`
   on macOS; `${XDG_DATA_HOME:-~/.local/share}/DisplayXR/DisplayProcessors/preferred`
   on Linux) — a single line containing the plug-in id. This is what
   `displayxr-cli dp use <id>` / `dp reset` write.

Android exposes only the env-var read (the writable override and `dp list`
are not shipped there in v1; see Non-goals above).

### 3.4 Desktop Linux: monitor enumeration and display claims (multi-screen M0)

The per-monitor registry (§1.1, #69 / ADR-015) is populated on desktop Linux
as on Windows. Two pieces make that possible.

**Monitor enumeration** (`os_display_edid_enumerate`,
`src/xrt/auxiliary/os/os_display_edid_linux.c`). No single source is a monitor
record: RandR knows where each monitor sits on the desktop and which one is
primary, but XWayland publishes no EDID property; DRM sysfs
(`/sys/class/drm/card*-*/{status,enabled,edid,modes}`) has the EDID and the
connector name but no desktop position. The enumerator takes the RandR
monitors from `os_display_desktop_enumerate` and ties each one to a connected
DRM connector, first rule that fires:

0. **randr-edid** — the X server publishes the output's own `EDID` property
   (native X does; XWayland does not). That is the monitor's identity. The
   connector is the one enabled connector carrying the same EDID (vendor,
   product, serial), or, among several identical ones, the one whose name
   agrees.
1. **name** — RandR output name equals the connector name with the card prefix
   and the kernel's subtype letter dropped (`card1-HDMI-A-1` → `HDMI-1`), and
   the connector agrees physically: its modes hold the monitor's device mode,
   or its EDID size is within 10 mm of RandR's. A bare name is not enough: the
   NVIDIA X driver numbers outputs from 0 (`DP-0`) while nvidia-drm numbers
   connectors from 1, and two GPUs can each have an `HDMI-A-1`. This is the
   normal case on Mutter's XWayland.
2. **mm** — exactly one unused, enabled connector whose EDID physical size is
   within 10 mm of RandR's (EDID stores cm in the base block and mm in the
   detailed timing; 340 vs 344 mm is the same panel).
3. **mode** — exactly one unused, enabled connector with the monitor's device
   mode among its modes.

"Device mode" is the compositor's current mode when Mutter reports it, else
the RandR rect. It is never the DRM-derived mode, which was itself found by
connector name.

Ambiguity is never guessed: a monitor no rule ties uniquely is listed with
its placement and no EDID identity. A connector is used at most once, and a
connected-but-disabled connector never joins by mm or mode. The record keeps
the card prefix (`card1-HDMI-A-1`). Connectors are read in name order, so
DRM-only records come out in the same order on every boot. From
the EDID the enumerator reads the manufacturer and product id (bytes 8–11),
the serial (12–15), the size in cm (21/22) and the first detailed timing's
pixels, mm and refresh. With **no reachable X server** (pure Wayland,
headless) every connected, enabled connector becomes a DRM-only record flagged
`origin_unknown` at (0, 0). The join method per monitor is logged once at INFO
(`plugin loader: monitor N … join=randr-edid|name|mm|mode|drm-only|none`), and
`displayxr-cli displays` prints it.

The plug-in-facing `xrt_display_descriptor` is unchanged (no ABI change). The
connector name, mm and device mode stay runtime-side (`os_display_edid_monitor`
plus a loader side table keyed by `monitor_id`). The `monitor_id` hash adds the
connector name (with its card prefix) where the platform has one, so DRM-only
records at (0, 0) stay distinct. Windows ids are unchanged.

**Claims from every plug-in.** `target_plugin_resolve_displays` now loads every
manifest plug-in on POSIX as a claim source (`collect_display_sources_platform`,
the twin of the Windows one): same roots and order as discovery,
`DXR_PLUGIN_EXCLUSIVE` honoured, the active plug-in reused rather than loaded
twice. The other plug-ins are claim sources only. No device is created from
them, and active-plug-in selection is unchanged. One consequence carries over
from Windows: with `XRT_PREFERRED_PLUGIN_ID=sim-display`, an installed vendor
plug-in is now loaded and probed for its claims. Use `DXR_PLUGIN_EXCLUSIVE` to
keep it out of the process (§2.2).

**Back-compat claim for a plug-in without `probe_displays`.** Such a plug-in
gets one synthesized `EDID`-confidence claim. On Windows that claim stays on
the primary monitor. Off-Windows, the **active** plug-in's claim goes to the
monitor its panel matches once the runtime has read `get_display_info`
(`target_plugin_note_active_panel`, called by the builder at system create and
on every display-info apply). The matching uses the ADR-033 resolver's rules:
the plug-in's origin when non-zero, then the connector's device mode, then the
pixel size, with ties broken on physical size. Without this, a laptop with the
3D panel on an external connector would route the laptop screen (the primary)
to the vendor DP and the panel to the fallback, because the active plug-in
wins every monitor it claims (#1521). A plug-in that implements
`probe_displays` never reaches the synthesized path, so its own claims always
decide. `displayxr-cli displays --claims` brings the system up headlessly
first, so it reports the same placement the runtime uses.

Example from a laptop (eDP-1, primary) with an Acer DS1 on HDMI, with
`leia-sr` active and no `probe_displays` in the plug-in yet:

```
monitor …  3456x2160 @ (0,0)      SDC 423F  300x190 mm  output=eDP-1   → sim-display  FALLBACK
monitor …  3840x2160 @ (3456,0)   ACR 0001  344x193 mm  output=HDMI-1  → leia-sr      EDID
```

On Linux nothing in the compositors reads the registry yet. The Vulkan
compositor uses the scalar `dp_factory_*`, so this milestone changes no
weaving. macOS and Android still enumerate no monitors.

---

## 4. Plug-in DLL contract

Each plug-in DLL exports **exactly one** symbol: `xrtPluginNegotiate`.
The signature is in `xrt/xrt_plugin.h`; the linkage decoration macro
is `XRT_PLUGIN_EXPORT` from the same header.

```c
XRT_PLUGIN_EXPORT xrt_result_t
xrtPluginNegotiate(uint32_t runtime_api_version,
                   const struct xrt_plugin_host_iface *host,
                   struct xrt_plugin_iface **out_iface,
                   uint32_t *out_plugin_api_version);
```

Build-side enforcement: plug-ins set the linker target's visibility to
`hidden` so any accidentally-non-static symbol stays private. The CI
assertion in `.github/workflows/build-windows.yml` runs
`dumpbin /exports DisplayXR-SimDisplay.dll | findstr xrtPluginNegotiate`
to catch regressions; vendor plug-in pipelines should run the
equivalent.

The vtable returned via `*out_iface` is owned by the plug-in DLL.
Lifetime: it must remain valid until the runtime calls
`iface->destroy(inst)`, which happens at process shutdown (or never,
since the DLL is intentionally leaked).

**See:** `src/xrt/drivers/sim_display/sim_display_plugin.c` (in-tree,
vendor-neutral) and the Leia plug-in's entry point in
[`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin)
(`src/drv_leia/`, ADR-019) — reference plug-in implementations. Each
delegates to per-API DP factories and device-creation functions in its own
tree; the entry-point TU is short (~150 lines).

### 4.1 Loadable without the platform; `probe()` is cheap; report platform state (ADR-045)

A registered plug-in is a fact, not a decision: the runtime may enumerate
it on a machine where the vendor platform it drives is missing, not yet
running, or has no display attached — and the order in which the user
installs the runtime, the plug-in and the vendor platform is arbitrary.
Every plug-in therefore MUST:

1. **Load without its platform.** `LoadLibraryExW` / `dlopen` of the plug-in
   binary must succeed when the vendor platform runtime is not installed.
   Resolve vendor libraries lazily (delay-load / `dlopen` by a path the
   plug-in derives itself) rather than as static imports the OS loader
   must satisfy — and never rely on the host process's `PATH`, which in the
   long-lived service is frozen at logon.
2. **Keep `probe()` and `get_platform_state()` within ~100 ms**, and never
   block on the vendor platform becoming ready. Both run on the
   `xrCreateInstance` hot path and on every re-probe (§4.2). Presence
   checks only — a registry value, a named kernel object, an EDID table
   lookup. A readiness wait belongs in `create_device` / the DP factory
   (or a background thread the plug-in owns), not in `probe()`.
3. **Report its platform state** through the optional
   `xrt_plugin_iface::get_platform_state` slot (`XRT_PLUGIN_HAS_PLATFORM_STATE`,
   appended per ADR-020 at unchanged ABI):

   | `xrt_plugin_platform_state` | Meaning |
   |---|---|
   | `UNKNOWN` (0) | Not reported (slot absent, older plug-in, call returned false). Runtime behaves as before. |
   | `READY` | Platform installed, running, display attached. |
   | `PLATFORM_ABSENT` | The vendor platform runtime is not installed. |
   | `PLATFORM_NOT_RUNNING` | Installed, its service/daemon is not running. |
   | `NO_DISPLAY` | Platform up, none of its displays attached. |
   | `INCOMPATIBLE` | Platform present but unusable (version, OS, GPU, …). |

   plus `hint` — a short (≤ 127 bytes UTF-8) vendor-written sentence the
   runtime shows verbatim and never parses ("install the … runtime",
   "connect the display"), and `flags`. `XRT_PLUGIN_PLATFORM_FLAG_FALLBACK`
   marks a plug-in that claims any system (the in-tree sim-display); vendor
   plug-ins MUST NOT set it.

   Call sequence: **load → negotiate → `get_platform_state` → `probe`**.
   The slot takes no instance, is thread-safe, and may also be called at any
   time after selection (diagnostics poll it while the plug-in is active).

### 4.2 Re-probe and the no-live-swap rule

The long-lived service re-evaluates selection on world events — display
topology change, device-node change, a change under the
`DisplayProcessors` registry key, and a slow timer while the active plug-in
is the fallback — debounced to at most one refresh per second. A refresh
adopts a better (lower `ProbeOrder`) plug-in **only while the active one
carries `XRT_PLUGIN_PLATFORM_FLAG_FALLBACK`** (or, for a plug-in that does
not report state, has id `sim-display`). An active vendor plug-in that
reports `NO_DISPLAY` is kept: the runtime surfaces the state (tray,
`displayxr-cli info` / `selftest`) and the DP passes pixels through
unwoven. In-process apps do not re-probe on world events.

**Adoption is complete via a restart when idle.** On adoption the weaving DP
and display info follow the new plug-in at once, but the head device (mode
table, eye tracking) was created by the fallback and is held by the system
compositor and every client's shared-memory snapshot, so it cannot be swapped
live. The system marks it stale (`xrt_system_compositor_info::head_device_stale`);
once no IPC client has been connected for 2 s the Windows service ends its main
loop and starts a successor (`--adoption-restart-after-pid <pid>`), which waits
for the old process to exit and builds its system on the new plug-in. A
successor never restarts itself for adoption again (no loop on a flapping
probe).

---

## 5. Registration lifetime (no cascade-uninstall)

A registration under `HKLM\Software\DisplayXR\DisplayProcessors\<id>` is
owned by the installer that wrote it, for its whole lifetime. Nothing may
depend on the order in which the runtime and a plug-in are installed or
uninstalled (epic #1803).

**The runtime never removes a vendor registration.** Its uninstaller
(`installer/DisplayXRInstaller.nsi`, Section "Uninstall"):

1. deletes only its own `sim-display` subkey;
2. deletes the root `PreferredPlugin` value only when it names
   `sim-display` or a plug-in that is no longer registered (a pin to a
   still-registered vendor plug-in is the user's choice and survives);
3. deletes the `DisplayProcessors` key itself only if it is then empty
   (`DeleteRegKey /ifempty`);
4. never runs a plug-in's `UninstallString`, never deletes files outside
   its own install directory, and removes the shared
   `C:\Program Files\DisplayXR` parent only non-recursively, so vendor
   plug-in directories (`C:\Program Files\DisplayXR\Plugins\<Vendor>`)
   survive.

Vendor plug-ins are separate products with their own Add/Remove Programs
entries; the runtime uninstaller says so in its log. (Earlier runtimes
ran every registered `UninstallString /S` and then deleted the whole key.
A plug-in whose uninstaller failed was left installed, with its files and
Add/Remove Programs entry, but **unregistered** — invisible to the
runtime, and version-skipped by a later bundle. That cascade is gone.)

**A plug-in may be installed before the runtime.** Its subkey can
pre-exist the runtime, and the runtime installer adopts it: install,
repair and upgrade write only the `sim-display` subkey and never rewrite
or delete another one. A runtime uninstall followed by a reinstall
therefore picks a vendor plug-in up again without reinstalling it.

**Vendor installer / uninstaller contract:**

- Honor `/S` (silent mode) and never show a modal under it (NSIS:
  `MessageBox ... /SD <default>`).
- Do not require the runtime. Install your files and write your own
  `<id>` subkey (64-bit view) whether or not a runtime is present; if you
  enforce a minimum runtime version, do it only when a runtime is
  installed.
- Release your DLL from a running `displayxr-service.exe` with Windows
  Restart Manager (`RmGetList` / `RmShutdown(RmForceShutdown)` /
  `RmRestart`), not by killing it; if you start the service yourself,
  start it through `explorer.exe`, never elevated (an elevated service
  cannot be reached by normal-integrity apps).
- On uninstall, delete your own `<id>` subkey and your own files, with or
  without the runtime present. Don't touch the runtime's files or any
  other subkey.
- `UninstallString` is optional and informational; the runtime does not
  execute it.

Workspace controllers are different: they require the runtime, so the
runtime uninstaller still cascades into each registered controller's
`UninstallString` (see
[workspace-controller-registration.md](workspace-controller-registration.md)),
but it too deletes the `WorkspaceControllers` key only if it is empty
afterwards, so a controller whose uninstaller failed keeps its
registration.

POSIX: `uninstall_command` in a JSON manifest is likewise informational —
no runtime packaging step runs it. A plug-in package removes its own
manifest + binary and leaves the runtime alone.

---

## 6. Version negotiation

`XRT_PLUGIN_API_VERSION_CURRENT` is defined in `xrt/xrt_plugin.h`. The current major is
`XRT_PLUGIN_API_VERSION_5` (`xrt_plugin.h:311`; ADR-020, ADR-022).
History:

- v1 → v2 (runtime v1.6.0): the one-time break that introduced the
  `struct_size` header on the display-processor vtables; ABI-v1
  plug-ins are rejected by the loader and must rebuild against v2
  headers.
- v2 → v3 (runtime v1.13.0, #441): `xrt_rendering_mode` gained
  `mode_flags` (bit 0 = `XRT_RENDERING_MODE_FLAG_HAS_TRACKING`) +
  `reserved[3]` — an element-stride change in the `xrt_device`-embedded
  array, so ABI-v2 plug-ins are rejected. The flags word + reserved
  padding make v3 the intended **last** rendering-mode layout break
  (ADR-022): future per-mode capabilities are new bits, not new fields.
- v3 → v4 (transparency): `set_chroma_key` is removed from all five DP
  vtables, shifting every slot after it — a layout break. The D3D12 and
  GL DP vtables gain `set_transparent_background`, and the extension
  struct drops `chromaKeyColor` (`XR_DXR_win32_window_binding`
  SPEC_VERSION 7 → 8). True transparency (alpha-capable swapchain +
  transparent present) becomes the sole path.
- v4 → v5 (#757): `struct vk_bundle` — whose raw pointer crosses the
  runtime → plug-in boundary via the VK DP factory — gained ABI-parity
  `#else` placeholder members for every `VK_USE_PLATFORM_*`-conditional
  PFN slot, so its layout no longer varies with configure-time feature
  detection. That inserts members versus v4 binaries (a layout break),
  but it is the **last** such break: the layout is now identical across
  configs by construction. The Linux VK DP contract also newly
  guarantees the dma-buf import extension set on the app device
  (`VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`).

Both the runtime and the plug-in pass their own version through
`xrtPluginNegotiate`. The runtime enforces a strict major match — a
plug-in reporting any version other than the runtime's
`XRT_PLUGIN_API_VERSION_CURRENT` is rejected at the loader (`ABI major
mismatch — plugin_api=%u, runtime expects %u`) and discovery falls back
to the next plug-in / `sim_display`. ADR-020 rule 3.

**Forward-compat for vtable additions within a major:** new methods on
`xrt_plugin_iface`, the DP vtables (`xrt_display_processor`,
`xrt_display_processor_<api>`), and new fields on
`xrt_plugin_display_info` MUST be appended at the end. Plug-ins built
against an older header report `struct_size = sizeof(struct …)` at
their compile time; the runtime guards each new vtable call on
`XRT_DP_HAS_SLOT(xdp, <new field>)` (DP vtables) or
`iface->struct_size > offsetof(struct xrt_plugin_iface, <new field>)`
(plug-in iface / display info) before dereferencing. See
`oxr_plugin_stub.c` for the structural asserts that enforce the
append-only rule at compile time.

Versioned struct bumps (the next would be `XRT_PLUGIN_API_VERSION_6`)
are reserved for non-additive changes (reordering, renaming, signature
changes on existing fields). Each major commits to the field order documented in
the header.

---

## 7. Empty-set behavior

If the registry/manifest root is absent, or every registered plug-in
declines `probe()`:

- `target_plugin_get_active()` returns `NULL`.
- The runtime has **no static-link fallback** (removed in #287 alongside
  the now-defunct `XRT_PLUGIN_BUILD_INPROC_FALLBACK` CMake option).
  Every display-processor implementation must come from a plug-in DLL.
- `target_builder_sim_display.c::open_system_impl` returns
  `XRT_ERROR_DEVICE_CREATION_FAILED` and apps see `xrCreateSession` fail.
- The expected install state is at least one DisplayProcessor plug-in DLL
  registered — typically the in-tree `DisplayXR-SimDisplay.dll` (shipped
  by the runtime installer) plus optionally vendor plug-ins (e.g.
  `DisplayXR-LeiaSR.dll` from `displayxr-leia-plugin`).

---

## 8. Logging

The runtime emits these one-shot lines at instance creation, all at
`WARN` level so they appear in the default-level log without enabling
`XRT_LOG=info`:

| Line                                                                                                                 | Meaning                                                                                                   |
| -------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `plugin loader: <N> registered plug-in(s); attempting in ProbeOrder ascending.`                                      | INFO — entry count after enumeration. Suppressed at default WARN level.                                   |
| `plugin loader:   [i/N] <id> (ProbeOrder=<order>, <path>)`                                                            | INFO — per-entry attempt trace. Suppressed at default WARN level.                                         |
| `plugin loader:   <id>: probe declined (no matching device).`                                                         | INFO — clean decline from probe (`XRT_ERROR_PROBER_NOT_SUPPORTED`). Suppressed at default WARN level.     |
| `plugin loader:   <id>: LoadLibrary(<path>) failed (err=<n>).`                                                       | WARN — DLL load failure. `err=126` with the binary present adds "a library the plug-in imports is missing" (`DEPENDENCY_MISSING`). |
| `plugin loader:   <id>: registered Binary '<path>' does not exist — skipping (orphan registration).`                  | WARN — `BINARY_MISSING` (ADR-045).                                                                        |
| `plugin loader:   <id>: platform state <STATE>[ — <hint>]`                                                           | WARN on change — what `get_platform_state` reported, before `probe()` (ADR-045).                          |
| `plug-in adoption: '<old>' -> '<new>' — weaving DP and display info follow now; …`                                   | WARN — a refresh adopted a better plug-in under the fallback's head device (service).                     |
| `plug-in adoption: no client connected — restarting the service so the head device follows '<id>'.`                 | WARN — the adoption restart (Windows service).                                                            |

| `plugin loader:   <id>: missing entry point 'xrtPluginNegotiate' — skipping.`                                        | WARN — DLL has no negotiate symbol. Plug-in DLL is structurally invalid.                                  |
| `plugin loader:   <id>: negotiate returned <code> (iface=<ptr>) — skipping.`                                         | WARN — negotiate failure. Usually version mismatch.                                                       |
| `plugin loader:   <id>: probe returned <code> — skipping.`                                                            | WARN — probe failure other than the clean `XRT_ERROR_PROBER_NOT_SUPPORTED` decline.                       |
| `plugin loader: active plug-in: id=<id> name='<name>' vendor='<vendor>' version='<version>' plugin_api=<v> probe_order=<n> path=<path>` | WARN — the winning plug-in. Authoritative line for "which DP shipped this session."                       |
| `plugin loader: no registered plug-in claimed the system — falling back to static drivers.`                          | WARN — every entry failed / declined. **The message is stale**: the static-link fallback was removed in #287 (see §7), so nothing loads and instance creation fails.  |
| `plugin loader: registry root HKLM\Software\DisplayXR\DisplayProcessors absent (rc=<n>) — no plug-ins to try.`       | INFO — no plug-ins registered. Same stale "static" wording as above; there is no fallback. Suppressed at default WARN.  |

Load failures, the `loading plug-in binary` breadcrumb, and the #461 skew check WARN on the **first**
attempt per plug-in per process (or when the outcome changes); repeats from re-probes log at INFO.

Vendor support flows checking "is the plug-in actually loading"
should look for the `active plug-in:` line in
`%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.<pid>_<ts>.log` (Windows) or
the corresponding POSIX log directory.

---

## 9. References

- `xrt/xrt_plugin.h` — the C ABI: negotiate signature, vtable shape,
  display-info struct, version macros.
- `docs/adr/ADR-019-vendor-plugin-aux-boundary.md` — why the runtime
  DLL exports aux's stateful TUs and the plug-in iface uses an import
  library to consume them.
- `docs/roadmap/vendor-plugin-architecture.md` — the original plan, the
  source of the design decisions this spec implements.
- `src/xrt/targets/common/target_plugin_loader.c` — runtime-side
  implementation.
- `src/xrt/drivers/sim_display/sim_display_plugin.c` — reference
  fallback plug-in.
- [`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin) (`src/drv_leia/`) — reference vendor plug-in (ships from its own repo, ADR-019).
- `installer/DisplayXRInstaller.nsi` — reference installer flow
  (sim-display registration; uninstall removes only its own
  registration — §5).
