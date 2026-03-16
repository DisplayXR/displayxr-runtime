---
status: Active
owner: David Fattal
updated: 2026-03-15
issues: [81]
code-paths: [src/external/openxr_includes/openxr/XR_EXT_display_info.h, src/xrt/state_trackers/oxr/]
---

# MANAGED vs MANUAL Eye Tracking Contract for Vendor-Controlled Display Transitions

## Summary

Formalize the contract between MANAGED and MANUAL eye tracking modes with respect to vendor-controlled 2D/3D display transitions on tracking loss. Strategy: **MANAGED = vendor SDK controls grace period, animations, auto 2D/3D switching; MANUAL = developer controls everything, SDK just reports isTracking immediately**. No new API needed -- this clarifies existing semantics and specifies vendor integration requirements.

## Background

3D displays need to handle a critical transition: **what happens when eye tracking is lost while the display is in 3D mode?** The user may have walked away, looked away, or the tracker lost lock. The display should gracefully degrade to 2D rather than show broken stereo.

Today, `XR_EXT_display_info` v6 provides two eye tracking modes:

- **MANAGED** (`XR_EYE_TRACKING_MODE_MANAGED_EXT = 0`): Vendor SDK controls grace period, animations, and auto 2D/3D switching. App sees filtered positions and an `isTracking` flag.
- **MANUAL** (`XR_EYE_TRACKING_MODE_MANUAL_EXT = 1`): Developer controls everything. SDK just reports `isTracking` immediately with no grace period, no animations, and no auto-switching. App handles tracking loss itself.

And the rendering mode API (v7/v8) provides:

- `xrRequestDisplayRenderingModeEXT(modeIndex)` -- switch rendering modes (which may flip hardware 2D/3D)
- `XrEventDataRenderingModeChangedEXT` -- notifies app of mode changes
- `XrEventDataHardwareDisplayStateChangedEXT` -- notifies app of hardware 3D state changes

**The gap:** There is no specification of how these two systems interact during tracking loss/recovery transitions.

## Contract

### MANAGED mode -- vendor controls everything

When the app is in MANAGED mode (default), the vendor SDK owns the full tracking-loss lifecycle:

1. **Tracking lost** -> Vendor SDK plays **collapse animation** (smoothly reduces IPD/parallax toward zero over a grace period, typically 500ms-2s)
2. **Grace period expires** -> Vendor SDK auto-switches hardware display to 2D mode
3. **Tracking resumes** -> Vendor SDK auto-switches hardware display back to 3D mode, plays **revival animation** (smoothly restores tracked IPD/parallax)
4. **During grace period**, if tracking resumes -> Vendor SDK plays revival animation directly, no 2D switch occurs

The app receives:
- Smoothed, continuous eye positions throughout (no jumps)
- `isTracking` remains vendor-determined (may stay `true` during grace period, goes `false` after)
- `XrEventDataRenderingModeChangedEXT` + `XrEventDataHardwareDisplayStateChangedEXT` when vendor auto-switches 2D<->3D

The app is passive -- it does not need to call `xrRequestDisplayRenderingModeEXT` during these transitions.

### MANUAL mode -- developer controls everything

When the app requests MANUAL mode, the vendor SDK must:

1. **Never play** collapse or revival animations
2. **Never auto-switch** the display between 2D and 3D on tracking loss/recovery
3. **Immediately report** `isTracking = false` when tracking is lost (no grace period hiding)
4. **Immediately report** `isTracking = true` when tracking resumes
5. **Return unprocessed** eye positions at all times

The app is responsible for its own strategy:
- Detect `isTracking` transition to `false`
- Animate convergence/IPD to zero (optional, app's choice)
- Call `xrRequestDisplayRenderingModeEXT(2D_mode)` when ready
- Detect `isTracking` transition to `true`
- Call `xrRequestDisplayRenderingModeEXT(3D_mode)` to resume
- Animate convergence/IPD back to tracked values

## Vendor Integration Requirements

### SDK wrapper API surface

The vendor SDK wrapper (e.g., `leia_sr.h`) must expose control over these behaviors. Suggested API pattern:

```c
// Grace period + animation control
bool vendor_sdk_set_tracking_loss_animation(struct vendor_sdk *sdk, bool enable);
bool vendor_sdk_set_auto_display_mode_switch(struct vendor_sdk *sdk, bool enable);

// Or combined:
bool vendor_sdk_set_managed_mode(struct vendor_sdk *sdk, bool enable);
// enable=true: SDK plays animations + auto-switches 2D/3D (MANAGED)
// enable=false: SDK returns positions directly, no animations, no auto-switch (MANUAL)
```

### Display processor integration

The display processor must:

1. **Store the active eye tracking mode** (like it now stores `view_count`)
2. **On mode change** (`xrRequestEyeTrackingModeEXT`): call vendor SDK to enable/disable managed behavior
3. **In `get_predicted_eye_positions()`**: vendor SDK already behaves differently based on the mode setting -- no display processor post-processing needed for MANAGED vs MANUAL (the SDK does it)

### Event propagation for vendor-initiated transitions (MANAGED mode)

When the vendor SDK auto-switches 2D/3D (during MANAGED tracking loss/recovery), the runtime must fire events to the app. This requires a **callback or polling mechanism** from the vendor SDK to the display processor:

```c
// Option A: Vendor SDK callback
typedef void (*vendor_display_mode_changed_fn)(void *userdata, bool is_3d);
bool vendor_sdk_set_display_mode_callback(struct vendor_sdk *sdk,
                                          vendor_display_mode_changed_fn cb,
                                          void *userdata);

// Option B: Polling (simpler, per-frame check)
bool vendor_sdk_get_current_hardware_3d_state(struct vendor_sdk *sdk, bool *out_is_3d);
```

The compositor (or display processor) checks for state changes each frame and pushes `XrEventDataHardwareDisplayStateChangedEXT` + `XrEventDataRenderingModeChangedEXT` when the hardware state flips.

### Capability advertisement

Vendor sets capability bits in `xrt_system_compositor_info`:

| Vendor capability | `supported_eye_tracking_modes` | `default_eye_tracking_mode` |
|---|---|---|
| SDK has grace period + animation + auto-switch | `3` (MANAGED \| MANUAL) | `0` (MANAGED) |
| SDK has managed filtering only, no auto-switch control | `1` (MANAGED only) | `0` (MANAGED) |
| SDK provides manual only (no filtering available) | `2` (MANUAL only) | `1` (MANUAL) |
| No eye tracking | `0` | N/A |

Ideally vendors support **both** modes (bits = 3), giving developers the choice.

## Non-goals

- No new OpenXR extension functions or structs -- existing API is sufficient
- No changes to `XrDisplayRenderingModeInfoEXT` -- eye tracking behavior is orthogonal to rendering mode definition
- No per-rendering-mode tracking behavior (keeps it simple; if needed later, can be added as a separate feature)

## Acceptance Criteria

- [ ] Vendor integration guide updated with MANAGED/MANUAL transition contract
- [ ] `XR_EXT_display_info.h` comments updated to document auto-switch behavior per mode
- [ ] Leia SR display processors pass eye tracking mode to SDK wrapper when `xrRequestEyeTrackingModeEXT` is called
- [ ] Event propagation path exists for vendor-initiated 2D/3D switches (MANAGED mode auto-transitions fire `XrEventDataRenderingModeChangedEXT` + `XrEventDataHardwareDisplayStateChangedEXT`)
- [ ] sim_display updated: MANUAL mode returns immediate `isTracking` transitions, MANAGED mode simulates grace period (optional, for testing)
