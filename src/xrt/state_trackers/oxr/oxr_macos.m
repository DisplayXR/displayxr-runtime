// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS event pumping for the OpenXR state tracker.
 * @author David Fattal
 * @ingroup st_oxr
 *
 * This file provides main-thread event processing required on macOS so that
 * NSWindow / CAMetalLayer content rendered on the compositor's background
 * thread actually reaches the display.  It is called from oxr_session_poll()
 * (oxr_session.c) on every xrPollEvent invocation.
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

/*!
 * Pump macOS events on the main thread.
 *
 * This performs three critical operations:
 * 1. Drains NSApp events (mouse, keyboard, window state).
 * 2. Flushes Core Animation transactions so the compositor's
 *    background-thread Metal drawable presents are composited on-screen.
 * 3. Runs the CFRunLoop briefly to process display-link and other
 *    pending sources.
 *
 * Without this, NSWindow/CAMetalLayer content rendered on the compositor's
 * background thread never reaches the display.
 */
void
oxr_macos_pump_events(void)
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
			[NSApp sendEvent:event];
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
