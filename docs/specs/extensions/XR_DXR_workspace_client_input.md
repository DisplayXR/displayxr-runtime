# XR_DXR_workspace_client_input

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_workspace_client_input` |
| Spec Version | 1 |
| Type Values | None (a plain event record, no `type` / `next`) |
| Author | The DisplayXR Project |
| Platform | Advertised on desktop Linux and macOS only. Not Windows, not Android. |
| Header | `src/external/openxr_includes/openxr/XR_DXR_workspace_client_input.h` |

---

## 1. Overview

Under a workspace controller (the DisplayXR Shell, or any registered equivalent), the service
composes every app into the workspace. On desktop Linux and macOS it does so on **one shared
surface** that the service owns: the content app's own window is not on screen, so the OS
delivers the user's keyboard and mouse to the service, not to the app.

The service already routes that input. It hit-tests each event against the placed windows and
keeps one queue per content app:

- **keys** follow workspace focus (the focused app), except the keys the controller reserves;
- **buttons and motion** follow the window under the cursor, and the controller also gets a
  copy of every button, motion and key;
- **scroll** goes to the window under the cursor only, or to the controller when the cursor is
  over no window.

Until this extension, the only drain was `xrEnumerateWorkspaceInputEventsDXR`
(`XR_DXR_spatial_workspace`), and enabling that extension makes a session a workspace
controller. A content app had no way to read its queue, so it got no input at all inside a
workspace on these platforms. `XR_DXR_workspace_client_input` is that drain.

### How Windows does it (no extension there)

On Windows the D3D11 service forwards a content app's input as ordinary window messages:
`PostMessage` into the app's own (hidden) HWND, with mouse coordinates mapped into the app
window through its tile rect and the modifier state stamped into the message. The app's normal
message loop sees the input. That path is unchanged and the extension is not advertised there.

## 2. When to enable it

Enable it whenever the runtime advertises it. It costs nothing and is a no-op outside a
workspace:

- **No controller active**, or **an in-process session**: the app's own window receives input
  natively. The drain returns zero events, and the runtime never duplicates native input.
- **Inside a workspace**: the app's own window is not mapped, so it receives nothing natively.
  The drain is the only source. There is no double delivery either way.

A workspace controller does not use this extension: it drains its own stream with
`xrEnumerateWorkspaceInputEventsDXR`. A controller session that calls this function gets zero
events.

## 3. API

```c
typedef enum XrWorkspaceClientInputEventTypeDXR {
    XR_WORKSPACE_CLIENT_INPUT_EVENT_KEY_DXR    = 0,
    XR_WORKSPACE_CLIENT_INPUT_EVENT_BUTTON_DXR = 1,
    XR_WORKSPACE_CLIENT_INPUT_EVENT_MOTION_DXR = 2,
    XR_WORKSPACE_CLIENT_INPUT_EVENT_SCROLL_DXR = 3,
    XR_WORKSPACE_CLIENT_INPUT_EVENT_TYPE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrWorkspaceClientInputEventTypeDXR;

typedef struct XrWorkspaceClientInputEventDXR {
    XrWorkspaceClientInputEventTypeDXR eventType;
    uint32_t   timestampMs;   // host monotonic ms, low 32 bits
    uint32_t   vkCode;        // KEY: Win32 VK_* code (letters = 'A'..'Z', digits '0'..'9')
    XrBool32   isDown;        // KEY, BUTTON
    uint32_t   button;        // BUTTON: 1 = left, 2 = right, 3 = middle
    uint32_t   buttonMask;    // MOTION: bit0 left, bit1 right, bit2 middle held
    uint32_t   modifiers;     // bit0 SHIFT, bit1 CTRL, bit2 ALT, bit3 SUPER (as the wire carries)
    float      scrollDeltaY;  // SCROLL: wheel notches, + = up / away
    XrBool32   positionValid; // BUTTON/MOTION/SCROLL: position is set
    XrVector2f position;      // cursor in the app's window CONTENT, normalized [0,1], top-left origin
} XrWorkspaceClientInputEventDXR;

XrResult xrEnumerateWorkspaceClientInputEventsDXR(
    XrSession                        session,
    uint32_t                         capacityInput,
    uint32_t*                        countOutput,
    XrWorkspaceClientInputEventDXR*  events);
```

### 3.1 `xrEnumerateWorkspaceClientInputEventsDXR`

A **destructive** drain of this session's routed queue, oldest first.

- Writes up to `capacityInput` events into `events` and sets `*countOutput` to the number
  written. Events written are removed from the queue; call again until fewer than
  `capacityInput` come back. One call returns at most 16 events (the size of one IPC reply).
- `capacityInput == 0` writes nothing, drains nothing, and sets `*countOutput = 0`. There is no
  two-call count idiom: a count query would have to drain.
- Returns `XR_SUCCESS` with 0 events when no workspace controller is active, when the session is
  in-process, or when the caller is the workspace controller.
- `XR_ERROR_VALIDATION_FAILURE` if `capacityInput > 0` and `events` is NULL.
- `XR_ERROR_RUNTIME_FAILURE` if the IPC call to the service failed.

Call it once per frame (before handling input), as with any input poll.

### 3.2 Event fields

Fields that do not apply to an event's type are zero.

| `eventType` | Fields |
|---|---|
| `KEY` | `vkCode`, `isDown`, `modifiers` |
| `BUTTON` | `button`, `isDown`, `modifiers`, `position` |
| `MOTION` | `buttonMask`, `modifiers`, `position` |
| `SCROLL` | `scrollDeltaY`, `modifiers`, `position` |

**`position`** is the cursor in the app's window **content** area (the area the app renders,
not the controller's chrome), normalized so the content spans `[0,1] x [0,1]` with a top-left
origin. Multiply by the app's own content size in pixels for a pixel position.
`positionValid` is false when the window had no placed rect when the event was routed.

**Pointer ownership.** A button press makes the window under the cursor the pointer owner.
While any button pressed on it is held, its motion and the release keep coming to that app even
when the cursor leaves the window (drag-to-rotate, a slider drag), and `position` may then fall
outside `[0,1]`. The release always reaches the app that saw the press, even if the controller
grabbed the input in the meantime, so an app is never left with a button held.

**Keys** are forwarded with their modifiers. A key reaches the focused app unless the controller
reserves it (`xrSetWorkspaceReservedKeysDXR`, `XR_DXR_spatial_workspace` spec 24): reserved
chords match exactly on (key, SHIFT/CTRL/ALT). Until the controller registers a set, the
built-in default reserves bare TAB, DELETE, ESC, `[` and `]`, the same list as the Windows
service; every other key, Ctrl chords included, reaches the app. The controller still receives
every key.

**Modifiers** are the wire's mask: bit0 SHIFT, bit1 CTRL, bit2 ALT, and bit3 SUPER where the
platform reports it.

## 4. Platform notes

- **Desktop Linux**: the service window's input (native Wayland or X11) is translated to Win32
  VK codes before routing, so `vkCode` is a Win32 VK code as documented.
- **macOS**: the service's AppKit pump historically forwards a printable key as its lowercase
  character (`'a'`, not `'A'`), with arrows and F11 mapped to VK codes. An app that handles both
  platforms should accept either form for letters until the macOS pump is normalized.

## 5. Implementation

- The service routes in `ipc_server_input_route()` (`src/xrt/ipc/server/ipc_server_input_queue.c`),
  shared by the Linux surface input (`ipc_server_linux_input.c`) and the macOS AppKit pump. A
  content app's copy of a pointer / motion / scroll event carries `local_u` / `local_v`, the
  cursor normalized to its placed content rect (`comp_multi_workspace_window_local_uv()`, NaN when
  it has none); the controller's copy is pushed unchanged.
- The IPC call `workspace_client_enumerate_input_events` drains the caller's own queue (keyed by
  its per-session compositor) on the shared-surface service, and answers zero events on every
  other build, including the Windows D3D11 service.
- The client bridge (`ipc_client_compositor.c`) translates the wire events: `KEY` to KEY,
  `POINTER` to BUTTON, `POINTER_MOTION` to MOTION, `SCROLL` to SCROLL; anything else is dropped.

## 6. Version history

- **1**: initial version.
