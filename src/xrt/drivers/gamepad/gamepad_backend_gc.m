// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS GameController.framework backend for the gamepad provider.
 *
 * `GCController.controllers` is a snapshot the framework keeps current
 * from its own discovery (Bluetooth and USB pads alike, Xbox / DualShock /
 * DualSense / MFi), so polling it needs no run loop of our own. Element
 * values on a `GCExtendedGamepad` are readable from any thread.
 *
 * Focus independence: by default macOS only delivers pad input to the
 * frontmost application. `shouldMonitorBackgroundEvents` (macOS 11.3+)
 * lifts that, which is what lets a pad drive the rig of a handle app whose
 * own window is in front of the runtime's process (or of the service).
 *
 * Compile-tested only unless the PR says otherwise — a pad has to be paired
 * to this Mac to exercise the path.
 *
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_MACOS

#include "gamepad_backend.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

static GCController *g_current = nil;

bool
gamepad_backend_available(void)
{
	return true;
}

bool
gamepad_backend_init(void)
{
	@autoreleasepool {
		if (@available(macOS 11.3, *)) {
			GCController.shouldMonitorBackgroundEvents = YES;
		}
		// Kick discovery once; wireless pads already paired show up without it.
		[GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
	}
	g_current = nil;
	return true;
}

void
gamepad_backend_fini(void)
{
	@autoreleasepool {
		[GCController stopWirelessControllerDiscovery];
		g_current = nil;
	}
}

static float
axis(GCControllerAxisInput *a)
{
	return a != nil ? a.value : 0.0f;
}

static bool
pressed(GCControllerButtonInput *b)
{
	return b != nil && b.isPressed;
}

static float
trigger(GCControllerButtonInput *b)
{
	return b != nil ? b.value : 0.0f;
}

static void
clear_state(struct gamepad_state *out)
{
	out->connected = false;
	out->buttons = 0;
	out->left_x = out->left_y = out->right_x = out->right_y = 0.0f;
	out->left_trigger = out->right_trigger = 0.0f;
}

void
gamepad_backend_poll(struct gamepad_state *out)
{
	@autoreleasepool {
		GCController *ctl = g_current;
		if (ctl == nil || ctl.extendedGamepad == nil || ![GCController.controllers containsObject:ctl]) {
			ctl = nil;
			for (GCController *c in GCController.controllers) {
				if (c.extendedGamepad != nil) {
					ctl = c;
					break;
				}
			}
			g_current = ctl;
		}
		if (ctl == nil) {
			clear_state(out);
			return;
		}

		GCExtendedGamepad *g = ctl.extendedGamepad;
		uint32_t b = 0;
		if (pressed(g.buttonA)) {
			b |= GAMEPAD_BTN_A;
		}
		if (pressed(g.buttonB)) {
			b |= GAMEPAD_BTN_B;
		}
		if (pressed(g.buttonX)) {
			b |= GAMEPAD_BTN_X;
		}
		if (pressed(g.buttonY)) {
			b |= GAMEPAD_BTN_Y;
		}
		if (pressed(g.leftShoulder)) {
			b |= GAMEPAD_BTN_SHOULDER_LEFT;
		}
		if (pressed(g.rightShoulder)) {
			b |= GAMEPAD_BTN_SHOULDER_RIGHT;
		}
		if (pressed(g.dpad.up)) {
			b |= GAMEPAD_BTN_DPAD_UP;
		}
		if (pressed(g.dpad.down)) {
			b |= GAMEPAD_BTN_DPAD_DOWN;
		}
		if (pressed(g.dpad.left)) {
			b |= GAMEPAD_BTN_DPAD_LEFT;
		}
		if (pressed(g.dpad.right)) {
			b |= GAMEPAD_BTN_DPAD_RIGHT;
		}
		if (@available(macOS 10.15, *)) {
			if (pressed(g.leftThumbstickButton)) {
				b |= GAMEPAD_BTN_THUMB_LEFT;
			}
			if (pressed(g.rightThumbstickButton)) {
				b |= GAMEPAD_BTN_THUMB_RIGHT;
			}
			if (pressed(g.buttonMenu)) {
				b |= GAMEPAD_BTN_MENU;
			}
			if (pressed(g.buttonOptions)) {
				b |= GAMEPAD_BTN_VIEW;
			}
		}
		if (@available(macOS 11.0, *)) {
			if (pressed(g.buttonHome)) {
				b |= GAMEPAD_BTN_GUIDE;
			}
		}

		out->connected = true;
		out->buttons = b;
		out->left_x = axis(g.leftThumbstick.xAxis);
		out->left_y = axis(g.leftThumbstick.yAxis);
		out->right_x = axis(g.rightThumbstick.xAxis);
		out->right_y = axis(g.rightThumbstick.yAxis);
		out->left_trigger = trigger(g.leftTrigger);
		out->right_trigger = trigger(g.rightTrigger);
	}
}

void
gamepad_backend_rumble(float low_frequency, float high_frequency)
{
	// GameController haptics go through CHHapticEngine (GCDeviceHaptics,
	// macOS 11+): an engine per pad, a pattern player, a run loop to keep
	// it alive. Out of scope for the first cut — the Windows backend has
	// rumble, the mac one reports it as applied and does nothing.
	(void)low_frequency;
	(void)high_frequency;
}

const char *
gamepad_backend_name(void)
{
	return "GameController";
}

#else
// Keep the translation unit non-empty where this backend is compiled out (-Wempty-translation-unit).
typedef int gamepad_backend_gc_not_built_here_t;
#endif // XRT_OS_MACOS
