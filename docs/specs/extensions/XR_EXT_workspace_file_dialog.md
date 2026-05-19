# XR_EXT_workspace_file_dialog

| Field | Value |
|---|---|
| **Extension Name** | `XR_EXT_workspace_file_dialog` |
| **Spec Version** | 1 |
| **Authors** | David Fattal (DisplayXR / Leia Inc.) |
| **Status** | Provisional — published with the DisplayXR runtime; subject to revision before Khronos registry submission. Spec is expected to bump once before first release tag as the picker UI lands in `displayxr-shell-pvt`. |
| **Header** | `src/external/openxr_includes/openxr/XR_EXT_workspace_file_dialog.h` |
| **OpenXR Version** | 1.0 |
| **Dependencies** | OpenXR 1.0 core. The session must be running under a workspace controller (`DISPLAYXR_WORKSPACE_SESSION=1`); `XR_EXT_spatial_workspace` is the parent contract that establishes that role on the controller side. |
| **Platforms** | Windows only (Tier 1 spatial picker). The Tier 0 flat-OS fallback path is also Windows-only — macOS workspace mode does not exist yet. |

---

## 1. Motivation

When an OpenXR app runs under a DisplayXR workspace controller, the runtime hides the app's HWND (`ShowWindow(hwnd, SW_HIDE)` at `oxr_session.c:2669`) and the controller puppets it. Any modal popup the app spawns — `GetOpenFileName`, `IFileOpenDialog::Show`, `MessageBox` — is owned by that hidden HWND, which breaks z-order, focus, and the taskbar/IME parent-chain. Tier 0 (ADR-017, #227) handles the mechanical side by re-parenting the popup onto a visible offscreen owner HWND so it survives. That covers ~90% of cases pragmatically.

What Tier 0 cannot do is preserve the 3D presentation: a flat Win32 dialog pops over a workspace window that has just transitioned to 2D. For specific moments worth polishing — file open, save-as, folder pick — we want a spatial-native picker rendered as its own 3D workspace window, adjacent to the requester, with consistent chrome and focus styling.

`XR_EXT_workspace_file_dialog` is that surface. The picker is a peer workspace participant (its own OpenXR handle app), **not** a layer inside the requester's window. Window-space layers (`XRT_LAYER_WINDOW_SPACE`, `xrt_compositor.h:86`) are per-window HUD sized as window-fraction — the wrong substrate for an OS-level picker.

---

## 2. Surface (spec_version 1)

### Lifecycle

- `xrRequestFilePickerEXT(session, &info, &requestId)` — begin an async pick. Returns immediately. The runtime allocates a monotonic 64-bit request ID, forwards the request to the active workspace controller over IPC, and the controller spawns its picker binary (`displayxr-file-picker.exe` in the DisplayXR Shell case). The picker runs as a normal OpenXR workspace participant inheriting the requester's display mode.

### Completion

The app polls `xrPollEvent` for `XR_TYPE_EVENT_DATA_FILE_PICKER_COMPLETE_EXT`. The event carries the matching `requestId`, an `XrFilePickerResultEXT` outcome, and (on success) the picked path as NUL-terminated UTF-8.

| Field | Type | Meaning |
|---|---|---|
| `requestId` | `XrAsyncRequestIdEXT` | Correlates with the `xrRequestFilePickerEXT` out-param. |
| `result` | `XrFilePickerResultEXT` | `SUCCESS_EXT` (path valid), `CANCELLED_EXT`, `PICKER_FAILED_EXT` (process crashed / synthesised by the workspace controller on child-exit without completion), `INVALID_PATH_EXT` (selection did not fit in the path buffer). |
| `path` | `char[512]` | UTF-8. Empty unless `result == SUCCESS_EXT`. Sized to comfortably hold Windows `MAX_PATH`; long-path picks (`\\?\…` style) return `INVALID_PATH_EXT` and the app should fall back to Tier 0. |

Async / event-based on purpose: a blocking entrypoint would deadlock single-threaded render loops and stall `xrWaitFrame`.

### Request parameters

`XrFilePickerInfoEXT` is a flat copyable value — the OpenXR `next` pointer chain is reserved for future use (must be `NULL` in spec_version 1). The IPC codegen does not follow pointer chains across the process boundary.

| Field | Notes |
|---|---|
| `mode` | `OPEN_EXT` / `SAVE_EXT` / `FOLDER_EXT`. |
| `flags` | `XR_FILE_PICKER_FLAG_MULTI_SELECT_BIT_EXT` is reserved for spec_version 2. spec_version 1 implementations may return `XR_ERROR_FEATURE_UNSUPPORTED` if the bit is set. |
| `title` | Window title; empty = picker chooses. |
| `defaultPath` | Starting directory; empty = picker chooses. |
| `filterCount` / `filters[]` | Up to 4 filter rows. Each row carries a user-visible `description` (`char[64]`) and a semicolon-delimited extension list (e.g. `"*.png;*.jpg;*.jpeg"`, `char[64]`). Tightening these vs. the proposal in #228 keeps the IPC payload inside the runtime's 1024-byte RPC budget — bumping it is a future change, not v1. |

### Fallback semantics

Not every workspace controller will ship a picker. The controller publishes its capability via the registry value `SupportsFileDialog = REG_DWORD 1` under its `HKLM\Software\DisplayXR\WorkspaceControllers\<id>` key (see `docs/specs/runtime/workspace-controller-registration.md`).

- **No controller registered, or controller present but capability bit absent.** `xrRequestFilePickerEXT` returns `XR_FILE_PICKER_FALLBACK_TIER0_EXT` (success-class) and writes `XR_NULL_ASYNC_REQUEST_ID_EXT` into `*requestId`. No completion event will be posted. The app is expected to fall back to a flat OS dialog (`GetOpenFileName` / `IFileOpenDialog`); the Tier 0 CBT hook (always installed under workspace mode — see ADR-017) handles z-order and focus restoration onto a visible offscreen owner HWND.
- **Session not under a workspace.** Returns `XR_ERROR_FEATURE_UNSUPPORTED`. Standalone sessions should call `GetOpenFileName` directly with no DisplayXR involvement.

This split keeps the extension surface decisive: a successful path means a completion event is guaranteed (modulo session destruction); the fallback code is the one signal the app needs to switch strategies. No fake events, no sentinel paths.

---

## 3. Lifecycle in detail

```
App                          Runtime                        Controller                       Picker exe
 |  xrRequestFilePickerEXT       |                              |                                 |
 |----------------------------->  |  IPC: file_picker_request    |                                 |
 |  (returns immediately)        |---------------------------->  |  CreateProcess + CLI args       |
 |                               |                              |--------------------------------> |
 |                               |                              |                                 |
 |  per-frame render loop       (workspace_modal_open against requester:                          |
 |  + xrPollEvent              |  dim, drop topmost, optional 3D→2D — same as Tier 0)             |
 |                               |                              |                                 |
 |                               |                              |  picker UI runs as workspace    |
 |                               |                              |  client. User picks / cancels.  |
 |                               |                              |                                 |
 |                               |                              |  IPC: file_picker_result        |
 |                               |   <-------------------------- |  (controller is sole authority) |
 |                               |                              |                                 |
 |                               |  pushes XrEventDataFilePickerCompleteEXT to requesting session  |
 |                               |                              |                                 |
 |  <- xrPollEvent receives -    |                              |                                 |
 |   completion event            |                              |                                 |
```

The controller is the sole authority for `file_picker_result` — the runtime rejects result IPCs from non-controller clients. The picker process never speaks to the runtime directly about results; it speaks to its parent controller via a controller-internal channel (private IPC inside `displayxr-shell-pvt`).

Crash handling:

- **Picker exit without result.** Controller observes the child PID exit, synthesises `result = XR_FILE_PICKER_RESULT_PICKER_FAILED_EXT`, sends the IPC.
- **Requester exit while picker open.** Controller monitors client liveness (M6 graceful-exit), issues `WM_CLOSE` then `TerminateProcess` on the picker child. The runtime drops the outstanding request on session destroy; the late picker result becomes a no-op (logged once).
- **Recursion guard.** The runtime rejects `xrRequestFilePickerEXT` calls from sessions launched by the picker spawn path (the controller sets `DISPLAYXR_WORKSPACE_CLIENT_IS_PICKER=1` on the picker's environment; the runtime gates the entrypoint on its absence).

---

## 4. Versioning

| Version | Change |
|---|---|
| 1 | Initial provisional surface: `xrRequestFilePickerEXT`, `XrEventDataFilePickerCompleteEXT`, fallback result code. Multi-select reserved but unimplemented. |

Expected near-term revisions before any spec-freeze attempt:

- Multi-select (`MULTI_SELECT_BIT_EXT` → variable-length result transport).
- Optional `next`-chain extension struct for picker-side hints (initial focus on a particular tile, recent-files surfacing).
- Spec-version bump once the picker UI lands in `displayxr-shell-pvt` and exercises edge cases of `XrFilePickerInfoEXT` (filter parsing, default-path normalisation).

The struct ABI is intentionally narrow in v1 to leave room.

---

## 5. Out of scope

- **Color / font / print pickers.** Tier 0 already handles these because they are standard `comdlg32` dialogs — the CBT hook re-parents onto a visible owner; z-order and focus restoration work. We only ship sibling extensions if a customer asks.
- **UWP `FileOpenPicker`.** Sandboxed in `explorer.exe`; no in-process HWND to hook, no spawnable handle-app equivalent. Apps should use legacy `GetOpenFileName` under workspace mode.
- **macOS.** Workspace shell does not exist on macOS yet.

---

## 6. References

- ADR-017 — modal-dialogs tiered strategy (T0/T1/T2 decision, T2 rejection rationale).
- `docs/specs/runtime/modal-dialog-handling.md` — Tier 0 mechanism, coverage matrix, COM/UWP boundary.
- `docs/specs/runtime/workspace-controller-registration.md` — capability flag schema, controller discovery.
- Issue [#228](https://github.com/DisplayXR/displayxr-runtime/issues/228) — Tier 1 design and distribution decisions (picker ships in shell installer, not runtime).
