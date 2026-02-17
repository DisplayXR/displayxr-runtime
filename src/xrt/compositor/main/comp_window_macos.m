// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS window code using NSWindow + CAMetalLayer for Vulkan via MoltenVK.
 * @author David Fattal
 * @ingroup comp_main
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

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
		// Must close on main thread
		if ([NSThread isMainThread]) {
			[w->window close];
		} else {
			dispatch_sync(dispatch_get_main_queue(), ^{
			    [w->window close];
			});
		}
		w->window = nil;
	}

	w->view = nil;
	w->metal_layer = nil;

	free(ct);
}

static void
comp_window_macos_flush(struct comp_target *ct)
{
	// NSApp event processing must happen on the main thread.
	// In IPC service mode the compositor renders on a background thread,
	// so skip here — the main loop's CFRunLoopRunInMode handles it.
	if (![NSThread isMainThread]) {
		return;
	}

	@autoreleasepool {
		// Pump the macOS event loop so the window remains responsive.
		NSEvent *event;
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                   untilDate:nil
		                                      inMode:NSDefaultRunLoopMode
		                                     dequeue:YES]) != nil) {
			[NSApp sendEvent:event];
		}
	}
}

static bool
comp_window_macos_init(struct comp_target *ct)
{
	struct comp_window_macos *w = (struct comp_window_macos *)ct;

	ensure_ns_app();

	// Window dimensions from compositor settings.
	uint32_t width = ct->c->settings.preferred.width;
	uint32_t height = ct->c->settings.preferred.height;

	NSRect frame = NSMakeRect(100, 100, width, height);

	// Block that creates the window — must run on main thread.
	void (^create_block)(void) = ^{
	    @autoreleasepool {
		    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
		                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;

		    w->window = [[NSWindow alloc] initWithContentRect:frame
		                                            styleMask:style
		                                              backing:NSBackingStoreBuffered
		                                                defer:NO];

		    [w->window setTitle:@"Monado"];
		    [w->window setAcceptsMouseMovedEvents:YES];

		    // Create a view with a CAMetalLayer for Vulkan rendering.
		    w->view = [[NSView alloc] initWithFrame:frame];
		    [w->view setWantsLayer:YES];

		    w->metal_layer = [CAMetalLayer layer];
		    [w->view setLayer:w->metal_layer];

		    [w->window setContentView:w->view];
		    [w->window makeKeyAndOrderFront:nil];

		    // Bring the app to the front.
		    [NSApp activateIgnoringOtherApps:YES];
	    }
	};

	if ([NSThread isMainThread]) {
		create_block();
	} else {
		dispatch_sync(dispatch_get_main_queue(), create_block);
	}

	if (w->window == nil || w->metal_layer == nil) {
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
	if ([NSThread isMainThread]) {
		[w->window setTitle:ns_title];
	} else {
		dispatch_async(dispatch_get_main_queue(), ^{
		    [w->window setTitle:ns_title];
		});
	}
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
	return false;
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
