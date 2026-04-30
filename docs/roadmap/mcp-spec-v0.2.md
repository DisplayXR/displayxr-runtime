# DisplayXR MCP Specification (v0.2)

**Status:** Draft — design doc, not yet scheduled
**Supersedes:** `display_xr_mcp_spec_v_0.md` (v0.1, private draft)

## 1. Thesis

Spatial computing has two problems that 2D computing doesn't:

1. **Stereo correctness is invisible-but-numerical.** When a cube "looks flat," the bug is in disparity math the eye can't read directly.
2. **6-DOF manipulation has no good input device.** A mouse gives you 2 DOF; arranging windows in depth is inherently slow and imprecise.

Agents — systems that operate on *numbers plus language* rather than *pixels plus mouse* — are a disproportionately good fit for both. DisplayXR MCP exposes the runtime's live state and control surface to agents over the Model Context Protocol, turning those two weaknesses into strengths.

This is a category VR headsets structurally can't address the same way (they have 6-DOF controllers; stereo is immersive, not a "does this look right on my desk" question). It is a DisplayXR-native capability, not a port of existing AI-for-XR tooling.

## 2. Driving user stories

The scope of v0.2 is exactly what's needed to make these two stories work end-to-end — nothing else.

### 2.1 Developer story — "Why does this cube look flat?"

Maya is porting a Unity demo. The cube renders with stereo separation, but it looks flat and ghosts slightly on head movement. Today she eyeballs it, hardcodes a convergence value, recompiles, repeats ten times, then posts a 2D screenshot in Slack that doesn't show the defect.

With the MCP:

> Maya: "The cube in `unity_demo.exe` looks flat. Figure out why."

The agent:
- `capture_frame` — reads the stereo atlas, measures disparity
- `diff_projection` — compares what the runtime recommended via `xrLocateViews` against what the app declared in its last `XrCompositionLayerProjectionView`; flags a FoV aspect mismatch
- `get_runtime_metrics` — confirms tracking and FPS are fine
- `tail_log` — notices the app reported a canvas size mismatched to display physical size
- Cross-references `docs/architecture/kooima-projection.md`
- Replies: *"Unity camera aspect is 16:9 but the display is 16:10; the app declared a FoV with wrong horizontal tangents relative to the recommended projection, flattening disparity by ~12%. Read `XR_EXT_display_info::canvasSize` instead of hardcoding."*

The point is not that the agent writes code — it is that the agent can see the pixels, read the numerical ground truth, and consult the docs simultaneously. No human debugger holds all three in head at once, and stereo bugs are exactly where the eye deceives.

### 2.2 User story — "Set me up for the 3pm review"

David wants three apps arranged for a design review: a reference PDF (left, arm's length), a splat viewer (centered, pushed back), Slack (right, closer). Arranging windows in 3D with a mouse is genuinely bad — 6 DOF of position, 2 DOF of input. Users fall back to Ctrl+1-4 presets and live with it.

With the MCP + voice:

> David: "Put the PDF on the left at arm's length, the splat viewer centered but pushed back, Slack on the right closer to me. Save this as 'review layout.'"

Agent: `list_windows` → `set_window_pose` ×3 → `save_workspace("review layout")`.

> David: "Start a 3D recording. I'll walk through the splat for 30 seconds."

Agent: `start_recording` → `stop_recording` → drops the SBS clip into Slack.

Natural language is inherently a better input modality than mouse for 6-DOF positioning. This isn't a gimmick layered on top of a GUI; it is the first interaction model where the GUI alternative is structurally weaker.

## 3. Architecture

DisplayXR runs in two deployment modes and v0.2 supports both. The same `displayxr-mcp` adapter binary serves both; it auto-detects which is available.

### 3.1 Handle-app mode (Phase A — ships first)

Handle apps link the runtime as a shared library — there is no service, no IPC. Everything MCP needs (recommended FoV, swapchain pixels, submitted projection layers, logs) is already in the app process.

```
Agent (Claude Code / Cursor)
        ↓ MCP (stdio)
displayxr-mcp --pid auto
        ↓ local socket / named pipe (per-PID)
In-process MCP server thread
    embedded in libopenxr_displayxr,
    started when DISPLAYXR_MCP=1 is set
        ↓
Runtime state, swapchains, U_LOG ring
```

- Runtime spawns an MCP server thread when `DISPLAYXR_MCP=1` is set at app launch.
- Server listens on `\\.\pipe\displayxr-mcp-<pid>` (Windows) or `/tmp/displayxr-mcp-<pid>.sock` (macOS).
- `displayxr-mcp` is a stdio-to-socket adapter: with no service running it enumerates handle-app sockets by PID and attaches.
- Zero app source changes — just an env var at launch.

### 3.2 Shell/service mode (Phase B)

```
Agent (Claude Code / Cursor / voice shell)
        ↓ MCP (stdio or HTTP)
displayxr-mcp
        ↓ existing shell-runtime IPC
displayxr-service    <-- unchanged
        ↓
Runtime / compositors / DP / per-app sessions
```

**Decision:** standalone `displayxr-mcp` binary, not in-process in `displayxr-service`. Rationale:
- Keeps the production service lean; MCP is dev/power-user-facing and optional.
- Reuses the existing `shell-runtime-contract` IPC; no new privileged surface in the service.
- Sandboxable — MCP server can be killed without impacting running apps.
- Transport-agnostic: stdio default for local agents, HTTP/SSE opt-in for remote.

### 3.3 Why Phase A first

- **Zero service dependency** — no IPC changes, ships standalone.
- **Hits the Maya story end-to-end.** Unity/Unreal/WebGPU porting work starts as a handle app; stereo debugging is the pain point that bites first.
- **Tight scope** — no shell means no window layout, workspaces, or recording arbitration to design. Half the v0.2 surface, and the conceptually cleanest half.
- Phase B ("User story — Set me up for the 3pm review") is structurally N/A in handle-app mode (no shell = no windows to arrange), so no feature is lost by sequencing.

## 4. Scope — v0.2 tool surface

Split into two phases by deployment mode. Phase A ships first; Phase B slots in once the service IPC is wired.

### 4.A — Phase A (handle-app mode, ships first)

Five tools, all runtime-internal, none requiring shell or service:

| Tool | Returns | Backed by |
|---|---|---|
| `get_kooima_params` | Display phys size, canvas, viewer pose + per-eye FoV the runtime handed the app via `xrLocateViews` | `oxr_system_fill_in()` + `xrLocateViews` path |
| `get_submitted_projection` | Per-eye pose, FoV, swapchain subImage rect from the app's last `XrCompositionLayerProjectionView` | Layer handling in `oxr_session_frame_end` |
| `diff_projection` | Structured diff of the two above, flagging common mismatches | Derived; pure computation |
| `capture_frame` | Stereo swapchain readback as PNG | In-process swapchain access |
| `tail_log` | Streaming U_LOG tail for this app's session | `u_logging.c` in-process ring |

Plus three trivial helpers usable in both phases: `list_sessions` (returns one in handle-app mode), `get_runtime_metrics`, `get_display_info` (wraps `XR_EXT_display_info`).

### 4.B — Phase B (shell/service mode)

Adds everything that requires multi-app or shell-owned state:

| Tool | Action | Backed by |
|---|---|---|
| `list_windows` | Shell windows with ids, titles, bounds, focus state | Shell window manager |
| `get_window_pose` / `set_window_pose` | Read/write 6-DOF window pose in meters | Shell `--pose` plumbing |
| `set_focus` | Focus a window | Shell focus manager |
| `save_workspace` / `load_workspace` | Named pose sets, persisted to disk | New — thin layer over `set_window_pose` |
| `start_recording` / `stop_recording` | SBS 3D capture to MP4 | `docs/roadmap/3d-capture.md` |

**Phase 2.G note:** `apply_layout_preset` shipped in the original Phase B
slice but was removed in Phase 2.G when layout-preset semantics moved
out of the runtime. AI agents that previously called
`apply_layout_preset` now compose presets out of per-client
`set_window_pose` calls. The first-party shell ships the reference
implementation in `src/xrt/targets/shell/main.c`.

Phase B's version of `capture_frame` reads the compositor atlas (via the existing `comp_d3d11_service.cpp` file-trigger path) and annotates with per-window bounding boxes.

### 4.C Three signals underpinning the stereo-introspection tools

The runtime does *not* see the app's internal projection pipeline (view matrix, near/far, scene depths). It sees three distinct signals and Phase A exposes exactly those — no more, no less:

1. What the runtime **recommended** via `xrLocateViews` (first-level Kooima from display geometry + head pose) — surfaced by `get_kooima_params`.
2. What the app **declared** it rendered with, via `XrCompositionLayerProjectionView` at frame submit — surfaced by `get_submitted_projection`.
3. What actually **landed in pixels**, measured from the submitted swapchain — surfaced by `capture_frame` (plus a follow-on `measure_disparity` in Phase B once the atlas provides structured per-window regions).

**Bug classes this enables an agent to catch:**

| Symptom | Detected via |
|---|---|
| App ignoring `xrLocateViews`, using hardcoded FoV | `recommended_fov` ≠ `declared_fov` |
| Aspect-ratio bug (Maya's Unity story) | `declared_fov` aspect ≠ swapchain subImage aspect, or ≠ display aspect |
| App rendering with stale head pose | `declared_pose` ≠ predicted pose at submit time |
| App honored FoV but stereo still wrong | Measured disparity inconsistent with declared projection |

### 4.4 Not in v0.2 — deferred with rationale

| Deferred | Why |
|---|---|
| `get_viewer_pose` | Streaming head-tracking data to cloud LLMs is a real privacy concern on a fixed-user desktop; also 60–120 Hz data is useless over MCP polling. Needs an event-subscription design and explicit per-tool consent first. |
| `set_convergence` / `set_depth_scale` | As specified in v0.1, these are layer violations. Convergence/disparity is app-side (Kooima projection lives in the app). A global runtime knob does not exist without a new app-side extension (see §6). |
| `launch_app` | Already trivially available via `displayxr-shell.exe app1 app2`. No new capability; can be added as a wrapper later if needed. |
| `attach_to_session` | Semantics were undefined in v0.1. |
| In-app scene introspection ("move that chair to the left") | Requires app cooperation via a new extension or SDK pattern. See §6 Tier 2. |

## 5. Safety model

Read/write separation (v0.1 §7) is necessary but insufficient for a desktop spatial runtime. v0.2 adds:

1. **Default transport is stdio-local.** Remote HTTP/SSE is opt-in and requires a launch flag.
2. **Biometric gate.** Any tool returning head-pose, eye-gaze, or per-frame tracking data is off by default and enabled per-session with user consent. `get_viewer_pose` sits behind this gate and is not in v0.2 scope.
3. **Per-app allowlist.** Window/pose/log tools can be scoped to a subset of running apps, so an agent debugging app A cannot read app B's frames.
4. **Tool audit log.** Every write tool call is logged with timestamp, tool name, args hash; viewable in shell.

## 6. Tier 2 — what this spec does *not* promise

The most exciting voice-control demos ("tune the depth of this app," "move that object in the scene") require the *app* to expose its own knobs, not just the runtime. The runtime can move windows; it cannot reach into an app's scene graph without cooperation.

Two candidate paths, both out of scope for v0.2:

- **`XR_EXT_app_introspection`** — a new OpenXR extension letting apps publish a small structured menu of controls (named params, ranges, units). The MCP server would surface these as dynamically discovered tools.
- **SDK-side MCP endpoints** — the Unity plugin and WebXR bridge expose their own MCP servers, sitting alongside the runtime's. Agents connect to both.

Either direction is a multi-month design problem with ecosystem implications. Treat it as v0.3+ and scope separately.

## 7. Delivery plan (sketch)

### Phase A — handle-app mode (~1 week)

1. In-process MCP server thread in `libopenxr_displayxr`, gated on `DISPLAYXR_MCP=1`; named-pipe / Unix-socket transport per PID.
2. `displayxr-mcp` adapter binary, stdio ↔ per-PID socket, auto-discovery.
3. Five Phase A tools from §4.A plus trivial helpers (`list_sessions`, `get_runtime_metrics`, `get_display_info`).
4. Dev-story demo on `cube_handle_d3d11_win` + Unity handle app.

**Gate:** Maya's stereo-debug story reproduced end-to-end on a real Leia display.

### Phase B — shell/service mode (~1 week after A)

1. Extend `displayxr-mcp` to talk shell-runtime IPC when the service is running.
2. Phase B tools from §4.B — window pose, focus, workspaces, recording.
3. Safety model §5 (stdio-local default, allowlist, audit log).
4. User-story demo: voice-driven layout + recording.

**Gate:** David's review-layout story reproduced end-to-end; privacy review of §5 before enabling any pose/tracking tool.

## 8. Open questions

- **Voice transport.** The user story assumes a voice front-end; DisplayXR does not ship one. Does v0.2 include a reference voice client, or is that left to third-party MCP clients (e.g. a Claude Desktop session)?
- **Workspace persistence format.** JSON on disk is obvious; should it live in the shell session state or a separate registry?
- **Multi-display.** `list_displays` exists but the rest of the tool surface is implicitly single-display. Defer multi-display addressing until `docs/roadmap/multi-display-single-machine.md` lands.

## 9. Versioning

- **v0.1** (private draft) — broad tool surface, no grounding stories, several layer violations.
- **v0.2** (this doc) — scoped to two driving stories; split into Phase A (handle-app mode, ships first, no service dependency) and Phase B (shell/service mode); deferred app-side and biometric tools with rationale.
- **v0.3+** — app-side introspection (Tier 2), multi-display, event subscriptions.
