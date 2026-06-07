# `xrt_plugin_iface` Reference

Per-method reference for the vendor plug-in vtable returned from `xrtPluginNegotiate`. For the higher-level onboarding flow (how to package, install, register), see [`vendor-plugin-onboarding.md`](../guides/vendor-plugin-onboarding.md). The C ABI is declared in [`src/xrt/include/xrt/xrt_plugin.h`](../../src/xrt/include/xrt/xrt_plugin.h).

## Lifecycle

```
runtime startup
  └── walks HKLM\Software\DisplayXR\DisplayProcessors\*
       (Windows; POSIX uses JSON manifests — see plugin-discovery.md)
       sorted by ProbeOrder, ascending
       │
       └── for each registered plug-in:
            LoadLibraryExW(plugin.dll)
            GetProcAddress("xrtPluginNegotiate")
            ├── xrtPluginNegotiate(rt_ver, host, &iface, &plugin_ver)
            │     returns XRT_SUCCESS + iface
            │     OR XRT_ERROR_PROBER_NOT_SUPPORTED + skip
            │
            ├── iface->probe(&instance)
            │     returns XRT_SUCCESS + instance handle → claim the system
            │     OR XRT_ERROR_PROBER_NOT_SUPPORTED + skip (try next plug-in)
            │
            ├── iface->create_device(instance, &xdev)
            │
            ├── iface->get_display_info(instance, xdev, &info)
            │
            ├── iface->set_pose_source(instance, xdev, qwerty_xdev)
            │     [optional binding for WASD/mouse pose source]
            │
            ├── iface->create_dp_<api>(...)   per app session, per graphics API
            │
            └── iface->destroy(instance)       runtime shutdown
```

Once a plug-in's `probe()` succeeds, the runtime stops scanning — subsequent plug-ins are not loaded. The first plug-in to claim the system wins.

## Entry point

### `xrtPluginNegotiate`

```c
XRT_PLUGIN_EXPORT xrt_result_t
xrtPluginNegotiate(uint32_t runtime_api_version,
                   const struct xrt_plugin_host_iface *host,
                   struct xrt_plugin_iface **out_iface,
                   uint32_t *out_plugin_api_version);
```

The single C-ABI symbol your plug-in DLL must export. Called once per process at first `xrCreateInstance`.

| Param | Contract |
|---|---|
| `runtime_api_version` | The `XRT_PLUGIN_API_VERSION_*` the runtime speaks. Compare against `XRT_PLUGIN_API_VERSION_CURRENT` from your headers; return `XRT_ERROR_PROBER_NOT_SUPPORTED` if you can't handle the runtime's version. |
| `host` | Read-only callback table from the runtime. v1 has only the struct-size header + reserved slots; future host-supplied callbacks land in the reserved space. Lifetime: valid for the duration of this call AND for the plug-in's lifetime. **Don't dereference past `host->struct_size`.** |
| `out_iface` | You return your vtable here. Storage is plug-in-owned; the runtime treats it as a read-only borrow until `destroy()`. Must remain valid until `destroy()` returns. Set `(*out_iface)->struct_size = sizeof(struct xrt_plugin_iface)` at YOUR compile time so the runtime can forward-version-detect. |
| `out_plugin_api_version` | The `XRT_PLUGIN_API_VERSION_*` you implement. Runtime compares; mismatch → plug-in skipped with a logged warning. |

**Returns:**
- `XRT_SUCCESS` → runtime proceeds to call `(*out_iface)->probe()`
- `XRT_ERROR_PROBER_NOT_SUPPORTED` → clean decline (e.g. version mismatch), runtime logs an info line and skips
- Other `XRT_ERROR_*` → hard failure, runtime logs a warning and skips

## Required vtable methods

### `probe`

```c
xrt_result_t (*probe)(struct xrt_plugin_instance **out_inst);
```

"Do you want to claim the current system?"

Called on the `xrCreateInstance` hot path for every registered plug-in until one succeeds — **sub-millisecond budget**. May consult the vendor SDK to check for connected hardware (e.g. EDID lookup, device enumeration), but if your check is expensive, cache the result statically on first call.

**Returns:**
- `XRT_SUCCESS` + populate `*out_inst` → you claim the system. The runtime owns the lifetime of the returned handle and frees it via `destroy()`.
- `XRT_ERROR_PROBER_NOT_SUPPORTED` → clean decline ("no device of this type on this system"). Runtime logs an info line and tries the next plug-in.
- Other `XRT_ERROR_*` → hard probe failure. Logged at warning level. Runtime skips this plug-in.

The plug-in defines the concrete layout of `xrt_plugin_instance`; the runtime treats it as an opaque `void *` keyed off this call's out-param and passes it back to every subsequent vtable call.

### `create_device`

```c
xrt_result_t (*create_device)(struct xrt_plugin_instance *inst,
                              struct xrt_device **out_dev);
```

Construct the plug-in's `xrt_device` — the head / HMD-equivalent device for the runtime's prober + system-builder. Called only after a successful `probe()`.

Ownership of `*out_dev` is transferred to the runtime, which destroys the device via the usual `xrt_device::destroy` vtable method (not through this iface's `destroy`).

### Per-API DP factories

```c
xrt_dp_factory_vk_fn_t    create_dp_vk;
xrt_dp_factory_d3d11_fn_t create_dp_d3d11;
xrt_dp_factory_d3d12_fn_t create_dp_d3d12;
xrt_dp_factory_gl_fn_t    create_dp_gl;
xrt_dp_factory_metal_fn_t create_dp_metal;
```

Construct the per-graphics-API display processor. Called per app session, per graphics API.

`NULL` means your plug-in doesn't support that graphics API on this platform — the runtime gracefully falls back to the sim-display DP for that API. **At least one** of the five must be non-NULL — a plug-in whose probe succeeds but offers no DP factory is rejected (it'd have nothing the compositor can drive).

Each factory's signature is owned by `xrt_display_processor_<api>.h` and is unchanged from the pre-plug-in shape — see those headers for the exact contracts. The plug-in iface just hands one back per supported API.

**Optional DP-vtable extensions a vendor can implement** (appended slots, gated by the DP `struct_size` per ADR-020 — an older plug-in simply doesn't have them):
- `get_handoff_color_capability` / `set_atlas_encoding` — ADR-021 color contract.
- `get_local_zone_caps` / `publish_local_zone_mask` / `clear_local_zone_mask` (D3D11, slots 12–14) — the local 2D/3D-zone hardware leg (#224, `docs/roadmap/local-3d-zones.md`): the runtime publishes the authored `XR_EXT_local_3d_zone` mask (R8 SRV + physical-pixel screen anchor, per frame while active) so switchable-lens panels can track per-window 3D. Report `zone_grid = 1×1` to OR-collapse to a global on/off panel — bit-compatible with today's `request_display_mode` arbitration. Caps struct: `xrt_display_zones.h`.

### `destroy`

```c
void (*destroy)(struct xrt_plugin_instance *inst);
```

Free `inst` and all plug-in-owned resources hanging off it. Called by the runtime at instance teardown, or after a negotiated plug-in is superseded by a later registration.

After `destroy()` returns, the runtime stops dereferencing both `inst` and the vtable; you can safely tear down DLL-static state here too.

## Optional vtable methods

### `get_display_info`

```c
bool (*get_display_info)(struct xrt_plugin_instance *inst,
                         struct xrt_device *xdev,
                         struct xrt_plugin_display_info *out_info);
```

Fill in vendor-neutral physical-display info. Lets the runtime populate `xrt_system_compositor_info` without calling any vendor-specific symbol directly — the headline ADR-019 goal.

**Forward-compat:** the runtime sets `out_info->struct_size` to its own `sizeof(struct xrt_plugin_display_info)` before the call; the plug-in **must not** write past that offset. Field additions append at the end with no API version bump.

Fields to populate:

| Field | Units / type | Notes |
|---|---|---|
| `display_width_m`, `display_height_m` | meters (float) | Physical panel dimensions |
| `nominal_viewer_x_m`, `nominal_viewer_y_m`, `nominal_viewer_z_m` | meters (float) | Default viewer position relative to display center. Drives Kooima projection defaults when the app has no head tracking. |
| `display_pixel_width`, `display_pixel_height` | pixels (uint32) | Native panel resolution |
| `recommended_view_scale_x`, `recommended_view_scale_y` | float, 1.0 = native | Vendor-recommended per-view scaling. <1.0 means downscale. |
| `display_screen_left`, `display_screen_top` | virtual-screen coords (int32) | Display top-left in Windows-style virtual-screen pixels. Used to position workspace windows. Both 0 = "no preference / display origin == desktop origin" (sim-display picks this). |
| `supported_eye_tracking_modes` | bitmask | bit 0 = MANAGED, bit 1 = MANUAL, `0` = no eye tracking. A typical hardware DP is MANAGED-only; the reference simulator (sim_display) declares `0` — its positions are nominal, not tracked (`SIM_DISPLAY_FAKE_TRACKING=1` dev toggle re-enables MANUAL for testing). Must be non-zero iff at least one `xrt_rendering_mode` sets `XRT_RENDERING_MODE_FLAG_HAS_TRACKING` in `mode_flags` (ABI v3, #441). |
| `default_eye_tracking_mode` | enum | 0 = MANAGED, 1 = MANUAL. |

**Returns:**
- `true` → struct populated, runtime uses your values
- `false` → plug-in couldn't produce info (e.g. vendor SDK declined). Runtime keeps the defaults already in `xsysc->info`.

**NULL is allowed.** A NULL pointer is treated as if the call returned `false`. Required to be non-NULL for plug-ins that ship a `create_device` implementation, otherwise the runtime has no source of display dimensions and falls back to OpenXR defaults.

### `set_pose_source`

```c
void (*set_pose_source)(struct xrt_plugin_instance *inst,
                        struct xrt_device *xdev,
                        struct xrt_device *source);
```

Bind an external pose source to the device returned by `create_device`. Used to wire the qwerty HMD (WASD/mouse camera controls) into your vendor device.

Each vendor's driver owns a private cast from `xrt_device *` back to its container struct; this iface method lets the runtime invoke that vendor-private binding without the runtime DLL knowing the vendor's struct layout.

Passing `source = NULL` clears the binding (the device falls back to its static pose).

**NULL is allowed.** NULL means your plug-in doesn't support external pose binding — the caller skips silently. If you support it, the canonical pattern (lifted from `drv_leia` / `drv_sim_display`):

```c
static void
my_plugin_set_pose_source(struct xrt_plugin_instance *inst,
                          struct xrt_device *xdev,
                          struct xrt_device *source)
{
    if (xdev == NULL) return;
    struct my_device *m = container_of(xdev, struct my_device, base);
    m->external_pose_source = source;
}
```

## Forward-compatibility rules

The iface is designed to evolve without breaking older plug-ins:

1. **New fields are only appended at the end.** Reordering or redefining an existing field bumps `XRT_PLUGIN_API_VERSION_CURRENT`.
2. **`struct_size` is the read-clamp.** The runtime sets `host->struct_size` to its own `sizeof(struct xrt_plugin_host_iface)`; plug-ins must not dereference past that. Plug-ins set `iface->struct_size` to their own compile-time size; the runtime must not dereference past that. Either side can detect a "newer than I know about" peer and clamp cleanly.
3. **Pure-additive struct changes do NOT bump the API version.** Only non-additive layout changes do.
4. **Optional methods are NULL-safe.** Adding a new optional method is a pure-additive change; old plug-ins return NULL for the new slot, the runtime handles NULL.

## Aux surface — separate from the iface

Logging, debug-variable tracking, frame metrics, Perfetto tracing, and unique-ID generation are **not** plumbed through this iface. Plug-ins reach them by linking the runtime DLL's import library (`DisplayXRClient.lib`) and getting `__declspec(dllimport)`'d symbols. See [ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md) for the rationale.

In practice your plug-in's CMakeLists looks like:

```cmake
target_compile_definitions(DisplayXR-MyVendor PRIVATE XRT_USING_RUNTIME_DLL)
target_link_libraries(DisplayXR-MyVendor PRIVATE
    $<TARGET_LINKER_FILE:${RUNTIME_TARGET}>    # DisplayXRClient.lib
    xrt-interfaces aux_util aux_os aux_math    # for non-exported helpers
)
```

And in any plug-in TU:

```c
#include "aux/util/u_logging.h"

void my_plugin_init(void) {
    U_LOG_W("plug-in version 1.0.0 init");  // resolves to DisplayXRClient.dll
}
```

## Related

- [Vendor plug-in onboarding](../guides/vendor-plugin-onboarding.md) — high-level guide for building + shipping a plug-in
- [Plug-in discovery spec](../specs/runtime/plugin-discovery.md) — registry / manifest formats, env-var overrides
- [ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md) — vendor / aux boundary rationale
- [`xrt_plugin.h`](../../src/xrt/include/xrt/xrt_plugin.h) — the C ABI header itself
