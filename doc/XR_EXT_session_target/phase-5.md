# XR_EXT_session_target - Phase 5 Implementation Summary

## Overview

Phase 5 integrates **SR EyeTracker for dynamic eye positions** to replace static IPD-based camera pose calculation. This enables real-time eye tracking for accurate stereoscopic rendering based on actual user eye positions.

## Status: Complete

Eye tracking integration is now functional:
- SR EyeTracker creates and starts at system compositor initialization
- Eye positions are continuously updated via callback stream
- `oxr_session_locate_views()` queries eye positions to calculate dynamic view poses
- Fallback to static IPD when eye tracking is unavailable

## Key Achievement

**Problem Solved:** Camera poses used static IPD (interpupillary distance), not actual eye positions.

**Solution:** Integrate SR::EyeTracker API to get real-time left/right eye positions and use them to calculate dynamic view poses.

## Architecture

### Eye Position Flow

```
┌──────────────────────────────────────────────────────────────────────┐
│                    System Compositor Initialization                   │
│                                                                      │
│   comp_multi_create_system_compositor()                              │
│       └─► leiasr_create_eye_tracker_only()                           │
│           └─► leiasr_eye_tracker_start()                             │
│               └─► SR::EyeTracker::openEyePairStream()                │
└──────────────────────────────────────────────────────────────────────┘
                                    │
                    Continuous eye position updates
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│                     LeiaEyePairListener::accept()                     │
│                                                                      │
│   Called by SR SDK on eye tracking thread:                           │
│   1. Convert mm → meters (divide by 1000)                            │
│   2. Store in thread-safe latestEyePair                              │
│   3. Update timestamp                                                │
└──────────────────────────────────────────────────────────────────────┘
                                    │
                    Query on xrLocateViews call
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│                    oxr_session_locate_views()                         │
│                                                                      │
│   1. Get eye tracker: oxr_session_get_eye_tracker(sess)              │
│   2. Query positions: leiasr_get_eye_positions(tracker, &pair)       │
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

### Shared Eye Tracker Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                   multi_system_compositor                            │
│                                                                     │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │  eye_tracker (struct leiasr *)                               │   │
│   │    └─► Shared by all sessions                                │   │
│   │    └─► One user = one pair of eyes                           │   │
│   │    └─► Created at system init, destroyed at system shutdown  │   │
│   └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│         ┌────────────────────┼────────────────────┐                 │
│         ▼                    ▼                    ▼                 │
│   ┌───────────┐        ┌───────────┐        ┌───────────┐          │
│   │ Session A │        │ Session B │        │ Session C │          │
│   │           │        │           │        │           │          │
│   │ Accesses  │        │ Accesses  │        │ Accesses  │          │
│   │ shared    │        │ shared    │        │ shared    │          │
│   │ eye_tracker│       │ eye_tracker│       │ eye_tracker│         │
│   └───────────┘        └───────────┘        └───────────┘          │
└─────────────────────────────────────────────────────────────────────┘
```

## Files Modified

| File | Change |
|------|--------|
| `src/xrt/drivers/leiasr/leiasr.h` | Added eye tracking structures and API functions |
| `src/xrt/drivers/leiasr/leiasr.cpp` | Implemented EyePairListener, eye tracker lifecycle, thread-safe storage |
| `src/xrt/compositor/multi/comp_multi_private.h` | Added `eye_tracker` pointer to `multi_system_compositor` |
| `src/xrt/compositor/multi/comp_multi_system.c` | Create/destroy shared eye tracker at system level |
| `src/xrt/compositor/multi/comp_multi_compositor.c` | Added `multi_compositor_get_eye_tracker()` accessor |
| `src/xrt/state_trackers/oxr/oxr_session.c` | Query eye positions and use for `default_eye_relation` |
| `src/xrt/drivers/CMakeLists.txt` | Link `SimulatedRealitySense.lib` |

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

### EyePairListener Implementation

```cpp
class LeiaEyePairListener : public SR::EyePairListener {
public:
    void accept(const SR_eyePair& eyePair) override {
        // Convert mm to meters
        leiasr_eye_pair pair;
        pair.left.x = eyePair.left.x / 1000.0f;
        pair.left.y = eyePair.left.y / 1000.0f;
        pair.left.z = eyePair.left.z / 1000.0f;
        pair.right.x = eyePair.right.x / 1000.0f;
        pair.right.y = eyePair.right.y / 1000.0f;
        pair.right.z = eyePair.right.z / 1000.0f;
        pair.timestamp_ns = os_monotonic_get_ns();
        pair.valid = true;

        owner_->updateEyePositions(pair);  // Thread-safe update
    }
};
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
struct leiasr *eye_tracker = oxr_session_get_eye_tracker(sess);
if (eye_tracker != NULL) {
    struct leiasr_eye_pair eye_pair;
    if (leiasr_get_eye_positions(eye_tracker, &eye_pair)) {
        // Calculate eye relation as vector from left to right eye
        default_eye_relation.x = eye_pair.right.x - eye_pair.left.x;
        default_eye_relation.y = eye_pair.right.y - eye_pair.left.y;
        default_eye_relation.z = eye_pair.right.z - eye_pair.left.z;
    }
}
#endif
```

## Key Design Decisions

1. **Shared Eye Tracker:** Single eye tracker instance at `multi_system_compositor` level, shared by all sessions (one user = one pair of eyes)

2. **Thread Safety:** Eye tracker callback runs on SR thread; use `std::mutex` for `latestEyePair` storage

3. **Unit Conversion:** SR SDK returns positions in millimeters; convert to meters by dividing by 1000

4. **API Choice:** Use `EyeTracker::openEyePairStream()` with `EyePairListener` (NOT `getPredictedEyePositions()` which is tuned for weaving latency)

5. **Separate SR Context:** Create dedicated `leiasr` instance for eye tracking (via `leiasr_create_eye_tracker_only()`), separate from per-session weavers

6. **Fallback:** If eye tracking unavailable or invalid, fall back to static IPD

7. **Integration Point:** Session level (`oxr_session_locate_views()`) ensures consistent poses for both `xrLocateViews` and compositor rendering

## Testing

### Verification Steps

1. **Build verification:**
   ```bash
   cmake --build build --config Release
   ```

2. **Log verification:**
   - System startup: "Created and started shared eye tracker"
   - Session queries: Eye positions used for view pose calculation
   - Shutdown: "Destroyed shared eye tracker"

3. **Runtime verification:**
   - Move head/eyes in front of SR display
   - Verify camera poses update dynamically
   - Cover eye tracker sensor - should revert to static IPD

### Expected Test Results

- Eye tracker starts at system compositor creation
- Eye positions update continuously via callback
- View poses reflect actual eye positions
- Fallback to IPD when eye tracking fails
- Clean shutdown of eye tracker

## Complete Implementation Status

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | Single app with external HWND | Complete |
| Phase 2 | Per-session infrastructure | Complete |
| Phase 3 | Per-session target/weaver creation | Complete |
| Phase 4 | Per-session render pipeline | Complete |
| **Phase 5** | **SR EyeTracker integration** | **Complete** |

## Future Improvements

1. **Eye position prediction:** Use timestamp to predict eye positions at display time
2. **Vergence tracking:** Incorporate eye vergence for focus depth estimation
3. **Head pose fusion:** Combine eye tracking with head tracking for full 6DOF
4. **Multi-user support:** Handle multiple users detected by eye tracker
5. **Performance optimization:** Reduce lock contention with lock-free queues

## References

- [phase-1.md](phase-1.md) - Single app external HWND implementation
- [phase-2.md](phase-2.md) - Per-session infrastructure and data structures
- [phase-3.md](phase-3.md) - Per-session target/weaver creation with service pattern
- [phase-4.md](phase-4.md) - Per-session render pipeline
- [SR SDK Documentation](https://developer.dimenco.eu/) - Leia SR SDK reference
