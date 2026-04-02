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

### Tracking loss and eye position reporting

**"Tracking lost" (`isTracking = false`)** means the viewer is outside the display's supported 3D view zone — it does **not** necessarily mean the physical tracker has lost lock on the viewer. Some vendors' trackers continue following the viewer even after `isTracking` flips to `false` (e.g., the viewer stepped too far to the side but the camera still sees them). Other vendors' trackers truly lose the viewer and can only report the last known position.

**Both modes guarantee valid eye positions at all times.** When `isTracking == false`, the vendor MUST continue returning usable eye positions — never zeros or uninitialized values. The reported positions depend on vendor capability:

- **Tracker still following viewer out-of-zone:** Vendor MAY report the actual (out-of-zone) tracked positions. This allows apps to maintain a coherent 3D effect even as the viewer moves away, enabling smooth developer-controlled transitions.
- **Tracker lost viewer entirely:** Vendor MUST report the last known valid position (frozen at the point tracking was lost).

This applies equally to MANAGED and MANUAL modes — the difference between modes is what the vendor *does* with those positions (animate them vs. pass them through), not whether positions are available.

### MANAGED mode -- vendor controls everything

When the app is in MANAGED mode (default), the vendor SDK owns the full tracking-loss lifecycle:

1. **Tracking lost** -> Vendor SDK enters a **grace period** (typically 500ms-2s) and plays a **collapse animation** — smoothly animating eye positions toward the nominal viewer position (collapsing IPD/parallax toward zero). The vendor MAY also apply **shader-side animation** on weaved frames during this period (e.g., gradually reducing 3D depth).
2. **Grace period expires** -> Vendor SDK auto-switches hardware display to 2D mode
3. **Tracking resumes** -> Vendor SDK auto-switches hardware display back to 3D mode, plays **revival animation** (smoothly restores tracked IPD/parallax from nominal back to tracked positions)
4. **During grace period**, if tracking resumes -> Vendor SDK plays revival animation directly, no 2D switch occurs

The app receives:
- **Animated eye positions** throughout the grace period — these are vendor-generated values (e.g., collapsing toward nominal viewpoint), **not** the raw tracked or last-known positions. This is the key difference from MANUAL mode.
- `isTracking` remains vendor-determined (may stay `true` during grace period, goes `false` after)
- `XrEventDataRenderingModeChangedEXT` + `XrEventDataHardwareDisplayStateChangedEXT` when vendor auto-switches 2D<->3D

The app is passive -- it does not need to call `xrRequestDisplayRenderingModeEXT` during these transitions.

**Recommendation (SHOULD):** In MANAGED mode, vendors SHOULD keep `isTracking = true` throughout the grace period (while the collapse animation plays) and set `isTracking = false` only when the grace period expires and the vendor switches the display to 2D. This gives apps a consistent signal: `isTracking == false` means the vendor has fully transitioned to fallback. Vendors MAY deviate if their SDK uses a different heuristic, but the timing SHOULD align with the actual 2D switch.

### MANUAL mode -- developer controls everything

When the app requests MANUAL mode, the vendor SDK must:

1. **Never play** collapse or revival animations
2. **Never auto-switch** the display between 2D and 3D on tracking loss/recovery
3. **Immediately report** `isTracking = false` when tracking is lost (no grace period hiding)
4. **Immediately report** `isTracking = true` when tracking resumes
5. **Continue returning valid eye positions** — if the tracker is still following the viewer out-of-zone, report those actual positions; if the tracker lost the viewer, report the last known position. No animation or smoothing applied.

The app is responsible for its own strategy:
- Detect `isTracking` transition to `false`
- Use the still-valid eye positions to design its own 3D-to-2D transition (e.g., animate convergence down while the 3D effect tracks the viewer's actual movement)
- Call `xrRequestDisplayRenderingModeEXT(2D_mode)` when ready
- Detect `isTracking` transition to `true`
- Call `xrRequestDisplayRenderingModeEXT(3D_mode)` to resume
- Animate convergence/IPD back to tracked values

### What the app sees: MANAGED vs MANUAL comparison

| Aspect | MANAGED | MANUAL |
|---|---|---|
| `isTracking` timing | Delayed — stays `true` during grace period | Immediate — flips as soon as out-of-zone |
| Eye positions during transition | **Animated** by vendor (collapsing toward nominal) | **Unmodified** — actual tracked position or last known |
| Shader effects | Vendor MAY animate weaved output | None — vendor passes frames through unchanged |
| 2D/3D hardware switch | Automatic at end of grace period | Never — app calls `xrRequestDisplayRenderingModeEXT` |
| App responsibility | Passive | Full control over transition strategy |

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
