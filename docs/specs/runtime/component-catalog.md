# Component catalog (`catalog/v1/stable.json`)

Status: schema 1 is P0 of epic #2702 (self-fetching installers), issue #2703, and is what is
published and consumed today. [Schema 2](#schema-2) (the public DisplayXR Catalog,
[ADR-064](../../adr/ADR-064-component-catalog-and-third-party-trust.md)) is proposed; the
generator can derive it and `dxr-get` reads it (ADR-064 steps 2 and 3, schema 1 as the
fallback); nothing publishes it yet.

The component catalog is a small signed JSON index of the DisplayXR installers a
client may fetch on demand. Its one consumer today is **`dxr-get`** (P1/P2 of the same
epic), which the browser installer and other component installers call to bring in a
missing or too-old runtime, and the Leia SR plug-in when a Leia display is detected.

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
   "recommended": "2.21.1", "kind": "display_plugin",
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
  Leia = the `DXR_RUNTIME_GIT_TAG` pin that leia-plugin's `installer/CMakeLists.txt` derives
  `MIN_RUNTIME_VERSION` from. Omitted when it cannot be read — never guessed.
- `leia_plugin.detect` — one `edid` rule per row of leia-plugin's
  `src/drv_leia/leia_edid_table.h` at the pinned tag (`man` = EDID bytes 8-9 little-endian,
  `prod` = bytes 10-11 little-endian, decimal; `prod < 16` → `low` (generic Acer DS ids),
  else `high`; a row without its own comment inherits the previous row's note), plus the
  fixed SR-platform registry rule. Clients install the plug-in unattended only on a `high` match.
  That parse is the **fallback**: when the pinned release carries a `displayxr-detect.json`
  (see [Display plug-in detect manifest](#display-plug-in-detect-manifest)), its rules, floor and
  registry key are used instead. For Leia both give the same bytes.

Clients ignore unknown keys and unknown components, and accept `schema == 1` only: any other
value is treated as an unreachable catalog. Schema 1 is **frozen** (see
[Compatibility with schema 1](#compatibility-with-schema-1)).

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

### Publishing schema 2

The same workflow can also publish `catalog/v2/stable.json`. It is **off by default** and is
turned on by the repository variable `DXR_PUBLISH_CATALOG_V2` = `true` (Settings, Variables),
or for one manual run by the `publish_v2` input of the workflow's dispatch form.

| | Off (the default) | On |
|---|---|---|
| Generator call | `generate --out out`, as before | the same call with `--out-v2 out-v2`: schema 2 is derived in the same run, with the same serial and the same key |
| Published | `feed/catalog/v1/stable.json` + `.sig` | those, plus `feed/catalog/v2/stable.json` + `.sig`, in the same commit |

Schema 1 never depends on schema 2:

- asking for schema 2 does not change one byte of the schema-1 document (pinned by
  `scripts/tests/test_gen_catalog.py`);
- if the combined generator run fails, the step starts over with the schema-1-only command.
  A schema-2 defect cannot stop the schema-1 publish. A schema-1 failure fails the job, as
  it always did;
- the schema-2 document is checked in its own step (`verify`, and that its serial and expiry
  equal the schema-1 document's). That step cannot fail the job. When it does not pass, the
  v2 files are not copied and the commit carries schema 1 alone;
- a v2 document that was published earlier is left as it is in that case. It stays valid
  until its own `expires`, then clients treat it as offline and fall back to schema 1.

What turning it on changes for users: a `dxr-get` that reads schema 2 (ADR-064 step 2 and
later) stops falling back to schema 1 and uses the v2 document. Older `dxr-get` builds never
request it. Turning the variable off again stops updates; the published v2 document then
expires within 30 days and clients fall back.

## Schema 2

Status: **Proposed** ([ADR-064](../../adr/ADR-064-component-catalog-and-third-party-trust.md)).
`gen_catalog.py generate --out-v2 DIR` derives a schema-2 document for the three first-party
components. `dxr-get` reads it and falls back to schema 1 while it is not published
([dxr-get.md](dxr-get.md)); nothing publishes it yet. Everything in this section that
the generator does not emit is specification for the steps of ADR-064's implementation plan,
and is marked *(planned)* where that matters.

Schema 2 is the format of the public **DisplayXR Catalog**: items from several publishers, of
every component class, on several platforms.

| | |
|---|---|
| Catalog | `https://updates.displayxr.org/catalog/v2/stable.json` *(planned URL)* |
| Signature | the same URL + `.sig` |
| Encoding | as schema 1: UTF-8, LF, no BOM, `indent=1`, trailing newline. The signature covers the exact bytes |

### Document

```json
{
 "schema": 2,
 "serial": 1795236734,
 "generated": "2026-11-20T04:52:14Z",
 "expires": "2026-12-20T04:52:14Z",
 "channel": "stable",
 "source": { "versions_json_sha": "<commit>" },
 "publishers": { "<publisher-id>": { } },
 "revoked": [ { "sha256": "<64 hex>", "reason": "…" } ],
 "items": [ { } ]
}
```

| Field | Req | Rule |
|---|---|---|
| `schema` | yes | `2` |
| `serial` | yes | unix seconds, strictly increasing. Generated in the same run as the schema-1 document and equal to its serial |
| `generated`, `expires` | yes | `YYYY-MM-DDTHH:MM:SSZ`. `expires` = `generated` + 30 days |
| `channel` | yes | `stable` |
| `source` | yes | where the index was built from. First-party derivation: `versions_json_sha`. Registry build *(planned)*: `registry_sha` |
| `publishers` | yes | object keyed by publisher id. See below |
| `revoked` | no | asset digests a client must refuse to install even when an item still lists them |
| `items` | yes | array, in registry order |

### Publisher

```json
"acme": {
 "name": "Acme Displays",
 "url": "https://acme.example",
 "support_url": "https://acme.example/support",
 "signers": {
  "windows-x64": [ { "kind": "authenticode", "subject_cn": "Acme Displays, Inc." } ],
  "linux-x86_64": [ { "kind": "ed25519", "key_id": "1a2b3c4d", "public_key": "<base64, 32 bytes>" } ]
 }
}
```

| Field | Req | Rule |
|---|---|---|
| key | yes | publisher id, `[a-z0-9][a-z0-9-]*`. Never reused for another party |
| `name` | yes | display name |
| `url`, `support_url` | no | `https` |
| `signers.<platform>` | no | the signing identities **delegated to this publisher** on that platform. An asset's signature is accepted only if it matches one of its own publisher's signers for the asset's platform. A publisher with no signers for a platform can list link-outs only on it: every fetchable asset is signed |
| signer `kind: authenticode` | | `subject_cn` = the leaf certificate's subject (its simple display name), compared exactly, as schema 1 does. Pinning the subject organisation or the issuer as well is a possible later tightening, not part of schema 2: it resists a look-alike certificate better, and makes a publisher's renewal or change of authority break installs until the registry is updated |
| signer `kind: ed25519` | | for platforms with no OS signer check: a detached signature `<asset>.sig` over the asset bytes, `key_id` = first 8 hex of sha256(raw public key) *(planned, step 5)* |

### Item

| Field | Req | Rule |
|---|---|---|
| `id` | yes | catalog id, `[a-z0-9][a-z0-9-]*`, or `<publisher-id>.<name>` for a third-party item. Unique across `id` and `aliases` of the whole document. Never reused |
| `aliases` | no | other names a client accepts for this item. The first-party items carry their schema-1 component name here (`leia-sr` ← `leia_plugin`), so `ensure-for leia_plugin` keeps resolving |
| `class` | yes | `runtime`, `display_processor`, `input_provider`, `conversion_provider`, `tracking_provider`, `workspace_controller`, `companion`, `app`, `sdk`. A client skips an item whose class it does not know |
| `registered_id` | for the six registered classes | the id the component registers under in its class root (`DisplayProcessors\<id>`, `InputProviders\<id>`, `ConversionProviders\<id>`, `TrackingProviders\<id>`, `WorkspaceControllers\<id>`; POSIX: the manifest `id`). It is what `displayxr-cli <class> list` shows, and how a client joins a catalog item to an installed component |
| `also_provides` | no | `[{ "class", "registered_id" }]` for a plug-in that carries components of more than one class (ADR-054 §5) |
| `name` | yes | display name |
| `summary` | no | one line, plain text, at most 200 characters |
| `publisher` | yes | a key of `publishers` |
| `tier` | no | `listed` or `verified`. Absent = `listed`. See § Trust |
| `conformance` | with `verified` | `{ "suite": "selftest", "runtime": "X.Y.Z", "date": "YYYY-MM-DD", "report_url": "https://…" }`: the class conformance run that earned the tier, and the runtime it ran against |
| `version` | yes | the publisher's current release, `X.Y.Z`. An asset's own `version` may lag it (see `assets`) |
| `abi` | no | object keyed by class: `{ "display_processor": { "min": 5, "max": 5 } }`. The class ABI **majors** the binary negotiates. The loader rejects a major mismatch (ADR-020), so a client refuses to install an item whose range does not contain the installed runtime's major for that class, instead of installing something that will be rejected at load. Omitted when not known; never guessed |
| `runtime` | no | `{ "min": "X.Y.Z", "max": "X.Y.Z" }`, either bound optional. The runtime versions the item works with. `min` is the item's advertised floor. `max` is for a known break, not a prediction |
| `requires` | no | array. See § Requires. A `{ "kind": "runtime", "version": ">=X.Y.Z" }` entry is the same statement as `runtime.min`; when both are present they must agree. The first-party derivation emits the `requires` form because that is what schema 1 carries |
| `assets` | no | object keyed by platform. Absent = nothing fetchable (a link-out, or a pin whose asset is not listable yet) |
| `provenance` | no | how the installed component binaries pass the load gate. See § Trust |
| `license` | no | see § Licence. Absent = no statement; a client shows "see publisher" |
| `detect` | no | array of rules, as schema 1's `leia_plugin.detect`, for any item. A `high`-confidence match pre-ticks the item in a setup UI and lets `ensure-for` install it unattended; a `low` one is reported only |
| `recommended` | no | `true` puts the item in the recommended set a setup UI pre-ticks without a hardware match (the open conversion provider) |
| `links` | no | `{ "homepage", "support", "privacy", "eula", "source", "listing" }`, each `https`. `listing` is the target of a link-out item |

**Platforms.** `windows-x64`, `linux-x86_64`, `macos-universal`, `android-arm64`. A client
ignores platforms it does not know.

**`abi` on the `runtime` item** states the class ABI majors that runtime version
*implements*. A client uses it for the runtime a plan is about to install; for the runtime
already on the machine it reads what that runtime's installer recorded
(`HKLM\Software\DisplayXR\Runtime\Abi\<class>` on Windows). With neither, the class ABI is
not checked and the client says so.

#### Asset

```json
"windows-x64": {
 "version": "2.23.0",
 "file": "DisplayXRLeiaSRSetup-2.23.0.exe",
 "size": 31457280,
 "sha256": "<64 lowercase hex>",
 "urls": ["https://github.com/<o>/<r>/releases/download/v2.23.0/<file>",
          "https://api.github.com/repos/<o>/<r>/releases/assets/<id>"],
 "signature": { "kind": "authenticode", "signer": "Leia, Inc." },
 "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
 "installed": { "kind": "registry_sz", "path": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version" },
 "uninstall": { "kind": "registry_command", "path": "HKLM\\Software\\DisplayXR\\DisplayProcessors\\leia-sr\\UninstallString", "args": ["/S"] }
}
```

| Field | Req | Rule |
|---|---|---|
| `version` | yes | the version of **this file**. It can be older than the item's `version` when the newest release is not listable yet (schema 1's carry-forward) |
| `file` | yes | file name to save as. No path separators |
| `size` | yes | bytes, positive integer |
| `sha256` | yes | 64 lowercase hex, of the file |
| `urls` | yes | non-empty, `https` only, tried in order. Publisher-hosted. A URL under `api.github.com` is fetched with `Accept: application/octet-stream` |
| `signature` | yes, unless `listing` | `{ "kind": "authenticode", "signer": "<CN>" }` or `{ "kind": "ed25519", "key_id": "…" }`. The named signer must be one of the publisher's `signers` for this platform; the registry and the generator's `verify` both check it. **Required on every asset that is not `listing`**: there is no unsigned, hash-pinned-only form |
| `installer.kind` | yes, unless `listing` | `exe` (a self-contained silent installer run with `args`), `msi`, `deb`, `pkg`. A client refuses a kind it does not implement. There is no "run this script" kind |
| `installer.args` | no | arguments for the silent install. Fixed strings: no placeholders, no shell |
| `installer.scope` | yes | `machine` or `user` |
| `installer.elevation` | yes | `required` or `none`. A client that is not elevated relaunches itself elevated for `required` |
| `installed` | yes, unless `listing` | how to read the installed version. `{ "kind": "registry_sz", "path": "HKLM\\…\\Value" }` (64-bit view), or `{ "kind": "registration_row" }` = the `Version` value of the item's own class row *(planned, step 3)*, or `{ "kind": "manifest" }` on POSIX |
| `uninstall` | no | how to remove it. `{ "kind": "registry_command", "path", "args" }` reads a command line from the registry (the class row's `UninstallString`) and runs it with `args`; `{ "kind": "package", "name" }` on POSIX. Absent = the client offers no uninstall and says so *(planned, step 3)* |
| `listing` | no | `{ "url": "https://…" }`: a **listing-only** asset. Nothing is downloaded or run; the client shows the link. All Android assets are listing-only (ADR-038), and so are `app` and `sdk` items from third parties |

#### Requires

```json
"requires": [
 { "kind": "runtime", "version": ">=2.66.8" },
 { "kind": "item", "id": "open-conversion", "version": ">=0.1.0" },
 { "kind": "external", "name": "Acme Depth Runtime", "min_version": "3.2",
   "detect": [ { "kind": "registry_sz_min", "key": "HKLM\\SOFTWARE\\Acme\\DepthRuntime", "value": "Version", "min": "3.2" } ],
   "info_url": "https://acme.example/depth-runtime" }
]
```

| Kind | Meaning | What a client does |
|---|---|---|
| `runtime` | the DisplayXR runtime, `>=X.Y.Z` | installs or upgrades the runtime first, as `ensure-for` does today |
| `item` | another catalog item, optional `>=X.Y.Z` | installs it first |
| `external` | something DisplayXR does not ship: a vendor service, a vendor model runtime | **detects** it with the `detect` rules and reports met / unmet before install, with `info_url`. Whether it also installs it depends on the form, below |

An `external` entry has two supported forms:

- **Declared and detected only** (no `asset`). The client installs nothing for it. An unmet
  prerequisite is shown before install with `info_url`; the user may still install the item.
- **Declared with its own installer asset.** The entry carries an `asset` keyed by platform,
  with the fields of an item asset (`version`, `file`, `size`, `sha256`, `urls`, `signature`,
  `installer`), signed by a signer delegated to the item's publisher:

  ```json
  { "kind": "external", "name": "Acme Depth Runtime", "min_version": "3.2",
    "detect": [ { "kind": "registry_sz_min", "key": "HKLM\\SOFTWARE\\Acme\\DepthRuntime", "value": "Version", "min": "3.2" } ],
    "info_url": "https://acme.example/depth-runtime",
    "asset": { "windows-x64": {
      "version": "3.2.1", "file": "AcmeDepthRuntimeSetup-3.2.1.exe", "size": 20971520, "sha256": "<64 hex>",
      "urls": ["https://downloads.acme.example/runtime/AcmeDepthRuntimeSetup-3.2.1.exe"],
      "signature": { "kind": "authenticode", "signer": "Acme Displays, Inc." },
      "installer": { "kind": "exe", "args": ["/quiet"], "scope": "machine", "elevation": "required" } } } }
  ```

  The client installs it **only when `detect` says it is absent**, never to upgrade one that
  is present, and **in order: before the item**. Its `size` is added to the item's, and the
  total is shown before the user commits. It is fetched and verified like any asset (below).
  Like every asset, it must be signed.

**Every asset is resumed and verified** (the schema-1 tool's rule, applied to every asset of
a plan since ADR-064 step 2). One item can now mean several downloads: its own asset and the assets of
its external prerequisites. For each one a client resumes an interrupted download with an
HTTP `Range` request, accepts only a complete file of exactly `size` bytes, compares its
SHA-256 with `sha256`, and then checks the signature. All assets of a selection are fetched
and verified before the first installer runs. What `dxr-get` does today, read from
`get_http.c`, `get_main.c` and `dxr-get.md`: it resumes a `.part` file with `Range` (and
starts over when the server ignores the range), retries transport errors with backoff, falls
back to the next URL, names the finished file by its hash so a cached copy is reused only if
it hashes to the catalog's digest, discards a file whose hash differs, and has no override
for the hash check. What is new is doing that for more than one asset per item, in a stated
order, with the total size known up front.

**EDID ids are little-endian words.** In an `edid` rule, `man` and `prod` are the 16-bit
words at EDID bytes 8-9 and 10-11, **both read little-endian**, written in decimal. That is
how `dxr-get` reads a monitor (`get_detect.c`: `edid[8] | edid[9] << 8`) and the convention
of the display plug-in's own table. Dell's bytes `0x10 0xAC` are therefore `man` = `44048`
(`0xAC10`), and product `0x4147` is `prod` = `16711`. It is not the big-endian word the
three-letter PNP id is packed in: a rule written from the PNP id would be `4268` (`0x10AC`)
for Dell and would never match.

`detect` rule kinds:

| `kind` | fields | matches when |
|---|---|---|
| `edid` | `man`, and `prod` **or** `prod_min` + `prod_max` (inclusive) | a present monitor's EDID has that manufacturer id and a product id in range. Both are 16-bit words from EDID bytes 8-9 / 10-11, decimal (Dell = `44048`), the Leia table's convention. |
| `usb` | `vid`, and `pid` **or** `pid_min` + `pid_max` (inclusive) | a present USB device (e.g. a tracking camera) has that VID:PID, decimal. Windows: SetupAPI's `USB` enumerator, so interface children count too. |
| `registry_dword_min` | `key` (`HKLM\...`), `value`, `min` | the DWORD (or decimal REG_SZ), 64-bit view, is `>= min` |
| `registry_sz_min` | `key`, `value`, `min` (dotted version) | the REG_SZ is a version at or above `min` |
| `registry_key_exists` | `key` | the HKLM key exists |
| `file_exists` | `path` | the file exists; an absolute path, no environment expansion |
| `service_exists` | `name` | a Windows service of that name is installed, running or not |

Every rule may carry `confidence` (`high` or `low`; required on `edid`, `usb`,
`registry_dword_min`) and `note` (shown as evidence). An `edid` rule with only `man`, or a
`usb` rule with only `vid`, matches any product of that vendor (Dell and Acer ship ordinary 2D
monitors), so it is **always low confidence**, whatever it claims: the generator downgrades it
and `dxr-get` enforces the same. `high` means an unattended install, so the generator allows
it only on evidence that names the product: a product-specific `edid` or `usb` rule, or
`registry_dword_min` (a device count the vendor's own platform reports). A `high` on
`registry_sz_min`, `registry_key_exists`, `file_exists` or `service_exists` — all true on
ordinary machines — is downgraded to `low` with a warning. A rule carries only its kind's fields
plus `kind`, `confidence` and `note`; the generator strips any other key, so a vendor manifest
cannot add unreviewed keys to the signed catalog, and `validate_v2` rejects an item's or an
external requirement's `detect` rule that is not already in that clean form. A malformed rule (a
half or inverted range, a product id given both exactly and as a range, a value outside 16
bits) matches nothing. A client treats a rule of an unknown kind as not matched and goes on to
the next rule; the generator drops it. Item ids and aliases `edids`, `usb` and
`display_plugins` are reserved: they are top-level keys of `dxr-get detect --json`.
`edid` ranges and `usb` are #2737 additions: a `dxr-get` from before them skips a range rule
(no `prod`) and a `usb` rule, which is why the generator still emits one exact `edid` rule per
Leia table row rather than compacting runs into ranges.

#### Licence

```json
"license": {
 "class": "paid",
 "spdx": "LicenseRef-Acme-EULA",
 "price_label": "from US$49",
 "purchase_url": "https://acme.example/buy/depth-pro",
 "sources": ["hardware", "key", "trial"],
 "activation": ["deep_link", "key"]
}
```

| Field | Rule |
|---|---|
| `class` | `open` (an OSI licence, `spdx` required), `free` (no charge, proprietary), `paid`, `hardware` (licensed with the vendor's hardware) |
| `spdx` | SPDX expression or `LicenseRef-…`. The full text is at `links.eula` |
| `price_label` | free text, shown as written. DisplayXR does not parse or verify it and takes no part in the sale |
| `purchase_url` | `https`, the **vendor's** checkout. Required for `paid` |
| `sources` | which entitlement sources the item may use, any of `hardware` (entitled by a connected display), `key` (a purchased or issued key), `trial` (a time-limited trial the plug-in grants), `oem` (an entitlement that came with the machine). Lets a front end say "included with supported displays" or "free trial" before install. Absent = no statement |
| `activation` | which user-driven methods the plug-in supports: `deep_link` (`displayxr://activate`), `key` (a pasted key stored as a `secret` setting). Required when `sources` includes `key`. `hardware`, `oem` and `trial` need no activation |

The catalog describes the terms. The **state** on a machine comes from the installed plug-in
through its licence-state slot *(planned, step 6)*, never from the catalog:

| Reported field | Req | Values |
|---|---|---|
| state | yes | `licensed`, `trial`, `expired`, `unlicensed` |
| message | no | one line for the user |
| purchase URL | no | overrides the manifest's `purchase_url` when present |
| source | no | `hardware`, `key`, `trial`, `oem`: where the current entitlement comes from |
| expiry | no | when the current entitlement ends (a trial, a term licence). Absent = it does not end |

`licensed` with source `hardware` is a first-class state: the connected display entitles the
user with no purchase and no click, and a front end shows "Included with your display" and
no Buy / Activate action. A front end shows `expiry` when it is present.

Installing never needs a licence, an account or a key. Any hardware or entitlement check is
made inside the vendor's own binary; the runtime and any public adapter in front of vendor
code only carry the state it reports. There is no DRM in the runtime.

### Trust

**Every installable binary is signed.** An item with a fetchable asset is listed only if the
installer and the component binaries it installs are signed by a signer delegated to its
publisher. `dxr-get` does not write `Trust\Allowlist` rows (the #2702 rule), so an unsigned
component binary could not pass the load gate, and the catalog does not list one.

**Tiers.** `listed`: the manifest validated and the assets are signed by the publisher's
delegated signer; DisplayXR has not tested the item. `verified`: the same, plus `conformance`
records a passed class conformance run. A client shows the tier wherever it shows the item.

**What a client checks before running an installer**, in order, failing closed:

1. the catalog signature, `expires` and serial (§ Signing and expiry);
2. the asset is not in `revoked`;
3. size and SHA-256 of the download equal the asset's;
4. the asset has a `signature`, the OS check passes (`WinVerifyTrust`), and the signer is one
   of the **item's publisher's** `signers` for this platform. There is no compiled-in signer
   list in a schema-2 client, and no way to install an asset without a signature.

**`provenance`** says how the *installed component binaries* (the DLL the loader opens, not
the installer) pass the load-time gate (plugin-discovery.md §4.3). It is informational: the
gate decides at every load from the file in front of it, and the engine writes nothing for it.

```json
"provenance": { "mode": "signed" }
```

| `mode` | Meaning |
|---|---|
| `signed` | the binaries carry the publisher's signature and read `signed` at the gate. The only mode on Windows |
| `platform-trusted` | POSIX packages installed into the root-owned locations the gate already vouches for |

**Registry rules** *(planned, step 1)*. A manifest is merged only if: its publisher exists
and the pull request comes from that publisher's registered maintainers; every URL is
`https` and resolves; the downloaded asset's size and hash match; every fetchable asset
is signed, the signature verifies and its signer is delegated to the publisher; `registered_id` is not claimed by
another publisher's item; and, for `verified`, the conformance report is attached. A new
publisher, and a new signer for an existing one, need a maintainer's review.

### Worked examples

Sizes, hashes, dates, floors, licence ids and third-party names in these examples are
illustrative. `acme` and `handco` are placeholders, not real publishers. The first example
has the exact shape the generator emits; the others show fields that are specified here and
filled in when an item is actually listed.

A display processor (the first-party Leia SR plug-in, as the generator emits it today; it
adds no `abi`, `tier` or `uninstall` because schema 1 does not carry them):

```json
{
 "id": "leia-sr",
 "aliases": ["leia_plugin"],
 "class": "display_processor",
 "registered_id": "leia-sr",
 "name": "Leia SR display processor",
 "publisher": "displayxr",
 "version": "2.23.0",
 "requires": [ { "kind": "runtime", "version": ">=2.66.8" } ],
 "assets": {
  "windows-x64": {
   "version": "2.23.0",
   "file": "DisplayXRLeiaSRSetup-2.23.0.exe",
   "size": 31457280,
   "sha256": "<64 hex>",
   "urls": ["<release URL>", "<API asset URL>"],
   "signature": { "kind": "authenticode", "signer": "Leia, Inc." },
   "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
   "installed": { "kind": "registry_sz", "path": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version" }
  }
 },
 "detect": [
  { "kind": "edid", "man": 44048, "prod": 16711, "confidence": "high", "note": "DELL_8K32" },
  { "kind": "registry_dword_min", "key": "HKLM\\SOFTWARE\\Dimenco\\Simulated Reality",
    "value": "NConnectedDevices", "min": 1, "confidence": "high", "note": "…" }
 ]
}
```

The open conversion provider (first-party, open licence, in the recommended set):

```json
{
 "id": "open-conversion",
 "class": "conversion_provider",
 "registered_id": "open-conversion",
 "name": "DisplayXR open conversion provider",
 "summary": "2D to 3D conversion with open models. The default converter.",
 "publisher": "displayxr",
 "tier": "verified",
 "conformance": { "suite": "selftest", "runtime": "2.69.2", "date": "2026-11-20", "report_url": "https://…" },
 "version": "0.1.1",
 "abi": { "conversion_provider": { "min": 1, "max": 1 } },
 "runtime": { "min": "2.45.0" },
 "recommended": true,
 "license": { "class": "open", "spdx": "BSL-1.0" },
 "assets": {
  "windows-x64": {
   "version": "0.1.1", "file": "DisplayXROpenConversionSetup-0.1.1.exe", "size": 12582912, "sha256": "<64 hex>",
   "urls": ["https://github.com/DisplayXR/displayxr-conversion-open/releases/download/v0.1.1/DisplayXROpenConversionSetup-0.1.1.exe"],
   "signature": { "kind": "authenticode", "signer": "Leia, Inc." },
   "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
   "installed": { "kind": "registration_row" },
   "uninstall": { "kind": "registry_command", "path": "HKLM\\Software\\DisplayXR\\ConversionProviders\\open-conversion\\UninstallString", "args": ["/S"] }
  }
 },
 "provenance": { "mode": "signed" },
 "links": { "source": "https://github.com/DisplayXR/displayxr-conversion-open" }
}
```

A vendor-licensed conversion provider with an external prerequisite (third party, paid,
its model runtime installed separately by the user):

```json
{
 "id": "acme.depth-pro",
 "class": "conversion_provider",
 "registered_id": "acme-depth-pro",
 "name": "Acme Depth Pro",
 "publisher": "acme",
 "tier": "verified",
 "conformance": { "suite": "selftest", "runtime": "2.69.2", "date": "2026-11-20", "report_url": "https://acme.example/dxr-conformance/1.4.0" },
 "version": "1.4.0",
 "abi": { "conversion_provider": { "min": 1, "max": 1 } },
 "runtime": { "min": "2.60.0" },
 "requires": [
  { "kind": "runtime", "version": ">=2.60.0" },
  { "kind": "external", "name": "Acme Depth Runtime", "min_version": "3.2",
    "detect": [ { "kind": "registry_sz_min", "key": "HKLM\\SOFTWARE\\Acme\\DepthRuntime", "value": "Version", "min": "3.2" } ],
    "info_url": "https://acme.example/depth-runtime" }
 ],
 "license": {
  "class": "paid", "spdx": "LicenseRef-Acme-EULA", "price_label": "from US$49",
  "purchase_url": "https://acme.example/buy/depth-pro", "sources": ["hardware", "key", "trial"], "activation": ["deep_link", "key"]
 },
 "assets": {
  "windows-x64": {
   "version": "1.4.0", "file": "AcmeDepthProSetup-1.4.0.exe", "size": 18874368, "sha256": "<64 hex>",
   "urls": ["https://downloads.acme.example/dxr/AcmeDepthProSetup-1.4.0.exe"],
   "signature": { "kind": "authenticode", "signer": "Acme Displays, Inc." },
   "installer": { "kind": "exe", "args": ["/quiet"], "scope": "machine", "elevation": "required" },
   "installed": { "kind": "registration_row" },
   "uninstall": { "kind": "registry_command", "path": "HKLM\\Software\\DisplayXR\\ConversionProviders\\acme-depth-pro\\UninstallString", "args": ["/quiet"] }
  }
 },
 "provenance": { "mode": "signed" },
 "links": { "support": "https://acme.example/support", "privacy": "https://acme.example/privacy", "eula": "https://acme.example/eula" }
}
```

Installing it is free. With the prerequisite missing the client says so before install and
shows `info_url` (this example uses the detection-only form). After install the provider
reports its own state: `licensed` with source `hardware` on a machine whose display entitles
it, otherwise `trial` or `unlicensed`; while it is `unlicensed` or `expired` selection falls
through to the next conversion provider.

An input provider with an external prerequisite and a **bounded runtime range** (third
party, Listed tier; its tracking service is installed separately, detection-only form):

```json
{
 "id": "handco.hands",
 "class": "input_provider",
 "registered_id": "handco-hands",
 "name": "Handco Hands",
 "publisher": "handco",
 "tier": "listed",
 "version": "0.3.0",
 "abi": { "input_provider": { "min": 1, "max": 1 } },
 "runtime": { "min": "2.60.0", "max": "2.69.99" },
 "requires": [
  { "kind": "runtime", "version": ">=2.60.0" },
  { "kind": "external", "name": "Handco Tracking Service", "min_version": "6.0",
    "detect": [ { "kind": "registry_key_exists", "key": "HKLM\\SOFTWARE\\Handco\\TrackingService" } ],
    "info_url": "https://handco.example/tracking-service" }
 ],
 "license": { "class": "free", "spdx": "LicenseRef-Handco", "sources": ["hardware"] },
 "assets": {
  "windows-x64": {
   "version": "0.3.0", "file": "HandcoHandsSetup-0.3.0.exe", "size": 2097152, "sha256": "<64 hex>",
   "urls": ["https://github.com/handco/dxr-hands/releases/download/v0.3.0/HandcoHandsSetup-0.3.0.exe"],
   "signature": { "kind": "authenticode", "signer": "Handco Ltd" },
   "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
   "installed": { "kind": "registration_row" },
   "uninstall": { "kind": "registry_command", "path": "HKLM\\Software\\DisplayXR\\InputProviders\\handco-hands\\UninstallString", "args": ["/S"] }
  }
 },
 "provenance": { "mode": "signed" },
 "links": { "support": "https://github.com/handco/dxr-hands/issues", "privacy": "https://handco.example/privacy" }
}
```

`runtime.max` is set when the publisher knows the item works only up to a runtime version.
An input provider is the usual case today: the devices it returns are the runtime's internal
device struct, which the input ABI major does not cover (ADR-063), so a provider is good for
the runtime range it was built and tested against. A client refuses to install the item on a
runtime outside the range and says which versions it needs; it never downgrades the runtime
to fit. `requires` carries the floor only, so the `runtime` object is where an upper bound
lives.

A workspace controller (no class ABI: it is a process contract over IPC, so compatibility
is the runtime range alone):

```json
{
 "id": "displayxr-shell",
 "class": "workspace_controller",
 "registered_id": "shell",
 "name": "DisplayXR Shell",
 "publisher": "displayxr",
 "version": "2.4.2",
 "runtime": { "min": "2.60.0" },
 "assets": {
  "windows-x64": {
   "version": "2.4.2", "file": "DisplayXRShellSetup-2.4.2.exe", "size": 12582912, "sha256": "<64 hex>",
   "urls": ["https://github.com/DisplayXR/displayxr-shell-releases/releases/download/v2.4.2/DisplayXRShellSetup-2.4.2.exe"],
   "signature": { "kind": "authenticode", "signer": "Leia, Inc." },
   "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
   "installed": { "kind": "registration_row" },
   "uninstall": { "kind": "registry_command", "path": "HKLM\\Software\\DisplayXR\\WorkspaceControllers\\shell\\UninstallString", "args": ["/S"] }
  }
 }
}
```

### Compatibility with schema 1

What deployed `dxr-get` builds do was read from `src/xrt/targets/get/get_main.c` on `main`
(not tested against a running binary):

| Behaviour | Where | Consequence |
|---|---|---|
| A catalog whose `schema` is not the number `1` is "unsupported schema", exit 10 (offline) | `load_catalog` | **Schema 2 cannot be served at the schema-1 URL.** It is a separate document, `catalog/v2/stable.json` |
| Every field is looked up by name (`cJSON_GetObjectItemCaseSensitive`); nothing enumerates keys | `jstr`, `win_block`, `plan_one`, `fetch_component` | unknown keys are ignored, at the top level, in a component and in a `windows` block |
| Components are looked up by the three names the code knows | `component`, `make_plan` | unknown components are ignored |
| A `detect` rule of an unknown `kind` is skipped | `detect_match` | new rule kinds would not break a schema-1 client |
| The signature covers the exact bytes; the payload signer must be in the catalog **and** in the compiled-in list | `load_catalog`, `fetch_component` | any byte change to the schema-1 file needs the same key, and a new signer cannot be introduced through schema 1 |
| The accepted serial is one registry value, `HKLM\SOFTWARE\DisplayXR\Update\CatalogSerial`, and a lower serial is refused | `load_catalog` | see the serial rule below |

Rules that follow:

- **Schema 1 is frozen.** Its shape, key order and encoding do not change, and it keeps being
  published at its URL for as long as a `dxr-get` that reads only schema 1 is in service (the
  browser installer embeds the `dxr-get` of its pinned runtime). `scripts/tests/test_gen_catalog.py`
  pins the bytes the generator writes for a fixed input.
- **Schema 2 is derived, not regenerated.** For the three schema-1 components,
  `build_catalog_v2` is a pure function of the schema-1 catalog of the same run: same
  serial, same assets, same hashes, same signer. It does not download or verify anything a
  second time, so the two documents cannot disagree about a file.
- **Schema 1 stays at three components.** Components that schema 1 cannot describe are
  listed in schema 2 only (`V2_ONLY_COMPONENTS`, see [Generating it](#generating-it)). They
  never enter the schema-1 document, so its bytes for a given input do not change.
- **Serials.** Both documents carry the same serial. A schema-2 client keeps its rollback
  serial in its own value (`CatalogSerialV2`), so an older schema-1 catalog accepted by an
  old `dxr-get` on the same machine can never make it refuse a schema-2 catalog, or the
  reverse.
- **Names.** A schema-2 client resolves a component by `id` or `aliases`, so the schema-1
  names in installers' command lines (`ensure-for browser`, `--for leia_plugin`) keep working.
- **Unknown values.** A schema-2 client ignores unknown keys, skips an item whose `class`,
  platform or `installer.kind` it does not know, and treats an unknown `detect` kind as not
  matched. It refuses, rather than skips, an unknown `signature.kind` and an installable
  asset with no `signature`.

### Signing and expiry

- One Ed25519 signature over the exact bytes of the document, in `stable.json.sig`, the same
  format and (today) the same key as schema 1. `gen_catalog.py verify` accepts both schemas
  and, for schema 2, also checks the structural rules above.
- `expires` = `generated` + 30 days. An expired catalog is *offline*: a client may do
  nothing on its authority. It never blocks anything already installed.
- A lower `serial` than the last accepted one is refused.
- **Root keys** *(planned, step 2)*: a schema-2 client compiles in two keys, the active one
  and a standby whose seed is kept offline. Rotation switches the signing secret to the
  standby and adds a new standby in the next client release.
- **Publisher signers** are data in the signed index. Adding one, replacing one and removing
  one are registry changes that take effect with the next index, with no client release. An
  offline client can act on a removed signer for at most the remaining life of the index it
  holds, 30 days.
- **`revoked`** lists asset digests to refuse regardless of signer, for one bad build.

### Generating it

```
python scripts/gen_catalog.py generate --out out --out-v2 out-v2 --no-sign
python scripts/tests/test_gen_catalog.py
```

`--out-v2` must be a different directory from `--out`. Without `--out-v2` the generator
behaves exactly as before; `publish-catalog.yml` does not pass it.

**Components listed in schema 2 only.** `V2_ONLY_COMPONENTS` in the generator holds the
first-party providers schema 1 has no words for. They are read from their own releases in
the same run (`build_extra_components`), with the same rule as schema 1: an asset is listed
only when its Authenticode signature is valid with a listed signer.

| Pin (`versions.json`) | Item `id` | Class | What schema 1 could not say | Where the number comes from |
|---|---|---|---|---|
| `conversion_open` | `open-conversion` | `conversion_provider` | `recommended: true`, the converter a setup UI pre-ticks (ADR-064 §7) | no floor is declared by that repository, so none is written |
| `leia_conversion` | `leia-conversion` | `conversion_provider` | an external prerequisite (the NeurD runtime), declared and detected only | floor = `DXR_CONV_MIN_RUNTIME_VERSION` in its `CMakeLists.txt` at the released tag |
| `ultraleap` | `ultraleap` | `input_provider` | an **exact** runtime range and an external prerequisite (the Ultraleap tracking software) | `runtime.min == runtime.max` = `DXR_RUNTIME_GIT_TAG` in its `CMakeLists.txt` at the released tag |

Rules that follow from "never guessed":

- A component with no `versions.json` pin has no release yet and is not listed (and costs
  no network access). `leia_conversion` and `ultraleap` are in that state today
  (versions-json-autobump.md, "A component with no release yet").
- If the Ultraleap provider's runtime pin cannot be read at its tag, the item is **not
  listed at all**: without the range it would read as "works on any runtime".
- The NeurD prerequisite carries no `info_url` and no installer `asset`: no public download
  of NeurD exists yet. When a public signed build exists the entry gains an asset.
- Its floor and its one detect rule are the **Windows** facts: 0.4.6 (the provider's
  `MinVersion`) and the `Version` value under `HKLM\SOFTWARE\LeiaInc\NeurD`, which NeurD's own
  installer writes. The rule's `note` says what it does not see. The Linux facts (below) are
  recorded here and in the generator, not in the document.

**What an external prerequisite cannot say.** Found while encoding NeurD; none of it is
worked around with an invented key or detector kind.

| Fact about NeurD | Expressible today | Why not |
|---|---|---|
| Windows floor 0.4.6 | yes: `min_version`, and `registry_sz_min` with `min` | |
| Linux: 0.4.6 converts, look-around needs 0.5.0 | **no** | `min_version` is one string for every platform; only `asset` is keyed by platform |
| Windows: registered under `HKLM\SOFTWARE\LeiaInc\NeurD` | yes: `registry_sz_min` | |
| Windows: found on `PATH` | **no** | no detector resolves a library name on `PATH` |
| Windows and Linux: the `NEURD_PATH` environment variable | **no** | no detector reads an environment variable; `file_exists` takes an absolute path with no expansion, on purpose |
| Linux: `/opt/immersity/neurd-runtime` | **no** | `detect` rules carry no platform, and `file_exists` accepts a Windows drive path only |
| Linux: the version in that directory's `VERSION.txt` | **no** | no detector reads a version from a file |
| macOS: nothing to install | yes: the item lists no macOS asset | |

Consequences today, all on the safe side: on Windows a NeurD present only through `PATH` or
`NEURD_PATH` is reported as unmet (the item still installs, and the provider finds it); the
authoritative answer on any box is `displayxr-cli conv status`. On Linux nothing is evaluated,
because the POSIX engine does not exist yet (ADR-064 step 5). What that step needs, as a
follow-up to this schema: a platform key on an external entry's `min_version` and `detect`,
and POSIX detector kinds (a file at an absolute POSIX path, a version read from a file, a
library that resolves).
- The Ultraleap asset name carries a build number (`DisplayXRUltraleapSetup-<v>.<build>.exe`);
  the highest build of the pinned version is taken.
- There is no carry-forward for these components yet: it needs a previously *published*
  schema-2 document, and none exists.

The validator additionally checks `runtime`: both bounds are `X.Y.Z`, `min` is not above
`max`, and `min` agrees with a `requires` entry of kind `runtime`.

### Display plug-in detect manifest

A display plug-in describes itself (#2737). Each release may attach a
**`displayxr-detect.json`** asset, schema 1:

```json
{
 "schema": 1,
 "id": "leia-sr",
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

| field | required | meaning |
|---|---|---|
| `schema` | yes | `1` |
| `detect` | yes | at least one valid rule (kinds above). Invalid rules are dropped with a warning |
| `id` | no | the schema-2 item id (and `registered_id`) for this plug-in. It **must equal the id the plug-in registers under** (`DisplayProcessors\<id>`, e.g. `leia-sr`), since the runtime and receipts key on it. Used when the generator has no `V2_ITEMS` entry for the component; without it such a component is not listed in schema 2. A first-party component's `V2_ITEMS` identity always wins |
| `label` | no | the schema-2 item `name` in that same case. Never written into schema 1 |
| `registry_version` | no | `HKLM\...\Value` where the plug-in's installer writes its version; becomes `windows.registry_version` |
| `requires.runtime` | no | `>=X.Y.Z`; becomes the component's `requires.runtime`. A floor **above the runtime pinned in `versions.json`** would make every `ensure-for` that detects the plug-in fail with exit 40 on every machine, so the generator then ignores the **whole** manifest (rules too) with a warning and uses the fallback, as for a release without one |

An invalid optional field is ignored with a warning; a manifest with the wrong `schema` or no
usable rule is rejected and the generator uses the fallback. For every `kind: display_plugin`
component the generator fetches the asset from the pinned release. A release without it falls
back to the per-vendor parse (Leia: `leia_edid_table.h` plus the installer CMake floor, as
above). The vendor generates the file at build time from the same table its plug-in reads, so
"the installer thinks this is our panel" and "the plug-in claims it" cannot diverge. Leia does
this from displayxr-leia-plugin#557 on.

Check one locally: `python scripts/gen_catalog.py validate-detect displayxr-detect.json`.
