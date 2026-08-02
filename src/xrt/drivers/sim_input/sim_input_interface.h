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
sim_input_create_controller(enum xrt_device_type type);

#ifdef __cplusplus
}
#endif
