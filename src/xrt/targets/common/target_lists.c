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
 * Builders. sim_display is the only one: it creates the head from the
 * display-processor plug-in (or the sim fallback) and the input from the
 * input-provider plug-ins / qwerty (#703).
 */
xrt_builder_create_func_t target_builder_list[] = {
#ifdef T_BUILDER_SIM_DISPLAY
    t_builder_sim_display_create,
#endif // T_BUILDER_SIM_DISPLAY

    NULL,
};

struct xrt_prober_entry_lists target_lists = {
    target_builder_list,
    NULL,
};
