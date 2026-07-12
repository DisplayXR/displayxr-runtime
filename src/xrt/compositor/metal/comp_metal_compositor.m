// Copyright 2025, The DisplayXR Project
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

#include <math.h>
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
 * Zone-mask state (XR_DXR_local_3d_zone, #439 Phase 3)
 *
 */

/*!
 * Compositor-side state for one authored 2D/3D zone mask.
 *
 * Tier 1/2 author into the CPU-canonical @p author_bytes; submit uploads
 * the bytes into @p tex (replaceRegion — Shared storage, sticky
 * last-submit-wins). There is no Tier-3 authoring render target on Metal.
 */
struct comp_metal_zone_mask
{
	id<MTLTexture> tex;    //!< R8Unorm, the staged (submitted) mask content
	uint8_t *author_bytes; //!< CPU authoring buffer, w*h, M in [0,255]
	uint32_t w;            //!< Mask width in client-window pixels
	uint32_t h;            //!< Mask height in client-window pixels
	bool submitted;        //!< True once submitted at least once
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

	//! XR_DXR_display_zones (ADR-027): projection-shader variants with
	//! alpha-over blending for zone draws into the atlas, so overlapping
	//! zones composite in layer-list order instead of overwriting. Mirrors
	//! D3D11's blend_premul / blend_alpha pair on the same draw site.
	id<MTLRenderPipelineState> zone_premult_pipeline;
	id<MTLRenderPipelineState> zone_unpremult_pipeline;

	//! Render pipeline for fullscreen blit (atlas→target passthrough).
	id<MTLRenderPipelineState> blit_pipeline;

	//! Sampler state for texture sampling.
	id<MTLSamplerState> sampler_linear;

	//! Depth stencil state.
	id<MTLDepthStencilState> depth_stencil_state;

	//! Depth disabled (always pass, no write) — zone draws, like D3D11's
	//! zone branch, must not depth-reject a later overlapping zone.
	id<MTLDepthStencilState> depth_stencil_state_disabled;

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

	//! True if XR_DXR_cocoa_window_binding requested transparent background.
	//! Drives atlas clear color (alpha=0 vs alpha=1) so per-pixel alpha from
	//! projection layers reaches the CAMetalLayer + desktop composite.
	bool transparent_background;

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

	//! True if app is legacy (no XR_DXR_display_info) — gates 1/2/3 key mode selection.
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

	/*
	 * XR_DXR_local_3d_zone consumer state (#439 Phase 3).
	 */

	//! The active (submitted) explicit zone mask, or NULL. Owned by the
	//! oxr handle via the zone_mask_* entry points; this is a borrow.
	struct comp_metal_zone_mask *zone_mask_active;

	//! Runtime-owned implicit mask rasterized from the frame's Local2D
	//! layer rects (M=0 inside, M=1 elsewhere) when no explicit mask.
	id<MTLTexture> implicit_mask_tex;
	uint8_t *implicit_mask_bytes;       //!< CPU raster buffer (w*h)
	uint32_t implicit_mask_w;           //!< Current implicit mask width
	uint32_t implicit_mask_h;           //!< Current implicit mask height
	struct xrt_rect implicit_rects[XRT_MAX_LAYERS]; //!< Rect cache for change detection
	uint32_t implicit_rect_count;       //!< Number of cached rects (0 = none rasterized)

	//! Window-sized 2D scratch the Local2D layers flatten into (the `twod`
	//! input of the masked composite) + the weave snapshot it lerps against.
	id<MTLTexture> local2d_scratch;
	id<MTLTexture> weave_scratch;
	uint32_t composite_w;               //!< Current scratch width (0 = not allocated)
	uint32_t composite_h;               //!< Current scratch height

	//! #491 part 3 — the flattened 2D-UNDER backdrop (Local2D layers before the
	//! projection in list order), handed to the DP via set_background_2d so it
	//! composites `backdrop over captured-desktop` under the 3D weave.
	id<MTLTexture> backdrop_scratch;
	uint32_t backdrop_w;                //!< Current backdrop scratch width (0 = none)
	uint32_t backdrop_h;

	//! Pipelines for the Local2D flatten (premultiplied over / unpremultiplied
	//! source) and the masked composite pass.
	id<MTLRenderPipelineState> local2d_premult_pipeline;
	id<MTLRenderPipelineState> local2d_unpremult_pipeline;
	id<MTLRenderPipelineState> masked_composite_pipeline;

	//! Point sampler for the 1:1 composite reads.
	id<MTLSamplerState> sampler_nearest;

	//! True when the last committed frame carried Local2D layers (the
	//! implicit mask is per-frame; this makes its canvas-supersede effect
	//! visible to out-of-frame readers like get_window_metrics).
	bool local_2d_last_frame;

	//! XR_DXR_display_zones (ADR-027): true when the current frame's
	//! accumulator carries XRT_LAYER_ZONE_3D layers (a "zones frame"). In a
	//! zones frame the canvas output rect, the sticky submitted mask, and
	//! the implicit-mask-from-Local2D rule are all inert; the effective
	//! canvas is the full client window (zones frames count as mask_active
	//! for metal_effective_canvas); the wish drives the post-weave lerp.
	bool zones_frame;

	//! Explicit per-frame wish (XrDisplayZonesFrameEndInfoDXR.wishMask) set
	//! via comp_metal_compositor_zones_set_frame_wish before commit; NULL =
	//! auto-derive from the zone rects. Not owned — zone_mask_destroy
	//! clears any dangling reference.
	struct comp_metal_zone_mask *frame_wish;

	//! Tier-1 fallback edge state: request_display_mode(true) fired once
	//! on the zones rising edge; never forces 2D on the falling edge.
	//! P4: only taken for legacy DPs (caps.supported == 0) — a zone-capable
	//! DP gets the per-frame wish publish instead.
	bool zones_mode_requested;

	//! #224 / ADR-027 hardware-DP zone leg (P4): cached get_local_zone_caps
	//! result. 0 = not queried yet, 1 = supported, 2 = legacy DP.
	int zone_dp_state;
	//! DP zone caps when zone_dp_state == 1.
	struct xrt_dp_local_zone_caps zone_dp_caps;
	//! Published-content generation: bumped on zone_mask_submit, on an
	//! auto-wish re-raster (metal_update_zone_wish_mask's dirty path), and
	//! on an explicit-frame-wish source change — NOT per frame.
	uint64_t zone_publish_seq;
	//! True while this client's mask is published to the DP — drives the
	//! clear-on-deactivate edge.
	bool zone_published;
	//! This frame's resolved wish texture + dims, set by
	//! metal_composite_local_2d in zones frames (the explicit frame wish's
	//! CPU-uploaded texture or the auto raster) and reset at the top of
	//! layer_commit. Borrowed, not retained. The publish runs after the
	//! frame's [cmd_buf commit]; the wish textures are CPU-uploaded
	//! (replaceRegion, Shared storage), so their content is valid at the
	//! call regardless of GPU progress.
	id<MTLTexture> zone_publish_tex;
	uint32_t zone_publish_w, zone_publish_h;
	//! Seq-bump cache: last explicit wish pointer actually published (the
	//! auto raster's dirty-cache lives in wish_rects above).
	struct comp_metal_zone_mask *zone_frame_wish_last;

	//! XR_DXR_display_zones AUTO wish raster (union of the frame's zone
	//! rects with a feathered edge), CPU-rasterized like the implicit mask
	//! but kept separate so the two dirty-caches can never cross-hit.
	id<MTLTexture> wish_mask_tex;
	uint8_t *wish_mask_bytes;                   //!< CPU raster buffer (w*h)
	uint32_t wish_mask_w;                       //!< Current wish mask width (0 = none)
	uint32_t wish_mask_h;
	struct xrt_rect wish_rects[XRT_MAX_LAYERS]; //!< Rect cache for change detection
	uint32_t wish_rect_count;

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

//! #439: any active mask — explicit submitted, or implicit from the last
//! committed frame's Local2D layers — supersedes the canvas output rect.
//! XR_DXR_display_zones: a zones frame supersedes it the same way (the
//! output rect is inert; each zone rect is its own canvas).
static inline bool
metal_mask_is_active(struct comp_metal_compositor *c)
{
	return (c->zone_mask_active != NULL && c->zone_mask_active->submitted) || c->local_2d_last_frame ||
	       c->zones_frame;
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
    "}\n"
    "\n"
    "// #439 Phase 3 — masked 2D/3D composite (MSL port of the D3D11\n"
    "// masked_composite.hlsl sampled-mask path). M=1 keeps the weave, M=0\n"
    "// shows the flattened 2D layer; BOTH rgb and alpha are lerped so each\n"
    "// layer's own transparency survives to the compose-under-bg pass\n"
    "// (spec 4.2 output-alpha rule).\n"
    "// #491 alpha_over path: premultiplied 'over' of the 2D atop the weave\n"
    "// (twod + (1-twod.a)*weave) — translucent 2D reveals the 3D scene, not\n"
    "// the desktop. Used for the IMPLICIT Local2D mask; explicit masks keep\n"
    "// the hard M-lerp.\n"
    "fragment float4 masked_composite_fragment(VertexOut in [[stage_in]],\n"
    "                                          texture2d<float> twod_tex [[texture(0)]],\n"
    "                                          texture2d<float> mask_tex [[texture(1)]],\n"
    "                                          texture2d<float> weave_tex [[texture(2)]],\n"
    "                                          constant uint &alpha_over [[buffer(0)]],\n"
    "                                          sampler smp [[sampler(0)]]) {\n"
    "    float4 twod = twod_tex.sample(smp, in.texCoord);\n"
    "    float4 weave = weave_tex.sample(smp, in.texCoord);\n"
    "    if (alpha_over != 0u) {\n"
    "        return twod + (1.0 - twod.a) * weave;\n"
    "    }\n"
    "    float M = saturate(mask_tex.sample(smp, in.texCoord).r);\n"
    "    return M * weave + (1.0 - M) * twod;\n"
    "}\n";

/*
 *
 * Format conversion
 *
 */

// Map an sRGB Metal pixel format to its plain UNORM sibling (identity
// otherwise). Used to sample an app sRGB swapchain through a non-decoding view
// so the compositor passes the app's display-referred bytes through to the DP
// unchanged. Mirrors the GL/D3D/VK sRGB-passthrough fixes.
static MTLPixelFormat
metal_srgb_to_unorm(MTLPixelFormat format)
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
	case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
	default: return format;
	}
}

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
	//
	// Blending DISABLED — projection layer pixels are written straight to the
	// atlas (including the source alpha channel). Mirrors D3D11 `blend_opaque`
	// in comp_d3d11_renderer.cpp. Source-alpha blending here destroys Unity's
	// alpha=0 in transparent regions (atlas dst.a=0 + src.a=0 with
	// out.a = src.a*1 + dst.a*(1-src.a) collapses to 0/0, RGB to the dark
	// atlas clear) so transparent-background mode never reaches the
	// CAMetalLayer with alpha < 1. Disabling blending lets Unity's swapchain
	// alpha pass through end-to-end (Phase 1 transparent overlay, #85).
	{
		MTLRenderPipelineDescriptor *proj_desc = [[MTLRenderPipelineDescriptor alloc] init];
		proj_desc.vertexFunction = proj_vs;
		proj_desc.fragmentFunction = proj_fs;
		proj_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		proj_desc.colorAttachments[0].blendingEnabled = NO;
		proj_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

		c->projection_pipeline = [c->device newRenderPipelineStateWithDescriptor:proj_desc error:&error];
		[proj_desc release];
		if (c->projection_pipeline == nil) {
			U_LOG_E("Failed to create projection pipeline: %s",
			        error.localizedDescription.UTF8String);
			goto cleanup;
		}
	}

	// Zone alpha-over pipelines (XR_DXR_display_zones, ADR-027): the same
	// projection shaders + depth attachment, but with "over" blending so
	// overlapping zones composite in layer-list order (the no-blend rationale
	// above protects full-canvas projection alpha passthrough; zone draws are
	// placed sub-rects whose content alpha is the compositing contract —
	// rule 4). Premultiplied by default; the unpremultiplied variant only
	// changes the source RGB factor (XR_COMPOSITION_LAYER_UNPREMULTIPLIED_
	// ALPHA_BIT). Alpha factors are One/OneMinusSrcAlpha in both so the
	// zone's own transparency survives into the atlas (the unzoned/zoned
	// alpha drives the desktop show-through downstream).
	{
		MTLRenderPipelineDescriptor *zone_desc = [[MTLRenderPipelineDescriptor alloc] init];
		zone_desc.vertexFunction = proj_vs;
		zone_desc.fragmentFunction = proj_fs;
		zone_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		zone_desc.colorAttachments[0].blendingEnabled = YES;
		zone_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
		zone_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		zone_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		zone_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		zone_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

		c->zone_premult_pipeline = [c->device newRenderPipelineStateWithDescriptor:zone_desc error:&error];
		if (c->zone_premult_pipeline == nil) {
			U_LOG_E("Failed to create zone premult pipeline: %s",
			        error.localizedDescription.UTF8String);
			[zone_desc release];
			goto cleanup;
		}

		zone_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		c->zone_unpremult_pipeline = [c->device newRenderPipelineStateWithDescriptor:zone_desc error:&error];
		[zone_desc release];
		if (c->zone_unpremult_pipeline == nil) {
			U_LOG_E("Failed to create zone unpremult pipeline: %s",
			        error.localizedDescription.UTF8String);
			goto cleanup;
		}
	}

	// Local-2D flatten pipelines (#439 Phase 3) — projection shaders with
	// alpha blending into the window-sized 2D scratch (no depth attachment).
	// Premultiplied "over" by default; the unpremultiplied variant only
	// changes the source RGB factor (XR_COMPOSITION_LAYER_UNPREMULTIPLIED_
	// ALPHA_BIT). Alpha factors are One/OneMinusSrcAlpha in both so the
	// flattened layer's own transparency survives (spec §4.2).
	{
		MTLRenderPipelineDescriptor *l2d_desc = [[MTLRenderPipelineDescriptor alloc] init];
		l2d_desc.vertexFunction = proj_vs;
		l2d_desc.fragmentFunction = proj_fs;
		l2d_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		l2d_desc.colorAttachments[0].blendingEnabled = YES;
		l2d_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
		l2d_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		l2d_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		l2d_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

		c->local2d_premult_pipeline = [c->device newRenderPipelineStateWithDescriptor:l2d_desc error:&error];
		if (c->local2d_premult_pipeline == nil) {
			U_LOG_E("Failed to create local-2D premult pipeline: %s",
			        error.localizedDescription.UTF8String);
			[l2d_desc release];
			goto cleanup;
		}

		l2d_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		c->local2d_unpremult_pipeline = [c->device newRenderPipelineStateWithDescriptor:l2d_desc error:&error];
		[l2d_desc release];
		if (c->local2d_unpremult_pipeline == nil) {
			U_LOG_E("Failed to create local-2D unpremult pipeline: %s",
			        error.localizedDescription.UTF8String);
			goto cleanup;
		}
	}

	// Masked composite pipeline (#439 Phase 3) — fullscreen mask-lerp of
	// {2D scratch, mask, weave snapshot} into the output. No blending: the
	// shader computes the final value (incl. alpha) directly.
	{
		id<MTLFunction> mc_fs = [library newFunctionWithName:@"masked_composite_fragment"];
		if (mc_fs == nil) {
			U_LOG_E("Failed to find masked_composite_fragment");
			goto cleanup;
		}
		MTLRenderPipelineDescriptor *mc_desc = [[MTLRenderPipelineDescriptor alloc] init];
		mc_desc.vertexFunction = blit_vs;
		mc_desc.fragmentFunction = mc_fs;
		mc_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

		c->masked_composite_pipeline = [c->device newRenderPipelineStateWithDescriptor:mc_desc error:&error];
		[mc_desc release];
		[mc_fs release];
		if (c->masked_composite_pipeline == nil) {
			U_LOG_E("Failed to create masked composite pipeline: %s",
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

	// Point sampler — 1:1 composite reads (#439 Phase 3, mirrors the D3D11
	// composite's point sampler).
	{
		MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
		sampler_desc.minFilter = MTLSamplerMinMagFilterNearest;
		sampler_desc.magFilter = MTLSamplerMinMagFilterNearest;
		sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
		sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
		c->sampler_nearest = [c->device newSamplerStateWithDescriptor:sampler_desc];
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
	{
		MTLDepthStencilDescriptor *ds_desc = [[MTLDepthStencilDescriptor alloc] init];
		ds_desc.depthCompareFunction = MTLCompareFunctionAlways;
		ds_desc.depthWriteEnabled = NO;
		c->depth_stencil_state_disabled = [c->device newDepthStencilStateWithDescriptor:ds_desc];
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
create_window_on_main_thread(struct comp_metal_compositor *c, uint32_t width, uint32_t height,
                             bool transparent_background, bool *out_success)
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

	if (transparent_background) {
		// macOS native transparency: NSWindow non-opaque + clear background +
		// CAMetalLayer non-opaque lets the desktop show through any pixels
		// the compositor writes with alpha < 1. sim_display preserves alpha
		// natively to the drawable; no chroma-key trick.
		[c->window setOpaque:NO];
		[c->window setBackgroundColor:[NSColor clearColor]];
		c->metal_layer.opaque = NO;
		U_LOG_W("Transparent background enabled: NSWindow + CAMetalLayer set isOpaque=NO");
	}

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
create_window(struct comp_metal_compositor *c, uint32_t width, uint32_t height,
              bool transparent_background)
{
	__block bool success = false;

	if ([NSThread isMainThread]) {
		// Already on main thread — call directly to avoid deadlock
		create_window_on_main_thread(c, width, height, transparent_background, &success);
	} else {
		dispatch_sync(dispatch_get_main_queue(), ^{
			create_window_on_main_thread(c, width, height, transparent_background, &success);
		});
	}

	return success;
}

static bool
setup_external_window(struct comp_metal_compositor *c, NSView *external_view,
                      bool transparent_background)
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

	if (transparent_background) {
		// External NSWindow is owned by the app — they're responsible for
		// setOpaque:NO + clear background. We only configure the layer the
		// compositor presents into.
		c->metal_layer.opaque = NO;
		U_LOG_W("Transparent background enabled: external CAMetalLayer set isOpaque=NO");
	}

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

	// Layered (arraySize>1) swapchains cannot be backed by a single-plane
	// IOSurface — a MTLTextureType2DArray needs `array_size` slices and an
	// IOSurface is a single 2D plane. Under single-pass-instanced the app
	// renders both eyes into slices 0/1 of one array texture and submits two
	// projection views with imageArrayIndex 0/1; the compositor samples the
	// slice (see the compose pass). Cross-API (Vulkan/MoltenVK) import of a
	// layered swapchain is not a supported path, so array swapchains skip the
	// IOSurface and allocate a plain array texture — the Metal native app gets
	// the id<MTLTexture> directly via comp_metal_swapchain_get_texture().
	const bool layered = info->array_size > 1;

	for (uint32_t i = 0; i < msc->image_count; i++) {
		MTLTextureDescriptor *desc = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:format
		                                width:info->width
		                               height:info->height
		                            mipmapped:(info->mip_count > 1) ? YES : NO];
		// PixelFormatView lets the compositor sample an sRGB swapchain through a
		// UNORM view (sRGB passthrough — see the compose pass) without the GPU
		// auto-decoding sRGB->linear. The app's own render target keeps the sRGB
		// format, so its encoding (if any) is unchanged.
		desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
		             MTLTextureUsagePixelFormatView;
		desc.storageMode = MTLStorageModeShared;

		if (layered) {
			desc.textureType = MTLTextureType2DArray;
			desc.arrayLength = info->array_size;
			// Layered swapchains have no IOSurface backing (below) and no CPU
			// access path — keep them GPU-private. Shared-storage render
			// targets also break Unity's Metal RenderSurface machinery when a
			// client wraps slice views as XR render targets (the editor SEGVs
			// creating the surface's main texture — displayxr-unity#204).
			desc.storageMode = MTLStorageModePrivate;
			msc->iosurfaces[i] = NULL;
			msc->images[i] = [c->device newTextureWithDescriptor:desc];
			if (msc->images[i] == nil) {
				U_LOG_E("Failed to create layered swapchain image %u", i);
				metal_swapchain_destroy(&msc->base.base);
				return XRT_ERROR_ALLOCATION;
			}
			// No IOSurface native handle for layered swapchains.
			msc->base.images[i].handle = (xrt_graphics_buffer_handle_t)0;
			continue;
		}

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
 * Window-space layer (XR_DXR_win32_window_binding). Positioned in fractional
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
 * Local-2D layer (XR_DXR_local_3d_zone v3, #439 Phase 3). Post-weave 2D
 * content at a client-window pixel rect, mask-gated. Here we just
 * accumulate; the Metal consumer (flatten + masked composite) runs in
 * layer_commit — search for `XRT_LAYER_LOCAL_2D` in this file.
 */
static xrt_result_t
metal_compositor_layer_local_2d(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc,
                                const struct xrt_layer_data *data)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	comp_layer_accum_local_2d(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * 3D display zone layer (XR_DXR_display_zones, ADR-027) — multi-swapchain
 * accumulate like projection; consumed by the zones-frame branch of
 * layer_commit (zone rect scaled into the window-spanning atlas tile).
 */
static xrt_result_t
metal_compositor_layer_zone_3d(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                               const struct xrt_layer_data *data)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	comp_layer_accum_zone_3d(&c->layer_accum, xsc, data);
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
			data.ipd_factor = ss.ipd_factor;
			data.parallax_factor = ss.parallax_factor;
			data.inv_convergence_distance = ss.inv_convergence_distance;
			data.half_tan_vfov = ss.half_tan_vfov;
			data.m2v = ss.m2v;
			data.virtual_display_height = ss.virtual_display_height;
			data.perspective_factor = ss.perspective_factor;
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
				// Force opaque: swapchain alpha is undefined for display
				// output, and left as-is the PNG renders transparent/black
				// (issue #425).
				rgba[i + 3] = 255;
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

/*
 *
 * Local 2D/3D zone consumer (#439 Phase 3)
 *
 */

/*!
 * Best available client-window backing dimensions. Order: the bound NSView's
 * backing size (handle + texture apps pass a real view), the active mask dims,
 * the output texture itself (handle apps: drawable == window).
 */
static bool
metal_window_backing_dims(struct comp_metal_compositor *c,
                          id<MTLTexture> output_texture,
                          uint32_t *out_w,
                          uint32_t *out_h)
{
	if (c->view != nil) {
		NSRect backing = [c->view convertRectToBacking:c->view.bounds];
		if (backing.size.width > 0 && backing.size.height > 0) {
			*out_w = (uint32_t)backing.size.width;
			*out_h = (uint32_t)backing.size.height;
			return true;
		}
	}
	if (c->zone_mask_active != NULL && c->zone_mask_active->w > 0 && c->zone_mask_active->h > 0) {
		*out_w = c->zone_mask_active->w;
		*out_h = c->zone_mask_active->h;
		return true;
	}
	if (output_texture != nil) {
		*out_w = (uint32_t)output_texture.width;
		*out_h = (uint32_t)output_texture.height;
		return true;
	}
	return false;
}

/*!
 * #439 Phase 2 rule, uniform across explicit and implicit masks: while a
 * mask is active the effective canvas is the client-window rect (top-left
 * anchored), regardless of any xrSetSharedTextureOutputRectDXR call.
 * Mirrors d3d11_effective_canvas.
 */
static struct u_canvas_rect
metal_effective_canvas(struct comp_metal_compositor *c,
                       id<MTLTexture> output_texture,
                       bool mask_active)
{
	if (!mask_active) {
		return (struct u_canvas_rect){0};
	}

	struct u_canvas_rect r = {0};
	uint32_t w = 0;
	uint32_t h = 0;
	if (metal_window_backing_dims(c, output_texture, &w, &h)) {
		r.valid = true;
		r.x = 0;
		r.y = 0;
		r.w = w;
		r.h = h;
	}
	return r;
}

/*!
 * Rasterize the implicit Tier-2-style mask from the frame's Local2D layer
 * rects: M=1 everywhere, M=0 inside the union of the rects (Q3). CPU fill +
 * replaceRegion; re-rasterized only when the rect set or dims change.
 * Returns the mask texture or nil on failure.
 */
static id<MTLTexture>
metal_update_implicit_mask(struct comp_metal_compositor *c,
                           const struct xrt_rect *rects,
                           uint32_t rect_count,
                           uint32_t w,
                           uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return nil;
	}

	bool dirty = (c->implicit_mask_tex == nil) || c->implicit_mask_w != w || c->implicit_mask_h != h ||
	             c->implicit_rect_count != rect_count;
	for (uint32_t i = 0; !dirty && i < rect_count; i++) {
		if (memcmp(&c->implicit_rects[i], &rects[i], sizeof(rects[i])) != 0) {
			dirty = true;
		}
	}
	if (!dirty) {
		return c->implicit_mask_tex;
	}

	if (c->implicit_mask_tex == nil || c->implicit_mask_w != w || c->implicit_mask_h != h) {
		[c->implicit_mask_tex release];
		c->implicit_mask_tex = nil;
		free(c->implicit_mask_bytes);
		c->implicit_mask_bytes = NULL;

		MTLTextureDescriptor *desc =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
		                                                       width:w
		                                                      height:h
		                                                   mipmapped:NO];
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		c->implicit_mask_tex = [c->device newTextureWithDescriptor:desc];
		c->implicit_mask_bytes = (uint8_t *)malloc((size_t)w * h);
		if (c->implicit_mask_tex == nil || c->implicit_mask_bytes == NULL) {
			U_LOG_E("Failed to allocate implicit zone mask (%ux%u)", w, h);
			[c->implicit_mask_tex release];
			c->implicit_mask_tex = nil;
			free(c->implicit_mask_bytes);
			c->implicit_mask_bytes = NULL;
			return nil;
		}
		c->implicit_mask_w = w;
		c->implicit_mask_h = h;
	}

	memset(c->implicit_mask_bytes, 0xFF, (size_t)w * h);
	for (uint32_t i = 0; i < rect_count; i++) {
		int32_t x0 = rects[i].offset.w;
		int32_t y0 = rects[i].offset.h;
		int32_t x1 = x0 + rects[i].extent.w;
		int32_t y1 = y0 + rects[i].extent.h;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > (int32_t)w) x1 = (int32_t)w;
		if (y1 > (int32_t)h) y1 = (int32_t)h;
		for (int32_t y = y0; y < y1; y++) {
			memset(c->implicit_mask_bytes + (size_t)y * w + x0, 0x00, (size_t)(x1 > x0 ? x1 - x0 : 0));
		}
	}

	[c->implicit_mask_tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
	                        mipmapLevel:0
	                          withBytes:c->implicit_mask_bytes
	                        bytesPerRow:w];

	memcpy(c->implicit_rects, rects, sizeof(rects[0]) * rect_count);
	c->implicit_rect_count = rect_count;

	U_LOG_I("Implicit zone mask rasterized: %ux%u, %u layer rect(s)", w, h, rect_count);
	return c->implicit_mask_tex;
}

/*!
 * XR_DXR_display_zones (ADR-027) — (re)rasterize the AUTO wish: union of the
 * frame's zone rects with an INWARD feathered edge: M=0 outside the zones;
 * inside each zone M ramps 0->1 over the first 16 px from the edge, so the
 * visual lerp fades zone content toward TRANSPARENT at the edge (never
 * toward the weave of empty atlas, which DPs may report opaque black). CPU
 * fill + replaceRegion like the implicit mask, with max-semantics across
 * zones (overlapping feathers never dim a zone core; small zones still
 * reach M=1 at the center because the inside-distance peaks there). NOTE:
 * the Metal CPU raster does a TRUE per-pixel distance feather —
 * clamp(dist_inside/16px) — rather than the GPU legs' stepped 8×2px inset
 * rings (D3D11 ClearView / D3D12 rect clears / VK vkCmdClearAttachments);
 * same 16-px footprint, smoother profile.
 * Re-rasterized only when the rect set or dims change. Returns the wish
 * texture or nil on failure.
 */
#define METAL_ZONE_WISH_FEATHER_PX 16
static id<MTLTexture>
metal_update_zone_wish_mask(struct comp_metal_compositor *c,
                            const struct xrt_rect *rects,
                            uint32_t rect_count,
                            uint32_t w,
                            uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return nil;
	}

	bool dirty = (c->wish_mask_tex == nil) || c->wish_mask_w != w || c->wish_mask_h != h ||
	             c->wish_rect_count != rect_count;
	for (uint32_t i = 0; !dirty && i < rect_count; i++) {
		if (memcmp(&c->wish_rects[i], &rects[i], sizeof(rects[i])) != 0) {
			dirty = true;
		}
	}
	if (!dirty) {
		return c->wish_mask_tex;
	}

	if (c->wish_mask_tex == nil || c->wish_mask_w != w || c->wish_mask_h != h) {
		[c->wish_mask_tex release];
		c->wish_mask_tex = nil;
		free(c->wish_mask_bytes);
		c->wish_mask_bytes = NULL;

		MTLTextureDescriptor *desc =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
		                                                       width:w
		                                                      height:h
		                                                   mipmapped:NO];
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		c->wish_mask_tex = [c->device newTextureWithDescriptor:desc];
		c->wish_mask_bytes = (uint8_t *)malloc((size_t)w * h);
		if (c->wish_mask_tex == nil || c->wish_mask_bytes == NULL) {
			U_LOG_E("Failed to allocate zone wish mask (%ux%u)", w, h);
			[c->wish_mask_tex release];
			c->wish_mask_tex = nil;
			free(c->wish_mask_bytes);
			c->wish_mask_bytes = NULL;
			return nil;
		}
		c->wish_mask_w = w;
		c->wish_mask_h = h;
	}

	// M=0 everywhere (no 3D wish), then per zone: an inside-distance feather
	// ramp from the rect edge to M=1 at >=16 px inside, folded in with max().
	memset(c->wish_mask_bytes, 0x00, (size_t)w * h);
	const float feather = (float)METAL_ZONE_WISH_FEATHER_PX;
	for (uint32_t i = 0; i < rect_count; i++) {
		int32_t rx0 = rects[i].offset.w;
		int32_t ry0 = rects[i].offset.h;
		int32_t rx1 = rx0 + rects[i].extent.w;
		int32_t ry1 = ry0 + rects[i].extent.h;
		if (rx1 <= rx0 || ry1 <= ry0) {
			continue;
		}
		int32_t ex0 = rx0 < 0 ? 0 : rx0;
		int32_t ey0 = ry0 < 0 ? 0 : ry0;
		int32_t ex1 = rx1 > (int32_t)w ? (int32_t)w : rx1;
		int32_t ey1 = ry1 > (int32_t)h ? (int32_t)h : ry1;
		for (int32_t y = ey0; y < ey1; y++) {
			// Distance from this row to the nearest horizontal edge,
			// measured INSIDE the rect (>= 1 on the innermost row).
			int32_t dy_in = (y - ry0 + 1) < (ry1 - y) ? (y - ry0 + 1) : (ry1 - y);
			uint8_t *row = c->wish_mask_bytes + (size_t)y * w;
			for (int32_t x = ex0; x < ex1; x++) {
				int32_t dx_in = (x - rx0 + 1) < (rx1 - x) ? (x - rx0 + 1) : (rx1 - x);
				int32_t d_in = dx_in < dy_in ? dx_in : dy_in;
				float fv = (float)d_in / feather;
				if (fv > 1.0f) {
					fv = 1.0f;
				}
				uint8_t v = (uint8_t)(fv * 255.0f + 0.5f);
				if (v > row[x]) {
					row[x] = v; // max across overlapping zones
				}
			}
		}
	}

	[c->wish_mask_tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
	                    mipmapLevel:0
	                      withBytes:c->wish_mask_bytes
	                    bytesPerRow:w];

	memcpy(c->wish_rects, rects, sizeof(rects[0]) * rect_count);
	c->wish_rect_count = rect_count;
	c->zone_publish_seq++; // #224 P4: new wish content generation for the DP publish

	U_LOG_W("Metal zone wish mask (auto): %ux%u, %u zone rect(s), %u-px feather", w, h, rect_count,
	        METAL_ZONE_WISH_FEATHER_PX);
	return c->wish_mask_tex;
}

/*!
 * Ensure the window-sized 2D scratch + weave snapshot textures exist at w×h.
 */
static bool
metal_ensure_composite_scratch(struct comp_metal_compositor *c, uint32_t w, uint32_t h)
{
	if (c->composite_w == w && c->composite_h == h && c->local2d_scratch != nil && c->weave_scratch != nil) {
		return true;
	}

	[c->local2d_scratch release];
	c->local2d_scratch = nil;
	[c->weave_scratch release];
	c->weave_scratch = nil;
	c->composite_w = 0;
	c->composite_h = 0;

	MTLTextureDescriptor *desc =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                       width:w
	                                                      height:h
	                                                   mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	c->local2d_scratch = [c->device newTextureWithDescriptor:desc];

	desc.usage = MTLTextureUsageShaderRead;
	c->weave_scratch = [c->device newTextureWithDescriptor:desc];

	if (c->local2d_scratch == nil || c->weave_scratch == nil) {
		U_LOG_E("Failed to allocate composite scratch textures (%ux%u)", w, h);
		[c->local2d_scratch release];
		c->local2d_scratch = nil;
		[c->weave_scratch release];
		c->weave_scratch = nil;
		return false;
	}
	c->composite_w = w;
	c->composite_h = h;
	U_LOG_I("Composite scratch allocated: %ux%u", w, h);
	return true;
}

// #491 part 3 — (re)allocate the 2D-under backdrop scratch (BGRA8, render-target
// + shader-read, premultiplied) at w×h. No-op when already matching.
static bool
metal_ensure_backdrop_scratch(struct comp_metal_compositor *c, uint32_t w, uint32_t h)
{
	if (c->backdrop_w == w && c->backdrop_h == h && c->backdrop_scratch != nil) {
		return true;
	}
	[c->backdrop_scratch release];
	c->backdrop_scratch = nil;
	c->backdrop_w = 0;
	c->backdrop_h = 0;

	MTLTextureDescriptor *desc =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                       width:w
	                                                      height:h
	                                                   mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	c->backdrop_scratch = [c->device newTextureWithDescriptor:desc];
	if (c->backdrop_scratch == nil) {
		U_LOG_E("Failed to allocate backdrop scratch texture (%ux%u)", w, h);
		return false;
	}
	c->backdrop_w = w;
	c->backdrop_h = h;
	return true;
}

// #439 Phase 3 / #491 part 3 — draw one Local2D layer into the currently-bound
// flatten render encoder (premultiplied / unpremultiplied "over", sRGB
// passthrough). Dest rect clips to the w×h window scratch; clipped fractions
// carry into the source UVs. Shared by the over-flatten (masked composite) and
// the under-flatten (backdrop).
static void
metal_flatten_one_local2d_layer(struct comp_metal_compositor *c,
                                id<MTLRenderCommandEncoder> enc,
                                struct comp_layer *layer,
                                uint32_t w,
                                uint32_t h)
{
	struct xrt_swapchain *sc = layer->sc_array[0];
	if (sc == NULL) {
		return;
	}
	struct comp_metal_swapchain *msc = metal_swapchain(sc);
	uint32_t img_idx = layer->data.local_2d.sub.image_index;
	if (img_idx >= msc->image_count || msc->images[img_idx] == nil) {
		return;
	}

	// Clip the dest rect to the window scratch; carry the clipped fractions
	// into the source UVs.
	const struct xrt_rect *dr = &layer->data.local_2d.rect;
	int32_t dx = dr->offset.w;
	int32_t dy = dr->offset.h;
	int32_t dw = dr->extent.w;
	int32_t dh = dr->extent.h;
	if (dw <= 0 || dh <= 0) {
		return;
	}
	int32_t x0 = dx < 0 ? 0 : dx;
	int32_t y0 = dy < 0 ? 0 : dy;
	int32_t x1 = (dx + dw) > (int32_t)w ? (int32_t)w : (dx + dw);
	int32_t y1 = (dy + dh) > (int32_t)h ? (int32_t)h : (dy + dh);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	float fx0 = (float)(x0 - dx) / (float)dw;
	float fy0 = (float)(y0 - dy) / (float)dh;
	float fx1 = (float)(x1 - dx) / (float)dw;
	float fy1 = (float)(y1 - dy) / (float)dh;

	struct xrt_normalized_rect nr = layer->data.local_2d.sub.norm_rect;
	if (nr.w <= 0.0f || nr.h <= 0.0f) {
		nr.x = 0.0f;
		nr.y = 0.0f;
		nr.w = 1.0f;
		nr.h = 1.0f;
	}

	// sRGB passthrough (see projection pass).
	id<MTLTexture> src_tex = msc->images[img_idx];
	id<MTLTexture> src_view = nil;
	{
		MTLPixelFormat unorm_fmt = metal_srgb_to_unorm(src_tex.pixelFormat);
		if (unorm_fmt != src_tex.pixelFormat) {
			src_view = [src_tex newTextureViewWithPixelFormat:unorm_fmt];
			if (src_view != nil) {
				src_tex = src_view;
			}
		}
	}

	MTLViewport vp;
	vp.originX = x0;
	vp.originY = y0;
	vp.width = x1 - x0;
	vp.height = y1 - y0;
	vp.znear = 0.0;
	vp.zfar = 1.0;
	[enc setViewport:vp];

	bool unpremult = (layer->data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0;
	[enc setRenderPipelineState:unpremult ? c->local2d_unpremult_pipeline : c->local2d_premult_pipeline];
	[enc setFragmentTexture:src_tex atIndex:0];
	[enc setFragmentSamplerState:c->sampler_linear atIndex:0];

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

	constants.src_rect[0] = nr.x + nr.w * fx0;
	constants.src_rect[2] = nr.w * (fx1 - fx0);
	if (layer->data.flip_y) {
		constants.src_rect[1] = nr.y + nr.h * (1.0f - fy0);
		constants.src_rect[3] = -(nr.h * (fy1 - fy0));
	} else {
		constants.src_rect[1] = nr.y + nr.h * fy0;
		constants.src_rect[3] = nr.h * (fy1 - fy0);
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

	[enc setVertexBytes:&constants length:sizeof(constants) atIndex:0];
	[enc setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

	[src_view release];
}

// #491 part 3 — flatten this frame's 2D-UNDER Local2D layers (before the
// projection in list order) into backdrop_scratch PRE-weave and return it (+
// region dims) so the caller hands it to the DP via set_background_2d (the DP
// composites `backdrop over captured-desktop` under the 3D). Returns nil (out
// dims 0) when there are no under-layers. The flatten is encoded on @p cmd_buf
// before the DP's process_atlas.
static id<MTLTexture>
metal_flatten_backdrop_2d(struct comp_metal_compositor *c,
                          id<MTLCommandBuffer> cmd_buf,
                          uint32_t dst_w,
                          uint32_t dst_h,
                          uint32_t *out_w,
                          uint32_t *out_h)
{
	*out_w = 0;
	*out_h = 0;
	if (!c->local_2d_last_frame || dst_w == 0 || dst_h == 0) {
		return nil;
	}

	// Under = Local2D layers BEFORE the projection. No projection ⟹ no backdrop.
	int32_t proj_idx = -1;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
		if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
			proj_idx = (int32_t)i;
			break;
		}
	}
	if (proj_idx < 0) {
		return nil;
	}
	bool have_under = false;
	for (int32_t i = 0; i < proj_idx; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			have_under = true;
			break;
		}
	}
	if (!have_under) {
		return nil;
	}

	uint32_t w = dst_w;
	uint32_t h = dst_h;
	if (!metal_ensure_backdrop_scratch(c, w, h)) {
		return nil;
	}

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = c->backdrop_scratch;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);

	id<MTLRenderCommandEncoder> enc = [cmd_buf renderCommandEncoderWithDescriptor:pass];
	for (int32_t i = 0; i < proj_idx; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		metal_flatten_one_local2d_layer(c, enc, layer, w, h);
	}
	[enc endEncoding];

	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("Metal #491 part3: flattened 2D-under backdrop %ux%u (handed to DP set_background_2d)", w, h);
	}

	*out_w = w;
	*out_h = h;
	return c->backdrop_scratch;
}

/*!
 * The Metal Local-2D consumer (#439 Phase 3, impl doc §4 steps 2/4/5):
 * resolve the frame's mask (explicit staged, else implicit from layer
 * rects), flatten the accumulated Local2D layers into the 2D scratch
 * (premultiplied over, list order), snapshot the weave, then mask-lerp
 * {2D, mask, weave} into the output's window rect.
 *
 * Returns true when the masked composite ran.
 */
static bool
metal_composite_local_2d(struct comp_metal_compositor *c,
                         id<MTLCommandBuffer> cmd_buf,
                         id<MTLTexture> output_texture,
                         const struct u_canvas_rect *eff,
                         bool have_local_2d)
{
	if (!eff->valid || eff->w == 0 || eff->h == 0 || output_texture == nil) {
		return false;
	}
	if (output_texture.pixelFormat != MTLPixelFormatBGRA8Unorm) {
		static bool warned_fmt = false;
		if (!warned_fmt) {
			warned_fmt = true;
			U_LOG_W("Masked composite skipped: output format %lu != BGRA8Unorm (one-time warning)",
			        (unsigned long)output_texture.pixelFormat);
		}
		return false;
	}
	if (output_texture.framebufferOnly) {
		// Runtime-owned-window drawables are framebufferOnly=YES — the
		// weave-snapshot blit can't read them. Handle/texture sessions
		// (external view / shared IOSurface) are the Phase-3 targets and
		// are readable; hosted is a follow-up.
		static bool warned_fbo = false;
		if (!warned_fbo) {
			warned_fbo = true;
			U_LOG_W("Masked composite skipped: output drawable is framebufferOnly (one-time warning)");
		}
		return false;
	}

	uint32_t w = eff->w;
	uint32_t h = eff->h;
	if (w > (uint32_t)output_texture.width) w = (uint32_t)output_texture.width;
	if (h > (uint32_t)output_texture.height) h = (uint32_t)output_texture.height;

	// #491 part 3 — split Local2D by list order vs the projection: layers BEFORE
	// the projection are the 2D-under backdrop (handed to the DP pre-weave) and
	// are excluded from the overlay mask + flatten here.
	int32_t proj_idx = -1;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
		if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
			proj_idx = (int32_t)i;
			break;
		}
	}

	// XR_DXR_display_zones: a zones frame ALWAYS runs the composite (the
	// feathered wish edge lerps the weave toward the 2D flatten even with
	// zero Local2D layers); the sticky mask + implicit-mask rules are inert.
	const bool zones_frame = c->zones_frame;

	// Step 2 — resolve the frame's mask. Zones frame: the WISH — the
	// explicit frame wish (referenced-at-frame-end: re-upload the CURRENT
	// authored bytes, mirroring zone_mask_submit's replaceRegion body, so
	// no xrSubmitLocal3DZoneDXR is required) or the auto feathered raster
	// from the zone rects.
	id<MTLTexture> mask_tex = nil;
	if (zones_frame && c->frame_wish != NULL && c->frame_wish->tex != nil &&
	    c->frame_wish->author_bytes != NULL) {
		struct comp_metal_zone_mask *fw = c->frame_wish;
		[fw->tex replaceRegion:MTLRegionMake2D(0, 0, fw->w, fw->h)
		           mipmapLevel:0
		             withBytes:fw->author_bytes
		           bytesPerRow:fw->w];
		mask_tex = fw->tex;

		// P4 publish source + seq: the explicit wish. Bump the generation
		// on a source change (pointer flip; Metal masks carry no author
		// generation, so a same-pointer re-author keeps its seq — vendors
		// treat same-seq as anchor-only updates).
		c->zone_publish_tex = fw->tex;
		c->zone_publish_w = fw->w;
		c->zone_publish_h = fw->h;
		if (c->zone_frame_wish_last != fw) {
			c->zone_frame_wish_last = fw;
			c->zone_publish_seq++;
		}
	} else if (zones_frame) {
		struct xrt_rect zone_rects[XRT_MAX_LAYERS];
		uint32_t zone_rect_count = 0;
		for (uint32_t i = 0; i < c->layer_accum.layer_count && zone_rect_count < XRT_MAX_LAYERS; i++) {
			if (c->layer_accum.layers[i].data.type != XRT_LAYER_ZONE_3D) {
				continue;
			}
			zone_rects[zone_rect_count++] = c->layer_accum.layers[i].data.zone_3d.rect;
		}
		mask_tex = metal_update_zone_wish_mask(c, zone_rects, zone_rect_count, w, h);
		if (mask_tex != nil) {
			// P4 publish source + seq: the auto raster (its dirty
			// re-raster path bumps the generation itself); a source
			// flip explicit -> auto is new content even when the rect
			// set is unchanged.
			if (c->zone_frame_wish_last != NULL) {
				c->zone_frame_wish_last = NULL;
				c->zone_publish_seq++;
			}
			c->zone_publish_tex = mask_tex;
			c->zone_publish_w = w;
			c->zone_publish_h = h;
		}
	} else if (c->zone_mask_active != NULL && c->zone_mask_active->submitted) {
		mask_tex = c->zone_mask_active->tex;
	} else if (have_local_2d) {
		struct xrt_rect rects[XRT_MAX_LAYERS];
		uint32_t rect_count = 0;
		for (uint32_t i = 0; i < c->layer_accum.layer_count && rect_count < XRT_MAX_LAYERS; i++) {
			if (c->layer_accum.layers[i].data.type != XRT_LAYER_LOCAL_2D) {
				continue;
			}
			if (proj_idx >= 0 && (int32_t)i < proj_idx) {
				continue; // under-layer (backdrop) — not part of the overlay mask
			}
			rects[rect_count++] = c->layer_accum.layers[i].data.local_2d.rect;
		}
		mask_tex = metal_update_implicit_mask(c, rects, rect_count, w, h);
	}
	if (mask_tex == nil) {
		return false;
	}

	if (!metal_ensure_composite_scratch(c, w, h)) {
		return false;
	}

	// Step 4 — fill the 2D scratch.
	{
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = c->local2d_scratch;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);

		id<MTLRenderCommandEncoder> enc = [cmd_buf renderCommandEncoderWithDescriptor:pass];

		// Flatten the OVER Local2D layers in layer-list (accumulation) order.
		// #491 part 3: under-layers (before the projection) are the DP backdrop
		// and are skipped here. XR_DXR_display_zones: zones frames have no
		// under/over split (2D-under reserved in v1) — every Local2D layer
		// flattens as 2D-over.
		for (uint32_t i = 0; have_local_2d && i < c->layer_accum.layer_count; i++) {
			struct comp_layer *layer = &c->layer_accum.layers[i];
			if (layer->data.type != XRT_LAYER_LOCAL_2D) {
				continue;
			}
			if (!zones_frame && proj_idx >= 0 && (int32_t)i < proj_idx) {
				continue; // under-layer (backdrop) — handled pre-weave
			}
			metal_flatten_one_local2d_layer(c, enc, layer, w, h);
		}

		[enc endEncoding];
	}

	// With no Local2D layers this frame the 2D scratch stays transparent —
	// where M=0 with no 2D coverage, final.a → 0 and the desktop shows
	// through (Q2).

	// Snapshot the weave (the DP just wrote output_texture).
	{
		id<MTLBlitCommandEncoder> blit = [cmd_buf blitCommandEncoder];
		[blit copyFromTexture:output_texture
		          sourceSlice:0
		          sourceLevel:0
		         sourceOrigin:MTLOriginMake(0, 0, 0)
		           sourceSize:MTLSizeMake(w, h, 1)
		            toTexture:c->weave_scratch
		     destinationSlice:0
		     destinationLevel:0
		    destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
	}

	// Step 5 — masked composite into the output's window rect. Pixels
	// beyond the window (worst-case shared surface band) stay untouched.
	{
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = output_texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;

		id<MTLRenderCommandEncoder> enc = [cmd_buf renderCommandEncoderWithDescriptor:pass];

		MTLViewport vp;
		vp.originX = 0;
		vp.originY = 0;
		vp.width = w;
		vp.height = h;
		vp.znear = 0.0;
		vp.zfar = 1.0;
		[enc setViewport:vp];

		// #491: the implicit (auto) Local2D mask composites the 2D over the
		// weave by its own premultiplied alpha (translucent 2D reveals the 3D
		// scene). The explicit authored mask keeps the hard M-lerp.
		// XR_DXR_display_zones: zones frames are ALWAYS the hard M-lerp
		// (final = M·weave + (1−M)·flatten(2D-over)) — composition follows
		// zone geometry + the wish, never the #491 alpha-over rule.
		const bool have_explicit =
		    !zones_frame && (c->zone_mask_active != NULL && c->zone_mask_active->submitted);
		uint alpha_over = (!zones_frame && have_local_2d && !have_explicit) ? 1u : 0u;

		[enc setRenderPipelineState:c->masked_composite_pipeline];
		[enc setFragmentTexture:c->local2d_scratch atIndex:0];
		[enc setFragmentTexture:mask_tex atIndex:1];
		[enc setFragmentTexture:c->weave_scratch atIndex:2];
		[enc setFragmentBytes:&alpha_over length:sizeof(alpha_over) atIndex:0];
		[enc setFragmentSamplerState:c->sampler_nearest atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

		[enc endEncoding];
	}

	return true;
}

// #224 / ADR-027 hardware-DP zone leg (P4) — one-time DP zone-capability
// probe, cached on the compositor. Returns true when the DP consumes
// published zone masks; caps are then in c->zone_dp_caps.
static bool
metal_zone_dp_supported(struct comp_metal_compositor *c)
{
	if (c->display_processor == NULL) {
		return false;
	}
	if (c->zone_dp_state == 0) { // 0 = unqueried, 1 = supported, 2 = legacy
		struct xrt_dp_local_zone_caps caps = {0};
		caps.struct_size = sizeof(caps);
		bool ok = xrt_display_processor_metal_get_local_zone_caps(c->display_processor, &caps);
		c->zone_dp_state = (ok && caps.supported != 0) ? 1 : 2;
		if (c->zone_dp_state == 1) {
			c->zone_dp_caps = caps;
			U_LOG_W("Metal zone DP: local zones supported, grid %ux%u max_mask %ux%u max_hz %u "
			        "wish_fractional=%u granularity=%u",
			        caps.zone_grid_width, caps.zone_grid_height, caps.max_mask_width,
			        caps.max_mask_height, caps.max_update_hz, caps.wish_fractional,
			        caps.switch_granularity);
		}
	}
	return c->zone_dp_state == 1;
}

// Keep the DP's view of this client's zone mask in sync with the
// compositor's — the Metal clone of d3d11_sync_zone_mask_to_dp (CODE-ONLY:
// needs a Mac eyeball). Called once per layer_commit after [cmd_buf commit];
// every publishable mask texture on this leg is CPU-uploaded (replaceRegion,
// Shared storage), so its content is valid at the call regardless of GPU
// progress. Zones frame: the WISH this frame's composite resolved; legacy
// frame: the sticky submitted mask. No resolvable source drives the
// clear-on-deactivate edge, once.
static void
metal_sync_zone_mask_to_dp(struct comp_metal_compositor *c)
{
	if (!metal_zone_dp_supported(c)) {
		return; // legacy DP — tier-1 global fallback path unchanged.
	}

	id<MTLTexture> tex = nil;
	uint32_t mask_w = 0;
	uint32_t mask_h = 0;
	if (c->zones_frame) {
		tex = c->zone_publish_tex;
		mask_w = c->zone_publish_w;
		mask_h = c->zone_publish_h;
	} else {
		struct comp_metal_zone_mask *mask = c->zone_mask_active;
		if (mask != NULL && mask->submitted && mask->tex != nil) {
			tex = mask->tex;
			mask_w = mask->w;
			mask_h = mask->h;
		}
	}

	if (tex == nil) {
		if (c->zone_published) {
			xrt_display_processor_metal_clear_local_zone_mask(c->display_processor);
			c->zone_published = false;
		}
		return;
	}

	// Screen-anchor the mask: content-view origin in physical (backing)
	// screen pixels, top-left convention. Cocoa screen coords are
	// bottom-left, so flip Y against the primary screen height.
	// @todo Mac eyeball — code-only, mirrors the backing-scale idiom used
	// for drawable sizing above; verify the anchor on a real panel.
	if (c->window == nil || c->window.contentView == nil) {
		return; // nothing to anchor to — skip the publish.
	}
	NSView *cv = c->window.contentView;
	NSRect content_win = [cv convertRect:cv.bounds toView:nil];
	NSRect content_scr = [c->window convertRectToScreen:content_win];
	CGFloat scale = c->window.backingScaleFactor > 0 ? c->window.backingScaleFactor : 1.0;
	CGFloat screen_h_pts = [[NSScreen mainScreen] frame].size.height;
	int32_t sx = (int32_t)llround(content_scr.origin.x * scale);
	int32_t sy = (int32_t)llround((screen_h_pts - (content_scr.origin.y + content_scr.size.height)) * scale);
	uint32_t sw = (uint32_t)llround(content_scr.size.width * scale);
	uint32_t sh = (uint32_t)llround(content_scr.size.height * scale);
	if (sw == 0 || sh == 0) {
		return;
	}

	bool ok = xrt_display_processor_metal_publish_local_zone_mask(c->display_processor, (__bridge void *)tex,
	                                                              mask_w, mask_h, sx, sy, sw, sh,
	                                                              c->zone_publish_seq);
	if (ok) {
		c->zone_published = true;
	}
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
			// One-shot diagnostic (displayxr-unity#204 bring-up): a permanently
			// nil drawable means presents never reach the WindowServer.
			static int s_nil_logged = 0;
			if (!s_nil_logged) {
				s_nil_logged = 1;
				U_LOG_W("layer_commit: nextDrawable nil (layer=%p device=%p size=%.0fx%.0f window=%p onscreen=%d)",
				        (void *)c->metal_layer, (void *)c->metal_layer.device,
				        c->metal_layer.drawableSize.width, c->metal_layer.drawableSize.height,
				        (void *)c->view.window, c->view.window != nil ? (int)[c->view.window isVisible] : -1);
			}
			return XRT_SUCCESS; // Non-fatal, skip this frame
		}
		static int s_first_drawable_logged = 0;
		if (!s_first_drawable_logged) {
			s_first_drawable_logged = 1;
			U_LOG_W("layer_commit: first drawable OK (size=%.0fx%.0f)",
			        c->metal_layer.drawableSize.width, c->metal_layer.drawableSize.height);
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

	// #439 Phase 3 — frame mask state. An explicit submitted mask, or any
	// Local2D layer this frame (which implies a Tier-2-style mask from its
	// rect, Q3), supersedes the canvas output rect: the weave spans the
	// client window and the mask is the sole 2D/3D selector (Phase-2 rule,
	// uniform — no third state).
	// XR_DXR_display_zones: the zones-frame flag is resolved in the same
	// scan (one coherent per-frame decision); zones frames count as
	// mask_active so metal_effective_canvas returns the full client window
	// (the output rect is inert).
	bool have_local_2d = false;
	bool zones_frame = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			have_local_2d = true;
		} else if (c->layer_accum.layers[i].data.type == XRT_LAYER_ZONE_3D) {
			zones_frame = true;
		}
	}
	const bool mask_active =
	    (c->zone_mask_active != NULL && c->zone_mask_active->submitted) || have_local_2d || zones_frame;
	c->local_2d_last_frame = have_local_2d;
	c->zones_frame = zones_frame;

	// XR_DXR_display_zones hardware leg (P4). Zone-capable DP: the per-frame
	// wish publish at the end of this commit drives the per-region switch —
	// skip the global fallback. Legacy DP (no zone slots): tier-1 fallback —
	// "any zone active => request 3D" once on the rising edge, no forced 2D
	// on the falling edge.
	if (zones_frame && !c->zones_mode_requested && !metal_zone_dp_supported(c)) {
		c->zones_mode_requested = true;
		comp_metal_compositor_request_display_mode(&c->base.base, true);
	} else if (!zones_frame) {
		c->zones_mode_requested = false;
	}

	// Reset this frame's resolved wish texture — metal_composite_local_2d
	// sets it in zones frames; a stale texture from an earlier frame must
	// never publish. (zone_publish_w/h persist harmlessly; the borrow is
	// gated on the texture.)
	c->zone_publish_tex = nil;

	struct u_canvas_rect eff_canvas = metal_effective_canvas(c, output_texture, mask_active);

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
				if (eff_canvas.valid) {
					// Effective canvas: the app's output rect, or the
					// full client window while a mask is active (#439).
					u_tiling_compute_canvas_view(mode, eff_canvas.w, eff_canvas.h,
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

	// CONTENT layout from the ACTIVE MODE (#542, ADR-028): decouple the atlas
	// tile count/size from the panel HARDWARE weave-state. The atlas geometry
	// is the active mode's (the submission clamps down to it below),
	// independent of c->hardware_display_3d — that flag drives only the DP
	// weave via request_display_mode. Defaults are the mode-derived values, so
	// zones frames and the no-projection-layer case stay byte-identical; the
	// main projection layer applies the mode clamp below.
	uint32_t atlas_view_w = c->view_width;
	uint32_t atlas_view_h = c->view_height;
	uint32_t atlas_cols = c->tile_columns;
	uint32_t atlas_rows = c->tile_rows;

	// Step 1: Render layers into atlas texture (skip if zero-copy)
	if (!zero_copy && c->atlas_texture != nil && c->layer_accum.layer_count > 0) {
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = c->atlas_texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		// Clear to (0,0,0,0) when the app requested transparent background so
		// projection-layer alpha=0 regions propagate through sim_display
		// alpha-native to the CAMetalLayer (isOpaque=NO) → desktop composite.
		// Otherwise clear to opaque black.
		// XR_DXR_display_zones (ADR-027): a zones frame composes N placed
		// zone layers into the window-spanning atlas — the unzoned area
		// must weave to nothing (transparent) so the feathered wish edge
		// blends toward the desktop.
		pass.colorAttachments[0].clearColor = (c->transparent_background || zones_frame)
		    ? MTLClearColorMake(0.0, 0.0, 0.0, 0.0)
		    : MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
		pass.depthAttachment.texture = c->depth_texture;
		pass.depthAttachment.loadAction = MTLLoadActionClear;
		pass.depthAttachment.storeAction = MTLStoreActionDontCare;
		pass.depthAttachment.clearDepth = 1.0;

		id<MTLRenderCommandEncoder> encoder = [cmd_buf renderCommandEncoderWithDescriptor:pass];

		// XR_DXR_display_zones: zone rects are client-window px and the
		// tile spans the full window in zones frames, so the zone scale
		// target is the effective canvas (= the window; mask_active
		// includes zones frames), falling back to the output dims.
		uint32_t zones_target_w = 0;
		uint32_t zones_target_h = 0;
		if (zones_frame) {
			if (eff_canvas.valid && eff_canvas.w > 0 && eff_canvas.h > 0) {
				zones_target_w = eff_canvas.w;
				zones_target_h = eff_canvas.h;
			} else if (output_texture != nil) {
				zones_target_w = (uint32_t)output_texture.width;
				zones_target_h = (uint32_t)output_texture.height;
			}
		}

		// Render each projection / zone layer (XR_DXR_display_zones: zone
		// layers blit through the same pass at a sub-tile viewport).
		for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
			struct comp_layer *layer = &c->layer_accum.layers[i];

			const bool is_zone = layer->data.type == XRT_LAYER_ZONE_3D;
			if (layer->data.type != XRT_LAYER_PROJECTION &&
			    layer->data.type != XRT_LAYER_PROJECTION_DEPTH && !is_zone) {
				continue;
			}

			// Verify app renders at the expected resolution (not stretched).
			// Zone layers are excluded — a zone renders at its own rect
			// size by design, not the view dims.
			if (!is_zone) {
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

			// CONTENT recipe is the ACTIVE MODE's (#542, ADR-028 §Decision 3):
			// the submission clamps DOWN to the mode, never the reverse. An
			// always-stereo app submits the xrLocateViews max view count even in
			// a mono mode (the documented compat guarantee), so without this
			// clamp a 2D mode would render a 2×1 strip instead of mono (the
			// left-shift bug class ADR-028 documents). The clamp also subsumes
			// the old legacy-app special case (a 2D mode ⇒ mono). The atlas is
			// independent of c->hardware_display_3d — that flag drives only the
			// DP weave via request_display_mode. Mirrors
			// comp_d3d11_renderer_compute_effective_layout.
			uint32_t view_count = layer->data.view_count;
			if (view_count == 0) view_count = 1;
			if (view_count > XRT_MAX_VIEWS) view_count = XRT_MAX_VIEWS;

			// Zone layers keep the mode-derived defaults (their rect is scaled
			// into the mode tile box below), so leave them alone. The mode grid
			// (c->tile_columns/rows) and per-view dims (c->view_width/height)
			// are synced from the active rendering mode every frame above.
			if (!is_zone) {
				uint32_t mode_cols = c->tile_columns > 0 ? c->tile_columns : 1;
				uint32_t mode_rows = c->tile_rows > 0 ? c->tile_rows : 1;
				uint32_t mode_tiles = mode_cols * mode_rows;
				if (view_count > mode_tiles) {
					view_count = mode_tiles;
				}
				if (view_count == 1) {
					// Mono: one tile spanning the full content region.
					atlas_cols = 1;
					atlas_rows = 1;
					atlas_view_w = mode_cols * c->view_width;
					atlas_view_h = mode_rows * c->view_height;
				} else {
					// The mode grid; an under-submitting app paints the
					// first view_count tiles.
					atlas_cols = mode_cols;
					atlas_rows = mode_rows;
					atlas_view_w = c->view_width;
					atlas_view_h = c->view_height;
				}
			}
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
				// Select the array slice (imageArrayIndex) AND apply sRGB->UNORM
				// passthrough in one view. Under single-pass-instanced the app
				// submits an arraySize>1 texture with per-view slice 0/1; a plain
				// 2D bind samples slice 0 for both eyes (flat output). A 2D view
				// pinned to the slice binds correctly to the texture2d<float>
				// shader — the Metal analog of the D3D12 #656 fix (mirrors
				// D3D11/GL/VK). The sRGB->UNORM sample avoids the GPU
				// auto-decoding sRGB->linear; the DP wants display-referred bytes
				// (no-op for UNORM).
				{
					MTLPixelFormat view_fmt = metal_srgb_to_unorm(src_tex.pixelFormat);
					if (src_tex.textureType == MTLTextureType2DArray) {
						uint32_t array_index = layer->data.proj.v[eye].sub.array_index;
						id<MTLTexture> v = [src_tex
						    newTextureViewWithPixelFormat:view_fmt
						                      textureType:MTLTextureType2D
						                           levels:NSMakeRange(0, src_tex.mipmapLevelCount)
						                           slices:NSMakeRange(array_index, 1)];
						if (v != nil) {
							src_tex = v;
						}
					} else if (view_fmt != src_tex.pixelFormat) {
						id<MTLTexture> v = [src_tex newTextureViewWithPixelFormat:view_fmt];
						if (v != nil) {
							src_tex = v;
						}
					}
				}

				// Use sub-image norm_rect to sample correct region of source texture
				struct xrt_normalized_rect nr = layer->data.proj.v[eye].sub.norm_rect;
				if (nr.w <= 0.0f || nr.h <= 0.0f) {
					nr.x = 0.0f;
					nr.y = 0.0f;
					nr.w = 1.0f;
					nr.h = 1.0f;
				}

				// Set viewport for this eye — tile-place by the mode-derived
				// grid (#542, ADR-028). No branch on the hardware flag: the DP
				// weaves (hardware-3D) or shows the tiles flat (hardware-2D)
				// per its own mode_3d from request_display_mode.
				MTLViewport vp;
				{
					uint32_t tile_x = eye % atlas_cols;
					uint32_t tile_y = eye / atlas_cols;
					vp.originX = tile_x * atlas_view_w;
					vp.originY = tile_y * atlas_view_h;
					vp.width = atlas_view_w;
					vp.height = atlas_view_h;
				}
				if (is_zone) {
					// XR_DXR_display_zones: scale the zone rect
					// (client-window px, top-left — same orientation
					// as Metal textures) into the tile box; in zones
					// frames the tile spans the full window, so
					// scale = tile/window. Zones draw through the
					// alpha-over pipeline variants below, so
					// overlapping zones composite in layer-list order
					// (D3D11 parity; the zones-frame atlas clear is
					// transparent, so a lone zone lands bit-identical
					// to the old no-blend write).
					if (zones_target_w == 0 || zones_target_h == 0) {
						continue;
					}
					const struct xrt_rect *zr = &layer->data.zone_3d.rect;
					const double zsx = vp.width / (double)zones_target_w;
					const double zsy = vp.height / (double)zones_target_h;
					vp.originX += (double)zr->offset.w * zsx;
					vp.originY += (double)zr->offset.h * zsy;
					vp.width = (double)zr->extent.w * zsx;
					vp.height = (double)zr->extent.h * zsy;
					if (vp.width <= 0.0 || vp.height <= 0.0) {
						continue;
					}
				}
				vp.znear = 0.0;
				vp.zfar = 1.0;
				[encoder setViewport:vp];

				// Set pipeline and textures. Zone draws blend alpha-over
				// (premultiplied unless the layer declares straight alpha)
				// with depth disabled — a later overlapping zone must
				// neither be depth-rejected nor overwrite (D3D11 parity).
				if (is_zone) {
					const bool unpremul =
					    (layer->data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0;
					[encoder setRenderPipelineState:(unpremul ? c->zone_unpremult_pipeline
					                                          : c->zone_premult_pipeline)];
					[encoder setDepthStencilState:c->depth_stencil_state_disabled];
				} else {
					[encoder setRenderPipelineState:c->projection_pipeline];
					[encoder setDepthStencilState:c->depth_stencil_state];
				}
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

		// Window-space layers (XR_DXR_win32_window_binding) — drawn into
		// each per-eye tile of the atlas with horizontal disparity shift,
		// so the display processor weaves them in stereo just like projection
		// content. Mirrors d3d11_compositor_layer_window_space + the renderer
		// window-space pass.
		// HUD tiles align with the projection tiles: same mode-derived grid
		// (#542, ADR-028), independent of the hardware weave-state.
		uint32_t mode_views_ws = atlas_cols;
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
			// sRGB passthrough (see projection pass): sample through a UNORM view.
			{
				MTLPixelFormat unorm_fmt = metal_srgb_to_unorm(src_tex.pixelFormat);
				if (unorm_fmt != src_tex.pixelFormat) {
					id<MTLTexture> v = [src_tex newTextureViewWithPixelFormat:unorm_fmt];
					if (v != nil) {
						src_tex = v;
					}
				}
			}

			// Source UV sub-rect (default to full texture if not specified)
			struct xrt_normalized_rect nr = ws->sub.norm_rect;
			if (nr.w <= 0.0f || nr.h <= 0.0f) {
				nr.x = 0.0f; nr.y = 0.0f; nr.w = 1.0f; nr.h = 1.0f;
			}

			for (uint32_t eye = 0; eye < mode_views_ws; eye++) {
				uint32_t tile_x = eye % atlas_cols;
				uint32_t tile_y = eye / atlas_cols;

				// Per-view disparity, graded across the view sweep (#413):
				// first view = -half, last = +half. Degenerates to the
				// classic -/+ pair for 2-view modes; one view = no shift.
				float half_disp = ws->disparity / 2.0f;
				float eye_shift = 0.0f;
				if (mode_views_ws > 1) {
					float t = (float)eye / (float)(mode_views_ws - 1);
					eye_shift = -half_disp + ws->disparity * t;
				}

				// Per-eye sub-rect within the atlas, in atlas pixels. The
				// projection_pipeline shader expects viewport=(0,0,1,1) and
				// emits a fullscreen triangle in NDC; the MTLViewport
				// clamps that to the sub-rect we want filled. This way we
				// reuse the existing shader without per-quad geometry.
				float tile_origin_x = (float)(tile_x * atlas_view_w);
				float tile_origin_y = (float)(tile_y * atlas_view_h);
				float tile_w = (float)atlas_view_w;
				float tile_h = (float)atlas_view_h;

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
		if (c->atlas_texture != nil && atlas_cols > 0 && atlas_rows > 0 &&
		    atlas_view_w > 0 && atlas_view_h > 0 && stat(trigger, &st) == 0) {
			unlink(trigger);
			// Mode-derived content region (#542, ADR-028): the active mode's.
			uint32_t cw = atlas_cols * atlas_view_w;
			uint32_t ch = atlas_rows * atlas_view_h;
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
		// Content is the active-mode layout (#542, ADR-028), orthogonal to the
		// hardware state — the DP weaves/flattens these tiles per its own
		// mode_3d (it picks weave vs blit from the tile grid: >1 ⇒ weave).
		uint32_t content_w = atlas_cols * atlas_view_w;
		uint32_t content_h = atlas_rows * atlas_view_h;

		// Verify content region fits within atlas
		if (content_w > c->atlas_width || content_h > c->atlas_height) {
			static int crop_err_log = 0;
			if (crop_err_log < 3) {
				U_LOG_W("CROP-BLIT OVERFLOW: content=%ux%u > atlas=%ux%u "
				        "(vw=%u vh=%u tiles=%ux%u legacy=%d)",
				        content_w, content_h,
				        c->atlas_width, c->atlas_height,
				        atlas_view_w, atlas_view_h,
				        atlas_cols, atlas_rows,
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

		// #491 part 3 — flatten the 2D-under layers PRE-weave and hand them to
		// the DP (it composites `backdrop over captured-desktop` under the 3D).
		// nil ⟹ no under-layers (DP clears its backdrop). Encoded on cmd_buf
		// before process_atlas. (Code-only on Windows — macOS CI gates the
		// compile; visual needs a Mac+Leia eyeball.)
		{
			uint32_t bd_w = 0, bd_h = 0;
			id<MTLTexture> bd_tex =
			    metal_flatten_backdrop_2d(c, cmd_buf, dp_target_w, dp_target_h, &bd_w, &bd_h);
			xrt_display_processor_metal_set_background_2d(c->display_processor,
			                                              (__bridge void *)bd_tex, bd_w, bd_h);
		}

		// Effective canvas (#439): while a mask is active this is the
		// full client-window rect — the DP weaves every pixel the mask
		// can select (Phase-2 rule).
		xrt_display_processor_metal_process_atlas(
		    c->display_processor,
		    (__bridge void *)cmd_buf,
		    (__bridge void *)dp_src,
		    atlas_view_w,
		    atlas_view_h,
		    atlas_cols,
		    atlas_rows,
		    (uint32_t)MTLPixelFormatBGRA8Unorm,
		    (__bridge void *)output_texture,
		    dp_target_w,
		    dp_target_h,
		    eff_canvas.valid ? eff_canvas.x : 0,
		    eff_canvas.valid ? eff_canvas.y : 0,
		    eff_canvas.valid ? eff_canvas.w : 0,
		    eff_canvas.valid ? eff_canvas.h : 0);
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

	// #439 Phase 3 — masked 2D/3D composite. When a mask is active
	// (explicit submitted mask, or implicit from this frame's Local2D
	// layer rects) the post-weave output is mask-lerped against the
	// flattened Local2D layers. Layers/mask are the 2D authority.
	if (mask_active) {
		metal_composite_local_2d(c, cmd_buf, output_texture, &eff_canvas, have_local_2d);
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

	// #224 / ADR-027 P4: sideband-sync this client's zone state with the DP
	// — in zones frames this publishes the WISH the composite resolved
	// (CPU-uploaded textures, content valid independent of GPU progress);
	// in legacy frames the sticky submitted mask; the clear edge otherwise.
	metal_sync_zone_mask_to_dp(c);

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

	// 4. Destroy display processor — first withdrawing this client's zone
	// contribution from the vendor's union (#224 P4 clear-on-teardown edge).
	if (c->display_processor != NULL) {
		if (c->zone_published) {
			xrt_display_processor_metal_clear_local_zone_mask(c->display_processor);
			c->zone_published = false;
		}
		xrt_display_processor_metal_destroy(&c->display_processor);
	}

	// 4. Release shared texture resources (MRR — explicit release)
	[c->shared_texture release];
	c->shared_texture = nil;
	if (c->shared_iosurface != NULL) {
		CFRelease(c->shared_iosurface);
		c->shared_iosurface = NULL;
	}

	// 4b. Release #439 zone-mask consumer resources. The active explicit
	// mask itself is owned by its oxr handle (destroyed via
	// zone_mask_destroy before the session compositor goes away) — only
	// drop the borrow here.
	c->zone_mask_active = NULL;
	// XR_DXR_display_zones: drop the frame-wish borrow + auto-wish raster
	// (+ the P4 publish-source borrow).
	c->frame_wish = NULL;
	c->zone_frame_wish_last = NULL;
	c->zone_publish_tex = nil;
	c->zones_frame = false;
	[c->wish_mask_tex release];
	c->wish_mask_tex = nil;
	free(c->wish_mask_bytes);
	c->wish_mask_bytes = NULL;
	[c->implicit_mask_tex release];
	c->implicit_mask_tex = nil;
	free(c->implicit_mask_bytes);
	c->implicit_mask_bytes = NULL;
	[c->local2d_scratch release];
	c->local2d_scratch = nil;
	[c->weave_scratch release];
	c->weave_scratch = nil;
	[c->backdrop_scratch release]; // #491 part 3
	c->backdrop_scratch = nil;
	[c->local2d_premult_pipeline release];
	c->local2d_premult_pipeline = nil;
	[c->local2d_unpremult_pipeline release];
	c->local2d_unpremult_pipeline = nil;
	[c->masked_composite_pipeline release];
	c->masked_composite_pipeline = nil;
	[c->sampler_nearest release];
	c->sampler_nearest = nil;

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
	[c->zone_premult_pipeline release];
	c->zone_premult_pipeline = nil;
	[c->zone_unpremult_pipeline release];
	c->zone_unpremult_pipeline = nil;
	[c->blit_pipeline release];
	c->blit_pipeline = nil;
	[c->sampler_linear release];
	c->sampler_linear = nil;
	[c->depth_stencil_state release];
	c->depth_stencil_state = nil;
	[c->depth_stencil_state_disabled release];
	c->depth_stencil_state_disabled = nil;

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

/*
 *
 * XR_DXR_local_3d_zone — zone-mask entry points (#439 Phase 3)
 *
 * Tier 1/2 author into the CPU-canonical buffer; submit uploads to the
 * R8Unorm texture (sticky, last-submit-wins). All entry points take the
 * compositor mutex (cheap).
 *
 */

xrt_result_t
comp_metal_compositor_zone_mask_create(struct xrt_compositor *xc, uint32_t w, uint32_t h, void **out_mask)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	if (out_mask == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	os_mutex_lock(&c->mutex);

	// 0 lets the compositor choose: the client-window backing size.
	if (w == 0 || h == 0) {
		uint32_t win_w = 0;
		uint32_t win_h = 0;
		if (!metal_window_backing_dims(c, c->shared_texture, &win_w, &win_h)) {
			os_mutex_unlock(&c->mutex);
			U_LOG_E("zone_mask_create: no window dims available for auto-size");
			return XRT_ERROR_ALLOCATION;
		}
		w = win_w;
		h = win_h;
	}

	struct comp_metal_zone_mask *mask = U_TYPED_CALLOC(struct comp_metal_zone_mask);
	if (mask == NULL) {
		os_mutex_unlock(&c->mutex);
		return XRT_ERROR_ALLOCATION;
	}

	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
	                                                                                width:w
	                                                                               height:h
	                                                                            mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeShared;
	mask->tex = [c->device newTextureWithDescriptor:desc];
	mask->author_bytes = (uint8_t *)malloc((size_t)w * h);
	if (mask->tex == nil || mask->author_bytes == NULL) {
		U_LOG_E("zone_mask_create: allocation failed (%ux%u)", w, h);
		[mask->tex release];
		free(mask->author_bytes);
		free(mask);
		os_mutex_unlock(&c->mutex);
		return XRT_ERROR_ALLOCATION;
	}
	mask->w = w;
	mask->h = h;
	// Default to all-3D — submit-without-author == whole-window 3D.
	memset(mask->author_bytes, 0xFF, (size_t)w * h);

	os_mutex_unlock(&c->mutex);

	U_LOG_W("Zone mask created: %ux%u (Metal, Tier 1/2)", w, h);
	*out_mask = mask;
	return XRT_SUCCESS;
}

xrt_result_t
comp_metal_compositor_zone_mask_set_whole(struct xrt_compositor *xc, void *mask_ptr, bool enable_3d)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	struct comp_metal_zone_mask *mask = (struct comp_metal_zone_mask *)mask_ptr;
	if (mask == NULL || mask->author_bytes == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	os_mutex_lock(&c->mutex);
	memset(mask->author_bytes, enable_3d ? 0xFF : 0x00, (size_t)mask->w * mask->h);
	os_mutex_unlock(&c->mutex);
	return XRT_SUCCESS;
}

xrt_result_t
comp_metal_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                          void *mask_ptr,
                                          uint32_t count,
                                          const struct xrt_rect *rects)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	struct comp_metal_zone_mask *mask = (struct comp_metal_zone_mask *)mask_ptr;
	if (mask == NULL || mask->author_bytes == NULL || (count > 0 && rects == NULL)) {
		return XRT_ERROR_ALLOCATION;
	}

	os_mutex_lock(&c->mutex);

	// M=0 everywhere, then M=1 inside each rect (client-window px, clamped).
	memset(mask->author_bytes, 0x00, (size_t)mask->w * mask->h);
	for (uint32_t i = 0; i < count; i++) {
		int32_t x0 = rects[i].offset.w;
		int32_t y0 = rects[i].offset.h;
		int32_t x1 = x0 + rects[i].extent.w;
		int32_t y1 = y0 + rects[i].extent.h;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > (int32_t)mask->w) x1 = (int32_t)mask->w;
		if (y1 > (int32_t)mask->h) y1 = (int32_t)mask->h;
		if (x1 <= x0 || y1 <= y0) {
			continue;
		}
		for (int32_t y = y0; y < y1; y++) {
			memset(mask->author_bytes + (size_t)y * mask->w + x0, 0xFF, (size_t)(x1 - x0));
		}
	}

	os_mutex_unlock(&c->mutex);
	return XRT_SUCCESS;
}

xrt_result_t
comp_metal_compositor_zone_mask_acquire_rt(
    struct xrt_compositor *xc, void *mask_ptr, void **out_rt, uint32_t *out_w, uint32_t *out_h)
{
	// No Metal Tier-3 binding type in XR_DXR_local_3d_zone v3 — oxr maps
	// this to XR_ERROR_FEATURE_UNSUPPORTED.
	(void)xc;
	(void)mask_ptr;
	(void)out_rt;
	(void)out_w;
	(void)out_h;
	return XRT_ERROR_NOT_IMPLEMENTED;
}

xrt_result_t
comp_metal_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	struct comp_metal_zone_mask *mask = (struct comp_metal_zone_mask *)mask_ptr;
	if (mask == NULL || mask->tex == nil || mask->author_bytes == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	os_mutex_lock(&c->mutex);
	[mask->tex replaceRegion:MTLRegionMake2D(0, 0, mask->w, mask->h)
	             mipmapLevel:0
	               withBytes:mask->author_bytes
	             bytesPerRow:mask->w];
	mask->submitted = true;
	c->zone_mask_active = mask;
	c->zone_publish_seq++; // #224 P4: new content generation for the DP publish
	os_mutex_unlock(&c->mutex);

	U_LOG_I("Zone mask submitted: %ux%u (Metal)", mask->w, mask->h);
	return XRT_SUCCESS;
}

void
comp_metal_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	struct comp_metal_zone_mask *mask = (struct comp_metal_zone_mask *)mask_ptr;
	if (mask == NULL) {
		return;
	}

	os_mutex_lock(&c->mutex);
	if (c->zone_mask_active == mask) {
		// Reverts to the rect-derived behavior on the next frame.
		c->zone_mask_active = NULL;
	}
	// XR_DXR_display_zones: never leave a dangling frame-wish reference.
	if (c->frame_wish == mask) {
		c->frame_wish = NULL;
	}
	// #224 P4: drop the seq-dedup cache (pointer may be reused by a future
	// alloc) and any per-frame publish source borrowed from this mask.
	if (c->zone_frame_wish_last == mask) {
		c->zone_frame_wish_last = NULL;
	}
	if (c->zone_publish_tex == mask->tex) {
		c->zone_publish_tex = nil;
	}
	os_mutex_unlock(&c->mutex);

	[mask->tex release];
	free(mask->author_bytes);
	free(mask);
}

void
comp_metal_compositor_zones_set_frame_wish(struct xrt_compositor *xc, void *mask)
{
	struct comp_metal_compositor *c = metal_comp(xc);

	// Per-frame reference (XR_DXR_display_zones): oxr sets this on every
	// zones frame before layer_commit, NULL meaning auto-derive. Consumed
	// by the commit's composite; harmlessly stale on zero-zone frames (the
	// zones branch never reads it there).
	os_mutex_lock(&c->mutex);
	c->frame_wish = (struct comp_metal_zone_mask *)mask;
	os_mutex_unlock(&c->mutex);
}

bool
comp_metal_compositor_zone_get_hw_caps(struct xrt_compositor *xc, uint32_t *out_grid_w, uint32_t *out_grid_h)
{
	// sim_display has no switchable-lens zone grid — compositor consumer only.
	(void)xc;
	if (out_grid_w != NULL) {
		*out_grid_w = 0;
	}
	if (out_grid_h != NULL) {
		*out_grid_h = 0;
	}
	return false;
}

bool
comp_metal_compositor_get_recommended_view_size(struct xrt_compositor *xc, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	if (out_w == NULL || out_h == NULL || c->view_width == 0 || c->view_height == 0) {
		return false;
	}
	*out_w = c->view_width;
	*out_h = c->view_height;
	return true;
}

xrt_result_t
comp_metal_compositor_create(struct xrt_device *xdev,
                             void *window_handle,
                             void *command_queue_ptr,
                             void *dp_factory_metal,
                             bool offscreen,
                             void *shared_iosurface,
                             bool transparent_background,
                             struct xrt_compositor_native **out_xc)
{
	U_LOG_W("comp_metal_compositor_create: window_handle=%p, offscreen=%d, shared_iosurface=%p, dp_factory=%p, transparent_background=%d",
	        window_handle, offscreen, shared_iosurface, dp_factory_metal, (int)transparent_background);

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
	c->transparent_background = transparent_background;

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
		if (!setup_external_window(c, external_view, transparent_background)) {
			os_mutex_destroy(&c->mutex);
			free(c);
			return XRT_ERROR_VULKAN;
		}
	} else if (shared_iosurface == NULL) {
		// Only create a window when there's no shared IOSurface.
		// With IOSurface, we render headless — no window needed.
		if (!create_window(c, display_width, display_height, transparent_background)) {
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
	xc->layer_local_2d = metal_compositor_layer_local_2d;
	xc->layer_zone_3d = metal_compositor_layer_zone_3d;
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

	// Compute window metrics compositor-side whenever we have a backing
	// NSView — our own window (hosted) OR the app's external view (handle /
	// texture). The metrics view is c->view in both cases (create_window
	// sets c->view = the content view; setup_external_window sets it to the
	// app's view). Before #396-W7 the apps did their own Kooima, so the
	// external-view path never needed this; the dogfood (runtime owns view
	// generation) exposed that the Metal sim/Leia DP doesn't implement
	// get_window_metrics, so the external-view session fell through to the
	// DP delegate (which returns false) and the runtime ran display-scoped —
	// stretching the scene to the display aspect (vertical squish on a
	// window whose aspect differs from the display). Mirrors the d3d12 fix
	// (d34bf0a57) and the d3d11/gl/vk_native compositor-side construction.
	NSView *metrics_view = c->view;
	if (metrics_view != nil && c->sys_info != NULL) {
		float disp_w_m = c->sys_info->display_width_m;
		float disp_h_m = c->sys_info->display_height_m;
		uint32_t disp_px_w = c->sys_info->display_pixel_width;
		uint32_t disp_px_h = c->sys_info->display_pixel_height;
		if (disp_px_w == 0 || disp_px_h == 0 || disp_w_m <= 0 || disp_h_m <= 0) {
			return false;
		}

		NSRect backing = [metrics_view convertRectToBacking:metrics_view.bounds];
		uint32_t win_px_w = (uint32_t)backing.size.width;
		uint32_t win_px_h = (uint32_t)backing.size.height;
		if (win_px_w == 0 || win_px_h == 0) {
			return false;
		}

		float pixel_size_x = disp_w_m / (float)disp_px_w;
		float pixel_size_y = disp_h_m / (float)disp_px_h;

		// Window centre offset within the display. Use the real on-screen
		// position when the view's window + screen are available (so
		// window-relative 3D tracks the window); fall back to centred (the
		// runtime-owned window is centred anyway).
		float win_center_px_x = (float)win_px_w / 2.0f;
		float win_center_px_y = (float)win_px_h / 2.0f;
		float disp_center_px_x = (float)disp_px_w / 2.0f;
		float disp_center_px_y = (float)disp_px_h / 2.0f;
		int32_t win_screen_left = 0;
		int32_t win_screen_top = 0;
		float offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
		float offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

		NSWindow *ns_win = metrics_view.window;
		NSScreen *screen = ns_win.screen ?: [NSScreen mainScreen];
		if (ns_win != nil && screen != nil) {
			// View bounds → screen points (AppKit bottom-up) → top-down
			// backing px relative to the screen origin.
			NSRect view_in_win = [metrics_view convertRect:metrics_view.bounds toView:nil];
			NSRect view_in_screen = [ns_win convertRectToScreen:view_in_win];
			NSRect screen_frame = [screen frame];
			CGFloat bs = [screen backingScaleFactor];
			float win_left_px = (float)((view_in_screen.origin.x - screen_frame.origin.x) * bs);
			// Flip Y to top-down: distance from the screen top to the view top.
			float view_top_pts = (screen_frame.origin.y + screen_frame.size.height) -
			                     (view_in_screen.origin.y + view_in_screen.size.height);
			float win_top_px = (float)(view_top_pts * bs);

			win_screen_left = (int32_t)win_left_px;
			win_screen_top = (int32_t)win_top_px;
			float wc_x = win_left_px + win_center_px_x;
			float wc_y = win_top_px + win_center_px_y;
			offset_x_m = (wc_x - disp_center_px_x) * pixel_size_x;
			offset_y_m = -((wc_y - disp_center_px_y) * pixel_size_y);
		}

		out_metrics->display_width_m = disp_w_m;
		out_metrics->display_height_m = disp_h_m;
		out_metrics->display_pixel_width = disp_px_w;
		out_metrics->display_pixel_height = disp_px_h;
		out_metrics->display_screen_left = 0;
		out_metrics->display_screen_top = 0;

		out_metrics->window_pixel_width = win_px_w;
		out_metrics->window_pixel_height = win_px_h;
		out_metrics->window_screen_left = win_screen_left;
		out_metrics->window_screen_top = win_screen_top;

		out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
		out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

		out_metrics->window_center_offset_x_m = offset_x_m;
		out_metrics->window_center_offset_y_m = offset_y_m;

		out_metrics->valid = true;
		return true;
	}

	// Fallback: delegate to display processor (ext/shared path)
	if (c->display_processor != NULL) {
		bool ok = xrt_display_processor_metal_get_window_metrics(c->display_processor, out_metrics);
		return ok;
	}

	return false;
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
comp_metal_compositor_set_eye_tracking_mode(struct xrt_compositor *xc, uint32_t mode)
{
	struct comp_metal_compositor *c = metal_comp(xc);
	if (c->display_processor == NULL) {
		return;
	}
	// Delegate to display processor (a no-op for sim_display, which has no tracking)
	xrt_display_processor_metal_set_eye_tracking_mode(c->display_processor, mode);
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
