# XR_DXR_input_providers

| | |
|---|---|
| **Name** | `XR_DXR_input_providers` |
| **Spec version** | 1 |
| **Header** | [`src/external/openxr_includes/openxr/XR_DXR_input_providers.h`](../../../src/external/openxr_includes/openxr/XR_DXR_input_providers.h) |
| **Type values** | `1004999410`–`412` ([registry](../../../src/external/openxr_includes/openxr/README.md)) |
| **Decision** | [ADR-034 Amendment 5](../../adr/ADR-034-input-provider-plugins.md) · [ADR-054 decision 8](../../adr/ADR-054-component-model.md) |
| **Runtime contract** | [`input-provider-discovery.md`](../runtime/input-provider-discovery.md) §3 *Per-client selection* |
| **Status** | Phase 1 (#2181): surface, IPC transport, per-app pin store and CLI. The per-client resolution in the arbiter is phase 2 — until it lands, every system reports **no providers** and the setter returns `XR_ERROR_FEATURE_UNSUPPORTED`. |

## 1. What it is for

A box may have several input providers installed at once — an optical hand
tracker, tracked controllers, a gamepad, a network feeder — plus the runtime's
own emulated (keyboard/mouse) controllers. DisplayXR keeps every one of them
resident and resolves the **controller**, **hand-tracking** and **gamepad**
roles *per client* (ADR-034 Amendment 5). This extension is the app's view of
that:

- **enumerate** the resident providers, with the provider's own display name,
  the families each supplies, whether its hardware is there, and which families
  *this app* currently resolves to it — enough for a HUD hint ("hands: <name>")
  or a selector;
- **request** a provider per family, or go back to the default;
- **persist** that request as the user's per-app pin — the same record the
  Dashboard and `displayxr-cli input use <id> --app <exe>` write.

### Apps never hardcode or recognise a vendor name

Show the strings the runtime returns and let the user pick. A `providerId` is
opaque: never parse it, never match on it, never ship a default that names
one. The single exception is `XR_INPUT_PROVIDER_ID_EMULATED_CONTROLLERS_DXR`
(`"qwerty"`), which names the runtime's own emulated pair, not a vendor's
component. This is ADR-054 decision 8: *apps express preferences, never
policy.*

## 2. Families

| `XrInputRoleFamilyDXR` | Roles | Notes |
|---|---|---|
| `CONTROLLERS` (1) | `/user/hand/left`, `/user/hand/right` action bindings | the emulated pair supplies only this family |
| `HAND_TRACKING` (2) | `XR_EXT_hand_tracking` joint data (unobstructed + conforming, L/R) | whether `XR_EXT_hand_tracking` exists at all is a per-system build-time fact and is not affected |
| `GAMEPAD` (3) | `/user/gamepad` | |

The **head pose** and the **navigation (rig) role** are shared by every app on
a display (the rig composer feeds the display processor's pose source) and are
never selected here.

## 3. Precedence

Per family, top wins:

| # | Level | Who sets it | Notes |
|---|---|---|---|
| 0 | `DXR_INPUT_PROVIDERS=0` in the hosting process, or `ForceQwerty` | CTS lanes, debug | No provider is resident; every level below is moot |
| 1 | HKLM `InputProviders\Policy\Lock` = 1 | the machine's admin | Hides every per-user value of the class — the per-app pin, the app's request, and the per-user `PreferredPlugin` — so only the machine's `PreferredPlugin` and `ProbeOrder` decide. Windows only |
| 2 | **Per-app pin** | the user: Dashboard, `input use <id> --app <exe>`, or this app's request with `persist` | Windows HKCU `Software\DisplayXR\InputProviders\PerApp\<exe-basename>\PreferredPlugin`; POSIX `input-preferred-<exe-basename>` in the per-user manifest dir. Applies to every family the pinned provider supplies |
| 3 | **Session request** | this app, `xrSetInputProviderPreferenceDXR` without `persist` | Per family; lasts for the instance |
| 4 | User default | `input use <id>` (`PreferredPlugin`) | Already rank 0 of the default walk |
| 5 | `ProbeOrder` | the provider's registration | The default walk |

Two rules hold at every level:

- **Only families the provider claims.** A preference hoists the provider for a
  family only if it supplies devices of that family; otherwise the family falls
  to the next level. Pinning an app to a controllers-only provider never parks
  its hand tracking while an optical tracker is resident.
- **Presence still gates.** A preferred provider whose hardware is absent is
  skipped by the walk exactly as an absent ranked one is, and the family falls
  through until it returns.

The per-app key is the **executable basename** (`SampleGame.exe`, with its
extension) of the app process — resolved by the service from the peer process
over IPC, read from the process itself in-process — never
`XrInstanceCreateInfo::applicationName`. It is the per-title keyspace of the
legacy camera profile (#1381), so the Dashboard shows one per-app record. An
app whose executable the service cannot resolve (a restricted-token peer) has
no pin and cannot persist one.

Why the pin beats the request: the pin is the user overriding the app. Since a
persisted request *writes* the pin, an in-app selector and the Dashboard can
never disagree; the order only matters for a non-persisted request.

## 4. API

### `xrEnumerateInputProvidersDXR(instance, systemId, capacity, count, providers)`

Two-call idiom; callable before any session. One `XrInputProviderPropertiesDXR`
per resident provider (and the emulated pair when it exists), in the current
rank order for this app:

| Field | Meaning |
|---|---|
| `providerId` | stable, opaque id — what a request names |
| `displayName` | the provider's own name for itself, verbatim; display only |
| `supportedFamilies` | families it can supply |
| `currentFamilies` | families this app's roles resolve to it **right now** |
| `requestedFamilies` | families this app's non-persisted request names it for |
| `isPresent` | its hardware / transport is there |
| `isUserDefault` | it is the user's default (`PreferredPlugin`) |
| `isAppPinned` | the user pinned this app to it |

A family that appears in no entry's `currentFamilies` has no holder for this
app (no present provider supplies it). A system with no selection support
returns a count of 0 — that is not an error.

### `xrSetInputProviderPreferenceDXR(session, info)`

`XrInputProviderPreferenceInfoDXR { family, providerId, persist }`. An empty
`providerId` clears this app's preference for the family; with `persist` it
also clears the user's per-app pin. An id that is not resident is accepted and
simply never resolves (the user may install it later, as with `input use`).

**Scope is the client, not the session.** `xrSyncActions` caches roles per
`XrInstance`, so the request applies to the instance and outlives the session
it was made on. The session parameter is proof of a running app.

`persist` writes the user's own store; set it only on an explicit user action
in the app's UI. Returns `XR_ERROR_FEATURE_UNSUPPORTED` when the system cannot
select providers, or when `persist` is set and there is no executable identity
or no per-user store to write.

The call returns once the request is recorded; roles move on the next
`xrSyncActions`.

### `XrEventDataInputProvidersChangedDXR`

Instance-scoped (no session). Queued when this app's preference was set, and
when `xrSyncActions` sees this app's controller / hand-tracking / gamepad role
assignment change (`changedFamilies` names which). Presence of a provider that
holds nothing for this app is current at enumeration time and does not raise
the event on its own. Rebinding itself rides the core
`XrEventDataInteractionProfileChanged`, unchanged.

## 5. Caveats

- **Handle apps and the emulated pair.** An app that passes its own window
  (`_handle` class) never gets the emulated qwerty controllers: the OpenXR
  layer skips them per session (`oxr_input.c`, `has_external_window`) so the
  keyboard stays the app's. That is a per-session policy *above* roles —
  pinning a handle app to `"qwerty"` still yields no controllers.
- **`xrGetSystemProperties` reads live roles.** It runs before any session, so
  a later request can change what its `trackingProperties` would have
  reported. Documented and accepted (ADR-034 Amendment 5, Consequences).
- **The shell is untouched.** Service-side input routing goes by window focus,
  never by device index.
- **No preference set ⇒ bit-identical to the default walk**; the CTS lane is
  unaffected (it suppresses providers per process with `DXR_INPUT_PROVIDERS=0`).

## 6. Implementation map

| Layer | Where |
|---|---|
| OpenXR entry points, two-call idiom, event | `src/xrt/state_trackers/oxr/oxr_input_providers.c` |
| Runtime-internal interface | `src/xrt/include/xrt/xrt_input_selection.h` (`xrt_system_devices::input_selection`) |
| IPC transport | `proto.json` `input_providers_enumerate` (paged, 4 per reply) / `input_set_provider_preference`; client `ipc_client_system_devices.c`; server handlers in `ipc_server_handler.c` |
| Per-app key | `u_exe_basename.{c,h}`; server-side from the peer pid at accept (`ipc_server_process.c`) and on an accepted peer declaration |
| Pin store + precedence | `target_input_plugin_get_app_pin` / `set_app_pin` / `list_app_pins` / `target_input_resolve_preference` (`target_input_plugin_loader.{c,h}`) |
| Live pickup | the component-policy watch (#2051): the Windows registry watch covers the subtree; POSIX folds every `input-preferred-*` file |
| CLI | `displayxr-cli input use <id> --app <exe>` / `input reset --app <exe>`; `input list --json` `per_app` |
| Per-client resolution (phase 2) | the arbiter's per-client overlay + generation, live holders in `input list --json` and the ADR-051 status snapshot |
| Tests | `tests_input_selection_policy` (precedence matrix, exe-basename key, POSIX pin store) |
