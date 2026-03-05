// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Metal compositor implementation.
 *
 * Mirrors the D3D11 native compositor but uses Metal instead:
 * - Creates Metal textures as swapchain images
 * - Renders layers into SBS stereo texture using Metal shaders
 * - Presents to CAMetalLayer via drawable
 * - Optionally processes through display processor (LeiaSR weaver)
 *
 * @author David Fattal
 * @ingroup comp_metal
 */

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>

#include "comp_metal_compositor.h"

#include "util/comp_layer_accum.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_processor_metal.h"
#include "xrt/xrt_system.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_time.h"
#include "os/os_threading.h"

#include "math/m_api.h"
#include "util/u_hud.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 *
 * Metal swapchain image structure
 *
 */

#define METAL_SWAPCHAIN_MAX_IMAGES 8

struct comp_metal_swapchain
{
	struct xrt_swapchain_native base;

	id<MTLTexture> images[METAL_SWAPCHAIN_MAX_IMAGES];

	uint32_t image_count;
	struct xrt_swapchain_create_info info;

	int32_t acquired_index;
	int32_t waited_index;
	uint32_t last_released_index;
};

/*
 *
 * HUD overlay view (semi-transparent text, rendered as NSView subview)
 *
 */

@interface CompHudOverlayView : NSView
@property (nonatomic, copy) NSString *hudText;
@end

@implementation CompHudOverlayView
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

/*
 *
 * Metal compositor structure
 *
 */

struct comp_metal_compositor
{
	//! Base type - must be first!
	struct xrt_compositor_native base;

	//! The device we are rendering for.
	struct xrt_device *xdev;

	//! Metal device (obtained from command queue).
	id<MTLDevice> device;

	//! Metal command queue (from app's graphics binding).
	id<MTLCommandQueue> command_queue;

	//! CAMetalLayer for presentation.
	CAMetalLayer *metal_layer;

	//! Render pipeline for SBS layer compositing.
	id<MTLRenderPipelineState> projection_pipeline;

	//! Render pipeline for fullscreen blit (stereo→target, SBS passthrough).
	id<MTLRenderPipelineState> blit_pipeline;

	//! Render pipeline for red-cyan anaglyph output.
	id<MTLRenderPipelineState> anaglyph_pipeline;

	//! Render pipeline for 50/50 alpha-blend output.
	id<MTLRenderPipelineState> blend_pipeline;

	//! Sampler state for texture sampling.
	id<MTLSamplerState> sampler_linear;

	//! Depth stencil state.
	id<MTLDepthStencilState> depth_stencil_state;

	//! SBS stereo texture (2 * view_width × view_height).
	id<MTLTexture> stereo_texture;

	//! Depth texture matching stereo texture.
	id<MTLTexture> depth_texture;

	//! Accumulated layers for the current frame.
	struct comp_layer_accum layer_accum;

	//! Per-eye view width (half of stereo texture).
	uint32_t view_width;

	//! Per-eye view height.
	uint32_t view_height;

	//! Window (either from app or self-created).
	NSWindow *window;

	//! View containing the CAMetalLayer.
	NSView *view;

	//! True if we created the window ourselves.
	bool owns_window;

	//! True if running in offscreen mode (hidden window, no visible UI).
	bool offscreen;

	//! App-provided IOSurface for shared texture output (retained, may be NULL).
	IOSurfaceRef shared_iosurface;

	//! Metal texture wrapping the shared IOSurface (render target).
	id<MTLTexture> shared_texture;

	//! Generic Metal display processor.
	struct xrt_display_processor_metal *display_processor;

	//! System devices (for qwerty driver keyboard input).
	struct xrt_system_devices *xsysd;

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;

	//! HUD overlay view (for runtime-owned windows).
	CompHudOverlayView *hud_view;

	//! HUD update timer (seconds since last update).
	float hud_timer;

	//! Last frame timestamp for FPS/frame time.
	uint64_t last_frame_ns;

	//! Smoothed frame time in ms.
	float smoothed_frame_time_ms;

	//! System compositor info (for display dimensions, nominal viewer).
	const struct xrt_system_compositor_info *sys_info;

	//! Thread safety.
	struct os_mutex mutex;
};

/*
 *
 * Helper functions
 *
 */

static inline struct comp_metal_compositor *
metal_comp(struct xrt_compositor *xc)
{
	return (struct comp_metal_compositor *)xc;
}

static inline struct comp_metal_swapchain *
metal_swapchain(struct xrt_swapchain *xsc)
{
	return (struct comp_metal_swapchain *)xsc;
}

/*
 *
 * Metal shader source (embedded MSL)
 *
 */

static NSString *const metal_shader_source = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct VertexOut {\n"
    "    float4 position [[position]];\n"
    "    float2 texCoord;\n"
    "};\n"
    "\n"
    "// Fullscreen triangle - no vertex buffer needed\n"
    "vertex VertexOut blit_vertex(uint vid [[vertex_id]]) {\n"
    "    VertexOut out;\n"
    "    // Generate fullscreen triangle covering clip space\n"
    "    out.texCoord = float2((vid << 1) & 2, vid & 2);\n"
    "    out.position = float4(out.texCoord * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return out;\n"
    "}\n"
    "\n"
    "fragment float4 blit_fragment(VertexOut in [[stage_in]],\n"
    "                              texture2d<float> tex [[texture(0)]],\n"
    "                              sampler smp [[sampler(0)]]) {\n"
    "    return tex.sample(smp, in.texCoord);\n"
    "}\n"
    "\n"
    "// Anaglyph: red from left eye (left half), cyan from right eye (right half)\n"
    "fragment float4 anaglyph_fragment(VertexOut in [[stage_in]],\n"
    "                                  texture2d<float> tex [[texture(0)]],\n"
    "                                  sampler smp [[sampler(0)]]) {\n"
    "    float2 left_uv  = float2(in.texCoord.x * 0.5, in.texCoord.y);\n"
    "    float2 right_uv = float2(in.texCoord.x * 0.5 + 0.5, in.texCoord.y);\n"
    "    float4 left  = tex.sample(smp, left_uv);\n"
    "    float4 right = tex.sample(smp, right_uv);\n"
    "    return float4(left.r, right.g, right.b, 1.0);\n"
    "}\n"
    "\n"
    "// Blend: 50/50 mix of left eye (left half) and right eye (right half)\n"
    "fragment float4 blend_fragment(VertexOut in [[stage_in]],\n"
    "                               texture2d<float> tex [[texture(0)]],\n"
    "                               sampler smp [[sampler(0)]]) {\n"
    "    float2 left_uv  = float2(in.texCoord.x * 0.5, in.texCoord.y);\n"
    "    float2 right_uv = float2(in.texCoord.x * 0.5 + 0.5, in.texCoord.y);\n"
    "    return mix(tex.sample(smp, left_uv), tex.sample(smp, right_uv), 0.5);\n"
    "}\n"
    "\n"
    "struct ProjectionConstants {\n"
    "    float4 viewport;  // x, y, width, height (output, in normalized coords)\n"
    "    float4 src_rect;  // x, y, width, height (input UV sub-region)\n"
    "    float4 color_scale;\n"
    "    float4 color_bias;\n"
    "};\n"
    "\n"
    "vertex VertexOut projection_vertex(uint vid [[vertex_id]],\n"
    "                                   constant ProjectionConstants &pc [[buffer(0)]]) {\n"
    "    VertexOut out;\n"
    "    float2 uv = float2((vid << 1) & 2, vid & 2);\n"
    "    // Map to viewport region\n"
    "    float2 pos = pc.viewport.xy + uv * pc.viewport.zw;\n"
    "    out.position = float4(pos * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    // Map UV to source sub-region\n"
    "    out.texCoord = pc.src_rect.xy + uv * pc.src_rect.zw;\n"
    "    return out;\n"
    "}\n"
    "\n"
    "fragment float4 projection_fragment(VertexOut in [[stage_in]],\n"
    "                                    texture2d<float> tex [[texture(0)]],\n"
    "                                    sampler smp [[sampler(0)]],\n"
    "                                    constant ProjectionConstants &pc [[buffer(0)]]) {\n"
    "    float4 color = tex.sample(smp, in.texCoord);\n"
    "    return color * pc.color_scale + pc.color_bias;\n"
    "}\n";

/*
 *
 * Format conversion
 *
 */

static MTLPixelFormat
xrt_format_to_metal(int64_t format)
{
	// Check if already a Metal format (MTLPixelFormat values are > 0 and < 1000 typically)
	if (format > 0 && format < 1000) {
		return (MTLPixelFormat)format;
	}

	// Map common Vulkan formats to Metal
	switch (format) {
	case 37: return MTLPixelFormatRGBA8Unorm;       // VK_FORMAT_R8G8B8A8_UNORM
	case 43: return MTLPixelFormatRGBA8Unorm_sRGB;   // VK_FORMAT_R8G8B8A8_SRGB
	case 44: return MTLPixelFormatBGRA8Unorm;         // VK_FORMAT_B8G8R8A8_UNORM
	case 50: return MTLPixelFormatBGRA8Unorm_sRGB;    // VK_FORMAT_B8G8R8A8_SRGB
	case 97: return MTLPixelFormatRGBA16Float;        // VK_FORMAT_R16G16B16A16_SFLOAT
	case 64: return MTLPixelFormatRGB10A2Unorm;       // VK_FORMAT_A2B10G10R10_UNORM_PACK32
	default: return MTLPixelFormatRGBA8Unorm;
	}
}

/*
 *
 * Shader compilation
 *
 */

static bool
compile_shaders(struct comp_metal_compositor *c)
{
	NSError *error = nil;
	id<MTLLibrary> library = [c->device newLibraryWithSource:metal_shader_source
	                                                 options:nil
	                                                   error:&error];
	if (library == nil) {
		U_LOG_E("Failed to compile Metal shaders: %s",
		        error.localizedDescription.UTF8String);
		return false;
	}

	id<MTLFunction> blit_vs = [library newFunctionWithName:@"blit_vertex"];
	id<MTLFunction> blit_fs = [library newFunctionWithName:@"blit_fragment"];
	id<MTLFunction> anaglyph_fs = [library newFunctionWithName:@"anaglyph_fragment"];
	id<MTLFunction> blend_fs = [library newFunctionWithName:@"blend_fragment"];
	id<MTLFunction> proj_vs = [library newFunctionWithName:@"projection_vertex"];
	id<MTLFunction> proj_fs = [library newFunctionWithName:@"projection_fragment"];

	// Blit pipeline
	MTLRenderPipelineDescriptor *blit_desc = [[MTLRenderPipelineDescriptor alloc] init];
	blit_desc.vertexFunction = blit_vs;
	blit_desc.fragmentFunction = blit_fs;
	blit_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

	c->blit_pipeline = [c->device newRenderPipelineStateWithDescriptor:blit_desc error:&error];
	if (c->blit_pipeline == nil) {
		U_LOG_E("Failed to create blit pipeline: %s",
		        error.localizedDescription.UTF8String);
		return false;
	}

	// Anaglyph pipeline (red from left eye, cyan from right)
	MTLRenderPipelineDescriptor *anag_desc = [[MTLRenderPipelineDescriptor alloc] init];
	anag_desc.vertexFunction = blit_vs;
	anag_desc.fragmentFunction = anaglyph_fs;
	anag_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

	c->anaglyph_pipeline = [c->device newRenderPipelineStateWithDescriptor:anag_desc error:&error];
	if (c->anaglyph_pipeline == nil) {
		U_LOG_E("Failed to create anaglyph pipeline: %s",
		        error.localizedDescription.UTF8String);
		return false;
	}

	// Blend pipeline (50/50 mix of both eyes)
	MTLRenderPipelineDescriptor *blend_desc = [[MTLRenderPipelineDescriptor alloc] init];
	blend_desc.vertexFunction = blit_vs;
	blend_desc.fragmentFunction = blend_fs;
	blend_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

	c->blend_pipeline = [c->device newRenderPipelineStateWithDescriptor:blend_desc error:&error];
	if (c->blend_pipeline == nil) {
		U_LOG_E("Failed to create blend pipeline: %s",
		        error.localizedDescription.UTF8String);
		return false;
	}

	// Projection pipeline (renders into stereo texture)
	MTLRenderPipelineDescriptor *proj_desc = [[MTLRenderPipelineDescriptor alloc] init];
	proj_desc.vertexFunction = proj_vs;
	proj_desc.fragmentFunction = proj_fs;
	proj_desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	proj_desc.colorAttachments[0].blendingEnabled = YES;
	proj_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
	proj_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	proj_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	proj_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	proj_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

	c->projection_pipeline = [c->device newRenderPipelineStateWithDescriptor:proj_desc error:&error];
	if (c->projection_pipeline == nil) {
		U_LOG_E("Failed to create projection pipeline: %s",
		        error.localizedDescription.UTF8String);
		return false;
	}

	// Sampler
	MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
	sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
	sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
	sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
	c->sampler_linear = [c->device newSamplerStateWithDescriptor:sampler_desc];

	// Depth stencil state
	MTLDepthStencilDescriptor *ds_desc = [[MTLDepthStencilDescriptor alloc] init];
	ds_desc.depthCompareFunction = MTLCompareFunctionLessEqual;
	ds_desc.depthWriteEnabled = YES;
	c->depth_stencil_state = [c->device newDepthStencilStateWithDescriptor:ds_desc];

	return true;
}

/*
 *
 * Stereo texture management
 *
 */

static bool
create_stereo_texture(struct comp_metal_compositor *c, uint32_t view_width, uint32_t view_height)
{
	// SBS stereo texture: 2 * view_width × view_height
	MTLTextureDescriptor *desc = [MTLTextureDescriptor
	    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                width:view_width * 2
	                               height:view_height
	                            mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;

	c->stereo_texture = [c->device newTextureWithDescriptor:desc];
	if (c->stereo_texture == nil) {
		U_LOG_E("Failed to create stereo texture");
		return false;
	}

	// Depth texture
	MTLTextureDescriptor *depth_desc = [MTLTextureDescriptor
	    texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
	                                width:view_width * 2
	                               height:view_height
	                            mipmapped:NO];
	depth_desc.usage = MTLTextureUsageRenderTarget;
	depth_desc.storageMode = MTLStorageModePrivate;

	c->depth_texture = [c->device newTextureWithDescriptor:depth_desc];
	if (c->depth_texture == nil) {
		U_LOG_E("Failed to create depth texture");
		return false;
	}

	c->view_width = view_width;
	c->view_height = view_height;

	U_LOG_I("Created stereo texture: %ux%u (per-eye: %ux%u)",
	        view_width * 2, view_height, view_width, view_height);

	return true;
}

/*
 *
 * Window management
 *
 */

@interface CompMetalView : NSView
@end

@implementation CompMetalView
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

static void
create_window_on_main_thread(struct comp_metal_compositor *c, uint32_t width, uint32_t height, bool *out_success)
{
	ensure_ns_app();
	NSRect frame = NSMakeRect(100, 100, width, height);
	NSWindowStyleMask style = NSWindowStyleMaskTitled |
	                          NSWindowStyleMaskClosable |
	                          NSWindowStyleMaskResizable |
	                          NSWindowStyleMaskMiniaturizable;

	c->window = [[NSWindow alloc] initWithContentRect:frame
	                                        styleMask:style
	                                          backing:NSBackingStoreBuffered
	                                            defer:NO];

	if (c->window == nil) {
		U_LOG_E("Failed to create NSWindow");
		*out_success = false;
		return;
	}

	[c->window setTitle:@"Monado OpenXR (Metal)"];

	CompMetalView *metalView = [[CompMetalView alloc] initWithFrame:frame];
	metalView.wantsLayer = YES;
	[c->window setContentView:metalView];

	c->view = metalView;
	c->metal_layer = (CAMetalLayer *)metalView.layer;

	c->metal_layer.device = c->device;
	c->metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	c->metal_layer.framebufferOnly = YES;

	CGFloat scale = c->window.backingScaleFactor;
	c->metal_layer.contentsScale = scale;
	c->metal_layer.drawableSize = CGSizeMake(width * scale, height * scale);

	if (!c->offscreen) {
		[c->window makeKeyAndOrderFront:nil];

		// Bring the app to the front.
		[NSApp activateIgnoringOtherApps:YES];

		// Pump the event loop once so the window actually appears.
		// Without this, makeKeyAndOrderFront is deferred until the
		// next event loop iteration, which may never happen because the
		// compositor renders on a background thread.
		NSEvent *event;
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                   untilDate:nil
		                                      inMode:NSDefaultRunLoopMode
		                                     dequeue:YES]) != nil) {
			[NSApp sendEvent:event];
		}
	} else {
		U_LOG_I("Offscreen mode — window created but hidden");
	}

	c->owns_window = true;

	if (!c->offscreen) {
		// Create HUD overlay view (bottom-left, hidden initially)
		NSRect hudFrame = NSMakeRect(10, 10, 420, 380);
		c->hud_view = [[CompHudOverlayView alloc] initWithFrame:hudFrame];
		[metalView addSubview:c->hud_view];
		[c->hud_view setHidden:YES];
	}

	*out_success = true;
}

static bool
create_window(struct comp_metal_compositor *c, uint32_t width, uint32_t height)
{
	__block bool success = false;

	if ([NSThread isMainThread]) {
		// Already on main thread — call directly to avoid deadlock
		create_window_on_main_thread(c, width, height, &success);
	} else {
		dispatch_sync(dispatch_get_main_queue(), ^{
			create_window_on_main_thread(c, width, height, &success);
		});
	}

	return success;
}

static bool
setup_external_window(struct comp_metal_compositor *c, NSView *external_view)
{
	c->view = external_view;
	c->owns_window = false;

	if ([external_view.layer isKindOfClass:[CAMetalLayer class]]) {
		c->metal_layer = (CAMetalLayer *)external_view.layer;
	} else {
		// View doesn't have a CAMetalLayer - create one
		void (^setup_layer)(void) = ^{
			external_view.wantsLayer = YES;
			CAMetalLayer *layer = [CAMetalLayer layer];
			layer.device = c->device;
			layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
			external_view.layer = layer;
			c->metal_layer = layer;
		};
		if ([NSThread isMainThread]) {
			setup_layer();
		} else {
			dispatch_sync(dispatch_get_main_queue(), setup_layer);
		}
	}

	if (c->metal_layer == nil) {
		U_LOG_E("Failed to get CAMetalLayer from external view");
		return false;
	}

	c->metal_layer.device = c->device;
	c->metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	c->metal_layer.framebufferOnly = NO; // Need shader read for blit

	// Ensure Retina backing scale is applied so drawables are at physical resolution
	CGFloat scale = 1.0;
	if (external_view.window != nil) {
		scale = external_view.window.backingScaleFactor;
	} else {
		scale = [NSScreen mainScreen].backingScaleFactor;
	}
	c->metal_layer.contentsScale = scale;

	return true;
}

/*
 *
 * Swapchain functions
 *
 */

static void
metal_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct comp_metal_swapchain *msc = metal_swapchain(xsc);

	for (uint32_t i = 0; i < msc->image_count; i++) {
		msc->images[i] = nil;
	}

	free(msc);
}

static xrt_result_t
metal_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct comp_metal_swapchain *msc = metal_swapchain(xsc);

	uint32_t index = (msc->last_released_index + 1) % msc->image_count;
	msc->acquired_index = (int32_t)index;

	*out_index = index;
	return XRT_SUCCESS;
}

static xrt_result_t
metal_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct comp_metal_swapchain *msc = metal_swapchain(xsc);
	msc->waited_index = (int32_t)index;
	return XRT_SUCCESS;
}

static xrt_result_t
metal_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_metal_swapchain *msc = metal_swapchain(xsc);
	msc->last_released_index = index;
	msc->acquired_index = -1;
	msc->waited_index = -1;
	return XRT_SUCCESS;
}

static xrt_result_t
metal_swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	(void)xsc;
	(void)direction;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
metal_swapchain_inc_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	(void)xsc;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
metal_swapchain_dec_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	(void)xsc;
	(void)index;
	return XRT_SUCCESS;
}

/*
 *
 * xrt_compositor member functions
 *
 */

static xrt_result_t
metal_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                                  const struct xrt_swapchain_create_info *info,
                                                  struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 3; // Triple buffering
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_create_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_swapchain **out_xsc)
{
	struct comp_metal_compositor *c = metal_comp(xc);

	struct comp_metal_swapchain *msc = U_TYPED_CALLOC(struct comp_metal_swapchain);
	if (msc == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	msc->info = *info;
	msc->image_count = 3;
	msc->acquired_index = -1;
	msc->waited_index = -1;

	MTLPixelFormat format = xrt_format_to_metal(info->format);

	for (uint32_t i = 0; i < msc->image_count; i++) {
		MTLTextureDescriptor *desc = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:format
		                                width:info->width
		                               height:info->height
		                            mipmapped:(info->mip_count > 1) ? YES : NO];
		desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModePrivate;
		if (info->array_size > 1) {
			desc.textureType = MTLTextureType2DArray;
			desc.arrayLength = info->array_size;
		}

		msc->images[i] = [c->device newTextureWithDescriptor:desc];
		if (msc->images[i] == nil) {
			U_LOG_E("Failed to create swapchain image %u", i);
			metal_swapchain_destroy(&msc->base.base);
			return XRT_ERROR_ALLOCATION;
		}

		// Store the Metal texture pointer in the native handle
		// This is what the app receives via xrEnumerateSwapchainImages
		msc->base.images[i].handle = (xrt_graphics_buffer_handle_t)(uintptr_t)(__bridge void *)msc->images[i];
	}

	// Set up vtable
	msc->base.base.destroy = metal_swapchain_destroy;
	msc->base.base.acquire_image = metal_swapchain_acquire_image;
	msc->base.base.wait_image = metal_swapchain_wait_image;
	msc->base.base.release_image = metal_swapchain_release_image;
	msc->base.base.barrier_image = metal_swapchain_barrier_image;
	msc->base.base.inc_image_use = metal_swapchain_inc_image_use;
	msc->base.base.dec_image_use = metal_swapchain_dec_image_use;
	msc->base.base.image_count = msc->image_count;
	msc->base.base.reference.count = 1;

	*out_xsc = &msc->base.base;

	U_LOG_I("Created Metal swapchain: %ux%u format=%d images=%u",
	        info->width, info->height, (int)info->format, msc->image_count);

	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_import_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_image_native *native_images,
                                   uint32_t image_count,
                                   struct xrt_swapchain **out_xsc)
{
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
metal_compositor_import_fence(struct xrt_compositor *xc,
                               xrt_graphics_sync_handle_t handle,
                               struct xrt_compositor_fence **out_xcf)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
metal_compositor_create_semaphore(struct xrt_compositor *xc,
                                   xrt_graphics_sync_handle_t *out_handle,
                                   struct xrt_compositor_semaphore **out_xcsem)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
metal_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	U_LOG_I("Metal compositor session begin - window=%p, owns_window=%d",
	        (__bridge void *)c->window, c->owns_window);
	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_end_session(struct xrt_compositor *xc)
{
	U_LOG_I("Metal compositor session end");
	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_predict_frame(struct xrt_compositor *xc,
                                int64_t *out_frame_id,
                                int64_t *out_wake_time_ns,
                                int64_t *out_predicted_gpu_time_ns,
                                int64_t *out_predicted_display_time_ns,
                                int64_t *out_predicted_display_period_ns)
{
	struct comp_metal_compositor *c = metal_comp(xc);

	os_mutex_lock(&c->mutex);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	int64_t period_ns = (int64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate);

	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = period_ns;

	c->last_display_time_ns = (uint64_t)*out_predicted_display_time_ns;

	os_mutex_unlock(&c->mutex);

	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_wait_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
	struct comp_metal_compositor *c = metal_comp(xc);

	int64_t period_ns = (int64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate);

	os_mutex_lock(&c->mutex);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;

	c->last_display_time_ns = (uint64_t)*out_predicted_display_time_ns;

	os_mutex_unlock(&c->mutex);

	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_mark_frame(struct xrt_compositor *xc,
                             int64_t frame_id,
                             enum xrt_compositor_frame_point point,
                             int64_t when_ns)
{
	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	return XRT_SUCCESS;
}

/*
 *
 * Layer accumulation
 *
 */

static xrt_result_t
metal_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	comp_layer_accum_begin(&c->layer_accum, data);
	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_layer_projection(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                   const struct xrt_layer_data *data)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                         struct xrt_device *xdev,
                                         struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                         struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                         const struct xrt_layer_data *data)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
metal_compositor_layer_quad(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * Update the HUD overlay text (throttled, main-thread safe).
 */
static void
metal_compositor_update_hud(struct comp_metal_compositor *c, float dt)
{
	if (c->hud_view == nil) {
		return;
	}

	bool visible = u_hud_is_visible();
	if (!visible) {
		if (![c->hud_view isHidden]) {
			dispatch_async(dispatch_get_main_queue(), ^{
			    [c->hud_view setHidden:YES];
			});
		}
		return;
	}

	// Throttle updates to every 0.5s
	c->hud_timer += dt;
	if (c->hud_timer < 0.5f) {
		return;
	}
	c->hud_timer = 0.0f;

	// Smooth frame time (exponential moving average)
	float alpha = 0.1f;
	c->smoothed_frame_time_ms = c->smoothed_frame_time_ms * (1.0f - alpha) + (dt * 1000.0f) * alpha;
	float fps = (c->smoothed_frame_time_ms > 0.0f) ? 1000.0f / c->smoothed_frame_time_ms : 0.0f;

	// Device name
	const char *dev_name = (c->xdev != NULL) ? c->xdev->str : "Unknown";

	// Output mode
	int32_t output_mode = 0;
	if (c->xdev != NULL) {
		xrt_device_get_property(c->xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, &output_mode);
	}
	const char *mode_names[] = {"SBS", "Anaglyph", "Blend"};
	const char *mode_name = (output_mode >= 0 && output_mode <= 2) ? mode_names[output_mode] : "?";

	// Display info from system compositor
	float disp_w_mm = 0, disp_h_mm = 0;
	float nom_y = 0, nom_z = 0;
	float ipd_mm = 60.0f;
	if (c->sys_info != NULL) {
		disp_w_mm = c->sys_info->display_width_m * 1000.0f;
		disp_h_mm = c->sys_info->display_height_m * 1000.0f;
		nom_y = c->sys_info->nominal_viewer_y_m * 1000.0f;
		nom_z = c->sys_info->nominal_viewer_z_m * 1000.0f;
	}

	// Eye positions (nominal ± IPD/2)
	float half_ipd = ipd_mm / 2.0f;

	// Qwerty stereo state
	const char *stereo_line1 = "";
	const char *stereo_line2 = "";
	char stereo_buf1[128] = {0};
	char stereo_buf2[128] = {0};
	char pos_buf[128] = {0};
	char fwd_buf[128] = {0};

#ifdef XRT_BUILD_DRIVER_QWERTY
	struct qwerty_stereo_state ss = {0};
	bool have_ss = qwerty_get_stereo_state(
	    c->xsysd->xdevs, c->xsysd->xdev_count, &ss);

	if (have_ss) {
		const char *mode_label = ss.camera_mode ? "Camera [P]" : "Display [P]";
		if (ss.camera_mode) {
			snprintf(stereo_buf1, sizeof(stereo_buf1),
			         "%s  IPD/Prlx:%.3f", mode_label, ss.cam_ipd_factor);
			snprintf(stereo_buf2, sizeof(stereo_buf2),
			         "Conv:%.2f dp  vFOV:%.1f",
			         ss.cam_convergence,
			         atanf(ss.cam_half_tan_vfov) * 2.0f * 57.2958f);
		} else {
			snprintf(stereo_buf1, sizeof(stereo_buf1),
			         "%s  IPD/Prlx:%.3f [Sh+Wh]", mode_label, ss.disp_ipd_factor);
			snprintf(stereo_buf2, sizeof(stereo_buf2),
			         "Conv:%.2f dp [Wh]  vFOV:%.1f  Persp*:%.2f",
			         0.0f, 0.0f, 0.0f);
		}
		stereo_line1 = stereo_buf1;
		stereo_line2 = stereo_buf2;
	}

	// Get virtual display/camera position from qwerty HMD
	{
		struct xrt_device *qwerty_hmd = NULL;
		for (uint32_t i = 0; i < c->xsysd->xdev_count; i++) {
			if (c->xsysd->xdevs[i] != NULL &&
			    strstr(c->xsysd->xdevs[i]->str, "Qwerty HMD") != NULL) {
				qwerty_hmd = c->xsysd->xdevs[i];
				break;
			}
		}
		if (qwerty_hmd != NULL) {
			struct xrt_space_relation rel = {0};
			xrt_device_get_tracked_pose(qwerty_hmd, XRT_INPUT_GENERIC_HEAD_POSE,
			                            0, &rel);
			snprintf(pos_buf, sizeof(pos_buf), "Pos  %.2f, %.2f, %.2f m",
			         rel.pose.position.x, rel.pose.position.y, rel.pose.position.z);
			// Forward direction from orientation
			struct xrt_vec3 forward = {0, 0, -1};
			struct xrt_vec3 fwd_world;
			math_quat_rotate_vec3(&rel.pose.orientation, &forward, &fwd_world);
			snprintf(fwd_buf, sizeof(fwd_buf), "Fwd  %.2f, %.2f, %.2f",
			         fwd_world.x, fwd_world.y, fwd_world.z);
		}
	}
#endif

	// Build HUD text
	NSString *text = [NSString stringWithFormat:
	    @"%s\n"
	    "FPS  %.0f   (%.1f ms)\n"
	    "Render  %u x %u\n"
	    "Window  %u x %u\n"
	    "\n"
	    "Display  %.0f x %.0f mm\n"
	    "L Eye  %.0f, %.0f, %.0f mm\n"
	    "R Eye  %.0f, %.0f, %.0f mm\n"
	    "\n"
	    "%s\n"
	    "%s\n"
	    "%s\n"
	    "%s\n"
	    "\n"
	    "Output: %s  (1/2/3)\n"
	    "TAB=HUD  V=2D/3D  P=Cam/Disp  ESC=Quit",
	    dev_name,
	    fps, c->smoothed_frame_time_ms,
	    c->view_width, c->view_height,
	    c->view_width * 2, c->view_height,
	    disp_w_mm, disp_h_mm,
	    -half_ipd, nom_y, nom_z,
	    half_ipd, nom_y, nom_z,
	    pos_buf, fwd_buf,
	    stereo_line1, stereo_line2,
	    mode_name];

	dispatch_async(dispatch_get_main_queue(), ^{
	    c->hud_view.hudText = text;
	    [c->hud_view setNeedsDisplay:YES];
	    [c->hud_view setHidden:NO];
	});
}

static xrt_result_t
metal_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_metal_compositor *c = metal_comp(xc);

	// Frame timing for HUD
	uint64_t now_ns = os_monotonic_get_ns();
	float dt = (c->last_frame_ns > 0) ? (float)(now_ns - c->last_frame_ns) / 1e9f : 0.016f;
	c->last_frame_ns = now_ns;

	// Update HUD overlay
	if (c->owns_window) {
		metal_compositor_update_hud(c, dt);
	}

	static uint32_t commit_count = 0;
	commit_count++;

	// Get output texture — either from shared IOSurface or CAMetalLayer drawable
	id<MTLTexture> output_texture = nil;
	id<CAMetalDrawable> drawable = nil;

	if (c->shared_texture != nil) {
		output_texture = c->shared_texture;
		if (commit_count <= 3) {
			fprintf(stderr, "METAL #%u: shared_texture=%lux%lu, stereo=%lux%lu, layers=%u\n",
			        commit_count,
			        (unsigned long)output_texture.width, (unsigned long)output_texture.height,
			        (unsigned long)c->stereo_texture.width, (unsigned long)c->stereo_texture.height,
			        c->layer_accum.layer_count);
		}
	} else {
		drawable = [c->metal_layer nextDrawable];
		if (drawable == nil) {
			if (commit_count <= 3) {
				fprintf(stderr, "METAL #%u: nextDrawable=nil, size=%.0fx%.0f, scale=%.1f\n",
				        commit_count, c->metal_layer.drawableSize.width,
				        c->metal_layer.drawableSize.height, c->metal_layer.contentsScale);
			}
			return XRT_SUCCESS; // Non-fatal, skip this frame
		}
		output_texture = drawable.texture;

		if (commit_count <= 3) {
			fprintf(stderr, "METAL #%u: drawable=%lux%lu, stereo=%lux%lu, layers=%u\n",
			        commit_count,
			        (unsigned long)output_texture.width, (unsigned long)output_texture.height,
			        (unsigned long)c->stereo_texture.width, (unsigned long)c->stereo_texture.height,
			        c->layer_accum.layer_count);
		}
	}

	id<MTLCommandBuffer> cmd_buf = [c->command_queue commandBuffer];
	if (cmd_buf == nil) {
		U_LOG_E("Failed to create command buffer");
		return XRT_SUCCESS;
	}

	// Step 1: Render layers into SBS stereo texture
	if (c->stereo_texture != nil && c->layer_accum.layer_count > 0) {
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = c->stereo_texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
		pass.depthAttachment.texture = c->depth_texture;
		pass.depthAttachment.loadAction = MTLLoadActionClear;
		pass.depthAttachment.storeAction = MTLStoreActionDontCare;
		pass.depthAttachment.clearDepth = 1.0;

		id<MTLRenderCommandEncoder> encoder = [cmd_buf renderCommandEncoderWithDescriptor:pass];

		// Render each projection layer
		for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
			struct comp_layer *layer = &c->layer_accum.layers[i];

			if (layer->data.type != XRT_LAYER_PROJECTION &&
			    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
				continue;
			}

			// For each eye (left=0, right=1)
			for (uint32_t eye = 0; eye < 2; eye++) {
				struct xrt_swapchain *sc = layer->sc_array[eye];
				if (sc == NULL) {
					if (commit_count <= 3) fprintf(stderr, "  eye%u: sc=NULL\n", eye);
					continue;
				}

				struct comp_metal_swapchain *msc = metal_swapchain(sc);
				uint32_t img_idx = layer->data.proj.v[eye].sub.image_index;
				if (img_idx >= msc->image_count) {
					if (commit_count <= 3) fprintf(stderr, "  eye%u: img_idx=%u >= count=%u\n", eye, img_idx, msc->image_count);
					continue;
				}

				id<MTLTexture> src_tex = msc->images[img_idx];
				if (src_tex == nil) {
					if (commit_count <= 3) fprintf(stderr, "  eye%u: src_tex=nil\n", eye);
					continue;
				}

				// Use sub-image norm_rect to sample correct region of source texture
				struct xrt_normalized_rect nr = layer->data.proj.v[eye].sub.norm_rect;
				if (nr.w <= 0.0f || nr.h <= 0.0f) {
					nr.x = 0.0f;
					nr.y = 0.0f;
					nr.w = 1.0f;
					nr.h = 1.0f;
				}

				if (commit_count <= 3) {
					fprintf(stderr, "  eye%u: src=%lux%lu fmt=%lu usage=0x%lx storage=%lu img=%u, nr=(%.2f,%.2f,%.2f,%.2f), vp=(%u,%u)\n",
					        eye, (unsigned long)src_tex.width, (unsigned long)src_tex.height,
					        (unsigned long)src_tex.pixelFormat, (unsigned long)src_tex.usage,
					        (unsigned long)src_tex.storageMode,
					        img_idx, nr.x, nr.y, nr.w, nr.h, c->view_width, c->view_height);
				}

				// Set viewport for this eye
				MTLViewport vp;
				vp.originX = eye * c->view_width;
				vp.originY = 0;
				vp.width = c->view_width;
				vp.height = c->view_height;
				vp.znear = 0.0;
				vp.zfar = 1.0;
				[encoder setViewport:vp];

				// Set pipeline and textures
				[encoder setRenderPipelineState:c->projection_pipeline];
				[encoder setDepthStencilState:c->depth_stencil_state];
				[encoder setFragmentTexture:src_tex atIndex:0];
				[encoder setFragmentSamplerState:c->sampler_linear atIndex:0];

				// Projection constants
				struct {
					float viewport[4];
					float src_rect[4];
					float color_scale[4];
					float color_bias[4];
				} constants;

				constants.viewport[0] = 0.0f;
				constants.viewport[1] = 0.0f;
				constants.viewport[2] = 1.0f;
				constants.viewport[3] = 1.0f;

				constants.src_rect[0] = nr.x;
				constants.src_rect[1] = nr.y;
				constants.src_rect[2] = nr.w;
				constants.src_rect[3] = nr.h;

				constants.color_scale[0] = 1.0f;
				constants.color_scale[1] = 1.0f;
				constants.color_scale[2] = 1.0f;
				constants.color_scale[3] = 1.0f;
				constants.color_bias[0] = 0.0f;
				constants.color_bias[1] = 0.0f;
				constants.color_bias[2] = 0.0f;
				constants.color_bias[3] = 0.0f;

				[encoder setVertexBytes:&constants length:sizeof(constants) atIndex:0];
				[encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];

				// Draw fullscreen triangle
				[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			}
		}

		[encoder endEncoding];
	}

	// Step 2: Blit stereo texture to drawable (or process through display processor)
	if (c->display_processor != NULL && c->stereo_texture != nil) {
		xrt_display_processor_metal_process_stereo(
		    c->display_processor,
		    (__bridge void *)cmd_buf,
		    (__bridge void *)c->stereo_texture,
		    c->view_width,
		    c->view_height,
		    (uint32_t)MTLPixelFormatRGBA8Unorm,
		    (__bridge void *)output_texture,
		    (uint32_t)output_texture.width,
		    (uint32_t)output_texture.height);
	} else {
		// No display processor: do SBS/anaglyph/blend conversion here.
		// Query output mode from the HMD device (set by 1/2/3 keys or
		// xrRequestDisplayRenderingModeEXT).
		int32_t output_mode = 0; // SBS default
		if (c->xdev != NULL) {
			xrt_device_get_property(c->xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, &output_mode);
		}

		// Select pipeline based on output mode
		id<MTLRenderPipelineState> pipeline;
		switch (output_mode) {
		case 1:  pipeline = c->anaglyph_pipeline; break;
		case 2:  pipeline = c->blend_pipeline;    break;
		default: pipeline = c->blit_pipeline;     break;
		}

		MTLRenderPassDescriptor *blit_pass = [MTLRenderPassDescriptor renderPassDescriptor];
		blit_pass.colorAttachments[0].texture = output_texture;
		blit_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		blit_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		blit_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

		id<MTLRenderCommandEncoder> blit_encoder = [cmd_buf renderCommandEncoderWithDescriptor:blit_pass];

		if (c->stereo_texture != nil) {
			[blit_encoder setRenderPipelineState:pipeline];
			[blit_encoder setFragmentTexture:c->stereo_texture atIndex:0];
			[blit_encoder setFragmentSamplerState:c->sampler_linear atIndex:0];
			[blit_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		}

		[blit_encoder endEncoding];
	}

	// Present and commit
	if (drawable != nil) {
		[cmd_buf presentDrawable:drawable];
	}
	[cmd_buf commit];

	// For shared texture mode, wait for GPU completion so the IOSurface
	// is fully written before the app reads it.
	if (c->shared_texture != nil) {
		[cmd_buf waitUntilCompleted];
	}

	// Reset layer accumulator
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static void
metal_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_metal_compositor *c = metal_comp(xc);

	U_LOG_I("Destroying Metal compositor");

	// Destroy display processor
	if (c->display_processor != NULL) {
		xrt_display_processor_metal_destroy(&c->display_processor);
	}

	// Release shared texture resources
	c->shared_texture = nil;
	if (c->shared_iosurface != NULL) {
		CFRelease(c->shared_iosurface);
		c->shared_iosurface = NULL;
	}

	// Release Metal resources
	c->stereo_texture = nil;
	c->depth_texture = nil;
	c->projection_pipeline = nil;
	c->blit_pipeline = nil;
	c->anaglyph_pipeline = nil;
	c->blend_pipeline = nil;
	c->sampler_linear = nil;
	c->depth_stencil_state = nil;
	c->metal_layer = nil;

	// Close window if we own it
	if (c->owns_window && c->window != nil) {
		if ([NSThread isMainThread]) {
			[c->window close];
		} else {
			dispatch_sync(dispatch_get_main_queue(), ^{
				[c->window close];
			});
		}
		c->window = nil;
	}

	c->view = nil;
	c->command_queue = nil;
	c->device = nil;

	os_mutex_destroy(&c->mutex);

	free(c);
}

/*
 *
 * Supported formats
 *
 */

static const int64_t metal_supported_formats[] = {
    (int64_t)MTLPixelFormatRGBA8Unorm,
    (int64_t)MTLPixelFormatRGBA8Unorm_sRGB,
    (int64_t)MTLPixelFormatBGRA8Unorm,
    (int64_t)MTLPixelFormatBGRA8Unorm_sRGB,
    (int64_t)MTLPixelFormatRGBA16Float,
    (int64_t)MTLPixelFormatRGB10A2Unorm,
    (int64_t)MTLPixelFormatDepth32Float,
};

#define METAL_NUM_SUPPORTED_FORMATS (sizeof(metal_supported_formats) / sizeof(metal_supported_formats[0]))

/*
 *
 * Public API
 *
 */

xrt_result_t
comp_metal_compositor_create(struct xrt_device *xdev,
                             void *window_handle,
                             void *command_queue_ptr,
                             void *dp_factory_metal,
                             bool offscreen,
                             void *shared_iosurface,
                             struct xrt_compositor_native **out_xc)
{
	struct comp_metal_compositor *c = U_TYPED_CALLOC(struct comp_metal_compositor);
	if (c == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	int ret = os_mutex_init(&c->mutex);
	if (ret != 0) {
		free(c);
		return XRT_ERROR_ALLOCATION;
	}

	c->xdev = xdev;
	c->command_queue = (__bridge id<MTLCommandQueue>)command_queue_ptr;
	c->device = c->command_queue.device;
	c->display_refresh_rate = 60.0f;
	c->offscreen = offscreen;

	// Get recommended rendering dimensions from device
	uint32_t recommended_width = 0;
	uint32_t recommended_height = 0;
	if (xdev != NULL && xdev->hmd != NULL) {
		recommended_width = xdev->hmd->views[0].display.w_pixels;
		recommended_height = xdev->hmd->views[0].display.h_pixels;
	}
	if (recommended_width == 0) recommended_width = 960;
	if (recommended_height == 0) recommended_height = 1080;

	// Window setup
	NSView *external_view = (__bridge NSView *)window_handle;
	if (external_view != nil) {
		if (!setup_external_window(c, external_view)) {
			os_mutex_destroy(&c->mutex);
			free(c);
			return XRT_ERROR_VULKAN;
		}
	} else {
		if (!create_window(c, recommended_width, recommended_height)) {
			os_mutex_destroy(&c->mutex);
			free(c);
			return XRT_ERROR_VULKAN;
		}
	}

	// Set up shared IOSurface texture if provided
	if (shared_iosurface != NULL) {
		IOSurfaceRef surface = (IOSurfaceRef)shared_iosurface;
		CFRetain(surface);
		c->shared_iosurface = surface;

		size_t iosW = IOSurfaceGetWidth(surface);
		size_t iosH = IOSurfaceGetHeight(surface);

		MTLTextureDescriptor *ioDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                                                                                  width:iosW
		                                                                                 height:iosH
		                                                                              mipmapped:NO];
		ioDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		ioDesc.storageMode = MTLStorageModeShared;
		c->shared_texture = [c->device newTextureWithDescriptor:ioDesc
		                                             iosurface:surface
		                                                 plane:0];
		if (c->shared_texture == nil) {
			U_LOG_E("Failed to create MTLTexture from IOSurface (%zux%zu)", iosW, iosH);
			CFRelease(c->shared_iosurface);
			c->shared_iosurface = NULL;
		} else {
			U_LOG_W("Created shared IOSurface texture: %zux%zu", iosW, iosH);
		}
	}

	// The HMD views report logical dimensions (e.g. 1512x823 on Retina).
	// The recommended swapchain height from oxr_system.c uses physical pixels
	// (display_pixel_height = 1646), but xdev->hmd->views[].h_pixels is logical.
	// Scale height to match the physical drawable resolution.
	CGFloat scale = c->metal_layer.contentsScale;
	if (scale > 1.0) {
		recommended_height = (uint32_t)(recommended_height * scale);
	}

	// Compile shaders
	if (!compile_shaders(c)) {
		os_mutex_destroy(&c->mutex);
		free(c);
		return XRT_ERROR_VULKAN;
	}

	// Create SBS stereo texture
	if (!create_stereo_texture(c, recommended_width, recommended_height)) {
		os_mutex_destroy(&c->mutex);
		free(c);
		return XRT_ERROR_VULKAN;
	}

	// Create display processor if factory provided
	if (dp_factory_metal != NULL) {
		xrt_dp_factory_metal_fn_t factory = (xrt_dp_factory_metal_fn_t)dp_factory_metal;
		xrt_result_t xret = factory(
		    (__bridge void *)c->device,
		    (__bridge void *)c->command_queue,
		    window_handle,
		    &c->display_processor);
		if (xret != XRT_SUCCESS) {
			U_LOG_W("Display processor creation failed, continuing without it");
		}
	}

	// Set up xrt_compositor_native vtable
	struct xrt_compositor *xc = &c->base.base;

	xc->get_swapchain_create_properties = metal_compositor_get_swapchain_create_properties;
	xc->create_swapchain = metal_compositor_create_swapchain;
	xc->import_swapchain = metal_compositor_import_swapchain;
	xc->import_fence = metal_compositor_import_fence;
	xc->create_semaphore = metal_compositor_create_semaphore;
	xc->begin_session = metal_compositor_begin_session;
	xc->end_session = metal_compositor_end_session;
	xc->predict_frame = metal_compositor_predict_frame;
	xc->wait_frame = metal_compositor_wait_frame;
	xc->mark_frame = metal_compositor_mark_frame;
	xc->begin_frame = metal_compositor_begin_frame;
	xc->discard_frame = metal_compositor_discard_frame;
	xc->layer_begin = metal_compositor_layer_begin;
	xc->layer_projection = metal_compositor_layer_projection;
	xc->layer_projection_depth = metal_compositor_layer_projection_depth;
	xc->layer_quad = metal_compositor_layer_quad;
	xc->layer_commit = metal_compositor_layer_commit;
	xc->destroy = metal_compositor_destroy;

	// Compositor info
	struct xrt_compositor_info *info = &c->base.base.info;
	info->format_count = METAL_NUM_SUPPORTED_FORMATS;
	for (uint32_t i = 0; i < info->format_count; i++) {
		info->formats[i] = metal_supported_formats[i];
	}

	// Native compositor manages its own visibility/focus
	info->initial_visible = true;
	info->initial_focused = true;

	*out_xc = &c->base;

	U_LOG_W("Metal compositor created: device=%s, stereo=%ux%u",
	        c->device.name.UTF8String, c->view_width * 2, c->view_height);

	return XRT_SUCCESS;
}

bool
comp_metal_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_vec3 *out_left_eye,
                                                  struct xrt_vec3 *out_right_eye)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	if (c->display_processor == NULL) {
		return false;
	}

	struct xrt_eye_pair pair;
	if (!xrt_display_processor_metal_get_predicted_eye_positions(c->display_processor, &pair)) {
		return false;
	}

	out_left_eye->x = pair.left.x;
	out_left_eye->y = pair.left.y;
	out_left_eye->z = pair.left.z;
	out_right_eye->x = pair.right.x;
	out_right_eye->y = pair.right.y;
	out_right_eye->z = pair.right.z;

	return true;
}

bool
comp_metal_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                             float *out_width_m,
                                             float *out_height_m)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	if (c->display_processor == NULL) {
		return false;
	}
	return xrt_display_processor_metal_get_display_dimensions(c->display_processor, out_width_m, out_height_m);
}

bool
comp_metal_compositor_get_window_metrics(struct xrt_compositor *xc,
                                         struct xrt_window_metrics *out_metrics)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	if (c->display_processor == NULL) {
		return false;
	}
	return xrt_display_processor_metal_get_window_metrics(c->display_processor, out_metrics);
}

bool
comp_metal_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	if (c->display_processor == NULL) {
		return false;
	}
	return xrt_display_processor_metal_request_display_mode(c->display_processor, enable_3d);
}

void
comp_metal_compositor_set_system_devices(struct xrt_compositor *xc,
                                         struct xrt_system_devices *xsysd)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	c->xsysd = xsysd;
}

void
comp_metal_compositor_set_sys_info(struct xrt_compositor *xc,
                                    const struct xrt_system_compositor_info *info)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	c->sys_info = info;
}

void *
comp_metal_get_system_default_device(void)
{
	return (__bridge void *)MTLCreateSystemDefaultDevice();
}
