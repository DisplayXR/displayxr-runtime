// Copyright 2021, Mateo de Mayo.
// Copyright 2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Interface to @ref drv_qwerty.
 * @author Mateo de Mayo <mateodemayo@gmail.com>
 * @ingroup drv_qwerty
 */

#pragma once

#include "xrt/xrt_compiler.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef union SDL_Event SDL_Event;

// Forward declaration for Win32 types (used in qwerty_process_win32)
#ifdef _WIN32
#include <stdbool.h>
#endif

/*!
 * @defgroup drv_qwerty Qwerty driver
 * @ingroup drv
 *
 * @brief Driver for emulated HMD and controllers through keyboard and mouse.
 */

/*!
 * @dir drivers/qwerty
 *
 * @brief @ref drv_qwerty files.
 */

/*!
 * Process an SDL_Event (like a key press) and dispatches a suitable action
 * to the appropriate qwerty_device.
 *
 * @note A qwerty_controller might not be in use (for example if you have
 * physical controllers connected), though its memory will be modified by these
 * events regardless. A qwerty_hmd not in use will not be modified as it never
 * gets created.
 *
 * @ingroup drv_qwerty
 */
void
qwerty_process_event(struct xrt_device **xdevs, size_t xdev_count, SDL_Event event);

#ifdef _WIN32
/*!
 * Process a Win32 message (like a key press or mouse event) and dispatches
 * a suitable action to the appropriate qwerty_device.
 *
 * This allows keyboard/mouse input from the main D3D11 window to control
 * qwerty devices without requiring the SDL debug GUI window.
 *
 * @param xdevs Array of devices to search for qwerty devices
 * @param xdev_count Number of devices in the array
 * @param message The Win32 message (WM_KEYDOWN, WM_MOUSEMOVE, etc.)
 * @param wParam The wParam from the Win32 message
 * @param lParam The lParam from the Win32 message
 * @param out_handled Optional output: set to true if the message was handled
 *
 * @ingroup drv_qwerty
 */
void
qwerty_process_win32(struct xrt_device **xdevs,
                     size_t xdev_count,
                     unsigned int message,
                     unsigned long long wParam,
                     long long lParam,
                     bool *out_handled);
#endif

#ifdef __APPLE__
/*!
 * Process a macOS NSEvent and dispatch a suitable action to the appropriate
 * qwerty_device.
 *
 * This allows keyboard/mouse input from the macOS compositor NSWindow to
 * control qwerty devices without requiring the SDL debug GUI window.
 * This is the macOS equivalent of qwerty_process_win32.
 *
 * @param xdevs Array of devices to search for qwerty devices
 * @param xdev_count Number of devices in the array
 * @param ns_event_ptr An NSEvent* cast to void* (avoids ObjC header deps)
 *
 * @ingroup drv_qwerty
 */
void
qwerty_process_macos(struct xrt_device **xdevs,
                     size_t xdev_count,
                     void *ns_event_ptr);
#endif

/*!
 * Create all qwerty devices.
 *
 * @ingroup drv_qwerty
 */
xrt_result_t
qwerty_create_devices(enum u_logging_level log_level,
                      struct xrt_device **out_hmd,
                      struct xrt_device **out_left,
                      struct xrt_device **out_right);


#ifdef __cplusplus
}
#endif
