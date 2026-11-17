# dxr-get — fetch, verify and install DisplayXR components

Epic [#2702](https://github.com/DisplayXR/displayxr-runtime/issues/2702) · P1 [#2704](https://github.com/DisplayXR/displayxr-runtime/issues/2704) · P2 [#2705](https://github.com/DisplayXR/displayxr-runtime/issues/2705) · catalog: `component-catalog.md` (PR #2706)

`dxr-get.exe` lets one DisplayXR component installer bring in the components it depends on.
The first user is the DisplayXR Browser installer, which used to leave a box with a missing or
too-old runtime in a broken state with only a warning. It is a small standalone Windows
executable (`src/xrt/targets/get/`, static CRT, links nothing from the runtime), so any installer
can embed it and run it on a machine that has no DisplayXR yet. The runtime installer also
installs it into the runtime directory.

## Commands

```
dxr-get [--catalog <url|file>] plan --for <component> [--min-runtime X.Y.Z] [--json]
dxr-get [--catalog <url|file>] ensure-for <component> [--min-runtime X.Y.Z] [--yes] [--no-start-service]
dxr-get [--catalog <url|file>] fetch <component> [--out <dir>]
dxr-get [--catalog <url|file>] install <component>
dxr-get [--catalog <url|file>] detect [--json]
dxr-get [--catalog <url|file>] catalog
```

Components: `runtime`, `leia_plugin`, `browser`. Human-readable log goes to stderr; stdout carries
only results (`plan` text or JSON, the `fetch` path, `detect` output).

- **`ensure-for <c>`** is what installers call. It makes sure the prerequisites of component
  `<c>` are present, then stops. It does not install `<c>` itself (the caller is doing that), and
  it is not an updater:
  - **runtime:** installed if missing or older than the floor. The floor is `--min-runtime` when
    given, else the catalog's `<c>.requires.runtime`, raised to the Leia plug-in's own floor when
    the plug-in is wanted.
  - **leia_plugin:** wanted only on a **high-confidence** catalog detect rule matching this
    machine (§Detection). It is installed if missing. It is also brought to the catalog version
    when the runtime is being installed or upgraded in the same pass, because the catalog pins
    are an ABI-matched set.
  - **Order:** everything is downloaded and verified first, then the DisplayXR service and its
    satellites (`displayxr-conversion-host`, `displayxr-tracking-host`) are stopped, the
    installers run (runtime first), and the service starts once at the end.
  - **Starting the service:** through the user's `explorer.exe`, so it runs at Medium integrity
    (#1478). Skip this with `--no-start-service`, for a caller that restarts the service itself
    (the bundle).
- **`plan`** prints what `ensure-for` would do. The `--json` form has one flat object per
  component: `component`, `installed` (or null), `state`
  (`ok | missing | too_old | not_fetchable`), `required`, `target`, `size` (bytes) and `reason`.
- **`install <c>`** fetches and installs one component, and does nothing if the installed
  version is at or above the catalog's.
- **`detect`** shows the evidence (monitor EDID ids) and whether the Leia rules match.

## Exit codes

| code | meaning |
|---|---|
| 0 | done, or nothing to do |
| 2 | usage |
| 10 | offline: catalog unreachable, unparsable, unsupported schema, or **expired** |
| 11 | catalog signature invalid, or its serial is older than one this machine already accepted |
| 20 | download failed (after retries on every URL) |
| 21 | verification failed: size, SHA-256, or Authenticode signer |
| 30 | a component installer exited non-zero |
| 40 | the required component has no signed Windows build in the catalog that satisfies the floor |
| 50 | installing needs an elevated process |

## Trust rules

- **Catalog signature:** Ed25519 over the exact bytes of `stable.json`, checked against keys
  compiled into dxr-get (key id `9b762f9a`). The catalog's `expires` must lie in the future.
  An expired catalog is treated as **offline**: it can make dxr-get do nothing, never install.
- **Rollback guard:** the highest serial accepted is kept in
  `HKLM\SOFTWARE\DisplayXR\Update\CatalogSerial`, and an older serial is refused.
- **Every payload is checked:** size and SHA-256 must equal the catalog's. Then `WinVerifyTrust`
  must report a valid Authenticode signature whose leaf signer CN equals both the catalog's
  `signer` and an entry in the compiled-in list (`Leia, Inc.`). The download is named by its hash
  (`%ProgramData%\DisplayXR\Downloads\<sha256>-<file>`), resumes with `Range`, and falls back
  from the release URL to the GitHub API asset URL (the 504 case).
- **What dxr-get never does:**
  - downgrade a component;
  - write `Trust\Allowlist`;
  - touch `ActiveRuntime` (the runtime installer owns it; see below);
  - follow a redirect to plain HTTP.
- **Dev overrides:**
  - `DXR_GET_CATALOG=<url|file>` points dxr-get at another catalog. That catalog must still be
    signed, so the override grants nothing.
  - `DXR_GET_DEV=1` plus `--no-verify-catalog` skips the catalog signature.
  - `DXR_GET_ALLOW_UNSIGNED=1` skips the Authenticode check. The hash check always stays on.

## Detection (P2)

The rules are data in the signed catalog (`leia_plugin.detect`), generated from the Leia
plug-in's own EDID table (`src/drv_leia/leia_edid_table.h` in displayxr-leia-plugin). Supporting
a new panel therefore needs a plug-in release and a catalog re-sign, not a new dxr-get.

- **`edid`** matches `{man, prod}` against the present monitors' EDIDs. Both 16-bit words are
  read little-endian, the plug-in table's convention: Dell shows as `man` 44048 (`0xAC10`). A Leia
  panel can carry another brand's manufacturer id (the 8K Leia display enumerates as Dell
  `DEL4147`), which is why the product id is part of every rule. The full EDID value is read,
  because 8K panels carry DisplayID extension blocks well past 256 bytes.
- **`registry_dword_min`** means the SR platform reports a connected device:
  `HKLM\SOFTWARE\Dimenco\Simulated Reality\NConnectedDevices >= 1`.
- **Low-confidence rules** (the generic Acer product ids 0–3) are reported by `detect` but never
  trigger an install.

## Runtime installer: ActiveRuntime on upgrade

Because installers can now chain the runtime installer silently, an **upgrade** no longer takes
`ActiveRuntime` back from another runtime the user activated on purpose, such as SteamVR or
Oculus. The installer still lists DisplayXR in `AvailableRuntimes`.

| situation | ActiveRuntime |
|---|---|
| fresh install | written (unchanged behaviour) |
| upgrade, value empty, ours (path contains `\DisplayXR\`), or pointing at a missing manifest | written |
| upgrade, value points at another runtime's existing manifest | **left as is**, with a log line |
| `/KEEPACTIVERUNTIME` | never written |
| `/FORCEACTIVERUNTIME` | always written (the bundle's explicit repair) |

The bundle installer keeps re-asserting ActiveRuntime itself, as its repair contract requires.
`displayxr-cli runtime activate` switches back at any time.
