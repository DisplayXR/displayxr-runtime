// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows system tray icon for DisplayXR service.
 * @ingroup ipc
 */

#pragma once

#include "service_config.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*service_tray_shutdown_cb)(void);
typedef void (*service_tray_config_change_cb)(const struct service_config *new_cfg);

/*!
 * Initialize the system tray icon on a dedicated thread.
 * @param shutdown_cb  Called from the tray thread when the user clicks "Exit".
 * @param config_cb    Called when the user changes a setting via the tray menu (may be NULL).
 * @param initial_cfg  Initial config to reflect in the menu state.
 * @return true on success.
 */
bool
service_tray_init(service_tray_shutdown_cb shutdown_cb,
                  service_tray_config_change_cb config_cb,
                  const struct service_config *initial_cfg);

/*!
 * Remove the tray icon and clean up. Blocks until the tray thread exits.
 */
void
service_tray_cleanup(void);

/*!
 * Get the tray icon's message-only HWND (for RegisterHotKey from the orchestrator).
 * Returns NULL if tray is not initialized.
 */
void *
service_tray_get_hwnd(void);

#ifdef __cplusplus
}
#endif
