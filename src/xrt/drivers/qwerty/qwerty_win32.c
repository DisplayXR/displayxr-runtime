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
 * provided via XR_EXT_win32_window_binding). When an app provides its own window,
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
#define SENSITIVITY 0.1f
// movement_speed units per pixel for mouse-driven XY translation (0.2 * 0.005 = 0.001 m/px)
#define POSITION_SENSITIVITY 0.2f

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

	struct qwerty_device *default_qdev = NULL;
	if (head >= 0 && head < (int)xdev_count && xdevs[head] == xd_hmd) {
		default_qdev = qwerty_device(xd_hmd);
	} else if (right >= 0 && right < (int)xdev_count && xdevs[right] == xd_right) {
		default_qdev = qwerty_device(xd_right);
	} else if (left >= 0 && left < (int)xdev_count && xdevs[left] == xd_left) {
		default_qdev = qwerty_device(xd_left);
	} else {
		// Fallback to right controller
		default_qdev = qwerty_device(xd_right);
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
		qsys = find_qwerty_system(xdevs, xdev_count);
		if (qsys == NULL) {
			return; // No qwerty devices found
		}
		default_qdev = default_qwerty_device(xdevs, xdev_count, qsys);
		default_qctrl = default_qwerty_controller(xdevs, xdev_count, qsys);
		cached = true;
		U_LOG_W("QWERTY Win32 input initialized - WASDQE move, RMB+drag look, LMB trigger, ESC quit");
	}

	if (qsys == NULL || !qsys->process_keys) {
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
			bool new_state = is_keydown;
			if (alt_pressed != new_state) {
				alt_change = true;
				alt_pressed = new_state;
			}
		}
		if (wParam == VK_LCONTROL || wParam == VK_CONTROL) {
			bool new_state = is_keydown;
			if (ctrl_pressed != new_state) {
				ctrl_change = true;
				ctrl_pressed = new_state;
			}
		}

		// Release all on focus change and reset mouse baseline to prevent jump
		if (alt_change || ctrl_change) {
			if (using_qhmd) {
				qwerty_release_all(qd_hmd);
			}
			qwerty_release_all(qd_right);
			qwerty_release_all(qd_left);
			GetCursorPos(&last_mouse_pos);
		}
	}

	// Determine focused device
	// Use GetAsyncKeyState for reliable modifier detection (avoids stuck keys from missed key-up events)
	bool alt_actual = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	bool ctrl_actual = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

	// Sync our tracked state with actual state (in case we missed key-up events)
	if (alt_pressed != alt_actual) {
		alt_pressed = alt_actual;
	}
	if (ctrl_pressed != ctrl_actual) {
		ctrl_pressed = ctrl_actual;
	}

	// Build target arrays for dual controller support (CTRL+ALT = both)
	struct qwerty_device *targets[2];
	struct qwerty_controller *ctrl_targets[2];
	int target_count;

	if (ctrl_pressed && alt_pressed) {
		targets[0] = qd_left;
		targets[1] = qd_right;
		ctrl_targets[0] = qleft;
		ctrl_targets[1] = qright;
		target_count = 2;
	} else if (ctrl_pressed) {
		targets[0] = qd_left;
		ctrl_targets[0] = qleft;
		target_count = 1;
	} else if (alt_pressed) {
		targets[0] = qd_right;
		ctrl_targets[0] = qright;
		target_count = 1;
	} else {
		targets[0] = default_qdev;
		ctrl_targets[0] = default_qctrl;
		target_count = 1;
	}

	// Update GUI tracking vars
	qsys->lctrl_focused = ctrl_pressed;
	qsys->rctrl_focused = alt_pressed;
	qsys->hmd_focused = (!ctrl_pressed && !alt_pressed && targets[0] == qd_hmd);

	// Handle key events
	if (is_keydown || is_keyup) {
		bool handled = true;

		// WASDQE Movement (applied to all targets)
		switch (wParam) {
		case 'W':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_forward(targets[i]);
				else
					qwerty_release_forward(targets[i]);
			}
			break;
		case 'A':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_left(targets[i]);
				else
					qwerty_release_left(targets[i]);
			}
			break;
		case 'S':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_backward(targets[i]);
				else
					qwerty_release_backward(targets[i]);
			}
			break;
		case 'D':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_right(targets[i]);
				else
					qwerty_release_right(targets[i]);
			}
			break;
		case 'Q':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_down(targets[i]);
				else
					qwerty_release_down(targets[i]);
			}
			break;
		case 'E':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_up(targets[i]);
				else
					qwerty_release_up(targets[i]);
			}
			break;

		// Arrow keys rotation
		case VK_LEFT:
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_look_left(targets[i]);
				else
					qwerty_release_look_left(targets[i]);
			}
			break;
		case VK_RIGHT:
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_look_right(targets[i]);
				else
					qwerty_release_look_right(targets[i]);
			}
			break;
		case VK_UP:
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_look_up(targets[i]);
				else
					qwerty_release_look_up(targets[i]);
			}
			break;
		case VK_DOWN:
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_look_down(targets[i]);
				else
					qwerty_release_look_down(targets[i]);
			}
			break;

		// Sprint
		case VK_LSHIFT:
		case VK_SHIFT:
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_sprint(targets[i]);
				else
					qwerty_release_sprint(targets[i]);
			}
			break;

		// Movement speed
		case VK_ADD:
			if (is_keydown) {
				for (int i = 0; i < target_count; i++)
					qwerty_change_movement_speed(targets[i], 1);
			}
			break;
		case VK_SUBTRACT:
			if (is_keydown) {
				for (int i = 0; i < target_count; i++)
					qwerty_change_movement_speed(targets[i], -1);
			}
			break;

		// Controller buttons
		case 'N':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_menu(ctrl_targets[i]);
				else
					qwerty_release_menu(ctrl_targets[i]);
			}
			break;
		case 'B':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_system(ctrl_targets[i]);
				else
					qwerty_release_system(ctrl_targets[i]);
			}
			break;

		// Thumbstick
		case 'F':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_thumbstick_left(ctrl_targets[i]);
				else
					qwerty_release_thumbstick_left(ctrl_targets[i]);
			}
			break;
		case 'H':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_thumbstick_right(ctrl_targets[i]);
				else
					qwerty_release_thumbstick_right(ctrl_targets[i]);
			}
			break;
		case 'T':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_thumbstick_up(ctrl_targets[i]);
				else
					qwerty_release_thumbstick_up(ctrl_targets[i]);
			}
			break;
		case 'G':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_thumbstick_down(ctrl_targets[i]);
				else
					qwerty_release_thumbstick_down(ctrl_targets[i]);
			}
			break;
		case 'V':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_thumbstick_click(ctrl_targets[i]);
				else
					qwerty_release_thumbstick_click(ctrl_targets[i]);
			}
			break;

		// Trackpad
		case 'J':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_trackpad_left(ctrl_targets[i]);
				else
					qwerty_release_trackpad_left(ctrl_targets[i]);
			}
			break;
		case 'L':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_trackpad_right(ctrl_targets[i]);
				else
					qwerty_release_trackpad_right(ctrl_targets[i]);
			}
			break;
		case 'I':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_trackpad_up(ctrl_targets[i]);
				else
					qwerty_release_trackpad_up(ctrl_targets[i]);
			}
			break;
		case 'K':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_trackpad_down(ctrl_targets[i]);
				else
					qwerty_release_trackpad_down(ctrl_targets[i]);
			}
			break;
		case 'M':
			for (int i = 0; i < target_count; i++) {
				if (is_keydown)
					qwerty_press_trackpad_click(ctrl_targets[i]);
				else
					qwerty_release_trackpad_click(ctrl_targets[i]);
			}
			break;

		// Controller follow HMD toggle
		case 'C':
			if (is_keydown) {
				if (ctrl_pressed || alt_pressed) {
					// Toggle focused controller(s)
					for (int i = 0; i < target_count; i++)
						qwerty_follow_hmd(ctrl_targets[i], !ctrl_targets[i]->follow_hmd);
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
				if (ctrl_pressed || alt_pressed) {
					for (int i = 0; i < target_count; i++)
						qwerty_reset_controller_pose(ctrl_targets[i]);
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
	// RMB = mouse look (hold and drag to rotate)
	// LMB = controller trigger
	// MMB = controller squeeze/grip
	switch (message) {
	case WM_RBUTTONDOWN:
		// Start mouse look mode
		mouse_look_active = true;
		GetCursorPos(&last_mouse_pos);
		SetCapture(GetActiveWindow()); // Capture mouse to receive events outside window
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_RBUTTONUP:
		// End mouse look mode
		mouse_look_active = false;
		ReleaseCapture();
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_LBUTTONDOWN:
		for (int i = 0; i < target_count; i++)
			qwerty_press_trigger(ctrl_targets[i]);
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_LBUTTONUP:
		for (int i = 0; i < target_count; i++)
			qwerty_release_trigger(ctrl_targets[i]);
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_MBUTTONDOWN:
		for (int i = 0; i < target_count; i++)
			qwerty_press_squeeze(ctrl_targets[i]);
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_MBUTTONUP:
		for (int i = 0; i < target_count; i++)
			qwerty_release_squeeze(ctrl_targets[i]);
		if (out_handled != NULL) {
			*out_handled = true;
		}
		break;

	case WM_MOUSEMOVE: {
		POINT current_pos;
		GetCursorPos(&current_pos);

		int dx = current_pos.x - last_mouse_pos.x;
		int dy = current_pos.y - last_mouse_pos.y;

		if (dx != 0 || dy != 0) {
			if (mouse_look_active) {
				// RMB held: rotation via mouse look
				float yaw = (float)(-dx) * SENSITIVITY;
				float pitch = (float)(-dy) * SENSITIVITY;
				for (int i = 0; i < target_count; i++)
					qwerty_add_look_delta(targets[i], yaw, pitch);
			} else if (ctrl_pressed || alt_pressed) {
				// Controller focused (no RMB): XY translation
				float pos_dx = (float)(dx) * POSITION_SENSITIVITY;
				float pos_dy = (float)(-dy) * POSITION_SENSITIVITY;
				for (int i = 0; i < target_count; i++)
					qwerty_add_position_delta(targets[i], pos_dx, pos_dy);
			}
		}

		last_mouse_pos = current_pos;

		if (out_handled != NULL) {
			*out_handled = true;
		}
	} break;

	case WM_MOUSEWHEEL: {
		// HIWORD of wParam contains wheel delta
		short delta = (short)HIWORD(wParam);
		int steps = delta / WHEEL_DELTA;
		if (steps != 0) {
			for (int i = 0; i < target_count; i++)
				qwerty_change_movement_speed(targets[i], (float)steps);
		}
		if (out_handled != NULL) {
			*out_handled = true;
		}
	} break;

	default:
		break;
	}
}
