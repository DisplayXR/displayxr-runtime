// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen segment display processors for the in-process Metal
 *         compositor (multi-screen on macOS, ADR-047 D2).
 * @ingroup comp_metal
 *
 * Mirrors d3d11/comp_d3d11_segments.cpp: the screen-table join, the lifecycle
 * with hysteresis, the decide loop and the two-pass weave order are the same
 * by construction. What is Metal: the GPU work (blit-encoder crops, a clear
 * pass, a sub-rect flat-2D blit with its own tiny pipeline) is encoded on the
 * frame's one command buffer, and each DP encodes its own render pass on it in
 * order. What is macOS: the table is cut in points and converted per segment
 * (see the header). Built without ARC, like comp_metal_compositor.m.
 */

#include "comp_metal_segments.h"

#include "os/os_threading.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "xrt/xrt_plugin.h"

#import <Metal/Metal.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Static, TU-local stb (same pattern as comp_metal_compositor.m).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


/*
 *
 * Flat-2D blit shader: one source sub-rect (normalized) into the viewport.
 *
 */

static NSString *const seg_blit_source = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 position [[position]]; float2 uv; };\n"
    "struct SegBlit { float4 src_rect; };\n"
    "vertex VOut seg_blit_vertex(uint vid [[vertex_id]], constant SegBlit &b [[buffer(0)]]) {\n"
    "    VOut o;\n"
    "    float2 t = float2((vid << 1) & 2, vid & 2);\n"
    "    o.position = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    o.uv = b.src_rect.xy + t * b.src_rect.zw;\n"
    "    return o;\n"
    "}\n"
    "fragment float4 seg_blit_fragment(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],\n"
    "                                  sampler smp [[sampler(0)]]) {\n"
    "    return tex.sample(smp, in.uv);\n"
    "}\n";


/*
 *
 * Internal types.
 *
 */

//! Per-screen runtime state (indexed like comp_metal_segments::screens).
struct seg_screen_state
{
	struct xrt_display_processor_metal *dp; //!< NULL until the lifecycle creates it.
	bool configured;                        //!< set_background_2d(NULL) sent once.
	int mode_sent;                          //!< last 2D/3D mode sent, -1 = none.
	bool tolerates_resample;
	id<MTLTexture> crop; //!< retained; the atlas tiles cut down to one segment
	uint32_t crop_w, crop_h;
};

//! One segment converted out of points (see the header).
struct seg_px
{
	struct comp_seg_rect window_px;      //!< drawable px — the DP canvas
	struct comp_seg_rect screen_px;      //!< that screen's own backing px
	int32_t origin_x, origin_y;          //!< present origin, screen backing px
	struct comp_seg_rect metric_screen;  //!< points × drawable scale (M3 metrics)
	enum comp_seg_1to1 screen_1to1;
};

struct comp_metal_segments
{
	id<MTLDevice> device;      //!< borrowed
	id<MTLCommandQueue> queue; //!< borrowed

	id<MTLRenderPipelineState> blit_pipeline; //!< retained, lazily built
	id<MTLSamplerState> sampler;              //!< retained, lazily built
	bool blit_failed;

	bool enabled;
	uint32_t screen_count;
	struct comp_segments_screen screens[COMP_SEGMENTS_MAX_SCREENS]; //!< desktop = POINTS
	float scale[COMP_SEGMENTS_MAX_SCREENS];                         //!< each screen's backing scale
	struct xrt_screen_binding bindings[COMP_SEGMENTS_MAX_SCREENS];
	struct xrt_screen_info info[COMP_SEGMENTS_MAX_SCREENS];
	const struct xrt_plugin_iface *iface[COMP_SEGMENTS_MAX_SCREENS];
	struct xrt_plugin_instance *inst[COMP_SEGMENTS_MAX_SCREENS];
	struct seg_screen_state st[COMP_SEGMENTS_MAX_SCREENS];

	//! This update's table in points (lifecycle, logging) and converted.
	struct comp_segment_table table_pt;
	struct comp_segment_table table; //!< window_rect / screen_rect / origin in px
	struct seg_px px[COMP_SEGMENTS_MAX];
	struct comp_metal_seg_window win;
	double sx, sy; //!< drawable px per window point

	struct comp_segments_lifecycle lc;

	//! The app thread reads st[i].dp for eyes; the weave creates/destroys it.
	struct os_mutex dp_mutex;

	bool mode_3d;
	uint32_t mode_index;

	bool have_logged_table;
	bool logged_split;
	struct comp_segment_table logged_table;
};


/*
 *
 * Helpers.
 *
 */

static int
screen_index_of(const struct comp_metal_segments *segs, uint64_t screen_id)
{
	for (uint32_t i = 0; i < segs->screen_count; i++) {
		if (segs->screens[i].id == screen_id) {
			return (int)i;
		}
	}
	return -1;
}

static void
crop_destroy(struct seg_screen_state *st)
{
	if (st->crop != nil) {
		[st->crop release];
		st->crop = nil;
	}
	st->crop_w = st->crop_h = 0;
}

static bool
crop_ensure(struct comp_metal_segments *segs, struct seg_screen_state *st, uint32_t w, uint32_t h)
{
	if (st->crop != nil && st->crop_w == w && st->crop_h == h) {
		return true;
	}
	crop_destroy(st);
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                                                width:w
	                                                                               height:h
	                                                                            mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	desc.storageMode = MTLStorageModePrivate;
	st->crop = [segs->device newTextureWithDescriptor:desc];
	if (st->crop == nil) {
		U_LOG_E("segments: crop texture %ux%u failed", w, h);
		return false;
	}
	st->crop_w = w;
	st->crop_h = h;
	return true;
}

static void
dp_release(struct comp_metal_segments *segs, uint32_t i)
{
	struct seg_screen_state *st = &segs->st[i];
	if (st->dp != NULL) {
		U_LOG_I("segments: retiring the segment DP for screen 0x%016llx ('%s')",
		        (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name);
		os_mutex_lock(&segs->dp_mutex);
		struct xrt_display_processor_metal *dp = st->dp;
		st->dp = NULL;
		os_mutex_unlock(&segs->dp_mutex);
		xrt_display_processor_metal_destroy(&dp);
	}
	st->configured = false;
	st->mode_sent = -1;
	st->tolerates_resample = false;
	crop_destroy(st);
}

/*!
 * Create screen @p i's segment DP through the plug-in's per-screen factory.
 * Windowless (NULL view) on purpose: its phase comes from set_present_origin.
 * No fallback to the plain `create_dp_metal` (see
 * xrt_plugin_iface::create_dp_vk_for_screen for why).
 */
static bool
dp_create(struct comp_metal_segments *segs, uint32_t i)
{
	struct seg_screen_state *st = &segs->st[i];
	const struct xrt_plugin_iface *iface = segs->iface[i];
	xrt_result_t xret = XRT_ERROR_DEVICE_CREATION_FAILED;
	struct xrt_display_processor_metal *dp = NULL;
	if (segs->screens[i].has_dp_factory && xrt_plugin_iface_has_create_dp_metal_for_screen(iface)) {
		xret = iface->create_dp_metal_for_screen(segs->inst[i], (__bridge void *)segs->device,
		                                         (__bridge void *)segs->queue, NULL, &segs->bindings[i], &dp);
	}
	if (xret != XRT_SUCCESS || dp == NULL || dp->process_atlas == NULL) {
		if (dp != NULL) {
			xrt_display_processor_metal_destroy(&dp);
		}
		U_LOG_W(
		    "segments: could not create a DP for screen 0x%016llx ('%s', plug-in '%s') via "
		    "create_dp_metal_for_screen (%d) — that segment stays flat 2D",
		    (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name, segs->screens[i].plugin_id,
		    (int)xret);
		return false;
	}
	os_mutex_lock(&segs->dp_mutex);
	st->dp = dp;
	os_mutex_unlock(&segs->dp_mutex);
	st->tolerates_resample = xrt_display_processor_metal_tolerates_resample(dp);
	st->configured = false;
	st->mode_sent = -1;
	// Lifecycle event (hysteresis-gated), not per frame.
	U_LOG_W(
	    "segments: created a segment DP for screen 0x%016llx ('%s', plug-in '%s') via "
	    "create_dp_metal_for_screen — %s",
	    (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name, segs->screens[i].plugin_id,
	    st->tolerates_resample ? "tolerates a resample" : "needs 1:1 pixels");
	return true;
}

static bool
blit_ensure(struct comp_metal_segments *segs)
{
	if (segs->blit_pipeline != nil) {
		return true;
	}
	if (segs->blit_failed) {
		return false;
	}
	NSError *error = nil;
	id<MTLLibrary> lib = [segs->device newLibraryWithSource:seg_blit_source options:nil error:&error];
	if (lib == nil) {
		U_LOG_E("segments: flat-2D blit shader failed: %s", error.localizedDescription.UTF8String);
		segs->blit_failed = true;
		return false;
	}
	id<MTLFunction> vs = [lib newFunctionWithName:@"seg_blit_vertex"];
	id<MTLFunction> fs = [lib newFunctionWithName:@"seg_blit_fragment"];
	MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
	desc.vertexFunction = vs;
	desc.fragmentFunction = fs;
	desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	segs->blit_pipeline = [segs->device newRenderPipelineStateWithDescriptor:desc error:&error];
	[desc release];
	[vs release];
	[fs release];
	[lib release];
	if (segs->blit_pipeline == nil) {
		U_LOG_E("segments: flat-2D blit pipeline failed: %s", error.localizedDescription.UTF8String);
		segs->blit_failed = true;
		return false;
	}
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
	sd.minFilter = MTLSamplerMinMagFilterLinear;
	sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	segs->sampler = [segs->device newSamplerStateWithDescriptor:sd];
	[sd release];
	return segs->sampler != nil;
}

//! Round a point edge to drawable px, clamped to [0, max].
static int32_t
edge_px(double pt, double scale, uint32_t max)
{
	double v = floor(pt * scale + 0.5);
	if (v < 0.0) {
		v = 0.0;
	}
	if (v > (double)max) {
		v = (double)max;
	}
	return (int32_t)v;
}

static int32_t
round_i(double v)
{
	return (int32_t)floor(v + 0.5);
}

/*!
 * Convert this update's point table into drawable / screen px (see the
 * header for the four rules). Fills segs->table and segs->px.
 */
static void
convert_table(struct comp_metal_segments *segs)
{
	segs->table = segs->table_pt;
	const double sx = segs->sx;
	const double sy = segs->sy;
	for (uint32_t k = 0; k < segs->table_pt.count; k++) {
		const struct comp_segment *g = &segs->table_pt.seg[k];
		struct comp_segment *o = &segs->table.seg[k];
		struct seg_px *p = &segs->px[k];
		const uint32_t i = g->screen_index;
		const struct comp_segments_screen *scr = &segs->screens[i];
		const double s_scr = segs->scale[i];

		// Window rect (pt, relative to the window) -> drawable px.
		const int32_t x0 = edge_px((double)g->window_rect.x, sx, segs->win.drawable_w);
		const int32_t x1 = edge_px((double)g->window_rect.x + g->window_rect.w, sx, segs->win.drawable_w);
		const int32_t y0 = edge_px((double)g->window_rect.y, sy, segs->win.drawable_h);
		const int32_t y1 = edge_px((double)g->window_rect.y + g->window_rect.h, sy, segs->win.drawable_h);
		p->window_px = (struct comp_seg_rect){x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};

		// Screen rect (pt, relative to the screen) -> that screen's backing px.
		const int32_t s0x = round_i((double)g->screen_rect.x * s_scr);
		const int32_t s0y = round_i((double)g->screen_rect.y * s_scr);
		const int32_t s1x = round_i(((double)g->screen_rect.x + g->screen_rect.w) * s_scr);
		const int32_t s1y = round_i(((double)g->screen_rect.y + g->screen_rect.h) * s_scr);
		p->screen_px = (struct comp_seg_rect){s0x, s0y, (uint32_t)(s1x - s0x), (uint32_t)(s1y - s0y)};

		// Present origin: origin + canvas offset == the segment's top-left on
		// the panel, in the panel's own backing px.
		p->origin_x = s0x - x0;
		p->origin_y = s0y - y0;

		// The metric space: points × the drawable scale.
		const int32_t m0x = round_i((double)g->screen_rect.x * sx);
		const int32_t m0y = round_i((double)g->screen_rect.y * sy);
		p->metric_screen = (struct comp_seg_rect){
		    m0x, m0y, (uint32_t)(round_i(((double)g->screen_rect.x + g->screen_rect.w) * sx) - m0x),
		    (uint32_t)(round_i(((double)g->screen_rect.y + g->screen_rect.h) * sy) - m0y)};

		// 1:1: the window's backing scale is the screen's, and the screen's
		// points × scale are its native mode.
		if (scr->native_w == 0 || scr->native_h == 0 || s_scr <= 0.0) {
			p->screen_1to1 = COMP_SEG_1TO1_UNKNOWN;
		} else {
			const bool same_scale = fabs(sx - s_scr) < 0.01 && fabs(sy - s_scr) < 0.01;
			const bool native = round_i((double)scr->desktop.w * s_scr) == (int32_t)scr->native_w &&
			                    round_i((double)scr->desktop.h * s_scr) == (int32_t)scr->native_h;
			p->screen_1to1 = (same_scale && native) ? COMP_SEG_1TO1_YES : COMP_SEG_1TO1_NO;
		}

		o->window_rect = p->window_px;
		o->screen_rect = p->screen_px;
		o->present_origin_x = p->origin_x;
		o->present_origin_y = p->origin_y;
		o->screen_1to1 = p->screen_1to1;
	}
}


/*
 *
 * Split-frame capture (debug): `touch $TMPDIR/displayxr_segments_trigger` and
 * the next SPLIT frame writes `$TMPDIR/displayxr_segments.output.png` (the
 * drawable after every DP and the flat-2D fill, when the drawable is readable)
 * and `$TMPDIR/displayxr_segments.seg<k>.png` (segment k's DP input, i.e. its
 * cropped views). Polled only on the split path, so a single-display frame
 * pays nothing. Under $TMPDIR, not /tmp, so concurrent DisplayXR apps (whose
 * Metal atlas trigger is the shared /tmp/dxr_atlas_trigger) do not race it.
 *
 */

static void
seg_dump_dir(char *out, size_t size)
{
	const char *t = getenv("TMPDIR");
	snprintf(out, size, "%s", (t != NULL && t[0] != '\0') ? t : "/tmp");
	size_t n = strlen(out);
	if (n > 1 && out[n - 1] == '/') {
		out[n - 1] = '\0';
	}
}

//! Encode a readback of @p tex into a shared buffer; write @p path on completion.
static void
seg_dump_texture(struct comp_metal_segments *segs, id<MTLCommandBuffer> cmd, id<MTLTexture> tex, const char *path)
{
	if (tex == nil || tex.framebufferOnly || tex.width == 0 || tex.height == 0) {
		U_LOG_W("segments: capture skipped for %s (texture not readable)", path);
		return;
	}
	const NSUInteger w = tex.width;
	const NSUInteger h = tex.height;
	const NSUInteger pitch = w * 4;
	id<MTLBuffer> buf = [segs->device newBufferWithLength:pitch * h options:MTLResourceStorageModeShared];
	if (buf == nil) {
		return;
	}
	id<MTLBlitCommandEncoder> bl = [cmd blitCommandEncoder];
	[bl copyFromTexture:tex
	                 sourceSlice:0
	                 sourceLevel:0
	                sourceOrigin:MTLOriginMake(0, 0, 0)
	                  sourceSize:MTLSizeMake(w, h, 1)
	                    toBuffer:buf
	           destinationOffset:0
	      destinationBytesPerRow:pitch
	    destinationBytesPerImage:pitch * h];
	[bl endEncoding];
	char *p = strdup(path);
	[cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
	  (void)done;
	  uint8_t *src = (uint8_t *)buf.contents;
	  uint8_t *rgba = (uint8_t *)malloc(pitch * h);
	  if (rgba != NULL && p != NULL) {
		  for (size_t i = 0; i < pitch * h; i += 4) {
			  rgba[i + 0] = src[i + 2];
			  rgba[i + 1] = src[i + 1];
			  rgba[i + 2] = src[i + 0];
			  rgba[i + 3] = 255;
		  }
		  stbi_write_png(p, (int)w, (int)h, 4, rgba, (int)pitch);
		  U_LOG_W("segments: wrote %s (%lux%lu)", p, (unsigned long)w, (unsigned long)h);
	  }
	  free(rgba);
	  free(p);
	  [buf release];
	}];
}


/*
 *
 * 'Exported' functions.
 *
 */

struct comp_metal_segments *
comp_metal_segments_create(void *mtl_device, void *command_queue)
{
	if (mtl_device == NULL) {
		return NULL;
	}
	struct comp_metal_segments *segs = U_TYPED_CALLOC(struct comp_metal_segments);
	if (segs == NULL) {
		return NULL;
	}
	segs->device = (__bridge id<MTLDevice>)mtl_device;
	segs->queue = (__bridge id<MTLCommandQueue>)command_queue;
	os_mutex_init(&segs->dp_mutex);
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
	segs->mode_index = UINT32_MAX;
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		segs->st[i].mode_sent = -1;
	}
	return segs;
}

void
comp_metal_segments_destroy(struct comp_metal_segments **segs_ptr)
{
	if (segs_ptr == NULL || *segs_ptr == NULL) {
		return;
	}
	struct comp_metal_segments *segs = *segs_ptr;
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	if (segs->blit_pipeline != nil) {
		[segs->blit_pipeline release];
	}
	if (segs->sampler != nil) {
		[segs->sampler release];
	}
	os_mutex_destroy(&segs->dp_mutex);
	free(segs);
	*segs_ptr = NULL;
}

void
comp_metal_segments_set_screens(struct comp_metal_segments *segs,
                                const struct xrt_screen_list *list,
                                const struct xrt_system_compositor_info *info,
                                uint64_t pinned_display_id)
{
	if (segs == NULL) {
		return;
	}
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
	segs->enabled = false;
	segs->screen_count = 0;
	segs->have_logged_table = false;
	segs->logged_split = false;
	memset(&segs->table, 0, sizeof(segs->table));
	memset(&segs->table_pt, 0, sizeof(segs->table_pt));

	const char *env = getenv("DXR_SEGMENTS");
	if (env != NULL && env[0] == '0') {
		U_LOG_W("segments: DXR_SEGMENTS=0 — a window spanning displays keeps the single-DP path");
		return;
	}
	if (list == NULL || info == NULL || list->count < 2) {
		return;
	}
	if (pinned_display_id != 0) {
		U_LOG_I("segments: session pinned to display 0x%016llx (XrSessionDisplayBindingDXR) — no segmentation",
		        (unsigned long long)pinned_display_id);
		return;
	}

	// The primary: the system-default screen, woven by the session's own DP.
	const struct xrt_dp_registry_entry *primary_entry = NULL;
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX; i++) {
		if ((list->screens[i].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) == 0) {
			continue;
		}
		for (uint32_t e = 0; e < info->dp_registry.entry_count; e++) {
			if (info->dp_registry.entries[e].monitor_id == list->screens[i].id) {
				primary_entry = &info->dp_registry.entries[e];
				break;
			}
		}
	}
	if (primary_entry == NULL) {
		U_LOG_I("segments: the DP registry does not know the system-default screen — no segmentation");
		return;
	}

	uint32_t n = 0;
	bool mixed = false;
	char vendors[256] = {0};
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX && n < COMP_SEGMENTS_MAX_SCREENS; i++) {
		const struct xrt_screen *s = &list->screens[i];
		if (s->desktop_width == 0 || s->desktop_height == 0) {
			continue;
		}
		const struct xrt_dp_registry_entry *e = NULL;
		for (uint32_t k = 0; k < info->dp_registry.entry_count; k++) {
			if (info->dp_registry.entries[k].monitor_id == s->id) {
				e = &info->dp_registry.entries[k];
				break;
			}
		}

		struct comp_segments_screen *cs = &segs->screens[n];
		memset(cs, 0, sizeof(*cs));
		cs->id = s->id;
		cs->desktop.x = s->desktop_left; // CGDisplayBounds, top-down points
		cs->desktop.y = s->desktop_top;
		cs->desktop.w = s->desktop_width;
		cs->desktop.h = s->desktop_height;
		cs->native_w = s->native_width;
		cs->native_h = s->native_height;
		cs->is_primary = (e == primary_entry);
		snprintf(cs->plugin_id, sizeof(cs->plugin_id), "%s", s->plugin_id);

		// The screen's backing scale: the desktop scale the enumeration
		// recorded, else native / points, else 1.
		float scale = s->desktop_scale;
		if (scale <= 0.0f && s->native_width > 0) {
			scale = (float)s->native_width / (float)s->desktop_width;
		}
		segs->scale[n] = scale > 0.0f ? scale : 1.0f;

		segs->iface[n] = NULL;
		segs->inst[n] = NULL;
		if (e != NULL && !cs->is_primary) {
			if (strcmp(e->plugin_id, primary_entry->plugin_id) != 0) {
				mixed = true;
				if (strstr(vendors, e->plugin_id) == NULL) {
					size_t used = strlen(vendors);
					snprintf(vendors + used, sizeof(vendors) - used, "%s%s", used > 0 ? ", " : "",
					         e->plugin_id);
				}
			}
			segs->iface[n] = (const struct xrt_plugin_iface *)e->owning_iface;
			segs->inst[n] = (struct xrt_plugin_instance *)e->owning_instance;
			cs->has_dp_factory = e->dp_factory_metal != NULL &&
			                     xrt_plugin_iface_has_create_dp_metal_for_screen(segs->iface[n]);
		}

		struct xrt_screen_binding *b = &segs->bindings[n];
		memset(b, 0, sizeof(*b));
		b->struct_size = (uint32_t)sizeof(*b);
		b->monitor_id = s->id;
		b->desktop_left = s->desktop_left;
		b->desktop_top = s->desktop_top;
		b->desktop_width = s->desktop_width;
		b->desktop_height = s->desktop_height;
		b->native_pixel_width = s->native_width;
		b->native_pixel_height = s->native_height;
		b->physical_width_mm = s->physical_width_mm;
		b->physical_height_mm = s->physical_height_mm;
		b->desktop_scale = segs->scale[n];
		snprintf(b->device_name, sizeof(b->device_name), "%s", s->device_name);
		if (e != NULL) {
			snprintf(b->serial, sizeof(b->serial), "%s", e->serial);
		}
		segs->info[n] = s->info;
		U_LOG_I("segments: screen [%u] 0x%016llx '%s' %d,%d %ux%u pt, native %ux%u, scale %.2f%s%s", n,
		        (unsigned long long)s->id, s->device_name, s->desktop_left, s->desktop_top, s->desktop_width,
		        s->desktop_height, s->native_width, s->native_height, segs->scale[n],
		        cs->is_primary ? " (primary)" : "", cs->has_dp_factory ? " (Metal per-screen DP)" : "");
		n++;
	}
	segs->screen_count = n;

	if (mixed) {
		U_LOG_I(
		    "segments: mixed-vendor screen table — primary '%s', other screens by '%s'; each segment DP "
		    "comes from its own screen's plug-in",
		    primary_entry->plugin_id, vendors);
	}
	segs->enabled = n >= 2;
	if (segs->enabled) {
		U_LOG_W(
		    "segments: %u screens, plug-in '%s' — a window spanning displays is woven per display "
		    "(multi-screen, Metal)",
		    n, primary_entry->plugin_id);
	}
}

bool
comp_metal_segments_enabled(const struct comp_metal_segments *segs)
{
	return segs != NULL && segs->enabled;
}

bool
comp_metal_segments_update(struct comp_metal_segments *segs, const struct comp_metal_seg_window *win, uint32_t mode_index)
{
	if (segs == NULL || !segs->enabled || win == NULL || win->frame_pt.w == 0 || win->frame_pt.h == 0 ||
	    win->drawable_w == 0 || win->drawable_h == 0) {
		return false;
	}
	segs->win = *win;
	segs->sx = (double)win->drawable_w / (double)win->frame_pt.w;
	segs->sy = (double)win->drawable_h / (double)win->frame_pt.h;

	comp_segments_compute(&win->frame_pt, NULL, segs->screens, segs->screen_count, &segs->table_pt);

	struct comp_segments_actions act;
	comp_segments_lifecycle_update(&segs->lc, &segs->table_pt, &act);
	for (uint32_t a = 0; a < act.count; a++) {
		const int i = screen_index_of(segs, act.items[a].screen_id);
		if (i < 0) {
			continue;
		}
		if (act.items[a].action == COMP_SEG_ACTION_CREATE) {
			const bool ok = dp_create(segs, (uint32_t)i);
			comp_segments_lifecycle_set_created(&segs->lc, act.items[a].screen_id, ok);
		} else if (act.items[a].action == COMP_SEG_ACTION_DESTROY) {
			dp_release(segs, (uint32_t)i);
		}
	}

	const bool split = comp_segments_table_is_split(&segs->table_pt, win->frame_pt.w, win->frame_pt.h);
	convert_table(segs);

	// Re-ask each DP's resample tolerance on a mode or table change only.
	const bool table_changed =
	    split && (!segs->have_logged_table || !comp_segments_table_equal(&segs->table, &segs->logged_table));
	if (mode_index != segs->mode_index || table_changed) {
		segs->mode_index = mode_index;
		for (uint32_t i = 0; i < segs->screen_count; i++) {
			if (segs->st[i].dp != NULL) {
				segs->st[i].tolerates_resample = xrt_display_processor_metal_tolerates_resample(segs->st[i].dp);
			}
		}
	}

	// One INFO line per table change while split (drawable px), and one on leaving it.
	if (table_changed) {
		char buf[768];
		comp_segments_table_format(&segs->table, buf, sizeof(buf));
		U_LOG_I("segments: window %d,%d %ux%u pt (drawable %ux%u px) -> %s", win->frame_pt.x, win->frame_pt.y,
		        win->frame_pt.w, win->frame_pt.h, win->drawable_w, win->drawable_h, buf);
		for (uint32_t k = 0; k < segs->table.count; k++) {
			const struct seg_px *p = &segs->px[k];
			U_LOG_I("segments:  [%u] screen 0x%016llx canvas %d,%d %ux%u px -> panel %d,%d %ux%u px, "
			        "present origin %d,%d, 1:1 %s",
			        k, (unsigned long long)segs->table.seg[k].screen_id, p->window_px.x, p->window_px.y,
			        p->window_px.w, p->window_px.h, p->screen_px.x, p->screen_px.y, p->screen_px.w,
			        p->screen_px.h, p->origin_x, p->origin_y,
			        p->screen_1to1 == COMP_SEG_1TO1_YES  ? "yes"
			        : p->screen_1to1 == COMP_SEG_1TO1_NO ? "no"
			                                             : "unknown");
		}
		segs->logged_table = segs->table;
		segs->have_logged_table = true;
	} else if (!split && segs->logged_split) {
		U_LOG_I("segments: window %d,%d %ux%u pt is on the primary display only — single-DP path",
		        win->frame_pt.x, win->frame_pt.y, win->frame_pt.w, win->frame_pt.h);
		segs->have_logged_table = false;
	}
	segs->logged_split = split;
	return split;
}

bool
comp_metal_segments_record(struct comp_metal_segments *segs, const struct comp_metal_segments_frame *f)
{
	if (segs == NULL || f == NULL || f->command_buffer == NULL || f->src_texture == NULL ||
	    f->target_texture == NULL || f->tile_columns == 0 || f->tile_rows == 0 || f->view_width == 0 ||
	    f->view_height == 0) {
		return false;
	}
	id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)f->command_buffer;
	id<MTLTexture> src = (__bridge id<MTLTexture>)f->src_texture;
	id<MTLTexture> target = (__bridge id<MTLTexture>)f->target_texture;
	const struct comp_segment_table *t = &segs->table;
	const struct comp_seg_rect canvas = {0, 0, f->target_width, f->target_height};

	/*
	 * 1. Decide each segment: which DP (if any) and whether it may weave.
	 */
	struct xrt_display_processor_metal *dp_of[COMP_SEGMENTS_MAX] = {0};
	struct seg_screen_state *st_of[COMP_SEGMENTS_MAX] = {0};
	bool weave[COMP_SEGMENTS_MAX] = {0};
	struct comp_seg_rect tile[COMP_SEGMENTS_MAX];
	bool have_tile[COMP_SEGMENTS_MAX] = {0};
	memset(tile, 0, sizeof(tile));

	for (uint32_t k = 0; k < t->count; k++) {
		const struct comp_segment *g = &t->seg[k];
		have_tile[k] = comp_segments_tile_rect(&g->window_rect, &canvas, f->view_width, f->view_height, &tile[k]);
		const uint32_t i = g->screen_index;
		st_of[k] = i < segs->screen_count ? &segs->st[i] : NULL;
		if (g->is_primary) {
			dp_of[k] = f->primary_dp;
			weave[k] = f->primary_dp != NULL;
		} else if (st_of[k] != NULL) {
			dp_of[k] = st_of[k]->dp;
			weave[k] = comp_segments_decide(dp_of[k] != NULL, st_of[k]->tolerates_resample, g->screen_1to1) ==
			           COMP_SEG_RENDER_WEAVE;
		}
		if (!have_tile[k] || st_of[k] == NULL) {
			weave[k] = false;
		}
	}

	/*
	 * 2. Clear the target: a segment DP confines its draw to its canvas, so
	 * whatever no segment covers would otherwise be undefined.
	 */
	{
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = target;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, f->transparent_background ? 0.0 : 1.0);
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
		[enc endEncoding];
	}

	/*
	 * 3. Crop each woven segment's views out of the atlas — crop before the
	 * DP is the law (ADR-030): a DP's atlas holds exactly its canvas.
	 */
	{
		id<MTLBlitCommandEncoder> blit = nil;
		for (uint32_t k = 0; k < t->count; k++) {
			if (!weave[k]) {
				continue;
			}
			const uint32_t cw = tile[k].w * f->tile_columns;
			const uint32_t ch = tile[k].h * f->tile_rows;
			if (!crop_ensure(segs, st_of[k], cw, ch)) {
				weave[k] = false; // no input image: flat 2D instead
				continue;
			}
			if (blit == nil) {
				blit = [cmd blitCommandEncoder];
			}
			for (uint32_t row = 0; row < f->tile_rows; row++) {
				for (uint32_t col = 0; col < f->tile_columns; col++) {
					const NSUInteger sx = col * f->view_width + (NSUInteger)tile[k].x;
					const NSUInteger sy = row * f->view_height + (NSUInteger)tile[k].y;
					if (sx + tile[k].w > src.width || sy + tile[k].h > src.height) {
						continue;
					}
					[blit copyFromTexture:src
					          sourceSlice:0
					          sourceLevel:0
					         sourceOrigin:MTLOriginMake(sx, sy, 0)
					           sourceSize:MTLSizeMake(tile[k].w, tile[k].h, 1)
					            toTexture:st_of[k]->crop
					     destinationSlice:0
					     destinationLevel:0
					    destinationOrigin:MTLOriginMake(col * tile[k].w, row * tile[k].h, 0)];
				}
			}
		}
		if (blit != nil) {
			[blit endEncoding];
		}
	}

	/*
	 * 4. Weave: the primary first (its DP was built for the single-DP path and
	 * may clear the whole target, so it must not run after a sibling), then
	 * left to right. Each encodes its own pass with canvas = its segment.
	 */
	bool any_woven = false;
	for (uint32_t pass = 0; pass < 2; pass++) {
		for (uint32_t k = 0; k < t->count; k++) {
			const struct comp_segment *g = &t->seg[k];
			if (!weave[k] || g->is_primary != (pass == 0)) {
				continue;
			}
			struct xrt_display_processor_metal *dp = dp_of[k];
			struct seg_screen_state *st = st_of[k];
			if (!g->is_primary) {
				if (!st->configured) {
					st->configured = true;
					xrt_display_processor_metal_set_background_2d(dp, NULL, 0, 0);
				}
				if (st->mode_sent != (int)segs->mode_3d) {
					st->mode_sent = (int)segs->mode_3d;
					xrt_display_processor_metal_request_display_mode(dp, segs->mode_3d);
				}
				// Phase only where window px reach the panel 1:1.
				if (g->screen_1to1 != COMP_SEG_1TO1_NO) {
					xrt_display_processor_metal_set_present_origin(dp, g->present_origin_x,
					                                               g->present_origin_y);
				}
			}
			xrt_display_processor_metal_process_atlas(
			    dp, (__bridge void *)cmd, (__bridge void *)st->crop, tile[k].w, tile[k].h, f->tile_columns,
			    f->tile_rows, (uint32_t)MTLPixelFormatBGRA8Unorm, (__bridge void *)target, f->target_width,
			    f->target_height, g->window_rect.x, g->window_rect.y, g->window_rect.w, g->window_rect.h);
			any_woven = true;
		}
	}

	/*
	 * 5. Flat 2D for every segment that could not be woven and for the canvas
	 * no segment covers: one view (the middle one — the left eye of a stereo
	 * pair), linearly scaled from the view tile to the window rect (#1654).
	 */
	struct comp_seg_rect flat_dst[COMP_SEGMENTS_MAX + COMP_SEGMENTS_MAX_UNCOVERED];
	uint32_t flat_n = 0;
	for (uint32_t k = 0; k < t->count; k++) {
		if (!weave[k] && have_tile[k]) {
			flat_dst[flat_n++] = t->seg[k].window_rect;
		}
	}
	flat_n += comp_segments_uncovered(t, &canvas, &flat_dst[flat_n], COMP_SEGMENTS_MAX_UNCOVERED);
	if (flat_n > 0 && blit_ensure(segs)) {
		const uint32_t views = f->tile_columns * f->tile_rows;
		const uint32_t vi = views > 0 ? (views - 1) / 2 : 0;
		const double base_x = (double)((vi % f->tile_columns) * f->view_width);
		const double base_y = (double)((vi / f->tile_columns) * f->view_height);
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = target;
		rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:segs->blit_pipeline];
		[enc setFragmentTexture:src atIndex:0];
		[enc setFragmentSamplerState:segs->sampler atIndex:0];
		for (uint32_t k = 0; k < flat_n; k++) {
			const struct comp_seg_rect *d = &flat_dst[k];
			struct comp_seg_rect s;
			if (!comp_segments_tile_rect(d, &canvas, f->view_width, f->view_height, &s)) {
				continue; // thinner than one source pixel
			}
			if (d->w == 0 || d->h == 0 || d->x < 0 || d->y < 0 ||
			    (uint64_t)d->x + d->w > f->target_width || (uint64_t)d->y + d->h > f->target_height) {
				continue;
			}
			const float src_rect[4] = {
			    (float)((base_x + s.x) / (double)src.width),
			    (float)((base_y + s.y) / (double)src.height),
			    (float)((double)s.w / (double)src.width),
			    (float)((double)s.h / (double)src.height),
			};
			MTLViewport vp = {(double)d->x, (double)d->y, (double)d->w, (double)d->h, 0.0, 1.0};
			[enc setViewport:vp];
			MTLScissorRect sc = {(NSUInteger)d->x, (NSUInteger)d->y, d->w, d->h};
			[enc setScissorRect:sc];
			[enc setVertexBytes:src_rect length:sizeof(src_rect) atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		}
		[enc endEncoding];
	}

	// Debug capture of this split frame (see seg_dump_texture).
	{
		char dir[512];
		char path[600];
		seg_dump_dir(dir, sizeof(dir));
		snprintf(path, sizeof(path), "%s/displayxr_segments_trigger", dir);
		struct stat sb;
		if (stat(path, &sb) == 0) {
			unlink(path);
			snprintf(path, sizeof(path), "%s/displayxr_segments.output.png", dir);
			seg_dump_texture(segs, cmd, target, path);
			for (uint32_t k = 0; k < t->count; k++) {
				if (weave[k] && st_of[k] != NULL && st_of[k]->crop != nil) {
					snprintf(path, sizeof(path), "%s/displayxr_segments.seg%u.png", dir, k);
					seg_dump_texture(segs, cmd, st_of[k]->crop, path);
				}
			}
		}
	}
	return any_woven;
}

void
comp_metal_segments_set_display_mode(struct comp_metal_segments *segs, bool enable_3d)
{
	if (segs == NULL) {
		return;
	}
	segs->mode_3d = enable_3d;
	for (uint32_t i = 0; i < segs->screen_count; i++) {
		struct seg_screen_state *st = &segs->st[i];
		if (st->dp != NULL && st->mode_sent != (int)enable_3d) {
			st->mode_sent = (int)enable_3d;
			xrt_display_processor_metal_request_display_mode(st->dp, enable_3d);
		}
	}
}

bool
comp_metal_segments_get_metrics(const struct comp_metal_segments *segs,
                                bool primary_has_dp,
                                struct xrt_segment_metrics *out)
{
	memset(out, 0, sizeof(*out));
	if (segs == NULL || !segs->enabled || !segs->logged_split) {
		return false;
	}
	const struct comp_segment_table *t = &segs->table;
	if (t->count == 0 || t->count > XRT_MAX_SEGMENTS) {
		return false; // more screens than view sets: one view set (M2)
	}
	const double sx = segs->sx;
	const double sy = segs->sy;
	for (uint32_t k = 0; k < t->count; k++) {
		const struct comp_segment *g = &t->seg[k];
		if (g->screen_index >= segs->screen_count) {
			return false;
		}
		const uint32_t i = g->screen_index;
		const struct seg_screen_state *st = &segs->st[i];
		const struct seg_px *p = &segs->px[k];
		const struct comp_segments_screen *scr = &segs->screens[i];
		struct xrt_segment_metric *m = &out->seg[k];
		m->screen_id = g->screen_id;
		m->window_rect.offset.w = p->window_px.x;
		m->window_rect.offset.h = p->window_px.y;
		m->window_rect.extent.w = (int)p->window_px.w;
		m->window_rect.extent.h = (int)p->window_px.h;
		m->screen_rect.offset.w = p->metric_screen.x;
		m->screen_rect.offset.h = p->metric_screen.y;
		m->screen_rect.extent.w = (int)p->metric_screen.w;
		m->screen_rect.extent.h = (int)p->metric_screen.h;
		m->screen_desktop_left = round_i((double)scr->desktop.x * sx);
		m->screen_desktop_top = round_i((double)scr->desktop.y * sy);
		m->screen_desktop_width = (uint32_t)round_i((double)scr->desktop.w * sx);
		m->screen_desktop_height = (uint32_t)round_i((double)scr->desktop.h * sy);
		m->screen_width_m = segs->info[i].width_m;
		m->screen_height_m = segs->info[i].height_m;
		m->nominal_viewer_x_m = segs->info[i].nominal_viewer_x_m;
		m->nominal_viewer_y_m = segs->info[i].nominal_viewer_y_m;
		m->nominal_viewer_z_m = segs->info[i].nominal_viewer_z_m;
		m->is_primary = g->is_primary;
		if (g->is_primary) {
			m->has_dp = primary_has_dp;
			m->tolerates_resample = false;
			m->woven = primary_has_dp;
		} else {
			m->has_dp = st->dp != NULL;
			m->tolerates_resample = st->tolerates_resample;
			m->woven = comp_segments_decide(m->has_dp, st->tolerates_resample, g->screen_1to1) ==
			           COMP_SEG_RENDER_WEAVE;
		}
	}
	out->count = t->count;
	out->canvas.offset.w = 0;
	out->canvas.offset.h = 0;
	out->canvas.extent.w = (int)segs->win.drawable_w;
	out->canvas.extent.h = (int)segs->win.drawable_h;
	out->window_screen_left = round_i((double)segs->win.frame_pt.x * sx);
	out->window_screen_top = round_i((double)segs->win.frame_pt.y * sy);
	out->window_pixel_width = segs->win.drawable_w;
	out->window_pixel_height = segs->win.drawable_h;
	return true;
}

struct xrt_display_processor_metal *
comp_metal_segments_snap_dp_at(struct comp_metal_segments *segs,
                               struct xrt_display_processor_metal *primary_dp,
                               double x_pt,
                               double y_pt)
{
	if (segs == NULL || !segs->enabled) {
		return primary_dp;
	}
	for (uint32_t i = 0; i < segs->screen_count; i++) {
		const struct comp_seg_rect *d = &segs->screens[i].desktop;
		if (x_pt >= (double)d->x && x_pt < (double)d->x + (double)d->w && y_pt >= (double)d->y &&
		    y_pt < (double)d->y + (double)d->h) {
			return segs->screens[i].is_primary ? primary_dp : segs->st[i].dp;
		}
	}
	return primary_dp;
}

bool
comp_metal_segments_get_eyes(struct comp_metal_segments *segs, uint64_t screen_id, struct xrt_eye_positions *out)
{
	if (segs == NULL || out == NULL) {
		return false;
	}
	bool ok = false;
	os_mutex_lock(&segs->dp_mutex);
	const int i = screen_index_of(segs, screen_id);
	if (i >= 0 && segs->st[i].dp != NULL) {
		ok = xrt_display_processor_metal_get_predicted_eye_positions(segs->st[i].dp, out) && out->valid;
	}
	os_mutex_unlock(&segs->dp_mutex);
	return ok;
}
