// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671): the macOS backend.
 * @ingroup aux_util
 *
 * ## What the tracker is given
 *
 * An `NSView *` (the view from XR_DXR_cocoa_window_binding, or the runtime's
 * own content view) or an `NSWindow *`, bridged to `void *`. A view is
 * resolved to its window each time it is sampled, so a view not yet in a
 * window, or moved to another one, is handled.
 *
 * ## The signal
 *
 * `-[NSWindow occlusionState]`: AppKit clears NSWindowOcclusionStateVisible
 * when no part of the window can be seen — fully covered by opaque windows,
 * minimized, on another Space, its app hidden, or the display asleep — which
 * is this policy's "hidden" with every rule the Windows tracker spells out
 * (translucent and non-occluding windows, other apps' overlays) already
 * applied by the window server. `isMiniaturized` and `isVisible` give the
 * reason for the log.
 *
 * Not clipped to the 3D screen: a window straddling two screens counts as
 * visible while any part of it is visible on either one (the "visible" side,
 * which keeps the lens in 3D).
 *
 * ## Main thread
 *
 * AppKit state is read on the main thread only, and the tracker thread never
 * waits for it: NSWindowDidChangeOcclusionStateNotification (posted on the
 * main thread) refreshes the sample, and the tracker thread also posts an
 * asynchronous refresh to the main queue at a low rate (at most one in
 * flight) so the first state, a view that gains its window late, and a lost
 * notification are all covered. The tracker thread reads the last sample with
 * one atomic load. If the main thread is not running its run loop, samples
 * stop arriving and the last one stands; nothing blocks.
 *
 * The state the main thread touches lives in a reference-counted holder that
 * each block retains, so the tracker can be destroyed while a block is still
 * queued.
 */

#include "util/u_window_visibility.h"
#include "util/u_window_visibility_posix.h"
#include "os/os_time.h"

#import <AppKit/AppKit.h>

#include <stdlib.h>

#if __has_feature(objc_arc)
#error "u_window_visibility_macos.m uses manual reference counting"
#endif

//! A low-rate refresh is posted at most this often while the window is known.
#define WVM_RESAMPLE_NS (1000ull * 1000ull * 1000ull)

enum wvm_reason
{
	WVM_VISIBLE = 0,
	WVM_NO_WINDOW,
	WVM_MINIMIZED,
	WVM_NOT_VISIBLE,
	WVM_OCCLUDED,
};

@interface DxrLensVisibilityHolder : NSObject
{
@public
	//! The tracked view or window (retained), main thread only.
	id target;
	//! Last sample: an enum wvm_reason (atomic).
	int reason;
	//! A refresh block is queued (atomic).
	int in_flight;
	//! At least one sample has run since the last retarget (atomic).
	int sampled;
}
@end

@implementation DxrLensVisibilityHolder
- (void)dealloc
{
	[target release];
	[super dealloc];
}
@end

struct u_wv_backend
{
	DxrLensVisibilityHolder *holder;
	id observer;
	uint64_t last_post_ns;
};

//! Main thread: the window @p target is in, or nil.
static NSWindow *
wvm_window_of(id target)
{
	if (target == nil) {
		return nil;
	}
	if ([target isKindOfClass:[NSWindow class]]) {
		return (NSWindow *)target;
	}
	if ([target isKindOfClass:[NSView class]]) {
		return [(NSView *)target window];
	}
	return nil;
}

//! Main thread: refresh the holder's sample.
static void
wvm_sample(DxrLensVisibilityHolder *h)
{
	int r = WVM_VISIBLE;
	NSWindow *w = wvm_window_of(h->target);
	if (h->target == nil || w == nil) {
		r = WVM_NO_WINDOW;
	} else if ([w isMiniaturized]) {
		r = WVM_MINIMIZED;
	} else if (![w isVisible]) {
		r = WVM_NOT_VISIBLE;
	} else if (([w occlusionState] & NSWindowOcclusionStateVisible) == 0) {
		r = WVM_OCCLUDED;
	}
	__atomic_store_n(&h->reason, r, __ATOMIC_SEQ_CST);
	__atomic_store_n(&h->sampled, 1, __ATOMIC_SEQ_CST);
}

//! Any thread: queue a refresh unless one is already queued. Never waits.
static void
wvm_post_sample(DxrLensVisibilityHolder *h)
{
	if (__atomic_exchange_n(&h->in_flight, 1, __ATOMIC_SEQ_CST) != 0) {
		return;
	}
	// The block retains the holder (manual reference counting retains
	// captured objects when the block is copied).
	dispatch_async(dispatch_get_main_queue(), ^{
		@autoreleasepool {
			__atomic_store_n(&h->in_flight, 0, __ATOMIC_SEQ_CST);
			wvm_sample(h);
		}
	});
}

struct u_wv_backend *
u_wv_backend_create(enum u_wv_source source)
{
	if (source != U_WV_SOURCE_NATIVE) {
		return NULL; // no Wayland here (#2689)
	}
	struct u_wv_backend *b = (struct u_wv_backend *)calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	@autoreleasepool {
		DxrLensVisibilityHolder *h = [[DxrLensVisibilityHolder alloc] init];
		if (h == nil) {
			free(b);
			return NULL;
		}
		b->holder = h; // owns the +1 from alloc
		// object:nil — the window can change under a view; the block filters.
		// NSNotificationCenter registration is thread-safe; the block runs on
		// the main queue.
		b->observer = [[[NSNotificationCenter defaultCenter]
		    addObserverForName:NSWindowDidChangeOcclusionStateNotification
		                object:nil
		                 queue:[NSOperationQueue mainQueue]
		            usingBlock:^(NSNotification *note) {
			            if (h->target != nil && [note object] == wvm_window_of(h->target)) {
				            wvm_sample(h);
			            }
		            }] retain];
	}
	return b;
}

void
u_wv_backend_set_target(struct u_wv_backend *b, void *native_window)
{
	DxrLensVisibilityHolder *h = b->holder;
	id next = (id)native_window;
	__atomic_store_n(&h->sampled, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&h->reason, WVM_VISIBLE, __ATOMIC_SEQ_CST);
	// The swap happens on the main thread, where the target is read. The
	// block retains @p next while it is queued (the caller's object is alive
	// now, at the call).
	dispatch_async(dispatch_get_main_queue(), ^{
		@autoreleasepool {
			[next retain];
			[h->target release];
			h->target = next;
			wvm_sample(h);
		}
	});
	b->last_post_ns = os_monotonic_get_ns();
}

bool
u_wv_backend_check(struct u_wv_backend *b, const char *panel, const char **out_reason)
{
	(void)panel; // occlusionState is window-wide (see the file comment)
	DxrLensVisibilityHolder *h = b->holder;
	const uint64_t now = os_monotonic_get_ns();
	const int sampled = __atomic_load_n(&h->sampled, __ATOMIC_SEQ_CST);
	const int r = __atomic_load_n(&h->reason, __ATOMIC_SEQ_CST);
	if (sampled == 0 || r == WVM_NO_WINDOW || now - b->last_post_ns >= WVM_RESAMPLE_NS) {
		wvm_post_sample(h);
		b->last_post_ns = now;
	}
	if (sampled == 0) {
		*out_reason = "not sampled yet";
		return false;
	}
	switch (r) {
	case WVM_NO_WINDOW: *out_reason = "no window yet"; return false;
	case WVM_MINIMIZED: *out_reason = "minimized"; return true;
	case WVM_NOT_VISIBLE: *out_reason = "not visible"; return true;
	case WVM_OCCLUDED: *out_reason = "fully covered or on another Space"; return true;
	default: *out_reason = "visible"; return false;
	}
}

int
u_wv_backend_fd(struct u_wv_backend *b)
{
	(void)b;
	return -1; // polling: the main thread's samples land in the holder
}

void
u_wv_backend_drain(struct u_wv_backend *b)
{
	(void)b;
}

void
u_wv_backend_destroy(struct u_wv_backend *b)
{
	if (b == NULL) {
		return;
	}
	@autoreleasepool {
		if (b->observer != nil) {
			[[NSNotificationCenter defaultCenter] removeObserver:b->observer];
			[b->observer release];
		}
		DxrLensVisibilityHolder *h = b->holder;
		// Drop the target on the main thread, where it is read; the block
		// keeps the holder alive until then.
		dispatch_async(dispatch_get_main_queue(), ^{
			[h->target release];
			h->target = nil;
		});
		[h release];
	}
	free(b);
}
