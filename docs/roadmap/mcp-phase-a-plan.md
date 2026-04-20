# MCP Phase A — Handle-App Mode Plan

**Branch:** `feature/mcp-phase-a-ci`
**Worktree:** `.claude/worktrees/mcp-phase-a`
**Spec:** `docs/roadmap/mcp-spec-v0.2.md` §3.1, §4.A, §4.C
**Target:** Maya's stereo-debug story runs end-to-end on a handle app.

## Goal

Ship a minimal in-process MCP server inside `libopenxr_displayxr` that exposes five tools over a per-PID socket, plus the `displayxr-mcp` stdio adapter that lets Claude Code / Cursor attach to a running handle app with no service and no IPC.

## Scope (exactly this, nothing more)

### Tools

1. `list_sessions` — returns one entry (`{pid, display_id, api}`) in handle-app mode.
2. `get_display_info` — wraps existing `XR_EXT_display_info` query.
3. `get_runtime_metrics` — FPS, predicted display period, tracking confidence.
4. `get_kooima_params` — display phys size, canvas, viewer pose at last frame, per-eye recommended FoV handed to the app via `xrLocateViews`.
5. `get_submitted_projection` — per-eye pose, FoV, swapchain subImage rect from the app's most recent `XrCompositionLayerProjectionView`.
6. `diff_projection` — structured diff + flagged mismatches (see spec §4.C bug-class table).
7. `capture_frame` — swapchain readback → PNG. Metal and GL on mac; D3D11 on Windows in a later slice.
8. `tail_log` — streaming U_LOG ring snapshots.

Trivial helpers (`list_sessions`, `get_display_info`, `get_runtime_metrics`) land alongside the stereo tools since they share infra.

### Out of scope for Phase A

- Anything requiring the shell or service (windows, workspaces, recording, focus).
- `measure_disparity` as a structured histogram — deferred to a Phase A.5 once `capture_frame` is solid.
- `get_viewer_pose` / any biometric tool — behind a privacy gate, Phase B at earliest.
- Remote HTTP/SSE transport — stdio-local only.

## Architecture

```
Claude Code (MCP client)
    ↕ stdio (JSON-RPC 2.0 framed by Content-Length headers)
displayxr-mcp   (new binary in src/xrt/targets/mcp_adapter/)
    ↕ per-PID socket: /tmp/displayxr-mcp-<pid>.sock (macOS)
                      \\.\pipe\displayxr-mcp-<pid> (Windows, Phase A Windows slice)
MCP server thread  (new in src/xrt/auxiliary/util/u_mcp_server.{h,c})
    ↕ direct state reads
oxr state tracker + active xrt_compositor swapchain
```

### Lifecycle

- Runtime checks `DISPLAYXR_MCP` env var in `xrInstanceCreate`. If set, spins up a detached thread running the MCP server.
- Server binds socket named by current PID; no listener = no overhead.
- Server is best-effort: failure to bind logs a `U_LOG_W` and continues; never blocks app startup.
- Thread joined at `xrDestroyInstance`; socket unlinked.

### State access

The MCP server thread reads runtime state through a thin **snapshot** API — no locking the render loop:

- `u_mcp_latest_frame_snapshot_t` — published atomically at the end of `oxr_session_frame_end`. Contains last-submitted projection layers, last recommended FoV, predicted display time, head pose used for prediction.
- `u_mcp_swapchain_capture()` — requests a one-shot readback tag on the swapchain; fulfilled on next frame boundary, not inline. Returns CPU-side pixels.

### `displayxr-mcp` adapter

Separate binary, no runtime link. Speaks MCP stdio to the agent, forwards JSON-RPC to the per-PID socket.

Modes:
- `--pid auto` — enumerate sockets, pick the single active session, error if 0 or >1.
- `--pid N` — attach to a specific PID.
- `--list` — print discovered sessions and exit.

Ships in `src/xrt/targets/mcp_adapter/`.

## Source tree additions

```
src/xrt/
├── auxiliary/util/
│   ├── u_mcp_server.h           # public header — start/stop, tool registration
│   ├── u_mcp_server.c           # server thread, socket listener, JSON-RPC dispatch
│   ├── u_mcp_tools_core.c       # list_sessions, get_display_info, get_runtime_metrics
│   ├── u_mcp_tools_stereo.c     # get_kooima_params, get_submitted_projection, diff_projection
│   ├── u_mcp_tools_capture.c    # capture_frame (per-API slices)
│   ├── u_mcp_tools_log.c        # tail_log ring subscription
│   ├── u_mcp_snapshot.h         # snapshot struct + atomic publish API
│   └── u_mcp_snapshot.c
├── state_trackers/oxr/
│   └── oxr_session_frame_end.c  # +publish snapshot at submit
└── targets/mcp_adapter/
    ├── CMakeLists.txt
    └── displayxr_mcp.c          # stdio ↔ socket adapter
```

## Delivery slices

Each slice is an independently mergeable commit. Landing order matters.

1. **Slice 1 — plumbing.** Empty MCP server thread, socket bind, JSON-RPC echo tool. `DISPLAYXR_MCP=1 cube_handle_metal_macos` + `displayxr-mcp --pid auto` handshakes. No runtime data yet.
2. **Slice 2 — core tools.** `list_sessions`, `get_display_info`, `get_runtime_metrics`. No frame-level data.
3. **Slice 3 — snapshot API.** `u_mcp_snapshot` published from `oxr_session_frame_end`. `get_kooima_params`, `get_submitted_projection` read from it.
4. **Slice 4 — diff.** `diff_projection` with all four bug classes from spec §4.C.
5. **Slice 5 — log tail.** `tail_log` streams `u_logging` ring snapshots; SSE-style incremental responses over JSON-RPC.
6. **Slice 6 — capture (macOS).** `capture_frame` for Metal + GL swapchains via the existing capture paths. PNG encoding via stb_image_write (already vendored).
7. **Slice 7 — Windows slice.** Named-pipe transport; D3D11 swapchain capture. Real Leia hardware validation.

Slices 1–6 are a mac-only milestone; slice 7 is the Windows port.

## Platform strategy

**Primary dev platform: macOS.** Iteration on mac handle apps (`cube_handle_metal_macos`, `cube_handle_gl_macos`, `cube_handle_vk_macos`) with `sim_display` exercises the full Maya debug flow — the stereo bugs Maya catches are all math bugs, not Leia-specific. Metal and GL swapchain readback paths are straightforward.

**Windows validation: slice 7.** Named pipe transport, D3D11/D3D12 swapchain capture, real Leia display confirms the weaver doesn't perturb any of the signals `diff_projection` reads. Use `scripts\build_windows.bat build` once the mac side is green; do not use CI for iteration.

**Cross-platform code:** transport is the only platform-specific module. Socket/pipe abstraction lives behind `u_mcp_transport.{h,c}`.

## Validation

Each slice lands with at least one scripted test:

- **Slice 1:** `tests/mcp/test_handshake.sh` — launches handle app under `DISPLAYXR_MCP=1`, runs `displayxr-mcp --pid $PID` with a scripted JSON-RPC init + echo.
- **Slice 3–4:** `tests/mcp/test_diff_projection.sh` — seeds a handle app that deliberately renders with the wrong aspect; asserts `diff_projection` flags `fov_aspect_mismatch`.
- **Slice 6:** `tests/mcp/test_capture_frame.sh` — captures a frame from `cube_handle_metal_macos`, asserts PNG size > 0 and center pixel non-black.

Final Maya-story demo: a short recorded session where Claude diagnoses the deliberately-broken aspect app in <60 seconds.

## Open implementation questions

- **JSON library.** Pull in `cJSON` (permissive licence, single .c/.h, tiny) or hand-write a minimal JSON-RPC encoder? Recommend `cJSON`.
- **Thread safety of the U_LOG ring.** `u_logging` currently has no ring buffer — `tail_log` needs one. Cheapest path: lock-free MPSC ring in `u_logging.c`, opt-in via `DISPLAYXR_MCP=1`.
- **Snapshot double-buffering.** Single writer (compositor thread) / single reader (MCP thread) — atomic pointer swap is sufficient; no mutex needed.
- **Security.** Socket perms default to 0600; owner only. No auth beyond OS file perms in Phase A.

## References

- Spec: `docs/roadmap/mcp-spec-v0.2.md`
- OpenXR layer submit path: `state_trackers/oxr/oxr_session_frame_end.c`
- Existing logging: `src/xrt/auxiliary/util/u_logging.{h,c}`
- Existing screenshot path (service mode, reference only): `compositor/multi/comp_d3d11_service.cpp`
