# DisplayXR Dashboard

`displayxr-dashboard.exe` — the runtime's desktop dashboard (Avalonia 11 / .NET 9), the
successor of the ImGui Control Panel (`src/xrt/targets/control_panel/`, still shipped
until its removal PR). Design: [ADR-051](../../docs/adr/ADR-051-display-status-model-and-dashboard.md)
and [display-dashboard.md](../../docs/roadmap/display-dashboard.md) §8.

The app is **dumb** (ADR-051 D4): it links no runtime or vendor code and only spawns
the sibling `displayxr-cli.exe`, rendering its JSON.

| Page | Reads | Writes |
|---|---|---|
| Home | the status feed; `info --json` once per show and on Refresh; `dp list --json`; `selftest --json` on click | `runtime activate`, `dp reset` |
| Displays | the status feed (screens, desktop map); `dp list --json` for the per-screen selector | `dp use <id> --screen <key>`, `dp reset --screen <key>` |
| Windows | the status feed (clients) | — |
| Performance | `perf list --json`, `info --json` (adapters) | `perf set` / `perf reset` |
| Developer | `dp list --json` | `dp use <id>`, `dp reset` (machine-wide; admin) |

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

Needs the .NET 9 SDK. Output: `_package\bin\displayxr-dashboard.exe` (~47 MB,
self-contained, compressed single file). CI: the `Dashboard` job in `build-windows.yml`
(tests + artifact `DisplayXR-Dashboard`); the `Runtime` job publishes it into the package
before the installer, which installs it beside `displayxr-cli.exe` with a Start-menu
shortcut "DisplayXR Dashboard".

## Run and debug

- The CLI is resolved next to the exe, then `C:\Program Files\DisplayXR\Runtime`;
  `DXR_DASHBOARD_CLI=<path>` overrides it (development).
- `--page displays|windows|performance|developer|home` opens on that page.
- `--fixture <file.ndjson>` replays snapshots from a file instead of `status --watch`
  (one line every 2 s, looping) and simulates the per-screen `dp` verbs in memory, so the
  live-service pages can be laid out without a service that answers. Fixtures and their
  generator: `src/dashboard/fixtures/` (`two-panels`, `stress-16x32`, `empty-headless`).
- Log: `%LOCALAPPDATA%\DisplayXR\dashboard.log` — lifecycle, child starts / exits, and
  every exception (unhandled AppDomain / TaskScheduler / Dispatcher handlers log and keep
  going). The Developer page shows its path and the session's error count.
- From an elevated shell, launch it through Explorer to drop elevation:
  `explorer.exe _package\bin\displayxr-dashboard.exe`.

## Layout of the code

- `Model/` — pure: the snapshot and CLI JSON models (tolerant readers, every key
  optional), `StatusText` (every string a page shows), `DesktopMapGeometry`.
- `Feed/` — `StatusFeed` (the state machine above, single-threaded on a scheduler),
  `CliProcessSource` (the real CLI), `FixtureProcessSource`, the job object, the vendor
  dashboard launcher (D7: the command line is never parsed).
- `Ui/`, `Pages/`, `MainWindow.cs` — code-built Avalonia views; styles and the palette in
  `Styles/`. A page rebuilds only when its own key changes (counters update in place), and
  never while one of its drop-downs is open.
- `DisplayXR.Dashboard.Tests/` — xunit: the JSON model, warnings → dot / badge, the map
  geometry, the feed state machine (fake process source + manual clock), the per-screen
  selector.
