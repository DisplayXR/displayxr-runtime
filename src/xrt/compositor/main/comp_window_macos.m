// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS window code using NSWindow + CAMetalLayer for Vulkan via MoltenVK.
 * @author David Fattal
 * @ingroup comp_main
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <stdlib.h>
#include <string.h>

#include "xrt/xrt_compiler.h"
#include "main/comp_window.h"
#include "util/u_misc.h"


/*
 *
 * Private structs.
 *
 */

/*!
 * NSView subclass whose backing layer is a CAMetalLayer.
 * This is the standard approach used by GLFW/SDL for Vulkan-on-macOS.
 * AppKit calls makeBackingLayer when setWantsLayer:YES is set.
 */
@interface MonadoMetalView : NSView
@end

@implementation MonadoMetalView
- (CALayer *)makeBackingLayer
{
	return [CAMetalLayer layer];
}

- (BOOL)wantsUpdateLayer
{
	return YES;
}

- (BOOL)acceptsFirstResponder
{
	return YES;
}
@end


/*!
 * A macOS window using NSWindow + CAMetalLayer.
 *
 * @implements comp_target_swapchain
 */
struct comp_window_macos
{
	struct comp_target_swapchain base;

	NSWindow *window;
	NSView *view;
	CAMetalLayer *metal_layer;
};


/*
 *
 * Forward declarations.
 *
 */

static void
comp_window_macos_destroy(struct comp_target *ct);

static void
comp_window_macos_flush(struct comp_target *ct);

static bool
comp_window_macos_init(struct comp_target *ct);

static bool
comp_window_macos_init_swapchain(struct comp_target *ct, uint32_t width, uint32_t height);

static void
comp_window_macos_update_window_title(struct comp_target *ct, const char *title);

static void
comp_window_macos_destroy_external(struct comp_target *ct);


/*
 *
 * Helper functions.
 *
 */

static inline struct vk_bundle *
get_vk(struct comp_window_macos *cwm)
{
	return &cwm->base.base.c->base.vk;
}

/*!
 * Ensure we have an NSApplication instance running. When loaded as a shared
 * library in a non-Cocoa host app, NSApp may not exist yet.
 */
static void
ensure_ns_app(void)
{
	if (NSApp == nil) {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	}
}


/*
 *
 * Member functions.
 *
 */

struct comp_target *
comp_window_macos_create(struct comp_compositor *c)
{
	struct comp_window_macos *w = U_TYPED_CALLOC(struct comp_window_macos);

	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);

	w->base.base.name = "macos";
	w->base.display = VK_NULL_HANDLE;
	w->base.base.destroy = comp_window_macos_destroy;
	w->base.base.flush = comp_window_macos_flush;
	w->base.base.init_pre_vulkan = comp_window_macos_init;
	w->base.base.init_post_vulkan = comp_window_macos_init_swapchain;
	w->base.base.set_title = comp_window_macos_update_window_title;
	w->base.base.c = c;

	return &w->base.base;
}

static void
comp_window_macos_destroy(struct comp_target *ct)
{
	struct comp_window_macos *w = (struct comp_window_macos *)ct;

	comp_target_swapchain_cleanup(&w->base);

	if (w->window != nil) {
		NSWindow *window = w->window;
		w->window = nil;
		dispatch_async(dispatch_get_main_queue(), ^{
		    [window close];
		});
	}

	w->view = nil;
	w->metal_layer = nil;

	free(ct);
}

static void
comp_window_macos_flush(struct comp_target *ct)
{
	(void)ct;
	// No-op on background thread. CA flushing happens on the main thread
	// in oxr_macos_pump_events(). Background-thread CA flushes can
	// interfere with main-thread CA transaction processing.
}

/*!
 * Block that creates the NSWindow + MonadoMetalView on the current thread.
 * Factored out so it can be dispatched to the main thread if needed.
 */
static bool
comp_window_macos_init_on_main_thread(struct comp_window_macos *w, uint32_t width, uint32_t height)
{
	ensure_ns_app();

	NSRect frame = NSMakeRect(100, 100, width, height);

	@autoreleasepool {
		NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
		                   NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;

		w->window = [[NSWindow alloc] initWithContentRect:frame
		                                        styleMask:style
		                                          backing:NSBackingStoreBuffered
		                                            defer:NO];

		[w->window setTitle:@"Monado"];
		[w->window setAcceptsMouseMovedEvents:YES];

		// Layer-backed approach: use MonadoMetalView with
		// makeBackingLayer to let AppKit manage the CAMetalLayer.
		// Don't pre-configure device/drawableSize — let MoltenVK
		// set everything during vkCreateSwapchainKHR.
		w->view = [[MonadoMetalView alloc] initWithFrame:frame];
		[w->view setWantsLayer:YES];
		w->metal_layer = (CAMetalLayer *)[w->view layer];
		w->metal_layer.contentsScale = [w->window backingScaleFactor];

		[w->window setContentView:w->view];
		[w->window makeKeyAndOrderFront:nil];

		// Bring the app to the front.
		[NSApp activateIgnoringOtherApps:YES];

		// Pump the event loop once so the window actually appears.
		// Without this, makeKeyAndOrderFront is deferred until the
		// next event loop iteration, which never happens because the
		// compositor renders on a background thread.
		NSEvent *event;
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                   untilDate:nil
		                                      inMode:NSDefaultRunLoopMode
		                                     dequeue:YES]) != nil) {
			[NSApp sendEvent:event];
		}
	}

	return (w->window != nil && w->metal_layer != nil);
}

static bool
comp_window_macos_init(struct comp_target *ct)
{
	struct comp_window_macos *w = (struct comp_window_macos *)ct;

	uint32_t width = ct->c->settings.preferred.width;
	uint32_t height = ct->c->settings.preferred.height;

	bool result;
	if ([NSThread isMainThread]) {
		// In-process runtime: xrBeginSession called from app's main thread.
		result = comp_window_macos_init_on_main_thread(w, width, height);
	} else {
		// IPC service mode: begin_session called from per-client IPC handler thread.
		// NSWindow/AppKit operations MUST run on the main thread.
		// The service's main thread runs CFRunLoopRunInMode which processes
		// GCD dispatches, so dispatch_sync will not deadlock.
		__block bool block_result = false;
		dispatch_sync(dispatch_get_main_queue(), ^{
		    block_result = comp_window_macos_init_on_main_thread(w, width, height);
		});
		result = block_result;
	}

	if (!result) {
		COMP_ERROR(ct->c, "Failed to create macOS window");
		return false;
	}

	return true;
}

static VkResult
comp_window_macos_create_surface(struct comp_window_macos *w, VkSurfaceKHR *out_surface)
{
	struct vk_bundle *vk = get_vk(w);
	VkResult ret;

	VkMetalSurfaceCreateInfoEXT surface_info = {
	    .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
	    .pLayer = w->metal_layer,
	};

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	ret = vk->vkCreateMetalSurfaceEXT( //
	    vk->instance,                  //
	    &surface_info,                 //
	    NULL,                          //
	    &surface);                     //
	if (ret != VK_SUCCESS) {
		COMP_ERROR(w->base.base.c, "vkCreateMetalSurfaceEXT: %s", vk_result_string(ret));
		return ret;
	}

	*out_surface = surface;
	return VK_SUCCESS;
}

static bool
comp_window_macos_init_swapchain(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct comp_window_macos *w = (struct comp_window_macos *)ct;
	VkResult ret;

	ret = comp_window_macos_create_surface(w, &w->base.surface.handle);
	if (ret != VK_SUCCESS) {
		return false;
	}

	return true;
}

static void
comp_window_macos_update_window_title(struct comp_target *ct, const char *title)
{
	struct comp_window_macos *w = (struct comp_window_macos *)ct;
	if (w->window == nil) {
		return;
	}

	NSString *ns_title = [NSString stringWithUTF8String:title];

	// NSWindow operations must be on the main thread.
	// compositor_begin_session may call this from the render thread.
	if ([NSThread isMainThread]) {
		[w->window setTitle:ns_title];
	} else {
		dispatch_async(dispatch_get_main_queue(), ^{
		    [w->window setTitle:ns_title];
		});
	}
}

/*!
 * No-op title setter for external windows — the app owns the window.
 */
static void
comp_window_macos_update_window_title_noop(struct comp_target *ct, const char *title)
{
	(void)ct;
	(void)title;
}


/*
 *
 * External window support (XR_EXT_macos_window_binding).
 *
 */

/*!
 * Destroy a comp_target that wraps an app-provided NSView.
 * Does NOT close the window — the application owns it.
 */
static void
comp_window_macos_destroy_external(struct comp_target *ct)
{
	struct comp_window_macos *w = (struct comp_window_macos *)ct;

	comp_target_swapchain_cleanup(&w->base);

	// Don't close window — app owns it
	w->window = nil;
	w->view = nil;
	w->metal_layer = nil;

	free(ct);
}

bool
comp_window_macos_create_from_external(struct comp_compositor *c,
                                       void *external_view,
                                       struct comp_target **out_ct)
{
	if (external_view == NULL) {
		COMP_ERROR(c, "External NSView is NULL");
		return false;
	}

	struct comp_window_macos *w = U_TYPED_CALLOC(struct comp_window_macos);

	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);

	w->base.base.name = "macos (external)";
	w->base.display = VK_NULL_HANDLE;
	w->base.base.c = c;

	// Use app's view — don't create our own window
	@autoreleasepool {
		NSView *view = (__bridge NSView *)external_view;
		w->view = view;
		w->window = [view window]; // May be nil if not yet attached
		w->metal_layer = (CAMetalLayer *)[view layer];

		// Enable Retina rendering: set contentsScale so the CAMetalLayer
		// produces drawables at physical pixel resolution, not point resolution.
		CGFloat scale = 1.0;
		if (w->window != nil) {
			scale = [w->window backingScaleFactor];
			w->metal_layer.contentsScale = scale;
		}

		// Get dimensions in pixels (points × backingScaleFactor)
		NSRect bounds = [view bounds];
		w->base.base.width = (uint32_t)(bounds.size.width * scale);
		w->base.base.height = (uint32_t)(bounds.size.height * scale);
	}

	if (w->metal_layer == nil) {
		COMP_ERROR(c, "External NSView has no CAMetalLayer backing");
		free(w);
		return false;
	}

	// Set function pointers — skip init_pre_vulkan (window already exists)
	w->base.base.destroy = comp_window_macos_destroy_external;
	w->base.base.flush = comp_window_macos_flush;
	w->base.base.init_pre_vulkan = NULL;
	w->base.base.init_post_vulkan = comp_window_macos_init_swapchain;
	w->base.base.set_title = comp_window_macos_update_window_title_noop;

	COMP_INFO(c, "Created macOS target from external NSView %p (%ux%u px)",
	          external_view, w->base.base.width, w->base.base.height);

	*out_ct = &w->base.base;
	return true;
}


/*
 *
 * Factory
 *
 */

static const char *instance_extensions[] = {
    VK_EXT_METAL_SURFACE_EXTENSION_NAME,
};

static bool
detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	// Return true so the deferred factory is selected during system init.
	// Actual window creation is deferred to xrBeginSession (main thread)
	// via target_service_init_main_target or per-session rendering.
	return true;
}

static bool
create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct comp_target *ct = comp_window_macos_create(c);
	if (ct == NULL) {
		return false;
	}

	COMP_DEBUG(c, "Using VK_PRESENT_MODE_FIFO_KHR for macOS window")
	c->settings.present_mode = VK_PRESENT_MODE_FIFO_KHR;

	*out_ct = ct;

	return true;
}

const struct comp_target_factory comp_target_factory_macos = {
    .name = "macOS Window",
    .identifier = "macos",
    .requires_vulkan_for_create = false,
    .is_deferred = true,
    .required_instance_version = 0,
    .required_instance_extensions = instance_extensions,
    .required_instance_extension_count = ARRAY_SIZE(instance_extensions),
    .optional_device_extensions = NULL,
    .optional_device_extension_count = 0,
    .detect = detect,
    .create_target = create_target,
};
