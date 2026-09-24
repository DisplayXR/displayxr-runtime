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
| `host` | Read-only callback table from the runtime — struct-size header, then host-supplied callbacks carved out of the reserved space (see [The host iface](#the-host-iface) below). Lifetime: valid for the duration of this call AND for the plug-in's lifetime. **Don't dereference past `host->struct_size`,** and NULL-check every callback. |
| `out_iface` | You return your vtable here. Storage is plug-in-owned; the runtime treats it as a read-only borrow until `destroy()`. Must remain valid until `destroy()` returns. Set `(*out_iface)->struct_size = sizeof(struct xrt_plugin_iface)` at YOUR compile time so the runtime can forward-version-detect. |
| `out_plugin_api_version` | The `XRT_PLUGIN_API_VERSION_*` you implement. Runtime compares; mismatch → plug-in skipped with a logged warning. |

**Returns:**
- `XRT_SUCCESS` → runtime proceeds to call `(*out_iface)->probe()`
- `XRT_ERROR_PROBER_NOT_SUPPORTED` → clean decline (e.g. version mismatch), runtime logs an info line and skips
- Other `XRT_ERROR_*` → hard failure, runtime logs a warning and skips

## The host iface

`struct xrt_plugin_host_iface` is the runtime→plug-in direction: things only
the host can supply. Every slot after the header was **carved out of the
original `reserved[]` block**, so `struct_size` never grew and no
`XRT_PLUGIN_API_VERSION_CURRENT` bump was needed — which is exactly why
**every callback must be NULL-checked** before use. A plug-in loaded by an
older runtime sees a zeroed slot.

| Slot | Platform | What it is |
|---|---|---|
| `struct_size`, `host_api_version` | all | Header. Read-clamp + a structural cross-check of the `runtime_api_version` argument. |
| `get_android_vm` | Android | The host's `JavaVM *`. A plug-in that statically links `aux_android` gets its own private, never-populated copy of the VM globals, so it MUST come through here rather than calling `android_globals_get_vm()`. |
| `get_android_activity` | Android | The host's Activity `jobject` when there is one, else the Service `Context` (the out-of-process runtime service has no Activity). Use for anything Activity-typed — orientation limiting, permission dialogs — and as the generic `android.content.Context` for `bindService`. |
| `get_android_class_host_context` | Android | A `Context` whose `getClassLoader()` can resolve classes shipped in the **runtime's** APK. **Class loading only.** See below. |

### `get_android_class_host_context` (ADR-036 D2, #1037)

```c
void *(*get_android_class_host_context)(void);   /* jobject, or NULL */
```

Under [ADR-036 D2](../adr/ADR-036-android-per-window-compositor-instances.md)
the compositor and the vendor plug-in run **in the app's own process**, and
[ADR-025](../adr/ADR-025-android-vendor-dp-out-of-process.md)'s requirement still
stands: the app carries no vendor `.so`, no vendor `<queries>` and **no vendor
Java glue**. So the glue ships in the runtime APK, and a vendor SDK that builds
its own `DexClassLoader` has to be pointed at the runtime's classloader —
which it takes from the `Context` it is handed.

The runtime therefore returns a Context created with

```java
context.createPackageContext(<runtime pkg>, CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY)
```

the same mechanism the runtime already uses to host
`org.freedesktop.monado.ipc.Client` (`loadClassFromRuntimeApk`,
`src/xrt/ipc/android/ipc_client_android.cpp`). The runtime package is derived
from the directory the runtime `.so` was loaded from — no runtime Java class is
involved, which matters because in-process there is no runtime Java at all,
only the `dlopen`'d `.so`.

**Contract:**

- **Use it ONLY for class loading.** It is a class-*hosting* Context: it still
  runs under the app's uid, so `getPackageManager()` visibility and
  `bindService()` identity remain the app's. It is not an Activity.
- **Activity-typed calls keep using `get_android_activity`.** Orientation
  limiting, permission dialogs and anything else needing a real Activity must
  not be routed here.
- **Out-of-process** (the runtime service) the runtime package *is* the calling
  process, so the host returns its own Context — the slot costs the plug-in
  nothing and needs no special-casing on the plug-in side.
- **NULL is normal** and means "older runtime, or the host could not make one".
  Fall back to `get_android_activity` (today's behaviour) rather than failing.
- The returned reference is a **host-owned JNI global ref**, cached for the
  process lifetime. Don't delete it.

Plug-ins that must also compile against an older runtime header can guard on
the feature macro, the same coupled-ABI-addition pattern the DP slots use:

```c
#ifdef XRT_PLUGIN_HOST_HAS_CLASS_HOST_CONTEXT
	vendor_set_class_context_accessor(host->get_android_class_host_context);
#endif
```

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

**DP semantic contract — hardware vs processing (ADR-028, #542):**
- `request_display_mode(enable_3d)` is **hardware-only**: drive the physical
  switchable element (lens, backlight, …) and nothing else. It must NOT
  select your weave-vs-flat path.
- Select **weave vs flat-blit from the per-frame atlas grid** handed to
  `process_atlas` (`tile_columns × tile_rows > 1` ⇒ weave, `1×1` ⇒ flat).
  The same grid should drive any mono-content special-casing (e.g.
  eye-position centering). The runtime guarantees the grid is the active
  mode's recipe (submissions are clamped to it), so a hardware override
  leaves your weave running — the panel shows the woven atlas flat, which is
  the app-authored transition state (MANUAL tracking-loss: element off
  instantly, parallax fades to zero, the image converges back to sharp).
  Per-vendor mechanisms are documented in each vendor's plug-in repo.

**Optional DP-vtable extensions a vendor can implement** (appended slots, gated by the DP `struct_size` per ADR-020 — an older plug-in simply doesn't have them):
- `get_handoff_color_capability` / `set_atlas_encoding` — ADR-021 color contract.
- `get_local_zone_caps` / `publish_local_zone_mask` / `clear_local_zone_mask` (D3D11, slots 12–14) — the local 2D/3D-zone hardware leg (#224, `docs/roadmap/local-3d-zones.md`): the runtime publishes the authored `XR_DXR_local_3d_zone` mask (R8 SRV + physical-pixel screen anchor, per frame while active) so switchable-lens panels can track per-window 3D. Report `zone_grid = 1×1` to OR-collapse to a global on/off panel — bit-compatible with today's `request_display_mode` arbitration. Caps struct: `xrt_display_zones.h`.

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
| `recommended_view_scale_x`, `recommended_view_scale_y` | float, 1.0 = native | Vendor-recommended per-view scaling. <1.0 means downscale. **Baseline hint only** — see the rule below. |
| `display_screen_left`, `display_screen_top` | virtual-screen coords (int32) | Display top-left in Windows-style virtual-screen pixels. Used to position workspace windows. Both 0 = "no preference / display origin == desktop origin" (sim-display picks this). |
| `supported_eye_tracking_modes` | bitmask | bit 0 = MANAGED, bit 1 = MANUAL, `0` = no eye tracking. A typical hardware DP is MANAGED-only; the reference simulator (sim_display) declares `0` — its positions are nominal, not tracked (`SIM_DISPLAY_FAKE_TRACKING=1` dev toggle re-enables MANUAL for testing). Must be non-zero iff at least one `xrt_rendering_mode` sets `XRT_RENDERING_MODE_FLAG_HAS_TRACKING` in `mode_flags` (ABI v3, #441). |
| `default_eye_tracking_mode` | enum | 0 = MANAGED, 1 = MANUAL. |

**The view-scale rule — one derivation, never two.**
`xrt_rendering_mode::view_scale_x/y` (your device's mode table) is the **single
source of truth** for view, tile and atlas sizing; `recommended_view_scale_*` is
a **baseline hint only**, so leave it at `0` (the runtime derives it from the
mode table) or derive it from the *same* numbers as the active 3D mode's scale —
never compute it independently.

The two feed different consumers, which is why a disagreement is a bug and not a
rounding difference:

| Consumer | Reads |
|---|---|
| `XrViewConfigurationView.recommended*` | the **scalar** (`oxr_system_fill_in`, frozen at `xrCreateInstance`) |
| `XrDisplayRenderingModeInfoDXR.viewScale*` / `viewWidthPixels` | the **mode table** |
| per-mode view dims + worst-case atlas (`u_tiling_compute_mode`, `u_tiling_compute_system_atlas_oriented`) | the **mode table** |
| the compositor's tile grid (`u_tiling_compute_canvas_view`) | the **mode table** |

A scalar *larger* than the mode's scale is the dangerous direction: the app is
sized per view from the scalar while the declared atlas was sized from the mode
table, so the atlas cannot hold the tiles the app was told to render. The runtime
also overwrites the scalar from the mode table on every rendering-mode change, so
a disagreeing value does not survive the first mode switch anyway.

**Returns:**
- `true` → struct populated, runtime uses your values
- `false` → plug-in couldn't produce info (e.g. vendor SDK declined). Runtime keeps the defaults already in `xsysc->info`.

**NULL is allowed.** A NULL pointer is treated as if the call returned `false`. Required to be non-NULL for plug-ins that ship a `create_device` implementation, otherwise the runtime has no source of display dimensions and falls back to OpenXR defaults.

**Called again after startup — keep it cheap.** A long-lived service can start before the vendor backend has identified the panel (an unattended reboot with the panel asleep; even a healthy boot needs a few seconds after the vendor platform service is up). The runtime therefore calls `get_display_info` not only at instance create but again on every client connect / per-client compositor create (`xrt_system_compositor_info::refresh_display_processors`), and re-applies the answer whenever the reported geometry (physical size, pixel size, nominal viewer, screen origin) differs from what it last applied — logging one `display info refreshed from plug-in after startup` WARN. The contract for the repeat calls: return `false` **fast** while the panel is still unknown (no blocking retry — spend any startup budget once, in `create_device`), and `true` with the real numbers once it is; keep your own head `xrt_device` (views, FOV, `hmd->views[].display`) correct in place, because the runtime re-derives only its own tiling / atlas / `xsysc->info` from the struct. Return values must be stable between calls for the same panel so the refresh stays a no-op.

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

## Where the window is: `set_window_screen_rect` (ADR-036 D6)

A compositor instance weaves into **one window**, so the interlacing phase has
to be referenced to that window's origin on the panel. The Vulkan DP variant
therefore carries an optional appended slot:

```c
void (*set_window_screen_rect)(struct xrt_display_processor_vk *xdp,
                               int32_t x, int32_t y,
                               uint32_t w, uint32_t h,
                               int32_t display_id);
```

- **Units.** The platform's own screen pixels for `display_id` — on Android the
  *current*-orientation coordinates `View.getLocationOnScreen()` returns. A
  vendor SDK that rotates into the panel's natural orientation internally (CNSDK
  does) takes them unchanged. `x`/`y` may be negative.
- **Composition.** Any per-atlas canvas (zone) offset is *added to* this origin
  by the DP: `phase = window origin + canvas offset`. It is therefore the base
  the `set_viewport` / zone rect stacks onto, not a replacement for it.
- **Cadence.** Sticky; the compositor re-asserts it every frame before
  `process_atlas`. Cache the last rect and skip the vendor call when unchanged.
- **Absent slot** (older plug-in `struct_size`) or NULL ⟹ display-scoped weaving,
  exactly today's behaviour. Never calling it is also legal.
- **ADR-033 is unchanged.** This *reports geometry*; the weaver still owns all
  phase, including snapping.
- Relationship to `set_present_origin` (Linux windowed weaving, #757): this is
  its platform-neutral successor — the same origin, plus size and display id. A
  DP implementing both takes whichever arrived most recently as authoritative.

Why it lives on `xrt_display_processor_vk` and not on the base
`xrt_display_processor`: the variant embeds the base **by value**, so growing the
base moves every variant slot and would misdispatch calls into an already-built
VK-variant plug-in — the exact silent break ADR-020 exists to prevent. The base
*is* the Vulkan interface, so appending to the variant costs no reach, and D3D11
carries its own placement slot (`xrt_display_processor_d3d11::set_window`,
#1008). **Appending to the base vtable is no longer ABI-neutral now that a
variant embeds it.**

## What is behind the window: `get_background_preview` (ADR-040)

The runtime decides how far behind the display plane a transparent app may
render ([ADR-040](../adr/ADR-040-rear-depth-budget.md)), and to decide it needs
to see the desktop under the app's canvas. On Windows the display processor
already captures those pixels for compose-under transparency, so this optional
appended slot lets it hand back a cheap preview:

```c
struct xrt_dp_background_preview {
	uint32_t struct_size;      /* sizeof(*this), set by the RUNTIME before the call */
	uint32_t generation;       /* monotonic; changes only when the capture advanced */
	uint32_t width, height;    /* preview dims, both <= 512; 0,0 = no preview */
	uint32_t stride_bytes;     /* >= width*4 */
	const uint8_t *bgra;       /* BGRA8, top-down */
	/* Monitor region the preview covers, in CANVAS-normalised coords:
	   (0,0) = canvas top-left, (1,1) = canvas bottom-right. Normally 0,0,1,1
	   (the desktop region under the app canvas); a DP may include a margin. */
	float canvas_u0, canvas_v0, canvas_u1, canvas_v1;
	uint32_t flags;            /* bit0 XRT_DP_BG_PREVIEW_STALE: source KNOWS the preview is invalid (unchanged desktop is NOT stale) */
	uint32_t reserved[7];
};

bool (*get_background_preview)(struct xrt_display_processor_d3d11 *xdp,
                               struct xrt_dp_background_preview *out);
```

- **Slot 24 on D3D11** (after `set_predicted_scanout` = 23), **appended at the
  end on every per-API variant** (`d3d11`, `d3d12`, `gl`, `vk`, `metal`) — never
  on the base vtable, for the reason given in the section above. Guarded the same
  way `set_frame_timing` is: `XRT_DP_HAS_SLOT` (the `struct_size` read-clamp) plus
  a NULL check, an `XRT_DP_<API>_HAS_*` define, and an `offsetof` `static_assert`.
- **Optional.** A NULL slot or a `false` return means *no source* — the runtime
  reports `CLIPPED_NO_SOURCE` and the app clips at the display plane, which is
  exactly the pre-ADR-040 behaviour. Failing closed is always safe, so return
  `false` freely: capture disabled, client-present mode, a cross-monitor drag,
  the capture API unavailable, HWND NULL.
- **Pixels only, never a verdict.** The plug-in returns an image and a
  generation; the runtime owns the perceptual policy. A DP that returned
  "background is busy" would put perception in vendor code — explicitly rejected
  in ADR-040.
- **Lifetime.** `bgra` is *borrowed*: it must stay valid until the next
  `process_atlas()` on the same DP (render thread). The runtime analyses
  immediately and never retains the pointer.
- **Cadence.** The runtime polls at most every 66 ms and re-analyses only when
  `generation` advanced, so a plug-in should produce the preview at its existing
  capture throttle (<= 15 Hz) and never per weave.
- **`struct_size` is the runtime's, not yours.** The runtime sets it before the
  call; write only the fields that fit within it.
- **The canvas rect became load-bearing in `XR_DXR_depth_budget` v2.** The slot
  itself is unchanged, but the runtime now maps the app's reported content
  bounds through `canvas_u0..v1` to narrow the analysis region. A plug-in that
  includes a margin around the canvas MUST say so there; one that leaves the
  four floats zeroed is read as the documented normal case, `0,0,1,1`.
- **Purely additive**, so no `XRT_PLUGIN_API_VERSION_CURRENT` bump (ADR-020).

## Where the window may LAND: `snap_window_rect`

A windowed weave takes its interlace phase from the window's absolute position on the panel.
Move the window a few pixels and the phase walks across the lens pitch, the subpixels shift
under the lenses, and the 3D dissolves into crosstalk jitter for the length of the drag. The
fix is not to correct the phase afterwards; it is to make sure the window only ever lands on
positions where the phase is the same. So the runtime asks:

```c
bool (*snap_window_rect)(struct xrt_display_processor_vk *xdp, /* or _d3d11 */
                         int32_t origin_x, int32_t origin_y,   /* drag-start top-left */
                         int32_t target_x, int32_t target_y,   /* proposed top-left */
                         int32_t *out_x, int32_t *out_y);      /* snapped top-left */
```

- **Slot 18 on D3D11** (#625), and **appended at the end of the Vulkan variant** (#1588,
  `XRT_DP_VK_HAS_SNAP_WINDOW_RECT`) with the identical signature, so a plug-in can share one
  body across both. Guarded exactly like every other appended slot: the `struct_size`
  read-clamp plus a NULL check, an `XRT_DP_<API>_HAS_*` define, and an `offsetof`
  `static_assert`. **Purely additive**, so no `XRT_PLUGIN_API_VERSION_CURRENT` bump (ADR-020).
- **The pitch never leaves the plug-in.** Two points go in, one comes back. The runtime holds
  no lens parameter and derives none; that is the ADR-019 line, and it is the reason this is
  a slot rather than a `get_lens_pitch()`.
- **Both coordinates.** A lenticular lattice is slanted, so the invariant is generally
  `x + slant·y`, not `x`. Snap on the invariant and return the `x` it implies for the given
  `y`; the runtime does no arithmetic of its own on the result.
- **One frame, and it does not matter which.** Only `target - origin` is used: the vendor
  canonicalises the displacement, snaps from (0,0) and re-adds the origin, and the phase
  search minimises `remainder(ph - ph0, 1.0)` — so a constant added to both points cancels.
  The lattice is anchored to the **drag origin**, not the panel or the desktop.
  Desktop-absolute and panel-relative give the identical answer, so **the runtime performs
  no conversion, on any platform**. The only thing that breaks this call is *mixing* frames
  between the two arguments.
- **Device pixels, never logical/scaled ones.** Translation cancels; a scale factor does
  not. A displacement in logical pixels on a fractionally-scaled output arrives multiplied
  and snaps to the wrong lattice point while still looking plausible.
- **A snap preserves the phase the window already had** — it does not search for a good one.
  Success on a badly-phased window keeps it badly phased. A correct result also never moves
  more than ~2 px in canonical space (the vendor search radius is 2); further than that means
  mixed frames or scaled pixels, not a large pitch.
- Only the top-left is snapped — the caller keeps the size (on an edge-resize the caller
  compensates the extent itself).
- **A pure query.** It must not move a window, re-phase a live weaver, or touch the hardware
  lens state, and it must be cheap and non-blocking: on Windows it runs inside the window
  proc's `WM_WINDOWPOSCHANGING`, and on Linux it can be called once per pointer-motion event.
- **Optional.** An absent slot or a `false` return means "no lattice to snap to" and the
  caller keeps its proposed position — which is what every platform did before the slot
  existed, and what `sim_display` does today (anaglyph has no lattice).
- **Who calls it — the window owner, and nobody else.** The runtime's own window proc for a
  runtime-owned window, the D3D11 service for a cross-process present owner, and — on
  desktop Linux, where a client cannot hook the window manager's drag — the **app**, through
  `xrWeaveSnapWindowRectDXR`. Always *before* the window moves.

  **The runtime feeds the weaver the window's TRUE origin and never quantises it.** This is
  a rule hardware established, not a style preference. A Vulkan build briefly re-snapped the
  origin it fed `set_present_origin`, on the theory that it could only ever correct a
  metrics read that caught an already-snapped window mid-flight. Measured on the DS1 with a
  real Leia DP, a 16-step drag: the fed origin diverged from the true window origin on **12
  of 13 moves, by up to 2 px** — window at panel-relative (726, 314), weaver told (726,
  316). Snapping an origin without moving the window to match *is* phase error against the
  lens; and a second snap chain anchored on its own previous output drifts away from the
  owner's, which is anchored on the drag origin. Only the party that knows where the window
  is going may snap it.

## Whether the content is transparent NOW: `set_transparency_active`

`set_transparent_background` is a **session capability**: it is declared at
`xrCreateSession`, because the window visual and the swapchain alpha mode are fixed
there. An app that *can* go transparent (an opaque scene with a transparency toggle)
therefore declares it from its first frame, and a DP that starts a desktop capture on
that call pays for the capture, the compose-under and the alpha-gate on every opaque
frame. Measured before this slot: a transparency-capable Linux app, opaque, cost the
same as a transparent one.

```c
void (*set_transparency_active)(struct xrt_display_processor_vk *xdp, bool active);
```

- **The runtime measures, the DP decides.** Every app frame the Vulkan compositor
  probes the atlas it is about to hand `process_atlas` for any alpha < 1 (a compute
  pass sampling one texel per 4x4 block, read back one frame later without a stall)
  and debounces the verdict: **ACTIVE on the first transparent frame, IDLE after 60
  consecutive opaque frames** (`comp_lazy_transparency.h`). The DP only hears
  transitions. What "idle" releases is the DP's business.
- **Default ACTIVE.** A runtime that predates the slot never calls it, so a DP must
  behave exactly as before until told otherwise. The runtime may call it once with
  `false` *before* `set_transparent_background(true)`, so a DP that starts a capture
  on enable can defer it until the content is transparent.
- **While IDLE the output must be opaque** — which it already is when the alpha-gate
  is skipped, because the content has no transparent pixel for it to punch.
- **The first transparent frame after an idle stretch weaves with the DP still idle**
  (the probe reads back one frame late). After that, the DP runs whatever it runs for
  a frame without a trusted background — on Leia/Linux, silhouette intersection —
  until its restarted capture delivers. Nothing captured before the idle stretch is
  reused: the desktop behind the window has had all that time to change.
- **Do not block the frame thread.** Starting a capture can take a large fraction of
  a second; start and stop it off-thread.
- **Scope today: desktop Linux, windowed `vk_native` sessions.** The shared-texture
  path and the Windows #918 split weave through call sites that do not run the probe,
  so the runtime never engages the policy there (a DP told "idle" there would never
  hear "active" again). `DXR_LAZY_TRANSPARENCY=0` disables it for A/B.
- Appended at the end of the Vulkan variant, `XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE`,
  guarded like every other appended slot. **Purely additive** — no
  `XRT_PLUGIN_API_VERSION_CURRENT` bump (ADR-020). `sim_display` implements it as a
  logging test double (`SIM TRANSPARENCY ACTIVE/IDLE`), which is how the runtime half
  is verified without vendor hardware.

## Frame-timing inputs are an offer, never a requirement

The runtime does a lot of work to make each frame reach the panel as late and
as predictably as possible, and it hands the results to the display processor.
**All of it is an offer.** A display processor that ignores every one of these
inputs must still render correctly; it simply forgoes the accuracy they enable.
Nothing in the runtime's frame path assumes a plug-in predicts anything.

Concretely, and verified rather than assumed:

| Input | How it reaches the plug-in | What happens if the plug-in ignores it |
|---|---|---|
| Measured weave→scanout residual and panel period | `set_frame_timing`, an **optional appended slot** on each per-API vtable | Guarded by both `XRT_DP_HAS_SLOT` (a `struct_size` read-clamp, so a plug-in compiled against older headers is safe) and a NULL check. The runtime skips the call. |
| Presentation paced to the previous frame's real scanout | Nothing — the runtime simply presents later | The plug-in sees an ordinary `process_atlas` call. Pacing changes *when* it is called, never *what* it must do. |
| Fresh viewer position at weave time | The plug-in's own `get_predicted_eye_positions`, if it chooses to re-predict | A plug-in that samples once, or not at all, produces a correct frame with a staler viewer position. No runtime path requires re-prediction. |

The corollary matters for anyone writing a plug-in: **do not treat late
presentation as a contract that you will be called at a particular time.** The
runtime may present earlier or later as the pipeline changes, and on Vulkan the
pacing depends on device features the *application* may not have enabled (see
below), in which case it is dormant and every frame still arrives.

The one place this asymmetry shows up in application code is Vulkan device
creation. Pacing needs `VK_KHR_present_id` + `VK_KHR_present_wait`, which can
only be enabled by whoever calls `vkCreateDevice`. Under
`XR_KHR_vulkan_enable2` that is the runtime, which requests both when the
driver reports support and proceeds without them when it does not. Under
`XR_KHR_vulkan_enable` it is the application, and if it does not enable them
the runtime logs one warning and runs unpaced. Either way nothing fails — the
difference is accuracy, not correctness. Application-side guidance:
[INV-5.9](../guides/displayxr-app-rules.md).

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

### Log levels — the one rule that matters

**Never call `U_LOG_W` (or `U_LOG_E`) on a per-frame path.** A plug-in shares the
application's log file, so a warning on a hot path becomes a flood at frame rate:
observed twice in shipping plug-in code, once at ~22 Hz producing 6,429 lines /
1.17 MB in six minutes, and once as ~2,900 lines in 14 seconds while a window
handle was being re-matched. Both were real hot-path I/O, and both buried
everything else in the log at the moment someone needed to read it.

The example above is correct precisely because init runs once.

- `U_LOG_W` / `U_LOG_E` — one-off init, error and lifecycle events **only**.
- `U_LOG_I` and below — anything recurring, and throttle it as well.
- For a condition that can persist across many frames (a lost window, a
  disconnected tracker, an unavailable device), log the **first occurrence** and
  then a **count on recovery**. That turns a flood into a signal, and it is the
  shape to reach for whenever a per-frame `U_LOG_W` feels tempting.

This is the same convention the runtime holds itself to — see
[debug logging](debug-logging.md).

## Related

- [Vendor plug-in onboarding](../guides/vendor-plugin-onboarding.md) — high-level guide for building + shipping a plug-in
- [Plug-in discovery spec](../specs/runtime/plugin-discovery.md) — registry / manifest formats, env-var overrides
- [ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md) — vendor / aux boundary rationale
- [`xrt_plugin.h`](../../src/xrt/include/xrt/xrt_plugin.h) — the C ABI header itself
