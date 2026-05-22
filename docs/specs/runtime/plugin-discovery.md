# Plug-in Discovery Contract

**Audience:** Vendor integrators shipping a display-processor plug-in DLL,
and runtime engineers maintaining the discovery path.

**Status:** v1. Windows shipping. macOS shipping as of issue #267 —
runtime dylib has zero `sim_display_*` symbols in its link line, the
`DisplayXR-SimDisplay.dylib` plug-in is discovered via the JSON
manifest path described in §3, and `scripts/build_macos.sh` packages
+ wires up the plug-in for dev runs via `XRT_PLUGIN_SEARCH_PATH`.
Linux uses the same loader code (POSIX branch in
`target_plugin_loader.c`), but is untested end-to-end because no
graphics-stack-complete Linux build target ships today.

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
4. For each entry, in order:
   - `LoadLibraryExW` / `dlopen` the DLL/`.dylib`.
   - Resolve the single exported symbol `xrtPluginNegotiate`.
   - Call it with `XRT_PLUGIN_API_VERSION_CURRENT` + a host iface. The
     plug-in returns its own `xrt_plugin_iface *` and the API version
     it speaks. Version mismatch → `XRT_ERROR_PROBER_NOT_SUPPORTED`,
     skip.
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
   `target_plugin_get_active()` returns `NULL` and the runtime falls back
   to the statically-linked driver symbols (if any — see
   `XRT_PLUGIN_BUILD_INPROC_FALLBACK` below).

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
├── leia-sr               (ProbeOrder=50,  ships in DisplayXR-LeiaSR-Setup)
└── future-vendor-name    (ProbeOrder=50,  ships in its own installer)
```

**Subkey values:**

| Value             | Type        | Required | Purpose                                                                                                                                  |
| ----------------- | ----------- | -------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `Binary`          | `REG_SZ`    | yes      | Absolute path to the plug-in DLL. Spaces in the path are allowed; the runtime does no shell-style splitting.                             |
| `DisplayName`     | `REG_SZ`    | yes      | Human-readable name; logged at probe attempt and at successful negotiate.                                                                |
| `Vendor`          | `REG_SZ`    | no       | Publisher name (e.g. `"Leia Inc."`). Logged at probe attempt.                                                                            |
| `Version`         | `REG_SZ`    | no       | Free-form vendor version string. Logged at successful negotiate.                                                                         |
| `UninstallString` | `REG_SZ`    | see §5   | Required for vendor plug-ins shipped by their own installer (powers the runtime's cascade-uninstall). Sim-display omits this — see §5.  |
| `ProbeOrder`      | `REG_DWORD` | no       | Lower runs first. Missing defaults to 100. Vendors should use 50; sim-display uses 200 so it's always the fallback.                      |

**ProbeOrder convention:**

| Range | Use                                                                                  |
| ----- | ------------------------------------------------------------------------------------ |
| `0`   | Reserved for future "highest priority override" plug-ins.                            |
| `50`  | Vendor display plug-ins (real hardware).                                             |
| `100` | Default when `ProbeOrder` is missing. Use only if you genuinely don't care.          |
| `200` | The sim-display fallback. No real-hardware plug-in should use this or higher.        |

**Sample install (PowerShell):**

```powershell
$key = "HKLM:\Software\DisplayXR\DisplayProcessors\leia-sr"
New-Item -Path $key -Force | Out-Null
New-ItemProperty -Path $key -Name "Binary"          -Value "C:\Program Files\LeiaSR\Plugin\DisplayXR-LeiaSR.dll" -PropertyType String -Force
New-ItemProperty -Path $key -Name "DisplayName"     -Value "DisplayXR Leia SR"     -PropertyType String -Force
New-ItemProperty -Path $key -Name "Vendor"          -Value "Leia Inc."             -PropertyType String -Force
New-ItemProperty -Path $key -Name "Version"         -Value "1.35.0.2011"           -PropertyType String -Force
New-ItemProperty -Path $key -Name "UninstallString" -Value "`"C:\Program Files\LeiaSR\Plugin\Uninstall.exe`"" -PropertyType String -Force
New-ItemProperty -Path $key -Name "ProbeOrder"      -Value 50                      -PropertyType DWord  -Force
```

**Sample install (NSIS):**

```nsi
SetRegView 64
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" "Binary"          "$INSTDIR\DisplayXR-LeiaSR.dll"
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" "DisplayName"     "DisplayXR Leia SR"
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" "Vendor"          "Leia Inc."
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" "Version"         "${VERSION}"
WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
WriteRegDWORD HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" "ProbeOrder"      50
```

The runtime installer's reference implementation (sim-display, no
`UninstallString`) lives in
`installer/DisplayXRInstaller.nsi` — the section labeled "Vendor
plug-in: sim-display".

---

## 3. POSIX: JSON-manifest discovery

> **Implementation status:** shipping on macOS (issue #267). Linux uses
> the same code path but is untested end-to-end. The `XRT_PLUGIN_SEARCH_PATH`
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
| `plugin.uninstall_command` | `UninstallString` | see §5 | Command-line invocation for cascade-uninstall. Required for vendor plug-ins.          |

Discovery roots are searched in priority order — the per-user root
above first, then any system roots a platform packager adds (e.g.
`/usr/share/displayxr/DisplayProcessors/` on Linux distributions). Per-user
entries with the same `<id>` shadow system entries.

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

**See:** `src/xrt/drivers/sim_display/sim_display_plugin.c` and
`src/xrt/drivers/leia/leia_plugin.c` — the two reference plug-in
implementations. Both delegate to existing per-API DP factories and
device-creation functions that already live in their respective driver
trees; the entry-point TU is short (~150 lines).

---

## 5. Cascade-uninstall

The runtime installer enumerates `HKLM\Software\DisplayXR\DisplayProcessors\*`
on uninstall and runs each subkey's `UninstallString /S` before
touching the runtime's own files. This lets vendor plug-in installers
clean up their files + registry entries while their dependency on
`DisplayXRClient.dll` is still on disk.

**Two-pass design** (see
`installer/DisplayXRInstaller.nsi` for the implementation):

1. **Collect pass:** enumerate subkeys, read `UninstallString`, append
   non-empty values to a pipe-delimited buffer. Done up-front because
   each chained uninstaller deletes its own subkey, which would shift
   `EnumRegKey` indices if iterated in-place.
2. **Run pass:** walk the pipe-delimited list, `nsExec::ExecToLog
   '"$R7" /S'` for each entry.

After the cascade, the runtime uninstaller `DeleteRegKey
HKLM\Software\DisplayXR\DisplayProcessors` to drop sim-display's subkey
(no `UninstallString`, runtime owns its lifecycle) plus any orphans
whose vendor uninstallers failed.

**Vendor uninstaller contract:**

- Honor `/S` (silent mode). The runtime invokes you silently; a
  GUI-only uninstaller blocks the runtime's uninstall flow.
- Delete your own `<id>` subkey under
  `HKLM\Software\DisplayXR\DisplayProcessors`. The runtime drops the
  parent key after the cascade, but the runtime might be uninstalled
  AFTER your vendor uninstaller in some flows; clean up after yourself.
- Delete your plug-in DLL + supporting files.
- Don't touch the runtime's files — your dependency on
  `DisplayXRClient.dll` is still load-bearing for other vendor plug-ins
  registered alongside yours.

POSIX equivalent: `uninstall_command` is invoked as `sh -c
"<command>"`. Same contract: silent, idempotent, cleans up your own
manifest + binary, leaves the runtime alone.

---

## 6. Version negotiation

`XRT_PLUGIN_API_VERSION_CURRENT` is defined in `xrt/xrt_plugin.h`. v1
is the only released line.

Both the runtime and the plug-in pass their own version through
`xrtPluginNegotiate`. Either side can decline (return
`XRT_ERROR_PROBER_NOT_SUPPORTED`) on a mismatch. The runtime logs the
mismatch and skips to the next plug-in in the probe order.

**Forward-compat for vtable additions:** new methods on
`xrt_plugin_iface` and new fields on `xrt_plugin_display_info` MUST be
appended at the end. Plug-ins built against an older header report
`struct_size = sizeof(struct xrt_plugin_iface)` at their compile time;
the runtime guards each new vtable call on
`iface->struct_size > offsetof(struct xrt_plugin_iface, <new field>)`
before dereferencing. See `oxr_plugin_stub.c` for the structural
asserts that enforce the append-only rule at compile time.

Versioned struct bumps (`XRT_PLUGIN_API_VERSION_2`) are reserved for
non-additive changes (reordering, renaming, signature changes on
existing fields). v1 commits to the field order documented in the
header.

---

## 7. Empty-set behavior

If the registry/manifest root is absent, or every registered plug-in
declines `probe()`:

- `target_plugin_get_active()` returns `NULL`.
- The runtime falls back to the statically-linked driver symbols
  available in this build (`drv_sim_display` is always linked;
  `drv_leia` only when `XRT_PLUGIN_BUILD_INPROC_FALLBACK=ON`, a
  developer-only CMake option per ADR-019).
- Production builds without any plug-in installed (`-DXRT_PLUGIN_BUILD_INPROC_FALLBACK=OFF`,
  no plug-in DLLs registered) still drive the simulated 3D display via
  the in-tree static `drv_sim_display`.
- A truly empty build — no plug-ins AND no static fallback — is
  surfaced by `target_builder_sim_display`'s `XRT_ERROR_DEVICE_CREATION_FAILED`
  return from `open_system_impl`. Apps see `xrCreateSession` fail.

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
| `plugin loader:   <id>: LoadLibrary(<path>) failed (err=<n>).`                                                       | WARN — DLL load failure. Surfaces in default log.                                                         |
| `plugin loader:   <id>: missing entry point 'xrtPluginNegotiate' — skipping.`                                        | WARN — DLL has no negotiate symbol. Plug-in DLL is structurally invalid.                                  |
| `plugin loader:   <id>: negotiate returned <code> (iface=<ptr>) — skipping.`                                         | WARN — negotiate failure. Usually version mismatch.                                                       |
| `plugin loader:   <id>: probe returned <code> — skipping.`                                                            | WARN — probe failure other than the clean `XRT_ERROR_PROBER_NOT_SUPPORTED` decline.                       |
| `plugin loader: active plug-in: id=<id> name='<name>' vendor='<vendor>' version='<version>' plugin_api=<v> probe_order=<n> path=<path>` | WARN — the winning plug-in. Authoritative line for "which DP shipped this session."                       |
| `plugin loader: no registered plug-in claimed the system — falling back to static drivers.`                          | WARN — every entry failed / declined. Static fallback active.                                              |
| `plugin loader: registry root HKLM\Software\DisplayXR\DisplayProcessors absent (rc=<n>) — no plug-ins to try.`       | INFO — no plug-ins registered. Static fallback active. Suppressed at default WARN.                        |

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
- `src/xrt/drivers/leia/leia_plugin.c` — reference vendor plug-in.
- `installer/DisplayXRInstaller.nsi` — reference installer flow
  (sim-display registration + cascade uninstall).
