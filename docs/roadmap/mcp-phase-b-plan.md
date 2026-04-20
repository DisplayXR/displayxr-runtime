# MCP Phase B — Shell/Service Mode Plan

> **Status: Complete.** Merged to `main` on 2026-04-16 (PR #160). All tools listed below are shipping.

**Branch:** `feature/mcp-phase-b-ci` (merged)
**Spec:** `docs/roadmap/mcp-spec-v0.2.md` §3.2, §4.B, §5
**Target:** David's "set me up for the 3pm review" story runs end-to-end against a live `displayxr-service` + shell.

## Goal

Extend the MCP surface shipped in Phase A so that when `displayxr-service` is running, agents can introspect the shell, arrange windows in 6-DOF, save and restore workspaces, and capture annotated stereo frames. Reuse every piece of Phase A infrastructure; add nothing the Maya story didn't need.

## Scope (exactly this, nothing more)

### Tools added (§4.B)

| Tool | Semantics | Backing IPC |
|---|---|---|
| `list_windows` | All shell windows: `{client_id, app_name, hwnd_proxy, pose, size_m, focus, visible}` | Shell window-manager state (read) |
| `get_window_pose` | 6-DOF pose + size in meters for one window | `shell_get_window_pose` (exists) |
| `set_window_pose` | Move/resize one window | `shell_set_window_pose` (exists) |
| `set_focus` | Focus a window | Existing shell focus path |
| `apply_layout_preset` | Trigger Ctrl+1..4 presets | Existing shell hotkey dispatch |
| `save_workspace` | Snapshot current poses → named JSON on disk | New — thin layer over `list_windows` + file I/O |
| `load_workspace` | Re-apply named JSON → batched `set_window_pose` calls | New — thin layer |

### Upgrades to Phase A tools

- `list_sessions` — in service mode, returns one entry per live per-app session (Phase A returned one).
- `capture_frame` — in service mode, reads the existing D3D11 service-atlas path (`comp_d3d11_service.cpp:7929`), splits atlas into `L.png` / `R.png`, and (new) annotates a JSON sidecar with per-window rectangles in atlas pixel space.
- `get_submitted_projection` / `get_kooima_params` — scoped by `client_id` so an agent can compare per-app projections.

### Safety model (§5) — this phase makes it real

- **stdio-local default.** No HTTP/SSE listener unless `displayxr-mcp --remote` is passed.
- **Per-app allowlist.** Adapter flag `--allow <client_id>[,<client_id>…]`; tools filter by allowlist. Default: all.
- **Tool audit log.** Every write tool call appends `{ts, tool, client_id, args_hash}` to a rotating log viewable in the shell. Read tools are not audited.
- **Biometric gate scaffolding.** Introduce the gate enum + per-session bit; no biometric tools enabled in this phase.

### Out of scope for Phase B

- **Recording (`start_recording` / `stop_recording`).** `docs/roadmap/3d-capture.md` is still in "Proposal" status; no stereo video pipeline exists. Deferred to Phase B.5 once 3d-capture MVP lands.
- **`measure_disparity`.** Still waiting on a Phase A.5 reference implementation; not service-specific.
- Multi-display addressing — spec §8 open question.
- Remote transport implementation — flag reserved, not wired.
- `get_viewer_pose` / any eye- or head-tracking tool — biometric gate scaffolding only, no tool.

## Architecture

```
Claude Code (MCP client)
    ↕ stdio (JSON-RPC 2.0, Content-Length framed)
displayxr-mcp   (existing binary from Phase A, extended)
    ↕ POSIX socket / named pipe
    │
    ├── per-PID socket  → in-process server (Phase A handle-app path, unchanged)
    └── service socket  → NEW: MCP sidecar attached to displayxr-service
                             ↕ existing shell-runtime IPC
                          displayxr-service (unchanged)
```

**Decision (spec §3.2 rationale):** MCP lives in a **sidecar process**, not inside `displayxr-service`. The sidecar spawns when the service starts with `--mcp` (or on-demand from the adapter). Keeps the production service lean, crashes are isolated, sandboxable per §5.

### Sidecar binary

New target: `src/xrt/targets/mcp_sidecar/displayxr_mcp_service.c`.

- Boots when `displayxr-service --mcp` is set, or spawned on first agent connection.
- Listens on a well-known service socket: `/tmp/displayxr-mcp-service.sock` (macOS) or `\\.\pipe\displayxr-mcp-service` (Windows).
- Speaks shell-runtime IPC on one side, serves MCP JSON-RPC on the other.
- Reuses Phase A's `u_mcp_server.{h,c}` dispatch + framing; adds a service-aware tool set in `u_mcp_tools_shell.{h,c}`.

### Adapter changes

`displayxr-mcp` discovery updated:

- `--target auto` (new default): if the service socket exists, attach to it; else fall back to per-PID enumeration (Phase A).
- `--target service` / `--target pid:<N>` to force mode.
- `--allow <csv>` / `--remote` as in §Safety.

### State access

- **Reads** go through shell-runtime IPC (`shell_get_window_pose`, window-list query). Latency is fine — MCP is human-typing cadence, not frame-cadence.
- **Writes** go through the same IPC the shell uses (`shell_set_window_pose`, layout-preset hotkey dispatch). No new privileged path.
- **Snapshot API from Phase A** stays PID-scoped but is now reachable via the sidecar — the sidecar proxies `get_kooima_params` / `get_submitted_projection` / `diff_projection` to the correct per-PID server by looking up the session → PID map in shell state.

## Source tree additions

```
src/xrt/
├── auxiliary/util/
│   ├── u_mcp_tools_shell.{h,c}       # list_windows, get/set_window_pose, set_focus,
│   │                                  # apply_layout_preset, save/load_workspace
│   ├── u_mcp_audit.{h,c}             # tool audit log (append + rotate)
│   └── u_mcp_allowlist.{h,c}         # per-client_id filtering
├── targets/mcp_sidecar/
│   ├── CMakeLists.txt
│   └── displayxr_mcp_service.c       # sidecar main: shell IPC ↔ MCP server loop
└── compositor/d3d11_service/
    └── comp_d3d11_service.cpp        # +trigger path: emit L/R split + window-bbox JSON
```

`displayxr-mcp` adapter (existing) gets `--target`, `--allow`, `--remote` flags.

## Delivery slices

Eight slices, each a mergeable commit.

1. **Sidecar plumbing.** Sidecar binary, service-socket listener, JSON-RPC echo + `initialize` + `tools/list`. Adapter learns `--target service`. No shell data yet. Test: `tests/mcp/test_service_handshake.sh`.
2. **list_windows (read-only).** Sidecar calls into shell-runtime IPC, returns window array. Test: launch shell with two cube apps, assert two entries.
3. **get/set_window_pose.** Read-through to existing IPC; write goes through audit log. Test: `set_window_pose` moves the window by a known delta; `get_window_pose` confirms.
4. **set_focus + apply_layout_preset.** Thin wrappers over hotkey dispatch. Test: preset 2 applied, verify poses match known preset layout.
5. **save/load_workspace.** JSON schema + on-disk format (see §Open). Round-trip test: save → move → load → poses restored.
6. **Audit log + allowlist.** Wire §5 scaffolding. Test: tool call filtered by allowlist returns error; write call shows up in audit log.
7. **capture_frame (service-mode).** Re-trigger existing atlas readback via MCP; split into L/R; emit per-window bbox JSON. Test: `test_capture_frame_service.sh` assembling from the existing shell_screenshot path.
8. **Session proxy.** Sidecar forwards `get_kooima_params`, `get_submitted_projection`, `diff_projection` to per-PID server by `client_id`. Test: diff_projection resolves the right session in a two-app shell.

Slices 1–6 can land on Windows dev machines with shell running. Slice 7 exercises the D3D11 service atlas. Slice 8 needs both sidecar + a Phase-A-capable per-app session — iterate with one cube_handle app under the shell.

## Platform strategy

**Primary dev platform: Windows.** The shell and D3D11 service compositor only run on Windows; Phase B is fundamentally a Windows milestone. Use `scripts\build_windows.bat build` on a local Leia SR machine; do not rely on CI for iteration.

**macOS coverage:** the adapter `--target service` fall-through path and the sidecar build are cross-compiled to keep the adapter portable, but there is no service on mac today, so end-to-end tests run on Windows only.

**MinGW check:** include sidecar + new aux_util TUs in `scripts/build-mingw-check.sh` to catch Win32 symbol typos before pushing.

## Validation

Each slice lands with at least one scripted test under `tests/mcp/`:

- `test_service_handshake.sh` — initialize + tools/list against sidecar.
- `test_list_windows.sh` — two-app shell, assert two entries, pose fields present.
- `test_set_window_pose.sh` — known delta, round-trip.
- `test_workspace_roundtrip.sh` — save, perturb, load, compare poses.
- `test_audit_log.sh` — write tool appears, read tool does not; allowlist denies out-of-scope `client_id`.
- `test_capture_frame_service.sh` — atlas split produces `L.png` + `R.png` + `windows.json`; window count matches.

**Phase B demo (gate):** a scripted David story — agent applies a three-window layout by name, saves it as "review layout," perturbs poses, reloads, captures an annotated frame.

## Open implementation questions

- **Workspace JSON location.** `%APPDATA%\DisplayXR\workspaces\<name>.json` on Windows, `~/.config/displayxr/workspaces/` on macOS. Versioned schema. (Spec §8 open question.)
- **Sidecar lifecycle.** Spawn with service (`displayxr-service --mcp`) or lazy-spawn on adapter connect? Recommend **lazy-spawn** — sidecar starts only when an agent actually connects; exits when the service exits or after N seconds idle.
- **Session → PID map.** Shell already tracks `client_id` ↔ process. Expose via a new IPC query `shell_list_sessions` or piggyback on `list_windows`? Lean toward piggyback — one round trip.
- **Atlas bbox derivation.** Per-window atlas rects are computed in `comp_d3d11_service.cpp`'s tile layout; expose them via a tiny sidecar C ABI rather than re-parsing IPC messages.
- **Audit log viewer.** File on disk + optional live-tail in the shell debug overlay? The shell overlay piece is nice-to-have; the file is mandatory.

## References

- Phase A plan/status: `docs/roadmap/mcp-phase-a-plan.md`, `mcp-phase-a-status.md`
- Spec: `docs/roadmap/mcp-spec-v0.2.md` §3.2, §4.B, §5
- Shell-runtime contract: `docs/roadmap/shell-runtime-contract.md`
- Existing atlas capture path: `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:7929`
- 3D capture roadmap (for deferred recording): `docs/roadmap/3d-capture.md`
