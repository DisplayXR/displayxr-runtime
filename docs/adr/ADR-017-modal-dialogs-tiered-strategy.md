# ADR-017: Tiered strategy for Win32 modal dialogs under the workspace shell

**Status:** Accepted
**Date:** 2026-05-15
**Issues:** #227 (Tier 0), #228 (Tier 1)
**Related:** [ADR-013 Universal App Launch Model](ADR-013-universal-app-launch-model.md)

## Context

ADR-013 established the universal app-launch model: the runtime hides the app's HWND under the workspace shell (`ShowWindow(SW_HIDE)`, `oxr_session.c:2669`) and the shell puppets it via `SetWindowPos` / `PostMessage`. This works for window resize and input, but it breaks any modal popup the app spawns — `GetOpenFileName`, `IFileOpenDialog::Show`, `MessageBox(hWnd, …)`. The popup is owned by a hidden HWND, which leaves:

- z-order undefined relative to the shell's fullscreen swap chain (in `displayxr-service.exe`, a different process);
- focus restoration broken (Windows disables and re-enables a hidden owner; closing the dialog leaves focus nowhere visible);
- taskbar / IME / drag-drop chains broken (they walk up the parent chain and find nothing reachable).

Apps like `displayxr-demo-gaussiansplat` work standalone but can't load files in workspace mode. This is the **shell-vs-OS boundary**: the workspace controller is a privileged compositor client of Windows, not a replacement for the Windows shell. We need a story for modal popups that respects that boundary.

## Decision

Three explicit tiers, ordered by ambition. We commit to T0 + T1 and **explicitly reject T2.**

### T0 — In-process owner-rewrite (universal, no app changes)

Right after `ShowWindow(SW_HIDE)`, the runtime creates an offscreen `WS_VISIBLE | WS_POPUP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED` (alpha 0) "dialog owner" window in the app process and installs a per-thread `WH_CBT` hook on the OpenXR session's UI thread. The hook handles `HCBT_CREATEWND` synchronously, *before* the dialog is materialized, and rewrites `lpcs->hwndParent` from the hidden app HWND to the visible dialog owner. Filter: parent-equality (`==` hidden app HWND), not a class allowlist. That filter is deterministic — child controls (parent is the dialog) and unrelated top-levels (parent is `NULL` or some other HWND) are left alone.

`WH_CBT` over `SetWinEventHook(EVENT_OBJECT_CREATE)` is essential: `EVENT_OBJECT_CREATE` fires *after* the dialog has cached the (hidden) owner for `EnableWindow`-style modal disable, leaving focus restoration broken.

The hook also notifies the workspace controller via a new IPC notification (`session_set_modal_state(is_open)`, ref-counted across nested popups, surfaces as `IPC_WORKSPACE_INPUT_EVENT_MODAL_OPEN / _CLOSE` in the existing `workspace_enumerate_input_events` channel). The shell side responds by:

- dropping its compositor swap chain from topmost / fullscreen-borderless to windowed for the duration (the actual z-order fix — cross-process `WS_EX_TOPMOST` doesn't beat another process's fullscreen swap chain reliably);
- triggering the existing `xrRequestDisplayModeEXT(XR_DISPLAY_MODE_2D_EXT)` path for the focused client so the 3D window flips to flat presentation;
- dimming the focus glow and suspending cursor raycast hit-tests against the requesting client so clicks can't steal focus from the dialog.

Coverage matrix: full for legacy `comdlg32` (`GetOpenFileName`, `MessageBox`); partial for COM `IFileOpenDialog` (visible HWND may live in `explorer.exe`, cross-process portions fall back to flat-OS behavior); none for UWP `FileOpenPicker` (sandboxed). See `docs/specs/runtime/modal-dialog-handling.md` for the full coverage table.

Frame starvation: existing infrastructure handles it. The compositor's per-view fetch path (`comp_d3d11_service.cpp:10495-10548`) detects "client hasn't produced a new frame" and reuses the persistent atlas slot; there is no hard timeout to extend. Diagnostic logs are already rate-limited to 10s windows.

### T1 — Spatial-native picker as a peer workspace window (opt-in)

For specific moments worth polishing — file open, color, settings — we provide an `XR_EXT_workspace_file_dialog` extension. The picker is a **separate OpenXR handle app** (`displayxr-file-picker.exe`, shipped from `displayxr-shell-pvt`) that participates in the workspace exactly like any other app. Async / event-based API: `xrRequestFilePickerEXT` returns immediately with an `XrAsyncRequestIdEXT`; the app polls `xrPollEvent` for `XR_TYPE_EVENT_DATA_FILE_PICKER_COMPLETE_EXT`. Blocking inside the runtime would deadlock single-threaded render loops.

Window-space layers (`XRT_LAYER_WINDOW_SPACE`, `xrt_compositor.h:86`) are per-window HUD sized as window-fraction — the wrong substrate for a picker. The picker is a peer window, not a layer. Tier 1 reuses Tier 0's modal mechanism for dim / input-gate / 3D→2D against the requester, so a polished picker still feels like a focused operation.

Tier 1 is a sibling extension to `XR_EXT_app_launcher`, **not** an extension of it — launcher's `BROWSE_EXT` returns *an app to launch*; picker returns *a file path*. Different schema, different placement.

### T2 — Universal Win32 interception — REJECTED

We do not detour `CreateWindowEx` / virtualize the desktop / COM-intercept `IFileOpenDialog` system-wide:

- Apps use Qt / wx / Imgui / raw listboxes, not just common dialogs.
- UAC runs on the secure desktop; we cannot touch it.
- DRM overlays, IME windows, tooltips bypass any hook approach.
- This is Wine-scope effort, and even Wine leaks.

This rejection is durable, not a "wait and see." The honest framing is: **the workspace controller is a privileged compositor client of Windows, not a replacement for the Windows shell.** Treat owned popups as a first-class case (T0), offer spatial-native paths only where polish pays back the spec/IPC cost (T1), and accept the rest as flat-OS fallback.

## Consequences

- The runtime grows a small Win32-only TU (`oxr_workspace_modal_win32.c`) and one new IPC RPC (`session_set_modal_state`) plus two new event types in the existing workspace event channel.
- Workspace controllers must handle `IPC_WORKSPACE_INPUT_EVENT_MODAL_OPEN / _CLOSE` to provide a coherent visual response (dim, drop topmost, 3D→2D). Controllers that ignore them get reduced UX (flat dialog over still-3D workspace) but apps still function.
- T1 is additive — apps without `XR_EXT_workspace_file_dialog` get T0 fallback; no breakage.
- T0 is best-effort across the COM / UWP boundary. `displayxr-demo-gaussiansplat` and similar legacy `comdlg32` apps get full coverage; modern shell-hosted dialogs and sandboxed pickers fall back to flat-OS behavior.
- We do not ship a Windows-replacement shim layer. Apps that need OS-level desktop integration should expect it to look like flat OS, not 3D.
