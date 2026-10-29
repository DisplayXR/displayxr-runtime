// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows gamepad backend: per-device dispatch between XInput and HID.
 *
 * Implements the @ref gamepad_backend_poll API on top of the two Windows
 * readers (`gamepad_backend_win.h`). XInput stays first-class: whenever an
 * XInput pad is connected it is the pad, exactly as before this file existed.
 * The HID reader (Sony DualSense / DualSense Edge / DualShock 4, which have no
 * XInput node) takes over only while no XInput pad is connected. A pad a
 * remapper exposes both ways (a virtual XInput pad over the raw HID one)
 * therefore reads once, through XInput.
 *
 * While HID is live the hub polls at 250 Hz, but XInput is only re-probed
 * once a second: an empty-slot `XInputGetState` scan is the slow query
 * Microsoft warns about, and a second is plenty for "an Xbox pad was just
 * plugged in beside the DualSense".
 *
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_WINDOWS

#include "gamepad_backend.h"
#include "gamepad_backend_win.h"

#include "util/u_logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string.h>

//! XInput re-probe spacing while the HID reader is live.
#define XINPUT_REPROBE_MS 1000ULL

enum live_reader
{
	LIVE_NONE = 0,
	LIVE_XINPUT,
	LIVE_HID,
};

static enum live_reader g_live = LIVE_NONE;
static bool g_hid_ok = false;
static ULONGLONG g_next_xinput_ms = 0;

bool
gamepad_backend_available(void)
{
	return true;
}

bool
gamepad_backend_init(void)
{
	g_live = LIVE_NONE;
	g_next_xinput_ms = 0;
	if (!gamepad_xinput_init()) {
		return false;
	}
	g_hid_ok = gamepad_hid_init();
	return true;
}

void
gamepad_backend_fini(void)
{
	gamepad_xinput_fini();
	if (g_hid_ok) {
		gamepad_hid_fini();
	}
	g_hid_ok = false;
	g_live = LIVE_NONE;
}

void
gamepad_backend_poll(struct gamepad_state *out)
{
	const ULONGLONG now = GetTickCount64();

	if (g_live != LIVE_HID || now >= g_next_xinput_ms) {
		struct gamepad_state x;
		memset(&x, 0, sizeof(x));
		gamepad_xinput_poll(&x);
		if (x.connected) {
			if (g_live == LIVE_HID) {
				gamepad_hid_rumble(0.0f, 0.0f);
				U_LOG_W("gamepad: an XInput pad appeared — it takes over from %s.", gamepad_hid_name());
			}
			g_live = LIVE_XINPUT;
			const int64_t ts = out->timestamp_ns;
			*out = x;
			out->timestamp_ns = ts;
			return;
		}
		g_next_xinput_ms = now + XINPUT_REPROBE_MS;
	}

	if (g_hid_ok) {
		gamepad_hid_poll(out);
	} else {
		out->connected = false;
	}
	if (out->connected) {
		g_live = LIVE_HID;
		return;
	}

	g_live = LIVE_NONE;
	out->buttons = 0;
	out->left_x = out->left_y = out->right_x = out->right_y = 0.0f;
	out->left_trigger = out->right_trigger = 0.0f;
}

void
gamepad_backend_rumble(float low_frequency, float high_frequency)
{
	switch (g_live) {
	case LIVE_XINPUT: gamepad_xinput_rumble(low_frequency, high_frequency); break;
	case LIVE_HID: gamepad_hid_rumble(low_frequency, high_frequency); break;
	default: break;
	}
}

const char *
gamepad_backend_name(void)
{
	switch (g_live) {
	case LIVE_XINPUT: return "xinput";
	case LIVE_HID: return gamepad_hid_name();
	default: return "none";
	}
}

#else
// Keep the translation unit non-empty where this backend is compiled out (-Wempty-translation-unit).
typedef int gamepad_backend_win_not_built_here_t;
#endif // XRT_OS_WINDOWS
