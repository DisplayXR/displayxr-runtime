---
status: Active
owner: David Fattal
updated: 2026-08-16
refs: [ADR-035 D1/D6, "#960", "#939", "#959", "#955"]
code-paths: [src/xrt/include/xrt/xrt_instance.h, src/xrt/ipc/server/ipc_server_handler.c, src/xrt/ipc/server/ipc_server_peer_creds.c, src/xrt/targets/service/service_client_class.c, src/xrt/targets/cli/cli_cmd_clients.c]
---
# IPC client classes — wire encoding, verification, quotas (#960)

Every IPC connection to `displayxr-service` carries a **class**. The client
**declares** it; the service **verifies** it against OS-derived facts and stores
the verified value on the connection. Only the verified class drives per-class
quotas, authorization and (Phase 2, #961) lease priority. This is ADR-035 D1/D6.

## Classes

| `enum xrt_client_class` | Value | Who | Declared when | Verified by |
|---|---|---|---|---|
| `APP` | 0 | any ordinary OpenXR app | default (zeroed struct) | — |
| `CONTROLLER` | 1 | the workspace controller (shell) | `XR_DXR_spatial_workspace` enabled | peer pid == orchestrator-spawned pid, **or** peer exe path == a registered controller `Binary` (`HKLM\Software\DisplayXR\WorkspaceControllers\*`, POSIX manifests, or the orchestrator's `workspace_binary` dev override), **or** `DXR_ALLOW_UNVERIFIED_CONTROLLER=1` on the service (dev; loud WARN) |
| `PRESENT_OWNER` | 2 | `XR_DXR_weave` present-owner (displayxr-browser) | `XR_DXR_weave` enabled | by use — only this class may call `weave_*` |
| `RELAY` | 3 | headless relay (WebXR bridge OpenXR session) | `XR_MND_headless` + `XR_DXR_display_info` | by use — may never create a compositor (`session_create` refuses) |
| `PROVIDER_HOST` | 4 | service-spawned input-provider host (Phase 4, #968) | reserved | not yet verifiable → demoted |
| `DIAG` | 5 | read-only diagnostics: `displayxr-cli clients`, the bridge's introspection connection | set explicitly by runtime-internal tools | peer exe lives in the service's own directory |

Precedence when the state tracker derives the declaration from the enabled
extension set (`oxr_instance.c`): CONTROLLER > RELAY > PRESENT_OWNER > APP.

**A failed claim is demoted to `APP`, never refused** — a demoted client is
simply powerless (`[CLASS] pid … declared X but … — demoted to APP`). Refusal
happens only at the quota.

## Wire encoding

- `struct xrt_application_info` (rides `describe_client` unchanged): new fields
  `bool ext_weave_enabled` and `uint32_t declared_client_class`.
- `struct ipc_app_state` (returned by `system_get_client_info` /
  `workspace_get_client_info`): new `uint32_t client_class` = the **verified**
  class. `info.declared_client_class` inside it is the claim.
- `ipc_client_state.class_verified` (server-side): set once at the first
  `describe_client`; the class is settled once per connection.
- No extension header changes; the git-tag handshake covers the struct growth
  (client DLL and service ship together — `scripts/push-runtime-pf.sh`).

## Quotas (ADR-035 D6)

Enforced at `describe_client`, after verification, counting **verified** peers of
the same class under `global_state.lock`:

| Class | Quota |
|---|---|
| CONTROLLER | 1 (the #959 reserved slot is released once a verified CONTROLLER is connected) |
| RELAY | 1 |
| PRESENT_OWNER | 2 |
| DIAG | 4 |
| PROVIDER_HOST | 2 (outside the app budget) |
| APP | `max_clients − 1` (the rest, minus the reserved controller slot) |

Refusal: `[CAP] refusing client pid=… class X quota n/n exhausted (#960)`; the
handshake returns `XRT_ERROR_CLIENT_LIMIT_REACHED` (new, −40), which the state
tracker maps to **`XR_ERROR_LIMIT_REACHED` from `xrCreateInstance`**. The #959
connection cap (`[CAP] … slots in use`) still applies first, at accept.

## What the class gates today

| Operation | Allowed class |
|---|---|
| `workspace_activate` (first-claim hole from #955 closed) | CONTROLLER |
| the 13 `require_workspace_controller` mutators (#955) + `workspace_enumerate_clients` / `workspace_get_client_info` | CONTROLLER (fail closed) |
| `weave_bind_window` / `weave_submit` / `weave_get_output` / `weave_get_fence` / `weave_snap_window_rect` | PRESENT_OWNER |
| `workspace_capture_frame` (whole-atlas capture) | CONTROLLER, DIAG always; APP only outside workspace mode (its standalone atlas is its own content) |
| `session_create` with a native compositor | not RELAY |
| session role flags `is_workspace_controller` / `is_bridge_relay` seen by the compositor | derived from the verified class, not the client's `xrt_session_info` claim |

## Observability

- `[CLASS] client id=… pid=… ('name') admitted as X (declared Y; n/q of class in use)` at every admission.
- `[HEALTH] … class=X …` per client (was `kind=`); the summary now prints `s->max_clients`.
- `displayxr-cli clients [--json]` — connects as DIAG and lists id / pid / class /
  name / session flags. Windows: run **non-elevated** (the shm handle duplication
  fails against an elevated client). `--declare <CLASS>` is a dev knob to
  exercise verification/quota.

## Verified on the box (2026-08-16)

shell (launched by hand, not orchestrator-spawned) → CONTROLLER via registered
binary; modelviewer forced-IPC → APP; bridge → RELAY + DIAG; `displayxr-cli`
→ DIAG; `--declare CONTROLLER` from the CLI → demoted to APP; second RELAY while
the bridge is up → `[CAP]` refusal, client sees `CLIENT_LIMIT_REACHED`.
