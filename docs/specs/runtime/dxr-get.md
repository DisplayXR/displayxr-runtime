# dxr-get — fetch, verify, install and remove DisplayXR components

Epic [#2702](https://github.com/DisplayXR/displayxr-runtime/issues/2702) · P1 [#2704](https://github.com/DisplayXR/displayxr-runtime/issues/2704) · P2 [#2705](https://github.com/DisplayXR/displayxr-runtime/issues/2705) · [ADR-064](../../adr/ADR-064-component-catalog-and-third-party-trust.md) steps 2 and 3 · catalog: [component-catalog.md](component-catalog.md)

`dxr-get.exe` is the install engine of the DisplayXR Catalog. It reads the signed catalog,
works out what a component needs, downloads and verifies it, runs the installers in order,
and records what it installed. Its first user is the DisplayXR Browser installer, which calls
it to bring in a missing or too-old runtime.

It is a small standalone Windows executable (`src/xrt/targets/get/`, static CRT, links nothing
from the runtime), so any installer can embed it and run it on a machine that has no DisplayXR
yet. The runtime installer also installs it into the runtime directory.

The code has two halves:

| File | What it holds |
|---|---|
| `get_core.c` | every decision: the catalog model, the plan, compatibility, signer delegation, the inventory. No operating-system calls. Unit-tested on every platform (`tests/tests_get_catalog.cpp`) |
| `get_main.c`, `get_http.c`, `get_verify.c`, `get_detect.c` | Windows: the network, the registry, Authenticode, the installers |

## Commands

```
dxr-get [--catalog <url|file>] plan --for <item> | --default [--min-runtime X.Y.Z] [--json]
dxr-get [--catalog <url|file>] ensure-for <item> [--min-runtime X.Y.Z] [--yes] [--no-start-service]
dxr-get [--catalog <url|file>] install <item> | --default [--yes] [--json] [--no-start-service]
dxr-get [--catalog <url|file>] fetch <item> [--out <dir>]
dxr-get [--catalog <url|file>] detect [--json]
dxr-get [--catalog <url|file>] catalog
dxr-get [--catalog <url|file>] list [--json]
dxr-get [--catalog <url|file>] outdated [--json]
dxr-get list --installed [--json]
dxr-get uninstall <id> [--json] [--no-start-service]
```

Options for callers that are themselves installers:

| Option | Verbs | Meaning |
|---|---|---|
| `--no-runtime` | `plan`, `ensure-for`, `install`, `list` | never install or upgrade the runtime itself. § The runtime installer |
| `--assume-runtime X.Y.Z` | `list`, `plan` (accepted by `ensure-for` and `install` too) | answer as if runtime `X.Y.Z` were installed, whatever the machine has |
| `--assume-abi <class>=<major>` | with `--assume-runtime` | the assumed runtime implements that major of that class ABI. Repeatable |
| `--default` | `plan`, `install` | stands in for the item: the catalog's recommended set plus hardware matches. § install --default |

`<item>` is a catalog id (`dxr-get list` shows them) or one of an item's `aliases`. The
schema-1 names `runtime`, `leia_plugin` and `browser` keep working, with either schema.

The human-readable log goes to stderr. stdout carries only results: the plan, the `fetch`
path, `detect`, `catalog`, `list`, `outdated` and the `--json` documents.

| Verb | Needs the catalog | Needs elevation | What it does |
|---|---|---|---|
| `plan --for X` | yes | no | prints what `ensure-for X` would do. Changes nothing |
| `ensure-for X` | yes | when something must be installed | installs what X **needs**, not X itself |
| `install X` | yes | when something must be installed | installs X **and** what it needs |
| `install --default` | yes | when something must be installed | installs the recommended set and the hardware matches |
| `fetch X` | yes | no | downloads and verifies X's installer, prints its path |
| `detect` | yes | no | shows the monitor EDID ids and which items' detect rules match |
| `catalog` | yes | no | prints the catalog document as fetched |
| `list` | yes | no | every catalog item with its installed version and state |
| `outdated` | yes | no | the items whose installed version is behind the catalog's |
| `list --installed` | **no** | no | the install receipts on this machine |
| `uninstall <id>` | **no** | yes | removes an item that `dxr-get` installed |

### ensure-for

This is what installers call. It makes sure the prerequisites of item `X` are present, then
stops. It does not install `X` itself (the caller is doing that), and it is not an updater.

- **Runtime.** Installed if it is missing or older than the floor. The floor is
  `--min-runtime` when given, else `X`'s own (`requires` kind `runtime`, or `runtime.min`).
  It is raised to the floor of every other item this pass installs.
- **Items `X` requires** (`requires` kind `item`). Installed if missing or below the stated
  floor, before anything that needs them. They are not brought to the catalog's version
  when they already satisfy the floor.
- **External prerequisites of `X`** (`requires` kind `external`). Detected with the catalog's
  rules and reported as present or missing. One that carries its own installer asset is
  installed when it is absent, and never to upgrade one that is present.
- **Hardware items.** Any other item whose `detect` rules match this machine with **high**
  confidence (§ Detection) is installed if missing. It is also brought to the catalog's
  version when the runtime is being installed or upgraded in the same pass, because the
  catalog's pins are an ABI-matched set. Today that is the Leia display plug-in on a Leia
  panel. A hardware item that cannot be installed never fails the pass.
- **Order.** Everything is downloaded and verified first. Then the DisplayXR service and its
  satellites (`displayxr-conversion-host`, `displayxr-tracking-host`) are stopped, the
  installers run (the runtime first, then prerequisites before what needs them), and the
  service starts once at the end.
- **Starting the service.** Through the user's `explorer.exe`, so it runs at Medium integrity
  (#1478). `--no-start-service` skips this, for a caller that restarts the service itself
  (the bundle). A plan that installs only applications does not stop or start the service.

### install

`install X` is `ensure-for X` plus `X` itself: the same prerequisites in the same order, then
`X`, brought to the catalog's version. It does nothing when `X` is installed at or above the
catalog's version, and it never downgrades. Hardware detection plays no part: only what `X`
requires is installed with it.

### install --default

For a bundle or a setup program that has no component page of its own. One call installs:

- every item the catalog marks `recommended` (the open conversion provider), and
- every item whose `detect` rules match this machine with **high** confidence (the display
  processor for the panel that is attached),

each with what it requires, in the usual order. The runtime item itself is never part of the
set; it is installed or upgraded only as a prerequisite, and not at all with `--no-runtime`.

Every candidate is judged alone first. One that cannot be installed here (not compatible with
the runtime, nothing fetchable, needs a runtime upgrade that `--no-runtime` forbids) is listed
in the plan as skipped, with the reason, and **does not fail the rest**: the exit code is 0
when everything that could be installed was. Skip-if-current applies to each item, so a
second run downloads nothing. With no connection the result is the offline one (exit 10, what
was skipped, the command to run later). A schema-1 catalog has no `recommended` flag, so on
the fallback only hardware matches are selected.

`plan --default [--json]` prints the same plan without installing. `install … --json` prints
the plan as JSON instead of text, for both forms of `install`.

### plan

`plan --for X` prints the `ensure-for X` plan. The text form has one line per component,
including the ones that need nothing:

```
DisplayXR Runtime: 2.64.0 is too old -> update to 2.70.0 (47.7 MB)
Leia SR display processor: 2.23.0 is installed and is the catalog's version -> not downloaded
Prerequisite MISSING: Handco Tracking Service (needed by handco.hands). DisplayXR does not install it; get it from https://…
Total download: 47.7 MB
Already downloaded: 12.0 MB, still to fetch: 35.7 MB
```

`--json`:

```json
{"for":"browser","schema":2,"gate":0,"total_size":50034808,"total_download":50034808,
 "actions":[
  {"component":"runtime","installed":"2.64.0","state":"too_old","required":">=2.66.8","target":"2.70.0",
   "size":50034808,"id":"runtime","kind":"item","action":"upgrade","current":false,"download":50034808},
  {"component":"leia_plugin","installed":"2.23.0","state":"ok","target":"2.23.0","size":31457280,
   "reason":"detected: display EDID 44048/16711 DELL_8K32 (high confidence)",
   "id":"leia-sr","kind":"item","action":"none","current":true,"hash_match":true,"download":0}],
 "prerequisites":[{"name":"Handco Tracking Service","for":"handco.hands","met":false,"installable":false,"info_url":"https://…"}]}
```

| Field | Meaning |
|---|---|
| `gate` | `0`, `40` (a needed component cannot be had) or `41` (not compatible). `gate_reason` says why. `ensure-for` and `install` exit with it |
| `total_size` | bytes of every asset the plan would install |
| `total_download` | the same, minus what the download cache already holds |
| `actions[].component` | the schema-1 name of the item (its first alias, else its id); for an external prerequisite, its name |
| `actions[].state` | `ok`, `missing`, `too_old`, `not_fetchable`, `incompatible` |
| `actions[].action` | `none` (nothing to do), `install`, `upgrade`, `reinstall` (same version, another build), `skip` (cannot be installed) |
| `actions[].current` | installed at exactly the catalog's version |
| `actions[].hash_match` | present when a receipt exists for the installed version: whether the installed asset's SHA-256 equals the catalog's |
| `actions[].download` | bytes still to fetch for this action; `0` when nothing is downloaded |
| `actions[].kind` | `item`, or `external` for a prerequisite's own installer |
| `prerequisites[]` | every external prerequisite, met or not, and whether it carries an installer |

**The action objects are flat, and `component` is their first key.** Deployed installers scan
this output with a string search, from `"component":"` to the first `}`, and read `state`,
`target` and `size` from that span (the DisplayXR Browser installer does). New fields are
added after the schema-1 ones, and nothing is ever nested in an action. A unit test
reproduces that scan.

### Skip-if-current

An item installed at the catalog's version is not downloaded again. The check is the
installed version (read the way the catalog says, § Receipts) against the asset's version,
and, when a receipt exists for that version, the receipt's SHA-256 against the asset's.

| Installed | Receipt hash | `ensure-for` | `install` |
|---|---|---|---|
| older than the floor | any | upgraded | upgraded |
| satisfies the floor, older than the catalog | any | left alone | upgraded |
| equal to the catalog | equal, or no receipt | not downloaded | not downloaded |
| equal to the catalog | **differs** | left alone | re-installed (`action: reinstall`) |
| newer than the catalog | any | left alone | left alone (never downgraded) |

A download that is already in the cache is not fetched again either; see § Downloads.

### list, outdated

`list` shows every item of the catalog whose class this build knows: id, class, installed
version, the catalog's version and a state (`not_installed`, `current`, `outdated`, `newer`,
`rebuilt` = same version but another build than the receipt records, `installed` = present
but the catalog has no build to compare with). It includes components that were installed by
their own installers, because the installed version is read from the machine, not from a
receipt. `outdated` is the same list filtered to `outdated` and `rebuilt`. Both exit 0.

`--json` gives `{"schema":2,"items":[…]}` or `{"schema":2,"outdated":[…]}`. One call answers
a setup page: no second fetch is needed for detection, sizes or compatibility.

| Field | Meaning |
|---|---|
| `id`, `name`, `class`, `registered_id`, `publisher`, `tier` | from the catalog item |
| `installed`, `available` | the installed version and the catalog's, or null |
| `state` | as above |
| `installable` | the catalog has a signed Windows build of it |
| `receipt` | `dxr-get` installed it |
| `listing` | for a link-out: where to get it |
| `summary` | the catalog's one-line description, when it has one |
| `aliases` | other names the item answers to (the schema-1 name). Always an array |
| `recommended` | the catalog's flag |
| `detected` | `high`, `low` or `none`: the best confidence among the item's detect rules that match this machine. `evidence` says what matched |
| `size` | bytes of the item's own asset |
| `download` | bytes `install <id>` would still fetch **for this item**: its asset plus the installer assets of its unmet external prerequisites, minus what the download cache holds. `0` when nothing would be installed. The runtime and other required items are not included; they have rows of their own |
| `compatible` | whether `install <id>` would be allowed by the runtime range and class ABI rules (§ Compatibility gating). `incompatible_reason` is the message when it is not |
| `prerequisites` | one entry per external prerequisite: `name`, `met`, `installable` (it carries its own installer), `info_url` |

`compatible` is judged the way `install` judges it, so by default a runtime that is too old
for the item is not a reason: `install` would upgrade it. With `--no-runtime` or
`--assume-runtime` the runtime is taken as fixed, and an item that needs a newer one is not
compatible. The fields that existed before these were added are unchanged.

`list --installed` reads the install receipts and nothing else. It needs no catalog and no
network. `--json` gives `{"installed":[…]}` with `id`, `name`, `class`, `publisher`,
`version`, `source_url`, `sha256`, `install_time`, `can_uninstall` and `uninstall_command`.

### uninstall

`uninstall <id>` removes an item that `dxr-get` installed. It needs no catalog and no
network: everything comes from the receipt.

The checks run in this order, and the first that fails is the answer. What elevation cannot
fix comes first, so a caller is told to elevate only when elevating is all that is missing
(otherwise it would elevate, run again, and only then learn it could never work):

1. No receipt for that id: exit 42. A component installed by its own installer is removed
   from Windows Settings.
2. The receipt holds no uninstall command (the catalog gave none): exit 43, elevated or not.
   The message names the item and says what to do instead: remove it from Windows Settings >
   Apps > Installed apps, or run its own uninstaller. **This is the answer for every item
   installed from the schema-1 catalog**, which carries no uninstall rules; so until schema 2
   is published, `uninstall` can only ever answer 42 or 43.
3. Another receipt lists this id in its requirements: exit 43, naming them.
4. Not elevated: exit 50.
5. The service and its satellites are stopped when the item's class is one the runtime or
   the service loads. The receipt's command runs. `dxr-get` waits for it, then waits up to
   two minutes for the item's installed-version value to disappear, because an NSIS
   uninstaller copies itself to `%TEMP%` and returns at once.
6. On success the receipt is deleted and the service is started again (not after removing
   the runtime). If the uninstaller fails, or the component still reports a version, the
   receipt is kept and the exit code is 30.

`--json` prints `{"id","result","exit","detail"}`; `result` is `uninstalled`,
`not_installed`, `no_uninstall_command`, `required_by_others`, `needs_elevation` or `failed`.

External prerequisites installed on the way are not uninstalled and get no receipt: they
have their own licences and other software may depend on them.

## The runtime installer

The runtime installer shows catalog items on its component page and installs the chosen ones
after its own files are in place. Two options exist for it.

**`--assume-runtime X.Y.Z`** makes `list` and `plan` answer for the runtime that is about to
be installed instead of the one on the machine: the runtime's installed version is taken to
be `X.Y.Z`, and the class ABI values in the registry, which describe the runtime installed
*now*, are ignored. The class ABI of the assumed runtime comes from `--assume-abi
<class>=<major>` (the installer has those numbers; it writes the same ones to
`Runtime\Abi`), else from the catalog's runtime item when `X.Y.Z` is exactly the version it
describes, else it is not checked.

```
dxr-get list --json --assume-runtime 2.70.0 --assume-abi display_processor=5 --assume-abi conversion_provider=1
```

**`--no-runtime`** forbids installing or upgrading the runtime itself. `dxr-get` must not
start a second runtime installer from inside the first. A plan that would need to move the
runtime is refused instead: exit **41**, with a message that names the version needed, the
version present and the switch. Nothing is downloaded. An item the present runtime already
satisfies installs normally. `install runtime --no-runtime` is refused the same way unless
the runtime is already at the catalog's version.

## Which catalog

With `--catalog` or `DXR_GET_CATALOG`, that one document is used, whichever schema it is.

Otherwise `dxr-get` fetches schema 2 (`catalog/v2/stable.json`) and **falls back to schema 1**
(`catalog/v1/stable.json`) when the schema-2 document is not published (any HTTP error),
unreadable, of an unknown schema, or expired. It does not fall back when the schema-2
signature or serial is refused (exit 11: that is a refusal, not an absence), and it does not
try schema 1 when the host could not be reached at all (same host, and the second attempt
would only double the wait).

A schema-1 catalog is converted to the schema-2 model when it is parsed, so every verb
behaves the same on both. That conversion is the only place the three schema-1 component
names appear in the code. What schema 1 does not carry is simply absent: no class ABI, no
uninstall command, no external prerequisites.

## Compatibility gating

An item is refused, with exit **41** and a message naming the versions, when:

- its `runtime.max` is below the runtime this machine has, or would have after the plan. The
  runtime is never downgraded, or held back, to fit;
- its `runtime.min` is above what the machine has and the catalog's runtime cannot reach it;
- its `abi` range for a class does not contain the major the runtime implements for that
  class. The loader would reject the plug-in (ADR-020), so it is not installed.

The runtime's class ABI majors come from `HKLM\Software\DisplayXR\Runtime\Abi\<class>`
(DWORD values `display_processor`, `input_provider`, `conversion_provider`,
`tracking_provider`), which the runtime installer writes from the class headers. Next to
them it writes `Abi\RuntimeVersion`, the version that wrote them, and `dxr-get` uses the
values **only while that stamp equals `Runtime\Version`**. An older runtime installed over a
newer one does not know the key and leaves it behind; without the stamp the gate would read
the newer runtime's ABI for the older one. A stamp that is absent or names another version
means the same as no values at all: not checked. When the
runtime is being replaced in the same plan, the catalog's runtime item says what the new one
implements (its own `abi` object). When neither source has the number, the ABI is **not
checked** and the plan says so (`… ABI not checked: the runtime does not publish it`): an
unknown ABI is not a refusal. Runtimes released before this change do not write the values.

The refusal applies to the item asked for and to the items it requires. A hardware item
picked up by detection that is out of range is reported as `incompatible` and skipped; it
does not fail the pass. `plan` exits 41 too, so an installer can tell the user before it asks
for consent.

A floor that comes from `requires` (or `--min-runtime`) and cannot be satisfied keeps the
schema-1 result, exit 40.

## Prerequisites

`requires` entries are resolved before the item, depth first:

| Kind | What happens |
|---|---|
| `runtime` | raises the runtime floor. The runtime is planned first |
| `item` | that item is planned before this one, with its own requirements before it. An unknown id or a cycle fails the plan (exit 40) instead of looping |
| `external`, no asset | detected and reported. A missing one is shown with its `info_url`; the item is still installed |
| `external`, with an asset for `windows-x64` | detected. When absent, its installer is downloaded, verified and run **immediately before the item**. Its size is part of the total. It must be signed by a signer delegated to the item's publisher, like any asset |

Detect rules for an external prerequisite are the catalog's `detect` kinds (§ Detection); the
prerequisite is met when any rule matches.

## Downloads

Every asset of a plan is fetched and verified **before the first installer runs**: the item's
own asset and the assets of its external prerequisites. The total size is printed before any
download starts.

What the downloader already did (P1), unchanged:

- the finished file is named by its hash (`%ProgramData%\DisplayXR\Downloads\<sha256>-<file>`)
  and a cached copy is used only if it hashes to the catalog's digest;
- an interrupted download is kept as `<name>.part` and resumed with an HTTP `Range` request;
  when the server ignores the range (answers 200) the download starts over;
- transport errors and 5xx answers are retried with backoff, four attempts per URL, then the
  next URL is tried (the release URL, then the GitHub API asset URL: the 504 case);
- only a complete file of exactly `size` bytes is accepted, its SHA-256 is compared, and a
  mismatch discards the file (exit 21). There is no override for the hash check.

What ADR-064 step 2 added:

- several assets per plan, each resumed and verified on its own, in plan order;
- the total size, and how much of it the cache already holds, known before the first byte
  (`total_size`, `total_download`, and `download` per action);
- a 206 answer whose `Content-Range` does not start at the offset asked for restarts the
  download, instead of appending the bytes at the wrong offset and failing the hash check at
  the end;
- the same size, hash and signer rules for an external prerequisite's asset.

## Receipts

After an item's installer exits 0, `dxr-get` writes a receipt under
`HKLM\Software\DisplayXR\Catalog\<id>` (64-bit view, `REG_SZ` values):

| Value | Content |
|---|---|
| `Name`, `Class`, `Publisher` | from the catalog item |
| `Version` | the version of the asset that was installed |
| `SourceUrl` | the asset's first URL |
| `Sha256`, `File` | the asset's digest and file name |
| `InstallTime` | UTC, `YYYY-MM-DDTHH:MM:SSZ` |
| `UninstallCommand` | the complete command line `uninstall` runs. Read **at install time** from where the catalog's `uninstall` rule points (`registry_command`: the registry value, then its `args`). The value is looked for in the 64-bit registry view, then in the 32-bit one, because a 32-bit installer writes its Add/Remove Programs entry there. Empty when the catalog gives no `uninstall` rule |
| `VersionPath` | the registry value that holds the item's installed version |
| `Requires` | comma-separated ids of the items this one required (`runtime` for a runtime floor) |

The installed version of an item is read, in this order: from the asset's `installed` rule
(`registry_sz`: a registry value; `registration_row`: the `Version` value of the item's own
class row, `HKLM\Software\DisplayXR\<ClassRoot>\<registered_id>\Version`); for an item with
no asset here, from the runtime's version value or the class row; and only when the catalog
gives nothing to read, from the receipt.

A receipt exists only for what `dxr-get` installed. A component installed by its own
installer or by the bundle has none: it shows in `list` and `outdated`, not in
`list --installed`, and `uninstall` answers 42 for it. The generator emits an `uninstall`
rule for the Leia plug-in and the browser (`V2_ITEMS` in `scripts/gen_catalog.py`), with the
values their installers write, below. It emits none for the runtime, and none yet for the
schema-2-only providers (open conversion, Leia conversion, Ultraleap): a receipt for one of
those carries no uninstall command, and `uninstall` answers 43 for it.

| Item | `uninstall.path` | `args` |
|---|---|---|
| `leia-sr` | `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR\UninstallString` | `["/S"]` |
| `browser` | `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR Browser\UninstallString` | `["/S"]` |

## Exit codes

| code | meaning |
|---|---|
| 0 | done, or nothing to do |
| 2 | usage |
| 10 | offline: catalog unreachable, unparsable, unsupported schema, or **expired** |
| 11 | catalog signature invalid, or its serial is older than one this machine already accepted |
| 20 | download failed (after retries on every URL) |
| 21 | verification failed: size, SHA-256, Authenticode signer, a signer the publisher was not delegated, or a revoked build |
| 30 | a component installer or uninstaller exited non-zero, was still running after an hour, or left the component installed |
| 40 | a needed component has no signed Windows build in the catalog that satisfies the floor |
| 41 | the item's runtime range or class ABI excludes this machine's runtime |
| 42 | `uninstall`: `dxr-get` has no receipt for that id |
| 43 | `uninstall`: no uninstall command was recorded, or other installed items require it |
| 50 | installing or uninstalling needs an elevated process. For `uninstall` it is given only when nothing else stands in the way: 42 and 43 are answered first, elevated or not |

Codes 0 to 40 and 50 mean what they meant for the schema-1 tool. `dxr-get` does not relaunch
itself elevated; a front end that wants that does it (ADR-064 step 4).

## Trust rules

- **Catalog signature.** Ed25519 over the exact bytes of `stable.json`, checked against the
  keys compiled into dxr-get (key id `9b762f9a`), for both schemas. The catalog's `expires`
  must lie in the future. An expired catalog is treated as **offline**: it can make dxr-get
  do nothing, never install.
- **Rollback guard.** The highest serial accepted is kept under
  `HKLM\SOFTWARE\DisplayXR\Update`, in `CatalogSerialV2` for schema 2 and `CatalogSerial` for
  schema 1, and an older serial is refused. The two values are separate so that a schema-1
  catalog accepted by an older `dxr-get` on the machine cannot make this one refuse a
  schema-2 catalog.
- **Every installable binary is signed.** An asset without a `signature` is not installable
  (state `not_fetchable`). There is no hash-pinned-only form.
- **Signers are per publisher.** The signed catalog names each publisher's signers
  (`publishers.<id>.signers.windows-x64`, by certificate subject). A payload is accepted only
  when (1) its asset's `signature.signer` is one of **its own item's publisher's** signers,
  and (2) `WinVerifyTrust` reports a valid Authenticode signature whose leaf subject equals
  that signer. One publisher's signer cannot vouch for another publisher's item. An unknown
  `signature.kind` is refused, not skipped.
- **Schema 1 keeps the compiled-in signer list.** A schema-1 catalog names no publishers, so
  for it the delegation is the list compiled into dxr-get (`Leia, Inc.`), exactly as before.
  For schema 2 the only thing compiled in is the catalog keys.
- **Revoked builds.** An asset whose SHA-256 is in the catalog's `revoked` list is refused.
- **Every payload is checked:** size and SHA-256 must equal the catalog's, then the signer
  rule above. This holds for an external prerequisite's asset too.
- **What dxr-get never does:**
  - downgrade a component;
  - write `Trust\Allowlist`;
  - touch `ActiveRuntime` (the runtime installer owns it; see below);
  - follow a redirect to plain HTTP, or fetch a plain-HTTP URL;
  - ask for an account, a key or a payment. Installing is anonymous.
- **Dev overrides:**
  - `DXR_GET_CATALOG=<url|file>` points dxr-get at another catalog. That catalog must still be
    signed, so the override grants nothing.
  - `DXR_GET_DEV=1` plus `--no-verify-catalog` skips the catalog signature.
  - `DXR_GET_ALLOW_UNSIGNED=1` skips the Authenticode check of the downloaded file. The hash
    check and the catalog-side signer delegation always stay on.

## Offline, and no unbounded wait

- **No catalog** (no network, a captive portal, an expired document): exit 10, before
  anything is changed. stderr says what was skipped and the command to run later; with
  `--json`, stdout carries `{"offline":true,"reason":…,"skipped":[…],"retry":"dxr-get …"}`.
  What is already installed keeps working. `list --installed` and `uninstall` do not need the
  catalog and work offline.
- **A download fails after the catalog was read:** exit 20, nothing is installed (downloads
  come before installers), the log names what was not installed, and the partial file is kept
  so the same command resumes it.
- **Bounds.** Each HTTP request has connect, send and receive timeouts (15 s, 15 s, 30 s,
  60 s) and at most four attempts with 1 s, 4 s and 15 s between them. A child installer or
  uninstaller is waited for at most one hour; one still running after that is reported as
  failed (exit 30) and is not killed. Stopping the service waits at most 5 s per process.

## Detection (P2)

Detect rules are data in the signed catalog: an item's `detect` array, and the `detect` array
of an external prerequisite. Supporting a new panel therefore needs a catalog change, not a
new dxr-get. A display plug-in's rules come from its own release asset,
`displayxr-detect.json` (#2737, `component-catalog.md` § Display plug-in detect manifest); the
Leia plug-in generates it from its EDID table (`src/drv_leia/leia_edid_table.h` in
displayxr-leia-plugin).

| Kind | Matches when |
|---|---|
| `edid` | `man` and `prod` (exact) or `prod_min`..`prod_max` (inclusive) match a present monitor's EDID ids. Both 16-bit words are read little-endian, the plug-in table's convention: Dell shows as `man` 44048 (`0xAC10`). A Leia panel can carry another brand's manufacturer id (the 8K Leia display enumerates as Dell `DEL4147`), which is why a product id is part of every high-confidence rule. The full EDID value is read, because 8K panels carry DisplayID extension blocks well past 256 bytes |
| `usb` | `vid` and `pid` (exact) or `pid_min`..`pid_max` match a present USB device (SetupAPI's `USB` enumerator, interface children included), e.g. a vendor's tracking camera |
| `registry_dword_min` | the DWORD `key`\\`value` is at least `min` (the SR platform's `NConnectedDevices >= 1`) |
| `registry_sz_min` | the string `key`\\`value` is a dotted version at or above `min` |
| `registry_key_exists` | the `HKLM` key exists |
| `file_exists` | `path` exists. An absolute path only; a path with `%` or without a drive is never matched |
| `service_exists` | a Windows service named `name` is installed (running or not) |

Registry rules read the 64-bit view. An `edid` rule with only `man`, or a `usb` rule with only
`vid`, is always treated as **low** confidence, whatever the catalog says. A malformed rule (half
or inverted range, exact and range together, a value outside 16 bits) matches nothing. A rule of
an unknown kind is not matched, and evaluation goes on to the next rule. The EDID and USB
evidence is gathered once per run, however many items' rules are evaluated.

`detect` evaluates the rules of **every** item that has some. `--json`:
`{"edids":[{man,prod}], "usb":[{vid,pid}], "display_plugins":[{id, component, label, match,
high_confidence, evidence}], "<schema-1 name>":{match, high_confidence, evidence, id}, ...}`.
`display_plugins` has one entry per item with detect rules (`component` is its schema-1 name);
the per-name objects (so the Leia plug-in's key is still `leia_plugin`) are kept for one
release. Low-confidence rules (the generic Acer product ids 0–3) are reported and never trigger
an install.

## What changed for the three first-party components

Their behaviour and exit codes are the schema-1 tool's, with these deliberate differences:

| Verb | Before | Now |
|---|---|---|
| `install X` | installed X only | installs what X requires first (a missing or too-old runtime; `--no-runtime` turns that into exit 41), and stops and restarts the service around a component the runtime loads |
| `plan` (text) | printed only what needed work | also prints one line per component that needs nothing |
| `plan --json` | the schema-1 fields | the same fields in the same order, plus new ones after them |
| `plan`: `reason` of a hardware item | `Leia display: <evidence>` | `detected: <evidence>`, for any item. Intentional: the text was Leia-specific. `reason` is free text for people; the DisplayXR Browser installer, the Leia plug-in installer and the runtime installer's listing shim were checked and none reads it (they read `component`, `state`, `target`, `size` and exit codes) |
| `ensure-for`, `install` | no record | write a receipt per installed item |
| `detect --json` | one `leia_plugin` object | the same object (plus its `id`), and one per other item with detect rules; plus `usb` evidence and a `display_plugins` list (#2737) |
| any | waited for a child installer without limit | waits at most one hour |

## Validating on a test machine

Schema 2 is not published, and a test catalog must be signed to be read. Test content is
never signed with the production catalog key. Instead, `dxr-get` can be **compiled** to trust
one throwaway key, and a script signs a fixture catalog with it.

**The test build.** CMake option `DXR_GET_TEST_ROOT_KEY` (64 hex digits of an Ed25519 public
key, empty by default). It is compile time only: no environment variable, registry value or
argument turns it on in a normal build. A binary built with it:

- prints `TEST ROOT KEY BUILD -- not for distribution` on every run, before anything else;
- trusts that one key **instead of** the catalog keys, so it cannot verify the real catalog;
- reads a local catalog file only: without `--catalog <file>`, or with a URL, it exits 2;
- is not installed into `_package\bin`, and makes the `installer` target fail.

Everything else is the shipping code path: the serial guard, expiry, publisher signers,
hashes, Authenticode. CI asserts that the option is unset and that no `dxr-get.exe` it builds
contains the banner text (`build-windows.yml`); `attach-dxr-get.yml` refuses to attach one.

**The fixture.** `scripts/make_test_catalog.py` (needs Python with `cryptography` for signing):

```
python scripts/make_test_catalog.py keygen --out C:\dxr-fixture
   -> C:\dxr-fixture\test-root.seed, and prints the public key and the CMake flag
cmake -DDXR_GET_TEST_ROOT_KEY=<64 hex> build
scripts\build_windows.bat build
python scripts/make_test_catalog.py build --key C:\dxr-fixture\test-root.seed --out C:\dxr-fixture\cat
build\src\xrt\targets\get\Release\dxr-get.exe --catalog C:\dxr-fixture\cat\stable.json list --json
```

Afterwards: `cmake -UDXR_GET_TEST_ROOT_KEY build`, rebuild, and delete the seed.

The fixture takes the three real first-party installers, with their sizes, hashes and URLs,
from the live schema-1 catalog (`--v1` names another). They are really signed, so a fetch
really verifies, and nothing has to be built, hosted or signed for the test. Around them it
adds items whose ids start with `fixture.`. A fixture item's installed-version value is one
nothing writes, so it always reads as not installed and its plan is the same on every run.
Each fixture item stands on the browser's installer (`fixture.abi-mismatch` on the Leia
plug-in's): installing one really runs that installer.

| Case | Command (after `--catalog <file>`) | Expected |
|---|---|---|
| Schema 2 is read, serial guard | `list --json` | `"schema":2`; `CatalogSerialV2` is written under `HKLM\SOFTWARE\DisplayXR\Update` when elevated |
| Every detector, met | `plan --for fixture.detectors-met --json` | four `prerequisites`, all `"met":true` (`registry_key_exists`, `file_exists`, `service_exists`, `registry_sz_min` against Windows' own key, file and service) |
| Every detector, unmet | `plan --for fixture.detectors-unmet` | three `Prerequisite MISSING` lines with their links; nothing is planned for them |
| Runtime range, maximum | `install fixture.runtime-too-new` | exit 41, naming `1.0.0` and the installed runtime |
| Runtime range, minimum | `install fixture.runtime-too-old` | exit 41, naming `99.0.0` |
| Class ABI | `install fixture.abi-mismatch` | exit 41 (`display_processor ABI 99`) when the runtime's ABI is known: a runtime that wrote `Runtime\Abi` with its stamp, a plan that moves the runtime to the catalog's version, or `list --json --assume-runtime <catalog runtime version>` for `"compatible":false`. On a runtime that never wrote the values and is newer than the catalog's, the plan says `ABI not checked` |
| Stale ABI values | install an older runtime over one that wrote `Runtime\Abi`, then the row above | `ABI not checked`: the stamp names another version |
| Several assets | `plan --for fixture.multi-asset --json`, then `install fixture.multi-asset` on a machine without the Leia plug-in | two actions with a `download`, `total_size` their sum; the prerequisite's installer runs before the item's |
| Default set | `plan --default`, `install --default` | the browser (`recommended`); also the Leia plug-in on a Leia panel |
| Wrong signer | `fetch fixture.wrong-signer` | downloads, hash ok, then exit 21: the file's signer is not the one the catalog names |
| Signer of another publisher | `install fixture.undelegated-signer` (elevated) | exit 40 before any download: `not delegated to publisher "fixture-other"` |
| Link-out | `list`, `install fixture.link-out` | `installable: false` with its `listing`; exit 40 |
| Uninstall with a rule | `install browser`, `list --installed`, `uninstall browser` | the receipt carries an `uninstall_command`; exit 0 and the receipt is gone |
| Offline | `--catalog C:\no\such\file plan --for browser --json` | exit 10, `{"offline":true,…}` |

## Not implemented yet

- A second, standby catalog root key (ADR-064 §4): it needs a key pair generated offline.
- Installer kinds other than `exe` (`msi`, `deb`, `pkg`): such an asset is `not_fetchable`.
- Elevation relaunch, and the `displayxr-cli catalog` front end (step 4).
- macOS and Linux (step 5).
- Progress output a setup UI can render.

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
