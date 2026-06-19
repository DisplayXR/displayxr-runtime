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
#include "ipc_server_input_queue.h"
#include "shared/ipc_protocol.h"

#include <stdbool.h>
#include <ctype.h>

// Translate an NSEvent (service-window input) into a wire input event and queue
// it for the IPC handler to forward to the client app (#48). Mirrors the Windows
// D3D11 service WndProc producer. Keyboard is reported as the lowercased Unicode
// character in key.vk_code — the macOS client maps characters, not VK codes — so
// the forwarded path drives the same handling the app's in-process NSEvent loop
// does (mode keys v/0-8, WASD camera). Returns true if the event was a captured
// input event (so the caller can skip sendEvent: for keys — no responder, no beep).
static bool
queue_ns_input_event(NSEvent *event)
{
	NSEventType type = [event type];
	struct ipc_workspace_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.timestamp_ms = (uint32_t)([event timestamp] * 1000.0);

	NSUInteger flags = [event modifierFlags];
	uint32_t mods = 0;
	if (flags & NSEventModifierFlagShift) mods |= 1u << 0;
	if (flags & NSEventModifierFlagControl) mods |= 1u << 1;
	if (flags & NSEventModifierFlagOption) mods |= 1u << 2;

	switch (type) {
	case NSEventTypeKeyDown:
	case NSEventTypeKeyUp: {
		// Swallow OS key auto-repeat: the client treats every forwarded keyDown as
		// a fresh press (one cycle/toggle per press), matching the in-process
		// !isRepeat gating. Without this, holding a key spams mode cycles. Held
		// movement (WASD) is unaffected — its g_input flag stays set until keyUp.
		if (type == NSEventTypeKeyDown && [event isARepeat]) {
			return true; // swallowed (no beep), not forwarded
		}
		NSString *chars = [event charactersIgnoringModifiers];
		if ([chars length] == 0) {
			return false; // modifier-only / dead key — nothing to forward
		}
		unichar ch = (unichar)tolower([chars characterAtIndex:0]);
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_KEY;
		ev.u.key.vk_code = (uint32_t)ch;
		ev.u.key.is_down = (type == NSEventTypeKeyDown) ? 1u : 0u;
		ev.u.key.modifiers = mods;
		ipc_server_input_queue_push(&ev);
		return true;
	}
	case NSEventTypeLeftMouseDown:
	case NSEventTypeLeftMouseUp:
	case NSEventTypeRightMouseDown:
	case NSEventTypeRightMouseUp: {
		NSPoint p = [event locationInWindow];
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER;
		ev.u.pointer.button =
		    (type == NSEventTypeRightMouseDown || type == NSEventTypeRightMouseUp) ? 2u : 1u;
		ev.u.pointer.is_down =
		    (type == NSEventTypeLeftMouseDown || type == NSEventTypeRightMouseDown) ? 1u : 0u;
		ev.u.pointer.cursor_x = (int64_t)p.x;
		ev.u.pointer.cursor_y = (int64_t)p.y;
		ev.u.pointer.modifiers = mods;
		ipc_server_input_queue_push(&ev);
		return false; // also let AppKit handle it (window focus/drag)
	}
	case NSEventTypeMouseMoved:
	case NSEventTypeLeftMouseDragged:
	case NSEventTypeRightMouseDragged: {
		NSPoint p = [event locationInWindow];
		uint32_t button_mask = (uint32_t)[NSEvent pressedMouseButtons];
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION;
		ev.u.pointer_motion.cursor_x = (int64_t)p.x;
		ev.u.pointer_motion.cursor_y = (int64_t)p.y;
		ev.u.pointer_motion.button_mask = button_mask;
		ev.u.pointer_motion.modifiers = mods;
		ipc_server_input_queue_push(&ev);
		return false;
	}
	case NSEventTypeScrollWheel: {
		NSPoint p = [event locationInWindow];
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_SCROLL;
		ev.u.scroll.delta_y = (float)[event scrollingDeltaY];
		ev.u.scroll.cursor_x = (int64_t)p.x;
		ev.u.scroll.cursor_y = (int64_t)p.y;
		ev.u.scroll.modifiers = mods;
		ipc_server_input_queue_push(&ev);
		return false;
	}
	default: return false;
	}
}

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

		// Pump pending UI events so the window appears + stays responsive, and
		// forward keyboard/mouse to the client app over IPC (#48). Key events are
		// captured and NOT sent to AppKit (no responder → system beep); mouse
		// events are both queued and forwarded to AppKit (window focus/move).
		NSEvent *event;
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                   untilDate:nil
		                                      inMode:NSDefaultRunLoopMode
		                                     dequeue:YES]) != nil) {
			bool swallow = queue_ns_input_event(event);
			if (!swallow) {
				[NSApp sendEvent:event];
			}
		}
	}
}
