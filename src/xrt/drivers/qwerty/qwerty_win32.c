// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Win32 input handler for qwerty devices (Windows D3D11 compositor).
 *
 * This allows keyboard/mouse input from the main D3D11 window to control
 * qwerty devices without requiring the SDL debug GUI window.
 *
 * NOTE: This is only used when Monado creates its own window (no HWND
 * provided via XR_EXT_session_target). When an app provides its own window,
 * the app handles input directly and this code is never called.
 *
 * @author David Fattal
 * @ingroup drv_qwerty
 */

#include "qwerty_device.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "xrt/xrt_device.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <string.h>
#include <stdbool.h>

// Amount of look_speed units a mouse delta of 1px in screen space will rotate the device.
// This value is multiplied by look_speed (0.02 for HMD) in qwerty_add_look_delta().
// To match sr_cube_openxr_ext (0.005 rad/px): 0.25 * 0.02 = 0.005
#define SENSITIVITY 0.25f

/*!
 * Find the qwerty_system from the device list.
 */
static struct qwerty_system *
find_qwerty_system(struct xrt_device **xdevs, size_t xdev_count)
{
	struct xrt_device *xdev = NULL;
	for (size_t i = 0; i < xdev_count; i++) {
		if (xdevs[i] == NULL) {
			continue;
		}
		// Check against tracker name to find qwerty devices
		const char *tracker_name = xdevs[i]->tracking_origin->name;
		if (strcmp(tracker_name, QWERTY_HMD_TRACKER_STR) == 0 ||
		    strcmp(tracker_name, QWERTY_LEFT_TRACKER_STR) == 0 ||
		    strcmp(tracker_name, QWERTY_RIGHT_TRACKER_STR) == 0) {
			xdev = xdevs[i];
			break;
		}
	}

	if (xdev == NULL) {
		return NULL;
	}

	struct qwerty_device *qdev = qwerty_device(xdev);
	struct qwerty_system *qsys = qdev->sys;
	return qsys;
}

/*!
 * Determine the default qwerty device based on which devices are in use.
 */
static struct qwerty_device *
default_qwerty_device(struct xrt_device **xdevs, size_t xdev_count, struct qwerty_system *qsys)
{
	int head;
	int left;
	int right;
	int gamepad;
	head = left = right = gamepad = XRT_DEVICE_ROLE_UNASSIGNED;
	u_device_assign_xdev_roles(xdevs, xdev_count, &head, &left, &right, &gamepad);

	struct xrt_device *xd_hmd = qsys->hmd ? &qsys->hmd->base.base : NULL;
	struct xrt_device *xd_left = &qsys->lctrl->base.base;
	struct xrt_device *xd_right = &qsys->rctrl->base.base;

	// Log role assignments for debugging
	U_LOG_W("QWERTY default_device: head=%d left=%d right=%d (count=%zu)", head, left, right, xdev_count);
	U_LOG_W("  xd_hmd=%p, xd_left=%p, xd_right=%p", (void*)xd_hmd, (void*)xd_left, (void*)xd_right);
	if (head >= 0 && head < (int)xdev_count) {
		U_LOG_W("  xdevs[head=%d]=%p (%s)", head, (void*)xdevs[head], xdevs[head] ? xdevs[head]->str : "NULL");
	} else {
		U_LOG_W("  head=%d is UNASSIGNED or out of range", head);
	}

	struct qwerty_device *default_qdev = NULL;
	if (head >= 0 && head < (int)xdev_count && xdevs[head] == xd_hmd) {
		default_qdev = qwerty_device(xd_hmd);
		U_LOG_W("  -> Selected HMD as default device");
	} else if (right >= 0 && right < (int)xdev_count && xdevs[right] == xd_right) {
		default_qdev = qwerty_device(xd_right);
		U_LOG_W("  -> Selected Right Controller as default device (HMD check failed)");
	} else if (left >= 0 && left < (int)xdev_count && xdevs[left] == xd_left) {
		default_qdev = qwerty_device(xd_left);
		U_LOG_W("  -> Selected Left Controller as default device");
	} else {
		// Fallback to right controller
		default_qdev = qwerty_device(xd_right);
		U_LOG_W("  -> Fallback to Right Controller (no role matches)");
	}

	return default_qdev;
}

/*!
 * Determine the default qwerty controller based on which devices are in use.
 */
static struct qwerty_controller *
default_qwerty_controller(struct xrt_device **xdevs, size_t xdev_count, struct qwerty_system *qsys)
{
	int head;
	int left;
	int right;
	int gamepad;
	head = left = right = gamepad = XRT_DEVICE_ROLE_UNASSIGNED;
	u_device_assign_xdev_roles(xdevs, xdev_count, &head, &left, &right, &gamepad);

	struct xrt_device *xd_left = &qsys->lctrl->base.base;
	struct xrt_device *xd_right = &qsys->rctrl->base.base;

	struct qwerty_controller *default_qctrl = NULL;
	if (xdevs[right] == xd_right) {
		default_qctrl = qwerty_controller(xd_right);
	} else if (xdevs[left] == xd_left) {
		default_qctrl = qwerty_controller(xd_left);
	} else {
		// Fallback to right controller
		default_qctrl = qwerty_controller(xd_right);
	}

	return default_qctrl;
}

void
qwerty_process_win32(struct xrt_device **xdevs,
                     size_t xdev_count,
                     unsigned int message,
                     unsigned long long wParam,
                     long long lParam,
                     bool *out_handled)
{
	// Cached state (persists across calls)
	static struct qwerty_system *qsys = NULL;
	static bool alt_pressed = false;
	static bool ctrl_pressed = false;
	static struct qwerty_device *default_qdev = NULL;
	static struct qwerty_controller *default_qctrl = NULL;
	static bool cached = false;
	static bool mouse_look_active = false;
	static POINT last_mouse_pos = {0, 0};

	// Default: not handled
	if (out_handled != NULL) {
		*out_handled = false;
	}

	// Initialize cache on first call
	if (!cached) {
		U_LOG_W("QWERTY Win32: First call - searching for qwerty system in %zu devices", xdev_count);
		for (size_t i = 0; i < xdev_count; i++) {
			if (xdevs[i] != NULL) {
				U_LOG_W("QWERTY Win32: Device[%zu] name='%s' tracker='%s'",
				        i, xdevs[i]->str, xdevs[i]->tracking_origin->name);
			}
		}
		qsys = find_qwerty_system(xdevs, xdev_count);
		if (qsys == NULL) {
			U_LOG_W("QWERTY Win32: No qwerty devices found in device list!");
			return; // No qwerty devices found
		}
		default_qdev = default_qwerty_device(xdevs, xdev_count, qsys);
		default_qctrl = default_qwerty_controller(xdevs, xdev_count, qsys);
		cached = true;
		U_LOG_W("QWERTY Win32 input initialized - WASDQE move, left-click+drag look, ESC quit");
		U_LOG_W("QWERTY Win32: qsys=%p hmd=%p lctrl=%p rctrl=%p process_keys=%d",
		        (void *)qsys, (void *)qsys->hmd, (void *)qsys->lctrl, (void *)qsys->rctrl, qsys->process_keys);
	}

	if (qsys == NULL || !qsys->process_keys) {
		U_LOG_W("QWERTY Win32: Ignoring input - qsys=%p process_keys=%d",
		        (void *)qsys, qsys ? qsys->process_keys : -1);
		return;
	}

	// Get device views
	struct qwerty_controller *qleft = qsys->lctrl;
	struct qwerty_device *qd_left = &qleft->base;

	struct qwerty_controller *qright = qsys->rctrl;
	struct qwerty_device *qd_right = &qright->base;

	bool using_qhmd = qsys->hmd != NULL;
	struct qwerty_hmd *qhmd = using_qhmd ? qsys->hmd : NULL;
	struct qwerty_device *qd_hmd = using_qhmd ? &qhmd->base : NULL;

	// Check modifier key state
	bool is_keydown = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
	bool is_keyup = (message == WM_KEYUP || message == WM_SYSKEYUP);

	// Handle CTRL/ALT for focus switching
	if (is_keydown || is_keyup) {
		bool alt_change = false;
		bool ctrl_change = false;

		if (wParam == VK_LMENU || wParam == VK_MENU) {
			alt_change = true;
			alt_pressed = is_keydown;
		}
		if (wParam == VK_LCONTROL || wParam == VK_CONTROL) {
			ctrl_change = true;
			ctrl_pressed = is_keydown;
		}

		// Release all on focus change
		if (alt_change || ctrl_change) {
			if (using_qhmd) {
				qwerty_release_all(qd_hmd);
			}
			qwerty_release_all(qd_right);
			qwerty_release_all(qd_left);
		}
	}

	// Determine focused device
	// Use GetAsyncKeyState for reliable modifier detection (avoids stuck keys from missed key-up events)
	bool alt_actual = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	bool ctrl_actual = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

	// Sync our tracked state with actual state (in case we missed key-up events)
	if (alt_pressed != alt_actual) {
		U_LOG_W("QWERTY Win32: alt_pressed sync %d -> %d (GetAsyncKeyState)", alt_pressed, alt_actual);
		alt_pressed = alt_actual;
	}
	if (ctrl_pressed != ctrl_actual) {
		U_LOG_W("QWERTY Win32: ctrl_pressed sync %d -> %d (GetAsyncKeyState)", ctrl_pressed, ctrl_actual);
		ctrl_pressed = ctrl_actual;
	}

	struct qwerty_device *qdev;
	if (ctrl_pressed) {
		qdev = qd_left;
	} else if (alt_pressed) {
		qdev = qd_right;
	} else {
		qdev = default_qdev;
	}

	// Determine focused controller
	struct qwerty_controller *qctrl = (qdev != qd_hmd) ? qwerty_controller(&qdev->base) : default_qctrl;

	// Update GUI tracking vars
	qsys->hmd_focused = (qdev == qd_hmd);
	qsys->lctrl_focused = (qdev == qd_left);
	qsys->rctrl_focused = (qdev == qd_right);

	// Handle key events
	if (is_keydown || is_keyup) {
		bool handled = true;

		// Log which device is being controlled
		const char *dev_name = "unknown";
		if (qdev == qd_hmd) dev_name = "HMD";
		else if (qdev == qd_left) dev_name = "Left";
		else if (qdev == qd_right) dev_name = "Right";

		// Diagnostic: log focus determination details on first WASD key
		static bool first_wasd_log = true;
		if (first_wasd_log && (wParam == 'W' || wParam == 'A' || wParam == 'S' || wParam == 'D')) {
			first_wasd_log = false;
			U_LOG_W("QWERTY Win32: Focus debug: alt=%d ctrl=%d qdev=%p qd_hmd=%p qd_left=%p qd_right=%p default=%p",
			        alt_pressed, ctrl_pressed, (void *)qdev, (void *)qd_hmd, (void *)qd_left, (void *)qd_right,
			        (void *)default_qdev);
		}

		// WASDQE Movement
		switch (wParam) {
		case 'W':
			U_LOG_W("QWERTY Win32: W key %s -> %s", is_keydown ? "DOWN" : "UP", dev_name);
			if (is_keydown)
				qwerty_press_forward(qdev);
			else
				qwerty_release_forward(qdev);
			break;
		case 'A':
			U_LOG_W("QWERTY Win32: A key %s -> %s", is_keydown ? "DOWN" : "UP", dev_name);
			if (is_keydown)
				qwerty_press_left(qdev);
			else
				qwerty_release_left(qdev);
			break;
		case 'S':
			U_LOG_W("QWERTY Win32: S key %s -> %s", is_keydown ? "DOWN" : "UP", dev_name);
			if (is_keydown)
				qwerty_press_backward(qdev);
			else
				qwerty_release_backward(qdev);
			break;
		case 'D':
			U_LOG_W("QWERTY Win32: D key %s -> %s", is_keydown ? "DOWN" : "UP", dev_name);
			if (is_keydown)
				qwerty_press_right(qdev);
			else
				qwerty_release_right(qdev);
			break;
		case 'Q':
			if (is_keydown)
				qwerty_press_down(qdev);
			else
				qwerty_release_down(qdev);
			break;
		case 'E':
			if (is_keydown)
				qwerty_press_up(qdev);
			else
				qwerty_release_up(qdev);
			break;

		// Arrow keys rotation
		case VK_LEFT:
			if (is_keydown)
				qwerty_press_look_left(qdev);
			else
				qwerty_release_look_left(qdev);
			break;
		case VK_RIGHT:
			if (is_keydown)
				qwerty_press_look_right(qdev);
			else
				qwerty_release_look_right(qdev);
			break;
		case VK_UP:
			if (is_keydown)
				qwerty_press_look_up(qdev);
			else
				qwerty_release_look_up(qdev);
			break;
		case VK_DOWN:
			if (is_keydown)
				qwerty_press_look_down(qdev);
			else
				qwerty_release_look_down(qdev);
			break;

		// Sprint
		case VK_LSHIFT:
		case VK_SHIFT:
			if (is_keydown)
				qwerty_press_sprint(qdev);
			else
				qwerty_release_sprint(qdev);
			break;

		// Movement speed
		case VK_ADD:
			if (is_keydown)
				qwerty_change_movement_speed(qdev, 1);
			break;
		case VK_SUBTRACT:
			if (is_keydown)
				qwerty_change_movement_speed(qdev, -1);
			break;

		// Controller buttons
		case 'N':
			if (is_keydown)
				qwerty_press_menu(qctrl);
			else
				qwerty_release_menu(qctrl);
			break;
		case 'B':
			if (is_keydown)
				qwerty_press_system(qctrl);
			else
				qwerty_release_system(qctrl);
			break;

		// Thumbstick
		case 'F':
			if (is_keydown)
				qwerty_press_thumbstick_left(qctrl);
			else
				qwerty_release_thumbstick_left(qctrl);
			break;
		case 'H':
			if (is_keydown)
				qwerty_press_thumbstick_right(qctrl);
			else
				qwerty_release_thumbstick_right(qctrl);
			break;
		case 'T':
			if (is_keydown)
				qwerty_press_thumbstick_up(qctrl);
			else
				qwerty_release_thumbstick_up(qctrl);
			break;
		case 'G':
			if (is_keydown)
				qwerty_press_thumbstick_down(qctrl);
			else
				qwerty_release_thumbstick_down(qctrl);
			break;
		case 'V':
			if (is_keydown)
				qwerty_press_thumbstick_click(qctrl);
			else
				qwerty_release_thumbstick_click(qctrl);
			break;

		// Trackpad
		case 'J':
			if (is_keydown)
				qwerty_press_trackpad_left(qctrl);
			else
				qwerty_release_trackpad_left(qctrl);
			break;
		case 'L':
			if (is_keydown)
				qwerty_press_trackpad_right(qctrl);
			else
				qwerty_release_trackpad_right(qctrl);
			break;
		case 'I':
			if (is_keydown)
				qwerty_press_trackpad_up(qctrl);
			else
				qwerty_release_trackpad_up(qctrl);
			break;
		case 'K':
			if (is_keydown)
				qwerty_press_trackpad_down(qctrl);
			else
				qwerty_release_trackpad_down(qctrl);
			break;
		case 'M':
			if (is_keydown)
				qwerty_press_trackpad_click(qctrl);
			else
				qwerty_release_trackpad_click(qctrl);
			break;

		// Controller follow HMD toggle
		case 'C':
			if (is_keydown) {
				if (qdev != qd_hmd) {
					qwerty_follow_hmd(qctrl, !qctrl->follow_hmd);
				} else {
					// Toggle both controllers
					bool both_not_following = !qleft->follow_hmd && !qright->follow_hmd;
					qwerty_follow_hmd(qleft, both_not_following);
					qwerty_follow_hmd(qright, both_not_following);
				}
			}
			break;

		// Reset controller pose
		case 'R':
			if (is_keydown) {
				if (qdev != qd_hmd) {
					qwerty_reset_controller_pose(qctrl);
				} else {
					// Reset both controllers
					qwerty_reset_controller_pose(qleft);
					qwerty_reset_controller_pose(qright);
				}
			}
			break;

		// ESC key to close window
		case VK_ESCAPE:
			if (is_keydown) {
				U_LOG_W("QWERTY Win32: ESC pressed - closing window");
				HWND hwnd = GetActiveWindow();
				if (hwnd != NULL) {
					PostMessageW(hwnd, WM_CLOSE, 0, 0);
				}
			}
			break;

		default:
			handled = false;
			break;
		}

		if (handled && out_handled != NULL) {
			*out_handled = true;
		}
	}

	// Mouse button events
	// Left mouse button = mouse look (matches sr_cube_openxr_ext)
	// Right mouse button = controller trigger
	// Middle mouse button = controller squeeze/grip
	switch (message) {
	case WM_LBUTTONDOWN:
		// Start mouse look mode
		U_LOG_W("QWERTY Win32: LMB DOWN - starting mouse look");
		mouse_look_active = true;
		GetCursorPos(&last_mouse_pos);
		SetCapture(GetActiveWindow()); // Capture mouse to receive events outside window
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_LBUTTONUP:
		// End mouse look mode
		U_LOG_W("QWERTY Win32: LMB UP - ending mouse look");
		mouse_look_active = false;
		ReleaseCapture();
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_RBUTTONDOWN:
		qwerty_press_trigger(qctrl);
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_RBUTTONUP:
		qwerty_release_trigger(qctrl);
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_MBUTTONDOWN:
		qwerty_press_squeeze(qctrl);
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_MBUTTONUP:
		qwerty_release_squeeze(qctrl);
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_MOUSEMOVE:
		if (mouse_look_active) {
			POINT current_pos;
			GetCursorPos(&current_pos);

			// Calculate delta
			int dx = current_pos.x - last_mouse_pos.x;
			int dy = current_pos.y - last_mouse_pos.y;

			if (dx != 0 || dy != 0) {
				float yaw = (float)(-dx) * SENSITIVITY;
				float pitch = (float)(-dy) * SENSITIVITY;
				// Log qdev pointer on first mouse look (for device identity debugging)
				static bool first_mouse_log = true;
				if (first_mouse_log) {
					first_mouse_log = false;
					U_LOG_W("QWERTY Win32: Mouse look qdev=%p (updating this device)", (void *)qdev);
				}
				U_LOG_W("QWERTY Win32: Mouse look delta dx=%d dy=%d yaw=%.3f pitch=%.3f", dx, dy, yaw, pitch);
				qwerty_add_look_delta(qdev, yaw, pitch);
			}

			last_mouse_pos = current_pos;

			if (out_handled != NULL) {
				*out_handled = true;
			}
		}
		break;

	case WM_MOUSEWHEEL:
		{
			// HIWORD of wParam contains wheel delta
			short delta = (short)HIWORD(wParam);
			int steps = delta / WHEEL_DELTA;
			if (steps != 0) {
				qwerty_change_movement_speed(qdev, (float)steps);
			}
			if (out_handled != NULL) {
				*out_handled = true;
			}
		}
		break;

	default:
		break;
	}
}
