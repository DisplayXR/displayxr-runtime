# MCP extraction — Windows validation procedure

This procedure validates the two coordinated MCP extraction PRs against a real Leia SR display. CI confirms the code compiles and links on every target platform; this procedure confirms it actually *works*.

**PRs covered:**

- [`DisplayXR/displayxr-runtime#200`](https://github.com/DisplayXR/displayxr-runtime/pull/200) — extracts MCP framework to `displayxr-mcp` (v0.2.3), strips Phase B from the runtime, deletes the legacy in-tree `mcp_adapter` target.
- [`DisplayXR/displayxr-shell-pvt#4`](https://github.com/DisplayXR/displayxr-shell-pvt/pull/4) — hosts the workspace MCP server inside `displayxr-shell.exe`, wraps eight `XR_EXT_spatial_workspace` PFNs as agent-callable tools.

The two PRs do not have to land in lockstep, but **runtime PR must merge first** so the shell's `FetchContent_Declare(displayxr_mcp v0.2.3)` resolves against the same pin the runtime uses. Phase 2 below validates the runtime PR alone; Phase 3 validates both together.

## Prerequisites

- Windows 10 / 11 with a Leia SR display attached and the SR SDK installed (`LEIASR_SDKROOT` env var set).
- VS 2022 (C++ workload), Vulkan SDK, Ninja, GitHub CLI (`gh`).
- A bash shell available — Git Bash (`C:\Program Files\Git\bin\bash.exe`) is fine; the test scripts under `tests/mcp/` are bash.
- Both PR branches checked out side-by-side (recommended: under `~/GitHub/`).

## Phase 1 — build both PRs locally

### Runtime

```cmd
cd %USERPROFILE%\GitHub\displayxr-runtime
gh pr checkout 200
scripts\build_windows.bat all
```

**Expected outputs:**

- `_package\bin\displayxr-service.exe`
- `_package\bin\openxr_displayxr.dll`
- `test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe`
- `_package\run_cube_handle_d3d11_win.bat`
- **The MCP adapter is now at `build\_deps\displayxr_mcp-build\displayxr-mcp.exe`** (pulled in via FetchContent — was at `build\src\xrt\targets\mcp_adapter\` before this PR).

If the build fails on `pthread.h` not found, `cJSON` not found, or any complaint about `displayxr_mcp`, capture the full output and stop — that's a foundation issue, not a Phase A/B problem.

### Shell

```cmd
cd %USERPROFILE%\GitHub\displayxr-shell-pvt
gh pr checkout 4
:: Build via the shell's own build script — see displayxr-shell-pvt/CLAUDE.md
:: (typically scripts\build_windows.bat or similar)
```

**Expected outputs:**

- `_package\bin\displayxr-shell.exe`

If the shell build fails on missing `displayxr_mcp` headers or symbols, the FetchContent didn't resolve — capture the CMake configure output.

## Phase 2 — Phase A regression check (runtime PR alone)

Verifies the runtime's renamed Phase A path (`oxr_mcp_tools.c` calling into `displayxr_mcp::mcp` via FetchContent) still serves the same set of introspection tools as before.

### 2.1 Launch a cube app with MCP enabled

```cmd
:: terminal 1
set DISPLAYXR_MCP=1
_package\run_cube_handle_d3d11_win.bat
```

The cube window should appear. **In the cube app's stderr/console**, look for a line near the top:

```
[mcp] [mcp-transport] listening on \\.\pipe\displayxr-mcp-<pid>
[mcp] [mcp] server started (pid=<pid>)
```

If those lines are missing, MCP is not enabled in the runtime — verify `DISPLAYXR_MCP=1` is set in the same `cmd` session that launched the run script.

### 2.2 Run Phase A test scripts

```bash
# terminal 2 — Git Bash, from displayxr-runtime repo root
bash tests/mcp/test_phase_a_win.sh
```

`test_phase_a_win.sh` auto-launches `cube_handle_d3d11_win.exe`, waits
for the per-PID MCP server to bind, drives `initialize` + `tools/list`
+ several `tools/call` round-trips, and tears the cube down on exit.
You can skip Phase 2.1 (manual cube launch) when running this script —
it manages the cube itself.

The other `test_*.sh` scripts under `tests/mcp/` (`test_handshake.sh`,
`test_core_tools.sh`, `test_diff_projection.sh`, `test_capture_frame.sh`)
are macOS-only — they target `cube_handle_metal_macos` and Unix domain
sockets and skip cleanly on Windows. Run them on the macOS validation pass.

**PASS criteria:**

- `test_phase_a_win.sh` prints `PASS` lines and exits 0.
- No crashes / access violations in the cube app's console during or after the test.
- `tools/list` returns at least: `echo`, `tail_log`, `list_sessions`, `get_display_info`, `get_runtime_metrics`, `get_kooima_params`, `get_submitted_projection`, `diff_projection`, `capture_frame`.

**FAIL — capture for triage:**

- Full stderr from the cube app from launch to crash.
- Full stderr from the failing test script.
- Output of `dir \\.\pipe\` filtered for `displayxr-mcp-*` to confirm the socket actually bound.

### 2.3 Verify the runtime no longer hosts an MCP service endpoint

This PR strips `mcp_server_maybe_start_named("service")` from `targets/service/main.c`. Confirm:

```cmd
:: stop everything from Phase 2.1 first, then:
set DISPLAYXR_MCP=1
_package\bin\displayxr-service.exe
:: in another terminal:
build\_deps\displayxr_mcp-build\displayxr-mcp.exe --target service
```

**Expected:** the adapter prints `displayxr-mcp: cannot connect to service` and exits 1. (Before this PR it would have connected.)

If it still connects, the service is still hosting MCP — block merge.

## Phase 3 — Phase B validation (both PRs together)

This is the higher-risk path: `shell_mcp_tools.c` is net-new code that has never been exercised against a live OpenXR session. Every Phase B tool here is a first-run.

### 3.1 Launch the shell with two apps and MCP enabled

```cmd
set DISPLAYXR_MCP=1
_package\bin\displayxr-shell.exe ^
    test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe ^
    test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

The shell auto-starts the service and launches the two cube apps in workspace mode. **In the shell's stderr**, look for:

```
[mcp] [mcp-transport] listening on \\.\pipe\displayxr-mcp-shell
[mcp] [mcp] server started (role=shell pid=<shell-pid>)
```

If those lines are missing, MCP is disabled in the shell. Same fix as 2.1 — `DISPLAYXR_MCP=1` must be in the launching `cmd`.

If the shell crashes on startup with `DISPLAYXR_MCP=1` but starts cleanly without it, that's a Phase B regression — capture stderr and stop.

### 3.2 The smoke check — `list_windows`

```bash
# terminal 2 — Git Bash, from displayxr-shell-pvt repo root
bash tests/mcp/test_list_windows.sh
```

**PASS criteria:**

- Returns ≥ 2 windows (`EXPECTED_WINDOWS=2` by default).
- Each window has:
  - `id` — non-zero integer
  - `name` — non-empty string matching the cube app's application_name
  - `pid` — matches one of the cube processes (verify with `tasklist | findstr cube_handle`)
  - `pose` — object with `x`, `y`, `z`, `qx`, `qy`, `qz`, `qw`, `width_m`, `height_m` all numeric, not all zero
  - `focused` — boolean
  - `visible` — boolean
- Test exits 0.

**FAIL diagnosis flowchart:**

| Symptom | Likely cause |
|---|---|
| `displayxr-mcp: cannot connect to shell` | shell didn't bind the socket — check Phase 3.1 stderr |
| Returns empty array `[]` | `xrEnumerateWorkspaceClientsEXT` returning 0 — cube apps didn't connect to workspace, or the PFN call from MCP thread is racing with the UI thread |
| Returns array but `name` is empty / garbage | `XrWorkspaceClientInfoEXT` field mapping broken in `shell_mcp_tools.c` |
| Returns array but `pose` is `null` | `xrGetWorkspaceClientWindowPoseEXT` returned non-success — log capture from shell stderr will show why |
| Test crashes the shell | thread-safety bug calling PFN from MCP thread — block merge |

### 3.3 Write tools

After list_windows passes:

```bash
bash tests/mcp/test_set_window_pose.sh
```

**PASS criteria:**

- Test calls `set_window_pose` with a known id + new pose, then calls `get_window_pose`, and the returned values match (within float epsilon).
- The visible cube window for that id moves to roughly the new pose (sanity check by eye).
- An audit entry appears in `%APPDATA%\DisplayXR\mcp-audit.log` containing the line `set_window_pose,<client_id>,<args_hash>`.

```bash
bash tests/mcp/test_workspace_roundtrip.sh
```

**PASS criteria:**

- `save_workspace` writes a JSON file under `%APPDATA%\DisplayXR\workspaces\<name>.json`.
- File contains `version: 1` and a `windows` array with pose data.
- After moving windows manually (or via `set_window_pose`), `load_workspace` restores them.
- Test exits 0.

```bash
bash tests/mcp/test_audit_log.sh
```

**PASS criteria:**

- `set_focus` (the trigger tool — was `apply_layout_preset` pre-Phase-2.G) appends a line to the audit log.
- `list_windows` (a read tool) does **not** add an audit entry.
- Test exits 0.

### 3.4 Optional: `request_client_exit`

This one is destructive — it asks an app to exit. Run last.

```bash
# manual call via the adapter
build/_deps/displayxr_mcp-build/displayxr-mcp.exe --target shell
# in the resulting stdio session, send:
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"request_client_exit","arguments":{"id":<id-of-one-cube>}}}
```

**Expected:** the addressed cube app exits cleanly within a few seconds. The other cube continues running. `list_windows` afterwards returns 1 window.

## Phase 4 — voice-control regression (optional, deferred)

Only relevant if the `feature/voice-control` branch on `displayxr-shell-pvt` is being kept alive. That branch's `voice_cli.py` currently spawns `displayxr-mcp.exe --target service`. After both PRs land, the line must change to `--target shell` (or the planned env-var switch `DISPLAYXR_VOICE_REAL_MCP=1` must select the shell role).

Test:

1. Launch shell as in Phase 3.1.
2. Start the voice CLI (per `displayxr-shell-pvt/feature/voice-control/src/xrt/targets/voice/README.md`).
3. Issue a voice command equivalent to `list_windows`.
4. Verify the agent reports the same windows as `test_list_windows.sh` did.

If the voice CLI exits immediately with `cannot connect to service`, the migration env var hasn't been wired through. Not blocking for the MCP extraction PRs themselves — the voice branch will rebase and pick up `--target shell` in its own follow-up.

## Reporting back

Comment on **runtime PR #200** with one of these states (the shell-pvt PR rolls up under it for tracking):

| State | Comment template |
|---|---|
| ✅ All green | "Validated on Windows + Leia SR. Phase A 2.1–2.3 + Phase B 3.1–3.3 all pass. Ready to merge." |
| ⚠️ Phase A pass, Phase B fail | "Phase A green; Phase B failed at `<test name>` — paste stderr below. Runtime PR is safe to merge alone, but block shell-pvt PR until investigated." |
| ❌ Phase A fail | "Phase A regression at `<test name>`. Paste stderr. Block both PRs." |

Always include:

- First ~50 lines of stderr from `displayxr-shell.exe` and from each `cube_handle_d3d11_win.exe` (so we can confirm whether the MCP server bound its socket).
- For any failing tool call, the raw JSON response the adapter printed.
- Output of `dir \\.\pipe\ | findstr displayxr-mcp` after step 3.1 — confirms the shell socket exists.

## Quick-reference command list (for an automation agent)

```cmd
:: setup (once)
set LEIASR_SDKROOT=<path>
set DISPLAYXR_MCP=1

:: Phase 1
cd %USERPROFILE%\GitHub\displayxr-runtime && gh pr checkout 200 && scripts\build_windows.bat all
cd %USERPROFILE%\GitHub\displayxr-shell-pvt && gh pr checkout 4 && <shell build cmd>

:: Phase 2 (Phase A — runtime alone)
:: terminal 1
cd %USERPROFILE%\GitHub\displayxr-runtime && _package\run_cube_handle_d3d11_win.bat
:: terminal 2
cd %USERPROFILE%\GitHub\displayxr-runtime && bash tests/mcp/test_handshake.sh && bash tests/mcp/test_core_tools.sh && bash tests/mcp/test_diff_projection.sh

:: Phase 2.3 (negative — service should NOT host MCP)
_package\bin\displayxr-service.exe
build\_deps\displayxr_mcp-build\displayxr-mcp.exe --target service
:: expect: "cannot connect to service" + exit 1

:: Phase 3 (Phase B — both PRs together)
:: terminal 1
cd %USERPROFILE%\GitHub\displayxr-runtime && _package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
:: terminal 2
cd %USERPROFILE%\GitHub\displayxr-shell-pvt && bash tests/mcp/test_list_windows.sh && bash tests/mcp/test_set_window_pose.sh && bash tests/mcp/test_workspace_roundtrip.sh && bash tests/mcp/test_audit_log.sh
```

Total runtime: ~10–15 min on a warm build, plus build time on the first run.
