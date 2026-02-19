// Copyright 2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface for the multi-client layer code.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_main
 */

#pragma once

#include "xrt/xrt_compositor.h"


#ifdef __cplusplus
extern "C" {
#endif

struct u_pacing_app_factory;
struct comp_target_service;
struct comp_target;
struct xrt_system_devices;

/*!
 * Callback type for passing system devices to the Vulkan window for input forwarding.
 * Used by comp_main to wire up qwerty input forwarding to the mswin/macos window.
 * May be NULL if not needed (e.g. sdl-test, null compositor).
 */
typedef void (*comp_window_set_system_devices_fn)(struct comp_target *ct, struct xrt_system_devices *xsysd);

/*!
 * Create a "system compositor" that can handle multiple clients (each
 * through a "multi compositor") and that drives a single native compositor.
 * Both the native compositor and the pacing factory is owned by the system
 * compositor and destroyed by it.
 *
 * @param xcn                        Native compositor that client are multi-plexed to.
 * @param upaf                       App pacing factory, one pacer created per client.
 * @param xsci                       Information to be exposed.
 * @param do_warm_start              Should we always submit a frame at startup.
 * @param target_service             Target service for per-session rendering (may be NULL).
 * @param xcn_is_comp_compositor     True if xcn is a comp_compositor (Vulkan main compositor).
 * @param set_window_system_devices  Optional callback for qwerty input forwarding (may be NULL).
 * @param out_xsysc                  Created @ref xrt_system_compositor.
 *
 * @public @memberof multi_system_compositor
 */
xrt_result_t
comp_multi_create_system_compositor(struct xrt_compositor_native *xcn,
                                    struct u_pacing_app_factory *upaf,
                                    const struct xrt_system_compositor_info *xsci,
                                    bool do_warm_start,
                                    struct comp_target_service *target_service,
                                    bool xcn_is_comp_compositor,
                                    comp_window_set_system_devices_fn set_window_system_devices,
                                    struct xrt_system_compositor **out_xsysc);


#ifdef __cplusplus
}
#endif
