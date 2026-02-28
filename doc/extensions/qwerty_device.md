# Qwerty Device Driver

| Property | Value |
|----------|-------|
| Driver Name | `drv_qwerty` |
| Platform | Cross-platform (SDL GUI), Windows (Win32 native) |
| Devices | HMD, Left Controller, Right Controller |
| Author | Mateo de Mayo (original), David Fattal (Win32 backend) |

---

## 1. Overview

The qwerty driver provides simulated HMD and controller devices that are controlled entirely via keyboard and mouse input. It allows developers to test OpenXR applications without physical VR hardware by moving a virtual headset and two virtual controllers through keyboard commands and mouse look.

**Note:** The qwerty device is a pure input device — it captures keyboard/mouse input on the Monado-owned window and translates it into HMD pose + controller state. It does not produce any display output. The qwerty builder uses `maybe.head` (last-resort fallback), not `certain.head` — display builders (Leia, sim_display) always win via `certain.head`. Display builders get qwerty input devices via the shared helper `t_builder_add_qwerty_input()`.

The driver has three input backends:

- **SDL backend** (`qwerty_sdl.c`) — Uses the Monado debug GUI window. Cross-platform.
- **Win32 backend** (`qwerty_win32.c`) — Uses the D3D11 compositor window directly. Windows only. Active when Monado creates its own window (no HWND provided via `XR_EXT_win32_window_binding`).
- **macOS backend** (`qwerty_macos.m`) — Uses the Vulkan compositor NSWindow directly. macOS only.

---

## 2. Architecture

### 2.1 Device Hierarchy

```
qwerty_system
├── qwerty_hmd         (optional, NULL if no HMD role assigned)
│   └── qwerty_device
│       └── xrt_device (XRT_DEVICE_GENERIC_HMD)
├── qwerty_controller  (left, always present)
│   └── qwerty_device
│       └── xrt_device (XRT_DEVICE_WMR_CONTROLLER)
└── qwerty_controller  (right, always present)
    └── qwerty_device
        └── xrt_device (XRT_DEVICE_WMR_CONTROLLER)
```

`qwerty_system` is the top-level container that owns all three devices and shared state (log level, `process_keys` toggle, focus tracking). Each `qwerty_device` holds an internal `xrt_pose` that is updated each frame based on accumulated keyboard and mouse input.

### 2.2 Input Flow

```
Win32 Window Message / SDL Event
        |
        v
qwerty_process_win32() / SDL event handler
        |
        v
qwerty_press_*() / qwerty_release_*()     (set boolean flags)
qwerty_add_look_delta()                     (accumulate yaw/pitch)
qwerty_add_position_delta()                 (accumulate XY translation)
        |
        v
qwerty_get_tracked_pose()                   (consume deltas, update pose)
        |
        v
xrt_space_relation                          (returned to OpenXR runtime)
```

Input methods set boolean flags or accumulate float deltas. These are consumed and reset to zero in `qwerty_get_tracked_pose()`, which the runtime calls each frame.

### 2.3 Controller Emulation

Controllers emulate `XRT_DEVICE_WMR_CONTROLLER` with binding profiles for WMR, Oculus Touch, Valve Index, HTC Vive, and Simple Controller interaction profiles. This means qwerty controllers work with any OpenXR application regardless of which interaction profile it requests.

Emulated inputs:

| Index | Input | Type |
|-------|-------|------|
| 0 | Trigger | float (0.0 or 1.0) |
| 1 | Menu | boolean |
| 2 | Squeeze | boolean |
| 3 | System/Home | boolean |
| 4 | Thumbstick | vec2 (-1/0/+1 per axis) |
| 5 | Thumbstick Click | boolean |
| 6 | Trackpad | vec2 (-1/0/+1 per axis) |
| 7 | Trackpad Touch | boolean (auto from direction) |
| 8 | Trackpad Click | boolean |
| 9 | Grip Pose | pose |
| 10 | Aim Pose | pose |

---

## 3. Pose Mechanics

### 3.1 Movement (WASD + QE)

Position is computed per-frame in `qwerty_get_tracked_pose()`:

```
pos_delta = {
    movement_speed * (right - left),       // X: strafe
    0,                                      // Y: handled separately
    movement_speed * (backward - forward),  // Z: forward/back
}
rotate(pos_delta, device_orientation)        // WASD moves relative to facing direction
pos_delta.y += movement_speed * (up - down)  // QE moves in world Y (always vertical)

pose.position += pos_delta
```

Sprint (SHIFT) multiplies `movement_speed` by `1.25^5 = ~3.05x`.

### 3.2 Rotation (Mouse Look / Arrow Keys)

Rotation accumulates yaw and pitch:

```
yaw_speed   = look_speed * (look_left - look_right) + yaw_delta
pitch_speed = look_speed * (look_up - look_down) + pitch_delta

pitch: local-space rotation around X axis (device tilts up/down)
yaw:   base-space rotation around Y axis (device turns left/right, always upright)
```

Yaw in base-space prevents gimbal lock and keeps the horizon level.

### 3.3 Mouse XY Translation (Controllers Only)

When a controller is focused (CTRL or ALT held) and RMB is not held, mouse movement translates the controller in world-space XY:

```
pose.position.x += dx * POSITION_SENSITIVITY * movement_speed
pose.position.y += -dy * POSITION_SENSITIVITY * movement_speed
                    ^-- negated: screen Y-down, world Y-up
```

This is world-space (not device-relative), so horizontal mouse movement always moves the controller horizontally regardless of its orientation.

### 3.4 HMD Parenting

Controllers can be "parented" to the HMD (toggled with `C`). When `follow_hmd` is true, the controller's pose is stored relative to the HMD, and the final pose is computed by composing: `controller_pose * hmd_pose`. This makes controllers move with the head by default.

When toggling follow mode, the controller's pose is transformed between global and HMD-relative coordinates so it stays in place visually.

### 3.5 Default Positions

| Device | Initial Position (meters) |
|--------|--------------------------|
| HMD | (0, 1.6, 0) — camera mode default (viewer at eye height) |
| Left Controller | (-0.2, -0.3, -0.5) relative to HMD |
| Right Controller | (0.2, -0.3, -0.5) relative to HMD |

Default startup is camera mode at (0, 1.6, 0). Display mode position is derived when toggling with P.

### 3.6 Movement Speed

| Device | Initial Speed | Units |
|--------|---------------|-------|
| HMD | 0.002 | meters/frame |
| Controller | 0.005 | meters/frame |

Speed is adjusted by mouse wheel or numpad +/- in exponential steps of 1.25x.

---

## 4. Stereo Controls

The qwerty driver supports two stereo projection modes: **Camera-centric** and **Display-centric**, toggled by the **P** key.

### 4.1 Dual State Model

Each mode has its own independent variable set:

| Variable | Camera Mode | Display Mode |
|----------|-------------|-------------|
| IPD factor | `cam_ipd_factor` [0.01, 1] default 1.0 | `disp_ipd_factor` [0.01, 1] default 1.0 |
| Parallax factor | `cam_parallax_factor` (= ipd always) | `disp_parallax_factor` (= ipd always) |
| Convergence | `cam_convergence` [0, 2] diopters, default 0.5 | — |
| Half-tan vFOV | `cam_half_tan_vfov` default 0.3249 (derived only) | — |
| Virtual height | — | `disp_vHeight` [0.1, 10] meters, default 1.3 |
| Perspective | derived: `screen_h / (2 * viewer_z * half_tan_vfov)` | always 1.0 |

IPD and parallax are always equal within each mode. SHIFT+wheel controls both simultaneously.

### 4.2 Controls

| Input | Camera Mode | Display Mode |
|-------|-------------|-------------|
| Wheel (HMD focused) | Convergence ±0.05, clamp [0, 2] | vHeight ×/÷ 1.05, clamp [0.1, 10] |
| SHIFT + Wheel (HMD) | IPD+Parallax ×/÷ 1.1, clamp [0.01, 1] | IPD+Parallax ×/÷ 1.1, clamp [0.01, 1] |
| P | Toggle mode (derive target state) | Toggle mode (derive target state) |
| Spacebar | Reset all to camera defaults | Reset all to camera defaults |
| V (HMD focused) | 2D/3D display mode toggle | 2D/3D display mode toggle |

Controller-focused wheel remains movement speed (unchanged).

### 4.3 Mode Toggle Derivation

When pressing P, the target mode's variables are derived from the current state for seamless continuity — no values are reset.

**Camera → Display:**
```
fwd = rotate((0,0,-1), orientation)
conv_dist = (convergence > 0.001) ? 1/convergence : 1000
disp_pos = cam_pos + fwd * conv_dist
disp_vHeight = clamp(2 * half_tan_vfov * conv_dist, 0.1, 10)
disp_ipd, disp_parallax: kept from previous display state
```

**Display → Camera:**
```
m2v = disp_vHeight / screen_height_m
cam_distance = nominal_viewer_z * m2v    (perspective=1)
cam_pos = disp_pos - fwd * cam_distance
cam_convergence = clamp(1/cam_distance, 0, 2)
cam_half_tan_vfov = disp_vHeight / (2 * cam_distance)
cam_ipd, cam_parallax: kept from previous camera state
```

### 4.4 HUD Display

Camera mode:
```
Camera [P]  IPD/Prlx:0.750 [Sh+Wh]
Conv:0.50 dp [Wh]  vFOV:36.0  Persp*:1.67
```

Display mode:
```
Display [P]  IPD/Prlx:1.000 [Sh+Wh]
vH:1.30m [Wh]
```

### 4.5 Hardware Config

The builder sets `nominal_viewer_z` and `screen_height_m` from the display device (e.g., sim_display defaults: 0.60m viewer distance, 0.194m screen height). These are used for display→camera derivation and derived perspective calculation.

---

## 5. Builder Integration

### 5.1 Shared Helper

Display builders (Leia, sim_display, future vendors) add qwerty keyboard/mouse
input to their system by calling `t_builder_add_qwerty_input()`:

```c
#include "target_builder_qwerty_input.h"

// In open_system_impl, after creating head:
struct xrt_device *qwerty_hmd = NULL;
t_builder_add_qwerty_input(xsysd, ubrh, U_LOGGING_INFO, &qwerty_hmd);
```

The helper creates the qwerty HMD + left/right controllers, adds them to the
device list, and assigns controllers to left/right roles. It is guarded by
`XRT_BUILD_DRIVER_QWERTY` and is a no-op when qwerty is not built.

### 5.2 Qwerty Builder as Fallback

The qwerty builder itself uses `maybe.head` (not `certain.head`). This means it
only wins when no display builder claims `certain.head` — serving as a last-resort
fallback for development when no real display is available.

| Builder | Head Claim | Priority | Use Case |
|---------|-----------|----------|----------|
| leia | `certain.head` | -15 | SR SDK hardware detected |
| sim_display | `certain.head` | -20 | `SIM_DISPLAY_ENABLE=1` |
| qwerty | `maybe.head` | -25 | No display builder active (fallback) |

### 5.3 Pose Delegation

The `out_qwerty_hmd` parameter enables pose delegation: the display builder's
head device stays as the HMD role (keeping display properties, Kooima FOV, and
display processor), but delegates its pose to the qwerty HMD for WASD/mouse
camera control.

- **sim_display** uses pose delegation: calls `sim_display_hmd_set_pose_source()`
  to make the sim_display HMD track the qwerty HMD's position/orientation.
- **Leia** does not use pose delegation (passes `NULL`): SR SDK eye tracking
  provides the head pose directly.

---

## 6. Win32 Input Reference

### 6.1 Device Focus

| Modifier | Focused Device | Notes |
|----------|---------------|-------|
| None | HMD (if present) or right controller | Default |
| CTRL | Left controller | Hold to focus |
| ALT | Right controller | Hold to focus |
| CTRL + ALT | Both controllers | Actions apply to both simultaneously |

Focus is determined by `GetAsyncKeyState()` for reliability (avoids stuck keys from missed key-up events). Releasing a modifier calls `qwerty_release_all()` on all devices and resets the mouse baseline to prevent position jumps.

### 6.2 Keyboard Controls

| Key | Action | Scope |
|-----|--------|-------|
| W / A / S / D | Move forward / left / backward / right | Focused device |
| Q / E | Move down / up | Focused device |
| Arrow keys | Rotate (look) | Focused device |
| SHIFT | Sprint (3x speed boost) | Focused device |
| Numpad +/- | Increase/decrease movement speed | Focused device |
| R | Reset controller pose | Both (no mod) or focused (with mod) |
| C | Toggle HMD parenting | Both (no mod) or focused (with mod) |
| N | Menu button | Focused controller |
| B | System button | Focused controller |
| P | Toggle camera/display stereo mode | HMD focused |
| Spacebar | Reset stereo to camera defaults | HMD focused |
| V | 2D/3D display toggle (HMD) / Thumbstick click (controller) | Focused device |
| T / F / G / H | Thumbstick up/left/down/right | Focused controller |
| I / J / K / L | Trackpad up/left/down/right | Focused controller |
| M | Trackpad click | Focused controller |
| ESC | Close window | Global |

### 6.3 Mouse Controls

| Action | No Modifier (HMD) | CTRL (Left) | ALT (Right) | CTRL+ALT (Both) |
|--------|-------------------|-------------|-------------|------------------|
| Move | -- | Translate XY | Translate XY | Translate both XY |
| RMB + Drag | Rotate HMD | Rotate controller | Rotate controller | Rotate both |
| LMB Click | Trigger | Trigger left | Trigger right | Trigger both |
| MMB Click | Squeeze | Squeeze left | Squeeze right | Squeeze both |
| Wheel | Convergence/vHeight (see §4) | Speed +/- | Speed +/- | Speed +/- both |
| SHIFT + Wheel | IPD+Parallax ×/÷ 1.1 | — | — | — |

### 6.4 Sensitivity Constants

| Constant | Value | Effect |
|----------|-------|--------|
| `SENSITIVITY` | 0.1 | `look_speed` units per pixel of mouse movement |
| `POSITION_SENSITIVITY` | 0.2 | `movement_speed` units per pixel of mouse movement |

Effective rotation: `0.1 * 0.02 = 0.002 rad/px` for HMD, `0.1 * 0.05 = 0.005 rad/px` for controllers.

Effective translation: `0.2 * 0.005 = 0.001 m/px` for controllers at default speed.

---

## 7. Touchpad Fallback (Palm Rejection Workaround)

### 7.1 Problem

Windows Precision Touchpad drivers suppress `WM_LBUTTONDOWN` and `WM_LBUTTONUP` messages when system modifier keys (CTRL, ALT) are held. This is a palm rejection feature. Since the qwerty driver uses CTRL/ALT for controller focus, laptop trackpad taps/clicks do not fire trigger or squeeze actions.

### 7.2 Solution

`WM_MOUSEMOVE` messages are delivered even with modifiers held, and their `wParam` contains `MK_LBUTTON` and `MK_MBUTTON` flags indicating the current button state. The Win32 handler detects button state transitions in `WM_MOUSEMOVE` as a fallback:

```c
bool lmb_down = (wParam & MK_LBUTTON) != 0;
if (lmb_down != lmb_was_down) {
    // Fire trigger press/release
    lmb_was_down = lmb_down;
}
```

The `lmb_was_down` / `mmb_was_down` trackers are also synced in the regular `WM_LBUTTONDOWN`/`UP` and `WM_MBUTTONDOWN`/`UP` handlers, so both code paths stay consistent.

### 7.3 Why Not Use Regular Keys Like SRHydra?

SRHydra uses regular letter keys (F/G) for controller focus instead of CTRL/ALT, avoiding the palm rejection issue entirely. However, regular letter keys would conflict with the existing WASD movement and controller button bindings. CTRL and ALT are conventional modifier keys that don't generate character input and are intuitively understood as "hold to modify behavior." The `wParam` fallback preserves this ergonomic choice while still working on touchpads.

---

## 8. Files Reference

### Core Driver

| File | Purpose |
|------|---------|
| `src/xrt/drivers/qwerty/qwerty_device.h` | Internal header: struct definitions, all function declarations |
| `src/xrt/drivers/qwerty/qwerty_device.c` | Device implementation: pose tracking, input state, binding profiles |
| `src/xrt/drivers/qwerty/qwerty_win32.c` | Win32 input backend: keyboard/mouse → qwerty device actions |

### Integration Points

| File | Purpose |
|------|---------|
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Window procedure dispatches Win32 messages to `qwerty_process_win32()` |
| `src/xrt/compositor/d3d11/comp_d3d11_window.h` | Declares `qwerty_process_win32()` for C++ callers |
| `src/xrt/compositor/CMakeLists.txt` | `comp_win32_window` static library links qwerty driver for Win32 builds |

### SDL Backend (Original)

| File | Purpose |
|------|---------|
| `src/xrt/drivers/qwerty/qwerty_sdl.c` | SDL event handler for debug GUI |

---

## 9. Implementation Notes

### 9.1 Static State

`qwerty_process_win32()` uses `static` variables for state that persists across calls (cached device pointers, modifier key state, mouse position, button tracking). This is safe because there is exactly one window and one message pump thread.

### 9.2 Focus Change Release

When CTRL or ALT state changes, `qwerty_release_all()` is called on all devices. This prevents stuck movement keys when switching focus (e.g., pressing W while HMD-focused, then pressing CTRL to switch to left controller -- W release would go to the wrong device without the release-all).

### 9.3 Dual Controller Targeting

All keyboard and mouse actions use `targets[]` / `ctrl_targets[]` arrays with `target_count`. When CTRL+ALT are both held, both arrays contain both controllers and `target_count = 2`. Every action loops over the array, so both controllers move, rotate, trigger, etc. simultaneously.
