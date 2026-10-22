// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop preview for the rear depth budget on macOS (#2264).
 * @ingroup comp_metal
 */

#import <Metal/Metal.h>

#include <stdatomic.h>
#include <stdlib.h>

#include "comp_metal_bg_preview.h"

#include "util/u_logging.h"

#define PREVIEW_SLOTS 3
//! The xrt_dp_background_preview contract: both dimensions <= 512.
#define PREVIEW_MAX_DIM 512u

struct preview_slot
{
	id<MTLBuffer> buf;
	_Atomic int state; //!< 0 free, 1 encoded (in flight), 2 completed, 3 handed out
	uint64_t serial;
	uint32_t w, h, stride;
};

struct comp_metal_bg_preview
{
	id<MTLDevice> device;
	//! Level 0 = the desktop under the canvas; the mip chain is the box filter.
	id<MTLTexture> mips;
	uint32_t mips_w, mips_h;
	struct preview_slot slots[PREVIEW_SLOTS];
	uint64_t next_serial;
};

struct comp_metal_bg_preview *
comp_metal_bg_preview_create(void *mtl_device)
{
	struct comp_metal_bg_preview *pv = calloc(1, sizeof(*pv));
	pv->device = (__bridge id<MTLDevice>)mtl_device;
	return pv;
}

void
comp_metal_bg_preview_destroy(struct comp_metal_bg_preview **pv_ptr)
{
	struct comp_metal_bg_preview *pv = *pv_ptr;
	if (pv == NULL) {
		return;
	}
	*pv_ptr = NULL;
	for (int i = 0; i < PREVIEW_SLOTS; i++) {
		pv->slots[i].buf = nil;
	}
	pv->mips = nil;
	pv->device = nil;
	free(pv);
}

void
comp_metal_bg_preview_record(struct comp_metal_bg_preview *pv,
                             void *mtl_command_buffer,
                             void *mtl_texture,
                             uint32_t width,
                             uint32_t height)
{
	if (pv == NULL || mtl_texture == NULL || width == 0 || height == 0) {
		return;
	}
	struct preview_slot *s = NULL;
	for (int i = 0; i < PREVIEW_SLOTS; i++) {
		if (atomic_load(&pv->slots[i].state) == 0) {
			s = &pv->slots[i];
			break;
		}
	}
	if (s == NULL) {
		return; // all in flight / held: skip this frame's preview
	}

	if (pv->mips == nil || pv->mips_w != width || pv->mips_h != height) {
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                                                                             width:width
		                                                                            height:height
		                                                                         mipmapped:YES];
		d.storageMode = MTLStorageModePrivate;
		d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
		pv->mips = [pv->device newTextureWithDescriptor:d];
		pv->mips_w = width;
		pv->mips_h = height;
		if (pv->mips == nil) {
			return;
		}
	}

	// The first level whose both sides fit the contract.
	NSUInteger level = 0;
	uint32_t lw = width, lh = height;
	while ((lw > PREVIEW_MAX_DIM || lh > PREVIEW_MAX_DIM) && level + 1 < pv->mips.mipmapLevelCount) {
		level++;
		lw = MAX(lw >> 1, 1u);
		lh = MAX(lh >> 1, 1u);
	}
	const uint32_t stride = lw * 4;
	const NSUInteger bytes = (NSUInteger)stride * lh;
	if (s->buf == nil || s->buf.length < bytes) {
		s->buf = [pv->device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
		if (s->buf == nil) {
			return;
		}
	}
	s->w = lw;
	s->h = lh;
	s->stride = stride;
	s->serial = ++pv->next_serial;
	atomic_store(&s->state, 1);

	id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)mtl_command_buffer;
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	[blit copyFromTexture:(__bridge id<MTLTexture>)mtl_texture
	          sourceSlice:0
	          sourceLevel:0
	         sourceOrigin:MTLOriginMake(0, 0, 0)
	           sourceSize:MTLSizeMake(width, height, 1)
	            toTexture:pv->mips
	     destinationSlice:0
	     destinationLevel:0
	    destinationOrigin:MTLOriginMake(0, 0, 0)];
	if (level > 0) {
		[blit generateMipmapsForTexture:pv->mips];
	}
	[blit copyFromTexture:pv->mips
	                 sourceSlice:0
	                 sourceLevel:level
	                sourceOrigin:MTLOriginMake(0, 0, 0)
	                  sourceSize:MTLSizeMake(lw, lh, 1)
	                    toBuffer:s->buf
	           destinationOffset:0
	      destinationBytesPerRow:stride
	    destinationBytesPerImage:bytes];
	[blit endEncoding];

	struct preview_slot *slot = s;
	[cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
	  (void)done;
	  atomic_store(&slot->state, 2);
	}];
}

bool
comp_metal_bg_preview_get(struct comp_metal_bg_preview *pv, struct xrt_dp_background_preview *out)
{
	if (pv == NULL || out == NULL) {
		return false;
	}
	struct preview_slot *newest = NULL;
	struct preview_slot *held = NULL;
	for (int i = 0; i < PREVIEW_SLOTS; i++) {
		struct preview_slot *s = &pv->slots[i];
		const int st = atomic_load(&s->state);
		if (st == 2 && (newest == NULL || s->serial > newest->serial)) {
			newest = s;
		} else if (st == 3) {
			held = s;
		}
	}
	if (newest != NULL) {
		// Hand out the newest; release the previously held slot and every
		// older completed one (superseded).
		for (int i = 0; i < PREVIEW_SLOTS; i++) {
			struct preview_slot *s = &pv->slots[i];
			const int st = atomic_load(&s->state);
			if (s != newest && (st == 2 || st == 3)) {
				atomic_store(&s->state, 0);
			}
		}
		atomic_store(&newest->state, 3);
		held = newest;
	}
	if (held == NULL) {
		return false;
	}
	out->generation = (uint32_t)held->serial;
	out->width = held->w;
	out->height = held->h;
	out->stride_bytes = held->stride;
	out->bgra = (const uint8_t *)held->buf.contents;
	// The backdrop is exactly the window content, which is the canvas.
	out->canvas_u0 = 0.0f;
	out->canvas_v0 = 0.0f;
	out->canvas_u1 = 1.0f;
	out->canvas_v1 = 1.0f;
	return true;
}
