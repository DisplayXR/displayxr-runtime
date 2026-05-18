# Handoff: #234 / #233 — workspace acked-flip + curtain (continuation)

## Branches in flight

Both branches are uncommitted-since-last-commit. Continue here.

| Repo | Branch | Last commit |
|---|---|---|
| `openxr-3d-display` | `workspace-acked-flip-curtain` (off main) | `30e5648a1` workspace: acked-flip + curtain (#233/#234) |
| `displayxr-shell-pvt` | `shell-v-hotkey-mode-toggle` (off main) | `43fd71b` shell: V hotkey toggles display 2D/3D via xrRequestDisplayRenderingModeEXT |

**Uncommitted runtime changes** (on top of `30e5648a1`):
- `src/xrt/state_trackers/oxr/oxr_objects.h`: new `oxr_session::is_active_workspace_controller` flag
- `src/xrt/state_trackers/oxr/oxr_workspace.c`: set/clear flag on `xrActivateSpatialWorkspaceEXT`/`Deactivate`
- `src/xrt/state_trackers/oxr/oxr_api_session.c`: gate at L1372 now exempts `is_active_workspace_controller`; new branch routes mode flip through `xsysc->request_workspace_mode_flip` hook
- `src/xrt/include/xrt/xrt_compositor.h`: new `xrt_system_compositor::request_workspace_mode_flip` function pointer
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.{cpp,h}`:
  - `comp_d3d11_service_workspace_request_mode_flip` extern C function
  - `system_request_workspace_mode_flip` set on xsysc vtable
  - `xrt_device_set_property(OUTPUT_MODE, target)` added to apply_pending's FLIPPING transition
  - `MFP_HW_CEILING_FRAMES` bumped 16 → 60
  - Diagnostic U_LOG_W at hook entry + silent no-op path
- `src/xrt/ipc/shared/proto.json`: new `workspace_request_display_mode` RPC
- `src/xrt/ipc/server/ipc_server_handler.c`: `ipc_handle_workspace_request_display_mode` calls `comp_d3d11_service_workspace_request_mode_flip`; `ipc_handle_workspace_activate` now force-resets to mode 1 via the same hook
- `src/xrt/ipc/client/ipc_client_compositor.c`: `ipc_syscomp_request_workspace_mode_flip` marshals to server; wired into `c->system.request_workspace_mode_flip`; `<stdbool.h>` included
- `test_apps/cube_handle_d3d11_win/main.cpp`: mirrors `xr.currentModeIndex` → `g_inputState.currentRenderingMode` so runtime-driven mode changes update the cube's rendering + HUD (NOT YET propagated to cube_handle_d3d12/gl/vk)

**Build config**: build dir has `/Zi /DEBUG` linker flags injected via `CMakeCache.txt` for PDB generation. Cache only — not in git. To revert, reconfigure cleanly: `"C:\Program Files\CMake\bin\cmake.exe" -DCMAKE_BUILD_TYPE=Release "C:\Users\Sparks i7 3080\Documents\GitHub\openxr-3d-display\build"` (without the /Zi flags).

## Architectural principle (user)

> An app should be required to handle any render-mode change on its own,
> AND when summoned via workspace it should sync render mode with the
> workspace's render mode at session start.

Two responsibilities:

1. **App-driven sync from events.** Every app must update its rendering state from `XrEventDataRenderingModeChangedEXT`. This is what the cube test app was missing (event handler updated `xr.currentModeIndex`, but the rendering logic read `g_inputState.currentRenderingMode` — partially fixed via mirror in `cube_handle_d3d11_win/main.cpp` this session; needs to propagate to other variants).

2. **Initial-state sync on session start.** When an app's session begins under a workspace, the runtime must communicate the current rendering mode immediately (not after the next user-driven flip). Options:
   - Runtime pushes `XrEventDataRenderingModeChangedEXT` (with a sentinel `previousModeIndex` if needed) at `xrBeginSession` for workspace clients
   - App queries an explicit "current mode" API at startup
   - Add an `isActive` field to `XrDisplayRenderingModeInfoEXT` so apps can find the active mode from `xrEnumerateDisplayRenderingModesEXT`

Current workaround: `ipc_handle_workspace_activate` resets to mode 1 so the controller and any new clients start aligned with the LeiaSR 3D default. This works but is opinionated — the cleaner long-term answer is option 1 or 3 so apps just learn the truth instead of being forced into mode 1.

## What's working

- **gauss demo L-key modal** flips 3D→2D→3D cleanly with curtain masking. Confirmed by user.
- **Cube close-button no longer crashes.** The use-after-free at `comp_d3d11_service.cpp:8213` (`sc->images[i].texture->GetDesc()` on freed swapchain) is fixed by taking strong `xrt_swapchain_reference` on `ws_snapshot[].sc_array[]` and releasing on slot unregister under render_mutex.
- **Shell V hotkey reaches the runtime.** Full chain works: shell `WM_HOTKEY V` → `xrRequestDisplayRenderingModeEXT` → OXR gate exempts active workspace controller → my new branch → `request_workspace_mode_flip` hook → IPC RPC `workspace_request_display_mode` → `comp_d3d11_service_workspace_request_mode_flip` → `multi_compositor_request_mode_flip` (acked-flip + curtain).
- **Workspace activate resets to mode 1** so Ctrl+Space lands in a deterministic 3D state regardless of what mode the previous session left the runtime in.
- **Cube test app HUD updates** when the runtime drives a mode change (via the new state mirror in main loop).

## What's still broken

### 1. Vendor SDK poll fires counter-correction flips (HIGH PRIORITY — blocks V toggle)

**Symptom**: User presses V to flip 3D→2D. Runtime flips successfully. ~200 ms later runtime flips BACK to 3D without user input. User can flip 3D→2D once but "can't switch back".

**Log signature**:
```
[mode_flip] hook: entering target=0          ← shell V press
[mode_flip] request 1 -> 0 — curtain ON
[mode_flip] FLIPPING: DP request_display_mode(0)
                                              ← ~180ms passes
[mode_flip] request 0 -> 1 — curtain ON       ← NO "hook: entering" — internal trigger
[mode_flip] FLIPPING: DP request_display_mode(1)
```

**Diagnosis**: The vendor SDK poll path in `comp_d3d11_service.cpp` (`compositor_layer_commit`, ~L10470) compares vendor `is_3d` against `sys->hardware_display_3d`. My acked-flip writes `sys->hardware_display_3d = target_is_3d` in `apply_pending`'s FLIPPING transition, but the vendor's cached state may still report the OLD value. The poll sees mismatch in the OPPOSITE direction and fires `request_mode_flip` to "correct" — pulling the mode back to where it was. The existing `pending_flip` gate releases when `mode_flip.phase == MFP_IDLE`, but the cached vendor state hasn't caught up by then.

**Fix candidates**:
- Add a post-flip cooldown timer (e.g., 2 seconds after IDLE) during which the vendor poll cannot fire `request_mode_flip`. Simplest.
- On my flip-land, also update `sys->cached_3d_state` to the new value so the poll sees no mismatch. May poison the cache vs reality.
- Make the vendor poll itself route through `request_mode_flip` always (no-op guard catches same-target). Could still ping-pong.

Cooldown is probably right. Look at how `sys->last_3d_state_poll_ns` is used and add a `last_flip_landed_ns` sibling.

### 2. V transition oscillation when app doesn't ack

**Symptom**: During the curtain window, content visibly oscillates between L/R views even though both eyes should be identical mono content.

**Root cause** (per-slot stride coupled to global `tile_columns`): the workspace per-tile blit at `comp_d3d11_service.cpp:7823` reads each per-client atlas with stride = `src_tex_w / sys->tile_columns`. When the global mode flips from 3D (tile_columns=2) to 2D (tile_columns=1) but a client hasn't acked yet (still writes 2-view stereo into its slot atlas), the workspace reads the full atlas as ONE tile, mashing both eyes' content together.

The "standalone V works" comparison the user pointed out: standalone has only ONE buffer pipeline (app swapchain → DP), so the per-tile blit stride mismatch never occurs. Shell mode introduces a `combined_atlas` middle layer where the stride matters.

**Proper fix**: decouple per-slot stride from `sys->tile_columns`. Per-slot stride should be derived from the slot's submitted projection layer view rects (`l->data.proj.v[].sub.rect` snapshot — cube's view[1] is at offset (1539,0) so slot stride = 2 regardless of global mode). The workspace's per-tile blit iterates `sys->tile_columns × tile_rows` destination tiles; for each destination tile, read from the slot's view[col_for_eye] using the SLOT's stride, not the global. This is the "right architecture" fix.

**Band-aid that's in place**: bumped `MFP_HW_CEILING_FRAMES` from 16 → 60 so the curtain holds longer, masking more of the catch-up window. Doesn't fix the underlying coupling.

### 3. Ctrl+Space teardown crash (#238)

Already filed: https://github.com/DisplayXR/displayxr-runtime/issues/238. Same class of UAF as the close-button bug. Close-button is fixed; Ctrl+Space (workspace deactivate) has the same race in a different teardown path — `ipc_server_per_client_thread.c:510` drops swapchain refs without `sys->render_mutex`, the deactivate path tears down per-client compositors without the mutex either. Workaround for now: exit via tray icon → Exit, not Ctrl+Space.

### 4. Cube state mirror only on d3d11 variant

`test_apps/cube_handle_d3d11_win/main.cpp` has the mirror. Need to propagate the same 4-line block to `cube_handle_d3d12_win`, `cube_handle_gl_win`, `cube_handle_vk_win`. Best done as a helper in `test_apps/common/` so future variants stay in sync. The mirror:

```c
if (xr.currentModeIndex != g_inputState.currentRenderingMode &&
    xr.currentModeIndex < xr.renderingModeCount) {
    g_inputState.currentRenderingMode = xr.currentModeIndex;
}
```

### 5. Bare V global hotkey is intrusive

The shell registers bare `V` (no modifier) as a global hotkey. While the shell is running, V is stolen from every other foreground app. User explicitly said: "once this works we will switch to Ctrl+V to avoid switching mode when doing regular typing". One-line change in `displayxr-shell-pvt/src/main.c:2892`: `RegisterHotKey(g_msg_hwnd, HOTKEY_MODE_TOGGLE, MOD_CONTROL | MOD_NOREPEAT, 'V')`.

## Key files modified

Runtime:
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.{cpp,h}` — state machine, hook, helpers
- `src/xrt/state_trackers/oxr/oxr_api_session.c` — gate exemption for controllers
- `src/xrt/state_trackers/oxr/oxr_workspace.c` — set/clear `is_active_workspace_controller`
- `src/xrt/state_trackers/oxr/oxr_objects.h` — new session flag
- `src/xrt/include/xrt/xrt_compositor.h` — new fn pointer on xsysc
- `src/xrt/ipc/shared/proto.json` — new RPC
- `src/xrt/ipc/server/ipc_server_handler.c` — server handler + reset-on-activate
- `src/xrt/ipc/client/ipc_client_compositor.c` — client wrapper, vtable wire
- `test_apps/cube_handle_d3d11_win/main.cpp` — state mirror
- `docs/specs/runtime/workspace-controller-registration.md` — display mode authority subsection

Shell:
- `src/main.c` — V hotkey + handler
- `src/shell_openxr.{h,cpp}` — PFN bundle additions

## Verification matrix

| Case | Status |
|---|---|
| gauss demo L-key modal in shell | ✅ user confirmed clean |
| cube close-button | ✅ no crash (was: UAF) |
| cube Ctrl+Space (workspace exit) | ❌ still crashes (#238) |
| shell V toggle 3D→2D | ⚠️ runtime flips correctly, visual oscillates during curtain (issue 2) |
| shell V toggle 2D→3D | ❌ vendor poll counter-correction undoes it (issue 1) |
| shell startup after V test | ✅ resets to mode 1 |
| cube HUD on runtime-driven mode change | ✅ HUD updates (state mirror works) |
| Bridge V-toggle | not tested in this session |
| Legacy IPC V-toggle (non-workspace) | not tested in this session |

## Memory notes added/strengthened during this session

- `feedback_dll_version_mismatch.md` — strengthened with a 4-step checklist + git_tag verify step. After ANY rebuild deploy BOTH `service.exe` AND `DisplayXRClient.dll` together; mismatch = `IPC_IGNORE_VERSION=1` error or `ReadFile: 109 The pipe has been ended`.

## Build/deploy cheat sheet

```bash
# 1. Kill before rebuild (build_windows.bat can't overwrite locked exes)
powershell -c "Get-Process displayxr-service,displayxr-shell -ErrorAction SilentlyContinue | Stop-Process -Force"

# 2. Build
"C:/Users/Sparks i7 3080/Documents/GitHub/openxr-3d-display/scripts/build_windows.bat" build

# 3. Deploy pair (both binaries together)
cp _package/bin/displayxr-service.exe _package/bin/DisplayXRClient.dll "/c/Program Files/DisplayXR/Runtime/"

# 4. Verify git_tag match (saves a debugging cycle if they diverge)
grep -ao "v[0-9]\+\.[0-9]\+\.[0-9]\+-[0-9]\+-g[0-9a-f]\+" "/c/Program Files/DisplayXR/Runtime/displayxr-service.exe" | head -1
grep -ao "v[0-9]\+\.[0-9]\+\.[0-9]\+-[0-9]\+-g[0-9a-f]\+" "/c/Program Files/DisplayXR/Runtime/DisplayXRClient.dll"  | head -1

# 5. For shell-side changes (separate repo)
cd "/c/Users/Sparks i7 3080/Documents/GitHub/displayxr-shell-pvt"
scripts/build-shell.bat
cp _package/bin/displayxr-shell.exe "/c/Program Files/DisplayXR/Runtime/"

# 6. Restart
"_package/bin/displayxr-service.exe" --shell &
"/c/Program Files/DisplayXR/Runtime/displayxr-shell.exe" \
  test_apps/cube_handle_d3d11_win/build/cube_handle_d3d11_win.exe
```

## Debugging with PDBs

```bash
# Procdump catches the crash + writes dump
"/c/Users/Sparks i7 3080/Tools/procdump64.exe" -accepteula -e -ma -x /tmp \
  "_package/bin/displayxr-service.exe" --shell

# Analyze
CDB="/c/Program Files/WindowsApps/Microsoft.WinDbg_1.2603.20001.0_x64__8wekyb3d8bbwe/amd64/cdb.exe"
"$CDB" -y "_package/bin" -srcpath "C:\\Users\\Sparks i7 3080\\Documents\\GitHub\\openxr-3d-display" \
  -z /tmp/displayxr-service*.dmp \
  -c ".lines -e; .ecxr; .srcfix; ln eip; kPnL 20; q"
```

The PDB next to the exe at `_package/bin/displayxr-service.pdb` (copy alongside if not present) is what cdb resolves against.
