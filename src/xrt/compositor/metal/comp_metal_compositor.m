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

	//! IOSurface backing each swapchain image (for cross-API sharing).
	IOSurfaceRef iosurfaces[METAL_SWAPCHAIN_MAX_IMAGES];

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

#import "util/comp_hud_overlay_macos.h"

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

	//! Sampler state for texture sampling.
	id<MTLSamplerState> sampler_linear;

	//! Depth stencil state.
	id<MTLDepthStencilState> depth_stencil_state;

	//! Atlas stereo texture (tile_columns * view_width × tile_rows * view_height).
	id<MTLTexture> atlas_texture;

	//! Depth texture matching stereo texture.
	id<MTLTexture> depth_texture;

	//! Accumulated layers for the current frame.
	struct comp_layer_accum layer_accum;

	//! Per-eye view width.
	uint32_t view_width;

	//! Per-eye view height.
	uint32_t view_height;

	//! Number of tile columns in the atlas (default 2 for SBS stereo).
	uint32_t tile_columns;

	//! Number of tile rows in the atlas (default 1 for SBS stereo).
	uint32_t tile_rows;

	//! Atlas texture width (worst-case, fixed at init).
	uint32_t atlas_width;

	//! Atlas texture height (worst-case, fixed at init).
	uint32_t atlas_height;

	//! Retina backing scale factor.
	float backing_scale;

	//! Window (either from app or self-created).
	NSWindow *window;

	//! View containing the CAMetalLayer.
	NSView *view;

	//! True if we created the window ourselves.
	bool owns_window;

	//! True if running in offscreen mode (hidden window, no visible UI).
	bool offscreen;

	//! True if swapchain content comes from GL (needs Y-flip on sample).
	bool source_is_gl;

	//! App-provided IOSurface for shared texture output (retained, may be NULL).
	IOSurfaceRef shared_iosurface;

	//! Metal texture wrapping the shared IOSurface (render target).
	id<MTLTexture> shared_texture;

	//! Generic Metal display processor.
	struct xrt_display_processor_metal *display_processor;

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

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
    "struct ProjectionConstants {\n"
    "    float4 viewport;  // x, y, width, height (output, in normalized coords)\n"
    "    float4 src_rect;  // x, y, width, height (input UV sub-region)\n"
    "    float4 color_scale;\n"
    "    float4 color_bias;\n"
    "    float  swizzle_rb; // 1.0 to swap R and B channels (GL BGRA IOSurface)\n"
    "    float  _pad[3];\n"
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
    "    if (pc.swizzle_rb > 0.5) color = float4(color.b, color.g, color.r, color.a);\n"
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
	// Handle both Vulkan format values and Metal format values.
	// The enum ranges don't overlap for the formats we support.
	switch (format) {
	// Vulkan format → Metal format
	case 37:  return MTLPixelFormatRGBA8Unorm;       // VK_FORMAT_R8G8B8A8_UNORM
	case 43:  return MTLPixelFormatRGBA8Unorm_sRGB;  // VK_FORMAT_R8G8B8A8_SRGB
	case 44:  return MTLPixelFormatBGRA8Unorm;        // VK_FORMAT_B8G8R8A8_UNORM
	case 50:  return MTLPixelFormatBGRA8Unorm_sRGB;   // VK_FORMAT_B8G8R8A8_SRGB
	case 64:  return MTLPixelFormatRGB10A2Unorm;      // VK_FORMAT_A2B10G10R10_UNORM_PACK32
	case 97:  return MTLPixelFormatRGBA16Float;       // VK_FORMAT_R16G16B16A16_SFLOAT
	case 126: return MTLPixelFormatDepth32Float;      // VK_FORMAT_D32_SFLOAT
	// Metal format pass-through (already correct)
	case 70:  return MTLPixelFormatRGBA8Unorm;
	case 71:  return MTLPixelFormatRGBA8Unorm_sRGB;
	case 80:  return MTLPixelFormatBGRA8Unorm;
	case 81:  return MTLPixelFormatBGRA8Unorm_sRGB;
	case 90:  return MTLPixelFormatRGB10A2Unorm;
	case 115: return MTLPixelFormatRGBA16Float;
	case 252: return MTLPixelFormatDepth32Float;
	default:  return MTLPixelFormatRGBA8Unorm;
	}
}

static uint32_t
metal_format_bytes_per_pixel(MTLPixelFormat format)
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatRGBA8Unorm_sRGB:
	case MTLPixelFormatBGRA8Unorm:
	case MTLPixelFormatBGRA8Unorm_sRGB:
	case MTLPixelFormatRGB10A2Unorm:
		return 4;
	case MTLPixelFormatRGBA16Float:
		return 8;
	default:
		return 4;
	}
}

/*
 *
 * Shader compilation
 *
 */

static uint32_t
metal_format_to_iosurface_fourcc(MTLPixelFormat format)
{
	switch (format) {
	case MTLPixelFormatBGRA8Unorm:
	case MTLPixelFormatBGRA8Unorm_sRGB:
		return 'BGRA';
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatRGBA8Unorm_sRGB:
		return 'RGBA';
	default:
		return 'BGRA';
	}
}

static MTLPixelFormat
iosurface_fourcc_to_metal_format(uint32_t fourcc)
{
	switch (fourcc) {
	case 'BGRA': return MTLPixelFormatBGRA8Unorm;
	case 'RGBA': return MTLPixelFormatRGBA8Unorm;
	default:     return MTLPixelFormatBGRA8Unorm;
	}
}

static bool
compile_shaders(struct comp_metal_compositor *c)
{
	if (c->device == nil) {
		U_LOG_E("Metal device is nil — cannot compile shaders");
		return false;
	}

	U_LOG_I("Compiling Metal shaders on device: %s",
	        c->device.name.UTF8String);

	// This file is compiled WITHOUT ARC (see CMakeLists.txt).
	// All objects created with new/alloc must be explicitly released.

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
	id<MTLFunction> proj_vs = [library newFunctionWithName:@"projection_vertex"];
	id<MTLFunction> proj_fs = [library newFunctionWithName:@"projection_fragment"];

	// Blit pipeline
	MTLRenderPipelineDescriptor *blit_desc = [[MTLRenderPipelineDescriptor alloc] init];
	blit_desc.vertexFunction = blit_vs;
	blit_desc.fragmentFunction = blit_fs;
	blit_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

	c->blit_pipeline = [c->device newRenderPipelineStateWithDescriptor:blit_desc error:&error];
	[blit_desc release];
	if (c->blit_pipeline == nil) {
		U_LOG_E("Failed to create blit pipeline: %s",
		        error.localizedDescription.UTF8String);
		goto cleanup;
	}

	// Projection pipeline (renders into stereo texture)
	{
		MTLRenderPipelineDescriptor *proj_desc = [[MTLRenderPipelineDescriptor alloc] init];
		proj_desc.vertexFunction = proj_vs;
		proj_desc.fragmentFunction = proj_fs;
		proj_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		proj_desc.colorAttachments[0].blendingEnabled = YES;
		proj_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		proj_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		proj_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		proj_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		proj_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

		c->projection_pipeline = [c->device newRenderPipelineStateWithDescriptor:proj_desc error:&error];
		[proj_desc release];
		if (c->projection_pipeline == nil) {
			U_LOG_E("Failed to create projection pipeline: %s",
			        error.localizedDescription.UTF8String);
			goto cleanup;
		}
	}

	// Sampler
	{
		MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
		sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
		sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
		sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
		sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
		c->sampler_linear = [c->device newSamplerStateWithDescriptor:sampler_desc];
		[sampler_desc release];
	}

	// Depth stencil state
	{
		MTLDepthStencilDescriptor *ds_desc = [[MTLDepthStencilDescriptor alloc] init];
		ds_desc.depthCompareFunction = MTLCompareFunctionLessEqual;
		ds_desc.depthWriteEnabled = YES;
		c->depth_stencil_state = [c->device newDepthStencilStateWithDescriptor:ds_desc];
		[ds_desc release];
	}

	// Release temporary objects (MRR — all new/alloc must be balanced)
	[proj_fs release];
	[proj_vs release];
	[blit_fs release];
	[blit_vs release];
	[library release];

	return true;

cleanup:
	[proj_fs release];
	[proj_vs release];
	[blit_fs release];
	[blit_vs release];
	[library release];
	return false;
}

/*
 *
 * Stereo texture management
 *
 */

static bool
create_atlas_texture(struct comp_metal_compositor *c,
                       uint32_t atlas_width,
                       uint32_t atlas_height,
                       uint32_t view_width,
                       uint32_t view_height)
{
	// Atlas texture at worst-case size (fixed, never resized)
	MTLTextureDescriptor *desc = [MTLTextureDescriptor
	    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                width:atlas_width
	                               height:atlas_height
	                            mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;

	c->atlas_texture = [c->device newTextureWithDescriptor:desc];
	if (c->atlas_texture == nil) {
		U_LOG_E("Failed to create stereo texture");
		return false;
	}

	// Depth texture
	MTLTextureDescriptor *depth_desc = [MTLTextureDescriptor
	    texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
	                                width:atlas_width
	                               height:atlas_height
	                            mipmapped:NO];
	depth_desc.usage = MTLTextureUsageRenderTarget;
	depth_desc.storageMode = MTLStorageModePrivate;

	c->depth_texture = [c->device newTextureWithDescriptor:depth_desc];
	if (c->depth_texture == nil) {
		U_LOG_E("Failed to create depth texture");
		return false;
	}

	c->atlas_width = atlas_width;
	c->atlas_height = atlas_height;
	c->view_width = view_width;
	c->view_height = view_height;

	U_LOG_I("Created atlas texture: %ux%u (per-view: %ux%u, tiles: %ux%u)",
	        atlas_width, atlas_height, view_width, view_height,
	        c->tile_columns, c->tile_rows);

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

	[c->window setTitle:@"DisplayXR — Metal Native Compositor"];

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
		// View doesn't have a CAMetalLayer (e.g. NSOpenGLView for GL apps).
		// Add a CAMetalLayer as a sublayer on top so both GL context and
		// Metal presentation can coexist.
		void (^setup_layer)(void) = ^{
			external_view.wantsLayer = YES;
			CAMetalLayer *layer = [CAMetalLayer layer];
			layer.device = c->device;
			layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
			layer.frame = external_view.bounds;
			layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
			[external_view.layer addSublayer:layer];
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
		if (msc->iosurfaces[i] != NULL) {
			CFRelease(msc->iosurfaces[i]);
			msc->iosurfaces[i] = NULL;
		}
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
	uint32_t bpp = metal_format_bytes_per_pixel(format);

	for (uint32_t i = 0; i < msc->image_count; i++) {
		// Create IOSurface backing for cross-API sharing (Vulkan import)
		NSDictionary *props = @{
			(id)kIOSurfaceWidth: @(info->width),
			(id)kIOSurfaceHeight: @(info->height),
			(id)kIOSurfaceBytesPerElement: @(bpp),
			(id)kIOSurfacePixelFormat: @(metal_format_to_iosurface_fourcc(format)),
		};
		IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
		if (surface == NULL) {
			U_LOG_E("Failed to create IOSurface for swapchain image %u", i);
			metal_swapchain_destroy(&msc->base.base);
			return XRT_ERROR_ALLOCATION;
		}
		msc->iosurfaces[i] = surface;

		// Create MTLTexture from IOSurface (shared storage required)
		MTLTextureDescriptor *desc = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:format
		                                width:info->width
		                               height:info->height
		                            mipmapped:(info->mip_count > 1) ? YES : NO];
		desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		if (info->array_size > 1) {
			desc.textureType = MTLTextureType2DArray;
			desc.arrayLength = info->array_size;
		}

		msc->images[i] = [c->device newTextureWithDescriptor:desc
		                                           iosurface:surface
		                                               plane:0];
		if (msc->images[i] == nil) {
			U_LOG_E("Failed to create swapchain image %u from IOSurface", i);
			metal_swapchain_destroy(&msc->base.base);
			return XRT_ERROR_ALLOCATION;
		}

		// Store IOSurfaceRef as native handle (for Vulkan import via MoltenVK)
		CFRetain(surface);
		msc->base.images[i].handle = (xrt_graphics_buffer_handle_t)surface;
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

	// Active rendering mode name (from active_rendering_mode_index, not OUTPUT_MODE property)
	const char *mode_name = "?";
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			mode_name = c->xdev->rendering_modes[idx].mode_name;
		}
	}

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
	    "Mode: %s (%s)  (0-3  V=Cycle)\n"
	    "TAB=HUD  P=Cam/Disp  ESC=Quit",
	    dev_name,
	    fps, c->smoothed_frame_time_ms,
	    c->view_width, c->view_height,
	    c->tile_columns * c->view_width, c->tile_rows * c->view_height,
	    disp_w_mm, disp_h_mm,
	    -half_ipd, nom_y, nom_z,
	    half_ipd, nom_y, nom_z,
	    pos_buf, fwd_buf,
	    stereo_line1, stereo_line2,
	    mode_name, c->hardware_display_3d ? "3D" : "2D"];

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

	// Get output texture — either from shared IOSurface or CAMetalLayer drawable
	id<MTLTexture> output_texture = nil;
	id<CAMetalDrawable> drawable = nil;

	if (c->shared_texture != nil) {
		output_texture = c->shared_texture;
	} else {
		drawable = [c->metal_layer nextDrawable];
		if (drawable == nil) {
			return XRT_SUCCESS; // Non-fatal, skip this frame
		}
		output_texture = drawable.texture;
	}

	id<MTLCommandBuffer> cmd_buf = [c->command_queue commandBuffer];
	if (cmd_buf == nil) {
		U_LOG_E("Failed to create command buffer");
		return XRT_SUCCESS;
	}

	// Sync hardware_display_3d, tile layout, and per-view dimensions
	// from device's active rendering mode (for _rt apps where qwerty
	// driver updates the device directly without going through
	// request_display_mode)
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			c->hardware_display_3d = mode->hardware_display_3d;
			if (mode->tile_columns > 0) {
				c->tile_columns = mode->tile_columns;
				c->tile_rows = mode->tile_rows;
			}
			if (mode->view_width_pixels > 0) {
				c->view_width = (uint32_t)(mode->view_width_pixels * c->backing_scale);
				c->view_height = (uint32_t)(mode->view_height_pixels * c->backing_scale);
			}
		}
	}

	// Step 1: Render layers into SBS stereo texture
	if (c->atlas_texture != nil && c->layer_accum.layer_count > 0) {
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = c->atlas_texture;
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
					continue;
				}

				struct comp_metal_swapchain *msc = metal_swapchain(sc);
				uint32_t img_idx = layer->data.proj.v[eye].sub.image_index;
				if (img_idx >= msc->image_count) {
					continue;
				}

				id<MTLTexture> src_tex = msc->images[img_idx];
				if (src_tex == nil) {
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

				// In 2D mode, only render eye 0 at full atlas (skip eye 1)
				if (!c->hardware_display_3d && eye > 0) {
					continue;
				}

				// Set viewport for this eye
				MTLViewport vp;
				if (!c->hardware_display_3d) {
					vp.originX = 0;
					vp.originY = 0;
					vp.width = c->tile_columns * c->view_width;
					vp.height = c->tile_rows * c->view_height;
				} else {
					// Tiled layout: place each eye in its tile
					uint32_t tile_x = eye % c->tile_columns;
					uint32_t tile_y = eye / c->tile_columns;
					vp.originX = tile_x * c->view_width;
					vp.originY = tile_y * c->view_height;
					vp.width = c->view_width;
					vp.height = c->view_height;
				}
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
					float swizzle_rb;
					float _pad[3];
				} constants;

				constants.viewport[0] = 0.0f;
				constants.viewport[1] = 0.0f;
				constants.viewport[2] = 1.0f;
				constants.viewport[3] = 1.0f;

				constants.src_rect[0] = nr.x;
				if (c->source_is_gl) {
					// GL renders bottom-up, IOSurface/Metal is top-down.
					// Flip Y: start at bottom of source rect, sample upward.
					constants.src_rect[1] = nr.y + nr.h;
					constants.src_rect[3] = -nr.h;
				} else {
					constants.src_rect[1] = nr.y;
					constants.src_rect[3] = nr.h;
				}
				constants.src_rect[2] = nr.w;

				constants.color_scale[0] = 1.0f;
				constants.color_scale[1] = 1.0f;
				constants.color_scale[2] = 1.0f;
				constants.color_scale[3] = 1.0f;
				constants.color_bias[0] = 0.0f;
				constants.color_bias[1] = 0.0f;
				constants.color_bias[2] = 0.0f;
				constants.color_bias[3] = 0.0f;
				constants.swizzle_rb = c->source_is_gl ? 1.0f : 0.0f;
				constants._pad[0] = constants._pad[1] = constants._pad[2] = 0.0f;

				[encoder setVertexBytes:&constants length:sizeof(constants) atIndex:0];
				[encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];

				// Draw fullscreen triangle
				[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			}
		}

		[encoder endEncoding];
	}

	// Step 2: Blit stereo texture to drawable (or process through display processor)
	if (c->hardware_display_3d && c->display_processor != NULL && c->atlas_texture != nil) {
		xrt_display_processor_metal_process_atlas(
		    c->display_processor,
		    (__bridge void *)cmd_buf,
		    (__bridge void *)c->atlas_texture,
		    c->view_width,
		    c->view_height,
		    c->tile_columns,
		    c->tile_rows,
		    (uint32_t)MTLPixelFormatBGRA8Unorm,
		    (__bridge void *)output_texture,
		    (uint32_t)output_texture.width,
		    (uint32_t)output_texture.height);
	} else {
		// No display processor or 2D mode: simple blit passthrough.
		MTLRenderPassDescriptor *blit_pass = [MTLRenderPassDescriptor renderPassDescriptor];
		blit_pass.colorAttachments[0].texture = output_texture;
		blit_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		blit_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		blit_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

		id<MTLRenderCommandEncoder> blit_encoder = [cmd_buf renderCommandEncoderWithDescriptor:blit_pass];

		if (c->atlas_texture != nil) {
			[blit_encoder setRenderPipelineState:c->blit_pipeline];
			[blit_encoder setFragmentTexture:c->atlas_texture atIndex:0];
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

	// Wrap teardown in @autoreleasepool so autoreleased Metal objects
	// (drawables, command buffers, textures) are drained immediately
	// while backing resources still exist — prevents crash when the
	// run loop's pool drains after the compositor is already freed.
	@autoreleasepool {

	// 1. GPU drain — wait for all in-flight command buffers to finish
	//    before releasing any resources they may reference.
	if (c->command_queue != nil) {
		id<MTLCommandBuffer> fence = [c->command_queue commandBuffer];
		[fence commit];
		[fence waitUntilCompleted];
	}

	// 2. Nil metal_layer early — prevents AppKit callbacks from
	//    acquiring new drawables during teardown.
	c->metal_layer = nil;

	// 3. Destroy display processor
	if (c->display_processor != NULL) {
		xrt_display_processor_metal_destroy(&c->display_processor);
	}

	// 4. Release shared texture resources (MRR — explicit release)
	[c->shared_texture release];
	c->shared_texture = nil;
	if (c->shared_iosurface != NULL) {
		CFRelease(c->shared_iosurface);
		c->shared_iosurface = NULL;
	}

	// 5. Release Metal resources (MRR — explicit release)
	[c->atlas_texture release];
	c->atlas_texture = nil;
	[c->depth_texture release];
	c->depth_texture = nil;
	[c->projection_pipeline release];
	c->projection_pipeline = nil;
	[c->blit_pipeline release];
	c->blit_pipeline = nil;
	[c->sampler_linear release];
	c->sampler_linear = nil;
	[c->depth_stencil_state release];
	c->depth_stencil_state = nil;

	// 6. Close window synchronously inside @autoreleasepool so the
	//    NSWindow dealloc (and its frame view cascade) happens NOW,
	//    while all backing resources are still valid.
	if (c->owns_window && c->window != nil) {
		// Detach custom views from hierarchy first — prevents
		// _recursiveBreakKeyViewLoop from walking freed subviews.
		if (c->hud_view != nil) {
			[c->hud_view removeFromSuperview];
			c->hud_view = nil;
		}
		if (c->view != nil) {
			[c->window setContentView:[[NSView alloc] init]];
		}
		c->view = nil;

		// Close must happen on the main thread. Use dispatch_sync
		// (not async) so dealloc occurs inside this @autoreleasepool.
		NSWindow *window = c->window;
		c->window = nil;
		void (^closeBlock)(void) = ^{
			[window orderOut:nil];
			[window close];
		};
		if ([NSThread isMainThread]) {
			closeBlock();
		} else {
			dispatch_sync(dispatch_get_main_queue(), closeBlock);
		}
	} else {
		// Not our window — just detach views.
		if (c->hud_view != nil) {
			[c->hud_view removeFromSuperview];
			c->hud_view = nil;
		}
		c->view = nil;

		// Drain any pending dispatch_async blocks (HUD hide/show)
		// that captured the raw 'c' pointer — they must complete
		// before we free the struct.
		if (![NSThread isMainThread]) {
			dispatch_sync(dispatch_get_main_queue(), ^{});
		}
	}

	// 8. Release remaining objects (MRR — explicit release required)
	[c->command_queue release];
	c->command_queue = nil;
	[c->device release];
	c->device = nil;

	} // @autoreleasepool — all autoreleased ObjC objects drained here

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

void *
comp_metal_swapchain_get_texture(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_metal_swapchain *msc = metal_swapchain(xsc);
	if (index >= msc->image_count) {
		return NULL;
	}
	return (__bridge void *)msc->images[index];
}

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
	// Set up Metal device and command queue.
	// This file is compiled WITHOUT ARC (see CMakeLists.txt), so ObjC
	// object lifetimes must be managed explicitly with retain/release.
	if (command_queue_ptr != NULL) {
		// Use app-provided command queue (Metal native apps)
		c->command_queue = (__bridge id<MTLCommandQueue>)command_queue_ptr;
		[c->command_queue retain];
		c->device = c->command_queue.device;
		[c->device retain];
	} else {
		// Create own Metal device + queue (Vulkan apps routed through Metal for presentation)
		c->device = MTLCreateSystemDefaultDevice();
		if (c->device == nil) {
			U_LOG_E("Failed to create Metal device");
			os_mutex_destroy(&c->mutex);
			free(c);
			return XRT_ERROR_VULKAN;
		}
		c->command_queue = [c->device newCommandQueue];
		if (c->command_queue == nil) {
			U_LOG_E("Failed to create Metal command queue");
			[c->device release];
			os_mutex_destroy(&c->mutex);
			free(c);
			return XRT_ERROR_VULKAN;
		}
		U_LOG_I("Created internal Metal device + command queue for Vulkan presentation");
	}
	c->display_refresh_rate = 60.0f;
	c->offscreen = offscreen;

	// Get display dimensions from device.
	// screens[0] holds the logical (point) size — used for NSWindow creation.
	// The stereo texture must be at Retina (physical pixel) resolution so
	// the app's retina-resolution swapchain isn't downscaled then re-upscaled.
	uint32_t display_width = 0;
	uint32_t display_height = 0;
	if (xdev != NULL && xdev->hmd != NULL &&
	    xdev->hmd->screens[0].w_pixels > 0) {
		display_width = xdev->hmd->screens[0].w_pixels;
		display_height = xdev->hmd->screens[0].h_pixels;
	}
	if (display_width == 0) display_width = 1920;
	if (display_height == 0) display_height = 1080;

	// Scale to Retina physical pixels
	CGFloat backing_scale = [NSScreen mainScreen].backingScaleFactor;
	c->backing_scale = (float)backing_scale;
	uint32_t pixel_width = (uint32_t)(display_width * backing_scale);
	uint32_t pixel_height = (uint32_t)(display_height * backing_scale);

	// Window / headless setup
	NSView *external_view = (__bridge NSView *)window_handle;
	if (external_view != nil) {
		if (!setup_external_window(c, external_view)) {
			os_mutex_destroy(&c->mutex);
			free(c);
			return XRT_ERROR_VULKAN;
		}
	} else if (shared_iosurface == NULL) {
		// Only create a window when there's no shared IOSurface.
		// With IOSurface, we render headless — no window needed.
		if (!create_window(c, display_width, display_height)) {
			os_mutex_destroy(&c->mutex);
			free(c);
			return XRT_ERROR_VULKAN;
		}
	} else {
		// Headless mode: shared IOSurface without a window.
		// No NSWindow, no NSView, no CAMetalLayer — output goes
		// directly to the IOSurface-backed MTLTexture.
		c->window = nil;
		c->view = nil;
		c->metal_layer = nil;
		c->owns_window = false;
		U_LOG_I("Headless mode — no window (IOSurface shared texture)");
	}

	// Set up shared IOSurface texture if provided
	if (shared_iosurface != NULL) {
		IOSurfaceRef surface = (IOSurfaceRef)shared_iosurface;
		CFRetain(surface);
		c->shared_iosurface = surface;

		size_t iosW = IOSurfaceGetWidth(surface);
		size_t iosH = IOSurfaceGetHeight(surface);

		uint32_t iosFourCC = IOSurfaceGetPixelFormat(surface);
		MTLPixelFormat iosFormat = iosurface_fourcc_to_metal_format(iosFourCC);

		MTLTextureDescriptor *ioDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:iosFormat
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

	// Initialize tile layout and per-view dimensions from active rendering mode
	uint32_t init_view_w = pixel_width / 2;  // fallback: half display
	uint32_t init_view_h = pixel_height;
	c->tile_columns = 2;
	c->tile_rows = 1;
	if (xdev != NULL && xdev->hmd != NULL) {
		uint32_t idx = xdev->hmd->active_rendering_mode_index;
		if (idx < xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &xdev->rendering_modes[idx];
			if (mode->tile_columns > 0) {
				c->tile_columns = mode->tile_columns;
				c->tile_rows = mode->tile_rows;
			}
			if (mode->view_width_pixels > 0) {
				init_view_w = (uint32_t)(mode->view_width_pixels * backing_scale);
				init_view_h = (uint32_t)(mode->view_height_pixels * backing_scale);
			}
		}
	}

	// Worst-case atlas = display resolution (Retina-scaled).
	// All modes tile within this space — never reallocated.
	uint32_t atlas_w = pixel_width;
	uint32_t atlas_h = pixel_height;

	// Compile shaders
	if (!compile_shaders(c)) {
		os_mutex_destroy(&c->mutex);
		free(c);
		return XRT_ERROR_VULKAN;
	}

	// Create atlas stereo texture at worst-case Retina resolution
	if (!create_atlas_texture(c, atlas_w, atlas_h, init_view_w, init_view_h)) {
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
	c->hardware_display_3d = true; // Start in 3D mode (session begin will confirm)

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

	U_LOG_W("Metal compositor created: device=%s, atlas=%ux%u (tiles %ux%u)",
	        c->device.name.UTF8String,
	        c->tile_columns * c->view_width, c->tile_rows * c->view_height,
	        c->tile_columns, c->tile_rows);

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
	c->hardware_display_3d = enable_3d;
	U_LOG_W("Metal compositor: hardware_display_3d = %s", enable_3d ? "true" : "false");
	if (c->display_processor == NULL) {
		return false;
	}
	// Delegate to display processor (may be a no-op for sim_display)
	xrt_display_processor_metal_request_display_mode(c->display_processor, enable_3d);
	return true;
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

void
comp_metal_compositor_set_source_gl(struct xrt_compositor *xc)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	c->source_is_gl = true;
}

void *
comp_metal_get_system_default_device(void)
{
	return (__bridge void *)MTLCreateSystemDefaultDevice();
}
