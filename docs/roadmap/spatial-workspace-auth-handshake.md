# Workspace Activation — Auth Handshake (draft)

**Status:** Draft, 2026-04-28. Resolves Open Question #1 from [spatial-workspace-extensions-headers-draft.md](spatial-workspace-extensions-headers-draft.md).

Defines how the runtime decides whether a connecting client is allowed to activate workspace mode. Today the answer is hardcoded brand-matching on `application_name == "displayxr-shell"` — exactly the coupling the workspace-extensions effort is trying to remove.

## The check today

`src/xrt/ipc/server/ipc_server_handler.c:294-300`:

```c
bool is_workspace_controller_session =
    ics->client_state.info.application_name[0] != '\0' &&
    strcmp((const char *)ics->client_state.info.application_name, "displayxr-shell") == 0;
```

Two things wrong with this:
1. **Brand-coupled.** Says "this runtime knows about one specific binary." A third-party workspace controller has to lie about its `XrApplicationInfo::applicationName` to get accepted, which conflicts with OpenXR's expectation that `applicationName` reflects the actual app identity.
2. **No actual auth.** Anything can claim to be `"displayxr-shell"`. A malicious local app could activate workspace mode and arrange windows however it likes. Today this is mitigated by the workspace controller's UX (it self-positions on launch), but the IPC channel itself imposes no check.

## Proposed approach: orchestrator-PID match (with manual-mode fallback)

### Service-managed mode (default — what end-users see)

The service orchestrator spawns the workspace binary by `service.json::workspace_binary`. After Phase 1, `service_orchestrator.c:262` does this:

```c
if (!launch_child(workspace_path, "--service-managed", &s_workspace_pi)) {
    return;
}
```

`s_workspace_pi.dwProcessId` is the spawned PID. The IPC server has access to `s_workspace_pi` through the orchestrator's static state (both run in the service process). When a client connects and calls `workspace_activate`, the server compares the connecting client's PID to `s_workspace_pi.dwProcessId`:

- **Match** → grant workspace role.
- **Mismatch** → return `XR_ERROR_FEATURE_UNSUPPORTED` (or the IPC equivalent `XRT_ERROR_NOT_AUTHORIZED` — see "Error code" below).

The connecting client's PID is already known to the server: on Windows it comes from `GetNamedPipeClientProcessId` on the IPC pipe; on macOS via `peerinfo` on the unix socket; the existing IPC infrastructure surfaces it as `ics->client_state.pid` (or equivalent). Verify in implementation.

### Manual-mode fallback (developer / multi-terminal workflow)

The `--workspace` flag (or legacy `--shell` alias) on `displayxr-service` was historically used in the multi-terminal dev workflow: terminal 1 starts the service, terminals 2/3 launch test apps. In that mode, the service didn't spawn the workspace controller — the user did.

For developer ergonomics, the manual flow keeps working:

- If the service was launched with `--workspace` (or `--shell`), and the orchestrator did NOT spawn a workspace controller, the IPC server falls back to **first-claim wins**: the first client that calls `workspace_activate` gets the role, regardless of PID. Subsequent clients get `XR_ERROR_LIMIT_REACHED`.
- This is a developer-only loosening; production deployments use service-managed mode.

A third option — `application_name` allowlist — is rejected. It re-creates the brand coupling we're trying to remove and doesn't add real authentication.

### Combined decision tree

```
xrActivateSpatialWorkspaceEXT called
        │
        ├── Another workspace already active? ──── yes ──► XR_ERROR_LIMIT_REACHED
        │                                          no
        ▼
┌───────────────────────┐
│ Was orchestrator      │
│ asked to spawn        │   no    First client to call workspace_activate
│ a workspace binary?   ├──────► wins (developer / manual mode).
└────────┬──────────────┘
         │ yes
         ▼
┌────────────────────────┐
│ caller PID ==          │   no
│ s_workspace_pi.PID ?   ├──────► XR_ERROR_FEATURE_UNSUPPORTED
└────────┬───────────────┘
         │ yes
         ▼
   Activate. Workspace session bound to this XrSession.
```

## Implementation sketch

### 1. Expose the spawned PID from orchestrator to IPC server

Add a getter to `service_orchestrator.h`:

```c
//! Returns the PID of the workspace controller spawned by this service,
//! or 0 if the orchestrator has not (yet) spawned one (e.g. AUTO mode
//! before the hotkey, or the service was launched with --workspace
//! manually so the orchestrator stayed out of the way).
unsigned long
service_orchestrator_get_workspace_pid(void);
```

Implementation in `service_orchestrator.c` reads `s_workspace_pi.dwProcessId` (Windows) — guard with the running flag:

```c
unsigned long
service_orchestrator_get_workspace_pid(void)
{
#ifdef XRT_OS_WINDOWS
    if (s_workspace_running) {
        return (unsigned long)s_workspace_pi.dwProcessId;
    }
#endif
    return 0;
}
```

The function returns `unsigned long` (not `DWORD`) so it can live in a public header without dragging in `<windows.h>` — matches the cross-platform `pid_t`-vs-`long` discipline already in `aux/util/`.

### 2. Capture the connecting-client PID

Existing IPC infrastructure already knows the client PID (named-pipe queries on Windows, peerinfo on POSIX). Confirm during implementation that `ics->client_state.pid` (or equivalent) is populated by the time `ipc_handle_workspace_activate` runs. If not, plumb it through — small enough to not need separate design.

### 3. Replace the application_name match in `ipc_handle_workspace_activate`

`src/xrt/ipc/server/ipc_server_handler.c`, replace the activate handler's authorization check with:

```c
xrt_result_t
ipc_handle_workspace_activate(volatile struct ipc_client_state *_ics)
{
    struct ipc_server *s = _ics->server;

    if (s->workspace_mode) {
        // Already active — re-ensure window for relaunch (existing behavior).
        ...
    }

    // Authorization: orchestrator-PID match, with manual-mode fallback.
    unsigned long expected_pid = service_orchestrator_get_workspace_pid();
    unsigned long caller_pid   = (unsigned long)_ics->client_state.pid;

    if (expected_pid != 0 && caller_pid != expected_pid) {
        IPC_WARN(s, "workspace_activate: PID mismatch (caller=%lu, expected=%lu) — denied",
                 caller_pid, expected_pid);
        return XRT_ERROR_NOT_AUTHORIZED; // see "Error code" below
    }

    // expected_pid == 0 → manual mode, first-claim wins (which it does
    // implicitly since we hit this code only on a first activate call).

    s->workspace_mode = true;
    ...
}
```

### 4. Replace the `application_name` match in `ipc_handle_session_create` qwerty-routing logic

`ipc_server_handler.c:294` currently uses the application_name match for a different purpose: deciding whether to route qwerty input to a session (workspace controller doesn't get qwerty input, app sessions do). Same fix:

```c
unsigned long workspace_pid = 0;
if (s->workspace_mode) {
    workspace_pid = service_orchestrator_get_workspace_pid();
}
bool is_workspace_controller_session =
    workspace_pid != 0 &&
    (unsigned long)ics->client_state.pid == workspace_pid;
```

This finally removes the literal `"displayxr-shell"` string from the runtime — the last brand-coupling point identified in Phase 1.

### 5. Tell the workspace controller it was authorized

Adds nothing on the controller side — `xrActivateSpatialWorkspaceEXT` returning `XR_SUCCESS` is the only signal needed. The controller doesn't need to know whether the runtime checked PID or application_name.

## Error code

OpenXR's standard error codes don't include "not authorized as workspace controller" cleanly. Options:

- **`XR_ERROR_FEATURE_UNSUPPORTED`** — semi-honest: from the caller's POV, the *capability* of being a workspace controller is unsupported (for them). Re-uses an existing code.
- **`XR_ERROR_LIMIT_REACHED`** — appropriate when another workspace is already active; not appropriate here (no resource limit issue).
- **`XR_ERROR_VALIDATION_FAILURE`** — wrong; the input is well-formed.
- **A new vendor error.** Cleanest; `XR_ERROR_WORKSPACE_NOT_AUTHORIZED_EXT` reserved by the spatial_workspace extension.

Recommend the new vendor error code, defined alongside the extension. Maps internally to `XRT_ERROR_NOT_AUTHORIZED` (an `xrt_result_t` value the runtime can use; the IPC layer translates).

## Edge cases

**Workspace controller crashes and respawns.** Orchestrator's watchdog thread (`workspace_watch_thread_func`) restarts the controller in Enable mode. The new process gets a new PID; `s_workspace_pi.dwProcessId` updates. The old session was already gone (socket closed when process died); the new session activates with the new PID. No race — single writer (the watchdog) per spawn cycle.

**Manual `displayxr-service --workspace` mode.** The orchestrator stays out of the spawning loop (it's a developer-only legacy flow). `service_orchestrator_get_workspace_pid()` returns 0; the activate handler treats it as first-claim wins. If a user wants to lock this down, they should not pass `--workspace` and let the orchestrator manage it.

**Configuration drift.** Suppose `service.json::workspace_binary` is changed between service start and a controller process connecting — the running orchestrator already has a `s_workspace_pi.dwProcessId` from the *original* binary. New binary's PID won't match. Resolved by: orchestrator re-reads config on `service_orchestrator_apply_config`, terminates the old process, spawns the new one — the new PID becomes authoritative. The IPC server queries the orchestrator at activate time, so it always sees current state.

**PID reuse.** PIDs are recycled by the OS. An attacker who can predict reuse could in theory connect with a forged PID. Mitigation: the orchestrator holds the spawned process's HANDLE (`s_workspace_pi.hProcess`) for its lifetime, which prevents the OS from immediately reusing the PID. As long as the watchdog is alive and holds the handle, the PID is uniquely ours. (This is a Win32-specific guarantee; macOS port uses an analogous mechanism via process handle.)

**Service-restart race.** If the service restarts but the workspace controller doesn't, the new service has no `s_workspace_pi.dwProcessId`. The still-alive controller calls `xrActivateSpatialWorkspaceEXT` from a previous session, gets PID-mismatch (or zero-expected-PID = manual mode = first-claim wins). Whether the controller is allowed depends on which mode the service started in. Acceptable: workspace controller crashes were already a recovery scenario; workspace will need to be redirected anyway.

**Multi-user.** Out of scope. The service is single-user-per-session. If multiple users on the same machine each run their own service, each gets their own orchestrator + workspace controller pair, each authoritative within its session.

## Migration path

Lands as a small Phase 2 sub-step (call it "Phase 2.0 — auth handshake"). Three-commit sequence:

1. **Add `service_orchestrator_get_workspace_pid()` getter.** Pure addition, no behavior change. Covered by header + impl.
2. **Wire PID check into `ipc_handle_workspace_activate`.** Behavior change: workspace activation now requires PID match (or manual mode). The legacy `application_name == "displayxr-shell"` check stays in place during this commit as a fallback so existing setups don't break.
3. **Remove the application_name fallback** + remove the literal `"displayxr-shell"` from `ipc_server_handler.c:296`. Final brand-decoupling commit. Tested with: orchestrator-spawned shell still works, manual `--workspace` flow still works, a forged `application_name="displayxr-shell"` from a different PID is now rejected.

After step 3, the runtime contains zero references to the shell binary by name (the only remaining `"displayxr-shell.exe"` is the *default value* of `cfg.workspace_binary`, which is correct: third parties override it via `service.json`).

## Out of scope

- **Code-signing / Authenticode verification.** Nice-to-have for a future tightening pass, not needed for Phase 2. The orchestrator-PID check already prevents arbitrary local processes from claiming workspace mode in service-managed deployments — that's the threat model we care about for Phase 2.
- **Cross-machine workspace controllers.** All workspace concerns are local to one machine.
- **Switching between workspace controllers at runtime.** Phase 2 keeps the "one workspace at a time" model. A controller change requires deactivate + activate.
