# XR_EXT_session_target - Phase 5 Implementation Summary

## Overview

Phase 5 integrates **SR weaver's LookaroundFilter for dynamic eye positions** to replace static IPD-based camera pose calculation. This enables real-time eye tracking for accurate stereoscopic rendering based on actual user eye positions.

## Status: Complete

Eye tracking integration via weaver's `getPredictedEyePositions()`:
- Each session's SR weaver provides predicted eye positions via the LookaroundFilter
- `oxr_session_locate_views()` queries eye positions to calculate dynamic view poses
- Uses application-adaptive latency prediction for smooth LookAround
- Fallback to static IPD when eye tracking is unavailable

## Key Achievement

**Problem Solved:** Camera poses used static IPD (interpupillary distance), not actual eye positions.

**Solution:** Use the SR weaver's `getPredictedEyePositions()` API which provides latency-compensated eye positions optimized for LookAround functionality.

## Why weaver->getPredictedEyePositions()?

The SR SDK provides two ways to access eye positions:

| Method | Filter | Use Case |
|--------|--------|----------|
| ~~`EyeTracker::openEyePairStream()`~~ | Tracker-side filter via UDP | **Deprecated** - Cannot adapt to app latency |
| **`weaver->getPredictedEyePositions()`** | LookaroundFilter | **Preferred** - Adapts to app update rate |

The weaver maintains two separate prediction filters:
1. **PredictingWeaverTracker** - Uses `[WeavingPoseFilter]` params for internal weaving
2. **PredictingEyeTracker** - Uses `[LookaroundFilter]` params for `getPredictedEyePositions()`

Both filters consume raw eye data (no double-filtering) and adapt latency prediction to your actual monitor refresh rate and application update rate.

## Architecture

### Eye Position Flow

```
┌──────────────────────────────────────────────────────────────────────┐
│                       Per-Session Weaver                              │
│                                                                      │
│   multi_compositor::session_render::weaver                           │
│       └─► weaver->getPredictedEyePositions(leftEye, rightEye)        │
│           └─► Uses LookaroundFilter                                  │
│           └─► Returns positions in millimeters                       │
└──────────────────────────────────────────────────────────────────────┘
                                    │
                    Query on xrLocateViews call
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│                    oxr_session_locate_views()                         │
│                                                                      │
│   1. Get positions: oxr_session_get_predicted_eye_positions()        │
│      └─► multi_compositor_get_predicted_eye_positions()              │
│          └─► leiasr_get_predicted_eye_positions()                    │
│              └─► weaver->getPredictedEyePositions()                  │
│   2. Convert mm → meters (divide by 1000)                            │
│   3. Calculate eye relation: right_eye - left_eye                    │
│   4. Pass to xrt_device_get_view_poses()                             │
└──────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│                      View Pose Calculation                            │
│                                                                      │
│   Before (static IPD):                                               │
│     Left camera:  head_pose + (-IPD/2, 0, 0)                         │
│     Right camera: head_pose + (+IPD/2, 0, 0)                         │
│                                                                      │
│   After (dynamic eye tracking):                                      │
│     eye_relation = (right.x - left.x, right.y - left.y, right.z - left.z)
│     Left camera:  head_pose + (-eye_relation/2)                      │
│     Right camera: head_pose + (+eye_relation/2)                      │
└──────────────────────────────────────────────────────────────────────┘
```

### Per-Session Weaver Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                   multi_system_compositor                            │
│                                                                     │
│         ┌────────────────────┬────────────────────┐                 │
│         ▼                    ▼                    ▼                 │
│   ┌───────────┐        ┌───────────┐        ┌───────────┐          │
│   │ Session A │        │ Session B │        │ Session C │          │
│   │           │        │           │        │           │          │
│   │ weaver A  │        │ weaver B  │        │ weaver C  │          │
│   │   ↓       │        │   ↓       │        │   ↓       │          │
│   │ getPre-   │        │ getPre-   │        │ getPre-   │          │
│   │ dictedEye │        │ dictedEye │        │ dictedEye │          │
│   │ Positions │        │ Positions │        │ Positions │          │
│   └───────────┘        └───────────┘        └───────────┘          │
│                                                                     │
│   Each session with per-session rendering has its own weaver        │
│   and can query eye positions from it.                              │
└─────────────────────────────────────────────────────────────────────┘
```

## Files Modified

| File | Change |
|------|--------|
| `src/xrt/drivers/leiasr/leiasr.h` | Added `leiasr_get_predicted_eye_positions()` and `leiasr_has_weaver()` |
| `src/xrt/drivers/leiasr/leiasr.cpp` | Implemented weaver-based eye position query |
| `src/xrt/compositor/multi/comp_multi_private.h` | Added `multi_compositor_get_predicted_eye_positions()` |
| `src/xrt/compositor/multi/comp_multi_compositor.c` | Implemented accessor to get eye positions from session weaver |
| `src/xrt/state_trackers/oxr/oxr_session.c` | Query eye positions using new weaver-based API |

## Implementation Details

### Eye Position Structures

```c
// Eye position in meters (converted from SR's millimeters)
struct leiasr_eye_position {
    float x;  // Horizontal position (positive = right)
    float y;  // Vertical position (positive = up)
    float z;  // Depth position (positive = toward viewer)
};

// Eye pair with both positions
struct leiasr_eye_pair {
    struct leiasr_eye_position left;
    struct leiasr_eye_position right;
    int64_t timestamp_ns;
    bool valid;
};
```

### Weaver-Based Eye Position Query

```cpp
// In leiasr.cpp:
bool
leiasr_get_predicted_eye_positions(struct leiasr *leiasr, struct leiasr_eye_pair *out_eye_pair)
{
    if (leiasr == nullptr || leiasr->weaver == nullptr) {
        return false;
    }

    // Get predicted eye positions from weaver's LookaroundFilter
    // The weaver returns positions in millimeters
    float leftEye[3], rightEye[3];
    leiasr->weaver->getPredictedEyePositions(leftEye, rightEye);

    // Convert from millimeters to meters
    out_eye_pair->left.x = leftEye[0] / 1000.0f;
    out_eye_pair->left.y = leftEye[1] / 1000.0f;
    out_eye_pair->left.z = leftEye[2] / 1000.0f;
    out_eye_pair->right.x = rightEye[0] / 1000.0f;
    out_eye_pair->right.y = rightEye[1] / 1000.0f;
    out_eye_pair->right.z = rightEye[2] / 1000.0f;
    out_eye_pair->timestamp_ns = os_monotonic_get_ns();
    out_eye_pair->valid = true;

    return true;
}
```

### View Pose Integration

```c
// In oxr_session_locate_views():
struct xrt_vec3 default_eye_relation = {
    sess->ipd_meters,
    0.0f,
    0.0f,
};

#ifdef XRT_HAVE_LEIA_SR
// Try to get dynamic eye positions from the session's SR weaver
// Uses the weaver's LookaroundFilter which adapts to application-specific latency
{
    struct leiasr_eye_pair eye_pair;
    if (oxr_session_get_predicted_eye_positions(sess, &eye_pair)) {
        // Calculate eye relation as vector from left to right eye
        default_eye_relation.x = eye_pair.right.x - eye_pair.left.x;
        default_eye_relation.y = eye_pair.right.y - eye_pair.left.y;
        default_eye_relation.z = eye_pair.right.z - eye_pair.left.z;
    }
}
#endif
```

## Key Design Decisions

1. **Per-Session Weaver:** Each session with per-session rendering (XR_EXT_session_target) has its own weaver, and eye positions are queried from that weaver

2. **LookaroundFilter:** Uses the weaver's `getPredictedEyePositions()` which uses the LookaroundFilter tuned for application update rate (NOT the deprecated EyeTracker stream)

3. **No Extra Library:** Doesn't require the `SimulatedRealitySense.lib` since eye positions come from the weaver which is always available

4. **Unit Conversion:** SR SDK returns positions in millimeters; convert to meters by dividing by 1000

5. **Fallback:** If session doesn't have a weaver (no per-session rendering) or eye positions unavailable, fall back to static IPD

6. **Integration Point:** Session level (`oxr_session_locate_views()`) ensures consistent poses for both `xrLocateViews` and compositor rendering

## Testing

### Verification Steps

1. **Build verification:**
   ```bash
   cmake --build build --config Release
   ```

2. **Runtime verification:**
   - Session must have per-session rendering enabled (external window handle)
   - Move head/eyes in front of SR display
   - Verify camera poses update dynamically with eye movement
   - Sessions without per-session rendering use static IPD

### Expected Test Results

- Sessions with per-session rendering get dynamic eye positions
- Eye positions update continuously via weaver's LookaroundFilter
- View poses reflect actual eye positions
- Fallback to IPD for sessions without per-session rendering
- Smooth, latency-compensated eye tracking for LookAround

## Complete Implementation Status

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | Single app with external HWND | Complete |
| Phase 2 | Per-session infrastructure | Complete |
| Phase 3 | Per-session target/weaver creation | Complete |
| Phase 4 | Per-session render pipeline | Complete |
| **Phase 5** | **SR weaver eye tracking (LookaroundFilter)** | **Complete** |

## Future Improvements

1. **Eye position prediction:** Use timestamp to predict eye positions at display time
2. **Vergence tracking:** Incorporate eye vergence for focus depth estimation
3. **Head pose fusion:** Combine eye tracking with head tracking for full 6DOF
4. **Multi-user support:** Handle multiple users detected by eye tracker
5. **Fallback for shared rendering:** Support eye tracking for sessions without per-session rendering

## References

- [phase-1.md](phase-1.md) - Single app external HWND implementation
- [phase-2.md](phase-2.md) - Per-session infrastructure and data structures
- [phase-3.md](phase-3.md) - Per-session target/weaver creation with service pattern
- [phase-4.md](phase-4.md) - Per-session render pipeline
- [SR SDK Documentation](https://developer.dimenco.eu/) - Leia SR SDK reference
