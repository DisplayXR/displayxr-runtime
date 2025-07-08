### Added
- Support for the `XR_EXT_hand_tracking_data_source` extension.

### Changed
- Differentiated the hand-tracking device role and `xrt_input_name` into two distinct types:
  - **`unobstructed`**: For standard hand-tracking where the hand is free.
  - **`conforming`**: For hand-tracking where a held object (e.g., a controller) obstructs full finger motion.
- This change allows for more flexible device configurations, supporting either a single `xrt_device` for both roles or separate devices for each.