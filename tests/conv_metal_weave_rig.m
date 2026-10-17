// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Headless Metal for the weave-rect tests; see conv_metal_weave_rig.h.
 * @ingroup tests
 */

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "conv_metal_weave_rig.h"

#include <stdlib.h>

struct cmw_rig
{
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
};

struct cmw_rig *
cmw_rig_create(void)
{
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	if (dev == nil) {
		return NULL;
	}
	struct cmw_rig *rig = calloc(1, sizeof(*rig));
	rig->device = dev;
	rig->queue = [dev newCommandQueue];
	if (rig->queue == nil) {
		cmw_rig_destroy(rig);
		return NULL;
	}
	return rig;
}

void
cmw_rig_destroy(struct cmw_rig *rig)
{
	if (rig == NULL) {
		return;
	}
	[rig->queue release];
	[rig->device release];
	free(rig);
}

void *
cmw_rig_device(struct cmw_rig *rig)
{
	return (void *)rig->device;
}

void *
cmw_rig_texture(struct cmw_rig *rig, uint32_t w, uint32_t h, bool bgra, const void *bytes)
{
	MTLTextureDescriptor *d =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bgra ? MTLPixelFormatBGRA8Unorm
	                                                                  : MTLPixelFormatRGBA8Unorm
	                                                       width:w
	                                                      height:h
	                                                   mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	d.storageMode = MTLStorageModeManaged; // CPU upload + readback on every Mac GPU
	id<MTLTexture> t = [rig->device newTextureWithDescriptor:d];
	if (t == nil) {
		return NULL;
	}
	void *zeros = NULL;
	if (bytes == NULL) {
		zeros = calloc((size_t)w * h, 4);
		bytes = zeros;
	}
	[t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:bytes bytesPerRow:(NSUInteger)w * 4u];
	free(zeros);
	return (void *)t;
}

void
cmw_rig_release(void *obj)
{
	[(id)obj release];
}

bool
cmw_rig_read(struct cmw_rig *rig, void *tex_ptr, void *bytes)
{
	id<MTLTexture> tex = (id<MTLTexture>)tex_ptr;
	@autoreleasepool {
		id<MTLCommandBuffer> cmd = [rig->queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
		[blit synchronizeResource:tex];
		[blit endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		if ([cmd status] != MTLCommandBufferStatusCompleted) {
			return false;
		}
	}
	[tex getBytes:bytes
	    bytesPerRow:[tex width] * 4u
	     fromRegion:MTLRegionMake2D(0, 0, [tex width], [tex height])
	    mipmapLevel:0];
	return true;
}

int
cmw_rig_weave(struct cmw_rig *rig,
              struct comp_multi_weave_metal_lift *ml,
              const struct comp_multi_weave_metal_lift_planner *pl,
              void *src,
              void *dst,
              const struct vk_conv_weave_rect *rects,
              uint32_t count)
{
	int lifted = 0;
	bool ok = false;
	@autoreleasepool {
		id<MTLCommandBuffer> cmd = [rig->queue commandBuffer];
		lifted = (int)comp_multi_weave_metal_lift_record(ml, pl, (void *)cmd, src, dst, rects, count);
		[cmd commit];
		[cmd waitUntilCompleted];
		ok = [cmd status] == MTLCommandBufferStatusCompleted;
	}
	comp_multi_weave_metal_lift_frame_done(ml, pl, ok);
	return ok ? lifted : -1;
}
