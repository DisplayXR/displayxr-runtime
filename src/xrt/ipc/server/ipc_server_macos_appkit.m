// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS AppKit pump for the service main thread (shell Tier 1, #48).
 * @author David Fattal
 * @ingroup ipc_server
 */

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h> // RegisterEventHotKey — system-wide Ctrl+Space toggle (#61)

#include "ipc_server_macos_appkit.h"
#include "ipc_server_input_queue.h"
#include "displayxr_logo_white_png.h" // embedded menu-bar status-item icon (#61)
#include "server/ipc_server.h"           // struct ipc_server::workspace_controller_pid (#61 lifecycle)
#include "multi/comp_multi_workspace.h" // cursor pointer-position publish (#48 Phase 2)
#include "shared/ipc_protocol.h"
#include "util/u_logging.h"

#include <stdbool.h>
#include <ctype.h>
#include <signal.h> // SIGTERM the controller on Ctrl+Space toggle (#61)
#include <spawn.h>  // posix_spawn the workspace controller (#61)
#include <stdlib.h> // getenv
#include <string.h> // strerror
#include <time.h>   // nanosleep (Cmd+Q grace period)

extern char **environ;

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

// Spawn the workspace controller (shell). The macOS analogue of the Windows
// service_orchestrator's spawn_workspace (#61): the service is the persistent
// root, and Ctrl+Space launches the controller on demand when none is running.
// DISPLAYXR_SHELL_PATH points at the controller binary (or a dev launcher script
// that sets the controller's env — XR_RUNTIME_JSON etc. — and execs it); the
// child inherits the service's environment. Productization can move this to the
// POSIX workspace-controller registry (service_workspace_registry) like Windows
// reads HKLM. No-op with a warning if the path is unset/unspawnable.
static void
spawn_workspace_controller(void)
{
	const char *path = getenv("DISPLAYXR_SHELL_PATH");
	if (path == NULL || path[0] == '\0') {
		U_LOG_W("Workspace: Ctrl+Space but DISPLAYXR_SHELL_PATH is unset — cannot spawn the controller");
		return;
	}
	char *const argv[] = {(char *)path, NULL};
	pid_t pid = 0;
	int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
	if (rc != 0) {
		U_LOG_W("Workspace: failed to spawn controller '%s': %s", path, strerror(rc));
		return;
	}
	U_LOG_W("Workspace: Ctrl+Space — spawned controller '%s' (pid %d)", path, (int)pid);
}

// TOGGLE the workspace controller (#61): SIGTERM it when running (its handler
// runs the cleanup that kills the apps it launched, then it exits and the IPC
// disconnect resets workspace_controller_pid → the next toggle spawns again);
// spawn it when absent. Mirrors the Windows service_orchestrator Ctrl+Space.
static void
do_workspace_toggle(struct ipc_server *s)
{
	unsigned long ctrl_pid = (s != NULL) ? s->workspace_controller_pid : 0;
	if (ctrl_pid != 0) {
		U_LOG_W("Workspace: toggle — SIGTERM controller (pid %lu)", ctrl_pid);
		kill((pid_t)ctrl_pid, SIGTERM);
	} else {
		// Prefer the orchestrator's registry-discovery + crash-respawn path
		// (#61); fall back to the dev DISPLAYXR_SHELL_PATH spawn when ipc_server
		// has no summon provider (sdl_test, non-orchestrator hosts).
		if (!ipc_server_request_workspace_summon()) {
			spawn_workspace_controller();
		}
	}
}

// System-wide Ctrl+Space toggle (#61). The service window is created lazily only
// once a client connects, so before the controller exists there is NO window to
// receive NSEvents — the toggle must be a GLOBAL hotkey (the macOS analogue of
// the Windows orchestrator's RegisterHotKey), which fires regardless of focus or
// whether the app has any window. The Carbon handler just sets a flag; the pump
// (which holds the ipc_server) performs the toggle.
static volatile bool g_toggle_requested = false;

static OSStatus
toggle_hotkey_handler(EventHandlerCallRef next, EventRef evt, void *user)
{
	(void)next;
	(void)evt;
	(void)user;
	g_toggle_requested = true;
	return noErr;
}

static void
install_toggle_hotkey(void)
{
	static bool installed = false;
	if (installed) {
		return;
	}
	installed = true;
	EventTypeSpec spec = {kEventClassKeyboard, kEventHotKeyPressed};
	InstallApplicationEventHandler(toggle_hotkey_handler, 1, &spec, NULL, NULL);
	EventHotKeyRef ref;
	// Primary: Ctrl+Space (Windows parity). On systems where the OS owns
	// Ctrl+Space for "Select the previous input source", this registration is
	// shadowed and won't fire — the fallback below covers that.
	EventHotKeyID id1 = {.signature = 'DXR1', .id = 1};
	RegisterEventHotKey(kVK_Space, controlKey, id1, GetApplicationEventTarget(), 0, &ref);
	// Fallback: Ctrl+Option+Space (conflict-free) → same toggle.
	EventHotKeyID id2 = {.signature = 'DXR2', .id = 2};
	RegisterEventHotKey(kVK_Space, controlKey | optionKey, id2, GetApplicationEventTarget(), 0, &ref);
	U_LOG_W("Workspace: registered toggle hotkey Ctrl+Space (fallback Ctrl+Option+Space)");
}

// macOS menu-bar status item (#61, Piece 3). A visible, discoverable surface for
// the spatial shell: it shows whether a workspace controller is running, lets the
// user summon one ("Open Workspace" → the orchestrator's registry-discovery +
// crash-respawn path via ipc_server_request_workspace_summon), and quits the
// service GRACEFULLY ("Quit" → raise SIGTERM → the kqueue EVFILT_SIGNAL path →
// service_orchestrator_shutdown, which reaps the controller — no orphan).
// Complements the global Ctrl+Space hotkey with a no-TCC, always-visible control.
@interface DXRStatusTarget : NSObject <NSMenuDelegate>
{
@public
	struct ipc_server *server;
	NSMenuItem *statusLine; // owned by the menu
	NSMenuItem *openItem;   // owned by the menu
}
@end

@implementation DXRStatusTarget
- (void)openWorkspace:(id)sender
{
	(void)sender;
	if (!ipc_server_request_workspace_summon()) {
		U_LOG_W("Workspace: 'Open Workspace' clicked but no summon provider is registered");
	}
}
- (void)quitService:(id)sender
{
	(void)sender;
	// Graceful: SIGTERM ourselves so the mainloop's EVFILT_SIGNAL handler stops
	// the server and main() runs service_orchestrator_shutdown(), which SIGTERMs
	// the managed controller. (Unlike Cmd+Q's exit(0), this can't orphan it.)
	U_LOG_W("Workspace: status-item Quit — graceful service shutdown");
	raise(SIGTERM);
}
- (void)menuNeedsUpdate:(NSMenu *)menu
{
	(void)menu;
	unsigned long pid = (server != NULL) ? server->workspace_controller_pid : 0;
	if (pid != 0) {
		statusLine.title = [NSString stringWithFormat:@"Workspace: running (PID %lu)", pid];
		[openItem setEnabled:NO];
	} else {
		statusLine.title = @"Workspace: stopped";
		[openItem setEnabled:YES];
	}
}
@end

// Retained for the process lifetime (MRC). The menu's delegate is an assign
// reference, so s_status_target must keep the target alive.
static DXRStatusTarget *s_status_target = nil;
static NSStatusItem *s_status_item = nil;

// Build the menu-bar status item + menu. Main thread; called once.
static void
install_status_item(struct ipc_server *s)
{
	if (s_status_item != nil) {
		return;
	}

	s_status_target = [[DXRStatusTarget alloc] init];
	s_status_target->server = s;

	s_status_item = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
	// Persist the user-chosen position. New status items are inserted next to the
	// notch (leftmost of the third-party area), where a crowded menu bar can hide
	// them behind the notch; with an autosaveName, macOS remembers wherever the
	// user ⌘-drags the item, across relaunches.
	s_status_item.autosaveName = @"DisplayXRService";
	// Icon-only, using the embedded DisplayXR logo (the macOS analogue of the
	// Windows tray icon, which embeds displayxr_white.ico). Marked as a template
	// image so macOS tints it for light/dark menu bars from its alpha — compact,
	// takes far less width than a text label.
	NSData *logo_data = [NSData dataWithBytes:displayxr_logo_white_png
	                                   length:displayxr_logo_white_png_len];
	NSImage *logo = [[[NSImage alloc] initWithData:logo_data] autorelease];
	if (logo != nil) {
		logo.template = YES;
		// Fit the menu-bar height (~18 pt tall), preserving aspect.
		NSSize px = logo.size;
		CGFloat h = 18.0;
		CGFloat w = (px.height > 0.0) ? (h * px.width / px.height) : h;
		logo.size = NSMakeSize(w, h);
		s_status_item.button.image = logo;
		s_status_item.button.imagePosition = NSImageOnly;
	} else {
		// Fallback if the embedded image fails to decode for any reason.
		s_status_item.button.title = @"DisplayXR";
	}

	NSMenu *menu = [[NSMenu alloc] init];
	menu.autoenablesItems = NO; // we control enabled state in menuNeedsUpdate:
	menu.delegate = s_status_target;

	NSMenuItem *status = [[[NSMenuItem alloc] initWithTitle:@"Workspace: starting…"
	                                                 action:nil
	                                          keyEquivalent:@""] autorelease];
	[status setEnabled:NO];
	[menu addItem:status];
	s_status_target->statusLine = status;

	NSMenuItem *open = [[[NSMenuItem alloc] initWithTitle:@"Open Workspace"
	                                               action:@selector(openWorkspace:)
	                                        keyEquivalent:@""] autorelease];
	open.target = s_status_target;
	[menu addItem:open];
	s_status_target->openItem = open;

	[menu addItem:[NSMenuItem separatorItem]];

	NSMenuItem *quit = [[[NSMenuItem alloc] initWithTitle:@"Quit DisplayXR Service"
	                                               action:@selector(quitService:)
	                                        keyEquivalent:@"q"] autorelease];
	quit.target = s_status_target;
	[menu addItem:quit];

	s_status_item.menu = menu; // status item retains the menu; menu retains items
	[menu release];

	U_LOG_W("Workspace: installed menu-bar status item (#61)");
}

void
ipc_server_macos_pump_main_thread(struct ipc_server *s)
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
			// Complete the AppKit launch sequence. We drive the run loop manually
			// (no [NSApp run]) for the compositor window, but the system status bar
			// only shows an NSStatusItem once the app has finished launching —
			// finishLaunching is the documented non-blocking way to do that.
			[NSApp finishLaunching];
			// System-wide Ctrl+Space toggle — must be a global hotkey because the
			// service has no window (hence no key responder) until a controller
			// connects (#61).
			install_toggle_hotkey();
			// Menu-bar status item (#61, Piece 3) — visible presence + Open/Quit.
			install_status_item(s);
			inited = true;
		}

		// Service the global toggle hotkey (set by the Carbon handler). Done here,
		// holding the ipc_server, so spawn/SIGTERM can read workspace_controller_pid.
		if (g_toggle_requested) {
			g_toggle_requested = false;
			do_workspace_toggle(s);
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
		// while a controller is up, so reclaim activation whenever we've lost it;
		// once active this is a no-op. (#59/#48)
		//
		// #61: ONLY while the workspace is active (a controller connected). With no
		// controller the surface window is hidden (see the render gate) and the
		// service is a background daemon — reclaiming activation here would steal
		// focus from the user's desktop apps AND makeKeyAndOrderFront would re-show
		// the window we just hid. Ctrl+Space (a global hotkey) respawns the
		// controller, which sets workspace_mode and re-shows the surface.
		bool workspace_up = (s != NULL) && s->workspace_mode;
		if (workspace_up && ![NSApp isActive]) {
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
			// Cmd+Q — graceful FULL shutdown escape hatch (#61). SIGTERM the
			// controller first (so its launched apps are cleaned up rather than
			// orphaned on the abrupt IPC drop), give it a brief moment, then exit
			// the service. Reachable once the surface window exists (a controller is
			// up). ESC is NO LONGER a kill-switch — it flows through to the
			// controller (restore-maximized / launcher-close). The Ctrl+Space toggle
			// is a global hotkey (above), not handled here, since the service may
			// have no window yet.
			if ([event type] == NSEventTypeKeyDown) {
				NSUInteger mf = [event modifierFlags];
				bool cmd = (mf & NSEventModifierFlagCommand) != 0;
				bool is_q = [[event charactersIgnoringModifiers] isEqualToString:@"q"];
				if (cmd && is_q) {
					U_LOG_W("Workspace: Cmd+Q — terminating service");
					unsigned long ctrl_pid = (s != NULL) ? s->workspace_controller_pid : 0;
					if (ctrl_pid != 0) {
						kill((pid_t)ctrl_pid, SIGTERM);
						struct timespec ts = {0, 300 * 1000 * 1000}; // 300 ms for shell cleanup
						nanosleep(&ts, NULL);
					}
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
