// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief X11 input handler for qwerty devices (Linux vk_native self-created window).
 *
 * The Linux equivalent of qwerty_win32.c: keyboard and mouse input that lands
 * on the runtime's self-created XCB window (hosted apps, and every OpenXR CTS
 * session) drives the qwerty devices. The key and mouse map is qwerty_win32.c's,
 * translated from Win32 virtual keys to level-0 X keysyms — keep the two in
 * step when either changes.
 *
 * The window decodes xcb events into a @ref qwerty_x11_input (keycode → keysym,
 * root coordinates) so this file needs no libxcb, only the keysym values from
 * <X11/keysym.h>.
 *
 * Like the Win32 handler this is only reached when the runtime owns the window;
 * an app that binds its own X11 window (XR_DXR_xlib_window_binding) handles its
 * own input.
 *
 * @ingroup drv_qwerty
 */

#include "qwerty_device.h"
#include "qwerty_interface.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_hud.h"
#include "util/u_logging.h"
#include "xrt/xrt_device.h"

#include <X11/keysym.h>

#include <stdbool.h>
#include <string.h>

// [QTRACE] pose/select-path tracer, off unless DXR_QTRACE=1 (docs/reference/debug-logging.md)
DEBUG_GET_ONCE_BOOL_OPTION(qxcb_qtrace, "DXR_QTRACE", false)

// Same tuning as qwerty_win32.c.
#define SENSITIVITY 0.1f
#define POSITION_SENSITIVITY 0.2f

// X core protocol modifier bits (xproto.h), spelled out so this file does not
// need the Xlib or xcb headers.
#define QXCB_SHIFT_MASK (1u << 0)
#define QXCB_CONTROL_MASK (1u << 2)
#define QXCB_MOD1_MASK (1u << 3) // Alt on every stock keymap.

// X core pointer buttons.
#define QXCB_BUTTON_LEFT 1
#define QXCB_BUTTON_MIDDLE 2
#define QXCB_BUTTON_RIGHT 3
#define QXCB_BUTTON_WHEEL_UP 4
#define QXCB_BUTTON_WHEEL_DOWN 5

static struct qwerty_system *
find_qwerty_system(struct xrt_device **xdevs, size_t xdev_count)
{
	for (size_t i = 0; i < xdev_count; i++) {
		// Same tracking_origin guard as the Win32/macOS front-ends (#59, #1538).
		if (xdevs[i] == NULL || xdevs[i]->tracking_origin == NULL) {
			continue;
		}
		const char *tracker_name = xdevs[i]->tracking_origin->name;
		if (strcmp(tracker_name, QWERTY_HMD_TRACKER_STR) == 0 ||
		    strcmp(tracker_name, QWERTY_LEFT_TRACKER_STR) == 0 ||
		    strcmp(tracker_name, QWERTY_RIGHT_TRACKER_STR) == 0) {
			return qwerty_device(xdevs[i])->sys;
		}
	}
	return NULL;
}

//! Mirror of qwerty_win32.c's default_qwerty_device.
static struct qwerty_device *
default_qwerty_device(struct xrt_device **xdevs, size_t xdev_count, struct qwerty_system *qsys)
{
	int head, left, right, gamepad;
	head = left = right = gamepad = XRT_DEVICE_ROLE_UNASSIGNED;
	u_device_assign_xdev_roles(xdevs, xdev_count, &head, &left, &right, &gamepad);

	struct xrt_device *xd_hmd = qsys->hmd ? &qsys->hmd->base.base : NULL;
	struct xrt_device *xd_left = &qsys->lctrl->base.base;
	struct xrt_device *xd_right = &qsys->rctrl->base.base;

	if (xd_hmd != NULL) {
		return qwerty_device(xd_hmd);
	}
	if (right >= 0 && (size_t)right < xdev_count && xdevs[right] == xd_right) {
		return qwerty_device(xd_right);
	}
	if (left >= 0 && (size_t)left < xdev_count && xdevs[left] == xd_left) {
		return qwerty_device(xd_left);
	}
	return qwerty_device(xd_right);
}

//! Mirror of qwerty_win32.c's default_qwerty_controller.
static struct qwerty_controller *
default_qwerty_controller(struct xrt_device **xdevs, size_t xdev_count, struct qwerty_system *qsys)
{
	int head, left, right, gamepad;
	head = left = right = gamepad = XRT_DEVICE_ROLE_UNASSIGNED;
	u_device_assign_xdev_roles(xdevs, xdev_count, &head, &left, &right, &gamepad);

	struct xrt_device *xd_left = &qsys->lctrl->base.base;
	struct xrt_device *xd_right = &qsys->rctrl->base.base;

	if (right >= 0 && (size_t)right < xdev_count && xdevs[right] == xd_right) {
		return qwerty_controller(xd_right);
	}
	if (left >= 0 && (size_t)left < xdev_count && xdevs[left] == xd_left) {
		return qwerty_controller(xd_left);
	}
	return qwerty_controller(xd_right);
}

static bool
is_ctrl_keysym(uint32_t ks)
{
	return ks == XK_Control_L || ks == XK_Control_R;
}

static bool
is_alt_keysym(uint32_t ks)
{
	return ks == XK_Alt_L || ks == XK_Alt_R || ks == XK_Meta_L || ks == XK_Meta_R;
}

static const char *
type_name(enum qwerty_x11_input_type t)
{
	switch (t) {
	case QWERTY_X11_KEY_PRESS: return "KEY_PRESS";
	case QWERTY_X11_KEY_RELEASE: return "KEY_RELEASE";
	case QWERTY_X11_BUTTON_PRESS: return "BUTTON_PRESS";
	case QWERTY_X11_BUTTON_RELEASE: return "BUTTON_RELEASE";
	case QWERTY_X11_MOTION: return "MOTION";
	case QWERTY_X11_FOCUS_IN: return "FOCUS_IN";
	case QWERTY_X11_FOCUS_OUT: return "FOCUS_OUT";
	}
	return "?";
}

void
qwerty_process_xcb(struct xrt_device **xdevs, size_t xdev_count, const struct qwerty_x11_input *input)
{
	if (input == NULL) {
		return;
	}

	// Resolved from the caller's xdevs on every call, never cached (#1538).
	struct qwerty_system *qsys = find_qwerty_system(xdevs, xdev_count);
	if (qsys == NULL) {
		return;
	}

	const bool qtrace = debug_get_bool_option_qxcb_qtrace();

	// Snapshot the front-end state (struct qwerty_system § "Platform input
	// front-end state"). input_lock is a LEAF: every qwerty_press_* /
	// qwerty_release_* below takes its own lock and runs with it released.
	bool first_bind = false;
	os_mutex_lock(&qsys->input_lock);
	if (!qsys->input_bound) {
		qsys->input_default_qdev = default_qwerty_device(xdevs, xdev_count, qsys);
		qsys->input_default_qctrl = default_qwerty_controller(xdevs, xdev_count, qsys);
		qsys->input_last_mouse_x = (float)input->root_x;
		qsys->input_last_mouse_y = (float)input->root_y;
		// #1700: buttons start UP and only a ButtonPress EVENT moves them down.
		// There is no live button state consulted anywhere in this file.
		qsys->input_lmb_was_down = false;
		qsys->input_mmb_was_down = false;
		qsys->input_bound = true;
		first_bind = true;
	}
	const bool process_keys = qsys->process_keys;
	struct qwerty_device *default_qdev = qsys->input_default_qdev;
	struct qwerty_controller *default_qctrl = qsys->input_default_qctrl;
	bool ctrl_pressed = qsys->input_ctrl_pressed;
	bool alt_pressed = qsys->input_alt_pressed;
	bool mouse_look_active = qsys->input_mouse_look_active;
	int32_t last_x = (int32_t)qsys->input_last_mouse_x;
	int32_t last_y = (int32_t)qsys->input_last_mouse_y;
	bool lmb_was_down = qsys->input_lmb_was_down;
	bool mmb_was_down = qsys->input_mmb_was_down;
	os_mutex_unlock(&qsys->input_lock);

	if (first_bind) {
		U_LOG_W(
		    "QWERTY X11 input bound to qwerty system %p by %s - WASDQE move, RMB+drag look, "
		    "CTRL/ALT controller focus, LMB trigger (select)",
		    (void *)qsys, type_name(input->type));
	}

	// Every event except pointer motion (the one per-frame event) is traced;
	// motion's button-driven edges cannot exist here, so there is nothing to miss.
	if (qtrace && input->type != QWERTY_X11_MOTION) {
		U_LOG_W("[QTRACE] XCB EVT %s keysym=0x%x button=%u state=0x%x", type_name(input->type), input->keysym,
		        input->button, input->state);
	}

	if (!process_keys) {
		return;
	}

#define QXCB_STORE_STATE()                                                                                             \
	do {                                                                                                           \
		os_mutex_lock(&qsys->input_lock);                                                                      \
		qsys->input_ctrl_pressed = ctrl_pressed;                                                               \
		qsys->input_alt_pressed = alt_pressed;                                                                 \
		qsys->input_mouse_look_active = mouse_look_active;                                                     \
		qsys->input_last_mouse_x = (float)last_x;                                                              \
		qsys->input_last_mouse_y = (float)last_y;                                                              \
		qsys->input_lmb_was_down = lmb_was_down;                                                               \
		qsys->input_mmb_was_down = mmb_was_down;                                                               \
		os_mutex_unlock(&qsys->input_lock);                                                                    \
	} while (0)

	struct qwerty_controller *qleft = qsys->lctrl;
	struct qwerty_device *qd_left = &qleft->base;
	struct qwerty_controller *qright = qsys->rctrl;
	struct qwerty_device *qd_right = &qright->base;
	const bool using_qhmd = qsys->hmd != NULL;
	struct qwerty_device *qd_hmd = using_qhmd ? &qsys->hmd->base : NULL;

	// Focus loss / unmap / teardown: X delivers no KeyRelease or ButtonRelease
	// to a window that no longer has focus or no longer exists, so anything held
	// would latch. Release it all — the #1700 lesson: a trigger held across a
	// session boundary must never become the next session's select.
	if (input->type == QWERTY_X11_FOCUS_OUT) {
		if (lmb_was_down) {
			qwerty_release_trigger(qleft);
			qwerty_release_trigger(qright);
			lmb_was_down = false;
		}
		if (mmb_was_down) {
			qwerty_release_squeeze(qleft);
			qwerty_release_squeeze(qright);
			mmb_was_down = false;
		}
		mouse_look_active = false;
		if (using_qhmd) {
			qwerty_release_all(qd_hmd);
		}
		qwerty_release_all(qd_right);
		qwerty_release_all(qd_left);
		ctrl_pressed = false;
		alt_pressed = false;
		QXCB_STORE_STATE();
		return;
	}
	if (input->type == QWERTY_X11_FOCUS_IN) {
		// Win32 WM_SETFOCUS: re-sync CTRL/ALT from the live modifier state so a
		// chord held into the window stays active. Buttons are NOT re-synced.
		const bool ctrl_now = (input->state & QXCB_CONTROL_MASK) != 0;
		const bool alt_now = (input->state & QXCB_MOD1_MASK) != 0;
		if (ctrl_pressed != ctrl_now || alt_pressed != alt_now) {
			if (using_qhmd) {
				qwerty_release_all(qd_hmd);
			}
			qwerty_release_all(qd_right);
			qwerty_release_all(qd_left);
			ctrl_pressed = ctrl_now;
			alt_pressed = alt_now;
		}
		last_x = input->root_x;
		last_y = input->root_y;
		QXCB_STORE_STATE();
		return;
	}

	const bool is_keydown = input->type == QWERTY_X11_KEY_PRESS;
	const bool is_keyup = input->type == QWERTY_X11_KEY_RELEASE;
	const uint32_t ks = input->keysym;

	// CTRL = left controller focus, ALT = right, both = both hands.
	if (is_keydown || is_keyup) {
		bool focus_change = false;
		if (is_ctrl_keysym(ks) && ctrl_pressed != is_keydown) {
			ctrl_pressed = is_keydown;
			focus_change = true;
		}
		if (is_alt_keysym(ks) && alt_pressed != is_keydown) {
			alt_pressed = is_keydown;
			focus_change = true;
		}
		if (focus_change) {
			if (using_qhmd) {
				qwerty_release_all(qd_hmd);
			}
			qwerty_release_all(qd_right);
			qwerty_release_all(qd_left);
			// Reset the mouse baseline so the focus switch does not jump.
			last_x = input->root_x;
			last_y = input->root_y;
		}
	}

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

	qsys->lctrl_focused = ctrl_pressed;
	qsys->rctrl_focused = alt_pressed;
	qsys->hmd_focused = (!ctrl_pressed && !alt_pressed && targets[0] == qd_hmd);

#define FOR_TARGETS(press_fn, release_fn)                                                                              \
	for (int i = 0; i < target_count; i++) {                                                                       \
		if (is_keydown)                                                                                        \
			press_fn(targets[i]);                                                                          \
		else                                                                                                   \
			release_fn(targets[i]);                                                                        \
	}
#define FOR_CTRL_TARGETS(press_fn, release_fn)                                                                         \
	for (int i = 0; i < target_count; i++) {                                                                       \
		if (is_keydown)                                                                                        \
			press_fn(ctrl_targets[i]);                                                                     \
		else                                                                                                   \
			release_fn(ctrl_targets[i]);                                                                   \
	}

	if (is_keydown || is_keyup) {
		switch (ks) {
		// WASDQE movement
		case XK_w: FOR_TARGETS(qwerty_press_forward, qwerty_release_forward); break;
		case XK_a: FOR_TARGETS(qwerty_press_left, qwerty_release_left); break;
		case XK_s: FOR_TARGETS(qwerty_press_backward, qwerty_release_backward); break;
		case XK_d: FOR_TARGETS(qwerty_press_right, qwerty_release_right); break;
		case XK_q: FOR_TARGETS(qwerty_press_down, qwerty_release_down); break;
		case XK_e: FOR_TARGETS(qwerty_press_up, qwerty_release_up); break;

		// Arrow keys: pitch / yaw
		case XK_Left: FOR_TARGETS(qwerty_press_look_left, qwerty_release_look_left); break;
		case XK_Right: FOR_TARGETS(qwerty_press_look_right, qwerty_release_look_right); break;
		case XK_Up: FOR_TARGETS(qwerty_press_look_up, qwerty_release_look_up); break;
		case XK_Down: FOR_TARGETS(qwerty_press_look_down, qwerty_release_look_down); break;

		// Z/X roll (#1692)
		case XK_z: FOR_TARGETS(qwerty_press_roll_left, qwerty_release_roll_left); break;
		case XK_x: FOR_TARGETS(qwerty_press_roll_right, qwerty_release_roll_right); break;

		// Sprint (Win32 VK_SHIFT covers both shifts)
		case XK_Shift_L:
		case XK_Shift_R: FOR_TARGETS(qwerty_press_sprint, qwerty_release_sprint); break;

		// Movement speed (numpad +/-, Win32 VK_ADD / VK_SUBTRACT)
		case XK_KP_Add:
			if (is_keydown) {
				for (int i = 0; i < target_count; i++)
					qwerty_change_movement_speed(targets[i], 1);
			}
			break;
		case XK_KP_Subtract:
			if (is_keydown) {
				for (int i = 0; i < target_count; i++)
					qwerty_change_movement_speed(targets[i], -1);
			}
			break;

		// Controller buttons: N = menu, B = system
		case XK_n: FOR_CTRL_TARGETS(qwerty_press_menu, qwerty_release_menu); break;
		case XK_b: FOR_CTRL_TARGETS(qwerty_press_system, qwerty_release_system); break;

		// Thumbstick + trackpad (T/F/G/H = up/left/down/right)
		case XK_f:
			FOR_CTRL_TARGETS(qwerty_press_thumbstick_left, qwerty_release_thumbstick_left);
			FOR_CTRL_TARGETS(qwerty_press_trackpad_left, qwerty_release_trackpad_left);
			break;
		case XK_h:
			FOR_CTRL_TARGETS(qwerty_press_thumbstick_right, qwerty_release_thumbstick_right);
			FOR_CTRL_TARGETS(qwerty_press_trackpad_right, qwerty_release_trackpad_right);
			break;
		case XK_t:
			FOR_CTRL_TARGETS(qwerty_press_thumbstick_up, qwerty_release_thumbstick_up);
			FOR_CTRL_TARGETS(qwerty_press_trackpad_up, qwerty_release_trackpad_up);
			break;
		case XK_g:
			FOR_CTRL_TARGETS(qwerty_press_thumbstick_down, qwerty_release_thumbstick_down);
			FOR_CTRL_TARGETS(qwerty_press_trackpad_down, qwerty_release_trackpad_down);
			break;

		case XK_v:
			if (qsys->hmd_focused) {
				// HMD focused: runtime-side 2D/3D toggle
				if (is_keydown)
					qwerty_toggle_display_mode(qsys);
			} else {
				FOR_CTRL_TARGETS(qwerty_press_thumbstick_click, qwerty_release_thumbstick_click);
			}
			break;

		// 1/2/3: rendering mode (HMD focused, keydown only)
		case XK_1:
		case XK_2:
		case XK_3:
			if (is_keydown && qsys->hmd_focused)
				qwerty_set_rendering_mode(qsys, (int)(ks - XK_1));
			break;

		// C: controllers follow the HMD
		case XK_c:
			if (is_keydown) {
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

		// R: reset controller pose
		case XK_r:
			if (is_keydown) {
				if (ctrl_pressed || alt_pressed) {
					for (int i = 0; i < target_count; i++)
						qwerty_reset_controller_pose(ctrl_targets[i]);
				} else {
					qwerty_reset_controller_pose(qleft);
					qwerty_reset_controller_pose(qright);
				}
			}
			break;

		// TAB toggles the runtime HUD
		case XK_Tab:
			if (is_keydown)
				u_hud_toggle();
			break;

		// P: camera/display rig toggle (HMD focused)
		case XK_p:
			if (is_keydown && qsys->hmd_focused)
				qwerty_toggle_camera_mode(qsys);
			break;

		// Space: reset the rig to its defaults (HMD focused)
		case XK_space:
			if (is_keydown && qsys->hmd_focused)
				qwerty_reset_view_state(qsys);
			break;

		// ESC: no-op, as on Win32.
		case XK_Escape: break;

		default: break;
		}
	}

#undef FOR_TARGETS
#undef FOR_CTRL_TARGETS

	// Mouse: LMB = trigger (select), MMB = squeeze, RMB = mouse look, wheel.
	// Every button action below is gated on the latch, so a press while down or
	// a release while up is not an edge and never reaches the device (#1700).
	if (input->type == QWERTY_X11_BUTTON_PRESS || input->type == QWERTY_X11_BUTTON_RELEASE) {
		const bool down = input->type == QWERTY_X11_BUTTON_PRESS;
		switch (input->button) {
		case QXCB_BUTTON_LEFT:
			if (down != lmb_was_down) {
				for (int i = 0; i < target_count; i++) {
					if (down)
						qwerty_press_trigger(ctrl_targets[i]);
					else
						qwerty_release_trigger(ctrl_targets[i]);
				}
				lmb_was_down = down;
				if (qtrace) {
					U_LOG_W("[QTRACE] XCB LMB %s edge ntargets=%d target0=%p", down ? "DOWN" : "UP",
					        target_count, (void *)ctrl_targets[0]);
				}
			} else if (qtrace) {
				U_LOG_W("[QTRACE] XCB LMB %s dropped (already %s)", down ? "DOWN" : "UP",
				        down ? "down" : "up");
			}
			break;
		case QXCB_BUTTON_MIDDLE:
			if (down != mmb_was_down) {
				for (int i = 0; i < target_count; i++) {
					if (down)
						qwerty_press_squeeze(ctrl_targets[i]);
					else
						qwerty_release_squeeze(ctrl_targets[i]);
				}
				mmb_was_down = down;
			}
			break;
		case QXCB_BUTTON_RIGHT:
			// X holds an implicit pointer grab while a button is down, so
			// motion keeps arriving outside the window (Win32 SetCapture).
			mouse_look_active = down;
			last_x = input->root_x;
			last_y = input->root_y;
			break;
		case QXCB_BUTTON_WHEEL_UP:
		case QXCB_BUTTON_WHEEL_DOWN: {
			// X reports each wheel notch as a press/release pair; act on the press.
			if (!down) {
				break;
			}
			const int steps = input->button == QXCB_BUTTON_WHEEL_UP ? 1 : -1;
			if (qsys->hmd_focused) {
				const bool shift = (input->state & QXCB_SHIFT_MASK) != 0;
				if (shift) {
					qwerty_adjust_view_factor(qsys, steps > 0 ? 1.1f : (1.0f / 1.1f));
				} else if (qwerty_is_camera_mode(qsys)) {
					qwerty_adjust_convergence(qsys, steps > 0 ? 1.0f : -1.0f);
				} else {
					qwerty_adjust_vheight(qsys, steps > 0 ? 1.05f : (1.0f / 1.05f));
				}
			} else {
				for (int i = 0; i < target_count; i++)
					qwerty_change_movement_speed(targets[i], (float)steps);
			}
		} break;
		default: break; // 6/7 (horizontal wheel) and extra buttons: unmapped, as on Win32.
		}
	}

	// Pointer motion: look while RMB is held, XY translate while a controller is
	// focused. Button bits in the motion state are deliberately ignored.
	if (input->type == QWERTY_X11_MOTION || input->type == QWERTY_X11_BUTTON_PRESS ||
	    input->type == QWERTY_X11_BUTTON_RELEASE) {
		const int dx = input->root_x - last_x;
		const int dy = input->root_y - last_y;
		if (input->type == QWERTY_X11_MOTION && (dx != 0 || dy != 0)) {
			if (mouse_look_active) {
				const float yaw = (float)(-dx) * SENSITIVITY;
				const float pitch = (float)(-dy) * SENSITIVITY;
				for (int i = 0; i < target_count; i++)
					qwerty_add_look_delta(targets[i], yaw, pitch);
			} else if (ctrl_pressed || alt_pressed) {
				const float pos_dx = (float)dx * POSITION_SENSITIVITY;
				const float pos_dy = (float)(-dy) * POSITION_SENSITIVITY;
				for (int i = 0; i < target_count; i++)
					qwerty_add_position_delta(targets[i], pos_dx, pos_dy);
			}
		}
		last_x = input->root_x;
		last_y = input->root_y;
	}

	QXCB_STORE_STATE();
#undef QXCB_STORE_STATE
}
