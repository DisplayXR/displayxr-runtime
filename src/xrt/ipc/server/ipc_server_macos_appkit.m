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
#include "multi/comp_multi_workspace.h" // cursor pointer-position publish (#48 Phase 2)
#include "shared/ipc_protocol.h"
#include "util/u_logging.h"

#include <stdbool.h>
#include <ctype.h>

// Convert an NSEvent locationInWindow (points, bottom-left origin) to target
// framebuffer pixels (top-left origin) via the window's backing scale +
// content-view height. This is the coordinate space the workspace controller's
// hit-test expects (matching the Windows ScreenToClient client-px/top-left
// contract, #61) — used both for the cursor composite and the queued POINTER
// events the shell hit-tests against. Single-app OOP: the service window fills
// the display, so window content px == display px.
static void
pointer_event_to_target_px(NSEvent *event, NSPoint p, int32_t *out_x, int32_t *out_y)
{
	NSWindow *win = [event window];
	CGFloat scale = (win != nil) ? [win backingScaleFactor] : 1.0;
	NSView *cv = (win != nil) ? [win contentView] : nil;
	CGFloat h_pts = (cv != nil) ? cv.bounds.size.height : 0.0;
	*out_x = (int32_t)(p.x * scale);
	*out_y = (int32_t)((h_pts - p.y) * scale);
}

// Publish the latest pointer position in target framebuffer pixels (top-left
// origin) for the service-side workspace cursor composite (#48).
static void
publish_pointer_px(NSEvent *event, NSPoint p)
{
	int32_t px = 0, py = 0;
	pointer_event_to_target_px(event, p, &px, &py);
	comp_multi_workspace_set_pointer_px(px, py);
}

// A workspace/controller key is reserved for the shell and never forwarded to a
// content app: TAB / ESC / DELETE / F11 / arrows / [ ] and any Ctrl-chord. Plain
// keys (letters, digits, space, WASD, mode keys v/0-8) are "content" keys and go
// to the focused app. (#61: mirrors the Windows split, where the compositor
// window keeps workspace chords and the focused app HWND gets the rest.)
static bool
is_workspace_key(uint32_t vk, uint32_t mods)
{
	if (mods & (1u << 1)) { // bit1 = CTRL (ipc_protocol.h modifiers layout)
		return true;
	}
	switch (vk) {
	case 0x09: // VK_TAB    — cycle focus
	case 0x1B: // VK_ESCAPE — restore maximized
	case 0x2E: // VK_DELETE — close window
	case 0x7A: // VK_F11     — maximize toggle
	case 0x25: // VK_LEFT
	case 0x26: // VK_UP
	case 0x27: // VK_RIGHT
	case 0x28: // VK_DOWN
	case 0xDB: // VK_OEM_4 [ — step Z back
	case 0xDD: // VK_OEM_6 ] — step Z fwd
		return true;
	default: return false;
	}
}

// Route one wire input event to the right per-target queue(s) (#61). The
// workspace controller always receives pointer buttons / motion / keys (it owns
// focus-on-mousedown, chrome/taskbar/launcher hit-testing, hover, and the
// workspace chords); content pointer/motion ALSO forward to the window under the
// cursor, and scroll routes EXCLUSIVELY (content scroll → app, workspace scroll →
// controller-resize — the fix for "the shell resizes on every scroll"). While
// the launcher band has grabbed input or a controller drag/resize gesture holds
// pointer capture, everything goes to the controller only. Keyboard follows the
// focused window, not the cursor. With no controller registered (bare
// XRT_FORCE_MODE=ipc app, no shell), hit-test/focus are NULL so every event lands
// on the controller queue, which that lone app drains — preserving the old path.
static void
route_input_event(const struct ipc_workspace_input_event *ev)
{
	bool grab = ipc_server_input_queue_input_grabbed();
	bool cap = ipc_server_input_queue_pointer_captured();

	switch (ev->event_type) {
	case IPC_WORKSPACE_INPUT_EVENT_KEY:
		ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev);
		if (!grab && !is_workspace_key(ev->u.key.vk_code, ev->u.key.modifiers)) {
			void *app = comp_multi_workspace_get_focused_client();
			if (app != NULL) {
				ipc_server_input_queue_push(app, ev);
			}
		}
		break;
	case IPC_WORKSPACE_INPUT_EVENT_SCROLL:
		if (grab || cap) {
			ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev);
		} else {
			void *app = comp_multi_workspace_hit_test_window_px(
			    (int32_t)ev->u.scroll.cursor_x, (int32_t)ev->u.scroll.cursor_y);
			ipc_server_input_queue_push(app != NULL ? app : IPC_INPUT_TARGET_CONTROLLER, ev);
		}
		break;
	case IPC_WORKSPACE_INPUT_EVENT_POINTER:
		ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev);
		if (!grab && !cap) {
			void *app = comp_multi_workspace_hit_test_window_px(
			    (int32_t)ev->u.pointer.cursor_x, (int32_t)ev->u.pointer.cursor_y);
			if (app != NULL) {
				ipc_server_input_queue_push(app, ev);
			}
		}
		break;
	case IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION:
		ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev);
		if (!grab && !cap) {
			void *app = comp_multi_workspace_hit_test_window_px(
			    (int32_t)ev->u.pointer_motion.cursor_x, (int32_t)ev->u.pointer_motion.cursor_y);
			if (app != NULL) {
				ipc_server_input_queue_push(app, ev);
			}
		}
		break;
	default: ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev); break;
	}
}

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
		unichar raw = [chars characterAtIndex:0];
		uint32_t vk;
		// #61: translate macOS arrow / function keys into the Windows VK_* codes the
		// controller's handlers expect (launcher arrows, F11 maximize, etc.). macOS
		// delivers these as NS*FunctionKey unicode values (0xF7xx), not VK codes.
		switch (raw) {
		case 0xF700: vk = 0x26; break; // NSUpArrowFunctionKey    → VK_UP
		case 0xF701: vk = 0x28; break; // NSDownArrowFunctionKey  → VK_DOWN
		case 0xF702: vk = 0x25; break; // NSLeftArrowFunctionKey  → VK_LEFT
		case 0xF703: vk = 0x27; break; // NSRightArrowFunctionKey → VK_RIGHT
		case 0xF70E: vk = 0x7A; break; // NSF11FunctionKey        → VK_F11
		default: {
			unichar ch = raw;
			// ONLY when Control is held does charactersIgnoringModifiers sometimes
			// return the ASCII control char (Ctrl+L→0x0C); map THOSE back to letters
			// so the controller's letter chords match. Gating on Control is essential
			// — otherwise Return (0x0D), Tab (0x09) etc. would be corrupted into
			// letters ('m', 'i', …), which broke Enter-to-launch + TAB.
			bool ctrl_held = (mods & (1u << 1)) != 0;
			if (ctrl_held && ch >= 1 && ch <= 26) {
				ch = (unichar)(ch + 'a' - 1);
			}
			vk = (uint32_t)tolower(ch);
			break;
		}
		}
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_KEY;
		ev.u.key.vk_code = vk;
		ev.u.key.is_down = (type == NSEventTypeKeyDown) ? 1u : 0u;
		ev.u.key.modifiers = mods;
		// Optional one-shot-per-press key diagnostic (gated; helps confirm chord
		// delivery from a headless test). Off unless DXR_KEY_DEBUG is set.
		static int s_key_dbg = -1;
		if (s_key_dbg < 0) {
			s_key_dbg = (getenv("DXR_KEY_DEBUG") != NULL) ? 1 : 0;
		}
		if (s_key_dbg && type == NSEventTypeKeyDown) {
			U_LOG_W("[key] vk=0x%02x ('%c') mods=0x%x", ev.u.key.vk_code,
			        (ev.u.key.vk_code >= 32 && ev.u.key.vk_code < 127) ? (char)ev.u.key.vk_code : '?',
			        ev.u.key.modifiers);
		}
		route_input_event(&ev);
		return true;
	}
	case NSEventTypeLeftMouseDown:
	case NSEventTypeLeftMouseUp:
	case NSEventTypeRightMouseDown:
	case NSEventTypeRightMouseUp: {
		NSPoint p = [event locationInWindow];
		int32_t px = 0, py = 0;
		pointer_event_to_target_px(event, p, &px, &py);
		publish_pointer_px(event, p);
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER;
		ev.u.pointer.button =
		    (type == NSEventTypeRightMouseDown || type == NSEventTypeRightMouseUp) ? 2u : 1u;
		ev.u.pointer.is_down =
		    (type == NSEventTypeLeftMouseDown || type == NSEventTypeRightMouseDown) ? 1u : 0u;
		ev.u.pointer.cursor_x = (int64_t)px;
		ev.u.pointer.cursor_y = (int64_t)py;
		ev.u.pointer.modifiers = mods;
		route_input_event(&ev);
		return false; // also let AppKit handle it (window focus/drag)
	}
	case NSEventTypeMouseMoved:
	case NSEventTypeLeftMouseDragged:
	case NSEventTypeRightMouseDragged: {
		// Do NOT enqueue motion from NSEvents (#61). mouseMoved events arrive with
		// [event window] == nil, so pointer_event_to_target_px falls back to
		// scale=1 / height=0 and produces garbage (unscaled x, negative y) that
		// flip-flopped against the correct values from the per-cycle CGEventGetLocation
		// poll, making hover spotty. The poll is the single source of cursor motion
		// (window-independent, correct, and carries the pressed-button mask for drags).
		return false; // let AppKit handle the event for window behavior
	}
	case NSEventTypeScrollWheel: {
		NSPoint p = [event locationInWindow];
		int32_t px = 0, py = 0;
		pointer_event_to_target_px(event, p, &px, &py);
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_SCROLL;
		ev.u.scroll.delta_y = (float)[event scrollingDeltaY];
		ev.u.scroll.cursor_x = (int64_t)px;
		ev.u.scroll.cursor_y = (int64_t)py;
		ev.u.scroll.modifiers = mods;
		route_input_event(&ev);
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

		// Keep the workspace surface as the ACTIVE app so keyboard events reach
		// it. keyDown is delivered only to the frontmost app's key window, but a
		// client launched after the service grabs frontmost on its own startup
		// (every Cocoa app calls activateIgnoringOtherApps), which steals key-window
		// status from the surface — so TAB (and any controller key) was silently
		// dropped until the user clicked the surface to re-key it (mouse events,
		// unlike keyDown, route to the window under the cursor regardless of key
		// status, which is why click-to-focus worked but TAB did not). The surface
		// is a borderless full-screen desktop replacement that owns the display
		// until the Esc/Cmd+Q kill-switch, so reclaim activation whenever we've
		// lost it; once active this is a no-op. (#59/#48)
		if (![NSApp isActive]) {
			[NSApp activateIgnoringOtherApps:YES];
			NSWindow *kw = [NSApp keyWindow];
			if (kw == nil) {
				kw = [[NSApp windows] firstObject];
			}
			[kw makeKeyAndOrderFront:nil];
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
		// button events are both queued and forwarded to AppKit (window focus).
		NSEvent *event;
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                   untilDate:nil
		                                      inMode:NSDefaultRunLoopMode
		                                     dequeue:YES]) != nil) {
			// Workspace kill-switch (#59): the service window is a borderless
			// full-screen surface that hides the menu bar + dock and owns the
			// display, so there is no OS affordance to escape if something wedges.
			// Esc here tears the whole workspace down (the service is the root —
			// exiting drops every client's IPC connection so they shut down too).
			// This is the macOS dev analogue of the Windows Ctrl+Space shell
			// toggle until a real global hotkey is ported. Cmd+Q does the same.
			if ([event type] == NSEventTypeKeyDown) {
				bool is_escape = ([event keyCode] == 53);
				bool is_cmd_q = (([event modifierFlags] & NSEventModifierFlagCommand) != 0) &&
				                [[event charactersIgnoringModifiers] isEqualToString:@"q"];
				if (is_escape || is_cmd_q) {
					U_LOG_W("Workspace: %s — terminating service (workspace kill-switch)",
					        is_escape ? "Escape" : "Cmd+Q");
					exit(0);
				}
			}
			bool swallow = queue_ns_input_event(event);
			if (!swallow) {
				[NSApp sendEvent:event];
			}
		}

		// Hover/cursor tracking via POLLING, not the event stream (#61). macOS does
		// not deliver NSEventTypeMouseMoved to this manually-pumped window, so
		// bare-hover motion never arrives as an event. Instead, each pump cycle reads
		// the current global cursor position — the macOS analogue of Windows'
		// per-frame GetCursorPos that feeds FRAME_TICK — converts it to display pixels
		// (top-left), and, when it changed, publishes it for the cursor sprite and
		// enqueues a POINTER_MOTION so the controller hit-tests hover. A steady poll
		// (smooth), independent of whether AppKit emits move events.
		{
			NSScreen *screen = [NSScreen mainScreen];
			if (screen != nil) {
				CGFloat scale = screen.backingScaleFactor;
				// CGEventGetLocation reads the LIVE cursor position (global display
				// points, top-left origin) independent of the event stream — unlike
				// [NSEvent mouseLocation], which is frozen in this manually-pumped app
				// (it only advances from processed events, which don't include moves).
				CGEventRef cge = CGEventCreate(NULL);
				CGPoint cgp = CGEventGetLocation(cge);
				if (cge != NULL) {
					CFRelease(cge);
				}
				int32_t px = (int32_t)(cgp.x * scale);
				int32_t py = (int32_t)(cgp.y * scale);
				static int32_t s_last_px = INT32_MIN, s_last_py = INT32_MIN;
				if (px != s_last_px || py != s_last_py) {
					s_last_px = px;
					s_last_py = py;
					comp_multi_workspace_set_pointer_px(px, py);
					struct ipc_workspace_input_event mev;
					memset(&mev, 0, sizeof(mev));
					mev.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION;
					mev.u.pointer_motion.cursor_x = (int64_t)px;
					mev.u.pointer_motion.cursor_y = (int64_t)py;
					mev.u.pointer_motion.button_mask = (uint32_t)[NSEvent pressedMouseButtons];
					route_input_event(&mev);
				}
			}
		}
	}
}
