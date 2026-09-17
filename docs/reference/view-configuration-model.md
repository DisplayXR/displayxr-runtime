# View-configuration model — what `PRIMARY_STEREO` means in DisplayXR

DisplayXR drives displays whose rendering modes span **1 to 4 views** (2D, stereo,
quad), while OpenXR makes the view count a property of the *view configuration*.
This page records how the runtime reconciles the two **today**, the spec deviation
that follows, and the planned fix — current behaviour, not a design we defend.

> **Known deviation.** DisplayXR advertises exactly one view configuration —
> `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO` for any device with more than one
> view — and reports the **maximum view count across all of that device's
> rendering modes**. On a device with a quad mode that is **4**, not 2, in every
> mode. The OpenXR spec ties `PRIMARY_STEREO` to two views. Tracked as
> [#1486](https://github.com/DisplayXR/displayxr-runtime/issues/1486).

## What the runtime advertises

There is exactly one `view_config_type` per system — the runtime models **one**
view configuration, never a list. The mapping is
`src/xrt/state_trackers/oxr/oxr_system.c:111-117`:

```c
sys->view_count = view_count;
if (view_count == 1) {
        sys->view_config_type = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
} else {
        // view_count >= 2: treat as stereo (including quad, lightfield, etc.)
        sys->view_config_type = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
}
```

`sys->view_count` is handed verbatim to `xrEnumerateViewConfigurationViews`
(`oxr_system.c:847`, the non-mono branch of `OXR_TWO_CALL_FILL_IN_HELPER`), and
`xrLocateViews` returns the same number (`oxr_session.c:1825` reads
`xdev->hmd->view_count`, `:1837` writes it to `*viewCountOutput`).

## Where the number comes from

A device reports the **max across its rendering modes**. The in-tree
`sim_display` driver is the reference (`src/xrt/drivers/sim_display/sim_display_device.c:754-762`):

```c
// view_count = max across all rendering modes
uint32_t max_views = 1;
for (uint32_t m = 0; m < hmd->base.rendering_mode_count; m++)
        if (hmd->base.rendering_modes[m].view_count > max_views)
                max_views = hmd->base.rendering_modes[m].view_count;
hmd->base.hmd->view_count = max_views;
```

`sim_display` declares 5 modes (`:690` — 2D, Anaglyph, Cropped SBS, Squeezed SBS,
Quad) and mode 4 is Quad with `view_count = 4` (`:735`). So **every** sim-display
instance reports 4, regardless of which mode is active. This is not latent and not
mode-dependent.

## The count is fixed for the instance lifetime

Two counts are in play, and only one of them moves:

| | source | changes on a mode switch? | what it governs |
|---|---|---|---|
| `view_count` | max across modes | **no** | what `xrEnumerateViewConfigurationViews` / `xrLocateViews` return |
| `active_view_count` | the active mode | yes | mono-vs-3D eye assignment inside `xrLocateViews` |

`oxr_session.c:1820-1833` states this deliberately. Because the returned count
never moves, the core spec rule ("the count is fixed by the
`XrViewConfigurationType`") and `XR_EXT_view_configuration_views_change`'s
view-count-immutability clause are **already satisfied**. The open question is
narrow: *which type do we name, and with how many views.*

## Why it is built this way

- **N-view displays are the point.** Quad / lightfield modes are a shipping
  capability of the display class; clamping the surface to 2 would make them
  unreachable through `xrLocateViews`.
- **One worst-case swapchain** ([ADR-010](../adr/ADR-010-shared-app-iosurface-worst-case-sized.md)):
  the app swapchain is sized once for the worst case across modes and never
  resized, so a stable max-sized view surface is the matching shape.
- **Apps are already told to size for the max.** `INV-3.1`
  ([app rules](../guides/displayxr-app-rules.md)) requires apps to locate into an
  `XRT_MAX_VIEWS` (8)-wide buffer and render/submit the **active mode's** count,
  never a hardcoded 2 — so an extension app is not surprised by a 4.

## What catches it

OpenXR-CTS 1.1.57 added the automated (untagged, **not** `[interactive]`) test
`xrLocateSpace_xrLocateViews`
(`src/conformance/conformance_test/test_xrLocateSpace.cpp:260`, verified present
at tag `openxr-cts-1.1.63.0`). For every advertised view-configuration type it
asserts that VIEW space equals the centroid of the `xrLocateViews` origins, and
for `PRIMARY_STEREO` it asserts `REQUIRE(views.size() == 2)`
(`test_xrLocateSpace.cpp:323`). Against the default sim-display configuration
that is a certain failure.

Until the deviation is fixed, the test is **excluded by name** from the CTS specs
in `.github/workflows/cts.yml` and `scripts/run_cts.ps1`, with a comment naming
#1486. The exclusion is not a claim of conformance — we do not claim conformance
on this test.

## What consumers see

- **Engine plug-ins (Unity, Unreal)** are fixed at **2 views** — no view synthesis
  exists anywhere in the stack — so a 4-view quad mode is already unfillable by
  them and nothing changes for them under any of the candidate fixes.
- **DisplayXR extension apps** follow INV-3.1 and are correct as written; legacy
  apps that hardcode 2 were already out of contract.

## Planned fix

The target is **a DXR-owned `XrViewConfigurationType`** advertised alongside a
conformant 2-view `PRIMARY_STEREO`: apps that want N-view opt into the DXR type
explicitly, and `PRIMARY_STEREO` becomes spec-clean. That is option B in
[#1486](https://github.com/DisplayXR/displayxr-runtime/issues/1486) and it needs
an enum value from the registered DXR block; it will most likely live in
`XR_DXR_display_info`'s block.

When that lands it ships behind one next-launch kill switch, whose name is
**reserved here so nothing else takes it**: `DXR_VIEW_CONFIG_LEGACY=1` restores
exactly today's mapping (any `view_count >= 2` → `PRIMARY_STEREO`, count = max
across modes). It is a Tier-1 test/dev lever and will be registered in
[`control-panel-performance-settings.md`](../roadmap/control-panel-performance-settings.md)
Appendix A when it is implemented. **Nothing reads it today.**
