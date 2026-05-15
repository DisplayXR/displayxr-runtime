# Modal dialog handling under the workspace shell (Windows)

**Status:** Implemented (Tier 0, GH #227). Tier 1 (#228) tracked separately.
**Scope:** Windows. macOS workspace shell is deferred; an analogous NSPanel sheet re-parenting design will follow.
**Related:** [ADR-017](../../adr/ADR-017-modal-dialogs-tiered-strategy.md), [ADR-013](../../adr/ADR-013-universal-app-launch-model.md).

## Problem

Under the workspace shell (`DISPLAYXR_WORKSPACE_SESSION=1`), the runtime hides the app's HWND (`oxr_session.c:2669`, `ShowWindow(SW_HIDE)`). Modal popups the app spawns are owned by that hidden window:

- z-order undefined relative to the shell's fullscreen swap chain (different process);
- focus return broken (modal disable / re-enable target a hidden window);
- taskbar / IME / drag-drop chains find nothing reachable up the parent chain.

## Tier 0 mechanism (in-process owner re-parenting)

Implemented in `src/xrt/state_trackers/oxr/oxr_workspace_modal_win32.c`. Process-global because workspace mode runs one OpenXR session per app process.

### Init (in `oxr_session.c` after the existing SW_HIDE block)

1. Register window class `DisplayXRModalDialogOwner` (once per process).
2. Create an offscreen visible owner window:
   - styles: `WS_POPUP | WS_VISIBLE`
   - extended: `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED`
   - position: (`-32000`, `-32000`), size 1×1, alpha 0 (`SetLayeredWindowAttributes` `LWA_ALPHA`)
   - `WS_VISIBLE` is required so Windows treats it as a real owner candidate for modal disable / re-enable.
3. Install per-thread `WH_CBT` hook (`SetWindowsHookExW(WH_CBT, …, GetCurrentThreadId())`).

### Hook proc

```c
HCBT_CREATEWND:
  if cbt->lpcs->hwndParent == hidden_app_hwnd:
    cbt->lpcs->hwndParent = visible_dialog_owner_hwnd     // mutate IN PLACE
    track(new_hwnd)                                       // for HCBT_DESTROYWND
    if first_modal: notify shell (workspace IPC)

HCBT_DESTROYWND:
  if untrack(hwnd):
    if last_modal: notify shell (workspace IPC)
```

**Synchronous in-place mutation is the whole point.** Using `SetWinEventHook(EVENT_OBJECT_CREATE)` instead would fire *after* the dialog has cached the hidden owner for `EnableWindow`-style modal disable; closing the dialog would then leave focus restoration broken.

**Filter is parent-equality, not a class allowlist.** Deterministic and side-effect-free for unrelated windows: child controls of the dialog have the dialog itself as their parent (not the app HWND), and unrelated top-levels have a different parent (or `NULL`).

### Shell-side IPC (notification only — actual response in `displayxr-shell-pvt`)

- New RPC `session_set_modal_state(bool is_open)` in `proto.json`. Caller is the app process via `openxr_displayxr.dll`; server records the flag on the per-client compositor slot.
- Two new event types in `ipc_workspace_input_event_type`:
  - `IPC_WORKSPACE_INPUT_EVENT_MODAL_OPEN` (8)
  - `IPC_WORKSPACE_INPUT_EVENT_MODAL_CLOSE` (9)
- Both surface in the existing `workspace_enumerate_input_events` poll the controller already runs. Same per-slot delta-vs-`_last_emitted` shadow pattern as `FOCUS_CHANGED` and `WINDOW_POSE_CHANGED`.

The expected shell-side response (implemented in `displayxr-shell-pvt`):

1. Drop the workspace compositor swap chain from topmost / fullscreen-borderless to windowed for the duration. **This is the actual z-order fix** — cross-process `WS_EX_TOPMOST` on the in-app owner doesn't beat another process's fullscreen swap chain reliably.
2. Trigger `xrRequestDisplayModeEXT(XR_DISPLAY_MODE_2D_EXT)` for the focused client (`oxr_api_session.c:1301`) so the 3D window flips to flat presentation while the dialog is up.
3. Dim the focus glow via `workspace_set_client_style`.
4. Suspend cursor raycast hit-tests against the requesting client so clicks can't steal focus from the dialog.
5. On `MODAL_CLOSE`, restore swap chain style, focus mode, glow, and raycasting.

Controllers that ignore the events get reduced UX (flat dialog over still-3D workspace) but apps still function — the in-process re-parenting alone restores focus / taskbar / parent-chain semantics.

## Coverage matrix

| API | Coverage | Why |
|---|---|---|
| `MessageBox(hWnd, …)`, `GetOpenFileName`, `GetSaveFileName`, `ChooseColor`, `ChooseFont`, `PrintDlg` (legacy `comdlg32`) | **Full** | All run in the calling thread; `WH_CBT` catches creation pre-disable. |
| `IFileOpenDialog::Show`, `IFileSaveDialog::Show` (COM) | **Partial** | COM proxies to the shell; the visible HWND may live in `explorer.exe`. In-process portions are re-parented; cross-process portions fall back to flat-OS behavior with the shell's z-order/dim concession. |
| UWP `FileOpenPicker` | **None** | Sandboxed; no in-process HWND to hook. Flat-OS fallback only. |
| `MessageBox(NULL, …)` (parentless) | **None** | Re-parent rule requires `hwndParent == hidden_app_hwnd`; a parentless top-level isn't matched. Same standalone behavior. |
| Custom Qt / wx / Imgui pickers | **N/A** | Render in-app already; no Win32 modal popup involved. |

For the partial / none rows, use Tier 1 (`XR_EXT_workspace_file_dialog`, GH #228) when polished spatial UX matters.

## Frame starvation

The app's render thread is blocked inside the dialog's modal pump, so `xrEndFrame` won't fire. The existing compositor architecture handles this gracefully without any timeout extension:

- Per-view fetch (`comp_d3d11_service.cpp:10495-10548`) detects "client hasn't produced a new frame" via `signaled == c->last_composed_fence_value[eye]` and reuses the persistent atlas slot.
- There is no hard "client hasn't presented for X seconds → kill" path. Client removal is explicit (workspace_deactivate, capture cleanup).
- Diagnostic logs are rate-limited to a 10s rolling window (`comp_d3d11_service.cpp:10637`); a 60s dialog produces ≤6 `[FENCE]` summary lines per client, not per-frame spam.

## Threads, COM workers, and `IFileOpenDialog`

The CBT hook is **per-thread**. We install on the OpenXR session-create thread (typically the app's UI thread). `IFileOpenDialog` instantiates COM workers on separate threads — those threads aren't hooked, which is the root of the "Partial" coverage row above. A future enhancement could re-arm `WH_CBT` on every `DLL_THREAD_ATTACH` in `openxr_displayxr.dll`, but the COM dialog's *visible* HWND often lives in `explorer.exe` anyway, where re-parenting cross-process is a wash. T1 is the right answer for that case, not deeper hooking.

## Why not Tier 2 (universal Win32 interception)

See ADR-017 §"T2 — REJECTED". Apps use Qt / wx / Imgui / raw listboxes, UAC runs on the secure desktop, DRM overlays bypass hooks, IME has its own message loop. Trying to virtualize it all is Wine-scope effort that still leaks. The honest framing is shell-vs-OS: we are a privileged compositor client of Windows, not a replacement for it.

## Verification

- Local mac: `./scripts/build-mingw-check.sh` clean for the Win32 surface (the new TU compiles standalone via direct `x86_64-w64-mingw32-gcc`).
- Windows: `_package\bin\displayxr-shell.exe` launches a `cube_handle_d3d11_win` instrumented with a `B`-key `GetOpenFileName(NULL, …)` smoke. Expected: dialog appears in front, focus returns to cube, 3D resumes.
- `MessageBox(hWnd, "test", "test", MB_OK)` smoke: appears on top, modal disable works correctly.
- 90s open-dialog soak: shell's compositor still presents the requester's last-good frame; no crashes.
- Multi-app: open dialogs from two apps simultaneously. Each dims independently; closing one doesn't restore the other.
