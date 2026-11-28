# Catalog-driven installers

Status: the runtime installer part is implemented and compile-checked; a Windows box has run
part of the checklist below (main @0cd768e2b), and what it found is fixed but **not yet re-run
on Windows**. The bundle part is a plan. Decision record:
[ADR-064 §7 and §8](../adr/ADR-064-component-catalog-and-third-party-trust.md). Engine:
[`dxr-get`](../specs/runtime/dxr-get.md). Catalog:
[component-catalog.md](../specs/runtime/component-catalog.md).

The direction: the vendor-neutral installers stop carrying vendor providers. They carry the
runtime core, show a checkable list of optional components taken from the signed DisplayXR
Catalog, and download what is chosen from its publisher. An offline full bundle stays for OEM
and air-gapped installs.

## 1. Runtime installer (`DisplayXRSetup-*.exe`)

Code: `installer/Catalog.nsh` (the page, the slots, the install step),
`installer/catalog_plan.ps1` (turns the listing JSON into an INI file NSIS can read),
`installer/tests/test_catalog_plan.py`.

### What the user sees

The existing components page gains one row per optional catalog item, between the runtime and
the conversion models:

| Item state | Row | Ticked |
|---|---|---|
| not installed | `N MB - <name> <version>` | if **recommended** by the catalog, or if its detection rule **matched this machine with high confidence** |
| installed, older than the catalog | `Update, N MB - <name> <version>` (the version it replaces is in the description) | **always** (already installed ⇒ ticked): the update comes with the runtime. Clearing the box keeps the installed version |
| installed at the catalog version, or newer | `Installed - <name> <installed>`, greyed | shown ticked, greyed; nothing is downloaded or run |
| no signed Windows build, a link-out item, or its runtime range excludes the runtime being installed | `Not available - <name>`, greyed, the reason in the description box | never (shown ticked and greyed when an older version of it is installed: it stays installed) |

The status leads the row because the list cuts a long row on the right; `Cached` (`Update,
cached`) stands in for the size when the download cache already holds everything.

A row is at most **50 characters**, because a row wider than the list is cut and brings the
horizontal scrollbar back. The number is measured, not computed: on Windows at 100 % scaling a
53-character row fits and a 59-character one (`Update, Downloaded - Leia display plug-in
2.23.0 to 2.23.1`) is cut after its 52nd character. The font is proportional, so the limit is
about 52 characters of ordinary text, and 50 leaves a little for wider letters. That is why an
update row names one version and why the cached word is short. The longest first-party row
is exactly 50 (`Update, cached - Leia SR display processor 2.23.11`); in a longer one the
**name** is cut and ends in `...`, the status and the version stay whole. The page uses the layout with
the description under the list (`MUI_COMPONENTSPAGE_SMALLDESC`), which gives the list about
twice the width and shows about five rows at once instead of nine. That box is two lines of
about 50 characters, so a description is at most **95** characters with what matters first:
for a row that is not available, the reason and nothing else, without the "Not available:"
lead when the reason is long (the row says it). The cap is below 2 x 50 because a line breaks
at a space. A longer description is cut at the last whole word that fits and ends in `...`,
so neither the marker nor half a word lands on a third line that the box does not show.

- **Already installed ⇒ ticked** holds for every item, whatever its class, recommended or
  not, detected or not. A component the user has is never presented as something they are
  about to lose. And **nothing on this page removes a component**: a cleared box means "do
  not install or update", for an installed item as for a new one. (The meta-bundle is
  different: it removes an installed component whose row is cleared. See section 4.)
- **Recommended** is the open conversion provider (`recommended: true` in the catalog), so
  "Convert to 3D" exists on a machine whose display vendor ships no converter (ADR-064 §7).
- **Detected** is whatever `dxr-get` reports: it reads the monitors' EDIDs and evaluates the
  catalog's rules (dxr-get.md § Detection). The installer does no hardware detection of its own.
- The line above the list says that ticked items are downloaded, that this needs an internet
  connection, and the total download size. It is recomputed on every tick.
- The description box says why an item is ticked, and names an external prerequisite that was
  not found and that DisplayXR does not install (for example the NeurD runtime for the Leia
  conversion provider). A prerequisite that carries its own installer in the catalog is
  installed by `dxr-get` before the item, and its size is in the row's download size.

### No connection

The core install does not depend on the catalog.

| When | What happens |
|---|---|
| the catalog cannot be reached when the page opens | no optional row is shown; the text above the list says the runtime alone is installed and to run Setup again later; the finish page repeats it and names the open conversion provider as not offered |
| a download or an item's installer fails later, or `dxr-get` refuses the item | the runtime is installed, the item is listed on the finish page and in `<install dir>\catalog-skipped.txt` with the reason and `dxr-get`'s exit code, Setup exits 0. "Run Setup again with an internet connection" is added only when an item failed for a connection reason (exit 10 or 20); a list of refusals ends after the list |
| the connection drops between two items | the remaining items are skipped without further attempts, and listed |

"How to get it later" is: run Setup again with a connection, or `dxr-get install <id>` from an
elevated prompt. `displayxr-cli catalog install` and the Dashboard's catalog rows (ADR-064
steps 4 and 8; on the Components tab) do not exist yet, so the installer does not mention them.

### Unattended (`/S`)

| Switch | Meaning |
|---|---|
| *(none)* | **unchanged**: the runtime only. No catalog request, no `dxr-get` call |
| `/COMPONENTS=<list>` | comma-separated item ids or aliases (`leia-sr`, `leia_plugin`, `open-conversion`, …) and the keywords `recommended`, `detected`, `default` (= `recommended,detected`), `installed` (installed items that have an update), `none`. Exactly `/COMPONENTS=default` is what the bundle passes; `default` does not include `installed`, so it means the same as `dxr-get install --default`. Unattended, exactly `/COMPONENTS=none` is the same as no switch: no listing, no PowerShell, no `dxr-get` call |
| `/CATALOG=<url\|file>` | passed to `dxr-get` as `--catalog`; the catalog must still be signed. For testing |
| `/NOSTART` | as before; items are then installed with `--no-start-service` |
| `/CATALOG=` and `/COMPONENTS=` values | a value containing a double quote is ignored |

In an interactive run `/COMPONENTS=` replaces the default pre-ticks, the "installed, has an
update" tick included (an item at the catalog version is still shown ticked and greyed). `/KEEPACTIVERUNTIME`,
`/FORCEACTIVERUNTIME`, `/NOMODELS`, `/MODELSPACK=` and `/CLOSEAPPS` are untouched, and so are
the `ActiveRuntime` rule and the allowlist rows: none of that code changed.

**What an unattended caller reads.** Setup's exit code is about the runtime only. A component
that was skipped, failed or refused never changes it: the runtime is installed and Setup exits
0. The components' outcome is `<install dir>\catalog-skipped.txt`: one line per item that was
not installed, `<name>: <reason> (dxr-get exit <n>)`. No file means nothing that was asked for
was skipped; a file left by an earlier run is removed at the start of each run. The file is
written directly, so it exists after `/S`.

`install.log` always describes the run that wrote it. Interactive, it is a copy of the
installer's details list, taken after the catalog step, so it holds the `Catalog:` lines. A
silent install has no such list (it used to write nothing and leave the previous run's log in
place); it now replaces the file with a short record: version, time, the parameters, that the
runtime was installed, and the catalog outcome (nothing asked for, no failure, or the same
lines as `catalog-skipped.txt`).

**An id that is named but not offered.** An id or alias in `/COMPONENTS=` that the catalog
lists as not installable here (its runtime range or class ABI excludes this runtime, it has no
signed Windows build, it is a link-out) is not ticked, and is not dropped: it is handed to
`dxr-get install` like an id the listing does not know, and the engine's refusal (41, or 40)
is what `catalog-skipped.txt` and the finish page report. The keywords never do this: an item
that is merely recommended or detected and not installable here is left alone. On the page the
row is unchanged: greyed, unticked, the reason in the description box.

The default is "nothing" on purpose. Installers that need the runtime run it silently through
`dxr-get` (`/S /NOSTART`), and a runtime installer that called back into `dxr-get` by default
would nest one engine run inside another.

### What it asks of `dxr-get`

Everything below is in [dxr-get.md](../specs/runtime/dxr-get.md) ("list", "install --default",
"The runtime installer"). The installer embeds the `dxr-get.exe` of its own build, so the two
always match.

| Call | Used for |
|---|---|
| `dxr-get [--catalog X] list --json --assume-runtime <this version> --assume-abi <class>=<major> …` | the whole page, in one catalog fetch |
| `dxr-get [--catalog X] install <id> --yes --no-runtime [--no-start-service]` | one call per chosen item, after the runtime section |
| `dxr-get [--catalog X] install --default --yes --no-runtime [--no-start-service]` | unattended `/COMPONENTS=default`: the recommended set plus the hardware matches, resolved by `dxr-get`. No listing and no PowerShell |
| exit codes 0, 10, 11, 20, 21, 30, 40, 41, 50 | the reason shown for a skipped item |

What the page takes from each `list` row, unchanged:

| Field | Becomes |
|---|---|
| `state` | `not_installed` is offered; `outdated` and `rebuilt` are offered as an update; `current`, `newer` and `installed` are shown as installed |
| `installable`, `listing` | not installable: shown as not available (a link-out says so) |
| `compatible`, `incompatible_reason` | not compatible: shown as not available, with `dxr-get`'s own sentence |
| `recommended`, `detected: "high"` | the pre-ticks. `low` never ticks |
| `installed` (a version) | already installed ⇒ ticked: with `state` `outdated` / `rebuilt` the row is pre-ticked; at the catalog version it is shown ticked and greyed |
| `download` | the size on the row and in the total: what `install <id>` would still fetch, prerequisite installers included, the download cache deducted. `0` reads "already downloaded" |
| `prerequisites[]` with `met: false` and no installer of its own | named in the description box as needed and not found |
| `summary`, `aliases` | the description, and the names `/COMPONENTS=` accepts |

`--assume-runtime` is what makes the answers right before anything is installed: `dxr-get`
judges every item against the runtime this installer carries, and the class ABI majors
(`--assume-abi`) are the same numbers the installer writes to `Runtime\Abi`. With an assumed
runtime the runtime is taken as fixed, so an item that needs a newer one is `compatible: false`
on the page instead of failing later.

`--no-runtime` is on every install this installer triggers: `dxr-get` never starts a second
runtime installer inside the first. An id passed with `/COMPONENTS=` that needs a newer runtime
is refused by `dxr-get` (exit 41) and reported as skipped: the listing already says it is not
compatible, and `catalog_plan.ps1` puts such an id in `passthrough` so that `install` is still
asked and the refusal comes from the engine, at install time.

`installer/catalog_plan.ps1` adds only the ticks (the pre-tick rule and `/COMPONENTS=`), the
`passthrough` list (requested ids the listing lacks or marks unavailable), the row texts, and
the order when there are more items than rows.

### Live and waiting

| Part | State |
|---|---|
| The installer builds and behaves as before when `dxr-get.exe` is not in the build | compiled, both variants |
| `/S` with no switch | unchanged by construction: the install step returns before any call |
| Rows, states, greyed installed items, hardware pre-tick, `/COMPONENTS=` | written against the merged engine; waits only on a Windows run |
| Open conversion provider pre-ticked | **waits on a published schema-2 catalog.** `publish-catalog.yml` publishes schema 1 only and `dxr-get` falls back to it; the fallback has no `recommended` flag. On a test box, see below |
| The open conversion provider, the Leia conversion provider and the Ultraleap provider as rows at all | wait on the generator change that lists them in schema 2 (#2733) and on signed releases |

Known gaps:

1. Unattended `/COMPONENTS=default` reports a failure of the whole call in
   `catalog-skipped.txt`; an individual item `dxr-get` skipped while the call succeeded (exit
   0) is recorded nowhere: it is only in `dxr-get`'s output, which goes to the details list,
   and a silent install has no such list (its `install.log` says so).
   `dxr-get plan --default` shows what would be skipped and why. Ids given by name do not have
   this gap: each is one `dxr-get install` call whose exit code is recorded.
2. At most eight rows. When the catalog offers more, the text says how many are not shown,
   and the rows kept are, in this order: ticked, then what can be ticked, then installed, then
   not available. So the rows dropped first are the ones that cannot be installed anyway. A
   dropped row cannot be ticked on the page; its id still works with `/COMPONENTS=`.
3. The page's listing needs Windows PowerShell. Where it is blocked by policy the page shows
   no optional row. Unattended `/COMPONENTS=default` does not need it.
4. The texts are English only, like the rest of the installer.

### To verify on Windows

A Windows box ran part of this list on main @0cd768e2b with a test-key `dxr-get` and the
fixture catalog. It found that line 6 did nothing (the refused id was dropped), that `/S`
writes no `install.log` and leaves the previous run's in place, that `/S /COMPONENTS=none`
still ran the listing, and that the page's rows were cut after about 22 characters with a
long reason clipped in the description box. Lines 1, 5, 6, 10 and 12 describe the behaviour
after those fixes, which has not been re-run on Windows (the page layout was changed without
seeing it).
`makensis` compiled the script on macOS with stub payloads, with and
without a `dxr-get.exe`; CI builds the real installer; the listing shim's tests ran under
PowerShell 7 against a fake `dxr-get` that prints a listing in the shape dxr-get.md gives. The
real `dxr-get.exe` has not been run by the shim, and Windows PowerShell 5.1 has not run it.

**Getting a schema-2 catalog onto the test box.** Schema 2 is not published, so the page's
full behaviour needs the test-root-key recipe of dxr-get.md § "Validating on a test machine":
`python scripts/make_test_catalog.py keygen`, a `dxr-get` built with
`-DDXR_GET_TEST_ROOT_KEY=<64 hex>`, then `make_test_catalog.py build` for a signed fixture
catalog (the three real first-party installers plus `fixture.*` items: an incompatible one, an
ABI mismatch, met and unmet prerequisites, a recommended one). Two consequences for this
installer:

- a test-key `dxr-get` reads a local catalog file only, so the installer is run with
  `/CATALOG=C:\dxr-fixture\cat\stable.json`;
- a test-key `dxr-get` is deliberately not installed into `_package\bin` and makes the
  `installer` target fail, so the test installer is built by hand: copy that `dxr-get.exe`
  into `_package\bin` and run `makensis` with the defines `installer/CMakeLists.txt` passes.
  That installer must not leave the test box. A sanctioned way to build it would be better;
  it does not exist yet.

1. Fresh install, interactive, fixture catalog: one row per item; the recommended fixture item
   and (on a Leia display) the Leia plug-in ticked, nothing vendor-specific on a plain monitor;
   the size line changing with each tick; the incompatible and ABI-mismatch fixture items
   greyed with `dxr-get`'s reason in the description box; the unmet-prerequisite item naming
   what is missing; an installed item greyed. Every row's status (`N MB`, `Cached`,
   `Update, N MB`, `Update, cached`, `Installed`, `Not available`) and its version readable
   with **no horizontal scrollbar on any row**, at 100 %, 125 % and 150 % scaling; run it
   twice, so the second run shows the cached forms; no description clipped by its box, the longest "not available" reason included.
2. The same listing by hand, to compare:
   `dxr-get --catalog <file> list --json --assume-runtime <version> --assume-abi display_processor=<n> …`.
3. With the production `dxr-get` and only schema 1 published (today): rows for the Leia SR
   plug-in and the browser, the plug-in pre-ticked on a Leia display.
4. No network when the page opens: the page text, the finish page, exit code 0, the runtime
   working. Network removed after the page: `catalog-skipped.txt`, the finish page list, exit 0.
5. `/S` with no switch: no `dxr-get` process, no network request, same result as the previous
   installer. `/S /COMPONENTS=none`: the same, no PowerShell and no `dxr-get` process either,
   and a `catalog-skipped.txt` left by an earlier run is gone.
   `/S /COMPONENTS=default` (one `install --default` call, no PowerShell),
   `/S /COMPONENTS=leia_plugin`, `/S /COMPONENTS=recommended`, `/S /COMPONENTS=installed`
   (updates what is installed and older, installs nothing new).
6. `--no-runtime`: `/S /COMPONENTS=<an item whose floor is above this runtime>`
   (`fixture.runtime-too-old`; also `fixture.runtime-too-new`, `fixture.abi-mismatch`): one
   `dxr-get install <id> --yes --no-runtime` runs and exits 41; Setup exits 0;
   `catalog-skipped.txt` has `<id>: it is not compatible with this version of the DisplayXR
   Runtime, or needs a newer one (dxr-get exit 41)`; no second runtime installer starts.
   `/S /COMPONENTS=fixture.link-out`: the same with exit 40. `/S /COMPONENTS=no.such.id`:
   exit 40, as before. Interactive with the same switch: the row is greyed and unticked, and
   the finish page lists the item as not installed.
7. Upgrade over an existing install with the Leia plug-in present: shown as installed
   (`Installed - ...`, ticked, greyed), or as an update (`Update, N MB - <name> <new version>`,
   ticked, the description starting `Updates the installed <old version>.`); the
   size line counts the update and not the greyed rows; clearing an update's box leaves the
   installed version in place and removes nothing; the description box shows at most two
   lines for every row, with no text cut mid-word;
   the service stopped and started the expected number of times; `ActiveRuntime`
   handling as documented in dxr-get.md; `Runtime\Abi\RuntimeVersion` stamped with the new
   version.
8. A second run right after the first: rows read "already downloaded" or installed, and
   nothing is fetched.
9. Order: with the open conversion provider ticked, the conversion-models section that follows
   takes its pack from the provider just installed.
10. The components page with eight rows plus the existing four: the list scrolls, the text
    above it fits in two lines, the description box sits under the list, and with more than
    eight optional items the rows not shown are the not-available ones first.
11. A box where PowerShell is restricted by policy: the page as before, and
    `/S /COMPONENTS=default` still working.
12. Log files. After an interactive install: `<install dir>\install.log` holds the `Catalog:`
    lines (verified on main @0cd768e2b). After `/S` over an install that has an older
    `install.log`: the file is replaced by the short record of this run (version, time,
    parameters, the catalog outcome), and `catalog-skipped.txt` exists exactly when something
    asked for was not installed.

## 2. The bundle (`displayxr-installer`)

Today `DisplayXRBundle-*.exe` embeds every child installer and runs them in order: runtime,
shell, MCP tools, Leia SR plug-in, open conversion provider, five demos
(`installer/windows/DisplayXRBundleInstaller.nsi`, `scripts/build-bundle.bat`).

### Already needed, independent of the rest

`scripts/check_bundle_coverage.ps1` fails on any `versions.json` key that is neither chained nor
listed in `$IntentionallyUnbundled`. The first release of the Leia conversion provider, of the
Ultraleap provider or of the open tracking provider adds a key (`leia_conversion`, `ultraleap`,
`tracking_open`) and the mirror carries it there in the same run, so **each name must be in
`$IntentionallyUnbundled` before that release**.
Prepared on the branch `feat/catalog-driven-bundle` of that repository.

### Two outputs from one script

| Output | Embeds | Network at install time |
|---|---|---|
| `DisplayXRBundle-<v>.exe` (default) | the runtime installer, the shell, MCP tools, the demos | needed for the catalog components; without it they are skipped and reported |
| `DisplayXRBundle-<v>-offline.exe` | everything the bundle embeds today, plus the providers a deployment asks for | none |

The demos stay embedded in both for now: they are first-party, not vendor providers, and
they are not in the catalog. The shell is described in schema 2 (section 4); whether the
online bundle keeps embedding it is one of the open decisions listed there.

### Change list

`scripts/build-bundle.bat`

1. A `--offline` switch. Without it, `leia_plugin` and `conversion_open` are not downloaded and
   their `/D` defines are not passed to `makensis`; `/DBUNDLE_ONLINE` is.
2. `--offline` keeps today's behaviour and the output name gains `-offline`.
3. `--offline-with <field,...>` embeds further pinned components (`leia_conversion`,
   `ultraleap`, `tracking_open`) for a deployment that needs them; each must have a pin and a release.

`installer/windows/DisplayXRBundleInstaller.nsi`

4. Under `BUNDLE_ONLINE`, the groups "Vendor plug-ins" and "Conversion providers" are replaced
   by one group "Components from the DisplayXR Catalog (downloaded)" with two rows:
   "Display processor for this display" and "Open conversion provider", both ticked. The
   bundle does not list the catalog itself; the runtime installer owns that.
5. The runtime child runs with `/NOSTART /FORCEACTIVERUNTIME /COMPONENTS=<sel>`: `default`
   when both rows are ticked (one `dxr-get install --default`), else `detected`,
   `recommended` or `none`.
6. **The version-skip path.** When the runtime is already current the bundle does not run the
   runtime child, so `/COMPONENTS=` would never be acted on. On that path the bundle runs the
   installed `dxr-get.exe` itself: `dxr-get install --default --yes --no-runtime
   --no-start-service`.
7. The existing "remove when unticked" branches for the Leia plug-in and the open conversion
   provider stay, reading the same `UninstallString` values.
8. After the runtime child, the bundle reads `<runtime dir>\catalog-skipped.txt` and shows its
   lines on its own finish page; the file's absence means nothing was skipped.
9. The bundle's own repair contract is unchanged: it re-asserts `ActiveRuntime` on every path,
   stops the DisplayXR processes once at the start and starts the service once at the end.
10. Under `BUNDLE_OFFLINE` nothing changes, and the runtime child gets `/COMPONENTS=none`
    explicitly so a future default can never make an offline bundle reach for the network.

`scripts/check_bundle_coverage.ps1`

11. `leia_plugin` and `conversion_open` stay "chained" (the offline variant chains them); the
    check gains a second assertion that the online variant passes neither to `makensis`.

`scripts/build-bundle.sh`, `scripts/build-bundle-linux.sh`, `Distribution.xml`

12. No change (section 3).

`.github/workflows/build-bundle.yml`, `publish-bundle.yml`

13. Build both variants on a pull request; publish both. `assert-versions-in-sync` is unchanged.

Docs: that repository's `README.md` and `CLAUDE.md` (the bundle no longer "chains every
component"), and `versions-json-autobump.md` here (bundle membership stops being a fixed list
of pins read by name).

### Order of work

1. A published schema-2 catalog that lists the open conversion provider. The engine reads it
   already and has the switches this needs (`--no-runtime`, `install --default`).
2. The runtime installer page (this change), verified on Windows.
3. A runtime release carrying both.
4. The bundle change, pinned to that runtime release, both variants built in CI.
5. The first bundle release built this way states in its notes that it needs a connection, and
   links the offline variant.

## 3. macOS, Linux

No behaviour change now. The POSIX install engine is ADR-064 step 5, and until it exists there
is nothing for an installer to call.

| Package | Today | With a POSIX `dxr-get` |
|---|---|---|
| macOS `.pkg` (`DisplayXR-Installer-*.pkg`) | the runtime and the sim-display plug-in | `productbuild` has no run-time list of choices from the network. The `.pkg` stays the core, and its postinstall prints one line naming the command to add components. A catalog UI on macOS is the Dashboard's, when it runs there |
| Linux `.deb` (`displayxr-runtime_*_amd64.deb`) | the runtime; the Leia SR plug-in and the demos are separate `.deb`s | a package must not download other packages in its maintainer scripts. The equivalent of the pre-ticked converter is a `Recommends:` on the open conversion provider's package once it ships as a `.deb` (it is a tarball today), and `Suggests:` for vendor providers. Detection-driven choice belongs to `dxr-get` or the Dashboard, run by the user |
| Linux tarball (`install.sh`) | a per-user install of the runtime | `install.sh --with <id>,...` calling the POSIX `dxr-get`, with the same keywords as `/COMPONENTS=`, and the same rule: no switch, no network |
| macOS / Linux bundles in `displayxr-installer` | chain the available packages | follow the Windows bundle once the engine exists; an offline variant is what they already are |

## 4. The Shell as a component the user picks

Status: the catalog item, the hotkey checks and the "already installed ⇒ ticked" rule of the
runtime installer page are implemented and unit-tested; the Shell installer's shortcut page
and the page change are compile-checked, **not run on Windows**. The bundle part is a
recorded decision with no code yet.

The Shell is an experimental showcase of the workspace concept. Hence: opt-in, never a
surprise for someone who already has it, and nothing extra built around it for now.

The DisplayXR Shell is one registered workspace controller
([workspace-controller-registration.md](../specs/runtime/workspace-controller-registration.md)).
It moves from "the bundle installs it" to "an item in the component list".

### The catalog item

`displayxr-shell` (alias `shell`, the `versions.json` pin), class `workspace_controller`,
`registered_id` `shell`. Schema 2 only: schema 1 stays at its three frozen names.
Generator: `V2_ONLY_COMPONENTS` / `V2_ITEMS` in `scripts/gen_catalog.py`; rules in
component-catalog.md § Generating it.

| Question | Answer |
|---|---|
| Ticked by default? | **No** (decided). No `recommended`, no `detect`: listed unticked on a machine without it, never installed by `install --default` / `/COMPONENTS=default` |
| An existing install | recognised through the registration row's `Version`. It comes up **ticked**: as an update when older, ticked and greyed when current (section 1, "already installed ⇒ ticked"). Nothing is removed by clearing the box |
| Uninstall | the registration row's `UninstallString` with `/S` |
| Runtime floor | the Shell installer's `MIN_RUNTIME_VERSION`, read at the released tag. Unreadable = the item is not listed |
| Unattended | `/COMPONENTS=shell` (or `displayxr-shell`) on the runtime installer, `dxr-get install shell` |
| Waits on | a published schema-2 catalog, and `displayxr-shell-pvt` in the token of `publish-catalog.yml` (component-catalog.md) |

### The launch shortcut

Where the choice lives: **in the Shell's own installer** (a page with a free-form field,
and a silent switch), stored where it already lives, the user's `service.json`, through
`displayxr-cli workspace set`. Not a per-item "option" in the catalog schema.

| | Installer page + switch (chosen) | A generic catalog "option" |
|---|---|---|
| Schema, engine, runtime installer page, Dashboard | unchanged | all four change; `installer.args` stops being "fixed strings: no placeholders" (component-catalog.md), which is a trust rule |
| Unattended | `/HOTKEY=<combo>`, `/HOTKEY=none`; no switch = nothing written | a value the engine would have to template into a command line |
| Upgrade | nothing written unless the user edits the field | the same, with more parts |
| Changing it later | the Dashboard's capture box, `workspace set` (both exist) | the same |
| macOS | the chord is fixed today (Control+Shift+Space); the `.pkg` has no page, and none is needed until the stored combo is applied there | the same limit |
| Linux | a `.deb` must not prompt and its scripts run as root, so they cannot write a per-user file: default chord, changed with the CLI or the Dashboard | the same limit |

The field takes any combo of the grammar and is pre-filled with the combo in effect
(`workspace get shell --hotkey`), which is `Ctrl+Shift+Space` on a new machine. `Ctrl+Space`
is one click away, with a line saying it takes autocomplete and input-method switching away
from applications. Validation is `workspace check-hotkey`: an invalid combo is refused, a
known conflict or a combo another program holds is a warning the user can accept. `none`
means no shortcut: the Shell is then opened from its Start menu entry or the tray.

Why a warning is needed at all: the service's hook is global and runs before every
application (`service_orchestrator.c`), so the chosen combo stops working everywhere else
while the Shell is not running, and the Shell's own global registration takes it while it
runs. `Ctrl+Tab` and `Ctrl+Shift+Tab` would break tab switching in browsers, editors and
terminals exactly as `Ctrl+Space` broke autocomplete.

Known limit: `service.json` is per-user and the installer is elevated. Under an ordinary UAC
prompt that is the same user. Under over-the-shoulder elevation, or a deployment tool running
as SYSTEM, `/HOTKEY=` writes another profile and the logged-on user keeps the default. A
per-machine default carried by the controller's registration would close that; it changes the
"default is never stored" rule of `service.json`. **Decided: not now.** The limitation stays
documented.

### The application entry

Today, per platform:

| Platform | Entry today | A direct launch |
|---|---|---|
| Windows | Start menu shortcut to the Shell executable (its installer has always created it) | starts the service if needed, but is **not** the same as a service launch: the Shell does not get `DISPLAYXR_WORKSPACE_HOTKEY`, so it registers `Ctrl+Shift+Space` as its dismiss chord whatever the user chose, including "none". A second launch prints "already running" and exits |
| macOS | none: the `.pkg` installs a bare executable, not an `.app` | same shape |
| Linux | none: the `.deb` ships no `.desktop` file | same shape |

**Decided: not now.** Windows already has the Start menu entry, and that is the application
entry for the time being. What follows is a documented follow-up, not planned work:

- the entry's result is "the service launches the registered controller", the same path as
  the hotkey. The Shell does that itself: started without `--service-managed`, it asks the
  service to launch it (`displayxr-cli workspace launch shell`, hidden) and exits, and runs
  standalone only when no service can be reached. The entry keeps pointing at the Shell
  executable, so there is no console window and no dead entry when the service is down;
- a second activation **focuses** the running workspace. It does not toggle: closing the
  workspace from a Start menu click is not what a launcher does;
- "no shortcut, application entry only" is a valid choice at install (`none`), once the
  first point holds. Until then a Start menu launch still takes `Ctrl+Shift+Space`;
- macOS gets a small `.app` wrapper in `/Applications`, Linux a `.desktop` file, both with
  the same behaviour.

### The bundle

Decided; **no code change now** (the bundle is `displayxr-installer`, and it changes with the
rest of section 2).

| Bundle | The Shell |
|---|---|
| Online | **stops embedding the Shell once a schema-2 catalog lists it.** The Shell row then drives `/COMPONENTS=...,shell` on the runtime child. Until that catalog is published the bundle keeps embedding it, as today |
| Offline | **keeps carrying the file.** The row is unticked on a machine without the Shell and ticked when the Shell is already installed |
| Both | a machine that has the Shell keeps it: updated when the bundle or the catalog carries a newer one, launch shortcut and launch mode untouched |
| `versions.json` | `shell` stays a pin: the dev orchestrator, the offline bundle and the catalog generator read it |

One thing the bundle change must get right, because the bundle differs from the runtime
installer page: **the bundle removes an installed component whose row is cleared**
(`DisplayXRBundleInstaller.nsi`, the `$G_ShellInstalled == 1` branch of the Shell section,
and the same shape for the Leia plug-in and the open conversion provider). A Shell row that
is simply "unticked by default" would therefore uninstall the Shell on every bundle run if
nothing re-selected it. The bundle's `.onInit` already does the re-selecting: after
`DetectComponent "DisplayXRShell"` it selects the section when the Shell's install marker
exists (`$0 != ""`, which also covers an install whose files were damaged, so the run
repairs it). So the whole change for the offline bundle is one word: declare the section
unselected, `Section /o "DisplayXR Shell" SecShell`, and keep that `SelectSection` line. The
comment above it ("default-checked") changes with it. The removal branch stays: it then
fires only when the user clears the box by hand. To verify on Windows: `/S` over an installed
Shell keeps it; `/S` on a clean machine does not install it; the interactive row follows the
same two states.
