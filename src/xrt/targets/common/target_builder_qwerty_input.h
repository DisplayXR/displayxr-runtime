// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared helper for adding qwerty keyboard/mouse input devices.
 * @author David Fattal
 * @ingroup drv_qwerty
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "util/u_logging.h"

struct xrt_system_devices;
struct u_builder_roles_helper;
struct xrt_device;

/*!
 * Add qwerty keyboard/mouse input devices to the system.
 *
 * Creates qwerty HMD + left/right controllers, adds them to the device list,
 * and assigns controllers to left/right roles.
 *
 * The qwerty HMD is returned via @p out_qwerty_hmd for optional pose delegation
 * (e.g., sim_display delegates its pose to qwerty for WASD camera control).
 * Callers that don't need pose delegation can pass NULL.
 *
 * Guarded by XRT_BUILD_DRIVER_QWERTY -- returns XRT_SUCCESS (no-op) if not built.
 *
 * @param xsysd           System devices to add qwerty devices to.
 * @param ubrh            Roles helper to assign left/right controllers.
 * @param log_level       Logging level for qwerty device creation.
 * @param out_qwerty_hmd  Optional output: receives the qwerty HMD device pointer.
 * @return XRT_SUCCESS on success (including no-op when qwerty not built).
 */
xrt_result_t
t_builder_add_qwerty_input(struct xrt_system_devices *xsysd,
                           struct u_builder_roles_helper *ubrh,
                           enum u_logging_level log_level,
                           struct xrt_device **out_qwerty_hmd);
