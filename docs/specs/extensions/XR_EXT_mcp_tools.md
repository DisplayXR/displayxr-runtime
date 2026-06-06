# XR_EXT_mcp_tools

| Field | Value |
|---|---|
| **Name** | `XR_EXT_mcp_tools` |
| **Spec version** | 1 |
| **Status** | Implemented (runtime ≥ v1.13, framework ≥ displayxr-mcp v0.4.0) |
| **Header** | [`XR_EXT_mcp_tools.h`](../../../src/external/openxr_includes/openxr/XR_EXT_mcp_tools.h) (auto-synced to `displayxr-extensions`) |
| **Design** | [Per-app MCP tools & workspace aggregator](../../roadmap/per-app-mcp-tools.md) |
| **Depends on** | Nothing at the API level. Functionally inert unless the MCP capability is enabled (`HKLM\Software\DisplayXR\Capabilities\MCP\Enabled` / `DISPLAYXR_MCP`). |

## 1. Overview

The runtime hosts an MCP (Model Context Protocol) server inside every
app process (`\\.\pipe\displayxr-mcp-<pid>` /
`/tmp/displayxr-mcp-<pid>.sock`). Until this extension, that server
exposed only what the *platform* knows — Phase A introspection. This
extension lets the **app** add its own tools — `play_pause`,
`load_model`, `run_animation` — so agents can operate the app, not just
its window:

```
agent → displayxr-mcp adapter → per-PID MCP server → [trampoline]
      → XrEventDataMCPToolCallEXT → app's xrPollEvent loop
      → xrSubmitMCPToolResultEXT → JSON-RPC response → agent
```

Through the workspace aggregator (`displayxr-mcp --target workspace`)
the tools appear namespaced as `<app-id>__<tool>`, where `<app-id>` is
the manifest `id` declared via `xrSetMCPAppInfoEXT`.

Apps never link the MCP framework; the coupling is wire-protocol-only
(same contract as every other DisplayXR extension, so the Unity/Unreal
plugins can bind it).

## 2. API

Five functions, one event. Full signatures + parameter docs in the
header; semantics summary:

| Function | Semantics |
|---|---|
| `xrSetMCPAppInfoEXT(session, info)` | Declare the app's stable id (`^[a-z0-9][a-z0-9-]{0,31}$`, = manifest `id` §3.4). Becomes the aggregator's tool-name prefix. Call before registering tools; changing it later notifies agents but churns their tool names — treat as immutable. |
| `xrRegisterMCPToolEXT(session, tool)` | Register `{name, description, inputSchemaJson}`. Strings are copied. Bare name: `^[a-z0-9][a-z0-9_-]{0,62}$`, no `__`. Duplicate → `XR_ERROR_NAME_DUPLICATED`; budget (32/process) → `XR_ERROR_LIMIT_REACHED`. Late registration is first-class: connected agents get `tools/list_changed`. |
| `xrUnregisterMCPToolEXT(session, name)` | Remove a tool; in-flight calls on it fail to the agent. |
| `xrGetMCPToolCallArgsEXT(session, callId, cap, count, buf)` | Two-call idiom fetch of a pending call's JSON arguments (`argsSize` in the event is the required capacity). |
| `xrSubmitMCPToolResultEXT(session, callId, success, resultJson)` | Answer a call. `success=XR_FALSE` → agent receives a tool error. Late submits on a timed-out call return `XR_SUCCESS` and are dropped. |

**Event** `XrEventDataMCPToolCallEXT` `{session, callId, toolName[64],
argsSize}` — delivered through the session's normal `xrPollEvent`
stream when an agent invokes a registered tool.

## 3. Dispatch contract

- **The MCP transport thread never calls app code.** The runtime-side
  trampoline parks the JSON-RPC request, pushes the event, and waits.
  Tool execution happens on the app's frame loop, where its state is
  naturally consistent — no locking discipline is imposed on apps.
- **Timeout:** a call unanswered after **5 s** fails to the agent
  (JSON-RPC tool error). Handle apps pump frames while unfocused, so a
  healthy app always drains its queue; the timeout exists for hung or
  modal-blocked apps.
- **Concurrency:** up to 16 calls may be pending per process; beyond
  that, agents receive errors immediately.
- **Session scope:** tools live on the `XrSession`. Session destroy
  unregisters everything and fails pending calls. (Phase A's
  session-free diagnostics cover the no-session case.)
- **Results:** `resultJson` should be a JSON object. Unparseable JSON
  is wrapped as `{"raw": "<string>"}` rather than dropped.

## 4. Gating & errors

All functions return `XR_ERROR_FEATURE_UNSUPPORTED` when the MCP
capability is off (the per-process server was never started). Apps
should treat this as "no agent surface available" and continue —
registering tools must never be load-bearing for normal operation.

The extension is always advertised by the runtime; the gate is
evaluated per call so an app binary behaves identically on machines
with and without the MCP Tools package installed.

## 5. Manifest pairing & linting

An app that registers MCP tools MUST declare the same id in its
`.displayxr.json` manifest (`id` field, [manifest spec §3.4]
(../runtime/displayxr-app-manifest.md)) that it passes to
`xrSetMCPAppInfoEXT` — agents discover the id pre-launch from the
manifest and at runtime from the MCP `_meta`. `scripts/check_displayxr_app.py`
lints the pairing (manifest `id` present + charset; code/manifest match
is checked when the source registers tools).

## 6. Description quality (normative-ish)

`description` is **agent-facing documentation**, not a label. The
agent decides *whether and how* to call your tool based solely on the
name, description, and schema. Write it like you'd write API docs:
what it does, what the arguments mean, units, side effects,
preconditions ("requires a model to be loaded — call load_model
first").

## 7. Example

```c
// Once, after session create:
XrMCPAppInfoEXT app_info = {.type = XR_TYPE_MCP_APP_INFO_EXT, .appId = "mediaplayer"};
xrSetMCPAppInfoEXT(session, &app_info);

XrMCPToolInfoEXT tool = {
    .type = XR_TYPE_MCP_TOOL_INFO_EXT,
    .name = "play_pause",
    .description = "Toggle playback of the currently open media file. "
                   "No-op (success=false) when no file is open.",
    .inputSchemaJson = "{\"type\":\"object\"}",
};
xrRegisterMCPToolEXT(session, &tool);

// In the frame loop's event pump:
case XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_EXT: {
    const XrEventDataMCPToolCallEXT *call = (const XrEventDataMCPToolCallEXT *)&event;
    if (strcmp(call->toolName, "play_pause") == 0) {
        bool ok = media_toggle_play();
        xrSubmitMCPToolResultEXT(session, call->callId, ok ? XR_TRUE : XR_FALSE,
                                 ok ? "{\"playing\":true}" : "{\"error\":\"no file open\"}");
    }
    break;
}
```

## 8. Relationship to the aggregator

Nothing in this extension knows about namespacing — apps register
**bare** names. The `<app-id>__<tool>` prefixing, sticky instance
suffixes, group-based exposure, and `workspace__list_apps` all live in
the `displayxr-mcp` adapter's `--target workspace` mode
([mcp-spec §10](https://github.com/DisplayXR/displayxr-mcp/blob/main/docs/mcp-spec.md)).
App-registered tools carry `_meta {"displayxr/group": "app"}` and are
exposed by default; Phase A introspection stays `diagnostic` (hidden
unless `--expose-diagnostics`).
