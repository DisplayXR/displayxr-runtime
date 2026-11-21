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
| Displays | the status feed (screens, desktop map); `dp list --json` for the per-screen selector (its options: that screen's `candidates`) | `dp use <id> --screen <key>`, `dp reset --screen <key>` |
| Windows | the status feed (clients) | — |
| Components | the status feed (clients by class; per screen `eye_tracking.viewer_source` / `viewer_reason` / `viewer_policy` for the tracking card); `info --json` (incl. `tracking_providers`: probe results, the primary screen's arbitration); `workspace list --json`; `dp\|input\|conv\|tracking list --json` (ADR-054 N3e: `enabled`, `enabled_source`, `preferred`, `provenance`, conv `built_in`, tracking `first`, `policy.policy_lock`; ADR-054 N5: each row's `licence` object, shown as a chip — `unlicensed`, `licence expired`, `trial until <date>`, `licensed`, or `included with your display` when the entitlement comes with the hardware; nothing for a component with no licence to report. The Dashboard only shows it: the component decides it) | `workspace set <id> --hotkey <combo>` / `--no-hotkey` / `--mode auto\|disabled`, `workspace reset <id>` (back to the runtime default, Ctrl+Shift+Space / auto), `workspace launch <id>`; `dp\|input\|conv enable\|disable <id>`, `dp\|input use <id>` / `reset`, `input use <id> --app <exe>` / `input reset --app <exe>` (the Input card's **Per app** table, ADR-034 Am.5: each pinned or running app's pin per role family and its live holder from `input list --json` `per_app` + `holders`; per user only — no machine-wide per-app pin exists — and disabled under `Policy\Lock`), `conv use <task> <id>` / `conv reset <task>\|all` (per user; `--machine` only from an elevated dashboard, otherwise every control's `--machine` command — Enabled, Prefer, Reset — is shown for an administrator). No Enabled or Prefer on the built-in `dp-lift` row; under `Policy\Lock` the class shows "Locked by machine policy" and its per-user controls are disabled (for tracking, the per-user Prefer / Reset too). **Tracking providers** (ADR-054 N6): `tracking enable\|disable <id>`, `tracking use <id>` / `tracking reset` (then re-reads `tracking list --json` and `info --json`); per screen the viewer source and why (provider; standby: the display tracks; no source; source paused; FIXED / OWN) beside the display's viewer policy, and a **Calibrate camera…** button on provider-served screens that opens the calibration window (below): `tracking calibrate --screen <key> --watch --json` (long-lived), `tracking calibrate solve --in <tmp> --mode b --eye mid --reject-mm 5 --json`, `tracking calibrate apply --screen <key> --camera <key> --pose <setting> --provider <id> --rms-mm <rms> --samples <n> --mode b [--depth-scale <iris>] --json` (writes the runtime-owned camera calibration store, shared by every provider; `--provider` is accepted and ignored; then re-reads `tracking list --json` and `info --json`) |
| Catalog | the install engine's `dxr-get list --json --no-runtime` (one call: state, versions, detection, sizes, compatibility, prerequisites), `dxr-get list --installed --json` when offline; `dp\|input\|conv\|tracking list --json` for an installed item's `licence` object | `dxr-get install <id> --yes --json --no-runtime`, `dxr-get uninstall <id> --json` (elevated for that one run, see below); `displayxr-cli <class> activate <id> @<key file> --json`; `displayxr-cli <class> activate-begin <id> --json` when the CLI has it |
| Performance | `perf list --json`, `info --json` (adapters) | `perf set` / `perf reset` |
| Developer | `perf list --all --json` (the read-only Tier 1/2 lever list: only while "Developer settings" is expanded, and on Refresh; collapsed again on every launch); `dp list --json` (`preferred` only: a machine-wide override is shown as a notice) | `perf reset <name>` / `perf reset` (managed levers only; the rest are environment-only), `dp reset` (machine-wide; admin) |

Every CLI run is an argument vector (`Model/CliArgs`, passed through
`ProcessStartInfo.ArgumentList`): an id or value is always exactly one argument, whatever
a manifest put in it; the log pane shows the readable form. A write's result line is
followed by every `note:` / `warning:` line the CLI printed.

**Components** groups what is plugged into the runtime by ROLE, never by product:
display processors, the workspace controller (its launch hotkey / mode / Reset / "Launch now",
phase 8, hidden when the CLI has no `launch` block; the shown combo is whatever `workspace list --json`
reports, so an older runtime shows its own default; `Model/Hotkey.Default` — Ctrl+Shift+Space, Ctrl+Space
before 2026-10-26 — only feeds the Reset tooltip and the fixture), present owners, input providers and
the rig role, the viewer-tracking providers with each screen's viewer source (ADR-054 N6), the 2D->3D conversion module (`info --json` `lift_caps`, kept for one release), the conversion
providers with each task's readiness and selection (the status feed's `conv_providers[]` /
`conv_tasks[]`, ADR-053; shown only when the CLI reports them), a stereo camera source (only when `info`
reports one) and diagnostics clients. Names are shown verbatim from the components'
own registration.

## Catalog tab

ADR-064 step 8. Spec of the engine: [dxr-get.md](../../docs/specs/runtime/dxr-get.md).

- **Browse.** Items grouped by what they are for (Displays, Input, 2D to 3D, Viewer tracking,
  Workspaces, Apps, …). Inside a group: what matches this machine first, then what the catalog
  recommends, then by name. A card shows the name, the publisher, the one-line summary and chips
  that state positive facts only: *Installed*, *Update available*, *Works with your display*
  (`detected: high`), *Recommended*, the download size, and the licence headline of an installed
  item. An item that is not installed has no state chip; its button says **Get it**. An item the
  engine would refuse (`compatible: false`) has no button and one calm line; the engine's own
  reason is in the detail view. Search, a filter per class and **Installed**.
- **Detail.** What it does, version, size, publisher, category, each external prerequisite with
  *Ready* / *Included* / *Get it first* and a link to its maker for an unmet one, the licence
  block, the actions, and **Manage in Components**. Every policy row of the Components page has
  **View in Catalog**, which opens the matching item (or, for a component the catalog does not
  list, the Catalog searched by its name).
- **Install / Update / Uninstall, with the Dashboard staying non-elevated.** The verb first runs
  as the user, output captured. The engine answers what elevation cannot fix before it asks for
  it (offline 10, bad signature 11, not compatible 41, nothing to do 0, no receipt 42, cannot
  uninstall 43), so none of those raises a prompt. Only on exit 50 is the same command started
  again through the shell's `runas` verb on `dxr-get.exe` itself: one Windows permission prompt,
  no command interpreter, arguments as a vector. An elevated child's output cannot be read from
  a non-elevated process, so that run reports its exit code only, and the page shows an
  indeterminate bar, the elapsed time and a phase line (the engine prints no progress a UI can
  render). The wait is bounded (75 min; the engine bounds a component installer at one hour).
  **Stop waiting** stops the wait, never the engine: a non-elevated process cannot stop an
  elevated one, and an installer is not interrupted half way; the page says so and Refresh shows
  the result. Every ending is a sentence (`Model/Catalog.cs` `CatalogExit`): each exit code, a
  declined prompt, an engine that did not start, a timeout. The list is re-read afterwards.
- **`--no-runtime` on every call.** The Dashboard and the engine live in the runtime's own
  directory, which the runtime installer replaces; the runtime is therefore never installed or
  upgraded from here. An item that needs a newer runtime reads as not compatible, and the runtime's
  own row links to the Download page when an update exists.
- **States that are not a list**, each with what to do next: opening; offline (with what the engine
  installed, from the receipts, still removable); the catalog refused (exit 11); the engine missing
  on Windows (a runtime that predates it: update from the Download page); **macOS and Linux**, where
  no engine exists yet: "Installs come from the Download page for now", with the link.
- **Licence** (ADR-064 §5), for an installed item whose component reports one. The state, in the
  component's own words plus the date ("Trial — ends 21 Dec 2026"), and "Your other
  2D-to-3D converters keep working".
  - `source: hardware` (or `oem`) is the zero-click case: "Included with your display", and
    **no Buy, no key box and no activation handler**, in any state. Buy and Paste a key are for the
    `key` and `trial` sources.
  - **Buy** opens the vendor's `purchase_url` in the default browser. Only an absolute `https` URL
    with a host and no credentials is ever opened (`SafeUrl`), on Windows through the shell's
    default verb on the URL itself, on macOS / Linux as the single argument of `open` / `xdg-open`:
    never through a command interpreter. When the component takes a deep-link token
    (`activation.deep_link`) and the CLI has `<class> activate-begin <id> --json`, that verb starts
    the activation and its single-use nonce is added to the checkout URL (`PurchaseLink`); a CLI
    without the verb is the silent fallback to the purchase URL as it is.
  - **Paste a key** opens a masked box. The key is written to a new file only its owner can read
    (mode 0600; on Windows an ACL with one entry, inheritance off, under `%LOCALAPPDATA%\DisplayXR`),
    the CLI gets `@<that file>`, and the file is overwritten and deleted whatever happens. The key is
    never an argument, an environment variable, a log line or a field of the page. Not offered when
    the component says it takes no key (`activation.key: false`).
  - **A key belongs in the key box, never in a URL.** A reusable licence key goes through the
    masked box (and, for a component that stores it, its `secret` setting). What comes back
    through `displayxr://activate` should be a one-time code the component exchanges with its
    maker's service: URLs end up in browser history, referrers and system logs. The Dashboard puts
    nothing but the item id, the single-use nonce and the return address in a URL.
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
  opens that item, `--catalog-do install|uninstall|key` runs a verb on it, and
  `--screenshot <png> [--screenshot-delay <s>]` saves the window and exits.
- Tests: `CatalogTests` (both list shapes, grouping and filters, every exit code, the unelevated
  then elevated order, the licence block, the key never in a `ProcessStartInfo`, the key file's
  permissions and removal, the URL rule, `activate-begin`).

## Camera calibration window

Components → Tracking providers → **Calibrate camera…** (only on a screen whose viewer source is a
provider) opens a borderless full-screen window on that screen (the monitor that best overlaps the
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
  Displays, Windows) is on screen and the window is not minimised. Moving between status
  pages keeps the same child. Leaving for Performance / Developer stops it after a 15 s
  linger (a quick hop back reuses it: a child that cannot reach the service builds a
  headless snapshot, and each of those creates a vendor instance); minimising stops it at
  once. Stopping = stdin closed (the CLI's stop signal), killed after 1.5 s if it lingers,
  and the snapshot is dropped.
- A child that exits after ≥ 10 s is restarted; two young exits in a row → fallback to
  `status --json` **every 30 s, never faster** (each headless run creates a vendor
  instance), with a "Live feed unavailable" banner. A poll that sees `source: service`, or
  every 4th poll, retries the watch child — at most one poll and one watch attempt per 30 s.
- A line that does not parse keeps the last good snapshot and shows "last read failed,
  retrying" in the header. Never a modal, never a crash.
- `info --json` (≈ 10 s: it loads every plug-in) is never polled.
- Every child is in a kill-on-close job object: no `displayxr-cli` outlives the dashboard.
- One dashboard per logon session (a second launch raises the first): each one holds a
  DIAG slot on the service (quota 4).
- Run it **non-elevated**: the service answers status reads only from non-elevated DIAG
  clients; elevated, the feed is headless (the Developer page says so).

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
  a watch child prints nothing for 10 s, a WARN and a "no snapshot" pill. Mixing a
  `_package\bin` dashboard with an installed service (or the reverse) is the usual way in.
- `--version` prints the version and exits (no window).
- `--page displays|windows|components|catalog|performance|developer|home` opens on that page.
- `--fixture <file.ndjson>` replays snapshots from a file instead of `status --watch`
  (one line every 2 s, looping) and simulates the per-screen `dp` verbs in memory, so the
  live-service pages can be laid out without a service that answers. Fixtures and their
  generator: `src/dashboard/fixtures/` (`two-panels`, `stress-16x32`, `components`,
  `empty-headless`). A `workspace-list.json` beside the fixture simulates the
  `workspace list|set|launch` verbs in memory, and a `dp-machine-override.txt` (one plug-in
  id) simulates a machine-wide `PreferredPlugin` that a global `dp reset` clears.
- Log: `%LOCALAPPDATA%\DisplayXR\dashboard.log` — lifecycle, child starts / exits, and
  every exception (unhandled AppDomain / TaskScheduler / Dispatcher handlers log and keep
  going). The Developer page shows its path and the session's error count.
- From an elevated shell, launch it through Explorer to drop elevation:
  `explorer.exe _package\bin\displayxr-dashboard.exe`.

## Layout of the code

- `Model/` — pure: the snapshot and CLI JSON models (tolerant readers, every key
  optional), `StatusText` (every string a page shows), `DesktopMapGeometry`,
  `ComponentPolicy` (the per-class Enabled / Preferred rows of `dp|input|conv list --json`
  and the CLI lines the Components controls run — ADR-054 N3e), `Tracking` (the tracking card:
  info's probe summary, each screen's viewer source / reason / policy — ADR-054 N6),
  `Calibration` / `CalibrationSession` (the camera calibration window's model), `Catalog` (the Catalog
  tab: the engine's list, the words of every exit code, the licence block, the URL rule).
- `Feed/` — `StatusFeed` (the state machine above, single-threaded on a scheduler),
  `CliProcessSource` (the real CLI), `FixtureProcessSource`, the job object, the vendor
  dashboard launcher (D7: the command line is never parsed), `CatalogService` (the install engine,
  the elevated run, the key file, opening a URL) and `FixtureCatalogEngine`.
- `Ui/`, `Pages/`, `MainWindow.cs` — code-built Avalonia views; styles and the palette in
  `Styles/`. A page rebuilds only when its own key changes (counters update in place), and
  never while one of its drop-downs is open.
- `DisplayXR.Dashboard.Tests/` — xunit: the JSON model, warnings → dot / badge, the map
  geometry, the feed state machine (fake process source + manual clock), the per-screen
  selector.
