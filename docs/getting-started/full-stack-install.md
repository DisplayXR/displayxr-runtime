# Full-stack DisplayXR install (macOS)

`scripts/setup-displayxr.sh` is a one-command orchestrator that downloads
each DisplayXR component's release asset from GitHub Releases and installs
it. It exists so contributors don't need to chase ~5 separate installer
links to get a working dev box.

> Tracking issue: [#283](https://github.com/DisplayXR/displayxr-runtime/issues/283).
> Companion [#284](https://github.com/DisplayXR/displayxr-runtime/issues/284)
> covers the end-user meta-installer; both consume the same
> [`versions.json`](../../versions.json) schema.

## Quick start

```bash
git clone https://github.com/DisplayXR/displayxr-runtime
cd displayxr-runtime
./scripts/setup-displayxr.sh
```

That installs the runtime + bundled `sim-display` plug-in into
`/Library/Application Support/DisplayXR/` and registers it as the active
OpenXR runtime at `/etc/xdg/openxr/1/active_runtime.json`.

## Flags

| Flag | Effect |
|---|---|
| _(none)_ | Install runtime only. |
| `--with mcp` | Also install DisplayXR MCP Tools (when a macOS asset is available — warn+skip otherwise). |
| `--with-demos` | Clone each `DisplayXR/displayxr-demo-*` repo into `./demos/<name>/`. Does not build them. |
| `--dry-run` | Print every download / install command without running it. |
| `--uninstall` | Chain the runtime's bundled uninstaller at `/Library/Application Support/DisplayXR/uninstall.sh`. |
| `-h`, `--help` | Show usage. |

`--with` and `--with-demos` combine freely. `--dry-run` works with all
other flags.

## Prerequisites

- macOS 13+ (the runtime targets the macOS 13 SDK; the `.pkg` itself
  enforces a min-OS check).
- [GitHub CLI](https://cli.github.com/) (`brew install gh`) authenticated
  via `gh auth login`. Same requirement as `scripts/build_windows.bat`.
- `sudo` (one prompt up front; orchestrator keeps the timestamp alive so
  per-component installs don't re-prompt).

## Platform availability matrix

What this orchestrator can actually install today depends on which
component repos ship a macOS asset:

| Component   | Repo                                  | macOS today | Windows today |
|-------------|---------------------------------------|-------------|---------------|
| runtime     | `displayxr-runtime`                   | yes (post v1.3.6) | yes |
| shell       | `displayxr-shell-releases`            | — (deferred)      | yes |
| leia_plugin | `displayxr-leia-plugin`               | — (vendor SDK is Windows-only) | yes |
| mcp_tools   | `displayxr-mcp`                       | — (future)        | yes |

Empty cells warn-and-skip cleanly: the orchestrator does not abort when a
component lacks a macOS asset for the pinned tag. (This also covers the
pre-v1.3.6 transition window where the runtime release itself has no
`.pkg` attached — that work landed in
[PR #279](https://github.com/DisplayXR/displayxr-runtime/pull/279) and
the first release after it is where the macOS asset first appears.)

## `versions.json`

Top-level pin file. Bump a value here to roll the dev box forward:

```json
{
  "runtime":     "v1.3.5",
  "shell":       "v1.2.5",
  "leia_plugin": "sr-sdk-v1.35.0.2011",
  "mcp_tools":   "v0.3.1",
  "demos": {
    "gaussiansplat": "v1.3.1"
  }
}
```

Each value must be a tag that exists in the corresponding repo's GitHub
Releases — the orchestrator validates with `gh release view <tag>` before
attempting any download.

Repo URLs and asset name globs live alongside in
[`scripts/lib/components.sh`](../../scripts/lib/components.sh). The same
file is intended to be sourced by future scripts in `displayxr-installer`
(#284) so both the dev orchestrator and the user meta-installer agree on
what each component is.

## Manual fallback

If you can't (or don't want to) use the orchestrator, the equivalent
manual flow is:

```bash
# 1. Runtime + sim-display plug-in
gh release download v1.3.5 \
    --repo DisplayXR/displayxr-runtime \
    --pattern 'DisplayXR-Installer-*.pkg' \
    --dir /tmp/dxr
sudo installer -pkg /tmp/dxr/DisplayXR-Installer-*.pkg -target /

# 2. (Optional) Demo
gh repo clone DisplayXR/displayxr-demo-gaussiansplat demos/displayxr-demo-gaussiansplat
```

The orchestrator does exactly this, plus pin validation, sudo timestamp
caching, smoke verification, and graceful handling of missing-asset
edge cases.

## Verifying the install

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

To run a test app against the system install, point any of the existing
run scripts at the system runtime manifest:

```bash
XR_RUNTIME_JSON="/Library/Application Support/DisplayXR/openxr_displayxr.json" \
    ./_package/DisplayXR-macOS/bin/cube_handle_metal_macos
```

The log should contain `active plug-in: id=sim-display`.

## Uninstalling

```bash
./scripts/setup-displayxr.sh --uninstall
```

This chains `/Library/Application Support/DisplayXR/uninstall.sh` (which
ships inside the `.pkg`) so the uninstall logic stays a single source of
truth. Cloned demo directories under `./demos/` are left in place — they
may carry local changes and the orchestrator refuses to blindly `rm -rf`
them.

## Follow-ups

- Windows `setup-displayxr.bat` — separate PR. Blocks on the Leia
  plug-in cutting its first user-facing installer release.
- `--from-source` flag — issue #283's power-user path. Defers until the
  binary path is in steady state.
- Code signing / notarization for the downloaded `.pkg` — tracked as
  issues [#280](https://github.com/DisplayXR/displayxr-runtime/issues/280)
  and [#281](https://github.com/DisplayXR/displayxr-runtime/issues/281).
- End-user meta-installer (single download for non-developers) —
  [#284](https://github.com/DisplayXR/displayxr-runtime/issues/284).
