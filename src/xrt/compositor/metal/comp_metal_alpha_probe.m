// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal lazy-transparency alpha probe. Compiled with ARC.
 * @ingroup comp_metal
 */

#import <Metal/Metal.h>
#include <stdatomic.h>

#include "comp_metal_alpha_probe.h"

#include "util/u_logging.h"

#define PROBE_SLOTS 3
#define PROBE_STRIDE 4u

static NSString *const kProbeSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct P { uint w; uint h; uint stride; };\n"
     "kernel void probe(texture2d<float, access::read> t [[texture(0)]],\n"
     "                  device atomic_uint *count [[buffer(0)]],\n"
     "                  constant P &p [[buffer(1)]],\n"
     "                  uint2 gid [[thread_position_in_grid]]) {\n"
     "  uint2 c = gid * p.stride;\n"
     "  if (c.x >= p.w || c.y >= p.h) return;\n"
     "  // 254.5/255: alpha that is not exactly opaque. A blended edge counts.\n"
     "  if (t.read(c).a < 0.998) atomic_fetch_add_explicit(count, 1u, memory_order_relaxed);\n"
     "}\n";

struct probe_slot
{
	id<MTLBuffer> buf;
	_Atomic int state; //!< 0 free, 1 encoded (in flight), 2 completed
	uint64_t serial;
};

struct comp_metal_alpha_probe
{
	id<MTLDevice> device;
	id<MTLComputePipelineState> pso;
	struct probe_slot slots[PROBE_SLOTS];
	uint64_t next_serial;
	uint64_t last_read_serial;
};

struct comp_metal_alpha_probe *
comp_metal_alpha_probe_create(void *mtl_device)
{
	id<MTLDevice> dev = (__bridge id<MTLDevice>)mtl_device;
	NSError *err = nil;
	id<MTLLibrary> lib = [dev newLibraryWithSource:kProbeSource options:nil error:&err];
	id<MTLFunction> fn = [lib newFunctionWithName:@"probe"];
	id<MTLComputePipelineState> pso = fn != nil ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
	if (pso == nil) {
		U_LOG_E("Metal alpha probe: kernel build failed: %s", err.localizedDescription.UTF8String);
		return NULL;
	}
	struct comp_metal_alpha_probe *p = calloc(1, sizeof(*p));
	p->device = dev;
	p->pso = pso;
	for (int i = 0; i < PROBE_SLOTS; i++) {
		p->slots[i].buf = [dev newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
	}
	return p;
}

void
comp_metal_alpha_probe_destroy(struct comp_metal_alpha_probe **probe_ptr)
{
	struct comp_metal_alpha_probe *p = *probe_ptr;
	if (p == NULL) {
		return;
	}
	*probe_ptr = NULL;
	for (int i = 0; i < PROBE_SLOTS; i++) {
		p->slots[i].buf = nil;
	}
	p->pso = nil;
	p->device = nil;
	free(p);
}

void
comp_metal_alpha_probe_record(struct comp_metal_alpha_probe *p,
                              void *mtl_command_buffer,
                              void *mtl_texture,
                              uint32_t width,
                              uint32_t height)
{
	if (p == NULL || mtl_texture == NULL || width == 0 || height == 0) {
		return;
	}
	struct probe_slot *s = NULL;
	for (int i = 0; i < PROBE_SLOTS; i++) {
		if (atomic_load(&p->slots[i].state) == 0) {
			s = &p->slots[i];
			break;
		}
	}
	if (s == NULL) {
		return; // all in flight / unread: skip this frame's probe
	}
	*(uint32_t *)s->buf.contents = 0;
	s->serial = ++p->next_serial;
	atomic_store(&s->state, 1);

	struct
	{
		uint32_t w, h, stride;
	} params = {width, height, PROBE_STRIDE};

	id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)mtl_command_buffer;
	id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
	[enc setComputePipelineState:p->pso];
	[enc setTexture:(__bridge id<MTLTexture>)mtl_texture atIndex:0];
	[enc setBuffer:s->buf offset:0 atIndex:0];
	[enc setBytes:&params length:sizeof(params) atIndex:1];
	MTLSize grid = MTLSizeMake((width + PROBE_STRIDE - 1) / PROBE_STRIDE, (height + PROBE_STRIDE - 1) / PROBE_STRIDE, 1);
	MTLSize tg = MTLSizeMake(16, 16, 1);
	[enc dispatchThreads:grid threadsPerThreadgroup:tg];
	[enc endEncoding];

	struct probe_slot *slot = s;
	[cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
	  (void)done;
	  atomic_store(&slot->state, 2);
	}];
}

bool
comp_metal_alpha_probe_read(struct comp_metal_alpha_probe *p, uint32_t *out_transparent_samples)
{
	if (p == NULL) {
		return false;
	}
	struct probe_slot *newest = NULL;
	for (int i = 0; i < PROBE_SLOTS; i++) {
		struct probe_slot *s = &p->slots[i];
		if (atomic_load(&s->state) == 2) {
			if (newest == NULL || s->serial > newest->serial) {
				newest = s;
			}
		}
	}
	if (newest == NULL) {
		return false;
	}
	uint32_t count = *(uint32_t *)newest->buf.contents;
	uint64_t serial = newest->serial;
	// Free every completed slot (older ones are superseded).
	for (int i = 0; i < PROBE_SLOTS; i++) {
		if (atomic_load(&p->slots[i].state) == 2) {
			atomic_store(&p->slots[i].state, 0);
		}
	}
	if (serial <= p->last_read_serial) {
		return false;
	}
	p->last_read_serial = serial;
	*out_transparent_samples = count;
	return true;
}
