# Runtime: `RENDERING_MODE_CHANGED_EXT` event fan-out to all sessions

**Tracking issue:** #142
**Blocks:** #139 Phase 2
**Target platform:** Windows (D3D11 service compositor is where the fix lives); other per-API compositors need the same pattern applied as a follow-up.
**Discovered by:** Phase 1 of WebXR Bridge v2 (#139) — see commit `7c5f60ec3` on `feature/webxr-bridge-v2`.

## Problem recap

`XrEventDataRenderingModeChangedEXT` only reaches an OpenXR session if that session's client calls `xrt_device_update_inputs` between mode changes. In practice that means only rendering clients (who do it per frame) see the event; headless/metadata sessions never do.

Root cause is client-side pull at `src/xrt/state_trackers/oxr/oxr_session.c:855-884`:

```c
struct xrt_device *head = GET_XDEV_BY_ROLE(sess->sys, head);
if (head != NULL && head->hmd != NULL) {
    uint32_t cur = head->hmd->active_rendering_mode_index;
    if (cur != sess->last_rendering_mode_index && ...) {
        oxr_event_push_XrEventDataRenderingModeChanged(log, sess, ...);
    }
}
```

`head->hmd->active_rendering_mode_index` on an IPC client is synced from shared memory only by `ipc_client_hmd_update_inputs` at `src/xrt/ipc/client/ipc_client_hmd.c:130-144`, which is only invoked from `xrt_device_update_inputs` — a per-frame call on rendering clients. Headless/non-rendering clients never call it, so their proxy is frozen at the value captured at session creation (`ipc_client_hmd.c:366`).

The server side already maintains per-client shared memory: `ipc_server_process.c:551-560` (`main_loop` copies `active_rendering_mode_index` into every client's `isms[ci]` at 20 Hz). That sync works. The break is on the state-tracker side, where only rendering clients pull.

## Fix design — server-side push via existing session event fan-out

The plumbing we need already exists:

- `u_system_broadcast_event(usys, xse)` in `src/xrt/auxiliary/util/u_system.c:236` fans out a `xrt_session_event` to every registered session on a `u_system`.
- IPC `session_poll_events` at `src/xrt/ipc/shared/proto.json:146` already ferries `union xrt_session_event` from server to client (`ipc_server_handler.c:732` → `xrt_session_poll_events` → the per-session queue populated by the broadcast helper).
- `oxr_session_poll_events` at `src/xrt/state_trackers/oxr/oxr_session.c:892-929` already dispatches on `xrt_session_event.type` (state change, overlay change, display refresh rate change, etc.).

So the fix is four small edits plus a removal, all inside the runtime:

### 1. Define the new session-event kinds

`src/xrt/include/xrt/xrt_session.h`:

```c
enum xrt_session_event_type
{
    ...
    XRT_SESSION_EVENT_EXIT_REQUEST = 11,
    XRT_SESSION_EVENT_RENDERING_MODE_CHANGE = 12,
    XRT_SESSION_EVENT_HARDWARE_DISPLAY_STATE_CHANGE = 13,
};

struct xrt_session_event_rendering_mode_change {
    enum xrt_session_event_type type;
    uint32_t previous_mode_index;
    uint32_t current_mode_index;
};

struct xrt_session_event_hardware_display_state_change {
    enum xrt_session_event_type type;
    bool hardware_display_3d;
};
```

Add both to `union xrt_session_event`. The union currently peaks at the `display_refresh_rate_change` variant (4 × `float` + `enum`); the new ones are smaller, so the IPC wire format does not grow and `proto.json` needs no change — just regenerate `proto.per` and friends per repo convention so the generated sibling files pick up the new enumerators.

**Critical gotcha:** the union is memcpy'd over the wire. Verify after regeneration that the union size is unchanged (`sizeof(union xrt_session_event) == before`) so the payload size matches on both sides; if it increased, bump a protocol version and handle backwards compat.

### 2. Plumb a broadcast call into the compositor's hotkey handler

`src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — the six sites where `active_rendering_mode_index` is mutated today (lines ~5862, 5865, 7014, 7017, 7073, 7075, per `git grep`). After setting the field, build a `xrt_session_event` and broadcast it.

The compositor needs access to a `struct u_system *`. The `xrt_system_compositor` instance already holds a back-reference via its owning system — follow the chain from the D3D11-service compositor down to `sys->usys` (may need to thread a pointer through `comp_d3d11_service_create_system`). Alternatively, the broadcast can be invoked from the driver side (`src/xrt/drivers/leia/leia_device.c`) via a new `xrt_device` callback — but that couples drivers to the event system and is probably worse.

Pseudocode:

```cpp
uint32_t prev = head->hmd->active_rendering_mode_index;
head->hmd->active_rendering_mode_index = new_mode_idx;

if (prev != new_mode_idx) {
    union xrt_session_event xse = {};
    xse.type = XRT_SESSION_EVENT_RENDERING_MODE_CHANGE;
    xse.rendering_mode_change.previous_mode_index = prev;
    xse.rendering_mode_change.current_mode_index = new_mode_idx;
    u_system_broadcast_event(sys->usys, &xse);

    bool prev_hw3d = head->hmd->rendering_modes[prev].hardware_display_3d;
    bool new_hw3d  = head->hmd->rendering_modes[new_mode_idx].hardware_display_3d;
    if (prev_hw3d != new_hw3d) {
        union xrt_session_event xse2 = {};
        xse2.type = XRT_SESSION_EVENT_HARDWARE_DISPLAY_STATE_CHANGE;
        xse2.hardware_display_state_change.hardware_display_3d = new_hw3d;
        u_system_broadcast_event(sys->usys, &xse2);
    }
}
```

Do not remove the server-side shmem sync at `ipc_server_process.c:551-560` — that's still valuable for rendering clients that check the field on subsequent `xrLocateViews` calls to pick the right view scales.

### 3. Dispatch the new event kinds in the state tracker

`src/xrt/state_trackers/oxr/oxr_session.c:892-929` — add two cases to the existing `switch (xse.type)` block:

```c
case XRT_SESSION_EVENT_RENDERING_MODE_CHANGE:
#ifdef OXR_HAVE_EXT_display_info
    // Mirror the pull-mode bookkeeping (view scales, hardware_display_3d,
    // last_rendering_mode_index) so the rest of the session state tracker
    // sees the same values it would have seen via the old path.
    {
        struct xrt_device *head = GET_XDEV_BY_ROLE(sess->sys, head);
        uint32_t cur = xse.rendering_mode_change.current_mode_index;
        if (head && head->hmd && cur < head->rendering_mode_count) {
            const struct xrt_rendering_mode *mode = &head->rendering_modes[cur];
            sess->hardware_display_3d = mode->hardware_display_3d;
            struct xrt_system_compositor *xsysc = sess->sys->xsysc;
            if (xsysc != NULL) {
                xsysc->info.recommended_view_scale_x = mode->view_scale_x;
                xsysc->info.recommended_view_scale_y = mode->view_scale_y;
            }
        }
        sess->last_rendering_mode_index = cur;
    }
    oxr_event_push_XrEventDataRenderingModeChanged(
        log, sess,
        xse.rendering_mode_change.previous_mode_index,
        xse.rendering_mode_change.current_mode_index);
#endif
    break;

case XRT_SESSION_EVENT_HARDWARE_DISPLAY_STATE_CHANGE:
#ifdef OXR_HAVE_EXT_display_info
    oxr_event_push_XrEventDataHardwareDisplayStateChanged(
        log, sess,
        xse.hardware_display_state_change.hardware_display_3d ? XR_TRUE : XR_FALSE);
#endif
    break;
```

### 4. Remove the now-redundant client-side pull

`oxr_session.c:855-884` — delete the `#ifdef OXR_HAVE_EXT_display_info` detection block entirely. The dispatch case above supersedes it.

Keep the `sess->last_rendering_mode_index` field and its initialization — still useful for the dispatch case's bookkeeping.

### 5. Verification

Four-way test matrix:

| Session kind | Expected |
|---|---|
| Headless bridge (#139, commit `7c5f60ec3`) | Gets `RENDERING_MODE_CHANGED previous=X current=Y` for every service hotkey press, logged by the bridge with re-enumerated view config. |
| Chrome WebXR (rendering client, IPC) | Continues to receive the event; re-enumerates its `XRWebGLLayer` dims on the next frame callback with no regression vs today. |
| `cube_hosted_d3d11_win` (IPC rendering client) | Still cycles modes via the V key / hotkey with no regression; its own `onRenderingModeChange` path still fires. |
| Two rendering clients simultaneously (shell scenario) | Both get the event. |

Also verify `sizeof(union xrt_session_event)` is unchanged after the header edits:

```bash
grep -A3 "session_poll_events" src/xrt/ipc/shared/proto.json
# Before and after regen, the wire-format blob size in generated
# src/xrt/ipc/shared/proto.h sibling must match.
```

### Out of scope

- Non-Windows compositors (`comp_metal_compositor.m`, `comp_gl_compositor.c`, `comp_d3d11_compositor.cpp` in-process, etc.) all have the same mutation pattern. They should be updated to the broadcast-event pattern eventually, but the D3D11 *service* compositor is the only one that serves multiple IPC clients today (and is therefore the only one where the missing fan-out actually bites).
- The `active_rendering_mode_index` shmem sync at `ipc_server_process.c:551-560`. Still valuable for rendering clients that read the index on subsequent frames without waiting for the event.
- Any changes to `XR_EXT_display_info` spec — the new session-event kinds are internal plumbing, not extension-visible.

## Files to modify

| File | Change |
|---|---|
| `src/xrt/include/xrt/xrt_session.h` | New enum values + new event structs + add to union. |
| `src/xrt/ipc/shared/proto.per` (+ regenerated siblings) | Regenerate to pick up new enumerators. No manual edit if the enum is referenced by name. |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | 6 sites: after each `active_rendering_mode_index =`, broadcast event. Thread `u_system *` through create_system if not already available. |
| `src/xrt/state_trackers/oxr/oxr_session.c` | Remove pull-mode block at 855-884. Add two dispatch cases at 892-929. |
| Possibly `src/xrt/compositor/d3d11_service/comp_d3d11_service.h` | If a `usys` pointer needs to be added to the service system struct. |

## Files to read first (required context)

- `src/xrt/state_trackers/oxr/oxr_session.c:855-884` — the old pull-mode detection (to understand exactly what state must be mirrored by the dispatch case).
- `src/xrt/state_trackers/oxr/oxr_session.c:892-929` — existing session-event dispatch (the pattern to extend).
- `src/xrt/auxiliary/util/u_system.c:236` — `u_system_broadcast_event` implementation (to understand fan-out semantics and what it can't do).
- `src/xrt/ipc/server/ipc_server_process.c:551-560` — the current shmem sync loop (do not delete).
- `src/xrt/ipc/client/ipc_client_hmd.c:130-144` — where the old pull-mode sync happens (for context on why headless sessions break).
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` around 5862, 7014, 7073 — the three hotkey handling paths that mutate `active_rendering_mode_index`.
- `src/xrt/include/xrt/xrt_session.h:203-215` — `union xrt_session_event` layout and size constraints.
- `docs/specs/XR_EXT_display_info.md` — verify the expected delivery semantics of `XrEventDataRenderingModeChangedEXT` (event must be delivered to every session with the extension enabled).

## Definition of done

- #142 closed.
- Running `docs/roadmap/webxr-bridge-v2-plan.md` Phase 1 verification against a fresh build shows `RENDERING_MODE_CHANGED` events in the bridge terminal on every hotkey press.
- `cube_hosted_d3d11_win` regression test unchanged.
- Commit message includes `Fixes #142 (#139)` so both tracking issues are linked.
