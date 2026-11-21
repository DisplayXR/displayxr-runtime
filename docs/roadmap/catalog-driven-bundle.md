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
  not found (for example the NeurD runtime for the Leia conversion provider). The installer
  never installs such a prerequisite.

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
| `/COMPONENTS=<list>` | comma-separated item ids or aliases (`leia-sr`, `leia_plugin`, `open-conversion`, …) and the keywords `recommended`, `detected`, `default` (= `recommended,detected`), `none` |
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

Only verbs the engine has ([dxr-get.md](../specs/runtime/dxr-get.md)); the installer embeds the
`dxr-get.exe` of its own build, so the two always match.

| Call | Used for |
|---|---|
| `dxr-get [--catalog X] list --json` | the rows: `id`, `name`, `class`, `installed`, `available`, `state`, `installable`, `listing`. It must succeed; exit 10 is "offline", 11 a refused catalog |
| `dxr-get [--catalog X] detect --json` | which items' detection rules match this machine (`high_confidence`), and each item's schema-1 name |
| `dxr-get [--catalog X] catalog` | the schema-2 document, for what `list` does not carry: `recommended`, `summary`, `aliases`, the Windows asset's `size`, `runtime { min, max }`, `requires` |
| `dxr-get [--catalog X] install <id> --yes [--no-start-service]` | one call per chosen item, after the runtime section |
| exit codes 0, 10, 11, 20, 21, 30, 40, 41, 50 | the reason shown for a skipped item |

`installer/catalog_plan.ps1` joins the three reports by item id. `list` states map to the
page's four: `not_installed` is offered, `outdated` and `rebuilt` are offered as an update,
`current`, `newer` and `installed` are shown as installed, and anything not `installable` is
shown as not available. The second and third calls are best effort: without them the rows
remain, nothing is pre-ticked, no size is shown and no runtime range is checked on the page
(`dxr-get install` still refuses an incompatible item, exit 41).

The shim re-judges each item's runtime range against the runtime **being installed**. `list`
does not judge it at all, and `install` judges it only after the user has committed.

### What the engine does not emit yet

Each of these is worked around above. With them, `list --json` alone would answer the page,
in one catalog fetch instead of three.

| Missing from `list --json` | Today's substitute | Cost of the substitute |
|---|---|---|
| `recommended` (the catalog's flag) | read from the `catalog` document | a second fetch; absent when the fallback schema-1 catalog is in use |
| `detected`: `high` / `low` / `none` per item | `detect --json`, joined by `id` | a third fetch |
| `download`: bytes to fetch for the item, including the installer assets of its unmet external prerequisites, minus what the cache holds | the asset's `size` from the document | prerequisite assets and the cache are not counted, so the total shown can be off. `plan --for X --json` has `total_download`, but it plans `ensure-for X` (X's prerequisites, not X) and costs one call per item |
| `summary`, `aliases` | from the document | as above |
| compatibility with a **given** runtime version (`--assume-runtime X.Y.Z`, or a `compatible` field computed against it) | the shim compares `runtime` / `requires` itself | duplicates the engine's rule; the class ABI check is not duplicated, so an ABI mismatch is only caught at `install` |
| unmet external prerequisites per item (`prerequisites[]` with `met`) | named from the document, not judged | the page says "also needs" even when the prerequisite is present |

Two requirements for the engine, beyond fields:

1. **`dxr-get` must not upgrade the runtime from inside the runtime installer.** `install X`
   installs what X requires, the runtime included. Run from the runtime installer, that would
   start a second runtime installer inside the first. The page never offers an item whose
   floor is above the runtime being installed, but an id passed with `/COMPONENTS=` reaches
   `install` unfiltered. The engine needs a switch for this (for example `--no-runtime`,
   answering 40 or 41 instead of planning the runtime), and the installer will pass it.
2. **A selection form for the bundle** (section 2, item 6): install "the recommended set and
   what the hardware matches" in one call, since on the bundle's version-skip path no
   installer is there to resolve those keywords.

### Live and waiting

| Part | State |
|---|---|
| The installer builds and behaves as before when `dxr-get.exe` is not in the build | compiled, both variants |
| `/S` with no switch | unchanged by construction: the install step returns before any call |
| Rows, states, greyed installed items, `/COMPONENTS=<id>` | written against the engine's real output; waits only on a Windows run |
| Hardware pre-tick | as above; needs a catalog whose items carry `detect` rules (the Leia SR item does) |
| Open conversion provider pre-ticked, download sizes, runtime-range check on the page | **waits on a published schema-2 catalog.** `publish-catalog.yml` publishes schema 1 only, and `dxr-get` falls back to it; the fallback has no `recommended`, and the `catalog` document it prints is schema 1, which the shim does not use |
| The open conversion provider, the Leia conversion provider and the Ultraleap provider as rows at all | wait on the generator change that lists them in schema 2 (#2733) and on signed releases |

Known gaps:

1. An id given explicitly with `/COMPONENTS=` that is not in the listing is left entirely to
   `dxr-get install` (requirement 1 above).
2. At most eight rows. When the catalog offers more, ticked items are kept and the text says
   how many are not shown.
3. The listing needs Windows PowerShell. Where it is blocked by policy the page shows no
   optional row.
4. Three catalog fetches before the page appears, behind a "Checking the DisplayXR Catalog"
   banner.
5. The texts are English only, like the rest of the installer.

### To verify on Windows

Nothing below has been run. `makensis` compiled the script on macOS with stub payloads, with and
without a `dxr-get.exe`, and the listing shim's tests ran under PowerShell 7 against a fake
`dxr-get` that answers with documents in the shape dxr-get.md gives. The real `dxr-get.exe`
has not been run by the shim. Windows PowerShell 5.1 has not run it.

1. Fresh install, interactive, schema-2 `dxr-get` and catalog (`/CATALOG=` with a local signed
   file): rows, pre-ticks on a Leia display and on a plain monitor, the size line changing with
   each tick, the description texts, a greyed installed item.
2. The same with no network: the page text, the finish page, exit code 0, the runtime working.
3. Network removed after the page: `catalog-skipped.txt`, the finish page list, exit code 0.
4. `/S` with no switch: no `dxr-get` process, no network request, same result as the previous
   installer. `/S /COMPONENTS=default`, `/S /COMPONENTS=leia_plugin`, `/S /COMPONENTS=none`.
5. Upgrade over an existing install with the Leia plug-in present: shown as installed, or as an
   upgrade; the service stopped and started the expected number of times; `ActiveRuntime`
   handling as documented in dxr-get.md.
6. With only the schema-1 catalog published (today): rows for the Leia SR plug-in and the
   browser, the plug-in pre-ticked on a Leia display, no sizes; `/S /COMPONENTS=leia_plugin`
   installs the plug-in.
7. Order: with the open conversion provider ticked, the conversion-models section that follows
   takes its pack from the provider just installed.
8. The components page with eight rows plus the existing four: the list scrolls, the text above
   it fits in two lines.
9. A box where PowerShell is restricted by policy.

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
5. The runtime child runs with `/NOSTART /FORCEACTIVERUNTIME /COMPONENTS=<sel>`, where `<sel>`
   is built from the two rows (`detected`, `recommended`), or `none`.
6. **The version-skip path.** When the runtime is already current the bundle does not run the
   runtime child, so `/COMPONENTS=` would never be acted on. On that path the bundle runs the
   installed `dxr-get.exe` itself. This needs one more `dxr-get` verb than the runtime
   installer uses, because the keywords have no installer to resolve them: a selection
   form of `install` (requirement 2 in section 1).
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

1. A published schema-2 catalog that lists the open conversion provider (the engine reads it
   already, #2734), and the engine switch that keeps `install` from upgrading the runtime.
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
