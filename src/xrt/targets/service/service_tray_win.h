// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows system tray icon for DisplayXR service.
 * @ingroup ipc
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*service_tray_shutdown_cb)(void);

/*!
 * Initialize the system tray icon on a dedicated thread.
 * @param shutdown_cb Called from the tray thread when the user clicks "Exit".
 * @return true on success.
 */
bool
service_tray_init(service_tray_shutdown_cb shutdown_cb);

/*!
 * Remove the tray icon and clean up. Blocks until the tray thread exits.
 */
void
service_tray_cleanup(void);

#ifdef __cplusplus
}
#endif
