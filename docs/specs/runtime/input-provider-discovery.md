# Input-Provider Discovery Contract

**Audience:** Integrators shipping an input-provider plug-in (a DLL/dylib/so
exposing tracked motion controllers or other input `xrt_device`s to the
runtime), and runtime engineers maintaining the discovery path.

**Status:** v1 — §§2–4 and 6 are **implemented** (Phase 1, #823:
`xrt/xrt_input_plugin.h`, `target_input_plugin_loader.c`, the builder
arbitration, `sim_input`, and the CLI/self-test diagnostics). §5 —
`net_input` and its wire protocol — has since **shipped**
(`src/xrt/drivers/net_input/`, `DisplayXR-NetInput`). §4b — the rig
(navigation) role, the rig composer, rig-local anchoring and the
persistent host iface — is **implemented** (#1380, ADR-034 *Amendment 4*).
The `Enabled` / `PreferredPlugin` pair and the `displayxr-cli input
enable|disable|use|reset` verbs (§3, §6) are **implemented** (N3a of the
ADR-054 component model: the same four verbs for every component class).
Live re-select when either value changes under a running service is
**implemented** (N3 step d, #2051 / PR #2129: `service_component_watch` +
`t_input_arbiter_rerank`). Still planned: the per-client recenter lease under
a workspace controller (§4b, ADR-035 D2).
The authoritative
rationale is `docs/adr/ADR-034-input-provider-plugins.md`. This spec
intentionally mirrors the display-processor contract
(`docs/specs/runtime/plugin-discovery.md`); where a rule is not restated
here, the DP rule applies unchanged.

**Host process.** Input providers are `dlopen`ed **in-process** — inside
`displayxr-service.exe` in service mode, and inside the app in every
in-process mode — so a provider shares the fate of whatever loaded it: a
provider that hangs or calls `exit()` takes the service and every connected
client down with it (#943). Which process *should* host them is
[ADR-035](../../adr/ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md)
decision D4.

Terminology: "motion controller" = tracked hand-held input device. Not the
workspace controller (shell) of ADR-014.

---

## 1. What an input provider is

An input provider is a dynamically-loaded module that creates one or more
`xrt_device`s carrying input state:

- **Left / right motion controllers** (first-class in v1): 6DOF pose,
  buttons/axes per a claimed interaction profile, haptic output.
- **One navigation device** (`XRT_DEVICE_TYPE_NAVIGATION`, #1380) if the
  provider drives the virtual rig — the camera the qwerty fly camera drives
  otherwise. See §4b.
- Hand-tracking sources filling the hand-tracking device roles; future
  generic trackers (same ABI, no vtable change).

Providers own their tracking stack entirely — camera access, IMU fusion,
radio/USB transport, threads. The runtime sees only `xrt_device` calls:

| Runtime call | Provider obligation |
|---|---|
| `update_inputs` | Refresh `xrt_input` array (buttons/axes) |
| `get_tracked_pose(name, at_time_ns)` | Return a predicted pose for the requested timestamp. Push timestamped samples into `m_relation_history` and predict on demand (`m_predict`); do not return "latest sample" for future timestamps. |
| `set_output` | Apply haptic events (amplitude/frequency/duration) |
| `destroy` | Tear down threads/transport |

## 2. C ABI

Header: `xrt/xrt_input_plugin.h`. Single exported symbol:

```c
xrt_result_t
xrtInputPluginNegotiate(uint32_t runtime_api_version,
                        const struct xrt_input_plugin_host_iface *host,
                        struct xrt_input_plugin_iface **out_iface,
                        uint32_t *out_plugin_api_version);
```

- ABI constant: `XRT_INPUT_PLUGIN_API_VERSION_CURRENT` = **1**. Mismatch →
  the loader REJECTS the entry before any vtable dispatch (ADR-020 rule
  3), same as the DP loader. Every struct carries `struct_size`; the
  vtable is append-only forever.
- `xrt_input_plugin_iface` v1 vtable (identity strings `id` /
  `display_name` / `vendor` / `version` precede the callbacks, as in the
  DP iface):
  - `probe(out_inst)` — cheap "is my hardware/transport present" check;
    on success writes a provider-defined instance handle (NULL is legal)
    that the loader passes back to every later call.
    `XRT_ERROR_PROBER_NOT_SUPPORTED` = clean decline.
  - `create_devices(inst, struct xrt_device **out_devices, uint32_t
    max_count, uint32_t *out_count)` — create all devices this provider
    supplies (bounded by `XRT_INPUT_PLUGIN_MAX_DEVICES` = 8).
    Each device self-describes: `device_type`
    (`XRT_DEVICE_TYPE_{LEFT,RIGHT,ANY}_HAND_CONTROLLER`, …) plus the
    interaction profile it binds (an existing profile from
    `bindings.json` — e.g. `…/khr/simple_controller` — reusing the same
    mechanism as `qwerty_device.c` / `sim_input_device.c`).
  - `destroy(inst)`.
  - Appended optional slots (ADR-020, `struct_size`-gated, guarded by
    `XRT_INPUT_PLUGIN_IFACE_HAS`): `get_presence(inst)` (§4),
    `describe_settings` (§3 *Component settings*) and
    `get_engaged_ns(inst, hand)` (ADR-034 *Amendment 6*, §4 item 2b) —
    `int64_t`, the `os_monotonic_get_ns` time of that hand's last
    **deliberate** use (0 = left, 1 = right; 0 = never), announced by
    `XRT_INPUT_PLUGIN_IFACE_HAS_GET_ENGAGED_NS`. NULL = engaged
    continuously while present (the pre-amendment behaviour).
- The DLL handle is intentionally leaked (same lifetime rule and rationale
  as the DP loader: one process, one provider, process lifetime).

## 3. Discovery

### Windows — registry

**Root:** `HKLM\Software\DisplayXR\InputProviders` (64-bit view).
Per-provider subkey, same value schema as DisplayProcessors: `Binary`
(absolute DLL path, required), `DisplayName` / `Vendor` / `Version`
(optional strings), `ProbeOrder` (DWORD; missing = 100), optional
`Enabled` (DWORD, 0 disables; the DP root has the same value since N3b,
plugin-discovery.md §2).

**Per-user policy (N3a, ADR-054 verbs `enable|disable` and `prefer`).** Two
values are also read from **HKCU**, where they win over HKLM; they are what
`displayxr-cli input …` writes without admin (`--machine` writes the HKLM
64-bit view instead). Nothing else is ever read from HKCU — the provider rows
themselves (`Binary`, `ProbeOrder`, …) are HKLM-only, so a per-user key can
never inject a DLL.

| Location | Value | Type | Purpose | Written by |
|---|---|---|---|---|
| `HKCU\…\InputProviders\<id>` (overrides `HKLM\…\InputProviders\<id>`) | `Enabled` | DWORD | 0 = the loader skips `<id>` (logged `disabled (Enabled=0, user|machine)`); absent = enabled | `displayxr-cli input enable|disable <id> [--machine]` |
| `HKCU\…\InputProviders` root, then `HKLM\…\InputProviders` root | `PreferredPlugin` | REG_SZ | `<id>` is hoisted to the front of the ProbeOrder sort and noted at arbitration priority 0, so it holds the hand/rig roles whenever it is present. Unregistered → one WARN, ignored; disabled → skipped (`Enabled=0` is the stronger verb); declines → normal fall-through | `displayxr-cli input use <id> [--machine]` / `input reset [--machine]` |
| `HKCU\…\InputProviders\Policy`, then `HKLM\…\InputProviders\Policy` | `EmulatedControllers` | DWORD | **The N7 gate (§4 item 3b).** 0 = the qwerty emulated controller pair is not published (no qwerty left/right device; the hand roles stay unassigned whenever no provider holds them); absent = 1. The qwerty head is not affected. Lives under the class root on purpose — one root per class — so the service's policy watch and the `Policy\Lock` rule cover it with no new plumbing | `displayxr-cli input emulated on\|off [--machine]` |
| `HKCU\…\InputProviders\PerApp\<exe-basename>` (HKCU only) | `PreferredPlugin` | REG_SZ | **Per-app pin (ADR-034 Amendment 5).** That app's controller / hand-tracking / gamepad roles prefer `<id>` for every family `<id>` supplies, ahead of the app's own request and of the root `PreferredPlugin`; presence-gated. `<exe-basename>` is the executable's file name with extension (`SampleGame.exe`), resolved from the process — never `applicationName`. `PerApp` is a reserved subkey (never a provider row). Hidden under `Policy\Lock` | `displayxr-cli input use <id> --app <exe>` / `input reset --app <exe>`; an app's persisted `xrSetInputProviderPreferenceDXR` |

The `Enabled` overlay is the shared component loader's
(`target_component_apply_user_overlay`, ADR-054 N2), so the conversion root's
kiosk rule applies here too: HKLM `InputProviders\Policy\Lock` = 1 makes the
machine value win for every row (conversion-provider-discovery.md §3.1) **and
hides the per-user `EmulatedControllers`, the per-user `PreferredPlugin` and every
per-app pin** (the last two since #2181 — a behaviour change: before, a lock did not
stop a per-user `input use <id>`; `input list --json` now reports
`preferred_source: "machine (locked)"`) (the root-policy read,
`target_component_read_policy_dword`); the `Policy` subkey is reserved and
never read as a provider row. A change to any of the three values reaches a
running service live — as a **re-rank** of the
resident providers, since the provider set is fixed per process (see
*Live re-select* under *Selection* below); in-process apps read it at their
next launch.

**Choosing a `ProbeOrder`:** lower wins. Pick a value distinct from every
provider yours expects to coexist with — the in-tree Ultraleap provider
ships at **50** and `sim_input` at **200**. Equal values are broken
deterministically by provider id (`strcmp`) and the loader logs one WARN
naming both ids, because which of the two then outranks the other is an
alphabetical accident rather than anyone's decision (#1466).
The `PreferredPlugin` override mirroring DP spec §2.1 is implemented (ADR-054
N3a; `displayxr-cli input use <id> [--machine]`, see the table above and the
loader's `target_input_plugin_loader.h`): the preferred id is hoisted ahead of
the ProbeOrder sort; without one, selection is pure ProbeOrder.

`scripts\register_dev_plugin.bat input [dll]` registers the freshly-built
sim-input (or a vendor DLL) from an elevated prompt;
`unregister-input` removes it.

### POSIX — JSON manifests

Manifest name pattern: `NNN-<name>-input-provider.json` in the same
directories the DP loader scans (`XRT_PLUGIN_SEARCH_PATH` for dev trees,
the installed plugin dir otherwise; the roots are shared via
`target_plugin_build_discovery_roots`). Schema is identical to the DP
manifest (`file_format_version` "1.0" + a `plugin` object with `id`,
`binary_path`, optional `display_name` / `vendor` / `version` /
`probe_order`, and optional boolean `enabled`, `false` = the loader skips
the provider; a non-boolean rejects the manifest as malformed — the shared
component loader's parser, ADR-054 N2). The
`-input-provider.json` suffix is the router: the DP loader skips such
manifests, the input loader requires them, so both plug-in types co-habit
one directory. Per-user manifests shadow system ones by `id`, as in the DP
loader.

**Preferred provider (N3a).** The DP loader's §3.3 pattern, same semantics
as the Windows `PreferredPlugin` above:

1. **`XRT_PREFERRED_INPUT_PLUGIN_ID` env var** — wins when set; a dev/CI
   override that needs no writable filesystem.
2. **An `input-preferred` file** in the per-user manifest dir
   (`~/Library/Application Support/DisplayXR/DisplayProcessors/input-preferred`
   on macOS; `${XDG_DATA_HOME:-~/.local/share}/DisplayXR/DisplayProcessors/input-preferred`
   on Linux — next to the DP loader's `preferred` and this loader's
   `force_qwerty`) — a single line containing the provider id. This is what
   `displayxr-cli input use <id>` / `input reset` write.

**Emulated-controller gate (N7).** An `emulated_controllers` file in the same
dir, the `force_qwerty` byte convention (first byte `'0'` = off, anything
else = on; absent = on) — what `displayxr-cli input emulated on|off` writes
(`on` writes `1`, so the source reads `file`; delete the file to return to
`default`). There is no machine tier on POSIX (`--machine` is refused, as for
the other verbs); the service's per-user-dir poll folds the file into the
input class's fingerprint, so a write is picked up live.

There is no per-user `Enabled` store on POSIX (nor is there one for
conversion providers): `displayxr-cli input enable|disable` reports that
and points at the manifest's `enabled` field. `--machine` is Windows-only.

### Android — file-name convention in the runtime APK (#132)

Android discovery is the same convention the display-processor and
conversion classes use (`plugin-discovery.md` §3.2), with its own prefix:
the shared component loader scans the runtime APK's native lib dir (the
directory `openxr_displayxr.so` was loaded from, plus `XRT_PLUGIN_SEARCH_PATH`
if set) for

```
libdxri<NNN>_<id>.so        NNN = ProbeOrder (three digits), id = the row id until negotiate
```

`TARGET_PLUGIN_ANDROID_PREFIX_INPUT` = `libdxri` (`target_component_loader.h`),
set on the input class row in `target_input_plugin_loader.c`. There is no
manifest and no per-user dir: ProbeOrder in the file name is the only ranking,
`Enabled` / `PreferredPlugin` do not exist (the CLI verbs report that), and the
settings store's only tier is the environment — `debug.xrt.DXR_INPUT_<ID>_<KEY>`
(§*Component settings*). A provider ships by being packaged into the runtime
APK's `jniLibs` (ADR-038's rule for vendor plug-ins applies to this class too):
`build.gradle`'s `cmake.targets` lists `drv_touch_input_plugin`, which CMake
emits as `libdxri100_touch_input.so`. A vendor's tracking provider takes a
lower number. The DP loader's sibling preload (#333) skips the `libdxri` prefix
exactly as it skips `libdxrp` / `libdxrc`, so a provider is only ever loaded
through the input class's own load path.

The platform stubs that said "no input providers on Android" are gone: the
class row has a prefix, the loader enumerates, `displayxr-cli selftest` /
`info --json` (and the dashboard's `displayxr-diag` query, which runs the same
headless probe) list what they find under `input_providers`.

### Component settings — the one options contract (ADR-054 decision 7, N3c)

Every component class — display processors (plugin-discovery.md §2.3), input
providers, conversion providers (conversion-provider-discovery.md §3.9) —
carries its per-component settings the same way, and a provider reads them
**only through its host iface**, never from its own environment
(`xrt/xrt_component_settings.h`; the input slot is
`xrt_input_plugin_host_iface::get_setting(id, key, buf, cap)`, announced by
`XRT_INPUT_PLUGIN_HOST_HAS_GET_SETTING`, carved from `reserved[1]` at an
unchanged ABI — the `get_display_geometry` precedent). In the service the
environment is the host's, not the user's shell, which is why a `getenv` in a
provider can never be reached by a CLI verb.

| Tier | Where | Wins over |
|---|---|---|
| 1 — environment | `DXR_INPUT_<ID>_<KEY>` in the **hosting** process's environment; `<ID>` = the id upper-cased, `-` → `_` (`DXR_INPUT_SIM_INPUT_ENABLE`). Android: the `debug.xrt.` system property of the same name. A set-but-empty variable is unset. | everything — the `u_setting` rule: a launcher, a `.bat` or a harness can always take control back from a setting somebody left behind |
| 2 — user | Windows: `HKCU\Software\DisplayXR\InputProviders\<id>\Settings\<KEY>` (`REG_SZ` or `REG_DWORD`). POSIX: `<id>.settings.json` in the per-user manifest dir (a flat object `{"KEY": "value"}`), next to `input-preferred`. | tier 3 — unless HKLM `InputProviders\Policy\Lock` = 1, the kiosk rule `Enabled` already follows (§3), which hides the user tier (never the environment) |
| 3 — machine / manifest | Windows: `HKLM\Software\DisplayXR\InputProviders\<id>\Settings\<KEY>` (64-bit view). POSIX: the manifest's `plugin.settings` object (`"KEY": "value"`; numbers and booleans read as strings). | the provider's compiled default |

Values are strings; the provider parses. Keys are `[A-Za-z0-9_]`, not
digit-first, matched case-insensitively and conventionally upper-case.
**Only `Settings\*` is ever read from HKCU / the per-user file**: a per-user
store can change what a registered provider is *told*, never which binary
loads. `u_setting_is_managed`'s allow-list (the runtime-policy `DXR_*`
levers) is a different store this contract never touches.

**Verbs** — one spelling across `dp` / `input` / `conv`
(`cli_component_options.c`):

| Verb | What |
|---|---|
| `displayxr-cli input options <id> [--json]` | Every key the provider declares (`xrt_input_plugin_iface::describe_settings`, `XRT_INPUT_PLUGIN_IFACE_HAS_DESCRIBE_SETTINGS`; or the POSIX manifest's `plugin.settings_schema` array `[{"key","type","default","help"}, …]`) unioned with every key the stores hold: key, type, default, effective value and its source (`environment` / `user` / `machine` / `manifest` / `absent`). Loads + negotiates the registered binary (no probe) to ask it. |
| `input set <id> <KEY> <value> [--machine]` | Writes the per-user store (no admin) or, with `--machine`, the HKLM 64-bit view (admin). Refused on POSIX with `--machine` — the manifest's `settings` object is the machine store there and is edited by hand. An unregistered id or an invalid key is refused with nothing written. After the write the CLI reads the key back and prints *Effective now:* the value the provider will actually see and why — including when the environment or `Policy\Lock` overrides what was just written. |
| `input unset <id> <KEY> [--machine]` | Removes it from that store; absent is success. |

**When it applies.** A provider reads its settings at init through the host
iface (`probe()` / `create_devices()`), so a write reaches the **next session
/ service start**; the verbs never poke the running service — restart it, or
launch a fresh app.

**Reference implementation.** `sim_input` is opt-in through this contract:
`displayxr-cli input set sim-input ENABLE 1` (or
`DXR_INPUT_SIM_INPUT_ENABLE=1` in the hosting process) replaces the former
`DXR_SIM_INPUT=1`; `NAV`, `NAV_HOLD_MS`, `NAV_RECENTER_MS` replace
`DXR_SIM_INPUT_NAV*` (§5); `HANDS`, `PINCH`, `HAND_DISTANCE_M` (#2191, §5) are
new and have no alias. The old names stay aliases for **one release**
(deprecation WARN once per key), read in `drivers/sim_input/sim_input_settings.c`
and nowhere else — `lint.yml`'s `sim-plugin-getenv-guard` fails any other
`getenv` under the reference plug-ins.

### Selection

Entries sorted by ProbeOrder ascending; **every provider whose `probe()`
succeeds stays resident** for the process lifetime (ADR-034 *Amendment 3* —
this replaced the v1 first-wins rule). ProbeOrder is the provider's
arbitration priority, exactly the DP loader's convention: lower wins. The
ranked list is exposed via `target_input_plugin_get_count()/get_iface(i)`;
`get_active()` aliases index 0. No registered provider is not an error: the
builder falls back to qwerty.

**Live re-select (ADR-054 N3d, #2051).** The provider set is fixed per
process (next section), so an `Enabled` / `PreferredPlugin` change while
the service runs cannot add or remove a provider — it **re-ranks** the
resident ones. The service re-reads both values
(`target_input_plugin_reread_policy`) and hands the arbiter a new standing
per resident provider (`t_input_arbiter_rerank`): the preferred one to
priority 0, every other to its `ProbeOrder`, a disabled one **withdrawn** —
skipped by every role walk exactly as presence loss skips it (§4), its
devices left in `xsysd->xdevs`, still polled for presence so re-enabling is
instant. The roles are recomputed at once and `generation_id` moves, so
every client sees the flip on its next `xrSyncActions` (`input haptic-test`
prints the new generation). The trigger is the DIAG call
`system_request_component_reselect(input)` the CLI makes after each write,
or the service's component-policy watch for an edit made without the CLI
(`service-architecture.md` §3.1; on POSIX the `input-preferred` file and
the manifests' `enabled` are polled). Two things stay a restart, and the
`enable` verb says so: a provider that was disabled or unregistered when
the service scanned is not resident and joins at the next service start
(the service log names it), and in-process apps re-read at their next
launch.

**Per-client selection (ADR-034 Amendment 5, 2026-10-17 — implemented in #2181).**
The ranked walk above is the *default*; the controller, hand-tracking and gamepad
roles are resolved **per client** (IPC connection / in-process process) with an
optional per-family preferred provider id layered on top: machine `Policy\Lock`
→ per-app pin (`HKCU\…\InputProviders\PerApp\<exe-basename>\PreferredPlugin`;
POSIX `input-preferred-<exe-basename>` in the per-user manifest dir; written by
`displayxr-cli input use <id> --app <exe>` and by an app's persisted request)
→ the app's session request (`XR_DXR_input_providers`) → `PreferredPlugin` →
`ProbeOrder`. A pin applies only to the families the pinned provider claims and
is still presence-gated. The rig role and the head are never per client. Each
client gets its own monotonic `generation_id`. `DXR_INPUT_PROVIDERS=0` and
`ForceQwerty` beat every pin.
The precedence for one family is the pure `target_input_resolve_preference`
(`target_input_plugin_loader.h`); the pin store is
`target_input_plugin_{get,set,list}_app_pin(s)`. A pin write reaches a running
service through the component-policy watch (the Windows watch covers the
`PerApp` subtree; POSIX folds every `input-preferred-*` file into the input
fingerprint). App-facing contract: [`XR_DXR_input_providers`](../extensions/XR_DXR_input_providers.md).
In the arbiter the overlay is a per-client table — each IPC connection, plus
the in-process implicit client whose view is `xsysd->get_roles` — whose
resolved roles are cached and recomputed only when the arbiter's state epoch
(a presence edge, a re-rank, a floor withdrawal) or the client's own overlay
moved; never on the presence poll thread (#958). The IPC server answers
`system_devices_get_roles` per connection.

**Engagement (ADR-034 Amendment 6, 2026-10-29).** For the **controller**
family, the default walk is "most recently engaged present candidate", not
"highest-ranked present candidate" (§4 item 2b) — but only for a client with
**no** controller preference: a per-app pin, a session request or a
`PreferredPlugin` naming a controller provider (with `Policy\Lock` shaping
which of them is visible, as above) is a hard choice and turns the switching
off for that client. `ControllerSwitching = presence` (§4 item 3c) turns it
off everywhere. Engagement is one global reading (one user); the resolution
stays per client.

### Lifetime — the provider is per-process, its devices are per-instance

A provider is loaded, negotiated and probed **once per process** and then
never unloaded (`target_input_plugin_loader.h`); the runtime does not call
`destroy()` at all today. The same fixedness applies to the qwerty pair: it
is created (or, with `EmulatedControllers = 0`, withheld) at system build,
and a later gate flip can only withdraw or re-admit a pair that exists. What DOES churn is `create_devices()`: the builder
calls it once per `xrt_system_devices`, i.e. once per `xrCreateInstance`,
and the devices it returns are destroyed through their own
`xrt_device::destroy` at `xrDestroyInstance`. A process may go round that
loop hundreds of times — the OpenXR conformance suite does.

So a provider **must not** hang its expensive machinery (a vendor SDK
connection, a thread pool, a device enumeration) off the device lifetime.
Create it lazily on the first `create_devices()`, keep it for the process,
and let the device objects be the cheap per-instance part. Re-spawning a
vendor SDK's thread pool on every instance is what #1545 diagnosed: 547
create/destroy cycles in one conformance process, each one racing a fresh
LeapC pool spin-up against the rest of the process's DLLs.

The in-tree Ultraleap provider is the reference shape: `g_ul_hub` (LeapC
connection + poll thread) is process-scoped and mutex-guarded, the two
motion controllers come and go, and the #941 idle watchdog closes the
tracking-service connection a few seconds after the last device stops
polling — so an idle box keeps a sleeping thread, not a live client.

## 4. Role arbitration (builder contract)

In `target_builder_sim_display.c`:

Arbitration is **presence-gated, dynamic and ranked** (ADR-034
*Amendments 1 + 3*). Three role families run through the same walk: the
two hand roles below, the dynamic hand-tracking slots (§4.5), and the rig
(navigation) role (§4b). The devices are static — every claiming provider's
pair plus qwerty's is created once and all live in `xsysd->xdevs` — but
which pair holds each hand role is re-resolved for the life of the
process: per hand, the highest-priority candidate that supplies that hand
and reports `PRESENT` wins; qwerty (always present, priority
`UINT32_MAX`) is the floor. Roles therefore move provider→provider on
plug/unplug, not just provider↔qwerty. The floor is **optional** since N7:
with `EmulatedControllers = 0` (item 3b below) no qwerty pair is noted, and
the walk simply runs out of candidates — the hand lands on -1, the shape
the rig role has always had.

**Profile stability (masquerade).** When the qwerty floor wins a hand on a
box that HAS ranked providers, the arbiter reports the top provider's
interaction profile instead of qwerty's own — qwerty carries
binding-profile remaps for the common controller profiles
(simple/touch/index/vive), so bindings still resolve. Combined with the
change-only event rule in oxr, a provider↔qwerty hot-swap then emits **no
`XrEventDataInteractionProfileChanged` at all**: apps see the pose source
move and nothing else. This exists because real-world WebXR content
(three.js `XRControllerModelFactory`) crashes its own render loop on
repeated profile churn — see `docs/roadmap/input-modality-switching.md`
§2. Provider-less boxes are bit-identical to before (full WMR qwerty);
a provider whose profile qwerty cannot emulate is reported honestly.

1. Input-provider loader runs **before** `t_builder_add_qwerty_input()`.
2. If the active provider supplied a left/right pair **and reports
   `XRT_INPUT_PROVIDER_PRESENCE_PRESENT`** (or predates the
   `get_presence` slot, which means "assume present"), those devices hold
   `xrt_system_roles.left/right` and their `*_profile` fields. Otherwise
   qwerty holds them. Qwerty always registers as a device, so the roles
   can come back to it — unless the `EmulatedControllers` gate (3b) is 0,
   in which case there is no qwerty pair and "otherwise" means **no role**.
2b. **Engagement — the controller role follows use (ADR-034 Amendment 6).**
   With `ControllerSwitching = use` (the default, item 3c) and no
   controller preference for the client (§3 *Per-client selection*), each
   hand's controller role goes to the **most recently engaged** present
   candidate instead of the highest-ranked one; ties and never-engaged keep
   the rank order, so with nobody engaging anything the result is the
   Amendment 3 walk. Engagement per candidate and hand:
   - a provider implementing `get_engaged_ns` — its last deliberate use
     (camera hands: a pinch; a controller: a button / trigger / stick edge;
     never mere visibility);
   - a provider **without** the slot — engaged continuously while present,
     so it keeps the role exactly as before (and beats a slot provider that
     is merely present);
   - the qwerty floor — the last keyboard / mouse input aimed at that
     emulated controller (`qwerty_controller_last_input_ns`, installed with
     `t_input_arbiter_set_floor_engagement`); the qwerty head's WASD /
     mouse-look is navigation and does not count.
   An engagement older than the candidate's latest return to `PRESENT` does
   not count, so a provider coming back does not reclaim on a stale pinch;
   the leader refreshing its own engagement moves nothing. The presence
   poll thread (never a client thread, #958) reads engagement at its
   ~250 ms rate for **every** present candidate with the slot — holder or
   not — but **only while a client resolved roles within the last second**:
   a provider may treat the calls as demand (keep its camera open) and idle
   when they stop. A change of who engagement puts first bumps the state
   epoch, so cached per-client roles recompute and `generation_id` moves.
   The masquerade above still applies, so a provider ↔ qwerty swap by
   engagement emits no `XrEventDataInteractionProfileChanged`. The hand-
   tracking, gamepad and rig roles are unaffected.
2a. `t_input_arbiter_install()` then replaces
   `xrt_system_devices::get_roles` with the arbiter's, which re-reads
   presence (rate-limited to ~250 ms) and bumps `generation_id` whenever
   the assignment changes. The OpenXR state tracker picks that up in
   `xrSyncActions` — rebinding actions and queueing
   `XrEventDataInteractionProfileChanged` — and the IPC layer forwards
   `get_roles` to the service, so this works for out-of-process clients
   too. Net effect: arbitration is re-evaluated **per app start and
   mid-session**, including a Leap unplugged (or plugged in) while apps
   are running.
3. Override: `HKLM\Software\DisplayXR\Input\ForceQwerty = 1` (DWORD) on
   Windows; on POSIX a `force_qwerty` file in the per-user manifest dir
   (next to the DP loader's `preferred` file — first byte `'0'` = off,
   anything else = on). Forces the fallback, registry/config-gated by
   convention (not an env var). Providers are then not loaded at all,
   so behavior is bit-identical to a box with none registered.
3b. **`EmulatedControllers = 0`** (ADR-054 §4.7 / N7, #2143) is the *other*
   half: it gates the **qwerty pair**, not the providers. Windows:
   `InputProviders\Policy\EmulatedControllers` (DWORD), HKCU first, then the
   HKLM 64-bit view, HKCU hidden under `InputProviders\Policy\Lock` = 1
   (the shared loader's root-policy read — the same N3 overlay rules as
   `Enabled`, which is why it lives under the class root rather than next
   to `Input\ForceQwerty`). POSIX: the `emulated_controllers` per-user file
   (§3). Absent = 1 = today's behaviour. Read through the input loader
   (`target_input_plugin_get_emulated_controllers()`, value + source
   `user|machine|file|default`), never `getenv` — this is class policy, not
   a component setting. With it at 0, `t_builder_add_qwerty_input()`
   publishes the qwerty **head only** (the look-around fallback is not a
   controller): no qwerty left/right in `xsysd->xdevs`, no floor candidate
   in the arbiter, and the hand roles sit at **-1 whenever no provider is
   present or admitted** — `xrGetCurrentInteractionProfile` returns
   `XR_NULL_PATH`, and the change-only event rule yields one
   `XrEventDataInteractionProfileChanged` per flip, never a storm. A lone
   provider is then still arbitrated (its hands must be able to fall to -1
   and come back), where before a single candidate left the static roles in
   place. Internally the qwerty driver still allocates the controller
   objects — every input front-end addresses them — but as private state
   that is never published and is freed with the head
   (`qwerty_create_devices_ex`). Orthogonal to item 3: `ForceQwerty = 1`
   skips the providers, `EmulatedControllers = 0` withholds the pair, and
   both together is a box with **no hand devices at all** — the builder
   logs one WARN naming both. **Live:** the policy watch covers the root
   already; a flip is applied as a *withdrawal / re-admission* of a pair
   that exists (`t_input_arbiter_set_floor_withdrawn`, exactly how
   `Enabled = 0` withdraws a provider — devices stay resident, the roles
   move on the next `xrSyncActions`), never as a device add: a pair the
   service did not create at start (it started with the gate off) joins at
   its next start, and on a box with no provider resident nothing
   arbitrates, so the gate applies at the next start — both said once in
   the log. The CTS lane relies on the pair being there (3a):
   `scripts/run_cts.ps1` asserts the gate is absent or 1 before a run and
   never writes it.
3c. **`ControllerSwitching`** (ADR-034 Amendment 6) — class policy next to
   `EmulatedControllers`, same read path and overlay rules:

   | Platform | Where | Values |
   |---|---|---|
   | Windows | `InputProviders\Policy\ControllerSwitching` (DWORD), HKCU then HKLM 64-bit view, HKCU hidden under `Policy\Lock` = 1 | `1` = `use` (default), `0` = `presence` |
   | POSIX | `controller_switching` file in the per-user manifest dir | `use` (default) or `presence` (a leading `0` also = presence) |
   | Android | — | always `use` |

   Absent = `use`. `presence` is exactly the pre-amendment walk (item 2).
   Read through the loader (`target_input_plugin_get_controller_switching()`,
   value + source `user|machine|file|default`), never `getenv`. CLI:
   `displayxr-cli input switching use|presence [--machine]` (POSIX refuses
   `--machine`); `input list --json` carries `controller_switching:
   {value, source}`. **Live:** the service's component-policy watch covers it
   (the Windows watch covers the `Policy` root; POSIX folds the file into the
   input fingerprint) and the arbiter applies it at the next resolve; an
   in-process app reads it at its next launch.
3a. **`DXR_INPUT_PROVIDERS=0`** (process environment, all platforms) does the
   same thing for **one process**: the lazy discovery pass in
   `target_input_plugin_get_iface()` is skipped entirely, no provider DLL is
   loaded, `target_input_plugin_get_count()` stays 0, `create_devices` is
   never called, and the hand roles sit on the qwerty floor. One `U_LOG_W` at
   the first (and only) discovery attempt records it.
   Why an env var when `ForceQwerty` exists: a CTS lane needs the suppression
   scoped to the process it launches, and a run killed mid-flight (the harness
   kills on timeout) must not be able to leave the box with input disabled
   machine-wide. `scripts/run_cts.ps1` sets it for every run (#1545, #1523) —
   the Ultraleap provider re-spins its LeapC thread pool on every
   `xrCreateInstance`, and those threads are what race the NVIDIA GL ICD the
   display-plug-in side drags in (see
   [plugin discovery §2.2](plugin-discovery.md#22-dxr_plugin_exclusive--load-one-plug-in-and-nothing-else)).
   Set it in the inherited environment — CRT `getenv`, same caveat as the DP
   loader's env overrides.
4. **Input providers never supply the head**, and the DP's
   `set_pose_source` hook is still the single binding point. What that
   hook receives is now the runtime's own **rig composer** rather than the
   qwerty HMD directly (#1380): while the rig role sits on the qwerty
   floor the composer is a pass-through and the head pose is unchanged;
   when a provider's `NAVIGATION` device holds the role the composer owns
   alignment, continuity, recenter and composition. See §4b. The display
   plug-in contract and `sim_display` are untouched by this.
5. **Hand-tracking roles (#825 Tier 2):** the same pass fills
   `static_roles.hand_tracking.{unobstructed,conforming}.{left,right}` from
   provider devices that set `supported.hand_tracking` and carry the
   matching `XRT_INPUT_HT_*` input (unobstructed = optical, conforming =
   controller-derived; first claimant wins per role). These roles gate the
   whole `XR_EXT_hand_tracking` path — system support, tracker creation,
   joint locates. Providers without hand-tracking inputs leave them empty;
   that is a valid configuration, never a failure. These *static* roles
   are build-time gating only — they decide whether `XR_EXT_hand_tracking`
   is supported at all, and by the `xrt_system_devices` contract they never
   change. The **source** a tracker actually reads from *is* arbitrated:
   ADR-034 Amendment 3's addendum added dynamic
   `hand_tracking.{unobstructed,conforming}.{left,right}` roles to
   `xrt_system_roles`, filled by the arbiter's presence walk under the same
   `generation_id` as the other roles (`target_input_arbiter.c`), so the
   hand-tracking source follows provider presence like every other role. An
   absent optical tracker reports inactive joints, and qwerty has no hand
   tracking to fall back to anyway.
6. **`sim_input` is opt-in at run time.** Being registered is not enough
   — its `probe()` declines unless its `ENABLE` setting is 1, read through
   the host iface (§3 *Component settings*): `displayxr-cli input set
   sim-input ENABLE 1`, or `DXR_INPUT_SIM_INPUT_ENABLE=1` in the
   environment of the process loading the runtime, which always wins. It
   synthesises a fixed motion pattern, so an un-gated registration silently
   displaces the qwerty fallback with phantom controllers. The pre-N3c
   `DXR_SIM_INPUT=1` keeps working for one release as an alias (deprecation
   WARN). Environment caveat unchanged: set a variable process-level — the
   runtime DLL has its own static-CRT environment block. Its `ENGAGE_MS`
   setting (default 0) is the in-tree reference for `get_engaged_ns`
   (item 2b): with n > 0 each active hand engages every n ms; at 0 the slot
   stays NULL and sim-input is engaged continuously while present.

## 4a. Pose anchoring — provider volumes are rig-relative

A provider reports poses **in its own tracking volume** and says nothing about
where that volume sits in the world. On a 3D display the sensor is on the same
desk as the panel, so the runtime anchors the volume to the **rig** — the
viewer + display + sensor assembly — not to the world (ADR-034 *Amendment 2*).

The resulting rule, which **diverges from HMD VR semantics and is not
optional**:

| Motion | Do provider poses follow? |
|---|---|
| Voluntary rig motion — WASD, mouse-look, a navigating provider (§4b) (translation **and** rotation) | **Yes** |
| Eye-tracked head parallax — viewer leaning/tilting about the panel | **No** |

Mechanically, `t_builder_add_input_provider_devices()` lists every device it
adds in `u_builder_roles_helper::rig_relative`; the builder helper then calls
`u_space_overseer_set_rig_source()` (the head *device* pose — the fly camera;
eye tracking lands later at view-pose level and never reaches it) and
`u_space_overseer_set_device_rig_relative()` per device. The space overseer
re-parents each device's tracking-origin space onto a `U_SPACE_TYPE_RIG` node
that resolves to `rig_now ∘ inverse(rig_initial)` — how far the rig has
**travelled**, captured once at build time.

Provider-facing consequences:

- **Do not compose a camera or navigation transform in the provider.** It will
  be applied twice. Report the sensor's honest reading plus your mount offset.
  This holds *especially* for a provider that also drives the rig (§4b): the
  rig role is what makes pre-compensating look attractive, and the runtime is
  about to add exactly the transform you would be subtracting.
- Your mount offset stays in **stage** space (e.g. a desk-mounted Leap at
  y≈1.45). Do not express it relative to the head. A provider that navigates
  can skip the mount offset entirely by publishing `XRT_TRACKING_TYPE_RIG_LOCAL`
  origins instead — see §4b.
- Devices that already follow the camera themselves are excluded — qwerty's
  controllers parent to the qwerty HMD (`follow_hmd`) and are never marked.
- Applies uniformly to grip/aim action spaces, `xrLocateSpace`, and the
  hand-joint base (`xrt_space_overseer::locate_device`), in-process and over
  IPC. Covered by `tests/tests_space_overseer_rig.cpp`.

## 4b. Rig (navigation) role

**Normative.** Implemented in `target_input_arbiter.c` (the role),
`target_rig_composer.c` (the composition), `u_builders.c` (rig-local
anchoring) and `target_input_plugin_loader.c` (the host iface). Rationale:
ADR-034 *Amendment 4*; issue #1380.

A provider may also drive the **rig** — the virtual camera the qwerty fly
camera drives today — by creating ONE extra device of type
`XRT_DEVICE_TYPE_NAVIGATION` alongside its controllers.

### 4b.1 The walk, and the floor

The role is arbitrated by the same presence-ranked walk as the hands, under the
same `generation_id` and over the same IPC `get_roles` forwarding:

> `xrt_system_roles::rig` is the `xsysd->xdevs` index of the navigation device
> of the **highest-priority (lowest ProbeOrder) candidate that has one and
> reports `PRESENT`**. Otherwise **-1**.

Two things differ from the hand walk:

- **At most one navigation device per provider.** The arbiter scans the
  devices a provider returned from `create_devices` in order and takes the
  first `XRT_DEVICE_TYPE_NAVIGATION` one. A second is a provider bug: the
  arbiter logs `input arbiter: '<id>' supplied more than one navigation
  device — keeping '<name>'` and ignores the rest. (Picking a different one
  per build would be worse than picking the first.)
- **The floor is an absence, not a candidate.** Qwerty supplies no navigation
  device, so `rig == -1` is not "no rig" — it means *the runtime's own fly
  camera holds it* (WASD / mouse-look). That is what every box without a
  navigating provider reports, and what an unplug falls back to.

A provider may supply a navigation device and **no** controllers; it is still a
candidate and can still win the rig. Conversely a provider with controllers and
no navigation device is skipped by this walk entirely and can never hold the
rig, whatever its ProbeOrder (`tests/tests_input_arbiter_rig.cpp`).

The arbiter normally declines to install itself when there is nothing to
arbitrate (fewer than two candidates). A navigation device is the exception:
the builder has no static seed for `roles.rig`, so a **lone** navigating
provider still forces the arbiter in. Without that, `roles.rig` would stay at
its `XRT_SYSTEM_ROLES_INIT` default of -1 and the provider would never be
reached.

### 4b.2 Presence is not validity

Presence semantics are the hands' exactly: a NULL `get_presence` (or a
`struct_size` predating the slot) means "assume present", `UNKNOWN` means not
present, and verdicts are cached ~250 ms.

Pose validity is a **different axis**, and conflating the two is the mistake
this section exists to prevent:

| | Answers | Owned by | Effect |
|---|---|---|---|
| **Presence** (`get_presence`) | "is my hardware/transport attached?" | the provider's transport thread | decides **who holds the role** |
| **Validity** (`relation_flags` on `NAVIGATION_POSE`) | "do I have navigation authority right now?" | the provider's solver | decides **whether the rig advances** |

A provider that is plugged in but has nothing to say (optical loss, a rest
state, no controller being held) clears the pose's `POSITION_VALID` /
`ORIENTATION_VALID` bits and **keeps** the role; the rig holds its last pose
and resumes from there. Flipping presence for the same condition hands the rig
to the fly camera and takes it back a frame later, which is visible as a jump
and is a provider bug.

### 4b.3 What the provider publishes

`get_tracked_pose(XRT_INPUT_GENERIC_NAVIGATION_POSE, t)` returns **N(t): the
absolute pose of the rig in the provider's own navigation frame F.**

- F is right-handed, +Y up, −Z forward, metres, gravity-aligned. Its **origin
  is private to the provider** and is never interpreted by the runtime —
  identity means "rig at F's origin", nothing more.
- N is **not** a delta, **not** a viewer or eye pose, and **un-parallaxed**.
  Eye tracking is applied later, at view-pose level, and must never enter N.
- `relation_flags`: set `POSITION_VALID | ORIENTATION_VALID` when the provider
  has navigation authority; clear them to say "hold the rig". The `*_TRACKED`
  bits are **ignored** for this input.
- The runtime guarantees nothing across separate `get_tracked_pose` calls. A
  provider serves navigation, grip/aim and joints for a requested timestamp
  from **one latched publication**, so that a head poll and an action sync in
  the same frame see a coherent set.

`XRT_INPUT_GENERIC_NAVIGATION_RECENTER` (boolean, optional) is a **durable
level with a publication timestamp**, not a pulse:

- `value.boolean = true`, `timestamp` = the monotonic time of the most recent
  reset. **Never cleared.** The composer consumes a recenter when
  `timestamp > last_consumed`.
- Publish the new timestamp **and** the post-reset N in the *same* publication
  (one atomic swap of the latched frame). No generation gating and no
  invalid-until-delivered hold are needed.
- Nothing else is signalled through this input. Two resets between polls
  collapse to one — a recenter is idempotent, and alignment uses the N
  delivered with the latest one.

The level-with-timestamp shape is **required, not stylistic**: `xrSyncActions`
sweeps `update_inputs` over every device (`oxr_input.c`), and so do the IPC
server's own device passes. A one-update pulse can therefore be consumed by
action sync before the composer ever polls the device, and the recenter would
be lost — intermittently, and preferentially on the IPC path. A durable level
is immune to how many `update_inputs` calls land in between.

### 4b.4 A navigating provider's other devices are rig-local

A provider that drives the rig **must** publish its controllers and hand joints
with `tracking_origin->type = XRT_TRACKING_TYPE_RIG_LOCAL`: poses relative to
the **display plane** — origin at the display centre, +X right, +Y up, +Z
toward the viewer, metres.

The builder (`u_builders.c::anchor_rig_local_origins`) sets such an origin's
`initial_offset` to `rig_initial`, the rig pose at system build, so the
*Amendment 2* rig delta collapses:

```
world = rig(t) ∘ inv(rig_initial) ∘ rig_initial ∘ L = rig(t) ∘ L
```

Consequences for the provider: **no standing height, no mount offset, no rig
knowledge.** Several devices may share one origin (a left/right pair normally
does) — it is anchored once, and logged once at init as `Rig-local origin
'<name>' anchored at the initial rig (x, y, z)`.

`XRT_TRACKING_TYPE_OTHER` / `_NONE` origins are untouched and keep today's
stage-anchored behaviour, mount offsets included, so no existing provider
changes. Anchoring happens after the head's own tracking origin is settled and
before the space overseer freezes each origin offset into a space; a head with
no valid pose anchors at identity.

**Where RIG_LOCAL lands, per session.** `L` is relative to the display plane
**the session sees** — the same plane `xrLocateViews` reports as
`XrViewDisplayRawDXR::displayPlanePose` and `XR_DXR_display_info`'s DISPLAY
space resolves to:

| Session | Its display plane | A RIG_LOCAL pose `L` is located at | Follows rig motion? |
|---|---|---|---|
| Runtime-window (`_hosted`, legacy, headless without `XR_DXR_display_info`) — in-process or IPC | the head **device** pose, `rig(t)` (the head device *is* the display; qwerty / the composer move it) | `rig(t) ∘ L` | yes — WASD / mouse-look / a navigating provider carry the hands (*Amendment 2*) |
| External-window (`_handle`, `_texture`, the Unity / Unreal providers) — in-process or IPC | the **identity at the head's tracking origin** (the app owns its camera) | `T_origin ∘ L` | no — the plane does not move for these sessions, so neither do the hands; an app that navigates declares it through `XR_DXR_view_rig` |
| Bridge relay (`XR_DXR_display_info` + headless) | the identity at the head's tracking origin | `T_origin ∘ L` | no |
| Any non-bridge session whose latest `xrLocateViews` chained an `XR_DXR_view_rig` descriptor | where the **rig** puts the physical plane: `P` = display point 0 mapped through the rig, unscaled (for a full-canvas display rig, its `pose` — the `displayPlanePose` it reports) | `T_origin ∘ P ∘ L` | with the rig — the app's views are in that frame |

The space graph is session-agnostic and resolves every RIG_LOCAL pose to the
first row's `rig(t) ∘ L`. For the other two rows the state tracker re-bases it
(`oxr_space.c::rig_local_plane_correction`, applied in `xrLocateSpace(s)` and
in the hand-joint base): `C = T_origin ∘ P ∘ inv(head_pose(t)) ∘ inv(T_origin)`,
left-multiplied, in the locate's base space, with `P` = identity for the
external-window / bridge-relay rows and the rig plane for the last row.

The rig row exists because `T_origin` is **not** the LOCAL origin: LOCAL sits
at the builder's 1.6 m stage offset above the head's tracking origin, so an
external-window app that chains a display rig at the LOCAL origin (the normal
engine / cube-app shape) saw its hands and grips 1.6 m below its views until
the rig plane was honoured (cube_handle_metal_macos `--hands --actions`,
wrists at y ≈ −1.56 instead of +0.04). `P` is recorded by each view locate
(in service mode from the server's applied rig, `raw.display_pose`) and cleared
by one that chains nothing; it goes through the same point core as spec-4
rig-scoped joints (`m_view_rig_map_relation`), so for an unscaled rig
base-space joints equal the rig-frame ones, and the spec-4 mapping undoes `P`
rather than the physical plane for RIG_LOCAL sources. A hand locate **before**
the session's first rigged view locate still sees the physical plane — locate
views first each frame. Pinned by `tests_oxr_view_space_rig_hands` ("RIG_LOCAL
hands follow the display plane a chained XR_DXR_view_rig puts in LOCAL"). It has to be per session: one
graph serves sessions of both kinds (several IPC clients), and an
external-window session flips the head device into `EXT_APP_MODE` — which
replaces the fly-camera pose with the display plug-in's static nominal viewer —
at `xrCreateSession`, long after the builder anchored the origin. Before this
re-base, an external-window session on a no-qwerty build saw the hands offset
by exactly that nominal viewer (≈ +0.1 m up, +0.6 m out), and on a qwerty build
by the 1.6 m fly-camera height. Pinned by
`tests_oxr_view_space_rig_hands` ("RIG_LOCAL hands sit on the display plane
the SESSION sees"): the runtime-window and bridge-relay classes must put the
same scripted hands at the same display-plane point to the millimetre.

**The recipe for a solver that already outputs world poses.** If your tracking
stack produces product poses `C_W` in its own world `W` that *already* contains
your navigation `P(t)` — which is the normal shape for a camera-drag solver —
do not try to undo it. Publish

```
N(t) = P(t)                 as NAVIGATION_POSE
L    = inv(P(t)) ∘ C_W      as the controller/joint pose, RIG_LOCAL
```

Then `world = rig(t) ∘ L = T_world_F ∘ P(t) ∘ inv(P(t)) ∘ C_W = T_world_F ∘ C_W`:
the runtime's single alignment maps your entire product world, with no
provider-side reference frame `P0` and no runtime rig knowledge on your side.

### 4b.5 What the runtime does with the role — the composer

The runtime binds a runtime-owned `xrt_device`, the **rig composer**
(`target_rig_composer.c`), as the head's pose source through the display
plug-in's existing `set_pose_source` hook. Its `XRT_INPUT_GENERIC_HEAD_POSE`
**is** `rig(t)`. The composer is not in `xsysd->xdevs`, holds no role, and is
created and destroyed by the sim-display builder.

Per poll of the head pose, in this order:

1. **Roles.** Read `xrt_system_devices_get_roles`. A change of the **holder
   index** — not every `generation_id` bump — is a handover. `rig_last` is
   left untouched: the rig continues from where it *is*, never from the new
   holder's frame. Taking the role drops the alignment (it is recomputed
   below, on the first valid N) and clears any pending recenter; handing back
   to the floor re-seeds the fly camera at `rig_last` so WASD continues from
   there. One WARN per handover, never per frame.
2. **Floor.** With `rig == -1` the composer returns the fly camera's relation
   verbatim and records it as `rig_last`. **WASD / mouse-look act only here** —
   while a provider holds the role the fly camera is not read at all.
3. **Provider.** `update_inputs` on the navigation device, then the durable
   RECENTER read, then `get_tracked_pose(NAVIGATION_POSE, at_timestamp_ns)`.
   `valid` = both VALID bits; TRACKED bits ignored.
4. **Epochs.** Recenter wins when two are due in one poll:
   - *pending recenter with timestamp r*: on the first sample that is valid
     **and** whose requested time is `>= r`, set
     `T_world_F = rig_initial ∘ inv(N)`, return `rig_initial` exactly, clear
     the pending flag. Otherwise hold `rig_last` and stay pending.
   - *not aligned* (fresh handover, or validity was lost): on the first valid
     N, `T_world_F = rig_last ∘ inv(N)`, then compose. Otherwise hold.
   - *aligned but N invalid*: drop the alignment (so the next valid sample
     re-aligns at the held rig) and hold.
   - *aligned and valid*: compose.
5. **Compose.** `rig(t) = T_world_F ∘ N(t)`, with `T_world_F` on the **left**,
   so a provider-local step `N → N ∘ T(0,0,-1)` moves the rig along its own
   forward. Emitted with `POSITION|ORIENTATION_VALID|TRACKED` and zero
   velocity.

**Continuity invariant.** At a handover, at an invalid → valid transition and
at any non-recenter re-alignment the first composed pose equals `rig_last`
exactly, because `(rig_last ∘ inv(N)) ∘ N = rig_last`. The single deliberate
jump is a recenter, which lands on `rig_initial`.

**No history.** Alignment state is piecewise-constant: a head query for a
timestamp *before* a recenter, issued after that recenter aligned, composes
with the current alignment. This is documented, not history-corrected.

Covered by `tests/tests_rig_composer.cpp` (alignment with non-identity N,
invalid-hold, the four recenter cases, handback re-seeding, hand-role churn,
concurrent polls) and `tests/tests_input_arbiter_rig.cpp` (the walk).

### 4b.6 Host iface and display geometry

`xrtInputPluginNegotiate` receives a `struct xrt_input_plugin_host_iface *`
that is **process-lifetime runtime storage** — one static, shared by every
provider in the process. A provider may retain it, and the callbacks inside it,
for as long as it lives. (It was a stack local before #1380; a provider that
kept the pointer read dead stack.)

`host->get_display_geometry(&geo)` fills `struct
xrt_input_host_display_geometry` — panel width/height in metres and the
**nominal** viewer position (display-centre origin, +Z toward the viewer). It
is geometry for a solver, never a head pose and never the tracked eyes.

Pointer lifetime and data readiness are separate, and so are their failures:

| Situation | Result | Out struct |
|---|---|---|
| Slot is NULL | — | older runtime; no geometry exists to ask for |
| `inout == NULL`, or `struct_size` too small to hold even `struct_size` | `XRT_ERROR_INPUT_UNSUPPORTED` | untouched |
| Called before the cache is published (e.g. from negotiate) | `XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY` | **untouched** — retry later |
| Cache published | `XRT_SUCCESS` | filled, prefix semantics |

The cache is populated by the system builder from the display plug-in's
`get_display_info`, **after** the head device exists and **before** any
provider's `create_devices` runs. So the data is ready from `create_devices` on
and static for the life of the system. Set `struct_size = sizeof(...)` before
the call; the runtime copies `min(your struct_size, its own)` bytes and leaves
your `struct_size` as you set it. Publication is logged once
(`input plugin loader: display geometry published to providers: …`), so a bug
report shows whether providers ever got geometry at all.

**`host->get_camera_pose(host, camera_key, screen_key, &pose)`** — where a camera sits relative
to a screen, for a camera-based input provider (hand tracking off a webcam). Carved from the former
`reserved[3]` (size and offsets unchanged, API major stays 1; NULL on an older runtime, announced by
`XRT_INPUT_PLUGIN_HOST_HAS_GET_CAMERA_POSE`). It is the **same function** the viewer-tracking host
iface carries: the runtime's camera calibration store, resolved display-declared > user-calibration
> placement-hint > default-prior (top centre of the bezel, `T = (0, H/2 + 8 mm, 0)`), so the
answer is never "not found" and always says its `source`. A calibration done once for head
tracking therefore serves the hands with no second procedure, and an uncalibrated camera gets the
same prior head tracking assumes. Store, precedence, placement geometry and the per-user iris
scale: `tracking-provider-discovery.md` §6 (*Camera calibration store*); struct:
`xrt/xrt_camera_pose.h`. Read it at `create_devices` (or on a slow timer, following `generation`),
never per frame.

### 4b.7 Testing without hardware

`sim_input` carries a scripted navigation device so the role walk, the
handover and the recenter path are exercised in CI with no hardware — see §5
for the device and the `DXR_SIM_INPUT*` environment gates that drive it.

## 4c. Gamepad role

**Normative.** Implemented in `target_input_arbiter.c` (the role) and
`target_builder_input_provider.c` (the device-type case); filled in-tree by the
`gamepad` provider (§5).

A provider may supply ONE device of type `XRT_DEVICE_TYPE_GAMEPAD` whose
`name` selects a gamepad interaction profile (`XRT_DEVICE_XBOX_CONTROLLER` =
`microsoft/xbox_controller`, the only one in `bindings.json`). It fills
`xrt_system_roles::gamepad` / `::gamepad_profile` — the `/user/gamepad` top
level path the action system has carried since Monado — which the legacy
builder assigned statically and the component path never assigned at all.

The role is arbitrated by the same presence-ranked walk as the hands and the
rig, under the same `generation_id` and over the same IPC `get_roles`
forwarding:

> `xrt_system_roles::gamepad` is the `xsysd->xdevs` index of the gamepad
> device of the **highest-priority present candidate that has one**.
> Otherwise **-1**, and `gamepad_profile` is `XRT_DEVICE_INVALID`.

What differs from the hands: **there is no floor candidate** (qwerty has no
pad), so -1 is the ordinary state of a box without a pad provider or with its
pad unplugged; the OpenXR layer then reports `XR_NULL_PATH` for
`/user/gamepad`, exactly as it does for an unassigned hand under N7. What
differs from the rig: the builder DOES seed a static assignment
(`u_builder_roles_helper::gamepad`, the highest-ranked candidate's device),
because `u_builder_setup_tracking_origins` and the static-roles cache expect
one — and the arbiter installs itself whenever a gamepad candidate exists
(like a lone navigating provider forces it in, §4b.1) so that seed is
presence-gated from the first `get_roles` on. A seeded-but-unplugged pad reads
-1 before any app can see it.

**Presence is "last-used input wins", not "plugged in".** A pad that merely
lives in a USB port must not hold the rig and the hands forever — that would
silently disable WASD / mouse-look for everyone whose pad is plugged in but
unused. So the in-tree provider reports `PRESENT` only while a pad is
connected **and** was touched (any button, a stick past the deadzone, a
trigger pull) within `IDLE_TIMEOUT_S` seconds (default 20; `0` = the
connected-only rule), timestamping activity on its poll thread; a freshly
plugged pad counts as untouched. Touch the pad and it takes the gamepad, rig
and hand roles at the arbiter's next presence poll (≤ ~250 ms, §4b.2); leave
it and the floor (fly camera / qwerty) comes back `IDLE_TIMEOUT_S` later.
This is a provider policy, expressed entirely through `get_presence` — the
arbiter's walk is unchanged, and a vendor provider may choose another rule.

At most one gamepad device per provider; a second is a provider bug (warned,
first kept), same rule as the navigation device. The gamepad device is NOT
rig-composed through the space graph (it has no pose), and it is not a hand:
`t_input_arbiter_provider_holds_roles` — the "does a provider hold the
HANDS" question `selftest` asks — ignores it, so a pad-only provider leaves
the hands to the floor and does not fail the self-test.

Why this matters on a flat 3D display: there is no tracked head and no
tracked hands, so VR-authored content needs a synthesised rig, and the
in-tree qwerty fly camera is withheld from HANDLE apps because their own
window owns the keyboard (`oxr_input.c`, `has_external_window`). A pad is
focus-independent, so one provider can fill all three roles for such an app at
once: `/user/gamepad` (this section), the rig (§4b — left stick = translate,
right stick = look / orbit, triggers = up / down, following the rig rule of
ADR-034 *Amendment 2*: navigation moves the rig, eye-tracked head parallax is
applied separately at view level and never enters N), and, optionally, an
emulated `khr/simple_controller` pair driven by the pad's buttons so the app
gets `xrSyncActions` controllers for the first time.

## 5. In-tree reference providers

| Provider | Status | Purpose |
|---|---|---|
| `sim_input` | **Shipped and packaged** (`src/xrt/drivers/sim_input/`, plug-in DLL `DisplayXR-SimInput`; the Windows installer registers the `sim-input` row and allowlists the DLL, the `.deb` / tarball / `.pkg` ship it with `200-sim-input-input-provider.json` — ADR-054 §2 clause 3; inert until `ENABLE` = 1) | Deterministic synthetic motion controllers (circular motion, scripted button presses; `khr/simple_controller`) — hardware-free CI gate, adapted from Monado's `simulated_controller.c`. Also serves scripted 26-joint hand tracking (#825 Tier 2: curl wave over the same analytic circle, via `u_hand_simulation`) — the hardware-free test vehicle for `XR_EXT_hand_tracking`. Hands and controllers are **display-plane-relative** (`RIG_LOCAL`, §4b.4) and placed in front of the glass; `HANDS` / `PINCH` / `HAND_DISTANCE_M` shape them (below, #2191). ProbeOrder 200. **Test vehicle only — not a product path:** `probe()` declines unless `DXR_SIM_INPUT` is set (§4.6). Dev builds stage it automatically (`build_macos.sh` / `build_linux.sh`); Windows registers via `register_dev_plugin.bat input sim`. |
| `net_input` | **Shipped** (`src/xrt/drivers/net_input/`, plug-in DLL `DisplayXR-NetInput`) | Loopback-TCP-fed devices — an external tracking process feeds timestamped poses + button state and receives haptic events back (wire protocol below). Opt-in: never registered by default. |
| `gamepad` | **Shipped and packaged** (`src/xrt/drivers/gamepad/`, plug-in DLL `DisplayXR-Gamepad`; the Windows installer registers the `gamepad` row at ProbeOrder 150 and allowlists the DLL; `build_macos.sh` / `build_linux.sh` stage `150-gamepad-input-provider.json`; Windows dev boxes: `register_dev_plugin.bat input gamepad`) | The one controller every desk has, on the existing input-provider class (§4c): a `XRT_DEVICE_TYPE_GAMEPAD` device on `microsoft/xbox_controller` (every button, stick, trigger and the four haptic outputs; `XR_EXT_dpad_binding` works over its sticks through the runtime's generic dpad emulation), a NAVIGATION device so a present pad drives the rig (§4b: left stick = translate in the rig's yaw frame, right stick = yaw / pitch, triggers = up / down, right-stick click = recenter — the durable level-with-timestamp of §4b.3, with the pose reset to F's origin in the same publication), and — `EMULATE_CONTROLLERS` = 1, the default — an emulated left / right `khr/simple_controller` pair at fixed display-plane-relative (`RIG_LOCAL`, §4b.4) poses: shoulders / X / A = select, View / Menu = menu, vibration → the pad's motors. **Presence-gated, so registered-by-default is inert:** `probe()` claims wherever the platform has a pad reader (XInput on Windows, GameController.framework on macOS with `shouldMonitorBackgroundEvents` so it is focus-independent too) and declines where it has none (desktop Linux — an evdev backend is the follow-up); `get_presence` follows the hub's hot-plug poll (250 Hz attached, 2 Hz detached) under the "last-used input wins" rule of §4c — touching a pad mid-session takes the gamepad, rig and hand roles at the arbiter's next presence poll; leaving it idle for `IDLE_TIMEOUT_S` (or unplugging it) hands them all back. Settings (ADR-054 decision 7, `displayxr-cli input options gamepad`): `DEADZONE` (radial, 0.15), `NAV_SPEED` (m/s, 1.0), `NAV_TURN_SPEED` (deg/s, 90), `NAV` (1), `EMULATE_CONTROLLERS` (1), `INVERT_LOOK_Y` (0), `IDLE_TIMEOUT_S` (20; the "last-used input wins" window of §4c — `0` = connected-only). The whole mapping policy is one table (`gamepad_map.c`, pinned by `tests/tests_gamepad_map.cpp`), so a per-title bindings mechanism (#1381) replaces the table, not the devices. |
| `touch_input` | **Shipped on Android** (`src/xrt/drivers/touch_input/`; `libdxri100_touch_input.so` in the runtime APK, `DisplayXR-TouchInput` on desktop builds where it is a dev-only artefact — not staged, not registered, `probe()` declines unless `ENABLE` = 1 because nothing feeds pointer events there) | The first Android input provider (#132, §5.2): touch gestures → a `NAVIGATION` device (the rig role of §4b) + an emulated `khr/simple_controller` (`ANY_HAND`, aim ray under the finger, SELECT on tap / hold). ProbeOrder 100. Presence is unconditional — the touch screen is the hardware. Settings (§3): `ENABLE` (Android default 1, `0` withdraws it), `CONTROLLER` (default 1; `0` = navigation only), `SENSITIVITY` (orbit-rate multiplier, default 1.0). |
| `ultraleap` | **Shipped, SDK-gated** (`src/xrt/drivers/ultraleap/`, plug-in DLL `DisplayXR-Ultraleap`; builds only where the Ultraleap Gemini SDK / LeapC is found — `LEAPSDK_DIR`) | Hand-as-motion-controller provider (#825 Tier 1, adapted from Monado's removed `ultraleap_v5`): palm pose → grip/aim, pinch → select, grab → menu; 26-joint sets filled for the Tier-2 `XR_EXT_hand_tracking` wiring. On Windows the SDK's `LeapC.dll` is staged **next to the plug-in** (#933) — the loader's `LOAD_WITH_ALTERED_SEARCH_PATH` then resolves it app-locally instead of from the system PATH, where the LeiaSR Platform ships its own shadowing copy. Opt-in: never registered by default. |

**`sim_input` and the rig role (#1380).** A *second*, independent opt-in,
`DXR_SIM_INPUT_NAV=1`, adds one scripted `XRT_DEVICE_TYPE_NAVIGATION` device
("Sim navigation") so the rig role of §4b runs hardware-free. Default **off**,
so every pre-existing `DXR_SIM_INPUT=1` run is byte-for-byte unchanged: no
navigation device, the rig stays on the qwerty floor. With it on, `N(t)` is a
slow figure-eight plus yaw in the provider's own frame F — a pure function of
the requested timestamp, so prediction is exact and CI is reproducible. The
controllers are `XRT_TRACKING_TYPE_RIG_LOCAL` either way (since #2191; with
`NAV` = 1 that is what the contract *requires* of a provider that navigates,
§4b.4). Two optional
fault knobs, both off by default: `DXR_SIM_INPUT_NAV_HOLD_MS=<n>` makes the
pose report invalid for 500 ms every *n* ms (the composer's hold / re-align
path) and `DXR_SIM_INPUT_NAV_RECENTER_MS=<n>` fires a scripted reset every *n*
ms, advancing the durable `NAVIGATION_RECENTER` timestamp **and** jumping the
scripted pose in the same publication. All three are env vars, not registry
values, for the reason `DXR_SIM_INPUT` is (§4.6): per-run developer switches,
not machine configuration — and the process-level caveat applies, set them
before launching the host process.

**`sim_input` hands (#2191).** Each hand's root — and the grip / aim / palm
poses, which ride the same circle so a marker drawn from the grip action lands
inside the rendered hand — sweeps a circle (4 s period) in the display
plane's XY, centred `HAND_DISTANCE_M` in front of the glass. Placement is by
**projection from the nominal viewer**: each centre sits on the viewer's
sightline to the display-plane point (±0.30·H, 0), so it projects
vertically centred and 0.30·H left / right of the display centre whatever the
viewer height and hand distance; the circle's projected radius is 0.10·H
capped at 4 cm. (Placing at a fixed height does not work: the nominal eye sits
above the centre, so a point floating in front of the glass at the centre's
height projects below it — the earlier −0.10·H placement landed on the bottom
edge of the view on a 0.30 × 0.19 m panel. Same intent as the reference viewer
app, whose Leap hands land centred on the display at a typical hand height.)
H and the nominal viewer come from `host->get_display_geometry` (§4b.6 — the
display plug-in's `get_display_info`); a host that publishes none falls back
to H = 0.40 m and the viewer (0, 0.10, 0.60). `HAND_DISTANCE_M` stays metres
and is capped at 0.8 of the viewer distance. The poses are published on a `RIG_LOCAL` origin, so they sit in
front of the panel the session sees in every reference space (§4b.4).
(Before #2191 a NAV-off run kept a stage-anchored circle that ended up 5 cm
*behind* the glass on a headless sim-display system and far out of view on
others.) The plug-in logs the resolved script once at `create_devices`
(`sim-input: hands=LR pinch=wave; display height 0.196 m (host display geometry); circle centres (display-plane frame, …)`).
Settings, through the options contract above (no legacy aliases — they are
new):

| Key | Type | Default | Effect |
|---|---|---|---|
| `HANDS` | enum | `both` | `both` \| `left` \| `right` \| `none` — which hands track. A hand left out stays a device (roles unchanged) but is never active: invalid poses, `isActive` false. |
| `PINCH` | enum | `wave` | `wave` — the #825 open→fist curl wave (3 s), both hands staggered half a cycle; `hold` — thumb and index tips held pinched (≈ 6.5 mm apart), other fingers relaxed; `open` — every finger relaxed, never a pinch. `HANDS=right PINCH=hold` is a single sustained pinch whose root moves on the circle: a one-hand drag. |
| `HAND_DISTANCE_M` | float | `0.25` | distance of the circle centres in front of the glass, metres; ≤ 0 / unparseable = the default |
| `HAND_SCALE_H_PER_M` | float | `2.5` | the hand's **own** size, in display heights per metre of real hand: joint offsets about the root (wrist / palm) and joint radii (and joint linear velocities relative to the root) scale by `gain·H`. The default is the Ultraleap provider's relative-mapping gain, so the sim hand is drawn the way the reference viewer app shows a Leap hand — a 10 cm hand on a 0.194 m panel shows as 0.25·H ≈ 4.9 cm. ≤ 0 = 1:1 metres (the pre-gain behaviour). The circle centres and radius are unchanged (the circle is already in display heights) |

**The sim hands are display-metric and do not follow the window canvas.**
`H` in the placement and in `HAND_SCALE_H_PER_M` is the PHYSICAL display
height, never the window or canvas, so resizing or moving an app's window
neither moves nor resizes the hands — the same as a real Leap seen through the
window as a portal (the Ultraleap provider's relative mapping is display-metric
in the same way).

Pinned by `tests/tests_sim_input_hands.cpp` (the pure script: placement, the
pinch-hold tip gap through the real `u_hand_simulation`, the hand scale — 0.25·H
± 10% per 10 cm of real hand by default, 1:1 at 0) and end to end by the
`[rig_hands]` / `[rig_hands_hold]` arms of `tests_oxr_view_space` (wrists
located in LOCAL against the `displayPlanePose` `xrLocateViews` reports).

### 5.2 touch_input — gestures, and where its events come from (#132)

On a flat 3D display there is no tracked head or hand, so content needs a
synthesised rig; on desktops the qwerty floor builds one from the keyboard and
mouse, and on a tablet the only thing the user has is the touch screen. The
touch provider is therefore first a **navigation** provider (§4b) and only
second a controller.

**Events reach it through the host iface, never through platform code of its
own.** `xrt_input_plugin_host_iface::poll_pointer_events` (carved from
`reserved[2]`, API major still 1, cover-check `struct_size` before use) drains
a process-wide ring of `struct xrt_input_pointer_event` — `DOWN` / `MOVE` /
`UP` / `CANCEL`, one event per pointer, surface pixels plus the surface extent
— that every surface the runtime owns pushes into (`util/u_pointer_sink`):

| Surface | Who pushes | Status |
|---|---|---|
| the runtime-spawned hosted `SurfaceView` (`_hosted`, in-process) | `MonadoView.onTouchEvent` → `nativeTouchEvent` (RegisterNatives in `android_custom_surface.cpp`); the app still receives every event through `Activity.dispatchTouchEvent` as before | **done** |
| an app-owned surface (`XR_DXR_android_surface_binding`) | the app, with `xrSubmitAndroidInputEventDXR` (spec v3) — it owns its `MotionEvent`s, so it chooses which to submit | **done** (in-process sessions) |
| the workspace overlay (ADR-036 D4): the controller owns the fullscreen surface | the controller, once #1996 settles how it forwards hit-tested events to a member client; the queue is per-process, so over IPC the events must cross to the service | **open — #1996** |

The provider owns every gesture rule (ADR-034 Amendment 4: no gesture policy
enters the runtime), in `touch_gesture.{h,c}` — platform-free, a pure function
of the events and timestamps fed in, pinned by `tests_touch_gesture`:

| Gesture | Effect |
|---|---|
| one-finger drag (past the slop) | **orbit** the rig about the pivot: yaw from dx (a full-width drag = half a turn), pitch from dy (full height = a quarter turn, clamped at ±85°) |
| two-finger pinch | **dolly**: pivot distance `d = d_at_pinch_start * dist0 / dist`, clamped to 0.1–10 × the rest distance |
| two-finger drag (centroid moves) | **pan**: the pivot slides in the rig's own XY plane, 1:1 with the fingers in display metres |
| tap (up within 300 ms, inside the slop) | **SELECT click**: a 120 ms pulse — longer than one `xrSyncActions` interval at any frame rate, so a sweep never eats it |
| press-and-hold (still past 300 ms) | **SELECT held** until the finger lifts; the aim ray follows the finger and the motion is *not* an orbit (a select-drag) |
| double tap | **recenter**: orbit / pan / dolly reset, and the durable `NAVIGATION_RECENTER` level advances (§4b.3) |

`N(t)` is absolute in the provider's frame F: F's origin is the rig at rest,
and the orbit pivot starts `d0` metres straight ahead, where `d0` is the
nominal viewer-to-panel distance from `get_display_geometry` (§4b.6) — so an
untouched scene orbits about the display plane, which is where its content
sits. `N = T(pivot) ∘ Ry(yaw) ∘ Rx(pitch) ∘ T(0,0,d)`; validity is
unconditional (an untouched rig is at rest, not lost) and the runtime's
alignment absorbs the initial pose (§4b.5). The controller is
`XRT_TRACKING_TYPE_RIG_LOCAL` (§4b.4): AIM at the nominal viewer pointing
through the finger's point on the display plane, GRIP at that point, MENU
never, haptics accepted and dropped. The surface is mapped onto the whole
panel — exact for a fullscreen `_hosted` surface, an approximation for a
sub-panel window.

**What makes it move the head.** The rig composer (§4b.5) is the head's pose
source through the display plug-in's `set_pose_source` hook. Android has no
qwerty, and the composer used to be created only on the qwerty path; the
builder now binds it without a floor device whenever a `NAVIGATION` device
exists (`target_builder_sim_display.c`), seeded from the head's own static
pose so a box whose provider never takes the role is unchanged. That still
needs the active DP to implement `set_pose_source`: `sim_display` does; a
vendor Android DP that leaves the slot NULL gets the controller but not the
rig — the plug-in's job to add, not the runtime's.

### 5.1 net_input wire protocol (v1)

Normative definition: `src/xrt/drivers/net_input/net_input_proto.h`
(struct layouts are static-asserted); executable reference:
`scripts/net_input_feeder.py`. Summary:

- **Transport:** TCP on `127.0.0.1` ONLY (the provider binds loopback,
  never a routable interface), default port **9427**, one feeder at a
  time. All fields little-endian.
- **Handshake:** each side sends `{u32 magic = "DXRI" (0x49525844),
  u32 version = 1}` immediately after connect and validates the peer's;
  mismatch → close.
- **STATE** (feeder → provider, 72 bytes): `type=1`, hand (0=L/1=R),
  active, button mask (bit0 select, bit1 menu), battery %, feeder
  monotonic `timestamp_ns` (0 = stamp on receipt), position[3],
  orientation quat[4] (x,y,z,w), linear + angular velocity[3].
- **HAPTIC** (provider → feeder, 24 bytes): `type=2`, hand, amplitude,
  frequency (0 = unspecified), duration_ns — emitted whenever the
  runtime applies haptic feedback to the device.
- **Clock mapping:** non-zero feeder timestamps are translated into the
  provider's monotonic domain with a latency-floor offset estimate
  (running minimum with slow drift decay) and pushed into
  `m_relation_history`; `get_tracked_pose(at_time_ns)` then
  interpolates/predicts at the requested timestamp.
- **Headless round-trip test:** `displayxr-cli input haptic-test
  [seconds]` fires vibrations on the hand-role devices while
  `scripts/net_input_feeder.py --assert-haptic` streams poses in and
  asserts the haptic events arrive back.

## 6. Diagnostics

- `displayxr-cli input list [--json]` — enumerate registered providers
  (no DLL load): ProbeOrder, `Enabled` and where it came from, the
  `PreferredPlugin` override (`preferred`, root-level; per row `preferred`),
  the one the loader would rank first (`*` / `active`), and the ForceQwerty
  state (mirror of `dp list`; what the dashboard's component row renders,
  ADR-051). The JSON `enabled_source` field reads, precisely:
  `user` = a per-user HKCU `Enabled` is in force, whatever its value (so an
  explicit HKCU `Enabled` = 1 is `user`); `machine` = disabled by HKLM
  `Enabled` = 0; `manifest` = disabled by the POSIX manifest's
  `"enabled": false`; `default` = enabled with no per-user value — which
  includes an explicit HKLM `Enabled` = 1 or `"enabled": true`, because the
  shared row reader records the value, not its presence. The root carries
  `policy.policy_lock` (bool; the key `dp list` / `conv list` emit too): HKLM
  `InputProviders\Policy\Lock` = 1, under which no row is `user` and a
  per-user `input enable|disable` is ignored (the CLI prints a `note:`);
  always false on POSIX.
- `displayxr-cli input enable | disable <id> [--machine]` — the ADR-054
  `enable|disable` verb: `Enabled` on the provider's row, HKCU (per user, no
  admin) by default, HKLM 64-bit view with `--machine`; HKCU wins (§3).
  Refuses an id that is not registered (HKLM row / manifest) — non-zero
  exit, nothing written — since a per-user `Enabled` for it would be an
  orphan row `input list` can never show.
- `displayxr-cli input use <id> [--machine]` / `input reset [--machine]` —
  the ADR-054 `prefer` verb: the `PreferredPlugin` override (§3; the
  `input-preferred` file on POSIX, where `--machine` is refused). Every
  write says that it applies to processes started afterwards.
- `displayxr-cli input use <id> --app <exe>` / `input reset --app <exe>` — the
  per-app pin (ADR-034 Amendment 5, §3): per user only (`--machine` refused —
  `Policy\Lock` is the machine's say); `--app` takes an executable name or a
  path, reduced to its basename. `input list` prints the pins; `input list
  --json` carries them as `per_app: [{app, preferred, registered}]` (what is
  stored — `policy.policy_lock` says whether they apply) and the running
  service's per-client LIVE holders as `holders: [{client, app, generation,
  holders{controllers, hand_tracking, gamepad}, preferred{…}}]` (`null` when
  no service is reachable); the ADR-051 snapshot carries the same rows as
  `input_holders`.
- `displayxr-cli input emulated on|off [--machine]` — the N7
  `EmulatedControllers` gate (§4 item 3b): HKCU by default, the HKLM 64-bit
  view with `--machine` (POSIX: the per-user `emulated_controllers` file;
  `--machine` refused). `input list` prints `Emulated controllers: on|off
  (source)` (JSON: `emulated_controllers: {value, source}`), and both
  `selftest --json` and `info --json` carry
  `input_providers.emulated_controllers: {"value": 0|1, "source":
  "user|machine|file|default"}` plus `input_providers.devices` (the system's
  device list as `{str, type}` rows) — what the conformance cell asserts the
  qwerty pair's presence / absence from. The write pokes the running service
  like `input disable` does; the verb says which half applies live (a pair
  that exists is withdrawn / re-admitted) and which waits for the next start.
- `displayxr-cli input options <id> [--json]` / `input set <id> <KEY>
  <value> [--machine]` / `input unset <id> <KEY> [--machine]` — the ADR-054
  `options` verb (§3 *Component settings*, N3c): what the provider declares
  + what the stores hold, with the effective value and its source; writes go
  to `Settings\<KEY>` on the provider's row (HKCU; HKLM with `--machine`) or
  the POSIX per-user `<id>.settings.json`, and the provider reads them at
  its next init.
- The gamepad role has no self-test arm of its own yet: `selftest --json` /
  `info --json` show the pad through `input_providers.devices` (a
  `{str: "Gamepad", type: XRT_DEVICE_TYPE_GAMEPAD}` row) and the provider
  through `input_providers.ranked` / `active_id` like any other, and the
  service log's `input arbiter: hand roles -> … gamepad -> '…'` line says who
  holds it. A pad-only provider never fails the hand-role check (§4c).
- `displayxr-cli selftest` — when a provider is registered and not
  ForceQwerty-overridden (the `EmulatedControllers` gate does not change the
  verdict: a present provider must still yield its pair, and with the gate
  at 0 and no provider the hand roles are legitimately unassigned), asserts
  provider-claimed left+right
  motion-controller role devices exist with a valid interaction profile
  (`CLI_SELFTEST_BAD_INPUT` on failure), and (#825 Tier 2) that any role
  device advertising `supported.hand_tracking` actually claimed a static
  hand-tracking role. Provider *absence never fails* — qwerty keeping the
  hand roles is the normal no-provider configuration, and a provider
  without hand-tracking inputs passes with the hand-track fields `n/a`.
  Wired into the hardware-free CI gate (`build-windows.yml` registers
  sim-input and additionally asserts the check *evaluated*).
- Verification matrix: in-process AND `XRT_FORCE_MODE=ipc` (the IPC layer
  already carries `device_*` input messages; the service path enumerates
  provider devices identically — verified on the macOS service:
  the IPC client reports the provider's left/right role devices).
