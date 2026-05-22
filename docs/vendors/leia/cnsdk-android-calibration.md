# CNSDK Android Calibration Procedure

The Android POC's CNSDK display processor makes three assumptions about
CNSDK conventions that the SDK docs leave ambiguous. Once a Lume Pad is
attached, validate each in this order — they're listed cheapest-to-test
first. Audit references in brackets.

## 1. Face position axis convention [B15]

**What's assumed:** in `src/xrt/drivers/leia/leia_cnsdk.cpp::leia_cnsdk_get_primary_face`,
CNSDK's `leia_core_get_primary_face` returns a 3-float position in
millimeters with axes x=right / y=up / z=toward viewer, origin at the
camera. The wrapper converts mm→m and subtracts the cached
`cameraCenterX/Y/Z` to land in `xrt_eye_position`'s display-relative
frame.

**What CNSDK actually documents** (cnsdk/include/leia/headTracking/common/types.h):
- `leia_headtracking_detected_face.posePosition` is "Head location in
  mm. The origin point is the location of the camera." ✓ matches.
- `leia_headtracking_face.point.pos` is "3D coordinate with camera
  transform and Kalman filter applied." Axis convention not stated.
- `posePoseAngle` is "in radians. **The rotation is a left handed
  coordinate system.**" Strong hint that positions may also follow a
  left-handed system.

**Test:** stand directly in front of the display, motionless. Note the
last reported eye position in logcat (look for `xrLocateViews` HUD
output or any debug printf you add). Then move your head 10 cm
**right**. Re-read.

| Result | What it means | Fix |
|---|---|---|
| x increased by ~0.10 | Right-handed, x=right. Convention matches. ✓ | — |
| x decreased by ~0.10 | Left-handed, x=right OR right-handed with x=left | Flip sign of `out_x` in `leia_cnsdk_get_primary_face` |
| y changed by ~0.10 | Axes are rotated; CNSDK uses landscape vs portrait orientation differently | Swap x/y, possibly negate one |

Repeat for **down** (y) and **toward display** (z).

If the cube appears to track the wrong direction, this is the first
thing to check.

## 2. Tile-to-eye mapping [B17]

**What's assumed:** the SBS atlas's tile (col 0, row 0) is the **left
eye** view and (col 1, row 0) is the **right eye**. The DP's
`blit_atlas_to_per_view` blits tile 0 → `view_img[0]`, tile 1 →
`view_img[1]`. `leia_cnsdk_weave` then calls
`leia_interlacer_vulkan_set_view_for_texture_array(interlacer, 0, view_iv[0])`
and `(interlacer, 1, view_iv[1])` — passing left as view-0, right as
view-1.

**What CNSDK actually does:** undocumented. CNSDK's interlacer treats
view 0 and view 1 as left and right per common convention, but this
isn't explicit in the headers.

**Test:** display the [Khronos OpenXR cube test pattern](https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/src/tests/hello_xr)
or any stereo-pair where left and right eyes have visually distinct
content (e.g., a number "1" rendered to left view, "2" to right). Look
at the Lume Pad with one eye covered, then the other.

| What you see | Meaning | Fix |
|---|---|---|
| "1" with left eye, "2" with right | Convention matches. ✓ | — |
| "2" with left eye, "1" with right | Tile-to-eye mapping is swapped | In `leia_cnsdk_weave`, swap the args to `set_view_for_texture_array` (pass right as 0, left as 1). Or in the DP, swap the destination index in `blit_atlas_to_per_view`. |
| Both eyes see "1" and "2" overlaid as a ghost | Interlacing pattern broken — likely a calibration issue, not view assignment | Check `leia_core_get_device_config` is returning the right `displayDots` — out of scope of this doc |

## 3. UV vertical flip [B18]

**What's assumed:** `leia_cnsdk.cpp::leia_cnsdk_weave` calls
`leia_interlacer_set_flip_input_uv_vertical(interlacer, true)` because
Vulkan's NDC is Y-down (origin at top-left) and CNSDK is presumed to
default to OpenGL convention (Y-up, origin bottom-left).

**Test:** display any content with clear top/bottom asymmetry — text
is ideal. A rendered string "HELLO" or just a single capital "A" works.

| What you see | Meaning | Fix |
|---|---|---|
| Right-side-up "HELLO" | UV flip convention is correct ✓ | — |
| Upside-down "OLLEH" / "HELLO" vertically mirrored | CNSDK is already flipping; we're double-flipping | Change `leia_interlacer_set_flip_input_uv_vertical(interlacer, true)` to `false` in `leia_cnsdk.cpp::leia_cnsdk_weave` |
| Letters look correct but offset / cropped | Different bug — not a flip issue. Check view_width / view_height accounting | Out of scope of this doc |

## Combined check

Once all three are pinned down, the cube test app (when it lands —
B13d) should:
1. Render a spinning cube at the depth corresponding to the wearer's
   head distance.
2. Move convincingly with head motion (left-right strafing).
3. Have correct horizontal disparity (look closer with one eye than the
   other).
4. Show right-side-up text/numbers on debug overlays.

If 1+3 are off, it's #1 (face axes). If 2 is off, it's #1 or #2 (eye
mapping). If 4 is off, it's #3 (UV flip).

## Where the convention assumptions live

| Assumption | File | Function | Audit ref |
|---|---|---|---|
| Face position axes / units | `src/xrt/drivers/leia/leia_cnsdk.cpp` | `leia_cnsdk_get_primary_face` | B15 |
| Tile-to-eye mapping | `src/xrt/drivers/leia/leia_cnsdk.cpp` | `leia_cnsdk_weave` (`set_view_for_texture_array`) | B17 |
| UV vertical flip | `src/xrt/drivers/leia/leia_cnsdk.cpp` | `leia_cnsdk_weave` (`set_flip_input_uv_vertical`) | B18 |

All three are 1–3 line changes once you know the correct values.

## Why these aren't fixed pre-emptively

Each is a 50/50 coin flip in either direction. Picking wrong is worse
than picking right (off-by-180° on axes is more confusing than
matching the spec assumption). Lume Pad on-device test is the
definitive answer; this doc lets you arrive at hardware ready to flip
the right knob.
