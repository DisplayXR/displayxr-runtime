# Component catalog (`catalog/v1/stable.json`)

Status: P0 of epic #2702 (self-fetching installers), issue #2703.

The component catalog is a small signed JSON index of the DisplayXR installers a
client may fetch on demand. Its one consumer today is **`dxr-get`** (P1/P2 of the same
epic), which the browser installer and other component installers call to bring in a
missing or too-old runtime, and a display plug-in when its display is detected (any vendor's:
see §Display plug-ins, #2737).

| | |
|---|---|
| Catalog | `https://updates.displayxr.org/catalog/v1/stable.json` |
| Signature | `https://updates.displayxr.org/catalog/v1/stable.json.sig` |
| Generator | `scripts/gen_catalog.py` (`generate`, `verify`) |
| Publisher | `.github/workflows/publish-catalog.yml` |
| Hosted by | GitHub Pages of `DisplayXR/displayxr-browser`, files at `feed/catalog/v1/` (its `pages.yml` serves `feed/` as the site root, same as `feed/feed.json` → `/feed.json`) |

## Schema 1

UTF-8, LF, no BOM, `indent=1`, key order as written by the generator, trailing newline.

```json
{
 "schema": 1,
 "serial": 1794877934,
 "generated": "2026-11-17T01:12:14Z",
 "expires": "2026-12-17T01:12:14Z",
 "channel": "stable",
 "source": { "versions_json_sha": "<displayxr-runtime commit the pins came from>" },
 "signers": { "windows": ["Leia, Inc."] },
 "components": {
  "runtime": {
   "recommended": "2.68.0",
   "label": "DisplayXR runtime",
   "windows": {
    "version": "2.68.0",
    "file": "DisplayXRSetup-2.68.0.exe",
    "size": 50034808,
    "sha256": "<64 lowercase hex>",
    "signer": "Leia, Inc.",
    "urls": ["<browser_download_url>", "https://api.github.com/repos/<o>/<r>/releases/assets/<id>"],
    "args": ["/S", "/NOSTART"],
    "registry_version": "HKLM\\Software\\DisplayXR\\Runtime\\Version"
   }
  },
  "leia_plugin": {
   "recommended": "2.21.1", "kind": "display_plugin", "label": "Leia display plug-in",
   "requires": { "runtime": ">=2.66.8" },
   "windows": { "...": "as above, args [\"/S\"], registry HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version" },
   "detect": [
    { "kind": "edid", "man": 44048, "prod": 16711, "confidence": "high", "note": "DELL_8K32" },
    { "kind": "registry_dword_min", "key": "HKLM\\SOFTWARE\\Dimenco\\Simulated Reality",
      "value": "NConnectedDevices", "min": 1, "confidence": "high", "note": "..." }
   ]
  },
  "browser": { "recommended": "1.10.3", "requires": { "runtime": ">=2.62.0" }, "windows": { "...": "args [\"/S\"]" } }
 }
}
```

Field rules:

- `serial` — unix seconds at generation; strictly increasing. Clients refuse a serial lower
  than the last one they accepted (rollback protection).
- `expires` — `generated` + 30 days. An expired catalog is treated as *offline*, never as
  authority. The weekly scheduled run re-signs well before that.
- `recommended` — the `versions.json` pin, leading `v` stripped.
- `label` — the human name `dxr-get` shows in its plan (the one consent box an installer
  shows). From the generator's component table, or a display plug-in's detect manifest.
  Optional: clients fall back to compiled-in names, then to the component key.
- `kind` — `"display_plugin"` marks a vendor display plug-in. Clients evaluate **every**
  component of that kind the same way (its `detect` rules); no vendor is special.
- `windows` — present only if the release has the asset **and** its Authenticode signature is
  `Valid` with leaf signer CN in `signers.windows` (checked by the generator with
  `Get-AuthenticodeSignature` on a Windows runner). Absent = "not fetchable". When a new pin's
  asset is not listable yet (e.g. the signed asset has not replaced the unsigned CI one), the
  generator **carries forward** the previously published, signature-verified `windows` block,
  so `windows.version` can lag `recommended` until the next run.
- `urls` — `urls[0]` is the release download URL; `urls[1]` the API asset URL (fetch with
  `Accept: application/octet-stream`).
- `requires.runtime` — the consumer's own advertised floor, `>=X.Y.Z`, read at the pinned tag:
  browser = `MIN_RUNTIME_VERSION` in browser-pvt `installer/DisplayXRBrowserInstaller.nsi`
  (location taken from `downstream-pins.json` `consumer_floors.displayxr-browser-pvt.declared_floor`);
  a display plug-in = its detect manifest's `requires.runtime`; for a Leia release without a
  manifest, the `DXR_RUNTIME_GIT_TAG` pin that leia-plugin's `installer/CMakeLists.txt` derives
  `MIN_RUNTIME_VERSION` from. Omitted when it cannot be read — never guessed.
- `<display plug-in>.detect` — the rules below, from the plug-in release's detect manifest
  (§Display plug-ins). Clients install a plug-in unattended only on a `high` match.

Clients ignore unknown keys and unknown components.

### Detect rules

| `kind` | fields | matches when |
|---|---|---|
| `edid` | `man`, and `prod` **or** `prod_min` + `prod_max` (inclusive) | a present monitor's EDID has that manufacturer id and a product id in range. Both are 16-bit words read **little-endian** from EDID bytes 8-9 / 10-11, decimal (Dell = `44048` = `0xAC10`) — the Leia plug-in table's convention. |
| `usb` | `vid`, and `pid` **or** `pid_min` + `pid_max` | a present USB device (e.g. a tracking camera) has that VID:PID. Decimal. |
| `registry_dword_min` | `key` (`HKLM\...`), `value`, `min` | the DWORD (or decimal REG_SZ) under HKLM, 64-bit view, is `>= min`. |

Every rule has `confidence` (`high` or `low`) and an optional `note` (shown as evidence).
A rule with only `man` (or only `vid`) matches any product of that vendor — Dell and Acer ship
ordinary 2D monitors too — so it is **always low confidence**, whatever it claims: the
generator downgrades it and `dxr-get` enforces the same. A rule of an unknown `kind` is
dropped by the generator and ignored by clients (forward compatibility), and a malformed rule
matches nothing. Linux/macOS evidence is not defined yet.

## Display plug-ins (#2737)

A display plug-in describes itself. Each release attaches a **`displayxr-detect.json`**
asset, schema 1:

```json
{
 "schema": 1,
 "label": "Leia display plug-in",
 "registry_version": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version",
 "requires": { "runtime": ">=2.66.8" },
 "detect": [
  { "kind": "edid", "man": 44048, "prod": 16711, "confidence": "high", "note": "DELL_8K32" },
  { "kind": "registry_dword_min", "key": "HKLM\\SOFTWARE\\Dimenco\\Simulated Reality",
    "value": "NConnectedDevices", "min": 1, "confidence": "high" }
 ]
}
```

- `schema` (required, `1`) and `detect` (required, at least one valid rule). `label`,
  `registry_version` (`HKLM\...\ValueName`, where the plug-in's installer writes its version) and
  `requires.runtime` (`>=X.Y.Z`) are optional; an invalid optional field is ignored with a
  warning, an invalid rule is dropped with a warning.
- The vendor generates it at build time from the same table its plug-in reads (Leia: from
  `src/drv_leia/leia_edid_table.h`), so the installer's idea of "this is our panel" and the
  plug-in's cannot diverge.
- Check one locally: `python scripts/gen_catalog.py validate-detect displayxr-detect.json`.

The generator's component list is a table (`COMPONENTS` in `gen_catalog.py`), not code: every
`kind: display_plugin` row is handled the same way. For each, it fetches the manifest from the
pinned release and uses its `label`, `requires.runtime`, `registry_version` (into
`windows.registry_version`) and `detect`. A release **without** the manifest falls back to the
row's `fallback` readers — for Leia, today's parse of `leia_edid_table.h` (one `edid` rule per
row; `prod < 16` → `low`, the generic Acer DS ids; a row without its own comment inherits the
previous row's note) plus the fixed SR-platform registry rule, and its installer CMake's floor.
A row with neither gets no `detect` and is therefore never installed unattended.

**Onboarding a display vendor** is then: one `COMPONENTS` row (`kind: "display_plugin"`, repo,
asset name, args, and its Authenticode CN in `signers`), a `versions.json` pin, and
`displayxr-detect.json` on each release. A row's `signers` are accepted by the generator for
that component only, and every signer a listed component carries is published in the top-level
`signers.windows`. `dxr-get` trusts that list because the catalog is signed (`dxr-get.md`
§Trust rules), so a new vendor signer needs no `dxr-get` release.

## Signing

Ed25519 over the exact bytes of `stable.json`. `stable.json.sig` = base64 of the 64-byte
signature (88 chars) + `\n`.

| Key id | Public key (base64, raw 32 bytes) |
|---|---|
| `9b762f9a` | `dPxNHS5QLtsrezVWd0ZeOYcNVVFKwuoTu8bzUMEu9DQ=` |

Key id = first 8 hex of sha256(raw public key). The private seed (base64 of 32 bytes) is the
Actions secret `DXR_CATALOG_ED25519_KEY` in displayxr-runtime; the generator refuses a key
whose id is not in its trusted list. Public keys are hardcoded in `gen_catalog.py`
(`PUBLIC_KEYS`) and compiled into `dxr-get` as a list.

Check any catalog: `python scripts/gen_catalog.py verify path/to/stable.json`.

### Rotation

1. Generate a new keypair offline; add its public key to `PUBLIC_KEYS` and to `dxr-get`'s
   compiled-in list (keep the old one).
2. Release a runtime carrying that `dxr-get`, and let it reach installers (the browser
   installer embeds the `dxr-get` of its pinned runtime).
3. Replace the `DXR_CATALOG_ED25519_KEY` secret; the next publish signs with the new key.
4. Only after old `dxr-get` builds are out of service, drop the old key from both lists.

A compromised key is handled the same way, with step 4 brought forward: shipping a `dxr-get`
without the key is the only revocation.

## Publishing

`publish-catalog.yml` (windows-latest) runs on a push to `main` touching `versions.json`
(or the generator/workflow), after a successful `Bump versions.json` run (sibling bumps
commit with `[skip ci]` and create no push run), weekly, and on dispatch. It mints the
publish-bot App token (`displayxr-browser` to write; `displayxr-browser-pvt` to read the
browser's floor), generates with `--previous` = the live catalog, verifies, and commits
`feed/catalog/v1/stable.json` + `.sig` to `displayxr-browser` `main`. That commit is **not**
`[skip ci]`: it must trigger that repo's push-driven `pages.yml` deploy.

Local dry run (no key needed): `python scripts/gen_catalog.py generate --out out --no-sign`.
