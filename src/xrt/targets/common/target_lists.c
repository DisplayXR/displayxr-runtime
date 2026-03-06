// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common things to pull into a target.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 */

#include "xrt/xrt_config_drivers.h"

#include "target_lists.h"
#include "target_builder_interface.h"


/*!
 * Builders
 */
xrt_builder_create_func_t target_builder_list[] = {
#ifdef T_BUILDER_QWERTY // High up to override any real hardware.
    t_builder_qwerty_create,
#endif // T_BUILDER_QWERTY

#ifdef T_BUILDER_LEIA // Leia 3D display (SR SDK / CNSDK).
    t_builder_leia_create,
#endif // T_BUILDER_LEIA

#ifdef T_BUILDER_SIM_DISPLAY // After overrides, before real hardware.
    t_builder_sim_display_create,
#endif // T_BUILDER_SIM_DISPLAY

#ifdef T_BUILDER_LEGACY
    t_builder_legacy_create,
#endif // T_BUILDER_LEGACY

    NULL,
};


/*!
 * Each entry should be a vendor ID (VID), product ID (PID), a "found" function,
 * and a string literal name.
 *
 * The "found" function must return `int` and take as parameters:
 *
 * - `struct xrt_prober *xp`
 * - `struct xrt_prober_device **devices`
 * - `size_t index`
 * - `struct xrt_device **out_xdevs` (an array of XRT_MAX_DEVICES_PER_PROBE
 * xrt_device pointers)
 *
 * It is called when devices[index] match the VID and PID in the list.
 * It should return 0 if it decides not to create any devices, negative on
 * error, and the number of devices created if it creates one or more: it should
 * assign sequential elements of out_xdevs to the created devices.
 */
struct xrt_prober_entry target_entry_list[] = {
    {0x0000, 0x0000, NULL, NULL, NULL}, // Terminate
};

struct xrt_prober_entry *target_entry_lists[] = {
    target_entry_list,
    NULL, // Terminate
};

xrt_auto_prober_create_func_t target_auto_list[] = {
    NULL, // Terminate
};

struct xrt_prober_entry_lists target_lists = {
    target_builder_list,
    target_entry_lists,
    target_auto_list,
    NULL,
};
