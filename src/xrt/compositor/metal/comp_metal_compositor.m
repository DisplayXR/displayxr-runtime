// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Metal compositor implementation.
 *
 * Mirrors the D3D11 native compositor but uses Metal instead:
 * - Creates Metal textures as swapchain images
 * - Renders layers into atlas texture using Metal shaders
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
#include "util/u_tiling.h"
#include "util/u_canvas.h"
#include "util/u_capture_intent.h"
#include <displayxr_mcp/mcp_capture.h>

// STB_IMAGE_WRITE_STATIC scopes all stbi_write_* to this TU so linking
// alongside other compositors that also implement stb doesn't produce
// duplicate symbols. STB_IMAGE_WRITE_IMPLEMENTATION still forces the
// single-header definitions to emit here.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

	//! Render pipeline for atlas layer compositing.
	id<MTLRenderPipelineState> projection_pipeline;

	//! Render pipeline for fullscreen blit (atlas→target passthrough).
	id<MTLRenderPipelineState> blit_pipeline;

	//! Sampler state for texture sampling.
	id<MTLSamplerState> sampler_linear;

	//! Depth stencil state.
	id<MTLDepthStencilState> depth_stencil_state;

	//! Atlas texture (tile_columns * view_width × tile_rows * view_height).
	id<MTLTexture> atlas_texture;

	//! Depth texture matching atlas texture.
	id<MTLTexture> depth_texture;

	//! Accumulated layers for the current frame.
	struct comp_layer_accum layer_accum;

	//! Per-eye view width.
	uint32_t view_width;

	//! Per-eye view height.
	uint32_t view_height;

	//! Number of tile columns in the atlas (default 2 for stereo).
	uint32_t tile_columns;

	//! Number of tile rows in the atlas (default 1 for stereo).
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

	//! Intermediate texture at content dims for DP input (crop from atlas).
	id<MTLTexture> dp_input_texture;
	uint32_t dp_input_width;   //!< Current dp_input_texture width (0 = not allocated)
	uint32_t dp_input_height;  //!< Current dp_input_texture height

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

	//! Last 3D mode index (for V-key toggle restore).
	uint32_t last_3d_mode_index;

	//! True if app is legacy (no XR_EXT_display_info) — gates 1/2/3 key mode selection.
	bool legacy_app_tile_scaling;

	//! System devices (for qwerty driver keyboard input).
	struct xrt_system_devices *xsysd;

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;

	//! HUD overlay (shared u_hud system).
	struct u_hud *hud;

	//! HUD texture for GPU upload.
	id<MTLTexture> hud_texture;

	//! Render pipeline for HUD blit with alpha blending.
	id<MTLRenderPipelineState> hud_blit_pipeline;

	//! Last frame timestamp for FPS/frame time.
	uint64_t last_frame_ns;

	//! Smoothed frame time in ms.
	float smoothed_frame_time_ms;

	//! System compositor info (for display dimensions, nominal viewer).
	const struct xrt_system_compositor_info *sys_info;

	//! Canvas output rect for shared-texture apps.
	struct u_canvas_rect canvas;

	//! Thread safety.
	struct os_mutex mutex;

	//! MCP capture request box (polled at top of layer_commit).
	struct mcp_capture_request mcp_capture;

	//! Per-frame capture intent. See u_capture_intent.h.
	struct u_capture_intent capture_intent;
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

	// HUD blit pipeline (same shaders, alpha blending enabled)
	{
		MTLRenderPipelineDescriptor *hud_desc = [[MTLRenderPipelineDescriptor alloc] init];
		hud_desc.vertexFunction = blit_vs;
		hud_desc.fragmentFunction = blit_fs;
		hud_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		hud_desc.colorAttachments[0].blendingEnabled = YES;
		hud_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		hud_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		hud_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		hud_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

		c->hud_blit_pipeline = [c->device newRenderPipelineStateWithDescriptor:hud_desc error:&error];
		[hud_desc release];
		if (c->hud_blit_pipeline == nil) {
			U_LOG_E("Failed to create HUD blit pipeline: %s",
			        error.localizedDescription.UTF8String);
			goto cleanup;
		}
	}

	// Projection pipeline (renders into atlas texture)
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
		U_LOG_E("Failed to create atlas texture");
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

	(void)0; // HUD is created later in comp_metal_compositor_create

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

	// Set initial drawableSize from the view's backing dimensions
	// so the first frame renders at the correct resolution.
	NSRect backing = [external_view convertRectToBacking:external_view.bounds];
	c->metal_layer.drawableSize = CGSizeMake(backing.size.width, backing.size.height);

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
 * Window-space layer (XR_EXT_win32_window_binding). Positioned in fractional
 * window coordinates with per-eye horizontal disparity shift. Mirrors
 * d3d11_compositor_layer_window_space — here we just accumulate; rendering
 * happens later in the per-tile pass (search for `XRT_LAYER_WINDOW_SPACE`
 * in this file).
 */
static xrt_result_t
metal_compositor_layer_window_space(struct xrt_compositor *xc,
                                     struct xrt_device *xdev,
                                     struct xrt_swapchain *xsc,
                                     const struct xrt_layer_data *data)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	comp_layer_accum_window_space(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * Update the HUD overlay text (throttled, main-thread safe).
 */
static void
metal_compositor_render_hud(struct comp_metal_compositor *c, float dt,
                            id<MTLCommandBuffer> cmd_buf, id<MTLTexture> output_texture)
{
	if (c->hud == NULL || !u_hud_is_visible() || output_texture == nil) {
		return;
	}

	// Smooth frame time (every frame for accuracy)
	float dt_ms = dt * 1000.0f;
	if (dt_ms > 0.0f) {
		c->smoothed_frame_time_ms = c->smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	float fps = (c->smoothed_frame_time_ms > 0.0f) ? (1000.0f / c->smoothed_frame_time_ms) : 0.0f;

	// Display dimensions from sys_info
	float disp_w_mm = 0, disp_h_mm = 0;
	float nom_x = 0, nom_y = 0, nom_z = 600.0f;
	if (c->sys_info != NULL) {
		disp_w_mm = c->sys_info->display_width_m * 1000.0f;
		disp_h_mm = c->sys_info->display_height_m * 1000.0f;
		nom_y = c->sys_info->nominal_viewer_y_m * 1000.0f;
		nom_z = c->sys_info->nominal_viewer_z_m * 1000.0f;
	}

	// Eye positions from display processor (fallback to nominal stereo)
	struct xrt_eye_positions eye_pos = {0};
	bool have_eyes = false;
	if (c->display_processor != NULL) {
		have_eyes = xrt_display_processor_metal_get_predicted_eye_positions(
		    c->display_processor, &eye_pos) && eye_pos.valid;
	}
	if (!have_eyes) {
		eye_pos.count = 2;
		eye_pos.eyes[0] = (struct xrt_eye_position){-0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
		eye_pos.eyes[1] = (struct xrt_eye_position){ 0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
	}

	// Window dimensions (from actual window backing, not drawable)
	uint32_t win_w = (uint32_t)output_texture.width;
	uint32_t win_h = (uint32_t)output_texture.height;
	if (c->window != nil) {
		NSRect backing = [c->window.contentView convertRectToBacking:c->window.contentView.bounds];
		win_w = (uint32_t)backing.size.width;
		win_h = (uint32_t)backing.size.height;
	}

	// Fill HUD data
	struct u_hud_data data = {0};
	data.device_name = (c->xdev != NULL) ? c->xdev->str : "Unknown";
	data.fps = fps;
	data.frame_time_ms = c->smoothed_frame_time_ms;
	data.mode_3d = c->hardware_display_3d;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			data.rendering_mode_name = c->xdev->rendering_modes[idx].mode_name;
		}
	}
	data.render_width = c->view_width;
	data.render_height = c->view_height;
	if (c->xdev != NULL && c->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(c->xdev->rendering_modes, c->xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = win_w;
	data.window_height = win_h;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	data.eye_count = eye_pos.count;
	for (uint32_t e = 0; e < eye_pos.count && e < 8; e++) {
		data.eyes[e].x = eye_pos.eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos.eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos.eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos.is_tracking;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(c->xsysd->xdevs, c->xsysd->xdev_count, &qwerty_pose)) {
			data.vdisp_x = qwerty_pose.position.x;
			data.vdisp_y = qwerty_pose.position.y;
			data.vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			data.forward_x = fwd_out.x;
			data.forward_y = fwd_out.y;
			data.forward_z = fwd_out.z;
		}

		struct qwerty_view_state ss;
		if (qwerty_get_view_state(c->xsysd->xdevs, c->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.cam_spread_factor = ss.cam_spread_factor;
			data.cam_parallax_factor = ss.cam_parallax_factor;
			data.cam_convergence = ss.cam_convergence;
			data.cam_half_tan_vfov = ss.cam_half_tan_vfov;
			data.disp_spread_factor = ss.disp_spread_factor;
			data.disp_parallax_factor = ss.disp_parallax_factor;
			data.disp_vHeight = ss.disp_vHeight;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(c->hud, &data);

	// Lazy-create Metal texture
	if (c->hud_texture == nil) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);
		MTLTextureDescriptor *desc = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		                                width:hud_w
		                               height:hud_h
		                            mipmapped:NO];
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		c->hud_texture = [c->device newTextureWithDescriptor:desc];
		dirty = true;
	}

	// Upload pixels if changed
	if (dirty && c->hud_texture != nil) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);
		MTLRegion region = MTLRegionMake2D(0, 0, hud_w, hud_h);
		[c->hud_texture replaceRegion:region
		                  mipmapLevel:0
		                    withBytes:u_hud_get_pixels(c->hud)
		                  bytesPerRow:hud_w * 4];
	}

	// Blit HUD to bottom-left of output with alpha blending.
	// Scale down if HUD would exceed 50% of output width.
	uint32_t hud_w = u_hud_get_width(c->hud);
	uint32_t hud_h = u_hud_get_height(c->hud);
	uint32_t out_w = (uint32_t)output_texture.width;
	uint32_t out_h = (uint32_t)output_texture.height;
	uint32_t margin = 10;
	float scale = 1.0f;
	float max_frac = 0.5f;
	if (hud_w > (uint32_t)(out_w * max_frac)) {
		scale = (out_w * max_frac) / (float)hud_w;
	}
	uint32_t draw_w = (uint32_t)(hud_w * scale);
	uint32_t draw_h = (uint32_t)(hud_h * scale);

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = output_texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;

	id<MTLRenderCommandEncoder> encoder = [cmd_buf renderCommandEncoderWithDescriptor:pass];
	[encoder setRenderPipelineState:c->hud_blit_pipeline];
	[encoder setFragmentTexture:c->hud_texture atIndex:0];
	[encoder setFragmentSamplerState:c->sampler_linear atIndex:0];

	MTLViewport vp;
	vp.originX = margin;
	vp.originY = (out_h > draw_h + margin) ? (out_h - draw_h - margin) : 0;
	vp.width = draw_w;
	vp.height = draw_h;
	vp.znear = 0;
	vp.zfar = 1;
	[encoder setViewport:vp];

	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];
}

// Blit the content region of c->atlas_texture (tile_columns × view_width
// by tile_rows × view_height — what the app actually wrote, matching what
// the compositor crops and hands to the DP) into a shared MTLBuffer, swap
// BGRA→RGBA, and write @p path as PNG. Caller must have already committed
// @p render_cmd_buf — we waitUntilCompleted on it for sync.
static bool
metal_compositor_capture_atlas_to_png(struct comp_metal_compositor *c,
                                      id<MTLCommandBuffer> render_cmd_buf,
                                      const char *path)
{
	if (c->atlas_texture == nil || c->tile_columns == 0 || c->tile_rows == 0 ||
	    c->view_width == 0 || c->view_height == 0) {
		return false;
	}

	bool ok = false;
	@autoreleasepool {
		uint32_t content_w = c->tile_columns * c->view_width;
		uint32_t content_h = c->tile_rows * c->view_height;
		// Clamp to the allocated atlas in case a mode switch produces
		// a content region larger than the worst-case pre-allocation.
		if (content_w > (uint32_t)c->atlas_texture.width) {
			content_w = (uint32_t)c->atlas_texture.width;
		}
		if (content_h > (uint32_t)c->atlas_texture.height) {
			content_h = (uint32_t)c->atlas_texture.height;
		}

		size_t row_pitch = (size_t)content_w * 4;
		size_t buf_bytes = row_pitch * content_h;
		id<MTLBuffer> staging = [c->device newBufferWithLength:buf_bytes
		                                               options:MTLResourceStorageModeShared];
		if (staging == nil) {
			return false;
		}

		// Render cmd_buf was already committed; wait for it so the
		// atlas contents are visible before we blit them out.
		[render_cmd_buf waitUntilCompleted];

		id<MTLCommandBuffer> blit_cb = [c->command_queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [blit_cb blitCommandEncoder];
		[blit copyFromTexture:c->atlas_texture
		          sourceSlice:0
		          sourceLevel:0
		         sourceOrigin:MTLOriginMake(0, 0, 0)
		           sourceSize:MTLSizeMake(content_w, content_h, 1)
		             toBuffer:staging
		    destinationOffset:0
		destinationBytesPerRow:row_pitch
		destinationBytesPerImage:buf_bytes];
		[blit endEncoding];
		[blit_cb commit];
		[blit_cb waitUntilCompleted];

		// Atlas format is BGRA8Unorm — swap into RGBA for stbi_write_png.
		uint8_t *bgra = (uint8_t *)staging.contents;
		uint8_t *rgba = malloc(buf_bytes);
		if (rgba != NULL) {
			for (size_t i = 0; i < buf_bytes; i += 4) {
				rgba[i + 0] = bgra[i + 2];
				rgba[i + 1] = bgra[i + 1];
				rgba[i + 2] = bgra[i + 0];
				rgba[i + 3] = bgra[i + 3];
			}
			ok = stbi_write_png(path, (int)content_w, (int)content_h, 4, rgba, (int)row_pitch) != 0;
			free(rgba);
		}
	}
	return ok;
}

// Run the capture readback if the per-frame intent matches @p mode_filter.
// Caller passes the in-flight render cmd_buf; capture waits on it before
// the blit-out so the atlas contents are visible.
static void
metal_compositor_dispatch_capture(struct comp_metal_compositor *c,
                                   id<MTLCommandBuffer> render_cmd_buf,
                                   uint32_t mode_filter)
{
	if (!u_capture_intent_should_capture(&c->capture_intent, mode_filter)) {
		return;
	}
	bool ok = metal_compositor_capture_atlas_to_png(c, render_cmd_buf,
	                                                 c->capture_intent.path);
	if (ok) {
		U_LOG_I("Atlas captured (mode=%u) to %s",
		        c->capture_intent.mode, c->capture_intent.path);
	} else {
		U_LOG_W("Atlas capture failed (mode=%u path=%s)",
		        c->capture_intent.mode, c->capture_intent.path);
	}
	u_capture_intent_complete(&c->capture_intent, &c->mcp_capture, ok);
}

static xrt_result_t
metal_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_metal_compositor *c = metal_comp(xc);

	// Capture-intent poll — see u_capture_intent.h. Consumed at the
	// projection-done boundary (PROJECTION_ONLY, once cmd-buf split
	// lands) or end of frame (POST_COMPOSE).
	u_capture_intent_poll(&c->capture_intent, &c->mcp_capture);

	// Frame timing for HUD
	uint64_t now_ns = os_monotonic_get_ns();
	float dt = (c->last_frame_ns > 0) ? (float)(now_ns - c->last_frame_ns) / 1e9f : 0.016f;
	c->last_frame_ns = now_ns;

	// Update CAMetalLayer drawable size on window resize
	if (c->metal_layer != nil && c->view != nil) {
		NSRect backing = [c->view convertRectToBacking:c->view.bounds];
		CGSize newSize = CGSizeMake(backing.size.width, backing.size.height);
		if (c->metal_layer.drawableSize.width != newSize.width ||
		    c->metal_layer.drawableSize.height != newSize.height) {
			c->metal_layer.drawableSize = newSize;
		}
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

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = c->xsysd->static_roles.head;
			if (head != NULL && head->hmd != NULL) {
				if (force_2d) {
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						c->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					head->hmd->active_rendering_mode_index = c->last_3d_mode_index;
				}
			}
			comp_metal_compositor_request_display_mode(&c->base.base, !force_2d);
		}

		// Rendering mode change from qwerty 0/1/2/3/4 keys.
		// Legacy apps only support V toggle — skip direct mode selection.
		if (!c->legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(c->xsysd->xdevs, c->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = c->xsysd->static_roles.head;
				if (head != NULL) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// Sync hardware_display_3d, tile layout, and per-view dimensions
	// from device's active rendering mode.
	// Legacy apps: view dims are fixed at compromise scale, only update tile layout.
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			c->hardware_display_3d = mode->hardware_display_3d;
			if (mode->tile_columns > 0) {
				c->tile_columns = mode->tile_columns;
				c->tile_rows = mode->tile_rows;
			}
			if (!c->legacy_app_tile_scaling && mode->view_width_pixels > 0) {
				c->view_width = mode->view_width_pixels;
				c->view_height = mode->view_height_pixels;
				if (c->canvas.valid) {
					u_tiling_compute_canvas_view(mode, c->canvas.w, c->canvas.h,
					                             &c->view_width, &c->view_height);
				} else if (!c->owns_window && output_texture != nil) {
					// Handle app: window may differ from display size,
					// derive view dims from actual drawable backing size.
					u_tiling_compute_canvas_view(mode,
					                             (uint32_t)output_texture.width,
					                             (uint32_t)output_texture.height,
					                             &c->view_width, &c->view_height);
				}
			}
		}
	}

	// HUD is rendered after weave, before present (see below)

	// Zero-copy check: can we pass the app's swapchain directly to the DP?
	bool zero_copy = false;
	id<MTLTexture> zc_texture = nil;

	if (c->layer_accum.layer_count == 1) {
		struct comp_layer *layer = &c->layer_accum.layers[0];
		if (layer->data.type == XRT_LAYER_PROJECTION ||
		    layer->data.type == XRT_LAYER_PROJECTION_DEPTH) {
			const struct xrt_rendering_mode *mode = NULL;
			if (c->xdev != NULL && c->xdev->hmd != NULL) {
				uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
				if (idx < c->xdev->rendering_mode_count)
					mode = &c->xdev->rendering_modes[idx];
			}
			if (mode != NULL && mode->view_count <= XRT_MAX_VIEWS) {
				uint32_t vc = mode->view_count;
				// All views must reference the same swapchain
				bool same_sc = (vc > 0 && layer->sc_array[0] != NULL);
				for (uint32_t v = 1; v < vc && same_sc; v++) {
					if (layer->sc_array[v] != layer->sc_array[0])
						same_sc = false;
				}
				if (same_sc) {
					uint32_t img_idx = layer->data.proj.v[0].sub.image_index;
					bool same_idx = true;
					for (uint32_t v = 1; v < vc; v++) {
						if (layer->data.proj.v[v].sub.image_index != img_idx) {
							same_idx = false;
							break;
						}
					}
					bool all_array_zero = same_idx;
					for (uint32_t v = 0; v < vc && all_array_zero; v++) {
						if (layer->data.proj.v[v].sub.array_index != 0)
							all_array_zero = false;
					}
					if (all_array_zero) {
						struct comp_metal_swapchain *msc = metal_swapchain(layer->sc_array[0]);
						int32_t rxs[XRT_MAX_VIEWS], rys[XRT_MAX_VIEWS];
						uint32_t rws[XRT_MAX_VIEWS], rhs[XRT_MAX_VIEWS];
						for (uint32_t v = 0; v < vc; v++) {
							rxs[v] = layer->data.proj.v[v].sub.rect.offset.w;
							rys[v] = layer->data.proj.v[v].sub.rect.offset.h;
							rws[v] = layer->data.proj.v[v].sub.rect.extent.w;
							rhs[v] = layer->data.proj.v[v].sub.rect.extent.h;
						}
						if (u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs,
						                           msc->info.width, msc->info.height, mode)) {
							zero_copy = true;
							zc_texture = msc->images[img_idx];
						}
					}
				}
			}
		}
	}

	// Step 1: Render layers into atlas texture (skip if zero-copy)
	if (!zero_copy && c->atlas_texture != nil && c->layer_accum.layer_count > 0) {
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

			// Verify app renders at the expected resolution (not stretched)
			{
				static int rect_check_log = 0;
				for (uint32_t v = 0; v < layer->data.view_count && v < XRT_MAX_VIEWS; v++) {
					const struct xrt_rect *r = &layer->data.proj.v[v].sub.rect;
					uint32_t expected_w = c->view_width;
					uint32_t expected_h = c->view_height;
					if (r->extent.w != (int32_t)expected_w || r->extent.h != (int32_t)expected_h) {
						if (rect_check_log < 5) {
							U_LOG_W("VIEW SIZE MISMATCH: view[%u] app_rect=%ux%u "
							        "expected=%ux%u (tiles=%ux%u legacy=%d) — "
							        "app renders wrong resolution, will be stretched!",
							        v, r->extent.w, r->extent.h,
							        expected_w, expected_h,
							        c->tile_columns, c->tile_rows,
							        c->legacy_app_tile_scaling);
							rect_check_log++;
						}
					} else if (rect_check_log < 3) {
						U_LOG_I("VIEW SIZE OK: view[%u] app_rect=%ux%u matches expected=%ux%u",
						        v, r->extent.w, r->extent.h, expected_w, expected_h);
						rect_check_log++;
					}
				}
			}

			// Use min of compositor's tile count and layer's submitted views
			uint32_t mode_views = c->hardware_display_3d ? (c->tile_columns * c->tile_rows) : 1;
			uint32_t view_count = (layer->data.view_count < mode_views) ? layer->data.view_count : mode_views;
			if (view_count == 0) view_count = 1;
			for (uint32_t eye = 0; eye < view_count; eye++) {
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

		// Projection-only capture point — atlas now contains projection
		// content for every tile; window-space layers haven't been drawn
		// yet. Capture requires the projection commands to actually be on
		// GPU (not just recorded), so we end the projection encoder, commit
		// the cmd_buf, run the capture (which waits + blits + encodes PNG),
		// then start a fresh cmd_buf + render encoder with LoadAction=Load
		// so the window-space pass paints on top of the preserved atlas.
		// Skipped when no PROJECTION_ONLY intent is pending.
		if (c->capture_intent.pending && c->capture_intent.mode == MCP_CAPTURE_MODE_PROJECTION_ONLY) {
			[encoder endEncoding];
			[cmd_buf commit];

			metal_compositor_dispatch_capture(c, cmd_buf, MCP_CAPTURE_MODE_PROJECTION_ONLY);

			cmd_buf = [c->command_queue commandBuffer];

			MTLRenderPassDescriptor *ws_pass = [MTLRenderPassDescriptor renderPassDescriptor];
			ws_pass.colorAttachments[0].texture = c->atlas_texture;
			ws_pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
			ws_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			ws_pass.depthAttachment.texture = c->depth_texture;
			ws_pass.depthAttachment.loadAction = MTLLoadActionLoad;
			ws_pass.depthAttachment.storeAction = MTLStoreActionDontCare;
			encoder = [cmd_buf renderCommandEncoderWithDescriptor:ws_pass];
		}

		// Window-space layers (XR_EXT_win32_window_binding) — drawn into
		// each per-eye tile of the atlas with horizontal disparity shift,
		// so the display processor weaves them in stereo just like projection
		// content. Mirrors d3d11_compositor_layer_window_space + the renderer
		// window-space pass.
		uint32_t mode_views_ws = c->hardware_display_3d ? (c->tile_columns * c->tile_rows) : 1;
		for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
			struct comp_layer *layer = &c->layer_accum.layers[i];
			if (layer->data.type != XRT_LAYER_WINDOW_SPACE) {
				continue;
			}
			const struct xrt_layer_window_space_data *ws = &layer->data.window_space;
			struct xrt_swapchain *sc = layer->sc_array[0];
			if (sc == NULL) {
				continue;
			}
			struct comp_metal_swapchain *msc = metal_swapchain(sc);
			uint32_t img_idx = ws->sub.image_index;
			if (img_idx >= msc->image_count) {
				continue;
			}
			id<MTLTexture> src_tex = msc->images[img_idx];
			if (src_tex == nil) {
				continue;
			}

			// Source UV sub-rect (default to full texture if not specified)
			struct xrt_normalized_rect nr = ws->sub.norm_rect;
			if (nr.w <= 0.0f || nr.h <= 0.0f) {
				nr.x = 0.0f; nr.y = 0.0f; nr.w = 1.0f; nr.h = 1.0f;
			}

			for (uint32_t eye = 0; eye < mode_views_ws; eye++) {
				uint32_t tile_x = eye % c->tile_columns;
				uint32_t tile_y = eye / c->tile_columns;

				// Per-eye horizontal disparity (eye 0 shifts left).
				float half_disp = ws->disparity / 2.0f;
				float eye_shift = (eye == 0) ? -half_disp : half_disp;

				// Per-eye sub-rect within the atlas, in atlas pixels. The
				// projection_pipeline shader expects viewport=(0,0,1,1) and
				// emits a fullscreen triangle in NDC; the MTLViewport
				// clamps that to the sub-rect we want filled. This way we
				// reuse the existing shader without per-quad geometry.
				float tile_origin_x = c->hardware_display_3d ? (float)(tile_x * c->view_width) : 0.0f;
				float tile_origin_y = c->hardware_display_3d ? (float)(tile_y * c->view_height) : 0.0f;
				float tile_w = c->hardware_display_3d ? (float)c->view_width
				                                       : (float)(c->tile_columns * c->view_width);
				float tile_h = c->hardware_display_3d ? (float)c->view_height
				                                       : (float)(c->tile_rows * c->view_height);

				MTLViewport vp;
				vp.originX = tile_origin_x + (ws->x + eye_shift) * tile_w;
				vp.originY = tile_origin_y + ws->y * tile_h;
				vp.width = ws->width * tile_w;
				vp.height = ws->height * tile_h;
				vp.znear = 0.0;
				vp.zfar = 1.0;
				if (vp.width <= 0.0 || vp.height <= 0.0) {
					continue;
				}
				[encoder setViewport:vp];

				[encoder setRenderPipelineState:c->projection_pipeline];
				[encoder setDepthStencilState:c->depth_stencil_state];
				[encoder setFragmentTexture:src_tex atIndex:0];
				[encoder setFragmentSamplerState:c->sampler_linear atIndex:0];

				struct {
					float viewport[4];
					float src_rect[4];
					float color_scale[4];
					float color_bias[4];
					float swizzle_rb;
					float _pad[3];
				} constants;

				// Fullscreen-triangle settings — MTLViewport already does
				// the sub-rect clamp.
				constants.viewport[0] = 0.0f;
				constants.viewport[1] = 0.0f;
				constants.viewport[2] = 1.0f;
				constants.viewport[3] = 1.0f;

				constants.src_rect[0] = nr.x;
				constants.src_rect[1] = nr.y;
				constants.src_rect[2] = nr.w;
				constants.src_rect[3] = nr.h;
				if (layer->data.flip_y) {
					constants.src_rect[1] = nr.y + nr.h;
					constants.src_rect[3] = -nr.h;
				}

				constants.color_scale[0] = 1.0f;
				constants.color_scale[1] = 1.0f;
				constants.color_scale[2] = 1.0f;
				constants.color_scale[3] = 1.0f;
				constants.color_bias[0] = 0.0f;
				constants.color_bias[1] = 0.0f;
				constants.color_bias[2] = 0.0f;
				constants.color_bias[3] = 0.0f;
				constants.swizzle_rb = 0.0f;
				constants._pad[0] = constants._pad[1] = constants._pad[2] = 0.0f;

				[encoder setVertexBytes:&constants length:sizeof(constants) atIndex:0];
				[encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
				[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			}
		}

		[encoder endEncoding];
	}

	// File-triggered atlas dump for autonomous screenshot verification.
	// `touch /tmp/dxr_atlas_trigger` and the next frame writes the
	// composited content region (post WS-layer pass, pre display
	// processor) to /tmp/dxr_atlas.png. Mirrors the Windows D3D11-
	// service screenshot trigger pattern; lets Claude Code inspect
	// composited output without TCC permissions for screencapture.
	{
		const char *trigger = "/tmp/dxr_atlas_trigger";
		struct stat st;
		if (c->atlas_texture != nil && c->tile_columns > 0 && c->tile_rows > 0 &&
		    c->view_width > 0 && c->view_height > 0 && stat(trigger, &st) == 0) {
			unlink(trigger);
			uint32_t cw = c->tile_columns * c->view_width;
			uint32_t ch = c->tile_rows * c->view_height;
			if (cw > (uint32_t)c->atlas_texture.width)  cw = (uint32_t)c->atlas_texture.width;
			if (ch > (uint32_t)c->atlas_texture.height) ch = (uint32_t)c->atlas_texture.height;
			size_t pitch = (size_t)cw * 4;
			size_t bytes = pitch * ch;
			id<MTLBuffer> stg = [c->device newBufferWithLength:bytes
			                                           options:MTLResourceStorageModeShared];
			if (stg != nil) {
				[cmd_buf commit];
				[cmd_buf waitUntilCompleted];
				id<MTLCommandBuffer> bcb = [c->command_queue commandBuffer];
				id<MTLBlitCommandEncoder> bl = [bcb blitCommandEncoder];
				[bl copyFromTexture:c->atlas_texture
				        sourceSlice:0 sourceLevel:0
				       sourceOrigin:MTLOriginMake(0, 0, 0)
				         sourceSize:MTLSizeMake(cw, ch, 1)
				           toBuffer:stg destinationOffset:0
				 destinationBytesPerRow:pitch
				destinationBytesPerImage:bytes];
				[bl endEncoding];
				[bcb commit];
				[bcb waitUntilCompleted];
				uint8_t *src = (uint8_t *)stg.contents;
				uint8_t *out = malloc(bytes);
				if (out != NULL) {
					for (size_t i = 0; i < bytes; i += 4) {
						out[i+0] = src[i+2]; out[i+1] = src[i+1];
						out[i+2] = src[i+0]; out[i+3] = src[i+3];
					}
					stbi_write_png("/tmp/dxr_atlas.png", (int)cw, (int)ch, 4, out, (int)pitch);
					free(out);
				}
				cmd_buf = [c->command_queue commandBuffer];
			}
		}
	}

	// Step 2: Process atlas through display processor, or simple blit fallback
	id<MTLTexture> atlas_src = zero_copy ? zc_texture : c->atlas_texture;
	if (c->display_processor != NULL && atlas_src != nil) {
		// Crop atlas to content dims before passing to DP.
		// The DP expects texture dimensions to match content exactly.
		uint32_t content_w = c->tile_columns * c->view_width;
		uint32_t content_h = c->tile_rows * c->view_height;

		// Verify content region fits within atlas
		if (content_w > c->atlas_width || content_h > c->atlas_height) {
			static int crop_err_log = 0;
			if (crop_err_log < 3) {
				U_LOG_W("CROP-BLIT OVERFLOW: content=%ux%u > atlas=%ux%u "
				        "(vw=%u vh=%u tiles=%ux%u legacy=%d)",
				        content_w, content_h,
				        c->atlas_width, c->atlas_height,
				        c->view_width, c->view_height,
				        c->tile_columns, c->tile_rows,
				        c->legacy_app_tile_scaling);
				crop_err_log++;
			}
		}
		id<MTLTexture> dp_src = atlas_src;

		if (content_w != c->atlas_width || content_h != c->atlas_height) {
			// Lazily (re)create intermediate texture at content dims
			if (c->dp_input_width != content_w || c->dp_input_height != content_h) {
				[c->dp_input_texture release];
				MTLTextureDescriptor *desc = [MTLTextureDescriptor
				    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				    width:content_w height:content_h mipmapped:NO];
				desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
				c->dp_input_texture = [c->device newTextureWithDescriptor:desc];
				c->dp_input_width = content_w;
				c->dp_input_height = content_h;
				U_LOG_I("Metal crop: created DP input texture %ux%u (atlas %ux%u)",
				        content_w, content_h, c->atlas_width, c->atlas_height);
			}

			// Blit content region from atlas into intermediate texture
			id<MTLBlitCommandEncoder> blit = [cmd_buf blitCommandEncoder];
			[blit copyFromTexture:atlas_src
			          sourceSlice:0
			          sourceLevel:0
			         sourceOrigin:MTLOriginMake(0, 0, 0)
			           sourceSize:MTLSizeMake(content_w, content_h, 1)
			            toTexture:c->dp_input_texture
			     destinationSlice:0
			     destinationLevel:0
			    destinationOrigin:MTLOriginMake(0, 0, 0)];
			[blit endEncoding];
			dp_src = c->dp_input_texture;
		}

		// DP target: full output_texture dims. The canvas sub-rect is
		// communicated via canvas_offset_x/y + canvas_width/height so the DP
		// writes into the sub-rect within the shared IOSurface, matching
		// where the app reads from (ADR-010).
		uint32_t dp_target_w = (uint32_t)output_texture.width;
		uint32_t dp_target_h = (uint32_t)output_texture.height;

		xrt_display_processor_metal_process_atlas(
		    c->display_processor,
		    (__bridge void *)cmd_buf,
		    (__bridge void *)dp_src,
		    c->view_width,
		    c->view_height,
		    c->tile_columns,
		    c->tile_rows,
		    (uint32_t)MTLPixelFormatBGRA8Unorm,
		    (__bridge void *)output_texture,
		    dp_target_w,
		    dp_target_h,
		    c->canvas.valid ? c->canvas.x : 0,
		    c->canvas.valid ? c->canvas.y : 0,
		    c->canvas.valid ? c->canvas.w : 0,
		    c->canvas.valid ? c->canvas.h : 0);
	} else {
		// No display processor: simple blit passthrough.
		MTLRenderPassDescriptor *blit_pass = [MTLRenderPassDescriptor renderPassDescriptor];
		blit_pass.colorAttachments[0].texture = output_texture;
		blit_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		blit_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		blit_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

		id<MTLRenderCommandEncoder> blit_encoder = [cmd_buf renderCommandEncoderWithDescriptor:blit_pass];

		if (atlas_src != nil) {
			[blit_encoder setRenderPipelineState:c->blit_pipeline];
			[blit_encoder setFragmentTexture:atlas_src atIndex:0];
			[blit_encoder setFragmentSamplerState:c->sampler_linear atIndex:0];
			[blit_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		}

		[blit_encoder endEncoding];
	}

	// HUD overlay (post-weave, before present)
	if (c->owns_window) {
		metal_compositor_render_hud(c, dt, cmd_buf, output_texture);
	}

	// Mirror the IOSurface output into the external window's CAMetalLayer
	// drawable so the native window actually shows content (otherwise it's
	// just whatever the AppKit background draws — i.e. solid grey).
	// Only relevant when both a shared IOSurface AND an external view are
	// present (the editor preview / Unity standalone path).
	if (c->shared_texture != nil && c->metal_layer != nil && !c->owns_window) {
		id<CAMetalDrawable> mirror = [c->metal_layer nextDrawable];
		if (mirror != nil) {
			id<MTLBlitCommandEncoder> blit = [cmd_buf blitCommandEncoder];
			NSUInteger w = MIN(output_texture.width, mirror.texture.width);
			NSUInteger h = MIN(output_texture.height, mirror.texture.height);
			[blit copyFromTexture:output_texture
			          sourceSlice:0
			          sourceLevel:0
			         sourceOrigin:MTLOriginMake(0, 0, 0)
			           sourceSize:MTLSizeMake(w, h, 1)
			            toTexture:mirror.texture
			     destinationSlice:0
			     destinationLevel:0
			    destinationOrigin:MTLOriginMake(0, 0, 0)];
			[blit endEncoding];
			[cmd_buf presentDrawable:mirror];
		}
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

	// Post-compose capture (#210). Skipped if the intent was projection-
	// only (consumed earlier once the cmd-buf split lands) or empty.
	metal_compositor_dispatch_capture(c, cmd_buf, MCP_CAPTURE_MODE_POST_COMPOSE);

	// Reset layer accumulator
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static void
metal_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_metal_compositor *c = metal_comp(xc);

	U_LOG_I("Destroying Metal compositor");

	// Uninstall the MCP capture hook before we drop any GPU resources —
	// the MCP thread can no longer request a readback against us after
	// this returns.
	mcp_capture_uninstall();
	mcp_capture_fini(&c->mcp_capture);

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

	// 3. Release DP crop texture
	[c->dp_input_texture release];
	c->dp_input_texture = nil;

	// 4. Destroy display processor
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

	// 5. Release HUD resources
	u_hud_destroy(&c->hud);
	[c->hud_texture release];
	c->hud_texture = nil;
	[c->hud_blit_pipeline release];
	c->hud_blit_pipeline = nil;

	// 6. Release Metal resources (MRR — explicit release)
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
		c->view = nil;
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
	U_LOG_W("comp_metal_compositor_create: window_handle=%p, offscreen=%d, shared_iosurface=%p, dp_factory=%p",
	        window_handle, offscreen, shared_iosurface, dp_factory_metal);

	struct comp_metal_compositor *c = U_TYPED_CALLOC(struct comp_metal_compositor);
	if (c == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	mcp_capture_init(&c->mcp_capture);
	mcp_capture_install(&c->mcp_capture);

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
	// The atlas texture must be at Retina (physical pixel) resolution so
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
	U_LOG_W("Window/headless decision: window_handle=%p, shared_iosurface=%p, offscreen=%d",
	        window_handle, shared_iosurface, offscreen);
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
				init_view_w = mode->view_width_pixels;
				init_view_h = mode->view_height_pixels;
			}
		}
	}

	// Worst-case atlas = max across all rendering modes.
	// With near-square tiling, a 1x2 layout can be taller than the display.
	uint32_t atlas_w = pixel_width;
	uint32_t atlas_h = pixel_height;
	if (xdev != NULL && xdev->rendering_mode_count > 0) {
		for (uint32_t mi = 0; mi < xdev->rendering_mode_count; mi++) {
			const struct xrt_rendering_mode *m = &xdev->rendering_modes[mi];
			U_LOG_W("MODE[%u] '%s': tiles=%ux%u scale=%.2fx%.2f view=%ux%u atlas=%ux%u",
			        mi, m->mode_name, m->tile_columns, m->tile_rows,
			        m->view_scale_x, m->view_scale_y,
			        m->view_width_pixels, m->view_height_pixels,
			        m->atlas_width_pixels, m->atlas_height_pixels);
		}
		u_tiling_compute_system_atlas(xdev->rendering_modes,
		                              xdev->rendering_mode_count,
		                              &atlas_w, &atlas_h);
		U_LOG_W("System atlas worst-case: %ux%u (fallback was %ux%u)",
		        atlas_w, atlas_h, pixel_width, pixel_height);
	}

	// Compile shaders
	if (!compile_shaders(c)) {
		os_mutex_destroy(&c->mutex);
		free(c);
		return XRT_ERROR_VULKAN;
	}

	// Create atlas texture at worst-case Retina resolution
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

	// Create HUD overlay for runtime-owned windows
	if (c->owns_window) {
		u_hud_create(&c->hud, pixel_width);
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
	xc->layer_window_space = metal_compositor_layer_window_space;
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
                                                  struct xrt_eye_positions *out_eye_pos)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	if (c->display_processor == NULL) {
		return false;
	}

	return xrt_display_processor_metal_get_predicted_eye_positions(c->display_processor, out_eye_pos);
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
	memset(out_metrics, 0, sizeof(*out_metrics));

	// If we own the window, compute metrics directly from our window + sys_info
	if (c->window != nil && c->sys_info != NULL) {
		float disp_w_m = c->sys_info->display_width_m;
		float disp_h_m = c->sys_info->display_height_m;
		uint32_t disp_px_w = c->sys_info->display_pixel_width;
		uint32_t disp_px_h = c->sys_info->display_pixel_height;
		if (disp_px_w == 0 || disp_px_h == 0 || disp_w_m <= 0 || disp_h_m <= 0) {
			return false;
		}

		NSRect backing = [c->window.contentView convertRectToBacking:c->window.contentView.bounds];
		uint32_t win_px_w = (uint32_t)backing.size.width;
		uint32_t win_px_h = (uint32_t)backing.size.height;
		if (win_px_w == 0 || win_px_h == 0) {
			return false;
		}

		float pixel_size_x = disp_w_m / (float)disp_px_w;
		float pixel_size_y = disp_h_m / (float)disp_px_h;

		out_metrics->display_width_m = disp_w_m;
		out_metrics->display_height_m = disp_h_m;
		out_metrics->display_pixel_width = disp_px_w;
		out_metrics->display_pixel_height = disp_px_h;
		out_metrics->display_screen_left = 0;
		out_metrics->display_screen_top = 0;

		out_metrics->window_pixel_width = win_px_w;
		out_metrics->window_pixel_height = win_px_h;
		out_metrics->window_screen_left = 0;
		out_metrics->window_screen_top = 0;

		out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
		out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

		// Center offset (assume centered — no screen position tracking yet)
		float win_center_px_x = (float)win_px_w / 2.0f;
		float win_center_px_y = (float)win_px_h / 2.0f;
		float disp_center_px_x = (float)disp_px_w / 2.0f;
		float disp_center_px_y = (float)disp_px_h / 2.0f;

		out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
		out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

		out_metrics->valid = true;
		u_canvas_apply_to_metrics(out_metrics, &c->canvas);
		return true;
	}

	// Fallback: delegate to display processor (ext/shared path)
	if (c->display_processor != NULL) {
		bool ok = xrt_display_processor_metal_get_window_metrics(c->display_processor, out_metrics);
		if (ok) {
			u_canvas_apply_to_metrics(out_metrics, &c->canvas);
		}
		return ok;
	}

	return false;
}

void
comp_metal_compositor_set_output_rect(struct xrt_compositor *xc,
                                       int32_t x, int32_t y,
                                       uint32_t w, uint32_t h)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	c->canvas = (struct u_canvas_rect){.valid = true, .x = x, .y = y, .w = w, .h = h};
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
	c->legacy_app_tile_scaling = info->legacy_app_tile_scaling;
	c->last_3d_mode_index = 1;

	// Legacy apps: fix view dims at the actual recommended size the app was told to render at.
	if (info->legacy_app_tile_scaling &&
	    info->legacy_view_width_pixels > 0 && info->legacy_view_height_pixels > 0) {
		c->view_width = info->legacy_view_width_pixels;
		c->view_height = info->legacy_view_height_pixels;
	}
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
