---
status: Active
owner: David Fattal
updated: 2026-03-15
issues: [81]
code-paths: [src/external/openxr_includes/openxr/XR_EXT_display_info.h, src/xrt/state_trackers/oxr/]
---

# SMOOTH vs RAW Eye Tracking Contract for Vendor-Controlled Display Transitions

## Summary

Formalize the contract between SMOOTH and RAW eye tracking modes with respect to vendor-controlled 2D/3D display transitions on tracking loss. Strategy: **SMOOTH = vendor owns all transitions, RAW = developer owns all transitions**. No new API needed -- this clarifies existing semantics and specifies vendor integration requirements.

## Background

3D displays need to handle a critical transition: **what happens when eye tracking is lost while the display is in 3D mode?** The user may have walked away, looked away, or the tracker lost lock. The display should gracefully degrade to 2D rather than show broken stereo.

Today, `XR_EXT_display_info` v6 provides two eye tracking modes:

- **SMOOTH** (`XR_EYE_TRACKING_MODE_SMOOTH_EXT = 0`): Vendor SDK handles grace period + smoothing. App sees filtered positions and an `isTracking` flag.
- **RAW** (`XR_EYE_TRACKING_MODE_RAW_EXT = 1`): Vendor SDK provides unfiltered positions. App sees raw positions + `isTracking` flag and handles tracking loss itself.

And the rendering mode API (v7/v8) provides:

- `xrRequestDisplayRenderingModeEXT(modeIndex)` -- switch rendering modes (which may flip hardware 2D/3D)
- `XrEventDataRenderingModeChangedEXT` -- notifies app of mode changes
- `XrEventDataHardwareDisplayStateChangedEXT` -- notifies app of hardware 3D state changes

**The gap:** There is no specification of how these two systems interact during tracking loss/recovery transitions.

## Contract

### SMOOTH mode -- vendor controls everything

When the app is in SMOOTH mode (default), the vendor SDK owns the full tracking-loss lifecycle:

1. **Tracking lost** -> Vendor SDK plays **collapse animation** (smoothly reduces IPD/parallax toward zero over a grace period, typically 500ms-2s)
2. **Grace period expires** -> Vendor SDK auto-switches hardware display to 2D mode
3. **Tracking resumes** -> Vendor SDK auto-switches hardware display back to 3D mode, plays **revival animation** (smoothly restores tracked IPD/parallax)
4. **During grace period**, if tracking resumes -> Vendor SDK plays revival animation directly, no 2D switch occurs

The app receives:
- Smooth, continuous eye positions throughout (no jumps)
- `isTracking` remains vendor-determined (may stay `true` during grace period, goes `false` after)
- `XrEventDataRenderingModeChangedEXT` + `XrEventDataHardwareDisplayStateChangedEXT` when vendor auto-switches 2D<->3D

The app is passive -- it does not need to call `xrRequestDisplayRenderingModeEXT` during these transitions.

### RAW mode -- developer controls everything

When the app requests RAW mode, the vendor SDK must:

1. **Never play** collapse or revival animations
2. **Never auto-switch** the display between 2D and 3D on tracking loss/recovery
3. **Immediately report** `isTracking = false` when tracking is lost (no grace period hiding)
4. **Immediately report** `isTracking = true` when tracking resumes
5. **Return unfiltered** eye positions at all times

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
bool vendor_sdk_set_smooth_mode(struct vendor_sdk *sdk, bool enable);
// enable=true: SDK plays animations + auto-switches 2D/3D (SMOOTH)
// enable=false: SDK returns raw positions, no animations, no auto-switch (RAW)
```

### Display processor integration

The display processor must:

1. **Store the active eye tracking mode** (like it now stores `view_count`)
2. **On mode change** (`xrRequestEyeTrackingModeEXT`): call vendor SDK to enable/disable smooth behavior
3. **In `get_predicted_eye_positions()`**: vendor SDK already behaves differently based on the mode setting -- no display processor post-processing needed for SMOOTH vs RAW (the SDK does it)

### Event propagation for vendor-initiated transitions (SMOOTH mode)

When the vendor SDK auto-switches 2D/3D (during SMOOTH tracking loss/recovery), the runtime must fire events to the app. This requires a **callback or polling mechanism** from the vendor SDK to the display processor:

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
| SDK has grace period + animation + auto-switch | `3` (SMOOTH \| RAW) | `0` (SMOOTH) |
| SDK has smooth filtering only, no auto-switch control | `1` (SMOOTH only) | `0` (SMOOTH) |
| SDK provides raw only (no filtering available) | `2` (RAW only) | `1` (RAW) |
| No eye tracking | `0` | N/A |

Ideally vendors support **both** modes (bits = 3), giving developers the choice.

## Non-goals

- No new OpenXR extension functions or structs -- existing API is sufficient
- No changes to `XrDisplayRenderingModeInfoEXT` -- eye tracking behavior is orthogonal to rendering mode definition
- No per-rendering-mode tracking behavior (keeps it simple; if needed later, can be added as a separate feature)

## Acceptance Criteria

- [ ] Vendor integration guide updated with SMOOTH/RAW transition contract
- [ ] `XR_EXT_display_info.h` comments updated to document auto-switch behavior per mode
- [ ] Leia SR display processors pass eye tracking mode to SDK wrapper when `xrRequestEyeTrackingModeEXT` is called
- [ ] Event propagation path exists for vendor-initiated 2D/3D switches (SMOOTH mode auto-transitions fire `XrEventDataRenderingModeChangedEXT` + `XrEventDataHardwareDisplayStateChangedEXT`)
- [ ] sim_display updated: RAW mode returns immediate `isTracking` transitions, SMOOTH mode simulates grace period (optional, for testing)
