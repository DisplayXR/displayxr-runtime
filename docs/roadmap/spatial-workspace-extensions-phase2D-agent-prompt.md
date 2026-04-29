# Phase 2.D Agent Prompt — Workspace Input Routing

Self-contained prompt for a fresh agent session implementing Phase 2.D of the workspace-extensions effort. Drop into a new session as the user message after `/clear`. The agent has no memory of the prior design conversations — this prompt assumes nothing.

---

## What Phase 2.D is for

You're picking up Phase 2.D of the **workspace-extensions migration**. Phases 2.A → 2.H have shipped (six branches; six commits each). The runtime now exposes:

- `XR_EXT_spatial_workspace v3` — lifecycle (activate/deactivate/get-state), capture clients (add/remove), window pose (set/get) + visibility, hit-test
- `XR_EXT_app_launcher v1` — launcher tile registry, click polling, running-tile mask, visibility

The first-party DisplayXR Shell continues to use internal IPC (`ipc_call_workspace_*`, `ipc_call_launcher_*`, `ipc_call_system_set_focused_client`). Its migration to the public extensions is **Phase 2.I**, gated on Phase 2.D landing — input routing is the last unshipped piece of the public surface that the shell needs.

Phase 2.D moves **input policy** from the runtime to the workspace controller. Today the runtime's WndProc classifies every keystroke and click — picking who gets focus, which keys are reserved, what title-bar drag does, how scroll wheel maps to resize/Z-depth, etc. After 2.D the controller decides; the runtime keeps high-frequency mechanism (Win32 message dispatch, cross-process delivery via PostMessage + SendInput, coordinate remapping, the capture-client input ring).

This is the migration the audit calls "high risk; design-first" because the input extension surface wasn't yet sketched. The design has now been completed — the architectural choice (hit-region as shared vocabulary) and the public API surface are fixed below. This phase is implementation.

## Read these in order before touching code

1. `docs/roadmap/spatial-workspace-extensions-plan.md` — three-phase master plan.
2. `docs/roadmap/spatial-workspace-extensions-headers-draft.md` — the API surface sketch. Phase 2.D updates `XR_EXT_spatial_workspace` to v4 with the additions in "What ships in Phase 2.D" below. Lines 509-540 are the "Open questions" section the design here resolves.
3. `docs/roadmap/spatial-workspace-extensions-phase2-audit.md` lines 97-103 — the 2.D entry. Notes the `shell_input_event` rename in 2.D scope.
4. `docs/roadmap/spatial-workspace-extensions-phase2A-agent-prompt.md` and `…phase2B-agent-prompt.md` — the prompts that drove 2.A / 2.B. Read for the established pattern; Phase 2.D follows the same six-commit shape.
5. `git log --oneline main | head -40` — surveys all six prior phase commits. Skim the diffs of `XR_EXT_spatial_workspace.h`, `oxr_workspace.c`, and `ipc_client_compositor.c` — Phase 2.D's deliverables build on those structures.

Also pull `MEMORY.md` and skim `feedback_use_build_windows_bat.md`, `reference_local_build_deps.md`, `feedback_test_before_ci.md`, `feedback_dll_version_mismatch.md`. These are project conventions you must follow.

## Branch + prerequisites

- Start from `main` after Phase 2.A → 2.H have merged.
- New branch: `feature/workspace-extensions-2D`.
- Confirm `main` includes the prior phase commits: `git log --oneline main | grep -E 'spatial_workspace|app_launcher|workspace_minimal'` should return ~30 hits across phases.

## The architectural choice (already decided)

Phase 2.D's design was settled in a prior planning session. The decision: **hit-region as shared vocabulary**. Both runtime and controller speak `XrWorkspaceHitRegionEXT` (the enum below). Runtime classifies (it has the geometry); controller decides what each region means.

This choice resolves four design questions atomically:
1. Hit-test ownership: runtime classifies, controller interprets
2. Event transport: bulk-drain (`xrEnumerateWorkspaceInputEventsEXT`), not `xrPollEvent` — `xrPollEvent` is the wrong shape for high-frequency input
3. Drag policy: stays in the controller via pointer-capture affordance; runtime never touches drag policy (it's coupled to carousel/layout/resume-timer state that must stay private)
4. Hover: events fire on region transitions only; no per-frame mouse-move events. Controllers wanting pixel-precise hover query `xrWorkspaceHitTestEXT` (already shipping in v3)

If you find yourself wanting to add a `POINTER_MOTION` event variant, a `xrBeginWorkspaceDragEXT(MOVE | RESIZE_NW | ROTATE)` function, a per-frame cursor push, or any policy enum besides hit-region — STOP. The design rejected those for principled reasons. Re-read this section before adding surface area.

## What ships in Phase 2.D

### `XR_EXT_spatial_workspace` v3 → v4

Bump `XR_EXT_spatial_workspace_SPEC_VERSION` 3 → 4. The v3 surface is unchanged; v4 is purely additive.

### New types

```c
typedef enum XrWorkspaceHitRegionEXT {
    XR_WORKSPACE_HIT_REGION_BACKGROUND_EXT       = 0,  // miss
    XR_WORKSPACE_HIT_REGION_CONTENT_EXT           = 1,
    XR_WORKSPACE_HIT_REGION_TITLE_BAR_EXT         = 2,
    XR_WORKSPACE_HIT_REGION_CLOSE_BUTTON_EXT      = 3,
    XR_WORKSPACE_HIT_REGION_MINIMIZE_BUTTON_EXT   = 4,
    XR_WORKSPACE_HIT_REGION_MAXIMIZE_BUTTON_EXT   = 5,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_N_EXT     = 10,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_S_EXT     = 11,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_E_EXT     = 12,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_W_EXT     = 13,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_NE_EXT    = 14,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_NW_EXT    = 15,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_SE_EXT    = 16,
    XR_WORKSPACE_HIT_REGION_EDGE_RESIZE_SW_EXT    = 17,
    XR_WORKSPACE_HIT_REGION_TASKBAR_EXT            = 20,
    XR_WORKSPACE_HIT_REGION_LAUNCHER_TILE_EXT      = 21,
    XR_WORKSPACE_HIT_REGION_MAX_ENUM_EXT           = 0x7FFFFFFF
} XrWorkspaceHitRegionEXT;

typedef enum XrWorkspaceInputEventTypeEXT {
    XR_WORKSPACE_INPUT_EVENT_POINTER_EXT         = 0,
    XR_WORKSPACE_INPUT_EVENT_POINTER_HOVER_EXT   = 1,
    XR_WORKSPACE_INPUT_EVENT_KEY_EXT              = 2,
    XR_WORKSPACE_INPUT_EVENT_SCROLL_EXT           = 3,
    XR_WORKSPACE_INPUT_EVENT_TYPE_MAX_ENUM_EXT    = 0x7FFFFFFF
} XrWorkspaceInputEventTypeEXT;

typedef struct XrWorkspaceInputEventEXT {
    XrWorkspaceInputEventTypeEXT eventType;
    uint32_t                      timestampMs;        // host monotonic ms
    union {
        struct {
            XrWorkspaceClientId       hitClientId;
            XrWorkspaceHitRegionEXT   hitRegion;
            XrVector2f                 localUV;
            int32_t                    cursorX;       // display pixels, top-left origin
            int32_t                    cursorY;
            uint32_t                   button;        // 1=L, 2=R, 3=M
            XrBool32                   isDown;
            uint32_t                   modifiers;     // bit0=SHIFT, bit1=CTRL, bit2=ALT
        } pointer;
        struct {  // fired only on region transitions
            XrWorkspaceClientId       prevClientId;
            XrWorkspaceHitRegionEXT   prevRegion;
            XrWorkspaceClientId       currentClientId;
            XrWorkspaceHitRegionEXT   currentRegion;
        } pointerHover;
        struct {
            uint32_t                   vkCode;        // Win32 VK_*; cross-platform mapping TBD
            XrBool32                   isDown;
            uint32_t                   modifiers;
        } key;
        struct {
            float                      deltaY;        // wheel ticks (+ = up)
            int32_t                    cursorX;
            int32_t                    cursorY;
            uint32_t                   modifiers;
        } scroll;
    };
} XrWorkspaceInputEventEXT;
```

Use a tagged C union (NOT a chained struct with `next` pointers). Variants are short and finite; chained next-pointer ergonomics aren't worth the per-event indirection. Match the pattern of `VkClearColorValue` in the Vulkan headers if you want a precedent.

### Five new functions

| Function | Wraps existing IPC RPC | Notes |
|---|---|---|
| `xrSetWorkspaceFocusedClientEXT(session, clientId)` | `workspace_set_focused_client` (NEW — wraps existing `system_set_focused_client`) | `clientId == XR_NULL_WORKSPACE_CLIENT_ID` clears focus |
| `xrGetWorkspaceFocusedClientEXT(session, *clientId)` | `workspace_get_focused_client` (NEW) | Read-only |
| `xrEnumerateWorkspaceInputEventsEXT(session, capacityInput, *countOutput, events)` | `workspace_enumerate_input_events` (NEW) | Standard two-call enumerate. Capacity-cap on the wire is **16 events per RPC** to fit `IPC_BUF_SIZE` (1024 bytes) |
| `xrEnableWorkspacePointerCaptureEXT(session, button)` | `workspace_pointer_capture_set` (NEW; `enabled=true`) | Runtime keeps delivering pointer events for `button` until release, even if cursor leaves any window |
| `xrDisableWorkspacePointerCaptureEXT(session)` | `workspace_pointer_capture_set` (NEW; `enabled=false`) | |

`xrWorkspaceHitTestEXT` (shipping in v3) gains an `XrWorkspaceHitRegionEXT *outHitRegion` out-parameter. **This is a signature change**, not a separate function. Call sites compiled against v3 break — accept that as the cost of v3→v4 versioning. The IPC `workspace_hit_test` RPC adds `hit_region: uint32_t` to its outputs.

### What you do NOT touch

- Anything in `XR_EXT_app_launcher` — separate extension, separate phase if it grows.
- All other functions in the headers-draft (frame capture, enumerate clients, lifecycle events) — those are later sub-phases.
- `src/xrt/targets/shell/main.c` — Shell migration to public extensions is **Phase 2.I**, NOT Phase 2.D. Shell continues to use internal IPC; the parallel-path is intentional.
- `webxr_bridge/main.cpp` — unrelated.
- macOS — Phase 2.D ships Windows-only. Cocoa input routing (`comp_metal_window` equivalent of WndProc) is a follow-up phase.

### Out-of-scope for Phase 2.D (explicit deferrals)

- **Per-key classification override** (`xrSetWorkspaceKeyClassificationEXT`). MVP hardcodes: TAB and DELETE consumed by runtime; ESC consumed when any window is maximized; everything else delivered via input event AND forwarded to focused HWND. Document this in the header comment for `xrEnumerateWorkspaceInputEventsEXT`.
- **Cursor shape control** (`xrSetWorkspaceCursorEXT`). Runtime continues to compute desired shape from hit-region.
- **Right-click context menu content** for launcher tiles. Runtime continues to render (Win32 popup); a controller can suppress by handling RIGHT-button pointer events on `LAUNCHER_TILE` region — it just can't customize the menu items yet.
- **Per-frame mouse-move events / cursor-position pushes**. Hover transitions fire on region change only.
- **Drag policy in the runtime**. Controller drives drag via pointer capture + `xrSetWorkspaceClientWindowPoseEXT` (already shipping in v2). Do not add `xrBeginWorkspaceDragEXT`.

## The 2.D rename set

The audit's Phase 2.D scope includes the deferred input-routing rename. Lands as Commit 1 of this phase:

| Old | New |
|---|---|
| `struct shell_input_event` | `struct workspace_input_event` |
| `SHELL_INPUT_RING_SIZE` | `WORKSPACE_INPUT_RING_SIZE` |
| `SHELL_SENDINPUT_MARKER` | `WORKSPACE_SENDINPUT_MARKER` |
| `WM_SHELL_SET_FOREGROUND` | `WM_WORKSPACE_SET_FOREGROUND` |
| `WM_SHELL_LAUNCH_APP` | `WM_WORKSPACE_LAUNCH_APP` |
| `is_shell_reserved_key` | `is_workspace_reserved_key` |

After this rename pass, `comp_d3d11_window.cpp` and `comp_d3d11_service.cpp` have **zero** `shell_*` identifiers in non-brand code. The literal `displayxr-shell.exe` binary name stays (it's a brand reference); the `"DisplayXR Shell"` window title stays (UI brand).

## Recommended commit sequence

Six commits. Same shape as 2.A → 2.F. Keep each reviewable in isolation; do not bundle.

### Commit 1 — Internal renames (the deferred 2.D set)

Mechanical sed pass on `comp_d3d11_window.{h,cpp}` and `comp_d3d11_service.cpp`. The audit calls these out as Phase 2.D scope because the rename has to land alongside the input-routing migration that uses these symbols.

Skip the `displayxr-shell.exe` binary literal and the `"DisplayXR Shell"` window title — those are brand strings.

**Acceptance**: build green; no functional change; `git grep -E 'SHELL_INPUT|shell_input_event|WM_SHELL|is_shell_reserved|SHELL_SENDINPUT' src/xrt/compositor` returns zero.

### Commit 2 — Hit-region enum + plumbing

New file: `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` — add the `XrWorkspaceHitRegionEXT` enum (verbatim from above) at the end of the existing v3 declarations.

Touch `src/xrt/ipc/shared/proto.json` — extend `workspace_hit_test` with `hit_region: uint32_t` output.

Touch `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — update `comp_d3d11_service_workspace_hit_test` (the public C wrapper from 2.F) to populate the new region output. The internal `workspace_raycast_hit_test` already returns rich hit info (slot, in_title_bar, in_close_btn, in_minimize_btn, in_maximize_btn, in_content, edge_flags); map those flags to a single `XrWorkspaceHitRegionEXT` value:
- `slot < 0` → `BACKGROUND_EXT`
- `in_close_btn` → `CLOSE_BUTTON_EXT`
- `in_minimize_btn` → `MINIMIZE_BUTTON_EXT`
- `in_maximize_btn` → `MAXIMIZE_BUTTON_EXT`
- `in_title_bar` → `TITLE_BAR_EXT`
- `edge_flags` → corresponding `EDGE_RESIZE_*_EXT` value (handle compound flags like RESIZE_N|RESIZE_W → `_NW_EXT`)
- `in_content` → `CONTENT_EXT`
- (taskbar / launcher hit detection lives in the launcher hit-test path; route those via `TASKBAR_EXT` / `LAUNCHER_TILE_EXT`)

Touch `src/xrt/compositor/d3d11_service/comp_d3d11_service.h` — add `XrWorkspaceHitRegionEXT *out_hit_region` parameter to the public C wrapper.

Touch `src/xrt/ipc/server/ipc_server_handler.c` — update `ipc_handle_workspace_hit_test` to plumb the region output through.

Touch `src/xrt/ipc/client/ipc_client_compositor.c` + `ipc_client.h` — update `comp_ipc_client_compositor_workspace_hit_test` bridge to take a region out-param.

Bump `XR_EXT_spatial_workspace_SPEC_VERSION` 3 → 4.

**Acceptance**: build green; standalone test app sees v4 in `xrEnumerateInstanceExtensionProperties`.

### Commit 3 — Update `xrWorkspaceHitTestEXT` signature

Touch `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` — extend `PFN_xrWorkspaceHitTestEXT` and the prototype with the new `XrWorkspaceHitRegionEXT *outHitRegion` final parameter.

Touch `src/xrt/state_trackers/oxr/oxr_workspace.c` — extend `oxr_xrWorkspaceHitTestEXT` to accept the new param and plumb it from the IPC bridge call to the caller.

Touch `src/xrt/state_trackers/oxr/oxr_api_funcs.h` — update the prototype.

Touch `test_apps/workspace_minimal_d3d11_win/main.cpp` — update the existing hit-test smoke calls to pass the new param and print the region.

**Acceptance**: build green; smoke test `xrWorkspaceHitTestEXT` prints `region=` for the two probe positions; the off-screen probe reports `BACKGROUND_EXT`.

### Commit 4 — Focus extension

Touch `src/xrt/ipc/shared/proto.json` — add:
- `workspace_set_focused_client` (in: `client_id: uint32_t`)
- `workspace_get_focused_client` (out: `client_id: uint32_t`)

Touch `src/xrt/ipc/server/ipc_server_handler.c` — add two handlers with the standard PID-auth gate. Both wrap `system_set_focused_client` / `system_get_focused_client` (existing system-scoped variants); the workspace-scoped wrappers add the auth check.

Touch `src/xrt/ipc/client/ipc_client_compositor.c` + `ipc_client.h` — add `comp_ipc_client_compositor_workspace_{set,get}_focused_client` bridges.

Touch `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` — add `xrSetWorkspaceFocusedClientEXT` and `xrGetWorkspaceFocusedClientEXT` PFN typedefs and prototypes.

Touch `src/xrt/state_trackers/oxr/oxr_workspace.c` — add the two entry points, with `XR_NULL_WORKSPACE_CLIENT_ID` interpretation for "clear focus."

Touch `src/xrt/state_trackers/oxr/oxr_api_funcs.h` and `oxr_api_negotiate.c` — declare prototypes and add `ENTRY_IF_EXT` lines.

**Acceptance**: PFN resolution non-null; setting/getting focus over IPC round-trips correctly.

### Commit 5 — Input event drain + pointer capture

The meatiest commit. Three new functions, all driven by extensions to the existing `workspace_input_event` ring (renamed in Commit 1).

Touch `src/xrt/ipc/shared/ipc_protocol.h` — add `struct workspace_input_event` (the wire format). Match the layout of the public `XrWorkspaceInputEventEXT` but use plain C types (no XR enum dependencies). Use a tagged union with the same discriminator pattern.

Touch `src/xrt/ipc/shared/proto.json` — add:
- `workspace_enumerate_input_events` (in: `capacity: uint32_t`; out: `count: uint32_t`, `events: struct workspace_input_event[16]`). Note: 16 not 64 — fits `IPC_BUF_SIZE`.
- `workspace_pointer_capture_set` (in: `enabled: bool`, `button: uint32_t`)

Touch `src/xrt/ipc/server/ipc_server_handler.c` — add the two handlers with PID-auth gate. The drain handler reads from the existing `workspace_input_event` ring (Commit 1 renamed it) and copies up to `capacity` events into the output array. The pointer-capture handler sets a runtime-side flag the WndProc honors.

Touch `src/xrt/ipc/client/ipc_client_compositor.c` + `ipc_client.h` — add bridges. `comp_ipc_client_compositor_workspace_enumerate_input_events` takes primitive parameters (uint32_t capacity, count*, events*) — no IPC types in the state tracker.

Touch `src/xrt/compositor/d3d11/comp_d3d11_window.{h,cpp}` — extend the existing ring's writers to also emit semantic events (POINTER, HOVER on transitions, KEY, SCROLL) into a *second* event queue that's read by the workspace controller. The capture-client ring stays as-is; the new public ring is parallel. The pointer-capture flag, when set, prevents button-up events from being filtered by the "outside content rect" gate. Hover transitions: track `current_hit_region` per WndProc tick; when it changes, emit one `HOVER` event.

Touch `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — when WndProc gets a click/key/scroll, hit-test the cursor position (if pointer event), construct a `workspace_input_event`, push into the public ring. Honor the pointer-capture flag in the click-handling path.

Touch `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` — add the input event types (enum + struct + union) and three new PFNs/prototypes.

Touch `src/xrt/state_trackers/oxr/oxr_workspace.c` — add three entry points. `oxr_xrEnumerateWorkspaceInputEventsEXT` translates between the wire `workspace_input_event` and the public `XrWorkspaceInputEventEXT` (field-by-field; the layouts intentionally match for ease of translation).

Touch `src/xrt/state_trackers/oxr/oxr_api_funcs.h` and `oxr_api_negotiate.c` — declare prototypes and add `ENTRY_IF_EXT` lines.

**Acceptance**: PFN resolution non-null; standalone smoke prints "no events available" (zero count) since standalone path doesn't reach the workspace state. Orchestrator success path: a click on Notepad's HWND (or whatever the test app captures) yields a POINTER event on `xrEnumerateWorkspaceInputEventsEXT`.

### Commit 6 — Test app extension + minor service-side log renames

Extend `test_apps/workspace_minimal_d3d11_win/main.cpp` to:
1. Add 5 more PFN lookups (now 19 total)
2. On the orchestrator success path, after the existing capture-client smoke:
   - Call `xrSetWorkspaceFocusedClientEXT(session, clientId)` with the captured client id
   - Call `xrGetWorkspaceFocusedClientEXT(session, &got)` and confirm round-trip
   - Call `xrEnumerateWorkspaceInputEventsEXT(session, 0, &count, NULL)` (count-query) — expect 0 events since no human is interacting
   - Call `xrEnableWorkspacePointerCaptureEXT(session, 1)` then `xrDisableWorkspacePointerCaptureEXT(session)` — expect XR_SUCCESS for both
3. Print the existing hit-test results with the new region info

Service-side log rename pass for any leftover `Shell:` log prefixes the prior phases missed in input-routing-related sites.

**Acceptance**: Build via `scripts\build_windows.bat test-apps`; standalone run still exits PASS via the activate-deny path; `_package\run_workspace_minimal_d3d11_win.bat` prints all 19 PFNs as XR_SUCCESS.

## Acceptance criteria for the whole phase

- ✅ `_package/` ships an updated `XR_EXT_spatial_workspace.h` with v4 surface
- ✅ A test client compiled against the new header can: enumerate the extension at v4, create instance, resolve all 19 PFNs, exercise focus + drain + pointer-capture against a real workspace session
- ✅ The first-party DisplayXR Shell continues to work unchanged (still uses internal IPC; that path is parallel to the extension path)
- ✅ Zero `shell_input_event` / `SHELL_INPUT_RING_SIZE` / `WM_SHELL_*` / `is_shell_reserved_key` / `SHELL_SENDINPUT_MARKER` mentions in the runtime source after Commit 1
- ✅ Windows MSVC CI green
- ✅ macOS CI green (the new header is platform-neutral; macOS implementation deferred)
- ✅ `build-mingw-check.sh aux_util` green for any cross-platform code touched
- ✅ Branch is six commits total, each reviewable in isolation

## Hand-off notes

- **Don't auto-commit individual sub-steps without testing.** Per `feedback_test_before_ci.md`: build locally, smoke-test, then commit. Use `scripts\build_windows.bat build` for incremental rebuilds. Don't push until the full sequence is locally green.
- **Push runtime binaries to `Program Files\DisplayXR\Runtime` after rebuild** — per `feedback_dll_version_mismatch.md`, the registered runtime is what elevated test contexts load; the `XR_RUNTIME_JSON` env var is ignored under elevation.
- **Watch for the proto generator's `int32_t` limitation** — we hit this in Phase 2.F. The generator (`ipcproto/common.py`) doesn't support `int32_t`; use `int64_t` over the wire and truncate at the IPC handler boundary. The cursor coordinates in `XrWorkspaceInputEventEXT.pointer.cursorX/Y` are int32 in the public API; the wire `workspace_input_event` should use `int64_t` and the state tracker truncates.
- **Watch for the IPC buffer size limit** — `IPC_BUF_SIZE` is 1024 bytes. The 16-event capacity for `workspace_enumerate_input_events` is the maximum that fits. If your `struct workspace_input_event` ends up bigger than ~60 bytes you'll need to either shrink it (drop fields) or reduce capacity (e.g., 8 events/call).
- **Watch for st_oxr's include path** — the state tracker does NOT have `ipc/` on its include path. The IPC bridge takes primitive C parameters (uint32_t, char*, etc.) so st_oxr never needs to `#include "shared/ipc_protocol.h"`. We hit this in Phase 2.B with `struct ipc_launcher_app`. For Phase 2.D, the bridge's `enumerate_input_events` takes `(struct xrt_compositor *xc, uint32_t capacity, uint32_t *count, void *events_buf, size_t events_stride, size_t events_count_max)` or similar — pass-through in opaque-buffer form. The translation `workspace_input_event ↔ XrWorkspaceInputEventEXT` happens in `oxr_workspace.c` after the IPC bridge returns.
- **The PRD layer order is preserved.** State tracker → IPC → compositor; the new extension code lives in the state tracker; it does not bypass IPC and does not call compositor-private headers directly.
- **Naming consistency check before commit:** `git grep 'shell_input\|SHELL_INPUT\|WM_SHELL\|is_shell_reserved\|SHELL_SENDINPUT' src/xrt/compositor` should return zero after Commit 1. The new event union and helper functions are brand-neutral by construction.
- **`/ci-monitor` is for after the user has tested and approved.** Don't run it yourself.
- **Test 2 (shell-mode regression)** is critical for this phase — input is the most fragile part of the migration. After all six commits land locally, drive a full shell-launch test (two cube apps, click around, TAB, DELETE, scroll-resize, title-bar drag) and confirm zero regressions. If anything feels off, that's a real bug, not a flaky test.

## What unblocks once Phase 2.D passes

- **Phase 2.I — first-party shell migrates to public extensions** (full prompt below).
- **Phase 2.G — layout presets + ESC cleanup**. Layout-preset semantics move to the controller (which computes per-window poses and pushes via `xrSetWorkspaceClientWindowPoseEXT`); the runtime stops owning preset names. The ESC carve-out and reentry state machine simplify or delete because the runtime no longer needs an "empty workspace" mode.
- **Phase 2.C — chrome rendering**. The heaviest remaining migration; allocate a full sub-session. Chrome (close/min/max buttons, app name, title bar) moves to controller-rendered overlays composited by the runtime. The runtime stops drawing chrome at all.
- **macOS input routing port**. Mirror the public surface with a Cocoa-native event-source equivalent of the Win32 WndProc. The headers stay platform-neutral; only the implementation differs.

---

# Phase 2.I Follow-Up Prompt — Shell Migration to Public Extensions

> ⚠️ **Follow-up work, NOT part of Phase 2.D.** This section is the agent prompt for Phase 2.I, which lands AFTER Phase 2.D ships. It is included here so the design intent for 2.I is captured alongside 2.D's surface (the surface 2.I depends on). When you're ready to do 2.I, drop this section into a fresh agent session as the user message after `/clear`. Do not start 2.I before 2.D is merged to `main`.

## What Phase 2.I is for

You're picking up Phase 2.I — the **shell migration** to public OpenXR extensions. After Phases 2.A through 2.D shipped, the runtime exposes `XR_EXT_spatial_workspace v4` and `XR_EXT_app_launcher v1` — the full public surface a workspace controller needs. Phase 2.I migrates the **first-party DisplayXR Shell** off internal IPC and onto the public extensions.

**Why this is the validation point**: a third-party workspace controller can theoretically be written against the public surface. Phase 2.I proves it by switching the first-party shell — which has historically used internal `ipc_call_*` directly — to the same surface a third party would use. If the public API is insufficient anywhere, gaps surface during 2.I and become high-priority follow-up work for `XR_EXT_spatial_workspace v5+`.

The shell's source becomes the **canonical reference workspace controller** — the working specification for "how to write a workspace controller against our public API."

## Read these in order before touching code

1. The full Phase 2.D prompt above — the public surface 2.I depends on.
2. `docs/roadmap/spatial-workspace-extensions-headers-draft.md` lines 1-320 — the `XR_EXT_spatial_workspace` design with the v4 surface from 2.D.
3. `git log --oneline main | grep -E 'spatial_workspace|app_launcher|workspace_minimal' | head -40` — surveys all prior phase commits. Know what's shipping before you migrate consumers of it.
4. `test_apps/workspace_minimal_d3d11_win/main.cpp` — the validation test app from 2.A onward. Models exactly the pattern the migrated shell will follow: `xrCreateInstance` with extensions enabled, `xrCreateSession` with D3D11 binding, `xrGetInstanceProcAddr` for each extension function, dispatch via PFN.

## Branch + prerequisites

- Start from `main` after Phase 2.D has merged.
- New branch: `feature/workspace-extensions-2I`.
- Confirm `main` includes 2.D commits — `git log --oneline main | grep -E 'EnumerateWorkspaceInputEvents|WorkspaceFocusedClient|WorkspaceHitRegion'` should return ~5 hits.

## Precondition: `xrCaptureWorkspaceFrameEXT` may need to land first

The shell's screenshot path uses `ipc_call_workspace_capture_frame(&ipc_c, &req, &result)` (around `src/xrt/targets/shell/main.c:1840`). The matching public extension function `xrCaptureWorkspaceFrameEXT` is **sketched in the headers-draft** (around lines 268-274) but **not yet implemented**.

You have three options:

1. **Implement `xrCaptureWorkspaceFrameEXT` first as Phase 2.I-prequel** (1-2 commits before the migration commits). Bumps `XR_EXT_spatial_workspace` v4 → v5. Cleanest — gives the shell's migration zero internal-IPC residue.
2. **Leave the screenshot path on internal IPC**. Shell migrates everything else but keeps `ipc_call_workspace_capture_frame` plus its `ipc_client_connection.h` includes for that one call. Cheapest but defeats the "shell becomes brand-neutral OpenXR client" payoff.
3. **Drop screenshot capability from the shell**. Move screenshots into a separate tool that uses internal IPC, leaving the shell pure-extension. Largest scope.

**Recommended**: Option 1. Implement `xrCaptureWorkspaceFrameEXT` as a 6-commit prequel matching the 2.A/2.B pattern (the IPC RPC is already shipping, so it's just the wrapping work). Then the shell migration is clean.

## What ships in Phase 2.I

The shell becomes a regular OpenXR client. Specifically:

- `src/xrt/targets/shell/main.c` deletes its `#include "client/ipc_client.h"`, `#include "client/ipc_client_connection.h"`, `#include "ipc_client_generated.h"` lines.
- All `ipc_call_workspace_*`, `ipc_call_launcher_*`, and `ipc_call_system_set_focused_client` invocations replace with `PFN_xr*EXT` dispatch via `xrGetInstanceProcAddr`.
- `ipc_client_connection_init` / `ipc_client_connection_fini` calls (around lines 1913, 2482, 2105, 2108) replace with `xrCreateInstance` / `xrDestroyInstance` plus `xrCreateSession` / `xrDestroySession`.

The shell's role inside the orchestrator's workspace-controller manifest (Phase 2.0) is unchanged — it's still the binary the orchestrator spawns. What changes is its **internal architecture**: from "IPC-private client" to "public-OpenXR client."

### Graphics binding question

OpenXR generally requires a graphics binding for `xrCreateSession`. The headers-draft notes (around line 45 of "Activation handshake") that workspace controllers don't render swapchains: "graphics binding can be `XR_NULL_HANDLE` — workspace controllers don't render swapchains; they only instruct."

But our runtime's session creation path may not yet honor `XR_NULL_HANDLE` graphics binding. **Check this early.** If not honored, the shell needs a minimal D3D11 binding (mirror `test_apps/workspace_minimal_d3d11_win/main.cpp` setup) — adds ~30 lines but is a known-good pattern. If honored, the shell can use `XR_NULL_HANDLE` and avoid D3D11 dependencies entirely.

If unsupported and you decide it should be supported, that's a small addition to `oxr_session.c` that's worth doing as part of 2.I — make `XR_NULL_HANDLE` graphics binding work for workspace-mode sessions only (gated on the session having `XR_EXT_spatial_workspace` enabled and being authorized).

### Functions the shell will call (post-migration)

| Today (internal IPC) | After 2.I (public extension) | Phase that shipped the extension |
|---|---|---|
| `ipc_call_workspace_activate` | `pfnActivateSpatialWorkspaceEXT(session)` | 2.A |
| `ipc_call_workspace_deactivate` | `pfnDeactivateSpatialWorkspaceEXT(session)` | 2.A |
| `ipc_call_workspace_get_state` | `pfnGetSpatialWorkspaceStateEXT(session, *active)` | 2.A |
| `ipc_call_workspace_add_capture_client` | `pfnAddWorkspaceCaptureClientEXT(session, hwnd, name, *clientId)` | 2.A |
| `ipc_call_workspace_remove_capture_client` | `pfnRemoveWorkspaceCaptureClientEXT(session, clientId)` | 2.A |
| `ipc_call_workspace_set_window_pose` | `pfnSetWorkspaceClientWindowPoseEXT(session, clientId, &pose, w, h)` | 2.C |
| `ipc_call_workspace_capture_frame` | `pfnCaptureWorkspaceFrameEXT(session, &req, *result)` | **2.I-prequel** (option 1 above) |
| `ipc_call_launcher_clear_apps` | `pfnClearLauncherAppsEXT(session)` | 2.B |
| `ipc_call_launcher_add_app` | `pfnAddLauncherAppEXT(session, &info)` | 2.B |
| `ipc_call_launcher_set_visible` | `pfnSetLauncherVisibleEXT(session, visible)` | 2.B |
| `ipc_call_launcher_poll_click` | `pfnPollLauncherClickEXT(session, *appIndex)` | 2.B |
| `ipc_call_launcher_set_running_tile_mask` | `pfnSetLauncherRunningTileMaskEXT(session, mask)` | 2.B |
| `ipc_call_system_set_focused_client` | `pfnSetWorkspaceFocusedClientEXT(session, clientId)` | 2.D |

The shell will additionally consume the new 2.D input surface (`xrEnumerateWorkspaceInputEventsEXT`, `xrEnable/DisableWorkspacePointerCaptureEXT`) **only if it has a use case**. Today the shell does not consume raw input — the runtime forwards directly to focused HWNDs. Post-2.I that hasn't necessarily changed; if the shell wants to take over focus-on-click policy or window-drag gestures, it can opt in incrementally as a separate sub-task.

## Recommended commit sequence (assuming prequel option 1)

### Prequel (2 commits)

1. **`extensions: extend XR_EXT_spatial_workspace with frame capture (v5)`** — add `XrWorkspaceCaptureRequestEXT`, `XrWorkspaceCaptureResultEXT`, `XrWorkspaceCaptureFlagsEXT` (per headers-draft lines 240-265), `xrCaptureWorkspaceFrameEXT` PFN + prototype. Bump `XR_EXT_spatial_workspace_SPEC_VERSION` 4 → 5.
2. **`oxr/ipc: implement and dispatch xrCaptureWorkspaceFrameEXT`** — IPC bridge in `ipc_client_compositor.c`, state-tracker entry point in `oxr_workspace.c`, dispatch wiring (oxr_api_funcs.h, oxr_api_negotiate.c). The IPC RPC `workspace_capture_frame` is already shipping; just wrap it. Test app exercises the function on the success path.

### Migration commits (4 commits)

3. **`shell: bootstrap as OpenXR client`** — at the top of `main()`, before the existing `ipc_client_connection_init` call (~line 1913), add `xrCreateInstance` enabling both extensions, get system, create session (with D3D11 binding or `XR_NULL_HANDLE`), resolve all 13+ PFNs into a struct. Keep the existing IPC connection in place; just add the OpenXR scaffolding. Verify the shell still launches and behaves identically.

4. **`shell: migrate workspace lifecycle + capture clients to extensions`** — replace the 5 workspace_* calls (activate, deactivate, get_state, add/remove capture client) and the set_window_pose / capture_frame calls with the corresponding PFN dispatch. Verify shell still works.

5. **`shell: migrate launcher to extensions`** — replace the 5 launcher_* calls (clear, add, set_visible, poll_click, set_running_tile_mask) with PFNs.

6. **`shell: migrate focus + drop internal-IPC includes`** — replace `ipc_call_system_set_focused_client` with `pfnSetWorkspaceFocusedClientEXT`. Delete the now-unused `#include "client/ipc_client.h"`, `#include "client/ipc_client_connection.h"`, `#include "ipc_client_generated.h"`. Delete the `ipc_client_connection_init/fini` calls. The shell is now a pure OpenXR consumer.

## Acceptance criteria for the whole phase

- ✅ `src/xrt/targets/shell/main.c` does not include any `client/ipc_*.h` or `ipc_client_generated.h` headers.
- ✅ `git grep ipc_call_ src/xrt/targets/shell/` returns zero.
- ✅ Shell launches normally as both: (a) orchestrator-spawned controller via Ctrl+Space, (b) direct `displayxr-shell.exe app1 app2` invocation.
- ✅ All shell-mode regression tests from prior phases (capture clients, launcher, window pose, click-to-focus, TAB cycle, DELETE close, scroll-resize) pass with no behavioral change.
- ✅ Windows MSVC CI green; macOS CI green (shell binary is Windows-only, but the public extension headers stay platform-neutral).
- ✅ Branch is one PR, six commits total (or eight including the prequel).

## Hand-off notes

- **The migration is mostly mechanical** — replace a function call with a PFN call. Don't take the opportunity to refactor adjacent code; keep the diff small and reviewable.
- **Watch the IPC connection lifecycle** — the shell currently `ipc_client_connection_init`s before any `ipc_call_*`. After migration, `xrCreateSession` does the equivalent. The order of operations matters: if the shell tries to activate workspace before the session is created, it fails. Make sure the OpenXR scaffolding lands FIRST (Commit 3) and the per-call migrations follow.
- **`XR_NULL_HANDLE` graphics binding behavior** — verify in Commit 3 whether the runtime accepts it. If not and you decide to support it, add to `oxr_session.c` the gate "if session enables `XR_EXT_spatial_workspace` and is authorized as workspace controller, allow `XR_NULL_HANDLE` graphics binding." Document this in the commit message.
- **Test 2 (shell-mode regression)** is the critical validation — every prior shell behavior must survive the migration. Before pushing, drive a full shell launch, click around, open the launcher, click a tile, open Notepad as a capture client, drag it, close it, deactivate and reactivate. Any single regression is a bug to fix in the same branch.
- **If something feels broken**, suspect the `xrCreateSession` graphics binding first. Most subtle bugs in this migration land there.

## Validation: did the public API turn out to be sufficient?

After 2.I lands, audit which functions the shell actually calls vs which the public surface offers:

- If the shell calls every public function, the surface is right-sized.
- If the shell calls only a subset, document which extension functions are unused by the first-party shell — those become candidates for "third-party-only" markers in the spec.
- If the shell can't accomplish something it could do before, that's a missing public function. File it as a high-priority follow-up; the surface is incomplete.

The third case is the failure mode 2.I is designed to detect. If it happens, the gap goes into `XR_EXT_spatial_workspace v6+` design and ships before any third-party controller picks up the surface.
