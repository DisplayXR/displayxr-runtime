# DisplayXR Dashboard

`displayxr-dashboard.exe` — the runtime's desktop dashboard (Avalonia 11 / .NET 10), the
successor of the ImGui Control Panel (`displayxr-control-panel.exe`, now removed; the
installer sweeps a stale copy and its Start Menu shortcut on upgrade). Design: [ADR-051](../../docs/adr/ADR-051-display-status-model-and-dashboard.md)
and [display-dashboard.md](../../docs/roadmap/display-dashboard.md) §8.

The app is **dumb** (ADR-051 D4): it links no runtime or vendor code and only spawns
the sibling `displayxr-cli.exe`, rendering its JSON.

| Page | Reads | Writes |
|---|---|---|
| Home | the status feed; `info --json` once per show and on Refresh; `dp list --json` (each screen's effective DP, read-only); `selftest --json` on click | `runtime activate`, `dp reset` (only when a machine-wide override is set) |
| Displays | the status feed (screens, desktop map); `dp list --json` for the per-screen selector (its options: that screen's `candidates`) | `dp use <id> --screen <key>`, `dp reset --screen <key>`; the camera calibration verbs (below) |
| Components | the status feed (clients by class; per screen `eye_tracking.viewer_source` / `viewer_reason` / `viewer_policy` for the tracking card); `info --json` (incl. `tracking_providers`: probe results, the primary screen's arbitration); `workspace list --json`; `dp options <id> --json` (a display driver's Options, below); `dp\|input\|conv\|tracking list --json` (ADR-054 N3e: `enabled`, `enabled_source`, `preferred`, `provenance`, conv `built_in`, tracking `first`, `policy.policy_lock`; ADR-054 N5: each row's `licence` object, shown in the row's Details as a *Licence* line — `unlicensed`, `licence expired`, `trial until <date>`, `licensed`, or `included with your display` when the entitlement comes with the hardware; nothing for a component with no licence to report. The Dashboard only shows it: the component decides it) | `workspace set <id> --hotkey <combo>` / `--no-hotkey` / `--mode auto\|disabled`, `workspace reset <id>` (back to the runtime default, Ctrl+Shift+Space / auto), `workspace launch <id>`; `dp set <id> <KEY> <value>` / `dp unset <id> <KEY>` (Options); `dp\|input\|conv enable\|disable <id>`, `dp\|input use <id>` / `reset`, `input use <id> --app <exe>` / `input reset --app <exe>` (the Input card's **Per app** table, ADR-034 Am.5: each pinned or running app's pin per role family and its live holder from `input list --json` `per_app` + `holders`; per user only — no machine-wide per-app pin exists — and disabled under `Policy\Lock`), `conv use <task> <id>` / `conv reset <task>\|all` (per user; `--machine` only from an elevated dashboard, otherwise every control's `--machine` command — Enabled, "Use this one", "Choose automatically" — is shown for an administrator, in the row's Details). No Enabled or "Use this one" on the built-in `dp-lift` row; under `Policy\Lock` the class shows "Managed by your administrator" (the technical `Policy\Lock` line is in each row's Details) and its per-user controls are disabled (for tracking, the per-user "Use this one" / "Choose automatically" too). **Viewer tracking** (tracking providers, ADR-054 N6): `tracking enable\|disable <id>`, `tracking use <id>` / `tracking reset` (then re-reads `tracking list --json` and `info --json`); per screen the viewer source and why (provider; standby: the display tracks; no source; source paused; FIXED / OWN) beside the display's viewer policy, with a link to Displays on provider-served screens, where the display's card has **Calibrate camera…**, which opens the calibration window (below): `tracking calibrate --screen <key> --watch --json` (long-lived), `tracking calibrate solve --in <tmp> --mode b --eye mid --reject-mm 5 --json`, `tracking calibrate apply --screen <key> --camera <key> --pose <setting> --provider <id> --rms-mm <rms> --samples <n> --mode b [--depth-scale <iris>] --json` (writes the runtime-owned camera calibration store, shared by every provider; `--provider` is accepted and ignored; then re-reads `tracking list --json` and `info --json`) |
| Components (catalog side) | the install engine's `dxr-get list --json --no-runtime` (one call: state, versions, detection, sizes, compatibility, prerequisites), `dxr-get list --installed --json` when offline; `dp\|input\|conv\|tracking list --json` for an installed item's `licence` object | `dxr-get install <id> --yes --json --no-runtime`, `dxr-get uninstall <id> --json` (elevated for that one run, see below); `displayxr-cli <class> activate <id> @<key file> --json`; `displayxr-cli <class> activate-begin <id> --json` (Buy) |
| Settings (`settings`; `performance` opens it) | `perf list --json`, `info --json` (adapters), `workspace list --json` | `perf set` / `perf reset`; the spatial desktop's `workspace set` / `reset` / `launch` (as on Components) |
| Troubleshooting > Running 3D apps (`windows` opens it) | the status feed (clients; present owners and status readers by role) | — |
| Troubleshooting > Graphics cards | `info --json` (adapters, the graphics pipeline's verdict and setting) | — |
| Troubleshooting > Advanced (`developer` opens it) | `perf list --json` (Current values); `perf list --all --json` (the read-only Tier 1/2 lever list: only while "Developer settings" is expanded, and on Refresh; collapsed again on every launch); `dp list --json` (`preferred` only: a machine-wide override is shown as a notice) | `perf reset <name>` / `perf reset` (managed levers only; the rest are environment-only), `dp reset` (machine-wide; admin) |

## Structure (five rail entries)

The rail is `Model/PageRoutes.cs` `Rail`: **Home**, **Displays**, **Components**, **Settings**, and,
set apart at the bottom under a rule, **Troubleshooting**. Everything that needs the runtime's own
vocabulary (variable names, registry paths, process ids, technical lines) is in Troubleshooting or
behind a Details toggle.

- **Home** answers "is my 3D display working?" (`Model/Verdict.cs`). One headline, one row per
  display with up to three facts (*3D display recognised*, *3D on*, *Sees you*; each yes, no or
  *not known yet*), the display driver by name (its id in the tooltip), then **Needs attention**
  (plain sentences, or "nothing") and **Check my setup** with *Last checked* (this session only; the
  Dashboard saves nothing). A display has live state only while an app draws on it through the
  running service (`screens[].dps`); with no app running both facts are *not known yet* and the
  headline never says 3D is on. Headlines: *3D is working on N display(s)*; *Your 3D display is
  ready* / *3D turns on when a 3D app runs.* (idle); *3D is off: N thing(s) need fixing*; *Your 3D
  display is recognised* (background service not reached); *No 3D display found*; *No display
  found*. A display claimed only by the simulated display driver (the snapshot's `plugins[].fallback`
  driver, which claims every display at `FALLBACK` so apps run anywhere) is an ordinary display, not
  a recognised 3D display, unless that driver was chosen for it (`claim.forced`, or a per-display
  `claim.preferred_plugin` naming it, from any source) or holds it only
  because a vendor driver is unhealthy (a CLAIM_FALLBACK / PLUGIN_NOT_READY warning, or a registered
  non-fallback driver that failed to load or whose platform is not ready: then it is a 3D display that
  needs attention, as before). When the simulated driver is the only display driver registered at all
  and no display fell back, *No 3D display found* reads "DisplayXR apps show in normal
  2D on this PC. Nothing is wrong." (an info level, not a warning) with a **Try a 3D preview** link to
  that driver's Details on Components, where its Options are (`Verdicts.OnlySimulated`). The rule
  "is this display a real 3D display" is one pure function, `StatusText.IsReal3D`, shared by Home and
  Displays. "Needs attention" is built only from what is already read: every warn / critical warning
  of the snapshot (the runtime's codes in plain words, a display maker's own warning quoted), the
  background service not reached, a failed "Check my setup", and another program set as the active
  OpenXR runtime. Its only buttons are ones the Dashboard already had (Open Displays, Open
  Components, Make DisplayXR the active OpenXR runtime). The graphics cards card keeps its plain
  half; its technical half is in Troubleshooting > Graphics cards.
- **Displays**: each card's face has the name, the plain badges, the plain state line, the warnings
  and its actions (the display maker's app, **Calibrate camera…** on a display that camera head
  tracking serves, **Details**: `Details of <display>` / `Hide details of <display>`). Behind Details:
  the resolution, driver and maker lines, the per-display driver selector with what it does, and the
  technical lines "Copy list" writes. The selector stays on the face when more than one driver could
  drive the display or more than one 3D display is present (`StatusText.DriverChoiceOnFace`). The
  desktop map and its legend show only with more than one display. The caption never prints a zero
  ("One display, recognised as 3D · whether it sees you is not known yet"). A display claimed only by
  the simulated display driver (`StatusText.IsReal3D` false, as on Home) has the badge *Ordinary 2D
  display*, the state line "The simulated display driver gives it a 3D preview." (while no app draws on
  it), a plain dot, a Details line "Not recognised as a 3D display · display driver: <name>, which
  gives it a 3D preview", and is not counted as "recognised as 3D"; its driver selector and the
  technical lines (FALLBACK) are unchanged.
- **Components**: what you have and what you can add (below). Running programs are not on it.
- **Settings** (was Performance): *Graphics card for 3D apps* (hidden on a PC with one graphics
  card), *Spatial desktop* (the shortcut, mode and "Open now": the same `Ui/WorkspaceLaunchEditor`
  as on Components; hidden when none is installed), *If 3D looks wrong* (Compatibility mode,
  Detailed logging). The "changed from the defaults" notice names settings, not variables, and links
  to Troubleshooting > Advanced.
- **Troubleshooting**: a tab strip of radio buttons named by their labels, **Running 3D apps** (the
  old Windows page, plus a *By role* card for apps with their own window and status readers),
  **Graphics cards** (the graphics pipeline's technical lines) and **Advanced** (the old Developer
  page with the *Current values* table). The header shows the part's caption and actions. It holds
  the status feed for all three parts. The part shown is kept for the session only.

| `--page` / `Navigate` id | Lands on |
|---|---|
| `home`, `displays`, `components`, `settings`, `troubleshooting` | that page |
| `catalog` | Components |
| `performance` | Settings |
| `windows` | Troubleshooting > Running 3D apps |
| `developer` | Troubleshooting > Advanced |
| `troubleshooting/apps` \| `graphics` \| `advanced` | that part of Troubleshooting (the pages' own links) |

Tests: `PageRoutesTests` (every id above), `VerdictTests` (each headline, the three-state facts, the
idle rule, the plain warning sentences).

Every CLI run is an argument vector (`Model/CliArgs`, passed through
`ProcessStartInfo.ArgumentList`): an id or value is always exactly one argument, whatever
a manifest put in it; the log pane shows the readable form. A write's result line is
followed by every `note:` / `warning:` line the CLI printed.

**Components** groups what is plugged into the runtime by ROLE, never by product. The
section titles are plain words: **Displays** (display drivers, i.e. display processors),
**Spatial desktop** (the workspace controller), **Input**, **Viewer tracking** (camera head
tracking), **2D to 3D in the display driver** (`lift_caps`), **2D to 3D** (conversion providers),
**2D-to-3D models**, **Stereo camera**. The running programs, **Apps with their own window** (present
owners) and **Status readers** (diagnostics clients), are `Components.RunningPrograms` and are shown
under Troubleshooting > Running 3D apps. By role:
display processors, the workspace controller (its launch shortcut / mode / Reset / "Open now",
phase 8, hidden when the CLI has no `launch` block; the same controls are on Settings; the shown combo is whatever `workspace list --json`
reports, so an older runtime shows its own default; `Model/Hotkey.Default` — Ctrl+Shift+Space, Ctrl+Space
before 2026-10-26 — only feeds the Reset tooltip and the fixture), input providers and
the rig role, the viewer-tracking providers with each screen's viewer source (ADR-054 N6), the 2D->3D conversion module (`info --json` `lift_caps`, kept for one release), the conversion
providers with each task's readiness and selection (the status feed's `conv_providers[]` /
`conv_tasks[]`, ADR-053; shown only when the CLI reports them), a stereo camera source (only when `info`
reports one). Names are shown verbatim from the components'
own registration.

## Components: installed and catalog items in one list

ADR-064 step 8. Spec of the engine: [dxr-get.md](../../docs/specs/runtime/dxr-get.md). The catalog
had a tab of its own for one release (v2.69.4); it is now part of **Components**, and the
`catalog` page id (`--page catalog`, `DashboardContext.Navigate("catalog")`) opens Components
(`Model/ComponentRows.cs` `PageRoutes`). The updates badge sits on the Components entry.

- **One list.** The sections are the Components sections, unchanged (display processors, the
  workspace controller, input providers, viewer-tracking providers, conversion providers, and
  the ones that only report: the conversion module, model packs, the stereo camera). Each section whose class the catalog has (`display_processor`,
  `input_provider`, `tracking_provider`, `conversion_provider`, `workspace_controller`) also
  carries that class's catalog items. A first group, **DisplayXR**, holds the catalog's row for the
  runtime itself. A last group, **Apps**, holds the catalog items that are not components (apps,
  companions, developer tools), each with its kind in Details (*Category*). Both print a
  count of their non-zero parts only ("2 installed", "1 available to install", or both;
  `CatalogText.Counts`).
- **One row per item, one status per row** (`Model/ComponentRows.cs`, `ComponentRows.Join`). An
  installed component and its catalog entry are joined by the id the component registers under
  (`registered_id`, else the catalog `id`) and are one row:

  | Status | When | Offered |
  |---|---|---|
  | **Installed** | present, and the catalog has the same build (or no catalog has been read: nothing is claimed) | the row's own controls; Details; Uninstall when the engine can do it |
  | **Update available** | present, and the catalog has another build (`outdated` / `rebuilt`) | the same, and **Update** |
  | **Available** | in the catalog, not on this machine | **Install** (or **Get it** for a link-out item); Details |
  | **Installed separately** | present, and the catalog does not list it, or lists it but the engine did not find this copy (a dev registration, another installer, a preinstall) | the row's own controls only, and "Remove with its own uninstaller." (last line of Details) |
  | **Part of the runtime** | as *Installed separately*, and the runtime's own installer shipped it: its registered binary (`binary_path` / `binary` of `<class> list --json`) is inside the `plugins` folder of the runtime directory, the folder of the `displayxr-cli` the Dashboard runs (`ComponentRows.ShippedByRuntime`) | the row's own controls only, and no removal sentence (it goes with the runtime) |

  A row that is neither installed nor installable (a connected client, a summary line) has no
  status. The built-in conversion adapter (`dp-lift`, compiled into the runtime) reads *Part of the runtime*; it is listed only when the display driver offers lift (an older driver that still carries a converter), so most PCs show no such row. Filters: **All / Installed /
  Updates**.
- **Installed rows keep their controls**: Enabled, **Use this one** / **Choose automatically**
  (accessible names `Use this one: <name>` / `Choose automatically: <name>`; per task "Use this one
  for <task>"), the launch row of a workspace controller, the per-app and per-screen tables. Each row
  has ONE strip: Enabled, "Use this one", Install / Update and Details on one line, Details opening
  below it in the same box. Every section card takes the full width, one per row. Under them, a row the catalog knows has
  **Install** / **Update**, **Details** (what it does, version, size, publisher, category, each
  external prerequisite with *Ready* / *Included* / *Get it first* and a link to its maker for an
  unmet one, the licence block, **Uninstall**). *Works with your display* (`detected: high`) is a
  chip on the row; Details says the same in one fixed sentence; the engine's `evidence` string (internal panel codes) is parsed and never shown. An item the engine would refuse
  (`compatible: false`) has no button and one calm line; the engine's own reason is in its detail.
- **What a row shows, and what is in Details** (`ComponentRow.Face`). On the row: the name, the status
  chip, at most one plain state word, *Works with your display* when it applies, and one plain line
  under the name (`ComponentItem.Summary`: publisher · version, and for a display driver which display
  it drives, "drives your main display"). Every technical line (registration id and ProbeOrder,
  hosting, licence class, the claims, each conversion task's state, the component's own hint, the
  workspace controller's status, process and shortcut source) is a labelled line of Details
  (`ComponentItem.LineFacts`), and so is how a row that cannot be removed from here is removed. The state
  word maps the component's technical state (`Model/Components.cs` `PlainStates`): **In use** (an
  `ACTIVE` display processor, a conversion provider selected for a task, a tracking provider
  serving a screen, the selected input provider while its hardware is present, a connected
  workspace controller), **Ready** (loaded or usable but not the one in use: `LOADED`, `BUILT_IN`,
  an input provider with its hardware present, the selected input provider with no device connected
  or not known — its row line reads "No device connected" instead of the publisher line only when
  the device is known to be absent; `input list
  --json` `active` is only "selected", the hardware is `info --json` `input_providers.hardware_present`
  / `active_id`, or a provider's own `hardware_present` when the CLI lists them, a tracking provider whose probe found a source, an available conversion module, an
  installed model pack), **Standby** (registered and allowed but not loaded or not needed:
  `NOT_ATTEMPTED`, `DECLINED`, superseded, a controller not running), **Turned off** (its Enabled
  value, or the loader's `DISABLED`), **Needs attention** (refused by the provenance gate: "Blocked:
  publisher not verified"; any
  other load failure, malformed descriptors, a failing task). A connected client or a summary line
  has none. Every other chip is a labelled line of Details (*Load state*, *Platform*, *Role*,
  *Enabled setting*, *Preference*, *Signature*, *Provenance gate*, *Licence*, *Probe*, *Hardware*,
  *Flags*, …; provenance reads "Approved on this PC", "Publisher not verified"), with what a policy
  change means and, from a non-elevated Dashboard, each control's machine-wide command. Enabled and
  "Use this one" stay on the row. Every row with any of that has a
  Details toggle (`Details of <name>` / `Hide details of <name>`), catalog row or not.
- **After an install the same row is the installed row**, with its detail open: the row's key is
  its class and registered id, which do not change. The status feed, the class's policy rows and
  the workspace list are re-read.
- **Uninstall is offered only when it can succeed.** The engine must have installed the item
  (`receipt: true`) AND say it can remove it (`can_uninstall: true` in `dxr-get list --json`:
  the receipt recorded an uninstall command). A receipt alone is not enough: an item installed
  from a catalog entry that names no uninstall command (every schema-1 item) would be refused
  with exit 43, so its row's Details end with "Remove it from Windows Settings > Apps." instead of a button
  (the runtime's own row shows the same sentence).
  A list without the field (an older engine) reads as "cannot remove". A row that is not from
  the catalog is never offered Update, Install or Uninstall. **Disable is not Uninstall**: the
  Enabled box turns a component off and leaves it on the machine; Uninstall (in Details)
  removes it.
- **What happens to an item is said on its row.** A success is the row's new status and one line
  ("… is up to date. It is ready to use.") that stays until the next action, with nothing to
  dismiss. A failure (a declined permission prompt, each exit code as a sentence) is a notice on
  that row that stays until it is dismissed.
- **When the catalog cannot be read** (offline, refused, the engine missing, a platform with no
  engine) one banner at the top says so, with **Try again** or the Download page. Everything
  installed stays below and works as usual. Rows from the last catalog read in this session are
  kept, their status reading *… (last known)*, and the banner says when that read was; with no earlier read
  the banner is all there is (offline, what the engine installed is still listed from its
  receipts and can still be removed). A *last known* row offers no Install, and an installed one
  no Update, until the catalog can be read again. While a read is in progress (the first one, Try
  again, Refresh; the engine retries for about 20 s before it answers offline) a "Checking for
  add-ons and updates…" line says so (`CatalogText.Checking`).
- **Install / Update / Uninstall, with the Dashboard staying non-elevated.** The verb first runs
  as the user, output captured. The engine answers what elevation cannot fix before it asks for
  it (offline 10, bad signature 11, not compatible 41, nothing to do 0, no receipt 42, cannot
  uninstall 43), so none of those raises a prompt. Only on exit 50 is the same command started
  again as the **work run**: the shell's `runas` verb on `dxr-get.exe` itself (one Windows
  permission prompt, no command interpreter, arguments as a vector).
- **The work run is detached and never killed.** It is started through the shell, is not put in
  the Dashboard's kill-on-close job and is given no kill token, so it outlives the wait and the
  window. Its output cannot be read (an elevated child's pipes are out of reach), so it reports
  its exit code only, and the page shows an indeterminate bar, the elapsed time and a phase line
  (the engine prints no progress a UI can render). The phase says "Waiting for your permission…"
  until the process is known to be running, and only then "Downloading, verifying and
  installing…". The wait is bounded (75 min; the engine bounds a component installer at one hour).
- **From an already elevated Dashboard** there is no unelevated first run: it would be the install
  itself, as a job child killed by Stop waiting or by closing the window. The work run is started
  directly (default verb, the elevation is inherited, no prompt) and answers the same exit codes.
- **Stop waiting** stops the wait, never the engine, elevated Dashboard or not. While the engine
  may still be at work, Install / Update / Uninstall stay disabled on every item, with a line saying
  what is still running (the engine takes no lock, so a second run must not start beside it). They
  come back when the process is known to have exited (the result is then shown and the list
  re-read) or when Refresh is pressed. Every ending is a sentence (`Model/Catalog.cs`
  `CatalogExit`): each exit code, a declined prompt, an engine that did not start, a timeout.
- **`--no-runtime` on every call.** The Dashboard and the engine live in the runtime's own
  directory, which the runtime installer replaces; the runtime is therefore never installed or
  upgraded from here. An item that needs a newer runtime reads as not compatible, and the runtime's
  own row links to the Download page when an update exists.
- **The banner's states**, each with what to do next: offline; the catalog refused (exit 11); the
  engine missing on Windows (a runtime that predates it: update from the Download page); **macOS
  and Linux**, where no engine exists yet: "Installs come from the Download page for now", with
  the link.
- **Licence** (ADR-064 §5), for an installed item whose component reports one. The state, in the
  component's own words plus the date ("Trial — ends 21 Dec 2026"), and "Your other
  2D-to-3D converters keep working".
  - `source: hardware` (or `oem`) is the zero-click case: "Included with your display", and
    **no Buy, no key box and no activation handler**, in any state. Buy and Paste a key are for the
    `key` and `trial` sources.
  - **Buy** (`docs/specs/runtime/activation-deep-link.md` §3, PR #2748) runs
    `displayxr-cli <class> activate-begin <id> --json` and acts on its `result`: `ok` opens its
    `url` **exactly as given** (it is complete: the vendor's page with `displayxr_nonce`,
    `displayxr_item` and `displayxr_return` already on it; the Dashboard appends and rebuilds
    nothing, and logs only `nonce_id`); `no-deep-link` opens the licence's `purchase_url` as it is;
    `no-purchase-url` opens nothing and removes the Buy button; `too-many`, `store-error` and
    `not-registered` show one calm sentence and leave the key box. A runtime without the verb
    (no JSON, an unknown-command or usage error) is "not available": the `purchase_url` as it is.
    The verb is not run at all when the component says `activation.deep_link: false`. Only an
    absolute `https` URL with a host and no credentials is ever opened (`SafeUrl`), on Windows
    through the shell's default verb on the URL itself, on macOS / Linux as the single argument of
    `open` / `xdg-open`: never through a command interpreter.
  - **After Buy the licence changes underneath the Dashboard** (the `displayxr://activate` handler
    delivers the one-time code to the component). While a Buy is pending the item's class list is
    re-read when the window comes back to the front and every 15 s (a poll that ends with the window), until the pending activation
    expires (`expires_unix`, 15 minutes) or the licence changes, then it stops. A purchase page
    opened without a return link gets the focus refresh only.
  - **Paste a key** opens a masked box. The key is written to a new file only its owner can read
    (mode 0600; on Windows an ACL with one entry, inheritance off, under `%LOCALAPPDATA%\DisplayXR`),
    the CLI gets `@<that file>`, and the file is overwritten and deleted whatever happens. The key is
    never an argument, an environment variable, a log line or a field of the page. Not offered when
    the component says it takes no key (`activation.key: false`).
  - **A key belongs in the key box, never in a link.** What comes back through the link is a
    one-time code the component redeems with its maker; a reusable licence key goes only through
    the masked box; `source: hardware` needs neither.
- **Where the engine is.** `dxr-get.exe` beside the CLI, then the installed runtime's;
  `DXR_DASHBOARD_GET=<path>` overrides it (development). **All of it sits behind
  `Feed/CatalogService.cs`** (`CatalogCommands`, `DxrGetEngine`): when `displayxr-cli catalog …`
  exists, that file is what changes.
- **Fixtures.** `--fixture` with a `catalog-list.json` beside the snapshot file simulates the engine
  in memory (`Feed/FixtureCatalogEngine.cs`: the engine's order of answers, a short delay for the
  elevated run, `fixture_exit` / `fixture_prompt` keys for the unhappy paths; an offline document
  shows the offline view), and `conv-list.json` / `tracking-list.json` beside it simulate those
  class lists and `activate` (the key `DXR-SIM-TEST-KEY` is accepted). Generator:
  `fixtures/gen_catalog_fixtures.py`. The shapes are written by hand from the emitter, not captured
  from an engine run.
- **Debug builds only** (`#if DEBUG`, absent from a published dashboard): `--catalog-item <id>`
  opens that item's row on Components, `--catalog-do install|uninstall|key` runs a verb on it, and
  `--screenshot <png> [--screenshot-delay <s>]` saves the window and exits.
- Ids are arguments: a catalog `id` and a `registered_id` must be one safe argument (letters,
  digits, `. _ -`, no leading dash); an item whose id is not is dropped, an unsafe `registered_id`
  is dropped, and the activation verbs refuse one.
- Tests: `ComponentRowsTests` (the join, the four statuses, no Uninstall without a receipt, the
  filters, the stale and receipts-only lists, the `catalog` route) and `CatalogTests` (both list shapes, grouping and filters, every exit code, the unelevated
  then elevated order, the licence block, the key never in a `ProcessStartInfo`, the key file's
  permissions and removal, the URL rule, every `activate-begin` result and the runtime without the verb).

## Components: a display driver's Options

ADR-065: a display driver serves two rendering modes at run time, 2D and 3D; which kind of 3D, and
everything else, is a per-driver **setting** that the driver declares and the runtime stores. A
display driver's Details on Components open with an **Options** block (then the technical labelled
lines, the scope and command-line notes, the catalog detail and the removal sentence; `Model/DriverOptions.cs`,
`ComponentsPage.OptionsBlockView`).

- **Read** `displayxr-cli dp options <id> --json` when the driver's Details are first opened (also
  when Home's "Try a 3D preview" or an install opens them), again after every write, and on Refresh;
  never polled. Fields used: `env_prefix`, and per entry of `settings[]`: `key`, `declared`, `type`
  (`bool` / `int` / `float` / `enum` / `string` / `secret`), `default`, `help`, `secret`, `value`,
  `set` (secrets), `source` (`environment` / `user` / `machine` / `manifest` / `absent`), `values`
  (an enum's accepted words; interim, the CLI reads them from the help; null or absent = not listed). An entry
  that is not declared (`type: null`, a value stored for a key the driver no longer declares) or of a
  type the Dashboard does not know is left out. Only a successful read with nothing to show is "no
  options" ("This display driver has no options to set here."). A failing run (a timeout, an older CLI
  without the verb, a driver that went away) or a document of another shape keeps what an earlier read
  showed and adds one line, "Couldn't read its options.", with **Try again** (accessible name `Try
  again reading options: <driver>`); no banner. The reply to a write stays visible even when its row
  is not drawn.
- **Write** `dp set <id> <KEY> <value>` (per user; a secret: `dp set <id> <KEY> --stdin`) and `dp unset <id> <KEY>` ("Use the default"),
  through the same action path as the other controls (`DashboardContext.RunOptionWriteAsync`); the
  CLI's reply, with its `note:` / `warning:` lines, shows on the row it was for. The Dashboard saves
  nothing itself, and never writes `--machine`.
- **Each row**: the label, a control by type, "Now: <value> · <where it comes from>", the help, and
  "Use the default" when this user has a value stored (`source: user`; clearing changes nothing for a
  value set for everyone on this PC). Where it comes from, in words: *Default* (`absent`), *Your
  choice* (`user`), *Set for everyone on this PC* (`machine`, `manifest`), *Set outside the Dashboard*
  (`environment`: read-only, with "Set outside the Dashboard, and that value wins: a change here would
  not apply."). The key, type, default, the driver's own help and the environment variable's name are
  the label's tooltip, never the face.
- **Controls**: an enum with known values is a drop-down (an enum with no friendly entry whose `values`
  the CLI lists gets a drop-down of those raw words under More options; a friendly entry keeps its labels
  but drops a value the CLI does not list, counting its aliases, and appends, as its raw word, one the CLI
  lists that it does not know; with no `values`, the entry's own list); a bool a switch (on writes `1`, off `0`); an
  int, a float, a string, and an enum whose values the CLI does not list, a text box with **Apply**
  (Enter applies, and Apply acts on the press so a rebuild cannot swallow the click; a submitted edit
  ends the edit at once, `Model/OptionEdits.cs`, so the new value and the reply show without waiting
  for the focus to leave; a whole number for an int, a number with a dot for decimals for a float (a comma, as in "1,000", is refused, never rewritten), a string kept exactly as typed but not only spaces, one line, no NUL, never starting with `--`, checked before anything is written). A **secret** is never shown: *Set* / *Not
  set*, a masked text box with **Set** (Enter sets; accessible name `Set <label>: <driver>`), and
  **Clear**. Set runs `dp set <id> <KEY> --stdin` and writes the value to the CLI's standard input
  as one line (`IProcessSource.RunWithInputAsync`, `DashboardContext.RunSecretWriteAsync`): it is never
  an argument, never kept as a draft, never logged, never in "Copy report", and the box is emptied as
  the write starts (checked first: not empty, one line, no NUL, at most 2047 bytes). A CLI older than
  `--stdin` answers that `set` needs a value; the row then says "This version of DisplayXR can only
  clear this value."
- **Friendly settings** come first, in words; every other one is under **More options (N)**,
  collapsed by default (accessible name `More options: <driver>`), with its raw key as the label.
  The friendly presentation is data, `FriendlyOptions.Table` (driver id or any driver, key, label,
  control, expected type, help, value labels, aliases, notes, order): adding one is a row there. An
  entry applies only when the setting is declared with the type it expects (MULTI_VIEWER declared as
  a bool or an int is listed under More options by its key); stored values match without regard to
  case ("SBS", "Quad").

  | Driver | Key | Label | Control | Values shown |
  |---|---|---|---|---|
  | simulated display (`sim-display`) | `OUTPUT` | 3D preview kind | drop-down | Red / cyan (anaglyph) `anaglyph`, Side by side `sbs`, Side by side, squeezed `squeezed` (also `squeezed_sbs`), Four views (2x2) `quad`, Interlaced `interlaced` (also `interlace`), Blend `blend`. `2d` / `passthrough` are not offered (Start in and Always 2D cover 2D); a stored one reads "No 3D preview kind is chosen: apps stay in 2D", nothing selected, and Start in then carries "Has no effect until a 3D preview kind is chosen." No top-and-bottom kind exists. |
  | simulated display | `START_3D` | Start in | two-way choice | 2D `0`, 3D preview `1` |
  | simulated display | `FORCE_MODE` | Always 2D | switch | on writes `0`; off always writes the default `-1` for this user (never an unset: `dp options` shows only the winning layer, so an unset could leave a machine-wide value winning); "Use the default" unsets; a stored `1` shows as its own state, *Always 3D preview*, with "Use the default" |
  | any driver | `MULTI_VIEWER` | Several viewers | drop-down | Follow the display's own setting `system`, On `on`, Off `off` |

  The four-view kind carries "For checking four-view apps. Most apps show only two of the views, or
  stay in 2D." under the control while it is in force. The CLI's JSON has no per-value view count
  (and the status snapshot only the kind in force), so this is keyed on the value name `quad`.
- **Under the block**: "Changes apply the next time a 3D app starts; apps already running keep what
  they started with. Apps that run through DisplayXR in the background (the browser, shell apps) pick
  up a change only after DisplayXR restarts." The service hosts those apps and read the settings when it
  started. The Dashboard has no "restart DisplayXR" action yet, so the sentence says it instead of a button.
- **Not supported here**: when the driver reports (`dp options --json` `status`, its
  `get_setting_status` slot) that the value in force cannot take effect on this PC, the row keeps its
  control editable and adds a warning chip "Not supported here" (tooltip: the driver's note), the
  driver's note under it ("This display driver can't use this value on this PC. …" when it gives none),
  "Using: <the value used instead, in the row's own words>", and "Now:" becomes "Chosen:". A
  `restart_required` status is an info chip "Takes effect after a restart". `applies` / `unknown`
  (or a CLI without `status`) show nothing new.
- **Accessible names**: each control `<label>: <driver>` (a two-way choice's buttons
  `<label> <choice>: <driver>`, Apply `Apply <label>: <driver>`), the reset `Use the default for
  <label>: <driver>`.
- **Fixtures**: `fixtures/dp-options-<id>.json` beside the snapshot file simulates `dp options <id>
  --json` and `dp set` / `dp unset` in memory (a driver with no file answers like an unregistered
  id), so a `--fixture` run never writes a setting. Hand-written in the CLI's shape
  (`src/xrt/targets/cli/cli_component_options.c`), with one entry of each kind the parse must handle.
- Tests: `DriverOptionsTests` (the parse and what it drops, no options, the friendly table and order,
  each control, the source words, the read-only environment value, the pin's writes, text checks,
  the commands, the accessible names, the context's read / write / re-read against the fixture).

## Camera calibration window

Displays → the display's card → **Calibrate camera…** (only on a display whose viewer source is a
provider; Components → Viewer tracking links there) opens a borderless full-screen window on that screen (the monitor that best overlaps the
screen's desktop rect from the status feed; a points-vs-pixels platform is matched at the feed's
scale). Method: [tracking-provider-discovery.md §6](../../docs/specs/runtime/tracking-provider-discovery.md#camera-pose-calibration).

- Black, a bright ring at the edges to light the face (+ / − adjust it), a white cross on each target
  in turn: a near pass (about 40 cm) then a far pass (about 70 cm). The targets are sized from the
  geometry (`CalibLayout`, spec §6 *Dashboard*): ideally five central dots near and a 3 × 3 grid at a
  12 % inset far; each is checked against a conservative 60° / 16:9 camera view from the hello's
  `camera_pose` (the prior), keeping the eyes plus 70 mm up, 70 mm down and 75 mm to each side 2°
  inside it, and moved toward the camera's axis (vertically first) when it does not fit; merged dots
  are dropped, and a pass where nothing fits moves back 5 cm at a time. The layout is logged. A
  target's display-space- One `tracking calibrate --screen <key> --watch --json` child for the window's life (stopped by
  closing its stdin, killed after 1.5 s; Esc stops it at any time). Prompt: close the left eye, find
  the closed eye's reflection with the right eye, put the cross on it, Space. Space captures at once
  when the eye midpoint's camera-space direction has been still (< 0.3°) for 0.5 s, otherwise it
  arms and captures as soon as it is (the ring round the cross brightens with the still run). While
  there is no face lock, or the measured face reaches the edge of the assumed view, the window says
  "move so your face stays in the camera's view" and holds the capture; a second Space captures
  anyway (the view is an assumption). The
  captured line is recorded verbatim with its dot as `{dot_m, sample}`, `eye: "mid"`.
- After the last target the far pass's mean estimated eye distance must be ≥ 1.5× the near pass's
  (`CalibGeometry.MinDistanceRatio`); otherwise the window says how far each pass was and queues the
  far pass again. Then the pairs go to `<LocalApplicationData>/DisplayXR/calibration-last-solve.json`
  (kept, overwritten by the next solve) and `calibrate solve` (mode `c`: full rotation + T_x, T_y; no
  focal scale; reject 5 mm). Only a `degenerate` refusal re-solves with mode `b`, and the result says
  it is the reduced model. `dashboard.log` gets one line per solve with the pose and its quality. The
  result shows RMS, max, iris scale, the pose, the two passes' distances and each sample's residual.
  R retakes the flagged samples (the rejected ones; on RMS > 5 mm, those over 5 mm), A starts over; a
  refusal (`ok: false`) shows the solver's reason.
- Space: the live check. A green dot follows the eye midpoint's predicted foot, computed in the
  Dashboard from each sample's camera block through the solved pose and iris scale
  (`T + R · (iris · s̃ · u)`, what the provider will do once applied); the prompt asks for a check
  leaning in and sitting back. Arrow keys nudge T_x / T_y by 1 mm. Enter runs `calibrate apply`;
  Backspace goes back to the result.
- All of it is `Model/Calibration.cs` + `Model/CalibrationSession.cs` (pure: NDJSON, dot geometry,
  stability, solve / apply parsing, the state machine); `Ui/CalibrationWindow.cs` only draws it.
  Tests: `CalibrationTests` over fixtures captured from the CLI with sim-tracking `CAMERA=1`.

## Feed cadence (ADR-051 D5)

- One long-lived `displayxr-cli status --watch --json` child while a status page (Home,
  Displays, Troubleshooting) is on screen and the window is not minimised. Moving between status
  pages keeps the same child. Leaving for Components / Settings stops it after a 15 s
  linger (a quick hop back reuses it: a child that cannot reach the service builds a
  headless snapshot, and each of those creates a vendor instance); minimising stops it at
  once. Stopping = stdin closed (the CLI's stop signal), killed after 1.5 s if it lingers,
  and the snapshot is dropped.
- A child that exits after ≥ 10 s is restarted; two young exits in a row → fallback to
  `status --json` **every 30 s, never faster** (each headless run creates a vendor
  instance), with a "Live status isn't available" banner. A poll that sees `source: service`, or
  every 4th poll, retries the watch child — at most one poll and one watch attempt per 30 s.
- A line that does not parse keeps the last good snapshot and shows "last read failed,
  retrying" in the header. Never a modal, never a crash.
- The header's pill says "DisplayXR is running" (`source: service`) or "Not running in the
  background" (headless), the technical source line in its tooltip. A headless snapshot also
  shows the banner "DisplayXR isn't running in the background" (`StatusText.NotRunningText`:
  what the pages show and what is missing; it does not promise that 3D apps work meanwhile).
  **Copy report** copies the full status snapshot as JSON (unchanged).
- `info --json` (≈ 10 s: it loads every plug-in) is never polled.
- Every child is in a kill-on-close job object: no `displayxr-cli` outlives the dashboard.
- One dashboard per logon session (a second launch raises the first): each one holds a
  DIAG slot on the service (quota 4).
- Run it **non-elevated**: the service answers status reads only from non-elevated DIAG
  clients; elevated, the feed is headless (Troubleshooting > Advanced says so).

## Build

```bat
scripts\build_windows.bat dashboard     REM just the dashboard (skipped with a WARN without dotnet)
scripts\build_windows.bat all           REM the dashboard is published before the installer packs
```

or directly (what both do):

```bat
dotnet publish src\dashboard\DisplayXR.Dashboard\DisplayXR.Dashboard.csproj -c Release -r win-x64 ^
  --self-contained true -p:PublishSingleFile=true -p:PublishTrimmed=false ^
  -p:EnableCompressionInSingleFile=true -o _package\bin
dotnet test src\dashboard\DisplayXR.Dashboard.sln
```

Needs the .NET 10 SDK. Output: `_package\bin\displayxr-dashboard.exe` (~47 MB,
self-contained, compressed single file). CI: the `Dashboard` job in `build-windows.yml`
(tests + artifact `DisplayXR-Dashboard`); the `Runtime` job publishes it into the package
before the installer, which installs it beside `displayxr-cli.exe` with a Start-menu
shortcut "DisplayXR Dashboard".

## Linux

The `.deb` and the tarball ship it (`scripts/package_deb_linux.sh`,
`scripts/package_linux.sh`): `/usr/lib/displayxr/bin/displayxr-dashboard` (tarball:
`bin/displayxr-dashboard`) beside `displayxr-cli`, plus a "DisplayXR Dashboard" menu entry
and icon from `scripts/linux/dashboard/`. Published as

```bash
dotnet publish src/dashboard/DisplayXR.Dashboard/DisplayXR.Dashboard.csproj -c Release -r linux-x64 \
  --self-contained true -p:PublishSingleFile=true -p:PublishTrimmed=false -p:EnableCompressionInSingleFile=true
```

(no .NET runtime needed on the target; `InvariantGlobalization` = no ICU; the Skia /
HarfBuzz natives are inside the file and extract on first run to `~/.net`, or
`$DOTNET_BUNDLE_EXTRACT_BASE_DIR`). The window needs fontconfig and the X11 client
libraries (the `.deb`'s `Recommends`). `displayxr-dashboard --version` prints the version
and exits without a display server: it is the packaging smoke on Ubuntu 22.04 / 24.04 /
26.04. CI: the `DashboardLinux` job in `build-linux.yml` (Linux tests + the publish, one
artifact for both packages). On Linux the CLI is resolved next to the binary, then
`/usr/lib/displayxr/bin`, `~/.local/share/displayxr/bin`, `/usr/bin`. The Windows-only
parts (the job object, raising the first instance) are no-ops there.

## Run and debug

- The CLI is resolved next to the exe, then `C:\Program Files\DisplayXR\Runtime` (Linux:
  see above); `DXR_DASHBOARD_CLI=<path>` overrides it (development).
- **Run the CLI that sits beside the running service.** The service admits a DIAG client
  (the status feed) only from its own directory (#960); any other `displayxr-cli` is
  admitted as APP and refused the status read. The CLI then says so on stderr and shows
  the headless snapshot; the dashboard logs that note (`status --watch says: …`) and, if
  a watch child prints nothing for 10 s, a WARN and a "No status yet" pill. Mixing a
  `_package\bin` dashboard with an installed service (or the reverse) is the usual way in.
- `--version` prints the version and exits (no window).
- Rail badges: Displays (things to check), Components (updates), Settings (changed settings),
  Troubleshooting (how many 3D apps are running), and an amber **1** on Home when one display driver is set for every
  display on this PC (`dp list --json` `preferred`). Each badge is said in words: its tooltip, the
  rail entry's accessible name ("Home: One display driver is set ..."), and on Home the notice that
  explains it and offers "Clear this setting".
- `--page home|displays|components|settings|troubleshooting` opens on that page; the older ids still
  land where their content is (`catalog`, `performance`, `windows`, `developer`: the table under
  *Structure*). The rail's order is Home, Displays, Components, Settings, then Troubleshooting set apart.
- `--fixture <file.ndjson>` replays snapshots from a file instead of `status --watch`
  (one line every 2 s, looping) and simulates the per-screen `dp` verbs in memory, so the
  live-service pages can be laid out without a service that answers. Fixtures and their
  generator: `src/dashboard/fixtures/` (`two-panels`, `stress-16x32`, `components`,
  `empty-headless`). A `dp-options-<id>.json` beside the fixture simulates `dp options` / `set` /
  `unset` for that display driver. A `workspace-list.json` beside the fixture simulates the
  `workspace list|set|launch` verbs in memory, and a `dp-machine-override.txt` (one plug-in
  id) simulates a machine-wide `PreferredPlugin` that a global `dp reset` clears.
- Log: `%LOCALAPPDATA%\DisplayXR\dashboard.log` — lifecycle, child starts / exits, and
  every exception (unhandled AppDomain / TaskScheduler / Dispatcher handlers log and keep
  going). Troubleshooting > Advanced shows its path and the session's error count.
- From an elevated shell, launch it through Explorer to drop elevation:
  `explorer.exe _package\bin\displayxr-dashboard.exe`.

## Layout of the code

- `Model/` — pure: the snapshot and CLI JSON models (tolerant readers, every key
  optional), `StatusText` (every string a page shows), `DesktopMapGeometry`,
  `ComponentPolicy` (the per-class Enabled / Preferred rows of `dp|input|conv list --json`
  and the CLI lines the Components controls run — ADR-054 N3e), `Tracking` (the tracking card:
  info's probe summary, each screen's viewer source / reason / policy — ADR-054 N6),
  `Calibration` / `CalibrationSession` (the camera calibration window's model), `Catalog` (the
  engine's list, the words of every exit code, the licence block, the URL rule), `Verdict` (Home's
  headline, rows and "Needs attention"), `PageRoutes` (the rail and every page id), `ComponentRows`
  (the Components list: installed components joined with catalog items, a row's status and what
  it may offer), `DriverOptions` (a display driver's settings from `dp options --json`, the friendly
  table and each Options row).
- `Feed/` — `StatusFeed` (the state machine above, single-threaded on a scheduler),
  `CliProcessSource` (the real CLI), `FixtureProcessSource`, the job object, the vendor
  dashboard launcher (D7: the command line is never parsed), `CatalogService` (the install engine,
  the elevated run, the key file, opening a URL) and `FixtureCatalogEngine`.
- `Ui/`, `Pages/`, `MainWindow.cs` — code-built Avalonia views; styles and the palette in
  `Styles/`. A page rebuilds only when its own key changes (counters update in place), and
  never while one of its drop-downs is open or one of its text boxes has the focus.
- `DisplayXR.Dashboard.Tests/` — xunit: the JSON model, warnings → dot / badge, the map
  geometry, the feed state machine (fake process source + manual clock), the per-screen
  selector.
