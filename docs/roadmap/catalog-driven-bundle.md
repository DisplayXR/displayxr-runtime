# Catalog-driven installers

Status: the runtime installer part is implemented and compile-checked, **not yet run on
Windows**. The bundle part is a plan. Decision record:
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
| not installed | `<name> <version> (download N MB)` | if **recommended** by the catalog, or if its detection rule **matched this machine with high confidence** |
| installed, older than the catalog | `<name> <installed> to <version> (download N MB)` | same rule |
| installed at the catalog version, or newer | `<name> <installed> - installed`, greyed | never; nothing is downloaded |
| no signed Windows build, a link-out item, or its runtime range excludes the runtime being installed | `<name> - not available`, greyed, the reason in the description box | never |

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
| a download or an item's installer fails later | the runtime is installed, the item is listed on the finish page and in `<install dir>\catalog-skipped.txt` with the reason, Setup exits 0 |
| the connection drops between two items | the remaining items are skipped without further attempts, and listed |

"How to get it later" is: run Setup again with a connection, or `dxr-get install <id>` from an
elevated prompt. `displayxr-cli catalog install` and the Dashboard's Catalog tab (ADR-064
steps 4 and 8) do not exist yet, so the installer does not mention them.

### Unattended (`/S`)

| Switch | Meaning |
|---|---|
| *(none)* | **unchanged**: the runtime only. No catalog request, no `dxr-get` call |
| `/COMPONENTS=<list>` | comma-separated item ids or aliases (`leia-sr`, `leia_plugin`, `open-conversion`, …) and the keywords `recommended`, `detected`, `default` (= `recommended,detected`), `none`. Exactly `/COMPONENTS=default` is what the bundle passes |
| `/CATALOG=<url\|file>` | passed to `dxr-get` as `--catalog`; the catalog must still be signed. For testing |
| `/NOSTART` | as before; items are then installed with `--no-start-service` |
| `/CATALOG=` and `/COMPONENTS=` values | a value containing a double quote is ignored |

In an interactive run `/COMPONENTS=` replaces the default pre-ticks. `/KEEPACTIVERUNTIME`,
`/FORCEACTIVERUNTIME`, `/NOMODELS`, `/MODELSPACK=` and `/CLOSEAPPS` are untouched, and so are
the `ActiveRuntime` rule and the allowlist rows: none of that code changed.

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
is refused by `dxr-get` (exit 41) and reported as skipped.

`installer/catalog_plan.ps1` adds only the ticks (the pre-tick rule and `/COMPONENTS=`), the
row texts, and the order when there are more items than rows.

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
   `catalog-skipped.txt`; an individual item `dxr-get` skipped while the call succeeded is in
   `install.log` only.
2. At most eight rows. When the catalog offers more, ticked items are kept and the text says
   how many are not shown.
3. The page's listing needs Windows PowerShell. Where it is blocked by policy the page shows
   no optional row. Unattended `/COMPONENTS=default` does not need it.
4. The texts are English only, like the rest of the installer.

### To verify on Windows

Nothing below has been run. `makensis` compiled the script on macOS with stub payloads, with and
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
   what is missing; an installed item greyed.
2. The same listing by hand, to compare:
   `dxr-get --catalog <file> list --json --assume-runtime <version> --assume-abi display_processor=<n> …`.
3. With the production `dxr-get` and only schema 1 published (today): rows for the Leia SR
   plug-in and the browser, the plug-in pre-ticked on a Leia display.
4. No network when the page opens: the page text, the finish page, exit code 0, the runtime
   working. Network removed after the page: `catalog-skipped.txt`, the finish page list, exit 0.
5. `/S` with no switch: no `dxr-get` process, no network request, same result as the previous
   installer. `/S /COMPONENTS=default` (one `install --default` call, no PowerShell),
   `/S /COMPONENTS=leia_plugin`, `/S /COMPONENTS=recommended`, `/S /COMPONENTS=none`.
6. `--no-runtime`: `/S /COMPONENTS=<an item whose floor is above this runtime>` is skipped
   with exit 41 in the log and a line in `catalog-skipped.txt`; no second runtime installer
   starts.
7. Upgrade over an existing install with the Leia plug-in present: shown as installed, or as
   an update; the service stopped and started the expected number of times; `ActiveRuntime`
   handling as documented in dxr-get.md; `Runtime\Abi\RuntimeVersion` stamped with the new
   version.
8. A second run right after the first: rows read "already downloaded" or installed, and
   nothing is fetched.
9. Order: with the open conversion provider ticked, the conversion-models section that follows
   takes its pack from the provider just installed.
10. The components page with eight rows plus the existing four: the list scrolls, the text
    above it fits in two lines.
11. A box where PowerShell is restricted by policy: the page as before, and
    `/S /COMPONENTS=default` still working.

## 2. The bundle (`displayxr-installer`)

Today `DisplayXRBundle-*.exe` embeds every child installer and runs them in order: runtime,
shell, MCP tools, Leia SR plug-in, open conversion provider, five demos
(`installer/windows/DisplayXRBundleInstaller.nsi`, `scripts/build-bundle.bat`).

### Already needed, independent of the rest

`scripts/check_bundle_coverage.ps1` fails on any `versions.json` key that is neither chained nor
listed in `$IntentionallyUnbundled`. The first release of the Leia conversion provider or of the
Ultraleap provider adds a key (`leia_conversion`, `ultraleap`) and the mirror carries it there
in the same run, so **both names must be in `$IntentionallyUnbundled` before either release**.
Prepared on the branch `feat/catalog-driven-bundle` of that repository.

### Two outputs from one script

| Output | Embeds | Network at install time |
|---|---|---|
| `DisplayXRBundle-<v>.exe` (default) | the runtime installer, the shell, MCP tools, the demos | needed for the catalog components; without it they are skipped and reported |
| `DisplayXRBundle-<v>-offline.exe` | everything the bundle embeds today, plus the providers a deployment asks for | none |

The demos and the shell stay embedded in both for now: they are first-party, not vendor
providers, and they are not in the catalog. Moving them is a later, separate step.

### Change list

`scripts/build-bundle.bat`

1. A `--offline` switch. Without it, `leia_plugin` and `conversion_open` are not downloaded and
   their `/D` defines are not passed to `makensis`; `/DBUNDLE_ONLINE` is.
2. `--offline` keeps today's behaviour and the output name gains `-offline`.
3. `--offline-with <field,...>` embeds further pinned components (`leia_conversion`,
   `ultraleap`) for a deployment that needs them; each must have a pin and a release.

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
