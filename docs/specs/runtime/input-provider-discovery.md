# Input-Provider Discovery Contract

**Audience:** Integrators shipping an input-provider plug-in (a DLL/dylib/so
exposing tracked motion controllers or other input `xrt_device`s to the
runtime), and runtime engineers maintaining the discovery path.

**Status:** v1 — §§2–4 and 6 are **implemented** (Phase 1, #823:
`xrt/xrt_input_plugin.h`, `target_input_plugin_loader.c`, the builder
arbitration, `sim_input`, and the CLI/self-test diagnostics). Still
planned: the `net_input` provider + its wire protocol (§5, Phase 2) and
the `PreferredPlugin` override (§3, noted inline). The authoritative
rationale is `docs/adr/ADR-034-input-provider-plugins.md`. This spec
intentionally mirrors the display-processor contract
(`docs/specs/runtime/plugin-discovery.md`); where a rule is not restated
here, the DP rule applies unchanged.

Terminology: "motion controller" = tracked hand-held input device. Not the
workspace controller (shell) of ADR-014.

---

## 1. What an input provider is

An input provider is a dynamically-loaded module that creates one or more
`xrt_device`s carrying input state:

- **Left / right motion controllers** (first-class in v1): 6DOF pose,
  buttons/axes per a claimed interaction profile, haptic output.
- Future (same ABI, no vtable change): generic trackers, hand-tracking
  sources filling the hand-tracking device roles.

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
- The DLL handle is intentionally leaked (same lifetime rule and rationale
  as the DP loader: one process, one provider, process lifetime).

## 3. Discovery

### Windows — registry

**Root:** `HKLM\Software\DisplayXR\InputProviders` (64-bit view).
Per-provider subkey, same value schema as DisplayProcessors: `Binary`
(absolute DLL path, required), `DisplayName` / `Vendor` / `Version`
(optional strings), `ProbeOrder` (DWORD; vendors 50, in-tree fallback
providers 200, missing = 100), optional `Enabled` (DWORD, 0 disables —
implemented for input providers; the DP root has no such value).
A `PreferredPlugin` override mirroring DP spec §2.1 is *planned, not yet
implemented* — v1 selection is pure ProbeOrder.

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
`probe_order`). The `-input-provider.json` suffix is the router: the DP
loader skips such manifests, the input loader requires them, so both
plug-in types co-habit one directory. Per-user manifests shadow system
ones by `id`, as in the DP loader.

### Selection

Entries sorted by ProbeOrder ascending; **first provider whose `probe()`
succeeds wins** and is cached for the process lifetime (v1 = single active
provider; multi-provider composition is explicitly deferred — see ADR-034).
No registered provider is not an error: the builder falls back to qwerty.

## 4. Role arbitration (builder contract)

In `target_builder_sim_display.c`:

Arbitration is **presence-gated and dynamic** (ADR-034 *Amendment 1*). The
devices are static — both candidate pairs are created once and both live
in `xsysd->xdevs` — but which pair holds the hand roles is re-resolved for
the life of the process.

1. Input-provider loader runs **before** `t_builder_add_qwerty_input()`.
2. If the active provider supplied a left/right pair **and reports
   `XRT_INPUT_PROVIDER_PRESENCE_PRESENT`** (or predates the
   `get_presence` slot, which means "assume present"), those devices hold
   `xrt_system_roles.left/right` and their `*_profile` fields. Otherwise
   qwerty holds them. Qwerty always registers as a device, so the roles
   can come back to it.
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
4. The head-pose path is untouched: the DP's `set_pose_source` hook keeps
   receiving the same pose source as today; input providers never supply
   the head.
5. **Hand-tracking roles (#825 Tier 2):** the same pass fills
   `static_roles.hand_tracking.{unobstructed,conforming}.{left,right}` from
   provider devices that set `supported.hand_tracking` and carry the
   matching `XRT_INPUT_HT_*` input (unobstructed = optical, conforming =
   controller-derived; first claimant wins per role). These roles gate the
   whole `XR_EXT_hand_tracking` path — system support, tracker creation,
   joint locates. Providers without hand-tracking inputs leave them empty;
   that is a valid configuration, never a failure. These roles are
   **static** by the `xrt_system_devices` contract and so are *not*
   arbitrated: an absent optical tracker reports inactive joints, and
   qwerty has no hand tracking to fall back to anyway.
6. **`sim_input` is opt-in at run time.** Being registered is not enough
   — its `probe()` declines unless `DXR_SIM_INPUT` is set in the
   environment of the process loading the runtime. It synthesises a fixed
   motion pattern, so an un-gated registration silently displaces the
   qwerty fallback with phantom controllers. (Env var rather than the
   usual registry gate on purpose: a per-run developer switch, not
   machine configuration. Set it process-level — the runtime DLL has its
   own static-CRT environment block.)

## 5. In-tree reference providers (planned)

| Provider | Status | Purpose |
|---|---|---|
| `sim_input` | **Shipped** (`src/xrt/drivers/sim_input/`, plug-in DLL `DisplayXR-SimInput`) | Deterministic synthetic motion controllers (circular motion, scripted button presses; `khr/simple_controller`) — hardware-free CI gate, adapted from Monado's `simulated_controller.c`. Also serves scripted 26-joint hand tracking (#825 Tier 2: curl wave over the same analytic circle, via `u_hand_simulation`) — the hardware-free test vehicle for `XR_EXT_hand_tracking`. ProbeOrder 200. **Test vehicle only — not a product path:** `probe()` declines unless `DXR_SIM_INPUT` is set (§4.6). Dev builds stage it automatically (`build_macos.sh` / `build_linux.sh`); Windows registers via `register_dev_plugin.bat input sim`. |
| `net_input` | **Shipped** (`src/xrt/drivers/net_input/`, plug-in DLL `DisplayXR-NetInput`) | Loopback-TCP-fed devices — an external tracking process feeds timestamped poses + button state and receives haptic events back (wire protocol below). Opt-in: never registered by default. |
| `ultraleap` | **Shipped, SDK-gated** (`src/xrt/drivers/ultraleap/`, plug-in DLL `DisplayXR-Ultraleap`; builds only where the Ultraleap Gemini SDK / LeapC is found — `LEAPSDK_DIR`) | Hand-as-motion-controller provider (#825 Tier 1, adapted from Monado's removed `ultraleap_v5`): palm pose → grip/aim, pinch → select, grab → menu; 26-joint sets filled for the Tier-2 `XR_EXT_hand_tracking` wiring. On Windows the SDK's `LeapC.dll` is staged **next to the plug-in** (#933) — the loader's `LOAD_WITH_ALTERED_SEARCH_PATH` then resolves it app-locally instead of from the system PATH, where the LeiaSR Platform ships its own shadowing copy. Opt-in: never registered by default. |

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
  (no DLL load), the predicted active one, ProbeOrder/Enabled, and the
  ForceQwerty state (mirror of `dp list`).
- `displayxr-cli selftest` — when a provider is registered and not
  ForceQwerty-overridden, asserts provider-claimed left+right
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
