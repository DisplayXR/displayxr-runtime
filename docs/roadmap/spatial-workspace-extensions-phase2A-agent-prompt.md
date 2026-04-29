# Phase 2.A Agent Prompt — Workspace Extension Scaffolding + Capture Client Migration

Self-contained prompt for a fresh agent session implementing Phase 2.A of the workspace-extensions effort. Drop into a new session as the user message after `/clear`. The agent has no memory of the design conversations — this prompt assumes nothing.

---

## What Phase 2.A is for

You're picking up Phase 2.A of the **workspace-extensions migration**. Phase 2.0 (just-merged: brand separation, controller manifest, PID-match auth, plus a `service_orchestrator.c` double-close race fix) made the runtime a standalone platform. Phase 2.A starts the **policy migration out of `comp_d3d11_service.cpp`** by standing up the first piece of public extension surface — `XR_EXT_spatial_workspace` — and wiring its lowest-risk function set (capture-client lifecycle) end-to-end through it.

Phase 2.A is deliberately the smallest useful cut. The scaffolding it builds is reused by every subsequent migration (2.B launcher, 2.C chrome, 2.D input, 2.F hit-test, etc.). Validating the dispatch path with the simplest function set first means later migrations only need to bring code, not infrastructure.

## Read these in order before touching code

1. `docs/roadmap/spatial-workspace-extensions-plan.md` — master plan, three-phase roadmap. You're inside Phase 2.
2. `docs/roadmap/spatial-workspace-extensions-headers-draft.md` — the API surface sketch. Phase 2.A implements a **subset only** (see "What ships in Phase 2.A" below). Treat the rest of the draft as spec for future sub-phases — do not implement unprompted.
3. `docs/roadmap/spatial-workspace-extensions-phase2-audit.md` — labels every remaining `shell_*` mention in `comp_d3d11_service.cpp` as mechanism / policy / gone. Phase 2.A only touches the **capture cluster** (lines 11620-11641); do not migrate anything else.
4. `docs/adr/ADR-016-workspace-controllers-own-tray-surface-and-lifecycle.md` — captures the cooperative-shutdown decision. Phase 2.A does not implement it (that's deferred to a later sub-phase that adds tray-menu extensions), but the ADR explains why some hooks you'll see in the orchestrator look provisional.
5. `docs/roadmap/workspace-runtime-contract.md` — IPC contract. The extension functions you wire wrap RPCs already named in this contract; you are not redesigning them.

Also pull `MEMORY.md` and skim `feedback_use_build_windows_bat.md`, `reference_local_build_deps.md`, and `feedback_test_before_ci.md`.

## Branch + prerequisites

- Start from `main` after Phase 2.0 has merged (`feature/shell-brand-separation` → `main`).
- New branch: `feature/workspace-extension-scaffolding-2a`.
- Confirm `main` includes the Phase 2.0 commits — `git log --oneline main | grep -E 'controller|workspace_activate|orchestrator_get_workspace_pid|double-close'` should return ~6 hits.

## What ships in Phase 2.A

### Subset of `XR_EXT_spatial_workspace` to implement

Only the capture-related surface plus the lifecycle functions capture depends on. No window-pose, no hit-test, no frame-capture, no app-launcher, no enumerate-clients (yet).

| Function | Wraps existing IPC RPC | Notes |
|---|---|---|
| `xrActivateSpatialWorkspaceEXT(session)` | `workspace_activate` | Auth check already in place from Phase 2.0 |
| `xrDeactivateSpatialWorkspaceEXT(session)` | `workspace_deactivate` | |
| `xrGetSpatialWorkspaceStateEXT(session, *active)` | `workspace_get_state` | Read-only |
| `xrAddWorkspaceCaptureClientEXT(session, hwnd, name, *clientId)` | `workspace_add_capture_client` | Windows HWND only — macOS chained struct is post-2.A |
| `xrRemoveWorkspaceCaptureClientEXT(session, clientId)` | `workspace_remove_capture_client` | |

Plus the type definitions those functions reference:
- `XrWorkspaceClientId` (uint32_t typedef)
- `XR_NULL_WORKSPACE_CLIENT_ID`
- `XrWorkspaceClientTypeEXT` enum (used now for capture-vs-OpenXR; full enumerate-clients work is deferred)
- The `XR_EXT_SPATIAL_WORKSPACE_EXTENSION_NAME` literal + `_SPEC_VERSION 1`

### Log-string rename (the trivial cleanup)

`comp_d3d11_service.cpp` lines **11620-11641** — capture-client lifecycle log strings. Rename `shell` → `workspace` in the log prefixes only. Code logic untouched.

### What you do NOT touch

- The other 156 `shell_*` mentions in `comp_d3d11_service.cpp`. The audit labels them; later sub-phases handle them.
- `comp_d3d11_window.{cpp,h}` — `shell_mode_active`, `set_shell_mode_active`, `set_shell_dp`, `shell_input_event`, `SHELL_INPUT_RING_SIZE` all stay. Phase 2.D / 2.E.
- `service_orchestrator.c` — Phase 2.0 finished its surface. The cooperative-shutdown protocol from ADR-016 is a later phase.
- `src/xrt/targets/shell/main.c` — the DisplayXR Shell keeps using internal IPC for now. The extension is for **future third-party controllers**; Shell migration to extensions is deferred (and out of scope for the first non-shell consumer to drive validation, see "Validation" below).
- `proto.json` / `proto.py` — IPC wire format unchanged; you wrap existing RPCs.
- `webxr_bridge/main.cpp` — its "shell mode" terminology is an unrelated feature name. Skip.
- `XR_EXT_app_launcher.h` — separate header, separate phase (2.B).
- All other functions in the headers-draft (window pose, hit-test, frame capture, enumerate clients, eye-tracking-mode forwarding) — those land in their own sub-phases.

## Recommended commit sequence

Six commits. Keep each reviewable in isolation; do not bundle.

### Commit 1 — `XR_EXT_spatial_workspace.h` header (pure addition)

New file: `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h`.

Copy the structure of `src/external/openxr_includes/openxr/XR_EXT_display_info.h` for the BSD-style header, include guards, `extern "C"` block, `XR_NO_PROTOTYPES` gate, and the `EXTENSION_NAME` / `SPEC_VERSION` defines. Then add the **Phase 2.A subset only** from `spatial-workspace-extensions-headers-draft.md`:

- Type values for the capture-related struct types (use the headers-draft's provisional `1000999100`-range values; coordinate with Khronos registry later).
- `XrWorkspaceClientId` typedef + `XR_NULL_WORKSPACE_CLIENT_ID`.
- `XrWorkspaceClientTypeEXT` enum with the two values defined.
- The five PFN typedefs and prototypes for the functions in the table above.

Acceptance: `_package/` headers ship the new file (CI's `publish-extensions.yml` will pick it up automatically). Header compiles standalone (`gcc -E` or MSVC `/EP` does not error).

### Commit 2 — Extension registration

Touch: `src/xrt/state_trackers/oxr/oxr_extension_support.h`.

Mirror the existing `XR_EXT_display_info` block (search for `OXR_EXTENSION_SUPPORT_EXT_display_info`):

```c
/*
 * XR_EXT_spatial_workspace
 */
#if defined(XR_EXT_spatial_workspace)
#define OXR_HAVE_EXT_spatial_workspace
#define OXR_EXTENSION_SUPPORT_EXT_spatial_workspace(_) \
    _(EXT_spatial_workspace, EXT_SPATIAL_WORKSPACE)
#else
#define OXR_EXTENSION_SUPPORT_EXT_spatial_workspace(_)
#endif
```

Add to the master support list (the macro at line ~1039):

```c
    OXR_EXTENSION_SUPPORT_EXT_spatial_workspace(_) \
```

Acceptance: `xrEnumerateInstanceExtensionProperties` lists `XR_EXT_spatial_workspace`. `xrCreateInstance` with that name in `enabledExtensionNames` succeeds.

### Commit 3 — State-tracker implementation file (new)

New file: `src/xrt/state_trackers/oxr/oxr_workspace.c`. Add to `oxr_state_tracker.dir` in `src/xrt/state_trackers/oxr/CMakeLists.txt`.

Implement the five functions. Each is a thin wrapper that:

1. Validates the session is current and the extension is enabled (`OXR_VERIFY_*` macros — see `oxr_api_session.c` for the pattern).
2. Translates parameters to the IPC RPC's wire format.
3. Calls the existing IPC RPC via `oxr_session->sys->...->ipc_*`.
4. Translates the IPC return back to `XrResult`.

Reference for IPC call patterns: `oxr_api_passthrough.c` does similar wrapping for `XR_FB_passthrough`. The capture-client RPCs already have a C entry in `ipc_client/` — search `ipc_call_workspace_add_capture_client`.

Make the activate/deactivate functions return:
- `XR_SUCCESS` on success
- `XR_ERROR_LIMIT_REACHED` if another workspace is already active (Phase 2.0 PID-auth path returns a distinct error code today; map it)
- `XR_ERROR_FEATURE_UNSUPPORTED` if not authorized (Phase 2.0 returns this; pass through)

Acceptance: function bodies compile, link, and return `XR_SUCCESS` against the in-process Shell when called from a test client. Failure path returns the documented error.

### Commit 4 — Dispatch wiring

Touch: `src/xrt/state_trackers/oxr/oxr_api_funcs.h`.

Add a block near the existing `OXR_HAVE_EXT_display_info` gate:

```c
#ifdef OXR_HAVE_EXT_spatial_workspace
XRAPI_ATTR XrResult XRAPI_CALL oxr_xrActivateSpatialWorkspaceEXT(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDeactivateSpatialWorkspaceEXT(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpatialWorkspaceStateEXT(XrSession session, XrBool32 *active);
XRAPI_ATTR XrResult XRAPI_CALL oxr_xrAddWorkspaceCaptureClientEXT(
    XrSession, uint64_t, const char *, XrWorkspaceClientId *);
XRAPI_ATTR XrResult XRAPI_CALL oxr_xrRemoveWorkspaceCaptureClientEXT(
    XrSession, XrWorkspaceClientId);
#endif
```

Then add the dispatch table entries in `oxr_api_negotiate.c` (search for the table that maps function names to function pointers — typically `xrGetInstanceProcAddr` body or a generated lookup table; Phase 2.0's existing extensions show the pattern).

Acceptance: `xrGetInstanceProcAddr(instance, "xrAddWorkspaceCaptureClientEXT", ...)` returns a non-null function pointer when the extension is enabled, NULL when it isn't. Calling the function pointer dispatches to your `oxr_xrAdd...` from Commit 3.

### Commit 5 — Log-string rename in `comp_d3d11_service.cpp`

Touch lines 11620-11641 only. Pure cosmetic — rename `Shell` / `shell` to `Workspace` / `workspace` in log prefixes for the capture-client cluster. Use `git diff -W` to confirm only that range moves; nothing functional.

If clang-format wants to reflow the area, allow it (run `git clang-format` before the commit).

### Commit 6 — Validation test

Add a small standalone OpenXR client that exercises the new extension. Either:
- Extend `tests/tests_comp_client_d3d11.cpp` with a workspace-mode test, or
- Add a new test app `test_apps/workspace_minimal_d3d11_win/` that:
  1. `xrCreateInstance` with `XR_EXT_spatial_workspace`
  2. `xrCreateSession` with D3D11 binding
  3. Call `xrActivateSpatialWorkspaceEXT` (expect `XR_SUCCESS` if launched under the orchestrator, `XR_ERROR_FEATURE_UNSUPPORTED` if launched standalone — both paths are valid test signals)
  4. If active, call `xrAddWorkspaceCaptureClientEXT` with a dummy HWND (Notepad's HWND works for ad-hoc), assert non-zero `clientId` returned, then `xrRemoveWorkspaceCaptureClientEXT`
  5. `xrDeactivateSpatialWorkspaceEXT`

The test app naming follows the pattern in CLAUDE.md (`workspace_minimal_d3d11_win`). Wire it up in `test_apps/CMakeLists.txt` and `_package/`.

Acceptance: test app builds + runs against the patched runtime. Stub builds (build-mingw-check.sh `aux_util workspace_minimal_d3d11_win`) compile clean.

## Acceptance criteria for the whole phase

- ✅ `_package/` ships `XR_EXT_spatial_workspace.h`. The `publish-extensions.yml` workflow picks it up next push.
- ✅ A test client compiled against the new header can: enumerate the extension, create instance with it enabled, activate workspace mode (PID-auth path), add + remove a capture client, deactivate.
- ✅ The first-party DisplayXR Shell continues to work unchanged. (It still uses internal IPC; that path is parallel to the extension path until a later sub-phase migrates it.)
- ✅ Capture-cluster log strings in `comp_d3d11_service.cpp` say "workspace" not "shell".
- ✅ Windows MSVC CI green. macOS CI green (the new header is platform-neutral; macOS test app is a Phase 2.A.macOS follow-up if needed).
- ✅ `build-mingw-check.sh aux_util` green for the new state-tracker file.
- ✅ Branch is one or two PRs against `main` — six commits total, each reviewable.

## Hand-off notes

- **Don't auto-commit individual sub-steps without testing.** The user's preference (per `feedback_test_before_ci.md`): build locally, smoke-test, then commit. Use `scripts\build_windows.bat build` for incremental rebuilds (per `feedback_use_build_windows_bat.md`). Don't push until the full sequence is locally green.
- **`/ci-monitor` is for after the user has tested and approved.** The user will run it themselves or invite you to.
- **The PRD layer order is being preserved.** State tracker → IPC → compositor; the new extension code lives in the state tracker; it does not bypass IPC and does not call compositor-private headers directly.
- **If you find an `OXR_VERIFY_SESSION_AND_INIT_LOG` (or whatever the project calls it) macro doesn't exist for workspace context, follow `oxr_api_passthrough.c`'s recipe verbatim** — that extension is the closest existing template. Don't invent new validation macros.
- **Naming consistency check before commit:** `grep -rn 'Shell\|shell' src/xrt/state_trackers/oxr/oxr_workspace.c` should return zero. The new file is brand-neutral by construction.

## What unblocks once Phase 2.A passes

- **Phase 2.B (launcher tile registry).** First *policy* migration. Pushes `xrAddLauncherAppEXT` / `xrPollLauncherClickEXT` / `xrSetLauncherRunningTileMaskEXT` and deletes the runtime's `registered_apps` array.
- **Phase 2.F (hit-test).** Lowest-risk policy-adjacent migration. Promotes the existing internal `workspace_raycast_hit_test` to `xrWorkspaceHitTestEXT`.
- The **window-pose** and **frame-capture** functions become next priority once Phase 2.B validates the launcher extension.

After Phase 2.A merges, the next prompt (`spatial-workspace-extensions-phase2B-agent-prompt.md`) writes itself: same shape, swapping in launcher functions and deleting the policy that moved out.
