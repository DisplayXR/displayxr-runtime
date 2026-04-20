# MCP Phase A Status: Handle-App Introspection

**Branch:** `feature/mcp-phase-a-ci`
**Status:** Complete on macOS — Windows transport code-complete, needs validation on a Leia SR display
**Date:** 2026-04-14

## Scope

Opt-in in-process MCP server inside `libopenxr_displayxr`, a per-PID unix-socket / named-pipe transport, and a `displayxr-mcp` stdio bridge so Claude Code / Cursor can introspect a running handle-app session. Zero runtime cost when the `DISPLAYXR_MCP` env var is unset.

**Spec:** [mcp-spec-v0.2.md](mcp-spec-v0.2.md) §3.1, §4.A, §4.C
**Plan:** [mcp-phase-a-plan.md](mcp-phase-a-plan.md)
**Agent prompt:** [mcp-phase-a-agent-prompt.md](mcp-phase-a-agent-prompt.md)
**Issue:** #150

## Delivery slices

| # | Slice | Status | Test |
|---|---|---|---|
| 1 | Server plumbing + stdio adapter | Done | `tests/mcp/test_handshake.sh` |
| 2 | Core tools (list_sessions, get_display_info, get_runtime_metrics) | Done | `tests/mcp/test_core_tools.sh` |
| 3 | Snapshot API + per-view tile getters (get_kooima_params, get_submitted_projection) | Done | `tests/mcp/test_snapshot.sh` |
| 4 | diff_projection bug classifier (3 of 4 bug classes) | Done | `tests/mcp/test_diff_projection.sh` |
| 5 | tail_log ring buffer | Done | `tests/mcp/test_tail_log.sh` |
| 6 | capture_frame tool + per-compositor hook API (Metal + GL on mac, D3D11 on Windows) | Done (full via #153) | `tests/mcp/test_capture_frame.sh`, `tests/mcp/test_capture_frame_gl.sh` |
| 7 | Windows named-pipe transport + portable adapter + Maya demo | Done (code) | `tests/mcp/demo_maya.sh` |

## Tools shipped

| Tool | Signal (spec §4.C) | Source |
|---|---|---|
| `list_sessions` | — | `oxr_mcp_tools.c` |
| `get_display_info` | — | `oxr_mcp_tools.c` |
| `get_runtime_metrics` | — | `oxr_mcp_tools.c` |
| `get_kooima_params` | recommended | `oxr_mcp_tools.c` + `u_mcp_snapshot` (inline) |
| `get_submitted_projection` | declared | `oxr_mcp_tools.c` + `u_mcp_snapshot` (inline) |
| `diff_projection` | recommended vs declared | `oxr_mcp_tools.c` |
| `capture_frame` | actual pixels | `oxr_mcp_tools.c` + `u_mcp_capture` + per-compositor hooks in Metal / GL / D3D11 native compositors |
| `tail_log` | — | `u_mcp_server.c` + `u_mcp_log_ring.c` |

Plus `initialize`, `ping`, `tools/list`, `tools/call`, `echo` as MCP protocol scaffolding.

## Architecture

- Stdio (Content-Length-framed JSON-RPC 2.0) ↔ `displayxr-mcp` ↔ per-PID unix socket / Windows named pipe ↔ detached MCP server thread inside `libopenxr_displayxr`.
- Server start/stop hooked from `oxr_instance_create` / `oxr_instance_destroy`; gated on `DISPLAYXR_MCP`.
- Session attach/detach from `oxr_session_create` / `oxr_session_destroy` so tool handlers have a guarded session pointer.
- Per-view snapshot (recommended + declared + display context) published atomically from `oxr_xrLocateViews` and `oxr_session_frame_end`'s `submit_projection_layer`. Reader does atomic load + struct copy; no lock on the compositor thread.
- `u_logging` sink ring installed only when MCP is enabled — existing log consumers unaffected.

## Design decisions

- **Per-view arrays, not stereo pairs.** All snapshot data is sized to `XRT_MAX_VIEWS` (8) with a per-signal view_count, so mono / stereo / quilted (4- and 8-view) displays all report correctly. The tool surface is tile-oriented, not stereo-oriented.
- **Two view counts per snapshot.** `xrLocateViews` reports the system max across all rendering modes; submit reports the active mode's count. Keeping both means `diff_projection` can pair views correctly in active-mode-dependent configs like Anaglyph vs Quad.
- **Tool code in the state tracker, not aux_util.** `oxr_mcp_tools.c` lives in `state_trackers/oxr/` because the tools need direct access to `oxr_session`. `aux_util` owns only the transport, server dispatch, and the logging ring — keeping vendor SDK / graphics-API headers out of util.
- **Atomic double-buffer publish, not a lock.** Two static snapshot slots; writer fills the inactive one and atomically swaps the pointer. Reader takes a consistent copy via memory_order_acquire. Zero latency added to the compositor.
- **capture_frame is a hook, not a stub.** Tool + `oxr_mcp_tools_set_capture_handler()` registration API are in place; per-compositor implementations (Metal/GL/D3D11) are tracked in #153 as the mechanical next step.
- **Adapter is a dumb pipe.** No JSON parsing in `displayxr-mcp` — two blocking threads pump bytes. Works on POSIX and Windows with the same logic.

## Commits (in order)

- `9e14f5c32` Fix macOS build: guard Windows-only shell window calls (#152)
- `9e214d509` Slice 1 — server plumbing + stdio adapter
- `caa9700f5` Slice 2 — core tools
- `a330b8a89` Slice 3 — snapshot API + per-view getters
- `0024f8435` Slice 4 — diff_projection bug classifier
- `07e3fa4e8` Slice 5 — tail_log ring buffer
- `13fc09f4d` Slice 6 — capture_frame stub + hook API
- `a6921fc38` Slice 7 — Windows transport + portable adapter + Maya demo

## Invariants verified

- Runtime with `DISPLAYXR_MCP` unset: normal startup, no socket, no log-ring sink, no extra cost. Confirmed by running `cube_handle_metal_macos` without the env var and observing no `/tmp/displayxr-mcp-*.sock` creation and no change in log output.
- Compositor render thread: no new locks, no new allocations per frame. Snapshot publish is a single atomic store.
- Layer separation (`docs/architecture/separation-of-concerns.md`): aux_util takes no new deps on compositor or graphics-API headers. cJSON was already a PUBLIC dep.

## Known follow-ups (not blocking Phase A)

- **Windows validation** — slice 7 transport + slice D D3D11 capture are code-complete but need a run on a Leia SR display. `tests/mcp/demo_maya.sh` is portable; expect it to pass once the Windows build lands.
- **`measure_disparity`** — deferred to Phase A.5, now unblocked since `capture_frame` returns real PNGs.
- **Biometric / viewer-pose tools** — Phase B behind a privacy gate, not in scope here.
- **File-format alignment with shell-phase8** — our `_atlas.png / _L.png / _R.png` naming matches shell-phase8's IPC capture. A future cleanup could factor the shared stride-copy + stb_write helpers into `u_image_write.{h,c}` once both surfaces land.

## "Done" demo

```bash
./scripts/build_macos.sh
./tests/mcp/demo_maya.sh
```

Output on a clean mac build:

```
=== Maya stereo-debug demo ===
  PASS  initialize
  PASS  list_sessions — api=metal
  PASS  get_kooima_params — view_count=4
  PASS  get_submitted_projection — declared=2 recommended=4
  PASS  diff_projection — pipeline_consistent=True, uses own projection math (not forwarding xrLocateViews)
  PASS  capture_frame — wrote ['L.png', 'R.png', 'atlas.png']
  PASS  tail_log — got 8 entries
```

Notably, `diff_projection` on the reference `cube_handle_metal_macos` reports `uses own projection math` — the test app recomputes Kooima client-side instead of forwarding `xrLocateViews`. A real observation from the first run of the tool (initially classified as a bug; reframed as a neutral observation after user feedback).
