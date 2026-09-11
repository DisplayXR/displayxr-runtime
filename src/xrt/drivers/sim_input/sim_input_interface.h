// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to the simulated motion-controller input provider.
 *
 * sim_input is the in-tree reference INPUT-PROVIDER plug-in (ADR-034 /
 * #823) — the input-side sibling of sim_display: deterministic synthetic
 * motion controllers (circular motion + scripted button presses) that
 * exercise the full action-system path with zero hardware, for the CI
 * self-test gate and dev boxes. Ships as a plug-in DLL
 * (`DisplayXR-SimInput`); the runtime discovers it via
 * `HKLM\Software\DisplayXR\InputProviders` (Windows) or a
 * `200-sim-input-input-provider.json` manifest (POSIX).
 *
 * @author David Fattal
 * @ingroup drv_sim_input
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;

/*!
 * Create one simulated motion controller.
 *
 * @param type `XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER` or
 *             `XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER` — drives the
 *             circle center, the button-script phase, and the role the
 *             builder assigns.
 * @param rig_local Publish DISPLAY-PLANE-RELATIVE poses
 *             (`XRT_TRACKING_TYPE_RIG_LOCAL`, #1380): origin at the display
 *             centre, +X right, +Y up, +Z toward the viewer, which is what a
 *             provider that also NAVIGATES must do (ADR-034 Amendment 4). The
 *             runtime anchors that origin at the initial rig, so
 *             `world = rig(t) o L`. False keeps the historical
 *             stage-anchored circle (`XRT_TRACKING_TYPE_OTHER`, centred just
 *             above the tabletop cube) byte-for-byte.
 *
 * The device binds the `khr/simple_controller` interaction profile
 * (`XRT_DEVICE_SIMPLE_CONTROLLER`): select + menu clicks, grip + aim
 * poses, one vibration output. Pose and buttons are pure functions of
 * the monotonic clock, so `get_tracked_pose(at_time_ns)` is
 * timestamp-correct by construction and runs are reproducible.
 *
 * @ingroup drv_sim_input
 */
struct xrt_device *
sim_input_create_controller(enum xrt_device_type type, bool rig_local);

/*!
 * Create the simulated NAVIGATION device — the hardware-free reference rig
 * driver for the rig role (ADR-034 Amendment 4 / #1380).
 *
 * One `XRT_DEVICE_TYPE_NAVIGATION` device carrying
 * `XRT_INPUT_GENERIC_NAVIGATION_POSE` (an ABSOLUTE pose in the provider's own
 * navigation frame F — a slow scripted figure-eight plus yaw, a pure function
 * of the requested timestamp) and `XRT_INPUT_GENERIC_NAVIGATION_RECENTER` (the
 * durable level-with-timestamp of the contract).
 *
 * @param hold_period_ms     Every n ms the pose reports INVALID for 500 ms, so
 *                           the composer's hold / re-align path runs. 0 = the
 *                           pose is always valid.
 * @param recenter_period_ms Fire a scripted reset every n ms: the RECENTER
 *                           timestamp advances AND the scripted pose jumps to
 *                           a new phase in the same publication. 0 = never.
 *
 * @ingroup drv_sim_input
 */
struct xrt_device *
sim_input_create_navigation(int64_t hold_period_ms, int64_t recenter_period_ms);

#ifdef __cplusplus
}
#endif
