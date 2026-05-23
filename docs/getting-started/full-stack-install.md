# Full-stack DisplayXR install

`scripts/setup-displayxr.sh` (macOS) and `scripts/setup-displayxr.bat`
(Windows) are one-command orchestrators that download each DisplayXR
component's release installer from GitHub Releases and run it. They
exist so contributors don't need to chase ~5 separate installer links
to get a working dev box.

> Tracking issue: [#283](https://github.com/DisplayXR/displayxr-runtime/issues/283).
> Companion [#284](https://github.com/DisplayXR/displayxr-runtime/issues/284)
> covers the end-user meta-installer; both consume the same
> [`versions.json`](../../versions.json) schema.

## Quick start

```bash
# macOS
git clone https://github.com/DisplayXR/displayxr-runtime
cd displayxr-runtime
./scripts/setup-displayxr.sh
```

That installs the runtime + bundled `sim-display` plug-in into
`/Library/Application Support/DisplayXR/` and registers it as the active
OpenXR runtime at `/etc/xdg/openxr/1/active_runtime.json`.

```bat
:: Windows — run from an ELEVATED command prompt
git clone https://github.com/DisplayXR/displayxr-runtime
cd displayxr-runtime
scripts\setup-displayxr.bat
```

That installs the runtime, the DisplayXR Shell, and the Leia SR plug-in
into `C:\Program Files\DisplayXR\Runtime\` and registers each at
`HKLM\Software\DisplayXR\…`. The runtime also becomes the active
OpenXR runtime via `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime`.

## Flags

| Flag | Effect |
|---|---|
| _(none)_ | macOS: runtime only. Windows: runtime + Shell + Leia plug-in. |
| `--with mcp` | Also install DisplayXR MCP Tools (macOS: warn-and-skip until a `.pkg` is published). |
| `--with-demos` | Clone each `DisplayXR/displayxr-demo-*` repo into `./demos/<name>/`. Does not build them. |
| `--dry-run` | Print every download / install command without running it. |
| `--uninstall` | macOS: chain `/Library/Application Support/DisplayXR/uninstall.sh`. Windows: silent-uninstall every `Publisher=DisplayXR` component. |
| `-h`, `--help` | Show usage. |

`--with` and `--with-demos` combine freely. `--dry-run` works with all
other flags.

## Prerequisites

### macOS

- macOS 13+ (the runtime targets the macOS 13 SDK; the `.pkg` itself enforces a min-OS check).
- [GitHub CLI](https://cli.github.com/) (`brew install gh`) authenticated via `gh auth login`.
- `sudo` (one prompt up front; the orchestrator keeps the timestamp alive so per-component installs don't re-prompt).

### Windows

- Windows 10 21H2+ or Windows 11.
- [GitHub CLI](https://cli.github.com/) (`winget install --id GitHub.cli`) authenticated via `gh auth login`. Same requirement as `scripts\build_windows.bat`.
- **Elevated command prompt** — right-click cmd.exe and choose "Run as administrator". The NSIS installers all write to HKLM + Program Files; non-elevated runs fail with a clearer error than individual installers report.
- PowerShell (preinstalled). The `.bat` shells out for JSON parsing and the publisher-scan uninstall path.

## Platform availability matrix

What this orchestrator can actually install depends on which component
repos ship an asset for your platform:

| Component   | Repo                                  | macOS today                        | Windows today                            |
|-------------|---------------------------------------|------------------------------------|------------------------------------------|
| runtime     | `displayxr-runtime`                   | yes (`DisplayXR-Installer-*.pkg`)  | yes (`DisplayXRSetup-*.exe`)             |
| shell       | `displayxr-shell-releases`            | — (macOS port deferred, M6)        | yes (`DisplayXRShellSetup-*.exe`)        |
| leia_plugin | `displayxr-leia-plugin`               | — (vendor SDK is Windows-only)     | yes (`DisplayXRLeiaSRSetup-*.exe`)       |
| mcp_tools   | `displayxr-mcp`                       | — (future)                         | yes (`DisplayXRMCPSetup-*.exe`)          |

Empty cells warn-and-skip cleanly: the orchestrator does not abort when
a component lacks an asset for the pinned tag.

## `versions.json`

Top-level pin file. Bump a value here to roll the dev box forward:

```json
{
  "runtime":     "v1.4.0",
  "shell":       "v1.2.5",
  "leia_plugin": "v1.0.1",
  "mcp_tools":   "v0.3.1",
  "demos": {
    "gaussiansplat": "v1.3.1"
  }
}
```

Each value must be a tag that exists in the corresponding repo's GitHub
Releases — both orchestrators validate with `gh release view <tag>`
before attempting any download.

Repo URLs and asset name globs live alongside in
[`scripts/lib/components.sh`](../../scripts/lib/components.sh) (sourced
by the macOS script; mirrored inline at the top of the Windows `.bat`
because batch can't source bash). The same table is intended to be
re-used by future scripts in `displayxr-installer` (#284) so all
consumers agree on what each component is.

## Manual fallback

The orchestrators do exactly this, plus pin validation, sudo / elevation
priming, smoke verification, and graceful handling of missing-asset edge
cases.

### macOS

```bash
gh release download v1.4.0 \
    --repo DisplayXR/displayxr-runtime \
    --pattern 'DisplayXR-Installer-*.pkg' \
    --dir /tmp/dxr
sudo installer -pkg /tmp/dxr/DisplayXR-Installer-*.pkg -target /
```

### Windows

```bat
:: From an elevated cmd
gh release download v1.4.0 --repo DisplayXR/displayxr-runtime --pattern "DisplayXRSetup-*.exe" --dir %TEMP%\dxr
%TEMP%\dxr\DisplayXRSetup-*.exe /S
gh release download v1.2.5 --repo DisplayXR/displayxr-shell-releases --pattern "DisplayXRShellSetup-*.exe" --dir %TEMP%\dxr
%TEMP%\dxr\DisplayXRShellSetup-*.exe /S
gh release download v1.0.1 --repo DisplayXR/displayxr-leia-plugin --pattern "DisplayXRLeiaSRSetup-*.exe" --dir %TEMP%\dxr
%TEMP%\dxr\DisplayXRLeiaSRSetup-*.exe /S
```

## Verifying the install

### macOS

After a successful run you should see:

```
/Library/Application Support/DisplayXR/
├── DisplayProcessors/200-sim-display.json
├── lib/openxr_displayxr.dylib
├── lib/displayxr/plugins/DisplayXR-SimDisplay.dylib
├── openxr_displayxr.json
└── uninstall.sh
/etc/xdg/openxr/1/active_runtime.json -> /Library/Application Support/DisplayXR/openxr_displayxr.json
```

To run a test app against the system install:

```bash
XR_RUNTIME_JSON="/Library/Application Support/DisplayXR/openxr_displayxr.json" \
    ./_package/DisplayXR-macOS/bin/cube_handle_metal_macos
```

The log should contain `active plug-in: id=sim-display`.

### Windows

Registry markers each component writes:

```
HKLM\Software\DisplayXR\Runtime\InstallPath              = C:\Program Files\DisplayXR\Runtime
HKLM\Software\DisplayXR\WorkspaceControllers\shell       (Shell)
HKLM\Software\DisplayXR\DisplayProcessors\leia-sr        (Leia plug-in)
HKLM\Software\DisplayXR\Capabilities\MCP\Enabled = 1     (MCP Tools)
HKLM\Software\Khronos\OpenXR\1\ActiveRuntime             (active runtime symlink-equivalent)
```

To launch the spatial workspace shell:

```bat
"C:\Program Files\DisplayXR\Runtime\displayxr-shell.exe" path\to\app.exe
```

The shell auto-starts the service, sets `XR_RUNTIME_JSON`, and launches
the app inside a spatial window.

## Uninstalling

### macOS

```bash
./scripts/setup-displayxr.sh --uninstall
```

Chains `/Library/Application Support/DisplayXR/uninstall.sh` (which
ships inside the `.pkg`) so the uninstall logic stays a single source
of truth.

### Windows

```bat
:: From an elevated cmd
scripts\setup-displayxr.bat --uninstall
```

Scans `HKLM\…\Uninstall\*` for entries with `Publisher=DisplayXR` and
runs each one's `QuietUninstallString` (falling back to
`UninstallString /S`). This covers runtime, Shell, Leia plug-in, and
MCP Tools in one pass regardless of which were installed.

Cloned demo directories under `./demos/` are left in place on both
platforms — they may carry local changes and the orchestrator refuses
to blindly remove them.

## Follow-ups

- `--from-source` flag — issue #283's power-user path (clone each repo
  + build from source). Defers until the binary path is in steady
  state.
- Code signing / notarization for the downloaded `.pkg` and `.exe`
  installers — tracked as issues
  [#280](https://github.com/DisplayXR/displayxr-runtime/issues/280) and
  [#281](https://github.com/DisplayXR/displayxr-runtime/issues/281).
- Bundling MCP Tools on macOS — blocked on upstream `displayxr-mcp`
  shipping a `.pkg`.
- End-user meta-installer (single download for non-developers) —
  [#284](https://github.com/DisplayXR/displayxr-runtime/issues/284).
