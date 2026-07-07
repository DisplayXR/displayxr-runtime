// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Xlib/X11 input handler for qwerty devices (desktop-Linux GL compositor).
 *
 * The Linux equivalent of qwerty_process_win32 / qwerty_process_macos: it turns
 * X11 keyboard/mouse events from the runtime's self-created compositor window
 * into qwerty device motion, so the runtime-hosted cube can be flown with
 * WASDQE + arrow keys + RMB-drag look, and the display/rendering modes toggled
 * with V / 1 / 2 / 3 — without the SDL debug GUI.
 *
 * Scoped to the HMD (camera) rig; controllers are not driven here (the hosted
 * test app uses none). Mirrors qwerty_win32.c's structure, trimmed. (#660)
 *
 * @ingroup drv_qwerty
 */

#include "qwerty_device.h"
#include "util/u_device.h"
#include "util/u_hud.h"
#include "util/u_logging.h"
#include "xrt/xrt_device.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <string.h>
#include <stdbool.h>

// look_speed units per screen-pixel of mouse motion (matches qwerty_win32.c).
#define SENSITIVITY 0.1f

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
	return qwerty_device(xdev)->sys;
}

void
qwerty_process_xlib(struct xrt_device **xdevs, size_t xdev_count, void *xevent_ptr)
{
	static struct qwerty_system *qsys = NULL;
	static struct qwerty_device *qd_hmd = NULL;
	static bool cached = false;
	static bool mouse_look_active = false;
	static int last_x = 0, last_y = 0;

	if (xevent_ptr == NULL) {
		return;
	}

	if (!cached) {
		qsys = find_qwerty_system(xdevs, xdev_count);
		if (qsys == NULL) {
			return; // No qwerty devices in this session.
		}
		qd_hmd = (qsys->hmd != NULL) ? &qsys->hmd->base : NULL;
		cached = true;
		U_LOG_W("QWERTY Xlib input ready — WASDQE move, arrows look, RMB-drag look, "
		        "Shift sprint, +/- speed, V=2D/3D, 1/2/3=mode, P=cam/display, Space=reset, TAB=HUD");
	}
	if (qsys == NULL || !qsys->process_keys || qd_hmd == NULL) {
		return;
	}

	XEvent *ev = (XEvent *)xevent_ptr;

	switch (ev->type) {
	case KeyPress:
	case KeyRelease: {
		bool down = (ev->type == KeyPress);
		KeySym ks = XLookupKeysym(&ev->xkey, 0);
		switch (ks) {
		// Translation (WASDQE)
		case XK_w: down ? qwerty_press_forward(qd_hmd)  : qwerty_release_forward(qd_hmd);  break;
		case XK_s: down ? qwerty_press_backward(qd_hmd) : qwerty_release_backward(qd_hmd); break;
		case XK_a: down ? qwerty_press_left(qd_hmd)     : qwerty_release_left(qd_hmd);     break;
		case XK_d: down ? qwerty_press_right(qd_hmd)    : qwerty_release_right(qd_hmd);    break;
		case XK_q: down ? qwerty_press_down(qd_hmd)     : qwerty_release_down(qd_hmd);     break;
		case XK_e: down ? qwerty_press_up(qd_hmd)       : qwerty_release_up(qd_hmd);       break;
		// Rotation (arrow keys)
		case XK_Left:  down ? qwerty_press_look_left(qd_hmd)  : qwerty_release_look_left(qd_hmd);  break;
		case XK_Right: down ? qwerty_press_look_right(qd_hmd) : qwerty_release_look_right(qd_hmd); break;
		case XK_Up:    down ? qwerty_press_look_up(qd_hmd)    : qwerty_release_look_up(qd_hmd);    break;
		case XK_Down:  down ? qwerty_press_look_down(qd_hmd)  : qwerty_release_look_down(qd_hmd);  break;
		// Sprint
		case XK_Shift_L:
		case XK_Shift_R: down ? qwerty_press_sprint(qd_hmd) : qwerty_release_sprint(qd_hmd); break;
		// Movement speed (keydown only)
		case XK_plus:
		case XK_equal:
		case XK_KP_Add: if (down) qwerty_change_movement_speed(qd_hmd, 1); break;
		case XK_minus:
		case XK_KP_Subtract: if (down) qwerty_change_movement_speed(qd_hmd, -1); break;
		// Mode hotkeys (keydown only)
		case XK_v: if (down) qwerty_toggle_display_mode(qsys); break;
		case XK_1: if (down) qwerty_set_rendering_mode(qsys, 0); break;
		case XK_2: if (down) qwerty_set_rendering_mode(qsys, 1); break;
		case XK_3: if (down) qwerty_set_rendering_mode(qsys, 2); break;
		case XK_p: if (down) qwerty_toggle_camera_mode(qsys); break;
		case XK_space: if (down) qwerty_reset_view_state(qsys); break;
		case XK_Tab: if (down) u_hud_toggle(); break;
		default: break;
		}
		break;
	}
	case ButtonPress:
		if (ev->xbutton.button == Button3) { // RMB → mouse-look
			mouse_look_active = true;
			last_x = ev->xbutton.x;
			last_y = ev->xbutton.y;
		}
		break;
	case ButtonRelease:
		if (ev->xbutton.button == Button3) {
			mouse_look_active = false;
		}
		break;
	case MotionNotify:
		if (mouse_look_active) {
			int dx = ev->xmotion.x - last_x;
			int dy = ev->xmotion.y - last_y;
			if (dx != 0 || dy != 0) {
				qwerty_add_look_delta(qd_hmd, (float)(-dx) * SENSITIVITY, (float)(-dy) * SENSITIVITY);
			}
		}
		last_x = ev->xmotion.x;
		last_y = ev->xmotion.y;
		break;
	default: break;
	}
}
