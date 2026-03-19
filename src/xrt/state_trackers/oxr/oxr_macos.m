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
 *
 * The window-closed atomic and its accessors (oxr_macos_window_closed,
 * oxr_macos_reset_window_closed, oxr_macos_set_window_closed) live in
 * aux_os/os_macos.c so all targets (displayxr-service, libopenxr_displayxr) can link them.
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <stdbool.h>

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_device.h"
#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

// Defined in os_macos.c (aux_os library, linked by all targets)
extern void oxr_macos_set_window_closed(void);

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
oxr_macos_pump_events(struct xrt_device **xdevs, uint32_t xdev_count, struct xrt_device *head,
                      bool legacy_app, bool external_window)
{
	@autoreleasepool {
		if (NSApp == nil) {
			return;
		}

		// When running inside a host app (external_window == true),
		// collect keyboard/mouse events so we can re-inject them
		// after processing.  This lets the runtime see all events
		// (Escape detection, qwerty driver) while returning
		// keyboard/mouse events to the host app's event queue.
		NSMutableArray<NSEvent *> *reinject = external_window ? [NSMutableArray array] : nil;

		// Drain NSApp events (mouse, keyboard, window lifecycle).
		NSEvent *event;
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                   untilDate:nil
		                                      inMode:NSDefaultRunLoopMode
		                                     dequeue:YES]) != nil) {

			// Detect Escape key.
			if ([event type] == NSEventTypeKeyDown &&
			    [event keyCode] == 53) { // 53 = Escape
				oxr_macos_set_window_closed();
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

			// Mark keyboard/mouse events for re-injection.
			if (external_window) {
				NSEventType etype = [event type];
				switch (etype) {
				case NSEventTypeKeyDown:
				case NSEventTypeKeyUp:
				case NSEventTypeFlagsChanged:
				case NSEventTypeMouseMoved:
				case NSEventTypeLeftMouseDown:
				case NSEventTypeLeftMouseUp:
				case NSEventTypeRightMouseDown:
				case NSEventTypeRightMouseUp:
				case NSEventTypeLeftMouseDragged:
				case NSEventTypeRightMouseDragged:
				case NSEventTypeScrollWheel:
					[reinject addObject:event];
					break;
				default:
					break;
				}
			}
		}

		// Re-inject collected keyboard/mouse events so the host
		// app (e.g. Unity) can process them via its own input system.
		for (NSEvent *ev in reinject) {
			[NSApp postEvent:ev atStart:YES];
		}

#ifdef XRT_BUILD_DRIVER_QWERTY
		// Poll for direct rendering mode change (1/2/3 keys via qwerty driver).
		// Legacy apps (no XR_EXT_display_info) only support V toggle between
		// mode 0 (2D) and mode 1 (default 3D) — skip direct mode selection.
		if (!legacy_app && xdevs != NULL && xdev_count > 0 && head != NULL) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(xdevs, xdev_count, &render_mode)) {
				// Wrap for V key cycling and feed back wrapped value
				if (head->rendering_mode_count > 0) {
					render_mode = render_mode % (int)head->rendering_mode_count;
					// Update qwerty's stored mode for correct V cycling
					qwerty_set_rendering_mode_silent(xdevs, xdev_count, render_mode);
				}
				xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				// Note: do NOT change hmd->view_count for _rt apps.
				// _rt apps always render stereo; the compositor/weaver
				// handles 2D by displaying one eye or blending.
			}
		}
#endif

		// Detect window close: track the first window we see and
		// check if it has been closed (no longer visible).  We must
		// NOT use [NSApp keyWindow] — that returns nil whenever the
		// window merely loses focus (user clicks outside), which is
		// not the same as the window being closed.
		{
			static NSWindow *s_trackedWindow = nil;
			if (s_trackedWindow == nil) {
				s_trackedWindow = [NSApp mainWindow];
			}
			if (s_trackedWindow != nil && ![s_trackedWindow isVisible]) {
				oxr_macos_set_window_closed();
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
