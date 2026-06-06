# Per-App MCP Tools & Workspace Aggregator

**Status:** design draft (June 2026). Pre-implementation.
**Scope:** three repos — `displayxr-mcp` (framework v0.4.0 + adapter aggregator), `displayxr-runtime` (new `XR_EXT_mcp_tools` extension + Phase A tool tagging), `displayxr-shell-pvt` (tool tagging + `list_windows` pid). Plus an additive amendment to the [app manifest spec](../specs/runtime/displayxr-app-manifest.md) (§3.4 `id`).
**Audience:** runtime contributors; app/demo authors who want agent-drivable apps; Unity/Unreal plugin owners.
**Builds on:** the post-extraction MCP architecture (Phase A handle-app introspection in `oxr_mcp_tools.c`, Phase B workspace tools in shell-pvt, named-pipe-per-host topology, `Capabilities\MCP` registry gate).

---

## 1. Thesis

Today MCP exposes what the *platform* knows: runtime introspection per app process (Phase A) and workspace window control in the shell (Phase B). Apps themselves are opaque — an agent can move the media player's window but cannot press play.

The fix is two independent axes:

- **Axis 1 — app-defined tools.** An app registers its own MCP tools (`play_pause`, `load_model`, `run_animation`, …) through a new OpenXR extension. The tools surface on the **existing per-PID MCP server** the runtime DLL already hosts in the app's process — no new server, no new pipe, no app-side dependency on the MCP framework.
- **Axis 2 — one agent-facing endpoint.** A new `displayxr-mcp --target workspace` adapter mode aggregates the shell pipe + every per-PID pipe behind a single MCP connection, with namespaced tool names and live membership updates.

End state — an agent drives the whole workspace through one socket:

```
shell__set_window_pose(window="Model Viewer", ...)   ← 3D window op (Phase B)
modelviewer__load_model(path="...")                  ← app-defined tool (this design)
modelviewer__run_animation(name="turntable")
mediaplayer__play_pause()
shell__capture_frame()                               ← visual verification (Phase B)
```

The axes ship independently: Axis 1 is immediately useful via the existing `--target pid:N`; Axis 2 is useful even with zero app-defined tools (it unifies shell + Phase A today).

### 1.1 The key observation

The MCP server already runs **inside every handle-app process** (started from `xrCreateInstance` when the `Capabilities\MCP` gate is on, pipe `\\.\pipe\displayxr-mcp-<pid>`). "Per-app tools" is therefore not a new-server problem; it is (a) an app-facing **registration API** and (b) an **aggregation/identity** problem. This document is mostly (b), because that is where the design stress lives.

---

## 2. Current state (what this builds on)

| Piece | Where | Relevant property |
|---|---|---|
| Framework (`mcp_server`, `mcp_transport`, `mcp_log_ring`, …) | `displayxr-mcp` repo, runtime pins v0.3.2 | Tools registered **before** `mcp_server_start()`; no change notifications; transport is one client per pipe. |
| Adapter `displayxr-mcp.exe` | `displayxr-mcp/adapter/displayxr_mcp.c` | **Protocol-blind** stdio↔pipe byte shuttle. `--target shell\|<role>\|pid:N\|auto`; `auto` already enumerates per-PID sessions. |
| Phase A tools (~12) | `oxr_mcp_tools.c` | Per-app-process introspection + session-free diagnostics (#378). Bare names (`capture_frame`, `get_kooima_params`, …). |
| Phase B tools | shell-pvt `src/shell_mcp_tools.c` | Workspace control. Bare names (`list_windows`, `set_window_pose`, …). |
| Gate | `Capabilities\MCP\Enabled` + `DISPLAYXR_MCP` env override | Caller-side, per host. Unchanged by this design. |

Constraint inherited from the extraction: **apps never link the MCP framework.** Engine apps (Unity) couple to DisplayXR only via the OpenXR extension wire protocol; this design preserves that.

---

## 3. Identity & namespacing

### 3.1 Separator: `__`, not `.`

Tool names must satisfy the strictest common client charset — Claude's API enforces `^[a-zA-Z0-9_-]{1,128}$`, so dotted names (`mediaplayer.play_pause`) are rejected client-side. The namespace separator is **double underscore**:

```
<app-id>__<tool-name>        mediaplayer__play_pause
shell__<tool-name>           shell__set_window_pose
workspace__<tool-name>       workspace__list_apps      (aggregator built-ins)
```

`__` is reserved: it may not appear in app ids (enforced by charset — ids contain no `_` at all) and SHOULD not appear in bare tool names.

### 3.2 App id: manifest field + extension-carried, cross-checked

The prefix is the app's **id**, a stable machine slug matching `^[a-z0-9][a-z0-9-]{0,31}$`:

- **Declared in the manifest** as the new optional `id` field ([manifest spec §3.4](../specs/runtime/displayxr-app-manifest.md)) — so launchers and agents know it pre-launch.
- **Declared at runtime** via `xrSetMCPAppInfoEXT` (§4) — the **authoritative** value; no filesystem lookup, works for registered-mode manifests (which do NOT sit next to the exe), works for engine apps (the Unity plugin fills it from its manifest-settings asset).
- `scripts/check_displayxr_app.py` gains an INV rule asserting the two match.

Rejected alternatives: slugified `name` (display names are mutable branding, slugification is unstable); runtime reading the sidecar manifest (breaks for registered mode; pushes launcher-domain manifest knowledge into the runtime DLL — a layering violation); reverse-DNS ids (`com-displayxr-mediaplayer__play_pause` burns agent context for no benefit at this ecosystem size).

**Fallback chain** when no id is declared (Browse-for-app entries, raw OpenXR apps): sanitized exe basename → such apps still get Phase A introspection under *some* prefix.

Spoofing is out of scope: the app executes its own tools; it can only lie about itself.

### 3.3 Instance collisions: sticky suffixes

Two running instances of the same app both claim `mediaplayer`. Rules:

1. First instance observed gets the bare id.
2. Subsequent collisions get `-2`, `-3`, … — assigned **sticky for the aggregator's lifetime**: never renumbered when an earlier instance exits, never reused while the aggregator runs. (Per-prefix tool names must stay stable mid-conversation; renumbering breaks agent tool memory and prompt caching.)
3. `workspace__list_apps` (§6.4) is the authoritative prefix → pid → window mapping.

Pid-in-every-prefix was rejected: unambiguous but churns the tool list on every app restart.

---

## 4. Axis 1 — `XR_EXT_mcp_tools` (app-defined tools)

### 4.1 API sketch

```c
// Once per session, before registering tools. appId matches manifest `id`.
typedef struct XrMCPAppInfoEXT {
    XrStructureType    type;                 // XR_TYPE_MCP_APP_INFO_EXT
    const void        *next;
    char               appId[32];            // ^[a-z0-9][a-z0-9-]{0,31}$
} XrMCPAppInfoEXT;
XrResult xrSetMCPAppInfoEXT(XrSession session, const XrMCPAppInfoEXT *info);

typedef struct XrMCPToolInfoEXT {
    XrStructureType    type;                 // XR_TYPE_MCP_TOOL_INFO_EXT
    const void        *next;
    const char        *name;                 // bare name; runtime/aggregator prefixes
    const char        *description;          // shown to the agent — write it well
    const char        *inputSchemaJson;      // JSON Schema (object), UTF-8
} XrMCPToolInfoEXT;
XrResult xrRegisterMCPToolEXT(XrSession session, const XrMCPToolInfoEXT *tool);
XrResult xrUnregisterMCPToolEXT(XrSession session, const char *name);
```

### 4.2 Dispatch: event queue, not callbacks

The MCP transport thread never calls into app code. Tool invocations ride OpenXR's event model and execute **on the app's frame loop**, where its state is naturally consistent:

```c
// Event payloads are fixed-size, so args use the OpenXR two-call idiom.
typedef struct XrEventDataMCPToolCallEXT {
    XrStructureType    type;                 // XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_EXT
    const void        *next;
    XrSession          session;
    uint64_t           callId;
    char               toolName[64];
    uint32_t           argsSize;             // bytes incl. NUL
} XrEventDataMCPToolCallEXT;

XrResult xrGetMCPToolCallArgsEXT(XrSession session, uint64_t callId,
                                 uint32_t capacity, uint32_t *countOutput, char *buffer);
XrResult xrSubmitMCPToolResultEXT(XrSession session, uint64_t callId,
                                  XrBool32 success, const char *resultJson);
```

Flow: transport thread receives `tools/call` → trampoline enqueues the request, parks the JSON-RPC id pending → app sees the event in its normal `xrPollEvent` pump → executes → `xrSubmitMCPToolResultEXT` completes the pending request. MCP is async JSON-RPC; a parked request costs nothing.

### 4.3 Lifecycle & failure rules

- **Scope:** tools live on the `XrSession`. Session end (or destroy) unregisters them and fails any pending calls. Phase A's session-free diagnostics already cover the no-session case.
- **Late registration is expected** (e.g. model viewer registers `run_animation` after a model loads) and triggers `notifications/tools/list_changed` on the per-PID server (§7). Same for unregistration.
- **Timeout:** the per-PID server fails a pending call after **5 s** (default; future per-tool override via `next`-chain) with a JSON-RPC error. The timeout lives in the *host*, not the aggregator — only the host can distinguish "app is hung" from "transport is slow". Handle apps pump frames even when unfocused, so a healthy app always drains its queue.
- **Naming:** duplicate `name` registration → `XR_ERROR_NAME_DUPLICATED_EXT`. Names matching Phase A built-ins are allowed (they are distinct groups; the per-PID server disambiguates internally and the aggregator exposes the app tool).

### 4.4 Engine bindings

The Unity (and later Unreal) plugin wraps the four entry points in C# and forwards `args`/`result` JSON strings. Because the coupling is wire-protocol-only, no plugin↔runtime source dependency is introduced — same contract as every other DisplayXR extension.

---

## 5. Tool groups (exposure metadata)

With ~12 Phase A tools per app process, shell + 4 apps ≈ 60 tools, mostly four identical copies of deep introspection. That wrecks agent tool-choice accuracy and context budget. Tools therefore carry a **group**, set where the tool is defined and surfaced in `tools/list` via `_meta`:

```c
enum mcp_tool_group {
    MCP_TOOL_GROUP_DIAGNOSTIC = 0,   // Phase A introspection, selftest, tail_log
    MCP_TOOL_GROUP_APP        = 1,   // registered via XR_EXT_mcp_tools
    MCP_TOOL_GROUP_WORKSPACE  = 2,   // shell Phase B
    MCP_TOOL_GROUP_CAPTURE    = 3,   // capture_frame — the verification primitive
};
```

| Group | Tagged by | Aggregator default |
|---|---|---|
| `WORKSPACE` | shell-pvt | exposed |
| `APP` | runtime (extension trampoline) | exposed |
| `CAPTURE` | runtime + shell | exposed (one per host) |
| `DIAGNOSTIC` | runtime | hidden unless `--expose-diagnostics` |

`shell__capture_frame` (whole workspace) and `mediaplayer__capture_frame` (that app's frame) coexist deliberately — different verbs, honestly disambiguated by prefix.

**Untagged tools** (pipes served by a ≤ v0.3.2 framework) default to `DIAGNOSTIC` — except `capture_frame`, which the aggregator special-cases to `CAPTURE` by name. Old runtimes thus degrade to "shell + capture", which is sane.

---

## 6. Axis 2 — the workspace aggregator (`--target workspace`)

### 6.1 Topology

```
Agent (one MCP connection, stdio)
        │
displayxr-mcp.exe --target workspace          ← MCP-TERMINATING (new)
        ├── \\.\pipe\displayxr-mcp-shell      → shell__*            (if running)
        ├── \\.\pipe\displayxr-mcp-<pidA>     → mediaplayer__* + diagnostics
        └── \\.\pipe\displayxr-mcp-<pidB>     → modelviewer__* + diagnostics
```

This is a category change for the adapter: today it shuttles bytes; `--target workspace` must speak MCP on both sides — per-backend `initialize` handshakes, merged `tools/list`, routed `tools/call`, **rewritten JSON-RPC ids** (the agent's id space is not any backend's id space), relayed notifications. The framework's `mcp_server` + vendored cJSON are reused for this; the existing 1:1 modes stay byte-shuttles, untouched.

### 6.2 Membership

- **Discovery by pipe enumeration**, not via the shell — the aggregator must work shell-less. Windows: poll the pipe namespace ~1 s (the `--target auto` enumeration code already exists); POSIX: `readdir` of `/tmp/displayxr-mcp-*.sock`. There is no pipe-creation notification on Windows; polling is cheap and sufficient.
- **Allowlist, not wildcard:** absorb `pid:` pipes + the `shell` role only. Explicitly never the `service` role — pre-extraction runtime installs bind a ghost `displayxr-mcp-service` pipe that must not be aggregated.
- **Join/leave** → re-merge → emit `notifications/tools/list_changed` to the agent. App tools registering late inside a live app arrive as a relayed `list_changed` from that backend (§7). Rule of thumb: **the aggregator never snapshots; it mirrors.**
- **Mid-call death:** backend pipe breaks with calls in flight → synthesize JSON-RPC errors for the in-flight ids + `list_changed`. Never hang the agent.
- **Shell correlation:** shell's `list_windows` adds a `pid` field so the aggregator can join pipe ↔ window mechanically (no title matching).

### 6.3 Concurrency

One reader thread (or overlapped IO) per backend; per-backend FIFO, parallel across backends — a slow `modelviewer__load_model` must not block `mediaplayer__play_pause`. Symmetric with the framework's thread-per-connection server side.

### 6.4 Built-in meta-tools (`workspace__*`)

```
workspace__list_apps
  → [ { prefix: "mediaplayer", pid: 41320, window: "Media Player — vacation.mp4",
        manifest_name: "Media Player", groups_exposed: ["app","capture"] }, … ]
    + { shell: "running" | "not_running" }
```

The authoritative prefix↔pid↔window map (also where sticky suffixes are reported). More meta-tools (e.g. `workspace__expose_diagnostics` to flip exposure at runtime) can follow; start with `list_apps` only.

### 6.5 Multi-client transport

Today each host pipe expects exactly one client; aggregator + a developer's manual `--target pid:N` session is two. `mcp_transport` becomes multi-instance (`CreateNamedPipe` with > 1 instance, thread per client) — debugging an app *while* an agent drives the workspace is exactly the workflow we want to keep.

### 6.6 Degradation matrix

| Topology | Behavior |
|---|---|
| Shell + N apps | Full surface. |
| Apps only (no shell) | App + capture tools; no `shell__*`; `workspace__list_apps` reports `shell: not_running`. Aggregator runs, does not wait. |
| Shell only | Phase B as today, plus `workspace__list_apps`. |
| Old runtime (≤ v0.3.2 pin) under new aggregator | No `_meta.group`, no `list_changed` from that pipe → tools exposed per the untagged rule (§5); membership changes still detected by pipe polling. |
| New runtime under old adapter (`--target pid:N`) | Works — app tools appear bare (unprefixed) on the per-PID pipe; prefixing is an aggregator concern. |

---

## 7. Framework changes (`displayxr-mcp` v0.4.0)

Prerequisites for **both** axes — this is the first PR regardless of build order:

1. **Post-start registration + `notifications/tools/list_changed`** — `mcp_server_register_tool` / new `mcp_server_unregister_tool` legal after `mcp_server_start()`; broadcast the notification to connected clients. (Without this, late-registered app tools are invisible even to a direct `--target pid:N` session.)
2. **`mcp_tool.group`** + `_meta` surfacing in `tools/list` (§5).
3. **Multi-instance transport** (§6.5).
4. **Adapter:** `--target workspace` aggregator + `workspace__list_apps` + `--expose-diagnostics` (§6).
5. **Spec:** namespacing, id rules, group semantics, aggregator behavior → `docs/mcp-spec.md`.

Items 1–3 are additive to the framework ABI; consumers re-pin at leisure.

## 8. Security & audit

Unchanged trust model. App tools execute **in the app, with the app's privileges** — the platform adds no capability the app didn't already have. The `Capabilities\MCP\Enabled` registry gate continues to gate each host; the aggregator ships in the MCP Tools package, which only exists if the user installed it. Each host's `mcp_audit` already logs its own calls (app-tool calls log with the app id); the aggregator adds one audit line per routing decision. `mcp_allowlist` applies per host, unchanged.

## 9. Phasing

| Phase | Repo | Deliverable | Usable result |
|---|---|---|---|
| **P0** | `displayxr-mcp` | v0.4.0: list_changed, groups, multi-instance, aggregator (§7) | One-connection shell + Phase A aggregation, today's tools |
| **P1** | `displayxr-runtime` | `XR_EXT_mcp_tools` (spec + impl + header sync), Phase A tools tagged `DIAGNOSTIC`/`CAPTURE`, re-pin v0.4.0; manifest spec §3.4; linter INV rule | Apps define tools; reachable via `--target pid:N` and the aggregator |
| **P1′** | shell-pvt | Tag tools `WORKSPACE`/`CAPTURE`; `list_windows` + pid; re-pin v0.4.0 | Clean default exposure |
| **P2** | demos | `displayxr-demo-mediaplayer` (play_pause, open_file, seek, slideshow) + model-viewer demo (load_model, rotate, list/run animations) adopt the extension | The agent-drives-the-workspace demo |
| **P3** | runtime + Unity plugin | Declarative `mcp` manifest block (pre-launch tool discovery); C# binding | Engine apps + launcher-visible tooling |

P0 → P1 order is hard (P1 re-pins v0.4.0); P1 vs P1′ is parallel; P2 needs P1 only (P0+P1 suffice via `--target pid:N`).

## 10. Open questions

1. **Per-tool timeout override** — `next`-chained on `XrMCPToolInfoEXT`, or a fixed 5 s until proven insufficient? (Lean: fixed until a real tool needs more; `load_model` on a big file is the likely first customer.)
2. **Result size cap** — per-PID pipe already moves capture PNGs; do app tools get the same ceiling or a smaller one (app JSON results should be small; images should go through `capture_frame`)?
3. **Aggregator exposure flips at runtime** — `workspace__expose_diagnostics` meta-tool vs restart-with-flag only. (Lean: flag only in P0; meta-tool if agents actually need it.)
4. **`hosted` apps** — same extension works (session exists); confirm nothing in the hosted window path assumes no app event consumers.
5. **Declarative manifest tools (P3)** — exact shape of the `mcp` block; reserved now in the manifest spec so nothing squats on the name.
