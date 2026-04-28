# Phase 2.0 Test Plan — Workspace Detection + Auth Handshake

Self-contained test plan for a Windows-side agent. Drop into a fresh session as the user message after `/clear`. Validates the 5 commits that landed Phase 2.0 (controller detection, conditional tray, sidecar manifest, PID-match auth).

Branch under test: `feature/shell-brand-separation` (Windows MSVC CI is green; functional behavior on a real Leia SR display is what's being validated here).

---

## What you're testing

Phase 2.0 of the shell brand separation effort made the runtime a standalone platform:

- The service can be installed without the DisplayXR Shell. With no controller binary present, the tray honestly reflects "bare runtime" — Bridge submenu only — and `Ctrl+Space` is a no-op.
- An optional `<binary>.controller.json` sidecar manifest lets the controller advertise a friendly display name for the tray submenu.
- Workspace activation is gated by **orchestrator-PID match** instead of the literal `application_name == "displayxr-shell"` check. A forged `applicationName` no longer grants workspace privileges.

Read these in order if you need more context:
1. `docs/roadmap/spatial-workspace-extensions-phase2-agent-prompt.md` — the implementation brief that drove the 5 commits.
2. `docs/roadmap/spatial-workspace-controller-detection.md` — manifest schema + detection design.
3. `docs/roadmap/spatial-workspace-auth-handshake.md` — auth design.

The 5 commits to validate:

| SHA prefix | Subject |
|---|---|
| `82a858eb` | service: workspace controller sidecar manifest loader |
| `20a2445f` | service: detect workspace controller, gate tray + hotkey |
| `e27ba153` | service: add service_orchestrator_get_workspace_pid() |
| `f08f8d7d` | ipc: PID-match auth for workspace_activate (with legacy fallback) |
| `cc435623` | ipc: drop legacy app-name fallback, finalize PID-only auth |

---

## Setup (one-time)

### Build

From the repo root in a Windows shell:

```bat
scripts\build_windows.bat all
```

First-time `all` downloads SR SDK / vcpkg / OpenXR loader. Subsequent iterations: `scripts\build_windows.bat build` (runtime only) or `test-apps` (test apps only). Output lands in `_package\` and `test_apps\*\build\`.

### Verify the binaries are where the test plan expects

```bat
dir _package\bin\displayxr-service.exe
dir _package\bin\displayxr-shell.exe
dir _package\bin\displayxr-webxr-bridge.exe
```

All three should exist. If not, the build didn't finish — re-run with `all`.

### Where logs live

The orchestrator and tray write to the U_LOG sink, which on Windows lands in:

```
%LOCALAPPDATA%\DisplayXR\service.log    (if set up; some setups log to stdout/Windows Event)
```

If `service.log` doesn't exist, run the service from a console with `displayxr-service.exe` directly so you can see stderr. The orchestrator log lines you care about are prefixed with `WARN:` (the `OW(...)` macro maps to `U_LOG_W`).

### How to read the tray submenu

Right-click the DisplayXR icon in the system tray. The menu structure under test is the workspace submenu — its presence and label.

### Stop the service between scenarios

Right-click tray → "Exit DisplayXR Service". Wait ~2 s before starting the next scenario so the service unwinds the orchestrator and the named pipe.

---

## The minimum useful run (30 minutes)

If you only have time for three tests, run **Scenario 1, 3, 4**. They cover the tray detection path, the manifest path, and the happy auth path — together they prove the major behavioral changes work end-to-end.

---

## Test scenarios

Each scenario lists **Setup → Run → Verify → Cleanup**. Capture screenshots of the tray menu where relevant — see the bottom of this doc for the screenshot procedure.

### Scenario 1 — Bare runtime (no controller installed)

**The big one.** Proves the runtime stands on its own without the shell.

**Setup**
```bat
ren _package\bin\displayxr-shell.exe displayxr-shell.exe.bak
del /q _package\bin\displayxr-shell.controller.json 2>nul
```

**Run**
```bat
_package\bin\displayxr-service.exe
```
(Don't pass `--workspace`. Plain default.)

**Verify**
- Tray right-click menu shows: **WebXR Bridge** submenu, **Start on Windows login** toggle, **Exit DisplayXR Service**. **No Workspace Controller submenu.**
- Service log contains: `WARN: Workspace controller not installed (looked for ...displayxr-shell.exe)`.
- Press `Ctrl+Space` somewhere on the desktop. Nothing happens. No process spawns. (Check Task Manager for `displayxr-shell.exe` — should not appear.)

**Cleanup**
```bat
ren _package\bin\displayxr-shell.exe.bak displayxr-shell.exe
```

---

### Scenario 2 — Shell present, no manifest (fallback label)

Proves the manifest is optional and the fallback display name kicks in.

**Setup**
- `_package\bin\displayxr-shell.exe` present (default state after build).
- No `_package\bin\displayxr-shell.controller.json`.

**Run**
```bat
_package\bin\displayxr-service.exe
```

**Verify**
- Tray right-click → submenu labeled **"Workspace Controller"** appears (the fallback).
- Submenu has Enable / Auto / Disable radio items; current selection matches `service.json` config.
- Service log contains: `WARN: Workspace controller detected: Workspace Controller (binary=...displayxr-shell.exe)`.
- Press `Ctrl+Space`: shell launches, compositor enters workspace mode. Verify by seeing the shell's UI (cursor / window chrome) on the SR display. ESC dismisses.

**Cleanup**
- Stop service from tray.

---

### Scenario 3 — Shell + valid manifest (custom display name)

Proves the manifest's `display_name` flows into the tray label.

**Setup**

Create `_package\bin\displayxr-shell.controller.json`:
```json
{
  "schema_version": 1,
  "display_name": "DisplayXR Shell",
  "vendor": "Leia Inc.",
  "version": "1.0.0"
}
```

**Run**
```bat
_package\bin\displayxr-service.exe
```

**Verify**
- Tray submenu labeled **"DisplayXR Shell"** (not "Workspace Controller").
- Submenu still has Enable / Auto / Disable.
- Service log: `WARN: Workspace controller detected: DisplayXR Shell (binary=...)`.
- Toggle Enable → shell auto-launches. Toggle Disable → shell terminates. Toggle Auto → shell exits, hotkey re-registered.

**Cleanup**
- Leave the manifest in place for Scenarios 4 / 5 / 6 (they assume Scenario 3's setup).

---

### Scenario 3b — Manifest with malformed schema (regression check)

Quick negative test: the loader should reject `schema_version != 1` and fall back to the default label.

**Setup**

Edit the manifest's `schema_version` to `99`.

**Run / Verify**
- Restart service.
- Service log: `WARN: workspace manifest: unsupported schema_version in ... (expected 1)`.
- Tray submenu reverts to **"Workspace Controller"** (fallback).

**Cleanup**
- Restore `schema_version` to `1`.

---

### Scenario 4 — PID auth happy path

The non-attacker case: orchestrator spawns the shell; the shell calls `workspace_activate`; auth allows it because `caller_pid == expected_pid`.

**Setup**
- Service running with shell mode = Auto and the manifest from Scenario 3 in place.

**Run**
1. Start the service: `_package\bin\displayxr-service.exe`.
2. Press `Ctrl+Space` to spawn the shell.
3. Launch a couple of test apps from the shell launcher (or via `_package\bin\displayxr-shell.exe app1.exe app2.exe` legacy mode).

**Verify**
- Shell window appears on the SR display.
- Apps composit normally — multi-app rendering, hit-test, etc. all work as in Phase 1.
- Service log shows **no `workspace_activate: PID mismatch`** warnings.
- Verify the orchestrator-spawned PID matches the shell's actual PID:
  ```bat
  tasklist | findstr displayxr-shell
  ```
  Compare to the orchestrator log line `Launched workspace controller (PID NNNN)` — same PID.

**Cleanup**
- ESC to dismiss shell, then exit service from tray.

---

### Scenario 5 — Manual mode (developer flow, no orchestrator-spawn)

Proves first-claim-wins still works for the dev workflow described in CLAUDE.md ("legacy multi-terminal").

**Setup**
- Use `_package\run_shell_service.bat` if it exists (starts service with `--workspace`), or run manually:
  ```bat
  _package\bin\displayxr-service.exe --workspace
  ```
  In `--workspace` mode the orchestrator does **not** spawn anything; `expected_pid == 0` (provider returns 0 because `s_workspace_running` stays false).

**Run**
- Manually launch the shell from a separate terminal:
  ```bat
  _package\bin\displayxr-shell.exe
  ```

**Verify**
- Shell connects, calls `workspace_activate`, gets `XR_SUCCESS` (compositor enters workspace mode).
- Service log shows **no PID-mismatch warnings** (auth check fell through because `expected_pid == 0`).

**Cleanup**
- Close shell, exit service.

---

### Scenario 6 — service.json backwards-compat (Phase 1 regression check)

Phase 1 added a fallback for the legacy `"shell"` JSON key. Phase 2.0 shouldn't have broken it.

**Setup**

Edit `%LOCALAPPDATA%\DisplayXR\service.json`. If the file doesn't exist, create it:
```json
{
  "shell": "enable",
  "bridge": "auto",
  "start_on_login": true
}
```
(The legacy `"shell"` key, no `"workspace"` key.)

**Run / Verify**
- Restart service.
- Tray Workspace Controller submenu shows **Enable** as the active radio (mapped from the legacy `"shell": "enable"` key).
- Shell auto-spawns on service start (Enable mode behavior).

**Cleanup**
- Either delete `service.json` or rewrite with the new key:
  ```json
  { "workspace": "auto", "bridge": "auto", "start_on_login": true }
  ```

---

### Scenario 7 (stretch) — Forged applicationName attack

Optional. Validates that the brand check is truly gone — a malicious app that sets `XrApplicationInfo.applicationName = "displayxr-shell"` is no longer treated as the workspace controller in qwerty routing.

**Setup**

Modify a copy of `cube_handle_d3d11_win` (in `test_apps\cube_handle_d3d11_win\`):
- Locate the `XrApplicationInfo` initialization (search for `applicationName`).
- Change `strcpy(...applicationName, "cube_handle_d3d11_win")` (or similar) to `strcpy(...applicationName, "displayxr-shell")`.
- Rebuild only that test app: `scripts\build_windows.bat test-apps`.

**Run**
- Service running with shell auto-spawned (Scenario 4 setup).
- Launch the modified cube app inside the workspace.

**Verify**
- The cube app is treated as a regular workspace tenant — qwerty head still applies (since `is_workspace_controller_session` is false because the cube app's PID is not the orchestrator-spawned shell's PID).
- The shell remains the only session that enjoys workspace-controller treatment.
- Inspect the cube's behavior on screen: it should respond to qwerty head input as a normal app would in workspace mode, not as the shell.

**Why this matters**: under Phase 1 / pre-2.0 code, the brand string match would have routed the cube app as the workspace controller, breaking its qwerty head input. Phase 2.0 closes that.

**Cleanup**
- Revert the test-app source change.

---

## Reporting back

After running, post a summary in this format. Be terse — the user reads the diff and the screenshots; the report is for "did it work / did it break".

```
## Phase 2.0 functional test results — <date> on <hardware>

| # | Scenario | Result | Notes |
|---|---|---|---|
| 1 | Bare runtime | PASS / FAIL | … |
| 2 | Shell + no manifest | PASS / FAIL | … |
| 3 | Shell + manifest | PASS / FAIL | … |
| 3b | Malformed schema | PASS / FAIL | … |
| 4 | PID auth happy | PASS / FAIL | … |
| 5 | Manual mode | PASS / FAIL | … |
| 6 | service.json back-compat | PASS / FAIL | … |
| 7 | Forged applicationName (stretch) | PASS / FAIL / SKIP | … |

Build SHA: <git rev-parse HEAD>
Screenshots: <paths or attached files>
Anything surprising: <free text — bugs, UX oddities, log noise, etc.>
```

If anything fails, **don't fix it** — return the failure detail to the main session. Phase 2.0 lives on `feature/shell-brand-separation`; fixes get reviewed and committed there before merging to main.

---

## Capturing tray menu screenshots

PowerShell + PrintWindow against the tray HWND is unreliable for popup menus (they're transient). Easier: use Windows' built-in **Snipping Tool** (`Win+Shift+S`) to grab the tray menu while it's open.

Save into `_package\` (gitignored) and reference in the report. Don't commit the screenshots.

---

## Capturing compositor frames (for Scenarios 4 / 5 / 7)

Use the file-trigger screenshot path documented in `CLAUDE.md`:

```bash
rm -f "/c/Users/SPARKS~1/AppData/Local/Temp/shell_screenshot.png"
touch "/c/Users/SPARKS~1/AppData/Local/Temp/shell_screenshot_trigger"
sleep 3
# Capture lands at C:\Users\SPARKS~1\AppData\Local\Temp\shell_screenshot.png
```

Read the resulting 3840×2160 SBS atlas with the Read tool to verify the compositor output. (Replace `SPARKS~1` with the actual short user-name on the machine.)

---

## Known non-issues

Save you the false-positive:

- clangd on the Mac side reports "unused includes" / "unknown identifiers" for the new `service_workspace_manifest.h`, `windows.h` symbols, etc. These are clangd-on-Mac analyzing Windows-only code without the right `#define`s. **Real CI is green** (Windows MSVC build passed at the SHA being tested).
- `grep -rn '"displayxr-shell"' src/` shows one hit in `targets/shell/main.c:1904`. That's the shell naming **itself** in `XrApplicationInfo` — correct, kept on purpose. After Phase 2.0 there are zero `"displayxr-shell"` literals in any auth or routing context.
- The `service_config.c:41` default value `"displayxr-shell.exe"` is the OEM-overridable workspace_binary path. Not a brand check.

---

## What unblocks once this passes

Phase 2.A (capture client lifecycle, first `XR_EXT_spatial_workspace.h` extension surface, beginning of policy migration out of `comp_d3d11_service.cpp`). The 162 internal `shell_*` mentions classified in `docs/roadmap/spatial-workspace-extensions-phase2-audit.md` start moving in 2.A onwards. Phase 2.0 was the prerequisite — runtime is now a real platform.
