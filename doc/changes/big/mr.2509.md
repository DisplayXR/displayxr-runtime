---
- mr.2521
- mr.2522
- mr.2523
---
### Added
- Support for the `XR_EXT_hand_tracking_data_source` extension.
- Added `OXR_HAND_TRACKING_PRIORITIZE_CONFORMING` debug environment variable. When enabled, prioritizes the conforming hand-tracking source over the unobstructed source when both are active. This is useful for debugging or forcing controller-based hand poses.

### Changed
- Differentiated the hand-tracking device role and `xrt_input_name` into two distinct types:
  - **`unobstructed`**: For standard hand-tracking where the hand is free.
  - **`conforming`**: For hand-tracking where a held object (e.g., a controller) obstructs full finger motion.
- This change allows for more flexible device configurations, supporting either a single `xrt_device` for both roles or separate devices for each.
