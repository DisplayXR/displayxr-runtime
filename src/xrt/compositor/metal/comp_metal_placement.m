// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-owned, phase-snapped, atomic window placement on macOS
 *         (ADR-050). See comp_metal_placement.h for the model.
 * @ingroup comp_metal
 *
 * Compiled with ARC (this file only — see CMakeLists.txt): the module holds
 * weak references to the app's window, view and delegate, which only ARC
 * expresses. It deliberately includes nothing from os/os_threading.h (whose
 * inlines are not ARC-clean); locking is a plain pthread mutex.
 *
 * The recipe is the one verified on the panel in LeiaSR's
 * tests/macos/metal_weaver/mtl_weaver_window.mm (a849a5c45, "perfect" on drag,
 * edge and corner resize). Its gotchas, all honoured here:
 *  - hit-test the mouse-down EVENT's location, never NSEvent.mouseLocation;
 *  - the zoom path is intercepted (windowShouldZoom:toFrame: declines);
 *  - windowWillResize:toSize: is a tripwire: AppKit never resizes natively;
 *  - never setFrame:display:YES;
 *  - drawable.texture's size is the authority (the compositor reads it);
 *  - the title bar height comes from contentRectForFrameRect (32 pt on 26);
 *  - snap per drag step, anchored at the GESTURE START; SR_DECLINED = target;
 *  - an R/B-only resize needs no snap; L/T snaps the origin, size absorbs it;
 *  - min content 320x200 pt;
 *  - one move per rendered frame (the pending rect is overwritten per event).
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>

#include "comp_metal_placement.h"

#include "util/u_logging.h"
#include "util/u_x11_scale.h" // u_x11_reachable_round + lattice ring order (pure math)

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if !__has_feature(objc_arc)
#error "comp_metal_placement.m must be compiled with -fobjc-arc"
#endif

enum
{
	EDGE_L = 1,
	EDGE_R = 2,
	EDGE_T = 4,
	EDGE_B = 8,
};

static const CGFloat kResizeZonePt = 8.0;
static const CGFloat kMinContentW = 320.0;
static const CGFloat kMinContentH = 200.0;

static bool
trace_enabled(void)
{
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("DXR_MACOS_PLACEMENT_TRACE");
		v = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return v == 1;
}

//! AppKit global space is bottom-up from the main display's bottom edge.
static CGFloat
main_screen_height(void)
{
	NSArray<NSScreen *> *screens = [NSScreen screens];
	return screens.count > 0 ? screens[0].frame.size.height : 0.0;
}

//! The screen holding the content rect's top-left (bottom-up points).
static NSScreen *
screen_for_top_left(NSRect content, NSWindow *win)
{
	const NSPoint tl = NSMakePoint(content.origin.x + 0.5, NSMaxY(content) - 0.5);
	for (NSScreen *s in [NSScreen screens]) {
		if (NSPointInRect(tl, s.frame)) {
			return s;
		}
	}
	return win.screen != nil ? win.screen : [NSScreen mainScreen];
}


/*
 *
 * Delegate proxy: zoom intercept + native-resize tripwire, everything else
 * forwarded to the app's delegate.
 *
 */

@class DXRMetalPlacement;

@interface DXRWindowDelegateProxy : NSProxy <NSWindowDelegate>
@property(nonatomic, weak) id<NSWindowDelegate> inner;
@property(nonatomic, weak) DXRMetalPlacement *owner;
@end

@interface DXRMetalPlacement : NSObject
{
@public
	pthread_mutex_t lock;
	comp_metal_placement_snap_fn snap;
	void *snap_userdata;

	// ---- main thread only (event monitor, delegate proxy) ----
	int gesture; //!< 0 none, 1 drag, 2 resize, 3 swallow until mouse-up (title double-click)
	unsigned edges;
	NSPoint mouse_start;   //!< global points
	NSRect content_start;  //!< bottom-up points
	int32_t anchor_px_x, anchor_px_y; //!< gesture-start content top-left, top-down backing px
	BOOL orig_movable;
	BOOL orig_movable_bg;
	BOOL warned_native_resize;
	BOOL warned_live_resize;
	//! Resize-cursor feedback (see update_resize_cursor): the edges whose
	//! cursor is shown, the cursor this module set (nil = none), and the
	//! cursor that was current before it (restored only if ours still is).
	unsigned cursor_edges;
	NSCursor *cursor_ours;
	NSCursor *cursor_prev;

	// ---- guarded by lock ----
	bool pending;
	NSRect pending_content;  //!< bottom-up points, unsnapped
	bool pending_origin_moves;
	bool pending_keep_far;   //!< resize: right/bottom edges stay, size absorbs the snap
	int32_t pending_anchor_x, pending_anchor_y;
	bool gesture_active;     //!< mirror of gesture != 0 for the commit thread

	// ---- commit thread ----
	bool have_last;
	NSRect last_frame;       //!< the window frame last applied / observed
	int32_t last_px_x, last_px_y; //!< last presented content top-left, top-down backing px
	_Atomic int inflight;    //!< a deferred main-thread apply is outstanding
	bool warned_app_move;
	bool warned_fallback;
	bool warned_mismatch;
	bool warned_lattice;
	uint64_t moves_applied;
	uint64_t mismatches;
	//! Post-present verification (main-thread presents that moved): the
	//! window's content origin right after the transaction commits must be
	//! the origin the frame was woven for; the drawable must be content x
	//! scale (a first drawable after a drawableSize change may lag one frame).
	uint64_t verified;
	uint64_t verify_mismatch;
	uint64_t drawable_lag;

	_Atomic unsigned native_resizes;
	_Atomic unsigned live_resizes;
	_Atomic bool closed;
	_Atomic bool cursor_poll_queued; //!< a main-thread cursor poll is outstanding
}
@property(nonatomic, weak) NSWindow *window;
@property(nonatomic, weak) NSView *view;
@property(nonatomic, strong) id monitor;
@property(nonatomic, strong) DXRWindowDelegateProxy *proxy;
@property(nonatomic, strong) NSMutableArray *observers;
- (void)queueZoom:(NSRect)frame;
- (void)noteNativeResize;
@end

@implementation DXRWindowDelegateProxy

- (BOOL)respondsToSelector:(SEL)sel
{
	if (sel == @selector(windowShouldZoom:toFrame:) || sel == @selector(windowWillResize:toSize:)) {
		return YES;
	}
	id i = self.inner;
	return i != nil && [i respondsToSelector:sel];
}

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
	if (protocol == @protocol(NSWindowDelegate)) {
		return YES;
	}
	id i = self.inner;
	return i != nil && [i conformsToProtocol:protocol];
}

// `window.delegate` returns this proxy, not the app's object: make
// -isEqual: / -hash answer as the app's delegate, so an app comparing its
// delegate with -isEqual: still finds it (== cannot be helped).
- (BOOL)isEqual:(id)object
{
	id i = self.inner;
	return object == self || (i != nil && [i isEqual:object]);
}

- (NSUInteger)hash
{
	id i = self.inner;
	return i != nil ? [i hash] : (NSUInteger)(uintptr_t)(__bridge void *)self;
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	id i = self.inner;
	NSMethodSignature *s = i != nil ? [i methodSignatureForSelector:sel] : nil;
	// Never reached for a selector we do not claim; a void signature keeps
	// the runtime from throwing if the inner delegate went away meanwhile.
	return s != nil ? s : [NSMethodSignature signatureWithObjCTypes:"v@:"];
}

- (void)forwardInvocation:(NSInvocation *)inv
{
	id i = self.inner;
	if (i != nil && [i respondsToSelector:inv.selector]) {
		[inv invokeWithTarget:i];
	}
}

- (BOOL)windowShouldZoom:(NSWindow *)window toFrame:(NSRect)newFrame
{
	id<NSWindowDelegate> i = self.inner;
	if (i != nil && [i respondsToSelector:@selector(windowShouldZoom:toFrame:)] &&
	    ![i windowShouldZoom:window toFrame:newFrame]) {
		return NO; // the app vetoed the zoom
	}
	// AppKit would animate the zoom outside the render loop (every
	// intermediate frame stutters in 3D): decline it and take the proposed
	// frame through the snapped pending-rect path instead.
	[self.owner queueZoom:newFrame];
	return NO;
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize
{
	// Sent for USER-driven resizes (never for setFrame:). AppKit's zoom: also
	// sends it (before windowShouldZoom:, which declines), so only a call
	// inside a live resize — AppKit's own edge tracking loop — means the
	// runtime-owned resize was bypassed. A tripwire, not a feature.
	if (sender.inLiveResize) {
		[self.owner noteNativeResize];
	}
	id<NSWindowDelegate> i = self.inner;
	if (i != nil && [i respondsToSelector:@selector(windowWillResize:toSize:)]) {
		return [i windowWillResize:sender toSize:frameSize];
	}
	return frameSize;
}

@end


/*
 *
 * Geometry helpers (main thread for hit tests; pure math otherwise).
 *
 */

static bool
in_title_bar(NSWindow *win, NSPoint m)
{
	return NSPointInRect(m, win.frame) && !NSPointInRect(m, [win contentRectForFrameRect:win.frame]);
}

static bool
on_standard_button(NSWindow *win, NSPoint m)
{
	const NSWindowButton buttons[3] = {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton};
	for (int k = 0; k < 3; k++) {
		NSButton *btn = [win standardWindowButton:buttons[k]];
		if (btn == nil || btn.hidden) {
			continue;
		}
		const NSRect r = [win convertRectToScreen:[btn convertRect:btn.bounds toView:nil]];
		if (NSPointInRect(m, r)) {
			return true;
		}
	}
	return false;
}

static unsigned
resize_edges_at(NSWindow *win, NSPoint m)
{
	if ((win.styleMask & NSWindowStyleMaskResizable) == 0) {
		return 0;
	}
	const NSRect f = win.frame;
	const CGFloat z = kResizeZonePt;
	if (m.x < NSMinX(f) - z || m.x > NSMaxX(f) + z || m.y < NSMinY(f) - z || m.y > NSMaxY(f) + z) {
		return 0;
	}
	unsigned e = 0;
	if (fabs(m.x - NSMinX(f)) <= z) {
		e |= EDGE_L;
	}
	if (fabs(m.x - NSMaxX(f)) <= z) {
		e |= EDGE_R;
	}
	if (fabs(m.y - NSMaxY(f)) <= z) {
		e |= EDGE_T; // AppKit: +y is up
	}
	if (fabs(m.y - NSMinY(f)) <= z) {
		e |= EDGE_B;
	}
	return e;
}

//! Content top-left (bottom-up points) -> top-down global backing px.
static void
content_px(NSRect content, CGFloat scale, int32_t *out_x, int32_t *out_y)
{
	*out_x = (int32_t)llround(content.origin.x * scale);
	*out_y = (int32_t)llround((main_screen_height() - NSMaxY(content)) * scale);
}

//! On the lattice of positions reachable from @p anchor under @p q?
static bool
on_lattice(int32_t anchor, int32_t v, uint32_t q)
{
	return q <= 1 || ((v - anchor) % (int32_t)q) == 0;
}


@implementation DXRMetalPlacement

- (void)noteNativeResize
{
	unsigned n = atomic_fetch_add(&native_resizes, 1u) + 1u;
	if (!warned_native_resize) {
		warned_native_resize = YES;
		U_LOG_W("macOS placement TRIPWIRE: AppKit started a native resize (windowWillResize:, count %u) — "
		        "the runtime-owned resize was bypassed",
		        n);
	}
}

- (void)setPendingContent:(NSRect)content originMoves:(bool)moves keepFar:(bool)keep_far
{
	pthread_mutex_lock(&lock);
	pending = true;
	pending_content = content;
	pending_origin_moves = moves;
	pending_keep_far = keep_far;
	pending_anchor_x = anchor_px_x;
	pending_anchor_y = anchor_px_y;
	pthread_mutex_unlock(&lock);
}

- (void)queueZoom:(NSRect)frame
{
	NSWindow *win = self.window;
	if (win == nil) {
		return;
	}
	const NSRect cur = [win contentRectForFrameRect:win.frame];
	content_px(cur, win.backingScaleFactor, &anchor_px_x, &anchor_px_y);
	NSRect target = [win contentRectForFrameRect:frame];
	target.origin.x = round(target.origin.x);
	target.origin.y = round(target.origin.y);
	target.size.width = round(target.size.width);
	target.size.height = round(target.size.height);
	// Move semantics (the size is the zoom's): absorbing the snap into the
	// size would grow a full-screen zoom 1-2 px past the screen.
	[self setPendingContent:target originMoves:true keepFar:false];
	if (trace_enabled()) {
		U_LOG_W("placement: zoom -> content %.0f,%.0f %.0fx%.0f pt", target.origin.x, target.origin.y,
		        target.size.width, target.size.height);
	}
}

//! Re-wrap the window's delegate if the app replaced it (main thread).
- (void)ensureProxy
{
	NSWindow *win = self.window;
	if (win == nil || self.proxy == nil) {
		return;
	}
	id cur = win.delegate;
	if (cur != (id)self.proxy) {
		self.proxy.inner = cur;
		win.delegate = self.proxy;
	}
}

- (NSRect)resizedContent:(CGFloat)dx dy:(CGFloat)dy
{
	NSWindow *win = self.window;
	const CGFloat minW = fmax(kMinContentW, win != nil ? win.contentMinSize.width : 0.0);
	const CGFloat minH = fmax(kMinContentH, win != nil ? win.contentMinSize.height : 0.0);
	NSRect r = content_start;
	if (edges & EDGE_R) {
		r.size.width = fmax(minW, round(r.size.width + dx));
	}
	if (edges & EDGE_T) {
		r.size.height = fmax(minH, round(r.size.height + dy));
	}
	if (edges & EDGE_L) {
		const CGFloat nx = fmin(round(r.origin.x + dx), NSMaxX(content_start) - minW);
		r.size.width = NSMaxX(content_start) - nx;
		r.origin.x = nx;
	}
	if (edges & EDGE_B) {
		const CGFloat ny = fmin(round(r.origin.y + dy), NSMaxY(content_start) - minH);
		r.size.height = NSMaxY(content_start) - ny;
		r.origin.y = ny;
	}
	return r;
}

- (void)doubleClickTitleBar
{
	NSWindow *win = self.window;
	NSString *action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
	if ([action isEqualToString:@"Minimize"]) {
		[win miniaturize:nil];
	} else if ([action isEqualToString:@"None"]) {
		// nothing
	} else {
		// Default ("Maximize" / "Fill" / unset): AppKit's zoom, which the
		// delegate proxy declines and re-queues through the snapped path.
		[win zoom:nil];
	}
}

//! The local event monitor (main thread). nil = swallowed.
- (NSEvent *)handleEvent:(NSEvent *)ev
{
	NSWindow *win = self.window;
	if (win == nil || atomic_load(&closed)) {
		return ev;
	}
	[self ensureProxy];

	const NSEventType type = ev.type;
	if (gesture != 0) {
		if (type == NSEventTypeLeftMouseDragged) {
			// The live cursor for the drag steps (as the verified recipe).
			const NSPoint m = [NSEvent mouseLocation];
			const CGFloat dx = m.x - mouse_start.x;
			const CGFloat dy = m.y - mouse_start.y;
			if (gesture == 3) {
				return nil;
			}
			if (gesture == 1) {
				NSRect t = content_start;
				t.origin.x = round(content_start.origin.x + dx);
				t.origin.y = round(content_start.origin.y + dy);
				[self setPendingContent:t originMoves:true keepFar:false];
			} else {
				const NSRect t = [self resizedContent:dx dy:dy];
				[self setPendingContent:t originMoves:(edges & (EDGE_L | EDGE_T)) != 0 keepFar:true];
			}
			return nil;
		}
		if (type == NSEventTypeLeftMouseUp) {
			if (trace_enabled() && gesture != 3) {
				U_LOG_W("placement: %s end", gesture == 1 ? "drag" : "resize");
			}
			gesture = 0;
			pthread_mutex_lock(&lock);
			gesture_active = false;
			pthread_mutex_unlock(&lock);
			return nil;
		}
		if (type == NSEventTypeLeftMouseDown) {
			// A down without the up (lost while another app tracked):
			// end the stale gesture and evaluate this one afresh.
			gesture = 0;
		} else {
			return ev;
		}
	}

	if (type != NSEventTypeLeftMouseDown || ev.window != win) {
		return ev;
	}
	if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0 || win.miniaturized) {
		return ev;
	}

	// Hit-test the EVENT's location: by the time a queued mouse-down is
	// pumped the cursor may already have left the edge zone.
	const NSPoint m = [win convertPointToScreen:ev.locationInWindow];
	const bool button = on_standard_button(win, m);
	if (button) {
		return ev; // the traffic lights stay AppKit's
	}
	const bool title = in_title_bar(win, m);
	const unsigned e = resize_edges_at(win, m);
	if (e == 0 && !title) {
		return ev; // content click: the app's
	}
	if (e == 0 && title && ev.clickCount >= 2) {
		// Swallow the matching mouse-up too: AppKit runs its own title-bar
		// double-click action (zoom / "Fill") on it otherwise.
		gesture = 3;
		[self doubleClickTitleBar];
		return nil;
	}

	mouse_start = m;
	content_start = [win contentRectForFrameRect:win.frame];
	content_px(content_start, win.backingScaleFactor, &anchor_px_x, &anchor_px_y);
	edges = e;
	gesture = e != 0 ? 2 : 1;
	pthread_mutex_lock(&lock);
	gesture_active = true;
	pthread_mutex_unlock(&lock);
	if (trace_enabled()) {
		U_LOG_W("placement: %s start (edges%s%s%s%s) content %.0fx%.0f at %d,%d px", gesture == 1 ? "drag" : "resize",
		        (e & EDGE_L) ? " L" : "", (e & EDGE_R) ? " R" : "", (e & EDGE_T) ? " T" : "",
		        (e & EDGE_B) ? " B" : "", content_start.size.width, content_start.size.height, anchor_px_x,
		        anchor_px_y);
	}
	// What the swallowed click would have done. A native title-bar / edge
	// click on a window that is not key ACTIVATES the app and makes the
	// window key. Measured on macOS 26 (sim-display, a second app
	// frontmost): the WindowServer marks the app active before the event is
	// delivered, but AppKit completes the activation (the window becoming
	// key) only when the mouse-down itself goes through -[NSApp sendEvent:].
	// Swallowed, the app is left active with NO key window, and neither
	// makeKeyAndOrderFront:, makeKeyWindow, [NSApp activate] nor
	// NSRunningApplication activation (at once or retried later) changes
	// that: no keyboard input, no resize cursor. So an activating click is
	// handed to AppKit, retargeted to the title-bar centre (an edge click as
	// it stands would start AppKit's own live resize). The window is
	// movable = NO, so AppKit does not drag it; the gesture stays ours (the
	// drags and the mouse-up still pass through this monitor).
	if (!NSApp.isActive) {
		if (@available(macOS 14.0, *)) {
			[NSApp activate];
		} else {
			[NSApp activateIgnoringOtherApps:YES];
		}
	}
	if (!win.isKeyWindow) {
		NSEvent *fwd = ev;
		if (e != 0) {
			const NSRect f = win.frame;
			const NSRect c = [win contentRectForFrameRect:f];
			const NSPoint title_mid = NSMakePoint(NSMidX(f), (NSMaxY(c) + NSMaxY(f)) * 0.5);
			NSEvent *t = [NSEvent mouseEventWithType:ev.type
			                                location:[win convertPointFromScreen:title_mid]
			                           modifierFlags:ev.modifierFlags
			                               timestamp:ev.timestamp
			                            windowNumber:ev.windowNumber
			                                 context:nil
			                             eventNumber:ev.eventNumber
			                              clickCount:ev.clickCount
			                                pressure:ev.pressure];
			if (t != nil) {
				fwd = t;
			}
		}
		if (trace_enabled()) {
			U_LOG_W("placement: activating click — mouse-down handed to AppKit%s so the window becomes key "
			        "(active=%d)",
			        fwd != ev ? " (retargeted to the title bar)" : "", NSApp.isActive ? 1 : 0);
		}
		return fwd;
	}
	[win makeKeyAndOrderFront:nil];
	return nil;
}

@end


/*
 *
 * Resize-cursor feedback (main thread).
 *
 * AppKit never sees the edge mouse-downs (the monitor swallows them), so it
 * never shows its frame-resize cursors, and the 8 pt zone OUTSIDE the window
 * gets no mouse-moved events at all. So the cursor is driven from a poll of
 * NSEvent.mouseLocation (once per present, see begin_present) against the
 * very zones handleEvent: hit-tests. Only the resize zones are touched: the
 * cursor this module set is restored to the one it replaced, and only if it
 * is still the current one (an app cursor set meanwhile is left alone).
 *
 */

static NSCursor *
resize_cursor_for(unsigned e)
{
	if (@available(macOS 15.0, *)) {
		NSUInteger pos = 0; // closed enum: build the bits, cast once
		if (e & EDGE_T) {
			pos |= NSCursorFrameResizePositionTop;
		}
		if (e & EDGE_B) {
			pos |= NSCursorFrameResizePositionBottom;
		}
		if (e & EDGE_L) {
			pos |= NSCursorFrameResizePositionLeft;
		}
		if (e & EDGE_R) {
			pos |= NSCursorFrameResizePositionRight;
		}
		return [NSCursor frameResizeCursorFromPosition:(NSCursorFrameResizePosition)pos
		                                  inDirections:NSCursorFrameResizeDirectionsAll];
	}
	// Pre-15: no public diagonal cursors; a corner shows the horizontal one.
	if (e & (EDGE_L | EDGE_R)) {
		return [NSCursor resizeLeftRightCursor];
	}
	return [NSCursor resizeUpDownCursor];
}

//! Is the window the topmost one at @p m (inside its frame)? Outside the frame
//! (the outer margin) the point belongs to whatever is below: accept it.
static bool
window_owns_point(NSWindow *win, NSPoint m)
{
	if (!NSPointInRect(m, win.frame)) {
		return true;
	}
	return [NSWindow windowNumberAtPoint:m belowWindowWithWindowNumber:0] == win.windowNumber;
}

static void
update_resize_cursor(DXRMetalPlacement *pl)
{
	NSWindow *win = pl.window;
	unsigned e = 0;
	if (trace_enabled()) {
		static int last_active = -1;
		const int active = NSApp.isActive ? 1 : 0;
		if (active != last_active) {
			last_active = active;
			U_LOG_W("placement: app %s", active ? "active" : "inactive (resize cursor off)");
		}
	}
	if (pl->gesture == 2) {
		e = pl->edges; // keep the cursor for the whole resize, wherever the mouse is
	} else if (win != nil && pl->gesture == 0 && !atomic_load(&pl->closed) && NSApp.isActive && win.isKeyWindow &&
	           win.isVisible && !win.miniaturized && (win.styleMask & NSWindowStyleMaskFullScreen) == 0) {
		const NSPoint m = [NSEvent mouseLocation];
		if (!on_standard_button(win, m) && window_owns_point(win, m)) {
			e = resize_edges_at(win, m);
		}
	}

	if (e != 0) {
		NSCursor *cur = [NSCursor currentCursor];
		if (pl->cursor_ours == nil) {
			pl->cursor_prev = cur;
		}
		if (e != pl->cursor_edges || cur != pl->cursor_ours) {
			// Also re-set when AppKit's cursor rects reset it (inner zone).
			NSCursor *want =
			    (e == pl->cursor_edges && pl->cursor_ours != nil) ? pl->cursor_ours : resize_cursor_for(e);
			[want set];
			const bool changed = e != pl->cursor_edges;
			pl->cursor_ours = want;
			pl->cursor_edges = e;
			if (changed && trace_enabled()) {
				U_LOG_W("placement: resize cursor (edges%s%s%s%s)", (e & EDGE_L) ? " L" : "",
				        (e & EDGE_R) ? " R" : "", (e & EDGE_T) ? " T" : "", (e & EDGE_B) ? " B" : "");
			}
		}
		return;
	}
	if (pl->cursor_ours != nil) {
		// Restore only what we replaced, and only if nobody changed it since.
		if ([NSCursor currentCursor] == pl->cursor_ours) {
			[(pl->cursor_prev != nil ? pl->cursor_prev : [NSCursor arrowCursor]) set];
		}
		if (trace_enabled()) {
			U_LOG_W("placement: resize cursor released");
		}
		pl->cursor_ours = nil;
		pl->cursor_prev = nil;
		pl->cursor_edges = 0;
	}
}

//! Per-present cursor poll from any thread: inline on main, else one queued
//! main-thread poll at a time (never piles up behind a busy main thread).
static void
poll_resize_cursor(DXRMetalPlacement *pl)
{
	if ([NSThread isMainThread]) {
		update_resize_cursor(pl);
		return;
	}
	bool expected = false;
	if (!atomic_compare_exchange_strong(&pl->cursor_poll_queued, &expected, true)) {
		return;
	}
	__weak DXRMetalPlacement *weak_pl = pl;
	dispatch_async(dispatch_get_main_queue(), ^{
		DXRMetalPlacement *s = weak_pl;
		if (s != nil) {
			atomic_store(&s->cursor_poll_queued, false);
			update_resize_cursor(s);
		}
	});
}


/*
 *
 * C API
 *
 */

static void
run_on_main_sync(void (^block)(void))
{
	if ([NSThread isMainThread]) {
		block();
	} else {
		dispatch_sync(dispatch_get_main_queue(), block);
	}
}

static inline DXRMetalPlacement *
placement_obj(struct comp_metal_placement *p)
{
	return (__bridge DXRMetalPlacement *)(void *)p;
}

bool
comp_metal_placement_eligible(
    void *ns_view, bool offscreen, bool shared_iosurface, bool app_owned, const char **out_why)
{
	const char *why = NULL;
	const char *native = getenv("DXR_MACOS_NATIVE_DRAG");
	const char *ws = getenv("DISPLAYXR_WORKSPACE_SESSION");
	NSView *view = (__bridge NSView *)ns_view;
	if (native != NULL && native[0] == '1') {
		why = "DXR_MACOS_NATIVE_DRAG=1 (AppKit/WindowServer drag, control)";
	} else if (app_owned) {
		why = "app opted out (XR_COCOA_WINDOW_PLACEMENT_APP_OWNED_BIT_DXR)";
	} else if (offscreen) {
		why = "offscreen session";
	} else if (shared_iosurface) {
		why = "shared-IOSurface session (the app presents)";
	} else if (ws != NULL && ws[0] == '1') {
		why = "workspace (shell) session";
	} else if (view == nil) {
		why = "no view";
	} else {
		__block NSWindow *win = nil;
		__block NSView *cv = nil;
		run_on_main_sync(^{
			win = view.window;
			cv = win.contentView;
		});
		if (win == nil) {
			why = "the view has no window";
		} else if (cv != view) {
			why = "the bound view is not its window's contentView (a sub-view placement is the app's)";
		}
	}
	if (out_why != NULL) {
		*out_why = why;
	}
	return why == NULL;
}

struct comp_metal_placement *
comp_metal_placement_create(void *ns_view, comp_metal_placement_snap_fn snap, void *userdata)
{
	NSView *view = (__bridge NSView *)ns_view;
	if (view == nil) {
		return NULL;
	}
	DXRMetalPlacement *pl = [[DXRMetalPlacement alloc] init];
	pthread_mutex_init(&pl->lock, NULL);
	pl->snap = snap;
	pl->snap_userdata = userdata;
	atomic_store(&pl->inflight, 0);
	atomic_store(&pl->native_resizes, 0u);
	atomic_store(&pl->live_resizes, 0u);
	atomic_store(&pl->closed, false);
	atomic_store(&pl->cursor_poll_queued, false);
	pl.view = view;

	__block bool ok = false;
	run_on_main_sync(^{
		NSWindow *win = view.window;
		if (win == nil) {
			return;
		}
		pl.window = win;
		pl->orig_movable = win.movable;
		pl->orig_movable_bg = win.movableByWindowBackground;
		// The WindowServer must never move this window itself: its drag
		// slides the last frame without a re-weave.
		win.movable = NO;
		win.movableByWindowBackground = NO;

		DXRWindowDelegateProxy *proxy = [DXRWindowDelegateProxy alloc];
		proxy.owner = pl;
		proxy.inner = win.delegate;
		pl.proxy = proxy;
		win.delegate = proxy;

		__weak DXRMetalPlacement *weak_pl = pl;
		pl.monitor = [NSEvent
		    addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged |
		                                          NSEventMaskLeftMouseUp)
		                                 handler:^NSEvent *(NSEvent *ev) {
			                                 DXRMetalPlacement *s = weak_pl;
			                                 return s != nil ? [s handleEvent:ev] : ev;
		                                 }];

		pl.observers = [NSMutableArray array];
		NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
		[pl.observers addObject:[nc addObserverForName:NSWindowWillStartLiveResizeNotification
		                                        object:win
		                                         queue:nil
		                                    usingBlock:^(NSNotification *n) {
			                                    (void)n;
			                                    DXRMetalPlacement *s = weak_pl;
			                                    if (s == nil) {
				                                    return;
			                                    }
			                                    unsigned c = atomic_fetch_add(&s->live_resizes, 1u) + 1u;
			                                    if (!s->warned_live_resize) {
				                                    s->warned_live_resize = YES;
				                                    U_LOG_W("macOS placement TRIPWIRE: AppKit live resize "
				                                            "started (count %u)",
				                                            c);
			                                    }
		                                    }]];
		[pl.observers addObject:[nc addObserverForName:NSWindowWillCloseNotification
		                                        object:win
		                                         queue:nil
		                                    usingBlock:^(NSNotification *n) {
			                                    (void)n;
			                                    DXRMetalPlacement *s = weak_pl;
			                                    if (s != nil) {
				                                    atomic_store(&s->closed, true);
			                                    }
		                                    }]];
		ok = true;
	});
	if (!ok) {
		pthread_mutex_destroy(&pl->lock);
		return NULL;
	}
	U_LOG_W("macOS window placement: runtime-owned drag + resize (phase-snapped, atomic present) — "
	        "DXR_MACOS_NATIVE_DRAG=1 or XR_COCOA_WINDOW_PLACEMENT_APP_OWNED_BIT_DXR opts out (ADR-050)");
	return (struct comp_metal_placement *)(__bridge_retained void *)pl;
}

void
comp_metal_placement_destroy(struct comp_metal_placement **p_ptr)
{
	if (p_ptr == NULL || *p_ptr == NULL) {
		return;
	}
	DXRMetalPlacement *pl = (__bridge_transfer DXRMetalPlacement *)(void *)*p_ptr;
	*p_ptr = NULL;
	run_on_main_sync(^{
		if (pl.monitor != nil) {
			[NSEvent removeMonitor:pl.monitor];
			pl.monitor = nil;
		}
		// Give back a resize cursor still set by this module.
		atomic_store(&pl->closed, true);
		pl->gesture = 0;
		update_resize_cursor(pl);
		for (id o in pl.observers) {
			[[NSNotificationCenter defaultCenter] removeObserver:o];
		}
		[pl.observers removeAllObjects];
		NSWindow *win = pl.window;
		if (win != nil) {
			if (win.delegate == (id)pl.proxy) {
				win.delegate = pl.proxy.inner;
			}
			win.movable = pl->orig_movable;
			win.movableByWindowBackground = pl->orig_movable_bg;
		}
		pl.proxy.owner = nil;
	});
	U_LOG_W("macOS window placement: released (moves applied %llu, AppKit-constrained %llu, verified %llu, "
	        "presented-vs-woven mismatches %llu, drawable lag %llu, native-resize tripwire %u, "
	        "live-resize tripwire %u)",
	        (unsigned long long)pl->moves_applied, (unsigned long long)pl->mismatches,
	        (unsigned long long)pl->verified, (unsigned long long)pl->verify_mismatch,
	        (unsigned long long)pl->drawable_lag, atomic_load(&pl->native_resizes), atomic_load(&pl->live_resizes));
	// Retained by any block still in flight on the main queue; freed with the
	// last reference. The mutex goes with the object (never contended now).
}

/*!
 * Snap @p target (top-down backing px) relative to @p anchor, landing on a
 * position the window can actually reach (anchor + q·Z², q = the backing
 * scale): the twin of vk_snap_search_lattice. Pure DP queries.
 */
static bool
snap_reachable(DXRMetalPlacement *pl,
               double probe_x,
               double probe_y,
               int32_t ax,
               int32_t ay,
               int32_t tx,
               int32_t ty,
               uint32_t q,
               int32_t *out_x,
               int32_t *out_y)
{
	*out_x = tx;
	*out_y = ty;
	if (pl->snap == NULL) {
		return false;
	}
	int32_t sx = tx, sy = ty;
	if (!pl->snap(pl->snap_userdata, probe_x, probe_y, ax, ay, tx, ty, &sx, &sy)) {
		return false; // no lattice / declined (no viewer yet): the target stands
	}
	if (on_lattice(ax, sx, q) && on_lattice(ay, sy, q)) {
		*out_x = sx;
		*out_y = sy;
		return true;
	}
	const int32_t bx = u_x11_reachable_round(ax, sx, q);
	const int32_t by = u_x11_reachable_round(ay, sy, q);
	const uint32_t n = u_x11_lattice_candidate_count(3);
	// Ring order finds the nearest RING with a phase-correct reachable point;
	// within that ring take the point nearest the DP's own answer (Euclidean),
	// so an axis the lattice does not constrain (y, for a vertical
	// lenticular) is not nudged by the ring's walk order.
	bool found = false;
	int32_t best_x = 0, best_y = 0, found_ring = -1;
	int64_t best_d2 = 0;
	for (uint32_t k = 0; k < n; k++) {
		int32_t i = 0, j = 0;
		u_x11_lattice_candidate(k, &i, &j);
		const int32_t ring = abs(i) > abs(j) ? abs(i) : abs(j);
		if (found && ring > found_ring) {
			break;
		}
		const int32_t px = bx + i * (int32_t)q;
		const int32_t py = by + j * (int32_t)q;
		int32_t rx = px, ry = py;
		if (!pl->snap(pl->snap_userdata, probe_x, probe_y, ax, ay, px, py, &rx, &ry)) {
			continue;
		}
		if (on_lattice(ax, rx, q) && on_lattice(ay, ry, q)) {
			const int64_t ddx = (int64_t)rx - sx, ddy = (int64_t)ry - sy;
			const int64_t d2 = ddx * ddx + ddy * ddy;
			if (!found || d2 < best_d2) {
				found = true;
				found_ring = ring;
				best_d2 = d2;
				best_x = rx;
				best_y = ry;
			}
		}
	}
	if (found) {
		*out_x = best_x;
		*out_y = best_y;
		return true;
	}
	if (!pl->warned_lattice) {
		pl->warned_lattice = true;
		U_LOG_W("macOS placement: no phase-correct position reachable within %u px (backing scale %u) — "
		        "landing on the nearest reachable pixel",
		        3u * q, q);
	}
	*out_x = bx;
	*out_y = by;
	return true;
}

//! Fill the present-origin fields of @p f from a content rect (bottom-up pts).
static void
fill_origin(struct comp_metal_placement_frame *f, NSRect content, NSWindow *win)
{
	const CGFloat main_h = main_screen_height();
	NSScreen *scr = screen_for_top_left(content, win);
	const CGFloat scale = win.backingScaleFactor > 0 ? win.backingScaleFactor : 1.0;
	const CGFloat sscale = scr != nil && scr.backingScaleFactor > 0 ? scr.backingScaleFactor : scale;
	f->scale = scale;
	f->content_x = content.origin.x;
	f->content_y = main_h - NSMaxY(content);
	f->content_w = content.size.width;
	f->content_h = content.size.height;
	content_px(content, scale, &f->origin_px_x, &f->origin_px_y);
	if (scr != nil) {
		f->present_origin_x = (int32_t)llround((content.origin.x - scr.frame.origin.x) * sscale);
		f->present_origin_y = (int32_t)llround((NSMaxY(scr.frame) - NSMaxY(content)) * sscale);
	} else {
		f->present_origin_x = f->origin_px_x;
		f->present_origin_y = f->origin_px_y;
	}
}

//! After a moving present (main thread): did the window land where the frame
//! was woven for, at the drawable's size?
static void
verify_present(DXRMetalPlacement *pl,
               NSWindow *win,
               id<CAMetalDrawable> drawable,
               const struct comp_metal_placement_frame *f)
{
	if (win == nil || !(f->moves || f->resizes)) {
		return;
	}
	const NSRect now = [win contentRectForFrameRect:win.frame];
	int32_t x = 0, y = 0;
	content_px(now, f->scale, &x, &y);
	pl->verified++;
	if (x != f->origin_px_x || y != f->origin_px_y) {
		pl->verify_mismatch++;
		U_LOG_W("placement VERIFY: window content at %d,%d px after present, frame woven for %d,%d", x, y,
		        f->origin_px_x, f->origin_px_y);
	}
	if (drawable != nil) {
		const uint32_t dw = (uint32_t)drawable.texture.width, dh = (uint32_t)drawable.texture.height;
		const uint32_t cw = (uint32_t)llround(f->content_w * f->scale), ch = (uint32_t)llround(f->content_h * f->scale);
		if (dw != cw || dh != ch) {
			pl->drawable_lag++;
			if (trace_enabled()) {
				U_LOG_W("placement: drawable %ux%u != content %ux%u px (lags a frame)", dw, dh, cw, ch);
			}
		}
	}
	if (trace_enabled()) {
		U_LOG_W("placement VERIFY: presented at %d,%d px == woven-for %d,%d (%s)", x, y, f->origin_px_x,
		        f->origin_px_y, (x == f->origin_px_x && y == f->origin_px_y) ? "ok" : "MISMATCH");
	}
}

static void
apply_frame(NSWindow *win, CAMetalLayer *layer, NSRect frame, CGFloat scale)
{
	if (NSEqualSizes(frame.size, win.frame.size)) {
		[win setFrameOrigin:frame.origin];
	} else {
		// NEVER display:YES — that draws outside this transaction.
		[win setFrame:frame display:NO];
		// Size the drawable from the content rect AppKit actually applied
		// (it may constrain the frame: min / max size), never from the one
		// asked for: a drawable woven at one size and scaled into another
		// breaks the lens phase.
		const NSRect applied = [win contentRectForFrameRect:win.frame];
		layer.drawableSize = CGSizeMake(round(applied.size.width * scale), round(applied.size.height * scale));
	}
}

bool
comp_metal_placement_begin_present(struct comp_metal_placement *p,
                                   void *metal_layer,
                                   struct comp_metal_placement_frame *out)
{
	memset(out, 0, sizeof(*out));
	if (p == NULL || metal_layer == NULL) {
		return false;
	}
	DXRMetalPlacement *pl = placement_obj(p);
	CAMetalLayer *layer = (__bridge CAMetalLayer *)metal_layer;
	NSWindow *win = pl.window;
	if (win == nil || atomic_load(&pl->closed)) {
		if (layer.presentsWithTransaction) {
			layer.presentsWithTransaction = NO;
		}
		return false;
	}
	if (!layer.presentsWithTransaction) {
		layer.presentsWithTransaction = YES;
	}
	poll_resize_cursor(pl);

	// DXR_MACOS_PLACEMENT_FORCE_DEFERRED=1 (test knob): take the off-main
	// hand-off path even on the main thread. The main queue cannot run the
	// hand-off block while this thread waits, so every moving frame exercises
	// the timeout + non-atomic fallback — the "never deadlock" guarantee.
	static int force_deferred = -1;
	if (force_deferred < 0) {
		const char *e = getenv("DXR_MACOS_PLACEMENT_FORCE_DEFERRED");
		force_deferred = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	const bool on_main = [NSThread isMainThread] && force_deferred == 0;
	if ([NSThread isMainThread]) {
		[pl ensureProxy];
	}

	const NSRect cur_frame = win.frame;
	const NSUInteger style = win.styleMask;
	const bool parked = (style & NSWindowStyleMaskFullScreen) != 0 || win.miniaturized;

	// Take the pending rect (coalesced: the last drag step wins).
	pthread_mutex_lock(&pl->lock);
	bool have = pl->pending && !parked;
	NSRect target = pl->pending_content;
	bool origin_moves = pl->pending_origin_moves;
	bool keep_far = pl->pending_keep_far;
	int32_t ax = pl->pending_anchor_x, ay = pl->pending_anchor_y;
	const bool gesture_active = pl->gesture_active;
	pl->pending = false;
	pthread_mutex_unlock(&pl->lock);

	// App-initiated move / resize: the frame changed outside this path. Snap
	// it relative to the last PRESENTED origin and apply on this present (the
	// frame the app's own move showed was off-phase: <= 1 frame).
	if (!have && !parked && !gesture_active && pl->have_last && atomic_load(&pl->inflight) == 0 &&
	    !NSEqualRects(cur_frame, pl->last_frame)) {
		if (!pl->warned_app_move) {
			pl->warned_app_move = true;
			U_LOG_W("macOS placement: window frame changed outside the runtime's path (app setFrame / "
			        "display change) — re-snapping it on the next atomic present (<= 1 off-phase frame)");
		}
		have = true;
		target = [NSWindow contentRectForFrameRect:cur_frame styleMask:style];
		origin_moves = true;
		keep_far = false;
		ax = pl->last_px_x;
		ay = pl->last_px_y;
	}
	if (parked) {
		pl->last_frame = cur_frame;
		pl->have_last = true;
	}

	const CGFloat scale = win.backingScaleFactor > 0 ? win.backingScaleFactor : 1.0;
	const CGFloat main_h = main_screen_height();
	NSRect content = [NSWindow contentRectForFrameRect:cur_frame styleMask:style];
	bool moves = false, resizes = false;
	int32_t want_x = 0, want_y = 0;

	if (have) {
		if (origin_moves) {
			const double probe_x = target.origin.x;
			const double probe_y = main_h - NSMaxY(target);
			int32_t tx = 0, ty = 0;
			content_px(target, scale, &tx, &ty);
			const uint32_t q = (uint32_t)lround(scale) > 0 ? (uint32_t)lround(scale) : 1u;
			int32_t sx = tx, sy = ty;
			(void)snap_reachable(pl, probe_x, probe_y, ax, ay, tx, ty, q, &sx, &sy);
			const CGFloat nx = (CGFloat)sx / scale;
			const CGFloat ntop = main_h - (CGFloat)sy / scale; // bottom-up top edge
			if (keep_far) {
				// Resize by the left / top edge: the far edges stay, the size
				// absorbs the snap's 1-2 px correction.
				const CGFloat right = NSMaxX(target), bottom = NSMinY(target);
				target.size.width = right - nx;
				target.size.height = ntop - bottom;
				target.origin.x = nx;
				target.origin.y = bottom;
			} else {
				target.origin.x = nx;
				target.origin.y = ntop - target.size.height;
			}
			want_x = sx;
			want_y = sy;
		} else {
			content_px(target, scale, &want_x, &want_y);
		}
		const NSRect frame = [NSWindow frameRectForContentRect:target styleMask:style];
		moves = !NSEqualPoints(frame.origin, cur_frame.origin);
		resizes = !NSEqualSizes(frame.size, cur_frame.size);
		out->frame_x = frame.origin.x;
		out->frame_y = frame.origin.y;
		out->frame_w = frame.size.width;
		out->frame_h = frame.size.height;
		content = target;
		pl->last_frame = frame;
		pl->have_last = true;
	} else if (!pl->have_last) {
		pl->last_frame = cur_frame;
		pl->have_last = true;
	}

	out->moves = moves;
	out->resizes = resizes;

	if (on_main) {
		// One transaction carries the move AND the frame woven for it.
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		out->txn_open = true;
		if (moves || resizes) {
			const NSRect fr = NSMakeRect(out->frame_x, out->frame_y, out->frame_w, out->frame_h);
			apply_frame(win, layer, fr, scale);
			// The APPLIED origin is the authority for the present origin
			// (AppKit may constrain a frame): read it back.
			const NSRect applied = [win contentRectForFrameRect:win.frame];
			int32_t gx = 0, gy = 0;
			content_px(applied, scale, &gx, &gy);
			if ((gx != want_x || gy != want_y) && origin_moves) {
				pl->mismatches++;
				if (!pl->warned_mismatch) {
					pl->warned_mismatch = true;
					U_LOG_W("macOS placement: window landed at %d,%d px, asked %d,%d (AppKit constrained "
					        "the frame) — weaving for where it landed",
					        gx, gy, want_x, want_y);
				}
			}
			content = applied;
			pl->last_frame = win.frame;
		}
	} else if (moves || resizes) {
		// Not the main thread: the window move must happen there. Size the
		// drawable now (before nextDrawable) and hand the move + present to
		// the main thread at end_present.
		out->deferred = true;
		if (resizes) {
			layer.drawableSize =
			    CGSizeMake(round(content.size.width * scale), round(content.size.height * scale));
		}
	}

	fill_origin(out, content, win);
	if (moves || resizes) {
		pl->moves_applied++;
		if (trace_enabled()) {
			U_LOG_W("placement: apply%s%s content %.0f,%.0f %.0fx%.0f pt -> origin %d,%d px (asked %d,%d), "
			        "present origin %d,%d%s",
			        moves ? " move" : "", resizes ? " resize" : "", out->content_x, out->content_y,
			        out->content_w, out->content_h, out->origin_px_x, out->origin_px_y, want_x, want_y,
			        out->present_origin_x, out->present_origin_y, out->deferred ? " (deferred to main)" : "");
		}
	}
	pl->last_px_x = out->origin_px_x;
	pl->last_px_y = out->origin_px_y;
	return true;
}

@interface DXRPlacementHandoff : NSObject
{
@public
	_Atomic int state; //!< 0 = open, 1 = main took it, 2 = the commit thread took it
	struct comp_metal_placement_frame frame; //!< outlives the commit thread's stack frame
}
@end
@implementation DXRPlacementHandoff
@end

void
comp_metal_placement_end_present(struct comp_metal_placement *p,
                                 void *command_buffer,
                                 void *drawable_ptr,
                                 struct comp_metal_placement_frame *f)
{
	id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
	id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)drawable_ptr;
	if (cb != nil) {
		// presentsWithTransaction: commit, wait until scheduled, then present
		// explicitly (Apple's documented sequence).
		[cb waitUntilScheduled];
	}

	if (f->txn_open) {
		if (drawable != nil) {
			[drawable present];
		}
		[CATransaction commit];
		f->txn_open = false;
		if (p != NULL) {
			verify_present(placement_obj(p), placement_obj(p).window, drawable, f);
		}
		return;
	}

	if (!f->deferred || p == NULL) {
		// Commit thread, nothing to move: an explicit transaction of its own
		// (a background thread has no run loop to commit an implicit one).
		[CATransaction begin];
		if (drawable != nil) {
			[drawable present];
		}
		[CATransaction commit];
		return;
	}

	DXRMetalPlacement *pl = placement_obj(p);
	NSWindow *win = pl.window;
	CAMetalLayer *layer = (CAMetalLayer *)pl.view.layer;
	const NSRect frame = NSMakeRect(f->frame_x, f->frame_y, f->frame_w, f->frame_h);
	DXRPlacementHandoff *h = [[DXRPlacementHandoff alloc] init];
	h->frame = *f;
	const struct comp_metal_placement_frame *f_copy_ptr = &h->frame;
	atomic_store(&h->state, 0);
	dispatch_semaphore_t sem = dispatch_semaphore_create(0);
	atomic_store(&pl->inflight, 1);
	(void)layer;

	dispatch_async(dispatch_get_main_queue(), ^{
		int expected = 0;
		const bool atomic_present = atomic_compare_exchange_strong(&h->state, &expected, 1);
		if (win != nil) {
			if (atomic_present) {
				[CATransaction begin];
				[CATransaction setDisableActions:YES];
			}
			if (NSEqualSizes(frame.size, win.frame.size)) {
				[win setFrameOrigin:frame.origin];
			} else {
				[win setFrame:frame display:NO];
			}
			pl->last_frame = win.frame;
			if (atomic_present) {
				if (drawable != nil) {
					[drawable present];
				}
				[CATransaction commit];
				verify_present(pl, win, drawable, f_copy_ptr);
			}
		}
		atomic_store(&pl->inflight, 0);
		dispatch_semaphore_signal(sem);
	});

	// About one refresh: the main thread is normally pumping events.
	if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC)) != 0) {
		int expected = 0;
		if (atomic_compare_exchange_strong(&h->state, &expected, 2)) {
			// The main thread is busy (or blocked on us): present here, the
			// move lands when it gets to it — non-atomic, <= 1 off-phase frame.
			if (!pl->warned_fallback) {
				pl->warned_fallback = true;
				U_LOG_W("macOS placement: main thread did not take the move within 20 ms — presenting "
				        "non-atomically (the move follows; <= 1 off-phase frame)");
			}
			[CATransaction begin];
			if (drawable != nil) {
				[drawable present];
			}
			[CATransaction commit];
		} else {
			// The main thread has it and is presenting right now.
			(void)dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC));
		}
	}
}
