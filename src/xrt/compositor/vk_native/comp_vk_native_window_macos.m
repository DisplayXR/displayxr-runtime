// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS window helper for the VK native compositor.
 *
 * Creates an NSWindow with a CAMetalLayer-backed NSView for Vulkan
 * presentation via VK_EXT_metal_surface / MoltenVK.
 *
 * Note: We do NOT set layer.device — MoltenVK manages the MTLDevice
 * association when VkSurfaceKHR is created from the CAMetalLayer.
 *
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "comp_vk_native_window_macos.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

/*!
 * NSView subclass whose backing layer is a CAMetalLayer.
 * Same pattern as CompMetalView in the Metal compositor.
 */
@interface CompVkNativeView : NSView
@end

@implementation CompVkNativeView
- (CALayer *)makeBackingLayer
{
	CAMetalLayer *layer = [CAMetalLayer layer];
	return layer;
}

- (BOOL)wantsUpdateLayer
{
	return YES;
}
@end

struct comp_vk_native_window_macos
{
	//! Self-owned NSWindow (NULL if external view).
	NSWindow *window;

	//! The view (either our own or the app's).
	NSView *view;

	//! The CAMetalLayer for Vulkan surface creation.
	CAMetalLayer *metal_layer;

	//! True if we created the window ourselves.
	bool owns_window;
};

static void
ensure_ns_app(void)
{
	if (NSApp == nil) {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	}
}

static void
create_window_on_main_thread(struct comp_vk_native_window_macos *win,
                              uint32_t width,
                              uint32_t height,
                              bool *out_success)
{
	ensure_ns_app();

	NSRect frame = NSMakeRect(100, 100, width, height);
	NSWindowStyleMask style = NSWindowStyleMaskTitled |
	                          NSWindowStyleMaskClosable |
	                          NSWindowStyleMaskResizable |
	                          NSWindowStyleMaskMiniaturizable;

	win->window = [[NSWindow alloc] initWithContentRect:frame
	                                          styleMask:style
	                                            backing:NSBackingStoreBuffered
	                                              defer:NO];
	if (win->window == nil) {
		U_LOG_E("Failed to create NSWindow for VK native compositor");
		*out_success = false;
		return;
	}

	[win->window setTitle:@"DisplayXR — VK Native Compositor"];

	CompVkNativeView *vkView = [[CompVkNativeView alloc] initWithFrame:frame];
	vkView.wantsLayer = YES;
	[win->window setContentView:vkView];

	win->view = vkView;
	win->metal_layer = (CAMetalLayer *)vkView.layer;

	// Do NOT set layer.device — MoltenVK handles this when VkSurfaceKHR is created
	win->metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	win->metal_layer.framebufferOnly = YES;

	CGFloat scale = win->window.backingScaleFactor;
	win->metal_layer.contentsScale = scale;
	win->metal_layer.drawableSize = CGSizeMake(width * scale, height * scale);

	[win->window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];

	// Pump the event loop once so the window actually appears
	NSEvent *event;
	while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
	                                    untilDate:nil
	                                       inMode:NSDefaultRunLoopMode
	                                      dequeue:YES]) != nil) {
		[NSApp sendEvent:event];
	}

	win->owns_window = true;
	*out_success = true;
}

xrt_result_t
comp_vk_native_window_macos_create(uint32_t width,
                                    uint32_t height,
                                    struct comp_vk_native_window_macos **out_win)
{
	struct comp_vk_native_window_macos *win = U_TYPED_CALLOC(struct comp_vk_native_window_macos);
	if (win == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	__block bool success = false;
	if ([NSThread isMainThread]) {
		create_window_on_main_thread(win, width, height, &success);
	} else {
		dispatch_sync(dispatch_get_main_queue(), ^{
			create_window_on_main_thread(win, width, height, &success);
		});
	}

	if (!success) {
		free(win);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_W("Created VK native macOS window (%ux%u)", width, height);
	*out_win = win;
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_window_macos_setup_external(void *ns_view,
                                            struct comp_vk_native_window_macos **out_win)
{
	if (ns_view == NULL) {
		U_LOG_E("External NSView is NULL");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct comp_vk_native_window_macos *win = U_TYPED_CALLOC(struct comp_vk_native_window_macos);
	if (win == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	NSView *view = (__bridge NSView *)ns_view;
	win->view = view;
	win->window = nil;
	win->owns_window = false;

	void (^setup_layer)(void) = ^{
		if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
			win->metal_layer = (CAMetalLayer *)view.layer;
		} else {
			view.wantsLayer = YES;
			CAMetalLayer *layer = [CAMetalLayer layer];
			// Do NOT set layer.device — MoltenVK handles this
			layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
			layer.frame = view.bounds;
			layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
			[view.layer addSublayer:layer];
			win->metal_layer = layer;
		}

		// Apply Retina scaling
		CGFloat scale = 1.0;
		if (view.window != nil) {
			scale = view.window.backingScaleFactor;
		} else {
			scale = [NSScreen mainScreen].backingScaleFactor;
		}
		win->metal_layer.contentsScale = scale;
	};

	if ([NSThread isMainThread]) {
		setup_layer();
	} else {
		dispatch_sync(dispatch_get_main_queue(), setup_layer);
	}

	if (win->metal_layer == nil) {
		U_LOG_E("Failed to get CAMetalLayer from external view");
		free(win);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_W("Set up VK native macOS external view");
	*out_win = win;
	return XRT_SUCCESS;
}

void *
comp_vk_native_window_macos_get_layer(struct comp_vk_native_window_macos *win)
{
	if (win == NULL) return NULL;
	return (__bridge void *)win->metal_layer;
}

void
comp_vk_native_window_macos_get_dimensions(struct comp_vk_native_window_macos *win,
                                            uint32_t *out_width,
                                            uint32_t *out_height)
{
	if (win == NULL || win->metal_layer == nil) {
		*out_width = 0;
		*out_height = 0;
		return;
	}

	CGSize size = win->metal_layer.drawableSize;
	*out_width = (uint32_t)size.width;
	*out_height = (uint32_t)size.height;
}

bool
comp_vk_native_window_macos_is_valid(struct comp_vk_native_window_macos *win)
{
	if (win == NULL) return false;
	if (!win->owns_window) return true; // External view — always valid
	if (win->window == nil) return false;
	return [win->window isVisible];
}

void
comp_vk_native_window_macos_destroy(struct comp_vk_native_window_macos **win_ptr)
{
	if (win_ptr == NULL || *win_ptr == NULL) return;

	struct comp_vk_native_window_macos *win = *win_ptr;

	if (win->owns_window && win->window != nil) {
		void (^close_window)(void) = ^{
			[win->window close];
		};
		if ([NSThread isMainThread]) {
			close_window();
		} else {
			dispatch_sync(dispatch_get_main_queue(), close_window);
		}
	}

	win->window = nil;
	win->view = nil;
	win->metal_layer = nil;

	free(win);
	*win_ptr = NULL;
}
