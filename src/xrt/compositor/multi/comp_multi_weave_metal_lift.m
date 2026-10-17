// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lifted weave rects recorded in Metal; see comp_multi_weave_metal_lift.h.
 * @ingroup comp_multi
 *
 * Manual retain/release (the runtime's .m files are not ARC). Shaders are
 * compiled at runtime from a source string, like the Metal weave backend's.
 */

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "comp_multi_weave_metal_lift.h"

#include "os/os_time.h"
#include "util/u_lift_mailbox.h"
#include "util/u_logging.h"

#include <IOSurface/IOSurface.h>

#include <stdlib.h>
#include <string.h>


/*
 *
 * State.
 *
 */

//! Snapshot chain depth: the final image plus halving steps (vk_conv_weave's VKCW_CHAIN_MAX).
#define MLIFT_CHAIN_MAX 6u

//! Frames without use after which a stream's Metal objects go (vk_conv_weave's VKCW_IDLE_COMPLETES).
#define MLIFT_IDLE_FRAMES 600u

//! Pipelines per colour-attachment format (the RGBA8 chain + the engine's destination).
#define MLIFT_PSO_MAX 4u

struct mlift_stream
{
	struct mlift_stream *next;
	uint64_t owner, wire;
	uint32_t idle;
	bool used_this_frame;

	id<MTLTexture> chain[MLIFT_CHAIN_MAX];
	id<MTLBuffer> snap_buf;
	id<MTLBuffer> lines_buf;
	bool logged_export;
};

//! One planned rect of the frame being recorded.
struct mlift_entry
{
	struct mlift_stream *st;
	const struct vk_conv_weave_rect *r; //!< the caller's, valid during record only
	struct vk_conv_weave_host_plan plan;
	bool snap_recorded;
	bool lines_recorded;
	bool woven;
	id<MTLTexture> result_tex; //!< +1, released at frame_done
};

struct comp_multi_weave_metal_lift
{
	id<MTLDevice> device; //!< borrowed
	id<MTLLibrary> lib;
	id<MTLFunction> vs, fs;
	MTLPixelFormat pso_fmt[MLIFT_PSO_MAX];
	id<MTLRenderPipelineState> pso[MLIFT_PSO_MAX];
	uint32_t pso_count;

	struct mlift_stream *streams;
	struct mlift_entry entries[XRT_LIFT_WEAVE_RECTS_MAX];
	uint32_t entry_count;
};

static const char *mlift_shader_src =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    // Fullscreen triangle; uv (0,0) = top-left of the viewport, y down.
    "vertex VOut mlift_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
    "  VOut o;\n"
    "  o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "  o.uv = p;\n"
    "  return o;\n"
    "}\n"
    // Stretch the source region over the viewport. Linear, clamped to the
    // region's own outermost texel centres (nothing outside it bleeds in: the
    // neighbouring view, the rest of the frame); nearest = exact texels (1:1).
    "struct StretchParams { float2 src_origin; float2 src_size; uint nearest; };\n"
    "fragment float4 mlift_fs(VOut in [[stage_in]],\n"
    "                         texture2d<float> src [[texture(0)]],\n"
    "                         constant StretchParams &p [[buffer(0)]]) {\n"
    "  float2 c = p.src_origin + in.uv * p.src_size;\n"
    "  if (p.nearest != 0) {\n"
    "    float2 q = clamp(floor(c), p.src_origin, p.src_origin + p.src_size - 1.0);\n"
    "    return src.read(uint2(q));\n"
    "  }\n"
    "  constexpr sampler s(coord::pixel, filter::linear, address::clamp_to_edge);\n"
    "  c = clamp(c, p.src_origin + 0.5, p.src_origin + p.src_size - 0.5);\n"
    "  return src.sample(s, c);\n"
    "}\n";

struct mlift_stretch_params
{
	float src_origin[2];
	float src_size[2];
	uint32_t nearest;
};


/*
 *
 * Helpers.
 *
 */

static id<MTLRenderPipelineState>
mlift_pso(struct comp_multi_weave_metal_lift *ml, MTLPixelFormat fmt)
{
	for (uint32_t i = 0; i < ml->pso_count; i++) {
		if (ml->pso_fmt[i] == fmt) {
			return ml->pso[i];
		}
	}
	if (ml->pso_count >= MLIFT_PSO_MAX) {
		return nil;
	}
	MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
	desc.vertexFunction = ml->vs;
	desc.fragmentFunction = ml->fs;
	desc.colorAttachments[0].pixelFormat = fmt;
	NSError *err = nil;
	id<MTLRenderPipelineState> pso = [ml->device newRenderPipelineStateWithDescriptor:desc error:&err];
	[desc release];
	if (pso == nil) {
		U_LOG_E("lift rect (metal): pipeline for pixel format %d failed: %s", (int)fmt,
		        err != nil ? [[err localizedDescription] UTF8String] : "?");
		return nil;
	}
	ml->pso_fmt[ml->pso_count] = fmt;
	ml->pso[ml->pso_count] = pso;
	ml->pso_count++;
	return pso;
}

static void
mlift_stream_free(struct mlift_stream *st)
{
	for (uint32_t i = 0; i < MLIFT_CHAIN_MAX; i++) {
		[st->chain[i] release];
	}
	[st->snap_buf release];
	[st->lines_buf release];
	free(st);
}

static struct mlift_stream *
mlift_stream_get(struct comp_multi_weave_metal_lift *ml, uint64_t owner, uint64_t wire)
{
	struct mlift_stream *st = ml->streams;
	while (st != NULL && (st->wire != wire || st->owner != owner)) {
		st = st->next;
	}
	if (st == NULL) {
		st = calloc(1, sizeof(*st));
		if (st == NULL) {
			return NULL;
		}
		st->owner = owner;
		st->wire = wire;
		st->next = ml->streams;
		ml->streams = st;
	}
	return st;
}

//! A private RGBA8 render target of @p w x @p h in @p slot (kept while the size holds).
static bool
mlift_tex_ensure(struct comp_multi_weave_metal_lift *ml, id<MTLTexture> *slot, uint32_t w, uint32_t h)
{
	if (*slot != nil && [*slot width] == w && [*slot height] == h) {
		return true;
	}
	[*slot release];
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                                             width:w
	                                                                            height:h
	                                                                         mipmapped:NO];
	d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModePrivate;
	*slot = [ml->device newTextureWithDescriptor:d];
	return *slot != nil;
}

//! A shared (CPU-visible) buffer of at least @p size bytes in @p slot.
static bool
mlift_buf_ensure(struct comp_multi_weave_metal_lift *ml, id<MTLBuffer> *slot, size_t size)
{
	if (*slot != nil && [*slot length] >= size) {
		return true;
	}
	[*slot release];
	*slot = [ml->device newBufferWithLength:size options:MTLResourceStorageModeShared];
	return *slot != nil;
}

//! One stretch draw: src (sx, sy, sw, sh) over the viewport (dx, dy, dw, dh) of the pass's target.
static void
mlift_draw(id<MTLRenderCommandEncoder> enc,
           id<MTLTexture> src,
           double sx,
           double sy,
           double sw,
           double sh,
           double dx,
           double dy,
           double dw,
           double dh,
           bool nearest)
{
	if (sw <= 0.0 || sh <= 0.0 || dw <= 0.0 || dh <= 0.0) {
		return;
	}
	MTLViewport vp = {dx, dy, dw, dh, 0.0, 1.0};
	[enc setViewport:vp];
	struct mlift_stretch_params p = {
	    .src_origin = {(float)sx, (float)sy},
	    .src_size = {(float)sw, (float)sh},
	    .nearest = nearest ? 1u : 0u,
	};
	[enc setFragmentTexture:src atIndex:0];
	[enc setFragmentBytes:&p length:sizeof(p) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

static void
mlift_tile_origin(const struct vk_conv_weave_rect *r, uint32_t v, int32_t *out_x, int32_t *out_y)
{
	const uint32_t cols = r->tile_columns > 0 ? r->tile_columns : 1;
	*out_x = (int32_t)((v % cols) * r->tile_w);
	*out_y = (int32_t)((v / cols) * r->tile_h);
}

//! The result's IOSurface as a UNORM texture of the conversion format (+1), or nil.
static id<MTLTexture>
mlift_wrap_result(struct comp_multi_weave_metal_lift *ml, const struct vk_conv_weave_host_plan *pl)
{
	MTLPixelFormat fmt;
	switch (pl->result_format) {
	case XRT_CONV_FMT_RGBA8_UNORM:
	case XRT_CONV_FMT_RGBA8_SRGB: fmt = MTLPixelFormatRGBA8Unorm; break;
	case XRT_CONV_FMT_BGRA8_UNORM:
	case XRT_CONV_FMT_BGRA8_SRGB: fmt = MTLPixelFormatBGRA8Unorm; break;
	default: return nil;
	}
	IOSurfaceRef surf = (IOSurfaceRef)pl->result_iosurface;
	if (surf == NULL || pl->result_w == 0 || pl->result_h == 0 || IOSurfaceGetWidth(surf) < pl->result_w ||
	    IOSurfaceGetHeight(surf) < pl->result_h) {
		return nil;
	}
	// The format comes from the plan, NOT IOSurfaceGetPixelFormat (0 on a
	// surface the Vulkan driver created).
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
	                                                                             width:pl->result_w
	                                                                            height:pl->result_h
	                                                                         mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead;
	return [ml->device newTextureWithDescriptor:d iosurface:surf plane:0];
}

//! Release the frame's per-frame objects (result textures) and forget its plans.
static void
mlift_frame_reset(struct comp_multi_weave_metal_lift *ml)
{
	for (uint32_t i = 0; i < ml->entry_count; i++) {
		[ml->entries[i].result_tex release];
	}
	memset(ml->entries, 0, sizeof(ml->entries));
	ml->entry_count = 0;
}


/*
 *
 * Recording.
 *
 */

//! The snapshot chain of @p e into its RGBA8 textures (one render pass per level).
static bool
mlift_encode_chain(struct comp_multi_weave_metal_lift *ml,
                   id<MTLCommandBuffer> cmd,
                   id<MTLTexture> src,
                   struct mlift_entry *e,
                   id<MTLRenderPipelineState> pso,
                   uint32_t *out_last)
{
	const struct vk_conv_weave_host_plan *p = &e->plan;
	struct mlift_stream *st = e->st;
	// vk_conv_weave's chain: halving steps while more than 2x away, then the final one.
	uint32_t lw[MLIFT_CHAIN_MAX], lh[MLIFT_CHAIN_MAX];
	uint32_t levels = 0;
	uint32_t cur_w = p->cw, cur_h = p->ch;
	while (levels + 1 < MLIFT_CHAIN_MAX && (cur_w > 2 * p->dw || cur_h > 2 * p->dh)) {
		cur_w = (cur_w + 1) / 2 > p->dw ? (cur_w + 1) / 2 : p->dw;
		cur_h = (cur_h + 1) / 2 > p->dh ? (cur_h + 1) / 2 : p->dh;
		lw[levels] = cur_w;
		lh[levels] = cur_h;
		levels++;
	}
	lw[levels] = p->dw;
	lh[levels] = p->dh;
	levels++;
	for (uint32_t i = 0; i < levels; i++) {
		if (!mlift_tex_ensure(ml, &st->chain[i], lw[i], lh[i])) {
			return false;
		}
	}
	id<MTLTexture> prev = src;
	double px = p->cx, py = p->cy, pw = p->cw, ph = p->ch;
	for (uint32_t i = 0; i < levels; i++) {
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = st->chain[i];
		rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; // every texel is written
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:pso];
		const bool one_to_one = (uint32_t)pw == lw[i] && (uint32_t)ph == lh[i];
		mlift_draw(enc, prev, px, py, pw, ph, 0.0, 0.0, lw[i], lh[i], one_to_one);
		[enc endEncoding];
		prev = st->chain[i];
		px = 0.0;
		py = 0.0;
		pw = lw[i];
		ph = lh[i];
	}
	*out_last = levels - 1;
	return true;
}

uint32_t
comp_multi_weave_metal_lift_record(struct comp_multi_weave_metal_lift *ml,
                                   const struct comp_multi_weave_metal_lift_planner *pl,
                                   void *cmd_ptr,
                                   void *src_ptr,
                                   void *dst_ptr,
                                   const struct vk_conv_weave_rect *rects,
                                   uint32_t count)
{
	if (ml == NULL || pl == NULL || pl->plan == NULL || cmd_ptr == NULL || src_ptr == NULL || dst_ptr == NULL ||
	    rects == NULL) {
		return 0;
	}
	id<MTLCommandBuffer> cmd = (id<MTLCommandBuffer>)cmd_ptr;
	id<MTLTexture> src = (id<MTLTexture>)src_ptr;
	id<MTLTexture> dst = (id<MTLTexture>)dst_ptr;
	mlift_frame_reset(ml); // a frame recorded and never completed (dropped)
	for (struct mlift_stream *st = ml->streams; st != NULL; st = st->next) {
		st->used_this_frame = false;
	}

	id<MTLRenderPipelineState> chain_pso = mlift_pso(ml, MTLPixelFormatRGBA8Unorm);
	id<MTLRenderPipelineState> dst_pso = mlift_pso(ml, [dst pixelFormat]);
	if (chain_pso == nil || dst_pso == nil) {
		return 0;
	}

	uint32_t lifted = 0;
	@autoreleasepool {
		// 1. Plans; the snapshots' chains (they read the source before the
		// draws below can overwrite it on the v6 layout).
		for (uint32_t i = 0; i < count && ml->entry_count < XRT_LIFT_WEAVE_RECTS_MAX; i++) {
			const struct vk_conv_weave_rect *r = &rects[i];
			if (r->binding == NULL) {
				continue;
			}
			struct mlift_entry *e = &ml->entries[ml->entry_count];
			memset(e, 0, sizeof(*e));
			if (!pl->plan(pl->ud, r, &e->plan)) {
				continue; // not lifted: weave it as drawn
			}
			lifted++;
			e->st = mlift_stream_get(ml, r->owner, r->binding->stream_id);
			if (e->st == NULL) {
				continue; // planned (pinned) but nothing recorded: complete drops it
			}
			e->r = r;
			e->st->used_this_frame = true;
			e->st->idle = 0;
			ml->entry_count++;
			if (e->plan.w == 0 || e->plan.h == 0) {
				continue;
			}
			if (e->plan.snapshot) {
				uint32_t last = 0;
				const size_t bytes = (size_t)e->plan.dw * e->plan.dh * 4u;
				if (mlift_buf_ensure(ml, &e->st->snap_buf, bytes) &&
				    mlift_encode_chain(ml, cmd, src, e, chain_pso, &last)) {
					id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
					[blit copyFromTexture:e->st->chain[last]
					                 sourceSlice:0
					                 sourceLevel:0
					                sourceOrigin:MTLOriginMake(0, 0, 0)
					                  sourceSize:MTLSizeMake(e->plan.dw, e->plan.dh, 1)
					                    toBuffer:e->st->snap_buf
					           destinationOffset:0
					      destinationBytesPerRow:(NSUInteger)e->plan.dw * 4u
					    destinationBytesPerImage:bytes];
					[blit endEncoding];
					e->snap_recorded = true;
				}
			}
			if (e->plan.lines) {
				const uint32_t n = U_LIFT_LETTERBOX_LINE_SAMPLES;
				const uint32_t w = e->plan.w, h = e->plan.h;
				const size_t bytes = (size_t)n * (w + h) * 4u;
				if (mlift_buf_ensure(ml, &e->st->lines_buf, bytes)) {
					id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
					for (uint32_t k = 0; k < n; k++) {
						const uint32_t cx = (uint32_t)e->plan.x + u_lift_letterbox_sample_pos(k, w);
						[blit copyFromTexture:src
						                 sourceSlice:0
						                 sourceLevel:0
						                sourceOrigin:MTLOriginMake(cx, (NSUInteger)e->plan.y, 0)
						                  sourceSize:MTLSizeMake(1, h, 1)
						                    toBuffer:e->st->lines_buf
						           destinationOffset:(NSUInteger)k * h * 4u
						      destinationBytesPerRow:4
						    destinationBytesPerImage:(NSUInteger)h * 4u];
					}
					for (uint32_t k = 0; k < n; k++) {
						const uint32_t ry = (uint32_t)e->plan.y + u_lift_letterbox_sample_pos(k, h);
						[blit copyFromTexture:src
						                 sourceSlice:0
						                 sourceLevel:0
						                sourceOrigin:MTLOriginMake((NSUInteger)e->plan.x, ry, 0)
						                  sourceSize:MTLSizeMake(w, 1, 1)
						                    toBuffer:e->st->lines_buf
						           destinationOffset:(NSUInteger)n * h * 4u + (NSUInteger)k * w * 4u
						      destinationBytesPerRow:(NSUInteger)w * 4u
						    destinationBytesPerImage:(NSUInteger)w * 4u];
					}
					[blit endEncoding];
					e->lines_recorded = true;
				}
			}
			if (e->plan.result) {
				e->result_tex = mlift_wrap_result(ml, &e->plan);
				if (e->result_tex == nil && !e->st->logged_export) {
					e->st->logged_export = true;
					U_LOG_I(
					    "lift rect: stream 0x%llx failed: export (result IOSurface %ux%u format %u "
					    "not wrappable); woven flat",
					    (unsigned long long)e->st->wire, e->plan.result_w, e->plan.result_h,
					    e->plan.result_format);
				}
			}
		}

		// 2. The results' Vulkan writes (an asynchronous S5 hand-over, step E)
		// finish before the pass below samples them: a GPU wait on the glue's
		// shared event, never a host stall. ORDER: this wait is encoded on the
		// command buffer BEFORE step 3's render encoder, the first and only
		// place a result texture is sampled. The pins taken in step 1's plans
		// are NOT released here: they drop only in vk_conv_weave_complete,
		// called after [cmd waitUntilCompleted] (comp_multi_weave_macos_metal.m,
		// the #2200 rule) -- step E moved the publish point, not the release.
		for (uint32_t i = 0; i < ml->entry_count; i++) {
			const struct mlift_entry *e = &ml->entries[i];
			if (e->result_tex == nil || e->plan.result_event == NULL) {
				continue;
			}
			id<MTLSharedEvent> ev = (id<MTLSharedEvent>)e->plan.result_event;
			if (ev.device == ml->device) {
				[cmd encodeWaitForEvent:ev value:e->plan.result_event_value];
			} else {
				// Another device's event cannot be encoded here: wait on the host,
				// bounded (the Vulkan submit was already made when it was planned).
				const uint64_t until = os_monotonic_get_ns() + 100ull * 1000ull * 1000ull;
				while (ev.signaledValue < e->plan.result_event_value && os_monotonic_get_ns() < until) {
					os_nanosleep(200 * 1000);
				}
			}
		}

		// 3. One pass into the destination: results (or flat), bars.
		if (ml->entry_count > 0) {
			MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
			rp.colorAttachments[0].texture = dst;
			rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
			rp.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
			[enc setRenderPipelineState:dst_pso];
			for (uint32_t i = 0; i < ml->entry_count; i++) {
				struct mlift_entry *e = &ml->entries[i];
				const struct vk_conv_weave_host_plan *p = &e->plan;
				const struct vk_conv_weave_rect *r = e->r;
				if (r == NULL || p->w == 0 || p->h == 0) {
					continue;
				}
				const uint32_t views = r->views;
				if (e->result_tex != nil && p->result_views > 0) {
					const double vw = (double)(p->result_w / p->result_views);
					for (uint32_t v = 0; v < views && v < XRT_CONV_MAX_VIEWS; v++) {
						int32_t tx = 0, ty = 0;
						mlift_tile_origin(r, v, &tx, &ty);
						const uint32_t sv = p->tile_view[v] < p->result_views ? p->tile_view[v] : 0;
						mlift_draw(enc, e->result_tex, sv * vw, 0.0, vw, p->result_h, tx + p->ax0,
						           ty + p->ay0, p->ax1 - p->ax0, p->ay1 - p->ay0, false);
					}
					e->woven = true;
					if (r->batch) {
						const int32_t rx1 = p->x + (int32_t)p->w, ry1 = p->y + (int32_t)p->h;
						const int32_t bar[4][4] = {
						    {p->x, p->y, (int32_t)p->w, p->ay0 - p->y}, // top
						    {p->x, p->ay1, (int32_t)p->w, ry1 - p->ay1}, // bottom
						    {p->x, p->ay0, p->ax0 - p->x, p->ay1 - p->ay0}, // left
						    {p->ax1, p->ay0, rx1 - p->ax1, p->ay1 - p->ay0}, // right
						};
						for (uint32_t k = 0; k < 4; k++) {
							for (uint32_t v = 0; v < views; v++) {
								int32_t tx = 0, ty = 0;
								mlift_tile_origin(r, v, &tx, &ty);
								mlift_draw(enc, src, bar[k][0], bar[k][1], bar[k][2],
								           bar[k][3], tx + bar[k][0], ty + bar[k][1],
								           bar[k][2], bar[k][3], true);
							}
						}
					}
				} else if (r->batch) {
					// Flat until the first result: the 2D frame in every tile.
					for (uint32_t v = 0; v < views; v++) {
						int32_t tx = 0, ty = 0;
						mlift_tile_origin(r, v, &tx, &ty);
						mlift_draw(enc, src, p->x, p->y, p->w, p->h, tx + p->x, ty + p->y, p->w, p->h,
						           true);
					}
				}
			}
			[enc endEncoding];
		}
	}
	return lifted;
}

void
comp_multi_weave_metal_lift_frame_done(struct comp_multi_weave_metal_lift *ml,
                                       const struct comp_multi_weave_metal_lift_planner *pl,
                                       bool gpu_ok)
{
	if (ml == NULL) {
		return;
	}
	if (pl != NULL && pl->done != NULL) {
		for (uint32_t i = 0; i < ml->entry_count; i++) {
			const struct mlift_entry *e = &ml->entries[i];
			struct vk_conv_weave_host_done d;
			memset(&d, 0, sizeof(d));
			if (gpu_ok) {
				d.snapshot = e->snap_recorded ? [e->st->snap_buf contents] : NULL;
				d.lines = e->lines_recorded ? [e->st->lines_buf contents] : NULL;
				if (e->woven) {
					d.woven_w = e->plan.result_w;
					d.woven_h = e->plan.result_h;
					d.woven_views = e->plan.result_views;
				}
			}
			pl->done(pl->ud, e->st->owner, e->st->wire, &d);
		}
	}
	mlift_frame_reset(ml);

	// Streams unused for long let their objects go (the GPU is idle for them).
	struct mlift_stream **pp = &ml->streams;
	while (*pp != NULL) {
		struct mlift_stream *st = *pp;
		if (!st->used_this_frame && ++st->idle > MLIFT_IDLE_FRAMES) {
			*pp = st->next;
			mlift_stream_free(st);
			continue;
		}
		st->used_this_frame = false;
		pp = &st->next;
	}
}

uint32_t
comp_multi_weave_metal_lift_stream_count(struct comp_multi_weave_metal_lift *ml)
{
	uint32_t n = 0;
	for (struct mlift_stream *st = ml != NULL ? ml->streams : NULL; st != NULL; st = st->next) {
		n++;
	}
	return n;
}


/*
 *
 * Lifetime.
 *
 */

struct comp_multi_weave_metal_lift *
comp_multi_weave_metal_lift_create(void *mtl_device)
{
	id<MTLDevice> dev = (id<MTLDevice>)mtl_device;
	if (dev == nil) {
		return NULL;
	}
	struct comp_multi_weave_metal_lift *ml = calloc(1, sizeof(*ml));
	if (ml == NULL) {
		return NULL;
	}
	ml->device = dev;
	@autoreleasepool {
		NSError *err = nil;
		NSString *src = [NSString stringWithUTF8String:mlift_shader_src];
		ml->lib = [dev newLibraryWithSource:src options:nil error:&err];
		if (ml->lib == nil) {
			U_LOG_E("lift rect (metal): shader compile failed: %s",
			        err != nil ? [[err localizedDescription] UTF8String] : "?");
			free(ml);
			return NULL;
		}
		ml->vs = [ml->lib newFunctionWithName:@"mlift_vs"];
		ml->fs = [ml->lib newFunctionWithName:@"mlift_fs"];
	}
	if (ml->vs == nil || ml->fs == nil) {
		comp_multi_weave_metal_lift_destroy(&ml);
		return NULL;
	}
	return ml;
}

void
comp_multi_weave_metal_lift_destroy(struct comp_multi_weave_metal_lift **ml_ptr)
{
	if (ml_ptr == NULL || *ml_ptr == NULL) {
		return;
	}
	struct comp_multi_weave_metal_lift *ml = *ml_ptr;
	mlift_frame_reset(ml);
	while (ml->streams != NULL) {
		struct mlift_stream *st = ml->streams;
		ml->streams = st->next;
		mlift_stream_free(st);
	}
	for (uint32_t i = 0; i < ml->pso_count; i++) {
		[ml->pso[i] release];
	}
	[ml->vs release];
	[ml->fs release];
	[ml->lib release];
	free(ml);
	*ml_ptr = NULL;
}
