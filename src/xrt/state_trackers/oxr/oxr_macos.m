// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS event pumping and window lifecycle for the OpenXR state tracker.
 * @author David Fattal
 * @ingroup st_oxr
 *
 * This file provides main-thread event processing required on macOS so that
 * NSWindow / CAMetalLayer content rendered on the compositor's background
 * thread actually reaches the display.  It is called from oxr_session_poll()
 * (oxr_session.c) on every xrPollEvent invocation.
 *
 * It also detects window close (close button) and Escape key, setting a
 * global flag that the compositor checks in wait_frame to trigger session loss.
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_device.h"
#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif
#include "sim_display_interface.h"

/*!
 * Global flag: set to true when the compositor window is closed or Escape
 * is pressed.  Checked by the compositor in multi_compositor_wait_frame()
 * to return XRT_ERROR_IPC_FAILURE and trigger graceful session shutdown.
 *
 * Atomic because it is written on the main thread (event pump) and read
 * on the compositor's background thread (wait_frame).
 */
static atomic_bool g_macos_window_closed = false;

/*!
 * Query whether the macOS compositor window has been closed.
 * Called from the compositor thread (multi_compositor_wait_frame).
 */
bool
oxr_macos_window_closed(void)
{
	return atomic_load(&g_macos_window_closed);
}

/*!
 * Reset the window-closed flag.  Called when a new session is created
 * so a previous session's close doesn't affect the new one.
 */
void
oxr_macos_reset_window_closed(void)
{
	atomic_store(&g_macos_window_closed, false);
}

/*!
 * Pump macOS events on the main thread.
 *
 * This performs five critical operations:
 * 1. Drains NSApp events (mouse, keyboard, window state).
 * 2. Forwards events to qwerty device handler (if available).
 * 3. Detects Escape key and window close, setting the shutdown flag.
 * 4. Flushes Core Animation transactions so the compositor's
 *    background-thread Metal drawable presents are composited on-screen.
 * 5. Runs the CFRunLoop briefly to process display-link and other
 *    pending sources.
 *
 * Without this, NSWindow/CAMetalLayer content rendered on the compositor's
 * background thread never reaches the display.
 */
void
oxr_macos_pump_events(struct xrt_device **xdevs, uint32_t xdev_count)
{
	@autoreleasepool {
		if (NSApp == nil) {
			return;
		}

		// Drain NSApp events (mouse, keyboard, window lifecycle).
		NSEvent *event;
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                   untilDate:nil
		                                      inMode:NSDefaultRunLoopMode
		                                     dequeue:YES]) != nil) {

			// Detect Escape key.
			if ([event type] == NSEventTypeKeyDown &&
			    [event keyCode] == 53) { // 53 = Escape
				atomic_store(&g_macos_window_closed, true);
			}

			// 1/2/3 keys: switch sim_display output mode.
			if ([event type] == NSEventTypeKeyDown) {
				unsigned short kc = [event keyCode];
				if (kc == 18) { // '1'
					sim_display_set_output_mode(SIM_DISPLAY_OUTPUT_SBS);
				} else if (kc == 19) { // '2'
					sim_display_set_output_mode(SIM_DISPLAY_OUTPUT_ANAGLYPH);
				} else if (kc == 20) { // '3'
					sim_display_set_output_mode(SIM_DISPLAY_OUTPUT_BLEND);
				}
			}

			bool skip_send = false;
#ifdef XRT_BUILD_DRIVER_QWERTY
			// Forward event to qwerty device handler for
			// keyboard/mouse control of simulated devices.
			if (xdevs != NULL && xdev_count > 0) {
				qwerty_process_macos(xdevs, xdev_count,
				                     (__bridge void *)event);
				// Don't forward key events to AppKit — the default
				// responder chain beeps on unhandled keyDown events.
				NSEventType etype = [event type];
				skip_send = (etype == NSEventTypeKeyDown ||
				             etype == NSEventTypeKeyUp);
			}
#endif

			if (!skip_send) {
				[NSApp sendEvent:event];
			}
		}

		// Detect window close: after [NSApp sendEvent:] processes a
		// windowWillClose notification, [NSApp keyWindow] becomes nil.
		// Track whether we ever had a key window to avoid false
		// positives during startup before the window is created.
		{
			static bool had_key_window = false;
			if ([NSApp keyWindow] != nil) {
				had_key_window = true;
			} else if (had_key_window) {
				atomic_store(&g_macos_window_closed, true);
			}
		}

		// Flush Core Animation on the main thread. This commits
		// pending transactions from the compositor's background
		// thread Metal drawable presents.
		[CATransaction flush];

		// Run the main CFRunLoop briefly to process display-link
		// and other pending sources. The 1ms timeout gives Core
		// Animation time to process the flushed transactions.
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
	}
}
