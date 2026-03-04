// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS input handler for qwerty devices (Vulkan compositor window).
 *
 * This allows keyboard/mouse input from the macOS compositor NSWindow to
 * control qwerty devices without requiring the SDL debug GUI window.
 * This is the macOS equivalent of qwerty_win32.c.
 *
 * @author David Fattal
 * @ingroup drv_qwerty
 */

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h> // For kVK_ keycodes

#include "qwerty_device.h"
#include "util/u_device.h"
#include "util/u_hud.h"
#include "util/u_logging.h"
#include "xrt/xrt_device.h"

#include <assert.h>
#include <string.h>
#include <stdbool.h>

// Amount of look_speed units a mouse delta of 1px in screen space will rotate the device.
#define SENSITIVITY 0.1f
// movement_speed units per pixel for mouse-driven XY translation
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
	// When the qwerty HMD exists, default to it for WASD/mouse look.
	// This works even when another device (e.g. sim_display) is the head
	// role, because sim_display delegates its pose to the qwerty HMD.
	if (qsys->hmd != NULL) {
		return &qsys->hmd->base;
	}

	// No qwerty HMD: fall back to right controller.
	return &qsys->rctrl->base;
}

/*!
 * Determine the default qwerty controller based on which devices are in use.
 */
static struct qwerty_controller *
default_qwerty_controller(struct xrt_device **xdevs, size_t xdev_count, struct qwerty_system *qsys)
{
	int head, left, right, gamepad;
	head = left = right = gamepad = XRT_DEVICE_ROLE_UNASSIGNED;
	u_device_assign_xdev_roles(xdevs, xdev_count, &head, &left, &right, &gamepad);

	struct xrt_device *xd_left = &qsys->lctrl->base.base;
	struct xrt_device *xd_right = &qsys->rctrl->base.base;

	struct qwerty_controller *default_qctrl = NULL;
	if (right >= 0 && right < (int)xdev_count && xdevs[right] == xd_right) {
		default_qctrl = qwerty_controller(xd_right);
	} else if (left >= 0 && left < (int)xdev_count && xdevs[left] == xd_left) {
		default_qctrl = qwerty_controller(xd_left);
	} else {
		default_qctrl = qwerty_controller(xd_right);
	}

	return default_qctrl;
}

void
qwerty_process_macos(struct xrt_device **xdevs,
                     size_t xdev_count,
                     void *ns_event_ptr)
{
	NSEvent *event = (__bridge NSEvent *)ns_event_ptr;

	// Cached state (persists across calls)
	static struct qwerty_system *qsys = NULL;
	static bool ctrl_pressed = false;  // CTRL = left controller focus
	static bool alt_pressed = false;   // ALT/Option = right controller focus
	static struct qwerty_device *default_qdev = NULL;
	static struct qwerty_controller *default_qctrl = NULL;
	static bool cached = false;
	static bool mouse_look_active = false;
	static NSPoint last_mouse_pos = {0, 0};

	// Initialize cache on first call
	if (!cached) {
		qsys = find_qwerty_system(xdevs, xdev_count);
		if (qsys == NULL) {
			return; // No qwerty devices found
		}
		default_qdev = default_qwerty_device(xdevs, xdev_count, qsys);
		default_qctrl = default_qwerty_controller(xdevs, xdev_count, qsys);
		cached = true;
		U_LOG_W("QWERTY macOS input initialized - WASDQE move, RMB+drag look, F/G controller focus");
	}

	if (qsys == NULL || !qsys->process_keys) {
		return;
	}

	struct qwerty_controller *qleft = qsys->lctrl;
	struct qwerty_device *qd_left = &qleft->base;
	struct qwerty_controller *qright = qsys->rctrl;
	struct qwerty_device *qd_right = &qright->base;

	bool using_qhmd = qsys->hmd != NULL;
	struct qwerty_hmd *qhmd = using_qhmd ? qsys->hmd : NULL;
	struct qwerty_device *qd_hmd = using_qhmd ? &qhmd->base : NULL;

	NSEventType type = [event type];

	// Handle F/G key state via flagsChanged won't work (they're not modifiers).
	// We track F/G in the keyDown/keyUp handler below.

	// Build target arrays for dual controller support (F+G = both)
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

	// Handle modifier key changes (CTRL/ALT for controller focus)
	if (type == NSEventTypeFlagsChanged) {
		NSUInteger flags = [event modifierFlags];
		bool new_ctrl = (flags & NSEventModifierFlagControl) != 0;
		bool new_alt = (flags & NSEventModifierFlagOption) != 0;

		if (new_ctrl != ctrl_pressed || new_alt != alt_pressed) {
			ctrl_pressed = new_ctrl;
			alt_pressed = new_alt;
			if (using_qhmd) qwerty_release_all(qd_hmd);
			qwerty_release_all(qd_right);
			qwerty_release_all(qd_left);
			last_mouse_pos = [NSEvent mouseLocation];

			// Rebuild targets after focus change
			if (ctrl_pressed && alt_pressed) {
				targets[0] = qd_left; targets[1] = qd_right;
				ctrl_targets[0] = qleft; ctrl_targets[1] = qright;
				target_count = 2;
			} else if (ctrl_pressed) {
				targets[0] = qd_left; ctrl_targets[0] = qleft; target_count = 1;
			} else if (alt_pressed) {
				targets[0] = qd_right; ctrl_targets[0] = qright; target_count = 1;
			} else {
				targets[0] = default_qdev; ctrl_targets[0] = default_qctrl; target_count = 1;
			}
			qsys->lctrl_focused = ctrl_pressed;
			qsys->rctrl_focused = alt_pressed;
			qsys->hmd_focused = (!ctrl_pressed && !alt_pressed && targets[0] == qd_hmd);
		}
		return; // Modifier change consumed
	}

	// Handle key events
	if (type == NSEventTypeKeyDown || type == NSEventTypeKeyUp) {
		bool is_down = (type == NSEventTypeKeyDown);
		unsigned short keyCode = [event keyCode];

		switch (keyCode) {
		// WASDQE Movement
		case kVK_ANSI_W:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_forward(targets[i]);
				else qwerty_release_forward(targets[i]);
			}
			break;
		case kVK_ANSI_A:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_left(targets[i]);
				else qwerty_release_left(targets[i]);
			}
			break;
		case kVK_ANSI_S:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_backward(targets[i]);
				else qwerty_release_backward(targets[i]);
			}
			break;
		case kVK_ANSI_D:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_right(targets[i]);
				else qwerty_release_right(targets[i]);
			}
			break;
		case kVK_ANSI_Q:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_down(targets[i]);
				else qwerty_release_down(targets[i]);
			}
			break;
		case kVK_ANSI_E:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_up(targets[i]);
				else qwerty_release_up(targets[i]);
			}
			break;

		// Arrow keys rotation
		case kVK_LeftArrow:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_look_left(targets[i]);
				else qwerty_release_look_left(targets[i]);
			}
			break;
		case kVK_RightArrow:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_look_right(targets[i]);
				else qwerty_release_look_right(targets[i]);
			}
			break;
		case kVK_UpArrow:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_look_up(targets[i]);
				else qwerty_release_look_up(targets[i]);
			}
			break;
		case kVK_DownArrow:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_look_down(targets[i]);
				else qwerty_release_look_down(targets[i]);
			}
			break;

		// Sprint
		case kVK_Shift:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_sprint(targets[i]);
				else qwerty_release_sprint(targets[i]);
			}
			break;

		// Movement speed
		case kVK_ANSI_KeypadPlus:
			if (is_down) {
				for (int i = 0; i < target_count; i++)
					qwerty_change_movement_speed(targets[i], 1);
			}
			break;
		case kVK_ANSI_KeypadMinus:
			if (is_down) {
				for (int i = 0; i < target_count; i++)
					qwerty_change_movement_speed(targets[i], -1);
			}
			break;

		// Controller buttons
		case kVK_ANSI_N:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_menu(ctrl_targets[i]);
				else qwerty_release_menu(ctrl_targets[i]);
			}
			break;
		case kVK_ANSI_B:
			for (int i = 0; i < target_count; i++) {
				if (is_down) qwerty_press_system(ctrl_targets[i]);
				else qwerty_release_system(ctrl_targets[i]);
			}
			break;

		// Thumbstick + Trackpad (T/F/G/H = up/left/down/right)
		case kVK_ANSI_F:
			for (int i = 0; i < target_count; i++) {
				if (is_down) {
					qwerty_press_thumbstick_left(ctrl_targets[i]);
					qwerty_press_trackpad_left(ctrl_targets[i]);
				} else {
					qwerty_release_thumbstick_left(ctrl_targets[i]);
					qwerty_release_trackpad_left(ctrl_targets[i]);
				}
			}
			break;
		case kVK_ANSI_H:
			for (int i = 0; i < target_count; i++) {
				if (is_down) {
					qwerty_press_thumbstick_right(ctrl_targets[i]);
					qwerty_press_trackpad_right(ctrl_targets[i]);
				} else {
					qwerty_release_thumbstick_right(ctrl_targets[i]);
					qwerty_release_trackpad_right(ctrl_targets[i]);
				}
			}
			break;
		case kVK_ANSI_T:
			for (int i = 0; i < target_count; i++) {
				if (is_down) {
					qwerty_press_thumbstick_up(ctrl_targets[i]);
					qwerty_press_trackpad_up(ctrl_targets[i]);
				} else {
					qwerty_release_thumbstick_up(ctrl_targets[i]);
					qwerty_release_trackpad_up(ctrl_targets[i]);
				}
			}
			break;
		case kVK_ANSI_G:
			for (int i = 0; i < target_count; i++) {
				if (is_down) {
					qwerty_press_thumbstick_down(ctrl_targets[i]);
					qwerty_press_trackpad_down(ctrl_targets[i]);
				} else {
					qwerty_release_thumbstick_down(ctrl_targets[i]);
					qwerty_release_trackpad_down(ctrl_targets[i]);
				}
			}
			break;

		case kVK_ANSI_V:
			if (qsys->hmd_focused) {
				// HMD focused: toggle runtime-side 2D/3D display mode
				if (is_down && ![event isARepeat])
					qwerty_toggle_display_mode(qsys);
			} else {
				// Controller focused: thumbstick click
				for (int i = 0; i < target_count; i++) {
					if (is_down) qwerty_press_thumbstick_click(ctrl_targets[i]);
					else qwerty_release_thumbstick_click(ctrl_targets[i]);
				}
			}
			break;

		// 1/2/3: rendering mode (HMD focused, keydown, no repeat)
		case kVK_ANSI_1:
		case kVK_ANSI_2:
		case kVK_ANSI_3:
			if (is_down && ![event isARepeat] && qsys->hmd_focused)
				qwerty_set_rendering_mode(qsys, keyCode - kVK_ANSI_1);
			break;

		// Controller follow HMD toggle
		case kVK_ANSI_C:
			if (is_down && ![event isARepeat]) {
				if (ctrl_pressed || alt_pressed) {
					for (int i = 0; i < target_count; i++)
						qwerty_follow_hmd(ctrl_targets[i], !ctrl_targets[i]->follow_hmd);
				} else {
					bool both_not_following = !qleft->follow_hmd && !qright->follow_hmd;
					qwerty_follow_hmd(qleft, both_not_following);
					qwerty_follow_hmd(qright, both_not_following);
				}
			}
			break;

		// Reset controller pose
		case kVK_ANSI_R:
			if (is_down && ![event isARepeat]) {
				if (ctrl_pressed || alt_pressed) {
					for (int i = 0; i < target_count; i++)
						qwerty_reset_controller_pose(ctrl_targets[i]);
				} else {
					qwerty_reset_controller_pose(qleft);
					qwerty_reset_controller_pose(qright);
				}
			}
			break;

		case kVK_Tab:
			if (is_down && ![event isARepeat])
				u_hud_toggle();
			break;

		// P = toggle camera/display mode (HMD focused only)
		case kVK_ANSI_P:
			if (is_down && ![event isARepeat] && qsys->hmd_focused)
				qwerty_toggle_camera_mode(qsys);
			break;

		// Spacebar = reset stereo to camera defaults (HMD focused only)
		case kVK_Space:
			if (is_down && ![event isARepeat] && qsys->hmd_focused)
				qwerty_reset_stereo(qsys);
			break;

		default:
			break;
		}
	}

	// Mouse button events
	switch (type) {
	case NSEventTypeRightMouseDown:
		mouse_look_active = true;
		last_mouse_pos = [NSEvent mouseLocation];
		break;

	case NSEventTypeRightMouseUp:
		mouse_look_active = false;
		break;

	case NSEventTypeLeftMouseDown:
		for (int i = 0; i < target_count; i++)
			qwerty_press_trigger(ctrl_targets[i]);
		break;

	case NSEventTypeLeftMouseUp:
		for (int i = 0; i < target_count; i++)
			qwerty_release_trigger(ctrl_targets[i]);
		break;

	case NSEventTypeOtherMouseDown:
		for (int i = 0; i < target_count; i++)
			qwerty_press_squeeze(ctrl_targets[i]);
		break;

	case NSEventTypeOtherMouseUp:
		for (int i = 0; i < target_count; i++)
			qwerty_release_squeeze(ctrl_targets[i]);
		break;

	case NSEventTypeMouseMoved:
	case NSEventTypeLeftMouseDragged:
	case NSEventTypeRightMouseDragged:
	case NSEventTypeOtherMouseDragged: {
		NSPoint current_pos = [NSEvent mouseLocation];
		float dx = (float)(current_pos.x - last_mouse_pos.x);
		float dy = (float)(current_pos.y - last_mouse_pos.y);

		if (dx != 0 || dy != 0) {
			if (mouse_look_active) {
				// RMB held: rotation via mouse look
				// macOS Y is inverted (bottom-up coordinates)
				float yaw = -dx * SENSITIVITY;
				float pitch = dy * SENSITIVITY;
				for (int i = 0; i < target_count; i++)
					qwerty_add_look_delta(targets[i], yaw, pitch);
			} else if (ctrl_pressed || alt_pressed) {
				// Controller focused (no RMB): XY translation
				float pos_dx = dx * POSITION_SENSITIVITY;
				float pos_dy = dy * POSITION_SENSITIVITY;
				for (int i = 0; i < target_count; i++)
					qwerty_add_position_delta(targets[i], pos_dx, pos_dy);
			}
		}

		last_mouse_pos = current_pos;
	} break;

	case NSEventTypeScrollWheel: {
		float delta_y = (float)[event scrollingDeltaY];
		if ([event hasPreciseScrollingDeltas]) {
			delta_y /= 10.0f; // Normalize trackpad scrolling
		}
		if (delta_y != 0) {
			if (qsys->hmd_focused) {
				// HMD focused: mouse wheel + modifiers for stereo controls
				NSUInteger flags = [event modifierFlags];
				if (flags & NSEventModifierFlagShift) {
					float mult = (delta_y > 0) ? 1.1f : (1.0f / 1.1f);
					qwerty_adjust_stereo_factor(qsys, mult);
				} else if (qsys->camera_mode) {
					qwerty_adjust_convergence(qsys, (delta_y > 0) ? 1.0f : -1.0f);
				} else {
					float mult = (delta_y > 0) ? 1.05f : (1.0f / 1.05f);
					qwerty_adjust_vheight(qsys, mult);
				}
			} else {
				// Controller focused: movement speed
				int steps = (delta_y > 0) ? 1 : -1;
				for (int i = 0; i < target_count; i++)
					qwerty_change_movement_speed(targets[i], (float)steps);
			}
		}
	} break;

	default:
		break;
	}
}
