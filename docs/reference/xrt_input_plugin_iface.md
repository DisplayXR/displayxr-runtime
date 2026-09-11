# `xrt_input_plugin_iface` — Input-Provider Plug-in Interface

**Status: implemented** (Phase 1, #823) — the header is
`src/xrt/include/xrt/xrt_input_plugin.h`; the runtime-side consumer is
`src/xrt/targets/common/target_input_plugin_loader.c`. Design rationale:
`docs/adr/ADR-034-input-provider-plugins.md`. Discovery and lifecycle:
`docs/specs/runtime/input-provider-discovery.md`. The in-tree reference
provider is `src/xrt/drivers/sim_input/`.

## v1 surface

```c
#define XRT_INPUT_PLUGIN_API_VERSION_CURRENT 1
#define XRT_INPUT_PLUGIN_ENTRYPOINT_NAME "xrtInputPluginNegotiate"
#define XRT_INPUT_PLUGIN_MAX_DEVICES 8

struct xrt_input_plugin_iface {
    uint32_t struct_size;   /* ABI guard, ADR-020 */
    uint32_t reserved_0;

    /* Identity, logged at probe and matched against the discovery id. */
    const char *id;
    const char *display_name;
    const char *vendor;   /* may be NULL */
    const char *version;  /* may be NULL */

    /* Cheap hardware/transport presence check; first success in
     * ProbeOrder wins. Writes a provider-defined instance handle
     * (NULL is legal) passed back to every later call. */
    xrt_result_t (*probe)(struct xrt_input_plugin_instance **out_inst);

    /* Create ALL devices this provider supplies (≤ max_count, which is
     * never above XRT_INPUT_PLUGIN_MAX_DEVICES). Ownership transfers to
     * the runtime (destroyed via xrt_device::destroy). */
    xrt_result_t (*create_devices)(struct xrt_input_plugin_instance *inst,
                                   struct xrt_device **out_devices,
                                   uint32_t max_count,
                                   uint32_t *out_count);

    void (*destroy)(struct xrt_input_plugin_instance *inst);

    /* Append-only below this line, forever. */
};
```

Exported symbol: `xrtInputPluginNegotiate` (see the discovery spec §2 for
the signature). The loader rejects any provider whose reported ABI major
differs from `XRT_INPUT_PLUGIN_API_VERSION_CURRENT` before touching the
vtable (ADR-020 rule 3). A `xrt_input_plugin_host_iface` (struct_size +
`host_api_version` + `get_display_geometry` + reserved slots) is passed to
negotiate — see below.

## The host iface (#1380)

```c
struct xrt_input_host_display_geometry {
    uint32_t struct_size;       /* caller sets to sizeof before the call */
    float display_width_m;
    float display_height_m;
    float nominal_viewer_x_m;   /* display-centre origin, +Z toward viewer */
    float nominal_viewer_y_m;
    float nominal_viewer_z_m;   /* never the tracked eyes */
};

struct xrt_input_plugin_host_iface {
    uint32_t struct_size;
    uint32_t host_api_version;
    xrt_result_t (*get_display_geometry)(struct xrt_input_host_display_geometry *inout);
    void *reserved[13];
};
```

**Lifetime of the pointer, readiness of the data — two different things,
and the reason there is a distinct result code.**

- The `struct xrt_input_plugin_host_iface *` handed to
  `xrtInputPluginNegotiate` is **process-lifetime runtime storage** (one
  static, filled once —
  `target_input_plugin_loader.c::input_host_iface()`). A provider may
  retain it, and the `get_display_geometry` pointer inside it, for as long
  as it lives. It was a stack local before #1380; a provider that kept it
  read dead stack.
- The **geometry** is a runtime-owned cache populated from the display
  plug-in's `get_display_info`, in the system builder, *after* the DP head
  exists and *before* any provider's `create_devices` runs
  (`target_builder_sim_display.c`). So: **ready from `create_devices` on,
  static for the life of the system, NOT ready during negotiate.**

Contract of the call:

| Situation | Returns | The out struct |
|---|---|---|
| Slot is `NULL` | — | older runtime; there is no geometry to ask for |
| `inout == NULL`, or `struct_size` too small to even hold `struct_size` | `XRT_ERROR_INPUT_UNSUPPORTED` | untouched |
| Called before the cache is published (e.g. from negotiate) | `XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY` | **untouched** — retry later |
| Cache published | `XRT_SUCCESS` | filled, prefix semantics |

*Prefix semantics* is the display-info convention: set `struct_size =
sizeof(...)` **before** the call; the runtime copies
`min(struct_size, sizeof(runtime's struct))` bytes and leaves your
`struct_size` as you set it. A provider built against an older, shorter
header therefore gets the leading fields it knows and nothing is written
past its own allocation.

The numbers are **nominal** — the panel as the display processor reports
it, and the nominal viewer position, never the tracked eyes. Use them for
solver geometry (how big the working volume is, where the plane sits),
never as a head pose.

## Display-plane-relative devices: `XRT_TRACKING_TYPE_RIG_LOCAL`

A provider that also drives the rig (ADR-034 Amendment 4) publishes its
controllers and hand joints **relative to the display plane** — origin at
the display centre, +X right, +Y up, +Z toward the viewer, metres — by
setting its devices' `tracking_origin->type` to
`XRT_TRACKING_TYPE_RIG_LOCAL`.

- **What the provider publishes:** `L`, the honest volume pose. No
  standing height, no mount offset, no rig pre-compensation (Amendment 2
  forbids the last one outright). A solver whose product poses `C_W` are
  already in its own navigated world `W` publishes `L = inv(P(t)) ∘ C_W`
  alongside `N(t) = P(t)`.
- **What the runtime anchors:** at system build the builder sets that
  origin's `initial_offset` to `rig_initial` — the rig pose at build,
  the same one `u_space_overseer::rig_source` arms from
  (`u_builders.c::anchor_rig_local_origins`, which runs after the
  tracking origins are settled and before the overseer freezes each
  offset into a space). Combined with the existing rig delta this
  collapses to `world = rig(t) ∘ inv(rig_initial) ∘ rig_initial ∘ L =
  rig(t) ∘ L`. Several devices sharing one origin are anchored once.
- **What is left alone:** `XRT_TRACKING_TYPE_OTHER` / `_NONE` origins keep
  today's stage-anchored behaviour, mount offsets included. Existing
  providers need no change.

Anchoring is logged once per origin at init
(`Rig-local origin '…' anchored at the initial rig (…)`), never per frame.

## Device obligations

Devices returned by `create_devices` are ordinary `xrt_device`s. Each
self-describes via `device_type`
(`XRT_DEVICE_TYPE_{LEFT,RIGHT,ANY}_HAND_CONTROLLER`; other types ride
along without claiming a hand role) and the interaction profile it binds
(`name` + optional `binding_profiles`, from `bindings.json`). The
provider must implement:

- `update_inputs` — refresh the `xrt_input` array.
- `get_tracked_pose` — timestamp-correct prediction. Recommended pattern
  for asynchronously-fed hardware: a provider-owned feed thread pushes
  timestamped samples into `m_relation_history`
  (`auxiliary/math/m_relation_history.h`); the callback
  interpolates/predicts for the requested time. See also `m_predict`,
  `m_filter_one_euro`. (`sim_input` needs none of this — its pose is an
  analytic function of the timestamp.)
- `set_output` — haptics sink (may be a no-op for haptic-less hardware).
- `destroy`.

Profile binding reuses the existing mechanism — see `qwerty_device.c`
for a device that binds multiple interaction profiles, and
`sim_input_device.c` for the minimal single-profile
(`khr/simple_controller`) case.

## `get_presence` — the liveness slot (required in practice)

```c
enum xrt_input_provider_presence (*get_presence)(struct xrt_input_plugin_instance *inst);
```

Returns `XRT_INPUT_PROVIDER_PRESENCE_{UNKNOWN,ABSENT,PRESENT}`: is your
hardware/transport there **right now**? `probe()` answers a different,
one-shot question ("should I be loaded"); this one is re-asked for the
life of the process and decides whether you or the qwerty fallback owns
the hand roles (ADR-034 *Amendment 1*).

Rules:

- **Non-blocking and allocation-free.** It is called from the role
  arbiter on the `xrSyncActions` path — per client, per frame, over IPC.
  Return a value your own transport thread maintains; never do discovery
  or I/O here. (The arbiter additionally rate-limits to ~250 ms, but do
  not rely on that.)
- **Presence is not visibility.** Hardware attached but seeing nothing is
  `PRESENT` with inactive inputs. Reporting `ABSENT` because the user's
  hands left the tracking volume makes the controllers flip to qwerty and
  back continuously.
- **`UNKNOWN` means "not yet"** — the arbiter treats it as not present.
  If you can never determine presence, leave the slot NULL instead;
  that means "assume present" and reproduces pre-amendment behaviour.
- Appended under `struct_size` cover, so it is optional for ABI purposes
  and the API major stays 1. The runtime guards every call with
  `XRT_INPUT_PLUGIN_IFACE_HAS(iface, get_presence)`.
- May be called before, after and between `create_devices()` calls, and
  with a NULL `inst` if that is what `probe()` produced.

In-tree examples: `ultraleap_provider.cpp` maintains it from LeapC
`Device` / `DeviceLost` / `ConnectionLost` events; `net_input_hub.c` maps
it to "a feeder is connected", since for a wire provider the peer *is*
the hardware.

## Role arbitration (what the runtime does with the devices)

`t_builder_add_input_provider_devices()` runs before the qwerty fallback,
and **both** pairs end up in `xsysd->xdevs`. Which pair holds
`xrt_system_roles.left/right` is then re-resolved continuously from
`get_presence` by `target_input_arbiter.c`: the provider owns the hands
while its hardware is present, qwerty owns them otherwise, and the roles
move between them mid-session on plug/unplug (the `generation_id` bump
makes the OpenXR state tracker rebind at the next `xrSyncActions`).

`HKLM\Software\DisplayXR\Input\ForceQwerty` (POSIX: a `force_qwerty` file
next to the manifests) skips providers entirely. Details: discovery spec
§4.

## What providers must NOT do

- Supply a head device (the display processor / builder owns the head).
- Claim workspace-controller registration (unrelated subsystem).
- Depend on runtime-internal symbols beyond the public `xrt_*` headers and
  the aux helpers exported to plug-ins — same boundary discipline as
  display-processor plug-ins (ADR-019). On ELF, export exactly one
  symbol (`xrtInputPluginNegotiate`) via
  `src/xrt/drivers/input_plugin_exports.version` or equivalent (#496).

## Diagnostics

- `displayxr-cli input list [--json]` — enumerate registered providers
  (no DLL load) + the predicted active one + the ForceQwerty state.
- `displayxr-cli selftest` — with a provider registered, not overridden,
  **and reporting `PRESENT`**, asserts left+right role devices with a
  valid interaction profile. Provider absence never fails, and neither
  does an unplugged provider or one that declined: qwerty holding the
  roles is the correct outcome. Only a provider that failed to *load*
  (missing entry point, ABI-major mismatch) fails the check.
- `displayxr-cli input haptic-test [s]` — re-resolves the roles every
  iteration and prints each `generation_id` change, so it doubles as a
  live view of the arbitration flipping.
- `scripts/register_dev_plugin.bat input <dll>` — register a vendor DLL
  on Windows. `input sim` registers the dev sim-input, which additionally
  needs `DXR_SIM_INPUT=1` in the loading process's environment before it
  will probe.
