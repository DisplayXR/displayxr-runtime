# Monado Qwerty Driver - Keyboard and Mouse Controls

The Monado OpenXR runtime includes a **qwerty driver** (`src/xrt/drivers/qwerty/`) that provides simulated HMD and controller input via keyboard and mouse. This is useful for development and testing without physical VR hardware.

## Overview

The qwerty driver creates three simulated devices:
- **Qwerty HMD** - A simulated head-mounted display
- **Qwerty Left Controller** - A simulated left hand controller
- **Qwerty Right Controller** - A simulated right hand controller

## Device Focus

Controls apply to the currently **focused device**. Use modifier keys to switch focus:

| Key | Focused Device |
|-----|----------------|
| (none) | HMD (default) |
| **LCTRL** (hold) | Left Controller |
| **LALT** (hold) | Right Controller |

## Movement Controls (WASDQE)

These move the focused device in 3D space:

| Key | Action |
|-----|--------|
| **W** | Move forward |
| **S** | Move backward |
| **A** | Move left |
| **D** | Move right |
| **E** | Move up |
| **Q** | Move down |

Movement is relative to the device's current orientation (except up/down which is in world space).

## Rotation Controls

### Keyboard Rotation

| Key | Action |
|-----|--------|
| **Left Arrow** | Look left (yaw) |
| **Right Arrow** | Look right (yaw) |
| **Up Arrow** | Look up (pitch) |
| **Down Arrow** | Look down (pitch) |

### Mouse Rotation

| Input | Action |
|-------|--------|
| **Right Click + Drag** | Rotate device (yaw and pitch) |

Mouse sensitivity is set to 0.1 units per pixel.

## Speed Controls

| Input | Action |
|-------|--------|
| **LSHIFT** (hold) | Sprint mode (~3x faster movement) |
| **Mouse Wheel** | Increase/decrease movement speed |
| **Numpad +** | Increase movement speed |
| **Numpad -** | Decrease movement speed |

Speed changes by a factor of 1.25x per step.

## Controller Actions

These only apply when a controller is focused (or to the default controller when HMD is focused):

| Input | Action |
|-------|--------|
| **Left Click** | Select click (trigger) |
| **Middle Click** | Menu click |

## Controller-Specific Controls

| Key | Action |
|-----|--------|
| **F** | Toggle controller(s) follow HMD mode |
| **R** | Reset controller pose(s) to default position |

When HMD is focused, **F** and **R** affect both controllers. When a specific controller is focused, they only affect that controller.

## Initial Device Positions

| Device | Starting Position |
|--------|-------------------|
| HMD | (0, 1.6, 0) - 1.6 meters height |
| Left Controller | (-0.2, -0.3, -0.5) relative to HMD |
| Right Controller | (0.2, -0.3, -0.5) relative to HMD |

## Default Speeds

| Device | Movement Speed | Look Speed |
|--------|----------------|------------|
| HMD | 0.002 m/frame | 0.02 rad/frame |
| Controllers | 0.005 m/frame | 0.05 rad/frame |

## Quick Reference Card

```
┌─────────────────────────────────────────────────────────────┐
│                    QWERTY DRIVER CONTROLS                    │
├─────────────────────────────────────────────────────────────┤
│  DEVICE FOCUS                                                │
│    (none) = HMD    LCTRL = Left    LALT = Right             │
├─────────────────────────────────────────────────────────────┤
│  MOVEMENT              │  ROTATION                          │
│    W = Forward         │    ↑ = Look Up                     │
│    S = Backward        │    ↓ = Look Down                   │
│    A = Left            │    ← = Look Left                   │
│    D = Right           │    → = Look Right                  │
│    E = Up              │    Right-Click+Drag = Free Look    │
│    Q = Down            │                                    │
├─────────────────────────────────────────────────────────────┤
│  SPEED                 │  CONTROLLER ACTIONS                │
│    LSHIFT = Sprint     │    Left-Click = Select             │
│    Scroll = Speed ±    │    Middle-Click = Menu             │
│    Num+/- = Speed ±    │    F = Toggle Follow HMD           │
│                        │    R = Reset Pose                  │
└─────────────────────────────────────────────────────────────┘
```

## Requirements

The qwerty driver requires:
- **SDL2** - For keyboard/mouse input handling
- **XRT_BUILD_DRIVER_QWERTY** - CMake option must be enabled (depends on `XRT_HAVE_SDL2`)

## Source Files

- `src/xrt/drivers/qwerty/qwerty_device.c` - Device implementation and pose tracking
- `src/xrt/drivers/qwerty/qwerty_device.h` - Internal structures and constants
- `src/xrt/drivers/qwerty/qwerty_sdl.c` - SDL2 event handling and key mappings
- `src/xrt/drivers/qwerty/qwerty_interface.h` - Public interface
- `src/xrt/drivers/qwerty/qwerty_prober.c` - Device discovery/probing
