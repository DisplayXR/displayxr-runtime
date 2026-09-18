---
status: Implemented as a vendor type — Khronos submission still open
owner: David Fattal
updated: 2026-09-17
issues: [80, 1486]
code-paths: [src/xrt/state_trackers/oxr/, src/external/openxr_includes/openxr/XR_DXR_display_info.h]
---

> **Status: Implemented as a vendor type.** The runtime ships
> `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR` (`XR_DXR_display_info`
> spec v19, [#1486](https://github.com/DisplayXR/displayxr-runtime/issues/1486)
> option B). What remains open on [#80](https://github.com/DisplayXR/displayxr-runtime/issues/80)
> is the **Khronos submission** of a cross-vendor `..._PRIMARY_MULTIVIEW`.
> How the shipped model behaves, end to end:
> [View-Configuration Model](../reference/view-configuration-model.md).

# Khronos Proposal: XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW

## Context

Our `XR_DXR_display_info` extension supports N-view rendering modes (1, 2, or 4 views).
Until #1486 those modes were reached under `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`,
which worked only because we control the runtime validation — the `xrEndFrame` check read
the active rendering mode's `view_count` instead of hardcoding "must be 1 or 2".

`PRIMARY_STEREO` semantically implies exactly 2 views. Reporting 4 views for it, and
accepting 4 projection views under it, was a spec stretch that other runtimes or
validation layers would reject — and that OpenXR-CTS 1.1.57's
`xrLocateSpace_xrLocateViews` fails outright (`REQUIRE(views.size() == 2)`).

## What shipped (the vendor type)

```c
// src/external/openxr_includes/openxr/XR_DXR_display_info.h, spec v19
#define XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR ((XrViewConfigurationType)1004999212)
```

- `xrEnumerateViewConfigurations` returns a **list**: a conformant, always-2-view
  `PRIMARY_STEREO`, plus `PRIMARY_MULTIVIEW_DXR` when `XR_DXR_display_info` is enabled on
  the instance.
- Under the DXR type, `xrEnumerateViewConfigurationViews` and `xrLocateViews` report the
  device's **max view count across rendering modes** (fixed for the instance lifetime), and
  `xrEndFrame` accepts a projection layer whose `viewCount` matches any rendering mode's
  count.
- Under `PRIMARY_STEREO` the runtime reports exactly 2 and rejects `viewCount > 2`, naming
  the DXR type as the opt-in.
- Kill switch `DXR_VIEW_CONFIG_LEGACY=1` restores the old single-type mapping.

App-side contract: `INV-3.1` in
[`docs/guides/displayxr-app-rules.md`](../guides/displayxr-app-rules.md).

## Remaining ask — standardize it

When submitting the `XR_DXR_display_info` spec to Khronos, propose the cross-vendor

```c
XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW
```

with the same semantics the DXR type already implements:

- Accept a variable number of views (1, 2, 4, or more) as specified by the active
  rendering mode.
- Be returned by `xrEnumerateViewConfigurations` when the enabling extension is supported.
- Allow `xrLocateViews` to return N views and `xrEndFrame` to accept N projection views.
- Coexist with `PRIMARY_STEREO` (a runtime may support both).

Until then, one caveat is worth carrying into the submission: the CTS **conformance layer**
exact-matches `XrViewConfigurationType` against the Khronos `xr.xml` list
(`conformance_layer/RuntimeFailure.h:73`, `Instance.cpp:54`), so any vendor value is
structurally un-blessable — a registered core (or `EXT`) enum is the only real fix. It does
not bite today only by accident; see the caveat in the
[View-Configuration Model](../reference/view-configuration-model.md#cts-status).

## Related

- [#1486](https://github.com/DisplayXR/displayxr-runtime/issues/1486) — the deviation and its fix (option B)
- Issue #77 (N-view rendering modes)
- `src/xrt/state_trackers/oxr/oxr_system.c` (`oxr_system_fill_in`, the advertised list)
- `src/xrt/state_trackers/oxr/oxr_session_frame_end.c` (`verify_projection_layer`, per-type `viewCount` rule)
- [`docs/specs/extensions/XR_DXR_display_info.md`](../specs/extensions/XR_DXR_display_info.md) §*New Enums*
