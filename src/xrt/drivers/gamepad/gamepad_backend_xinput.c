// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows XInput backend for the gamepad provider.
 *
 * XInput is the one pad API every Windows box has (the `xinput9_1_0`
 * import resolves on Windows 7 and later; `XInput1_4` would add nothing we
 * use). It is focus-INDEPENDENT — `XInputGetState` reads the pad whatever
 * window is in front — which is the whole reason a pad can drive the rig for
 * a handle app whose own window owns the keyboard (`oxr_input.c`,
 * `has_external_window`).
 *
 * Hot-plug: Microsoft's guidance is not to poll an EMPTY slot every frame
 * (the query is slow when nothing is there). The hub already polls slowly
 * while disconnected; here we additionally remember the slot that was last
 * connected and try it first, and scan the other three only when it is gone.
 *
 * This is one of the two Windows readers: `gamepad_backend_win.c` owns the
 * `gamepad_backend_*` API and dispatches between it and the HID reader
 * (`gamepad_backend_hid_win.c`, Sony pads, which have no XInput node).
 *
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_WINDOWS

#include "gamepad_backend_win.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>

static DWORD g_slot = 0;
static bool g_slot_connected = false;

bool
gamepad_xinput_init(void)
{
	g_slot = 0;
	g_slot_connected = false;
	return true;
}

void
gamepad_xinput_fini(void)
{
	if (g_slot_connected) {
		XINPUT_VIBRATION off = {0, 0};
		XInputSetState(g_slot, &off);
	}
	g_slot_connected = false;
}

static float
stick_axis(SHORT v)
{
	// −32768..32767 → −1..1; the asymmetric range is why the negative side divides by 32768.
	return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

static void
fill_state(const XINPUT_GAMEPAD *g, struct gamepad_state *out)
{
	uint32_t b = 0;
	if (g->wButtons & XINPUT_GAMEPAD_A) {
		b |= GAMEPAD_BTN_A;
	}
	if (g->wButtons & XINPUT_GAMEPAD_B) {
		b |= GAMEPAD_BTN_B;
	}
	if (g->wButtons & XINPUT_GAMEPAD_X) {
		b |= GAMEPAD_BTN_X;
	}
	if (g->wButtons & XINPUT_GAMEPAD_Y) {
		b |= GAMEPAD_BTN_Y;
	}
	if (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) {
		b |= GAMEPAD_BTN_SHOULDER_LEFT;
	}
	if (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) {
		b |= GAMEPAD_BTN_SHOULDER_RIGHT;
	}
	if (g->wButtons & XINPUT_GAMEPAD_LEFT_THUMB) {
		b |= GAMEPAD_BTN_THUMB_LEFT;
	}
	if (g->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) {
		b |= GAMEPAD_BTN_THUMB_RIGHT;
	}
	if (g->wButtons & XINPUT_GAMEPAD_START) {
		b |= GAMEPAD_BTN_MENU;
	}
	if (g->wButtons & XINPUT_GAMEPAD_BACK) {
		b |= GAMEPAD_BTN_VIEW;
	}
	if (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) {
		b |= GAMEPAD_BTN_DPAD_UP;
	}
	if (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) {
		b |= GAMEPAD_BTN_DPAD_DOWN;
	}
	if (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) {
		b |= GAMEPAD_BTN_DPAD_LEFT;
	}
	if (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) {
		b |= GAMEPAD_BTN_DPAD_RIGHT;
	}
	out->buttons = b;
	out->left_x = stick_axis(g->sThumbLX);
	out->left_y = stick_axis(g->sThumbLY);
	out->right_x = stick_axis(g->sThumbRX);
	out->right_y = stick_axis(g->sThumbRY);
	out->left_trigger = (float)g->bLeftTrigger / 255.0f;
	out->right_trigger = (float)g->bRightTrigger / 255.0f;
}

void
gamepad_xinput_poll(struct gamepad_state *out)
{
	XINPUT_STATE st;

	// The known slot first; a miss there means a scan of the others.
	ZeroMemory(&st, sizeof(st));
	if (XInputGetState(g_slot, &st) == ERROR_SUCCESS) {
		g_slot_connected = true;
		out->connected = true;
		fill_state(&st.Gamepad, out);
		return;
	}
	g_slot_connected = false;

	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
		if (i == g_slot) {
			continue;
		}
		ZeroMemory(&st, sizeof(st));
		if (XInputGetState(i, &st) == ERROR_SUCCESS) {
			g_slot = i;
			g_slot_connected = true;
			out->connected = true;
			fill_state(&st.Gamepad, out);
			return;
		}
	}

	out->connected = false;
	out->buttons = 0;
	out->left_x = out->left_y = out->right_x = out->right_y = 0.0f;
	out->left_trigger = out->right_trigger = 0.0f;
}

void
gamepad_xinput_rumble(float low_frequency, float high_frequency)
{
	if (!g_slot_connected) {
		return;
	}
	if (!(low_frequency == low_frequency)) {
		low_frequency = 0.0f;
	}
	if (!(high_frequency == high_frequency)) {
		high_frequency = 0.0f;
	}
	if (low_frequency < 0.0f) {
		low_frequency = 0.0f;
	}
	if (high_frequency < 0.0f) {
		high_frequency = 0.0f;
	}
	if (low_frequency > 1.0f) {
		low_frequency = 1.0f;
	}
	if (high_frequency > 1.0f) {
		high_frequency = 1.0f;
	}
	XINPUT_VIBRATION vib;
	vib.wLeftMotorSpeed = (WORD)(low_frequency * 65535.0f);
	vib.wRightMotorSpeed = (WORD)(high_frequency * 65535.0f);
	XInputSetState(g_slot, &vib);
}

#else
// Keep the translation unit non-empty where this backend is compiled out (-Wempty-translation-unit).
typedef int gamepad_backend_xinput_not_built_here_t;
#endif // XRT_OS_WINDOWS
