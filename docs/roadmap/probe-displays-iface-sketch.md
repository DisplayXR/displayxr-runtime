# Design sketch: `probe_displays()` — per-display vendor claims

> **Status: SHIPPED — historical.** This sketch's ABI is now the real
> `xrt_plugin_iface::probe_displays()` (runtime **v1.9.0**, leia **v1.2.0**). For current
> state and the remaining compositor work see
> [multi-display-single-machine.md → Phase 3 design decisions](multi-display-single-machine.md#phase-3-design-decisions).
> Kept only for the original rationale; do not treat as a live proposal.

**Status:** sketch / discussion. Refines [ADR-015](../adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md)
and the [multi-display roadmap](multi-display-single-machine.md) (#69) now that the
vendor-neutral EDID enumerator and a vendor validator both exist in tree.

## What already exists (don't rebuild)

- **Vendor-neutral enumeration** — `src/xrt/auxiliary/os/os_display_edid.{h,win32.c}`:
  `os_display_edid_enumerate()` → per-monitor `{manufacturer_id, product_id,
  screen pos, pixel dims, refresh, primary, HMONITOR}`. Built, Windows-complete,
  **no callers yet**. Now surfaced read-only via `displayxr-cli displays` (#380).
- **A vendor validator** — the Leia plug-in's `leia_edid_probe_display()` already
  does the work: EDID-table match (60+ panels) over the shared `os_display_edid`
  helper, plus SR-SDK-installed + SR-service-running checks, returning
  `{hw_found, sdk_installed, service_running, mfr/product, dims, screen, serial}`.
  Today it backs the **binary, system-level** `xrt_plugin_iface::probe()`.

The gap ADR-015/#69 fills: turn that binary "do I claim this *system*?" into a
per-**monitor** claim list, so DisplayXR can route monitor→DP (incl. mixed-vendor
and force-sim-on-one-monitor).

## Proposed ABI addition (append-only, ADR-020-safe)

A new **optional** method appended to the end of `xrt_plugin_iface` (after
`set_pose_source`), NULL-safe, guarded by `struct_size` — no version bump:

```c
// Vendor-neutral descriptor the runtime hands the plug-in (built from
// os_display_edid). The plug-in echoes back monitor_id for the ones it claims.
struct xrt_display_descriptor {
    uint32_t struct_size;
    uint64_t monitor_id;        // runtime-assigned, stable for this boot
    uint16_t edid_manufacturer; // raw EDID bytes 8-9
    uint16_t edid_product;      // raw EDID bytes 10-11
    uint32_t pixel_width, pixel_height;
    uint32_t refresh_mhz;       // milli-Hz (no float across the ABI)
    int32_t  screen_left, screen_top;
    uint32_t flags;             // bit0 = primary
    // future: const uint8_t *edid_blob; uint32_t edid_blob_len; (append)
};

enum xrt_display_claim_confidence {
    XRT_DISPLAY_CLAIM_FALLBACK  = 10,  // sim_display: "anything unclaimed"
    XRT_DISPLAY_CLAIM_EDID      = 50,  // matched my EDID table
    XRT_DISPLAY_CLAIM_VERIFIED  = 100, // EDID + SDK/service/serial handshake
};

struct xrt_display_claim {
    uint64_t monitor_id;     // which descriptor this claims
    uint32_t confidence;     // xrt_display_claim_confidence
    uint32_t supported_apis; // bitmask: which create_dp_<api> work for this monitor
    char     serial[64];     // vendor device serial (e.g. a hardware serial); "" if n/a
};

// Returns the number of claims written to out_claims (<= max_claims).
uint32_t (*probe_displays)(struct xrt_plugin_instance *inst,
                           const struct xrt_display_descriptor *displays,
                           uint32_t display_count,
                           struct xrt_display_claim *out_claims,
                           uint32_t max_claims);
```

## Runtime flow

1. `os_display_edid_enumerate()` → assign each monitor a stable `monitor_id`,
   build `xrt_display_descriptor[]`.
2. For each registered plug-in (ProbeOrder order), call `probe_displays()` with
   the descriptor array; collect claims.
3. **Resolve per monitor:** highest `confidence` wins; ties broken by ProbeOrder;
   a per-display user override (see below) trumps all.
4. Build the **DP factory registry** (`monitor_id → {iface, instance, per-API
   factories}`) replacing the scalar `dp_factory_*` on
   `xrt_system_compositor_info`. Single-display = single-entry (backward compat).
5. Compositor looks up the DP factory by the monitor a window is on (ADR-015 §3–5
   own the split-weave / per-display lifecycle).

## Backward compatibility

- `probe_displays == NULL` (or `struct_size` too small) → fall back to today's
  binary `probe()`: a successful `probe()` is treated as a single
  `XRT_DISPLAY_CLAIM_EDID` claim on the primary monitor. Existing single-display
  plug-ins keep working unchanged.
- `sim_display` implements `probe_displays()` as: claim every descriptor at
  `XRT_DISPLAY_CLAIM_FALLBACK` — so it backstops any monitor no vendor claimed,
  and a per-display override can still force it.

## Per-display override (extends #378's `PreferredPlugin`)

#378 added a single global `PreferredPlugin`. Generalize to per-monitor:
a `PreferredPlugin\<monitor-key>` value (monitor-key = `MFR+PRODUCT` or a
device-path hash) pins a specific plug-in id to a specific display; the global
value remains the default. `displayxr-cli dp` / the Control Panel "Displays" tab
gain a per-row assignment.

## Vendor migration example (minimal)

`probe_displays()` for the example vendor plug-in is a thin wrapper over its
existing `leia_edid_probe_display()`: match each descriptor against the EDID
table → `XRT_DISPLAY_CLAIM_EDID`; upgrade to `XRT_DISPLAY_CLAIM_VERIFIED` when
SDK + service + hardware serial are present (fill `serial`). No new detection
logic — just re-shape the already-collected result into per-monitor claims.

## Open questions

- `monitor_id` stability across hotplug/resolution change within a boot — key off
  the EDID device-instance path, not the transient HMONITOR.
- Confidence for two *same-vendor* panels (two displays from one vendor): per-claim `serial`
  disambiguates which DP/camera-calibration pairs with which monitor (ADR-015 §6).
- Whether to pass the raw EDID blob (some vendors need vendor-specific EDID
  extension blocks beyond mfr/product) — appendable later without an ABI bump.
