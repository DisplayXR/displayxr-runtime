# Phase 2.B Agent Prompt — Launcher Tile Registry Extension (`XR_EXT_app_launcher`)

Self-contained prompt for a fresh agent session implementing Phase 2.B of the workspace-extensions effort. Drop into a new session as the user message after `/clear`. The agent has no memory of the prior design conversations — this prompt assumes nothing.

---

## What Phase 2.B is for

You're picking up Phase 2.B of the **workspace-extensions migration**. Phase 2.A (just-merged) shipped `XR_EXT_spatial_workspace` with the lifecycle + capture-client surface, validated the dispatch path end-to-end, and established the IPC-bridge pattern (`comp_ipc_client_compositor_workspace_*` accessors in `ipc_client_compositor.c`) that st_oxr uses to reach IPC without pulling the ipc_client include path.

Phase 2.B stands up the **second** public extension — `XR_EXT_app_launcher` — and wraps the five launcher RPCs that already ship over IPC (`launcher_clear_apps`, `launcher_add_app`, `launcher_set_visible`, `launcher_poll_click`, `launcher_set_running_tile_mask`). The five become five OpenXR functions on a new extension. The runtime keeps its tile-grid renderer (mechanism: how to draw N tiles); the workspace controller keeps owning the tile *registry* (policy: which N tiles).

This is the **first policy migration** in the strict sense: post-2.B, a third-party workspace controller can stand up a launcher without ever calling internal `ipc_call_launcher_*` functions. The first-party DisplayXR Shell continues to use internal IPC — its migration to extensions is deferred (parallel path stays).

## Read these in order before touching code

1. `docs/roadmap/spatial-workspace-extensions-plan.md` — three-phase master plan.
2. `docs/roadmap/spatial-workspace-extensions-headers-draft.md` — full API surface. Phase 2.B implements the **five-function `XR_EXT_app_launcher` subset** (lines 323-437 of the headers-draft). The `XR_EXT_spatial_workspace` header is already shipping; do not re-touch it.
3. `docs/roadmap/spatial-workspace-extensions-phase2-audit.md` — labels every remaining `shell_*` mention in `comp_d3d11_service.cpp`. Phase 2.B touches the **launcher cluster** at lines 327-367 (state-comment headers), 8198-8484 (rendering — log strings only), and 11960-12118 (the five service functions — log strings). Do not migrate any other cluster.
4. `docs/roadmap/spatial-workspace-extensions-phase2A-agent-prompt.md` — the prompt that drove Phase 2.A. Read for the established pattern; Phase 2.B follows the same six-commit shape.
5. `git log --oneline feature/workspace-extensions-2A | head -10` — the six Phase 2.A commits. Skim the diffs of `oxr_workspace.c`, `XR_EXT_spatial_workspace.h`, and `ipc_client_compositor.c` — Phase 2.B's deliverables are structurally identical with launcher functions substituted in.

Also pull `MEMORY.md` and skim `feedback_use_build_windows_bat.md`, `reference_local_build_deps.md`, `feedback_test_before_ci.md`, `feedback_dll_version_mismatch.md`.

## Branch + prerequisites

- Start from `main` after Phase 2.A has merged.
- New branch: `feature/workspace-extensions-2B`.
- Confirm `main` includes the Phase 2.A commits — `git log --oneline main | grep -E 'spatial_workspace|workspace_minimal|capture-client logs'` should return ~6 hits.

## What ships in Phase 2.B

### Subset of `XR_EXT_app_launcher` to implement

All five launcher RPCs become OpenXR functions plus the supporting types. The full surface from the headers-draft is small enough to land in one phase.

| Function | Wraps existing IPC RPC | Notes |
|---|---|---|
| `xrClearLauncherAppsEXT(session)` | `launcher_clear_apps` | Empty the runtime's launcher_apps[] cache. |
| `xrAddLauncherAppEXT(session, *info)` | `launcher_add_app` | One tile per call. Translate `XrLauncherAppInfoEXT` → `struct ipc_launcher_app`. See "name+path mapping" below. |
| `xrSetLauncherVisibleEXT(session, XrBool32)` | `launcher_set_visible` | |
| `xrPollLauncherClickEXT(session, *appIndex)` | `launcher_poll_click` | Returns `int32_t` (signed; -1 = no click; special negative values for special-action tiles — see below). |
| `xrSetLauncherRunningTileMaskEXT(session, mask)` | `launcher_set_running_tile_mask` | |

Plus the type definitions those functions reference:
- `XrLauncherAppInfoEXT` struct (XR_TYPE_LAUNCHER_APP_INFO_EXT = 1000999110)
- `XR_LAUNCHER_MAX_APPS_EXT` and `XR_LAUNCHER_INVALID_TILE_INDEX_EXT` constants
- `XR_EXT_APP_LAUNCHER_EXTENSION_NAME` literal + `_SPEC_VERSION 1`

### Log-string rename in the launcher cluster

`comp_d3d11_service.cpp` lines 8198-8484 (rendering) and 11960-12118 (service functions): rename any `shell` / `Shell` log prefixes to `workspace` / `Workspace`. Code logic untouched. Use `git grep -n 'Shell\|shell' src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp | awk -F: '$2>=8198 && $2<=8484 || $2>=11960 && $2<=12118'` to find the exact sites; each is a one-token edit. The state-comment headers at 327-367 also drop "shell" terminology — `Phase 5.8: spatial launcher app registry, pushed from the shell process via clear+add IPC calls` becomes `Phase 5.8: spatial launcher app registry, pushed from the workspace controller via clear+add IPC calls`, etc.

### What you do NOT touch

- The other ~150 `shell_*` mentions in `comp_d3d11_service.cpp`. The audit labels them; subsequent sub-phases handle them.
- `comp_d3d11_window.{cpp,h}` — `shell_mode_active`, `set_shell_mode_active`, `set_shell_dp`, `shell_input_event`, `SHELL_INPUT_RING_SIZE` all stay. Phase 2.D / 2.E.
- `ipc_protocol.h` `struct ipc_launcher_app` — wire format unchanged. The state-tracker translates `XrLauncherAppInfoEXT` to/from this struct at the boundary.
- `proto.json` / `proto.py` — IPC wire format unchanged.
- `src/xrt/targets/shell/main.c` — Shell keeps using internal IPC. Migrating Shell to extensions is a later sub-phase; parallel-path is intentional.
- `webxr_bridge/main.cpp` — unrelated.
- The `pending_launcher_remove_full_index` field and right-click context menu — those are part of the click-event payload and stay as-is (the remove action is signalled via a special negative `appIndex` value through `xrPollLauncherClickEXT`; the controller dispatches accordingly). The runtime side does not change.
- `hidden_tile_mask` — runtime-side session state; clears on every controller re-push. Stays.

### Special action values for `xrPollLauncherClickEXT`

The existing IPC `launcher_poll_click` returns an `int64_t tile_index`. The runtime overloads this with signed sentinels for special UI actions:
- `-1` — no pending click
- non-negative — appIndex of the clicked tile
- `IPC_LAUNCHER_ACTION_BROWSE` (negative sentinel; check `ipc_protocol.h`) — user clicked the "Browse" tile
- `IPC_LAUNCHER_ACTION_REMOVE` (negative sentinel) — user right-click-removed a tile (the appIndex of the removed tile is in `pending_launcher_remove_full_index`, *not* in the return value; this is a known wart — Phase 2.B preserves the existing semantics rather than redesigning)

The OpenXR header exposes only the int32_t `appIndex`; the controller is expected to look up the negative sentinels in `XR_EXT_app_launcher.h` (define them as `XR_LAUNCHER_APPINDEX_BROWSE_EXT`, `XR_LAUNCHER_APPINDEX_REMOVE_EXT`) so it can dispatch on them. The "remove which tile" question is left to a follow-up phase that promotes the click event to an event-data struct (`XrEventDataLauncherClickEXT` is sketched in the headers-draft but **not** implemented in 2.B — the simple int32_t return path matches today's IPC).

### `XrLauncherAppInfoEXT` ↔ `struct ipc_launcher_app` mapping

The public header expresses one tile as:
```c
typedef struct XrLauncherAppInfoEXT {
    XrStructureType  type;
    void *next;
    char             name[XR_MAX_APPLICATION_NAME_SIZE];
    char             iconPath[XR_MAX_PATH_LENGTH];
    uint32_t         appIndex;
} XrLauncherAppInfoEXT;
```

The IPC struct (in `ipc_protocol.h`) carries more fields than the public surface needs:
```c
struct ipc_launcher_app {
    char name[IPC_LAUNCHER_NAME_MAX];          // -> XrLauncherAppInfoEXT.name
    char exe_path[IPC_LAUNCHER_PATH_MAX];      // *** not exposed via the public API ***
    char type[IPC_LAUNCHER_TYPE_MAX];          // "3d" / "2d" — not exposed
    char icon_path[IPC_LAUNCHER_PATH_MAX];     // -> XrLauncherAppInfoEXT.iconPath
    char icon_3d_path[IPC_LAUNCHER_PATH_MAX];  // not exposed
    char icon_3d_layout[IPC_LAUNCHER_TYPE_MAX];// not exposed
};
```

Resolution for Phase 2.B:
- The state-tracker wrapper writes only `name` and `icon_path` into the IPC struct. `exe_path` / `type` / `icon_3d_path` / `icon_3d_layout` are zeroed.
- This is a known-narrow pass; the runtime currently uses `exe_path` (server-side) only when launching binaries on user click — but Phase 2.B is for **third-party workspace controllers**, which launch their own binaries on click. They do not need the runtime to know `exe_path`.
- A later sub-phase extends `XrLauncherAppInfoEXT` (e.g., via a chained struct or by promoting fields to the public API) once we settle on what's policy vs. mechanism.
- `appIndex` is a new public-only concept. Today's IPC does not carry it — the runtime indexes tiles by their position in `launcher_apps[]`. For Phase 2.B, the public `appIndex` IS the `launcher_apps[]` slot index. The state-tracker enforces `appIndex < XR_LAUNCHER_MAX_APPS_EXT` and ignores the value otherwise (the IPC RPC appends to the next free slot; the runtime cannot honor a caller-chosen index without proto changes). Document this clearly in the header — it is a Phase-2.B simplification that resolves when the IPC carries the index explicitly.

The first-party shell already populates the IPC fields through internal C calls; that path is unchanged. The new public path is *narrower* than the internal path — that is by design.

## Recommended commit sequence

Six commits. Same shape as Phase 2.A. Keep each reviewable in isolation; do not bundle.

### Commit 1 — `XR_EXT_app_launcher.h` header (pure addition)

New file: `src/external/openxr_includes/openxr/XR_EXT_app_launcher.h`. Copy the structure of the just-merged `XR_EXT_spatial_workspace.h`:
- BSL-1.0 license header + doc comment
- Include guards `#ifndef XR_EXT_APP_LAUNCHER_H`
- `#include <openxr/openxr.h>` plus `#include <openxr/XR_EXT_spatial_workspace.h>` (commented as "for future XrWorkspaceClientId integration; not strictly required by the Phase 2.B subset")
- `extern "C" {` block, `#define XR_EXT_app_launcher 1`, `_SPEC_VERSION 1`, `_EXTENSION_NAME` literal
- `XR_TYPE_LAUNCHER_APP_INFO_EXT = 1000999110`
- `XR_LAUNCHER_MAX_APPS_EXT 64`, `XR_LAUNCHER_INVALID_TILE_INDEX_EXT ((int32_t)-1)`
- The two `XR_LAUNCHER_APPINDEX_*_EXT` sentinels — match the values of `IPC_LAUNCHER_ACTION_BROWSE` and `IPC_LAUNCHER_ACTION_REMOVE` in `ipc_protocol.h` so the runtime can pass them through unchanged
- `XrLauncherAppInfoEXT` struct
- Five `PFN_*` typedefs and prototypes

Add `#include "openxr/XR_EXT_app_launcher.h"` to `src/xrt/include/xrt/xrt_openxr_includes.h` (after the spatial_workspace include).

Acceptance: file compiles standalone; `_package/` headers ship the new file when `publish-extensions.yml` next runs.

### Commit 2 — IPC-client compositor wrappers (the bridge st_oxr→ipc)

Five new functions in `src/xrt/ipc/client/ipc_client_compositor.c`, plus matching declarations in `src/xrt/ipc/client/ipc_client.h`. Each one mirrors the `comp_ipc_client_compositor_workspace_*` family from Phase 2.A:

```c
xrt_result_t
comp_ipc_client_compositor_launcher_clear_apps(struct xrt_compositor *xc);

xrt_result_t
comp_ipc_client_compositor_launcher_add_app(struct xrt_compositor *xc,
                                            const struct ipc_launcher_app *app);

xrt_result_t
comp_ipc_client_compositor_launcher_set_visible(struct xrt_compositor *xc, bool visible);

xrt_result_t
comp_ipc_client_compositor_launcher_poll_click(struct xrt_compositor *xc, int64_t *out_tile_index);

xrt_result_t
comp_ipc_client_compositor_launcher_set_running_tile_mask(struct xrt_compositor *xc, uint64_t mask);
```

Each is a one-liner: extract `ipc_client_compositor` from `xc`, call the matching `ipc_call_launcher_*` on `icc->ipc_c`, return the `xrt_result_t`.

Note that `launcher_add_app` takes an `ipc_launcher_app *` here — the state-tracker side does the `XrLauncherAppInfoEXT → ipc_launcher_app` translation, then calls this wrapper. Don't push the public type into ipc_client; the IPC layer should not know about OpenXR types.

Acceptance: builds, link succeeds, no warnings.

### Commit 3 — State-tracker implementation (`oxr_app_launcher.c`)

New file: `src/xrt/state_trackers/oxr/oxr_app_launcher.c`. Mirror `oxr_workspace.c`:
- Top-of-file forward declarations for the five `comp_ipc_client_compositor_launcher_*` functions
- Static `session_is_ipc_client(sess)` helper (or move it to a shared header — `oxr_workspace.c` already has one; either lift to `oxr_workspace_helpers.h` or duplicate, your choice — duplication is fine for a five-function file)
- Static `xret_to_xr_result(log, xret, label)` helper (same as 2.A; same XRT_ERROR_NOT_AUTHORIZED → XR_ERROR_FEATURE_UNSUPPORTED mapping)
- Five `oxr_xr*EXT` entry points wrapping the bridges

Per-function shape mirrors `oxr_xrAddWorkspaceCaptureClientEXT`:

```c
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAddLauncherAppEXT(XrSession session, const XrLauncherAppInfoEXT *info)
{
    OXR_TRACE_MARKER();
    struct oxr_session *sess = NULL;
    struct oxr_logger log;
    OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrAddLauncherAppEXT");
    OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
    OXR_VERIFY_EXTENSION(&log, sess->sys->inst, EXT_app_launcher);
    OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, info, XR_TYPE_LAUNCHER_APP_INFO_EXT);

    if (!session_is_ipc_client(sess)) {
        return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
                         "xrAddLauncherAppEXT requires an IPC-mode session");
    }

    if (info->appIndex >= XR_LAUNCHER_MAX_APPS_EXT) {
        return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
                         "appIndex %u >= XR_LAUNCHER_MAX_APPS_EXT", info->appIndex);
    }

    // Translate to IPC struct. exe_path/type/icon_3d_* are not exposed
    // by the Phase 2.B public surface and are zeroed; see header notes.
    struct ipc_launcher_app ipc_app = {0};
    snprintf(ipc_app.name, sizeof(ipc_app.name), "%s", info->name);
    snprintf(ipc_app.icon_path, sizeof(ipc_app.icon_path), "%s", info->iconPath);

    xrt_result_t xret = comp_ipc_client_compositor_launcher_add_app(&sess->xcn->base, &ipc_app);
    return xret_to_xr_result(&log, xret, "launcher_add_app");
}
```

For `xrPollLauncherClickEXT`, the IPC RPC returns `int64_t` (the wire type) but the public API exposes `int32_t *`. Truncate; the sentinel values fit in int32. Document in the function body that values outside int32 range are runtime bugs (and assert-debug them).

Acceptance: function bodies compile, link, return `XR_SUCCESS` against a workspace-authorized session and `XR_ERROR_FEATURE_UNSUPPORTED` against a standalone session.

### Commit 4 — Extension registration + dispatch wiring

Three small touches:

1. `src/xrt/state_trackers/oxr/oxr_extension_support.h` — add support block immediately after the `EXT_spatial_workspace` block (~line 575):
   ```c
   #if defined(XR_EXT_app_launcher)
   #define OXR_HAVE_EXT_app_launcher
   #define OXR_EXTENSION_SUPPORT_EXT_app_launcher(_) \
       _(EXT_app_launcher, EXT_APP_LAUNCHER)
   #else
   #define OXR_EXTENSION_SUPPORT_EXT_app_launcher(_)
   #endif
   ```
   Plus `OXR_EXTENSION_SUPPORT_EXT_app_launcher(_) \` in the master macro (~line 1052).

2. `src/xrt/state_trackers/oxr/oxr_api_funcs.h` — add gated prototype block immediately after the `OXR_HAVE_EXT_spatial_workspace` block:
   ```c
   #ifdef OXR_HAVE_EXT_app_launcher
   XRAPI_ATTR XrResult XRAPI_CALL oxr_xrClearLauncherAppsEXT(XrSession session);
   XRAPI_ATTR XrResult XRAPI_CALL oxr_xrAddLauncherAppEXT(
       XrSession session, const XrLauncherAppInfoEXT *info);
   XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetLauncherVisibleEXT(
       XrSession session, XrBool32 visible);
   XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPollLauncherClickEXT(
       XrSession session, int32_t *outAppIndex);
   XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetLauncherRunningTileMaskEXT(
       XrSession session, uint64_t mask);
   #endif
   ```

3. `src/xrt/state_trackers/oxr/oxr_api_negotiate.c` — add `ENTRY_IF_EXT(...)` block immediately after the `EXT_spatial_workspace` block:
   ```c
   #ifdef OXR_HAVE_EXT_app_launcher
   ENTRY_IF_EXT(xrClearLauncherAppsEXT, EXT_app_launcher);
   ENTRY_IF_EXT(xrAddLauncherAppEXT, EXT_app_launcher);
   ENTRY_IF_EXT(xrSetLauncherVisibleEXT, EXT_app_launcher);
   ENTRY_IF_EXT(xrPollLauncherClickEXT, EXT_app_launcher);
   ENTRY_IF_EXT(xrSetLauncherRunningTileMaskEXT, EXT_app_launcher);
   #endif
   ```

4. `src/xrt/state_trackers/oxr/CMakeLists.txt` — add `oxr_app_launcher.c` to the unconditional source list (paired with `oxr_workspace.c`).

Acceptance:
- `xrEnumerateInstanceExtensionProperties` lists `XR_EXT_app_launcher` v1
- `xrCreateInstance` succeeds with the extension enabled
- `xrGetInstanceProcAddr(..., "xrAddLauncherAppEXT", ...)` returns non-null
- Calling the returned pointer dispatches to `oxr_xrAddLauncherAppEXT`

### Commit 5 — Log-string rename in the launcher cluster

Touch `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` only at:
- Lines 327-367 — state-comment headers
- Lines 8198-8484 — rendering log strings
- Lines 11960-12118 — service function log strings

Rename `Shell` / `shell` → `Workspace` / `workspace` in these ranges only. Confirm with `git diff -W` that no functional code moves; if clang-format reflows, run `git clang-format` before staging.

Acceptance: `git diff` shows only literal-string + comment-prose replacements; runtime behavior unchanged.

### Commit 6 — Validation: extend `workspace_minimal_d3d11_win`

Either extend the Phase 2.A test app `test_apps/workspace_minimal_d3d11_win/main.cpp` to also enable `XR_EXT_app_launcher` and exercise the five new functions, or add a sister app `test_apps/launcher_minimal_d3d11_win/`. **Recommend extending** — keeping all the workspace-extension smoke tests in one binary makes the orchestrator-launched success path easier to drive, and there's no rendering or input that would warrant a separate app.

The success path (when launched under an authorized orchestrator) should:
1. Enable both `XR_EXT_spatial_workspace` and `XR_EXT_app_launcher` at instance create
2. After `xrActivateSpatialWorkspaceEXT` succeeds, call `xrClearLauncherAppsEXT`
3. Call `xrAddLauncherAppEXT` 2-3 times with synthetic tile data (name="TestApp1", iconPath="" is acceptable for this smoke; the runtime renders a placeholder if the icon doesn't load)
4. Call `xrSetLauncherVisibleEXT(session, XR_TRUE)`
5. Call `xrSetLauncherRunningTileMaskEXT(session, 0x3)` (mark first two tiles as running)
6. Call `xrPollLauncherClickEXT` once — expect `XR_LAUNCHER_INVALID_TILE_INDEX_EXT` (no human is clicking)
7. Call `xrSetLauncherVisibleEXT(session, XR_FALSE)`, `xrClearLauncherAppsEXT`, then `xrDeactivateSpatialWorkspaceEXT`

The standalone path (no orchestrator authorization) still exits PASS after the activate-deny — same as Phase 2.A.

Acceptance:
- `scripts\build_windows.bat test-apps` builds the updated test app
- Standalone run still exits PASS via the deny path
- `scripts\build-mingw-check.sh aux_util workspace_minimal_d3d11_win` green

## Acceptance criteria for the whole phase

- ✅ `_package/` ships `XR_EXT_app_launcher.h` (CI's `publish-extensions.yml` picks it up automatically).
- ✅ A test client compiled against the new header can: enumerate the extension, create instance with both `XR_EXT_spatial_workspace` and `XR_EXT_app_launcher` enabled, activate workspace, clear+add+visible+running+poll, deactivate.
- ✅ The first-party DisplayXR Shell continues to work unchanged (still uses internal IPC; that path is parallel to the extension path).
- ✅ Launcher-cluster log strings in `comp_d3d11_service.cpp` say "workspace" not "shell".
- ✅ Windows MSVC CI green. macOS CI green (header is platform-neutral).
- ✅ `build-mingw-check.sh aux_util` green.
- ✅ Branch is one or two PRs against `main` — six commits total, each reviewable.

## Hand-off notes

- **Don't auto-commit individual sub-steps without testing.** Per `feedback_test_before_ci.md`: build locally, smoke-test, then commit. Use `scripts\build_windows.bat build` for incremental rebuilds. Don't push until the full sequence is locally green.
- **Push runtime binaries to `Program Files\DisplayXR\Runtime` after rebuild** — per `feedback_dll_version_mismatch.md`, the registered runtime is what elevated test contexts load; the `XR_RUNTIME_JSON` env var is ignored under elevation.
- **`/ci-monitor` is for after the user has tested and approved.** The user will run it themselves or invite you to.
- **The PRD layer order is preserved.** State tracker → IPC → compositor; the new extension code lives in the state tracker; it does not bypass IPC and does not call compositor-private headers directly.
- **Naming consistency check before commit:** `grep -rn 'Shell\|shell' src/xrt/state_trackers/oxr/oxr_app_launcher.c` should return zero. The new file is brand-neutral by construction.
- **The XR_EXT_spatial_workspace header is shipping** — do not modify it in this phase. If you find yourself wanting to add `XrWorkspaceClientId` references to launcher functions, defer to a later phase that promotes the click event to a struct.

## What unblocks once Phase 2.B passes

- **Phase 2.F (hit-test)** — promotes `workspace_raycast_hit_test` to `xrWorkspaceHitTestEXT`. Lowest-risk policy-adjacent migration after launcher.
- **Window-pose / visibility** functions (`xrSetWorkspaceClientWindowPoseEXT`, `xrSetWorkspaceClientVisibilityEXT`) — natural next move once the launcher pattern is validated; same wrapping shape, different RPCs.
- **Phase 2.C (chrome rendering)** is a heavier lift (composite-pipeline change); skip-ahead to 2.F or window-pose for the next phase, save chrome for later.
