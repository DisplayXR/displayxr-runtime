---
status: Active
owner: David Fattal
updated: 2026-11-23
anchors: main @ 985385f6e (v2.70.4)
code-paths: [src/xrt/ipc/server/ipc_server_input_queue.c, src/xrt/ipc/server/ipc_server_linux_input.c, src/xrt/ipc/server/ipc_server_macos_appkit.m, src/xrt/compositor/d3d11/comp_d3d11_window.cpp, src/xrt/state_trackers/oxr/oxr_workspace.c]
---
# Workspace input routing

How the pointer and keyboard reach a content app that runs inside a workspace controller
(the DisplayXR Shell, or any registered equivalent), on each platform.

## 1. The problem

An app launched into the workspace gets `DISPLAYXR_WORKSPACE_SESSION=1` in its environment
(set by the service orchestrator's spawn, `service_orchestrator.c`, and by the controller for
the apps it launches; see `xrt_instance.h`). It runs as an IPC client, and its own window is
never shown:

- Windows: the runtime hides the app's HWND at session creation
  (`ShowWindow(hwnd, SW_HIDE)` in `src/xrt/state_trackers/oxr/oxr_session.c`).
- Desktop Linux: the runtime withdraws an X11 window itself; a Wayland app has to create
  its window hidden (`src/xrt/state_trackers/oxr/oxr_workspace_window_linux.c`, which
  warns when it cannot). `DxrLinuxWindow` in `displayxr-common` does this through
  `DxrLinuxWindowDesc::start_hidden`.

The service composites the app's swapchain into the shared spatial surface. So the OS
delivers the user's pointer and keyboard to the **service's** surface, never to the app's
window. Unless the service forwards input, the app gets none.

## 2. Per platform

| | Windows | Desktop Linux, macOS |
|---|---|---|
| Delivery | **Push.** The service `PostMessage`s each event into the app's hidden HWND. | **Pull.** The service queues events per client; the app drains its queue. |
| Where | WndProc of the service window, `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | `ipc_server_input_route()`, `src/xrt/ipc/server/ipc_server_input_queue.c` |
| App change | None. Input arrives as ordinary window messages in the app's own message loop. | Enable `XR_DXR_workspace_client_input` and drain it (or use `DxrLinuxWindow`, which does). |
| Extension | Not advertised. | `XR_DXR_workspace_client_input`, spec 1, advertised only on desktop Linux and macOS. |
| Lone Ctrl | Held back until the chord is known (#667). | Forwarded at once (gap, #2781). |

### 2.1 Windows: post into the hidden HWND

The D3D11 service window's WndProc forwards each key and mouse message to the target app's
HWND with `PostMessage`. Mouse coordinates are remapped from the workspace window into the
app window through its tile rect; a pointer outside the rect is not forwarded. Since
`PostMessage` cannot carry keyboard state across processes, the modifier mask is stamped
into spare `lParam` bits, and a `WH_GETMESSAGE` hook the runtime installs in the app
re-applies it before the app's own `TranslateMessage` (`oxr_workspace_modal_win32.c`). Because the app sees normal window messages, nothing in the
app changes: any Win32 message loop, engine or legacy app gets its input this way.

(Captured 2D windows are the one exception: their input goes through a ring and
`SendInput`, see `struct workspace_input_event` in `comp_d3d11_window.h`.)

**Lone Ctrl (#667).** The reserved-key table matches the *terminating* key of a chord, so
the bare Ctrl-down of a controller chord such as Ctrl+L would reach the app before the
chord completes. When the controller reserves any Ctrl chord, the WndProc buffers the
Ctrl prefix and forwards it only once the next key proves it is not a controller chord, or
on Ctrl-up, or after a short timeout (`kb_has_ctrl_reserved`, set in
`comp_d3d11_window_set_reserved_keys()`).

### 2.2 Desktop Linux and macOS: queue and drain

Wayland has no way for one client to post input into another client's surface, so the
Windows design does not carry over. Instead the service routes each event into a
per-client queue and the app pulls its share.

- **Producers.** Desktop Linux: the service window's input (native Wayland or X11) is
  translated to Win32 VK codes and routed in `ipc_server_linux_input.c`. macOS: the AppKit
  pump in `ipc_server_macos_appkit.m`. Both call `ipc_server_input_route()`.
- **Queues.** One ring per target (`ipc_server_input_queue_push()`): the controller's
  (`IPC_INPUT_TARGET_CONTROLLER`) plus one per content client, keyed by its per-session
  compositor. A queue is dropped when its client disconnects.
- **Drain.** The app calls `xrEnumerateWorkspaceClientInputEventsDXR` (state tracker:
  `src/xrt/state_trackers/oxr/oxr_workspace.c`; IPC call
  `workspace_client_enumerate_input_events`; client translation in
  `src/xrt/ipc/client/ipc_client_compositor.c`). It is a destructive drain, at most 16 events
  per call; call it once per frame until fewer than the capacity come back.
- **Position.** A content app's copy of a pointer, motion or scroll event carries the
  cursor normalized to its window content, `[0,1]` with a top-left origin
  (`comp_multi_workspace_window_local_uv()`). During a drag that started on the app the
  value is not clamped and may fall outside `[0,1]`.

Extension spec: [XR_DXR_workspace_client_input](../specs/extensions/XR_DXR_workspace_client_input.md).
Header: `src/external/openxr_includes/openxr/XR_DXR_workspace_client_input.h`.
Landed in #2779, released in v2.70.4.

**`DxrLinuxWindow` (displayxr-common v2.36.0).** `DxrLinuxWindow::attach_session()` resolves
the drain function; `pump()` / `pump_events()` drain it every frame and emit ordinary
`DxrWindowEvent`s, so an app that already handles its window's events needs no other change.
The pure conversions live in `common/linux/dxr_workspace_input.h`:

| Wire | `DxrWindowEvent` | Helper |
|---|---|---|
| Win32 VK code | X11 keysym at shift level 0 (`'A'` becomes `XK_a`) | `vk_to_keysym()` |
| button 1 left, 2 right, 3 middle | button 1 left, 2 middle, 3 right (2 and 3 swapped) | `button_from_wire()` |
| normalized content position | content buffer pixels, rounded, not clamped | `uv_to_px()` |
| wheel notches, + = up | `scroll_steps`, fractions round away from zero | `scroll_steps_from_wire()` |

## 3. The routing policy

Both designs apply the same rules. They are two implementations, not one shared function:
Windows in `comp_d3d11_window.cpp`, desktop Linux and macOS in `ipc_server_input_route()`
(one function shared by those two platforms). The reserved-key IPC call is common:
`ipc_handle_workspace_set_reserved_keys()` in `ipc_server_handler.c` hands the table to
`comp_d3d11_service_workspace_set_reserved_keys()` on Windows and to
`ipc_server_input_queue_set_reserved_keys()` on the shared-surface service.

- **Keys** go to the focused client, unless the chord is reserved.
- **Reserved chords** are never forwarded to an app. The controller registers its own set
  with `xrSetWorkspaceReservedKeysDXR` (`XR_DXR_spatial_workspace` spec 24); matching is exact
  on (key, Shift/Ctrl/Alt). Only the controller may set it (the handler checks the caller
  against the orchestrator's controller PID). Until it does, the built-in default reserves
  bare Tab, Delete, Esc, `[` and `]` (`is_default_reserved_key()` in both files). Every other
  key, Ctrl chords and arrows included, reaches the app with its modifiers.
- **Buttons and motion** go to the window under the cursor. A press makes that app the
  pointer owner: its motion and the release keep going to it until no button is held,
  even if the cursor leaves the window.
- **Scroll** goes to the window under the cursor only, or to the controller when the
  cursor is over no window.
- **The controller** also receives every key, button and motion event on its own stream
  (`xrEnumerateWorkspaceInputEventsDXR`).
- **Input grab** (the controller's launcher is open, `xrSetWorkspaceInputGrabDXR`) and
  **pointer capture** (a controller drag/resize gesture, `xrEnableWorkspacePointerCaptureDXR`)
  send everything to the controller only.

## 4. Decisions

**Pull, not injection, on Linux and macOS.** Wayland forbids injecting into another
client's surface. A queue the app drains works the same under Wayland, X11 and AppKit, and
needs no OS privilege.

**Windows stays on `PostMessage`.** Letting Windows apps opt into the queue is deferred. It
would only help apps with no window, cross-integrity cases where UIPI blocks
`PostMessage`, or apps that want one code path on every platform. If it is ever done, an app
that enables the extension must switch `PostMessage` off for that client, so nothing is
delivered twice.

**No double delivery.** Outside a workspace (no controller, or an in-process session) the
drain returns zero events and the app's window gets input natively. Inside a workspace the
window is hidden and gets nothing natively, so the drain is the only source.

**Known gaps.**

- #2781: desktop Linux and macOS forward a lone Ctrl at once; Windows holds it until the
  chord is known (#667).
- #2782: the macOS AppKit pump sends a printable key as its lowercase character (`'a'`), not
  its VK code (`'A'`), so reserved letter chords never match there. An app that handles both
  platforms should accept either form for letters until this is fixed.

## 5. App checklist (desktop Linux and macOS)

1. Enumerate instance extensions and enable `XR_DXR_workspace_client_input` at
   `xrCreateInstance` **only if it is listed**. It is a no-op outside a workspace, so enable
   it whenever it is offered.
2. If the app uses `DxrLinuxWindow`, pin `displayxr-common` v2.36.0 or later and call
   `attach_session()` after `xrCreateSession`. Input then arrives as normal window events.
3. Otherwise resolve `xrEnumerateWorkspaceClientInputEventsDXR` with `xrGetInstanceProcAddr`
   and drain it once per frame, before handling input. Treat `vkCode` as a Win32 VK code
   (see #2782 for letters on macOS) and scale `position` by the app's own content size.
4. Create the window hidden when `DISPLAYXR_WORKSPACE_SESSION=1` on Wayland.

Apps that do this today: `test_apps/cube_handle_vk_linux` and `test_apps/cube_zones_vk_linux`
(#2785), and the modelviewer, avatar, earthview, gaussiansplat and mediaplayer demos.

## See also

- [XR_DXR_workspace_client_input](../specs/extensions/XR_DXR_workspace_client_input.md) — the app-side API
- [XR_DXR_spatial_workspace](../specs/extensions/XR_DXR_spatial_workspace.md) — the controller side (its input stream, reserved keys, grab, capture)
- [Workspace controller registration](../specs/runtime/workspace-controller-registration.md)
- [Service architecture](service-architecture.md)
- [ADR-018](../adr/ADR-018-workspace-hit-test-plumbing-vs-policy.md) — hit-test is plumbing, gesture policy is the controller's
