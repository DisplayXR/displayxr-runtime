// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Dev probe for the Windows gamepad backend (XInput + HID, #2472).
 *
 * Runs the real backend exactly as the hub does — init, poll at 250 Hz —
 * and prints the live reader (`gamepad_backend_name()`) plus the pad state
 * at 2 Hz for 15 s, then exits. With `--rumble` it also pulses both motors
 * for half a second at the start. Not a test (it needs a pad and a person);
 * target `gamepad_backend_probe`, never built by default.
 */

#include "gamepad_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <string.h>

static void
print_buttons(uint32_t b)
{
	static const struct
	{
		uint32_t bit;
		const char *name;
	} names[] = {
	    {GAMEPAD_BTN_A, "A"},
	    {GAMEPAD_BTN_B, "B"},
	    {GAMEPAD_BTN_X, "X"},
	    {GAMEPAD_BTN_Y, "Y"},
	    {GAMEPAD_BTN_SHOULDER_LEFT, "LB"},
	    {GAMEPAD_BTN_SHOULDER_RIGHT, "RB"},
	    {GAMEPAD_BTN_THUMB_LEFT, "LS"},
	    {GAMEPAD_BTN_THUMB_RIGHT, "RS"},
	    {GAMEPAD_BTN_MENU, "Menu"},
	    {GAMEPAD_BTN_VIEW, "View"},
	    {GAMEPAD_BTN_DPAD_UP, "Up"},
	    {GAMEPAD_BTN_DPAD_DOWN, "Down"},
	    {GAMEPAD_BTN_DPAD_LEFT, "Left"},
	    {GAMEPAD_BTN_DPAD_RIGHT, "Right"},
	    {GAMEPAD_BTN_GUIDE, "Guide"},
	};
	printf("buttons=0x%04X [", (unsigned)b);
	bool first = true;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (b & names[i].bit) {
			printf("%s%s", first ? "" : " ", names[i].name);
			first = false;
		}
	}
	printf("]");
}

int
main(int argc, char **argv)
{
	const bool rumble = argc > 1 && strcmp(argv[1], "--rumble") == 0;

	if (!gamepad_backend_available() || !gamepad_backend_init()) {
		printf("backend init failed\n");
		return 1;
	}

	const ULONGLONG start = GetTickCount64();
	ULONGLONG next_print = start;
	bool rumbling = false;
	bool rumbled = false;

	while (GetTickCount64() - start < 15000) {
		struct gamepad_state s;
		memset(&s, 0, sizeof(s));
		gamepad_backend_poll(&s);
		const ULONGLONG now = GetTickCount64();

		if (rumble && s.connected && !rumbled) {
			gamepad_backend_rumble(1.0f, 1.0f);
			rumbled = rumbling = true;
		}
		if (rumbling && now - start > 500) {
			gamepad_backend_rumble(0.0f, 0.0f);
			rumbling = false;
		}

		if (now >= next_print) {
			next_print += 500;
			printf("t=%5.1fs %-15s connected=%d LX=%+.3f LY=%+.3f RX=%+.3f RY=%+.3f LT=%.3f RT=%.3f ",
			       (double)(now - start) / 1000.0, gamepad_backend_name(), s.connected ? 1 : 0,
			       (double)s.left_x, (double)s.left_y, (double)s.right_x, (double)s.right_y,
			       (double)s.left_trigger, (double)s.right_trigger);
			print_buttons(s.buttons);
			printf("\n");
			fflush(stdout);
		}
		Sleep(s.connected ? 4 : 100);
	}

	if (rumbling) {
		gamepad_backend_rumble(0.0f, 0.0f);
	}
	gamepad_backend_fini();
	return 0;
}
