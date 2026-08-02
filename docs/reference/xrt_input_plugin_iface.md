# `xrt_input_plugin_iface` — Input-Provider Plug-in Interface

**Status: skeleton** — the header `xrt/xrt_input_plugin.h` does not exist
yet. This page reserves the reference slot and records the v1 surface as
designed; it becomes the full callback catalog when the header lands.
Design rationale: `docs/adr/ADR-034-input-provider-plugins.md`. Discovery
and lifecycle: `docs/specs/runtime/input-provider-discovery.md`.

## v1 surface (designed)

```c
#define XRT_INPUT_PLUGIN_API_VERSION_CURRENT 1

struct xrt_input_plugin_iface {
    uint32_t struct_size;   /* ABI guard, ADR-020 */

    xrt_result_t (*probe)(struct xrt_input_plugin_instance *inst);

    xrt_result_t (*create_devices)(struct xrt_input_plugin_instance *inst,
                                   struct xrt_device **out_devices,
                                   uint32_t max_count,
                                   uint32_t *out_count);

    void (*destroy)(struct xrt_input_plugin_instance *inst);

    /* Append-only below this line, forever. */
};
```

Exported symbol: `xrtInputPluginNegotiate` (see the discovery spec §2 for
the signature and version-gating semantics).

## Device obligations

Devices returned by `create_devices` are ordinary `xrt_device`s. The
provider must implement:

- `update_inputs` — refresh the `xrt_input` array.
- `get_tracked_pose` — timestamp-correct prediction. Recommended pattern:
  a provider-owned feed thread pushes timestamped samples into
  `m_relation_history` (`auxiliary/math/m_relation_history.h`); the
  callback interpolates/predicts for the requested time. See also
  `m_predict`, `m_filter_one_euro`.
- `set_output` — haptics sink (may be a no-op for haptic-less hardware).
- `destroy`.

Profile binding reuses the existing mechanism — see `qwerty_device.c` for a
device that binds multiple interaction profiles from `bindings.json`.

## What providers must NOT do

- Supply a head device (the display processor / builder owns the head).
- Claim workspace-controller registration (unrelated subsystem).
- Depend on runtime-internal symbols beyond the public `xrt_*` headers and
  the aux helpers exported to plug-ins — same boundary discipline as
  display-processor plug-ins (ADR-019).
