---
status: Proposal
owner: David Fattal
updated: 2026-03-15
issues: [80]
code-paths: [src/xrt/state_trackers/oxr/]
---

# Khronos Proposal: XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW

## Context

Our `XR_EXT_display_info` extension supports N-view rendering modes (1, 2, or 4 views) under `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`. This works today because we control the runtime validation -- we dynamically check the active rendering mode's `view_count` instead of hardcoding "must be 1 or 2".

However, `PRIMARY_STEREO` semantically implies exactly 2 views. Submitting 4 views under a "stereo" view configuration type is a spec stretch that other runtimes or validation layers would reject.

## Proposal

When submitting the `XR_EXT_display_info` extension spec to Khronos, propose a new view configuration type:

```c
XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW
```

This type would:
- Accept a variable number of views (1, 2, 4, or more) as specified by the active rendering mode
- Be returned by `xrEnumerateViewConfigurations` when `XR_EXT_display_info` is supported
- Allow `xrLocateViews` to return N views and `xrEndFrame` to accept N projection views
- Coexist with `PRIMARY_STEREO` (runtime can support both)

## Current Workaround

In `oxr_session_frame_end.c`, the `PRIMARY_STEREO` validation dynamically reads `head->rendering_modes[active_idx].view_count` instead of hardcoding 2. This works for our runtime but won't pass external OpenXR validation layers.

## Related

- Issue #77 (N-view rendering modes)
- `src/xrt/state_trackers/oxr/oxr_session_frame_end.c` lines 609-621
- `src/xrt/state_trackers/oxr/oxr_system.c` line 115 (`view_count >= 2: treat as stereo`)
