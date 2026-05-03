# Workspace-Extensions Follow-ups (post 2.D + 2.I)

Outstanding items left over from the Phase 2.D + 2.I + decoupling work. Each item names a suggested phase to fold the fix into, but agents picking up later phases should re-evaluate scope.

## Functional gaps

### 1. Pointer-capture flag stored but not enforced ✅ shipped (2.G C1)

**Resolution:** Folded into Phase 2.G Commit 1. The WM_*BUTTONUP path in `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` now reads `workspace_pointer_capture_enabled` + `workspace_pointer_capture_button` and bypasses the in-rect / dragging gate when capture is held for the matching button. Controller-driven drag affordances keep receiving the release event when the cursor leaves the content rect.

**Limitation:** if the cursor leaves the workspace window entirely (no SetCapture is in flight), Windows routes the up event to whichever window the cursor is over — the WndProc never sees it. Fixing that needs `SetCapture` plumbing on enable / `ReleaseCapture` on disable, which Commit 1 deliberately did not add. File a follow-up if controller drags past the window border start losing release events.

### 2. Per-frame motion events on the public input drain ✅ shipped (2.K)

**Resolution:** Phase 2.K landed the surface. `XR_EXT_spatial_workspace` bumps to spec_version 6 and adds three new event variants on the input drain — `POINTER_MOTION_EXT` (per-frame WM_MOUSEMOVE while pointer capture is enabled, hit-test-enriched), `FRAME_TICK_EXT` (vsync-aligned per-frame timestamp), and `FOCUS_CHANGED_EXT` (focused-client transitions only) — plus `xrRequestWorkspaceClientExitEXT` / `xrRequestWorkspaceClientFullscreenEXT` so controllers can drive lifecycle from custom chrome.

The WndProc no longer skips `WM_MOUSEMOVE` while capture is held, and the pointer-capture setter now drives Win32 `SetCapture` / `ReleaseCapture` so motion outside the workspace window keeps reaching the WndProc during a drag (the residual SetCapture limitation called out in item #1 is closed).

The shell side ports the deleted runtime carousel into a controller-owned state machine — auto-rotation, drag-to-rotate, scroll-radius, TAB-snap, momentum — and adds a per-client animation framework that drives smooth Ctrl+1↔Ctrl+2↔Ctrl+3 preset transitions (300 ms ease-out cubic) plus connect-time glides. Variable poll cadence (16 ms while animating or in carousel, 500 ms idle) keeps idle CPU near zero.

See `spatial-workspace-extensions-phase2K-plan.md` for the full design and `spatial-workspace-extensions-phase2-audit.md` for the per-commit summary.

### 3. Shell activate-failure no longer auto-reconnects (open — now lives in `displayxr-shell-pvt`)

**State:** Before Phase 2.I C10 the shell maintained its own IPC connection and could tear it down + recreate it when activate failed (service died). After C10 the shell has no IPC connection of its own — the runtime DLL holds it. Activate failure now just bails with a "restart the service and press Ctrl+Space" message.

**Why it matters:** A service crash is recoverable today only by exiting and re-launching the shell. Less polished than the pre-migration UX.

**Where to fix:** Shell-side. The reconnect path now requires `xrDestroySession` + `xrCreateSession` (and probably `xrDestroyInstance` + `xrCreateInstance` since the runtime DLL holds connection state on the instance, not the session). Wrap the existing activate-failure path with that recreate logic.

**New tracking location:** This issue now belongs to
`DisplayXR/displayxr-shell-pvt` (Phase 2.J Step 1 moved the shell
source there in 2026-05). Re-file it as a shell-pvt issue when
prioritized.

### 4. `XRT_FORCE_MODE=ipc` shell workaround ✅ shipped (2.J prequel)

**Resolution:** `xrt_application_info` gained an `ext_spatial_workspace_enabled` flag, populated in `oxr_instance.c` from the parsed enabled-extensions list. `target.c::xrt_instance_create` now checks the flag at the top of its dispatch — sessions with `XR_EXT_spatial_workspace` enabled go straight to `ipc_instance_create` regardless of sandbox / env-var state. The shell dropped its `SetEnvironmentVariableA("XRT_FORCE_MODE", "ipc")` call and is now genuinely runtime-agnostic; any OpenXR runtime that surfaces `XR_EXT_spatial_workspace` will see the same enable signal at instance create.

## Repo-extraction residue

These three documented spots in the runtime still mention the shell app by name. They were intentionally left during the Phase 2.I-followup decoupling because the shell directory hasn't physically moved yet. They go away in Phase 2.J.

### 5. `targets/CMakeLists.txt` `add_subdirectory(shell)` ✅ shipped (Phase 2.J Step 1 — shell source extraction)

**Resolution:** Shell source moved to
[DisplayXR/displayxr-shell-pvt](https://github.com/DisplayXR/displayxr-shell-pvt).
`src/xrt/targets/CMakeLists.txt`'s `add_subdirectory(shell)` block
deleted. `installer/DisplayXRShellInstaller.nsi` and the
`shell_installer` CMake target also removed (the shell repo owns its
own installer build now). The runtime side is genuinely
shell-source-free and unblocks the master plan's privacy-collapse step.

### 6. `service_config.c` default binary literal `"displayxr-shell.exe"` ✅ shipped (workspace-controller registry)

**Resolution:** Default `workspace_binary` is now empty. The orchestrator enumerates `HKLM\Software\DisplayXR\WorkspaceControllers\*` (populated by each workspace app's installer) and picks the first registered controller. The runtime owns no specific workspace app — see `docs/specs/workspace-controller-registration.md` for the registration contract.

### 7. Two transitional comments documenting the removed legacy `shell` JSON key ✅ shipped (workspace-controller registry)

**Resolution:** Both comments deleted. The migration window for the pre-rename `"shell"` JSON key has lapsed; users on those builds will pick up the new workspace-controller registry on next install.

## Suggested integration

| Phase | Folds in items |
|---|---|
| **2.G** | ~~#1 (pointer-capture enforcement)~~ ✅ shipped |
| **2.K** | ~~#2 (per-frame motion events + interactive carousel / drag in the shell)~~ ✅ shipped |
| **2.J prequel** | ~~#4 (XRT_FORCE_MODE workaround)~~ ✅ shipped |
| **2.J — installer split** | ~~#6 (default-binary literal), #7 (transitional comments)~~ ✅ shipped via workspace-controller registry |
| **2.J — source-tree move** | ~~#5 (CMake `add_subdirectory(shell)`)~~ ✅ shipped — shell source moved to `displayxr-shell-pvt`. #3 (shell auto-reconnect on activate-failure) lives there now. |

If you're picking up a phase listed above, check this doc and decide whether to fold the relevant item in or defer further. Update this doc when an item lands so the list stays current.
