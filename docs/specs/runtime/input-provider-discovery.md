# Input-Provider Discovery Contract

**Audience:** Integrators shipping an input-provider plug-in (a DLL/dylib/so
exposing tracked motion controllers or other input `xrt_device`s to the
runtime), and runtime engineers maintaining the discovery path.

**Status:** v0 **draft** — contract designed, not yet implemented. The
authoritative rationale is `docs/adr/ADR-034-input-provider-plugins.md`.
This spec intentionally mirrors the display-processor contract
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

Header: `xrt/xrt_input_plugin.h` (to be added). Single exported symbol:

```c
xrt_result_t
xrtInputPluginNegotiate(uint32_t host_api_version,
                        const struct xrt_input_plugin_host *host,
                        struct xrt_input_plugin_iface **out_iface,
                        uint32_t *out_plugin_api_version);
```

- ABI constant: `XRT_INPUT_PLUGIN_API_VERSION_CURRENT` = **1**. Mismatch →
  loader skips the entry (`XRT_ERROR_PROBER_NOT_SUPPORTED`), same as the DP
  loader. ADR-020 rules apply: every struct carries `struct_size`; the
  vtable is append-only forever.
- `xrt_input_plugin_iface` v1 vtable:
  - `probe(inst)` — cheap "is my hardware/transport present" check.
    `XRT_ERROR_PROBER_NOT_SUPPORTED` = clean decline.
  - `create_devices(inst, struct xrt_device **out_devices, uint32_t max,
    uint32_t *out_count)` — create all devices this provider supplies.
    Each device self-describes: `device_type`
    (`XRT_DEVICE_TYPE_{LEFT,RIGHT,ANY}_HAND_CONTROLLER`, …) plus the
    interaction profile it binds (an existing profile from
    `bindings.json` — e.g. `…/khr/simple_controller` — reusing the same
    mechanism as `qwerty_device.c`).
  - `destroy(inst)`.
- The DLL handle is intentionally leaked (same lifetime rule and rationale
  as the DP loader: one process, one provider, process lifetime).

## 3. Discovery

### Windows — registry

**Root:** `HKLM\Software\DisplayXR\InputProviders` (64-bit view).
Per-provider subkey, same value schema as DisplayProcessors: `Path`
(absolute DLL path), `ProbeOrder` (DWORD; vendors 50, in-tree fallback
providers 200, missing = 100), optional `Enabled` (DWORD, 0 disables).
`PreferredPlugin` override behaves as in the DP spec §2.1.

`scripts/register_dev_plugin.bat` grows an `input` mode to register the
freshly-built in-tree providers from an elevated prompt.

### POSIX — JSON manifests

Manifest name pattern: `NNN-<name>-input-provider.json` in the same
directories the DP loader scans (`XRT_PLUGIN_SEARCH_PATH` for dev trees,
the installed plugin dir otherwise). Schema mirrors the DP manifest
(`library_path`, `probe_order`, `plugin_api_version`).

### Selection

Entries sorted by ProbeOrder ascending; **first provider whose `probe()`
succeeds wins** and is cached for the process lifetime (v1 = single active
provider; multi-provider composition is explicitly deferred — see ADR-034).
No registered provider is not an error: the builder falls back to qwerty.

## 4. Role arbitration (builder contract)

In `target_builder_sim_display.c`:

1. Input-provider loader runs **before** `t_builder_add_qwerty_input()`.
2. If the active provider supplied a left/right pair, those devices claim
   `xrt_system_roles.left/right` (and their `*_profile` fields). Qwerty
   still registers as a device (debug value) but does not claim hand roles.
3. Override: `HKLM\Software\DisplayXR\Input\ForceQwerty = 1` (DWORD) /
   `force_qwerty` in the POSIX config — forces the fallback, registry-gated
   by convention. With no provider registered, behavior is exactly today's.
4. The head-pose path is untouched: the DP's `set_pose_source` hook keeps
   receiving the same pose source as today; input providers never supply
   the head.

## 5. In-tree reference providers (planned)

| Provider | Purpose |
|---|---|
| `sim_input` | Deterministic synthetic motion controllers (circular motion, scripted button presses) — hardware-free CI gate, adapted from Monado's `simulated_controller.c`. ProbeOrder 200. |
| `net_input` | Loopback-TCP-fed devices — a versioned, documented wire protocol (derived from Monado's `remote` driver) so an external tracking process can feed timestamped poses + button state and receive haptic events. The wire protocol will be specified in this document when implemented. |

## 6. Diagnostics

- `displayxr-cli input list` — enumerate registered providers, active
  provider, devices + claimed profiles (mirror of `dp list`).
- `displayxr-cli selftest` — when a provider is registered, asserts
  left/right motion-controller devices exist with a valid interaction
  profile (extends the existing hardware-free CI gate).
- Verification matrix: in-process AND `XRT_FORCE_MODE=ipc` (the IPC layer
  already carries `device_*` input messages; the service path must
  enumerate provider devices identically).
