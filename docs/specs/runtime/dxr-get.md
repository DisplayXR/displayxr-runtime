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
dxr-get [--catalog <url|file>] install <item>... | --default [--yes] [--json] [--no-start-service]
dxr-get [--catalog <url|file>] fetch <item> [--out <dir>]
dxr-get [--catalog <url|file>] detect [--json]
dxr-get [--catalog <url|file>] catalog
dxr-get [--catalog <url|file>] list [--json]
dxr-get [--catalog <url|file>] outdated [--json]
dxr-get list --installed [--json]
dxr-get uninstall <id>... [--json] [--no-start-service]
```

Every verb also takes `--log-pipe <token>` (§ --log-pipe).

**The command line is read as UTF-16** (`GetCommandLineW`) and handled as UTF-8, so a
`--catalog` or `--out` path outside the ANSI code page works. An argument that holds a control
character is refused (§ --log-pipe).

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

### Several items

`install` and `uninstall` accept several items on one command line, so that a front end asks
for elevation once (`displayxr-cli catalog`). **One item is the verb as it always was: its
stdout, its messages and its exit code are unchanged.** What follows applies only when two or
more are given (at most 32). The other verbs still take one.

- **Order.** The items are handled one after the other, in the order given.
- **Each is judged alone.** Every item gets its own plan, made when its turn comes, so it sees
  what the items before it installed (skip-if-current applies to each). Its verdict is the
  single-item verb's, in the same order: 41, nothing to do, 50, 40, then the plan runs. One
  that cannot be installed or removed is reported and skipped, and **the rest proceed**.
- **Exit code:** the first item exit code that is not 0, in the order given; 0 when none failed.
- **Unchanged:** `--no-runtime` (it applies to every item), receipts (one per installed item,
  written as each finishes), the trust rules, downloads, and the service being stopped and
  started around each item that needs it.
- **Messages.** The single-item messages, and one line per item when it ends:
  `result: <id>: <exit code> <word>`. The word is `installed`, `nothing_to_do`, `incompatible`,
  `needs_elevation`, `not_fetchable` or `failed` for `install`, and `uninstalled` or
  `not_uninstalled` for `uninstall`. A front end that cannot read the exit code of each item
  (an elevated run) reads these lines from `--log-pipe`.
- **`--json`.** One document, printed at the end, instead of one per item:

```json
{"exit":41,"items":[
  {"id":"open-conv","exit":0,"result":"installed","plan":{"for":"open-conv","schema":2,"gate":0,"actions":[…]}},
  {"id":"leia-sr","exit":41,"result":"incompatible","plan":{"for":"leia-sr","gate":41,"gate_reason":"…","actions":[…]}}]}
```

  For `install`, `plan` is the item's single-item `--json` document. For `uninstall`, each entry
  is the single-item object (`{"id","result","exit","detail"}`).
- **Offline.** The catalog is read once, before the first item: exit 10 with the offline
  document, as for one item, and nothing is attempted.
- `install --default` takes no items, as before.

### --log-pipe

`--log-pipe <token>` also sends every message `dxr-get` writes to stderr to a **named pipe the
caller owns**: `\\.\pipe\dxr-get-log-<token>`. It exists for a front end that started `dxr-get`
elevated and cannot read its output.

`dxr-get` takes **no path to write a log to**. It may be running elevated on a command line
that came from a process that is not, so anything the caller could aim would be an elevated
write to a place the caller chose. With the pipe, the elevated process writes to no file:

- **The token is 32 lowercase hex digits and nothing else.** Anything else is a usage error
  (exit 2). `dxr-get` builds the pipe name itself, so it always lies in the named-pipe
  namespace and can never name a file, a device or a directory.
- **The caller creates the pipe**, under a random token, before it starts `dxr-get`, and reads
  it while `dxr-get` runs. `dxr-get` only connects. What it opened must be a pipe.
- **The caller cannot impersonate `dxr-get`** through it: the pipe is opened with an anonymous
  impersonation level (`SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS`).
- **A write waits at most 2 seconds.** A reader that stops reading costs one such wait; the
  channel is then closed and the run goes on with stderr only. No unbounded wait.
- When nothing listens on the pipe, one message says so and the run goes on with stderr only.
- stdout (the plan, the `--json` documents) is not copied to it.
- Nothing `dxr-get` logs is a secret: it handles no credentials, keys or tokens.

**Control characters.** An argument that holds an ASCII control character (0x00-0x1F, 0x7F) is
a usage error, exit 2, before anything else is done: a line feed in an item id would otherwise
put a second, forged `result:` line into what the front end reads. And in everything
`dxr-get` logs, to stderr and to the pipe, a control character other than the line feed and the
tab is replaced by `?`, whatever text it came from (an argument, the registry, a catalog).

The code is `src/xrt/targets/get/get_log_channel.h` (the writer; header only, so the unit
test's stand-in engine uses the same code) and, for the reader, `cli_catalog_os.c`.

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

`uninstall <id>` removes an item that `dxr-get` installed (several ids: § Several items). It needs no catalog and no
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
itself elevated; a front end that wants that does it: [`displayxr-cli catalog`](#displayxr-cli-catalog).

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
  for it the delegation is the list compiled into dxr-get (`Leia, Inc.`, in `get_signers.h`),
  exactly as before. `displayxr-cli catalog` reads the same list before an elevated start.
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
- **Nothing the calling user can set weakens verification in a build that ships.** A signed
  `dxr-get.exe` is started elevated behind a consent prompt that shows its publisher, so no
  environment variable or argument may make it install something the signed catalog does not
  name. The switches:

  | Switch | What it does | Exists in |
  |---|---|---|
  | `--catalog <url\|file>`, `DXR_GET_CATALOG=<url\|file>` | another catalog **location**. That catalog must still carry a valid signature under a compiled-in key, and the serial guard applies, so it grants nothing | every build |
  | `DXR_GET_DEV=1` plus `--no-verify-catalog` | the catalog signature (and the serial guard) is not checked | a **test-root-key build only** |
  | `DXR_GET_ALLOW_UNSIGNED=1` | the Authenticode check of a downloaded file is skipped. The size and hash checks and the catalog-side signer delegation stay on | a **test-root-key build only** |

  A test-root-key build is one compiled with `DXR_GET_TEST_ROOT_KEY` (§ Validating on a test
  machine): it is never installed or attached to a release, and it prints a banner on every
  run. In every other build `--no-verify-catalog` is a usage error (exit 2) and the two
  environment variables are **ignored**, with one log line saying so. The decision is one
  function, `dxr_dev_switches_resolve` (`get_core.c`), unit-tested for both kinds of build;
  the Windows CI also runs the built `dxr-get.exe` with all three set against an unsigned
  catalog file and requires exit 2 and exit 10.

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
| `install`, `uninstall` | exactly one item; a second was a usage error (exit 2) | several items (§ Several items). One item behaves as before |

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
hashes, Authenticode. It is also the only build in which `DXR_GET_DEV=1` with
`--no-verify-catalog` (an unsigned fixture, `make_test_catalog.py build --no-sign`) and
`DXR_GET_ALLOW_UNSIGNED=1` (an unsigned test payload) do anything (§ Trust rules); the recipe
below needs neither, because the fixture is signed with the test key and its payloads are the
real, signed installers. CI asserts that the option is unset and that no `dxr-get.exe` it builds
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

## displayxr-cli catalog

`displayxr-cli catalog` is the command-line front end (ADR-064 step 4). It holds no catalog
logic: it runs `dxr-get`, relays its exit code, and wraps its JSON. `catalog` is distribution;
choosing among installed components stays with the per-class verbs (`dp`, `input`, `conv`,
`tracking`: ADR-054 decision 6).

```
displayxr-cli catalog list [--installed] [--json]
displayxr-cli catalog outdated [--json]
displayxr-cli catalog show <id> [--json]
displayxr-cli catalog install <id>... | --default [--elevate|--no-elevate] [--allow-unsigned-engine] [--json]
displayxr-cli catalog uninstall <id>... [--elevate|--no-elevate] [--allow-unsigned-engine] [--json]
displayxr-cli catalog update <id>... | --all [--elevate|--no-elevate] [--allow-unsigned-engine] [--json]
```

Every verb also takes `--catalog <url|file>` (handed to the engine, which still requires a
signature) and `--timeout <seconds>`. A catalog **file** is handed on as an absolute path,
resolved once against the CLI's directory: the elevated engine starts in another one. A URL
is handed on as written. The CLI reads its own command line as UTF-16 and hands the engine
UTF-8, so paths and ids outside the ANSI code page survive; an argument with a control
character is a usage error (exit 2), as it is for the engine.

| Verb | Engine calls | Notes |
|---|---|---|
| `list` | `list --json` | a table with the licence state of installed plug-ins beside the engine's columns |
| `list --installed` | `list --installed --json` | the receipts; no catalog, no network |
| `outdated` | `outdated --json` | |
| `show <id>` | `list --json` | one item, by id or alias. An id the catalog does not list: exit 40 |
| `install <id>...` | `install <ids> --no-runtime --json`; then, for the ids that answered 50, `install <those ids> --no-runtime --log-pipe <token>`, elevated | one call for all the ids, and at most one elevated call |
| `install --default` | `install --default --no-runtime --json`, then elevated as above | |
| `uninstall <id>...` | `uninstall <ids> --json`; then `uninstall <those ids> --log-pipe <token>`, elevated | |
| `update --all` | `outdated --json`, then `install` as above for every row that can be installed | the runtime row and rows without a build are reported, not installed. One run updates at most 32 items; a row past that is reported as not updated in this run (exit 0), and running the command again does the rest |
| `update <id>...` | `outdated --json`, then `install` as above | an id that is not outdated is explained from one `list --json`: current (exit 0), not installed (42), not listed (40) |

**Where the engine is.** `dxr-get.exe` next to `displayxr-cli.exe`, else in the directory named
by `HKLM\Software\DisplayXR\Runtime\InstallPath`. No environment variable or option names
another engine, because that path may be started elevated. Without an engine every verb exits
60 and prints nothing else. On macOS and Linux there is no engine yet (ADR-064 step 5), so
that is the answer there.

**The runtime itself is not installed, replaced or updated from the CLI.** `install` always
passes `--no-runtime`, so an item that needs a newer runtime is refused with exit 41 and a
message that says to update the runtime first. `catalog install runtime` and `catalog update
runtime` exit **64** without calling the engine; `update --all` reports a newer runtime and
leaves it. `--allow-runtime` is accepted and answers 64: it is reserved for when this works.

The reason is where the engine runs from. `dxr-get.exe` and `displayxr-cli.exe` are files of
the runtime's own directory, and both are running while the runtime installer would replace
them. The installer (`installer/DisplayXRInstaller.nsi`) writes them with a plain `File`
command. It stops the service and its two satellites, and it probes `DisplayXRClient.dll` for
being in use before it writes anything (#2713), but it takes no step for a running
`dxr-get.exe` or `displayxr-cli.exe`: no process check, no rename, no replace-on-reboot. The
write of a file that is being executed fails, and in a silent install (the mode `dxr-get`
starts installers in) that failure ends the installer after `DisplayXRClient.dll` and other
files were already replaced. *This is read from the script; it was not run.* Other installers
that call `dxr-get` do not have the problem, because they run their own copy from a temporary
directory. The follow-up is in ADR-064's step table (step 4b): the engine relaunching itself
from a protected temporary location, or the runtime installer handling its own running tools.
Until then the runtime is updated with its installer or the bundle.

**Elevation.** The CLI itself is never elevated. A verb that changes the machine first runs the
engine with the rights the CLI has, with every item on one command line. The engine answers
what elevation cannot fix first (41, 42, 43, nothing to do), so exit 50 for an item means an
elevated run is all that it is missing; an already elevated terminal does the work in that
first call. For the items that answered 50:

| Call | What happens |
|---|---|
| from a terminal, no `--json` | the engine is started elevated with `ShellExecuteEx` "runas": **one consent prompt for the whole command**, which names `dxr-get.exe` |
| `--json`, or stdin / stderr not a terminal | nothing is started. Exit 50, and the message says to add `--elevate` |
| `--elevate` | started elevated in both cases |
| `--no-elevate` | never started elevated |

An item whose plan `gate` is 40 is reported as 40 and is not part of the elevated call (the
engine checks for elevation before it reports that gate). A declined prompt is exit 50, and
nothing was changed.

**The engine's signature is checked before every elevated start**, and only then. `dxr-get.exe`
must pass three rules:

1. it carries a valid Authenticode signature that chains to a trusted root (`WinVerifyTrust`,
   no revocation fetch: the check `dxr-get` makes of a downloaded installer);
2. its signer's subject name is one of the **first-party signers**, the list `dxr-get` itself
   compiles in for schema-1 payloads (`src/xrt/targets/get/get_signers.h`, one header both
   include; today `Leia, Inc.`). A valid signature of any other publisher is refused, whether
   or not the CLI is signed;
3. when `displayxr-cli.exe` is signed too, the engine's signer is the CLI's.

The file is **held open across the check and the start**. Before the signature is read the CLI
opens `dxr-get.exe` sharing reading only, reads the signature through that handle, and keeps
the handle until the CLI exits. While it is open the file cannot be written, deleted or
renamed, nor a directory above it renamed, so the file the consent prompt starts is the file
that was checked. A file that cannot be opened that way is treated as unverified.

Otherwise the items that needed elevation exit **65** and nothing is started.
`--allow-unsigned-engine` lifts the check, and is the only thing that does. It is for
development builds (CI builds are unsigned), it must be given explicitly, and `--elevate` does
not imply it. Running an unsigned engine with the rights the CLI already has (`list`, or any
verb from an elevated terminal) is not affected.

**What the elevated engine said.** An elevated child's output cannot be read, so the CLI
creates a named pipe under a random 128-bit token (`FILE_FLAG_FIRST_PIPE_INSTANCE`, one
instance, local clients only), hands the engine the token with `--log-pipe`, reads the pipe
while the engine runs, and closes it afterwards (§ --log-pipe). No file is created, by either
process, so there is nothing to clean up after a timeout or a declined prompt. A person sees
the engine's messages as `dxr-get: …` lines (the first 400); with `--json` they are the call's
`log` string.

**Each item's own result.** With several items the engine's exit code is only the first
failure, and each item's own code is its `result:` line in the log. Those lines are never
lost to the size of the log: the CLI keeps 256 KB of messages and, past that, still keeps
every `result:` line. And the CLI does not guess:

| Situation | Outcome |
|---|---|
| the pipe could not be created, several items need elevation | the engine is **not started**. Each such item exits **61**; nothing was changed. One item is started without the pipe: its exit code is the whole answer |
| the elevated run exits 0 | every item succeeded (0 means no failure), with or without `result:` lines |
| the elevated run exits non-zero and an item has no `result:` line | that item exits **63**: its outcome is not known. It does not inherit another item's code |
| the first (unelevated) call exits 0 without the JSON document | **63** for the items it should have answered; nothing is reported as installed or removed |

**Waits are bounded.** 10 minutes for `list`, `show` and `outdated`, 2 hours for the verbs that
install; `--timeout` changes either. When the bound passes the CLI exits 62. A listing call is
ended; a call that may be running an installer is left running, as `dxr-get` leaves a slow
installer running.

**After a change** the CLI says what to expect, from the class the receipt records. `dxr-get`
stops the service and starts it again around any item the runtime or the service loads, so the
service has the new set. An app that was already running does not: it keeps the display
processor it started with until its next session, and an app that hosts the runtime in-process
discovers input and tracking providers at its next launch. An application item restarts nothing.

### Exit codes

The engine's codes are returned unchanged (§ Exit codes). With several items, each is tried and
the exit code is the first failure in the order given. The CLI adds:

| code | meaning |
|---|---|
| 2 | the `catalog` command line is malformed |
| 40 | also: `show` or `update` of an id the catalog does not list |
| 42 | also: `update` of an item that is not installed |
| 50 | also: the consent prompt was declined |
| 60 | no engine: `dxr-get` is not installed, or the platform has none |
| 61 | the engine could not be started; also: several items needed elevation and the log pipe could not be created, so nothing was started |
| 62 | the engine was still running when the wait ended |
| 63 | the engine exited 0 without the JSON document the verb needs (every verb, `install` and `uninstall` included), or an elevated run failed without reporting a result for the item |
| 64 | cannot be done from the CLI: installing or updating the runtime itself, or `--allow-runtime` |
| 65 | `dxr-get.exe` is not validly signed, so it was not started elevated |

### `--json`

One document on stdout. The engine's log still goes to stderr. The engine's own documents are
embedded unchanged under `calls[].output`; the CLI renames nothing in them.

```json
{"schema":1, "verb":"install", "exit":0, "message":"done",
 "engine":"C:\\Program Files\\DisplayXR\\Runtime\\dxr-get.exe",
 "calls":[
  {"args":["install","leia-sr","open-conv","--no-runtime","--json"], "exit":50,
   "message":"needs an elevated process", "elevated":false,
   "output":{"exit":50,"items":[{"id":"leia-sr","exit":50,"result":"needs_elevation","plan":{…}},
                                {"id":"open-conv","exit":50,"result":"needs_elevation","plan":{…}}]}},
  {"args":["install","leia-sr","open-conv","--no-runtime","--log-pipe","3f9c…32 hex digits"],
   "exit":0, "message":"done", "elevated":true, "output":null,
   "log":"…\nresult: leia-sr: 0 installed\n…\nresult: open-conv: 0 installed\n"}],
 "items":[
  {"id":"leia-sr", "action":"install", "exit":0, "message":"leia-sr: installed", "elevated":true,
   "changed":["leia-sr"],
   "expect":[{"id":"leia-sr","class":"display_processor","note":"…"}]},
  {"id":"open-conv", "action":"install", "exit":0, "message":"open-conv: installed", "elevated":true,
   "changed":["open-conv"], "expect":[{"id":"open-conv","class":"conversion_provider","note":"…"}]}]}
```

| Field | Meaning |
|---|---|
| `schema` | the version of this envelope: `1` |
| `verb`, `exit`, `message` | the verb, the process exit code, and its one-line meaning |
| `engine` | the engine's path, or null when there is none (then `calls` is empty and `exit` is 60) |
| `calls[]` | every engine invocation that is part of the answer, in order: `args`, `exit`, `message`, `elevated`, and `output`, the engine's stdout document as it printed it (null when it printed none, and always null for an elevated call). An elevated call also has `log`: the engine's messages from its `--log-pipe` channel, or null |
| `licences` | `list`, `outdated`, `show`: the licence object of each **installed** plug-in item, keyed by catalog id. It is the `licence` object of `<class> list --json` (ADR-054 Amendment 2). An item that is not installed, not a plug-in, or not registered on this machine has no key |
| `item` | `show`: the engine's row for that item, or null |
| `items[]` | `install`, `uninstall`, `update`: one outcome per item: `id` (null for `--default`), `action` (`install`, `uninstall`, or `none` when no engine call was made for it), `exit`, `message`, `elevated`, `changed` (the ids the engine installed or removed) and `expect` (what to expect, per changed item of a class the runtime loads). The `none` entries come first, then the items the engine was asked for, in the order given |

For `list` the engine's listing is `calls[0].output` (`{"schema":2,"items":[…]}`), for
`outdated` it is `{"schema":2,"outdated":[…]}`, and offline it is the engine's
`{"offline":true,…}` document with `exit` 10. With one item, `install` and `uninstall` embed
the engine's single-item document; with several, its `{"exit","items":[…]}` document
(§ Several items).

The code is `src/xrt/targets/cli/cli_catalog.c` (every decision, no operating-system calls),
`cli_catalog_os.c` (finding and running the engine, the signature check, the log pipe) and
`cli_cmd_catalog.c` (the licence join). `tests/tests_cli_catalog.cpp` drives it against a fake
engine on every platform.

## Not implemented yet

- A second, standby catalog root key (ADR-064 §4): it needs a key pair generated offline.
- Installer kinds other than `exe` (`msi`, `deb`, `pkg`): such an asset is `not_fetchable`.
- Updating the runtime from the installed tools: `displayxr-cli catalog` refuses it (exit 64),
  because `dxr-get.exe` runs from the directory the runtime installer has to replace
  (§ displayxr-cli catalog; ADR-064 step 4b).
- One stop and start of the service for a several-item run: it is stopped and started around
  each item that needs it, so N such items mean N restarts (ADR-064 step 4c).
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
