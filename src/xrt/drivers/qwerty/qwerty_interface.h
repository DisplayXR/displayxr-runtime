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

#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif


struct xrt_pose;

/*!
 * Snapshot of qwerty view tuning state.
 * Each mode (camera/display) has its own independent variable set.
 * Camera mode: ipd_factor, parallax_factor, convergence, half_tan_vfov.
 * Display mode: ipd_factor, parallax_factor, vHeight (perspective always 1.0).
 * @ingroup drv_qwerty
 */
struct qwerty_view_state
{
	bool camera_mode; //!< true=camera (default), false=display

	// Camera-centric variables
	float cam_spread_factor;      //!< [0.01,1] default 1.0 (= cam_parallax always)
	float cam_parallax_factor; //!< [0.01,1] default 1.0 (= cam_ipd always)
	float cam_convergence;     //!< [0,2] diopters, default 0.5
	float cam_half_tan_vfov;   //!< default 0.3249 — derived only, not user-adjustable

	// Display-centric variables
	float disp_spread_factor;      //!< [0.01,1] default 1.0 (= disp_parallax always)
	float disp_parallax_factor; //!< [0.01,1] default 1.0 (= disp_ipd always)
	float disp_vHeight;         //!< [0.1,10] meters, default 1.3

	// Hardware config
	float nominal_viewer_z; //!< meters (e.g. 0.6 from sim_display)
	float screen_height_m;  //!< meters (e.g. 0.194 from sim_display)
};
typedef union SDL_Event SDL_Event;

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

/*!
 * Get the current qwerty HMD pose without side effects.
 *
 * Finds the qwerty HMD in @p xdevs and returns its current internal
 * pose state. Unlike get_tracked_pose(), this does not consume input
 * deltas or accumulate movement — it just reads the current state.
 *
 * @param xdevs     Array of devices to search for the qwerty HMD.
 * @param xdev_count Number of devices in the array.
 * @param[out] out_pose Receives the current qwerty HMD pose.
 * @return true if a qwerty HMD was found and out_pose was set.
 *
 * @ingroup drv_qwerty
 */
bool
qwerty_get_hmd_pose(struct xrt_device **xdevs, size_t xdev_count, struct xrt_pose *out_pose);

/*!
 * Check if a runtime-side display mode toggle is pending.
 *
 * Scans @p xdevs for a qwerty_system and checks its toggle state.
 * If a toggle is pending, clears the pending flag and sets @p out_force_2d
 * to the current force_2d_mode state.
 *
 * @param xdevs Array of devices to search for qwerty devices
 * @param xdev_count Number of devices in the array
 * @param[out] out_force_2d Receives the current force_2d_mode value
 * @return true if a toggle was pending (and is now cleared), false otherwise
 *
 * @ingroup drv_qwerty
 */
bool
qwerty_check_display_mode_toggle(struct xrt_device **xdevs, size_t xdev_count, bool *out_force_2d);

/*!
 * Check if a rendering mode change is pending (1/2/3 key).
 *
 * Scans @p xdevs for a qwerty_system and checks its pending state.
 * If a change is pending, clears the pending flag and sets @p out_mode
 * to the requested rendering mode index.
 *
 * @param xdevs Array of devices to search for qwerty devices
 * @param xdev_count Number of devices in the array
 * @param[out] out_mode Receives the rendering mode index (0=SBS, 1+=vendor-defined)
 * @return true if a change was pending (and is now cleared), false otherwise
 *
 * @ingroup drv_qwerty
 */
bool
qwerty_check_rendering_mode_change(struct xrt_device **xdevs, size_t xdev_count, int *out_mode);

/*!
 * Update the qwerty stored rendering mode without triggering a pending change.
 * Used by the polling code to feed back wrapped mode indices (for V key cycling).
 *
 * @ingroup drv_qwerty
 */
void
qwerty_set_rendering_mode_silent(struct xrt_device **xdevs, size_t xdev_count, int mode);

/*!
 * Get the current qwerty view tuning state.
 *
 * Scans @p xdevs for a qwerty_system and copies its view state.
 *
 * @param xdevs     Array of devices to search for qwerty devices.
 * @param xdev_count Number of devices in the array.
 * @param[out] out  Receives the current 3D state.
 * @return true if a qwerty system was found and out was set.
 *
 * @ingroup drv_qwerty
 */
bool
qwerty_get_view_state(struct xrt_device **xdevs,
                        size_t xdev_count,
                        struct qwerty_view_state *out);


/*!
 * Enable or disable qwerty input processing (keyboard/mouse events).
 *
 * When disabled, all qwerty input handlers (SDL, Win32, macOS) return
 * early without processing events. Used to silence qwerty for _ext/_shared
 * apps where the app owns the window and input.
 *
 * @param xdevs     Array of devices to search for qwerty devices.
 * @param xdev_count Number of devices in the array.
 * @param enabled   true to enable input processing, false to disable.
 *
 * @ingroup drv_qwerty
 */
void
qwerty_set_process_keys(struct xrt_device **xdevs, size_t xdev_count, bool enabled);


#ifdef __cplusplus
}
#endif
