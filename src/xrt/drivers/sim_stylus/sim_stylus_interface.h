// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to the simulated 6-DoF stylus input provider.
 *
 * sim_stylus is the in-tree reference STYLUS provider (ADR-034 Amendment 8):
 * one scripted pen on the `XR_LOGITECH_mx_ink_stylus_interaction` profile
 * (`XRT_DEVICE_MX_INK_STYLUS`), declared
 * `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER` (the builder puts it on the
 * user's dominant hand) and published on an
 * `XRT_TRACKING_TYPE_RIG_LOCAL_PHYSICAL` origin (a tip touching the glass is
 * located where the app renders that point of the glass). It exists so a
 * vendor writing a real stylus provider, and an app binding the profile,
 * have a hardware-free counterpart. Ships as `DisplayXR-SimStylus`.
 *
 * @ingroup drv_sim_stylus
 */

#pragma once

#include "xrt/xrt_defines.h"

#include "sim_stylus_script.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;

/*!
 * Create the simulated stylus.
 *
 * @param device_type `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER` (the builder
 *                    resolves it), or an explicit LEFT / RIGHT.
 * @param cfg         The resolved script (copied).
 */
struct xrt_device *
sim_stylus_create(enum xrt_device_type device_type, const struct sim_stylus_script_config *cfg);

#ifdef __cplusplus
}
#endif
