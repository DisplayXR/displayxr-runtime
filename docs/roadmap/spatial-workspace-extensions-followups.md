# Workspace-Extensions Follow-ups (post 2.D + 2.I)

Outstanding items left over from the Phase 2.D + 2.I + decoupling work. Each item names a suggested phase to fold the fix into, but agents picking up later phases should re-evaluate scope.

## Functional gaps

### 1. Pointer-capture flag stored but not enforced

**State:** `xrEnableWorkspacePointerCaptureEXT(button)` and `xrDisableWorkspacePointerCaptureEXT()` ship in `XR_EXT_spatial_workspace v4`. The bridge sets a flag (`workspace_pointer_capture_enabled` + `workspace_pointer_capture_button`) on the d3d11 window. The WndProc click-handling path **does not yet read this flag** — out-of-content button-up events are still filtered as if capture were never requested.

**Why it matters:** Real drag affordances (controller-driven move/resize) need button-up events to keep flowing even when the cursor leaves any window. Today a controller calling Enable + dragging past the workspace bounds loses the release event.

**Where to fix:** `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` — the WM_LBUTTONUP / WM_RBUTTONUP / WM_MBUTTONUP path that already filters out-of-content events. Gate the filter on `workspace_pointer_capture_enabled && workspace_pointer_capture_button == evt.button`.

**Suggested phase:** Phase 2.G Commit 1 (the WndProc input filtering path is the same area). Otherwise its own micro-PR — small change.

### 2. HOVER events never emitted

**State:** `XR_WORKSPACE_INPUT_EVENT_POINTER_HOVER_EXT` enum value, `XrWorkspaceInputEventEXT.pointerHover` struct variant, and the matching wire-format `ipc_workspace_input_event::u.pointer_hover` are all defined and ship in v4. **Nothing pushes them.** Per Phase 2.D's design (region-transition events only, no per-frame motion), the WndProc would need to track current_hit_region per WM_MOUSEMOVE tick and push a HOVER when it changes — but the cross-thread call from WndProc into `workspace_raycast_hit_test` is non-trivial (it acquires the service-side `render_mutex`).

**Why it matters:** Controllers wanting hover-driven UI today have to poll `xrWorkspaceHitTestEXT` per frame. That's more API surface than they should need to use for a basic feature.

**Where to fix:** Either (a) add a hit-test pass on the d3d11_window WndProc thread (needs careful threading), or (b) emit cursor-position events at lower fidelity (per WM_MOUSEMOVE tick) and let the service-side drain enrich each into a HOVER event by hit-testing at drain time.

**Suggested phase:** Phase 2.C (chrome rendering) — the chrome system needs hover for button highlighting, so it'll force this design decision naturally.

### 3. Shell activate-failure no longer auto-reconnects

**State:** Before Phase 2.I C10 the shell maintained its own IPC connection and could tear it down + recreate it when activate failed (service died). After C10 the shell has no IPC connection of its own — the runtime DLL holds it. Activate failure now just bails with a "restart the service and press Ctrl+Space" message.

**Why it matters:** A service crash is recoverable today only by exiting and re-launching the shell. Less polished than the pre-migration UX.

**Where to fix:** Shell-side. The reconnect path now requires `xrDestroySession` + `xrCreateSession` (and probably `xrDestroyInstance` + `xrCreateInstance` since the runtime DLL holds connection state on the instance, not the session). Wrap the existing activate-failure path with that recreate logic.

**Suggested phase:** Phase 2.J (shell repo extraction) — the cleanup naturally happens when the shell is its own repo and can iterate freely. Until then, it's a known UX regression.

### 4. `XRT_FORCE_MODE=ipc` shell workaround

**State:** `shell_openxr.cpp::shell_openxr_init` calls `SetEnvironmentVariableA("XRT_FORCE_MODE", "ipc")` before `xrCreateInstance`. This forces the DisplayXR hybrid runtime to pick the IPC client compositor over the in-process native one. **It's runtime-specific** — a workspace controller running against any other OpenXR runtime wouldn't need it (and wouldn't have an `XRT_FORCE_MODE` env var to set).

**Why it matters:** Once the shell forks to its own repo, this hack is awkward — the shell is supposed to be runtime-agnostic. The cleaner solution is for the runtime to detect a workspace-controller session and pick IPC mode automatically (e.g., when `XR_EXT_spatial_workspace` is in the enabled extension list).

**Where to fix:** `src/xrt/auxiliary/util/u_sandbox.c::u_sandbox_should_use_ipc` — add a check: if the calling instance has `XR_EXT_spatial_workspace` enabled, return `true`. Or hand the decision to `xrt_instance_create` based on the createInfo's enabled-extension list (cleaner but requires more plumbing).

**Suggested phase:** Phase 2.J prequel (before extracting the shell) — once fixed, the shell drops the SetEnvironmentVariableA call and is genuinely runtime-agnostic.

## Repo-extraction residue

These three documented spots in the runtime still mention the shell app by name. They were intentionally left during the Phase 2.I-followup decoupling because the shell directory hasn't physically moved yet. They go away in Phase 2.J.

### 5. `targets/CMakeLists.txt` `add_subdirectory(shell)`

**State:** A comment marks it as the "legacy name" with a TODO to remove when the binary moves. The line itself stays as long as `src/xrt/targets/shell/` exists in this repo.

**Where to fix:** Phase 2.J commit that removes `src/xrt/targets/shell/` entirely.

### 6. `service_config.c` default binary literal `"displayxr-shell.exe"`

**State:** The orchestrator uses this string as the default binary to spawn when no config exists. Once the shell ships from a separate repo, this default either:
- Becomes config-only (no compiled-in default).
- Updates to whatever name the new repo's binary uses.
- Drops to NULL and the orchestrator simply doesn't auto-spawn.

**Where to fix:** Phase 2.J. The decision is partly product (do we want auto-spawn out of the box?).

### 7. Two transitional comments documenting the removed legacy `shell` JSON key

**State:** `src/xrt/targets/service/service_config.c:181` and `src/xrt/targets/service/service_config.h:37` have one-line comments noting that the legacy `"shell"` JSON key was removed in Phase 2.I-followup decoupling. Kept for users discovering the breaking change.

**Where to fix:** Delete after the migration window settles (~3 months post-2.I publication). Tiny commit, can ride with any other cleanup.

## Suggested integration

| Phase | Folds in items |
|---|---|
| **2.G** | #1 (pointer-capture enforcement) |
| **2.C** | #2 (HOVER events) |
| **2.J prequel** | #4 (XRT_FORCE_MODE workaround) |
| **2.J** | #3, #5, #6 (shell extraction, residues) |
| **(any time)** | #7 (transitional comments) |

If you're picking up a phase listed above, check this doc and decide whether to fold the relevant item in or defer further. Update this doc when an item lands so the list stays current.
