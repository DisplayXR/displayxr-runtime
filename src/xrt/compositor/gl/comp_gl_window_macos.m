// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS window helper for the native GL compositor.
 *
 * Creates an NSWindow with an NSOpenGLView for direct OpenGL presentation.
 * The compositor renders into the NSOpenGLView's context and presents via
 * CGLFlushDrawable (double-buffered).
 *
 * @author David Fattal
 * @ingroup comp_gl
 */

// Silence OpenGL deprecation warnings (macOS deprecated OpenGL in 10.14)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#import <IOSurface/IOSurface.h>

#include "comp_gl_window_macos.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

/*
 *
 * HUD overlay view (semi-transparent text, rendered as NSView subview)
 *
 */

@interface CompGLHudOverlayView : NSView
@property (nonatomic, copy) NSString *hudText;
@end

@implementation CompGLHudOverlayView
- (instancetype)initWithFrame:(NSRect)frame {
	self = [super initWithFrame:frame];
	if (self) { _hudText = @""; [self setWantsLayer:YES]; }
	return self;
}
- (BOOL)isOpaque { return NO; }
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
- (void)drawRect:(NSRect)dirtyRect {
	(void)dirtyRect;
	if (_hudText.length == 0) return;
	NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:6 yRadius:6];
	[[NSColor colorWithCalibratedRed:0 green:0 blue:0 alpha:0.5] setFill];
	[bg fill];
	NSFont *font = [NSFont fontWithName:@"Menlo" size:11];
	if (!font) font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
	NSDictionary *attrs = @{
	    NSFontAttributeName: font,
	    NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.9 green:0.9 blue:0.9 alpha:1.0]
	};
	NSRect textRect = NSInsetRect(self.bounds, 8, 8);
	[_hudText drawWithRect:textRect
	               options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
	            attributes:attrs context:nil];
}
@end

struct comp_gl_window_macos
{
	//! Self-owned NSWindow (NULL if external view).
	NSWindow *window;

	//! The view (either our own NSOpenGLView or the app's).
	NSView *view;

	//! The NSOpenGLContext for rendering.
	NSOpenGLContext *gl_context;

	//! True if we created the window ourselves.
	bool owns_window;

	//! HUD overlay view (for runtime-owned windows).
	CompGLHudOverlayView *hud_view;
};

static void
ensure_ns_app(void)
{
	if (NSApp == nil) {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	}
}

/*!
 * Create an NSOpenGLPixelFormat with a core profile context.
 */
static NSOpenGLPixelFormat *
create_pixel_format(void)
{
	NSOpenGLPixelFormatAttribute attrs[] = {
	    NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
	    NSOpenGLPFAColorSize, 24,
	    NSOpenGLPFAAlphaSize, 8,
	    NSOpenGLPFADepthSize, 24,
	    NSOpenGLPFADoubleBuffer,
	    NSOpenGLPFAAccelerated,
	    0,
	};
	return [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
}

static void
create_window_on_main_thread(struct comp_gl_window_macos *win,
                              uint32_t width,
                              uint32_t height,
                              void *app_cgl_ctx,
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
		U_LOG_E("Failed to create NSWindow for GL compositor");
		*out_success = false;
		return;
	}

	[win->window setTitle:@"DisplayXR — GL Native Compositor"];

	// Create pixel format and GL context
	NSOpenGLPixelFormat *pf = create_pixel_format();
	if (pf == nil) {
		U_LOG_E("Failed to create NSOpenGLPixelFormat");
		*out_success = false;
		return;
	}

	// Share textures with app context if provided
	NSOpenGLContext *share_ctx = nil;
	if (app_cgl_ctx != NULL) {
		// Wrap the app's CGLContextObj in an NSOpenGLContext for sharing
		share_ctx = [[NSOpenGLContext alloc] initWithCGLContextObj:(CGLContextObj)app_cgl_ctx];
	}

	// Create NSOpenGLView
	NSOpenGLView *glView = [[NSOpenGLView alloc] initWithFrame:frame pixelFormat:pf];
	if (glView == nil) {
		U_LOG_E("Failed to create NSOpenGLView");
		*out_success = false;
		return;
	}

	// Create context sharing with app
	NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:share_ctx];
	if (ctx == nil) {
		U_LOG_E("Failed to create NSOpenGLContext");
		*out_success = false;
		return;
	}

	[glView setOpenGLContext:ctx];
	[ctx setView:glView];

	// Enable vsync
	GLint swapInterval = 1;
	[ctx setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];

	// Use a plain NSView as container so the HUD overlay doesn't
	// trigger NSOpenGLView display cycles (which cause GL context
	// threading crashes).
	NSView *container = [[NSView alloc] initWithFrame:frame];
	[container setAutoresizesSubviews:YES];
	[glView setFrame:container.bounds];
	[glView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
	[container addSubview:glView];

	[win->window setContentView:container];
	win->view = glView;
	win->gl_context = ctx;

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

	// Create HUD overlay view as sibling of glView (not subview)
	// so HUD redraws don't interfere with GL context
	NSRect hudFrame = NSMakeRect(10, 10, 420, 380);
	win->hud_view = [[CompGLHudOverlayView alloc] initWithFrame:hudFrame];
	[container addSubview:win->hud_view positioned:NSWindowAbove relativeTo:glView];
	[win->hud_view setHidden:YES];

	win->owns_window = true;
	*out_success = true;
}

xrt_result_t
comp_gl_window_macos_create(uint32_t width,
                            uint32_t height,
                            void *app_cgl_ctx,
                            struct comp_gl_window_macos **out_win)
{
	struct comp_gl_window_macos *win = U_TYPED_CALLOC(struct comp_gl_window_macos);
	if (win == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	__block bool success = false;
	if ([NSThread isMainThread]) {
		create_window_on_main_thread(win, width, height, app_cgl_ctx, &success);
	} else {
		dispatch_sync(dispatch_get_main_queue(), ^{
			create_window_on_main_thread(win, width, height, app_cgl_ctx, &success);
		});
	}

	if (!success) {
		free(win);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_W("Created GL native macOS window (%ux%u)", width, height);
	*out_win = win;
	return XRT_SUCCESS;
}

xrt_result_t
comp_gl_window_macos_setup_external(void *ns_view,
                                    void *app_cgl_ctx,
                                    struct comp_gl_window_macos **out_win)
{
	if (ns_view == NULL) {
		U_LOG_E("External NSView is NULL");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct comp_gl_window_macos *win = U_TYPED_CALLOC(struct comp_gl_window_macos);
	if (win == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	NSView *view = (__bridge NSView *)ns_view;
	win->view = view;
	win->window = nil;
	win->owns_window = false;

	__block bool success = false;
	void (^setup_ctx)(void) = ^{
		NSOpenGLPixelFormat *pf = create_pixel_format();
		if (pf == nil) {
			U_LOG_E("Failed to create NSOpenGLPixelFormat for external view");
			success = false;
			return;
		}

		// If the view is already an NSOpenGLView, use its context
		if ([view isKindOfClass:[NSOpenGLView class]]) {
			NSOpenGLView *glView = (NSOpenGLView *)view;
			win->gl_context = [glView openGLContext];
			if (win->gl_context == nil) {
				// Create context for the existing NSOpenGLView
				NSOpenGLContext *share_ctx = nil;
				if (app_cgl_ctx != NULL) {
					share_ctx = [[NSOpenGLContext alloc]
					    initWithCGLContextObj:(CGLContextObj)app_cgl_ctx];
				}
				win->gl_context = [[NSOpenGLContext alloc]
				    initWithFormat:pf
				     shareContext:share_ctx];
				[glView setOpenGLContext:win->gl_context];
				[win->gl_context setView:glView];
			}
		} else {
			// Regular NSView — create our own GL context and set it on the view
			NSOpenGLContext *share_ctx = nil;
			if (app_cgl_ctx != NULL) {
				share_ctx = [[NSOpenGLContext alloc]
				    initWithCGLContextObj:(CGLContextObj)app_cgl_ctx];
			}
			win->gl_context = [[NSOpenGLContext alloc]
			    initWithFormat:pf
			     shareContext:share_ctx];
			[win->gl_context setView:view];
		}

		if (win->gl_context == nil) {
			U_LOG_E("Failed to create GL context for external view");
			success = false;
			return;
		}

		// Enable vsync
		GLint swapInterval = 1;
		[win->gl_context setValues:&swapInterval
		              forParameter:NSOpenGLContextParameterSwapInterval];

		success = true;
	};

	if ([NSThread isMainThread]) {
		setup_ctx();
	} else {
		dispatch_sync(dispatch_get_main_queue(), setup_ctx);
	}

	if (!success) {
		free(win);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_W("Set up GL native macOS external view");
	*out_win = win;
	return XRT_SUCCESS;
}

xrt_result_t
comp_gl_window_macos_create_offscreen(void *app_cgl_ctx,
                                       struct comp_gl_window_macos **out_win)
{
	struct comp_gl_window_macos *win = U_TYPED_CALLOC(struct comp_gl_window_macos);
	if (win == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	win->window = nil;
	win->view = nil;
	win->owns_window = false;

	// Create offscreen GL context via CGL directly (no NSView needed).
	// Must use matching pixel format for context sharing to work.
	CGLContextObj share = (CGLContextObj)app_cgl_ctx;
	CGLPixelFormatObj pf = NULL;
	CGLError err;

	if (share != NULL) {
		// Get the pixel format from the app's context for compatibility
		pf = CGLGetPixelFormat(share);
	}

	if (pf == NULL) {
		// Fallback: create our own pixel format
		CGLPixelFormatAttribute attrs[] = {
		    kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
		    kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
		    kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
		    kCGLPFAAccelerated,
		    (CGLPixelFormatAttribute)0,
		};
		GLint npix = 0;
		err = CGLChoosePixelFormat(attrs, &pf, &npix);
		if (err != kCGLNoError || pf == NULL) {
			U_LOG_E("CGLChoosePixelFormat failed: %d", err);
			free(win);
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}
	}

	CGLContextObj cgl_ctx = NULL;
	err = CGLCreateContext(pf, share, &cgl_ctx);

	if (err != kCGLNoError || cgl_ctx == NULL) {
		U_LOG_E("CGLCreateContext failed: %d", err);
		free(win);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Wrap in NSOpenGLContext for consistency with other code paths
	win->gl_context = [[NSOpenGLContext alloc] initWithCGLContextObj:cgl_ctx];
	if (win->gl_context == nil) {
		U_LOG_E("Failed to wrap CGLContext in NSOpenGLContext");
		CGLDestroyContext(cgl_ctx);
		free(win);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_W("Created offscreen GL context for shared texture mode");
	*out_win = win;
	return XRT_SUCCESS;
}

xrt_result_t
comp_gl_window_macos_map_iosurface(struct comp_gl_window_macos *win,
                                    void *iosurface_handle,
                                    uint32_t *out_gl_texture,
                                    uint32_t *out_width,
                                    uint32_t *out_height)
{
	if (win == NULL || win->gl_context == nil || iosurface_handle == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	IOSurfaceRef surface = (IOSurfaceRef)iosurface_handle;
	uint32_t w = (uint32_t)IOSurfaceGetWidth(surface);
	uint32_t h = (uint32_t)IOSurfaceGetHeight(surface);

	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_RECTANGLE, tex);

	CGLContextObj cgl = [win->gl_context CGLContextObj];
	CGLError err = CGLTexImageIOSurface2D(
	    cgl, GL_TEXTURE_RECTANGLE, GL_RGBA8,
	    (GLsizei)w, (GLsizei)h,
	    GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
	    surface, 0);
	if (err != kCGLNoError) {
		U_LOG_E("CGLTexImageIOSurface2D failed: %d", err);
		glDeleteTextures(1, &tex);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);

	*out_gl_texture = tex;
	*out_width = w;
	*out_height = h;

	U_LOG_W("IOSurface mapped to GL texture %u: %ux%u", tex, w, h);
	return XRT_SUCCESS;
}

void *
comp_gl_window_macos_get_cgl_context(struct comp_gl_window_macos *win)
{
	if (win == NULL || win->gl_context == nil) return NULL;
	return (void *)[win->gl_context CGLContextObj];
}

bool
comp_gl_window_macos_make_current(struct comp_gl_window_macos *win)
{
	if (win == NULL || win->gl_context == nil) return false;
	[win->gl_context makeCurrentContext];
	return true;
}

void
comp_gl_window_macos_swap_buffers(struct comp_gl_window_macos *win)
{
	if (win == NULL || win->gl_context == nil) return;
	[win->gl_context flushBuffer];
}

void
comp_gl_window_macos_get_dimensions(struct comp_gl_window_macos *win,
                                    uint32_t *out_width,
                                    uint32_t *out_height)
{
	if (win == NULL || win->view == nil) {
		*out_width = 0;
		*out_height = 0;
		return;
	}

	// Get backing pixel size (Retina-aware)
	NSRect backing = [win->view convertRectToBacking:win->view.bounds];
	*out_width = (uint32_t)backing.size.width;
	*out_height = (uint32_t)backing.size.height;
}

bool
comp_gl_window_macos_is_valid(struct comp_gl_window_macos *win)
{
	if (win == NULL) return false;
	if (!win->owns_window) return true; // External view — always valid
	if (win->window == nil) return false;
	return [win->window isVisible];
}

void
comp_gl_window_macos_destroy(struct comp_gl_window_macos **win_ptr)
{
	if (win_ptr == NULL || *win_ptr == NULL) return;

	struct comp_gl_window_macos *win = *win_ptr;

	// Clear current context
	if ([NSOpenGLContext currentContext] == win->gl_context) {
		[NSOpenGLContext clearCurrentContext];
	}

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

	win->gl_context = nil;
	win->hud_view = nil;
	win->window = nil;
	win->view = nil;

	free(win);
	*win_ptr = NULL;
}

void
comp_gl_window_macos_update_hud(struct comp_gl_window_macos *win, const char *text)
{
	if (win == NULL || win->hud_view == nil) return;

	NSString *str = [NSString stringWithUTF8String:text];
	dispatch_async(dispatch_get_main_queue(), ^{
	    win->hud_view.hudText = str;
	    [win->hud_view setNeedsDisplay:YES];
	    [win->hud_view setHidden:NO];
	});
}

void
comp_gl_window_macos_hide_hud(struct comp_gl_window_macos *win)
{
	if (win == NULL || win->hud_view == nil) return;

	if (![win->hud_view isHidden]) {
		dispatch_async(dispatch_get_main_queue(), ^{
		    [win->hud_view setHidden:YES];
		});
	}
}
