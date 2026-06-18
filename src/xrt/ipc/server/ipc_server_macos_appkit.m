// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS AppKit pump for the service main thread (shell Tier 1, #48).
 * @author David Fattal
 * @ingroup ipc_server
 */

#import <Cocoa/Cocoa.h>

#include "ipc_server_macos_appkit.h"

#include <stdbool.h>

void
ipc_server_macos_pump_main_thread(void)
{
	@autoreleasepool {
		static bool inited = false;
		if (!inited) {
			// A service is a background process by default; promote it to a
			// regular app so the compositor's NSWindow can become key + front.
			if (NSApp == nil) {
				[NSApplication sharedApplication];
				[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
			}
			inited = true;
		}

		// Drain the main dispatch queue (this is what unblocks the comp_multi
		// render thread's dispatch_sync that creates the NSWindow) plus any
		// AppKit runloop sources, non-blocking.
		while (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true) == kCFRunLoopRunHandledSource) {
			// keep draining until nothing left this pass
		}

		// Pump pending UI events so the window appears + stays responsive.
		NSEvent *event;
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                   untilDate:nil
		                                      inMode:NSDefaultRunLoopMode
		                                     dequeue:YES]) != nil) {
			[NSApp sendEvent:event];
		}
	}
}
