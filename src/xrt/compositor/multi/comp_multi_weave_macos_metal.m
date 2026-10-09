// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave on macOS (#759) — the native Metal backend.
 * @author David Fattal
 * @ingroup comp_multi
 *
 * The Metal twin of comp_multi_weave_macos_vk.c, for a plug-in whose only
 * macOS display processor is a Metal one (the Leia SR plug-in exports
 * create_dp_metal and no create_dp_vk). Same engine shape, same contract:
 *
 *  - Device: ONE MTLCreateSystemDefaultDevice per service (lazy, process
 *    lifetime) + one MTLCommandQueue per client. Both outlive the DP, which
 *    may hold a vendor weaver created on them.
 *  - DP: dp_factory_metal(device, queue, NULL window) — windowless; its phase
 *    comes only from set_present_origin, fed before every weave from the
 *    front end's resolved panel-relative origin.
 *  - Input / overlay: the caller's IOSurfaces wrapped as BGRA8 MTLTextures
 *    (newTextureWithDescriptor:iosurface:plane:), cached by IOSurfaceID in the
 *    front end.
 *  - Output: a service-allocated global BGRA8 IOSurface wrapped as a
 *    RenderTarget|ShaderRead texture and exported directly.
 *  - v3 batch: the same window-sized 2W x H SBS scratch as the vk backend.
 *    Each rect's halves are un-squeezed by a tiny MSL pass (fullscreen
 *    triangle, viewport = the destination tile, fragment coordinate clamped
 *    to the half-rect's own outermost texel centres — the vk backend's
 *    staging-copy trick, done in the shader). v5 firstChunk = a Clear(0,0,0,0)
 *    load action on that pass. ONE process_atlas over the 2x1 scratch.
 *  - v6 N-view: the input directly when the packed region fills it, else one
 *    blit into a packed-size crop texture; ONE process_atlas at the layout's
 *    grid. When the DP's output must not be resampled (no
 *    XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE) the front end sizes the output to
 *    the window, so the lattice reaches the panel 1:1.
 *  - Alpha: a DP that is not alpha-native gets a post-weave alpha-only pass
 *    (out.a = max over the views' alpha at that pixel) so a present-owner's
 *    alpha-based draw-back keeps working (cleared v5 gaps stay transparent).
 *  - v4: a second pass blends the premultiplied overlay over the output
 *    (One / OneMinusSrcAlpha).
 *  - Sync: [cmd commit] + [cmd waitUntilCompleted] before the IPC reply.
 *
 * Shaders are compiled at runtime from a source string (no build-time
 * metallib), like sim_display_processor_metal.m. Manual retain/release (the
 * runtime's .m files are not ARC). Everything runs on the client's IPC thread
 * under mc->weave.mutex — creation, every weave and teardown (the SR weaver's
 * single-thread rule).
 */

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "xrt/xrt_display_processor_metal.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_misc.h"
#include "util/u_logging.h"

#include "comp_multi_private.h"
#include "comp_multi_weave_macos_backend.h"

#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdlib.h>


/*
 *
 * State.
 *
 */

struct weave_metal
{
	bool initialized;
	bool failed; //!< One-shot: don't re-run a hopeless bring-up every submit.

	id<MTLCommandQueue> queue;
	id<MTLRenderPipelineState> unsqueeze_pso; //!< SBS half -> scratch tile (no blend).
	id<MTLRenderPipelineState> alpha_pso;     //!< Post-weave alpha reconstruction (alpha-only write).
	id<MTLRenderPipelineState> over_pso;      //!< v4 premul "over" (One / OneMinusSrcAlpha).

	id<MTLTexture> in_tex;
	id<MTLTexture> overlay_tex;
	id<MTLTexture> out_tex;
	id<MTLTexture> sbs_tex; //!< 2*out_w x out_h, private.
	id<MTLTexture> crop_tex;
	uint32_t crop_w, crop_h;

	bool alpha_native;
	bool tolerates_resample;
	bool has_present_origin_slot;
};

static inline struct weave_metal *
wm_of(struct multi_compositor *mc)
{
	return (struct weave_metal *)mc->weave.metal;
}

//! One device per service process, created on first use, never released.
static id<MTLDevice>
weave_metal_device(void)
{
	static id<MTLDevice> device = nil;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		device = MTLCreateSystemDefaultDevice(); // +1, process lifetime
	});
	return device;
}

static const char *weave_metal_shader_src =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    // Fullscreen triangle; uv (0,0) = top-left of the viewport, y down.
    "vertex VOut weave_fs_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
    "  VOut o;\n"
    "  o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "  o.uv = p;\n"
    "  return o;\n"
    "}\n"
    // Un-squeeze one SBS half into its tile: stretch the half-rect over the
    // viewport, clamped to the half-rect's own outermost texel centres so
    // nothing outside it (the other eye, the caller's never-cleared ring)
    // bleeds in.
    "struct UnsqueezeParams { float2 src_origin; float2 src_size; };\n"
    "fragment float4 weave_unsqueeze_fs(VOut in [[stage_in]],\n"
    "                                   texture2d<float> src [[texture(0)]],\n"
    "                                   constant UnsqueezeParams &p [[buffer(0)]]) {\n"
    "  constexpr sampler s(coord::pixel, filter::linear, address::clamp_to_edge);\n"
    "  float2 c = p.src_origin + in.uv * p.src_size;\n"
    "  c = clamp(c, p.src_origin + 0.5, p.src_origin + p.src_size - 0.5);\n"
    "  return src.sample(s, c);\n"
    "}\n"
    // Post-weave alpha for a DP that is not alpha-native: max over the views'
    // alpha at the same view-relative position (alpha-only colour write).
    "struct AlphaParams { uint view_w; uint view_h; uint cols; uint view_count; };\n"
    "fragment float4 weave_alpha_fs(VOut in [[stage_in]],\n"
    "                               texture2d<float> atlas [[texture(0)]],\n"
    "                               constant AlphaParams &p [[buffer(0)]]) {\n"
    "  uint vx = min(uint(in.uv.x * float(p.view_w)), p.view_w - 1);\n"
    "  uint vy = min(uint(in.uv.y * float(p.view_h)), p.view_h - 1);\n"
    "  float a = 0.0;\n"
    "  for (uint t = 0; t < p.view_count; t++) {\n"
    "    uint2 c = uint2((t % p.cols) * p.view_w + vx, (t / p.cols) * p.view_h + vy);\n"
    "    a = max(a, atlas.read(c).a);\n"
    "  }\n"
    "  return float4(0.0, 0.0, 0.0, a);\n"
    "}\n"
    // v4 overlay: premultiplied atlas sampled over the whole output.
    "fragment float4 weave_over_fs(VOut in [[stage_in]], texture2d<float> ov [[texture(0)]]) {\n"
    "  constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);\n"
    "  return ov.sample(s, in.uv);\n"
    "}\n";

struct weave_unsqueeze_params
{
	float src_origin[2];
	float src_size[2];
};

struct weave_alpha_params
{
	uint32_t view_w, view_h, cols, view_count;
};

static id<MTLRenderPipelineState>
weave_metal_make_pso(id<MTLDevice> dev,
                     id<MTLLibrary> lib,
                     NSString *frag_name,
                     bool premul_over,
                     MTLColorWriteMask write_mask)
{
	id<MTLFunction> vs = [lib newFunctionWithName:@"weave_fs_vs"];
	id<MTLFunction> fs = [lib newFunctionWithName:frag_name];
	if (vs == nil || fs == nil) {
		[vs release];
		[fs release];
		return nil;
	}
	MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
	desc.vertexFunction = vs;
	desc.fragmentFunction = fs;
	desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	desc.colorAttachments[0].writeMask = write_mask;
	if (premul_over) {
		desc.colorAttachments[0].blendingEnabled = YES;
		desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
		desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
		desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
		desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	}
	NSError *err = nil;
	id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:desc error:&err];
	if (pso == nil) {
		U_LOG_E("weave(#759) metal: pipeline '%s' failed: %s", [frag_name UTF8String],
		        err != nil ? [[err localizedDescription] UTF8String] : "?");
	}
	[desc release];
	[vs release];
	[fs release];
	return pso;
}

static void
weave_metal_release_output(struct multi_compositor *mc, struct weave_metal *wm)
{
	[wm->out_tex release];
	wm->out_tex = nil;
	[wm->sbs_tex release];
	wm->sbs_tex = nil;
	if (mc->weave.out_iosurface != NULL) {
		CFRelease((IOSurfaceRef)mc->weave.out_iosurface);
		mc->weave.out_iosurface = NULL;
	}
	mc->weave.out_w = 0;
	mc->weave.out_h = 0;
}

//! Global BGRA IOSurface (kIOSurfaceIsGlobal: the IPC carries the bare ID and
//! the caller's IOSurfaceLookup must find it).
static IOSurfaceRef
weave_metal_create_iosurface(uint32_t w, uint32_t h)
{
	int32_t iw = (int32_t)w, ih = (int32_t)h, bpe = 4;
	uint32_t fmt = 'BGRA';
	CFStringRef keys[5] = {CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"), CFSTR("IOSurfaceBytesPerElement"),
	                       CFSTR("IOSurfacePixelFormat"), CFSTR("IOSurfaceIsGlobal")};
	CFNumberRef nums[4] = {
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &iw),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &ih),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt),
	};
	CFTypeRef values[5] = {nums[0], nums[1], nums[2], nums[3], kCFBooleanTrue};
	CFDictionaryRef props = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 5,
	                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef surf = IOSurfaceCreate(props);
	CFRelease(props);
	for (int i = 0; i < 4; i++) {
		CFRelease(nums[i]);
	}
	return surf;
}

//! BGRA8 texture over an IOSurface (+1), or nil.
static id<MTLTexture>
weave_metal_wrap_iosurface(IOSurfaceRef surface, MTLTextureUsage usage)
{
	const NSUInteger w = IOSurfaceGetWidth(surface);
	const NSUInteger h = IOSurfaceGetHeight(surface);
	if (w == 0 || h == 0) {
		return nil;
	}
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                                                width:w
	                                                                               height:h
	                                                                            mipmapped:NO];
	desc.usage = usage;
	return [weave_metal_device() newTextureWithDescriptor:desc iosurface:surface plane:0];
}

//! Private (GPU-only) BGRA8 texture (+1), or nil.
static id<MTLTexture>
weave_metal_private_texture(uint32_t w, uint32_t h, MTLTextureUsage usage)
{
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                                                width:w
	                                                                               height:h
	                                                                            mipmapped:NO];
	desc.usage = usage;
	desc.storageMode = MTLStorageModePrivate;
	return [weave_metal_device() newTextureWithDescriptor:desc];
}


/*
 *
 * Backend hooks.
 *
 */

static bool
mtlb_ensure_engine(struct multi_compositor *mc)
{
	struct weave_metal *wm = wm_of(mc);
	if (wm != NULL && wm->initialized) {
		return true;
	}
	if (wm != NULL && wm->failed) {
		return false;
	}
	if (wm == NULL) {
		wm = calloc(1, sizeof(*wm));
		if (wm == NULL) {
			return false;
		}
		mc->weave.metal = wm;
	}
	wm->failed = true; // cleared on success below

	@autoreleasepool {
		id<MTLDevice> dev = weave_metal_device();
		if (dev == nil) {
			U_LOG_E("weave(#759) metal: MTLCreateSystemDefaultDevice failed — cannot weave");
			return false;
		}
		wm->queue = [dev newCommandQueue];
		if (wm->queue == nil) {
			U_LOG_E("weave(#759) metal: newCommandQueue failed");
			return false;
		}

		NSError *err = nil;
		NSString *src = [NSString stringWithUTF8String:weave_metal_shader_src];
		id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
		if (lib == nil) {
			U_LOG_E("weave(#759) metal: shader compile failed: %s",
			        err != nil ? [[err localizedDescription] UTF8String] : "?");
			return false;
		}
		wm->unsqueeze_pso = weave_metal_make_pso(dev, lib, @"weave_unsqueeze_fs", false, MTLColorWriteMaskAll);
		wm->alpha_pso = weave_metal_make_pso(dev, lib, @"weave_alpha_fs", false, MTLColorWriteMaskAlpha);
		wm->over_pso = weave_metal_make_pso(dev, lib, @"weave_over_fs", true, MTLColorWriteMaskAll);
		[lib release];
		if (wm->unsqueeze_pso == nil || wm->alpha_pso == nil || wm->over_pso == nil) {
			return false;
		}

		xrt_dp_factory_metal_fn_t factory = (xrt_dp_factory_metal_fn_t)mc->msc->base.info.dp_factory_metal;
		if (factory == NULL) {
			U_LOG_E("weave(#759) metal: the plug-in has no Metal DP factory — cannot weave");
			return false;
		}
		// Windowless: the present-owner owns its NSWindow; phase comes from
		// set_present_origin.
		xrt_result_t xret = factory((void *)dev, (void *)wm->queue, NULL, &mc->weave.dp_metal);
		if (xret != XRT_SUCCESS || mc->weave.dp_metal == NULL) {
			U_LOG_E("weave(#759) metal: Metal DP factory failed: %d", xret);
			mc->weave.dp_metal = NULL;
			return false;
		}

		struct xrt_display_processor_metal *dp = mc->weave.dp_metal;
		wm->alpha_native = xrt_display_processor_metal_is_alpha_native(dp);
		wm->tolerates_resample = xrt_display_processor_metal_tolerates_resample(dp);
		wm->has_present_origin_slot = XRT_DP_HAS_SLOT(dp, set_present_origin) && dp->set_present_origin != NULL;

		wm->failed = false;
		wm->initialized = true;
		U_LOG_W(
		    "weave(#759): macOS weave engine initialized — backend=metal on '%s' (BGRA8, synchronous; DP "
		    "alpha-native=%s, tolerates-resample=%s, set_present_origin=%s)",
		    [[dev name] UTF8String], wm->alpha_native ? "yes" : "no (post-weave alpha pass)",
		    wm->tolerates_resample ? "yes" : "no (v6 output sized to the window)",
		    wm->has_present_origin_slot ? "yes" : "no (display-scoped phase)");
	}
	return true;
}

static bool
mtlb_import_input(struct multi_compositor *mc, IOSurfaceRef surface)
{
	struct weave_metal *wm = wm_of(mc);
	@autoreleasepool {
		wm->in_tex = weave_metal_wrap_iosurface(surface, MTLTextureUsageShaderRead);
	}
	if (wm->in_tex == nil) {
		U_LOG_E("weave(#759) metal: could not wrap the input IOSurface (%zux%zu) as a BGRA8 texture",
		        IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface));
		return false;
	}
	mc->weave.in_w = (uint32_t)IOSurfaceGetWidth(surface);
	mc->weave.in_h = (uint32_t)IOSurfaceGetHeight(surface);
	return true;
}

static void
mtlb_release_input(struct multi_compositor *mc)
{
	struct weave_metal *wm = wm_of(mc);
	if (wm != NULL) {
		[wm->in_tex release];
		wm->in_tex = nil;
	}
	mc->weave.in_w = 0;
	mc->weave.in_h = 0;
}

static bool
mtlb_import_overlay(struct multi_compositor *mc, IOSurfaceRef surface)
{
	struct weave_metal *wm = wm_of(mc);
	@autoreleasepool {
		wm->overlay_tex = weave_metal_wrap_iosurface(surface, MTLTextureUsageShaderRead);
	}
	if (wm->overlay_tex == nil) {
		U_LOG_E("weave(#759) metal v4: could not wrap the overlay IOSurface as a BGRA8 texture");
		return false;
	}
	mc->weave.overlay_w = (uint32_t)IOSurfaceGetWidth(surface);
	mc->weave.overlay_h = (uint32_t)IOSurfaceGetHeight(surface);
	return true;
}

static void
mtlb_release_overlay(struct multi_compositor *mc)
{
	struct weave_metal *wm = wm_of(mc);
	if (wm != NULL) {
		[wm->overlay_tex release];
		wm->overlay_tex = nil;
	}
	mc->weave.overlay_w = 0;
	mc->weave.overlay_h = 0;
}


static bool
mtlb_output_tolerates_resample(struct multi_compositor *mc)
{
	struct weave_metal *wm = wm_of(mc);
	return wm != NULL && wm->tolerates_resample;
}

static bool
mtlb_ensure_output(struct multi_compositor *mc, uint32_t want_w, uint32_t want_h, bool nview)
{
	struct weave_metal *wm = wm_of(mc);
	const bool resize = wm->out_tex == nil || mc->weave.out_w != want_w || mc->weave.out_h != want_h;
	if (resize) {
		// Submits are synchronous: nothing of ours is in flight.
		weave_metal_release_output(mc, wm);
		IOSurfaceRef surf = weave_metal_create_iosurface(want_w, want_h);
		if (surf == NULL) {
			U_LOG_E("weave(#759) metal: IOSurfaceCreate(%ux%u) for the output failed", want_w, want_h);
			return false;
		}
		@autoreleasepool {
			const MTLTextureUsage usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			wm->out_tex = weave_metal_wrap_iosurface(surf, usage);
		}
		if (wm->out_tex == nil) {
			U_LOG_E("weave(#759) metal: could not wrap the output IOSurface (%ux%u)", want_w, want_h);
			CFRelease(surf);
			return false;
		}
		mc->weave.out_iosurface = (void *)surf; // the +1 from IOSurfaceCreate
		mc->weave.out_w = want_w;
		mc->weave.out_h = want_h;
	}
	// The 2W x H SBS scratch (non-v6 paths). Created on demand, so a client
	// that switches from v6 to batch at the same size still gets one.
	if (!nview && wm->sbs_tex == nil) {
		@autoreleasepool {
			const MTLTextureUsage usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			wm->sbs_tex = weave_metal_private_texture(want_w * 2, want_h, usage);
		}
		if (wm->sbs_tex == nil) {
			U_LOG_E("weave(#759) metal: SBS scratch (%ux%u) allocation failed", want_w * 2, want_h);
			return false;
		}
	}
	return true;
}

//! v3 / legacy: un-squeeze every rect's SBS halves into the scratch tiles.
static void
weave_metal_encode_unsqueeze(struct multi_compositor *mc,
                             struct weave_metal *wm,
                             id<MTLCommandBuffer> cmd,
                             const struct comp_multi_weave_macos_params *p)
{
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = wm->sbs_tex;
	// v5 firstChunk (browser#22): clear to premultiplied transparent so regions
	// BETWEEN the woven tiles come out alpha 0; otherwise the scratch persists
	// across submits (stale regions re-weave harmlessly; the caller composites
	// back only its current rects).
	rp.colorAttachments[0].loadAction = p->first_chunk ? MTLLoadActionClear : MTLLoadActionLoad;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;

	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
	[enc setRenderPipelineState:wm->unsqueeze_pso];
	[enc setFragmentTexture:wm->in_tex atIndex:0];

	struct xrt_rect legacy_rect = {
	    .offset = {.w = 0, .h = 0},
	    .extent = {.w = (int)p->want_w, .h = (int)p->want_h},
	};
	const struct xrt_rect *rects = p->rect_count > 0 ? p->rects : &legacy_rect;
	uint32_t count = p->rect_count > 0 ? p->rect_count : 1;
	if (count > COMP_MULTI_WEAVE_EYE_STAGE_SLOTS) {
		count = COMP_MULTI_WEAVE_EYE_STAGE_SLOTS; // The IPC server already rejects more.
	}

	for (uint32_t i = 0; i < count; i++) {
		// xrt_offset names its fields w/h; they hold x/y here.
		const int32_t rx = rects[i].offset.w;
		const int32_t ry = rects[i].offset.h;
		const int32_t rw = rects[i].extent.w;
		const int32_t rh = rects[i].extent.h;
		if (rw <= 0 || rh <= 0) {
			continue;
		}
		if (rx < 0 || ry < 0 || (uint32_t)(rx + rw) > mc->weave.in_w || (uint32_t)(ry + rh) > mc->weave.in_h) {
			continue;
		}
		const int32_t half = rw / 2;
		if (half <= 0) {
			continue;
		}
		// Left half -> left tile at the rect's window position (stretched to the
		// full rect width); right half -> right tile (+out_w). Odd widths keep
		// the vk backend's split: left = rw / 2, right = the rest.
		const int32_t src_x[2] = {rx, rx + half};
		const int32_t src_w[2] = {half, rw - half};
		const int32_t tile_x[2] = {0, (int32_t)mc->weave.out_w};
		for (uint32_t e = 0; e < 2; e++) {
			MTLViewport vp = {(double)(tile_x[e] + rx), (double)ry, (double)rw, (double)rh, 0.0, 1.0};
			[enc setViewport:vp];
			struct weave_unsqueeze_params up = {
			    .src_origin = {(float)src_x[e], (float)ry},
			    .src_size = {(float)src_w[e], (float)rh},
			};
			[enc setFragmentBytes:&up length:sizeof(up) atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		}
	}
	[enc endEncoding];
}

static bool
mtlb_record_and_wait(struct multi_compositor *mc, const struct comp_multi_weave_macos_params *p)
{
	struct weave_metal *wm = wm_of(mc);
	struct xrt_display_processor_metal *dp = mc->weave.dp_metal;
	if (wm == NULL || dp == NULL || wm->in_tex == nil || wm->out_tex == nil) {
		return false;
	}

	bool ok = false;
	@autoreleasepool {
		id<MTLCommandBuffer> cmd = [wm->queue commandBuffer];
		if (cmd == nil) {
			U_LOG_E("weave(#759) metal: commandBuffer failed");
			return false;
		}

		// Atlas the DP samples + the grid it describes. Batch: the window-sized
		// 2x1 SBS scratch. v6 (#774): the caller's packed atlas — the input
		// itself, or a packed-size crop of its top-left region (ADR-030).
		id<MTLTexture> atlas = nil;
		uint32_t view_w = 0, view_h = 0, cols = 2, rows = 1, view_count = 2;
		if (!p->nview) {
			if (wm->sbs_tex == nil) {
				return false;
			}
			weave_metal_encode_unsqueeze(mc, wm, cmd, p);
			atlas = wm->sbs_tex;
			view_w = mc->weave.out_w;
			view_h = mc->weave.out_h;
		} else {
			if (p->v6_zero_copy) {
				atlas = wm->in_tex;
			} else {
				if (wm->crop_tex == nil || wm->crop_w != p->packed_w || wm->crop_h != p->packed_h) {
					[wm->crop_tex release];
					wm->crop_tex = weave_metal_private_texture(p->packed_w, p->packed_h,
					                                           MTLTextureUsageShaderRead);
					wm->crop_w = wm->crop_tex != nil ? p->packed_w : 0;
					wm->crop_h = wm->crop_tex != nil ? p->packed_h : 0;
					if (wm->crop_tex == nil) {
						U_LOG_E("weave(#759) metal v6: crop texture (%ux%u) allocation failed",
						        p->packed_w, p->packed_h);
						return false;
					}
				}
				// Tiles are contiguous: the packed region is ONE rectangle.
				id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
				[blit copyFromTexture:wm->in_tex
				             sourceSlice:0
				             sourceLevel:0
				            sourceOrigin:MTLOriginMake(0, 0, 0)
				              sourceSize:MTLSizeMake(p->packed_w, p->packed_h, 1)
				               toTexture:wm->crop_tex
				        destinationSlice:0
				        destinationLevel:0
				       destinationOrigin:MTLOriginMake(0, 0, 0)];
				[blit endEncoding];
				atlas = wm->crop_tex;
			}
			view_w = p->cvw;
			view_h = p->cvh;
			cols = p->tile_columns;
			rows = p->tile_rows;
			view_count = p->view_count;
		}

		// Phase: re-asserted every submit so a moved window keeps its lattice
		// (the helper is a no-op for a DP without the slot).
		if (p->have_present_origin) {
			xrt_display_processor_metal_set_present_origin(dp, p->present_origin_x, p->present_origin_y);
		}

		// ONE process_atlas per submit; the DP encodes its own pass into the
		// target. Target = the whole output, no canvas sub-rect.
		xrt_display_processor_metal_process_atlas(dp, (void *)cmd, (void *)atlas, view_w, view_h, cols, rows,
		                                          (uint32_t)MTLPixelFormatBGRA8Unorm, (void *)wm->out_tex,
		                                          mc->weave.out_w, mc->weave.out_h, 0, 0, 0, 0);

		// A DP that is not alpha-native writes opaque pixels; rebuild the alpha
		// from the views so cleared gaps stay transparent for the caller's
		// whole-window draw-back.
		if (!wm->alpha_native) {
			MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
			rp.colorAttachments[0].texture = wm->out_tex;
			rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
			rp.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
			[enc setRenderPipelineState:wm->alpha_pso];
			[enc setFragmentTexture:atlas atIndex:0];
			struct weave_alpha_params ap = {
			    .view_w = view_w,
			    .view_h = view_h,
			    .cols = cols > 0 ? cols : 1,
			    .view_count = view_count < cols * rows ? view_count : cols * rows,
			};
			[enc setFragmentBytes:&ap length:sizeof(ap) atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
		}

		// v4 overlay (browser#18): composite the premultiplied 2D atlas OVER the
		// woven output — not woven, so crisp 2D lands at screen depth. Same rule
		// as the vk backend: a cached overlay is composited every submit.
		if (wm->overlay_tex != nil) {
			MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
			rp.colorAttachments[0].texture = wm->out_tex;
			rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
			rp.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
			[enc setRenderPipelineState:wm->over_pso];
			[enc setFragmentTexture:wm->overlay_tex atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
		}

		// Synchronous completion (the macOS contract).
		[cmd commit];
		[cmd waitUntilCompleted];
		if ([cmd status] != MTLCommandBufferStatusCompleted) {
			NSError *err = [cmd error];
			U_LOG_E("weave(#759) metal: command buffer failed (status %d): %s", (int)[cmd status],
			        err != nil ? [[err localizedDescription] UTF8String] : "?");
		} else {
			ok = true;
		}
	}
	return ok;
}

static bool
mtlb_get_eyes(struct multi_compositor *mc, struct xrt_eye_positions *out_eyes)
{
	return mc->weave.dp_metal != NULL &&
	       xrt_display_processor_metal_get_predicted_eye_positions(mc->weave.dp_metal, out_eyes);
}

static bool
mtlb_request_display_mode(struct multi_compositor *mc, bool enable_3d, bool *out_has_slot)
{
	struct xrt_display_processor_metal *dp = mc->weave.dp_metal;
	*out_has_slot = dp != NULL && XRT_DP_HAS_SLOT(dp, request_display_mode) && dp->request_display_mode != NULL;
	return *out_has_slot ? xrt_display_processor_metal_request_display_mode(dp, enable_3d) : true;
}

static bool
mtlb_get_hardware_3d_state(struct multi_compositor *mc, bool *out_is_3d)
{
	return mc->weave.dp_metal != NULL &&
	       xrt_display_processor_metal_get_hardware_3d_state(mc->weave.dp_metal, out_is_3d);
}

static void
mtlb_fini(struct multi_compositor *mc)
{
	struct weave_metal *wm = wm_of(mc);
	// The DP first: a vendor DP destroys its weaver (created on our device +
	// queue) before its own SDK instance, and both must go before the queue.
	if (mc->weave.dp_metal != NULL) {
		xrt_display_processor_metal_destroy(&mc->weave.dp_metal);
	}
	if (wm == NULL) {
		return;
	}
	weave_metal_release_output(mc, wm);
	[wm->in_tex release];
	[wm->overlay_tex release];
	[wm->crop_tex release];
	[wm->unsqueeze_pso release];
	[wm->alpha_pso release];
	[wm->over_pso release];
	[wm->queue release];
	free(wm);
	mc->weave.metal = NULL;
	mc->weave.in_w = mc->weave.in_h = 0;
	mc->weave.overlay_w = mc->weave.overlay_h = 0;
}

const struct comp_multi_weave_macos_backend comp_multi_weave_macos_backend_metal = {
    .name = "metal",
    .feeds_present_origin = true,
    .ensure_engine = mtlb_ensure_engine,
    .import_input = mtlb_import_input,
    .release_input = mtlb_release_input,
    .import_overlay = mtlb_import_overlay,
    .release_overlay = mtlb_release_overlay,
    .output_tolerates_resample = mtlb_output_tolerates_resample,
    .ensure_output = mtlb_ensure_output,
    .record_and_wait = mtlb_record_and_wait,
    .get_eyes = mtlb_get_eyes,
    .request_display_mode = mtlb_request_display_mode,
    .get_hardware_3d_state = mtlb_get_hardware_3d_state,
    .fini = mtlb_fini,
};
