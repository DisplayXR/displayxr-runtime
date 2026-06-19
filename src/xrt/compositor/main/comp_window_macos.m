// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS present target for the out-of-process service compositor.
 * @author David Fattal
 * @ingroup comp_main
 *
 * DisplayXR (macOS shell, Tier 1): a @ref comp_target_swapchain subclass that
 * presents to a runtime-owned NSWindow inside the service process via a
 * VK_EXT_metal_surface (MoltenVK) swapchain over a CAMetalLayer. Mirrors
 * @ref comp_window_android (the out-of-process per-session target pattern), but
 * — unlike Android, where the app injects its ANativeWindow across IPC
 * (android_globals) — macOS cannot pass an NSView across process boundaries, so
 * for the single-app Tier-1 path the service creates and owns the NSWindow (the
 * hosted model). The NSWindow + CAMetalLayer are created on the main thread;
 * the VkSurfaceKHR is created from the CAMetalLayer in init_post_vulkan.
 *
 * The NSWindow/CAMetalLayer creation reuses the proven pattern from the VK
 * native compositor's @ref comp_vk_native_window_macos.m; the surface creation
 * mirrors @ref comp_vk_native_target.cpp (vkCreateMetalSurfaceEXT). Note: ARC is
 * NOT enabled for this file — Cocoa object pointers are held in a plain C struct
 * (the NSWindow is retained by NSApp's window list) just like the VK native
 * helper.
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "xrt/xrt_compiler.h"

#include "util/u_misc.h"
#include "util/u_logging.h"

#include "vk/vk_helpers.h"

#include "main/comp_compositor.h"
#include "main/comp_target_swapchain.h"
#include "main/comp_window_macos.h"

#include <stdlib.h>


/*
 *
 * NSView subclass + private struct.
 *
 */

/*!
 * NSView subclass whose backing layer is a CAMetalLayer. Same pattern as
 * CompMetalView (comp_metal_compositor.m) and CompVkNativeView.
 */
@interface CompWindowMacosView : NSView
@end

@implementation CompWindowMacosView
- (CALayer *)makeBackingLayer
{
	return [CAMetalLayer layer];
}
- (BOOL)wantsUpdateLayer
{
	return YES;
}
@end

/*!
 * A macOS present target.
 *
 * @implements comp_target_swapchain
 */
struct comp_window_macos
{
	struct comp_target_swapchain base;

	//! Runtime-owned window + its CAMetalLayer (created on the main thread).
	NSWindow *window;
	NSView *view;
	CAMetalLayer *metal_layer;
};


/*
 *
 * Functions.
 *
 */

static inline struct vk_bundle *
get_vk(struct comp_window_macos *cwm)
{
	// cwm->base.base.c is the owning compositor. In the service this is a
	// null_compositor up-cast to comp_compositor; both begin with comp_base,
	// so base.vk is the same bundle the system compositor allocates from.
	return &cwm->base.base.c->base.vk;
}

static bool
comp_window_macos_init(struct comp_target *ct)
{
	(void)ct;
	return true;
}

//! Build the NSWindow + CAMetalLayer. Must run on the main thread (AppKit).
static void
create_window_on_main_thread(struct comp_window_macos *cwm, uint32_t width, uint32_t height, bool *out_success)
{
	if (NSApp == nil) {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	}

	// Size the window to the MAIN DISPLAY's full aspect ratio, not the passed
	// dims. The display processor weaves a full-display atlas (physical pixels,
	// e.g. 3024x1964 = 1.54:1) and presents it into this surface; if the window
	// aspect differs the content is non-uniformly scaled (vertical squish). The
	// sim's visible-frame dims (which exclude menu bar / dock) and the
	// 1920x1080 fallback both have the wrong aspect — NSScreen.frame is the full
	// display (its point aspect == the physical-pixel aspect == the atlas
	// aspect), so derive the window shape from it. Fit to ~85% of the screen
	// height so the title bar stays reachable. (#48 aspect fix.)
	CGFloat win_w = (CGFloat)width;
	CGFloat win_h = (CGFloat)height;
	NSScreen *screen = [NSScreen mainScreen];
	if (screen != nil) {
		NSRect sf = screen.frame;
		if (sf.size.width > 0 && sf.size.height > 0) {
			CGFloat aspect = sf.size.width / sf.size.height;
			win_h = sf.size.height * 0.85;
			win_w = win_h * aspect;
		}
	}

	NSRect frame = NSMakeRect(100, 100, win_w, win_h);
	NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
	                          NSWindowStyleMaskMiniaturizable;

	cwm->window = [[NSWindow alloc] initWithContentRect:frame
	                                          styleMask:style
	                                            backing:NSBackingStoreBuffered
	                                              defer:NO];
	if (cwm->window == nil) {
		U_LOG_E("comp_window_macos: failed to create NSWindow");
		*out_success = false;
		return;
	}

	[cwm->window setTitle:@"DisplayXR — Service Compositor"];

	CompWindowMacosView *v = [[CompWindowMacosView alloc] initWithFrame:frame];
	v.wantsLayer = YES;
	[cwm->window setContentView:v];

	cwm->view = v;
	cwm->metal_layer = (CAMetalLayer *)v.layer;

	// Do NOT set layer.device — MoltenVK associates the MTLDevice when the
	// VkSurfaceKHR is created from the CAMetalLayer.
	cwm->metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	cwm->metal_layer.framebufferOnly = NO;

	CGFloat scale = cwm->window.backingScaleFactor;
	cwm->metal_layer.contentsScale = scale;
	cwm->metal_layer.drawableSize = CGSizeMake(win_w * scale, win_h * scale);

	[cwm->window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];

	*out_success = true;
}

static VkResult
comp_window_macos_create_surface(struct comp_window_macos *cwm, VkSurfaceKHR *out_surface)
{
	struct vk_bundle *vk = get_vk(cwm);

	PFN_vkCreateMetalSurfaceEXT pfn = vk->vkCreateMetalSurfaceEXT;
	if (pfn == NULL) {
		pfn = (PFN_vkCreateMetalSurfaceEXT)vk->vkGetInstanceProcAddr(vk->instance, "vkCreateMetalSurfaceEXT");
	}
	if (pfn == NULL) {
		U_LOG_E("comp_window_macos: vkCreateMetalSurfaceEXT not available — VK_EXT_metal_surface must be enabled");
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}

	VkMetalSurfaceCreateInfoEXT surface_info = {
	    .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
	    .pLayer = (const CAMetalLayer *)cwm->metal_layer,
	};

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkResult ret = pfn(vk->instance, &surface_info, NULL, &surface);
	if (ret != VK_SUCCESS) {
		U_LOG_E("comp_window_macos: vkCreateMetalSurfaceEXT: %s", vk_result_string(ret));
		return ret;
	}

	VK_NAME_SURFACE(vk, surface, "comp_window_macos surface");
	*out_surface = surface;
	return VK_SUCCESS;
}

static bool
comp_window_macos_init_swapchain(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;

	// AppKit requires NSWindow creation on the main thread. The comp_multi
	// per-session render thread is not the main thread, so dispatch the
	// window build to the main queue. (The service main thread must drain the
	// main dispatch queue / run an NSApp runloop for this to complete — see
	// the macOS service mainloop.)
	__block bool ok = false;
	if ([NSThread isMainThread]) {
		create_window_on_main_thread(cwm, width, height, &ok);
	} else {
		dispatch_sync(dispatch_get_main_queue(), ^{
			create_window_on_main_thread(cwm, width, height, &ok);
		});
	}
	if (!ok || cwm->metal_layer == nil) {
		U_LOG_E("comp_window_macos: failed to create NSWindow/CAMetalLayer");
		return false;
	}

	VkResult ret = comp_window_macos_create_surface(cwm, &cwm->base.surface.handle);
	if (ret != VK_SUCCESS) {
		return false;
	}

	U_LOG_W("comp_window_macos: VkSurfaceKHR created from CAMetalLayer (%ux%u)", width, height);
	return true;
}

static void
comp_window_macos_flush(struct comp_target *ct)
{
	(void)ct;
}

static void
comp_window_macos_update_window_title(struct comp_target *ct, const char *title)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	if (cwm->window == nil || title == NULL) {
		return;
	}
	NSString *t = [NSString stringWithUTF8String:title];
	void (^set_title)(void) = ^{
		[cwm->window setTitle:t];
	};
	if ([NSThread isMainThread]) {
		set_title();
	} else {
		dispatch_async(dispatch_get_main_queue(), set_title);
	}
}

static void
comp_window_macos_destroy(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;

	comp_target_swapchain_cleanup(&cwm->base);

	if (cwm->window != nil) {
		NSWindow *w = cwm->window;
		void (^close_window)(void) = ^{
			[w close];
		};
		if ([NSThread isMainThread]) {
			close_window();
		} else {
			dispatch_sync(dispatch_get_main_queue(), close_window);
		}
	}
	cwm->window = nil;
	cwm->view = nil;
	cwm->metal_layer = nil;

	free(ct);
}

struct comp_target *
comp_window_macos_create(struct comp_compositor *c)
{
	struct comp_window_macos *w = U_TYPED_CALLOC(struct comp_window_macos);
	if (w == NULL) {
		return NULL;
	}

	// Display-timing has not been exercised on the macOS service path; force
	// fake timing as the Android target does.
	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);

	w->base.base.name = "macOS";
	w->base.base.destroy = comp_window_macos_destroy;
	w->base.base.flush = comp_window_macos_flush;
	w->base.base.init_pre_vulkan = comp_window_macos_init;
	w->base.base.init_post_vulkan = comp_window_macos_init_swapchain;
	w->base.base.set_title = comp_window_macos_update_window_title;
	w->base.base.c = c;

	return &w->base.base;
}
